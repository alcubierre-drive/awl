#define _GNU_SOURCE
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_addr.h>

#include "ipaddr.h"

static int is_not_in_exclude_list( const char* name, char exclude_list[4][16], int nexclude );

/* The kernel announces every IPv4 address and route that is added or removed
 * (DHCP lease, link down, VPN, a changed metric, ...) on these multicast
 * groups; nothing else is shown by the widget, so nothing else needs to wake
 * us. */
static int rtnl_open( void ) {
    int fd = socket( AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE );
    if (fd < 0) return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK,
                              .nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE };
    if (bind( fd, (struct sockaddr*)&sa, sizeof sa ) < 0) {
        close( fd );
        return -1;
    }
    return fd;
}

awl_ipaddr_t* ip_init( void ) {
    awl_ipaddr_t* ip = calloc(1, sizeof(awl_ipaddr_t));
    if (!ip) return NULL;
    strcpy( ip->exclude_list[ip->n_exclude_list++], "lo" );
    strcpy( ip->exclude_list[ip->n_exclude_list++], "virbr0" );
    strcpy( ip->exclude_list[ip->n_exclude_list++], "docker0" );
    ip->best = -1;
    ip->connectivity = -1;
    pthread_mutex_init( &ip->lock, NULL );
    ip->nl_fd = rtnl_open();
    return ip;
}

void ip_free( awl_ipaddr_t* ip ) {
    if (!ip) return;
    if (ip->nl_fd >= 0) close( ip->nl_fd );
    pthread_mutex_destroy( &ip->lock );
    free(ip);
}

int ip_dispatch( awl_ipaddr_t* ip ) {
    // the messages only tell us that something changed; fresh queries have
    // the full picture. ENOBUFS (lost events) ends up here as well.
    char buf[8192];
    while (recv( ip->nl_fd, buf, sizeof buf, 0 ) > 0) {}
    return ip_update( ip );
}

/* Sends req on a fresh rtnetlink socket and hands each reply message to
 * cb(nh, ctx): all of a dump's until NLMSG_DONE, or a single request's
 * answer. Returns 0, or -1 if it failed or the kernel answered with an error
 * (e.g. no route). */
static int rtnl_query( struct nlmsghdr* req, void (*cb)( struct nlmsghdr* nh, void* ctx ), void* ctx ) {
    int fd = socket( AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE );
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 1 };
    setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv );
    if (send( fd, req, req->nlmsg_len, 0 ) < 0) {
        close( fd );
        return -1;
    }
    int dump = !!(req->nlmsg_flags & NLM_F_DUMP), res = 0, done = 0;
    char buf[16384] __attribute__((aligned(NLMSG_ALIGNTO)));
    while (!done) {
        ssize_t len = recv( fd, buf, sizeof buf, 0 );
        if (len <= 0) { res = -1; break; }
        for (struct nlmsghdr* nh = (struct nlmsghdr*)buf; NLMSG_OK( nh, (size_t)len );
                nh = NLMSG_NEXT( nh, len )) {
            if (nh->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr* err = NLMSG_DATA( nh );
                if (err->error) res = -1;
                done = 1;
                break;
            }
            cb( nh, ctx );
        }
        if (!dump) done = 1;
    }
    close( fd );
    return res;
}

typedef struct {
    awl_ipaddr_t* ip;
    awl_ipaddr_entry_t* e;
    int n;
} addr_ctx_t;

static void on_addr( struct nlmsghdr* nh, void* data ) {
    addr_ctx_t* ctx = data;
    if (nh->nlmsg_type != RTM_NEWADDR || ctx->n >= AWL_IP_MAX) return;
    struct ifaddrmsg* ifa = NLMSG_DATA( nh );
    if (ifa->ifa_family != AF_INET) return;

    uint32_t local = 0, address = 0;
    int has_local = 0, has_address = 0;
    const char* label = NULL;
    int rlen = IFA_PAYLOAD( nh );
    for (struct rtattr* rta = IFA_RTA( ifa ); RTA_OK( rta, rlen ); rta = RTA_NEXT( rta, rlen )) {
        switch (rta->rta_type) {
            case IFA_LOCAL: memcpy( &local, RTA_DATA( rta ), 4 ); has_local = 1; break;
            case IFA_ADDRESS: memcpy( &address, RTA_DATA( rta ), 4 ); has_address = 1; break;
            case IFA_LABEL: label = RTA_DATA( rta ); break;
        }
    }
    if (!has_local && !has_address) return;
    char name[IF_NAMESIZE] = "";
    if (!label) label = if_indextoname( ifa->ifa_index, name );
    if (!label) label = "?";
    if (!is_not_in_exclude_list( label, ctx->ip->exclude_list, ctx->ip->n_exclude_list )) return;
    awl_ipaddr_entry_t* e = &ctx->e[ctx->n++];
    // IFA_ADDRESS is the peer's on point-to-point links
    e->addr = has_local ? local : address;
    snprintf( e->ifname, sizeof e->ifname, "%s", label );
    e->ifindex = (int)ifa->ifa_index;
    e->metric = -1;
}

