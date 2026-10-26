#pragma once

#include <stdatomic.h>
#include <pulse/mainloop-api.h>

/** the PulseAudio connection, on the poller thread; opaque */
typedef struct PulseAudio PulseAudio;

/** :c:member:`pulse_test_t.cause`: what made a change, as far as awl can
 * tell; PulseAudio doesn't say who changed a volume */
typedef enum {
    /** the first reading after a (re)connection */
    PULSE_CAUSE_CONNECT,
    /** a change to the same sink on the same port: a mixer, another
     * program, or our own asks (which only the asker knows) */
    PULSE_CAUSE_OTHER,
    /** the default sink switched to another one */
    PULSE_CAUSE_SINK,
    /** the default sink switched its port (headphones plugged, say) */
    PULSE_CAUSE_PORT,
} pulse_cause_t;

/** The default sink's volume. */
typedef struct pulse_test_t {
    _Atomic float value;
    atomic_int muted;
    /** whether the speaker sink (:c:func:`pulse_init`) is muted; 0 without one */
    atomic_int speaker_muted;
    /** what made the last change to them, a :c:type:`pulse_cause_t`; set
     * before them */
    atomic_int cause;
    /** counts the default sink's switches (:c:enumerator:`PULSE_CAUSE_SINK`,
     * :c:enumerator:`PULSE_CAUSE_PORT`), news even if they leave the volume
     * as it was; switch_cause is the last one's cause */
    atomic_int switches, switch_cause;

    /** requests for the loop thread, from any thread (pulsetest.c) */
    atomic_int requests;
    /** the volume changes asked for and not yet made, in per mille */
    atomic_int volume_delta;
    /** an eventfd that wakes the loop for them */
    int wake_fd;

    /** loop thread only */
    PulseAudio* pa;
} pulse_test_t;

/** Tracks the default sink on the loop behind api (poller.h), and the sink
 * named speaker's mute state (NULL or "": none); max is the volume
 * :c:func:`pulse_volume_adjust` goes up to (1 is 100%). NULL on failure. */
pulse_test_t* pulse_init( pa_mainloop_api* api, float max, const char* speaker );
/** Only once the loop thread has stopped. No-op for NULL. */
void pulse_free( pulse_test_t* p );

/** Changes the default sink's volume by delta (1 is 100%), keeping its
 * balance; up no further than :c:func:`pulse_init`'s max, or than it is if that's
 * more. Any
 * thread; returns at once, the bar follows by the sink's events. Dropped
 * while there is no server or no default sink. No-op for NULL. */
void pulse_volume_adjust( pulse_test_t* p, float delta );
/** Mutes the default sink, or unmutes it; likewise. */
void pulse_mute_toggle( pulse_test_t* p );
/** Mutes the default source (the microphone), or unmutes it; likewise. */
void pulse_mic_mute_toggle( pulse_test_t* p );
/** Mutes the speaker sink (:c:func:`pulse_init`), or unmutes it; likewise, and a
 * no-op without one. */
void pulse_speaker_mute_toggle( pulse_test_t* p );
