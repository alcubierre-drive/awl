/* nl_langinfo_l()'s _NL_TIME_FIRST_WEEKDAY, strftime_l() */
#define _GNU_SOURCE

#include "calpopup.h"
#include "calendar.h"
#include "plugins.h"

#include <langinfo.h>
#include <locale.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the parts: the buttons, the event list (only scrolls) and the 6 x 7 days */
enum {
    P_MPREV, P_MNEXT, P_YPREV, P_YNEXT, P_DPREV, P_DNEXT, P_TODAY, P_EVO, P_LIST,
    P_DAY0, P_COUNT = P_DAY0 + 42
};

typedef struct { int x, y, w, h; } box_t;

/** the popup; everything in physical pixels, from the content's top left */
static struct {
    awl_window_t* win;
    /** the environment's LC_TIME (LC_ALL, LC_TIME, LANG), the popup's own:
     * awl's process stays in the C locale; 0: none, so the C locale's */
    locale_t loc;
    /** the selected day; month 0-based */
    int year, month, day;
    /** no events to show (the last query failed): no list, no evolution */
    int plain;
    /** the months fetched so far */
    awl_cal_month_t* cache;
    /** the first event shown, of a list longer than fits */
    int list_top;
    /** scrolling not yet turned into steps */
    double acc;
    /** today, year * 10000 + month * 100 + day, when last looked */
    int today;

    /* what does not change with the content: for font at scale */
    struct fcft_font* font;
    float scale;
    int widest_day, widest_month, widest_wday;

    /* measure()'s layout */
    box_t part[P_COUNT];
    int has[P_COUNT];
    int w, h, fh, bh, cell_h, grid_y, names_y, day_y, list_y, list_h, row_h, rows_fit;
    int dot_w, time_w, gap;
    box_t month_label, year_label, day_label;
} C;

/* ---- dates ---- */

static int month_key( int year, int month ) {
    return year * 12 + month;
}

static int ymd( const struct tm* t ) {
    return (t->tm_year + 1900) * 10000 + t->tm_mon * 100 + t->tm_mday;
}

static int today_ymd( void ) {
    time_t now = time( NULL );
    struct tm t;
    localtime_r( &now, &t );
    return ymd( &t );
}

/** the locale's first day of the week, 0 Sunday, as GTK's calendar has it */
static int week_start( void ) {
    int first = C.loc ? nl_langinfo_l( _NL_TIME_FIRST_WEEKDAY, C.loc )[0] : nl_langinfo( _NL_TIME_FIRST_WEEKDAY )[0];
    unsigned origin = (unsigned)(uintptr_t)(C.loc ? nl_langinfo_l( _NL_TIME_WEEK_1STDAY, C.loc )
                                                  : nl_langinfo( _NL_TIME_WEEK_1STDAY ));
    int day1 = origin == 19971201 ? 1 : 0; /* else 19971130, a Sunday */
    int s = (day1 + first - 1) % 7;
    return s < 0 ? 0 : s;
}

static awl_cal_month_t* cached( int key ) {
    for (awl_cal_month_t* m = C.cache; m; m = m->next)
        if (m->key == key) return m;
    return NULL;
}

/** whether e touches [d0, d1) (its end is exclusive, an instant counts) */
static int touches( const awl_cal_event_t* e, time_t d0, time_t d1 ) {
    time_t end = e->end > e->start ? e->end : e->start + 1;
    return e->start < d1 && end > d0;
}

/* ---- drawing ---- */

static int px( float v ) {
    return (int)lroundf( v * C.scale );
}

/** text centered in b (offset by x0, y0), cut with an ellipsis if wider */
static void text_center( pixman_image_t* pix, struct fcft_font* font, int x0, int y0, box_t b,
                         const char* s, uint32_t fg ) {
    int w = (int)awl_host->text_width( font, s );
    int x = w < b.w ? b.x + (b.w - w) / 2 : b.x;
    awl_host->text_over( pix, font, x0 + x, y0 + b.y, (unsigned)(w < b.w ? w + 1 : b.w), (unsigned)b.h, s, fg );
}

