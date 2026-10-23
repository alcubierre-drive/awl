#include "backlight.h"
#include "../awl_host.h"
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <systemd/sd-bus.h>

/** Tracks the backlight-tooler daemon (``backlight-tooler -D``) through its
 * D-Bus interface: whether it owns ``org.BacklightTooler`` on the user bus,
 * whether its automatic updates are on (``Interval`` > 0), and the last
 * screen and webcam brightness it saw. All of it is followed by signals,
 * NameOwnerChanged and the daemon's PropertiesChanged, so nothing is
 * polled. Runs on the plugins' shared loop (poller.h): the bus
 * fd and its timeout are event sources there, and every call is
 * asynchronous, so a slow bus never holds up the other plugins. The bar's
 * requests (:c:func:`backlight_toggle`, :c:func:`backlight_update`, :c:func:`backlight_adjust`) go out on the same
 * connection; they are the only thing that starts the daemon, through its
 * systemd user unit, never D-Bus auto-start. Everything but the atomics and
 * the wake fd is only touched on the loop thread, or before it starts and
 * after it has stopped; the first connection is made from the loop too. If
 * the bus is unavailable or the connection drops, the last known state is
 * kept, requests are dropped, and a reconnect is attempted every
 * RETRY_USEC. */

#define BUS_NAME  "org.BacklightTooler"
#define BUS_PATH  "/org/BacklightTooler"
#define BUS_IFACE "org.BacklightTooler"
#define UNIT      "backlight-tooler.service"
#define RETRY_USEC (30 * 1000000ull)
/** how long a call may go unanswered before it counts as failed */
#define CALL_TIMEOUT_USEC (2 * 1000000ull)

/** :c:member:`awl_backlight_t.requests`; a second toggle before the loop got to the
 * first one cancels it */
enum awl_backlight_request { REQ_TOGGLE = 1, REQ_UPDATE = 2, REQ_ADJUST = 4 };

struct awl_backlight_bus_t {
    awl_backlight_t* b;
    pa_mainloop_api* api;
    sd_bus* bus;
    sd_bus_slot* owner_match;
    sd_bus_slot* props_match;
    pa_io_event* io;
    pa_time_event* timeout; /* the bus's next timeout, NULL if none */
    pa_time_event* retry;   /* the next connection attempt, NULL if none */
    pa_io_event* wake;      /* awl_backlight_t.wake_fd */
    int failed;             /* set by callbacks, which can't drop the bus */
    /* the daemon's Interval and ConfigInterval as last seen; Interval as
     * last set by a toggle until the daemon tells otherwise */
    uint64_t interval, config_interval;
    /* brightness changes (per mille) asked for while the daemon wasn't
     * running, sent when it is */
    int pending_adjust;
};

static void bus_update( awl_backlight_bus_t* s );
static void send_adjust( awl_backlight_bus_t* s );

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

static int set_int( atomic_int* a, int v ) {
    return atomic_exchange( a, v ) != v;
}

/** brightness in [0,1] as per mille; the daemon's -1 (none yet) as
 * :c:macro:`BACKLIGHT_UNKNOWN`, anything else (NaN, out of range) as :c:macro:`BACKLIGHT_INVALID` */
static int to_permille( double v ) {
    if (v >= 0 && v <= 1) return (int)(v * 1000 + 0.5);
    return v == -1 ? BACKLIGHT_UNKNOWN : BACKLIGHT_INVALID;
}

/** the daemon went away: nothing is known anymore */
static void set_gone( awl_backlight_bus_t* s ) {
    awl_backlight_t* b = s->b;
    s->interval = s->config_interval = 0;
    int changed = set_int( &b->running, 0 );
    changed |= set_int( &b->enabled, 0 );
    changed |= set_int( &b->screen, BACKLIGHT_UNKNOWN );
    changed |= set_int( &b->webcam, BACKLIGHT_UNKNOWN );
    if (changed) awl_host->redraw_request();
}

/** a method reply: whether it is a proper one; marks the connection as
 * broken otherwise */
static int reply_ok( sd_bus_message* m, awl_backlight_bus_t* s ) {
    if (!sd_bus_message_is_method_error( m, NULL )) return 1;
    s->failed = 1;
    return 0;
}

/** Reads an a{sv} of the daemon's properties, keeps those we track and
 * requests a redraw if any of them changed. ``running``: 1 if the daemon is
 * known to run now (a GetAll reply), 0 to leave it as is. */
