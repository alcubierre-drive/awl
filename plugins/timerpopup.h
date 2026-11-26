#pragma once

/**
 * The clock's timer, a popup that the clock's right click cycles to
 * (plugins_widgets.c), an awl_window (awl_window.h) that
 * draws itself in two rows: the timer running, with the time left and when
 * it ends, or "stopped"; then Start, Stop and the duration between ‹ ›
 * buttons. The wheel anywhere on the popup steps the duration as the
 * buttons do: by a minute up to 10 minutes, 5 up to an hour, 15 beyond.
 * Start (re)starts the timer with the duration, Stop stops it. When it
 * runs out, it sends a critical notification with Dismiss and snooze
 * buttons (5 and 10 minutes, and the duration it ran if neither); a
 * click elsewhere on it dismisses it too. One timer, in memory only;
 * one popup, on one monitor at a time. Main thread.
 */

#include "awl_bar_type.h"
#include "handles.h"

/** the timer's state, its window and timer made on first use; NULL if out
 * of memory. The functions below take NULL as a no-op. */
awl_timerpopup_t* awl_timerpopup_start( awl_plugin_data_t* p );
/** gives back the window, the timer and the state (the plugin library's
 * fini): a running timer is dropped */
void awl_timerpopup_stop( awl_timerpopup_t* tp );
/** Shows the popup for w on its monitor, centered on center_x (logical
 * pixels from the monitor's left edge). Sets :c:member:`w->popup_window <widget_t.popup_window>`. */
void awl_timerpopup_show( awl_timerpopup_t* tp, widget_t* w, int center_x );
void awl_timerpopup_hide( awl_timerpopup_t* tp );
/** whether the popup is shown */
int awl_timerpopup_shown( awl_timerpopup_t* tp );
/** Notices the timer running out (late after a suspend: awl's timers stand
 * still meanwhile) and counts the popup down while it is shown; call on
 * every bar redraw (the clock's measure()). */
void awl_timerpopup_poll( awl_timerpopup_t* tp );
/** :c:member:`awl_plugin_api_t.notification_action`: a snooze of the run-out
 * notification runs the timer again for the key's seconds */
void awl_timerpopup_action( awl_timerpopup_t* tp, uint32_t id, const char* key );
