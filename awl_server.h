#pragma once

/**
 * The compositor's state, :c:type:`awl_server_t`: made by ``main()``, set
 * up by setup() (awl.c) and passed to all of awl's modules. The display and its event loop, the
 * backend, the renderer, the scene with its layers, the seat, the config,
 * and as member structs the monitors (awl_output.c), the clients
 * (awl_client.c), the pointer (awl_cursor.c) and the popups (awl_popup.c).
 * Every function that needs it takes it first (``awl_server_t *srv``);
 * listeners get it from their object's back pointer or from
 * ``wl_container_of`` on :c:member:`awl_server_t.listen`. Main thread only:
 * other threads reach awl through ``awl_host`` and awl_redraw.h. What
 * belongs to the process rather than the compositor is in awl_process.h.
 */

#ifdef AWL_HOSTED
#error "awl_server.h is awl's: the library gets awl_server_t only as a pointer to hand back to awl_host"
#endif

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
    /** the border width plugins_config.c had, to tell whether a reload
     * changed it */
    unsigned int applied_borderpx;
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
    /** where in it the pointer grabbed it, client-relative */
    int grabcx, grabcy;
    /** the constraint in effect, or NULL */
    struct wlr_pointer_constraint_v1 *active_constraint;
    /** a press an awl window took swallows its release too */
    int swallow_release;
} awl_cursor_t;

/* the modules' own state, defined in their .c files (awl_server_t's
 * members below) */
struct awl_lock;
struct awl_bars;
struct awl_wallpaper;
struct awl_alerts;
struct awl_input;
struct awl_conf;
struct awl_background;
struct awl_desktop;
struct awl_sni_watcher;
struct awl_notifs;
struct awl_test;

/** The popups, awl_popup.c's */
typedef struct {
    /** every popup, Popup.link, the newest last (on top) */
    struct wl_list list;
    /** the popups' tree, made with the first one; the persistent ones', made
     * with the first of them (popupsplace()) */
    struct wlr_scene_tree *tree, *persistent_tree;
    /** the popup the pointer is over, if it takes the pointer */
    awl_window_t *hovered;
    /** the popup that had the keyboard at the last :c:func:`popupsgrabupdate` */
    awl_window_t *keyboard;
} awl_popups_t;

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
    awl_popups_t popups;
    /** the session is locked: by the locker, or still, after it crashed */
    int locked;

    /* The modules' own state: each struct is defined only in its module's
     * .c, which makes it in its init; other modules go through its
     * functions. */
    /** awl_lockscreen.c's, from lockinit() to awl_lock_fini() */
    struct awl_lock *lock;
    /** awl_bar.c's: the hover, the redraw, the fonts in use; from barinit() to barfree() */
    struct awl_bars *bars;
    /** awl_wallpaper.c's: the timer and its mode; from wallpaperinit() to wallpaperfini() */
    struct awl_wallpaper *wallpaper;
    /** awl_alert.c's: the alerts' texts and tree; from awl_alert_init() to awl_alert_fini() */
    struct awl_alerts *alerts;
    /** awl_input.c's: the keyboard group and the devices; from inputinit() to inputfree() */
    struct awl_input *input;
    /** awl_config.c's: the library's config, which cfg points to while it's
     * loaded, and a pending plugin_restart(); from configinit() to configfini() */
    struct awl_conf *conf;
    /** awl_background.c's: the wallpaper's images and fade; from
     * background_init() to background_fini() */
    struct awl_background *background;
    /** awl_desktop.c's: the desktop panels; from desktop_init() to desktop_fini() */
    struct awl_desktop *desktop;
    /** awl_sni.c's: the StatusNotifierWatcher, NULL if it couldn't start */
    struct awl_sni_watcher *sni;
    /** awl_notifications.c's: the notification server and its history;
     * from awl_notifications_start() to awl_notifications_stop() */
    struct awl_notifs *notifs;
    /** awl_test.c's: AWL_TEST_WINDOWS' popups; NULL without it */
    struct awl_test *test;
    /** awl_redraw.h's fd's event handler (redraw_fire()) */
    struct wl_event_source *redraw_source;
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
