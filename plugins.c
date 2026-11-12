#include "awl.h"
#include "plugins.h"
#include "plugins/ipaddr.h"
#include "plugins/stats.h"
#include "plugins/temp.h"
#include "plugins/bat.h"
#include "plugins/date.h"
#include "plugins/pulsetest.h"
#include "plugins/poller.h"
#include "plugins/loop.h"
#include "plugins/persistent.h"
#include "plugins/logind.h"
#include "plugins/osd.h"
#include "plugins/calpopup.h"
#include "plugins/batpopup.h"
#include "plugins/sysmon.h"
#include "plugins/tray_menu.h"
#include "plugins/tray_tooltip.h"
#include "plugins/notify.h"
#include "plugins_git.h"

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** how long the tray's loop gets to stop: it may hang in an icon file's
 * read, like the poller in sysfs */
#define TRAY_STOP_TIMEOUT_MS 2000

static void awl_plugin_start( awl_plugin_data_t* p ) {
    const awl_plugin_config_t* pc = &p->plugin_config;
#define AWL_BAR_COLOR_SET( name ) p->colors.name = color_8bit_to_16bit( pc->colors.name );
    AWL_BAR_COLORS( AWL_BAR_COLOR_SET )
#undef AWL_BAR_COLOR_SET

    p->ip = ip_init();
    p->stats = stats_init( 16, 16, 16 );

    p->temp = calloc(1,sizeof(awl_temperature_t));
    // CPU package sensor (cheap MSR read); the ACPI thermal zone goes through
    // the EC and costs ~2ms per read, so it is only the fallback
    if (temp_find_hwmon( "coretemp", "Package id", p->temp->f_files[p->temp->f_ntemps],
                         sizeof(p->temp->f_files[0]) )) {
        p->temp->f_t_max[p->temp->f_ntemps] = pc->temperature.coretemp.max;
        p->temp->f_t_min[p->temp->f_ntemps] = pc->temperature.coretemp.min;
    } else {
        strcpy( p->temp->f_files[p->temp->f_ntemps], "/sys/class/thermal/thermal_zone0/temp" );
        p->temp->f_t_max[p->temp->f_ntemps] = pc->temperature.thermal_zone.max;
        p->temp->f_t_min[p->temp->f_ntemps] = pc->temperature.thermal_zone.min;
    }
    strcpy( p->temp->f_labels[p->temp->f_ntemps++], "" );
    // here could go another thermal zone
    temp_init(p->temp);

    p->bat = bat_init( pc->battery.dir, pc->battery.ac_dir );
    p->date = date_init();
    // the tray on a loop of its own: its D-Bus traffic and icon lookups
    // never hold up the poller's tick (plugins/loop.h)
    // its menus' window and tooltips, the main thread's (tray_menu.h)
    p->tray_menu = awl_tray_menu_window_new( p );
    p->tray_tip = awl_tray_tooltip_new( p );
    if ((p->tray_loop = awl_loop_new())) {
        p->sni = sni_new( awl_loop_event( p->tray_loop ), p->tray_menu, p->config.tray.icon_theme );
        if (p->sni && awl_loop_start( p->tray_loop, "tray" )) {
            sni_free( p->sni );
            p->sni = NULL;
        }
        if (!p->sni) {
            awl_loop_free( p->tray_loop );
            p->tray_loop = NULL;
        }
    }
    // one thread for all of the others, see plugins/poller.h
    p->poller = poller_new( p->stats, p->temp, p->date, p->bat, p->ip );
    if (!p->poller) return;
    p->pulse = pulse_init( poller_api( p->poller ), pc->volume.max, pc->volume.speaker );
    p->backlight = backlight_init( poller_api( p->poller ) );
    poller_set_paused( p->poller, p->paused );
    if (poller_start( p->poller )) {
        pulse_free( p->pulse );
        backlight_free( p->backlight );
        poller_free( p->poller );
        p->pulse = NULL;
        p->backlight = NULL;
        p->poller = NULL;
    }
}

/** returns nonzero if the tray's loop is stuck (it reads icon files, which
 * may be on a dead mount) */
static int awl_tray_stop( awl_plugin_data_t* p ) {
    int stuck = awl_loop_stop( p->tray_loop, TRAY_STOP_TIMEOUT_MS );
    if (stuck)
        awl_wrn_printf( "plugins: the tray thread is stuck, leaking what it uses\n" );
    /* the tray's popups and timers, main thread's; they only queue to sni
     * under its lock, so they go even if it is stuck. A menu open queues
     * its "closed", which sni_free() still sends. sni's poll timer goes
     * with pluginsreap() if sni stays. */
    awl_tray_menu_window_fini( p->tray_menu );
    awl_tray_tooltip_fini( p->tray_tip );
    if (stuck)
        return 1;
    sni_free( p->sni );
    awl_loop_free( p->tray_loop );
    return 0;
}

