#pragma once
/*
 * See LICENSE file for copyright and license details.
 */

/**
 * Every type the plugin library (``libawlplugins.so``) shares with awl:
 * ``Monitor`` and ``Client``, the configuration (`awl_config_t`, which
 * plugins_config.c fills in), the popups' painters, the wallpaper's images,
 * and the headers they need. awl's own types are in awl_core.h, its state
 * and functions in its modules' headers. The library is built against this,
 * so a change to a struct here is a change of the plugin ABI
 * (awl_plugin_abi.h).
 */
#include <errno.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "awl_draw.h"

/* macros */
#ifndef MAX
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#endif
#ifndef MIN
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#endif
/** the number of elements of array X */
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define TEXTW(mon, text)        (awl_draw_text_width((mon)->drw.font, text) + (mon)->lrpad)

/* enums */
/** `awl_config_t.bordercolors` */
enum BorderColor { BorderNorm, BorderSel, BorderUrg, BorderLast };
/** cursor */
enum CursorMode { CurNormal, CurPressed, CurMove, CurResize };
/** where a click went, `Button.click` */
enum ClickRegion { ClkBar, ClkClient, ClkRoot };

/** The argument a key or button binding passes to its function. */
typedef union {
    int i;
    uint32_t ui;
    float f;
    const void *v;
} Arg;

