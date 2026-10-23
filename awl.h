#pragma once
/*
 * See LICENSE file for copyright and license details.
 */

/**
 * Every type the plugin library (``libawlplugins.so``) shares with awl:
 * :c:type:`Monitor` and :c:type:`Client`, the configuration (:c:type:`awl_config_t`, which
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

#include "awl_bar_type.h"

/* macros */
#ifndef MAX
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#endif
#ifndef MIN
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#endif
/** the number of elements of array X */
#define LENGTH(X)               (sizeof X / sizeof X[0])

/* enums */
/** :c:type:`awl_colors_t`'s border, border_focused and border_urgent, in this order */
enum BorderColor { BorderNorm, BorderSel, BorderUrg,
    /** how many there are */
    BorderLast };
/** cursor */
enum CursorMode { CurNormal, CurPressed, CurMove, CurResize };
/** where a click went, :c:member:`Button.click` */
enum ClickRegion { ClkBar, ClkClient, ClkRoot };

/** The argument a key or button binding passes to its function. */
typedef union {
    /** an int: a direction, a count, an index */
    int i;
    /** a mask, e.g. of tags */
    uint32_t ui;
    /** a float: a factor, an amount */
    float f;
    /** anything else, e.g. an argv */
    const void *v;
} Arg;

/** A pointer button binding, :c:member:`awl_config_t.buttons`. */
typedef struct {
    /** where it was, a :c:enum:`ClickRegion` */
    unsigned int click;
    /** WLR_MODIFIER_*, :c:macro:`MODKEY` for :c:member:`awl_config_t.modkey` */
    unsigned int mod;
    /** a BTN_* */
    unsigned int button;
    /** what it does, called with arg */
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

/** A key binding, :c:member:`awl_config_t.keys`. */
typedef struct {
    /** WLR_MODIFIER_*, :c:macro:`MODKEY` for :c:member:`awl_config_t.modkey` */
    uint32_t mod;
    xkb_keysym_t keysym;
    /** what it does, called with arg */
    void (*func)(const Arg *);
    const Arg arg;
} Key;

/** A layout, :c:member:`awl_config_t.layouts`. */
typedef struct {
    const char *symbol;
    /** NULL: floating */
    void (*arrange)(Monitor *);
} Layout;

/** a view's layout and its settings, :c:member:`Monitor.ptlt` */
struct PertagLayout {
    /** the selected layout lt[sellt] and the previous one (:c:func:`setlayout`) */
    const Layout *lt[2];
    unsigned int sellt;
    /** master area share (setmfact()) and number of master clients
     * (incnmaster()) */
    float mfact;
    int nmaster;
};

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
    uint32_t bar_over; /* how far the bar's widgets overflow it, see baroverflow() */
    struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
    struct wl_listener frame;
    struct wl_listener destroy;
    struct wl_listener request_state;
    /** its lock surface is gone */
    struct wl_listener destroy_lock_surface;
    struct wlr_session_lock_surface_v1 *lock_surface;
    /** a frame showing the lock went out since it began (:c:func:`lockcovered`) */
    int lock_shown;
    /** the alert on this output (:c:func:`awl_alert_draw`), and what it was drawn for */
    struct wlr_scene_buffer *alert;
    unsigned int alert_version;
    float alert_scale;
    struct wlr_box alert_area;
    struct wlr_box m; /* monitor area, layout-relative */
    struct {
        int width, height;
        int real_width, real_height; /* non-scaled */
        float scale;
    } bar_area; /* the bar's size; scale: the one its font was loaded for */
    struct wlr_box w; /* window area, layout-relative */
    /** its layer surfaces by layer, background to overlay
     * (LayerSurface.link) */
    struct wl_list layers[4];
    unsigned int seltags;
    uint32_t tagset[2];
    int gamma_lut_changed;
    uint8_t showbar:1;
    uint8_t closedbar:1;
    /** whether each view keeps its own layout and its settings in ptlt[]
     * (:c:member:`MonitorRule.pertag`, togglepertag); if not, all share ptlt[0] */
    uint8_t pertag:1;
    char ltsymbol[16];
    /** the layouts and their settings, read through monlt(): with pertag,
     * [1+t] for tag t viewed alone and [0] for views of several tags;
     * without, [0] for all views */
    struct PertagLayout ptlt[32];
    int asleep;
    /** the bar: its font, its widgets and what they show; bar.m is NULL
     * before createmon() sets it up and after cleanupmon() */
    awl_bar_t bar;
    /** the popups' font at this monitor's scale (awl_window.h,
     * cfg->popup.font and .fontsize), from the font registry; NULL while
     * they use the bar's */
    struct fcft_font *popupfont;
    /** the desktop file list's (awl_desktop.h, cfg->desktop.font and
     * .fontsize), the same way; NULL while it uses the bar's */
    struct fcft_font *desktopfont;
};

