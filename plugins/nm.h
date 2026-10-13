#pragma once

#include <stdint.h>
#include <pulse/mainloop-api.h>

/** NetworkManager's connectivity check (its ``Connectivity`` property) */
enum {
    AWL_NM_CONNECTIVITY_UNKNOWN = 0,
    AWL_NM_CONNECTIVITY_NONE = 1,
    AWL_NM_CONNECTIVITY_PORTAL = 2,
    AWL_NM_CONNECTIVITY_LIMITED = 3,
    AWL_NM_CONNECTIVITY_FULL = 4,
};

/** What NetworkManager says about the network, as far as the IP widget
 * cares. */
typedef struct {
    /** NetworkManager runs on the system bus and has answered */
    int present;
    /** AWL_NM_CONNECTIVITY_* */
    int connectivity;
    /** the primary connection's first IPv4 address (network byte order) and
     * the interface it is on ("" if unknown); has_addr 0 without either */
    int has_addr;
    uint32_t addr;
    char ifname[16];
} awl_nm_state_t;

typedef struct awl_nm_t awl_nm_t;

/** Follows NetworkManager on the system bus from the loop of `api`, if it
 * is there: its owner, connectivity and primary connection, all by signals
 * and asynchronous calls. changed(userdata) runs on that loop whenever
 * `nm_state()` changes. NetworkManager is optional: without it (or without
 * a system bus) the state stays not present, and a reconnect is tried now
 * and then. Created and freed while the loop isn't running; the first
 * connection is made from the loop. */
awl_nm_t* nm_new( pa_mainloop_api* api, void (*changed)( void* userdata ), void* userdata );
/** the current state; loop thread only */
const awl_nm_state_t* nm_state( const awl_nm_t* nm );
void nm_free( awl_nm_t* nm );
