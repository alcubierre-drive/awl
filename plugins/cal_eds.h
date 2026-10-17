#pragma once

/**
 * The calendar's events from evolution-data-server (the calendars GNOME
 * Online Accounts, gnome-calendar and evolution use), without linking it:
 * libecal-2.0 is loaded on the first query and never unloaded (its GTypes
 * can't be registered twice). Without the library, or without EDS running,
 * queries fail and say why, and the calendar is a plain one. Plain C and no
 * GLib in the interface, so the plugins can use it too.
 *
 * ``AWL_ECAL_LIBRARY`` in the environment names the library to load instead
 * of the usual sonames; ``none`` loads nothing.
 */

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAL_OK = 0,
    /** the library can't be loaded, or AWL_ECAL_LIBRARY is none; for good */
    CAL_ERR_NOLIB,
    /** it lacks one of the functions used here; for good */
    CAL_ERR_NOSYM,
    /** EDS's source registry is unreachable; asked again after
     * CAL_RETRY_SECONDS, until then queries fail right away */
    CAL_ERR_NOREGISTRY,
    /** cal_backend_cancel() was called */
    CAL_ERR_CANCELLED,
} cal_status_t;

#define CAL_RETRY_SECONDS 60

/** One occurrence of an event, in local time; all-day ones from midnight to
 * midnight. The strings are valid during the callback only; color is
 * "#rrggbb" or "". */
typedef struct {
    const char *summary, *location, *color;
    time_t start, end;
    int all_day;
} cal_event_t;

typedef void (*cal_event_cb)(const cal_event_t *e, void *data);
/** warn: a warning, else for information; msg without a newline */
typedef void (*cal_log_cb)(int warn, const char *msg);

typedef struct cal_backend cal_backend_t;

/** A backend that has loaded nothing yet; NULL only without memory. log
 * (may be NULL) gets why queries fail, once per cause, and the calendars
 * that can't be opened. */
cal_backend_t *cal_backend_open(cal_log_cb log);
/** Calls cb for every occurrence between start and end in the enabled
 * calendars selected in gnome-calendar/evolution. Blocks for D-Bus; from one
 * thread at a time. Connections stay open for the next query. */
cal_status_t cal_backend_query(cal_backend_t *b, time_t start, time_t end, cal_event_cb cb, void *data);
/** Cancels the running query and every later one; any thread. */
void cal_backend_cancel(cal_backend_t *b);
/** Drops the connections and frees b, NULL is fine; not while a query runs.
 * The library stays loaded. */
void cal_backend_close(cal_backend_t *b);
const char *cal_status_str(cal_status_t s);

#ifdef __cplusplus
}
#endif
