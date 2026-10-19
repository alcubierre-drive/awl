#include "awl_xwayland.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_layout.h"
#include "awl_log.h"
#include "awl_output.h"

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void associatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void dissociatex11(struct wl_listener *listener, void *data);
static void sethints(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);

struct wlr_xwayland *xwayland;
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};

void
activatex11(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, activate);

    /* Only "managed" windows can be activated */
    if (!client_is_unmanaged(c))
        wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void
associatex11(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, associate);

    LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
    LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

void
configurex11(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    if (!client_surface(c) || !client_surface(c)->mapped) {
        wlr_xwayland_surface_configure(c->surface.xwayland,
                event->x, event->y, event->width, event->height);
        return;
    }
    if (client_is_unmanaged(c)) {
        wlr_scene_node_set_position(&c->scene->node, event->x, event->y);
        wlr_xwayland_surface_configure(c->surface.xwayland,
                event->x, event->y, event->width, event->height);
        return;
    }
    if ((c->isfloating && c != grabc) || !sellayout(c->mon)->arrange) {
        resize(c, (struct wlr_box){.x = event->x - c->bw,
                .y = event->y - c->bw, .width = event->width + c->bw * 2,
                .height = event->height + c->bw * 2}, 0);
    } else {
        arrange(c->mon);
    }
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
    struct wlr_xwayland_surface *xsurface = data;
    Client *c;

    /* Allocate a Client for this surface */
    c = xsurface->data = ecalloc(1, sizeof(*c));
    c->surface.xwayland = xsurface;
    c->type = X11;
    c->bw = client_is_unmanaged(c) ? 0 : cfg->borderpx;
    c->isvisible = 1;

    /* Listen to the various events it can emit */
    LISTEN(&xsurface->events.associate, &c->associate, associatex11);
    LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
    LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
    LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
    LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
    LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
    LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
    LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

void
dissociatex11(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, dissociate);
    wl_list_remove(&c->map.link);
    wl_list_remove(&c->unmap.link);
}

void
sethints(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, set_hints);
    struct wlr_surface *surface = client_surface(c);
    if (c == focustop(selmon) || !c->surface.xwayland->hints)
        return;

    c->isurgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
    drawbars();

    if (c->isurgent && surface && surface->mapped)
        client_set_border_color(c, (float[])RGBA(borders[BorderUrg]));
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
    struct wlr_xcursor *xcursor;

    /* assign the one and only seat */
    wlr_xwayland_set_seat(xwayland, seat);

    /* Set the default XWayland cursor to match the rest of awl. */
    if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
        wlr_xwayland_set_cursor(xwayland, wlr_xcursor_image_get_buffer(xcursor->images[0]),
                xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}

void
xwaylandinit(void)
{
    /*
     * Initialise the XWayland X server.
     * It will be started when the first X client is started.
     */
    if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
        wl_signal_add(&xwayland->events.ready, &xwayland_ready);
        wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

        setenv("DISPLAY", xwayland->display_name, 1);
    } else {
        awl_wrn_printf("failed to setup XWayland X server, continuing without it\n");
    }
}

void
xwaylandfini(void)
{
    /* Only registered in xwaylandinit() if wlr_xwayland_create() succeeded;
     * removing an unregistered listener dereferences its NULL link. */
    if (xwayland) {
        wl_list_remove(&new_xwayland_surface.link);
        wl_list_remove(&xwayland_ready.link);
    }
    wlr_xwayland_destroy(xwayland);
    xwayland = NULL;
}
#endif
