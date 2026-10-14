#pragma once

/**
 * Lock requests and the logind thread's state, part of awl. The plugins'
 * logind thread (plugins/logind.h) calls `awl_lock_request()` and
 * `awl_lock_logind_state()` from its own thread, as
 * ``awl_host->lock_request()`` and ``awl_host->logind_state()``; awl's main
 * loop polls `awl_lock_fd()` and picks both up with `awl_lock_take()`.
 * Requests made before it gets around to it coalesce into one.
 */

/** Creates the eventfd. Returns it, or -1 on failure (requests and states
 * are then dropped, so the logind alert stays up). */
int awl_lock_init( void );
/** Closes the eventfd. Only call once nothing can call the two below anymore. */
void awl_lock_fini( void );
/** Asks awl to lock the screen. Safe to call from any thread. */
void awl_lock_request( void );
/** What the logind thread listens for now, `awl_logind_flags` (awl_host.h);
 * 0 when it stops. Safe to call from any thread. */
void awl_lock_logind_state( unsigned flags );
/** The eventfd to poll for readability, or -1. */
int awl_lock_fd( void );
/** Main thread, from the fd's event handler: resets it, returns whether a
 * lock was requested since the last call, and puts the logind thread's
 * latest state into ``*flags``. */
int awl_lock_take( unsigned* flags );
