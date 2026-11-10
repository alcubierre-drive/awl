/* memfd_create() */
#define _GNU_SOURCE

#include "awl_plugin_host.h"
#include "awl_plugin_abi.h"
#include "awl_client.h"
#include "awl_dict.h"
#include "awl_layout.h"
#include "awl_lockscreen.h"
#include "awl_notifications.h"
#include "awl_output.h"
#include "awl_popup.h"
#include "awl_redraw.h"
#include "awl_window.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/** :c:member:`awl_host_t.timer_add() <awl_host_t.timer_add>`'s timers, ``link`` in timers: the plugins',
 * reaped after they stop (:c:func:`pluginsreap`) */
struct awl_timer {
    struct wl_event_source* source;
    void (*fn)( void* data );
    void* data;
    struct wl_list link;
};
static struct wl_list timers = { &timers, &timers };

static int timer_fire( void* data ) {
    awl_timer_t* t = data;
    t->fn( t->data );
    return 0;
}

/** a timer on the main loop that calls fn( data ), unarmed */
static awl_timer_t* timer_add( awl_server_t* srv, void (*fn)( void* data ), void* data ) {
    awl_timer_t* t;
    if (!srv->event_loop || !fn) return NULL;
    t = ecalloc( 1, sizeof(*t) );
    t->fn = fn;
    t->data = data;
    t->source = wl_event_loop_add_timer( srv->event_loop, timer_fire, t );
    wl_list_insert( &timers, &t->link );
    return t;
}

/** fn in ms milliseconds, instead of when it was due; 0 disarms it */
static void timer_set( awl_timer_t* t, int ms ) {
    if (t) wl_event_source_timer_update( t->source, ms > 0 ? ms : 0 );
}

/** NULL is fine */
static void timer_remove( awl_timer_t* t ) {
    if (!t) return;
    wl_list_remove( &t->link );
    wl_event_source_remove( t->source );
    free( t );
}

/** :c:member:`awl_host_t.notif_invoke`: with a token of awl's, so the client may
 * bring its window up */
static void notifinvoke( awl_server_t* srv, uint32_t id, const char* key ) {
    awl_notifications_invoke( id, key, activationtoken(srv) );
}

/** after the library's fini, before it is unloaded: destroys the popups and
 * timers it left (warning about them), whose callbacks point into it */
static void pluginsreap( awl_server_t *srv ) {
    awl_timer_t *t, *tmp;
    int np = popupsreap(srv), nt = 0;
    wl_list_for_each_safe( t, tmp, &timers, link ) {
        nt++;
        timer_remove( t );
    }
    if (np || nt)
        awl_wrn_printf( "plugins: the library left %d popup(s) and %d timer(s), removed\n", np, nt );
}

/* The library calls these without a server: the plugin ABI has no argument for
 * it. */