static void strtime( char* buf, size_t n, const char* fmt, time_t t ) {
    struct tm tm;
    localtime_r( &t, &tm );
    size_t len = C.loc ? strftime_l( buf, n, fmt, &tm, C.loc ) : strftime( buf, n, fmt, &tm );
    if (!len) *buf = 0;
}

/** the widths that only depend on the font: the widest day heading, month
 * name and weekday abbreviation */
static void fit_font( struct fcft_font* font ) {
    char buf[64];
    C.widest_day = C.widest_month = C.widest_wday = 0;
    for (int d = 1; d <= 366; d++) { /* 2024 is a leap year */
        strtime( buf, sizeof buf, "%A, %e %B", awl_calendar_day_start( 2024, 0, d ) );
        int w = (int)awl_host->text_width( font, buf );
        if (w > C.widest_day) C.widest_day = w;
    }
    for (int m = 0; m < 12; m++) {
        strtime( buf, sizeof buf, "%B", awl_calendar_day_start( 2024, m, 1 ) );
        int w = (int)awl_host->text_width( font, buf );
        if (w > C.widest_month) C.widest_month = w;
    }
    for (int d = 0; d < 7; d++) { /* 2024-01-07 is a Sunday */
        strtime( buf, sizeof buf, "%a", awl_calendar_day_start( 2024, 0, 7 + d ) );
        int w = (int)awl_host->text_width( font, buf );
        if (w > C.widest_wday) C.widest_wday = w;
    }
}

/** the selected day's events, from the cache; ``*n`` of them, NULL while its
 * month is not fetched */
static const awl_cal_event_t** day_events( int* n ) {
    static const awl_cal_event_t* list[256];
    awl_cal_month_t* m = cached( month_key( C.year, C.month ) );
    *n = 0;
    if (!m) return NULL;
    time_t d0 = awl_calendar_day_start( C.year, C.month, C.day ),
           d1 = awl_calendar_day_start( C.year, C.month, C.day + 1 );
    for (int i = 0; i < m->n && *n < (int)(sizeof list / sizeof *list); i++)
        if (touches( &m->events[i], d0, d1 )) list[(*n)++] = &m->events[i];
    return list;
}

static void set_part( int p, int x, int y, int w, int h ) {
    C.part[p] = (box_t){ x, y, w, h };
    C.has[p] = 1;
}

