#include "poller.h"
#include "../awl_host.h"
#include "thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <pulse/mainloop.h>

/** Lets the kernel delay the 1 s tick by this much to batch it with other
 * wakeups; nobody sees the clock or the graph being 20 ms late. */
#define TIMER_SLACK_NS 20000000
/** Safety nets, run on the 1 s tick so they never cause a wakeup of their
 * own: some firmware doesn't send a uevent for every capacity step, and
 * without the netlink socket the IP address would never update. */
#define BAT_REREAD_TICKS 60
/** After any kernel uevent, the battery is re-read on this many more ticks:
 * the events of plugging in come first, and the firmware moves the battery
 * from "Discharging" over "Not charging" to "Charging" about 2 s later,
 * without an event of its own. */
#define BAT_SETTLE_TICKS 10
#define IP_FALLBACK_TICKS 5
/** How long :c:func:`poller_stop` waits for the thread, which may hang in a sysfs
 * read (an ACPI thermal zone goes through the EC); like logind's. */
#define STOP_TIMEOUT_MS 2000

struct awl_poller_t {
    awl_stats_t* stats;
    awl_temperature_t* temp;
    awl_date_t* date;
    awl_battery_t* bat;
    awl_ipaddr_t* ip;

    awl_thread_t thread;
    int started;
    atomic_int paused;

    /* loop thread only (once started) */
    pa_mainloop* ml;
    pa_mainloop_api* api;
    pa_io_event *wake, *bat_io, *ip_io;
    pa_time_event* tick; /* NULL while paused */
    int fresh;           /* the next tick re-reads everything */
    int bat_settle;      /* ticks left that re-read the battery */
    unsigned long ticks;
};

static void update_all( awl_poller_t* p ) {
    stats_update( p->stats );
    temp_update( p->temp );
    date_update( p->date );
    bat_update( p->bat );
    bat_dock_update( p->bat );
    if (p->ip) ip_update( p->ip );
    awl_host->redraw_request();
}

static void tick_callback( pa_mainloop_api* a, pa_time_event* e, const struct timeval* tv, void* userdata ) {
    (void)tv;
    awl_poller_t* p = userdata;
    if (p->fresh) {
        p->fresh = 0;
        update_all( p );
    } else {
        p->ticks++;
        stats_update( p->stats );
        temp_update( p->temp );
        date_update( p->date );
        if (p->bat_settle > 0) {
            p->bat_settle--;
            bat_update( p->bat );
        } else if (p->ticks % BAT_REREAD_TICKS == 0) {
            bat_update( p->bat );
            if (p->bat->uevent_fd < 0) bat_dock_update( p->bat );
        }
        if (p->ip && p->ip->nl_fd < 0 && p->ticks % IP_FALLBACK_TICKS == 0)
            ip_update( p->ip );
        /* the graph moves every second */
        awl_host->redraw_request();
    }
    /* the next full wall-clock second */
    struct timeval next;
    gettimeofday( &next, NULL );
    next.tv_sec++;
    next.tv_usec = 0;
    a->time_restart( e, &next );
}

/** starts or stops the tick to match p->paused */
static void sync_paused( awl_poller_t* p ) {
    int paused = atomic_load( &p->paused );
    if (paused && p->tick) {
        p->api->time_free( p->tick );
        p->tick = NULL;
    } else if (!paused && !p->tick) {
        /* the bar is visible again: catch up on everything at once */
        struct timeval now;
        gettimeofday( &now, NULL );
        p->fresh = 1;
        p->tick = p->api->time_new( p->api, &now, tick_callback, p );
    }
}

static void wake_callback( pa_mainloop_api* a, pa_io_event* e, int fd, pa_io_event_flags_t f, void* userdata ) {
    (void)e; (void)fd; (void)f;
    awl_poller_t* p = userdata;
    if (awl_thread_woken( &p->thread ))
        a->quit( a, 0 );
    else
        sync_paused( p );
}

static void bat_callback( pa_mainloop_api* a, pa_io_event* e, int fd, pa_io_event_flags_t f, void* userdata ) {
    (void)a; (void)e; (void)fd; (void)f;
    awl_poller_t* p = userdata;
    if (bat_dispatch( p->bat ) && !atomic_load( &p->paused )) awl_host->redraw_request();
    p->bat_settle = BAT_SETTLE_TICKS;
}

static void ip_callback( pa_mainloop_api* a, pa_io_event* e, int fd, pa_io_event_flags_t f, void* userdata ) {
    (void)a; (void)e; (void)fd; (void)f;
    awl_poller_t* p = userdata;
    if (ip_dispatch( p->ip ) && !atomic_load( &p->paused )) awl_host->redraw_request();
}

/* NetworkManager's view changed: it may pick another address */
static void nm_changed( void* userdata ) {
    awl_poller_t* p = userdata;
    if (ip_update( p->ip ) && !atomic_load( &p->paused )) awl_host->redraw_request();
}

static void* poller_run( void* arg ) {
    awl_poller_t* p = arg;
    prctl( PR_SET_TIMERSLACK, TIMER_SLACK_NS, 0, 0, 0 );
    p->wake = p->api->io_new( p->api, p->thread.wake_fd, PA_IO_EVENT_INPUT, wake_callback, p );
    if (!p->wake) return NULL;
    sync_paused( p );
    pa_mainloop_run( p->ml, NULL );
    return NULL;
}

awl_poller_t* poller_new( awl_stats_t* stats, awl_temperature_t* temp, awl_date_t* date,
                          awl_battery_t* bat, awl_ipaddr_t* ip ) {
    awl_poller_t* p = calloc(1, sizeof(awl_poller_t));
    if (!p) return NULL;
    p->stats = stats;
    p->temp = temp;
    p->date = date;
    p->bat = bat;
    p->ip = ip;
    atomic_init( &p->paused, 0 );
    if (!(p->ml = pa_mainloop_new())) {
        free( p );
        return NULL;
    }
    p->api = pa_mainloop_get_api( p->ml );
    if (bat->uevent_fd >= 0)
        p->bat_io = p->api->io_new( p->api, bat->uevent_fd, PA_IO_EVENT_INPUT, bat_callback, p );
    /* without ip (out of memory) no addresses, nor NetworkManager */
    if (ip && ip->nl_fd >= 0)
        p->ip_io = p->api->io_new( p->api, ip->nl_fd, PA_IO_EVENT_INPUT, ip_callback, p );
    if (ip) ip->nm = nm_new( p->api, nm_changed, p );
    return p;
}

pa_mainloop_api* poller_api( awl_poller_t* p ) {
    return p->api;
}

int poller_start( awl_poller_t* p ) {
    if (awl_thread_start( &p->thread, "poller", poller_run, p )) return -1;
    p->started = 1;
    return 0;
}

void poller_set_paused( awl_poller_t* p, int paused ) {
    if (!p) return;
    if (atomic_exchange( &p->paused, !!paused ) == !!paused) return;
    if (p->started) awl_thread_wake( &p->thread );
}

int poller_stop( awl_poller_t* p ) {
    if (!p || !p->started) return 0;
    if (awl_thread_stop( &p->thread, STOP_TIMEOUT_MS )) return 1;
    p->started = 0;
    return 0;
}

void poller_free( awl_poller_t* p ) {
    if (!p) return;
    /* its event sources first; the loop frees whatever is left */
    if (p->ip) {
        nm_free( p->ip->nm );
        p->ip->nm = NULL;
    }
    pa_mainloop_free( p->ml );
    free( p );
}
