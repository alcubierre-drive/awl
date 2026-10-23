/* The volume's and brightness's notifications, see osd.h. Main thread only. */

#include "osd.h"
#include "../plugins.h"

#include <math.h>
#include <string.h>
#include <time.h>

#define CFG (&awl_plugin_config()->osd)

/** ms: how long it waits for a change before it sends the value as it
 * is; how long after the last ask a change counts as its answer */
enum { QUIET_MS = 150, WATCH_MS = 1500 };

/** what a notification shows */
typedef enum { OSD_VOLUME, OSD_BRIGHTNESS, OSD_N } kind_t;

/** a value's watch */
typedef struct {
    /** when a change was last asked for, ms; 0: never */
    long long asked;
    /** nothing sent since */
    int due;
    /** the state last sent (:c:func:`state`), or found when the watch began */
    long long shown;
} watch_t;

static struct {
    awl_timer_t* timer;
    watch_t w[OSD_N];
} O;

static long long now_ms( void ) {
    struct timespec t;
    clock_gettime( CLOCK_MONOTONIC, &t );
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/** k's value and what goes with it in one number; -1 not known */
static long long state( kind_t k ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return -1;
    if (k == OSD_VOLUME) {
        if (!P->pulse) return -1;
        const long long v = lroundf( atomic_load( &P->pulse->value ) * 1000.0f );
        return v * 4 + 2 * !!atomic_load( &P->pulse->muted ) + !!atomic_load( &P->pulse->speaker_muted );
    }
    if (!P->backlight) return -1;
    const int b = atomic_load( &P->backlight->screen );
    return b < 0 ? -1 : b;
}

/** 0 to 100 */
static int percent( float f ) {
    const long v = lroundf( f * 100.0f );
    return v < 0 ? 0 : v > 100 ? 100 : (int)v;
}

/** k's notification for state s (known) */
static void send( kind_t k, long long s ) {
    const awl_plugin_config_t* pc = awl_plugin_config();
    awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
    n.urgency = AWL_NOTIF_LOW;
    n.flags = AWL_NOTIF_OSD;
    n.timeout = CFG->timeout;
    n.look.width = CFG->width;
    n.look.image_size = CFG->image_size;
    /* the low urgency's frame is too dim for the bar's fill: the text's */
    n.look.progress = pc->colors.notify_progress ? pc->colors.notify_progress
                    : pc->colors.notify_low_fg ? pc->colors.notify_low_fg : awl_config()->colors.popup_fg;
    if (k == OSD_VOLUME) {
        const float v = (float)(s / 4) / 1000.0f, max = pc->volume.max;
        const int muted = (int)(s & 3);
        n.tag = "osd-volume";
        n.image = muted || v <= 0 ? pc->osd.volume_icons.muted
                : v <= 1.0f / 3 ? pc->osd.volume_icons.low
                : v <= 2.0f / 3 ? pc->osd.volume_icons.medium
                : v <= 1 ? pc->osd.volume_icons.high : pc->osd.volume_icons.over;
        if (v <= 1) {
            n.value = percent( v );
        } else {
            /* above 100% the bar starts over, up to max */
            n.value = max > 1 ? percent( (v - 1) / (max - 1) ) : 100;
            n.look.progress = pc->colors.bad;
        }
    } else {
        const float b = (float)s / 1000.0f;
        n.tag = "osd-brightness";
        n.image = b <= 0 ? pc->osd.brightness_icons.off
                : b <= 1.0f / 3 ? pc->osd.brightness_icons.low
                : b <= 2.0f / 3 ? pc->osd.brightness_icons.medium : pc->osd.brightness_icons.high;
        n.value = percent( b );
    }
    if (n.image && !*n.image) n.image = NULL;
    awl_notify_send( &n );
}

/** whether w's last ask is recent enough for what changes to be its answer */
static int watching( const watch_t* w, long long now ) {
    return w->asked && now - w->asked < WATCH_MS;
}

void awl_osd_redraw( void ) {
    const long long now = now_ms();
    for (int k = 0; k < OSD_N; k++) {
        watch_t* w = &O.w[k];
        if (!watching( w, now )) continue;
        const long long s = state( (kind_t)k );
        if (s < 0 || s == w->shown) continue;
        send( (kind_t)k, s );
        w->shown = s;
        w->due = 0;
    }
}

/** no change came in QUIET_MS: what is asked for goes out as it is (at its
 * limit, say); not known yet, it waits for :c:func:`awl_osd_redraw` */
static void quiet( void* data ) {
    (void)data;
    for (int k = 0; k < OSD_N; k++) {
        watch_t* w = &O.w[k];
        const long long s = state( (kind_t)k );
        if (!w->due || s < 0) continue;
        send( (kind_t)k, s );
        w->shown = s;
        w->due = 0;
    }
}

static void ask( kind_t k ) {
    watch_t* w = &O.w[k];
    const long long now = now_ms();
    if (!O.timer) return;
    /* what it is now isn't news; what it changes to is */
    if (!watching( w, now )) w->shown = state( k );
    w->asked = now;
    w->due = 1;
    awl_host->timer_set( O.timer, QUIET_MS );
}

void awl_osd_volume( void ) {
    ask( OSD_VOLUME );
}

void awl_osd_brightness( void ) {
    ask( OSD_BRIGHTNESS );
}

int awl_osd_start( void ) {
    memset( &O, 0, sizeof O );
    O.timer = awl_host->timer_add( quiet, NULL );
    return 0;
}

void awl_osd_stop( void ) {
    awl_host->timer_remove( O.timer );
    memset( &O, 0, sizeof O );
}
