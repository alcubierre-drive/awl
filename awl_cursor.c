#include "awl_cursor.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_desktop.h"
#include "awl_input.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_notifications.h"
#include "awl_output.h"
#include "awl_popup.h"

typedef struct {
    struct wlr_pointer_constraint_v1 *constraint;
    struct wl_listener destroy;
} PointerConstraint;

static int desktopat(double x, double y);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void cursorwarptohint(void);
static void destroydragicon(struct wl_listener *listener, void *data);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
        uint32_t time);

static int grabcx, grabcy; /* client-relative */
static struct wlr_pointer_constraint_v1 *active_constraint;

void
axisnotify(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits an axis event,
     * for example when you move the scroll wheel. */
    struct wlr_pointer_axis_event *event = data;
    wlr_idle_notifier_v1_notify_activity(awl->idle_notifier, awl->seat);
    awl_notifications_activity();

    Client* c = NULL;
    awl_window_t *twin;
    int row, col;

    /* A popup that takes the pointer gets the scrolling, as it gets the
     * presses; one with only a click callback drops it: its motion took the
     * pointer focus from what is below anyway. An open xdg_popup's grab
     * comes first, as for the buttons. */
    if (!wlr_seat_pointer_has_grab(awl->seat) && (twin = popuppointer())) {
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: axis %d %g (%d/120) at %.0f,%.0f\n", (void *)twin,
                event->orientation, event->delta, event->delta_discrete, awl->cursor.wlr->x, awl->cursor.wlr->y);
        /* the callback may destroy it */
        awl_window_pointer_axis(twin, awl->cursor.wlr->x, awl->cursor.wlr->y, event->orientation, event->delta,
                event->delta_discrete, &row, &col);
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: that is row %d, column %d\n", (void *)twin, row, col);
        return;
    }
    xytonode(awl->cursor.wlr->x, awl->cursor.wlr->y, NULL, &c, NULL, NULL, NULL);
    if (!c && !awl->clients.exclusive_focus && !awl->locked && baraxis(event->delta))
        return;
    /* TODO: allow usage of scroll whell for mousebindings, it can be implemented
     * checking the event's orientation and the delta of the event */
    /* Notify the client with pointer focus of the axis event. */
    wlr_seat_pointer_notify_axis(awl->seat,
            event->time_msec, event->orientation, event->delta,
            event->delta_discrete, event->source, event->relative_direction);
}

int
desktopat(double x, double y)
{
    /* only the wallpaper, background layer surfaces (LyrBg) and the desktop
     * panels, which aren't in layers[], are below; LyrBlock only matters
     * while locked */
    int i;
    for (i = LyrBottom; i < LyrBlock; i++)
        if (wlr_scene_node_at(&awl->layers[i]->node, x, y, NULL, NULL))
            return 0;
    return xytomon(x, y) != NULL;
}

