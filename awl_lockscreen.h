#pragma once

/**
 * The screen lock, part of awl (awl_lockscreen.c). awl locks with an
 * ext-session-lock client, :c:member:`cfg->lock.locker <awl_config_t.lock.locker>`, started on a lock
 * request (``loginctl lock-session`` and before suspend, through the
 * plugins' logind thread, or the lock_screen action, e.g. on idle through
 * :c:member:`cfg->on_idle <awl_config_t.on_idle>`). While the
 * session is locked only the locker's surfaces are shown (over a blur,
 * :c:member:`cfg->lock.blur <awl_config_t.lock.blur>`) and get input. A lock covers the screen
 * once every output has shown it; before a suspend, the logind thread waits
 * for that. A locker that doesn't lock in time is stopped; one that goes
 * while locked leaves the session locked, and awl starts the next itself,
 * a few times until the next unlock (awl_lockscreen.c). While the logind thread doesn't listen for all it should, an
 * alert is up (awl_alert.h).
 *
 * Main thread only, except the logind thread's two below
 * (plugins/logind.h): it calls :c:func:`awl_lock_request` and
 * :c:func:`awl_lock_logind_state` from its own thread, as
 * :c:member:`awl_host->lock_request() <awl_host_t.lock_request>` and :c:member:`awl_host->logind_state() <awl_host_t.logind_state>`. Both wake
 * the main loop through an eventfd, :c:member:`awl_process_t.lock`'s;
 * requests made before it gets around to them coalesce into one.
 */

#include "awl_server.h"
#include <sys/types.h>

/** The locker's name that is awl's own, awlock (lock/), built into awl if
 * the meson option ``lock`` allows (``AWL_AWLOCK`` defined then). As
 * :c:member:`cfg->lock.locker <awl_config_t.lock.locker>`'s argv[0], awl
 * starts it through its lock helper (awl_lockhelper.h), configured by the
 * rest of :c:member:`awl_config_t.lock` and :c:type:`awl_colors_t`'s
 * lock_*; it takes no arguments. Without ``AWL_AWLOCK``, a program by that
 * name in ``PATH``. */
#define AWL_LOCKER "awlock"


/** :c:member:`awl_server_t.lock`, the eventfd for the logind thread and its
 * event handler, the timers, the blur over the screen while locked; before
 * the plugins start */
void lock_init( awl_server_t *srv );
/** removes the handler and the timers; :c:func:`awl_lock_fini` closes the eventfd */
void lock_fini( awl_server_t *srv );
/** ext-session-lock's new locks */
void lock_session( struct wl_listener *listener, void *data );
/** a monitor's lock surface is gone (:c:member:`Monitor.destroy_lock_surface`) */
void destroy_lock_surface( struct wl_listener *listener, void *data );
/** m rendered a frame: with the locker's surface on it, that counts for
 * covering the screen */
void lock_shown( awl_server_t *srv, Monitor *m );
/** the output layout changed: the blur covers it all */
void lock_resize( awl_server_t *srv );
/** a key was pressed while locked */
void lock_keystroke( awl_server_t *srv );
/** pid, a child, exited: if it was the locker, it may never have locked;
 * if it was the lock helper, there is no awlock anymore */
void locker_exited( awl_server_t *srv, pid_t pid );
/** after plugins_config.c changed: the blur */
void lock_configure( awl_server_t *srv );

/** Asks awl, proc's, to lock the screen. Any thread. */
void awl_lock_request( awl_process_t *proc );
/** What the logind thread listens for now, :c:enum:`awl_logind_flags` (awl_host.h);
 * 0 when it stops. Any thread. */
void awl_lock_logind_state( awl_process_t *proc, unsigned flags );
/** Closes the eventfd (requests and states are dropped from then on) and
 * frees :c:member:`awl_server_t.lock`. Only once nothing can call the two
 * above anymore: after the plugins are unloaded. */
void awl_lock_fini( awl_server_t *srv );
