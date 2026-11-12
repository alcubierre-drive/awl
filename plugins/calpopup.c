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
#include <glib.h>

/** the longest zone label and time kept, with the \0 */
#define MAX_STRLEN_ZONES 32

/* on id == NULL return local time */
static int gettime_tz(const char* id, const char* format, char out[], size_t nout);

/* the parts: the buttons, the event list (only scrolls) and the 6 x 7 days */
enum {
    P_MPREV, P_MNEXT, P_YPREV, P_YNEXT, P_DPREV, P_DNEXT, P_TODAY, P_EVO, P_LIST,
    P_DAY0, P_COUNT = P_DAY0 + 42
};

typedef struct { int x, y, w, h; } box_t;

/** the popup; everything in physical pixels, from the content's top left */
struct awl_calpopup {
    /** the root */
    awl_plugin_data_t* p;
    awl_window_t* win;
    /** the thread that fetches the months (plugins/calendar.h) */
    awl_calendar_t* cal;
    /** the selected day's events, day_events()'s */
    const awl_cal_event_t* day_list[256];
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

    /* other time zones: label and time, and measure()'s row for each */
    int n_zones;
    char zones[AWL_PLUGINS_MAX_N_ZONES][2][MAX_STRLEN_ZONES];
    box_t zone_row[AWL_PLUGINS_MAX_N_ZONES];

    /* measure()'s layout */
    box_t part[P_COUNT];
    int has[P_COUNT];
    int w, h, fh, bh, cell_h, grid_y, names_y, day_y, list_y, list_h, row_h, rows_fit;
    int dot_w, time_w, gap;
    box_t month_label, year_label, day_label;
};

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
static int week_start( awl_calpopup_t* cp ) {
    int first = cp->loc ? nl_langinfo_l( _NL_TIME_FIRST_WEEKDAY, cp->loc )[0] : nl_langinfo( _NL_TIME_FIRST_WEEKDAY )[0];
    unsigned origin = (unsigned)(uintptr_t)(cp->loc ? nl_langinfo_l( _NL_TIME_WEEK_1STDAY, cp->loc )
                                                  : nl_langinfo( _NL_TIME_WEEK_1STDAY ));
    int day1 = origin == 19971201 ? 1 : 0; /* else 19971130, a Sunday */
    int s = (day1 + first - 1) % 7;
    return s < 0 ? 0 : s;
}

static awl_cal_month_t* cached( awl_calpopup_t* cp, int key ) {
    for (awl_cal_month_t* m = cp->cache; m; m = m->next)
        if (m->key == key) return m;
    return NULL;
}

/** whether e touches [d0, d1) (its end is exclusive, an instant counts) */
static int touches( const awl_cal_event_t* e, time_t d0, time_t d1 ) {
    time_t end = e->end > e->start ? e->end : e->start + 1;
    return e->start < d1 && end > d0;
}

/* ---- drawing ---- */

static int px( awl_calpopup_t* cp, float v ) {
    return (int)lroundf( v * cp->scale );
}

/** text centered in b (offset by x0, y0), cut with an ellipsis if wider */
static void text_center( pixman_image_t* pix, struct fcft_font* font, int x0, int y0, box_t b,
                         const char* s, uint32_t fg ) {
    int w = (int)awl_host->text_width( font, s );
    int x = w < b.w ? b.x + (b.w - w) / 2 : b.x;
    awl_host->text_over( pix, font, x0 + x, y0 + b.y, (unsigned)(w < b.w ? w + 1 : b.w), (unsigned)b.h, s, fg );
}

static void strtime( awl_calpopup_t* cp, char* buf, size_t n, const char* fmt, time_t t ) {
    struct tm tm;
    localtime_r( &t, &tm );
    size_t len = cp->loc ? strftime_l( buf, n, fmt, &tm, cp->loc ) : strftime( buf, n, fmt, &tm );
    if (!len) *buf = 0;
}

/** the widths that only depend on the font: the widest day heading, month
 * name and weekday abbreviation */