static void host_focusclient( Client* c, int lift ) { focusclient( awl, c, lift ); }
static Monitor* host_monitor_named( const char* name ) { return monitornamed( awl, name ); }
static Monitor* host_monitor_selected( void ) { return monitorselected( awl ); }
static void host_clientsetvisible( Client *c, int visible ) { clientsetvisible( awl, c, visible ); }
static void host_arrange( Monitor *m ) { arrange( awl, m ); }
static awl_window_t * host_popupcreate( void ) { return popupcreate( awl ); }
static void host_popupdestroy( awl_window_t *win ) { popupdestroy( awl, win ); }
static void host_popupsetmenustyle( awl_window_t *win ) { popupsetmenustyle( awl, win ); }
static void host_popupsetpersistent( awl_window_t *win, int persistent ) { popupsetpersistent( awl, win, persistent ); }
static void host_popupsetcolors( awl_window_t *win, uint32_t bg, uint32_t fg, uint32_t border ) { popupsetcolors( awl, win, bg, fg, border ); }
static void host_popupsetborderwidth( awl_window_t *win, int width ) { popupsetborderwidth( awl, win, width ); }
static void host_popuproom( awl_window_t *win, Monitor *m, int *x, int *y, int *w, int *h ) { popuproom( awl, win, m, x, y, w, h ); }
static int host_popupbox( awl_window_t *win, int *x, int *y, int *w, int *h ) { return popupbox( awl, win, x, y, w, h ); }
static void host_awl_window_set_text( awl_window_t* win, const char* text ) { awl_window_set_text( awl, win, text ); }
static void host_awl_window_set_painter( awl_window_t* win, const awl_window_painter_t* painter, void* data ) { awl_window_set_painter( awl, win, painter, data ); }
static void host_awl_window_redraw( awl_window_t* win ) { awl_window_redraw( awl, win ); }
static void host_popupshow( awl_window_t *win, Monitor *m, int center_x ) { popupshow( awl, win, m, center_x ); }
static void host_popupshowbeside( awl_window_t *win, Monitor *m, int x, int y, int w, int h, int left ) { popupshowbeside( awl, win, m, x, y, w, h, left ); }
static void host_popupshowat( awl_window_t *win, Monitor *m, int at, int dx, int dy ) { popupshowat( awl, win, m, at, dx, dy ); }
static void host_popuphide( awl_window_t *win ) { popuphide( awl, win ); }
static void host_awl_window_set_click( awl_window_t* win, awl_window_click_t click, void* data ) { awl_window_set_click( awl, win, click, data ); }
static void host_awl_window_set_scroll( awl_window_t* win, awl_window_scroll_t scroll, void* data ) { awl_window_set_scroll( awl, win, scroll, data ); }
static void host_popupsetkey( awl_window_t *win, awl_window_key_t key, void *data ) { popupsetkey( awl, win, key, data ); }
static void host_notifinvoke( uint32_t id, const char* key ) { notifinvoke( awl, id, key ); }
static void host_awl_notifications_hold( uint32_t id, int held ) { awl_notifications_hold( awl, id, held ); }
static uint32_t host_awl_notifications_send( const awl_notif_send_t* send ) { return awl_notifications_send( awl, send ); }
static uint32_t host_awl_notifications_show( uint64_t seq ) { return awl_notifications_show( awl, seq ); }
static awl_timer_t* host_timer_add( void (*fn)( void* data ), void* data ) { return timer_add( awl, fn, data ); }

static awl_host_t host = {
    .abi = AWL_PLUGIN_ABI,
    .sizeof_host = sizeof(awl_host_t),
    .sizeof_draw = sizeof(awl_bar_t),
    .sizeof_widget = sizeof(widget_t),
    .sizeof_config = sizeof(awl_config_t),

    .redraw_request = awl_redraw_request,
    .log = awl_dbg_printf,
    .lock_request = awl_lock_request,
    .logind_state = awl_lock_logind_state,
    .cfg = {
        .s_new = &awl_dict_new,
        .s_destroy = &awl_dict_destroy,
        .s_set = &awl_dict_set,
        .s_get = &awl_dict_get,
        .s_del = &awl_dict_del,
        .s_len = &awl_dict_len,
        .s_next = &awl_dict_next,
        .s_iter = &awl_dict_iter,
        .s_lock = &awl_dict_lock,
        .s_unlock = &awl_dict_unlock,
    },

    .text = awl_draw_text,
    .text_over = awl_draw_text_over,
    .text_width = awl_draw_text_width,
    .font_style = awl_font_style,
    .font_release = awl_font_release,

    .config_defaults = configdefaults,
    .actions = &awl_actions,
    .arranges = &awl_arranges,
    .focusclient = host_focusclient,
    .client_set_visible = host_clientsetvisible,
    .arrange = host_arrange,
    .monitor_named = host_monitor_named,
    .monitor_selected = host_monitor_selected,
    .monitor_scale = monitorscale,

    .window_create = host_popupcreate,
    .window_destroy = host_popupdestroy,
    .window_set_menu_style = host_popupsetmenustyle,
    .window_set_persistent = host_popupsetpersistent,
    .window_set_colors = host_popupsetcolors,
    .window_set_border_width = host_popupsetborderwidth,
    .window_room = host_popuproom,
    .window_box = host_popupbox,
    .window_set_text = host_awl_window_set_text,
    .window_set_painter = host_awl_window_set_painter,
    .window_redraw = host_awl_window_redraw,
    .window_show = host_popupshow,
    .window_show_beside = host_popupshowbeside,
    .window_show_at = host_popupshowat,
    .window_hide = host_popuphide,
    .window_monitor = awl_window_monitor,
    .window_part_box = popuppartbox,
    .window_set_click = host_awl_window_set_click,
    .window_set_scroll = host_awl_window_set_scroll,
    .window_set_hover = awl_window_set_hover,
    .window_set_hidden = awl_window_set_hidden,
    .window_set_key = host_popupsetkey,

    .notif_list = awl_notifications_list,
    .notif_close = awl_notifications_close,
    .notif_invoke = host_notifinvoke,
    .notif_hold = host_awl_notifications_hold,
    .notif_send = host_awl_notifications_send,
    .notif_history = awl_notifications_history,
    .notif_forget = awl_notifications_forget,
    .notif_show = host_awl_notifications_show,

    .timer_add = host_timer_add,
    .timer_set = timer_set,
    .timer_remove = timer_remove,
};

