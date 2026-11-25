#pragma once

/**
 * awl's own locker, awlock (lock/), as a fork of awl (awl_lockhelper.c).
 * ``main()`` forks the helper first, before awl has a thread, a GPU, a
 * Wayland socket or an open file; it stays that bare: it only waits on a
 * socket. For each locker awl sends it awlock's configuration
 * (:c:struct:`awlock_config_t`) and its end of a socketpair whose other
 * end awl made a Wayland client of. The helper forks again, and that child
 * is awlock (:c:func:`awlock_main`, lock/awlock_main.h): a fork of a
 * process with nothing to inherit, and its own process, so a locker that
 * crashes takes only itself, and the helper forks the next one.
 *
 * A fork of awl itself could not be: awl's threads may hold locks the child
 * would inherit locked, and awlock needs malloc, PAM and D-Bus.
 *
 * The helper exits when awl does (the socket's end of file). Its lockers are
 * its children, not awl's: awl sees one go by its client's destroy. Only
 * with ``AWL_AWLOCK`` (the meson option ``lock``); without, these do
 * nothing and :c:func:`lockhelper_run` fails. Main thread only.
 */

#include <stdbool.h>
#include <sys/types.h>

#include "lock/awlock_main.h" /* awlock_config_t */

/* awl_process.h's: the helper is awl_process_t.lockhelper */
typedef struct awl_process_t awl_process_t;

/** forks proc's helper; first thing in ``main()``, before any thread. A
 * helper that can't be forked is logged, and there is no awlock. */
void lockhelper_start( awl_process_t *proc );
/** Has the helper start awlock with cfg on fd, the locker's end of its
 * Wayland connection; fd is the caller's still. False (logged) if the
 * helper is gone or its socket full. */
bool lockhelper_run( awl_process_t *proc, const awlock_config_t *cfg, int fd );
/** pid, a child, exited: if it was the helper, there are no more lockers */
void lockhelper_exited( awl_process_t *proc, pid_t pid );
