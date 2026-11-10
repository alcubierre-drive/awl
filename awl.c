#include "version.h"
#include "awl_server.h"
#include "awl_alert.h"
#include "awl_background.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_desktop.h"
#include "awl_input.h"
#include "awl_layer.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_plugin_host.h"
#include "awl_popup.h"
#include "awl_redraw.h"
#include "awl_sni.h"
#include "awl_notifications.h"
#include "awl_spawn.h"
#include "awl_test.h"
#include "awl_wallpaper.h"
#include "awl_xwayland.h"
#include "tray/awl_tray.h"

/* awl's start, its main loop and its end: setup() makes the wlroots objects
 * and hooks the modules' handlers up to them, run() starts the backend and
 * the main loop, cleanup() takes it all down again. Everything else is in
 * the modules, see awl_server.h. */

static void cleanup(void);
static void cleanuplisteners(void);
static void gpureset(struct wl_listener *listener, void *data);
static int handlesigquit(int signo, void *data);
static void run(char *startup_cmd);
static void setup(void);

static awl_server_t server = {
    /* empty from the start: no init in setup() */
    .popups = {&server.popups, &server.popups},
    .listen.cursor_axis = {.notify = axisnotify},
    .listen.cursor_button = {.notify = buttonpress},
    .listen.cursor_frame = {.notify = cursorframe},
    .listen.cursor_motion = {.notify = motionrelative},
    .listen.cursor_motion_absolute = {.notify = motionabsolute},
    .listen.gpu_reset = {.notify = gpureset},
    .listen.layout_change = {.notify = updatemons},
    .listen.new_idle_inhibitor = {.notify = createidleinhibitor},
    .listen.new_input_device = {.notify = inputdevice},
    .listen.new_virtual_keyboard = {.notify = virtualkeyboard},
    .listen.new_virtual_pointer = {.notify = virtualpointer},
    .listen.new_pointer_constraint = {.notify = createpointerconstraint},
    .listen.new_output = {.notify = createmon},
    .listen.new_xdg_toplevel = {.notify = createnotify},
    .listen.new_xdg_popup = {.notify = createpopup},
    .listen.new_xdg_decoration = {.notify = createdecoration},
    .listen.new_layer_surface = {.notify = createlayersurface},
    .listen.output_mgr_apply = {.notify = outputmgrapply},
    .listen.output_mgr_test = {.notify = outputmgrtest},
    .listen.output_power_mgr_set_mode = {.notify = powermgrsetmode},
    .listen.request_activate = {.notify = urgent},
    .listen.request_cursor = {.notify = setcursor},
    .listen.request_set_psel = {.notify = setpsel},
    .listen.request_set_sel = {.notify = setsel},
    .listen.request_set_cursor_shape = {.notify = setcursorshape},
    .listen.request_start_drag = {.notify = requeststartdrag},
    .listen.start_drag = {.notify = startdrag},
    .listen.new_session_lock = {.notify = locksession},
};
awl_server_t *awl = &server;



/** bars are redrawn on demand: see awl_redraw.h */
static struct wl_event_source* redraw_source = NULL;
static int redraw_fire( int fd, uint32_t mask, void* data ) {
    (void)fd; (void)mask; (void)data;
    awl_redraw_drain();
    awl_plugins_redraw();
    drawbars();
    desktop_update_all();
    wallpapertake();
    awl_alert_draw_all();
    return 0;
}


void
chvt(const Arg *arg)
{
    wlr_session_change_vt(awl->session, arg->ui);
}

void
cleanup(void)
{
    sigset_t quitsigs;

    if (redraw_source) wl_event_source_remove(redraw_source);
    redraw_source = NULL;
    lockfini();
    barfini();
    wallpaperfini();
    background_fini();

    awl_sni_watcher_stop();
    awl_notifications_stop();
    awl_tray_app_cleanup();

    /* The event loop doesn't run any more, so nothing reads the signalfd:
     * let SIGINT and SIGTERM end awl the usual way again
     * instead of leaving it to SIGKILL should anything below hang. */
    sigemptyset(&quitsigs);
    sigaddset(&quitsigs, SIGINT);
    sigaddset(&quitsigs, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &quitsigs, NULL);

    cleanuplisteners();
#ifdef XWAYLAND
    xwaylandfini();
#endif
    wl_display_destroy_clients(awl->dpy);
    stopchildren();
    wlr_xcursor_manager_destroy(awl->cursor.mgr);

    inputfini();

    /* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
     * Destroy it until it's fixed on the wlroots side */
    wlr_backend_destroy(awl->backend);

    /* The monitors, and with them the bars' widgets, are gone: the plugins
     * stop while their popups' scene and their timers' loop are still there
     * to give back. cfg points into the library. */
    awl->cfg = builtinconfig();
    awl_plugins_unload();

    wl_display_destroy(awl->dpy);
    popupsfini();
    wlr_scene_node_destroy(&awl->scene->tree.node);
    awl_draw_fini();
    configfini();
    /* only now nothing can request a redraw or a lock anymore */
    awl_redraw_fini();
    awl_lock_fini();
}

