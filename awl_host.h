#pragma once

/**
 * awl's host table, `awl_host_t`: everything the code outside the awl binary
 * reaches awl through. libawlplugins.so gets it from `awl_plugin_entry()`
 * (awl_plugin_abi.h) and keeps it as `awl_host`; it has no other references
 * into awl. Code built with ``AWL_HOSTED`` is on that side: there
 * awl_log.h's macros go through ``awl_host->log``.
 *
 * Plain C types only (awl's own structs are opaque here). awl fills the table in awl_plugin_host.c; changing it is an
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
typedef struct awl_config_t awl_config_t;
typedef struct awl_timer awl_timer_t;
typedef struct Client Client;
typedef struct awl_actions_t awl_actions_t;
typedef struct awl_arranges_t awl_arranges_t;

/** what the plugins' logind thread (plugins/logind.h) listens for, as it
 * tells `awl_host_t.logind_state` */
typedef enum awl_logind_flags {
    /** our session's ``Lock`` signal (``loginctl lock-session``) */
    AWL_LOGIND_LOCK = 1,
    /** ``PrepareForSleep``, with a delay inhibitor held, so suspend waits
     * for the lock; only with the config's lock.on_suspend */
    AWL_LOGIND_SLEEP = 2,
} awl_logind_flags_t;

/** a notification's urgency, its hint's values */
typedef enum awl_notif_urgency {
    AWL_NOTIF_LOW = 0,
    AWL_NOTIF_NORMAL = 1,
    AWL_NOTIF_CRITICAL = 2,
} awl_notif_urgency_t;

/** `awl_notif_t.flags`, from its hints */
enum {
    /** ``transient``: not for a history */
    AWL_NOTIF_TRANSIENT = 1,
    /** ``resident``: an action doesn't close it */
    AWL_NOTIF_RESIDENT = 2,
    /** awl's own (`awl_host_t.notif_send`): no D-Bus client replaces or
     * closes it */
    AWL_NOTIF_AWL = 4,
};

/** why a notification closed, NotificationClosed's reasons */
typedef enum awl_notif_reason {
    AWL_NOTIF_EXPIRED = 1,
    AWL_NOTIF_DISMISSED = 2,
    AWL_NOTIF_CLOSED = 3,
    AWL_NOTIF_UNDEFINED = 4,
} awl_notif_reason_t;

/**
 * How a notification looks beyond `awl_plugin_config_t.notify`, for awl's
 * own (`awl_notif_send_t`); D-Bus has no say in it, a D-Bus one's is all 0.
 * 0 is the config's everywhere.
 */
typedef struct awl_notif_look_t {
    /** the body's lines shown at most, < 0 any */
    int max_lines;
    /** logical pixels: the window's content width, the image's side */
    int width, image_size;
    /** 0xRRGGBBAA: text, background, frame, the progress bar's fill */
    uint32_t fg, bg, frame, progress;
} awl_notif_look_t;

/**
 * An open notification as awl's server (awl_notifications.h) keeps it,
 * read only. Every string is set, "" for none, and valid UTF-8 (D-Bus
 * checks), capped in length: the body at 16 KiB, the rest shorter.
 */
typedef struct awl_notif_t {
    /** its id, unique among the open ones; a replacement keeps it */
    uint32_t id;
    /** grows with every change to it, so new != replaced != unchanged */
    uint64_t generation;
    const char *app, *summary;
    /** raw, with the client's markup (plugins/markup.h parses it) */
    const char* body;
    /** the image, the first of these that yields one (the spec's order):
     * image_data if its data isn't NULL, image_path, app_icon (each an
     * absolute path, a file:// URL or an icon name), desktop_entry's Icon */
    struct {
        /** checked: 8 bits, channels 4 if has_alpha else 3, 1 to 1024
         * pixels on each side, rows packed (stride w * channels) */
        const uint8_t* data;
        int w, h, stride, has_alpha, channels;
    } image_data;
    const char *image_path, *app_icon, *desktop_entry;
    const char* category;
    /** actions[2 * i] the key, actions[2 * i + 1] the label, i < nactions;
     * the key "default" is the click on the notification */
    const char* const* actions;
    int nactions;
    awl_notif_urgency_t urgency;
    /** the ``value`` hint, 0 to 100 (a progress bar); -1 none */
    int value;
    /** AWL_NOTIF_TRANSIENT, AWL_NOTIF_RESIDENT */
    unsigned flags;
    /** ms it stays from its last change, 0 until it is dismissed */
    int timeout;
    /** awl's own look for it, all 0 for a D-Bus one */
    awl_notif_look_t look;
} awl_notif_t;

