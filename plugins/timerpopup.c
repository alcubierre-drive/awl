#include "timerpopup.h"
#include "plugins.h"
#include "insult.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>
#include <time.h>

/** the second row's parts, left to right */
enum { P_START, P_STOP, P_DOWN, P_TIME, P_UP, P_COUNT };

/** the duration's bounds and its first value, seconds */
#define DUR_MIN 60
#define DUR_MAX (12 * 3600)
#define DUR_DEFAULT (5 * 60)


/** the timer and its popup; layout in physical pixels, from the content's
 * top left */
struct awl_timerpopup {
    /** the root */
    awl_plugin_data_t* p;
    awl_window_t* win;
    /** fires when the timer is due */
    awl_timer_t* timer;
    /** what Start runs, seconds */
    int dur;
    /** the duration it runs (for the notification) and when it ends, wall
     * clock seconds; end 0 stopped. The wall clock goes on through a
     * suspend. */
    int ran;
    time_t end;
    /** the second the popup last showed */
    time_t shown;
    /** scrolling not yet turned into steps */
    double acc;
    /** the insults' rand_r() state, seeded from the kernel's random
     * numbers at start */
    unsigned seed;

    /* measure()'s layout */
    float scale;
    /** the buttons' text's inset, which the first row's has too */
    int w, bh, pad;
    awl_box_t part[P_COUNT];
};

static const char* const labels[P_COUNT] = { "Start", "Stop", "‹", NULL, "›" };

/** s seconds as 4:05 or 1:02:03 */
static void fmt( char* buf, size_t n, int s ) {
    if (s >= 3600) snprintf( buf, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60 );
    else snprintf( buf, n, "%d:%02d", s / 60, s % 60 );
}

/** the first row's two halves: "4:05 left" and "until 14:35:13", or
 * "stopped" and "" */
static void line1( awl_timerpopup_t* tp, char* left, char* until, size_t n, time_t now ) {
    *until = '\0';
    if (!tp->end) {
        snprintf( left, n, "stopped" );
        return;
    }
    char t[16];
    struct tm tm;
    fmt( t, sizeof t, tp->end > now ? (int)(tp->end - now) : 0 );
    snprintf( left, n, "%s left", t );
    localtime_r( &tp->end, &tm );
    strftime( t, sizeof t, "%H:%M:%S", &tm );
    snprintf( until, n, "until %s", t );
}

/* ---- the popup's painter ---- */

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    awl_timerpopup_t* tp = data;
    (void)max_h;
    if (!font) return 0;
    tp->scale = scale;
    const int pad = tp->pad = px( tp, 6 ), gap = px( tp, 10 );
    tp->bh = font->height + px( tp, 6 );
    /* wide enough for the widest of each, so it doesn't change size */
    int bw[P_COUNT];
    for (int i = 0; i < P_COUNT; i++)
        bw[i] = (int)awl_host->text_width( font, labels[i] ? labels[i] : "00:00:00" ) + 2 * pad;
    int row2 = 0;
    for (int i = 0; i < P_COUNT; i++) row2 += bw[i];
    int row1 = (int)awl_host->text_width( font, "00:00:00 left" ) + gap +
               (int)awl_host->text_width( font, "until 00:00:00" ) + 2 * pad;
    int W = MAX( row2 + gap, row1 );
    if (W > max_w) W = max_w;
    /* Start and Stop on the left, ‹ time › on the right */
    int x = 0;
    for (int i = 0; i < P_COUNT; i++) {
        if (i == P_DOWN) x = W - bw[P_DOWN] - bw[P_TIME] - bw[P_UP];
        tp->part[i] = (awl_box_t){ x, tp->bh, bw[i], tp->bh };
        x += bw[i];
    }
    tp->w = W;
    *w = W;
    *h = 2 * tp->bh;
    return W > 0;
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover ) {
    awl_timerpopup_t* tp = data;
    (void)scale;
    const awl_colors_t* c = &tp->p->config.colors;
    char buf[64];
    time_t now = time( NULL );
    tp->shown = now;
    if (hover >= 0 && hover < P_COUNT && hover != P_TIME) {
        awl_box_t b = tp->part[hover];
        awl_fill_shape( pix, (float)(x0 + b.x), (float)(y0 + b.y), (float)b.w, (float)b.h, (float)px( tp, 4 ), 0,
                        c->popup_hover );
    }
    /* the time left on the left, faint when stopped; when it ends faint
     * on the right; inset as the buttons' text, so they line up with
     * Start's and ›'s */
    char until[32];
    const int in = tp->pad, inner = MAX( tp->w - 2 * in, 0 );
    line1( tp, buf, until, sizeof until, now );
    text_in( pix, font, x0, y0, (awl_box_t){ in, 0, inner, tp->bh }, buf, tp->end ? c->popup_fg : c->popup_dim, 0 );
    if (*until) {
        int uw = MIN( (int)awl_host->text_width( font, until ), inner );
        text_in( pix, font, x0, y0, (awl_box_t){ tp->w - in - uw, 0, uw, tp->bh }, until, c->popup_dim, 0 );
    }
    for (int i = 0; i < P_COUNT; i++) {
        const char* s = labels[i];
        if (!s) {
            fmt( buf, sizeof buf, tp->dur );
            s = buf;
        }
        int dim = (i == P_STOP && !tp->end) || (i == P_DOWN && tp->dur <= DUR_MIN) || (i == P_UP && tp->dur >= DUR_MAX);
        text_in( pix, font, x0, y0, tp->part[i], s, dim ? c->popup_dim : c->popup_fg, 1 );
    }
}