void
cleanuplisteners(void)
{
    wl_list_remove(&awl->listen.cursor_axis.link);
    wl_list_remove(&awl->listen.cursor_button.link);
    wl_list_remove(&awl->listen.cursor_frame.link);
    wl_list_remove(&awl->listen.cursor_motion.link);
    wl_list_remove(&awl->listen.cursor_motion_absolute.link);
    wl_list_remove(&awl->listen.gpu_reset.link);
    wl_list_remove(&awl->listen.new_idle_inhibitor.link);
    wl_list_remove(&awl->listen.layout_change.link);
    wl_list_remove(&awl->listen.new_input_device.link);
    wl_list_remove(&awl->listen.new_virtual_keyboard.link);
    wl_list_remove(&awl->listen.new_virtual_pointer.link);
    wl_list_remove(&awl->listen.new_pointer_constraint.link);
    wl_list_remove(&awl->listen.new_output.link);
    wl_list_remove(&awl->listen.new_xdg_toplevel.link);
    wl_list_remove(&awl->listen.new_xdg_decoration.link);
    wl_list_remove(&awl->listen.new_xdg_popup.link);
    wl_list_remove(&awl->listen.new_layer_surface.link);
    wl_list_remove(&awl->listen.output_mgr_apply.link);
    wl_list_remove(&awl->listen.output_mgr_test.link);
    wl_list_remove(&awl->listen.output_power_mgr_set_mode.link);
    wl_list_remove(&awl->listen.request_activate.link);
    wl_list_remove(&awl->listen.request_cursor.link);
    wl_list_remove(&awl->listen.request_set_psel.link);
    wl_list_remove(&awl->listen.request_set_sel.link);
    wl_list_remove(&awl->listen.request_set_cursor_shape.link);
    wl_list_remove(&awl->listen.request_start_drag.link);
    wl_list_remove(&awl->listen.start_drag.link);
    wl_list_remove(&awl->listen.new_session_lock.link);
}

void
gpureset(struct wl_listener *listener, void *data)
{
    struct wlr_renderer *old_renderer = awl->renderer;
    struct wlr_allocator *old_alloc = awl->alloc;
    struct Monitor *m;
    /* must match setup(): the scene holds scenefx-only nodes (blur,
     * per-node opacity) that a plain wlroots renderer cannot render */
    if (!(awl->renderer = fx_renderer_create(awl->backend)))
        die("couldn't recreate renderer");

    if (!(awl->alloc = wlr_allocator_autocreate(awl->backend, awl->renderer)))
        die("couldn't recreate allocator");

    wl_list_remove(&awl->listen.gpu_reset.link);
    wl_signal_add(&awl->renderer->events.lost, &awl->listen.gpu_reset);

    wlr_compositor_set_renderer(awl->compositor, awl->renderer);

    wl_list_for_each(m, &awl->out.mons, link) {
        wlr_output_init_render(m->wlr_output, awl->alloc, awl->renderer);
        /* the bar's texture dies with the old renderer, and the scene
         * no longer has the buffer to re-upload it from */
        m->bar_shown = NULL;
    }

    wlr_allocator_destroy(old_alloc);
    wlr_renderer_destroy(old_renderer);
    awl_redraw_request();
}

int
handlesigquit(int signo, void *data)
{
    quit(NULL);
    return 0;
}

void
quit(const Arg *arg)
{
    wl_display_terminate(awl->dpy);
}

