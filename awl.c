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

static void cleanup(awl_server_t *srv);
static void cleanuplisteners(awl_server_t *srv);
static void gpureset(struct wl_listener *listener, void *data);
static int handlesigquit(int signo, void *data);
static void run(awl_server_t *srv, char *startup_cmd);
static void setup(awl_server_t *srv);

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
    awl_server_t *srv = data;
    (void)fd; (void)mask; (void)data;
    awl_redraw_drain();
    awl_plugins_redraw();
    drawbars(srv);
    desktop_update_all();
    wallpapertake(srv);
    awl_alert_draw_all(srv);
    return 0;
}


void
chvt(awl_server_t *srv, const Arg *arg)
{
    wlr_session_change_vt(srv->session, arg->ui);
}

void
cleanup(awl_server_t *srv)
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

    cleanuplisteners(srv);
#ifdef XWAYLAND
    xwaylandfini(srv);
#endif
    wl_display_destroy_clients(srv->dpy);
    stopchildren();
    wlr_xcursor_manager_destroy(srv->cursor.mgr);

    inputfini();

    /* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
     * Destroy it until it's fixed on the wlroots side */
    wlr_backend_destroy(srv->backend);

    /* The monitors, and with them the bars' widgets, are gone: the plugins
     * stop while their popups' scene and their timers' loop are still there
     * to give back. cfg points into the library. */
    srv->cfg = builtinconfig();
    awl_plugins_unload(srv);

    wl_display_destroy(srv->dpy);
    popupsfini();
    wlr_scene_node_destroy(&srv->scene->tree.node);
    awl_draw_fini();
    configfini(srv);
    /* only now nothing can request a redraw or a lock anymore */
    awl_redraw_fini();
    awl_lock_fini();
}

void
cleanuplisteners(awl_server_t *srv)
{
    wl_list_remove(&srv->listen.cursor_axis.link);
    wl_list_remove(&srv->listen.cursor_button.link);
    wl_list_remove(&srv->listen.cursor_frame.link);
    wl_list_remove(&srv->listen.cursor_motion.link);
    wl_list_remove(&srv->listen.cursor_motion_absolute.link);
    wl_list_remove(&srv->listen.gpu_reset.link);
    wl_list_remove(&srv->listen.new_idle_inhibitor.link);
    wl_list_remove(&srv->listen.layout_change.link);
    wl_list_remove(&srv->listen.new_input_device.link);
    wl_list_remove(&srv->listen.new_virtual_keyboard.link);
    wl_list_remove(&srv->listen.new_virtual_pointer.link);
    wl_list_remove(&srv->listen.new_pointer_constraint.link);
    wl_list_remove(&srv->listen.new_output.link);
    wl_list_remove(&srv->listen.new_xdg_toplevel.link);
    wl_list_remove(&srv->listen.new_xdg_decoration.link);
    wl_list_remove(&srv->listen.new_xdg_popup.link);
    wl_list_remove(&srv->listen.new_layer_surface.link);
    wl_list_remove(&srv->listen.output_mgr_apply.link);
    wl_list_remove(&srv->listen.output_mgr_test.link);
    wl_list_remove(&srv->listen.output_power_mgr_set_mode.link);
    wl_list_remove(&srv->listen.request_activate.link);
    wl_list_remove(&srv->listen.request_cursor.link);
    wl_list_remove(&srv->listen.request_set_psel.link);
    wl_list_remove(&srv->listen.request_set_sel.link);
    wl_list_remove(&srv->listen.request_set_cursor_shape.link);
    wl_list_remove(&srv->listen.request_start_drag.link);
    wl_list_remove(&srv->listen.start_drag.link);
    wl_list_remove(&srv->listen.new_session_lock.link);
}

