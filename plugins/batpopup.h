#pragma once

/**
 * The battery's popup, shared by the dock and the battery widget (either's
 * hover opens it), an awl_window (awl_window.h) that draws itself: the
 * charge and the battery's status, what the machine runs and sits on, and
 * the charge thresholds (plugins/bat.h's bat_thresholds()), each with ‹ ›
 * buttons that step it by :c:member:`awl_plugin_config_t.battery`'s
 * threshold_step; the wheel over a threshold's row steps it too. The start
 * stays below the end. A threshold the battery doesn't have isn't shown.
 * Writing them takes write access to the sysfs files (root's by default,
 * README.md's Configuration has a udev rule for that): a failure shows in
 * the popup. One popup, on one monitor at a time. Main
 * thread.
 */

#include "awl_bar_type.h"

/** Shows the popup for w on its monitor, centered on center_x (logical
 * pixels from the monitor's left edge). Sets w->popup_window. */
void awl_batpopup_show( widget_t* w, int center_x );
void awl_batpopup_hide( void );
/** Re-reads what the popup shows while it is shown, redrawing it if that
 * changed; call on every bar redraw (the widgets' measure()). */
void awl_batpopup_poll( void );
/** gives back the window (the plugin library's fini) */
void awl_batpopup_stop( void );