void
run(char *startup_cmd)
{
    /* Add a Unix socket to the Wayland display. */
    const char *socket = wl_display_add_socket_auto(awl->dpy);
    if (!socket)
        die("startup: display_add_socket_auto");
    setenv("WAYLAND_DISPLAY", socket, 1);
    /* the rest of awl's environment is the config's (configapply()) */
    environmentimport();
    /* the icon theme of the programs started with TRAY_ICONS */
    awl_tray_app_set_theme(awl->cfg->tray.icon_theme);

    /* Start the backend. This will enumerate outputs and inputs, become the DRM
     * master, etc */
    if (!wlr_backend_start(awl->backend))
        die("startup: backend_start");

    /* Now that the socket exists and the backend is started, run the
     * startup command and cfg->autostart */
    spawnstartup(startup_cmd);
    autostartall();

    /* Mark stdout as non-blocking to avoid the startup script
     * causing awl to freeze when a user neither closes stdin
     * nor consumes standard input in his startup script */

    if (fd_set_nonblock(STDOUT_FILENO) < 0)
        close(STDOUT_FILENO);

    barsdirty();

    /* At this point the outputs are initialized, choose initial selmon based on
     * cursor position, and set default cursor image */
    awl->out.selmon = xytomon(awl->cursor.wlr->x, awl->cursor.wlr->y);

    /* TODO hack to get cursor to display in its initial location (100, 100)
     * instead of (0, 0) and then jumping. Still may not be fully
     * initialized, as the image/coordinates are not transformed for the
     * monitor when displayed here */
    wlr_cursor_warp_closest(awl->cursor.wlr, NULL, awl->cursor.wlr->x, awl->cursor.wlr->y);
    wlr_cursor_set_xcursor(awl->cursor.wlr, awl->cursor.mgr, "default");

    if (awl->cfg->lock.at_start)
        lockscreen(NULL);

    /* Run the Wayland event loop. This does not return until you exit the
     * compositor. Starting the backend rigged up all of the necessary event
     * loop configuration to listen to libinput events, DRM events, generate
     * frame events at the refresh rate, and so on. */
    wl_display_run(awl->dpy);
}