/** the app button's text, NULL while there is no button */
static const char* app_label( void ) {
    const char* const* app = awl_plugin_config()->calendar.app;
    if (C.plain || !app || !app[0]) return NULL;
    const char* label = awl_plugin_config()->calendar.app_label;
    return label && *label ? label : app[0];
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    const awl_popup_config_t* c = &awl_config()->popup;
    if (!font) return 0;
    C.scale = scale;
    if (C.font != font) {
        fit_font( font );
        C.font = font;
    }
    memset( C.has, 0, sizeof C.has );
    const int fh = C.fh = font->height, pad = px( 5 ), gap = C.gap = px( 6 );
    const int bh = C.bh = fh + px( 6 ), cell_h = C.cell_h = fh + px( 6 );
    const int arrow_w = MAX( (int)awl_host->text_width( font, "‹" ), (int)awl_host->text_width( font, "›" ) ) + 2 * pad;
    int cell_w = MAX( (int)awl_host->text_width( font, "00" ), C.widest_wday );
    cell_w += 2 * pad;
    const int month_w = C.widest_month + 2 * pad;
    const int year_w = (int)awl_host->text_width( font, "0000" ) + 2 * pad;
    const int day_w = C.widest_day + 2 * pad;
    const int today_w = (int)awl_host->text_width( font, "Today" ) + 2 * pad;
    const char* app = app_label();
    const int evo_w = app ? (int)awl_host->text_width( font, app ) + 2 * pad : 0;

    /* as wide as the widest of the rows */
    int W = 8 * cell_w;
    int head = 4 * arrow_w + month_w + year_w + gap;
    int dayrow = 2 * arrow_w + day_w + gap + today_w + (app ? gap + evo_w : 0);
    if (head > W) W = head;
    if (dayrow > W) W = dayrow;
    int n = 0;
    const awl_cal_event_t** ev = C.plain ? NULL : day_events( &n );
    C.dot_w = fh / 2;
    C.time_w = MAX( (int)awl_host->text_width( font, "all day" ), (int)awl_host->text_width( font, "00:00–00:00" ) );
    if (!C.plain) {
        /* a summary gets about 30 characters, as the GTK popup gave it */
        int cap = 30 * (int)awl_host->text_width( font, "x" );
        int widest = (int)awl_host->text_width( font, "No events" );
        for (int i = 0; ev && i < n; i++) {
            int sw = (int)awl_host->text_width( font, ev[i]->summary );
            if (*ev[i]->location)
                sw += (int)awl_host->text_width( font, " · " ) + (int)awl_host->text_width( font, ev[i]->location );
            if (sw > cap) sw = cap;
            sw += C.dot_w + gap + C.time_w + gap;
            if (sw > widest) widest = sw;
        }
        if (widest > W) W = widest;
    }
    if (W > max_w) W = max_w;
    cell_w = W / 8;
    const int grid_x = (W - 8 * cell_w) / 2;

    /* the month and the year, each with its arrows */
    int y = 0;
    set_part( P_MPREV, 0, y, arrow_w, bh );
    C.month_label = (box_t){ arrow_w, y, month_w, bh };
    set_part( P_MNEXT, arrow_w + month_w, y, arrow_w, bh );
    int yx = W - 2 * arrow_w - year_w;
    set_part( P_YPREV, yx, y, arrow_w, bh );
    C.year_label = (box_t){ yx + arrow_w, y, year_w, bh };
    set_part( P_YNEXT, yx + arrow_w + year_w, y, arrow_w, bh );
    y += bh + px( 2 );
    /* the weekdays, then the weeks, each with its number in front */
    C.names_y = y;
    y += cell_h;
    C.grid_y = y;
    for (int i = 0; i < 42; i++)
        set_part( P_DAY0 + i, grid_x + (1 + i % 7) * cell_w, y + (i / 7) * cell_h, cell_w, cell_h );
    y += 6 * cell_h + gap;
    /* < the day >, then Today and the app's button on the right */
    C.day_y = y;
    set_part( P_DPREV, 0, y, arrow_w, bh );
    C.day_label = (box_t){ arrow_w, y, day_w, bh };
    set_part( P_DNEXT, arrow_w + day_w, y, arrow_w, bh );
    int bx = W;
    if (app) {
        bx -= evo_w;
        set_part( P_EVO, bx, y, evo_w, bh );
        bx -= gap;
    }
    set_part( P_TODAY, bx - today_w, y, today_w, bh );
    y += bh;

    /* the events: between the configured heights, scrolling beyond */
    C.row_h = fh + px( 4 );
    C.list_h = 0;
    if (!C.plain) {
        y += gap;
        C.list_y = y;
        int natural = (n > 0 ? n : 1) * C.row_h;
        int lo = px( (float)(c->calendar.list_min_height > 0 ? c->calendar.list_min_height : 0) );
        int hi = c->calendar.list_max_height > 0 ? px( (float)c->calendar.list_max_height ) : max_h;
        if (hi < lo) hi = lo;
        C.list_h = natural < lo ? lo : natural > hi ? hi : natural;
        if (y + C.list_h > max_h) C.list_h = max_h - y;
        if (C.list_h < C.row_h) C.list_h = C.row_h;
        set_part( P_LIST, 0, y, W, C.list_h );
        y += C.list_h;
    }
    C.rows_fit = C.list_h / C.row_h > 0 ? C.list_h / C.row_h : 1;
    int top_max = n > C.rows_fit ? n - C.rows_fit : 0;
    if (C.list_top > top_max) C.list_top = top_max;
    if (C.list_top < 0) C.list_top = 0;

    C.w = W;
    C.h = y;
    *w = W;
    *h = y;
    return 1;
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover ) {
    const awl_colors_t* c = &awl_config()->colors;
    const awl_plugin_colors_t* pc = &awl_plugin_config()->colors;
    char buf[128];
    const float r = (float)px( 4 );
    if (hover >= 0 && hover != P_LIST && C.has[hover]) {
        box_t b = C.part[hover];
        awl_fill_shape( pix, (float)(x0 + b.x), (float)(y0 + b.y), (float)b.w, (float)b.h, r, 0, c->popup_hover );
    }

    /* the heading */
    time_t first = awl_calendar_day_start( C.year, C.month, 1 );
    text_center( pix, font, x0, y0, C.part[P_MPREV], "‹", c->popup_fg );
    strtime( buf, sizeof buf, "%B", first );
    text_center( pix, font, x0, y0, C.month_label, buf, c->popup_fg );
    text_center( pix, font, x0, y0, C.part[P_MNEXT], "›", c->popup_fg );
    text_center( pix, font, x0, y0, C.part[P_YPREV], "‹", c->popup_fg );
    snprintf( buf, sizeof buf, "%d", C.year );
    text_center( pix, font, x0, y0, C.year_label, buf, c->popup_fg );
    text_center( pix, font, x0, y0, C.part[P_YNEXT], "›", c->popup_fg );

    /* weekdays and weeks */
    const int ws = week_start(), cw = C.part[P_DAY0].w;
    struct tm t;
    localtime_r( &first, &t );
    const int offset = (t.tm_wday - ws + 7) % 7;
    for (int col = 0; col < 7; col++) {
        strtime( buf, sizeof buf, "%a", awl_calendar_day_start( 2024, 0, 7 + (ws + col) % 7 ) );
        box_t b = { C.part[P_DAY0 + col].x, C.names_y, cw, C.cell_h };
        text_center( pix, font, x0, y0, b, buf, c->popup_dim );
    }
    awl_cal_month_t* events = C.plain ? NULL : cached( month_key( C.year, C.month ) );
    const int today = today_ymd(), thursday = (4 - ws + 7) % 7;
    for (int i = 0; i < 42; i++) {
        box_t b = C.part[P_DAY0 + i];
        time_t d0 = awl_calendar_day_start( C.year, C.month, i - offset + 1 );
        time_t d1 = awl_calendar_day_start( C.year, C.month, i - offset + 2 );
        localtime_r( &d0, &t );
        if (i % 7 == 0) {
            /* ISO week number, of the week's Thursday */
            strtime( buf, sizeof buf, "%V", awl_calendar_day_start( C.year, C.month, i - offset + 1 + thursday ) );
            box_t wb = { b.x - cw, b.y, cw, b.h };
            text_center( pix, font, x0, y0, wb, buf[0] == '0' ? buf + 1 : buf, c->popup_dim );
        }
        const int other = t.tm_mon != C.month, sel = !other && t.tm_mday == C.day;
        if (sel)
            awl_fill_shape( pix, (float)(x0 + b.x + 1), (float)(y0 + b.y), (float)(b.w - 2), (float)b.h, r, 0, pc->calendar_selected );
        uint32_t fg = ymd( &t ) == today ? pc->calendar_today : other ? c->popup_dim : c->popup_fg;
        snprintf( buf, sizeof buf, "%d", t.tm_mday );
        text_center( pix, font, x0, y0, b, buf, fg );
        /* a dot under the days with events */
        int marked = 0;
        for (int k = 0; !other && events && k < events->n && !marked; k++)
            marked = touches( &events->events[k], d0, d1 );
        if (marked) {
            float d = fmaxf( 2.f, (float)px( 3 ) );
            awl_fill_shape( pix, (float)x0 + (float)b.x + ((float)b.w - d) / 2,
                            (float)(y0 + b.y + b.h) - d - (float)px( 1 ), d, d, d / 2, 0, fg );
        }
    }

    /* the day */
    text_center( pix, font, x0, y0, C.part[P_DPREV], "‹", c->popup_fg );
    strtime( buf, sizeof buf, "%A, %e %B", awl_calendar_day_start( C.year, C.month, C.day ) );
    text_center( pix, font, x0, y0, C.day_label, buf, c->popup_fg );
    text_center( pix, font, x0, y0, C.part[P_DNEXT], "›", c->popup_fg );
    text_center( pix, font, x0, y0, C.part[P_TODAY], "Today", c->popup_fg );
    if (C.has[P_EVO]) text_center( pix, font, x0, y0, C.part[P_EVO], app_label(), c->popup_fg );

    /* its events, once fetched */
    if (!C.has[P_LIST]) return;
    int n;
    const awl_cal_event_t** ev = day_events( &n );
    if (!ev) return;
    if (!n) {
        awl_host->text_over( pix, font, x0, y0 + C.list_y, (unsigned)C.w, (unsigned)C.row_h, "No events", c->popup_dim );
        return;
    }
    for (int i = C.list_top, row = 0; i < n && row < C.rows_fit; i++, row++) {
        const awl_cal_event_t* e = ev[i];
        int y = y0 + C.list_y + row * C.row_h, x = x0;
        unsigned rgb;
        uint32_t dot = sscanf( e->color, "#%6x", &rgb ) == 1 ? rgb << 8 | 0xff : 0x888888ff;
        float d = (float)C.dot_w;
        awl_fill_shape( pix, (float)x, (float)y + ((float)C.row_h - d) / 2, d, d, d / 2, 0, dot );
        x += C.dot_w + C.gap;
        if (e->all_day) snprintf( buf, sizeof buf, "all day" );
        else {
            char a[16], b[16];
            strtime( a, sizeof a, "%H:%M", e->start );
            strtime( b, sizeof b, "%H:%M", e->end );
            snprintf( buf, sizeof buf, "%s–%s", a, b );
        }
        awl_host->text_over( pix, font, x, y, (unsigned)C.time_w + 1, (unsigned)C.row_h, buf, c->popup_fg );
        x += C.time_w + C.gap;
        int room = x0 + C.w - x;
        if (room <= 0) continue;
        int sw = (int)awl_host->text_width( font, e->summary );
        awl_host->text_over( pix, font, x, y, (unsigned)(sw < room ? sw + 1 : room), (unsigned)C.row_h, e->summary, c->popup_fg );
        room -= sw;
        if (*e->location && room > (int)awl_host->text_width( font, " · …" )) {
            snprintf( buf, sizeof buf, " · %s", e->location );
            awl_host->text_over( pix, font, x + sw, y, (unsigned)room, (unsigned)C.row_h, buf, c->popup_dim );
        }
    }
}

