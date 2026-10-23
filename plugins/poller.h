#pragma once

#include "stats.h"
#include "temp.h"
#include "date.h"
#include "bat.h"
#include "ipaddr.h"
#include <pulse/mainloop-api.h>

/**
 * One thread drives all of the plugins: a PulseAudio main loop, which also
 * runs pulsetest.c's and backlight.c's event sources. Stats, temperature and
 * clock tick once per wall-clock second, battery and IP address only wake
 * it when the kernel reports a change. Every wakeup ends in at most one
 * redraw request per plugin.
 */
typedef struct awl_poller_t awl_poller_t;

/** Sets up the loop, not running yet; ip may be NULL (no addresses).
 * NULL on failure. */
awl_poller_t* poller_new( awl_stats_t* stats, awl_temperature_t* temp, awl_date_t* date,
                          awl_battery_t* bat, awl_ipaddr_t* ip );
/** For the other plugins' event sources: from the calling thread until
 * :c:func:`poller_start`, then only from callbacks on the loop. */
pa_mainloop_api* poller_api( awl_poller_t* p );
/** Starts the thread. Returns 0, or -1 if it can't. */
int poller_start( awl_poller_t* p );
/** Paused, the 1 s tick stops (battery and IP events are still handled);
 * resuming re-reads everything and requests a redraw. Any thread; no-op for
 * NULL. */
void poller_set_paused( awl_poller_t* p, int paused );
/** Stops the thread, if it was started, waiting up to 2 s. Returns 0 once it
 * has exited (or for NULL): the other plugins free their event sources after
 * this, then :c:func:`poller_free`. Nonzero if it is stuck (in a sysfs read, say)
 * and left running: then nothing it polls or runs may be freed, the loop
 * included, and the library must stay mapped (:c:func:`awl_thread_stop`). */
int poller_stop( awl_poller_t* p );
/** Frees the loop; after :c:func:`poller_stop`. */
void poller_free( awl_poller_t* p );
