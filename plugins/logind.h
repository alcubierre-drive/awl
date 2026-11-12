#pragma once

/**
 * The logind thread: listens to systemd-logind on the system bus and locks
 * the screen through awl (:c:member:`awl_host->lock_request() <awl_host_t.lock_request>`)
 *
 * - on our session's ``Lock`` signal (``loginctl lock-session``, or
 *   ``lock-sessions`` from a sleep hook), and
 * - before suspend, on ``PrepareForSleep(true)``, if the config dictionary's
 *   ``lock_on_suspend`` (the config's :c:member:`lock.on_suspend <awl_config_t.lock.on_suspend>`) is nonzero. A delay inhibitor makes
 *   logind wait for that lock, up to its ``InhibitDelayMaxSec``: the thread
 *   lets go of it once awl says the lock covers every output
 *   (:c:func:`awl_logind_covered`), and takes a new one on resume.
 *
 * It also suspends the machine on request (:c:func:`awl_logind_suspend`, logind's
 * ``Suspend``: what ``systemctl suspend`` calls), with whatever lock that
 * brings on.
 *
 * It also keeps the session's ``LockedHint`` (``loginctl show-session``)
 * up to date, and tells awl what it listens for (:c:member:`awl_host->logind_state() <awl_host_t.logind_state>`),
 * so awl can raise an alert while it doesn't. A lost bus or a restarted
 * logind is reconnected to after a few seconds.
 */

#include "handles.h"

/** Starts the thread. Returns its handle, or NULL (logged): awl's alert
 * then goes up; the functions below take NULL. */
awl_logind_t* awl_logind_start( void );
/** Stops it and frees lg; returns nonzero if it had to be left running (lg
 * then stays). */
int awl_logind_stop( awl_logind_t* lg );
/** :c:member:`awl_plugin_api_t.session_covered`; any thread */
void awl_logind_covered( awl_logind_t* lg, int covered );
/** Asks logind to suspend (the suspend action); returns at once, a refusal
 * is logged. Any thread. */
void awl_logind_suspend( awl_logind_t* lg );