static void* handle = NULL;
/** the memfd handle was loaded from, see :c:func:`libopen` */
static int handle_fd = -1;
static const awl_plugin_api_t* api = NULL;
static int paused = 0;
static int covered = 0;

/** $AWL_PLUGINS, or libawlplugins.so next to the awl binary */
static int libpath( char* out, size_t n ) {
    const char* env = getenv( "AWL_PLUGINS" );
    if (env && *env)
        return snprintf( out, n, "%s", env ) < (int)n ? 0 : -1;

    char exe[PATH_MAX];
    ssize_t len = readlink( "/proc/self/exe", exe, sizeof(exe)-1 );
    if (len < 0) return -1;
    exe[len] = 0;
    char* slash = strrchr( exe, '/' );
    if (slash) *slash = 0;
    return snprintf( out, n, "%s/libawlplugins.so", exe ) < (int)n ? 0 : -1;
}

/** Copies the file at path into a memfd, so the loader sees a new file every
 * time: dlopen() hands back the already loaded copy for the same file, and a
 * build overwriting the mapped file in place would crash awl. */
static int memfd_copy( const char* path ) {
    int src = open( path, O_RDONLY | O_CLOEXEC );
    if (src < 0) return -1;
    int dst = memfd_create( "libawlplugins.so", MFD_CLOEXEC );
    if (dst < 0) { close( src ); return -1; }

    char buf[1 << 16];
    ssize_t r;
    while ((r = read( src, buf, sizeof(buf) )) > 0) {
        for (ssize_t off = 0; off < r; ) {
            ssize_t w = write( dst, buf + off, r - off );
            if (w < 0) { r = -1; goto out; }
            off += w;
        }
    }
out:
    close( src );
    if (r < 0) { close( dst ); return -1; }
    return dst;
}

/** The memfd stays open, in ``*out_fd``, until the library is unloaded: dlopen()
 * also hands back an already loaded library of the same name, and a closed
 * one's fd number, i.e. its /proc/PID/fd/N, is the next memfd's. It is
 * /proc/PID, not /proc/self: a debugger reads the library's symbols from the
 * name the loader has, and its own /proc/self is a different process. */