static void fit_font( awl_calpopup_t* cp, struct fcft_font* font ) {
    char buf[64];
    cp->widest_day = cp->widest_month = cp->widest_wday = 0;
    for (int d = 1; d <= 366; d++) { /* 2024 is a leap year */
        strtime( cp, buf, sizeof buf, "%A, %e %B", awl_calendar_day_start( 2024, 0, d ) );
        int w = (int)awl_host->text_width( font, buf );
        if (w > cp->widest_day) cp->widest_day = w;
    }
    for (int m = 0; m < 12; m++) {
        strtime( cp, buf, sizeof buf, "%B", awl_calendar_day_start( 2024, m, 1 ) );
        int w = (int)awl_host->text_width( font, buf );
        if (w > cp->widest_month) cp->widest_month = w;
    }
    for (int d = 0; d < 7; d++) { /* 2024-01-07 is a Sunday */
        strtime( cp, buf, sizeof buf, "%a", awl_calendar_day_start( 2024, 0, 7 + d ) );
        int w = (int)awl_host->text_width( font, buf );
        if (w > cp->widest_wday) cp->widest_wday = w;
    }
}

/** the selected day's events, from the cache; ``*n`` of them, NULL while its
 * month is not fetched */
static const awl_cal_event_t** day_events( awl_calpopup_t* cp, int* n ) {
    awl_cal_month_t* m = cached( cp, month_key( cp->year, cp->month ) );
    *n = 0;
    if (!m) return NULL;
    time_t d0 = awl_calendar_day_start( cp->year, cp->month, cp->day ),
           d1 = awl_calendar_day_start( cp->year, cp->month, cp->day + 1 );
    for (int i = 0; i < m->n && *n < (int)(sizeof cp->day_list / sizeof *cp->day_list); i++)
        if (touches( &m->events[i], d0, d1 )) cp->day_list[(*n)++] = &m->events[i];
    return cp->day_list;
}

static void set_part( awl_calpopup_t* cp, int p, int x, int y, int w, int h ) {
    cp->part[p] = (box_t){ x, y, w, h };
    cp->has[p] = 1;
}

