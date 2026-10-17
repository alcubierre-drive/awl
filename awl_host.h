#pragma once

/**
 * awl's host table, `awl_host_t`: everything the code outside the awl binary
 * reaches awl through. libawlplugins.so gets it from `awl_plugin_entry()`
 * (awl_plugin_abi.h), libawltray.so from `awl_tray_init()`; both keep it as
 * `awl_host` and have no other references into awl. Code built with
 * ``AWL_HOSTED`` (either library) is on that side: there awl_log.h's macros
 * go through ``awl_host->log``.
 *
 * Plain C types only (awl's own structs are opaque here), so the C++ tray
 * includes it too. awl fills the table in awl_plugin_host.c; changing it is an
 * ABI change of the plugin library (`AWL_PLUGIN_ABI`).
 */

#include <stddef.h>
#include <stdint.h>
#include <pixman-1/pixman.h>
#include "awl_dict_type.h"
#include "awl_log.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fcft_font;
typedef struct Monitor Monitor;
typedef struct awl_window awl_window_t;
typedef struct awl_window_painter_t awl_window_painter_t;
typedef struct awl_popup_config_t awl_popup_config_t;
typedef struct Client Client;
typedef struct awl_actions_t awl_actions_t;
typedef struct awl_arranges_t awl_arranges_t;

/** what the plugins' logind thread (plugins/logind.h) listens for, as it
 * tells `awl_host_t.logind_state` */
typedef enum awl_logind_flags {
    /** our session's ``Lock`` signal (``loginctl lock-session``) */
    AWL_LOGIND_LOCK = 1,
    /** ``PrepareForSleep``, with a delay inhibitor held, so suspend waits
     * for the lock; only with config_awl.h's lock_on_suspend */
    AWL_LOGIND_SLEEP = 2,
} awl_logind_flags_t;

/**
 * What awl provides to libawlplugins.so and libawltray.so. All functions are
 * main-thread only, except `redraw_request`, `log`, `lock_request`,
 * `logind_state`, `post` and `cfg`'s, which any thread may call. The tray, on
 * its GTK thread, uses only those; what it does on the main thread (its
 * menus, tray/menu_window.c) it gets there through `post`.
 */
