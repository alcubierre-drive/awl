#pragma once

#include <systemd/sd-event.h>

/**
 * An sd-event loop on a thread of its own (thread.h), for plugins that only
 * wait on their event sources, e.g. the tray's host (sni.h), so that their
 * work never delays the poller's 1 s tick (poller.h). sd-bus connections
 * run on it with ``sd_bus_attach_event()``.
 */
typedef struct awl_loop_t awl_loop_t;

/** Sets up the loop, not running yet. NULL on failure. */
awl_loop_t* awl_loop_new( void );
/** For event sources: from the calling thread until `awl_loop_start()`, then
 * only from callbacks on the loop. */
sd_event* awl_loop_event( awl_loop_t* l );
/** Starts the thread, named name (at most 11 characters). Returns 0, or -1
 * if it can't. */
int awl_loop_start( awl_loop_t* l, const char* name );
/** Stops the thread, if it was started, waiting up to timeout_ms. Returns 0
 * once it has exited (or for NULL): free the event sources, then
 * `awl_loop_free()`. Nonzero if it is stuck and left running: then nothing it
 * runs may be freed, the loop included, and the library must stay mapped
 * (`awl_thread_stop()`). */
int awl_loop_stop( awl_loop_t* l, int timeout_ms );
/** Frees the loop; after `awl_loop_stop()`. NULL is fine. */
void awl_loop_free( awl_loop_t* l );
