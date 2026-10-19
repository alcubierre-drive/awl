#pragma once

/**
 * The screen lock, part of awl (awl_lockscreen.c). awl locks with an
 * ext-session-lock client, config_awl.h's ``locker``, started on a lock
 * request (``loginctl lock-session`` and before suspend, through the
 * plugins' logind thread, or the lockscreen action). While the
 * session is locked only the locker's surfaces are shown (over a blur,
 * config_awl.h's ``locked_blur``) and get input. A lock covers the screen
 * once every output has shown it; before a suspend, the logind thread waits
 * for that. While the logind thread doesn't listen for all it should, an
 * alert is up (awl_alert.h).
 *
 * Main thread only, except the logind thread's two below
 * (plugins/logind.h): it calls `awl_lock_request()` and
 * `awl_lock_logind_state()` from its own thread, as
 * ``awl_host->lock_request()`` and ``awl_host->logind_state()``. Both wake
 * the main loop through an eventfd; requests made before it gets around to
 * them coalesce into one.
 */

#include "awl_server.h"
#include <sys/types.h>

/** the session is locked: by the locker, or still, after it crashed */
extern int locked;

/** the eventfd for the logind thread and its event handler, the timers,
 * the blur over the screen while locked; before the plugins start */
void lockinit(void);
/** removes the handler and the timers; awl_lock_fini() closes the eventfd */
void lockfini(void);
/** ext-session-lock's new locks */
void locksession(struct wl_listener *listener, void *data);
/** a monitor's lock surface is gone (``Monitor.destroy_lock_surface``) */
void destroylocksurface(struct wl_listener *listener, void *data);
/** m rendered a frame: with the locker's surface on it, that counts for
 * covering the screen */
void lockshown(Monitor *m);
/** the output layout changed: the blur covers it all */
void lockresize(void);
/** a key was pressed while locked */
void lockkeystroke(void);
/** pid, a child, exited: if it was the locker, it may never have locked */
void lockerexited(pid_t pid);
/** after config_plugins.h changed: the blur */
void lockconfigure(void);

/** Asks awl to lock the screen. Any thread. */
void awl_lock_request(void);
/** What the logind thread listens for now, `awl_logind_flags` (awl_host.h);
 * 0 when it stops. Any thread. */
void awl_lock_logind_state(unsigned flags);
/** Closes the eventfd (requests and states are dropped from then on). Only
 * once nothing can call the two above anymore: after the plugins are
 * unloaded. */
void awl_lock_fini(void);
