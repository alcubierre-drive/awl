#include "logind.h"
#include "thread.h"
#include "../awl_host.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

/** Everything asynchronous, like plugins/nm.c, but on a thread of its own: a
 * stuck poller loop must not hold up a lock before suspend. */
#define LOGIND_NAME    "org.freedesktop.login1"
#define LOGIND_PATH    "/org/freedesktop/login1"
#define LOGIND_MANAGER LOGIND_NAME ".Manager"
#define LOGIND_SESSION LOGIND_NAME ".Session"
#define RETRY_USEC (5 * 1000000ull)
#define CALL_TIMEOUT_USEC (5 * 1000000ull)

typedef struct {
    awl_thread_t t;
    sd_bus* bus;
    sd_bus_slot *owner_match, *lock_match, *unlock_match, *sleep_match;
    /** the session's object path, "" until GetSession answered */
    char session[256];
    /** a reply or a call failed; the connection is dropped and retried */
    int failed;
    /** :c:enumerator:`AWL_LOGIND_LOCK`, :c:enumerator:`AWL_LOGIND_SLEEP`: whose matches are in place */
    unsigned subscribed;
    /** the delay inhibitor, -1 while none is held */
    int inhibit_fd;
    /** an Inhibit call is out */
    int inhibiting;
    /** PrepareForSleep(true) came, the inhibitor isn't let go of yet */
    int sleeping;
    /** the config dictionary's "lock_on_suspend", read on connecting */
    int lock_on_suspend;
    /** what awl was told last (``report()``) */
    unsigned reported;
    /** :c:func:`awl_logind_covered`'s, and what LockedHint was last set to (-1: not yet) */
    atomic_int covered;
    int hint;
    /** CLOCK_MONOTONIC usec of the next connection attempt, 0 while connected */
    uint64_t retry_at;
    /** connection attempts failed in a row: only the first one warns */
    int failures;
    /** :c:func:`awl_logind_suspend` asked, the thread hasn't called Suspend yet */
    atomic_int suspend;
} logind_t;

static logind_t lg = { .inhibit_fd = -1, .hint = -1 };
static int running;

static uint64_t now_usec( void ) {
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/** tells awl what we listen for, if that changed */
static void report( logind_t* s ) {
    unsigned flags = s->subscribed & AWL_LOGIND_LOCK;
    if (s->lock_on_suspend && (s->subscribed & AWL_LOGIND_SLEEP) &&
        (s->inhibit_fd >= 0 || s->sleeping))
        flags |= AWL_LOGIND_SLEEP;
    if (flags == s->reported) return;
    s->reported = flags;
    awl_host->logind_state( flags );
}

static int reply_ok( sd_bus_message* m, logind_t* s, const char* what ) {
    const sd_bus_error* e = sd_bus_message_get_error( m );
    if (!e) return 1;
    awl_dbg_printf( s->failures++ ? awl_loglevel_vrb : awl_loglevel_wrn,
                    "logind: %s: %s\n", what, e->message ? e->message : e->name );
    s->failed = 1;
    return 0;
}

static void release( logind_t* s ) {
    if (s->inhibit_fd < 0) return;
    close( s->inhibit_fd );
    s->inhibit_fd = -1;
}

/** lets the suspend go ahead once the screen is covered */
static void check_release( logind_t* s ) {
    if (!s->sleeping || s->inhibit_fd < 0 || !atomic_load( &s->covered )) return;
    awl_log_printf( "logind: screen locked, letting the system sleep\n" );
    release( s );
}

/** Inhibit(ssss) -> h; the fd is the message's, so it is duplicated */
static int on_inhibit( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    logind_t* s = userdata;
    int fd = -1;
    s->inhibiting = 0;
    if (!reply_ok( m, s, "Inhibit" )) return 0;
    if (sd_bus_message_read( m, "h", &fd ) < 0 ||
        (fd = fcntl( fd, F_DUPFD_CLOEXEC, 3 )) < 0) {
        awl_wrn_printf( "logind: no inhibitor fd\n" );
        s->failed = 1;
        return 0;
    }
    release( s );
    s->inhibit_fd = fd;
    report( s );
    return 0;
}

static void inhibit( logind_t* s ) {
    if (!s->lock_on_suspend || s->inhibit_fd >= 0 || s->inhibiting) return;
    if (sd_bus_call_method_async( s->bus, NULL, LOGIND_NAME, LOGIND_PATH, LOGIND_MANAGER, "Inhibit",
                                  on_inhibit, s, "ssss", "sleep", "awl",
                                  "lock the screen before sleeping", "delay" ) < 0)
        s->failed = 1;
    else
        s->inhibiting = 1;
}

/** PrepareForSleep(b start) */
static int on_sleep( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    logind_t* s = userdata;
    int start = 0;
    if (sd_bus_message_read( m, "b", &start ) < 0) return 0;
    if (start) {
        if (!s->lock_on_suspend) return 0;
        awl_log_printf( "logind: going to sleep, locking%s\n",
                        s->inhibit_fd >= 0 ? "" : " (without an inhibitor: suspend won't wait)" );
        s->sleeping = 1;
        awl_host->lock_request();
        check_release( s );
    } else {
        if (s->sleeping && s->inhibit_fd >= 0)
            awl_wrn_printf( "logind: slept before the screen was locked\n" );
        s->sleeping = 0;
        inhibit( s );
    }
    report( s );
    return 0;
}

static int on_lock( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)m; (void)userdata; (void)ret_error;
    awl_log_printf( "logind: Lock\n" );
    awl_host->lock_request();
    return 0;
}

