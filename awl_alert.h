#pragma once

/**
 * Alerts, part of awl: conditions the user must not miss, such as the screen
 * no longer being locked before suspend. An alert is raised by awl itself,
 * never by the plugin library (which may be the very thing that is missing).
 * While one is up, every output shows it in the bottom left corner of its
 * window area (above a bottom bar),
 * above everything (`awl_alert_draw()`, with `awl_alert_text()`), and
 * raising it logs an error and sends a critical notification, awl's own
 * (`awl_notifications_send()`). Main thread only.
 */

#include "awl_server.h"

/** the alerts there are; each is up or not */
typedef enum awl_alert_id {
    /** the plugins' logind thread doesn't listen to logind (awl_lockscreen.c's
     * ``logindupdate()``): no lock on ``loginctl lock-session``, or none
     * before suspend */
    AwlAlertLogind,
    /** the locker lockscreen() started exited before it ever locked: a
     * wrong cfg->lock.locker, or it crashed */
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
/** The text of the first alert that is up, or NULL if none is. */
const char* awl_alert_text( void );

/** the alerts' tree, over everything, the lock screen included */
void awl_alert_init( void );
/** m's alert, hidden until `awl_alert_draw()` */
void awl_alert_addmon( Monitor* m );
/** The alert on m, as cfg->alert says, in the bar's font: in the bottom
 * left corner of m's window area (m->w: clear of the bar and of exclusive
 * layer surfaces), cfg->alert.margin logical pixels from its edges, above
 * everything, the lock screen included. Redrawn only when the alert, m's
 * window area or its scale changed;
 * called on every redraw request and whenever m is arranged. */
void awl_alert_draw( Monitor* m );
/** after the config changed: the alerts' look */
void awl_alert_configure( void );
/** `awl_alert_draw()` on every monitor */
void awl_alert_draw_all( void );
