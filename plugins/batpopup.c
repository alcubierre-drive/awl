#include "batpopup.h"
#include "plugins.h"
#include "plugins/colors.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
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
static struct {
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
} B;

static int px( float v ) {
    return (int)lroundf( v * B.scale );
}

/** whether s changed */
static int snap( void ) {
    awl_plugin_data_t* P = awl_plugin_get();
    batsnap_t s = { .charge = -1, .charging = -1, .ac = -1, .docked = -1, .start = -1, .end = -1 };
    if (P && P->bat) {
        s.charging = atomic_load( &P->bat->charging );
        if (s.charging >= 0) s.charge = (int)lroundf( atomic_load( &P->bat->charge ) * 100.0f );
        s.ac = atomic_load( &P->bat->ac );
        s.docked = atomic_load( &P->bat->docked );
        bat_status( P->bat, s.status, sizeof s.status );
        bat_thresholds( P->bat, &s.start, &s.end );
    }
    if (!memcmp( &s, &B.s, sizeof s )) return 0;
    B.s = s;
    return 1;
}

/** the first row: "85%  Charging" */
static void line1( char* buf, size_t n ) {
    if (B.s.charge < 0) snprintf( buf, n, "%s", *B.s.status ? B.s.status : "no battery" );
    else snprintf( buf, n, "%d%%%s%s", B.s.charge, *B.s.status ? "  " : "", B.s.status );
}

/** the second: "on AC · docked" */
static void line2( char* buf, size_t n ) {
    const char* power = B.s.ac > 0 ? "on AC" : B.s.ac == 0 ? "on battery" : "";
    const char* dock = B.s.docked > 0 ? "docked" : B.s.docked == 0 ? "undocked" : "";
    snprintf( buf, n, "%s%s%s", power, *power && *dock ? " · " : "", dock );
}

static const char* const labels[2] = { "Start charging below", "Stop charging at" };