/** returns nonzero if the poller or the tray's loop is stuck */
static int awl_plugin_stop( awl_plugin_data_t* p ) {
    int stuck = awl_tray_stop( p );
    if (poller_stop(p->poller)) {
        awl_wrn_printf( "plugins: the poller thread is stuck, leaking what it uses\n" );
        /* its loop still holds pulse, backlight and ip's nm, and it polls
         * stats, temp, bat, date and ip: all leaked with the library, which
         * stays mapped. Nothing here reads p itself. */
        return 1;
    }
    pulse_free(p->pulse);
    backlight_free(p->backlight);
    poller_free(p->poller);
    ip_free(p->ip);
    stats_free(p->stats);
    free(p->temp);
    bat_free(p->bat);
    date_free(p->date);
    return stuck;
}

/* everything below is the library's side of awl_plugin_abi.h */

const awl_host_t* awl_host = NULL;
static awl_plugin_data_t* plugin_data = NULL;

awl_plugin_data_t* awl_plugin_get( void ) {
    return plugin_data;
}


/** sets the config dictionary's "t_plugins_loaded" to the local time now, as
 * "YY/MM/DD HH:MM:SS" (24h): when this library was loaded, at awl's start or
 * by a :c:func:`plugin_restart` that brought it. From the entry point, once per copy
 * of the library: a restart in place (a broken new library) calls
 * api_init() again on this copy, and keeps the time. */
static void loaded_stamp( void ) {
    char buf[32];
    if (!awl_localtime(buf)) {
        AWL_HOST_CFG_SET(awl_host, "t_plugins_loaded", AWL_DICT_STR(buf));
    }
}

static void api_init( int paused ) {
    /* this library's git revision, see build-aux/plugins_git.sh */
    AWL_HOST_CFG_SET( awl_host, "plugins_git", AWL_DICT_STR( PLUGINS_GIT ) );
    if (!(plugin_data = calloc( 1, sizeof(awl_plugin_data_t) ))) {
        awl_err_printf( "plugins: out of memory, not running\n" );
        return;
    }
    /* first: everything below reads it */
    awl_config_init( plugin_data );
    plugin_data->paused = paused;
    plugin_data->widgets = awl_widgets_start();
    /* plugins/persistent.h's file: $XDG_STATE_HOME/awl/persistent */
    AWL_HOST_CFG_SET( awl_host, AWL_PERSISTENT_KEY, AWL_DICT_STR( "awl/persistent" ) );
    /* what the logind thread reads on connecting; awl's own is its
     * defaults' until it takes this library's config */
    AWL_HOST_CFG_SET( awl_host, "lock_on_suspend", AWL_DICT_NUM( plugin_data->config.lock.on_suspend ) );
    /* first: the lock before suspend matters more than anything below */
    plugin_data->logind = awl_logind_start();
    awl_plugin_start( plugin_data );
    plugin_data->desktop = awl_desktop_start( plugin_data );
    plugin_data->launcher = awl_launcher_start( plugin_data );
    plugin_data->wallpaper = awl_wallpaper_start();
    plugin_data->calpopup = awl_calpopup_start( plugin_data );
    plugin_data->batpopup = awl_batpopup_start( plugin_data );
    plugin_data->sysmon = awl_sysmon_start( plugin_data );
    plugin_data->notify = awl_notify_start( plugin_data );
    plugin_data->osd = awl_osd_start( plugin_data );
}

static int api_fini( void ) {
    if (!plugin_data) return 0;
    awl_osd_stop( plugin_data->osd );
    int stuck = awl_notify_stop( plugin_data->notify );
    stuck |= awl_calpopup_stop( plugin_data->calpopup );
    awl_batpopup_stop( plugin_data->batpopup );
    awl_sysmon_stop( plugin_data->sysmon );
    stuck |= awl_desktop_stop( plugin_data->desktop );
    stuck |= awl_launcher_stop( plugin_data->launcher );
    stuck |= awl_wallpaper_stop( plugin_data->wallpaper );
    stuck |= awl_logind_stop( plugin_data->logind );
    stuck |= awl_plugin_stop( plugin_data );
    awl_widgets_stop( plugin_data->widgets );
    /* even if stuck: the poller has its own pointers, never this */
    free( plugin_data );
    plugin_data = NULL;
    return stuck;
}

static void api_set_paused( int paused ) {
    if (!plugin_data) return;
    plugin_data->paused = paused;
    if (plugin_data->poller) poller_set_paused( plugin_data->poller, paused );
}