static int on_unlock( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)m; (void)userdata; (void)ret_error;
    /* only the locker unlocks a session lock */
    awl_log_printf( "logind: Unlock (ignored)\n" );
    return 0;
}

/** any change of logind's owner: it restarted, start over */
static int on_owner_changed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)m; (void)ret_error;
    logind_t* s = userdata;
    awl_wrn_printf( "logind: restarted, reconnecting\n" );
    s->failed = 1;
    return 0;
}

static int on_lock_subscribed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    logind_t* s = userdata;
    if (!reply_ok( m, s, "match on Lock" )) return 0;
    s->subscribed |= AWL_LOGIND_LOCK;
    report( s );
    return 0;
}

static int on_sleep_subscribed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    logind_t* s = userdata;
    if (!reply_ok( m, s, "match on PrepareForSleep" )) return 0;
    s->subscribed |= AWL_LOGIND_SLEEP;
    /* after the match: a PrepareForSleep can't come between the two unseen */
    inhibit( s );
    report( s );
    return 0;
}

static int add_session_match( logind_t* s, sd_bus_slot** slot, const char* member,
                              sd_bus_message_handler_t cb, sd_bus_message_handler_t installed ) {
    char rule[512];
    snprintf( rule, sizeof rule, "type='signal',sender='" LOGIND_NAME "',path='%s',"
              "interface='" LOGIND_SESSION "',member='%s'", s->session, member );
    return sd_bus_add_match_async( s->bus, slot, rule, cb, installed, s );
}

/** GetSession(s) -> o */
static int on_session( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    logind_t* s = userdata;
    const char* path = NULL;
    if (!reply_ok( m, s, "GetSession" )) return 0;
    s->failures = 0;
    if (sd_bus_message_read( m, "o", &path ) < 0 || !path) {
        s->failed = 1;
        return 0;
    }
    snprintf( s->session, sizeof s->session, "%s", path );
    awl_log_printf( "logind: session %s\n", s->session );
    if (add_session_match( s, &s->lock_match, "Lock", on_lock, on_lock_subscribed ) < 0 ||
        add_session_match( s, &s->unlock_match, "Unlock", on_unlock, NULL ) < 0)
        s->failed = 1;
    s->hint = -1; /* set it on the new session */
    return 0;
}

static void drop_bus( logind_t* s ) {
    s->owner_match = sd_bus_slot_unref( s->owner_match );
    s->lock_match = sd_bus_slot_unref( s->lock_match );
    s->unlock_match = sd_bus_slot_unref( s->unlock_match );
    s->sleep_match = sd_bus_slot_unref( s->sleep_match );
    /* not sd_bus_flush_close_unref(): that blocks until the queue is out */
    if (s->bus) sd_bus_close( s->bus );
    s->bus = sd_bus_unref( s->bus );
    release( s );
    s->session[0] = 0;
    s->failed = s->inhibiting = s->sleeping = 0;
    s->subscribed = 0;
    s->hint = -1;
    report( s );
}