void
gpureset(struct wl_listener *listener, void *data)
{
    awl_server_t *srv = wl_container_of(listener, srv, listen.gpu_reset);
    struct wlr_renderer *old_renderer = srv->renderer;
    struct wlr_allocator *old_alloc = srv->alloc;
    struct Monitor *m;
    /* must match setup(): the scene holds scenefx-only nodes (blur,
     * per-node opacity) that a plain wlroots renderer cannot render */
    if (!(srv->renderer = fx_renderer_create(srv->backend)))
        die("couldn't recreate renderer");

    if (!(srv->alloc = wlr_allocator_autocreate(srv->backend, srv->renderer)))
        die("couldn't recreate allocator");

    wl_list_remove(&srv->listen.gpu_reset.link);
    wl_signal_add(&srv->renderer->events.lost, &srv->listen.gpu_reset);

    wlr_compositor_set_renderer(srv->compositor, srv->renderer);

    wl_list_for_each(m, &srv->out.mons, link) {
        wlr_output_init_render(m->wlr_output, srv->alloc, srv->renderer);
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
    awl_server_t *srv = data;
    quit(srv, NULL);
    return 0;
}

void
quit(awl_server_t *srv, const Arg *arg)
{
    wl_display_terminate(srv->dpy);
}

void
run(awl_server_t *srv, char *startup_cmd)
{
    /* Add a Unix socket to the Wayland display. */
    const char *socket = wl_display_add_socket_auto(srv->dpy);
    if (!socket)
        die("startup: display_add_socket_auto");
    setenv("WAYLAND_DISPLAY", socket, 1);
    /* the rest of awl's environment is the config's (configapply()) */
    environmentimport();
    /* the icon theme of the programs started with TRAY_ICONS */
    awl_tray_app_set_theme(srv->cfg->tray.icon_theme);

    /* Start the backend. This will enumerate outputs and inputs, become the DRM
     * master, etc */
    if (!wlr_backend_start(srv->backend))
        die("startup: backend_start");

    /* Now that the socket exists and the backend is started, run the
     * startup command and cfg->autostart */
    spawnstartup(startup_cmd);
    autostartall(srv);

    /* Mark stdout as non-blocking to avoid the startup script
     * causing awl to freeze when a user neither closes stdin
     * nor consumes standard input in his startup script */

    if (fd_set_nonblock(STDOUT_FILENO) < 0)
        close(STDOUT_FILENO);

    barsdirty(srv);

    /* At this point the outputs are initialized, choose initial selmon based on
     * cursor position, and set default cursor image */
    srv->out.selmon = xytomon(srv, srv->cursor.wlr->x, srv->cursor.wlr->y);

    /* TODO hack to get cursor to display in its initial location (100, 100)
     * instead of (0, 0) and then jumping. Still may not be fully
     * initialized, as the image/coordinates are not transformed for the
     * monitor when displayed here */
    wlr_cursor_warp_closest(srv->cursor.wlr, NULL, srv->cursor.wlr->x, srv->cursor.wlr->y);
    wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr, "default");

    if (srv->cfg->lock.at_start)
        lockscreen(srv, NULL);

    /* Run the Wayland event loop. This does not return until you exit the
     * compositor. Starting the backend rigged up all of the necessary event
     * loop configuration to listen to libinput events, DRM events, generate
     * frame events at the refresh rate, and so on. */
    wl_display_run(srv->dpy);
}

