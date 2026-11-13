#pragma once

/**
 * The types awl's modules share that the plugin library never sees, and the
 * headers of wlroots, scenefx and XWayland they need; awl_server.h, which
 * every module includes, includes this. What the library sees too is in
 * awl.h.
 */
#ifdef AWL_HOSTED
#error "awl_core.h is awl's: Monitor and Client are opaque to the library"
#endif

#include "awl.h"

#include <getopt.h>
#include <libdrm/drm_fourcc.h>
#include <signal.h>
#include <sys/wait.h>

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
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
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
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#endif

// stupid SINCE_VERSION
#include "xdg-shell-protocol.h"

/** a 0xRRGGBBAA color as wlroots' 4 floats, e.g. ``(float[4])RGBA(cfg->colors.root)`` */
#define RGBA(hex)               { (((hex) >> 24) & 0xFF) / 255.0f, \
                                  (((hex) >> 16) & 0xFF) / 255.0f, \
                                  (((hex) >> 8) & 0xFF) / 255.0f, \
                                  ((hex) & 0xFF) / 255.0f }

/** client types */
enum ClientType { XDGShell, LayerShell, X11 };
/** scene layers, bottom to top */
enum SceneLayer {
    /** the background layer surfaces */
    LyrBg,
    LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrOverlay, LyrBlock,
    /** how many there are */
    NUM_LAYERS };

typedef struct Buffer Buffer;

/** A window, XDG shell or XWayland. */
struct Client {
    /* Must keep these three elements in this order */
    unsigned int type; /* XDGShell or X11* */
    struct wlr_box geom; /* layout-relative, includes border */
    Monitor *mon;
    struct wlr_scene_tree *scene;
    /** for its listeners, which only get the client */
    awl_server_t *srv;
    struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
    /** the border clock's hour and minute marks (:c:func:`borderclock`),
     * over the border; NULL for unmanaged clients */
    struct wlr_scene_buffer *clock[2];
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
    bool isfloating:1;
    bool isurgent:1;
    bool isfullscreen:1;
    bool isvisible:1;
    bool ismaximized:1;
    bool isontop:1;
    uint32_t resize; /* configure serial of a pending resize */
};

/** a view's layout and its settings, :c:member:`Monitor.ptlt` */
struct PertagLayout {
    /** the selected layout lt[sellt] and the previous one (:c:func:`setlayout`) */
    const Layout *lt[2];
    /** which of :c:member:`~PertagLayout.lt` is selected */
    unsigned int sellt;
    /** master area share (:c:func:`setmfact`) */
    float mfact;
    /** number of master clients (incnmaster()) */
    int nmaster;
};

/** An output, with its bar. */
struct Monitor {
    struct wl_list link;
    /** for its listeners, which only get the monitor */
    awl_server_t *srv;
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
    /** the alert on this output (:c:func:`awl_alert_draw`) */
    struct wlr_scene_buffer *alert;
    /** the alerts' version :c:member:`~Monitor.alert` was drawn for, -1:
     * draw it again */
    unsigned int alert_version;
    /** the output's scale it was drawn at */
    float alert_scale;
    /** the window area (w) it was drawn for */
    struct wlr_box alert_area;
    /** the border clock's hour and minute marks in
     * :c:enumerator:`BorderClockScreen` mode (:c:func:`borderclock`), on
     * top of the tiled windows' layer; NULL until then */
    struct wlr_scene_buffer *clock[2];
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
    bool gamma_lut_changed:1;
    bool showbar:1;
    bool closedbar:1;
    /** whether each view keeps its own layout and its settings in ptlt[]
     * (:c:member:`MonitorRule.pertag`, togglepertag); if not, all share ptlt[0] */
    bool pertag:1;
    /** a frame showing the lock went out since it began (:c:func:`lockcovered`) */
    bool lock_shown:1;
    /** a power management client (wlr-output-power-management) turned it off */
    bool asleep:1;
    char ltsymbol[16];
    /** the layouts and their settings, read through :c:func:`monlt`: with pertag,
     * [1+t] for tag t viewed alone and [0] for views of several tags;
     * without, [0] for all views */
    struct PertagLayout ptlt[32];
    /** the bar: its font, its widgets and what they show; bar.m is NULL
     * before createmon() sets it up and after cleanupmon() */
    awl_bar_t bar;
    /** the popups' font at this monitor's scale (awl_window.h,
     * cfg->popup.font and .fontsize), from the font registry; NULL while
     * it isn't found, then they use the bar's */
    struct fcft_font *popupfont;
    /** the desktop file list's (awl_desktop.h, cfg->desktop.font and
     * .fontsize), the same way */
    struct fcft_font *desktopfont;
};

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
    short mapped;
    struct wlr_layer_surface_v1 *layer_surface;

    struct wl_listener destroy;
    struct wl_listener unmap;
    struct wl_listener surface_commit;
    /** for its listeners, which only get the surface */
    awl_server_t *srv;
} LayerSurface;

/** A ``wlr_buffer`` of ARGB pixels in memory, for the bar and the desktop
 * panels. */
struct Buffer {
    struct wlr_buffer base;
    size_t stride;
    size_t w, h;
    uint32_t data[];
};
