#include "awl_cursor.h"
#include "awl_bar.h"
#include "awl_capture.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_desktop.h"
#include "awl_input.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_popup.h"

typedef struct {
    struct wlr_pointer_constraint_v1 *constraint;
    struct wl_listener destroy;
    awl_server_t *srv;
} PointerConstraint;

/** a drag icon's destroy listener */
typedef struct {
    struct wl_listener destroy;
    awl_server_t *srv;
} DragIcon;

static int desktop_at( awl_server_t *srv, double x, double y );
static void cursor_constrain( awl_server_t *srv, struct wlr_pointer_constraint_v1 *constraint );
static void cursor_warp_to_hint( awl_server_t *srv );
static void destroy_drag_icon( struct wl_listener *listener, void *data );
static void destroy_pointer_constraint( struct wl_listener *listener, void *data );
static void pointer_focus( awl_server_t *srv, Client *c, struct wlr_surface *surface, double sx, double sy,
                          uint32_t time );

void axis_notify( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.cursor_axis);
    /* This event is forwarded by the cursor when a pointer emits an axis event,
     * for example when you move the scroll wheel. */
    struct wlr_pointer_axis_event *event = data;
    activity(srv);

    Client* c = NULL;
    awl_window_t *twin;

    /* a grab drops the scrolling */
    if (srv->cursor.mode == CurGrab)
        return;

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(srv->seat);
    uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

    /* A popup that takes the pointer gets the scrolling, as it gets the
     * presses; one with only a click callback drops it: its motion took the
     * pointer focus from what is below anyway. An open xdg_popup's grab
     * comes first, as for the buttons. */
    if (!wlr_seat_pointer_has_grab(srv->seat) && (twin = popup_pointer(srv))) {
        int row, col;
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: axis %d %g (%d/120) at %.0f,%.0f\n", (void *)twin,
                event->orientation, event->delta, event->delta_discrete, srv->cursor.wlr->x, srv->cursor.wlr->y);
        /* the callback may destroy it */
        awl_window_pointer_axis(twin, srv->cursor.wlr->x, srv->cursor.wlr->y, event->orientation, event->delta,
                event->delta_discrete, &row, &col);
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: that is row %d, column %d\n", (void *)twin, row, col);
        return;
    }
    xy_to_node(srv, srv->cursor.wlr->x, srv->cursor.wlr->y, NULL, &c, NULL, NULL, NULL);
    if (!c && !srv->clients.exclusive_focus && !srv->locked && bar_axis(srv, event->delta, CLEAN_MASK(mods)))
        return;
    /* No bindings for the wheel (as MODKEY+wheel): Button binds presses only,
     * on purpose. Notify the client with pointer focus of the axis event. */
    wlr_seat_pointer_notify_axis(srv->seat,
            event->time_msec, event->orientation, event->delta,
            event->delta_discrete, event->source, event->relative_direction);
}

int desktop_at( awl_server_t *srv, double x, double y ) {
    /* only the wallpaper, background layer surfaces (LyrBg) and the desktop
     * panels, which aren't in layers[], are below; LyrBlock only matters
     * while locked */
    for (int i = LyrBottom; i < LyrBlock; i++)
        if (wlr_scene_node_at(&srv->layers[i]->node, x, y, NULL, NULL))
            return 0;
    return xy_to_mon(srv, x, y) != NULL;
}

