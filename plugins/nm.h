#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pulse/mainloop-api.h>

/** NetworkManager's connectivity check (its ``Connectivity`` property) */
enum awl_nm_connectivity {
    AWL_NM_CONNECTIVITY_UNKNOWN = 0,
    AWL_NM_CONNECTIVITY_NONE = 1,
    AWL_NM_CONNECTIVITY_PORTAL = 2,
    AWL_NM_CONNECTIVITY_LIMITED = 3,
    AWL_NM_CONNECTIVITY_FULL = 4,
};

/** What NetworkManager says about the network, as far as the IP widget
 * cares. */
typedef struct {
    /** AWL_NM_CONNECTIVITY_* */
    int connectivity;
    /** NetworkManager runs on the system bus and has answered */
    bool present:1;
    /** whether addr and ifname are set: 0 without either */
    bool has_addr:1;
    /** the primary connection's first IPv4 address, network byte order */
    uint32_t addr;
    /** the interface it is on, "" if unknown */
    char ifname[16];
} awl_nm_state_t;

/** the NetworkManager connection, on the poller's loop; opaque */
typedef struct awl_nm_t awl_nm_t;

/** Follows NetworkManager on the system bus from the loop of ``api``, if it
 * is there: its owner, connectivity and primary connection, all by signals
 * and asynchronous calls. changed(userdata) runs on that loop whenever
 * :c:func:`nm_state` changes. NetworkManager is optional: without it (or without
 * a system bus) the state stays not present, and a reconnect is tried now
 * and then. Created and freed while the loop isn't running; the first
 * connection is made from the loop. */
awl_nm_t* nm_new( pa_mainloop_api* api, void (*changed)( void* userdata ), void* userdata );
/** the current state; loop thread only */
const awl_nm_state_t* nm_state( const awl_nm_t* nm );
void nm_free( awl_nm_t* nm );