static void read_properties( awl_backlight_bus_t* s, sd_bus_message* m, int running ) {
    awl_backlight_t* b = s->b;
    int changed = running ? set_int( &b->running, 1 ) : 0;
    if (sd_bus_message_enter_container( m, 'a', "{sv}" ) < 0) goto out;
    while (sd_bus_message_enter_container( m, 'e', "sv" ) > 0) {
        const char* key = NULL;
        if (sd_bus_message_read( m, "s", &key ) < 0) goto out;
        int r;
        if (!strcmp( key, "Interval" )) {
            uint64_t interval;
            if ((r = sd_bus_message_read( m, "v", "t", &interval )) >= 0) {
                s->interval = interval;
                changed |= set_int( &b->enabled, interval != 0 );
            }
        } else if (!strcmp( key, "ConfigInterval" )) {
            r = sd_bus_message_read( m, "v", "t", &s->config_interval );
        } else if (!strcmp( key, "ScreenBrightness" ) || !strcmp( key, "WebcamValue" )) {
            double v;
            if ((r = sd_bus_message_read( m, "v", "d", &v )) >= 0)
                changed |= set_int( key[0] == 'S' ? &b->screen : &b->webcam, to_permille( v ) );
        } else {
            r = sd_bus_message_skip( m, "v" );
        }
        if (r < 0 || sd_bus_message_exit_container( m ) < 0) goto out;
    }
    sd_bus_message_exit_container( m );
out:
    if (changed) awl_host->redraw_request();
}

/** Properties.GetAll(BUS_IFACE) -> a{sv}. An error means the daemon went
 * away or hangs; the former is told by NameOwnerChanged, which follows, and
 * the latter is no reason to drop the connection. */
static int on_properties( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_backlight_bus_t* s = userdata;
    if (!sd_bus_message_is_method_error( m, NULL )) {
        read_properties( s, m, 1 );
        send_adjust( s );
    }
    return 0;
}

/** Calls the daemon's iface.member with the arguments of ``types``; the reply
 * goes to callback, or nowhere if NULL. Built by hand to turn off
 * auto-start: a call never starts the daemon. */
static void call_daemon( awl_backlight_bus_t* s, const char* iface, const char* member,
                         sd_bus_message_handler_t callback, const char* types, ... ) {
    sd_bus_message* m = NULL;
    va_list ap;
    va_start( ap, types );
    if (sd_bus_message_new_method_call( s->bus, &m, BUS_NAME, BUS_PATH, iface, member ) < 0 ||
        sd_bus_message_set_auto_start( m, 0 ) < 0 ||
        (types && sd_bus_message_appendv( m, types, ap ) < 0) ||
        sd_bus_call_async( s->bus, NULL, m, callback, s, 0 ) < 0)
        s->failed = 1;
    va_end( ap );
    sd_bus_message_unref( m );
}

static void query_properties( awl_backlight_bus_t* s ) {
    call_daemon( s, "org.freedesktop.DBus.Properties", "GetAll", on_properties, "s", BUS_IFACE );
}

/** PropertiesChanged(s interface, a{sv} changed, as invalidated), only the
 * daemon's interface (arg0 in the match) */
static int on_properties_changed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_backlight_bus_t* s = userdata;
    if (sd_bus_message_skip( m, "s" ) < 0) return 0;
    /* not `running` yet: a signal can come before the GetAll reply that
     * follows the daemon's start, and alone would flash a running daemon
     * with its updates off */
    read_properties( s, m, 0 );
    /* read_properties() stops early on a malformed message; then this fails */
    char** invalidated = NULL;
    if (sd_bus_message_read_strv( m, &invalidated ) >= 0 && invalidated) {
        int requery = invalidated[0] != NULL;
        for (char** i = invalidated; *i; ++i) free( *i );
        free( invalidated );
        if (requery) query_properties( s );
    }
    return 0;
}

/** NameOwnerChanged(s name, s old_owner, s new_owner), only for :c:macro:`BUS_NAME`
 * (arg0 in the match) */
static int on_owner_changed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_backlight_bus_t* s = userdata;
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (sd_bus_message_read( m, "sss", &name, &old_owner, &new_owner ) < 0) return 0;
    if (new_owner[0])
        query_properties( s );
    else
        set_gone( s );
    return 0;
}

/** GetNameOwner(s) -> s; NameHasNoOwner if the daemon isn't running */
static int on_owner( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_backlight_bus_t* s = userdata;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    if (e && sd_bus_error_has_name( e, SD_BUS_ERROR_NAME_HAS_NO_OWNER ))
        set_gone( s );
    else if (reply_ok( m, s ))
        query_properties( s );
    return 0;
}

static int on_subscribed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    reply_ok( m, userdata );
    return 0;
}

static void schedule_retry( awl_backlight_bus_t* s, uint64_t usec );