void button_press( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.cursor_button);
    struct wlr_pointer_button_event *event = data;
    Client *c;
    awl_window_t *twin;

    activity(srv);

    /* a grab takes every press and release, before an open menu's grab */
    if (srv->cursor.mode == CurGrab) {
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(srv->seat);
        grab_button(srv, event->button, event->state == WL_POINTER_BUTTON_STATE_PRESSED,
                keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0);
        return;
    }

    /* a press an awl window took (below) swallows its release too, so
     * nothing below sees half a click */
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && srv->cursor.swallow_release) {
        srv->cursor.swallow_release = 0;
        return;
    }
    /* An open xdg_popup (a client's menu) holds a pointer grab and
     * closes itself on a press outside its client; that only works if the
     * press reaches the grab, so the bar, the desktop and the bindings don't
     * get to see it first. */
    if (wlr_seat_pointer_has_grab(srv->seat)) {
        /* the press that opened the menu (or started a drag) set this */
        if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && srv->cursor.mode == CurPressed)
            srv->cursor.mode = CurNormal;
        wlr_seat_pointer_notify_button(srv->seat,
                event->time_msec, event->button, event->state);
        return;
    }
    /* a popup that takes the pointer gets the press, and its release */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && !srv->locked
            && (twin = popup_at(srv, srv->cursor.wlr->x, srv->cursor.wlr->y))) {
        int row, col;
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: button %u at %.0f,%.0f\n", (void *)twin,
                event->button, srv->cursor.wlr->x, srv->cursor.wlr->y);
        /* the callback may destroy it */
        awl_window_pointer_button(twin, srv->cursor.wlr->x, srv->cursor.wlr->y, event->button, &row, &col);
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: that is row %d, column %d\n", (void *)twin, row, col);
        srv->cursor.swallow_release = 1;
        return;
    }
    /* the other popups take no input; a press anywhere but on the bar (whose
     * widgets close their own on a press, see below) closes them, and goes on
     * to what is below */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && !wl_list_empty(&srv->popups.list)
            && !pointer_on_bar(srv))
        popups_hide_all(srv);

    /* the bar's are ClkBar (bar_button()) */
    unsigned int click = ClkRoot;
    xy_to_node(srv, srv->cursor.wlr->x, srv->cursor.wlr->y, NULL, &c, NULL, NULL, NULL);
    if (c)
        click = ClkClient;

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(srv->seat);
    uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

    if (!c && !srv->clients.exclusive_focus && event->state == WL_POINTER_BUTTON_STATE_PRESSED && !srv->locked
            && bar_button(srv, event->button, CLEAN_MASK(mods), &click))
        return;

    switch (event->state) {
    case WL_POINTER_BUTTON_STATE_PRESSED:
        srv->cursor.mode = CurPressed;
        set_sel_mon(srv, xy_to_mon(srv, srv->cursor.wlr->x, srv->cursor.wlr->y));
        if (srv->locked)
            break;

        /* Change focus if the button was _pressed_ over a client */
        xy_to_node(srv, srv->cursor.wlr->x, srv->cursor.wlr->y, NULL, &c, NULL, NULL, NULL);
        if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
            focus_client(srv, c, 1);

        for (const Button *b = srv->cfg->buttons.v; b < srv->cfg->buttons.v + srv->cfg->buttons.n; b++) {
            if (CLEAN_MASK(mods) == CLEAN_MASK(bind_mods(srv->cfg, b->mod)) &&
                    event->button == b->button && b->func) {
                if (b->click == click) {
                    b->func(srv, &b->arg);
                    return;
                }
            }
        }
        /* the library declines the modified clicks it has no use for */
        if (click == ClkRoot && !srv->clients.exclusive_focus && desktop_at(srv, srv->cursor.wlr->x, srv->cursor.wlr->y)
                && desktop_click(srv->proc, srv->desktop, event->button, CLEAN_MASK(mods)))
            return;
        break;
    case WL_POINTER_BUTTON_STATE_RELEASED:
        /* If you released any buttons, we exit interactive move/resize mode. */
        if (!srv->locked && srv->cursor.mode != CurNormal && srv->cursor.mode != CurPressed) {
            wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr, "default");
            srv->cursor.mode = CurNormal;
            /* Drop the window off on its new monitor */
            srv->out.sel_mon = xy_to_mon(srv, srv->cursor.wlr->x, srv->cursor.wlr->y);
            set_mon(srv, srv->cursor.grab_client, srv->out.sel_mon, 0);
            srv->cursor.grab_client = NULL;
            /* the cursor image of what is below: set_cursor() ignored the
             * clients' during the grab, and the one under the pointer kept
             * its focus, so it would only set its own again on the next
             * enter. A leave and an enter make it set it now. */
            wlr_seat_pointer_notify_clear_focus(srv->seat);
            motion_notify(srv, 0, NULL, 0, 0, 0, 0);
            return;
        }
        srv->cursor.mode = CurNormal;
        break;
    }

    /* If the event wasn't handled by the compositor, notify the client with
     * pointer focus that a button press has occurred */
    wlr_seat_pointer_notify_button(srv->seat,
            event->time_msec, event->button, event->state);
}

void create_pointer_constraint( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_pointer_constraint);
    PointerConstraint *pointer_constraint = calloc_or_die(1, sizeof(*pointer_constraint));
    pointer_constraint->srv = srv;
    pointer_constraint->constraint = data;
    LISTEN(&pointer_constraint->constraint->events.destroy,
            &pointer_constraint->destroy, destroy_pointer_constraint);
}

void cursor_constrain( awl_server_t *srv, struct wlr_pointer_constraint_v1 *constraint ) {
    if (srv->cursor.active_constraint == constraint)
        return;

    if (srv->cursor.active_constraint)
        wlr_pointer_constraint_v1_send_deactivated(srv->cursor.active_constraint);

    srv->cursor.active_constraint = constraint;
    wlr_pointer_constraint_v1_send_activated(constraint);
}