static int hit( void* data, int x, int y ) {
    awl_timerpopup_t* tp = data;
    return parts_hit( tp->part, NULL, P_COUNT, x, y );
}

static int box( void* data, int p, int* x, int* y, int* w, int* h ) {
    awl_timerpopup_t* tp = data;
    return parts_box( tp->part, NULL, P_COUNT, p, x, y, w, h );
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- what it does ---- */

static void redraw( awl_timerpopup_t* tp ) {
    if (tp->win) awl_host->window_redraw( awl_host->srv, tp->win );
}

/** sends the run-out notification if the timer is due, else arms the
 * timer for when it will be */
static void check( awl_timerpopup_t* tp ) {
    if (!tp->end) return;
    time_t now = time( NULL );
    if (now < tp->end) {
        long ms = (long)(tp->end - now) * 1000;
        awl_host->timer_set( tp->timer, ms > 86400000 ? 86400000 : (int)ms );
        return;
    }
    char body[256], dur[16], insult[160];
    fmt( dur, sizeof dur, tp->ran );
    awl_insult( insult, sizeof insult, &tp->seed );
    /* the body has markup; the insults' words have no '<' or '&' */
    snprintf( body, sizeof body, "<b>%s</b> Your time (%s) is up.", insult, dur );
    awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
    n.summary = "Timer";
    n.body = body;
    n.image = "alarm-symbolic;appointment-soon";
    n.urgency = AWL_NOTIF_CRITICAL;
    n.timeout = 0;
    n.tag = "timer";
    awl_notify_send( &n );
    tp->end = 0;
    redraw( tp );
}

static void fire( void* data ) {
    check( data );
}

static void run( awl_timerpopup_t* tp ) {
    if (!tp->timer && !(tp->timer = awl_host->timer_add( awl_host->srv, fire, tp ))) return;
    tp->ran = tp->dur;
    tp->end = time( NULL ) + tp->dur;
    check( tp );
    redraw( tp );
}

static void halt( awl_timerpopup_t* tp ) {
    tp->end = 0;
    awl_host->timer_set( tp->timer, 0 );
    redraw( tp );
}

/** steps the duration by n steps: a minute up to 10, 5 up to an hour, 15
 * beyond; a step off the grid lands on it */
static void step( awl_timerpopup_t* tp, int n ) {
    for (; n > 0; n--) {
        int s = tp->dur < 600 ? 60 : tp->dur < 3600 ? 300 : 900;
        tp->dur = (tp->dur / s + 1) * s;
    }
    for (; n < 0; n++) {
        int s = tp->dur <= 600 ? 60 : tp->dur <= 3600 ? 300 : 900;
        tp->dur = (tp->dur + s - 1) / s * s - s;
    }
    tp->dur = tp->dur < DUR_MIN ? DUR_MIN : tp->dur > DUR_MAX ? DUR_MAX : tp->dur;
    redraw( tp );
}

static void click( awl_window_t* win, int part, int col, uint32_t button, void* data ) {
    awl_timerpopup_t* tp = data;
    (void)win; (void)col;
    if (button != BTN_LEFT) return;
    switch (part) {
    case P_START: run( tp ); break;
    case P_STOP: halt( tp ); break;
    case P_DOWN: step( tp, -1 ); break;
    case P_UP: step( tp, 1 ); break;
    default: break;
    }
}

/** anywhere on it: up lengthens, down shortens */
static void scroll( awl_window_t* win, int part, int col, uint32_t orientation, double delta,
                    int32_t value120, void* data ) {
    awl_timerpopup_t* tp = data;
    (void)win; (void)part; (void)col;
    if (orientation != 0) return;
    int n = scroll_steps( &tp->acc, value120, delta );
    if (!n) return;
    step( tp, -n );
}

void awl_timerpopup_show( awl_timerpopup_t* tp, widget_t* w, int center_x ) {
    if (!tp) return;
    awl_window_t* win = popup_window( &tp->win, &painter, click, scroll, tp );
    if (!win) return;
    w->popup_window = win;
    if (!awl_host->window_monitor( win )) tp->acc = 0;
    awl_host->window_redraw( awl_host->srv, win );
    awl_host->window_show( awl_host->srv, win, w->bar->m, center_x );
}

void awl_timerpopup_hide( awl_timerpopup_t* tp ) {
    if (tp && tp->win) awl_host->window_hide( awl_host->srv, tp->win );
}

int awl_timerpopup_shown( awl_timerpopup_t* tp ) {
    return tp && tp->win && awl_host->window_monitor( tp->win );
}

void awl_timerpopup_poll( awl_timerpopup_t* tp ) {
    if (!tp) return;
    if (tp->end && time( NULL ) >= tp->end) check( tp );
    else if (tp->end && awl_timerpopup_shown( tp ) && time( NULL ) != tp->shown) redraw( tp );
}

awl_timerpopup_t* awl_timerpopup_start( awl_plugin_data_t* p ) {
    awl_timerpopup_t* tp = calloc( 1, sizeof *tp );
    if (!tp) return NULL;
    tp->p = p;
    tp->dur = DUR_DEFAULT;
    /* without random numbers (an old kernel's early boot), at least not
     * the same insults every time */
    if (getrandom( &tp->seed, sizeof tp->seed, GRND_NONBLOCK ) != sizeof tp->seed)
        tp->seed = (unsigned)time( NULL ) ^ (unsigned)getpid();
    return tp;
}

void awl_timerpopup_stop( awl_timerpopup_t* tp ) {
    if (!tp) return;
    awl_host->timer_remove( tp->timer );
    if (tp->win) awl_host->window_destroy( awl_host->srv, tp->win );
    free( tp );
}
