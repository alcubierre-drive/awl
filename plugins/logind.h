#pragma once

/**
 * The logind thread: listens to systemd-logind on the system bus and locks
 * the screen through awl (``awl_host->lock_request()``)
 *
 * - on our session's ``Lock`` signal (``loginctl lock-session``, or
 *   ``lock-sessions`` from a sleep hook), and
 * - before suspend, on ``PrepareForSleep(true)``, if the config dictionary's
 *   ``lock_on_suspend`` (the config's lock.on_suspend) is nonzero. A delay inhibitor makes
 *   logind wait for that lock, up to its ``InhibitDelayMaxSec``: the thread
 *   lets go of it once awl says the lock covers every output
 *   (`awl_logind_covered()`), and takes a new one on resume.
 *
 * It also keeps the session's ``LockedHint`` (``loginctl show-session``)
 * up to date, and tells awl what it listens for (``awl_host->logind_state()``),
 * so awl can raise an alert while it doesn't. A lost bus or a restarted
 * logind is reconnected to after a few seconds.
 */

/** Starts the thread. Returns 0, or -1 (logged): awl's alert then goes up. */
int awl_logind_start( void );
/** Stops it; returns nonzero if it had to be left running. */
int awl_logind_stop( void );
/** `awl_plugin_api_t.session_covered`; any thread */
void awl_logind_covered( int covered );
