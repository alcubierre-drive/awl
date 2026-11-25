#pragma once

/**
 * The outputs, part of awl: a :c:type:`Monitor` for each, from the backend's
 * new_output to the output's destruction, their layout, the output
 * management and power protocols, and which monitor is selected
 * (awl_output.c). Main thread only.
 */

#include "awl_server.h"


/* the backend's new outputs, the output layout's changes, and requests of
 * the output management and output power management protocols */
void create_mon( struct wl_listener *listener, void *data );
void update_mons( struct wl_listener *listener, void *data );
void output_mgr_apply( struct wl_listener *listener, void *data );
void output_mgr_test( struct wl_listener *listener, void *data );
void power_mgr_set_mode( struct wl_listener *listener, void *data );

/** the monitor at x, y (layout coordinates), NULL if none */
Monitor *xy_to_mon( awl_server_t *srv, double x, double y );
/** the enabled monitor add places after sel_mon (or before, add < 0), NULL
 * if none is enabled */
Monitor *next_mon( awl_server_t *srv, int add );
/** selects m; the bars mark which one is */
void set_sel_mon( awl_server_t *srv, Monitor *m );
/** the monitor whose output is named name, NULL if none */
Monitor *monitor_named( awl_server_t *srv, const char *name );
/** sel_mon, for the host table */
Monitor *monitor_selected( awl_server_t *srv );
float monitor_scale( Monitor *m );
