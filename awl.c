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

struct wl_display *dpy;
struct wl_event_loop *event_loop;
struct wlr_backend *backend;
struct wlr_session *session;
struct wlr_renderer *drw;
struct wlr_allocator *alloc;
struct wlr_compositor *compositor;
struct wlr_scene *scene;
struct wlr_scene_tree *layers[NUM_LAYERS];
struct wlr_scene_rect *root_bg;
struct wlr_seat *seat;
struct wlr_idle_notifier_v1 *idle_notifier;

static struct wlr_scene_tree *desktop_tree;
static struct wlr_scene_tree *background_tree;
static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wlr_keyboard_shortcuts_inhibit_manager_v1 *keyboard_shortcuts_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
static struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_output_power_manager_v1 *power_mgr;
static struct wlr_session_lock_manager_v1 *session_lock_mgr;

/** bars are redrawn on demand: see awl_redraw.h */
static struct wl_event_source* redraw_source = NULL;
static int redraw_fire( int fd, uint32_t mask, void* data ) {
    (void)fd; (void)mask; (void)data;
    awl_redraw_drain();
    drawbars();
    desktop_update_all();
    wallpapertake();
    awl_alert_draw_all();
    return 0;
}

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener gpu_reset = {.notify = gpureset};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
static struct wl_listener new_input_device = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
static struct wl_listener new_pointer_constraint = {.notify = createpointerconstraint};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_toplevel = {.notify = createnotify};
static struct wl_listener new_xdg_popup = {.notify = createpopup};
static struct wl_listener new_xdg_decoration = {.notify = createdecoration};
static struct wl_listener new_layer_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener output_power_mgr_set_mode = {.notify = powermgrsetmode};
static struct wl_listener request_activate = {.notify = urgent};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener new_session_lock = {.notify = locksession};

void
chvt(const Arg *arg)
{
    wlr_session_change_vt(session, arg->ui);
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
    wl_display_destroy_clients(dpy);
    stopchildren();
    wlr_xcursor_manager_destroy(cursor_mgr);

    inputfini();

    /* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
     * Destroy it until it's fixed on the wlroots side */
    wlr_backend_destroy(backend);

    /* The monitors, and with them the bars' widgets, are gone: the plugins
     * stop while their popups' scene and their timers' loop are still there
     * to give back. cfg points into the library. */
    cfg = builtinconfig();
    awl_plugins_unload();

    wl_display_destroy(dpy);
    popupsfini();
    wlr_scene_node_destroy(&scene->tree.node);
    awl_draw_fini();
    configfini();
    /* only now nothing can request a redraw or a lock anymore */
    awl_redraw_fini();
    awl_lock_fini();
}

void
cleanuplisteners(void)
{
    wl_list_remove(&cursor_axis.link);
    wl_list_remove(&cursor_button.link);
    wl_list_remove(&cursor_frame.link);
    wl_list_remove(&cursor_motion.link);
    wl_list_remove(&cursor_motion_absolute.link);
    wl_list_remove(&gpu_reset.link);
    wl_list_remove(&new_idle_inhibitor.link);
    wl_list_remove(&layout_change.link);
    wl_list_remove(&new_input_device.link);
    wl_list_remove(&new_virtual_keyboard.link);
    wl_list_remove(&new_virtual_pointer.link);
    wl_list_remove(&new_pointer_constraint.link);
    wl_list_remove(&new_output.link);
    wl_list_remove(&new_xdg_toplevel.link);
    wl_list_remove(&new_xdg_decoration.link);
    wl_list_remove(&new_xdg_popup.link);
    wl_list_remove(&new_layer_surface.link);
    wl_list_remove(&output_mgr_apply.link);
    wl_list_remove(&output_mgr_test.link);
    wl_list_remove(&output_power_mgr_set_mode.link);
    wl_list_remove(&request_activate.link);
    wl_list_remove(&request_cursor.link);
    wl_list_remove(&request_set_psel.link);
    wl_list_remove(&request_set_sel.link);
    wl_list_remove(&request_set_cursor_shape.link);
    wl_list_remove(&request_start_drag.link);
    wl_list_remove(&start_drag.link);
    wl_list_remove(&new_session_lock.link);
}