void
buttonpress(struct wl_listener *listener, void *data)
{
    unsigned int click;
    struct wlr_pointer_button_event *event = data;
    struct wlr_keyboard *keyboard;
    uint32_t mods;
    Client *c;
    const Button *b;
    awl_window_t *twin;
    static int swallow_release;

    wlr_idle_notifier_v1_notify_activity(awl->idle_notifier, awl->seat);
    awl_notifications_activity();

    /* a press an awl window took (below) swallows its release too, so
     * nothing below sees half a click */
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && swallow_release) {
        swallow_release = 0;
        return;
    }
    /* An open xdg_popup (a client's menu) holds a pointer grab and
     * closes itself on a press outside its client; that only works if the
     * press reaches the grab, so the bar, the desktop and the bindings don't
     * get to see it first. */
    if (wlr_seat_pointer_has_grab(awl->seat)) {
        /* the press that opened the menu (or started a drag) set this */
        if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && awl->cursor.mode == CurPressed)
            awl->cursor.mode = CurNormal;
        wlr_seat_pointer_notify_button(awl->seat,
                event->time_msec, event->button, event->state);
        return;
    }
    /* a popup that takes the pointer gets the press, and its release */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && !awl->locked
            && (twin = popupat(awl->cursor.wlr->x, awl->cursor.wlr->y))) {
        int row, col;
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: button %u at %.0f,%.0f\n", (void *)twin,
                event->button, awl->cursor.wlr->x, awl->cursor.wlr->y);
        /* the callback may destroy it */
        awl_window_pointer_button(twin, awl->cursor.wlr->x, awl->cursor.wlr->y, event->button, &row, &col);
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: that is row %d, column %d\n", (void *)twin, row, col);
        swallow_release = 1;
        return;
    }
    /* the other popups take no input; a press anywhere but on the bar (whose
     * widgets close their own on a press, see below) closes them, and goes on
     * to what is below */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && !wl_list_empty(&awl->popups)
            && !pointeronbar())
        popupshideall();

    click = ClkRoot; // TODO we don't treat these cases
    xytonode(awl->cursor.wlr->x, awl->cursor.wlr->y, NULL, &c, NULL, NULL, NULL);
    if (c)
        click = ClkClient;

    if (!c && !awl->clients.exclusive_focus && event->state == WL_POINTER_BUTTON_STATE_PRESSED && !awl->locked
            && barbutton(event->button, &click))
        return;

    switch (event->state) {
    case WL_POINTER_BUTTON_STATE_PRESSED:
        awl->cursor.mode = CurPressed;
        setselmon(xytomon(awl->cursor.wlr->x, awl->cursor.wlr->y));
        if (awl->locked)
            break;

        /* Change focus if the button was _pressed_ over a client */
        xytonode(awl->cursor.wlr->x, awl->cursor.wlr->y, NULL, &c, NULL, NULL, NULL);
        if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
            focusclient(c, 1);

        keyboard = wlr_seat_get_keyboard(awl->seat);
        mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
        for (b = awl->cfg->buttons.v; b < awl->cfg->buttons.v + awl->cfg->buttons.n; b++) {
            if (CLEANMASK(mods) == CLEANMASK(bindmods(b->mod)) &&
                    event->button == b->button && b->func) {
                if (b->click == click) {
                    b->func(&b->arg);
                    return;
                }
            }
        }
        /* the library declines the modified clicks it has no use for */
        if (click == ClkRoot && !awl->clients.exclusive_focus && desktopat(awl->cursor.wlr->x, awl->cursor.wlr->y)
                && desktop_click(event->button, CLEANMASK(mods)))
            return;
        break;
    case WL_POINTER_BUTTON_STATE_RELEASED:
        /* If you released any buttons, we exit interactive move/resize mode. */
        /* TODO: should reset to the pointer focus's current setcursor */
        if (!awl->locked && awl->cursor.mode != CurNormal && awl->cursor.mode != CurPressed) {
            wlr_cursor_set_xcursor(awl->cursor.wlr, awl->cursor.mgr, "default");
            awl->cursor.mode = CurNormal;
            /* Drop the window off on its new monitor */
            awl->out.selmon = xytomon(awl->cursor.wlr->x, awl->cursor.wlr->y);
            setmon(awl->cursor.grabc, awl->out.selmon, 0);
            awl->cursor.grabc = NULL;
            return;
        }
        awl->cursor.mode = CurNormal;
        break;
    }

    /* If the event wasn't handled by the compositor, notify the client with
     * pointer focus that a button press has occurred */
    wlr_seat_pointer_notify_button(awl->seat,
            event->time_msec, event->button, event->state);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
    PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
    pointer_constraint->constraint = data;
    LISTEN(&pointer_constraint->constraint->events.destroy,
            &pointer_constraint->destroy, destroypointerconstraint);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
    if (active_constraint == constraint)
        return;

    if (active_constraint)
        wlr_pointer_constraint_v1_send_deactivated(active_constraint);

    active_constraint = constraint;
    wlr_pointer_constraint_v1_send_activated(constraint);
}

void
cursorframe(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits a frame
     * event. Frame events are sent after regular pointer events to group
     * multiple events together. For instance, two axis events may happen at the
     * same time, in which case a frame event won't be sent in between. */
    /* Notify the client with pointer focus of the frame event. */
    wlr_seat_pointer_notify_frame(awl->seat);
}