/** A rule for outputs whose name contains name (NULL: any),
 * :c:member:`awl_config_t.monrules`; the first that matches counts. */
typedef struct {
    const char *name;
    float mfact;
    int nmaster;
    float scale;
    /** the index of its layout in :c:member:`awl_config_t.layouts` (out of range: 0) */
    int layout;
    enum wl_output_transform rr;
    /** its position; -1, -1 places it automatically (other negative values
     * trouble Xwayland clients) */
    int x, y;
    /** whether each tag gets its own layout (see togglepertag); 0 if left out */
    int pertag;
} MonitorRule;

/** An output's part in an :c:type:`OutputProfile`: the first one whose match is
 * part of an output's name (eDP-1, DP-3) or of its description (make,
 * model, serial, as wlr-randr lists them) counts for it. */
typedef struct {
    const char *match;
    /** on: enabled (in its preferred mode if it was off), at x, y (layout
     * coordinates), scaled by scale (0: as it is), turned by rr; off:
     * disabled, the rest ignored */
    int on;
    int x, y;
    float scale;
    enum wl_output_transform rr;
} OutputSetting;

/** A layout of the outputs, :c:member:`awl_config_t.outputprofiles`, which the
 * outputprofile action applies by name (its .v). Outputs none of its
 * settings match stay as they are. It is refused (a notification says why)
 * while an output it turns on isn't there, or if it would leave no output
 * on: outputs are sometimes found only a while after they are plugged in. */
typedef struct {
    const char *name;
    struct { const OutputSetting *v; size_t n; } outputs;
} OutputProfile;

/** An environment variable awl sets for what it starts,
 * :c:member:`awl_config_t.environment`: value NULL unsets it. */
typedef struct {
    const char *name, *value;
} EnvVar;

/** A rule for new windows whose app id and title contain id and title (NULL:
 * any), :c:member:`awl_config_t.rules`; every rule that matches counts. */
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
 * which wallpaper the wallpaper action (:c:member:`Arg.i`) and the timer switch to; the
 * ones after :c:enum:`WallpaperModeCount <WallpaperMode>` are no timer modes: back to the one shown
 * before, where the timer goes next, and the way back from there (for
 * :c:enum:`WallpaperRand <WallpaperMode>`, :c:enum:`WallpaperBack <WallpaperMode>`)
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

/** awl's default :c:member:`awl_popup_config_t.column_spacing` and row_spacing */
#define AWL_WINDOW_COLUMN_SPACING 12
#define AWL_WINDOW_ROW_SPACING 2
/** the last row of a popup with more rows than fit */
#define AWL_WINDOW_ELLIPSIS "…"

/**
 * A window that draws what is inside its frame itself (a calendar, a menu),
 * :c:func:`awl_window_set_painter`: instead of text and cells, parts the painter
 * numbers itself. Everything in physical pixels, positions from the content's
 * top left (inside the border and the padding). Main thread; font is the
 * popups' of the monitor the window is on (:c:member:`Monitor.popupfont`, else the
 * bar's), at that monitor's scale, never NULL.
 */
