#define _GNU_SOURCE /* asprintf() */
#include "awl_bus.h"
#include "awl_log.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
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

/** a{ss} of the names' values, for UpdateActivationEnvironment */
static int append_env_dict( sd_bus_message* m, const char* const* names ) {
    int r = sd_bus_message_open_container( m, 'a', "{ss}" );
    for (const char* const* n = names; r >= 0 && *n; n++) {
        const char* v = getenv( *n );
        if (v) r = sd_bus_message_append( m, "{ss}", *n, v );
    }
    return r < 0 ? r : sd_bus_message_close_container( m );
}

/** as: NAME=value strings, for SetEnvironment */
static int append_env_strv( sd_bus_message* m, const char* const* names ) {
    int r = sd_bus_message_open_container( m, 'a', "s" );
    for (const char* const* n = names; r >= 0 && *n; n++) {
        const char* v = getenv( *n );
        char* kv;
        if (!v) continue;
        if (asprintf( &kv, "%s=%s", *n, v ) < 0) return -ENOMEM;
        r = sd_bus_message_append( m, "s", kv );
        free( kv );
    }
    return r < 0 ? r : sd_bus_message_close_container( m );
}

static void env_call( sd_bus* bus, const char* what, const char* dest, const char* path,
                      const char* iface, const char* member,
                      int (*append)( sd_bus_message*, const char* const* ), const char* const* names ) {
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r;
    if ((r = sd_bus_message_new_method_call( bus, &m, dest, path, iface, member )) >= 0 &&
        (r = append( m, names )) >= 0)
        r = sd_bus_call( bus, m, 0, &e, &reply );
    if (r < 0)
        awl_wrn_printf( "environment: %s: %s\n", what, e.message ? e.message : strerror( -r ) );
    sd_bus_error_free( &e );
    sd_bus_message_unref( reply );
    sd_bus_message_unref( m );
}

void awl_bus_import_environment( const char* const* names ) {
    sd_bus* bus = NULL;
    int r;
    if ((r = sd_bus_open_user_with_description( &bus, "awl-environment" )) < 0 ||
        (r = sd_bus_set_method_call_timeout( bus, 2 * 1000000ull )) < 0) {
        awl_wrn_printf( "environment: no session bus (%s)\n", strerror( -r ) );
        sd_bus_unref( bus );
        return;
    }
    /* what `dbus-update-activation-environment --systemd` does */
    env_call( bus, "systemd", "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
              "org.freedesktop.systemd1.Manager", "SetEnvironment", append_env_strv, names );
    env_call( bus, "D-Bus activation", "org.freedesktop.DBus", "/org/freedesktop/DBus",
              "org.freedesktop.DBus", "UpdateActivationEnvironment", append_env_dict, names );
    sd_bus_flush_close_unref( bus );
}
