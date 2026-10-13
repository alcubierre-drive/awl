#pragma once

#include <pthread.h>
#include <stdint.h>
#include "nm.h"

/** The most addresses `awl_ipaddr_t` keeps. */
#define AWL_IP_MAX 8

/** One IPv4 address and where it came from. */
typedef struct {
    uint32_t addr; /**< network byte order */
    char ifname[16];
    int ifindex;
    /** the lowest metric of the interface's default routes in the main
     * table, -1 if it has none */
    int metric;
} awl_ipaddr_entry_t;

/** The machine's IPv4 addresses. */
struct awl_ipaddr_t {
    /** output, under `lock`: the first n_addr IPv4 addresses of the
     * interfaces not excluded below, in the kernel's order; none means
     * disconnected. best is the index of NetworkManager's primary
     * connection's address if NetworkManager runs and has one (then by_nm),
     * else of the one the kernel would send from to the internet (the source
     * of ``ip route get 1.1.1.1``) if routed, else the first one; -1 without
     * any. routed: whether the kernel has a route to the internet.
     * connectivity: NetworkManager's AWL_NM_CONNECTIVITY_*, -1 without it. */
    awl_ipaddr_entry_t entry[AWL_IP_MAX];
    int n_addr, best, routed, by_nm, connectivity;
    pthread_mutex_t lock;

    /** input, read-only after `ip_init()`: interface names to skip */
    char exclude_list[4][16];
    int n_exclude_list;

    /** rtnetlink IPv4 address and route events, -1 if unavailable */
    int nl_fd;
    /** NetworkManager, preferred over the kernel when it runs; NULL for
     * none. Set by the poller, whose loop it runs on. */
    awl_nm_t* nm;
};
typedef struct awl_ipaddr_t awl_ipaddr_t;

awl_ipaddr_t* ip_init( void );
/** Re-reads the addresses; returns nonzero if they changed. */
int ip_update( awl_ipaddr_t* ip );
/** Call when ``nl_fd`` is readable. Drains it and re-reads; returns like
 * `ip_update()`. */
int ip_dispatch( awl_ipaddr_t* ip );
/** A copy of the output fields, taken under the lock; returns n_addr. */
int ip_snapshot( awl_ipaddr_t* ip, awl_ipaddr_entry_t entry[AWL_IP_MAX], int* best, int* routed,
                 int* by_nm, int* connectivity );
void ip_free( awl_ipaddr_t* ip );
