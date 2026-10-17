#pragma once

/**
 * Work for awl's main thread, part of awl: `awl_post()` queues a function
 * from any thread, awl's main loop polls `awl_post_fd()` and runs what is
 * queued (`awl_post_run()`), in order. The tray hands its menus over this
 * way, as ``awl_host->post()``. Like awl_redraw.h, but each call is run, not
 * coalesced.
 */

/** Creates the eventfd. Returns it, or -1 on failure (posts are then
 * dropped). */
int awl_post_init( void );
/** Closes the eventfd and drops what is still queued without running it.
 * Only call once nothing can post anymore. */
void awl_post_fini( void );
/** Queues fn( data ) for the main thread. Safe to call from any thread. If
 * awl quits first, fn never runs. */
void awl_post( void (*fn)( void* data ), void* data );
/** Runs what is queued; call from the fd's event handler. What the
 * functions post meanwhile runs on the next wakeup. */
void awl_post_run( void );
/** The eventfd to poll for readability, or -1. */
int awl_post_fd( void );
