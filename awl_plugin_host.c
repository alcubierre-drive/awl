/* memfd_create() */
#define _GNU_SOURCE

#include "awl_plugin_host.h"
#include "awl_plugin_abi.h"
#include "awl_dict.h"
#include "awl_lock.h"
#include "awl_redraw.h"
#include "tray/awl_tray.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static awl_host_t host = {
    .abi = AWL_PLUGIN_ABI,
    .sizeof_host = sizeof(awl_host_t),
    .sizeof_draw = sizeof(awl_draw_t),
    .sizeof_widget = sizeof(widget_t),
    .sizeof_monitor = sizeof(Monitor),
    .sizeof_client = sizeof(Client),

    .redraw_request = awl_redraw_request,
    .log = awl_dbg_printf,
    .lock_request = awl_lock_request,
    .logind_state = awl_lock_logind_state,

    .text = awl_draw_text_color2,
    .font_getwidth = awl_draw_font_getwidth,

    .actions = &awl_actions,
    .arranges = &awl_arranges,
    .focusclient = focusclient,
    .arrange = arrange,

    .tray_width = awl_tray_width,
    .tray_set_widget_x = awl_tray_set_widget_x,
    .calendar_toggle = awl_tray_calendar_toggle,
    .calendar_show = awl_tray_calendar_show,
    .calendar_hide = awl_tray_calendar_hide,
    .calendar_scroll = awl_tray_calendar_scroll,
    .popup_show = awl_tray_popup_show,
    .popup_update = awl_tray_popup_update,
    .popup_hide = awl_tray_popup_hide,
    .window_create = textwincreate,
    .window_destroy = textwindestroy,
    .window_show = textwinshow,
    .window_update = textwinupdate,
    .window_hide = textwinhide,

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
};

static void* handle = NULL;
/** the memfd handle was loaded from, see `libopen()` */
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
 * one's fd number, i.e. its /proc/self/fd/N, is the next memfd's. */
static const awl_plugin_api_t* libopen( void** out, int* out_fd ) {
    char path[PATH_MAX], fdpath[64];
    if (libpath( path, sizeof(path) )) {
        awl_err_printf( "plugins: can't determine the library path\n" );
        return NULL;
    }
    int fd = memfd_copy( path );
    if (fd < 0) {
        awl_err_printf( "plugins: can't read %s: %s\n", path, strerror( errno ) );
        return NULL;
    }
    snprintf( fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd );
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
    *out = h;
    *out_fd = fd;
    return a;
}

int awl_plugins_load( int p, awl_dict_t* cfg ) {
    paused = p;
    host.cfg.s = cfg;
    if (!(api = libopen( &handle, &handle_fd )))
        return -1;
    api->init( paused );
    api->session_covered( covered );
    return 0;
}

/** stops the plugins; unloads the library unless a thread of it is still
 * running, which would then crash */
static void stop( void* h, int fd ) {
    if (api && api->fini()) {
        awl_wrn_printf( "plugins: a plugin thread is stuck, keeping its library loaded\n" );
        return;
    }
    if (h) dlclose( h );
    if (fd >= 0) close( fd );
}

int awl_plugins_reload( void (*detach)( void ), void (*attach)( void ) ) {
    void* h = NULL;
    int fd = -1;
    const awl_plugin_api_t* next = libopen( &h, &fd );

    // nothing may point into the old library past this
    detach();
    if (next) {
        stop( handle, handle_fd );
        handle = h;
        handle_fd = fd;
        api = next;
    } else if (api) {
        api->fini(); // restart in place; a stuck thread stays stuck either way
    }
    if (api) {
        api->init( paused );
        api->session_covered( covered );
    }
    attach();
    return next ? 0 : -1;
}

void awl_plugins_unload( void ) {
    stop( handle, handle_fd );
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

void awl_plugins_bar_widgets( awl_draw_t* bar ) {
    if (api) api->bar_widgets( bar );
}

const awl_host_t* awl_host_table( void ) {
    return &host;
}

const awl_plugin_api_t* awl_plugins_api( void ) {
    return api;
}
