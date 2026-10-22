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

struct wlr_cursor *cursor;
struct wlr_xcursor_manager *cursor_mgr;
struct wlr_pointer_constraints_v1 *pointer_constraints;
struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
struct wlr_scene_tree *drag_icon;
unsigned int cursor_mode;
Client *grabc;
static int grabcx, grabcy; /* client-relative */
static struct wlr_pointer_constraint_v1 *active_constraint;

void
axisnotify(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits an axis event,
     * for example when you move the scroll wheel. */
    struct wlr_pointer_axis_event *event = data;
    wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
    awl_notifications_activity();

    Client* c = NULL;
    awl_window_t *twin;
    int row, col;

    /* A popup that takes the pointer gets the scrolling, as it gets the
     * presses; one with only a click callback drops it: its motion took the
     * pointer focus from what is below anyway. An open xdg_popup's grab
     * comes first, as for the buttons. */
    if (!wlr_seat_pointer_has_grab(seat) && (twin = popuppointer())) {
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: axis %d %g (%d/120) at %.0f,%.0f\n", (void *)twin,
                event->orientation, event->delta, event->delta_discrete, cursor->x, cursor->y);
        /* the callback may destroy it */
        awl_window_pointer_axis(twin, cursor->x, cursor->y, event->orientation, event->delta,
                event->delta_discrete, &row, &col);
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: that is row %d, column %d\n", (void *)twin, row, col);
        return;
    }
    xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
    if (!c && !exclusive_focus && !locked && baraxis(event->delta))
        return;
    /* TODO: allow usage of scroll whell for mousebindings, it can be implemented
     * checking the event's orientation and the delta of the event */
    /* Notify the client with pointer focus of the axis event. */
    wlr_seat_pointer_notify_axis(seat,
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
        if (wlr_scene_node_at(&layers[i]->node, x, y, NULL, NULL))
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

    wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
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
    if (wlr_seat_pointer_has_grab(seat)) {
        /* the press that opened the menu (or started a drag) set this */
        if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && cursor_mode == CurPressed)
            cursor_mode = CurNormal;
        wlr_seat_pointer_notify_button(seat,
                event->time_msec, event->button, event->state);
        return;
    }
    /* a popup that takes the pointer gets the press, and its release */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && !locked
            && (twin = popupat(cursor->x, cursor->y))) {
        int row, col;
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: button %u at %.0f,%.0f\n", (void *)twin,
                event->button, cursor->x, cursor->y);
        /* the callback may destroy it */
        awl_window_pointer_button(twin, cursor->x, cursor->y, event->button, &row, &col);
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: that is row %d, column %d\n", (void *)twin, row, col);
        swallow_release = 1;
        return;
    }
    /* the other popups take no input; a press anywhere but on the bar (whose
     * widgets close their own on a press, see below) closes them, and goes on
     * to what is below */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && !wl_list_empty(&popups)
            && !pointeronbar())
        popupshideall();

    click = ClkRoot; // TODO we don't treat these cases
    xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
    if (c)
        click = ClkClient;

    if (!c && !exclusive_focus && event->state == WL_POINTER_BUTTON_STATE_PRESSED && !locked
            && barbutton(event->button, &click))
        return;

    switch (event->state) {
    case WL_POINTER_BUTTON_STATE_PRESSED:
        cursor_mode = CurPressed;
        setselmon(xytomon(cursor->x, cursor->y));
        if (locked)
            break;

        /* Change focus if the button was _pressed_ over a client */
        xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
        if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
            focusclient(c, 1);

        keyboard = wlr_seat_get_keyboard(seat);
        mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
        for (b = cfg->buttons.v; b < cfg->buttons.v + cfg->buttons.n; b++) {
            if (CLEANMASK(mods) == CLEANMASK(bindmods(b->mod)) &&
                    event->button == b->button && b->func) {
                if (b->click == click) {
                    b->func(&b->arg);
                    return;
                }
            }
        }
        /* the library declines the modified clicks it has no use for */
        if (click == ClkRoot && !exclusive_focus && desktopat(cursor->x, cursor->y)
                && desktop_click(event->button, CLEANMASK(mods)))
            return;
        break;
    case WL_POINTER_BUTTON_STATE_RELEASED:
        /* If you released any buttons, we exit interactive move/resize mode. */
        /* TODO: should reset to the pointer focus's current setcursor */
        if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
            wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
            cursor_mode = CurNormal;
            /* Drop the window off on its new monitor */
            selmon = xytomon(cursor->x, cursor->y);
            setmon(grabc, selmon, 0);
            grabc = NULL;
            return;
        }
        cursor_mode = CurNormal;
        break;
    }

    /* If the event wasn't handled by the compositor, notify the client with
     * pointer focus that a button press has occurred */
    wlr_seat_pointer_notify_button(seat,
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
    wlr_seat_pointer_notify_frame(seat);
}