static void api_wallpaper( WallpaperMode mode, int by_timer ) {
    if (!plugin_data) return;
    awl_wallpaper_thread_t* ch = plugin_data->wallpaper;
    if (!by_timer || plugin_data->plugin_config.osd.wallpaper_timer) awl_osd_wallpaper_ask( plugin_data->osd );
    switch (mode) {
    case WallpaperNext: awl_wallpaper_step( ch, +1 ); break;
    case WallpaperPrev: awl_wallpaper_step( ch, -1 ); break;
    case WallpaperRand: awl_wallpaper_random( ch ); break;
    case WallpaperBack: awl_wallpaper_back( ch ); break;
    default: break; /* awl resolves the timer ones */
    }
}

void awl_notify( const char* title, const char* body ) {
    awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
    n.summary = title;
    n.body = body;
    awl_notify_send( &n );
}

uint32_t awl_notify_send( const awl_notif_send_t* n ) {
    uint32_t id = awl_host->notif_send( awl_host->srv, n );
    if (!id) awl_wrn_printf( "notify: '%s' not sent\n", n->summary ? n->summary : "" );
    return id;
}

/** :c:func:`awl_notify_config`'s lines so far */
typedef struct {
    char** lines;
    size_t n, cap;
} ConfigLines;

/** what the body's markup (plugins/markup.h) would take for a tag or an
 * entity */
static void escape( FILE* f, const char* s ) {
    for (; *s; s++) {
        switch (*s) {
        case '&': fputs( "&amp;", f ); break;
        case '<': fputs( "&lt;", f ); break;
        case '>': fputs( "&gt;", f ); break;
        default: fputc( *s, f );
        }
    }
}

/** one entry's line; under the dictionary's lock (:c:func:`awl_dict_iter`), so its
 * strings stay valid */
static int32_t configline( const char* key, const awl_dict_val_t* v, void* ctx ) {
    ConfigLines* cl = ctx;
    if (cl->n == cl->cap) {
        size_t cap = cl->cap ? 2 * cl->cap : 16;
        char** l = realloc( cl->lines, cap * sizeof(*l) );
        if (!l) return AWL_DICT_ERR_NOMEM;
        cl->lines = l;
        cl->cap = cap;
    }
    char* line = NULL;
    size_t len;
    FILE* f = open_memstream( &line, &len );
    if (!f) return AWL_DICT_ERR_NOMEM;
    escape( f, key );
    fputc( '=', f );
    switch (v->kind) {
    case AWL_DICT_KIND_STR: escape( f, v->str ); break;
    case AWL_DICT_KIND_NUM: fprintf( f, "%lld", (long long)v->num ); break;
    case AWL_DICT_KIND_FRAC: fprintf( f, "%g", v->frac ); break;
    case AWL_DICT_KIND_PTR: fprintf( f, "%p", v->ptr ); break;
    case AWL_DICT_KIND_OWNED: {
        /* ISO C has no cast from function to object pointer */
        void* fn;
        memcpy( &fn, &v->owned.free, sizeof(fn) );
        fprintf( f, "%p(%p)", v->owned.ptr, fn );
        break;
    }
    default: fprintf( f, "? (kind %d)", (int)v->kind ); break;
    }
    if (fclose( f )) {
        free( line );
        return AWL_DICT_ERR_NOMEM;
    }
    cl->lines[cl->n++] = line;
    return AWL_DICT_OK;
}

static int cmpline( const void* a, const void* b ) {
    return strcmp( *(char* const*)a, *(char* const*)b );
}

void awl_volume_adjust( awl_plugin_data_t* p, float delta ) {
    if (!p || !p->pulse) return;
    pulse_volume_adjust( p->pulse, delta );
    awl_osd_volume( p->osd );
}

void awl_volume_mute_toggle( awl_plugin_data_t* p ) {
    if (!p || !p->pulse) return;
    pulse_mute_toggle( p->pulse );
    awl_osd_volume( p->osd );
}

void awl_mic_mute_toggle( awl_plugin_data_t* p ) {
    if (p) pulse_mic_mute_toggle( p->pulse );
}

void awl_speaker_mute_toggle( awl_plugin_data_t* p ) {
    if (!p || !p->pulse) return;
    pulse_speaker_mute_toggle( p->pulse );
    awl_osd_volume( p->osd );
}

void awl_brightness_adjust( awl_plugin_data_t* p, float delta ) {
    if (!p || !p->backlight) return;
    backlight_adjust( p->backlight, delta );
    awl_osd_brightness( p->osd, delta );
}

