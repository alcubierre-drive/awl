// awlock's entry point, for awl_lockhelper.c: awlock is part of awl, a
// fork of awl's lock helper, not a program of its own.
#ifndef AWLOCK_MAIN_H
#define AWLOCK_MAIN_H

/* awl_process.h's */
typedef struct awl_process_t awl_process_t;

/** awlock's main(), in a fork of awl's lock helper (awl_lockhelper.h).
 * proc is the fork's copy of awl's, whose log (awl_log.h) awlock's goes
 * to; argv as a main()'s, argv[0] the locker's name; wayland_fd its
 * connection to awl (a socket :c:func:`wl_client_create` took the other
 * end of); its exit status */
int awlock_main(awl_process_t *proc, int argc, char **argv, int wayland_fd);

#endif
