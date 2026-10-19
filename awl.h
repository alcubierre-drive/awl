#pragma once
/*
 * See LICENSE file for copyright and license details.
 */

/**
 * The types awl shares with its config files and the plugin library
 * (``Monitor``, ``Client``, the bindings, the rules), and the headers they
 * need. awl's own state and functions are in its modules' headers, the
 * core in awl_server.h. The library is built against this, so a change to a
 * struct here is a change of the plugin ABI (awl_plugin_abi.h).
 */
#include <errno.h>
#include <getopt.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <libdrm/drm_fourcc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <scenefx/types/fx/clipped_region.h>
#include <scenefx/types/wlr_scene.h>
#pragma GCC diagnostic pop

#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#endif

// stupid SINCE_VERSION
#include "xdg-shell-protocol.h"
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
/** bordercolors[] */
enum BorderColor { BorderNorm, BorderSel, BorderUrg, BorderLast };
/** cursor */
enum CursorMode { CurNormal, CurPressed, CurMove, CurResize };
/** client types */
enum ClientType { XDGShell, LayerShell, X11 };
/** scene layers, bottom to top */
enum SceneLayer { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrOverlay, LyrBlock, NUM_LAYERS };
/** where a click went, `Button.click` */
enum ClickRegion { ClkBar, ClkClient, ClkRoot };

/** The argument config_plugins.h's bindings pass to their function. */
typedef union {
    int i;
    uint32_t ui;
    float f;
    const void *v;
} Arg;

/** A pointer button binding in config_plugins.h's ``buttons[]``. */
typedef struct {
    unsigned int click;
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

/** A key binding in config_plugins.h's ``keys[]``. */
typedef struct {
    uint32_t mod;
    xkb_keysym_t keysym;
    void (*func)(const Arg *);
    const Arg arg;
} Key;

/** A layer-shell surface: panels, notifications, the launcher. */
typedef struct {
    /* Must keep these three elements in this order */
    unsigned int type; /* LayerShell */
    struct wlr_box geom;
    Monitor *mon;
    struct wlr_scene_tree *scene;
    struct wlr_scene_tree *popups;
    struct wlr_scene_layer_surface_v1 *scene_layer;
    struct wl_list link;
    short mapped, is_notification, is_launcher;
    struct wlr_layer_surface_v1 *layer_surface;

    struct wl_listener destroy;
    struct wl_listener unmap;
    struct wl_listener surface_commit;

    struct wlr_scene_blur* blur;
} LayerSurface;

/** A layout in config_plugins.h's ``layouts[]``. */
typedef struct {
    const char *symbol;
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
     * popup_config.font and .fontsize), from the font registry; NULL while
     * they use the bar's */
    struct fcft_font *popupfont;
    /** the desktop file list's (awl_desktop.h, desktop_config.font and
     * .fontsize), the same way; NULL while it uses the bar's */
    struct fcft_font *desktopfont;
    /** the bar's text padding, left and right together */
    int lrpad;
};

/** A ``wlr_buffer`` of ARGB pixels in memory, for the bar and the desktop
 * panels. */
struct Buffer {
    struct wlr_buffer base;
    size_t stride;
    size_t w, h;
    uint32_t data[];
};

/** A rule in config_plugins.h's ``monrules[]``, for outputs whose name matches. */
typedef struct {
    const char *name;
    float mfact;
    int nmaster;
    float scale;
    const Layout *lt;
    enum wl_output_transform rr;
    int x, y;
    /** whether each tag gets its own layout (see togglepertag); 0 if left out */
    int pertag;
} MonitorRule;

/** A rule in config_plugins.h's ``rules[]``, for new windows whose app id and title
 * match. */
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
/** config_plugins.h's ``wallpaper_config`` */
typedef struct {
    /** its ``*.png`` files; relative to ``$HOME`` unless absolute. The config
     * dictionary's ``wallpaper_dir`` starts as this, from the config_plugins.h built
     * into awl; the wallpaper thread reads that key, so a reloaded config_plugins.h's
     * dir doesn't count. */
    const char *dir;
    /** from one to the next, 0: switch at once */
    unsigned int fade_ms;
    /** seconds between the timer's changes, 0: none */
    unsigned int interval;
    /** what the timer switches to, until wallpapermode */
    WallpaperMode mode;
} WallpaperConfig;

static inline int awl_localtime(char buf[32]) {
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) || !strftime(buf, 32, "%y/%m/%d-%H:%M:%S", &tm))
        return 1;
    return 0;
}

