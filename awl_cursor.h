#pragma once

/**
 * The pointer, part of awl: the cursor and its image, motion, presses and
 * the wheel and where they go (awl's popups, the bar, the desktop, the
 * bindings, the clients), pointer focus, moving and resizing windows,
 * pointer constraints, drag and drop (awl_cursor.c). Main thread only.
 */

#include "awl_server.h"


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
void motionnotify(awl_server_t *srv, uint32_t time, struct wlr_input_device *device, double dx, double dy,
        double dx_unaccel, double dy_unaccel);
/** what is at x, y: the surface, its client or layer surface (each may be
 * NULL), and x, y in the node's coordinates */
void xytonode(awl_server_t *srv, double x, double y, struct wlr_surface **psurface,
        Client **pc, LayerSurface **pl, double *nx, double *ny);