void awl_notify_config( void ) {
    ConfigLines cl = { 0 };
    /* holds the lock throughout; nothing waits under it but malloc() */
    int32_t r = awl_host->cfg.s_iter( awl_host->cfg.s, configline, &cl );
    qsort( cl.lines, cl.n, sizeof(*cl.lines), cmpline );

    char* body = NULL;
    size_t len;
    FILE* f = open_memstream( &body, &len );
    for (size_t i = 0; i < cl.n; i++) {
        if (f) fprintf( f, i ? "\n%s" : "%s", cl.lines[i] );
        free( cl.lines[i] );
    }
    free( cl.lines );
    if (f && r != AWL_DICT_OK) fprintf( f, cl.n ? "\n(%s)" : "(%s)",
            r == AWL_DICT_ERR_INVALID ? "no dictionary" : "out of memory" );
    if (!f || fclose( f )) {
        free( body );
        return;
    }
    char head[128] = {0};
    snprintf( head, 127, "AWL config (ABI v%i)", AWL_PLUGIN_ABI );
    awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
    n.summary = head;
    n.body = body[0] ? body : "(empty)";
    AWL_SET( n.look.max_lines, 0 ); /* every entry, up to the screen's height */
    awl_notify_send( &n );
    free( body );
}

/** awl takes the wallpaper to show it: if all changes asked for are done,
 * this is the one they end on */
static awl_image_t* api_wallpaper_take( void ) {
    if (!plugin_data) return NULL;
    awl_image_t* img = awl_wallpaper_take( plugin_data->wallpaper );
    if (img) awl_osd_wallpaper_taken( plugin_data->osd );
    return img;
}

/** the wallpaper timer's mode changed (wallpapermode); awl set the config
 * dictionary's "wallpaper_mode" before, which the OSD reads */
static void api_wallpaper_mode( WallpaperMode mode ) {
    (void)mode;
    if (plugin_data) awl_osd_wallpaper( plugin_data->osd );
}

/** none while the plugins don't run (their init ran out of memory) */
static void api_config( awl_config_t* c ) {
    awl_config_fill( plugin_data, c );
}

static void api_bar_widgets( awl_bar_t* bar ) {
    if (plugin_data) awl_widgets_create( plugin_data, bar );
}

static uint64_t api_desktop_version( void ) {
    return plugin_data ? awl_desktop_version( plugin_data->desktop ) : 0;
}

static void api_desktop_measure( struct fcft_font* font, int avail_w, int avail_h, int r, float scale,
                                 int* x, int* y, int* w, int* h ) {
    *x = *y = *w = *h = 0;
    if (plugin_data && plugin_data->desktop)
        awl_desktop_measure( plugin_data->desktop, font, avail_w, avail_h, r, scale, x, y, w, h );
}

static void api_desktop_draw( pixman_image_t* pix, struct fcft_font* font, uint32_t* data, int stride,
                              int w, int h, int r, float scale ) {
    if (plugin_data && plugin_data->desktop)
        awl_desktop_draw( plugin_data->desktop, pix, font, data, stride, w, h, r, scale );
}

static int api_desktop_click( int button, uint32_t mods ) {
    return plugin_data ? awl_desktop_click( plugin_data->desktop, button, mods ) : 0;
}

static void api_session_covered( int covered ) {
    if (plugin_data) awl_logind_covered( plugin_data->logind, covered );
}

static void api_notifications_changed( void ) {
    if (plugin_data) awl_notify_changed( plugin_data->notify );
}

static void api_redraw( void ) {
    if (plugin_data) awl_osd_redraw( plugin_data->osd );
}

static const awl_plugin_api_t api = {
    .abi = AWL_PLUGIN_ABI,
    .init = api_init,
    .fini = api_fini,
    .set_paused = api_set_paused,
    .bar_widgets = api_bar_widgets,
    .desktop_version = api_desktop_version,
    .desktop_measure = api_desktop_measure,
    .desktop_draw = api_desktop_draw,
    .desktop_click = api_desktop_click,
    .wallpaper = api_wallpaper,
    .wallpaper_take = api_wallpaper_take,
    .wallpaper_mode = api_wallpaper_mode,
    .session_covered = api_session_covered,
    .config = api_config,
    .notifications_changed = api_notifications_changed,
    .redraw = api_redraw,
};

/** what awl looks up (:c:macro:`AWL_PLUGIN_ENTRY`): keeps host as
 * :c:var:`awl_host` and returns the library's table; NULL if awl was built
 * from other headers (the ABI or a shared struct's size differs) */
__attribute__((visibility("default")))
const awl_plugin_api_t* awl_plugin_entry( const awl_host_t* host ) {
    if (!host || host->abi != AWL_PLUGIN_ABI ||
        host->sizeof_host != sizeof(awl_host_t) ||
        host->sizeof_draw != sizeof(awl_bar_t) ||
        host->sizeof_widget != sizeof(widget_t) ||
        host->sizeof_config != sizeof(awl_config_t))
        return NULL;
    awl_host = host;
    loaded_stamp();
    return &api;
}
