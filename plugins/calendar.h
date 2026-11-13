#pragma once

/**
 * The calendar's events, fetched on a plugin thread of their own: the
 * queries (cal_eds.h) block on D-Bus to evolution-data-server, which must not
 * stall awl. The main thread asks for months with :c:func:`awl_calendar_request`
 * and collects what came back with :c:func:`awl_calendar_take`; every result
 * requests a redraw, so the clock widget's measure() finds it soon.
 *
 * Months are keys year * 12 + month (month 0-based, as in struct tm).
 */

#include "cal_eds.h"
#include "handles.h"

#include <stdbool.h>
#include <time.h>

/** one occurrence, as :c:type:`cal_event_t`, the strings owned (never NULL) */
typedef struct {
    char *summary, *location, *color;
    time_t start, end;
    bool all_day;
} awl_cal_event_t;

/** a month's events, sorted: the all-day ones first, then by start */
typedef struct awl_cal_month_t {
    int key;
    /** :c:enumerator:`CAL_OK`, or why there are no events */
    cal_status_t status;
    awl_cal_event_t* events;
    int n;
    struct awl_cal_month_t* next;
} awl_cal_month_t;

/** Starts the thread; it loads libecal on the first request. Returns its
 * handle, or NULL (logged) without a thread: the functions below take NULL,
 * requests are then dropped. Main thread. */
awl_calendar_t* awl_calendar_start( void );
/** Cancels the running query, stops the thread and frees w. Returns nonzero
 * if it is stuck and had to be left running (w then stays). Main thread. */
int awl_calendar_stop( awl_calendar_t* w );
/** Queues month key, unless it is queued already. Main thread. */
void awl_calendar_request( awl_calendar_t* w, int key );
/** The results since the last call, oldest first, NULL if none; free each
 * with :c:func:`awl_calendar_month_free`. Main thread. */
awl_cal_month_t* awl_calendar_take( awl_calendar_t* w );
/** frees m (not the ones after it); NULL is fine */
void awl_calendar_month_free( awl_cal_month_t* m );

/** the first second of month key, local time */
time_t awl_calendar_month_start( int key );
/** the first second of that day, local time; mktime() carries day over
 * into the months around */
time_t awl_calendar_day_start( int year, int month, int day );
