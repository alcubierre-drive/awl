#pragma once

/**
 * A connection to the session bus on awl's main loop, with sd-bus, for
 * the services awl itself provides (awl_sni.h, awl_notifications.h): it
 * connects from the loop, processes the bus whenever its fd or timeout
 * says so, and connects again a few seconds after it is lost or the
 * service failed. Each service has a connection of its own, so one's
 * failure doesn't take the other's down. Main thread only.
 */

typedef struct sd_bus sd_bus;
struct wl_event_loop;
typedef struct awl_bus awl_bus_t;

typedef struct awl_bus_ops_t {
    /** in the log: "<name>: no session bus ..." */
    const char* name;
    /** sd-bus' description of the connection */
    const char* description;
    /** sets a new connection up: objects, matches, the name; < 0 if it
     * can't (then it is dropped and connected again later) */
    int (*setup)( sd_bus* bus, void* data );
    /** the connection goes away, lost or stopped: drop the slots on it
     * (they hold it) and whatever lives only as long as it does */
    void (*dropped)( void* data );
} awl_bus_ops_t;

/** Starts connecting from loop; ops is kept. NULL (logged) if it can't. */
awl_bus_t* awl_bus_start( struct wl_event_loop* loop, const awl_bus_ops_t* ops, void* data );
/** From a callback of the bus: the service failed (e.g. its name was
 * refused), so after the callback the connection is dropped and made
 * again later. */
void awl_bus_fail( awl_bus_t* b );
/** Disconnects and frees b; NULL is fine. */
void awl_bus_stop( awl_bus_t* b );