void
gpureset(struct wl_listener *listener, void *data)
{
    struct wlr_renderer *old_drw = drw;
    struct wlr_allocator *old_alloc = alloc;
    struct Monitor *m;
    /* must match setup(): the scene holds scenefx-only nodes (blur,
     * per-node opacity) that a plain wlroots renderer cannot render */
    if (!(drw = fx_renderer_create(backend)))
        die("couldn't recreate renderer");

    if (!(alloc = wlr_allocator_autocreate(backend, drw)))
        die("couldn't recreate allocator");

    wl_list_remove(&gpu_reset.link);
    wl_signal_add(&drw->events.lost, &gpu_reset);

    wlr_compositor_set_renderer(compositor, drw);

    wl_list_for_each(m, &mons, link) {
        wlr_output_init_render(m->wlr_output, alloc, drw);
        /* the bar's texture dies with the old renderer, and the scene
         * no longer has the buffer to re-upload it from */
        m->bar_shown = NULL;
    }

    wlr_allocator_destroy(old_alloc);
    wlr_renderer_destroy(old_drw);
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
    wl_display_terminate(dpy);
}

void
run(char *startup_cmd)
{
    /* Add a Unix socket to the Wayland display. */
    const char *socket = wl_display_add_socket_auto(dpy);
    if (!socket)
        die("startup: display_add_socket_auto");
    setenv("WAYLAND_DISPLAY", socket, 1);
    setenv("XDG_CURRENT_DESKTOP", "wlroots", 1 );
    system("systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XDG_CURRENT_DESKTOP");
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    setenv("QT_STYLE_OVERRIDE","kvantum",1);
    setenv("DESKTOP_SESSION","kde",1);
    setenv("QT_AUTO_SCREEN_SCALE_FACTOR","0",1);
    setenv("EDITOR","nvim",1);
    setenv("SYSTEMD_EDITOR","/usr/bin/nvim",1);
    setenv("SSH_AUTH_SOCK","1",1);
    setenv("NO_AT_BRIDGE","1",1);
    /* setup() filled in "desktop_dir" and the library's init() may have
     * changed it since; later changes don't reach the environment */
    awl_dict_val_t dir;
    awl_dict_lock(cfgdict);
    if (awl_dict_get(cfgdict, "desktop_dir", &dir) == AWL_DICT_OK
            && dir.kind == AWL_DICT_KIND_STR)
        setenv("GRIM_DEFAULT_DIR", dir.str, 1);
    awl_dict_unlock(cfgdict);

    /* the icon theme of the programs started with TRAY_ICONS */
    awl_tray_app_set_theme(cfg->tray.icon_theme);

    /* Start the backend. This will enumerate outputs and inputs, become the DRM
     * master, etc */
    if (!wlr_backend_start(backend))
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

    drawbars();

    /* At this point the outputs are initialized, choose initial selmon based on
     * cursor position, and set default cursor image */
    selmon = xytomon(cursor->x, cursor->y);

    /* TODO hack to get cursor to display in its initial location (100, 100)
     * instead of (0, 0) and then jumping. Still may not be fully
     * initialized, as the image/coordinates are not transformed for the
     * monitor when displayed here */
    wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
    wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

    if (cfg->lock.at_start)
        lockscreen(NULL);

    /* Run the Wayland event loop. This does not return until you exit the
     * compositor. Starting the backend rigged up all of the necessary event
     * loop configuration to listen to libinput events, DRM events, generate
     * frame events at the refresh rate, and so on. */
    wl_display_run(dpy);
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
    cfg = builtinconfig();

    wlrloginit();

    /* The Wayland display is managed by libwayland. It handles accepting
     * clients from the Unix socket, manging Wayland globals, and so on. */
    dpy = wl_display_create();
    event_loop = wl_display_get_event_loop(dpy);
    wl_event_loop_add_signal(event_loop, SIGCHLD, handlesigchld, NULL);
    wl_event_loop_add_signal(event_loop, SIGINT, handlesigquit, NULL);
    wl_event_loop_add_signal(event_loop, SIGTERM, handlesigquit, NULL);
    barinit();
    wallpaperinit();

    /* The backend is a wlroots feature which abstracts the underlying input and
     * output hardware. The autocreate option will choose the most suitable
     * backend based on the current environment, such as opening an X11 window
     * if an X11 server is running. */
    if (!(backend = wlr_backend_autocreate(event_loop, &session)))
        die("couldn't create backend");
    /* the hooks of test/live.sh and test/windows.sh, if asked for */
    testinit();

    /* Initialize the scene graph used to lay out windows */
    scene = wlr_scene_create();
    root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, (float[4])RGBA(cfg->rootcolor));
    for (i = 0; i < NUM_LAYERS; i++) {
        layers[i] = wlr_scene_tree_create(&scene->tree);
    }
    drag_icon = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);
    /* the wallpaper, under background layer surfaces (see awl_background.h) */
    background_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_place_below(&background_tree->node, &layers[LyrBg]->node);
    background_init(background_tree, event_loop);
    /* the desktop_dir panels, over the wallpaper (see awl_desktop.h) */
    desktop_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_place_above(&desktop_tree->node, &layers[LyrBg]->node);
    desktop_init(desktop_tree, &(desktop_style_t){
            .blur = cfg->blur.launcher, .radius = cfg->blur.launcher_radius,
            .blur_strength = cfg->blur.strength, .blur_alpha = cfg->blur.alpha });
    /* the alerts, over everything (awl_alert_draw() keeps it on top) */
    awl_alert_init();

    /* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
     * can also specify a renderer using the WLR_RENDERER env var.
     * The renderer is responsible for defining the various pixel formats it
     * supports for shared memory, this configures that for clients. */

    // TODO this is for scenefx
    // if (!(drw = wlr_renderer_autocreate(backend)))
    if (!(drw = fx_renderer_create(backend)))
        die("couldn't create renderer");
    wl_signal_add(&drw->events.lost, &gpu_reset);

    /* Create shm, drm and linux_dmabuf interfaces by ourselves.
     * The simplest way is to call:
     *      wlr_renderer_init_wl_display(drw);
     * but we need to create the linux_dmabuf interface manually to integrate it
     * with wlr_scene. */
    wlr_renderer_init_wl_shm(drw, dpy);

    if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
        wlr_drm_create(dpy, drw);
        wlr_scene_set_linux_dmabuf_v1(scene,
                wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
    }

    if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
            && backend->features.timeline)
        wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

    /* Autocreates an allocator for us.
     * The allocator is the bridge between the renderer and the backend. It
     * handles the buffer creation, allowing wlroots to render onto the
     * screen */
    if (!(alloc = wlr_allocator_autocreate(backend, drw)))
        die("couldn't create allocator");

    /* This creates some hands-off wlroots interfaces. The compositor is
     * necessary for clients to allocate surfaces and the data device manager
     * handles the clipboard. Each of these wlroots interfaces has room for you
     * to dig your fingers in and play with their behavior if you want. Note that
     * the clients cannot set the selection directly without compositor approval,
     * see the setsel() function. */
    compositor = wlr_compositor_create(dpy, 6, drw);
    wlr_subcompositor_create(dpy);
    wlr_data_device_manager_create(dpy);
    wlr_export_dmabuf_manager_v1_create(dpy);
    wlr_screencopy_manager_v1_create(dpy);
    wlr_ext_image_copy_capture_manager_v1_create(dpy, 1);
    wlr_ext_output_image_capture_source_manager_v1_create(dpy, 1);
    wlr_data_control_manager_v1_create(dpy);
    wlr_primary_selection_v1_device_manager_create(dpy);
    wlr_viewporter_create(dpy);
    wlr_single_pixel_buffer_manager_v1_create(dpy);
    wlr_fractional_scale_manager_v1_create(dpy, 1);
    wlr_presentation_create(dpy, backend, 2);
    wlr_alpha_modifier_v1_create(dpy);

    /* Initializes the interface used to implement urgency hints */
    activation = wlr_xdg_activation_v1_create(dpy);
    wl_signal_add(&activation->events.request_activate, &request_activate);

    wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(dpy));

    power_mgr = wlr_output_power_manager_v1_create(dpy);
    wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

    /* Creates an output layout, which is a wlroots utility for working with an
     * arrangement of screens in a physical layout. */
    output_layout = wlr_output_layout_create(dpy);
    wl_signal_add(&output_layout->events.change, &layout_change);

    wlr_xdg_output_manager_v1_create(dpy, output_layout);

    /* Configure a listener to be notified when new outputs are available on the
     * backend. */
    wl_list_init(&mons);
    wl_signal_add(&backend->events.new_output, &new_output);

    /* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
     * Wayland protocol which is used for application windows. For more
     * detail on shells, refer to the article:
     *
     * https://drewdevault.com/2018/07/29/Wayland-shells.html
     */
    wl_list_init(&clients);
    wl_list_init(&fstack);

    xdg_shell = wlr_xdg_shell_create(dpy, 6);
    wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
    wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

    layer_shell = wlr_layer_shell_v1_create(dpy, 3);
    wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

    idle_notifier = wlr_idle_notifier_v1_create(dpy);

    idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
    wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

    keyboard_shortcuts_inhibit_mgr = wlr_keyboard_shortcuts_inhibit_v1_create(dpy);
    session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
    wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);

    /* Use decoration protocols to negotiate server-side decorations */
    wlr_server_decoration_manager_set_default_mode(
            wlr_server_decoration_manager_create(dpy),
            WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
    wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

    pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
    wl_signal_add(&pointer_constraints->events.new_constraint, &new_pointer_constraint);

    relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

    /*
     * Creates a cursor, which is a wlroots utility for tracking the cursor
     * image shown on screen.
     */
    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, output_layout);

    /* Creates an xcursor manager, another wlroots utility which loads up
     * Xcursor themes to source cursor images from and makes sure that cursor
     * images are available at all scale factors on the screen (necessary for
     * HiDPI support). Scaled cursors will be loaded with each output. */
    cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
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
    wl_signal_add(&cursor->events.motion, &cursor_motion);
    wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
    wl_signal_add(&cursor->events.button, &cursor_button);
    wl_signal_add(&cursor->events.axis, &cursor_axis);
    wl_signal_add(&cursor->events.frame, &cursor_frame);

    cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
    wl_signal_add(&cursor_shape_mgr->events.request_set_shape, &request_set_cursor_shape);

    /*
     * Configures a seat, which is a single "seat" at which a user sits and
     * operates the computer. This conceptually includes up to one keyboard,
     * pointer, touch, and drawing tablet device. We also rig up a listener to
     * let us know when new input devices are available on the backend.
     */
    wl_signal_add(&backend->events.new_input, &new_input_device);
    virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
    wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
            &new_virtual_keyboard);
    virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
    wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
            &new_virtual_pointer);

    seat = wlr_seat_create(dpy, "seat0");
    /* there is always a cursor (see inputdevice()), even before the first
     * pointer device or with only a virtual one, which doesn't come through
     * inputdevice() */
    wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER);
    wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
    wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
    wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
    wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
    wl_signal_add(&seat->events.start_drag, &start_drag);

    inputinit();

    output_mgr = wlr_output_manager_v1_create(dpy);
    wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
    wl_signal_add(&output_mgr->events.test, &output_mgr_test);

    configinit();
    /* before the plugins: their threads wake these from the start, and a
     * wake without the eventfd is lost (the logind state then stays unseen
     * and its alert up) */
    if (awl_redraw_init() >= 0)
        redraw_source = wl_event_loop_add_fd(event_loop, awl_redraw_fd(),
                WL_EVENT_READABLE, redraw_fire, NULL);
    lockinit();
    /* before the plugins: their tray widget asks it for the items */
    awl_sni_watcher_start(event_loop);
    awl_plugins_load(0, cfgdict);
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
