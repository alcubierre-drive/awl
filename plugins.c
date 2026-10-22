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
#include "plugins/calpopup.h"
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
    const awl_plugin_config_t* pc = awl_plugin_config();
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
    p->temp_color = &temp_color;

    p->bat = bat_init( pc->battery.dir, pc->battery.ac_dir );
    p->date = date_init();
    // the tray on a loop of its own: its D-Bus traffic and icon lookups
    // never hold up the poller's tick (plugins/loop.h)
    if ((p->tray_loop = awl_loop_new())) {
        p->sni = sni_new( awl_loop_event( p->tray_loop ) );
        if (p->sni && awl_loop_start( p->tray_loop, "tray" )) {
            awl_tray_menu_window_fini();
            awl_tray_tooltip_fini();
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
    awl_tray_menu_window_fini();
    awl_tray_tooltip_fini();
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
 * by a plugin_restart that brought it. A restart in place (a broken new
 * library) calls api_init() again on this copy, and keeps the time. */
static void loaded_stamp( void ) {
    static int stamped = 0; // each load is a fresh copy of the library
    if (stamped) return;
    stamped = 1;
    char buf[32];
    if (!awl_localtime(buf)) {
        AWL_HOST_CFG_SET(awl_host, "t_plugins_loaded", AWL_DICT_STR(buf));
    }
}

static void api_init( int paused ) {
    /* first: everything below reads it */
    awl_config_init();
    loaded_stamp();
    /* this library's git revision, see build-aux/plugins_git.sh */
    AWL_HOST_CFG_SET( awl_host, "plugins_git", AWL_DICT_STR( PLUGINS_GIT ) );
    plugin_data = calloc(1,sizeof(awl_plugin_data_t));
    plugin_data->paused = paused;
    /* plugins/persistent.h's file: $XDG_STATE_HOME/awl/persistent */
    AWL_HOST_CFG_SET( awl_host, AWL_PERSISTENT_KEY, AWL_DICT_STR( "awl/persistent" ) );
    /* what the logind thread reads on connecting; awl's own is its
     * defaults' until it takes this library's config */
    AWL_HOST_CFG_SET( awl_host, "lock_on_suspend", AWL_DICT_NUM( awl_config()->lock.on_suspend ) );
    /* first: the lock before suspend matters more than anything below */
    awl_logind_start();
    awl_plugin_start( plugin_data );
    awl_desktop_start();
    awl_launcher_start();
    awl_wallpaper_start();
    awl_calpopup_start();
    awl_notify_start();
}

static int api_fini( void ) {
    if (!plugin_data) return 0;
    int stuck = awl_notify_stop();
    stuck |= awl_calpopup_stop();
    stuck |= awl_desktop_stop();
    stuck |= awl_launcher_stop();
    stuck |= awl_wallpaper_stop();
    stuck |= awl_logind_stop();
    stuck |= awl_plugin_stop( plugin_data );
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

static void api_wallpaper( WallpaperMode mode ) {
    switch (mode) {
    case WallpaperNext: awl_wallpaper_step( +1 ); break;
    case WallpaperPrev: awl_wallpaper_step( -1 ); break;
    case WallpaperRand: awl_wallpaper_random(); break;
    case WallpaperBack: awl_wallpaper_back(); break;
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
    uint32_t id = awl_host->notif_send( n );
    if (!id) awl_wrn_printf( "notify: '%s' not sent\n", n->summary ? n->summary : "" );
    return id;
}

/** `awl_notify_config()`'s lines so far */
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

/** one entry's line; under the dictionary's lock (awl_dict_iter()), so its
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

void awl_volume_adjust( float delta ) {
    awl_plugin_data_t* p = awl_plugin_get();
    if (p) pulse_volume_adjust( p->pulse, delta );
}

void awl_volume_mute_toggle( void ) {
    awl_plugin_data_t* p = awl_plugin_get();
    if (p) pulse_mute_toggle( p->pulse );
}

void awl_mic_mute_toggle( void ) {
    awl_plugin_data_t* p = awl_plugin_get();
    if (p) pulse_mic_mute_toggle( p->pulse );
}

void awl_speaker_mute_toggle( void ) {
    awl_plugin_data_t* p = awl_plugin_get();
    if (p) pulse_speaker_mute_toggle( p->pulse );
}

void awl_brightness_adjust( float delta ) {
    awl_plugin_data_t* p = awl_plugin_get();
    if (p) backlight_adjust( p->backlight, delta );
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
    n.look.max_lines = -1; /* every entry, up to the screen's height */
    awl_notify_send( &n );
    free( body );
}

/** the wallpaper to tell about once it's shown (`awl_notify_wallpaper_shown()`);
 * main thread */
static const char* shown_title;

void awl_notify_wallpaper_shown( const char* title ) {
    shown_title = title;
}

/** awl takes the wallpaper to show it: if all changes asked for are done,
 * this is the one they end on */
static awl_image_t* api_wallpaper_take( void ) {
    awl_image_t* img = awl_wallpaper_take();
    int cur, n;
    if (img && shown_title && awl_wallpaper_settled( &cur, &n )) {
        char body[32];
        snprintf( body, sizeof(body), "%d/%d", cur + 1, n );
        awl_notify( shown_title, body );
        shown_title = NULL;
    }
    return img;
}

/** the wallpaper timer's mode changed (wallpapermode) */
static void api_wallpaper_mode( WallpaperMode mode ) {
    static const char* names[] = {
        [WallpaperRand] = "rand", [WallpaperNext] = "next", [WallpaperPrev] = "prev",
        [WallpaperModeCount] = "",
    };
    const char* off = awl_config()->wallpaper.interval ? "" : " (timer off)";
    char body[64];
    int cur, n;
    /* where it stands now, as awl_notify_wallpaper_shown() tells it */
    if (awl_wallpaper_settled( &cur, &n ))
        snprintf( body, sizeof(body), "%s: %d/%d%s", names[mode], cur + 1, n, off );
    else
        snprintf( body, sizeof(body), "%s%s", names[mode], off );
    awl_notify( "Wallpaper timer", body );
}

static const awl_plugin_api_t api = {
    .abi = AWL_PLUGIN_ABI,
    .init = api_init,
    .fini = api_fini,
    .set_paused = api_set_paused,
    .bar_widgets = awl_widgets_create,
    .desktop_version = awl_desktop_version,
    .desktop_measure = awl_desktop_measure,
    .desktop_draw = awl_desktop_draw,
    .desktop_click = awl_desktop_click,
    .wallpaper = api_wallpaper,
    .wallpaper_take = api_wallpaper_take,
    .wallpaper_mode = api_wallpaper_mode,
    .session_covered = awl_logind_covered,
    .config = awl_config_fill,
    .notifications_changed = awl_notify_changed,
};

__attribute__((visibility("default")))
const awl_plugin_api_t* awl_plugin_entry( const awl_host_t* host ) {
    if (!host || host->abi != AWL_PLUGIN_ABI ||
        host->sizeof_host != sizeof(awl_host_t) ||
        host->sizeof_draw != sizeof(awl_bar_t) ||
        host->sizeof_widget != sizeof(widget_t) ||
        host->sizeof_config != sizeof(awl_config_t))
        return NULL;
    awl_host = host;
    return &api;
}
