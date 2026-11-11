#pragma once

/**
 * The system monitor, the popup of the bar's graphs and temperature widgets
 * (either's hover opens it), an awl_window (awl_window.h) that draws
 * itself: the whole history plugins/stats.h and plugins/temp.h keep
 * (a logical pixel a second: at least the last five minutes, more as
 * its text makes it wider, up to the :c:macro:`AWL_STATS_HIST` samples
 * kept), a graph each for the
 * CPU (the kernel's part darker, waiting for I/O on top), memory, swap
 * (if the system has any) and every temperature sensor, each with its
 * numbers: the CPU's user, system and I/O shares and the load, memory and
 * swap in GiB, a sensor's minimum, average and maximum over the history.
 * A graph is the curve through every sample (a monotone cubic,
 * plugins/spline.h: spikes keep their height, nothing overshoots), each
 * pixel covered by exactly the area under it, so it is smooth at any
 * scale.
 * The pointer over the graphs picks a moment: a line marks it in all of
 * them and their numbers are that moment's. It follows the samples while
 * shown. One popup, on one monitor at a time. Main thread.
 */

#include "awl_bar_type.h"

/** Shows the popup for w on its monitor, centered on center_x (logical
 * pixels from the monitor's left edge). Sets w->popup_window. */
void awl_sysmon_show( widget_t* w, int center_x );
void awl_sysmon_hide( void );
/** Redraws the popup if it is shown and there are new samples; call on
 * every bar redraw (the widgets' measure()). */
void awl_sysmon_poll( void );
/** gives back the window (the plugin library's fini) */
void awl_sysmon_stop( void );