static int hit( void* data, int x, int y ) {
    for (int p = 0; p < P_COUNT; p++) {
        box_t b = C.part[p];
        if (C.has[p] && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return p;
    }
    return -1;
}

static int box( void* data, int p, int* x, int* y, int* w, int* h ) {
    if (p < 0 || p >= P_COUNT || !C.has[p]) return 0;
    *x = C.part[p].x;
    *y = C.part[p].y;
    *w = C.part[p].w;
    *h = C.part[p].h;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- what it does ---- */

static void redraw( void ) {
    if (C.win) awl_host->window_redraw( C.win );
}

/** selects the day (mktime() normalizes it), fetching its month anew */
static void select_day( int year, int month, int day ) {
    time_t t0 = awl_calendar_day_start( year, month, day );
    struct tm t;
    localtime_r( &t0, &t );
    C.year = t.tm_year + 1900;
    C.month = t.tm_mon;
    C.day = t.tm_mday;
    C.list_top = 0;
    awl_calendar_request( month_key( C.year, C.month ) );
}

static void step_day( int n ) {
    select_day( C.year, C.month, C.day + n );
}

/** keeps the day of the month, or the month's last if it has fewer */
static void step_month( int n ) {
    int key = month_key( C.year, C.month ) + n;
    time_t last = awl_calendar_day_start( key / 12, key % 12 + 1, 0 );
    struct tm t;
    localtime_r( &last, &t );
    select_day( key / 12, key % 12, C.day < t.tm_mday ? C.day : t.tm_mday );
}

static void go_today( void ) {
    int today = today_ymd();
    select_day( today / 10000, today / 100 % 100, today % 100 );
}

static void click( awl_window_t* win, int part, int col, uint32_t button, void* data ) {
    if (button != BTN_LEFT || part < 0) return;
    switch (part) {
    case P_MPREV: step_month( -1 ); break;
    case P_MNEXT: step_month( 1 ); break;
    case P_YPREV: step_month( -12 ); break;
    case P_YNEXT: step_month( 12 ); break;
    case P_DPREV: step_day( -1 ); break;
    case P_DNEXT: step_day( 1 ); break;
    case P_TODAY: go_today(); break;
    case P_EVO:
        awl_host->actions->spawn( &(Arg){ .v = awl_plugin_config()->calendar.app } );
        awl_calpopup_hide();
        return;
    case P_LIST: return;
    default: {
        /* a day, maybe of the months around */
        time_t first = awl_calendar_day_start( C.year, C.month, 1 );
        struct tm t;
        localtime_r( &first, &t );
        select_day( C.year, C.month, part - P_DAY0 - (t.tm_wday - week_start() + 7) % 7 + 1 );
    }
    }
    redraw();
}

/** a wheel notch is a step, a touchpad's 10 units as on the bar (awl_bar.c's
 * SCROLL_LIMIT); over a list longer than shown, rows, else months */
static void scroll( awl_window_t* win, int part, int col, uint32_t orientation, double delta,
                    int32_t value120, void* data ) {
    if (orientation != 0) return;
    C.acc += value120 ? value120 / 120.0 : delta / 10.0;
    int n = (int)C.acc;
    if (!n) return;
    C.acc -= n;
    int nev = 0;
    if (part == P_LIST && day_events( &nev ) && nev > C.rows_fit) {
        C.list_top += n; /* measure() keeps it in range */
    } else {
        step_month( n );
    }
    redraw();
}

int awl_calpopup_start( void ) {
    memset( &C, 0, sizeof C );
    /* the day and month names and the first day of the week, as the user's
     * other programs have them */
    if (!(C.loc = newlocale( LC_TIME_MASK, "", (locale_t)0 )))
        awl_wrn_printf( "calendar: the environment's LC_TIME isn't installed, weeks start on Sunday\n" );
    C.today = today_ymd();
    go_today(); /* warms up: connects to EDS and fetches this month */
    awl_calendar_start();
    awl_calendar_request( month_key( C.year, C.month ) );
    return 0;
}

int awl_calpopup_stop( void ) {
    int stuck = awl_calendar_stop();
    for (awl_cal_month_t* m = C.cache; m;) {
        awl_cal_month_t* next = m->next;
        awl_calendar_month_free( m );
        m = next;
    }
    if (C.loc) freelocale( C.loc );
    awl_host->window_destroy( C.win );
    memset( &C, 0, sizeof C );
    return stuck;
}

/** the window, made on first use; NULL before awl is set up */
static awl_window_t* window( void ) {
    if (C.win) return C.win;
    if (!(C.win = awl_host->window_create())) return NULL;
    awl_host->window_set_painter( C.win, &painter, NULL );
    awl_host->window_set_click( C.win, click, NULL );
    awl_host->window_set_scroll( C.win, scroll, NULL );
    return C.win;
}

void awl_calpopup_show( widget_t* w, int center_x ) {
    awl_window_t* win = window();
    if (!win) return;
    w->popup_window = win;
    Monitor* m = w->bar->m;
    if (awl_host->window_monitor( win ) == m) return;
    /* always opens on today */
    go_today();
    C.acc = 0;
    awl_host->window_redraw( win );
    awl_host->window_show( win, m, center_x );
}

void awl_calpopup_hide( void ) {
    if (C.win) awl_host->window_hide( C.win );
}

void awl_calpopup_scroll( int amount ) {
    if (!C.win || !awl_host->window_monitor( C.win ) || !amount) return;
    step_month( amount > 0 ? 1 : -1 );
    redraw();
}

void awl_calpopup_poll( void ) {
    int changed = 0;
    for (awl_cal_month_t* m = awl_calendar_take(); m;) {
        awl_cal_month_t* next = m->next;
        m->next = NULL;
        if (m->status == CAL_ERR_CANCELLED) {
            awl_calendar_month_free( m );
        } else {
            /* until a query works: EDS may come up later */
            int plain = m->status != CAL_OK;
            changed |= plain != C.plain;
            C.plain = plain;
            if (plain) {
                awl_calendar_month_free( m );
            } else {
                changed |= m->key == month_key( C.year, C.month );
                awl_cal_month_t** p = &C.cache;
                while (*p && (*p)->key != m->key) p = &(*p)->next;
                if (*p) {
                    m->next = (*p)->next;
                    awl_calendar_month_free( *p );
                }
                *p = m;
            }
        }
        m = next;
    }
    int today = today_ymd();
    if (today != C.today) {
        C.today = today;
        changed = 1;
    }
    if (changed && C.win && awl_host->window_monitor( C.win )) redraw();
}
