#include "awl_bus.h"
#include "awl_log.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <systemd/sd-bus.h>
#include <wayland-server-core.h>

#define RETRY_MS 5000

struct awl_bus {
    struct wl_event_loop* loop;
    const awl_bus_ops_t* ops;
    void* data;
    sd_bus* bus;
    struct wl_event_source *io, *timeout, *retry;
    int failed;
};

static void bus_update( awl_bus_t* b );

static void drop_bus( awl_bus_t* b ) {
    if (b->io) wl_event_source_remove( b->io );
    if (b->timeout) wl_event_source_remove( b->timeout );
    b->io = b->timeout = NULL;
    if (b->bus) b->ops->dropped( b->data );
    /* not sd_bus_flush_close_unref(): that blocks until the queue is out */
    if (b->bus) sd_bus_close( b->bus );
    b->bus = sd_bus_unref( b->bus );
    b->failed = 0;
}

static int bus_io( int fd, uint32_t mask, void* data ) {
    (void)fd; (void)mask;
    bus_update( data );
    return 0;
}

static int bus_timeout( void* data ) {
    bus_update( data );
    return 0;
}

static void connect_bus( awl_bus_t* b );

static int retry_fire( void* data ) {
    connect_bus( data );
    return 0;
}

static void retry( awl_bus_t* b, int ms ) {
    if (!b->retry) b->retry = wl_event_loop_add_timer( b->loop, retry_fire, b );
    /* 0 would disarm it */
    if (b->retry) wl_event_source_timer_update( b->retry, ms > 0 ? ms : 1 );
}

static void connect_bus( awl_bus_t* b ) {
    int r;
    if ((r = sd_bus_open_user_with_description( &b->bus, b->ops->description )) < 0 ||
        (r = b->ops->setup( b->bus, b->data )) < 0 ||
        !(b->io = wl_event_loop_add_fd( b->loop, sd_bus_get_fd( b->bus ), WL_EVENT_READABLE, bus_io, b ))) {
        awl_wrn_printf( "%s: no session bus (%s), retrying\n", b->ops->name, strerror( r < 0 ? -r : ENOMEM ) );
        drop_bus( b );
        retry( b, RETRY_MS );
        return;
    }
    bus_update( b );
}

/** Handles whatever the bus has, then waits for what it wants next. */
static void bus_update( awl_bus_t* b ) {
    int r;
    while ((r = sd_bus_process( b->bus, NULL )) > 0)
        ;
    if (r < 0 || b->failed) {
        awl_wrn_printf( "%s: lost the session bus, reconnecting\n", b->ops->name );
        drop_bus( b );
        retry( b, RETRY_MS );
        return;
    }
    int events = sd_bus_get_events( b->bus );
    wl_event_source_fd_update( b->io, (events & POLLIN ? WL_EVENT_READABLE : 0) |
                                      (events & POLLOUT ? WL_EVENT_WRITABLE : 0) );
    uint64_t until;
    if (sd_bus_get_timeout( b->bus, &until ) < 0 || until == UINT64_MAX) {
        if (b->timeout) wl_event_source_timer_update( b->timeout, 0 );
        return;
    }
    /* until is on CLOCK_MONOTONIC */
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    int ms = until > now ? (int)((until - now + 999) / 1000) : 1;
    if (!b->timeout) b->timeout = wl_event_loop_add_timer( b->loop, bus_timeout, b );
    if (b->timeout) wl_event_source_timer_update( b->timeout, ms > 0 ? ms : 1 );
}

awl_bus_t* awl_bus_start( struct wl_event_loop* loop, const awl_bus_ops_t* ops, void* data ) {
    awl_bus_t* b = calloc( 1, sizeof(*b) );
    if (!b) {
        awl_err_printf( "%s: can't start\n", ops->name );
        return NULL;
    }
    *b = (awl_bus_t){ .loop = loop, .ops = ops, .data = data };
    /* the first connection from the loop: no socket work during setup */
    retry( b, 0 );
    if (!b->retry) {
        awl_err_printf( "%s: can't start\n", ops->name );
        free( b );
        return NULL;
    }
    return b;
}

void awl_bus_fail( awl_bus_t* b ) {
    b->failed = 1;
}

void awl_bus_stop( awl_bus_t* b ) {
    if (!b) return;
    drop_bus( b );
    if (b->retry) wl_event_source_remove( b->retry );
    free( b );
}