typedef struct awl_window_painter_t {
    /** the content's size in font at scale, at most max_w x max_h, into
     * ``*w``, ``*h``; returns 0 if there is nothing to show */
    int (*measure)( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h );
    /** paints the content as :c:member:`measure() <awl_window_painter_t.measure>` last laid it out, its top left at
     * x0, y0 of pix (the window's image, the frame already in it); hover the
     * part the pointer is over, -1 none */
    void (*paint)( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover );
    /** the part at px, py, -1 none: what the pointer hovers, and the row
     * clicks and scrolls report (their column 0, or -1 with the row) */
    int (*hit)( void* data, int px, int py );
    /** where part is, for :c:func:`awl_window_cell_center`; 0 if there is no such
     * part. May be NULL. */
    int (*box)( void* data, int part, int* x, int* y, int* w, int* h );
} awl_window_painter_t;

/** where :c:func:`awl_window_show_at` puts a window in its monitor's window area */
enum {
    AWL_WINDOW_AT_CENTER,
    /** centered, at the top: a launcher's place */
    AWL_WINDOW_AT_TOP,
    /** the corners: notifications' places */
    AWL_WINDOW_AT_TOP_LEFT,
    AWL_WINDOW_AT_TOP_RIGHT,
    AWL_WINDOW_AT_BOTTOM_LEFT,
    AWL_WINDOW_AT_BOTTOM_RIGHT,
    /** strips: as wide as the monitor, as high as its bar, flush, square
     * and without a border or padding (the painter's measure() gets that
     * size as max_w, max_h); over the bar (where it is, shown or not), or
     * along the monitor's top or bottom edge, whichever the bar's is */
    AWL_WINDOW_AT_BAR,
    /** the strip along the monitor's top edge */
    AWL_WINDOW_AT_STRIP_TOP,
    /** the strip along the monitor's bottom edge */
    AWL_WINDOW_AT_STRIP_BOTTOM,
};

/** the molokai palette, 0xRRGGBBAA, which the defaults' colors come from */
#define molokai_blue 0x66d9efffu
#define molokai_red 0xf92672ffu
#define molokai_green 0xa6e22effu
#define molokai_orange 0xfd971fffu
#define molokai_purple 0xae81ffffu
#define molokai_gray 0x232526ffu
#define molokai_dark_gray 0x1b1d1effu
#define molokai_light_gray 0x455354ffu

/** :c:member:`awl_config_t.colors`: the colors awl itself draws with, 0xRRGGBBAA
 * (the plugins' own are :c:type:`awl_plugin_colors_t`, plugins.h). Where one says
 * "0: ...", 0 takes that instead. */
typedef struct awl_colors_t {
    /** behind everything, and behind fullscreen windows (an alpha of 0
     * shows what is below) */
    uint32_t root, fullscreen;
    /** windows' borders: the others', the focused one's, an urgent one's */
    uint32_t border, border_focused, border_urgent;
    /** awl's own popups (awl_window.h), the bar widgets' (the clock's
     * calendar, the IP addresses) and awl's: background, text, border; the
     * row under the pointer and the buttons' background; what matters less
     * (the calendar's neighbouring months' days, the IP popup's missing
     * routes) */
    uint32_t popup_bg, popup_fg, popup_border, popup_hover, popup_dim;
    /** the tray's and the notifications' menus, each 0: the popups'
     * (highlight: popup_hover, highlight_fg: menu_fg): background, text,
     * border; the entry under the pointer (and the one whose submenu is
     * open) and its text; disabled entries, shortcuts and separators */
    uint32_t menu_bg, menu_fg, menu_border, menu_highlight, menu_highlight_fg, menu_dim;
    /** the alert (awl_alert.h): its text and background */
    uint32_t alert_fg, alert_bg;
} awl_colors_t;

/** :c:member:`awl_config_t.popup`: how awl's own popups (awl_window.h) look, the bar
 * widgets' (the clock's calendar, the IP addresses) and awl's; their
 * colors are :c:type:`awl_colors_t`'s popup_*. Sizes logical pixels. */
