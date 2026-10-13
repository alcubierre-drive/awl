/*#include "../awl_log.h"*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pulse/pulseaudio.h>
#include "pulsetest.h"
#include "../awl_host.h"

/* Tracks the default sink's volume/mute through PulseAudio subscriptions,
 * on the plugins' shared loop (poller.h), which only wakes for server events
 * (sink changes, default sink changes). Everything but value/muted is only
 * touched on the loop thread, or before it starts and after it has stopped.
 * The first connection is made from the loop too. If the server goes away
 * (or isn't there yet), the last value is kept and a new connection is
 * attempted every RETRY_USEC; no timer runs while connected. */

#define RETRY_USEC (3 * PA_USEC_PER_SEC)

struct PulseAudio {
    pa_mainloop_api* _mainloop_api;
    pa_context* _context;
    pa_time_event* _retry;
    char name[256]; // the default sink's

    pulse_test_t* t;
};

static void retry_callback( pa_mainloop_api* a, pa_time_event* e, const struct timeval* tv, void* userdata );
static void context_state_callback( pa_context* c, void* userdata );
static void subscribe_callback( pa_context* c, pa_subscription_event_type_t type, uint32_t idx, void* userdata );
static void sink_info_callback( pa_context* c, const pa_sink_info* i, int eol, void* userdata );
static void server_info_callback( pa_context* c, const pa_server_info* i, void* userdata );

static void PulseAudio_connect( PulseAudio* p );
static void PulseAudio_drop_context( PulseAudio* p );
static void PulseAudio_schedule_retry( PulseAudio* p, pa_usec_t usec );

pulse_test_t* pulse_init( pa_mainloop_api* api ) {
    pulse_test_t* p = calloc(1, sizeof(pulse_test_t));
    if (!p) return NULL;
    p->pa = calloc(1, sizeof(PulseAudio));
    if (!p->pa) {
        free( p );
        return NULL;
    }
    p->pa->_mainloop_api = api;
    p->pa->t = p;
    atomic_init( &p->value, 0 );
    atomic_init( &p->muted, 0 );
    PulseAudio_schedule_retry( p->pa, 0 );
    if (!p->pa->_retry) {
        free( p->pa );
        free( p );
        return NULL;
    }
    return p;
}

void pulse_free( pulse_test_t* p ) {
    if (!p) return;
    PulseAudio_drop_context( p->pa );
    if (p->pa->_retry) p->pa->_mainloop_api->time_free( p->pa->_retry );
    free( p->pa );
    free( p );
}

static void retry_callback( pa_mainloop_api* a, pa_time_event* e, const struct timeval* tv, void* userdata ) {
    (void)tv;
    PulseAudio* pa = (PulseAudio*)userdata;
    a->time_free( e );
    pa->_retry = NULL;
    PulseAudio_connect( pa );
}

static void context_state_callback( pa_context* c, void* userdata ) {
    PulseAudio* pa = (PulseAudio*)userdata;
    pa_operation* op = NULL;
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
        case PA_CONTEXT_READY:
            /*P_awl_log_printf( "pulse connection established.." );*/
            pa_context_set_subscribe_callback( c, subscribe_callback, userdata );
            op = pa_context_subscribe( c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL );
            if (op) pa_operation_unref( op );
            op = pa_context_get_server_info( c, server_info_callback, userdata );
            if (op) pa_operation_unref( op );
            break;
        case PA_CONTEXT_TERMINATED:
        case PA_CONTEXT_FAILED:
        default:
            /*P_awl_err_printf( "pulse connection lost: %s", pa_strerror(pa_context_errno(c)) );*/
            // keep the last value; reconnect later
            PulseAudio_drop_context( pa );
            PulseAudio_schedule_retry( pa, RETRY_USEC );
            break;
    }
}

static void subscribe_callback( pa_context* c, pa_subscription_event_type_t type, uint32_t idx, void* userdata ) {
    unsigned facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    pa_operation* op = NULL;
    switch (facility) {
        case PA_SUBSCRIPTION_EVENT_SINK:
            op = pa_context_get_sink_info_by_index( c, idx, sink_info_callback, userdata );
            break;
        case PA_SUBSCRIPTION_EVENT_SERVER:
            // the default sink may have changed
            op = pa_context_get_server_info( c, server_info_callback, userdata );
            break;
        default:
            break;
    }
    if (op) pa_operation_unref( op );
}

static void sink_info_callback( pa_context* c, const pa_sink_info* i, int eol, void* userdata ) {
    (void)c;
    (void)eol;

    PulseAudio* pa = userdata;
    pulse_test_t* t = pa->t;
    if (i && i->name && !strcmp( i->name, pa->name )) {
        float value = (float)pa_cvolume_avg(&(i->volume)) / (float)PA_VOLUME_NORM;
        int muted = i->mute;
        int changed = atomic_exchange( &t->value, value ) != value;
        changed |= atomic_exchange( &t->muted, muted ) != muted;
        if (changed) awl_host->redraw_request();
    }
}

static void server_info_callback( pa_context* c, const pa_server_info* i, void* userdata ) {
    PulseAudio* pa = userdata;
    // i is NULL on failure, default_sink_name is NULL when there is no sink
    const char* name = (i && i->default_sink_name) ? i->default_sink_name : "";
    /*P_awl_log_printf( "pulse sink name = %s", name );*/
    int same = !strcmp( pa->name, name );
    if (!same) snprintf( pa->name, sizeof(pa->name), "%s", name );
    if (same || !name[0]) return;
    pa_operation* op = pa_context_get_sink_info_by_name( c, name, sink_info_callback, userdata );
    if (op) pa_operation_unref( op );
}

/* Creates a context and starts connecting; on failure a retry is scheduled.
 * Loop thread only. */
static void PulseAudio_connect( PulseAudio* p ) {
    p->_context = pa_context_new( p->_mainloop_api, "awl volume" );
    if (!p->_context) {
        /*P_awl_err_printf( "pulse pa_context_new() failed." );*/
        PulseAudio_schedule_retry( p, RETRY_USEC );
        return;
    }
    pa_context_set_state_callback( p->_context, context_state_callback, p );
    if (pa_context_connect( p->_context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL ) < 0) {
        /*P_awl_err_printf( "pulse pa_context_connect() failed: %s", pa_strerror(pa_context_errno(p->_context)));*/
        PulseAudio_drop_context( p );
        PulseAudio_schedule_retry( p, RETRY_USEC );
    }
}

static void PulseAudio_drop_context( PulseAudio* p ) {
    if (!p->_context) return;
    pa_context* c = p->_context;
    p->_context = NULL;
    pa_context_set_state_callback( c, NULL, NULL );
    pa_context_set_subscribe_callback( c, NULL, NULL );
    pa_context_disconnect( c );
    pa_context_unref( c );
    // force a fresh sink lookup after reconnecting
    p->name[0] = 0;
}

static void PulseAudio_schedule_retry( PulseAudio* p, pa_usec_t usec ) {
    if (p->_retry) return;
    struct timeval tv;
    pa_timeval_add( pa_gettimeofday( &tv ), usec );
    p->_retry = p->_mainloop_api->time_new( p->_mainloop_api, &tv, retry_callback, p );
}
