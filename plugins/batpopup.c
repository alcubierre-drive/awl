#include "batpopup.h"
#include "plugins.h"
#include "plugins/colors.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the parts: each threshold's buttons, then its row (only scrolls; after
 * the buttons, so hit() finds those first) */
enum { P_SDOWN, P_SUP, P_EDOWN, P_EUP, P_SROW, P_EROW, P_COUNT };

typedef struct { int x, y, w, h; } box_t;

/** what the popup shows */
typedef struct {
    /** percent, -1 unknown */
    int charge;
    /** awl_battery_t's */
    int charging, ac, docked;
    char status[32];
    /** the thresholds, percent; -1 the battery doesn't have it */
    int start, end;
} batsnap_t;

/** the popup; layout in physical pixels, from the content's top left */
struct awl_batpopup {
    /** the root */
    awl_plugin_data_t* p;
    awl_window_t* win;
    batsnap_t s;
    /** the last write's errno, 0 none; shown until one works or it reopens */
    int err;
    /** scrolling not yet turned into steps */
    double acc;

    /* measure()'s layout */
    float scale;
    int w, fh, bh, row1_y, row2_y, err_y, label_w;
    box_t part[P_COUNT], value[2];
    int has[P_COUNT];
};

static int px( awl_batpopup_t* bp, float v ) {
    return (int)lroundf( v * bp->scale );
}

/** whether s changed */
static int snap( awl_batpopup_t* bp ) {
    awl_plugin_data_t* P = bp->p;
    batsnap_t s = { .charge = -1, .charging = -1, .ac = -1, .docked = -1, .start = -1, .end = -1 };
    if (P && P->bat) {
        s.charging = atomic_load( &P->bat->charging );
        if (s.charging >= 0) s.charge = (int)lroundf( atomic_load( &P->bat->charge ) * 100.0f );
        s.ac = atomic_load( &P->bat->ac );
        s.docked = atomic_load( &P->bat->docked );
        bat_status( P->bat, s.status, sizeof s.status );
        bat_thresholds( P->bat, &s.start, &s.end );
    }
    if (!memcmp( &s, &bp->s, sizeof s )) return 0;
    bp->s = s;
    return 1;
}

/** the first row: "85%  Charging" */
static void line1( awl_batpopup_t* bp, char* buf, size_t n ) {
    if (bp->s.charge < 0) snprintf( buf, n, "%s", *bp->s.status ? bp->s.status : "no battery" );
    else snprintf( buf, n, "%d%%%s%s", bp->s.charge, *bp->s.status ? "  " : "", bp->s.status );
}

/** the second: "on AC · docked" */
static void line2( awl_batpopup_t* bp, char* buf, size_t n ) {
    const char* power = bp->s.ac > 0 ? "on AC" : bp->s.ac == 0 ? "on battery" : "";
    const char* dock = bp->s.docked > 0 ? "docked" : bp->s.docked == 0 ? "undocked" : "";
    snprintf( buf, n, "%s%s%s", power, *power && *dock ? " · " : "", dock );
}

static const char* const labels[2] = { "Start charging below", "Stop charging at" };

static void set_part( awl_batpopup_t* bp, int p, int x, int y, int w, int h ) {
    bp->part[p] = (box_t){ x, y, w, h };
    bp->has[p] = 1;
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    awl_batpopup_t* bp = data;
    (void)max_h;
    if (!font) return 0;
    bp->scale = scale;
    memset( bp->has, 0, sizeof bp->has );
    char buf[128];
    const int fh = bp->fh = font->height, pad = px( bp, 5 ), gap = px( bp, 6 );
    const int bh = bp->bh = fh + px( bp, 6 );
    const int arrow_w = MAX( (int)awl_host->text_width( font, "‹" ), (int)awl_host->text_width( font, "›" ) ) + 2 * pad;
    const int value_w = (int)awl_host->text_width( font, "100%" ) + 2 * pad;

    int W = 0;
    line1( bp, buf, sizeof buf );
    W = MAX( W, (int)awl_host->text_width( font, buf ) );
    line2( bp, buf, sizeof buf );
    W = MAX( W, (int)awl_host->text_width( font, buf ) );
    bp->label_w = 0;
    for (int i = 0; i < 2; i++) bp->label_w = MAX( bp->label_w, (int)awl_host->text_width( font, labels[i] ) );
    const int has_start = bp->s.start >= 0, has_end = bp->s.end >= 0;
    if (has_start || has_end) W = MAX( W, bp->label_w + gap + 2 * arrow_w + value_w );
    if (bp->err) {
        snprintf( buf, sizeof buf, "can't set: %s", strerror( bp->err ) );
        W = MAX( W, (int)awl_host->text_width( font, buf ) );
    }
    if (W > max_w) W = max_w;

    int y = 0;
    y += bh; /* the charge */
    y += bh; /* the power and the dock */
    const int bx = W - 2 * arrow_w - value_w;
    for (int i = 0; i < 2; i++) {
        if (!(i ? has_end : has_start)) continue;
        if (i) bp->row2_y = y; else bp->row1_y = y;
        set_part( bp, i ? P_EDOWN : P_SDOWN, bx, y, arrow_w, bh );
        bp->value[i] = (box_t){ bx + arrow_w, y, value_w, bh };
        set_part( bp, i ? P_EUP : P_SUP, bx + arrow_w + value_w, y, arrow_w, bh );
        set_part( bp, i ? P_EROW : P_SROW, 0, y, W, bh );
        y += bh;
    }
    bp->err_y = y;
    if (bp->err) y += bh;

    bp->w = W;
    *w = W;
    *h = y;
    return W > 0;
}

