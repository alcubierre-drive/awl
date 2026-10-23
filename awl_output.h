#pragma once

/**
 * The outputs, part of awl: a :c:type:`Monitor` for each, from the backend's
 * new_output to the output's destruction, their layout, the output
 * management and power protocols, and which monitor is selected
 * (awl_output.c). Main thread only.
 */

#include "awl_server.h"

/** every Monitor, newest first */
extern struct wl_list mons;
/** the selected monitor: the focused client's, or the pointer's with
 * sloppyfocus; NULL while no output is enabled */
extern Monitor *selmon;
/** the output layout's box, around every output */
extern struct wlr_box sgeom;
extern struct wlr_output_layout *output_layout;
extern struct wlr_output_manager_v1 *output_mgr;
/** set while a monitor is destroyed (cleanupmon()): no bar is drawn then */
extern int in_cleanupmon;

/* the backend's new outputs, the output layout's changes, and requests of
 * the output management and output power management protocols */
void createmon(struct wl_listener *listener, void *data);
void updatemons(struct wl_listener *listener, void *data);
void outputmgrapply(struct wl_listener *listener, void *data);
void outputmgrtest(struct wl_listener *listener, void *data);
void powermgrsetmode(struct wl_listener *listener, void *data);

/** the monitor at x, y (layout coordinates), NULL if none */
Monitor *xytomon(double x, double y);
/** the enabled monitor add places after selmon (or before, add < 0), NULL
 * if none is enabled */
Monitor *nextmon(int add);
/** selects m; the bars mark which one is */
void setselmon(Monitor *m);
/** the monitor whose output is named name, NULL if none */
Monitor *monitornamed(const char *name);
/** selmon, for the host table */
Monitor *monitorselected(void);
float monitorscale(Monitor *m);
