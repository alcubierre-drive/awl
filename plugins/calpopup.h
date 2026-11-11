#pragma once

/**
 * The bar clock's calendar popup, an awl_window (awl_window.h) that draws
 * itself: the month with week numbers, the selected day and its events
 * from evolution-data-server (plugins/calendar.h), a Today button and one
 * to evolution. Without events (no libecal, no EDS) a plain calendar,
 * without the list and that button. Looks like awl's other popups
 * (:c:member:`awl_config_t.popup`). One popup, shown on one
 * monitor at a time. Main thread.
 *
 * In it: the arrows step months, years and days, a day selects it, the
 * wheel pages months (over an event list longer than it shows, scrolls
 * that). No keyboard: a press outside closes it.
 */

#include "awl_bar_type.h"
#include "handles.h"

/** the popup's state; starts fetching events, this month's first. NULL if
 * out of memory; the functions below take NULL as a no-op. */
awl_calpopup_t* awl_calpopup_start( awl_plugin_data_t* p );
/** stops that, gives back the window and frees cp (the plugin library's
 * fini); nonzero if the thread is stuck (see awl_calendar_stop()) */
int awl_calpopup_stop( awl_calpopup_t* cp );
/** Shows the popup for w (the clock) on its monitor, centered on center_x
 * (logical pixels from the monitor's left edge), on today; nothing if it is
 * shown there already. Sets w->popup_window. */
void awl_calpopup_show( awl_calpopup_t* cp, widget_t* w, int center_x );
void awl_calpopup_hide( awl_calpopup_t* cp );
/** a month forward (amount > 0) or back while shown */
void awl_calpopup_scroll( awl_calpopup_t* cp, int amount );
/** Collects fetched months and notices a new day, redrawing the popup if
 * that changes it; call on every bar redraw (the clock's measure()). */
void awl_calpopup_poll( awl_calpopup_t* cp );