static const awl_plugin_api_t* libopen( void** out, int* out_fd ) {
    char path[PATH_MAX], fdpath[64];
    if (libpath( path, sizeof(path) )) {
        awl_err_printf( "plugins: can't determine the library path\n" );
        return NULL;
    }
    awl_log_printf( "plugins: found the library: %s\n", path );
    int fd = memfd_copy( path );
    if (fd < 0) {
        awl_err_printf( "plugins: can't read %s: %s\n", path, strerror( errno ) );
        return NULL;
    }
    snprintf( fdpath, sizeof(fdpath), "/proc/%d/fd/%d", (int)getpid(), fd );
    void* h = dlopen( fdpath, RTLD_NOW | RTLD_LOCAL );
    if (!h) {
        awl_err_printf( "plugins: %s: %s\n", path, dlerror() );
        close( fd );
        return NULL;
    }

    // ISO C has no cast from object to function pointer
    void* sym = dlsym( h, AWL_PLUGIN_ENTRY );
    awl_plugin_entry_t entry;
    memcpy( &entry, &sym, sizeof(entry) );
    const awl_plugin_api_t* a = entry ? entry( &host ) : NULL;
    if (!a || a->abi != AWL_PLUGIN_ABI) {
        awl_err_printf( "plugins: %s: %s\n", path, !entry ? "no entry point" :
                 "built against different headers than this awl (rebuild both)" );
        dlclose( h );
        close( fd );
        return NULL;
    }
    awl_log_printf( "plugins: loaded %s (ABI %d)\n", path, a->abi );
    *out = h;
    *out_fd = fd;
    return a;
}

int awl_plugins_load( int p, awl_dict_t* cfg ) {
    paused = p;
    host.cfg.s = cfg;
    if (!(api = libopen( &handle, &handle_fd )))
        return -1;
    awl_log_printf( "plugins: setting up\n" );
    api->init( paused );
    api->session_covered( covered );
    awl_log_printf( "plugins: set up\n" );
    return 0;
}

/** stops the plugins and removes what they left (:c:func:`pluginsreap`); unloads
 * the library unless a thread of it is still running, which would then
 * crash */
static void stop( awl_server_t* srv, void* h, int fd ) {
    awl_log_printf( "plugins: tearing down\n" );
    int stuck = api && api->fini();
    pluginsreap(srv);
    if (stuck) {
        awl_wrn_printf( "plugins: a plugin thread is stuck, keeping its library loaded\n" );
        return;
    }
    if (h) dlclose( h );
    if (fd >= 0) close( fd );
    awl_log_printf( "plugins: torn down, the library is unloaded\n" );
}

int awl_plugins_reload( awl_server_t* srv, void (*detach)( awl_server_t* srv ), void (*attach)( awl_server_t* srv ) ) {
    awl_log_printf( "plugins: reloading\n" );
    void* h = NULL;
    int fd = -1;
    const awl_plugin_api_t* next = libopen( &h, &fd );

    // nothing may point into the old library past this
    detach( srv );
    if (next) {
        stop( srv, handle, handle_fd );
        handle = h;
        handle_fd = fd;
        api = next;
    } else if (api) {
        awl_wrn_printf( "plugins: no new library, restarting the old one in place\n" );
        api->fini(); // restart in place; a stuck thread stays stuck either way
        pluginsreap(srv);
        awl_log_printf( "plugins: torn down, the library stays loaded\n" );
    }
    if (api) {
        awl_log_printf( "plugins: setting up\n" );
        api->init( paused );
        api->session_covered( covered );
    }
    attach( srv );
    awl_log_printf( "plugins: reload done%s\n", next ? "" : " (the old library)" );
    return next ? 0 : -1;
}

void awl_plugins_unload( awl_server_t *srv ) {
    stop( srv, handle, handle_fd );
    api = NULL;
    handle = NULL;
    handle_fd = -1;
    host.cfg.s = NULL;
}

void awl_plugins_set_paused( int p ) {
    paused = p;
    if (api) api->set_paused( paused );
}

void awl_plugins_set_covered( int c ) {
    covered = c;
    if (api) api->session_covered( covered );
}

void awl_plugins_notifications_changed( void ) {
    if (api) api->notifications_changed();
}

void awl_plugins_redraw( void ) {
    if (api) api->redraw();
}

void awl_plugins_bar_widgets( awl_bar_t* bar ) {
    if (api) api->bar_widgets( bar );
}

const awl_plugin_api_t* awl_plugins_api( void ) {
    return api;
}