/** text in b (offset by x0, y0), centered or left-aligned, cut with an
 * ellipsis if wider */
static void text_in( pixman_image_t* pix, struct fcft_font* font, int x0, int y0, box_t b, const char* s,
                     uint32_t fg, int center ) {
    int w = (int)awl_host->text_width( font, s );
    int x = center && w < b.w ? b.x + (b.w - w) / 2 : b.x;
    awl_host->text_over( pix, font, x0 + x, y0 + b.y, (unsigned)(w < b.w ? w + 1 : b.w), (unsigned)b.h, s, fg );
}

/** whether threshold i (0 start, 1 end) can go by dir (±1) */
static int can_step( awl_batpopup_t* bp, int i, int dir ) {
    int v = i ? bp->s.end : bp->s.start;
    if (v < 0) return 0;
    if (dir < 0) return i ? v > MAX( bp->s.start + 1, 1 ) : v > 0;
    return i ? v < 100 : v < (bp->s.end >= 0 ? bp->s.end - 1 : 99);
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover ) {
    awl_batpopup_t* bp = data;
    (void)scale;
    const awl_colors_t* c = &awl_config()->colors;
    const awl_plugin_config_t* pc = awl_plugin_config();
    char buf[128];
    if (hover >= 0 && hover < P_SROW && bp->has[hover]) {
        box_t b = bp->part[hover];
        awl_fill_shape( pix, (float)(x0 + b.x), (float)(y0 + b.y), (float)b.w, (float)b.h, (float)px( bp, 4 ), 0,
                        c->popup_hover );
    }

    /* the charge in the bar's color for it, then the status */
    int x = 0;
    if (bp->s.charge >= 0) {
        snprintf( buf, sizeof buf, "%d%%", bp->s.charge );
        uint32_t fg = colormap_color( bp->s.charging > 0 ? &pc->colors.battery_charging : &pc->colors.battery,
                                      (float)bp->s.charge / 100.0f );
        text_in( pix, font, x0, y0, (box_t){ 0, 0, bp->w, bp->bh }, buf, fg, 0 );
        x = (int)awl_host->text_width( font, buf ) + (int)awl_host->text_width( font, "  " );
    }
    if (x < bp->w) {
        const char* status = bp->s.charge >= 0 ? bp->s.status : (*bp->s.status ? bp->s.status : "no battery");
        text_in( pix, font, x0, y0, (box_t){ x, 0, bp->w - x, bp->bh }, status, c->popup_fg, 0 );
    }
    line2( bp, buf, sizeof buf );
    text_in( pix, font, x0, y0, (box_t){ 0, bp->bh, bp->w, bp->bh }, buf, c->popup_dim, 0 );

    /* the thresholds */
    for (int i = 0; i < 2; i++) {
        if (!bp->has[i ? P_EROW : P_SROW]) continue;
        const int y = i ? bp->row2_y : bp->row1_y;
        const int bx = bp->part[i ? P_EDOWN : P_SDOWN].x;
        text_in( pix, font, x0, y0, (box_t){ 0, y, bx > 0 ? bx : 0, bp->bh }, labels[i], c->popup_fg, 0 );
        text_in( pix, font, x0, y0, bp->part[i ? P_EDOWN : P_SDOWN], "‹", can_step( bp, i, -1 ) ? c->popup_fg : c->popup_dim, 1 );
        snprintf( buf, sizeof buf, "%d%%", i ? bp->s.end : bp->s.start );
        text_in( pix, font, x0, y0, bp->value[i], buf, c->popup_fg, 1 );
        text_in( pix, font, x0, y0, bp->part[i ? P_EUP : P_SUP], "›", can_step( bp, i, 1 ) ? c->popup_fg : c->popup_dim, 1 );
    }
    if (bp->err) {
        snprintf( buf, sizeof buf, "can't set: %s", strerror( bp->err ) );
        text_in( pix, font, x0, y0, (box_t){ 0, bp->err_y, bp->w, bp->bh }, buf, pc->colors.bad, 0 );
    }
}

