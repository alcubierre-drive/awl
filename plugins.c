#include "awl.h"
#include "plugins.h"
#include "plugins/ipaddr.h"
#include "plugins/stats.h"
#include "plugins/temp.h"
#include "plugins/bat.h"
#include "plugins/date.h"
#include "plugins/pulsetest.h"
#include "plugins/poller.h"
#include "plugins/persistent.h"
#include "plugins/logind.h"
#include "plugins_git.h"

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void awl_plugin_start( awl_plugin_data_t* p ) {
    p->awl_colors = awl_colors();

    p->ip = ip_init();
    p->stats = stats_init( 16, 16, 16 );

    p->temp = calloc(1,sizeof(awl_temperature_t));
    // CPU package sensor (cheap MSR read); the ACPI thermal zone goes through
    // the EC and costs ~2ms per read, so it is only the fallback
    if (temp_find_hwmon( "coretemp", "Package id", p->temp->f_files[p->temp->f_ntemps],
                         sizeof(p->temp->f_files[0]) )) {
        // idles around 50, throttles at TjMax (110)
        p->temp->f_t_max[p->temp->f_ntemps] = 100;
        p->temp->f_t_min[p->temp->f_ntemps] = 50;
    } else {
        strcpy( p->temp->f_files[p->temp->f_ntemps], "/sys/class/thermal/thermal_zone0/temp" );
        p->temp->f_t_max[p->temp->f_ntemps] = 90;
        p->temp->f_t_min[p->temp->f_ntemps] = 40;
    }
    strcpy( p->temp->f_labels[p->temp->f_ntemps++], "" );
    // here could go another thermal zone
    temp_init(p->temp);
    p->temp_color = &temp_color;

    p->bat = bat_init();
    p->date = date_init();
    // one thread for all of them, see plugins/poller.h
    p->poller = poller_new( p->stats, p->temp, p->date, p->bat, p->ip );
    if (!p->poller) return;
    p->pulse = pulse_init( poller_api( p->poller ) );
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

static void awl_plugin_stop( awl_plugin_data_t* p ) {
    poller_stop(p->poller);
    pulse_free(p->pulse);
    backlight_free(p->backlight);
    poller_free(p->poller);
    ip_free(p->ip);
    stats_free(p->stats);
    free(p->temp);
    bat_free(p->bat);
    date_free(p->date);
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
    loaded_stamp();
    /* this library's git revision, see plugins_git.h in the Makefile */
    AWL_HOST_CFG_SET( awl_host, "plugins_git", AWL_DICT_STR( PLUGINS_GIT ) );
    plugin_data = calloc(1,sizeof(awl_plugin_data_t));
    plugin_data->paused = paused;
    /* plugins/persistent.h's file: $XDG_STATE_HOME/awl/persistent */
    AWL_HOST_CFG_SET( awl_host, AWL_PERSISTENT_KEY, AWL_DICT_STR( "awl/persistent" ) );
    /* first: the lock before suspend matters more than anything below */
    awl_logind_start();
    awl_plugin_start( plugin_data );
    awl_desktop_start();
    awl_wallpaper_start();
}

static int api_fini( void ) {
    if (!plugin_data) return 0;
    int stuck = awl_desktop_stop();
    stuck |= awl_wallpaper_stop();
    stuck |= awl_logind_stop();
    awl_plugin_stop( plugin_data );
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
    awl_host->actions->spawn( &(Arg){ .v = (const char*[]){
            "notify-send", "-a", "awl", title, body, NULL } } );
}

/** `awl_notify_config()`'s lines so far */
typedef struct {
    char** lines;
    size_t n, cap;
} ConfigLines;

/** what notify-send's body markup would take for a tag or an entity */
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

void awl_backlight_toggle( void ) {
    awl_plugin_data_t* p = awl_plugin_get();
    if (p) backlight_toggle( p->backlight );
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
    awl_notify( head, body[0] ? body : "(empty)" );
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
    const char* off = awl_config()->wallpaper->interval ? "" : " (timer off)";
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
    .config = awl_config,
};

__attribute__((visibility("default")))
const awl_plugin_api_t* awl_plugin_entry( const awl_host_t* host ) {
    if (!host || host->abi != AWL_PLUGIN_ABI ||
        host->sizeof_host != sizeof(awl_host_t) ||
        host->sizeof_draw != sizeof(awl_draw_t) ||
        host->sizeof_widget != sizeof(widget_t) ||
        host->sizeof_monitor != sizeof(Monitor) ||
        host->sizeof_client != sizeof(Client))
        return NULL;
    awl_host = host;
    return &api;
}
