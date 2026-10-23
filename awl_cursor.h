#pragma once

/**
 * The pointer, part of awl: the cursor and its image, motion, presses and
 * the wheel and where they go (awl's popups, the bar, the desktop, the
 * bindings, the clients), pointer focus, moving and resizing windows,
 * pointer constraints, drag and drop (awl_cursor.c). Main thread only.
 */

#include "awl_server.h"

extern struct wlr_cursor *cursor;
extern struct wlr_xcursor_manager *cursor_mgr;
extern struct wlr_pointer_constraints_v1 *pointer_constraints;
extern struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
/** the drag icons, under LyrBlock */
extern struct wlr_scene_tree *drag_icon;
/** what the pointer does, :c:enum:`CursorMode` */
extern unsigned int cursor_mode;
/** the client being moved or resized */
extern Client *grabc;

/* the cursor's events, the seat's requests, new pointer constraints */
void axisnotify(struct wl_listener *listener, void *data);
void buttonpress(struct wl_listener *listener, void *data);
void cursorframe(struct wl_listener *listener, void *data);
void motionabsolute(struct wl_listener *listener, void *data);
void motionrelative(struct wl_listener *listener, void *data);
void setcursor(struct wl_listener *listener, void *data);
void setcursorshape(struct wl_listener *listener, void *data);
void requeststartdrag(struct wl_listener *listener, void *data);
void startdrag(struct wl_listener *listener, void *data);
void createpointerconstraint(struct wl_listener *listener, void *data);

/** The pointer moved by dx, dy (unaccelerated: dx_unaccel, dy_unaccel) on
 * device; time 0 only looks again what is under it, after a change of the
 * scene (pointer focus, the cursor image, the hover). */
void motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
        double dx_unaccel, double dy_unaccel);
/** what is at x, y: the surface, its client or layer surface (each may be
 * NULL), and x, y in the node's coordinates */
void xytonode(double x, double y, struct wlr_surface **psurface,
        Client **pc, LayerSurface **pl, double *nx, double *ny);
