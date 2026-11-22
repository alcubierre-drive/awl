#include "pa_bus.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <systemd/sd-bus.h>

/** how long a call may go unanswered before it counts as failed */
#define CALL_TIMEOUT_USEC (2 * 1000000ull)

struct pa_bus {
    pa_mainloop_api* api;
    const pa_bus_ops_t* ops;
    void* data;
    sd_bus* bus;
    sd_bus_slot* owner_match;
    pa_io_event* io;
    pa_time_event *timeout, *retry;
    bool failed;
};

/** a timeval ``usec`` from now on the wall clock, which pa_mainloop_api takes */
static struct timeval in_usec( uint64_t usec ) {
    struct timeval tv;
    gettimeofday( &tv, NULL );
    tv.tv_sec += usec / 1000000;
    tv.tv_usec += usec % 1000000;
    if (tv.tv_usec >= 1000000) {
        tv.tv_sec++;
        tv.tv_usec -= 1000000;
    }
    return tv;
}

/** NameOwnerChanged(s name, s old_owner, s new_owner) of ops->name */
static int on_owner_changed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    pa_bus_t* b = userdata;
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (sd_bus_message_read( m, "sss", &name, &old_owner, &new_owner ) < 0) return 0;
    (new_owner[0] ? b->ops->appeared : b->ops->gone)( b->data );
    return 0;
}

/** GetNameOwner(s) -> s; NameHasNoOwner if the service isn't running */
static int on_owner( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    pa_bus_t* b = userdata;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    if (e && sd_bus_error_has_name( e, SD_BUS_ERROR_NAME_HAS_NO_OWNER ))
        b->ops->gone( b->data );
    else if (!e)
        b->ops->appeared( b->data );
    else
        b->failed = true;
    return 0;
}

static void drop_bus( pa_bus_t* b ) {
    if (b->io) b->api->io_free( b->io );
    if (b->timeout) b->api->time_free( b->timeout );
    b->io = NULL;
    b->timeout = NULL;
    b->owner_match = sd_bus_slot_unref( b->owner_match );
    b->ops->dropped( b->data );
    /* not sd_bus_flush_close_unref(): that blocks until the queue is out */
    if (b->bus) sd_bus_close( b->bus );
    b->bus = sd_bus_unref( b->bus );
    b->failed = false;
}

static void bus_io( pa_mainloop_api* a, pa_io_event* e, int fd, pa_io_event_flags_t f, void* userdata ) {
    (void)a; (void)e; (void)fd; (void)f;
    pa_bus_update( userdata );
}

static void bus_timeout( pa_mainloop_api* a, pa_time_event* e, const struct timeval* tv, void* userdata ) {
    (void)tv;
    pa_bus_t* b = userdata;
    a->time_free( e );
    b->timeout = NULL;
    pa_bus_update( b );
}

static void schedule_retry( pa_bus_t* b, uint64_t usec );

static void connect_bus( pa_bus_t* b ) {
    char rule[256];
    snprintf( rule, sizeof rule, "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
              "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'", b->ops->name );
    /* the bus handles these in order: the matches are in place before the
     * owner is asked for, so no change slips through in between; a match
     * that fails (NULL install callback) closes the connection */
    if ((b->ops->system ? sd_bus_open_system( &b->bus ) : sd_bus_open_user( &b->bus )) < 0 ||
        sd_bus_set_method_call_timeout( b->bus, CALL_TIMEOUT_USEC ) < 0 ||
        !(b->io = b->api->io_new( b->api, sd_bus_get_fd( b->bus ), PA_IO_EVENT_INPUT, bus_io, b )) ||
        sd_bus_add_match_async( b->bus, &b->owner_match, rule, on_owner_changed, NULL, b ) < 0 ||
        b->ops->setup( b->bus, b->data ) < 0 ||
        sd_bus_call_method_async( b->bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetNameOwner", on_owner, b, "s", b->ops->name ) < 0) {
        drop_bus( b );
        schedule_retry( b, b->ops->retry_usec );
        return;
    }
    pa_bus_update( b );
}

static void retry_callback( pa_mainloop_api* a, pa_time_event* e, const struct timeval* tv, void* userdata ) {
    (void)tv;
    pa_bus_t* b = userdata;
    a->time_free( e );
    b->retry = NULL;
    connect_bus( b );
}

static void schedule_retry( pa_bus_t* b, uint64_t usec ) {
    if (b->retry) return;
    struct timeval tv = in_usec( usec );
    b->retry = b->api->time_new( b->api, &tv, retry_callback, b );
}

void pa_bus_update( pa_bus_t* b ) {
    int r;
    while ((r = sd_bus_process( b->bus, NULL )) > 0)
        ;
    if (r < 0 || b->failed) {
        /* connection lost (e.g. the bus restarted) or a broken reply */
        drop_bus( b );
        schedule_retry( b, b->ops->retry_usec );
        return;
    }

    int events = sd_bus_get_events( b->bus );
    b->api->io_enable( b->io, (events & POLLIN ? PA_IO_EVENT_INPUT : 0) |
                              (events & POLLOUT ? PA_IO_EVENT_OUTPUT : 0) );

    uint64_t until;
    if (sd_bus_get_timeout( b->bus, &until ) < 0 || until == UINT64_MAX) {
        if (b->timeout) b->api->time_free( b->timeout );
        b->timeout = NULL;
        return;
    }
    /* until is on CLOCK_MONOTONIC */
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    struct timeval tv = in_usec( until > now ? until - now : 0 );
    if (b->timeout)
        b->api->time_restart( b->timeout, &tv );
    else
        b->timeout = b->api->time_new( b->api, &tv, bus_timeout, b );
}

void pa_bus_fail( pa_bus_t* b ) {
    b->failed = true;
}

pa_bus_t* pa_bus_start( pa_mainloop_api* api, const pa_bus_ops_t* ops, void* data ) {
    pa_bus_t* b = calloc( 1, sizeof *b );
    if (!b) return NULL;
    *b = (pa_bus_t){ .api = api, .ops = ops, .data = data };
    /* the first connection from the loop: no socket work on the caller's thread */
    schedule_retry( b, 0 );
    if (b->retry) return b;
    free( b );
    return NULL;
}

void pa_bus_stop( pa_bus_t* b ) {
    if (!b) return;
    drop_bus( b );
    if (b->retry) b->api->time_free( b->retry );
    free( b );
}