void cursor_frame( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.cursor_frame);
    /* This event is forwarded by the cursor when a pointer emits a frame
     * event. Frame events are sent after regular pointer events to group
     * multiple events together. For instance, two axis events may happen at the
     * same time, in which case a frame event won't be sent in between. */
    (void)data;
    /* Notify the client with pointer focus of the frame event. */
    wlr_seat_pointer_notify_frame(srv->seat);
}

void cursor_warp_to_hint( awl_server_t *srv ) {
    Client *c = NULL;
    double sx = srv->cursor.active_constraint->current.cursor_hint.x;
    double sy = srv->cursor.active_constraint->current.cursor_hint.y;

    toplevel_from_wlr_surface(srv->cursor.active_constraint->surface, &c, NULL);
    if (c && srv->cursor.active_constraint->current.cursor_hint.enabled) {
        wlr_cursor_warp(srv->cursor.wlr, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
        wlr_seat_pointer_warp(srv->cursor.active_constraint->seat, sx, sy);
    }
}

void destroy_drag_icon( struct wl_listener *listener, void *data ) {
    DragIcon *icon = wl_container_of(listener, icon, destroy);
    awl_server_t *srv = icon->srv;
    (void)data;
    /* Focus enter isn't sent during drag, so refocus the focused node. */
    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    motion_notify(srv, 0, NULL, 0, 0, 0, 0);
    wl_list_remove(&icon->destroy.link);
    free(icon);
}

void destroy_pointer_constraint( struct wl_listener *listener, void *data ) {
    PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);
    awl_server_t *srv = pointer_constraint->srv;
    (void)data;

    if (srv->cursor.active_constraint == pointer_constraint->constraint) {
        cursor_warp_to_hint(srv);
        srv->cursor.active_constraint = NULL;
    }

    wl_list_remove(&pointer_constraint->destroy.link);
    free(pointer_constraint);
}