static void connect_bus( logind_t* s ) {
    awl_dict_val_t v;
    s->lock_on_suspend = AWL_HOST_CFG_GET( awl_host, "lock_on_suspend", &v ) == AWL_DICT_OK &&
                         v.kind == AWL_DICT_KIND_NUM && v.num;
    const char* id = getenv( "XDG_SESSION_ID" );
    s->retry_at = 0;
    /* the bus handles these in order: the matches are in place before
     * anything is asked */
    int r;
    if ((r = sd_bus_open_system( &s->bus )) < 0 ||
        (r = sd_bus_set_method_call_timeout( s->bus, CALL_TIMEOUT_USEC )) < 0 ||
        (r = sd_bus_add_match_async( s->bus, &s->owner_match,
                "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" LOGIND_NAME "'",
                on_owner_changed, NULL, s )) < 0 ||
        (r = sd_bus_add_match_async( s->bus, &s->sleep_match,
                "type='signal',sender='" LOGIND_NAME "',path='" LOGIND_PATH "',"
                "interface='" LOGIND_MANAGER "',member='PrepareForSleep'",
                on_sleep, on_sleep_subscribed, s )) < 0 ||
        /* "auto": the caller's session, or else the user's graphical one */
        (r = sd_bus_call_method_async( s->bus, NULL, LOGIND_NAME, LOGIND_PATH, LOGIND_MANAGER,
                "GetSession", on_session, s, "s", id && *id ? id : "auto" )) < 0) {
        awl_dbg_printf( s->failures++ ? awl_loglevel_vrb : awl_loglevel_wrn,
                        "logind: can't connect to the system bus: %s\n", strerror( -r ) );
        drop_bus( s );
        s->retry_at = now_usec() + RETRY_USEC;
    }
}

/** handles whatever the bus has; drops a broken connection */
static void bus_update( logind_t* s ) {
    int r;
    while ((r = sd_bus_process( s->bus, NULL )) > 0)
        ;
    if (r < 0 || s->failed) {
        if (r < 0) awl_wrn_printf( "logind: lost the system bus: %s\n", strerror( -r ) );
        drop_bus( s );
        s->retry_at = now_usec() + RETRY_USEC;
        return;
    }
    /* the screen's state, for the inhibitor and the session's LockedHint */
    int covered = atomic_load( &s->covered );
    check_release( s );
    if (s->session[0] && covered != s->hint &&
        sd_bus_call_method_async( s->bus, NULL, LOGIND_NAME, s->session, LOGIND_SESSION,
                                  "SetLockedHint", NULL, NULL, "b", covered ) >= 0)
        s->hint = covered;
    report( s );
}

static int on_suspend( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    /* a refusal (polkit, an inhibitor) isn't the connection's fault */
    const sd_bus_error* e = sd_bus_message_get_error( m );
    if (e) awl_wrn_printf( "logind: Suspend: %s\n", e->message ? e->message : e->name );
    return 0;
}

/** calls Suspend if :c:func:`awl_logind_suspend` asked */
static void check_suspend( logind_t* s ) {
    if (!atomic_exchange( &s->suspend, 0 )) return;
    int r = s->bus ? sd_bus_call_method_async( s->bus, NULL, LOGIND_NAME, LOGIND_PATH, LOGIND_MANAGER,
                                               "Suspend", on_suspend, s, "b", 0 )
                   : -ENOTCONN;
    if (r < 0) awl_wrn_printf( "logind: can't suspend: %s\n", strerror( -r ) );
}

static void* run( void* arg ) {
    logind_t* s = arg;
    connect_bus( s );
    for (;;) {
        uint64_t now = now_usec(), until = s->retry_at ? s->retry_at : UINT64_MAX;
        struct pollfd p[2] = {
            { .fd = s->t.wake_fd, .events = POLLIN },
            { .fd = -1 },
        };
        if (s->bus) {
            uint64_t t;
            p[1].fd = sd_bus_get_fd( s->bus );
            p[1].events = sd_bus_get_events( s->bus );
            if (sd_bus_get_timeout( s->bus, &t ) >= 0 && t < until) until = t;
        }
        int timeout = until == UINT64_MAX ? -1
                    : until <= now ? 0 : (int)((until - now + 999) / 1000);
        if (poll( p, 2, timeout ) < 0 && errno != EINTR) {
            awl_err_printf( "logind: poll: %s\n", strerror( errno ) );
            break;
        }
        if ((p[0].revents & POLLIN) && awl_thread_woken( &s->t )) break;
        if (!s->bus && s->retry_at && now_usec() >= s->retry_at)
            connect_bus( s );
        check_suspend( s );
        if (s->bus)
            bus_update( s );
    }
    drop_bus( s );
    return NULL;
}

int awl_logind_start( void ) {
    if (running) return 0;
    lg.inhibit_fd = -1;
    lg.hint = -1;
    lg.reported = 0;
    if (awl_thread_start( &lg.t, "logind", run, &lg )) return -1;
    running = 1;
    return 0;
}

int awl_logind_stop( void ) {
    if (!running) return 0;
    running = 0;
    return awl_thread_stop( &lg.t, 2000 ) ? 1 : 0;
}

void awl_logind_covered( int covered ) {
    atomic_store( &lg.covered, !!covered );
    if (running) awl_thread_wake( &lg.t );
}

void awl_logind_suspend( void ) {
    if (!running) {
        awl_wrn_printf( "logind: not running, can't suspend\n" );
        return;
    }
    atomic_store( &lg.suspend, 1 );
    awl_thread_wake( &lg.t );
}