static int hit( void* data, int x, int y ) {
    awl_batpopup_t* bp = data;
    for (int p = 0; p < P_COUNT; p++) {
        box_t b = bp->part[p];
        if (bp->has[p] && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return p;
    }
    return -1;
}

static int box( void* data, int p, int* x, int* y, int* w, int* h ) {
    awl_batpopup_t* bp = data;
    if (p < 0 || p >= P_COUNT || !bp->has[p]) return 0;
    *x = bp->part[p].x;
    *y = bp->part[p].y;
    *w = bp->part[p].w;
    *h = bp->part[p].h;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- what it does ---- */

static void redraw( awl_batpopup_t* bp ) {
    if (bp->win) awl_host->window_redraw( awl_host->srv, bp->win );
}

/** steps threshold i (0 start, 1 end) by n steps, kept in its range: the
 * start below the end */
static void step( awl_batpopup_t* bp, int i, int n ) {
    awl_plugin_data_t* P = bp->p;
    if (!P || !P->bat || !n) return;
    snap( bp );
    int v = i ? bp->s.end : bp->s.start;
    if (v < 0) return;
    int lo = i ? MAX( bp->s.start + 1, 1 ) : 0, hi = i ? 100 : (bp->s.end >= 0 ? bp->s.end - 1 : 99);
    int step = awl_plugin_config()->battery.threshold_step;
    int to = v + n * (step > 0 ? step : 1);
    to = to < lo ? lo : to > hi ? hi : to;
    if (to != v) {
        bp->err = bat_threshold_set( P->bat, i, to );
        if (bp->err)
            awl_wrn_printf( "battery: can't set the charge %s threshold to %d%%: %s\n", i ? "end" : "start", to,
                            strerror( bp->err ) );
    }
    snap( bp );
    redraw( bp );
}

/** the threshold a part belongs to, -1 none */
static int threshold_of( int part ) {
    switch (part) {
    case P_SDOWN: case P_SUP: case P_SROW: return 0;
    case P_EDOWN: case P_EUP: case P_EROW: return 1;
    default: return -1;
    }
}

static void click( awl_window_t* win, int part, int col, uint32_t button, void* data ) {
    awl_batpopup_t* bp = data;
    (void)win; (void)col;
    if (button != BTN_LEFT) return;
    switch (part) {
    case P_SDOWN: step( bp, 0, -1 ); break;
    case P_SUP: step( bp, 0, 1 ); break;
    case P_EDOWN: step( bp, 1, -1 ); break;
    case P_EUP: step( bp, 1, 1 ); break;
    default: break;
    }
}

/** over a threshold's row: up raises it, down lowers it; a wheel notch is
 * a step, a touchpad's 10 units as on the bar (awl_bar.c's SCROLL_LIMIT) */
static void scroll( awl_window_t* win, int part, int col, uint32_t orientation, double delta,
                    int32_t value120, void* data ) {
    awl_batpopup_t* bp = data;
    (void)win; (void)col;
    int i = threshold_of( part );
    if (orientation != 0 || i < 0) {
        bp->acc = 0;
        return;
    }
    bp->acc += value120 ? value120 / 120.0 : delta / 10.0;
    int n = (int)bp->acc;
    if (!n) return;
    bp->acc -= n;
    step( bp, i, -n );
}

/** the window, made on first use; NULL before awl is set up */
static awl_window_t* window( awl_batpopup_t* bp ) {
    if (bp->win) return bp->win;
    if (!(bp->win = awl_host->window_create( awl_host->srv ))) return NULL;
    awl_host->window_set_painter( awl_host->srv, bp->win, &painter, bp );
    awl_host->window_set_click( awl_host->srv, bp->win, click, bp );
    awl_host->window_set_scroll( awl_host->srv, bp->win, scroll, bp );
    return bp->win;
}

void awl_batpopup_show( awl_batpopup_t* bp, widget_t* w, int center_x ) {
    if (!bp) return;
    awl_window_t* win = window( bp );
    if (!win) return;
    w->popup_window = win;
    if (!awl_host->window_monitor( win )) {
        bp->err = 0;
        bp->acc = 0;
    }
    snap( bp );
    awl_host->window_redraw( awl_host->srv, win );
    awl_host->window_show( awl_host->srv, win, w->bar->m, center_x );
}

void awl_batpopup_hide( awl_batpopup_t* bp ) {
    if (!bp) return;
    if (bp->win) awl_host->window_hide( awl_host->srv, bp->win );
}

void awl_batpopup_poll( awl_batpopup_t* bp ) {
    if (!bp) return;
    if (bp->win && awl_host->window_monitor( bp->win ) && snap( bp )) redraw( bp );
}

awl_batpopup_t* awl_batpopup_start( awl_plugin_data_t* p ) {
    awl_batpopup_t* bp = calloc( 1, sizeof *bp );
    if (bp) bp->p = p;
    return bp;
}

void awl_batpopup_stop( awl_batpopup_t* bp ) {
    if (!bp) return;
    if (bp->win) awl_host->window_destroy( awl_host->srv, bp->win );
    free( bp );
}
