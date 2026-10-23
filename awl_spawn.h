#pragma once

/**
 * Child processes, part of awl: the startup command (-s), cfg->autostart, what the spawn action and the screen lock start (awl_spawn.c).
 * The startup command and the autostarted programs are terminated when awl
 * exits. Main thread only.
 */

#include "awl_server.h"
#include <sys/types.h>

/** starts cmd with /bin/sh, if not NULL */
void spawnstartup(const char *cmd);
/** starts cfg->autostart, once at awl's start */
void autostartall(void);
/** starts arg->v, an argv (:c:macro:`TRAY_ICONS` first: the rest in the tray's icon
 * theme); its pid, -1 if it couldn't */
pid_t spawn_pid(const Arg *arg);
/** reaps the children that exited (SIGCHLD) */
int handlesigchld(int signo, void *data);
/** Sends SIGTERM to the startup command and every autostarted program (each
 * leads its own process group), then waits for them; whatever is still
 * running after CHILD_STOP_MS gets SIGKILL, so a child that ignores SIGTERM
 * can't keep awl from exiting. */
void stopchildren(void);