void motion_absolute( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.cursor_motion_absolute);
    /* This event is forwarded by the cursor when a pointer emits an _absolute_
     * motion event, from 0..1 on each axis. This happens, for example, when
     * wlroots is running under a Wayland window rather than KMS+DRM, and you
     * move the mouse over the window. You could enter the window from any edge,
     * so we have to warp the mouse there. Also, some hardware emits these events. */
    struct wlr_pointer_motion_absolute_event *event = data;
    double lx, ly;

    if (!event->time_msec) /* this is 0 with virtual pointers */
        wlr_cursor_warp_absolute(srv->cursor.wlr, &event->pointer->base, event->x, event->y);

    wlr_cursor_absolute_to_layout_coords(srv->cursor.wlr, &event->pointer->base, event->x, event->y, &lx, &ly);
    double dx = lx - srv->cursor.wlr->x;
    double dy = ly - srv->cursor.wlr->y;
    motion_notify(srv, event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

void motion_notify( awl_server_t *srv, uint32_t time, struct wlr_input_device *device, double dx, double dy,
                   double dx_unaccel, double dy_unaccel ) {
    double sx = 0, sy = 0, sx_confined, sy_confined;
    Client *c = NULL, *w = NULL;
    LayerSurface *l = NULL;
    struct wlr_surface *surface = NULL;
    struct wlr_pointer_constraint_v1 *constraint;

    /* time is 0 in internal calls meant to restore pointer focus. */
    if (time) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
                srv->cursor.relative_mgr, srv->seat, (uint64_t)time * 1000,
                dx, dy, dx_unaccel, dy_unaccel);

        wl_list_for_each(constraint, &srv->cursor.constraints->constraints, link)
            cursor_constrain(srv, constraint);

        if (srv->cursor.active_constraint && srv->cursor.mode != CurResize && srv->cursor.mode != CurMove
                && srv->cursor.mode != CurGrab) {
            toplevel_from_wlr_surface(srv->cursor.active_constraint->surface, &c, NULL);
            if (c && srv->cursor.active_constraint->surface == srv->seat->pointer_state.focused_surface) {
                sx = srv->cursor.wlr->x - c->geom.x - c->bw;
                sy = srv->cursor.wlr->y - c->geom.y - c->bw;
                if (wlr_region_confine(&srv->cursor.active_constraint->region, sx, sy,
                        sx + dx, sy + dy, &sx_confined, &sy_confined)) {
                    dx = sx_confined - sx;
                    dy = sy_confined - sy;
                }

                if (srv->cursor.active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
                    return;
            }
        }

        wlr_cursor_move(srv->cursor.wlr, device, dx, dy);
        activity(srv);

        /* Update sel_mon (even while dragging a window) */
        if (srv->cfg->sloppy_focus)
            set_sel_mon(srv, xy_to_mon(srv, srv->cursor.wlr->x, srv->cursor.wlr->y));
    }

    /* a grab gets the motion, nothing below does */
    if (srv->cursor.mode == CurGrab) {
        wlr_scene_node_set_position(&srv->cursor.drag_icon->node, (int)round(srv->cursor.wlr->x), (int)round(srv->cursor.wlr->y));
        grab_motion(srv);
        return;
    }

    /* Find the client under the pointer and send the event along. */
    xy_to_node(srv, srv->cursor.wlr->x, srv->cursor.wlr->y, &surface, &c, NULL, &sx, &sy);

    if (srv->cursor.mode == CurPressed && !srv->seat->drag
            && surface != srv->seat->pointer_state.focused_surface
            && toplevel_from_wlr_surface(srv->seat->pointer_state.focused_surface, &w, &l) >= 0) {
        c = w;
        surface = srv->seat->pointer_state.focused_surface;
        sx = srv->cursor.wlr->x - (l ? l->scene->node.x : w->geom.x);
        sy = srv->cursor.wlr->y - (l ? l->scene->node.y : w->geom.y);
    }

    /* Update drag icon's position */
    wlr_scene_node_set_position(&srv->cursor.drag_icon->node, (int)round(srv->cursor.wlr->x), (int)round(srv->cursor.wlr->y));

    /* If we are currently grabbing the mouse, handle and return */
    if (srv->cursor.mode == CurMove) {
        /* Move the grabbed client to the new position. */
        resize(srv, srv->cursor.grab_client, (struct wlr_box){.x = (int)round(srv->cursor.wlr->x) - srv->cursor.grab_cx, .y = (int)round(srv->cursor.wlr->y) - srv->cursor.grab_cy,
            .width = srv->cursor.grab_client->geom.width, .height = srv->cursor.grab_client->geom.height}, 1);
        return;
    } else if (srv->cursor.mode == CurResize) {
        resize(srv, srv->cursor.grab_client, (struct wlr_box){.x = srv->cursor.grab_client->geom.x, .y = srv->cursor.grab_client->geom.y,
            .width = (int)round(srv->cursor.wlr->x) - srv->cursor.grab_client->geom.x, .height = (int)round(srv->cursor.wlr->y) - srv->cursor.grab_client->geom.y}, 1);
        return;
    }

    /* awl's popups that take the pointer keep it from what is below */
    if (popup_motion(srv))
        return;

    /* If there's no client surface under the cursor, set the cursor image to a
     * default. This is what makes the cursor image appear when you move it
     * off of a client or over its border. */
    if (!surface && !srv->seat->drag)
        wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr, "default");

    update_hover(srv, c);
    pointer_focus(srv, c, surface, sx, sy, time);
}

void motion_relative( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.cursor_motion);
    /* This event is forwarded by the cursor when a pointer emits a _relative_
     * pointer motion event (i.e. a delta) */
    struct wlr_pointer_motion_event *event = data;
    /* The cursor doesn't move unless we tell it to. The cursor automatically
     * handles constraining the motion to the output layout, as well as any
     * special configuration applied for the specific input device which
     * generated the event. You can pass NULL for the device if you want to move
     * the cursor around without any input. */
    motion_notify(srv, event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
}

void move_resize( awl_server_t *srv, const Arg *arg ) {
    if (srv->cursor.mode != CurNormal && srv->cursor.mode != CurPressed)
        return;
    xy_to_node(srv, srv->cursor.wlr->x, srv->cursor.wlr->y, NULL, &srv->cursor.grab_client, NULL, NULL, NULL);
    if (!srv->cursor.grab_client || client_is_unmanaged(srv->cursor.grab_client) || srv->cursor.grab_client->is_fullscreen)
        return;

    /* Float the window and tell motion_notify to grab it */
    set_floating(srv, srv->cursor.grab_client, 1);
    switch (srv->cursor.mode = arg->ui) {
    case CurMove:
        srv->cursor.grab_cx = (int)round(srv->cursor.wlr->x) - srv->cursor.grab_client->geom.x;
        srv->cursor.grab_cy = (int)round(srv->cursor.wlr->y) - srv->cursor.grab_client->geom.y;
        wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr, "all-scroll");
        break;
    case CurResize:
        /* Doesn't work for X11 output - the next absolute motion event
         * returns the cursor to where it started */
        wlr_cursor_warp_closest(srv->cursor.wlr, NULL,
                srv->cursor.grab_client->geom.x + srv->cursor.grab_client->geom.width,
                srv->cursor.grab_client->geom.y + srv->cursor.grab_client->geom.height);
        wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr, "se-resize");
        break;
    }
}

