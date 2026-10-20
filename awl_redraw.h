#pragma once

/**
 * Bar redraw requests, part of awl. awl's main loop polls `awl_redraw_fd()`
 * and redraws the bars once per wakeup; anything that changes what the bar
 * shows calls `awl_redraw_request()`, the plugin threads as
 * ``awl_host->redraw_request()``. A change on awl's main thread that only
 * the bars show takes `barsdirty()` (awl_bar.h) instead, which redraws them
 * before the loop waits again. Requests made before the
 * main loop gets around to it coalesce into a single redraw.
 */

/** Creates the eventfd. Returns it, or -1 on failure (requests are then
 * silently dropped). */
int awl_redraw_init( void );
/** Closes the eventfd. Only call once nothing can request a redraw anymore. */
void awl_redraw_fini( void );
/** Safe to call from any thread. */
void awl_redraw_request( void );
/** Resets the eventfd's counter; call from the fd's event handler. */
void awl_redraw_drain( void );
/** The eventfd to poll for readability, or -1. */
int awl_redraw_fd( void );
