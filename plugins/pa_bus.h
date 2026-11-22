#pragma once

/**
 * A D-Bus connection on a pa_mainloop_api (the plugins' shared loop,
 * poller.h) that watches one service: it connects from the loop,
 * reconnects after a failure, and tells when the service comes and goes
 * (its ``NameOwnerChanged``). Like awl_bus.h on awl's own loop. Loop thread
 * only, but for :c:func:`pa_bus_start` and :c:func:`pa_bus_stop`.
 */

#include <stdbool.h>
#include <stdint.h>
#include <pulse/mainloop-api.h>

typedef struct sd_bus sd_bus;
/** opaque */
typedef struct pa_bus pa_bus_t;

/** the service watched and what its user does */
typedef struct pa_bus_ops_t {
    /** the service's name on the bus */
    const char* name;
    /** on the system bus, else the user bus */
    bool system;
    /** how long to wait before connecting again, after the bus failed */
    uint64_t retry_usec;
    /** a new connection: the user's matches on it, which are in place
     * before the service is looked for; < 0 if it can't */
    int (*setup)( sd_bus* bus, void* data );
    /** the service is there, found or started */
    void (*appeared)( void* data );
    /** it isn't, or it stopped */
    void (*gone)( void* data );
    /** the connection goes away, lost or stopped: drop the slots on it */
    void (*dropped)( void* data );
} pa_bus_ops_t;

/** Connects from the loop, soon; ops is kept. NULL if it can't. */
pa_bus_t* pa_bus_start( pa_mainloop_api* api, const pa_bus_ops_t* ops, void* data );
/** From the user: the connection is broken, so it is dropped and made
 * again later, once the loop gets to it (:c:func:`pa_bus_update`). */
void pa_bus_fail( pa_bus_t* b );
/** Handles whatever the bus has, then waits for what it wants next; for
 * calls queued outside the bus' callbacks. */
void pa_bus_update( pa_bus_t* b );
/** Disconnects and frees b; NULL is fine. */
void pa_bus_stop( pa_bus_t* b );