typedef struct awl_popup_config_t {
    int border_width, radius, padding;
    /** the gap to the bar and to the monitor's sides, see :c:func:`awl_window_margin` */
    int margin;
    /** between a text popup's columns and rows */
    int column_spacing, row_spacing;
    /** the font, as :c:member:`awl_config_t.font` and fontsize (size appended to
     * font); NULL and 0 each take the bar's */
    const char *font;
    int fontsize;
    /** the clock's calendar's event list, which scrolls beyond the
     * maximum (0: none) */
    struct {
        int list_min_height, list_max_height;
    } calendar;
    /** the tray's and the notifications' menus, their own look (colors:
     * :c:type:`awl_colors_t`'s menu_*); what is 0 (left out) is the popups':
     * border_width, radius and blur (blur.popups, blur what is behind
     * them). A negative one is none. */
    struct {
        int border_width, radius, blur;
    } menu;
    /** the persistent popups (the notifications) over fullscreen clients
     * too, in the overlay layer; 0: under them, as the other popups */
    int persistent_overlay;
} awl_popup_config_t;

/** a menu's look, :c:func:`awl_menu_style` */
typedef struct awl_menu_style_t {
    uint32_t bg, fg, border, highlight, highlight_fg, dim;
    int border_width, radius, blur;
} awl_menu_style_t;

/** the menus' look (:c:type:`awl_colors_t`'s menu_*, :c:member:`awl_popup_config_t.menu`)
 * with what is left out filled in from the popups', negative sizes and blur
 * as 0; blur_popups is the popups' blur (:c:member:`awl_config_t.blur`'s
 * popups) */
static inline awl_menu_style_t awl_menu_style( const awl_colors_t* k, const awl_popup_config_t* c,
                                               int blur_popups ) {
    awl_menu_style_t m = {
        .bg = k->menu_bg ? k->menu_bg : k->popup_bg,
        .fg = k->menu_fg ? k->menu_fg : k->popup_fg,
        .border = k->menu_border ? k->menu_border : k->popup_border,
        .highlight = k->menu_highlight ? k->menu_highlight : k->popup_hover,
        .dim = k->menu_dim ? k->menu_dim : k->popup_dim,
        .border_width = c->menu.border_width < 0 ? 0 : c->menu.border_width ? c->menu.border_width : c->border_width,
        .radius = c->menu.radius < 0 ? 0 : c->menu.radius ? c->menu.radius : c->radius,
        .blur = c->menu.blur < 0 ? 0 : c->menu.blur ? c->menu.blur : blur_popups,
    };
    m.highlight_fg = k->menu_highlight_fg ? k->menu_highlight_fg : m.fg;
    return m;
}

/** :c:member:`awl_config_t.tray`, the tray widget and its programs */
typedef struct awl_tray_config_t {
    /** The icon theme of the tray's icons and their menus, e.g.
     * "Papirus-Dark" (NULL or "": the desktop's). An icon it lacks, or all
     * of them if it isn't installed, comes from the desktop's theme
     * (gtk-3.0/settings.ini), then Adwaita and hicolor. Programs keep their
     * theme, unless started with :c:macro:`TRAY_ICONS`. Icons an item
     * sends as images, or looks up in its own IconThemePath, stay as they
     * are. */
    const char *icon_theme;
    /** the widget's icons' size and the gap between them (its background
     * is :c:member:`awl_plugin_colors_t.tray_bg`) */
    float icon_size, icon_gap;
} awl_tray_config_t;

/** :c:member:`awl_config_t.desktop`, the desktop file list (awl_desktop.h); it is
 * blurred like the popups (:c:member:`awl_config_t.blur`) */
typedef struct awl_desktop_config_t {
    /** the font, as :c:member:`awl_config_t.font` and fontsize (size appended to
     * font); NULL and 0 each take the bar's */
    const char *font;
    int fontsize;
    /** the alpha of hidden files' names (the colors are :c:type:`awl_plugin_colors_t`'s
     * desktop_*), its distance from the usable area's corner */
    uint8_t hidden_alpha;
    float margin;
    /** its corner radius, logical pixels */
    int radius;
    /** how many columns the entries are spread over, evenly; the files
     * first, then the folders from the first column on again */
    int columns;
} awl_desktop_config_t;

