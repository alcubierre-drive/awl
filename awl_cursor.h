#pragma once

/**
 * The pointer, part of awl: the cursor and its image, motion, presses and
 * the wheel and where they go (awl's popups, the bar, the desktop, the
 * bindings, the clients), pointer focus, moving and resizing windows,
 * pointer constraints, drag and drop (awl_cursor.c). Main thread only.
 */

#include "awl_server.h"


/* the cursor's events, the seat's requests, new pointer constraints */
void axis_notify( struct wl_listener *listener, void *data );
void button_press( struct wl_listener *listener, void *data );
void cursor_frame( struct wl_listener *listener, void *data );
void motion_absolute( struct wl_listener *listener, void *data );
void motion_relative( struct wl_listener *listener, void *data );
void set_cursor( struct wl_listener *listener, void *data );
void set_cursor_shape( struct wl_listener *listener, void *data );
void request_start_drag( struct wl_listener *listener, void *data );
void start_drag( struct wl_listener *listener, void *data );
void create_pointer_constraint( struct wl_listener *listener, void *data );

/** The pointer moved by dx, dy (unaccelerated: dx_unaccel, dy_unaccel) on
 * device; time 0 only looks again what is under it, after a change of the
 * scene (pointer focus, the cursor image, the hover). */
void motion_notify( awl_server_t *srv, uint32_t time, struct wlr_input_device *device, double dx, double dy,
                   double dx_unaccel, double dy_unaccel );
/** what is at x, y: the surface, its client or layer surface (each may be
 * NULL), and x, y in the node's coordinates */
void xy_to_node( awl_server_t *srv, double x, double y, struct wlr_surface **psurface, Client **pc, LayerSurface **pl,
               double *nx, double *ny );
