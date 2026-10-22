/*#include "../awl_log.h"*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <pulse/pulseaudio.h>
#include "pulsetest.h"
#include "../awl_host.h"

/* Tracks the default sink's volume/mute through PulseAudio subscriptions,
 * on the plugins' shared loop (poller.h), which only wakes for server events
 * (sink changes, default sink changes), and the speaker sink's mute state
 * likewise. Everything but value/muted is only
 * touched on the loop thread, or before it starts and after it has stopped.
 * The first connection is made from the loop too. If the server goes away
 * (or isn't there yet), the last value is kept and a new connection is
 * attempted every RETRY_USEC; no timer runs while connected. Volume and
 * mute changes (pulse_volume_adjust() and the toggles) go out on the same
 * context, from the loop: other threads only leave them in the atomics and
 * wake it through wake_fd. */

#define RETRY_USEC (3 * PA_USEC_PER_SEC)

/** pulse_test_t.requests; a second toggle before the loop got to the first
 * one cancels it */
enum { REQ_MUTE = 1, REQ_MIC_MUTE = 2, REQ_SPEAKER_MUTE = 4 };

struct PulseAudio {
    pa_mainloop_api* _mainloop_api;
    pa_context* _context;
    pa_time_event* _retry;
    pa_io_event* _wake; // pulse_test_t.wake_fd
    char name[256]; // the default sink's
    char speaker[256]; // pulse_init()'s, "" if none
    pa_volume_t max; // pulse_init()'s
    // the default sink's as last seen, or as last set
    pa_cvolume volume;
    int volume_known, muted;

    pulse_test_t* t;
};

static void retry_callback( pa_mainloop_api* a, pa_time_event* e, const struct timeval* tv, void* userdata );
static void context_state_callback( pa_context* c, void* userdata );
static void subscribe_callback( pa_context* c, pa_subscription_event_type_t type, uint32_t idx, void* userdata );
static void sink_info_callback( pa_context* c, const pa_sink_info* i, int eol, void* userdata );
static void server_info_callback( pa_context* c, const pa_server_info* i, void* userdata );
static void wake_callback( pa_mainloop_api* a, pa_io_event* e, int fd, pa_io_event_flags_t f, void* userdata );

static void PulseAudio_connect( PulseAudio* p );
static void PulseAudio_drop_context( PulseAudio* p );
static void PulseAudio_schedule_retry( PulseAudio* p, pa_usec_t usec );

pulse_test_t* pulse_init( pa_mainloop_api* api, float max, const char* speaker ) {
    pulse_test_t* p = calloc(1, sizeof(pulse_test_t));
    if (!p) return NULL;
    p->pa = calloc(1, sizeof(PulseAudio));
    if (!p->pa) {
        free( p );
        return NULL;
    }
    p->pa->_mainloop_api = api;
    p->pa->t = p;
    double v = (double)PA_VOLUME_NORM * (max > 0 ? max : 1.0) + 0.5;
    p->pa->max = v > (double)PA_VOLUME_MAX ? PA_VOLUME_MAX : (pa_volume_t)v;
    snprintf( p->pa->speaker, sizeof(p->pa->speaker), "%s", speaker ? speaker : "" );
    atomic_init( &p->value, 0 );
    atomic_init( &p->muted, 0 );
    atomic_init( &p->speaker_muted, 0 );
    atomic_init( &p->requests, 0 );
    atomic_init( &p->volume_delta, 0 );
    p->wake_fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    if (p->wake_fd < 0 ||
        !(p->pa->_wake = api->io_new( api, p->wake_fd, PA_IO_EVENT_INPUT, wake_callback, p->pa )))
        goto fail;
    PulseAudio_schedule_retry( p->pa, 0 );
    if (!p->pa->_retry) goto fail;
    return p;
fail:
    if (p->pa->_wake) api->io_free( p->pa->_wake );
    if (p->wake_fd >= 0) close( p->wake_fd );
    free( p->pa );
    free( p );
    return NULL;
}

