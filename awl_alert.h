#pragma once

/**
 * Alerts, part of awl: conditions the user must not miss, such as the screen
 * no longer being locked before suspend. An alert is raised by awl itself,
 * never by the plugin library (which may be the very thing that is missing).
 * While one is up, every output shows it in its bottom left corner,
 * above everything (awl.c's ``drawalert()``, with `awl_alert_text()`), and
 * raising it logs an error and sends a critical desktop notification
 * (``notify-send``). Main thread only.
 */

/** the alerts there are; each is up or not */
typedef enum awl_alert_id {
    /** the plugins' logind thread doesn't listen to logind (awl.c's
     * ``logindcheck()``): no lock on ``loginctl lock-session``, or none
     * before suspend */
    AwlAlertLogind,
    /** the locker lockscreen() started exited before it ever locked: a
     * wrong config_awl.h locker, or it crashed */
    AwlAlertLocker,
    AwlAlertLast,
} awl_alert_id_t;

/** Raises the alert id with the text fmt (short: the bar shows it), or
 * changes its text. If either is new, logs it and requests a redraw; only
 * raising it notifies. */
void awl_alert_set( awl_alert_id_t id, const char* fmt, ... )
    __attribute__(( format( printf, 2, 3 ) ));
/** Takes the alert id down, if up; logs that. */
void awl_alert_clear( awl_alert_id_t id );
/** Whether the alert id is up. */
int awl_alert_up( awl_alert_id_t id );
/** Changes whenever `awl_alert_text()` may have. */
unsigned int awl_alert_version( void );
/** The text of the first alert that is up, or NULL if none is. */
const char* awl_alert_text( void );
