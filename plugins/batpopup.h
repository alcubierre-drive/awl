#pragma once

/**
 * The battery's popup, shared by the dock and the battery widget (either's
 * hover opens it), an awl_window (awl_window.h) that draws itself: the
 * charge and the battery's status, what the machine runs and sits on, and
 * the charge thresholds (plugins/bat.h's :c:func:`bat_thresholds`), each with ‹ ›
 * buttons that step it by :c:member:`awl_plugin_config_t.battery`'s
 * threshold_step; the wheel over a threshold's row steps it too. The start
 * stays below the end. A threshold the battery doesn't have isn't shown.
 * Writing them takes write access to the sysfs files (root's by default,
 * README.md's Configuration has a udev rule for that): a failure shows in
 * the popup. One popup, on one monitor at a time. Main
 * thread.
 */

#include "awl_bar_type.h"
#include "handles.h"

/** the popup's state, its window made on first use; NULL if out of
 * memory. The functions below take NULL as a no-op. */
awl_batpopup_t* awl_batpopup_start( awl_plugin_data_t* p );
/** gives back the window and the state (the plugin library's fini) */
void awl_batpopup_stop( awl_batpopup_t* bp );
/** Shows the popup for w on its monitor, centered on center_x (logical
 * pixels from the monitor's left edge). Sets :c:member:`w->popup_window <widget_t.popup_window>`. */
void awl_batpopup_show( awl_batpopup_t* bp, widget_t* w, int center_x );
void awl_batpopup_hide( awl_batpopup_t* bp );
/** Re-reads what the popup shows while it is shown, redrawing it if that
 * changed; call on every bar redraw (the widgets' measure()). */
void awl_batpopup_poll( awl_batpopup_t* bp );
