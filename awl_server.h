#pragma once

/**
 * The compositor's state, :c:type:`awl_server_t`: made by setup() (awl.c)
 * and shared by all of awl's modules. The display and its event loop, the
 * backend, the renderer, the scene with its layers, the seat, the config,
 * and as member structs the monitors (awl_output.c), the clients
 * (awl_client.c), the pointer (awl_cursor.c) and the popups (awl_popup.c).
 * For now it's reached through the :c:var:`awl` global, which goes away
 * once the functions take it as a parameter (PLAN-globals.md). Main thread
 * only.
 */

#include "awl_core.h"
#include "awl_util.h"
#include "awl_dict_type.h"

/** listens to E with L, calling H */
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
/** the same with a listener of its own, which H frees */
#define LISTEN_STATIC(E, H)     do { struct wl_listener *_l = ecalloc(1, sizeof*_l); _l->notify = (H); wl_signal_add((E), _l); } while (0)

/** The monitors, awl_output.c's */
typedef struct {
    /** every Monitor, newest first */
    struct wl_list mons;
    /** the selected monitor: the focused client's, or the pointer's with
     * sloppyfocus; NULL while no output is enabled */
    Monitor *selmon;
    /** the output layout's box, around every output */
    struct wlr_box sgeom;
    struct wlr_output_layout *layout;
    struct wlr_output_manager_v1 *mgr;
    /** set while a monitor is destroyed (cleanupmon()): no bar is drawn then.
     * The dying monitor's own bar is safe without it (barremovemon() clears
     * m->bar.m first, and m is off mons before closemon()); what it still holds
     * back is the other monitors' redraws while closemon() moves the clients
     * over (arrange() and focus changes draw the bars), so they draw once,
     * afterwards. Dates from a crash fix before barremovemon() existed; to drop
     * it, test unplugging and replugging a monitor. */
    int in_cleanupmon;
} awl_outputs_t;

/** The clients, awl_client.c's */
typedef struct {
    /** the managed clients, Client.link: tiling order */
    struct wl_list list;
    /** the same, Client.flink: focus order, the focused first */
    struct wl_list fstack;
    /** the layer surface or unmanaged client that holds the keyboard, or NULL */
    void *exclusive_focus;
    /** cfg->colors' border, border_focused and border_urgent, copied: the clients' borders outlive a reload */
    uint32_t borders[BorderLast];
} awl_clients_t;

/** The pointer, awl_cursor.c's */
typedef struct {
    struct wlr_cursor *wlr;
    struct wlr_xcursor_manager *mgr;
    struct wlr_pointer_constraints_v1 *constraints;
    struct wlr_relative_pointer_manager_v1 *relative_mgr;
    /** the drag icons, under LyrBlock */
    struct wlr_scene_tree *drag_icon;
    /** what the pointer does, :c:enum:`CursorMode` */
    unsigned int mode;
    /** the client being moved or resized */
    Client *grabc;
} awl_cursor_t;

/** The compositor: one, made by setup() */
typedef struct awl_server_t {
    struct wl_display *dpy;
    struct wl_event_loop *event_loop;
    struct wlr_backend *backend;
    /** NULL without a session (nested, headless) */
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *alloc;
    struct wlr_compositor *compositor;

    struct wlr_scene *scene;
    /** the scene's layers, bottom to top, :c:enum:`SceneLayer`; the wallpaper and the
     * desktop panels are under LyrBg (awl_background.h, awl_desktop.h), awl's
     * popups right above LyrTop (awl_popup.h), the alerts above everything
     * (awl_alert.h) */
    struct wlr_scene_tree *layers[NUM_LAYERS];
    /** the root color behind everything, the size of all outputs */
    struct wlr_scene_rect *root_bg;
    /** the wallpaper's, below LyrBg (awl_background.h) */
    struct wlr_scene_tree *background_tree;
    /** the desktop panels', right above LyrBg (awl_desktop.h) */
    struct wlr_scene_tree *desktop_tree;

    struct wlr_seat *seat;
    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
    /** xdg-activation, the global */
    struct wlr_xdg_activation_v1 *activation;
    /* the protocols' globals only setup() uses */
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *keyboard_shortcuts_inhibit_mgr;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
    struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
    struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
    struct wlr_output_power_manager_v1 *power_mgr;
    struct wlr_session_lock_manager_v1 *session_lock_mgr;
#ifdef XWAYLAND
    /** NULL if it couldn't be set up */
    struct wlr_xwayland *xwayland;
#endif

    /** The config in effect: the library's (awl's defaults with
     * plugins_config.c's changes), or while none is loaded the defaults. The
     * library's points into it, so it's only good until the next reload;
     * :c:func:`pluginsdetach` falls back to the defaults. */
    const awl_config_t *cfg;
    /** The config values that can change while awl runs, by name, for awl and the
     * library (:c:member:`awl_host_t.cfg`). Lives from setup() to cleanup(), across reloads. */
    awl_dict_t *cfgdict;

    awl_outputs_t out;
    awl_clients_t clients;
    awl_cursor_t cursor;
    /** every popup, Popup.link, the newest last (on top) */
    struct wl_list popups;
    /** the session is locked: by the locker, or still, after it crashed */
    int locked;
    /** setup()'s listeners to the globals above, cleanuplisteners() removes them */
    struct {
        struct wl_listener cursor_axis;
        struct wl_listener cursor_button;
        struct wl_listener cursor_frame;
        struct wl_listener cursor_motion;
        struct wl_listener cursor_motion_absolute;
        struct wl_listener gpu_reset;
        struct wl_listener layout_change;
        struct wl_listener new_idle_inhibitor;
        struct wl_listener new_input_device;
        struct wl_listener new_virtual_keyboard;
        struct wl_listener new_virtual_pointer;
        struct wl_listener new_pointer_constraint;
        struct wl_listener new_output;
        struct wl_listener new_xdg_toplevel;
        struct wl_listener new_xdg_popup;
        struct wl_listener new_xdg_decoration;
        struct wl_listener new_layer_surface;
        struct wl_listener output_mgr_apply;
        struct wl_listener output_mgr_test;
        struct wl_listener output_power_mgr_set_mode;
        struct wl_listener request_activate;
        struct wl_listener request_cursor;
        struct wl_listener request_set_psel;
        struct wl_listener request_set_sel;
        struct wl_listener request_set_cursor_shape;
        struct wl_listener request_start_drag;
        struct wl_listener start_drag;
        struct wl_listener new_session_lock;
#ifdef XWAYLAND
        struct wl_listener new_xwayland_surface;
        struct wl_listener xwayland_ready;
#endif
    } listen;
} awl_server_t;

/** the compositor, for now: goes away once everything takes it as a parameter */
extern awl_server_t *awl;