void
cursorwarptohint(void)
{
    Client *c = NULL;
    double sx = active_constraint->current.cursor_hint.x;
    double sy = active_constraint->current.cursor_hint.y;

    toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
    if (c && active_constraint->current.cursor_hint.enabled) {
        wlr_cursor_warp(awl->cursor.wlr, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
        wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
    }
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
    /* Focus enter isn't sent during drag, so refocus the focused node. */
    focusclient(focustop(awl->out.selmon), 1);
    motionnotify(0, NULL, 0, 0, 0, 0);
    wl_list_remove(&listener->link);
    free(listener);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
    PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

    if (active_constraint == pointer_constraint->constraint) {
        cursorwarptohint();
        active_constraint = NULL;
    }

    wl_list_remove(&pointer_constraint->destroy.link);
    free(pointer_constraint);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits an _absolute_
     * motion event, from 0..1 on each axis. This happens, for example, when
     * wlroots is running under a Wayland window rather than KMS+DRM, and you
     * move the mouse over the window. You could enter the window from any edge,
     * so we have to warp the mouse there. Also, some hardware emits these events. */
    struct wlr_pointer_motion_absolute_event *event = data;
    double lx, ly, dx, dy;

    if (!event->time_msec) /* this is 0 with virtual pointers */
        wlr_cursor_warp_absolute(awl->cursor.wlr, &event->pointer->base, event->x, event->y);

    wlr_cursor_absolute_to_layout_coords(awl->cursor.wlr, &event->pointer->base, event->x, event->y, &lx, &ly);
    dx = lx - awl->cursor.wlr->x;
    dy = ly - awl->cursor.wlr->y;
    motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
        double dx_unaccel, double dy_unaccel)
{
    double sx = 0, sy = 0, sx_confined, sy_confined;
    Client *c = NULL, *w = NULL;
    LayerSurface *l = NULL;
    struct wlr_surface *surface = NULL;
    struct wlr_pointer_constraint_v1 *constraint;

    /* time is 0 in internal calls meant to restore pointer focus. */
    if (time) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
                awl->cursor.relative_mgr, awl->seat, (uint64_t)time * 1000,
                dx, dy, dx_unaccel, dy_unaccel);

        wl_list_for_each(constraint, &awl->cursor.constraints->constraints, link)
            cursorconstrain(constraint);

        if (active_constraint && awl->cursor.mode != CurResize && awl->cursor.mode != CurMove) {
            toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
            if (c && active_constraint->surface == awl->seat->pointer_state.focused_surface) {
                sx = awl->cursor.wlr->x - c->geom.x - c->bw;
                sy = awl->cursor.wlr->y - c->geom.y - c->bw;
                if (wlr_region_confine(&active_constraint->region, sx, sy,
                        sx + dx, sy + dy, &sx_confined, &sy_confined)) {
                    dx = sx_confined - sx;
                    dy = sy_confined - sy;
                }

                if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
                    return;
            }
        }

        wlr_cursor_move(awl->cursor.wlr, device, dx, dy);
        wlr_idle_notifier_v1_notify_activity(awl->idle_notifier, awl->seat);
        awl_notifications_activity();

        /* Update selmon (even while dragging a window) */
        if (awl->cfg->sloppyfocus)
            setselmon(xytomon(awl->cursor.wlr->x, awl->cursor.wlr->y));
    }

    /* Find the client under the pointer and send the event along. */
    xytonode(awl->cursor.wlr->x, awl->cursor.wlr->y, &surface, &c, NULL, &sx, &sy);

    if (awl->cursor.mode == CurPressed && !awl->seat->drag
            && surface != awl->seat->pointer_state.focused_surface
            && toplevel_from_wlr_surface(awl->seat->pointer_state.focused_surface, &w, &l) >= 0) {
        c = w;
        surface = awl->seat->pointer_state.focused_surface;
        sx = awl->cursor.wlr->x - (l ? l->scene->node.x : w->geom.x);
        sy = awl->cursor.wlr->y - (l ? l->scene->node.y : w->geom.y);
    }

    /* Update drag icon's position */
    wlr_scene_node_set_position(&awl->cursor.drag_icon->node, (int)round(awl->cursor.wlr->x), (int)round(awl->cursor.wlr->y));

    /* If we are currently grabbing the mouse, handle and return */
    if (awl->cursor.mode == CurMove) {
        /* Move the grabbed client to the new position. */
        resize(awl->cursor.grabc, (struct wlr_box){.x = (int)round(awl->cursor.wlr->x) - grabcx, .y = (int)round(awl->cursor.wlr->y) - grabcy,
            .width = awl->cursor.grabc->geom.width, .height = awl->cursor.grabc->geom.height}, 1);
        return;
    } else if (awl->cursor.mode == CurResize) {
        resize(awl->cursor.grabc, (struct wlr_box){.x = awl->cursor.grabc->geom.x, .y = awl->cursor.grabc->geom.y,
            .width = (int)round(awl->cursor.wlr->x) - awl->cursor.grabc->geom.x, .height = (int)round(awl->cursor.wlr->y) - awl->cursor.grabc->geom.y}, 1);
        return;
    }

    /* awl's popups that take the pointer keep it from what is below */
    if (popupmotion())
        return;

    /* If there's no client surface under the cursor, set the cursor image to a
     * default. This is what makes the cursor image appear when you move it
     * off of a client or over its border. */
    if (!surface && !awl->seat->drag)
        wlr_cursor_set_xcursor(awl->cursor.wlr, awl->cursor.mgr, "default");

    updatehover(c);
    pointerfocus(c, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits a _relative_
     * pointer motion event (i.e. a delta) */
    struct wlr_pointer_motion_event *event = data;
    /* The cursor doesn't move unless we tell it to. The cursor automatically
     * handles constraining the motion to the output layout, as well as any
     * special configuration applied for the specific input device which
     * generated the event. You can pass NULL for the device if you want to move
     * the cursor around without any input. */
    motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
    if (awl->cursor.mode != CurNormal && awl->cursor.mode != CurPressed)
        return;
    xytonode(awl->cursor.wlr->x, awl->cursor.wlr->y, NULL, &awl->cursor.grabc, NULL, NULL, NULL);
    if (!awl->cursor.grabc || client_is_unmanaged(awl->cursor.grabc) || awl->cursor.grabc->isfullscreen)
        return;

    /* Float the window and tell motionnotify to grab it */
    setfloating(awl->cursor.grabc, 1);
    switch (awl->cursor.mode = arg->ui) {
    case CurMove:
        grabcx = (int)round(awl->cursor.wlr->x) - awl->cursor.grabc->geom.x;
        grabcy = (int)round(awl->cursor.wlr->y) - awl->cursor.grabc->geom.y;
        wlr_cursor_set_xcursor(awl->cursor.wlr, awl->cursor.mgr, "all-scroll");
        break;
    case CurResize:
        /* Doesn't work for X11 output - the next absolute motion event
         * returns the cursor to where it started */
        wlr_cursor_warp_closest(awl->cursor.wlr, NULL,
                awl->cursor.grabc->geom.x + awl->cursor.grabc->geom.width,
                awl->cursor.grabc->geom.y + awl->cursor.grabc->geom.height);
        wlr_cursor_set_xcursor(awl->cursor.wlr, awl->cursor.mgr, "se-resize");
        break;
    }
}

