#pragma once

#include <stdatomic.h>
#include <pulse/mainloop-api.h>

typedef struct awl_backlight_bus_t awl_backlight_bus_t;

/** awl_backlight_t.screen and .webcam: not known (no daemon, or no
 * reading yet) */
#define BACKLIGHT_UNKNOWN (-1)
/** awl_backlight_t.screen and .webcam: the daemon reported something that
 * isn't a brightness (NaN, out of [0,1]) */
#define BACKLIGHT_INVALID (-2)

/** The backlight-tooler daemon, as seen on the user bus (backlight.c). */
typedef struct awl_backlight_t {
    /** whether the daemon runs (owns ``org.BacklightTooler``) */
    atomic_int running;
    /** whether it runs and updates automatically (``Interval`` > 0) */
    atomic_int enabled;
    /** screen brightness (``ScreenBrightness``) in per mille, or
     * BACKLIGHT_UNKNOWN / BACKLIGHT_INVALID */
    atomic_int screen;
    /** last webcam reading (``WebcamValue``) in per mille, likewise */
    atomic_int webcam;
    /** requests for the loop thread, from any thread (backlight.c) */
    atomic_int requests;
    /** the brightness changes asked for and not yet sent, in per mille */
    atomic_int adjust;
    /** an eventfd that wakes the loop for them */
    int wake_fd;
    /** loop thread only */
    awl_backlight_bus_t* bus;
} awl_backlight_t;

/** Tracks the daemon on the loop behind api (poller.h); NULL on failure. */
awl_backlight_t* backlight_init( pa_mainloop_api* api );
/** Only once the loop thread has stopped. No-op for NULL. */
void backlight_free( awl_backlight_t* b );

/** Switches the daemon's automatic updates off if they are on, and on (at
 * its ``ConfigInterval``, with an ``Update()`` right away) if they are off;
 * starts the daemon's user unit if
 * it isn't running. Any thread; returns at once, the bar follows by its
 * signals. No-op for NULL. */
void backlight_toggle( awl_backlight_t* b );
/** Has the daemon measure and set the brightness now (``Update()``); starts
 * its user unit if it isn't running, which updates once on start. Any
 * thread, returns at once. No-op for NULL. */
void backlight_update( awl_backlight_t* b );
/** Has the daemon change the screen brightness by delta (1 is all of it,
 * ``Adjust()``); starts its user unit if it isn't running and sends it once
 * the daemon is there. Any thread, returns at once. No-op for NULL. */
void backlight_adjust( awl_backlight_t* b, float delta );