/**
 * A notification awl sends itself (`awl_host_t.notif_send`): its app is
 * "awl", it gets the AWL_NOTIF_AWL flag. Start from AWL_NOTIF_SEND_INIT.
 */
typedef struct awl_notif_send_t {
    /** NULL for ""; the body with markup (plugins/markup.h) */
    const char *summary, *body;
    /** an absolute path, a file:// URL or an icon name; NULL none */
    const char* image;
    awl_notif_urgency_t urgency;
    /** 0 to 100 (a progress bar); -1 none */
    int value;
    /** ms; 0 until it is dismissed; < 0 the config's for its urgency, as a
     * D-Bus one's (critical: until dismissed) */
    int timeout;
    /** replaces the open one of awl's own with the same tag in place (its
     * id, place and timer's hold stay); NULL none */
    const char* tag;
    awl_notif_look_t look;
} awl_notif_send_t;

#define AWL_NOTIF_SEND_INIT { .urgency = AWL_NOTIF_NORMAL, .value = -1, .timeout = -1 }

/**
 * What awl provides to libawlplugins.so, by topic. All functions are
 * main-thread only, except the first group's (`redraw_request`, `log`,
 * `lock_request`, `logind_state` and `cfg`'s), which any thread may call.
 */
typedef struct awl_host_t {
    /** `AWL_PLUGIN_ABI` as awl was built */
    uint32_t abi;
    /** sizeof_host, sizeof_draw, sizeof_widget and sizeof_config: the
     * layouts the library compiles against. `Monitor` and `Client` aren't
     * among them: the library only passes them back to awl, it never reads
     * them (what a bar's widgets need of its monitor is in `awl_bar_t`). */
    size_t sizeof_host, sizeof_draw, sizeof_widget, sizeof_config;

    /* ---- any thread ---- */

    /** `awl_redraw_request()`, called as ``awl_host->redraw_request()`` */
    void (*redraw_request)( void );
    /** `awl_dbg_printf()`: the libraries' `awl_err_printf()` & co. (awl_log.h)
     * expand to it */
    void (*log)( awl_loglevel_t loglevel, const char* fmt, ... )
        __attribute__(( format( printf, 2, 3 ) ));
    /** `awl_lock_request()`: lock the screen (the config's lock.locker),
     * unless it is or is about to be */
    void (*lock_request)( void );
    /** `awl_lock_logind_state()`: what the logind thread listens for now,
     * `awl_logind_flags` or'ed (0 when it stops); awl raises an alert while
     * that's less than it needs */
    void (*logind_state)( unsigned flags );
    /**
     * config values that can change while awl runs, by name. awl owns the
     * dictionary (``cfg.s``, awl_config.h's ``cfgdict``): it lives as long as awl
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

    /* ---- drawing ---- */

    /** `awl_draw_text()`, text in font into pix, e.g. a bar widget's draw()
     * with its pix and its bar's font */
    int (*text)( pixman_image_t* pix, struct fcft_font* font, int x, int y, unsigned int w,
                 unsigned int h, unsigned int lpad, const char* text, pixman_color_t fg,
                 pixman_color_t bg );
    /** `awl_draw_text_over()`, text in fg (0xRRGGBBAA) over what is in pix,
     * e.g. in a painter's paint(); cut with an ellipsis at w */
    int (*text_over)( pixman_image_t* pix, struct fcft_font* font, int x, int y, unsigned int w,
                      unsigned int h, const char* text, uint32_t fg );
    /** `awl_draw_text_width()` */
    unsigned int (*text_width)( struct fcft_font* font, const char* text );
    /** `awl_font_style()`: font (e.g. a painter's) in bold and/or italic,
     * `AWL_FONT_BOLD`, `AWL_FONT_ITALIC`; font itself if that doesn't load.
     * Give it back with font_release(). */
    struct fcft_font* (*font_style)( struct fcft_font* font, int style );
    /** `awl_font_release()` */
    void (*font_release)( struct fcft_font* font );

    /* ---- awl's state and actions ---- */

    /** `configdefaults()`: fills in c with awl's defaults
     * (awl_config_defaults.h), which plugins_config.c changes; they point into awl, so they stay
     * valid */
    void (*config_defaults)( awl_config_t* c );
    /** what plugins_config.c binds keys and buttons to */
    const awl_actions_t* actions;
    /** what the config's layouts arrange with */
    const awl_arranges_t* arranges;
    /** `focusclient()` */
    void (*focusclient)( Client* c, int lift );
    /** `clientsetvisible()`: shows c, or minimizes it, and arranges its
     * monitor */
    void (*client_set_visible)( Client* c, int visible );
    /** `arrange()` */
    void (*arrange)( Monitor* m );
    /** `monitornamed()`: the monitor of the output named name, NULL if none */
    Monitor* (*monitor_named)( const char* name );
    /** the selected monitor (``selmon``), NULL if there is none */
    Monitor* (*monitor_selected)( void );
    /** m's output scale (what its popups are drawn at), 1 for NULL */
    float (*monitor_scale)( Monitor* m );

    /* ---- popups (awl_window.h), NULL windows are fine everywhere ---- */

    /** `popupcreate()`: a hidden, empty popup, NULL before awl is set up.
     * The library destroys what it made before it is unloaded (its fini);
     * awl removes what it left then, with a warning */
    awl_window_t* (*window_create)( void );
    /** `popupdestroy()` */
    void (*window_destroy)( awl_window_t* win );
    /** `popupsetmenustyle()`: the window looks like a menu,
     * `awl_config_t.popup`'s menu (awl_popup_menu_config()), from now on */
    void (*window_set_menu_style)( awl_window_t* win );
    /** `popupsetpersistent()`: the window stays until it is hidden (a
     * notification): a press elsewhere and other popups shown leave it,
     * showing it closes no menu, and it is above the other popups, over
     * fullscreen clients too if `awl_config_t.popup`'s persistent_overlay;
     * out of sight while the screen is locked, back after. 0: an ordinary
     * popup again. */
    void (*window_set_persistent)( awl_window_t* win, int persistent );
    /** `popupsetcolors()`: the window's own background, text and border
     * colors (0xRRGGBBAA; a painter's content is its own), 0 for the
     * popups' (`awl_config_t.popup`) */
    void (*window_set_colors)( awl_window_t* win, uint32_t bg, uint32_t fg, uint32_t border );
    /** `popuproom()`: the room the window has on m at a spot
     * (window_show_at): m's window area less the window's margins, layout
     * coordinates */
    void (*window_room)( awl_window_t* win, Monitor* m, int* x, int* y, int* w, int* h );
    /** `popupbox()`: where the window was last placed, layout coordinates
     * and logical size, its frame included; returns whether it is visible */
    int (*window_box)( awl_window_t* win, int* x, int* y, int* w, int* h );
    /** `awl_window_set_text()`: rows separated by '\n', columns by '\t';
     * a shown window follows at once */
    void (*window_set_text)( awl_window_t* win, const char* text );
    /** `awl_window_set_painter()`: the window draws itself with painter
     * (awl.h; copied, with data) instead of its text, NULL for
     * the text again. Its parts stand in for the cells: clicks and scrolls
     * get the part as row, column 0. */
    void (*window_set_painter)( awl_window_t* win, const awl_window_painter_t* painter, void* data );
    /** `awl_window_redraw()`: what the painter draws changed */
    void (*window_redraw)( awl_window_t* win );
    /** `popupshow()`: shows the window for a bar widget on m, centered on
     * center_x (logical pixels from m's left edge), on the bar's side; one
     * that isn't a menu closes the menus open */
    void (*window_show)( awl_window_t* win, Monitor* m, int center_x );
    /** `popupshowbeside()`: shows the window on m beside the box x, y, w, h
     * (layout coordinates, logical size), a row of another window: a
     * submenu beside its menu, on the right unless left, else where it fits */
    void (*window_show_beside)( awl_window_t* win, Monitor* m, int x, int y, int w, int h, int left );
    /** `popupshowat()`: shows the window on m at a spot of its window
     * area, an AWL_WINDOW_AT_* (awl.h; the margin away from its edges),
     * moved by dx, dy logical pixels (the strips: over the bar, or along an edge);
     * one that isn't a menu closes the menus open */
    void (*window_show_at)( awl_window_t* win, Monitor* m, int at, int dx, int dy );
    /** `popuphide()` */
    void (*window_hide)( awl_window_t* win );
    /** `awl_window_monitor()`: where the window is shown, NULL while hidden
     * (awl also hides it, on a press elsewhere unless it is persistent, or
     * with its monitor) */
    Monitor* (*window_monitor)( const awl_window_t* win );
    /** `popuppartbox()`: where part of the window's painter is, layout
     * coordinates and logical size; 0 if it is not shown */
    int (*window_part_box)( awl_window_t* win, int part, int* x, int* y, int* w, int* h );
    /** `awl_window_set_click()`: the window takes the pointer, presses go to
     * click (row, col: the cell, -1 off the text; button a BTN_*) */
    void (*window_set_click)( awl_window_t* win,
            void (*click)( awl_window_t* win, int row, int col, uint32_t button, void* data ), void* data );
    /** `awl_window_set_scroll()`: the window takes the pointer, scrolling
     * goes to scroll (row, col as for window_set_click; orientation a
     * WL_POINTER_AXIS_*, 0 vertical; delta positive down or right;
     * value120 a wheel's in 120ths of a notch, 0 for a touchpad's). Like
     * window_set_click, a window with either callback takes all of the
     * pointer: presses and scrolling its callbacks don't take are dropped. */
    void (*window_set_scroll)( awl_window_t* win,
            void (*scroll)( awl_window_t* win, int row, int col, uint32_t orientation,
                            double delta, int32_t value120, void* data ), void* data );
    /** `awl_window_set_hover()`: hover( win, row, inside, data ) whenever
     * the row under the pointer changes (-1: none) or it comes onto the
     * window or leaves it (inside); needs window_set_click's callback */
    void (*window_set_hover)( awl_window_t* win,
            void (*hover)( awl_window_t* win, int row, int inside, void* data ), void* data );
    /** `awl_window_set_hidden()`: hidden( win, data ) whenever the window
     * is hidden, by window_hide or by awl (a press elsewhere, its monitor
     * going away; not a persistent one out of sight while the screen is
     * locked); NULL: no more */
    void (*window_set_hidden)( awl_window_t* win, void (*hidden)( awl_window_t* win, void* data ), void* data );
    /** `popupsetkey()`: while shown, the window has the keyboard (the
     * newest such one): every key pressed, and repeated while held, goes to
     * key( win, sym, utf8, mods, data ) before awl's bindings, which get
     * what it doesn't take (it returns 0); no client gets any, none has the
     * focus meanwhile. sym an XKB_KEY_*, utf8 what the key types ("" for
     * nothing, a control character), mods WLR_MODIFIER_*'s. Locking the
     * screen hides it. NULL: no more. */
    void (*window_set_key)( awl_window_t* win,
            int (*key)( awl_window_t* win, uint32_t sym, const char* utf8, uint32_t mods, void* data ),
            void* data );

    /* ---- notifications (awl_notifications.h) ---- */

    /** the open notifications into out, newest first, at most max; returns
     * how many there are. They stay valid until the library returns to
     * awl's loop or calls notif_close or notif_invoke. */
    int (*notif_list)( const awl_notif_t** out, int max );
    /** closes notification id, if it is open, for reason (the user's
     * dismissal: AWL_NOTIF_DISMISSED) */
    void (*notif_close)( uint32_t id, awl_notif_reason_t reason );
    /** the user chose action key of notification id: ActionInvoked, after
     * ActivationToken if token (an xdg-activation token) isn't NULL; then
     * it closes, unless it is resident */
    void (*notif_invoke)( uint32_t id, const char* key, const char* token );
    /** held: id doesn't expire (the pointer is over it); released, its
     * timeout starts over */
    void (*notif_hold)( uint32_t id, int held );
    /** sends n (copied) as awl's own notification, past D-Bus: it comes
     * whether or not a client could reach the server; returns its id, 0 if
     * it couldn't (out of memory, the server not started) */
    uint32_t (*notif_send)( const awl_notif_send_t* n );

    /* ---- timers on awl's main loop ---- */

    /** a timer calling fn( data ), unarmed; NULL before
     * setup(). Like the popups, the library removes its timers before it is
     * unloaded; awl removes what it left then, with a warning */
    awl_timer_t* (*timer_add)( void (*fn)( void* data ), void* data );
    /** fn in ms milliseconds, instead of when it was due; 0
     * disarms it */
    void (*timer_set)( awl_timer_t* t, int ms );
    /** NULL is fine */
    void (*timer_remove)( awl_timer_t* t );
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
/** awl, as this library sees it: set by `awl_plugin_entry()` (plugins.c)
 * before anything else runs */
extern const awl_host_t* awl_host;
#endif

#ifdef __cplusplus
}
#endif
