#include "calendar.h"
#include "thread.h"
#include "awl_host.h"

#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** at most this many months queued; the popup asks for one or two */
#define QUEUE_MAX 16

typedef struct {
    awl_thread_t thread;
    pthread_mutex_t lock;
    /** under lock: the months to fetch, and what came back (results is the
     * oldest, last the newest) */
    int queue[QUEUE_MAX];
    int nqueue;
    awl_cal_month_t *results, *last;
    /** the thread's only */
    cal_backend_t* backend;
} worker_t;

static worker_t* W = NULL;

time_t awl_calendar_month_start( int key ) {
    struct tm t = { .tm_year = key / 12 - 1900, .tm_mon = key % 12, .tm_mday = 1, .tm_isdst = -1 };
    return mktime( &t );
}

time_t awl_calendar_day_start( int year, int month, int day ) {
    struct tm t = { .tm_year = year - 1900, .tm_mon = month, .tm_mday = day, .tm_isdst = -1 };
    return mktime( &t );
}

void awl_calendar_month_free( awl_cal_month_t* m ) {
    if (!m) return;
    for (int i = 0; i < m->n; i++) {
        free( m->events[i].summary );
        free( m->events[i].location );
        free( m->events[i].color );
    }
    free( m->events );
    free( m );
}

static void log_backend( int warn, const char* msg ) {
    if (warn) awl_wrn_printf( "calendar: %s\n", msg );
    else awl_log_printf( "calendar: %s\n", msg );
}

static char* dup( const char* s ) {
    return strdup( s ? s : "" );
}

/** cal_backend_query()'s callback: appends e to the month */
static void collect( const cal_event_t* e, void* data ) {
    awl_cal_month_t* m = data;
    awl_cal_event_t* ev = realloc( m->events, (size_t)(m->n + 1) * sizeof(*ev) );
    if (!ev) return;
    m->events = ev;
    ev[m->n++] = (awl_cal_event_t){ dup( e->summary ), dup( e->location ), dup( e->color ),
                                    e->start, e->end, e->all_day };
}

static int by_start( const void* a, const void* b ) {
    const awl_cal_event_t *x = a, *y = b;
    if (x->all_day != y->all_day) return y->all_day - x->all_day;
    return (x->start > y->start) - (x->start < y->start);
}

static void* worker( void* arg ) {
    worker_t* w = arg;
    for (;;) {
        struct pollfd p = { .fd = w->thread.wake_fd, .events = POLLIN };
        if (poll( &p, 1, -1 ) < 0) continue;
        if ((p.revents & POLLIN) && awl_thread_woken( &w->thread )) break;
        for (;;) {
            pthread_mutex_lock( &w->lock );
            int key = -1;
            if (w->nqueue) {
                key = w->queue[0];
                memmove( w->queue, w->queue + 1, (size_t)--w->nqueue * sizeof(int) );
            }
            pthread_mutex_unlock( &w->lock );
            if (key < 0 || atomic_load( &w->thread.quit )) break;

            awl_cal_month_t* m = calloc( 1, sizeof(*m) );
            if (!m) break;
            m->key = key;
            m->status = w->backend ? cal_backend_query( w->backend, awl_calendar_month_start( key ),
                                                        awl_calendar_month_start( key + 1 ), collect, m )
                                   : CAL_ERR_NOLIB;
            if (m->n) qsort( m->events, (size_t)m->n, sizeof(*m->events), by_start );
            pthread_mutex_lock( &w->lock );
            if (w->last) w->last->next = m;
            else w->results = m;
            w->last = m;
            pthread_mutex_unlock( &w->lock );
            awl_host->redraw_request();
        }
    }
    return NULL;
}

int awl_calendar_start( void ) {
    if (W) return 0;
    worker_t* w = calloc( 1, sizeof(*w) );
    if (!w) return -1;
    pthread_mutex_init( &w->lock, NULL );
    w->backend = cal_backend_open( log_backend );
    if (awl_thread_start( &w->thread, "calendar", worker, w )) {
        cal_backend_close( w->backend );
        pthread_mutex_destroy( &w->lock );
        free( w );
        return -1;
    }
    W = w;
    return 0;
}

int awl_calendar_stop( void ) {
    worker_t* w = W;
    if (!w) return 0;
    W = NULL;
    /* a query blocked on D-Bus returns once cancelled */
    if (w->backend) cal_backend_cancel( w->backend );
    if (awl_thread_stop( &w->thread, 1000 )) return 1;
    cal_backend_close( w->backend );
    for (awl_cal_month_t* m = w->results; m;) {
        awl_cal_month_t* next = m->next;
        awl_calendar_month_free( m );
        m = next;
    }
    pthread_mutex_destroy( &w->lock );
    free( w );
    return 0;
}

void awl_calendar_request( int key ) {
    worker_t* w = W;
    if (!w || key < 0) return;
    pthread_mutex_lock( &w->lock );
    int queued = 0;
    for (int i = 0; i < w->nqueue; i++) queued |= w->queue[i] == key;
    if (!queued && w->nqueue < QUEUE_MAX) w->queue[w->nqueue++] = key;
    pthread_mutex_unlock( &w->lock );
    awl_thread_wake( &w->thread );
}

awl_cal_month_t* awl_calendar_take( void ) {
    worker_t* w = W;
    if (!w) return NULL;
    pthread_mutex_lock( &w->lock );
    awl_cal_month_t* m = w->results;
    w->results = w->last = NULL;
    pthread_mutex_unlock( &w->lock );
    return m;
}