void pulse_free( pulse_test_t* p ) {
    if (!p) return;
    PulseAudio_drop_context( p->pa );
    if (p->pa->_retry) p->pa->_mainloop_api->time_free( p->pa->_retry );
    p->pa->_mainloop_api->io_free( p->pa->_wake );
    close( p->wake_fd );
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
            if (pa->speaker[0]) {
                op = pa_context_get_sink_info_by_name( c, pa->speaker, sink_info_callback, userdata );
                if (op) pa_operation_unref( op );
            }
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
    if (i && i->name && pa->speaker[0] && !strcmp( i->name, pa->speaker ) &&
        atomic_exchange( &t->speaker_muted, !!i->mute ) != !!i->mute)
        awl_host->redraw_request();
    if (i && i->name && !strcmp( i->name, pa->name )) {
        float value = (float)pa_cvolume_avg(&(i->volume)) / (float)PA_VOLUME_NORM;
        int muted = i->mute;
        pa->volume = i->volume;
        pa->volume_known = 1;
        pa->muted = muted;
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
    if (!same) {
        snprintf( pa->name, sizeof(pa->name), "%s", name );
        pa->volume_known = 0;
    }
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
    p->volume_known = 0;
}

static void PulseAudio_schedule_retry( PulseAudio* p, pa_usec_t usec ) {
    if (p->_retry) return;
    struct timeval tv;
    pa_timeval_add( pa_gettimeofday( &tv ), usec );
    p->_retry = p->_mainloop_api->time_new( p->_mainloop_api, &tv, retry_callback, p );
}

/* the speaker sink as it is now: muted the other way */
static void speaker_mute_callback( pa_context* c, const pa_sink_info* i, int eol, void* userdata ) {
    (void)eol; (void)userdata;
    if (!i) return;
    pa_operation* op = pa_context_set_sink_mute_by_index( c, i->index, !i->mute, NULL, NULL );
    if (op) pa_operation_unref( op );
}

/* the default source as it is now: muted the other way */
static void source_mute_callback( pa_context* c, const pa_source_info* i, int eol, void* userdata ) {
    (void)eol; (void)userdata;
    if (!i) return;
    pa_operation* op = pa_context_set_source_mute_by_index( c, i->index, !i->mute, NULL, NULL );
    if (op) pa_operation_unref( op );
}

static void set_volume( PulseAudio* pa, int permille ) {
    pa_cvolume cv = pa->volume;
    pa_volume_t step = (pa_volume_t)((double)PA_VOLUME_NORM * abs( permille ) / 1000.0 + 0.5);
    if (permille > 0) {
        pa_volume_t max = pa_cvolume_max( &cv );
        if (!pa_cvolume_inc_clamp( &cv, step, max > pa->max ? max : pa->max )) return;
    } else if (!pa_cvolume_dec( &cv, step )) {
        return;
    }
    pa_operation* op = pa_context_set_sink_volume_by_name( pa->_context, pa->name, &cv, NULL, NULL );
    if (!op) return;
    pa_operation_unref( op );
    // so that the next change adds to this one even before the sink's event
    pa->volume = cv;
}

static void wake_callback( pa_mainloop_api* a, pa_io_event* e, int fd, pa_io_event_flags_t f, void* userdata ) {
    (void)a; (void)e; (void)f;
    PulseAudio* pa = userdata;
    uint64_t count;
    if (read( fd, &count, sizeof count ) < 0) {}
    int req = atomic_exchange( &pa->t->requests, 0 );
    int delta = atomic_exchange( &pa->t->volume_delta, 0 );
    if (!pa->_context || pa_context_get_state( pa->_context ) != PA_CONTEXT_READY) return;
    pa_operation* op;
    if (req & REQ_MIC_MUTE) {
        op = pa_context_get_source_info_by_name( pa->_context, "@DEFAULT_SOURCE@", source_mute_callback, pa );
        if (op) pa_operation_unref( op );
    }
    if ((req & REQ_SPEAKER_MUTE) && pa->speaker[0]) {
        op = pa_context_get_sink_info_by_name( pa->_context, pa->speaker, speaker_mute_callback, pa );
        if (op) pa_operation_unref( op );
    }
    // the sink's, only once it is known
    if (!pa->name[0] || !pa->volume_known) return;
    if (req & REQ_MUTE) {
        op = pa_context_set_sink_mute_by_name( pa->_context, pa->name, !pa->muted, NULL, NULL );
        if (op) {
            pa_operation_unref( op );
            pa->muted = !pa->muted;
        }
    }
    if (delta) set_volume( pa, delta );
}

static void request( pulse_test_t* p, int req, int delta ) {
    if (!p) return;
    if (req) atomic_fetch_xor( &p->requests, req );
    if (delta) atomic_fetch_add( &p->volume_delta, delta );
    uint64_t one = 1;
    /* EAGAIN only if the counter would overflow: the loop is due anyway */
    if (write( p->wake_fd, &one, sizeof one ) < 0) {}
}

void pulse_volume_adjust( pulse_test_t* p, float delta ) {
    if (!(delta >= -1.0f && delta <= 1.0f)) return;
    request( p, 0, (int)lroundf( delta * 1000.0f ) );
}

void pulse_mute_toggle( pulse_test_t* p ) {
    request( p, REQ_MUTE, 0 );
}

void pulse_mic_mute_toggle( pulse_test_t* p ) {
    request( p, REQ_MIC_MUTE, 0 );
}

void pulse_speaker_mute_toggle( pulse_test_t* p ) {
    request( p, REQ_SPEAKER_MUTE, 0 );
}