void pointer_focus( awl_server_t *srv, Client *c, struct wlr_surface *surface, double sx, double sy, uint32_t time ) {
    if (surface != srv->seat->pointer_state.focused_surface &&
            srv->cfg->sloppy_focus && time && c && !client_is_unmanaged(c))
        focus_client(srv, c, 0);

    /* If surface is NULL, clear pointer focus */
    if (!surface) {
        wlr_seat_pointer_notify_clear_focus(srv->seat);
        return;
    }

    if (!time) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
    }

    /* Let the client know that the mouse cursor has entered one
     * of its surfaces, and make keyboard focus follow if desired.
     * wlroots makes this a no-op if surface is already focused */
    wlr_seat_pointer_notify_enter(srv->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(srv->seat, time, sx, sy);
}

void request_start_drag( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;

    if (wlr_seat_validate_pointer_grab_serial(srv->seat, event->origin,
            event->serial))
        wlr_seat_start_pointer_drag(srv->seat, event->drag, event->serial);
    else
        wlr_data_source_destroy(event->drag->source);
}

void set_cursor( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.request_cursor);
    /* This event is raised by the seat when a client provides a cursor image */
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    /* If we're "grabbing" the cursor, don't use the client's image, we will
     * restore it after "grabbing" sending a leave event, followed by a enter
     * event, which will result in the client requesting set the cursor surface */
    if (srv->cursor.mode != CurNormal && srv->cursor.mode != CurPressed)
        return;
    /* This can be sent by any client, so we check to make sure this one
     * actually has pointer focus first. If so, we can tell the cursor to
     * use the provided surface as the cursor image. It will set the
     * hardware cursor on the output that it's currently on and continue to
     * do so as the cursor moves between outputs. */
    if (event->seat_client == srv->seat->pointer_state.focused_client)
        wlr_cursor_set_surface(srv->cursor.wlr, event->surface,
                event->hotspot_x, event->hotspot_y);
}

void set_cursor_shape( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.request_set_cursor_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    if (srv->cursor.mode != CurNormal && srv->cursor.mode != CurPressed)
        return;
    /* This can be sent by any client, so we check to make sure this one
     * actually has pointer focus first. If so, we can tell the cursor to
     * use the provided cursor shape. */
    if (event->seat_client == srv->seat->pointer_state.focused_client)
        wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr,
                wlr_cursor_shape_v1_name(event->shape));
}

void start_drag( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.start_drag);
    struct wlr_drag *drag = data;
    if (!drag->icon)
        return;

    drag->icon->data = &wlr_scene_drag_icon_create(srv->cursor.drag_icon, drag->icon)->node;
    DragIcon *icon = calloc_or_die(1, sizeof(*icon));
    icon->srv = srv;
    LISTEN(&drag->icon->events.destroy, &icon->destroy, destroy_drag_icon);
}

void xy_to_node( awl_server_t *srv, double x, double y, struct wlr_surface **psurface, Client **pc, LayerSurface **pl,
               double *nx, double *ny ) {
    struct wlr_surface *surface = NULL;
    struct wlr_scene_surface *scene_surface = NULL;
    Client *c = NULL;
    LayerSurface *l = NULL;

    /* the topmost layer with something of a client's or a layer surface's
     * there has it: what is below a window's border must not lend it its
     * surface */
    for (int layer = NUM_LAYERS - 1; !surface && !c && !l && layer >= 0; layer--) {
        struct wlr_scene_node *node = wlr_scene_node_at(&srv->layers[layer]->node, x, y, nx, ny);
        if (!node)
            continue;

        if (node->type == WLR_SCENE_NODE_BUFFER) {
            scene_surface = wlr_scene_surface_try_from_buffer(
                    wlr_scene_buffer_from_node(node));
            if (!scene_surface) continue;
            surface = scene_surface->surface;
        }
        /* Walk the tree to find a node that knows the client */
        for (struct wlr_scene_node *pnode = node; pnode && !c; pnode = &pnode->parent->node)
            c = pnode->data;
        /* the walk went on past the node that had it */
        if (c && c->type == LayerShell) {
            l = (LayerSurface *)c;
            c = NULL;
        }
    }

    if (psurface) *psurface = surface;
    if (pc) *pc = c;
    if (pl) *pl = l;
}