/* the IPv4 addresses, from an RTM_GETADDR dump; how many, -1 on failure */
static int dump_addrs( awl_ipaddr_t* ip, awl_ipaddr_entry_t* e ) {
    struct {
        struct nlmsghdr nh;
        struct ifaddrmsg ifa;
    } req = {
        .nh = { .nlmsg_len = sizeof req, .nlmsg_type = RTM_GETADDR,
                .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 1 },
        .ifa = { .ifa_family = AF_INET },
    };
    addr_ctx_t ctx = { .ip = ip, .e = e };
    return rtnl_query( &req.nh, on_addr, &ctx ) < 0 ? -1 : ctx.n;
}

/* a default route of the main table: the lowest metric per interface */
static void on_route( struct nlmsghdr* nh, void* data ) {
    addr_ctx_t* ctx = data;
    if (nh->nlmsg_type != RTM_NEWROUTE) return;
    struct rtmsg* rt = NLMSG_DATA( nh );
    if (rt->rtm_family != AF_INET || rt->rtm_dst_len != 0 || rt->rtm_type != RTN_UNICAST)
        return;
    uint32_t table = rt->rtm_table, oif = 0, metric = 0;
    int rlen = RTM_PAYLOAD( nh );
    for (struct rtattr* rta = RTM_RTA( rt ); RTA_OK( rta, rlen ); rta = RTA_NEXT( rta, rlen )) {
        switch (rta->rta_type) {
            case RTA_TABLE: memcpy( &table, RTA_DATA( rta ), 4 ); break;
            case RTA_OIF: memcpy( &oif, RTA_DATA( rta ), 4 ); break;
            case RTA_PRIORITY: memcpy( &metric, RTA_DATA( rta ), 4 ); break;
        }
    }
    if (table != RT_TABLE_MAIN || !oif) return;
    for (int i=0; i<ctx->n; ++i)
        if (ctx->e[i].ifindex == (int)oif && (ctx->e[i].metric < 0 || (uint32_t)ctx->e[i].metric > metric))
            ctx->e[i].metric = (int)metric;
}

static void dump_metrics( awl_ipaddr_t* ip, awl_ipaddr_entry_t* e, int n ) {
    struct {
        struct nlmsghdr nh;
        struct rtmsg rt;
    } req = {
        .nh = { .nlmsg_len = sizeof req, .nlmsg_type = RTM_GETROUTE,
                .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 1 },
        .rt = { .rtm_family = AF_INET },
    };
    addr_ctx_t ctx = { .ip = ip, .e = e, .n = n };
    rtnl_query( &req.nh, on_route, &ctx );
}

typedef struct {
    uint32_t oif, prefsrc;
    int has_prefsrc;
} route_ctx_t;

static void on_route_get( struct nlmsghdr* nh, void* data ) {
    route_ctx_t* ctx = data;
    if (nh->nlmsg_type != RTM_NEWROUTE) return;
    struct rtmsg* rt = NLMSG_DATA( nh );
    int rlen = RTM_PAYLOAD( nh );
    for (struct rtattr* rta = RTM_RTA( rt ); RTA_OK( rta, rlen ); rta = RTA_NEXT( rta, rlen )) {
        switch (rta->rta_type) {
            case RTA_OIF: memcpy( &ctx->oif, RTA_DATA( rta ), 4 ); break;
            case RTA_PREFSRC: memcpy( &ctx->prefsrc, RTA_DATA( rta ), 4 ); ctx->has_prefsrc = 1; break;
        }
    }
}

/* What ``ip route get 1.1.1.1`` answers: the kernel's own choice for the
 * internet, policy rules, metrics and VPNs included. Nothing is sent. The
 * index of the entry it would send from, -1 if there is no such route. */
