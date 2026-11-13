/* The volume's, brightness's, wallpaper's and desktop files' notifications,
 * see osd.h. Main thread only. */

#include "osd.h"
#include "../plugins.h"
#include "wallpaper.h"

#include <stdio.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CFG (&o->p->plugin_config.osd)

/** ms: how long it waits for a change before it sends the value as it
 * is; how long after the last ask a change counts as its answer */
enum osd_times { QUIET_MS = 150, WATCH_MS = 1500 };

/** what a notification shows */
typedef enum { OSD_VOLUME, OSD_BRIGHTNESS, OSD_N } kind_t;

/** a value's watch */
typedef struct {
    /** when a change was last asked for, ms; 0: never */
    long long asked;
    /** nothing sent since */
    bool due;
    /** the state last sent (:c:func:`state`), or found when the watch began */
    long long shown;
    /** NULL: the watch is for our asks; else it began with a change we
     * didn't ask for (:c:func:`unasked`), which its changes go out as */
    const char* label;
    /** the state the last redraw found; -1 not known */
    long long seen;
    /** :c:func:`switches` as the last redraw found it */
    int switches;
} watch_t;

struct awl_osd {
    /** the root, for the daemons' state */
    awl_plugin_data_t* p;
    awl_timer_t* timer;
    watch_t w[OSD_N];
    /** what the brightness asks under way should come to, per mille, as
     * shown already; -1: none under way, or no guess */
    long long guess;
    /** a wallpaper change is under way that shows once it's done
     * (:c:func:`awl_osd_wallpaper_ask`) */
    bool wallpaper_due;
};