static void set_part( int p, int x, int y, int w, int h ) {
    B.part[p] = (box_t){ x, y, w, h };
    B.has[p] = 1;
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    (void)data; (void)max_h;
    if (!font) return 0;
    B.scale = scale;
    memset( B.has, 0, sizeof B.has );
    char buf[128];
    const int fh = B.fh = font->height, pad = px( 5 ), gap = px( 6 );
    const int bh = B.bh = fh + px( 6 );
    const int arrow_w = MAX( (int)awl_host->text_width( font, "‹" ), (int)awl_host->text_width( font, "›" ) ) + 2 * pad;
    const int value_w = (int)awl_host->text_width( font, "100%" ) + 2 * pad;

    int W = 0;
    line1( buf, sizeof buf );
    W = MAX( W, (int)awl_host->text_width( font, buf ) );
    line2( buf, sizeof buf );
    W = MAX( W, (int)awl_host->text_width( font, buf ) );
    B.label_w = 0;
    for (int i = 0; i < 2; i++) B.label_w = MAX( B.label_w, (int)awl_host->text_width( font, labels[i] ) );
    const int has_start = B.s.start >= 0, has_end = B.s.end >= 0;
    if (has_start || has_end) W = MAX( W, B.label_w + gap + 2 * arrow_w + value_w );
    if (B.err) {
        snprintf( buf, sizeof buf, "can't set: %s", strerror( B.err ) );
        W = MAX( W, (int)awl_host->text_width( font, buf ) );
    }
    if (W > max_w) W = max_w;

    int y = 0;
    y += bh; /* the charge */
    y += bh; /* the power and the dock */
    const int bx = W - 2 * arrow_w - value_w;
    for (int i = 0; i < 2; i++) {
        if (!(i ? has_end : has_start)) continue;
        if (i) B.row2_y = y; else B.row1_y = y;
        set_part( i ? P_EDOWN : P_SDOWN, bx, y, arrow_w, bh );
        B.value[i] = (box_t){ bx + arrow_w, y, value_w, bh };
        set_part( i ? P_EUP : P_SUP, bx + arrow_w + value_w, y, arrow_w, bh );
        set_part( i ? P_EROW : P_SROW, 0, y, W, bh );
        y += bh;
    }
    B.err_y = y;
    if (B.err) y += bh;

    B.w = W;
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
static int can_step( int i, int dir ) {
    int v = i ? B.s.end : B.s.start;
    if (v < 0) return 0;
    if (dir < 0) return i ? v > MAX( B.s.start + 1, 1 ) : v > 0;
    return i ? v < 100 : v < (B.s.end >= 0 ? B.s.end - 1 : 99);
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover ) {
    (void)data; (void)scale;
    const awl_colors_t* c = &awl_config()->colors;
    const awl_plugin_config_t* pc = awl_plugin_config();
    char buf[128];
    if (hover >= 0 && hover < P_SROW && B.has[hover]) {
        box_t b = B.part[hover];
        awl_fill_shape( pix, (float)(x0 + b.x), (float)(y0 + b.y), (float)b.w, (float)b.h, (float)px( 4 ), 0,
                        c->popup_hover );
    }

    /* the charge in the bar's color for it, then the status */
    int x = 0;
    if (B.s.charge >= 0) {
        snprintf( buf, sizeof buf, "%d%%", B.s.charge );
        uint32_t fg = colormap_color( B.s.charging > 0 ? &pc->colors.battery_charging : &pc->colors.battery,
                                      (float)B.s.charge / 100.0f );
        text_in( pix, font, x0, y0, (box_t){ 0, 0, B.w, B.bh }, buf, fg, 0 );
        x = (int)awl_host->text_width( font, buf ) + (int)awl_host->text_width( font, "  " );
    }
    if (x < B.w) {
        const char* status = B.s.charge >= 0 ? B.s.status : (*B.s.status ? B.s.status : "no battery");
        text_in( pix, font, x0, y0, (box_t){ x, 0, B.w - x, B.bh }, status, c->popup_fg, 0 );
    }
    line2( buf, sizeof buf );
    text_in( pix, font, x0, y0, (box_t){ 0, B.bh, B.w, B.bh }, buf, c->popup_dim, 0 );

    /* the thresholds */
    for (int i = 0; i < 2; i++) {
        if (!B.has[i ? P_EROW : P_SROW]) continue;
        const int y = i ? B.row2_y : B.row1_y;
        const int bx = B.part[i ? P_EDOWN : P_SDOWN].x;
        text_in( pix, font, x0, y0, (box_t){ 0, y, bx > 0 ? bx : 0, B.bh }, labels[i], c->popup_fg, 0 );
        text_in( pix, font, x0, y0, B.part[i ? P_EDOWN : P_SDOWN], "‹", can_step( i, -1 ) ? c->popup_fg : c->popup_dim, 1 );
        snprintf( buf, sizeof buf, "%d%%", i ? B.s.end : B.s.start );
        text_in( pix, font, x0, y0, B.value[i], buf, c->popup_fg, 1 );
        text_in( pix, font, x0, y0, B.part[i ? P_EUP : P_SUP], "›", can_step( i, 1 ) ? c->popup_fg : c->popup_dim, 1 );
    }
    if (B.err) {
        snprintf( buf, sizeof buf, "can't set: %s", strerror( B.err ) );
        text_in( pix, font, x0, y0, (box_t){ 0, B.err_y, B.w, B.bh }, buf, pc->colors.bad, 0 );
    }
}

static int hit( void* data, int x, int y ) {
    (void)data;
    for (int p = 0; p < P_COUNT; p++) {
        box_t b = B.part[p];
        if (B.has[p] && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return p;
    }
    return -1;
}

static int box( void* data, int p, int* x, int* y, int* w, int* h ) {
    (void)data;
    if (p < 0 || p >= P_COUNT || !B.has[p]) return 0;
    *x = B.part[p].x;
    *y = B.part[p].y;
    *w = B.part[p].w;
    *h = B.part[p].h;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- what it does ---- */

static void redraw( void ) {
    if (B.win) awl_host->window_redraw( awl_host->srv, B.win );
}

/** steps threshold i (0 start, 1 end) by n steps, kept in its range: the
 * start below the end */
static void step( int i, int n ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->bat || !n) return;
    snap();
    int v = i ? B.s.end : B.s.start;
    if (v < 0) return;
    int lo = i ? MAX( B.s.start + 1, 1 ) : 0, hi = i ? 100 : (B.s.end >= 0 ? B.s.end - 1 : 99);
    int step = awl_plugin_config()->battery.threshold_step;
    int to = v + n * (step > 0 ? step : 1);
    to = to < lo ? lo : to > hi ? hi : to;
    if (to != v) {
        B.err = bat_threshold_set( P->bat, i, to );
        if (B.err)
            awl_wrn_printf( "battery: can't set the charge %s threshold to %d%%: %s\n", i ? "end" : "start", to,
                            strerror( B.err ) );
    }
    snap();
    redraw();
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
    (void)win; (void)col; (void)data;
    if (button != BTN_LEFT) return;
    switch (part) {
    case P_SDOWN: step( 0, -1 ); break;
    case P_SUP: step( 0, 1 ); break;
    case P_EDOWN: step( 1, -1 ); break;
    case P_EUP: step( 1, 1 ); break;
    default: break;
    }
}

/** over a threshold's row: up raises it, down lowers it; a wheel notch is
 * a step, a touchpad's 10 units as on the bar (awl_bar.c's SCROLL_LIMIT) */
static void scroll( awl_window_t* win, int part, int col, uint32_t orientation, double delta,
                    int32_t value120, void* data ) {
    (void)win; (void)col; (void)data;
    int i = threshold_of( part );
    if (orientation != 0 || i < 0) {
        B.acc = 0;
        return;
    }
    B.acc += value120 ? value120 / 120.0 : delta / 10.0;
    int n = (int)B.acc;
    if (!n) return;
    B.acc -= n;
    step( i, -n );
}

/** the window, made on first use; NULL before awl is set up */
static awl_window_t* window( void ) {
    if (B.win) return B.win;
    if (!(B.win = awl_host->window_create( awl_host->srv ))) return NULL;
    awl_host->window_set_painter( awl_host->srv, B.win, &painter, NULL );
    awl_host->window_set_click( awl_host->srv, B.win, click, NULL );
    awl_host->window_set_scroll( awl_host->srv, B.win, scroll, NULL );
    return B.win;
}

void awl_batpopup_show( widget_t* w, int center_x ) {
    awl_window_t* win = window();
    if (!win) return;
    w->popup_window = win;
    if (!awl_host->window_monitor( win )) {
        B.err = 0;
        B.acc = 0;
    }
    snap();
    awl_host->window_redraw( awl_host->srv, win );
    awl_host->window_show( awl_host->srv, win, w->bar->m, center_x );
}

void awl_batpopup_hide( void ) {
    if (B.win) awl_host->window_hide( awl_host->srv, B.win );
}

void awl_batpopup_poll( void ) {
    if (B.win && awl_host->window_monitor( B.win ) && snap()) redraw();
}

void awl_batpopup_stop( void ) {
    if (B.win) awl_host->window_destroy( awl_host->srv, B.win );
    memset( &B, 0, sizeof B );
}