void
pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
        uint32_t time)
{
    struct timespec now;

    if (surface != awl->seat->pointer_state.focused_surface &&
            awl->cfg->sloppyfocus && time && c && !client_is_unmanaged(c))
        focusclient(c, 0);

    /* If surface is NULL, clear pointer focus */
    if (!surface) {
        wlr_seat_pointer_notify_clear_focus(awl->seat);
        return;
    }

    if (!time) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
    }

    /* Let the client know that the mouse cursor has entered one
     * of its surfaces, and make keyboard focus follow if desired.
     * wlroots makes this a no-op if surface is already focused */
    wlr_seat_pointer_notify_enter(awl->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(awl->seat, time, sx, sy);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
    struct wlr_seat_request_start_drag_event *event = data;

    if (wlr_seat_validate_pointer_grab_serial(awl->seat, event->origin,
            event->serial))
        wlr_seat_start_pointer_drag(awl->seat, event->drag, event->serial);
    else
        wlr_data_source_destroy(event->drag->source);
}

void
setcursor(struct wl_listener *listener, void *data)
{
    /* This event is raised by the seat when a client provides a cursor image */
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    /* If we're "grabbing" the cursor, don't use the client's image, we will
     * restore it after "grabbing" sending a leave event, followed by a enter
     * event, which will result in the client requesting set the cursor surface */
    if (awl->cursor.mode != CurNormal && awl->cursor.mode != CurPressed)
        return;
    /* This can be sent by any client, so we check to make sure this one
     * actually has pointer focus first. If so, we can tell the cursor to
     * use the provided surface as the cursor image. It will set the
     * hardware cursor on the output that it's currently on and continue to
     * do so as the cursor moves between outputs. */
    if (event->seat_client == awl->seat->pointer_state.focused_client)
        wlr_cursor_set_surface(awl->cursor.wlr, event->surface,
                event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    if (awl->cursor.mode != CurNormal && awl->cursor.mode != CurPressed)
        return;
    /* This can be sent by any client, so we check to make sure this one
     * actually has pointer focus first. If so, we can tell the cursor to
     * use the provided cursor shape. */
    if (event->seat_client == awl->seat->pointer_state.focused_client)
        wlr_cursor_set_xcursor(awl->cursor.wlr, awl->cursor.mgr,
                wlr_cursor_shape_v1_name(event->shape));
}

void
startdrag(struct wl_listener *listener, void *data)
{
    struct wlr_drag *drag = data;
    if (!drag->icon)
        return;

    drag->icon->data = &wlr_scene_drag_icon_create(awl->cursor.drag_icon, drag->icon)->node;
    LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void
xytonode(double x, double y, struct wlr_surface **psurface,
        Client **pc, LayerSurface **pl, double *nx, double *ny)
{
    struct wlr_scene_node *node, *pnode;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_surface *scene_surface = NULL;
    Client *c = NULL;
    LayerSurface *l = NULL;
    int layer;

    /* the topmost layer with something of a client's or a layer surface's
     * there has it: what is below a window's border must not lend it its
     * surface */
    for (layer = NUM_LAYERS - 1; !surface && !c && !l && layer >= 0; layer--) {
        if (!(node = wlr_scene_node_at(&awl->layers[layer]->node, x, y, nx, ny)))
            continue;

        if (node->type == WLR_SCENE_NODE_BUFFER) {
            scene_surface = wlr_scene_surface_try_from_buffer(
                    wlr_scene_buffer_from_node(node));
            if (!scene_surface) continue;
            surface = scene_surface->surface;
        }
        /* Walk the tree to find a node that knows the client */
        for (pnode = node; pnode && !c; pnode = &pnode->parent->node)
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