static void drop_bus( awl_backlight_bus_t* s ) {
    if (s->io) s->api->io_free( s->io );
    if (s->timeout) s->api->time_free( s->timeout );
    s->io = NULL;
    s->timeout = NULL;
    s->owner_match = sd_bus_slot_unref( s->owner_match );
    s->props_match = sd_bus_slot_unref( s->props_match );
    /* not sd_bus_flush_close_unref(): that blocks until the queue is out */
    if (s->bus) sd_bus_close( s->bus );
    s->bus = sd_bus_unref( s->bus );
    s->failed = 0;
    s->pending_adjust = 0;
}

static void bus_io( pa_mainloop_api* a, pa_io_event* e, int fd, pa_io_event_flags_t f, void* userdata ) {
    (void)a; (void)e; (void)fd; (void)f;
    bus_update( userdata );
}

static void bus_timeout( pa_mainloop_api* a, pa_time_event* e, const struct timeval* tv, void* userdata ) {
    (void)tv;
    awl_backlight_bus_t* s = userdata;
    a->time_free( e );
    s->timeout = NULL;
    bus_update( s );
}

static void connect_bus( awl_backlight_bus_t* s ) {
    /* The bus handles these in order, so both matches are in place before
     * the owner is asked for, and no change can slip through in between. */
    if (sd_bus_open_user( &s->bus ) < 0 ||
        sd_bus_set_method_call_timeout( s->bus, CALL_TIMEOUT_USEC ) < 0 ||
        !(s->io = s->api->io_new( s->api, sd_bus_get_fd( s->bus ), PA_IO_EVENT_INPUT, bus_io, s )) ||
        sd_bus_add_match_async( s->bus, &s->owner_match,
                "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" BUS_NAME "'",
                on_owner_changed, on_subscribed, s ) < 0 ||
        sd_bus_add_match_async( s->bus, &s->props_match,
                "type='signal',sender='" BUS_NAME "',path='" BUS_PATH "',"
                "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
                "arg0='" BUS_IFACE "'",
                on_properties_changed, on_subscribed, s ) < 0 ||
        sd_bus_call_method_async( s->bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetNameOwner", on_owner, s, "s", BUS_NAME ) < 0) {
        drop_bus( s );
        schedule_retry( s, RETRY_USEC );
        return;
    }
    bus_update( s );
}

static void retry_callback( pa_mainloop_api* a, pa_time_event* e, const struct timeval* tv, void* userdata ) {
    (void)tv;
    awl_backlight_bus_t* s = userdata;
    a->time_free( e );
    s->retry = NULL;
    connect_bus( s );
}

static void schedule_retry( awl_backlight_bus_t* s, uint64_t usec ) {
    if (s->retry) return;
    struct timeval tv = in_usec( usec );
    s->retry = s->api->time_new( s->api, &tv, retry_callback, s );
}

/** Handles whatever the bus has, then waits for what it wants next. */
static void bus_update( awl_backlight_bus_t* s ) {
    int r;
    while ((r = sd_bus_process( s->bus, NULL )) > 0)
        ;
    if (r < 0 || s->failed) {
        /* connection lost or the daemon can't be watched */
        drop_bus( s );
        schedule_retry( s, RETRY_USEC );
        return;
    }

    int events = sd_bus_get_events( s->bus );
    s->api->io_enable( s->io, (events & POLLIN ? PA_IO_EVENT_INPUT : 0) |
                              (events & POLLOUT ? PA_IO_EVENT_OUTPUT : 0) );

    uint64_t until;
    if (sd_bus_get_timeout( s->bus, &until ) < 0 || until == UINT64_MAX) {
        if (s->timeout) s->api->time_free( s->timeout );
        s->timeout = NULL;
        return;
    }
    /* until is on CLOCK_MONOTONIC */
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    struct timeval tv = in_usec( until > now ? until - now : 0 );
    if (s->timeout)
        s->api->time_restart( s->timeout, &tv );
    else
        s->timeout = s->api->time_new( s->api, &tv, bus_timeout, s );
}

/** Properties.Set(Interval): refused (a daemon without a writable Interval,
 * an interval it won't take) or not answered, so the toggle's guess is
 * wrong; back to what the daemon says. */
static int on_set( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_backlight_bus_t* s = userdata;
    if (sd_bus_message_is_method_error( m, NULL )) query_properties( s );
    return 0;
}

/** the daemon's user unit; it updates once when it starts */
static void start_unit( awl_backlight_bus_t* s ) {
    if (sd_bus_call_method_async( s->bus, NULL, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                "org.freedesktop.systemd1.Manager", "StartUnit", NULL, NULL, "ss", UNIT, "replace" ) < 0)
        s->failed = 1;
}

static void do_toggle( awl_backlight_bus_t* s ) {
    if (!atomic_load( &s->b->running )) {
        start_unit( s );
        return;
    }
    uint64_t target = s->interval ? 0 : s->config_interval;
    /* off, and nothing to turn on: [daemon] interval=0, or an old daemon */
    if (target == s->interval) return;
    call_daemon( s, "org.freedesktop.DBus.Properties", "Set", on_set, "ssv",
            BUS_IFACE, "Interval", "t", target );
    /* back on: catch up now rather than one interval from now */
    if (target) call_daemon( s, BUS_IFACE, "Update", NULL, NULL );
    /* so that the next toggle undoes this one even before the daemon's
     * PropertiesChanged is in */
    s->interval = target;
}

static void do_update( awl_backlight_bus_t* s ) {
    if (atomic_load( &s->b->running ))
        call_daemon( s, BUS_IFACE, "Update", NULL, NULL );
    else
        start_unit( s );
}

/** sends pending_adjust, if any; the daemon takes [-1,1] */
static void send_adjust( awl_backlight_bus_t* s ) {
    int d = s->pending_adjust;
    s->pending_adjust = 0;
    if (d < -1000) d = -1000;
    if (d > 1000) d = 1000;
    if (d) call_daemon( s, BUS_IFACE, "Adjust", NULL, "d", d / 1000.0 );
}

static void do_adjust( awl_backlight_bus_t* s, int permille ) {
    s->pending_adjust += permille;
    if (atomic_load( &s->b->running ))
        send_adjust( s );
    else
        start_unit( s );
}

static void wake_callback( pa_mainloop_api* a, pa_io_event* e, int fd, pa_io_event_flags_t f, void* userdata ) {
    (void)a; (void)e; (void)f;
    awl_backlight_bus_t* s = userdata;
    uint64_t count;
    if (read( fd, &count, sizeof count ) < 0) {}
    int req = atomic_exchange( &s->b->requests, 0 );
    int adjust = atomic_exchange( &s->b->adjust, 0 );
    if (!req || !s->bus) return;
    if (req & REQ_TOGGLE) do_toggle( s );
    if (req & REQ_UPDATE) do_update( s );
    if (adjust) do_adjust( s, adjust );
    bus_update( s );
}

static void request( awl_backlight_t* b, int req ) {
    if (!b) return;
    if (req == REQ_TOGGLE)
        atomic_fetch_xor( &b->requests, req );
    else
        atomic_fetch_or( &b->requests, req );
    uint64_t one = 1;
    /* EAGAIN only if the counter would overflow: the loop is due anyway */
    if (write( b->wake_fd, &one, sizeof one ) < 0) {}
}

void backlight_toggle( awl_backlight_t* b ) {
    request( b, REQ_TOGGLE );
}

void backlight_update( awl_backlight_t* b ) {
    request( b, REQ_UPDATE );
}

void backlight_adjust( awl_backlight_t* b, float delta ) {
    if (!b || !(delta >= -1.0f && delta <= 1.0f)) return;
    atomic_fetch_add( &b->adjust, (int)lroundf( delta * 1000.0f ) );
    request( b, REQ_ADJUST );
}

awl_backlight_t* backlight_init( pa_mainloop_api* api ) {
    awl_backlight_t* b = calloc(1, sizeof *b);
    if (!b) return NULL;
    atomic_init( &b->running, 0 );
    atomic_init( &b->enabled, 0 );
    atomic_init( &b->screen, BACKLIGHT_UNKNOWN );
    atomic_init( &b->webcam, BACKLIGHT_UNKNOWN );
    atomic_init( &b->requests, 0 );
    atomic_init( &b->adjust, 0 );
    b->wake_fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    if (b->wake_fd < 0 || !(b->bus = calloc(1, sizeof *b->bus)))
        goto fail;
    b->bus->b = b;
    b->bus->api = api;
    b->bus->wake = api->io_new( api, b->wake_fd, PA_IO_EVENT_INPUT, wake_callback, b->bus );
    if (!b->bus->wake) goto fail;
    schedule_retry( b->bus, 0 );
    if (!b->bus->retry) goto fail;
    return b;
fail:
    if (b->bus && b->bus->wake) api->io_free( b->bus->wake );
    if (b->wake_fd >= 0) close( b->wake_fd );
    free( b->bus );
    free( b );
    return NULL;
}

void backlight_free( awl_backlight_t* b ) {
    if (!b) return;
    drop_bus( b->bus );
    if (b->bus->retry) b->bus->api->time_free( b->bus->retry );
    b->bus->api->io_free( b->bus->wake );
    close( b->wake_fd );
    free( b->bus );
    free( b );
}