static long long now_ms( void ) {
    struct timespec t;
    clock_gettime( CLOCK_MONOTONIC, &t );
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/** k's value and what goes with it in one number; -1 not known */
static long long state( awl_osd_t* o, kind_t k ) {
    awl_plugin_data_t* P = o->p;
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

/** the brightness's icon for b in [0,1]: off, low, medium, high */
static int brightness_level( float b ) {
    return b <= 0 ? 0 : b <= 1.0f / 3 ? 1 : b <= 2.0f / 3 ? 2 : 3;
}

/** what the brightness's notification shows for state s, in one number: a
 * guess off by less than that isn't worth a correction */
static long long brightness_looks( long long s ) {
    const float b = (float)s / 1000.0f;
    return percent( b ) * 4 + brightness_level( b );
}

/** k's notification for state s (known); label: NULL for an answer to our
 * asks, else the summary of a change we didn't ask for, which also gets
 * the bar in :c:member:`colors.osd_unasked <awl_plugin_colors_t.osd_unasked>` */
/** what all of them share: low, an OSD, the osd config's size, no frame;
 * the bar (if any) in :c:member:`colors.osd_progress <awl_plugin_colors_t.osd_progress>` */
static awl_notif_send_t base( const awl_osd_t* o ) {
    const awl_plugin_config_t* pc = &o->p->plugin_config;
    awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
    n.urgency = AWL_NOTIF_LOW;
    n.flags = AWL_NOTIF_OSD;
    n.timeout = CFG->timeout;
    AWL_SET( n.look.width, CFG->width.v );
    AWL_SET( n.look.image_size, CFG->image_size.v );
    AWL_SET( n.look.frame_width, 0 );
    /* the low urgency's frame is too dim for the bar's fill: osd_progress
     * falls back on the text's */
    AWL_SET( n.look.progress, pc->colors.osd_progress.v );
    return n;
}

static void send( const awl_osd_t* o, kind_t k, long long s, const char* label ) {
    const awl_plugin_config_t* pc = &o->p->plugin_config;
    awl_notif_send_t n = base( o );
    if (label) AWL_SET( n.look.progress, pc->colors.osd_unasked );
    n.summary = label;
    if (k == OSD_VOLUME) {
        const float v = (float)(s / 4) / 1000.0f, max = pc->volume.max;
        const int muted = (int)(s & 2), speaker_muted = (int)(s & 1);
        n.tag = "osd-volume";
        n.image = muted || v <= 0 ? pc->osd.volume_icons.muted
                : speaker_muted ? pc->osd.volume_icons.headphones
                : v <= 1.0f / 3 ? pc->osd.volume_icons.low
                : v <= 2.0f / 3 ? pc->osd.volume_icons.medium
                : v <= 1 ? pc->osd.volume_icons.high : pc->osd.volume_icons.over;
        n.value = percent( v );
        /* above 100% the full bar fills again, up to max */
        if (v > 1) n.look.over = max > 1 ? percent( (v - 1) / (max - 1) ) : 100;
    } else {
        const float b = (float)s / 1000.0f;
        const char* const icons[] = { pc->osd.brightness_icons.off, pc->osd.brightness_icons.low,
                pc->osd.brightness_icons.medium, pc->osd.brightness_icons.high };
        n.tag = "osd-brightness";
        n.image = icons[brightness_level( b )];
        n.value = percent( b );
    }
    if (n.image && !*n.image) n.image = NULL;
    awl_notify_send( &n );
}

/** whether w's last ask is recent enough for what changes to be its answer */
static int watching( const watch_t* w, long long now ) {
    return w->asked && now - w->asked < WATCH_MS;
}

/** the brightness asks under way are all answered: the daemon's word
 * replaces the guess, shown only if it looks different */
static void settle( awl_osd_t* o ) {
    awl_plugin_data_t* P = o->p;
    if (!P || !P->backlight || atomic_load( &P->backlight->unanswered ) > 0) return;
    watch_t* w = &o->w[OSD_BRIGHTNESS];
    const long long s = state( o, OSD_BRIGHTNESS );
    o->guess = -1;
    if (s < 0) return;
    if (brightness_looks( s ) != brightness_looks( w->shown )) send( o, OSD_BRIGHTNESS, s, w->label );
    w->shown = s;
}

/** k's switches of device (pulsetest.h), news even with the same state; 0
 * for the brightness */
static int switches( awl_osd_t* o, kind_t k ) {
    awl_plugin_data_t* P = o->p;
    return k == OSD_VOLUME && P && P->pulse ? atomic_load( &P->pulse->switches ) : 0;
}

/** the label of k's last change, as a change we didn't ask for
 * (:c:member:`awl_plugin_config_t.osd`'s brightness_unasked and
 * volume_unasked); switched: whether the device switched since the last
 * redraw, which it was then even if a change of the other kind came after */
static const char* label( awl_osd_t* o, kind_t k, int switched ) {
    awl_plugin_data_t* P = o->p;
    if (!P) return NULL;
    if (k == OSD_BRIGHTNESS) {
        if (!P->backlight) return NULL;
        switch (atomic_load( &P->backlight->source )) {
            case BACKLIGHT_SOURCE_UNKNOWN: return CFG->brightness_unasked.unknown;
            case BACKLIGHT_SOURCE_START: return CFG->brightness_unasked.start;
            case BACKLIGHT_SOURCE_TIMER: return CFG->brightness_unasked.timer;
            case BACKLIGHT_SOURCE_UPDATE: return CFG->brightness_unasked.update;
            case BACKLIGHT_SOURCE_ADJUST: return CFG->brightness_unasked.adjust;
            default: return NULL;
        }
    }
    if (!P->pulse) return NULL;
    switch (atomic_load( switched ? &P->pulse->switch_cause : &P->pulse->cause )) {
        case PULSE_CAUSE_CONNECT: return CFG->volume_unasked.connect;
        case PULSE_CAUSE_OTHER: return CFG->volume_unasked.other;
        case PULSE_CAUSE_SINK: return CFG->volume_unasked.sink;
        case PULSE_CAUSE_PORT: return CFG->volume_unasked.port;
        default: return NULL;
    }
}

/** A change to k none of our asks accounts for: the daemon's automatic
 * brightness, an Update() or another client's Adjust(), another output or
 * port. Not from not known (the daemon or PulseAudio (re)connects, a
 * reload), the brightness only if it looks different, and only if its
 * cause has a label (:c:func:`label`). Its watch begins: what follows within WATCH_MS (a
 * new sink's volume after its port, say) goes out the same way. */
static void unasked( awl_osd_t* o, kind_t k, long long now ) {
    watch_t* w = &o->w[k];
    const long long s = state( o, k ), seen = w->seen;
    const int sw = switches( o, k ), switched = sw != w->switches;
    w->seen = s;
    w->switches = sw;
    if (watching( w, now ) || s < 0 || seen < 0) return;
    if (k == OSD_BRIGHTNESS && (o->guess >= 0 || brightness_looks( s ) == brightness_looks( seen ))) return;
    if (s == seen && !switched) return;
    const char* l = label( o, k, switched );
    if (!l) return;
    send( o, k, s, l );
    w->shown = s;
    w->asked = now;
    w->label = l;
    w->due = 0;
}

void awl_osd_redraw( awl_osd_t* o ) {
    if (!o) return;
    const long long now = now_ms();
    if (o->guess >= 0) settle( o );
    for (int k = 0; k < OSD_N; k++) {
        watch_t* w = &o->w[k];
        /* a guess stands until it's settled */
        if (k == OSD_BRIGHTNESS && o->guess >= 0) continue;
        if (!watching( w, now )) continue;
        const long long s = state( o, (kind_t)k );
        if (s < 0 || s == w->shown) continue;
        send( o, (kind_t)k, s, w->label );
        w->shown = s;
        w->due = 0;
    }
    for (int k = 0; k < OSD_N; k++) unasked( o, (kind_t)k, now );
}

/** no change came in QUIET_MS: what is asked for goes out as it is (at its
 * limit, say); not known yet, it waits for :c:func:`awl_osd_redraw` */
static void quiet( void* data ) {
    awl_osd_t* o = data;
    for (int k = 0; k < OSD_N; k++) {
        watch_t* w = &o->w[k];
        const long long s = state( o, (kind_t)k );
        if (!w->due || s < 0) continue;
        send( o, (kind_t)k, s, w->label );
        w->shown = s;
        w->due = 0;
    }
}

static void ask( awl_osd_t* o, kind_t k ) {
    watch_t* w = &o->w[k];
    const long long now = now_ms();
    if (!o->timer) return;
    /* what it is now isn't news; what it changes to is */
    if (!watching( w, now )) w->shown = state( o, k );
    w->asked = now;
    w->label = NULL;
    w->due = 1;
    awl_host->timer_set( o->timer, QUIET_MS );
}

void awl_osd_volume( awl_osd_t* o ) {
    if (!o) return;
    ask( o, OSD_VOLUME );
}

void awl_osd_brightness( awl_osd_t* o, float delta ) {
    if (!o) return;
    awl_plugin_data_t* P = o->p;
    watch_t* w = &o->w[OSD_BRIGHTNESS];
    /* on top of the asks under way, if any, else of what it is */
    const long long base = o->guess >= 0 ? o->guess : state( o, OSD_BRIGHTNESS );
    if (!o->timer || !P || !P->backlight || base < 0) {
        /* nothing to guess from: wait for the daemon */
        ask( o, OSD_BRIGHTNESS );
        return;
    }
    /* clamped as the daemon does */
    const long long min = atomic_load( &P->backlight->screen_min );
    long long g = base + lroundf( delta * 1000.0f );
    if (g < min) g = min;
    if (g > 1000) g = 1000;
    send( o, OSD_BRIGHTNESS, g, NULL );
    o->guess = g;
    w->asked = now_ms();
    w->label = NULL;
    w->shown = g;
    w->due = 0;
}

awl_osd_t* awl_osd_start( awl_plugin_data_t* p ) {
    awl_osd_t* o = calloc( 1, sizeof *o );
    if (!o) return NULL;
    o->p = p;
    o->guess = -1;
    for (int k = 0; k < OSD_N; k++) o->w[k].seen = -1;
    o->timer = awl_host->timer_add( awl_host->srv, quiet, o );
    return o;
}

void awl_osd_stop( awl_osd_t* o ) {
    if (!o) return;
    awl_host->timer_remove( o->timer );
    free( o );
}

/* ---- the wallpaper and the desktop files ---- */

void awl_osd_wallpaper( awl_osd_t* o ) {
    if (!o) return;
    awl_notif_send_t n = base( o );
    int cur, count;
    const int settled = awl_wallpaper_settled( o->p->wallpaper, &cur, &count ) && count > 0;
    /* the timer's mode, as awl keeps it in the config dictionary */
    char mode[16] = "";
    awl_dict_val_t v;
    AWL_HOST_CFG_LOCK( awl_host );
    if (AWL_HOST_CFG_GET( awl_host, "wallpaper_mode", &v ) == AWL_DICT_OK && v.kind == AWL_DICT_KIND_STR)
        snprintf( mode, sizeof mode, "%s", v.str );
    AWL_HOST_CFG_UNLOCK( awl_host );
    const char* off = o->p->config.wallpaper.interval ? "" : " (off)";
    char title[64];
    if (settled)
        snprintf( title, sizeof title, "%d/%d%s%s%s", cur + 1, count, *mode ? " · " : "", mode, off );
    else
        snprintf( title, sizeof title, "%s%s", *mode ? mode : "Wallpaper", off );
    n.tag = "osd-wallpaper";
    n.summary = title;
    n.image = CFG->wallpaper_icon && *CFG->wallpaper_icon ? CFG->wallpaper_icon : NULL;
    /* the position in the folder; no bar while a change is under way */
    n.value = settled ? (cur + 1) * 100 / count : -1;
    awl_notify_send( &n );
}

void awl_osd_wallpaper_ask( awl_osd_t* o ) {
    if (!o) return;
    o->wallpaper_due = 1;
}

void awl_osd_wallpaper_taken( awl_osd_t* o ) {
    int cur, count;
    if (!o) return;
    if (!o->wallpaper_due || !awl_wallpaper_settled( o->p->wallpaper, &cur, &count )) return;
    o->wallpaper_due = 0;
    awl_osd_wallpaper( o );
}

/** text alone, centered, icon beside it (NULL, "": none), as the
 * notification tag */
static void text_osd( const awl_osd_t* o, const char* tag, const char* text, const char* icon ) {
    awl_notif_send_t n = base( o );
    n.tag = tag;
    n.summary = text;
    n.image = icon && *icon ? icon : NULL;
    awl_notify_send( &n );
}

void awl_osd_desktop( awl_osd_t* o, const char* text ) {
    if (!o) return;
    text_osd( o, "osd-desktop", text, CFG->desktop_icon );
}

void awl_osd_dnd( awl_osd_t* o, int on ) {
    if (!o) return;
    const awl_plugin_config_t* pc = &o->p->plugin_config;
    text_osd( o, "osd-dnd", on ? "Do not disturb" : "Notifications enabled",
              on ? pc->notify.bell_icon_dnd : pc->notify.bell_icon );
}