void
cursorwarptohint(void)
{
    Client *c = NULL;
    double sx = active_constraint->current.cursor_hint.x;
    double sy = active_constraint->current.cursor_hint.y;

    toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
    if (c && active_constraint->current.cursor_hint.enabled) {
        wlr_cursor_warp(cursor, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
        wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
    }
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
    /* Focus enter isn't sent during drag, so refocus the focused node. */
    focusclient(focustop(selmon), 1);
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
        wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

    wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
    dx = lx - cursor->x;
    dy = ly - cursor->y;
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
                relative_pointer_mgr, seat, (uint64_t)time * 1000,
                dx, dy, dx_unaccel, dy_unaccel);

        wl_list_for_each(constraint, &pointer_constraints->constraints, link)
            cursorconstrain(constraint);

        if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
            toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
            if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
                sx = cursor->x - c->geom.x - c->bw;
                sy = cursor->y - c->geom.y - c->bw;
                if (wlr_region_confine(&active_constraint->region, sx, sy,
                        sx + dx, sy + dy, &sx_confined, &sy_confined)) {
                    dx = sx_confined - sx;
                    dy = sy_confined - sy;
                }

                if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
                    return;
            }
        }

        wlr_cursor_move(cursor, device, dx, dy);
        wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
        awl_notifications_activity();

        /* Update selmon (even while dragging a window) */
        if (cfg->sloppyfocus)
            setselmon(xytomon(cursor->x, cursor->y));
    }

    /* Find the client under the pointer and send the event along. */
    xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

    if (cursor_mode == CurPressed && !seat->drag
            && surface != seat->pointer_state.focused_surface
            && toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &l) >= 0) {
        c = w;
        surface = seat->pointer_state.focused_surface;
        sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
        sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
    }

    /* Update drag icon's position */
    wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

    /* If we are currently grabbing the mouse, handle and return */
    if (cursor_mode == CurMove) {
        /* Move the grabbed client to the new position. */
        resize(grabc, (struct wlr_box){.x = (int)round(cursor->x) - grabcx, .y = (int)round(cursor->y) - grabcy,
            .width = grabc->geom.width, .height = grabc->geom.height}, 1);
        return;
    } else if (cursor_mode == CurResize) {
        resize(grabc, (struct wlr_box){.x = grabc->geom.x, .y = grabc->geom.y,
            .width = (int)round(cursor->x) - grabc->geom.x, .height = (int)round(cursor->y) - grabc->geom.y}, 1);
        return;
    }

    /* awl's popups that take the pointer keep it from what is below */
    if (popupmotion())
        return;

    /* If there's no client surface under the cursor, set the cursor image to a
     * default. This is what makes the cursor image appear when you move it
     * off of a client or over its border. */
    if (!surface && !seat->drag)
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

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
    if (cursor_mode != CurNormal && cursor_mode != CurPressed)
        return;
    xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
    if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
        return;

    /* Float the window and tell motionnotify to grab it */
    setfloating(grabc, 1);
    switch (cursor_mode = arg->ui) {
    case CurMove:
        grabcx = (int)round(cursor->x) - grabc->geom.x;
        grabcy = (int)round(cursor->y) - grabc->geom.y;
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "all-scroll");
        break;
    case CurResize:
        /* Doesn't work for X11 output - the next absolute motion event
         * returns the cursor to where it started */
        wlr_cursor_warp_closest(cursor, NULL,
                grabc->geom.x + grabc->geom.width,
                grabc->geom.y + grabc->geom.height);
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
        break;
    }
}

void
pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
        uint32_t time)
{
    struct timespec now;

    if (surface != seat->pointer_state.focused_surface &&
            cfg->sloppyfocus && time && c && !client_is_unmanaged(c))
        focusclient(c, 0);

    /* If surface is NULL, clear pointer focus */
    if (!surface) {
        wlr_seat_pointer_notify_clear_focus(seat);
        return;
    }

    if (!time) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
    }

    /* Let the client know that the mouse cursor has entered one
     * of its surfaces, and make keyboard focus follow if desired.
     * wlroots makes this a no-op if surface is already focused */
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
    struct wlr_seat_request_start_drag_event *event = data;

    if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
            event->serial))
        wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
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
    if (cursor_mode != CurNormal && cursor_mode != CurPressed)
        return;
    /* This can be sent by any client, so we check to make sure this one
     * actually has pointer focus first. If so, we can tell the cursor to
     * use the provided surface as the cursor image. It will set the
     * hardware cursor on the output that it's currently on and continue to
     * do so as the cursor moves between outputs. */
    if (event->seat_client == seat->pointer_state.focused_client)
        wlr_cursor_set_surface(cursor, event->surface,
                event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    if (cursor_mode != CurNormal && cursor_mode != CurPressed)
        return;
    /* This can be sent by any client, so we check to make sure this one
     * actually has pointer focus first. If so, we can tell the cursor to
     * use the provided cursor shape. */
    if (event->seat_client == seat->pointer_state.focused_client)
        wlr_cursor_set_xcursor(cursor, cursor_mgr,
                wlr_cursor_shape_v1_name(event->shape));
}

void
startdrag(struct wl_listener *listener, void *data)
{
    struct wlr_drag *drag = data;
    if (!drag->icon)
        return;

    drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
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
        if (!(node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny)))
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