void
setup(void)
{
    int drm_fd, i;
    sigset_t handled;

    /* These signals are handled in the event loop (a signalfd). That
     * only works if no thread can take them, so block them here, before any
     * thread starts, and every thread inherits the mask; spawned children
     * unblock them again (see unblocksignals()). */
    sigemptyset(&handled);
    sigaddset(&handled, SIGCHLD);
    sigaddset(&handled, SIGINT);
    sigaddset(&handled, SIGTERM);
    if (getenv("AWL_TEST_OUTPUTS")) {
        sigaddset(&handled, SIGUSR1);
        sigaddset(&handled, SIGUSR2);
    }
    sigprocmask(SIG_BLOCK, &handled, NULL);
    /* a dead client must not kill awl (GIO ignores it anyway) */
    signal(SIGPIPE, SIG_IGN);

    /* until the library is loaded */
    awl->cfg = builtinconfig();

    wlrloginit();

    /* The Wayland display is managed by libwayland. It handles accepting
     * clients from the Unix socket, manging Wayland globals, and so on. */
    awl->dpy = wl_display_create();
    awl->event_loop = wl_display_get_event_loop(awl->dpy);
    wl_event_loop_add_signal(awl->event_loop, SIGCHLD, handlesigchld, NULL);
    wl_event_loop_add_signal(awl->event_loop, SIGINT, handlesigquit, NULL);
    wl_event_loop_add_signal(awl->event_loop, SIGTERM, handlesigquit, NULL);
    barinit();
    wallpaperinit();

    /* The backend is a wlroots feature which abstracts the underlying input and
     * output hardware. The autocreate option will choose the most suitable
     * backend based on the current environment, such as opening an X11 window
     * if an X11 server is running. */
    if (!(awl->backend = wlr_backend_autocreate(awl->event_loop, &awl->session)))
        die("couldn't create backend");
    /* the hooks of test/live.sh and test/windows.sh, if asked for */
    testinit();

    /* Initialize the scene graph used to lay out windows */
    awl->scene = wlr_scene_create();
    awl->root_bg = wlr_scene_rect_create(&awl->scene->tree, 0, 0, (float[4])RGBA(awl->cfg->colors.root));
    for (i = 0; i < NUM_LAYERS; i++) {
        awl->layers[i] = wlr_scene_tree_create(&awl->scene->tree);
    }
    awl->cursor.drag_icon = wlr_scene_tree_create(&awl->scene->tree);
    wlr_scene_node_place_below(&awl->cursor.drag_icon->node, &awl->layers[LyrBlock]->node);
    /* the wallpaper, under background layer surfaces (see awl_background.h) */
    awl->background_tree = wlr_scene_tree_create(&awl->scene->tree);
    wlr_scene_node_place_below(&awl->background_tree->node, &awl->layers[LyrBg]->node);
    background_init(awl->background_tree, awl->event_loop);
    /* the desktop_dir panels, over the wallpaper (see awl_desktop.h) */
    awl->desktop_tree = wlr_scene_tree_create(&awl->scene->tree);
    wlr_scene_node_place_above(&awl->desktop_tree->node, &awl->layers[LyrBg]->node);
    desktop_init(awl->desktop_tree, &(desktop_style_t){
            .blur = awl->cfg->blur.popups, .radius = awl->cfg->desktop.radius,
            .blur_strength = awl->cfg->blur.strength, .blur_alpha = awl->cfg->blur.alpha });
    /* the alerts, over everything (awl_alert_draw() keeps it on top) */
    awl_alert_init();

    /* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
     * can also specify a renderer using the WLR_RENDERER env var.
     * The renderer is responsible for defining the various pixel formats it
     * supports for shared memory, this configures that for clients. */

    // TODO this is for scenefx
    // if (!(renderer = wlr_renderer_autocreate(backend)))
    if (!(awl->renderer = fx_renderer_create(awl->backend)))
        die("couldn't create renderer");
    wl_signal_add(&awl->renderer->events.lost, &awl->listen.gpu_reset);

    /* Create shm, drm and linux_dmabuf interfaces by ourselves.
     * The simplest way is to call:
     *      wlr_renderer_init_wl_display(renderer);
     * but we need to create the linux_dmabuf interface manually to integrate it
     * with wlr_scene. */
    wlr_renderer_init_wl_shm(awl->renderer, awl->dpy);

    if (wlr_renderer_get_texture_formats(awl->renderer, WLR_BUFFER_CAP_DMABUF)) {
        wlr_drm_create(awl->dpy, awl->renderer);
        wlr_scene_set_linux_dmabuf_v1(awl->scene,
                wlr_linux_dmabuf_v1_create_with_renderer(awl->dpy, 5, awl->renderer));
    }

    if ((drm_fd = wlr_renderer_get_drm_fd(awl->renderer)) >= 0 && awl->renderer->features.timeline
            && awl->backend->features.timeline)
        wlr_linux_drm_syncobj_manager_v1_create(awl->dpy, 1, drm_fd);

    /* Autocreates an allocator for us.
     * The allocator is the bridge between the renderer and the backend. It
     * handles the buffer creation, allowing wlroots to render onto the
     * screen */
    if (!(awl->alloc = wlr_allocator_autocreate(awl->backend, awl->renderer)))
        die("couldn't create allocator");

    /* This creates some hands-off wlroots interfaces. The compositor is
     * necessary for clients to allocate surfaces and the data device manager
     * handles the clipboard. Each of these wlroots interfaces has room for you
     * to dig your fingers in and play with their behavior if you want. Note that
     * the clients cannot set the selection directly without compositor approval,
     * see the setsel() function. */
    awl->compositor = wlr_compositor_create(awl->dpy, 6, awl->renderer);
    wlr_subcompositor_create(awl->dpy);
    wlr_data_device_manager_create(awl->dpy);
    wlr_export_dmabuf_manager_v1_create(awl->dpy);
    wlr_screencopy_manager_v1_create(awl->dpy);
    wlr_ext_image_copy_capture_manager_v1_create(awl->dpy, 1);
    wlr_ext_output_image_capture_source_manager_v1_create(awl->dpy, 1);
    wlr_data_control_manager_v1_create(awl->dpy);
    wlr_primary_selection_v1_device_manager_create(awl->dpy);
    wlr_viewporter_create(awl->dpy);
    wlr_single_pixel_buffer_manager_v1_create(awl->dpy);
    wlr_fractional_scale_manager_v1_create(awl->dpy, 1);
    wlr_presentation_create(awl->dpy, awl->backend, 2);
    wlr_alpha_modifier_v1_create(awl->dpy);

    /* Initializes the interface used to implement urgency hints, and
     * awl's own tokens (activationtoken()) */
    awl->activation = wlr_xdg_activation_v1_create(awl->dpy);
    wl_signal_add(&awl->activation->events.request_activate, &awl->listen.request_activate);

    wlr_scene_set_gamma_control_manager_v1(awl->scene, wlr_gamma_control_manager_v1_create(awl->dpy));

    awl->power_mgr = wlr_output_power_manager_v1_create(awl->dpy);
    wl_signal_add(&awl->power_mgr->events.set_mode, &awl->listen.output_power_mgr_set_mode);

    /* Creates an output layout, which is a wlroots utility for working with an
     * arrangement of screens in a physical layout. */
    awl->out.layout = wlr_output_layout_create(awl->dpy);
    wl_signal_add(&awl->out.layout->events.change, &awl->listen.layout_change);

    wlr_xdg_output_manager_v1_create(awl->dpy, awl->out.layout);

    /* Configure a listener to be notified when new outputs are available on the
     * backend. */
    wl_list_init(&awl->out.mons);
    wl_signal_add(&awl->backend->events.new_output, &awl->listen.new_output);

    /* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
     * Wayland protocol which is used for application windows. For more
     * detail on shells, refer to the article:
     *
     * https://drewdevault.com/2018/07/29/Wayland-shells.html
     */
    wl_list_init(&awl->clients.list);
    wl_list_init(&awl->clients.fstack);

    awl->xdg_shell = wlr_xdg_shell_create(awl->dpy, 6);
    wl_signal_add(&awl->xdg_shell->events.new_toplevel, &awl->listen.new_xdg_toplevel);
    wl_signal_add(&awl->xdg_shell->events.new_popup, &awl->listen.new_xdg_popup);

    awl->layer_shell = wlr_layer_shell_v1_create(awl->dpy, 3);
    wl_signal_add(&awl->layer_shell->events.new_surface, &awl->listen.new_layer_surface);

    awl->idle_notifier = wlr_idle_notifier_v1_create(awl->dpy);

    awl->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(awl->dpy);
    wl_signal_add(&awl->idle_inhibit_mgr->events.new_inhibitor, &awl->listen.new_idle_inhibitor);

    awl->keyboard_shortcuts_inhibit_mgr = wlr_keyboard_shortcuts_inhibit_v1_create(awl->dpy);
    awl->session_lock_mgr = wlr_session_lock_manager_v1_create(awl->dpy);
    wl_signal_add(&awl->session_lock_mgr->events.new_lock, &awl->listen.new_session_lock);

    /* Use decoration protocols to negotiate server-side decorations */
    wlr_server_decoration_manager_set_default_mode(
            wlr_server_decoration_manager_create(awl->dpy),
            WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    awl->xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(awl->dpy);
    wl_signal_add(&awl->xdg_decoration_mgr->events.new_toplevel_decoration, &awl->listen.new_xdg_decoration);

    awl->cursor.constraints = wlr_pointer_constraints_v1_create(awl->dpy);
    wl_signal_add(&awl->cursor.constraints->events.new_constraint, &awl->listen.new_pointer_constraint);

    awl->cursor.relative_mgr = wlr_relative_pointer_manager_v1_create(awl->dpy);

    /*
     * Creates a cursor, which is a wlroots utility for tracking the cursor
     * image shown on screen.
     */
    awl->cursor.wlr = wlr_cursor_create();
    wlr_cursor_attach_output_layout(awl->cursor.wlr, awl->out.layout);

    /* Creates an xcursor manager, another wlroots utility which loads up
     * Xcursor themes to source cursor images from and makes sure that cursor
     * images are available at all scale factors on the screen (necessary for
     * HiDPI support). Scaled cursors will be loaded with each output. */
    awl->cursor.mgr = wlr_xcursor_manager_create(NULL, 24);
    setenv("XCURSOR_SIZE", "24", 1);

    /*
     * wlr_cursor *only* displays an image on screen. It does not move around
     * when the pointer moves. However, we can attach input devices to it, and
     * it will generate aggregate events for all of them. In these events, we
     * can choose how we want to process them, forwarding them to clients and
     * moving the cursor around. More detail on this process is described in
     * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
     *
     * And more comments are sprinkled throughout the notify functions above.
     */
    wl_signal_add(&awl->cursor.wlr->events.motion, &awl->listen.cursor_motion);
    wl_signal_add(&awl->cursor.wlr->events.motion_absolute, &awl->listen.cursor_motion_absolute);
    wl_signal_add(&awl->cursor.wlr->events.button, &awl->listen.cursor_button);
    wl_signal_add(&awl->cursor.wlr->events.axis, &awl->listen.cursor_axis);
    wl_signal_add(&awl->cursor.wlr->events.frame, &awl->listen.cursor_frame);

    awl->cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(awl->dpy, 1);
    wl_signal_add(&awl->cursor_shape_mgr->events.request_set_shape, &awl->listen.request_set_cursor_shape);

    /*
     * Configures a seat, which is a single "seat" at which a user sits and
     * operates the computer. This conceptually includes up to one keyboard,
     * pointer, touch, and drawing tablet device. We also rig up a listener to
     * let us know when new input devices are available on the backend.
     */
    wl_signal_add(&awl->backend->events.new_input, &awl->listen.new_input_device);
    awl->virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(awl->dpy);
    wl_signal_add(&awl->virtual_keyboard_mgr->events.new_virtual_keyboard,
            &awl->listen.new_virtual_keyboard);
    awl->virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(awl->dpy);
    wl_signal_add(&awl->virtual_pointer_mgr->events.new_virtual_pointer,
            &awl->listen.new_virtual_pointer);

    awl->seat = wlr_seat_create(awl->dpy, "seat0");
    /* there is always a cursor (see inputdevice()), even before the first
     * pointer device or with only a virtual one, which doesn't come through
     * inputdevice() */
    wlr_seat_set_capabilities(awl->seat, WL_SEAT_CAPABILITY_POINTER);
    wl_signal_add(&awl->seat->events.request_set_cursor, &awl->listen.request_cursor);
    wl_signal_add(&awl->seat->events.request_set_selection, &awl->listen.request_set_sel);
    wl_signal_add(&awl->seat->events.request_set_primary_selection, &awl->listen.request_set_psel);
    wl_signal_add(&awl->seat->events.request_start_drag, &awl->listen.request_start_drag);
    wl_signal_add(&awl->seat->events.start_drag, &awl->listen.start_drag);

    inputinit();

    awl->out.mgr = wlr_output_manager_v1_create(awl->dpy);
    wl_signal_add(&awl->out.mgr->events.apply, &awl->listen.output_mgr_apply);
    wl_signal_add(&awl->out.mgr->events.test, &awl->listen.output_mgr_test);

    configinit();
    /* before the plugins: their threads wake these from the start, and a
     * wake without the eventfd is lost (the logind state then stays unseen
     * and its alert up) */
    if (awl_redraw_init() >= 0)
        redraw_source = wl_event_loop_add_fd(awl->event_loop, awl_redraw_fd(),
                WL_EVENT_READABLE, redraw_fire, NULL);
    lockinit();
    /* before the plugins: their tray widget asks it for the items */
    awl_sni_watcher_start(awl->event_loop);
    awl_notifications_start(awl->event_loop, awl_plugins_notifications_changed);
    awl_plugins_load(0, awl->cfgdict);
    configuse();
    awl_draw_init();

    /* Make sure XWayland clients don't connect to the parent X server,
     * e.g when running in the x11 backend or the wayland backend and the
     * compositor has Xwayland support */
    unsetenv("DISPLAY");
#ifdef XWAYLAND
    xwaylandinit();
#endif
}

int
main(int argc, char *argv[])
{
    char *startup_cmd = NULL;
    const char *logfile = AWL_LOGFILE;
    int c;

    while ((c = getopt(argc, argv, "s:l:hdv")) != -1) {
        if (c == 's')
            startup_cmd = optarg;
        else if (c == 'l')
            logfile = optarg;
        else if (c == 'd')
            awl_log_set_level(awl_loglevel_vrb); /* wlroots' too, see wlrloginit() */
        else if (c == 'v') {
            puts("awl " VERSION);
            return EXIT_SUCCESS;
        }
        else
            goto usage;
    }
    if (optind < argc)
        goto usage;

    /* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
    if (!getenv("XDG_RUNTIME_DIR"))
        die("XDG_RUNTIME_DIR must be set");
    /* only now: awl -v or a usage error must not truncate a running awl's log */
    awl_log_init(logfile);

    setup();

    run(startup_cmd);
    cleanup();
    return EXIT_SUCCESS;

usage:
    fprintf(stderr, "Usage: %s [-v] [-d] [-l logfile] [-s startup command]\n", argv[0]);
    return EXIT_FAILURE;
}
