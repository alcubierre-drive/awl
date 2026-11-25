#include "awl_xwayland.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_layout.h"
#include "awl_log.h"
#include "awl_output.h"

#ifdef XWAYLAND
static void activate_x11( struct wl_listener *listener, void *data );
static void associate_x11( struct wl_listener *listener, void *data );
static void configure_x11( struct wl_listener *listener, void *data );
static void create_notify_x11( struct wl_listener *listener, void *data );
static void dissociate_x11( struct wl_listener *listener, void *data );
static void set_hints( struct wl_listener *listener, void *data );
static void xwayland_ready( struct wl_listener *listener, void *data );


void activate_x11( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, activate);
    (void)data;

    /* Only "managed" windows can be activated */
    if (!client_is_unmanaged(c))
        wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void associate_x11( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, associate);
    (void)data;

    LISTEN(&client_surface(c)->events.map, &c->map, map_notify);
    LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmap_notify);
}

void configure_x11( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, configure);
    awl_server_t *srv = c->srv;
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
    if ((c->is_floating && c != srv->cursor.grab_client) || !selected_layout(c->mon)->arrange) {
        resize(srv, c, (struct wlr_box){.x = event->x - c->bw,
                .y = event->y - c->bw, .width = event->width + c->bw * 2,
                .height = event->height + c->bw * 2}, 0);
    } else {
        arrange(srv, c->mon);
    }
}

void create_notify_x11( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_xwayland_surface);
    struct wlr_xwayland_surface *xsurface = data;

    /* Allocate a Client for this surface */
    Client *c = xsurface->data = calloc_or_die(1, sizeof(*c));
    c->srv = srv;
    c->surface.xwayland = xsurface;
    c->type = X11;
    c->bw = client_is_unmanaged(c) ? 0 : srv->cfg->border_px;
    c->is_visible = 1;

    /* Listen to the various events it can emit */
    LISTEN(&xsurface->events.associate, &c->associate, associate_x11);
    LISTEN(&xsurface->events.destroy, &c->destroy, destroy_notify);
    LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociate_x11);
    LISTEN(&xsurface->events.request_activate, &c->activate, activate_x11);
    LISTEN(&xsurface->events.request_configure, &c->configure, configure_x11);
    LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreen_notify);
    LISTEN(&xsurface->events.set_hints, &c->set_hints, set_hints);
    LISTEN(&xsurface->events.set_title, &c->set_title, update_title);
}

void dissociate_x11( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, dissociate);
    (void)data;
    wl_list_remove(&c->map.link);
    wl_list_remove(&c->unmap.link);
}

void set_hints( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, set_hints);
    awl_server_t *srv = c->srv;
    struct wlr_surface *surface = client_surface(c);
    (void)data;
    if (c == focus_top(srv, srv->out.sel_mon) || !c->surface.xwayland->hints)
        return;

    c->is_urgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
    bars_dirty(srv);

    if (c->is_urgent && surface && surface->mapped)
        client_set_border_color(c, (float[])RGBA(srv->clients.borders[BorderUrg]));
}

void xwayland_ready( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.xwayland_ready);
    struct wlr_xcursor *xcursor;
    (void)data;

    /* assign the one and only seat */
    wlr_xwayland_set_seat(srv->xwayland, srv->seat);

    /* Set the default XWayland cursor to match the rest of awl. */
    if ((xcursor = wlr_xcursor_manager_get_xcursor(srv->cursor.mgr, "default", 1)))
        wlr_xwayland_set_cursor(srv->xwayland, wlr_xcursor_image_get_buffer(xcursor->images[0]),
                xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}

void xwayland_init( awl_server_t *srv ) {
    /*
     * Initialise the XWayland X server.
     * It will be started when the first X client is started.
     */
    if ((srv->xwayland = wlr_xwayland_create(srv->dpy, srv->compositor, 1))) {
        LISTEN(&srv->xwayland->events.ready, &srv->listen.xwayland_ready, xwayland_ready);
        LISTEN(&srv->xwayland->events.new_surface, &srv->listen.new_xwayland_surface, create_notify_x11);

        setenv("DISPLAY", srv->xwayland->display_name, 1);
    } else {
        awl_wrn_printf("failed to setup XWayland X server, continuing without it\n");
    }
}

void xwayland_fini( awl_server_t *srv ) {
    /* Only registered in xwayland_init() if wlr_xwayland_create() succeeded;
     * removing an unregistered listener dereferences its NULL link. */
    if (srv->xwayland) {
        wl_list_remove(&srv->listen.new_xwayland_surface.link);
        wl_list_remove(&srv->listen.xwayland_ready.link);
    }
    wlr_xwayland_destroy(srv->xwayland);
    srv->xwayland = NULL;
}
#endif