typedef struct awl_host_t {
    /** `AWL_PLUGIN_ABI` as awl was built */
    uint32_t abi;
    /** sizeof_host, sizeof_draw, sizeof_widget, sizeof_monitor and
     * sizeof_client: the layouts the library compiles against */
    size_t sizeof_host, sizeof_draw, sizeof_widget, sizeof_monitor, sizeof_client;

    /** `awl_redraw_request()`, called as ``awl_host->redraw_request()`` */
    void (*redraw_request)( void );
    /** `awl_dbg_printf()`: the libraries' `awl_err_printf()` & co. (awl_log.h)
     * expand to it */
    void (*log)( awl_loglevel_t loglevel, const char* fmt, ... )
        __attribute__(( format( printf, 2, 3 ) ));
    /** `awl_lock_request()`: lock the screen (config_awl.h's locker),
     * unless it is or is about to be */
    void (*lock_request)( void );
    /** `awl_lock_logind_state()`: what the logind thread listens for now,
     * `awl_logind_flags` or'ed (0 when it stops); awl raises an alert while
     * that's less than it needs */
    void (*logind_state)( unsigned flags );

    /** drawing: `awl_draw_text()`, text in font into pix, e.g. a bar
     * widget's draw() with its pix and its bar's font */
    int (*text)( pixman_image_t* pix, struct fcft_font* font, int x, int y, unsigned int w,
                 unsigned int h, unsigned int lpad, const char* text, pixman_color_t fg,
                 pixman_color_t bg );
    /** drawing: `awl_draw_text_over()`, text in fg (0xRRGGBBAA) over what is
     * in pix, e.g. in a painter's paint(); cut with an ellipsis at w */
    int (*text_over)( pixman_image_t* pix, struct fcft_font* font, int x, int y, unsigned int w,
                      unsigned int h, const char* text, uint32_t fg );
    /** drawing: `awl_draw_text_width()` */
    unsigned int (*text_width)( struct fcft_font* font, const char* text );

    /** actions: what config_plugins.h binds keys and buttons to */
    const awl_actions_t* actions;
    /** actions: what config_plugins.h's layouts arrange with */
    const awl_arranges_t* arranges;
    /** `focusclient()` */
    void (*focusclient)( Client* c, int lift );
    /** `arrange()` */
    void (*arrange)( Monitor* m );

    /** tray: `awl_tray_width()` */
    uint32_t (*tray_width)( const char* monitor_id );
    /** tray: `awl_tray_set_widget_x()` */
    void (*tray_set_widget_x)( const char* monitor_id, uint32_t x );
    /** awl's own text popups (awl_window.h): `textwincreate()`; a window
     * lives until window_destroy() or until the plugins detach */
    awl_window_t* (*window_create)( void );
    /** `textwindestroy()` */
    void (*window_destroy)( awl_window_t* win );
    /** `textwinshow()` */
    void (*window_show)( awl_window_t* win, Monitor* m, int center_x, const char* text, int toggle );
    /** `textwinupdate()` */
    void (*window_update)( awl_window_t* win, const char* text );
    /** `textwinhide()` */
    void (*window_hide)( awl_window_t* win );
    /** `textwinsetclick()`: the window takes the pointer, presses go to
     * click (row, col: the cell, -1 off the text; button a BTN_*) */
    void (*window_set_click)( awl_window_t* win,
            void (*click)( awl_window_t* win, int row, int col, uint32_t button, void* data ), void* data );

    /**
     * config values that can change while awl runs, by name. awl owns the
     * dictionary (``cfg.s``, awl.c's ``cfgdict``): it lives as long as awl
     * and keeps its entries across reloads.
     *
     * Any thread may use it, plugin threads included (awl_dict.h has the
     * details): every call is atomic on its own, under the dictionary's
     * lock. A thread that needs several calls to see the same dictionary
     * holds the lock across them, with `AWL_HOST_CFG_LOCK` and
     * `AWL_HOST_CFG_UNLOCK`: a read-modify-write, an iteration, and any use
     * of a string or owned pointer it got, which another thread could free
     * by overwriting or deleting the entry. Numbers and plain pointers are
     * copied out and need no lock. The lock is recursive. Hold it briefly
     * and never wait for anything under it: awl's main thread takes it too.
     *
     * A value must not point into
     * the library, which may be unloaded while the entry stays: a string is
     * copied, but a pointer, an owned pointer and its free function must be
     * awl's, libc's or heap memory (``AWL_DICT_OWNED( p, free )``). The
     * `AWL_HOST_CFG_*` macros below go through this.
     */
    awl_dict_c cfg;

    /** `textwinsetscroll()`: the window takes the pointer, scrolling goes
     * to scroll (row, col as for window_set_click; orientation a
     * WL_POINTER_AXIS_*, 0 vertical; delta positive down or right;
     * value120 a wheel's in 120ths of a notch, 0 for a touchpad's). Like
     * window_set_click, a window with either callback takes all of the
     * pointer: presses and scrolling its callbacks don't take are dropped. */
    void (*window_set_scroll)( awl_window_t* win,
            void (*scroll)( awl_window_t* win, int row, int col, uint32_t orientation,
                            double delta, int32_t value120, void* data ), void* data );
    /** `textwinsetpainter()`: the window draws itself with painter
     * (awl_window_draw.h; copied, with data) instead of its text, NULL for
     * the text again. Its parts stand in for the cells: clicks and scrolls
     * get the part as row, column 0. */
    void (*window_set_painter)( awl_window_t* win, const awl_window_painter_t* painter, void* data );
    /** `textwinredraw()`: what the painter draws changed */
    void (*window_redraw)( awl_window_t* win );
    /** `textwinmonitor()`: where the window is shown, NULL while hidden
     * (awl also hides it, on a press elsewhere or with its monitor) */
    Monitor* (*window_monitor)( awl_window_t* win );

    /** `awl_post()`: runs fn( data ) on the main thread, soon; any thread.
     * fn never runs if awl quits first. */
    void (*post)( void (*fn)( void* data ), void* data );
    /** `textwincreatekept()`: like window_create, for the tray's menus: the
     * plugins detaching leave it, a press on the tray's windows doesn't hide
     * it */
    awl_window_t* (*window_create_kept)( void );
    /** `monitornamed()`: the monitor of the output named name, NULL if none */
    Monitor* (*monitor_named)( const char* name );
    /** `popupconfig()`: config_plugins.h's popup_config (awl_window_draw.h)
     * as it is now; valid until the next config reload */
    const awl_popup_config_t* (*popup_config)( void );
    /** `textwinsethidden()`: hidden( win, data ) whenever the window is
     * hidden, by window_hide or by awl (a press elsewhere, its monitor
     * going away); NULL: no more */
    void (*window_set_hidden)( awl_window_t* win, void (*hidden)( awl_window_t* win, void* data ), void* data );
} awl_host_t;

/** `awl_dict_get()` on `awl_host_t.cfg`; valptr is an ``awl_dict_val_t*`` */
#define AWL_HOST_CFG_GET( host, key, valptr ) \
    (host)->cfg.s_get( (host)->cfg.s, (key), (valptr) )
/** `awl_dict_set()` on `awl_host_t.cfg`; val is an ``awl_dict_val_t``, e.g.
 * ``AWL_DICT_STR( "x" )`` */
#define AWL_HOST_CFG_SET( host, key, val ) \
    (host)->cfg.s_set( (host)->cfg.s, (key), &(val) )
/** `awl_dict_del()` on `awl_host_t.cfg` */
#define AWL_HOST_CFG_DEL( host, key ) \
    (host)->cfg.s_del( (host)->cfg.s, (key) )
/** `awl_dict_lock()` on `awl_host_t.cfg`, e.g. around getting a string and
 * using it::
 *
 *     awl_dict_val_t v;
 *     AWL_HOST_CFG_LOCK( awl_host );
 *     if (AWL_HOST_CFG_GET( awl_host, "name", &v ) == AWL_DICT_OK &&
 *         v.kind == AWL_DICT_KIND_STR)
 *         use( v.str );
 *     AWL_HOST_CFG_UNLOCK( awl_host );
 */
#define AWL_HOST_CFG_LOCK( host ) \
    (host)->cfg.s_lock( (host)->cfg.s )
/** `awl_dict_unlock()` on `awl_host_t.cfg` */
#define AWL_HOST_CFG_UNLOCK( host ) \
    (host)->cfg.s_unlock( (host)->cfg.s )

#ifdef AWL_HOSTED
/** awl, as this library sees it: set by `awl_plugin_entry()` (plugins.c) or
 * `awl_tray_init()` (tray/awl_tray_bridge.cpp) before anything else runs */
extern const awl_host_t* awl_host;
#endif

#ifdef __cplusplus
}
#endif