void
setup(awl_server_t *srv)
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
    srv->cfg = builtinconfig();

    wlrloginit(srv);

    /* The Wayland display is managed by libwayland. It handles accepting
     * clients from the Unix socket, manging Wayland globals, and so on. */
    srv->dpy = wl_display_create();
    srv->event_loop = wl_display_get_event_loop(srv->dpy);
    wl_event_loop_add_signal(srv->event_loop, SIGCHLD, handlesigchld, srv);
    wl_event_loop_add_signal(srv->event_loop, SIGINT, handlesigquit, srv);
    wl_event_loop_add_signal(srv->event_loop, SIGTERM, handlesigquit, srv);
    barinit(srv);
    wallpaperinit(srv);

    /* The backend is a wlroots feature which abstracts the underlying input and
     * output hardware. The autocreate option will choose the most suitable
     * backend based on the current environment, such as opening an X11 window
     * if an X11 server is running. */
    if (!(srv->backend = wlr_backend_autocreate(srv->event_loop, &srv->session)))
        die("couldn't create backend");
    /* the hooks of test/live.sh and test/windows.sh, if asked for */
    testinit(srv);

    /* Initialize the scene graph used to lay out windows */
    srv->scene = wlr_scene_create();
    srv->root_bg = wlr_scene_rect_create(&srv->scene->tree, 0, 0, (float[4])RGBA(srv->cfg->colors.root));
    for (i = 0; i < NUM_LAYERS; i++) {
        srv->layers[i] = wlr_scene_tree_create(&srv->scene->tree);
    }
    srv->cursor.drag_icon = wlr_scene_tree_create(&srv->scene->tree);
    wlr_scene_node_place_below(&srv->cursor.drag_icon->node, &srv->layers[LyrBlock]->node);
    /* the wallpaper, under background layer surfaces (see awl_background.h) */
    srv->background_tree = wlr_scene_tree_create(&srv->scene->tree);
    wlr_scene_node_place_below(&srv->background_tree->node, &srv->layers[LyrBg]->node);
    background_init(srv->background_tree, srv->event_loop);
    /* the desktop_dir panels, over the wallpaper (see awl_desktop.h) */
    srv->desktop_tree = wlr_scene_tree_create(&srv->scene->tree);
    wlr_scene_node_place_above(&srv->desktop_tree->node, &srv->layers[LyrBg]->node);
    desktop_init(srv->desktop_tree, &(desktop_style_t){
            .blur = srv->cfg->blur.popups, .radius = srv->cfg->desktop.radius,
            .blur_strength = srv->cfg->blur.strength, .blur_alpha = srv->cfg->blur.alpha });
    /* the alerts, over everything (awl_alert_draw() keeps it on top) */
    awl_alert_init(srv);

    /* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
     * can also specify a renderer using the WLR_RENDERER env var.
     * The renderer is responsible for defining the various pixel formats it
     * supports for shared memory, this configures that for clients. */

    // TODO this is for scenefx
    // if (!(renderer = wlr_renderer_autocreate(backend)))
    if (!(srv->renderer = fx_renderer_create(srv->backend)))
        die("couldn't create renderer");
    wl_signal_add(&srv->renderer->events.lost, &srv->listen.gpu_reset);

    /* Create shm, drm and linux_dmabuf interfaces by ourselves.
     * The simplest way is to call:
     *      wlr_renderer_init_wl_display(renderer);
     * but we need to create the linux_dmabuf interface manually to integrate it
     * with wlr_scene. */
    wlr_renderer_init_wl_shm(srv->renderer, srv->dpy);

    if (wlr_renderer_get_texture_formats(srv->renderer, WLR_BUFFER_CAP_DMABUF)) {
        wlr_drm_create(srv->dpy, srv->renderer);
        wlr_scene_set_linux_dmabuf_v1(srv->scene,
                wlr_linux_dmabuf_v1_create_with_renderer(srv->dpy, 5, srv->renderer));
    }

    if ((drm_fd = wlr_renderer_get_drm_fd(srv->renderer)) >= 0 && srv->renderer->features.timeline
            && srv->backend->features.timeline)
        wlr_linux_drm_syncobj_manager_v1_create(srv->dpy, 1, drm_fd);

    /* Autocreates an allocator for us.
     * The allocator is the bridge between the renderer and the backend. It
     * handles the buffer creation, allowing wlroots to render onto the
     * screen */
    if (!(srv->alloc = wlr_allocator_autocreate(srv->backend, srv->renderer)))
        die("couldn't create allocator");

    /* This creates some hands-off wlroots interfaces. The compositor is
     * necessary for clients to allocate surfaces and the data device manager
     * handles the clipboard. Each of these wlroots interfaces has room for you
     * to dig your fingers in and play with their behavior if you want. Note that
     * the clients cannot set the selection directly without compositor approval,
     * see the setsel() function. */
    srv->compositor = wlr_compositor_create(srv->dpy, 6, srv->renderer);
    wlr_subcompositor_create(srv->dpy);
    wlr_data_device_manager_create(srv->dpy);
    wlr_export_dmabuf_manager_v1_create(srv->dpy);
    wlr_screencopy_manager_v1_create(srv->dpy);
    wlr_ext_image_copy_capture_manager_v1_create(srv->dpy, 1);
    wlr_ext_output_image_capture_source_manager_v1_create(srv->dpy, 1);
    wlr_data_control_manager_v1_create(srv->dpy);
    wlr_primary_selection_v1_device_manager_create(srv->dpy);
    wlr_viewporter_create(srv->dpy);
    wlr_single_pixel_buffer_manager_v1_create(srv->dpy);
    wlr_fractional_scale_manager_v1_create(srv->dpy, 1);
    wlr_presentation_create(srv->dpy, srv->backend, 2);
    wlr_alpha_modifier_v1_create(srv->dpy);

    /* Initializes the interface used to implement urgency hints, and
     * awl's own tokens (activationtoken()) */
    srv->activation = wlr_xdg_activation_v1_create(srv->dpy);
    wl_signal_add(&srv->activation->events.request_activate, &srv->listen.request_activate);

    wlr_scene_set_gamma_control_manager_v1(srv->scene, wlr_gamma_control_manager_v1_create(srv->dpy));

    srv->power_mgr = wlr_output_power_manager_v1_create(srv->dpy);
    wl_signal_add(&srv->power_mgr->events.set_mode, &srv->listen.output_power_mgr_set_mode);

    /* Creates an output layout, which is a wlroots utility for working with an
     * arrangement of screens in a physical layout. */
    srv->out.layout = wlr_output_layout_create(srv->dpy);
    wl_signal_add(&srv->out.layout->events.change, &srv->listen.layout_change);

    wlr_xdg_output_manager_v1_create(srv->dpy, srv->out.layout);

    /* Configure a listener to be notified when new outputs are available on the
     * backend. */
    wl_list_init(&srv->out.mons);
    wl_signal_add(&srv->backend->events.new_output, &srv->listen.new_output);

    /* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
     * Wayland protocol which is used for application windows. For more
     * detail on shells, refer to the article:
     *
     * https://drewdevault.com/2018/07/29/Wayland-shells.html
     */
    wl_list_init(&srv->clients.list);
    wl_list_init(&srv->clients.fstack);

    srv->xdg_shell = wlr_xdg_shell_create(srv->dpy, 6);
    wl_signal_add(&srv->xdg_shell->events.new_toplevel, &srv->listen.new_xdg_toplevel);
    wl_signal_add(&srv->xdg_shell->events.new_popup, &srv->listen.new_xdg_popup);

    srv->layer_shell = wlr_layer_shell_v1_create(srv->dpy, 3);
    wl_signal_add(&srv->layer_shell->events.new_surface, &srv->listen.new_layer_surface);

    srv->idle_notifier = wlr_idle_notifier_v1_create(srv->dpy);

    srv->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(srv->dpy);
    wl_signal_add(&srv->idle_inhibit_mgr->events.new_inhibitor, &srv->listen.new_idle_inhibitor);

    srv->keyboard_shortcuts_inhibit_mgr = wlr_keyboard_shortcuts_inhibit_v1_create(srv->dpy);
    srv->session_lock_mgr = wlr_session_lock_manager_v1_create(srv->dpy);
    wl_signal_add(&srv->session_lock_mgr->events.new_lock, &srv->listen.new_session_lock);

    /* Use decoration protocols to negotiate server-side decorations */
    wlr_server_decoration_manager_set_default_mode(
            wlr_server_decoration_manager_create(srv->dpy),
            WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    srv->xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(srv->dpy);
    wl_signal_add(&srv->xdg_decoration_mgr->events.new_toplevel_decoration, &srv->listen.new_xdg_decoration);

    srv->cursor.constraints = wlr_pointer_constraints_v1_create(srv->dpy);
    wl_signal_add(&srv->cursor.constraints->events.new_constraint, &srv->listen.new_pointer_constraint);

    srv->cursor.relative_mgr = wlr_relative_pointer_manager_v1_create(srv->dpy);

    /*
     * Creates a cursor, which is a wlroots utility for tracking the cursor
     * image shown on screen.
     */
    srv->cursor.wlr = wlr_cursor_create();
    wlr_cursor_attach_output_layout(srv->cursor.wlr, srv->out.layout);

    /* Creates an xcursor manager, another wlroots utility which loads up
     * Xcursor themes to source cursor images from and makes sure that cursor
     * images are available at all scale factors on the screen (necessary for
     * HiDPI support). Scaled cursors will be loaded with each output. */
    srv->cursor.mgr = wlr_xcursor_manager_create(NULL, 24);
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
    wl_signal_add(&srv->cursor.wlr->events.motion, &srv->listen.cursor_motion);
    wl_signal_add(&srv->cursor.wlr->events.motion_absolute, &srv->listen.cursor_motion_absolute);
    wl_signal_add(&srv->cursor.wlr->events.button, &srv->listen.cursor_button);
    wl_signal_add(&srv->cursor.wlr->events.axis, &srv->listen.cursor_axis);
    wl_signal_add(&srv->cursor.wlr->events.frame, &srv->listen.cursor_frame);

    srv->cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(srv->dpy, 1);
    wl_signal_add(&srv->cursor_shape_mgr->events.request_set_shape, &srv->listen.request_set_cursor_shape);

    /*
     * Configures a seat, which is a single "seat" at which a user sits and
     * operates the computer. This conceptually includes up to one keyboard,
     * pointer, touch, and drawing tablet device. We also rig up a listener to
     * let us know when new input devices are available on the backend.
     */
    wl_signal_add(&srv->backend->events.new_input, &srv->listen.new_input_device);
    srv->virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(srv->dpy);
    wl_signal_add(&srv->virtual_keyboard_mgr->events.new_virtual_keyboard,
            &srv->listen.new_virtual_keyboard);
    srv->virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(srv->dpy);
    wl_signal_add(&srv->virtual_pointer_mgr->events.new_virtual_pointer,
            &srv->listen.new_virtual_pointer);

    srv->seat = wlr_seat_create(srv->dpy, "seat0");
    /* there is always a cursor (see inputdevice()), even before the first
     * pointer device or with only a virtual one, which doesn't come through
     * inputdevice() */
    wlr_seat_set_capabilities(srv->seat, WL_SEAT_CAPABILITY_POINTER);
    wl_signal_add(&srv->seat->events.request_set_cursor, &srv->listen.request_cursor);
    wl_signal_add(&srv->seat->events.request_set_selection, &srv->listen.request_set_sel);
    wl_signal_add(&srv->seat->events.request_set_primary_selection, &srv->listen.request_set_psel);
    wl_signal_add(&srv->seat->events.request_start_drag, &srv->listen.request_start_drag);
    wl_signal_add(&srv->seat->events.start_drag, &srv->listen.start_drag);

    inputinit(srv);

    srv->out.mgr = wlr_output_manager_v1_create(srv->dpy);
    wl_signal_add(&srv->out.mgr->events.apply, &srv->listen.output_mgr_apply);
    wl_signal_add(&srv->out.mgr->events.test, &srv->listen.output_mgr_test);

    configinit(srv);
    /* before the plugins: their threads wake these from the start, and a
     * wake without the eventfd is lost (the logind state then stays unseen
     * and its alert up) */
    if (awl_redraw_init() >= 0)
        redraw_source = wl_event_loop_add_fd(srv->event_loop, awl_redraw_fd(),
                WL_EVENT_READABLE, redraw_fire, srv);
    lockinit(srv);
    /* before the plugins: their tray widget asks it for the items */
    awl_sni_watcher_start(srv->event_loop);
    awl_notifications_start(srv, srv->event_loop, awl_plugins_notifications_changed);
    awl_plugins_load(srv, 0, srv->cfgdict);
    configuse(srv);
    awl_draw_init();

    /* Make sure XWayland clients don't connect to the parent X server,
     * e.g when running in the x11 backend or the wayland backend and the
     * compositor has Xwayland support */
    unsetenv("DISPLAY");
#ifdef XWAYLAND
    xwaylandinit(srv);
#endif
}

int
main(int argc, char *argv[])
{
    awl_server_t *srv = awl;
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

    setup(srv);

    run(srv, startup_cmd);
    cleanup(srv);
    return EXIT_SUCCESS;

usage:
    fprintf(stderr, "Usage: %s [-v] [-d] [-l logfile] [-s startup command]\n", argv[0]);
    return EXIT_FAILURE;
}