/** the app button's text, NULL while there is no button */
static const char* app_label( awl_calpopup_t* cp ) {
    const char* const* app = cp->p->plugin_config.calendar.app;
    if (cp->plain || !app || !app[0]) return NULL;
    const char* label = cp->p->plugin_config.calendar.app_label;
    return label && *label ? label : app[0];
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    awl_calpopup_t* cp = data;
    const awl_popup_config_t* c = &cp->p->config.popup;
    const awl_plugin_config_t* pc = &cp->p->plugin_config;
    if (!font) return 0;
    cp->scale = scale;
    if (cp->font != font) {
        fit_font( cp, font );
        cp->font = font;
    }
    memset( cp->has, 0, sizeof cp->has );
    const int fh = cp->fh = font->height, pad = px( cp, 5 ), gap = cp->gap = px( cp, 6 );
    const int bh = cp->bh = fh + px( cp, 6 ), cell_h = cp->cell_h = fh + px( cp, 6 );
    const int arrow_w = MAX( (int)awl_host->text_width( font, "‹" ), (int)awl_host->text_width( font, "›" ) ) + 2 * pad;
    int cell_w = MAX( (int)awl_host->text_width( font, "00" ), cp->widest_wday );
    cell_w += 2 * pad;
    const int month_w = cp->widest_month + 2 * pad;
    const int year_w = (int)awl_host->text_width( font, "0000" ) + 2 * pad;
    const int day_w = cp->widest_day + 2 * pad;
    const int today_w = (int)awl_host->text_width( font, "Today" ) + 2 * pad;
    const char* app = app_label( cp );
    const int evo_w = app ? (int)awl_host->text_width( font, app ) + 2 * pad : 0;

    /* as wide as the widest of the rows */
    int W = 8 * cell_w;
    int head = 4 * arrow_w + month_w + year_w + gap;
    int dayrow = 2 * arrow_w + day_w + gap + today_w + (app ? gap + evo_w : 0);
    if (head > W) W = head;
    if (dayrow > W) W = dayrow;
    int n = 0;
    const awl_cal_event_t** ev = cp->plain ? NULL : day_events( cp, &n );
    cp->dot_w = fh / 2;
    cp->time_w = MAX( (int)awl_host->text_width( font, "all day" ), (int)awl_host->text_width( font, "00:00–00:00" ) );
    if (!cp->plain) {
        /* a summary gets about 30 characters, as the GTK popup gave it */
        int cap = 30 * (int)awl_host->text_width( font, "x" );
        int widest = (int)awl_host->text_width( font, "No events" );
        for (int i = 0; ev && i < n; i++) {
            int sw = (int)awl_host->text_width( font, ev[i]->summary );
            if (*ev[i]->location)
                sw += (int)awl_host->text_width( font, " · " ) + (int)awl_host->text_width( font, ev[i]->location );
            if (sw > cap) sw = cap;
            sw += cp->dot_w + gap + cp->time_w + gap;
            if (sw > widest) widest = sw;
        }
        if (widest > W) W = widest;
    }
    if (W > max_w) W = max_w;
    cell_w = W / 8;
    const int grid_x = (W - 8 * cell_w) / 2;

    int y = 0;

    /* the TZs */
    const char* zfmt = pc->calendar.zone_format;
    char tLoc[MAX_STRLEN_ZONES] = {0}; gettime_tz(NULL, zfmt, tLoc, sizeof tLoc);
    cp->n_zones = 0;
    for (int z=0; z<MIN(pc->calendar.n_zones, AWL_PLUGINS_MAX_N_ZONES); ++z) {
        char tId[MAX_STRLEN_ZONES] = {0};
        int err = gettime_tz(pc->calendar.zones[z].zone, zfmt, tId, sizeof tId);
        if (err) continue;
        if (strcmp(tLoc, tId)) {
            const char* id = pc->calendar.zones[z].id ? pc->calendar.zones[z].id : "";
            strncpy(cp->zones[cp->n_zones][0], id, MAX_STRLEN_ZONES - 1);
            cp->zones[cp->n_zones][0][MAX_STRLEN_ZONES - 1] = '\0';
            strcpy(cp->zones[cp->n_zones][1], tId); /* gettime_tz() terminated it */
            cp->zone_row[cp->n_zones] = (box_t){ 0, y, W, bh };
            y += bh + px( cp, 2 );
            cp->n_zones++;
        }
    }

    /* the month and the year, each with its arrows */
    set_part( cp, P_MPREV, 0, y, arrow_w, bh );
    cp->month_label = (box_t){ arrow_w, y, month_w, bh };
    set_part( cp, P_MNEXT, arrow_w + month_w, y, arrow_w, bh );
    int yx = W - 2 * arrow_w - year_w;
    set_part( cp, P_YPREV, yx, y, arrow_w, bh );
    cp->year_label = (box_t){ yx + arrow_w, y, year_w, bh };
    set_part( cp, P_YNEXT, yx + arrow_w + year_w, y, arrow_w, bh );
    y += bh + px( cp, 2 );
    /* the weekdays, then the weeks, each with its number in front */
    cp->names_y = y;
    y += cell_h;
    cp->grid_y = y;
    for (int i = 0; i < 42; i++)
        set_part( cp, P_DAY0 + i, grid_x + (1 + i % 7) * cell_w, y + (i / 7) * cell_h, cell_w, cell_h );
    y += 6 * cell_h + gap;
    /* < the day >, then Today and the app's button on the right */
    cp->day_y = y;
    set_part( cp, P_DPREV, 0, y, arrow_w, bh );
    cp->day_label = (box_t){ arrow_w, y, day_w, bh };
    set_part( cp, P_DNEXT, arrow_w + day_w, y, arrow_w, bh );
    int bx = W;
    if (app) {
        bx -= evo_w;
        set_part( cp, P_EVO, bx, y, evo_w, bh );
        bx -= gap;
    }
    set_part( cp, P_TODAY, bx - today_w, y, today_w, bh );
    y += bh;

    /* the events: between the configured heights, scrolling beyond */
    cp->row_h = fh + px( cp, 4 );
    cp->list_h = 0;
    if (!cp->plain) {
        y += gap;
        cp->list_y = y;
        int natural = (n > 0 ? n : 1) * cp->row_h;
        int lo = px( cp, (float)(c->calendar.list_min_height > 0 ? c->calendar.list_min_height : 0) );
        int hi = c->calendar.list_max_height > 0 ? px( cp, (float)c->calendar.list_max_height ) : max_h;
        if (hi < lo) hi = lo;
        cp->list_h = natural < lo ? lo : natural > hi ? hi : natural;
        if (y + cp->list_h > max_h) cp->list_h = max_h - y;
        if (cp->list_h < cp->row_h) cp->list_h = cp->row_h;
        set_part( cp, P_LIST, 0, y, W, cp->list_h );
        y += cp->list_h;
    }
    cp->rows_fit = cp->list_h / cp->row_h > 0 ? cp->list_h / cp->row_h : 1;
    int top_max = n > cp->rows_fit ? n - cp->rows_fit : 0;
    if (cp->list_top > top_max) cp->list_top = top_max;
    if (cp->list_top < 0) cp->list_top = 0;

    cp->w = W;
    cp->h = y;
    *w = W;
    *h = y;
    return 1;
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover ) {
    awl_calpopup_t* cp = data;
    const awl_colors_t* c = &cp->p->config.colors;
    const awl_plugin_colors_t* pc = &cp->p->plugin_config.colors;
    char buf[128];
    const float r = (float)px( cp, 4 );
    if (hover >= 0 && hover != P_LIST && cp->has[hover]) {
        box_t b = cp->part[hover];
        awl_fill_shape( pix, (float)(x0 + b.x), (float)(y0 + b.y), (float)b.w, (float)b.h, r, 0, c->popup_hover );
    }

    /* the timezones */
    for (int z=0; z<cp->n_zones; ++z) {
        const box_t b = cp->zone_row[z];
        int twl = (int)awl_host->text_width( font, cp->zones[z][0] );
        int twr = (int)awl_host->text_width( font, cp->zones[z][1] );
        awl_host->text_over( pix, font, x0 + b.x, y0 + b.y,
                (unsigned)twl + 1, (unsigned)b.h, cp->zones[z][0], c->popup_dim );
        awl_host->text_over( pix, font, x0 + b.x + b.w - twr, y0 + b.y,
                (unsigned)twr + 1, (unsigned)b.h, cp->zones[z][1], c->popup_fg );
    }

    /* the heading */
    time_t first = awl_calendar_day_start( cp->year, cp->month, 1 );
    text_center( pix, font, x0, y0, cp->part[P_MPREV], "‹", c->popup_fg );
    strtime( cp, buf, sizeof buf, "%B", first );
    text_center( pix, font, x0, y0, cp->month_label, buf, c->popup_fg );
    text_center( pix, font, x0, y0, cp->part[P_MNEXT], "›", c->popup_fg );
    text_center( pix, font, x0, y0, cp->part[P_YPREV], "‹", c->popup_fg );
    snprintf( buf, sizeof buf, "%d", cp->year );
    text_center( pix, font, x0, y0, cp->year_label, buf, c->popup_fg );
    text_center( pix, font, x0, y0, cp->part[P_YNEXT], "›", c->popup_fg );

    /* weekdays and weeks */
    const int ws = week_start( cp ), cw = cp->part[P_DAY0].w;
    struct tm t;
    localtime_r( &first, &t );
    const int offset = (t.tm_wday - ws + 7) % 7;
    for (int col = 0; col < 7; col++) {
        strtime( cp, buf, sizeof buf, "%a", awl_calendar_day_start( 2024, 0, 7 + (ws + col) % 7 ) );
        box_t b = { cp->part[P_DAY0 + col].x, cp->names_y, cw, cp->cell_h };
        text_center( pix, font, x0, y0, b, buf, c->popup_dim );
    }
    awl_cal_month_t* events = cp->plain ? NULL : cached( cp, month_key( cp->year, cp->month ) );
    const int today = today_ymd(), thursday = (4 - ws + 7) % 7;
    for (int i = 0; i < 42; i++) {
        box_t b = cp->part[P_DAY0 + i];
        time_t d0 = awl_calendar_day_start( cp->year, cp->month, i - offset + 1 );
        time_t d1 = awl_calendar_day_start( cp->year, cp->month, i - offset + 2 );
        localtime_r( &d0, &t );
        if (i % 7 == 0) {
            /* ISO week number, of the week's Thursday */
            strtime( cp, buf, sizeof buf, "%V", awl_calendar_day_start( cp->year, cp->month, i - offset + 1 + thursday ) );
            box_t wb = { b.x - cw, b.y, cw, b.h };
            text_center( pix, font, x0, y0, wb, buf[0] == '0' ? buf + 1 : buf, c->popup_dim );
        }
        const int other = t.tm_mon != cp->month, sel = !other && t.tm_mday == cp->day;
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
            float d = fmaxf( 2.f, (float)px( cp, 3 ) );
            awl_fill_shape( pix, (float)x0 + (float)b.x + ((float)b.w - d) / 2,
                            (float)(y0 + b.y + b.h) - d - (float)px( cp, 1 ), d, d, d / 2, 0, fg );
        }
    }

    /* the day */
    text_center( pix, font, x0, y0, cp->part[P_DPREV], "‹", c->popup_fg );
    strtime( cp, buf, sizeof buf, "%A, %e %B", awl_calendar_day_start( cp->year, cp->month, cp->day ) );
    text_center( pix, font, x0, y0, cp->day_label, buf, c->popup_fg );
    text_center( pix, font, x0, y0, cp->part[P_DNEXT], "›", c->popup_fg );
    text_center( pix, font, x0, y0, cp->part[P_TODAY], "Today", c->popup_fg );
    if (cp->has[P_EVO]) text_center( pix, font, x0, y0, cp->part[P_EVO], app_label( cp ), c->popup_fg );

    /* its events, once fetched */
    if (!cp->has[P_LIST]) return;
    int n;
    const awl_cal_event_t** ev = day_events( cp, &n );
    if (!ev) return;
    if (!n) {
        awl_host->text_over( pix, font, x0, y0 + cp->list_y, (unsigned)cp->w, (unsigned)cp->row_h, "No events", c->popup_dim );
        return;
    }
    for (int i = cp->list_top, row = 0; i < n && row < cp->rows_fit; i++, row++) {
        const awl_cal_event_t* e = ev[i];
        int y = y0 + cp->list_y + row * cp->row_h, x = x0;
        unsigned rgb;
        uint32_t dot = sscanf( e->color, "#%6x", &rgb ) == 1 ? rgb << 8 | 0xff : 0x888888ff;
        float d = (float)cp->dot_w;
        awl_fill_shape( pix, (float)x, (float)y + ((float)cp->row_h - d) / 2, d, d, d / 2, 0, dot );
        x += cp->dot_w + cp->gap;
        if (e->all_day) snprintf( buf, sizeof buf, "all day" );
        else {
            char a[16], b[16];
            strtime( cp, a, sizeof a, "%H:%M", e->start );
            strtime( cp, b, sizeof b, "%H:%M", e->end );
            snprintf( buf, sizeof buf, "%s–%s", a, b );
        }
        awl_host->text_over( pix, font, x, y, (unsigned)cp->time_w + 1, (unsigned)cp->row_h, buf, c->popup_fg );
        x += cp->time_w + cp->gap;
        int room = x0 + cp->w - x;
        if (room <= 0) continue;
        int sw = (int)awl_host->text_width( font, e->summary );
        awl_host->text_over( pix, font, x, y, (unsigned)(sw < room ? sw + 1 : room), (unsigned)cp->row_h, e->summary, c->popup_fg );
        room -= sw;
        if (*e->location && room > (int)awl_host->text_width( font, " · …" )) {
            snprintf( buf, sizeof buf, " · %s", e->location );
            awl_host->text_over( pix, font, x + sw, y, (unsigned)room, (unsigned)cp->row_h, buf, c->popup_dim );
        }
    }
}