/** :c:member:`awl_config_t.wallpaper`: the wallpaper action ({.i = WallpaperNext, ...,
 * see :c:enum:`WallpaperMode`}) shows one of dir's ``*.png`` files, scaled to cover
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

/** in a binding's mod: :c:member:`awl_config_t.modkey`, whatever it is when the
 * binding is pressed */
#define MODKEY (1u << 31)

/**
 * The configuration that can change while awl runs: awl's defaults
 * (awl_config_defaults.h, :c:member:`awl_host_t.config_defaults`) with plugins_config.c's
 * changes, built into the plugin library, so ``ninja -C build`` and plugin_restart
 * apply them; while no library is loaded, the defaults alone. What only a
 * restart applies are meson.options' compile-time switches.
 *
 * What awl itself reads is here (the library may read it too), each group
 * of settings whole even where awl reads only part of it; what only the
 * plugins read is their own :c:type:`awl_plugin_config_t` (plugins.h), which
 * plugins_config.c changes alike. Everything it points to is the
 * library's or awl's, so awl copies what it keeps past a reload. Monitor
 * rules apply to outputs appearing afterwards.
 */
typedef struct awl_config_t {
    /** what :c:macro:`MODKEY` in a binding stands for, a WLR_MODIFIER_* */
    uint32_t modkey;
    /** a list is v[0] to v[n - 1]; set one to an array with :c:macro:`SETLIST() <SETLIST>` */
    struct { const Key *v; size_t n; } keys;
    /** clicks on the bar go to its widgets, on the bare desktop to the
     * desktop (left/right/middle: previous/next/random wallpaper; with
     * Shift: show/hide the file list, cycle the wallpaper timer, hidden
     * files); these bindings come first */
    struct { const Button *v; size_t n; } buttons;
    /** the window rules: every one that matches a new window applies, in
     * order */
    struct { const Rule *v; size_t n; } rules;
    /** at least one; :c:member:`MonitorRule.layout` and setlayout take an index into
     * them. After a reload every monitor keeps the layout at its index. */
    struct { const Layout *v; size_t n; } layouts;
    /** the output rules: the first one that matches a new output applies */
    struct { const MonitorRule *v; size_t n; } monrules;
    /** the outputprofile action's layouts */
    struct { const OutputProfile *v; size_t n; } outputprofiles;
    /** awl's environment, which what it starts inherits: set at the start
     * and on every reload; a variable a reload drops goes back to what it
     * was before awl changed it. Of these, DISPLAY, WAYLAND_DISPLAY and
     * XDG_CURRENT_DESKTOP also go to systemd's and the session bus'
     * environment: at the start, and again on a reload that changes one.
     * GRIM_DEFAULT_DIR, unless it is here, is the config dictionary's
     * desktop_dir. */
    struct { const EnvVar *v; size_t n; } environment;

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
    /** the colors awl draws with: the windows', the popups', the menus' */
    awl_colors_t colors;
    /** windows' borders' width */
    unsigned int borderpx;
    /** the bar's font, its size appended to it */
    const char *font;
    int fontsize;
    /** the bar's emoji, at fontsize: a color font, so the widgets' emoji
     * don't come from whatever text font has them in black and white
     * (fontconfig's generic "emoji" is Noto Color Emoji or the like); NULL,
     * or one that isn't in color: text instead of emoji */
    const char *emoji_font;
    /** every blur's strength and alpha: windows' (rules), the popups'
     * (the notifications', the launcher's, the menus'), the desktop file
     * list's and the lock screen's; popups: whether to blur behind the
     * popups, the menus (:c:member:`awl_popup_config_t.menu` may differ) and the
     * desktop file list; windowlist: the bar's
     * window list shows inactive and minimized windows translucent over a
     * blur, the focused one opaque */
    struct {
        float strength, alpha;
        int popups;
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

    /** the alert (awl_alert.h) in every output's window area's bottom left
     * corner: its opacity, its font size relative to the bar's, its
     * distance from the corner (its colors are :c:type:`awl_colors_t`'s alert_*) */
    struct {
        float opacity, font_scale;
        int margin;
    } alert;

    /** the screen lock */
    struct {
        /** an ext-session-lock client (argv, NULL terminated), started on
         * logind's Lock (loginctl lock-session), before suspend and by the
         * lockscreen action */
        const char *const *locker;
        /** lock right after awl started */
        int at_start;
        /** suspend waits until the locker covers every output (a logind
         * delay inhibitor) */
        int on_suspend;
        /** awl blurs the screen under the locker */
        int blur;
    } lock;
    /** the programs started with awl, each an argv (NULL terminated, at
     * most 7 words), terminated when it exits; only at awl's start, a reload
     * starts none. A first word :c:macro:`TRAY_ICONS` starts the rest in
     * tray.icon_theme instead of the desktop's, see there */
    struct { const char *(*v)[8]; size_t n; } autostart;
    /** what wlroots logs: WLR_SILENT, WLR_ERROR, WLR_INFO or WLR_DEBUG
     * (awl -d: WLR_DEBUG anyway) */
    int wlr_log_level;

    /** the tray, see there */
    awl_tray_config_t tray;
    /** the popups: the notifications, the menus, the tooltips */
    awl_popup_config_t popup;
    /** the desktop's file list */
    awl_desktop_config_t desktop;
    /** the wallpaper */
    awl_wallpaper_config_t wallpaper;

    /** the notification server (awl_notifications.h): how long a
     * notification stays, in ms, by urgency (low, normal), when its client
     * leaves that to the server; 0: until dismissed. Critical ones always
     * stay. */
    struct {
        int timeout_low, timeout_normal;
        /** after that many ms without a key or the pointer, the timeouts
         * stop; the next key or motion resumes them with what was left at
         * the last one, at least half the timeout: only time the user is
         * there counts (0: never pause) */
        int idle_pause;
    } notifications;
} awl_config_t;

/** an argv's first word (an autostart's, a spawn binding's) that starts
 * the rest in the tray's icon theme instead of the desktop's: for GTK
 * programs whose tray menus draw their own icons, like nm-applet. Only that
 * program gets it, through a dconf profile of its own in memory, not what
 * it starts in turn (libawltray-env.so next to awl, preloaded into it,
 * sees to that). */
#define TRAY_ICONS "awl:tray-icons"

/** a binding's function (:c:member:`Key.func`, :c:member:`Button.func`): one of :c:macro:`AWL_ACTIONS`,
 * or the library's own actions (plugins_config.c). In the library its
 * wrapper around :c:member:`awl_host_t.actions`, awl_action_<name>; in awl the
 * function itself. */
#ifdef AWL_HOSTED
#define AWL_ACTION(name)        awl_action_##name
#else
#define AWL_ACTION(name)        name
#endif
/** a layout's arrange function (:c:member:`Layout.arrange`), one of :c:macro:`AWL_ARRANGES`;
 * awl_layout_<name> in the library, like :c:macro:`AWL_ACTION() <AWL_ACTION>` */
#ifdef AWL_HOSTED
#define AWL_LAYOUT(name)        awl_layout_##name
#else
#define AWL_LAYOUT(name)        name
#endif
/** sets the list l (:c:member:`awl_config_t.keys` & co.) to the array a */
#define SETLIST(l, a)           ((l).v = (a), (l).n = LENGTH(a))
/** the four bindings of tag TAG: view, toggleview, tag, toggletag */
#define TAGKEYS(KEY,SKEY,TAG) \
    { MODKEY,                    KEY,            AWL_ACTION(view),            {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_CTRL,  KEY,            AWL_ACTION(toggleview),      {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_SHIFT, SKEY,           AWL_ACTION(tag),             {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,SKEY,AWL_ACTION(toggletag), {.ui = 1ul << TAG} }
/** Ctrl-Alt-Fn: to VT n */
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_XF86Switch_VT_##n, AWL_ACTION(chvt), {.ui = (n)} }
/** spawn's argument for a shell command */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

static inline int awl_localtime(char buf[32]) {
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) || !strftime(buf, 32, "%y/%m/%d-%H:%M:%S", &tm))
        return 1;
    return 0;
}