/** A pointer button binding, `awl_config_t.buttons`. */
typedef struct {
    unsigned int click;
    /** WLR_MODIFIER_*, MODKEY for `awl_config_t.modkey` */
    unsigned int mod;
    unsigned int button;
    void (*func)(const Arg *);
    const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct Client Client;
/** A window, XDG shell or XWayland. */
struct Client {
    /* Must keep these three elements in this order */
    unsigned int type; /* XDGShell or X11* */
    struct wlr_box geom; /* layout-relative, includes border */
    Monitor *mon;
    struct wlr_scene_tree *scene;
    struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
    struct wlr_scene_blur *blur;
    float one_minus_alpha;
    struct wlr_scene_tree *scene_surface;
    struct wl_list link;
    struct wl_list flink;
    union {
        struct wlr_xdg_surface *xdg;
        struct wlr_xwayland_surface *xwayland;
    } surface;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener commit;
    struct wl_listener map;
    struct wl_listener maximize;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener set_title;
    struct wl_listener fullscreen;
    struct wl_listener set_decoration_mode;
    struct wl_listener destroy_decoration;
    struct wlr_box prev; /* layout-relative, includes border */
    struct wlr_box bounds;
#ifdef XWAYLAND
    struct wl_listener activate;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener configure;
    struct wl_listener set_hints;
#endif
    unsigned int bw;
    uint32_t tags;
    struct { uint8_t
        isfloating:1,
        isurgent:1,
        isfullscreen:1,
        isvisible:1,
        ismaximized:1,
        isontop:1;
    };
    uint32_t resize; /* configure serial of a pending resize */
};

/** A key binding, `awl_config_t.keys`. */
typedef struct {
    /** WLR_MODIFIER_*, MODKEY for `awl_config_t.modkey` */
    uint32_t mod;
    xkb_keysym_t keysym;
    void (*func)(const Arg *);
    const Arg arg;
} Key;

/** A layout, `awl_config_t.layouts`. */
typedef struct {
    const char *symbol;
    /** NULL: floating */
    void (*arrange)(Monitor *);
} Layout;

typedef struct Buffer Buffer;
/** An output, with its bar. */
struct Monitor {
    struct wl_list link;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wlr_scene_buffer *scene_buffer; /* bar buffer */
    struct wlr_scene_blur *bar_blur; /* behind the bar's window list */
    Buffer *bar_bufs[2]; /* bar render targets, reused; see barbuffer() */
    Buffer *bar_shown; /* the slot scene_buffer shows, NULL if unknown */
    struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
    struct wl_listener frame;
    struct wl_listener destroy;
    struct wl_listener request_state;
    struct wl_listener destroy_lock_surface;
    struct wlr_session_lock_surface_v1 *lock_surface;
    /** a frame showing the lock went out since it began (lockcovered()) */
    int lock_shown;
    /** the alert on this output (`awl_alert_draw()`), and what it was drawn for */
    struct wlr_scene_buffer *alert;
    unsigned int alert_version;
    float alert_scale;
    struct wlr_box alert_area;
    struct wlr_box m; /* monitor area, layout-relative */
    struct {
        int width, height;
        int real_width, real_height; /* non-scaled */
        float scale;
    } b; /* bar area */
    struct wlr_box w; /* window area, layout-relative */
    struct wl_list layers[4]; /* LayerSurface.link */
    unsigned int seltags;
    uint32_t tagset[2];
    int gamma_lut_changed;
    uint8_t showbar:1;
    uint8_t closedbar:1;
    /** whether each view keeps its own layout and its settings in ptlt[]
     * (MonitorRule.pertag, togglepertag); if not, all share ptlt[0] */
    uint8_t pertag:1;
    char ltsymbol[16];
    /** the layouts and their settings, read through monlt(): with pertag,
     * [1+t] for tag t viewed alone and [0] for views of several tags;
     * without, [0] for all views */
    struct PertagLayout {
        /** the selected layout lt[sellt] and the previous one (setlayout()) */
        const Layout *lt[2];
        unsigned int sellt;
        /** master area share (setmfact()) and number of master clients
         * (incnmaster()) */
        float mfact;
        int nmaster;
    } ptlt[32];
    int asleep;
    /** the bar: its font, its widgets and what they show; drw.m is NULL
     * before createmon() sets it up and after cleanupmon() */
    awl_draw_t drw;
    /** the popups' font at this monitor's scale (awl_window.h,
     * cfg->popup.font and .fontsize), from the font registry; NULL while
     * they use the bar's */
    struct fcft_font *popupfont;
    /** the desktop file list's (awl_desktop.h, cfg->desktop.font and
     * .fontsize), the same way; NULL while it uses the bar's */
    struct fcft_font *desktopfont;
    /** the bar's text padding, left and right together */
    int lrpad;
};

/** A rule for outputs whose name contains name (NULL: any),
 * `awl_config_t.monrules`; the first that matches counts. */
typedef struct {
    const char *name;
    float mfact;
    int nmaster;
    float scale;
    /** the index of its layout in `awl_config_t.layouts` (out of range: 0) */
    int layout;
    enum wl_output_transform rr;
    /** its position; -1, -1 places it automatically (other negative values
     * trouble Xwayland clients) */
    int x, y;
    /** whether each tag gets its own layout (see togglepertag); 0 if left out */
    int pertag;
} MonitorRule;

/** A rule for new windows whose app id and title contain id and title (NULL:
 * any), `awl_config_t.rules`; every rule that matches counts. */
typedef struct {
    const char *id;
    const char *title;
    uint32_t tags;
    short isfloating;
    short monitor;
    int w, h;
    int blur;
    float one_minus_alpha;
} Rule;

/**
 * which wallpaper the wallpaper action (`Arg.i`) and the timer switch to; the
 * ones after `WallpaperModeCount` are no timer modes: back to the one shown
 * before, where the timer goes next, and the way back from there (for
 * `WallpaperRand`, `WallpaperBack`)
 */
typedef enum { WallpaperNext, WallpaperPrev, WallpaperRand, WallpaperModeCount,
               WallpaperBack, WallpaperTimerNext, WallpaperTimerBack } WallpaperMode;

/** A decoded wallpaper, from the library's thread (plugins/wallpaper.h) to
 * awl's background (awl_background.h): premultiplied ARGB8888
 * (``DRM_FORMAT_ARGB8888``), rows of stride bytes; one ``malloc()``, freed
 * by whoever has it last. */
typedef struct awl_image_t {
    int width, height, stride;
    uint8_t data[];
} awl_image_t;

/** awl's default `awl_popup_config_t.column_spacing` and row_spacing */
#define AWL_WINDOW_COLUMN_SPACING 12
#define AWL_WINDOW_ROW_SPACING 2
/** the last row of a popup with more rows than fit */
#define AWL_WINDOW_ELLIPSIS "…"

/**
 * A window that draws what is inside its frame itself (a calendar, a menu),
 * awl_window_set_painter(): instead of text and cells, parts the painter
 * numbers itself. Everything in physical pixels, positions from the content's
 * top left (inside the border and the padding). Main thread; font is the
 * popups' of the monitor the window is on (Monitor.popupfont, else the
 * bar's), at that monitor's scale, never NULL.
 */
typedef struct awl_window_painter_t {
    /** the content's size in font at scale, at most max_w x max_h, into
     * *w, *h; returns 0 if there is nothing to show */
    int (*measure)( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h );
    /** paints the content as measure() last laid it out, its top left at
     * x0, y0 of pix (the window's image, the frame already in it); hover the
     * part the pointer is over, -1 none */
    void (*paint)( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover );
    /** the part at px, py, -1 none: what the pointer hovers, and the row
     * clicks and scrolls report (their column 0, or -1 with the row) */
    int (*hit)( void* data, int px, int py );
    /** where part is, for awl_window_cell_center(); 0 if there is no such
     * part. May be NULL. */
    int (*box)( void* data, int part, int* x, int* y, int* w, int* h );
} awl_window_painter_t;

/** `awl_config_t.popup`: how awl's own popups (awl_window.h) look, the bar
 * widgets' (the clock's calendar, the IP addresses) and awl's. Colors
 * 0xRRGGBBAA, sizes logical pixels. What only the plugins draw with is in
 * their own config (plugins.h, `awl_plugin_config_t.popup`). */
typedef struct awl_popup_config_t {
    uint32_t bg, fg, border;
    /** the row under the pointer, the buttons' background */
    uint32_t hover;
    int border_width, radius, padding;
    /** the gap to the bar and to the monitor's sides, see awl_window_margin() */
    int margin;
    /** between a text popup's columns and rows */
    int column_spacing, row_spacing;
    /** the font, as `awl_config_t.font` and fontsize (size appended to
     * font); NULL and 0 each take the bar's */
    const char *font;
    int fontsize;
    /** the tray's menus, their own look; what is 0 (left out) is the
     * popups': bg, fg, border, highlight (hover), border_width, radius and
     * blur (blur.launcher). A negative border_width, radius or blur is
     * none. */
    struct awl_popup_menu_config_t {
        uint32_t bg, fg, border;
        /** the entry under the pointer, the one whose submenu is open */
        uint32_t highlight;
        int border_width, radius;
        /** blur what is behind them, as the launcher */
        int blur;
    } menu;
} awl_popup_config_t;

/** c's menu look with what is left out filled in from the popups' (see
 * menu), negative sizes and blur as 0; blur_launcher is the popups' blur
 * (`awl_config_t.blur`'s launcher) */
static inline struct awl_popup_menu_config_t awl_popup_menu_config( const awl_popup_config_t* c, int blur_launcher ) {
    struct awl_popup_menu_config_t m = c->menu;
    if (!m.bg) m.bg = c->bg;
    if (!m.fg) m.fg = c->fg;
    if (!m.border) m.border = c->border;
    if (!m.highlight) m.highlight = c->hover;
    m.border_width = m.border_width < 0 ? 0 : m.border_width ? m.border_width : c->border_width;
    m.radius = m.radius < 0 ? 0 : m.radius ? m.radius : c->radius;
    m.blur = m.blur < 0 ? 0 : m.blur ? m.blur : blur_launcher;
    return m;
}

/** `awl_config_t.tray`; the tray widget's own look is the plugins'
 * (`awl_plugin_config_t.tray`). */
typedef struct awl_tray_config_t {
    /** The icon theme of the tray's icons and their menus, e.g.
     * "Papirus-Dark" (NULL or "": the desktop's). An icon it lacks, or all
     * of them if it isn't installed, comes from the desktop's theme
     * (gtk-3.0/settings.ini), then Adwaita and hicolor. Programs keep their
     * theme, unless started with TRAY_ICONS. Icons an item
     * sends as images, or looks up in its own IconThemePath, stay as they
     * are. */
    const char *icon_theme;
} awl_tray_config_t;

/** `awl_config_t.desktop`, the desktop file list (awl_desktop.h); it is
 * blurred like the launcher (`awl_config_t.blur`) */
typedef struct awl_desktop_config_t {
    /** the font, as `awl_config_t.font` and fontsize (size appended to
     * font); NULL and 0 each take the bar's */
    const char *font;
    int fontsize;
} awl_desktop_config_t;

/** `awl_config_t.wallpaper`: the wallpaper action ({.i = WallpaperNext, ...,
 * see `WallpaperMode`}) shows one of dir's ``*.png`` files, scaled to cover
 * each monitor; so does a timer every interval seconds, going where mode
 * says (the wallpapermode action cycles it) */
typedef struct awl_wallpaper_config_t {
    /** relative to ``$HOME`` unless absolute; NULL: no wallpaper. The config
     * dictionary's ``wallpaper_dir`` starts as awl's default's; the
     * wallpaper thread reads that key, so a reloaded plugins_config.c's dir
     * doesn't count. */
    const char *dir;
    /** from one to the next, 0: switch at once */
    unsigned int fade_ms;
    /** seconds between the timer's changes, 0: none */
    unsigned int interval;
    /** what the timer switches to, until the wallpapermode action */
    WallpaperMode mode;
} awl_wallpaper_config_t;

/** in a binding's mod: `awl_config_t.modkey`, whatever it is when the
 * binding is pressed */
#define MODKEY (1u << 31)

/**
 * The configuration that can change while awl runs: awl's defaults
 * (awl_defaults.c, `awl_host_t.config_defaults`) with plugins_config.c's
 * changes, built into the plugin library, so `make` and plugin_restart
 * apply them; while no library is loaded, the defaults alone. What only a
 * restart applies are config.mk's compile-time switches.
 *
 * Only what awl itself reads is here (the library may read it too); what
 * only the plugins read is their own `awl_plugin_config_t` (plugins.h),
 * which plugins_config.c changes alike. Everything it points to is the
 * library's or awl's, so awl copies what it keeps past a reload. Monitor
 * rules apply to outputs appearing afterwards.
 */
typedef struct awl_config_t {
    /** what MODKEY in a binding stands for, a WLR_MODIFIER_* */
    uint32_t modkey;
    /** a list is v[0] to v[n - 1]; set one to an array with SETLIST() */
    struct { const Key *v; size_t n; } keys;
    /** clicks on the bar go to its widgets, on the bare desktop to the
     * desktop (left/right/middle: previous/next/random wallpaper; with
     * Shift: show/hide the file list, cycle the wallpaper timer, hidden
     * files); these bindings come first */
    struct { const Button *v; size_t n; } buttons;
    struct { const Rule *v; size_t n; } rules;
    /** at least one; MonitorRule.layout and setlayout take an index into
     * them. After a reload every monitor keeps the layout at its index. */
    struct { const Layout *v; size_t n; } layouts;
    struct { const MonitorRule *v; size_t n; } monrules;

    /** the bar: whether new outputs show it (togglebar hides and shows
     * it; a reload that changes this shows or hides every bar), and
     * whether at the top or at the bottom */
    struct { int show, top; } bar;
    /** how many tags there are, 1 to 31; the bar shows their numbers */
    int ntags;
    /** focus follows the pointer */
    int sloppyfocus;
    /** idle inhibitors inhibit even while their surface isn't visible */
    int bypass_surface_visibility;
    /** windows' borders: width and colors (0xRRGGBBAA) */
    unsigned int borderpx;
    uint32_t bordercolors[BorderLast];
    /** the bar's font, its size appended to it */
    const char *font;
    int fontsize;
    /** behind everything, and behind fullscreen windows (0xRRGGBBAA; an
     * alpha of 0 shows what is below) */
    uint32_t rootcolor, fullscreen_bg;
    /** every blur's strength and alpha: windows' (rules), notifications',
     * the launcher's, the desktop file list's and the lock screen's; and
     * whether to blur behind layer surfaces of the namespaces
     * "notifications" and "launcher" (the launcher's also for the desktop
     * file list and the popups), with corner radius; windowlist: the bar's
     * window list shows inactive and minimized windows translucent over a
     * blur, the focused one opaque */
    struct {
        float strength, alpha;
        int notifications, notifications_radius;
        int launcher, launcher_radius;
        int windowlist;
    } blur;
    /** the keymap (rules, model, layout, variant, options; NULL: the
     * default) and the key repeat */
    struct {
        struct xkb_rule_names xkb;
        int repeat_rate, repeat_delay;
    } keyboard;
    /** libinput's settings for pointer devices (touchpads, mice) */
    struct {
        int tap_to_click, tap_and_drag, drag_lock, natural_scrolling, disable_while_typing,
            left_handed, middle_button_emulation;
        /** LIBINPUT_CONFIG_SCROLL_NO_SCROLL, _2FG, _EDGE or _ON_BUTTON_DOWN */
        enum libinput_config_scroll_method scroll_method;
        /** LIBINPUT_CONFIG_CLICK_METHOD_NONE, _BUTTON_AREAS or _CLICKFINGER */
        enum libinput_config_click_method click_method;
        /** LIBINPUT_CONFIG_SEND_EVENTS_ENABLED, _DISABLED or
         * _DISABLED_ON_EXTERNAL_MOUSE */
        uint32_t send_events_mode;
        /** LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT or _ADAPTIVE, and the speed */
        enum libinput_config_accel_profile accel_profile;
        double accel_speed;
        /** LIBINPUT_CONFIG_TAP_MAP_LRM or _LMR: 1/2/3 finger taps as
         * left/right/middle or left/middle/right */
        enum libinput_config_tap_button_map button_map;
    } pointer;

    /** the alert (awl_alert.h) on every output's bottom left corner: its
     * text and background (0xRRGGBBAA), its opacity, its font size relative
     * to the bar's, its distance from the corner */
    struct {
        uint32_t fg, bg;
        float opacity, font_scale;
        int margin;
    } alert;

    /** the screen lock: locker, an ext-session-lock client (argv, NULL
     * terminated), is started on logind's Lock (loginctl lock-session),
     * before suspend and by the lockscreen action; at_start: lock right
     * after awl started; on_suspend: suspend waits until the locker covers
     * every output (a logind delay inhibitor); blur: awl blurs the screen
     * under the locker */
    struct {
        const char *const *locker;
        int at_start, on_suspend, blur;
    } lock;
    /** the programs started with awl, each an argv (NULL terminated, at
     * most 7 words), terminated when it exits; only at awl's start, a reload
     * starts none. A first word TRAY_ICONS starts the rest in
     * tray.icon_theme instead of the desktop's, see there */
    struct { const char *(*v)[8]; size_t n; } autostart;
    /** what wlroots logs: WLR_SILENT, WLR_ERROR, WLR_INFO or WLR_DEBUG
     * (awl -d: WLR_DEBUG anyway) */
    int wlr_log_level;

    awl_tray_config_t tray;
    awl_popup_config_t popup;
    awl_desktop_config_t desktop;
    awl_wallpaper_config_t wallpaper;
} awl_config_t;

/** an argv's first word (an autostart's, a spawn binding's) that starts
 * the rest in the tray's icon theme instead of the desktop's: for GTK
 * programs whose tray menus draw their own icons, like nm-applet. Only that
 * program gets it, through a dconf profile of its own in memory, not what
 * it starts in turn (libawltray-env.so next to awl, preloaded into it,
 * sees to that). */
#define TRAY_ICONS "awl:tray-icons"

/** sets the list l (`awl_config_t.keys` & co.) to the array a */
#define SETLIST(l, a)           ((l).v = (a), (l).n = LENGTH(a))
/** the four bindings of tag TAG: view, toggleview, tag, toggletag */
#define TAGKEYS(KEY,SKEY,TAG) \
    { MODKEY,                    KEY,            view,            {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_CTRL,  KEY,            toggleview,      {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_SHIFT, SKEY,           tag,             {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,SKEY,toggletag, {.ui = 1ul << TAG} }
/** Ctrl-Alt-Fn: to VT n */
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_XF86Switch_VT_##n, chvt, {.ui = (n)} }
/** spawn's argument for a shell command */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

static inline int awl_localtime(char buf[32]) {
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) || !strftime(buf, 32, "%y/%m/%d-%H:%M:%S", &tm))
        return 1;
    return 0;
}