static int hit( void* data, int x, int y ) {
    awl_calpopup_t* cp = data;
    for (int p = 0; p < P_COUNT; p++) {
        box_t b = cp->part[p];
        if (cp->has[p] && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return p;
    }
    return -1;
}

static int box( void* data, int p, int* x, int* y, int* w, int* h ) {
    awl_calpopup_t* cp = data;
    if (p < 0 || p >= P_COUNT || !cp->has[p]) return 0;
    *x = cp->part[p].x;
    *y = cp->part[p].y;
    *w = cp->part[p].w;
    *h = cp->part[p].h;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- what it does ---- */

static void redraw( awl_calpopup_t* cp ) {
    if (cp->win) awl_host->window_redraw( awl_host->srv, cp->win );
}

/** selects the day (mktime() normalizes it), fetching its month anew */
static void select_day( awl_calpopup_t* cp, int year, int month, int day ) {
    time_t t0 = awl_calendar_day_start( year, month, day );
    struct tm t;
    localtime_r( &t0, &t );
    cp->year = t.tm_year + 1900;
    cp->month = t.tm_mon;
    cp->day = t.tm_mday;
    cp->list_top = 0;
    awl_calendar_request( cp->cal, month_key( cp->year, cp->month ) );
}

static void step_day( awl_calpopup_t* cp, int n ) {
    select_day( cp, cp->year, cp->month, cp->day + n );
}

/** keeps the day of the month, or the month's last if it has fewer */
static void step_month( awl_calpopup_t* cp, int n ) {
    int key = month_key( cp->year, cp->month ) + n;
    time_t last = awl_calendar_day_start( key / 12, key % 12 + 1, 0 );
    struct tm t;
    localtime_r( &last, &t );
    select_day( cp, key / 12, key % 12, cp->day < t.tm_mday ? cp->day : t.tm_mday );
}

static void go_today( awl_calpopup_t* cp ) {
    int today = today_ymd();
    select_day( cp, today / 10000, today / 100 % 100, today % 100 );
}

static void click( awl_window_t* win, int part, int col, uint32_t button, void* data ) {
    awl_calpopup_t* cp = data;
    if (button != BTN_LEFT || part < 0) return;
    switch (part) {
    case P_MPREV: step_month( cp, -1 ); break;
    case P_MNEXT: step_month( cp, 1 ); break;
    case P_YPREV: step_month( cp, -12 ); break;
    case P_YNEXT: step_month( cp, 12 ); break;
    case P_DPREV: step_day( cp, -1 ); break;
    case P_DNEXT: step_day( cp, 1 ); break;
    case P_TODAY: go_today( cp ); break;
    case P_EVO:
        awl_host->actions->spawn( awl_host->srv, &(Arg){ .v = cp->p->plugin_config.calendar.app } );
        awl_calpopup_hide( cp );
        return;
    case P_LIST: return;
    default: {
        /* a day, maybe of the months around */
        time_t first = awl_calendar_day_start( cp->year, cp->month, 1 );
        struct tm t;
        localtime_r( &first, &t );
        select_day( cp, cp->year, cp->month, part - P_DAY0 - (t.tm_wday - week_start( cp ) + 7) % 7 + 1 );
    }
    }
    redraw( cp );
}

/** a wheel notch is a step, a touchpad's 10 units as on the bar (awl_bar.c's
 * SCROLL_LIMIT); over a list longer than shown, rows, else months */
static void scroll( awl_window_t* win, int part, int col, uint32_t orientation, double delta,
                    int32_t value120, void* data ) {
    awl_calpopup_t* cp = data;
    if (orientation != 0) return;
    cp->acc += value120 ? value120 / 120.0 : delta / 10.0;
    int n = (int)cp->acc;
    if (!n) return;
    cp->acc -= n;
    int nev = 0;
    if (part == P_LIST && day_events( cp, &nev ) && nev > cp->rows_fit) {
        cp->list_top += n; /* measure() keeps it in range */
    } else {
        step_month( cp, n );
    }
    redraw( cp );
}

awl_calpopup_t* awl_calpopup_start( awl_plugin_data_t* p ) {
    awl_calpopup_t* cp = calloc( 1, sizeof *cp );
    if (!cp) return NULL;
    cp->p = p;
    /* the day and month names and the first day of the week, as the user's
     * other programs have them */
    if (!(cp->loc = newlocale( LC_TIME_MASK, "", (locale_t)0 )))
        awl_wrn_printf( "calendar: the environment's LC_TIME isn't installed, weeks start on Sunday\n" );
    cp->today = today_ymd();
    cp->cal = awl_calendar_start();
    go_today( cp ); /* warms up: connects to EDS and fetches this month */
    return cp;
}

int awl_calpopup_stop( awl_calpopup_t* cp ) {
    if (!cp) return 0;
    int stuck = awl_calendar_stop( cp->cal );
    for (awl_cal_month_t* m = cp->cache; m;) {
        awl_cal_month_t* next = m->next;
        awl_calendar_month_free( m );
        m = next;
    }
    if (cp->loc) freelocale( cp->loc );
    awl_host->window_destroy( awl_host->srv, cp->win );
    free( cp );
    return stuck;
}

/** the window, made on first use; NULL before awl is set up */
static awl_window_t* window( awl_calpopup_t* cp ) {
    if (cp->win) return cp->win;
    if (!(cp->win = awl_host->window_create( awl_host->srv ))) return NULL;
    awl_host->window_set_painter( awl_host->srv, cp->win, &painter, cp );
    awl_host->window_set_click( awl_host->srv, cp->win, click, cp );
    awl_host->window_set_scroll( awl_host->srv, cp->win, scroll, cp );
    return cp->win;
}

void awl_calpopup_show( awl_calpopup_t* cp, widget_t* w, int center_x ) {
    if (!cp) return;
    awl_window_t* win = window( cp );
    if (!win) return;
    w->popup_window = win;
    Monitor* m = w->bar->m;
    if (awl_host->window_monitor( win ) == m) return;
    /* always opens on today */
    go_today( cp );
    cp->acc = 0;
    awl_host->window_redraw( awl_host->srv, win );
    awl_host->window_show( awl_host->srv, win, m, center_x );
}

void awl_calpopup_hide( awl_calpopup_t* cp ) {
    if (!cp) return;
    if (cp->win) awl_host->window_hide( awl_host->srv, cp->win );
}

void awl_calpopup_scroll( awl_calpopup_t* cp, int amount ) {
    if (!cp) return;
    if (!cp->win || !awl_host->window_monitor( cp->win ) || !amount) return;
    step_month( cp, amount > 0 ? 1 : -1 );
    redraw( cp );
}

void awl_calpopup_poll( awl_calpopup_t* cp ) {
    if (!cp) return;
    int changed = 0;
    for (awl_cal_month_t* m = awl_calendar_take( cp->cal ); m;) {
        awl_cal_month_t* next = m->next;
        m->next = NULL;
        if (m->status == CAL_ERR_CANCELLED) {
            awl_calendar_month_free( m );
        } else {
            /* until a query works: EDS may come up later */
            int plain = m->status != CAL_OK;
            changed |= plain != cp->plain;
            cp->plain = plain;
            if (plain) {
                awl_calendar_month_free( m );
            } else {
                changed |= m->key == month_key( cp->year, cp->month );
                awl_cal_month_t** p = &cp->cache;
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
    if (today != cp->today) {
        cp->today = today;
        changed = 1;
    }
    if (changed && cp->win && awl_host->window_monitor( cp->win )) redraw( cp );
}

static int gettime_tz(const char* id, const char* format, char out[], size_t nout) {
    if (!format || !*format || !nout) return 1;
    GTimeZone *tz = id ? g_time_zone_new_identifier(id) : g_time_zone_new_local();
    if (!tz) return 1;
    GDateTime *now = g_date_time_new_now(tz);
    gchar *formatted = g_date_time_format(now, format);
    int result = 0;
    if (formatted)
        strncpy(out, formatted, nout-1), out[nout-1] = '\0';
    else
        result = 1;
    g_free(formatted);
    g_date_time_unref(now);
    g_time_zone_unref(tz);
    return result;
}
