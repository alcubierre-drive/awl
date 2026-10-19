#pragma once

/**
 * The bar clock's calendar popup, an awl_window (awl_window.h) that draws
 * itself: the month with week numbers, the selected day and its events
 * from evolution-data-server (plugins/calendar.h), a Today button and one
 * to evolution. Without events (no libecal, no EDS) a plain calendar,
 * without the list and that button. Looks like awl's other popups
 * (`awl_config_t.popup`). One popup, shown on one
 * monitor at a time. Main thread.
 *
 * In it: the arrows step months, years and days, a day selects it, the
 * wheel pages months (over an event list longer than it shows, scrolls
 * that). No keyboard: a press outside closes it.
 */

#include "awl_draw.h"

/** starts fetching events, this month's first; returns 0 */
int awl_calpopup_start( void );
/** stops that and gives back the window (the plugin library's fini);
 * nonzero if the thread is stuck (see awl_calendar_stop()) */
int awl_calpopup_stop( void );
/** Shows the popup for w (the clock) on its monitor, centered on center_x
 * (logical pixels from the monitor's left edge), on today; nothing if it is
 * shown there already. Sets w->popup_window. */
void awl_calpopup_show( widget_t* w, int center_x );
void awl_calpopup_hide( void );
/** a month forward (amount > 0) or back while shown */
void awl_calpopup_scroll( int amount );
/** Collects fetched months and notices a new day, redrawing the popup if
 * that changes it; call on every bar redraw (the clock's measure()). */
void awl_calpopup_poll( void );
