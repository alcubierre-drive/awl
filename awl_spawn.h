#pragma once

/**
 * Child processes, part of awl: the startup command (-s), :c:member:`cfg->autostart <awl_config_t.autostart>`, what the spawn action and the screen lock start (awl_spawn.c).
 * The startup command and the autostarted programs are terminated when awl
 * exits; they are :c:member:`awl_process_t.spawn`. Main thread only.
 */

#include "awl_server.h"
#include <sys/types.h>

/** starts cmd with /bin/sh, if not NULL; it is proc's to stop */
void spawn_startup( awl_process_t *proc, const char *cmd );
/** starts :c:member:`cfg->autostart <awl_config_t.autostart>`, once at awl's start */
void autostart_all( awl_server_t *srv );
/** starts :c:member:`arg->v <Arg.v>`, an argv (:c:macro:`TRAY_ICONS` first: the rest in the tray's icon
 * theme); its pid, -1 if it couldn't */
pid_t spawn_pid( awl_process_t *proc, const Arg *arg );
/** reaps the children that exited (SIGCHLD) */
int handle_sigchld( int signo, void *data );
/** Sends SIGTERM to proc's startup command and every autostarted program (each
 * leads its own process group), then waits for them; whatever is still
 * running after CHILD_STOP_MS gets SIGKILL, so a child that ignores SIGTERM
 * can't keep awl from exiting. */
void stop_children( awl_process_t *proc );
