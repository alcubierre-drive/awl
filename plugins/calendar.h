#pragma once

/**
 * The calendar's events, fetched on a plugin thread of their own: the
 * queries (cal_eds.h) block on D-Bus to evolution-data-server, which must not
 * stall awl. The main thread asks for months with `awl_calendar_request()`
 * and collects what came back with `awl_calendar_take()`; every result
 * requests a redraw, so the clock widget's measure() finds it soon.
 *
 * Months are keys year * 12 + month (month 0-based, as in struct tm).
 */

#include "cal_eds.h"

#include <time.h>

/** one occurrence, as cal_event_t, the strings owned (never NULL) */
typedef struct {
    char *summary, *location, *color;
    time_t start, end;
    int all_day;
} awl_cal_event_t;

/** a month's events, sorted: the all-day ones first, then by start */
typedef struct awl_cal_month {
    int key;
    /** CAL_OK, or why there are no events */
    cal_status_t status;
    awl_cal_event_t* events;
    int n;
    struct awl_cal_month* next;
} awl_cal_month_t;

/** Starts the thread; it loads libecal on the first request. Returns 0, or
 * -1 (logged) without a thread: requests are then dropped. Main thread. */
int awl_calendar_start( void );
/** Cancels the running query and stops the thread. Returns nonzero if it is
 * stuck and had to be left running (its memory stays). Main thread. */
int awl_calendar_stop( void );
/** Queues month key, unless it is queued already. Main thread. */
void awl_calendar_request( int key );
/** The results since the last call, oldest first, NULL if none; free each
 * with `awl_calendar_month_free()`. Main thread. */
awl_cal_month_t* awl_calendar_take( void );
/** frees m (not the ones after it); NULL is fine */
void awl_calendar_month_free( awl_cal_month_t* m );

/** the first second of month key, local time */
time_t awl_calendar_month_start( int key );
/** the first second of that day, local time; mktime() carries day over
 * into the months around */
time_t awl_calendar_day_start( int year, int month, int day );