static int route_get( awl_ipaddr_entry_t* e, int n ) {
    struct {
        struct nlmsghdr nh;
        struct rtmsg rt;
        struct rtattr dst;
        uint32_t addr;
    } req = {
        .nh = { .nlmsg_len = sizeof req, .nlmsg_type = RTM_GETROUTE,
                .nlmsg_flags = NLM_F_REQUEST, .nlmsg_seq = 1 },
        .rt = { .rtm_family = AF_INET, .rtm_dst_len = 32 },
        .dst = { .rta_len = RTA_LENGTH( 4 ), .rta_type = RTA_DST },
        .addr = htonl( 0x01010101 ), // 1.1.1.1
    };
    route_ctx_t ctx = {0};
    if (rtnl_query( &req.nh, on_route_get, &ctx ) < 0 || !ctx.oif)
        return -1;
    // the source address on the outgoing interface, or that interface's first
    int first = -1;
    for (int i=0; i<n; ++i) {
        if (e[i].ifindex != (int)ctx.oif) continue;
        if (ctx.has_prefsrc && e[i].addr == ctx.prefsrc) return i;
        if (first < 0) first = i;
    }
    return first;
}

/* Without netlink: getifaddrs(), which knows no routes. */
static int dump_getifaddrs( awl_ipaddr_t* ip, awl_ipaddr_entry_t* e ) {
    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) == -1)
        return -1;
    int n = 0;
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL && n < AWL_IP_MAX; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!is_not_in_exclude_list(ifa->ifa_name, ip->exclude_list, ip->n_exclude_list)) continue;
        e[n].addr = ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
        snprintf( e[n].ifname, sizeof e[n].ifname, "%s", ifa->ifa_name );
        e[n].ifindex = (int)if_nametoindex( ifa->ifa_name );
        e[n].metric = -1;
        n++;
    }
    freeifaddrs(ifaddr);
    return n;
}

int ip_update( awl_ipaddr_t* ip ) {
    awl_ipaddr_entry_t e[AWL_IP_MAX];
    memset( e, 0, sizeof e );
    int best = -1, routed = 0;
    int n = dump_addrs( ip, e );
    if (n >= 0) {
        dump_metrics( ip, e, n );
        best = route_get( e, n );
        routed = best >= 0;
    } else {
        memset( e, 0, sizeof e );
        n = dump_getifaddrs( ip, e );
    }
    if (n < 0) return 0;

    // NetworkManager knows which connection is primary, and whether it
    // reaches the internet: its address wins if it is one of ours
    int by_nm = 0, connectivity = -1;
    const awl_nm_state_t* nm = ip->nm ? nm_state( ip->nm ) : NULL;
    if (nm && nm->present) {
        connectivity = nm->connectivity;
        int match = -1;
        for (int i=0; nm->has_addr && i<n; ++i) {
            if (e[i].addr != nm->addr) continue;
            if (match < 0 || !strcmp( e[i].ifname, nm->ifname )) match = i;
        }
        if (match >= 0) {
            best = match;
            by_nm = 1;
        }
    }
    if (best < 0 && n > 0) best = 0;

    pthread_mutex_lock( &ip->lock );
    int changed = n != ip->n_addr || best != ip->best || routed != ip->routed
               || by_nm != ip->by_nm || connectivity != ip->connectivity
               || memcmp( e, ip->entry, sizeof e );
    memcpy( ip->entry, e, sizeof e );
    ip->n_addr = n;
    ip->best = best;
    ip->routed = routed;
    ip->by_nm = by_nm;
    ip->connectivity = connectivity;
    pthread_mutex_unlock( &ip->lock );
    return changed;
}

int ip_snapshot( awl_ipaddr_t* ip, awl_ipaddr_entry_t entry[AWL_IP_MAX], int* best, int* routed,
                 int* by_nm, int* connectivity ) {
    pthread_mutex_lock( &ip->lock );
    int n = ip->n_addr;
    memcpy( entry, ip->entry, sizeof ip->entry );
    if (best) *best = ip->best;
    if (routed) *routed = ip->routed;
    if (by_nm) *by_nm = ip->by_nm;
    if (connectivity) *connectivity = ip->connectivity;
    pthread_mutex_unlock( &ip->lock );
    return n;
}

static int is_not_in_exclude_list( const char* name, char exclude_list[4][16], int nexclude ) {
    int is_in_list = 0;
    for (int i=0; i<nexclude; ++i)
        is_in_list += !strcmp(name, exclude_list[i]);
    return !is_in_list;
}
