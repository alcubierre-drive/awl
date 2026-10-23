#pragma once

/**
 * The plugin threads (poller.c's event loop, loop.c's (the tray's), plugins_desktop_panel.c's
 * scanner, wallpaper.c's decoder) all start and stop the same way: the thread sleeps
 * in ``poll()`` on, among others, ``wake_fd``; :c:func:`awl_thread_stop` sets
 * ``quit`` and makes ``wake_fd`` readable, the thread notices in
 * :c:func:`awl_thread_woken` and returns. No thread is ever cancelled.
 */

#include <pthread.h>
#include <stdatomic.h>

/** A plugin thread, see :c:func:`awl_thread_start`. */
typedef struct {
    pthread_t thread;
    /** eventfd */
    int wake_fd;
    atomic_int quit;
    const char* name;
    void* (*fn)( void* );
    void* arg;
} awl_thread_t;

/** Runs fn(arg) on a new thread named name (at most 11 characters). All
 * signals are blocked in it, they are awl's main thread's. Logs the thread
 * id to stderr. Returns 0, or -1 (logged) if it can't. t must stay where it
 * is until the thread is stopped. */
int awl_thread_start( awl_thread_t* t, const char* name, void* (*fn)( void* ), void* arg );
/** Makes ``wake_fd`` readable. Any thread. */
void awl_thread_wake( awl_thread_t* t );
/** On the thread, once ``wake_fd`` is readable: resets it and returns whether
 * the thread is to quit. */
int awl_thread_woken( awl_thread_t* t );
/** Asks the thread to quit and waits for it up to timeout_ms, or forever if
 * negative. Returns 0 once it has exited, or -1 if it is still running (stuck
 * in the kernel, e.g. on a dead mount): it is then left behind, and t and
 * everything the thread uses must stay allocated and its code mapped. */
int awl_thread_stop( awl_thread_t* t, int timeout_ms );
