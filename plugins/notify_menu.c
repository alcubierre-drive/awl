/* The notifications' menu, see notify_menu.h. Main thread only. */

#include "notify_menu.h"
#include "notify.h"
#include "../plugins.h"
#include "../awl_log.h"

#include <fcft/fcft.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CFG (&awl_plugin_config()->notify)

/** what the server keeps open at most (awl_notifications.c); how long the
 * pointer may be off the menu and what opened it before it closes, ms (as
 * the tray's menus) */
enum { MAX_OPEN = 256, MAX_PAST = 100, LEAVE_DELAY = 300 };

typedef enum { ROW_DND, ROW_ALL, ROW_TITLE, ROW_ACTION, ROW_EMPTY, ROW_HISTORY, ROW_PAST } kind_t;

/** a row, copied out of the records */
typedef struct {
    kind_t kind;
    uint32_t id;
    /** ROW_TITLE, ROW_PAST: the summary, "(app)" after it (NULL none),
     * and whether it has a default action; ROW_ACTION: the label and the
     * key */
    char *text, *app, *key;
    int has_default;
    /** a "Show" button: ROW_PAST's all, ROW_TITLE's while do-not-disturb
     * hides it (`awl_notify_hidden()`) */
    int can_show;
    /** ROW_PAST: its entry and when it closed */
    uint64_t seq;
    char when[16];
    /* measure()'s, physical pixels from the content's top */
    int y, h;
} row_t;

static struct {
    awl_window_t* win;
    row_t* rows;
    int n;

    /* measure()'s, physical pixels */
    struct fcft_font *font, *bold, *italic;
    float scale;
    /** the rows that fit, the rest behind a last row at more_y saying how
     * many notifications that is (more), if any */
    int shown, more, more_y;
    int w, rowh, pad, indent, colsp;
    /** a "Show" button's width */
    int show_w;
    /** where text_at() cuts, 0: the right padding */
    int right;

    /** the pointer on the menu, on what opened it (the widget; -1: the "+k
     * more" window, which doesn't say, until the pointer gets into the
     * menu); leaving: leave_timer armed */
    int inside, on_source, leaving;
    awl_timer_t* leave_timer;
} M;

static int px( float v ) {
    return (int)lroundf( v * M.scale );
}

static const char* dnd_text( void ) {
    return awl_notify_dnd() ? "on" : "off";
}

/** the history's count, as its row shows it */
static char past_count[16];

static const char SHOW[] = "Show";

/* ---- the rows ---- */

static void rows_free( void ) {
    for (int i = 0; i < M.n; i++) {
        free( M.rows[i].text );
        free( M.rows[i].app );
        free( M.rows[i].key );
    }
    free( M.rows );
    M.rows = NULL;
    M.n = 0;
}

/** s on one line: control characters (a summary's newlines) as spaces */
static char* one_line( const char* s ) {
    char* t = strdup( s );
    if (t)
        for (char* c = t; *c; c++)
            if ((unsigned char)*c < 0x20 || *c == 0x7f) *c = ' ';
    return t;
}

static row_t* row_add( kind_t kind, uint32_t id ) {
    row_t* r = &M.rows[M.n++];
    *r = (row_t){ .kind = kind, .id = id };
    return r;
}

/** row t's title: summary, else app; "(app)" after a summary */
static int title_set( row_t* t, const char* summary, const char* app ) {
    t->text = one_line( *summary ? summary : app );
    if (*summary && *app && (t->app = malloc( strlen( app ) + 3 ))) sprintf( t->app, "(%s)", app );
    if (t->text) return 1;
    free( t->app );
    M.n--;
    return 0;
}

/** the history's rows after the open ones */
static void past_rows( const awl_notif_past_t* const* past, int k ) {
    if (!k) return;
    snprintf( past_count, sizeof past_count, "%d", k );
    row_add( ROW_HISTORY, 0 );
    const time_t now = time( NULL );
    struct tm today, then;
    localtime_r( &now, &today );
    for (int i = 0; i < k; i++) {
        row_t* r = row_add( ROW_PAST, past[i]->id );
        if (!title_set( r, past[i]->summary, past[i]->app )) continue;
        r->seq = past[i]->seq;
        r->can_show = 1;
        const time_t t = (time_t)past[i]->closed;
        localtime_r( &t, &then );
        const int same_day = then.tm_year == today.tm_year && then.tm_yday == today.tm_yday;
        strftime( r->when, sizeof r->when, same_day ? "%H:%M" : "%b %d", &then );
    }
}

/** the rows for what the server keeps open now, then its history */
static void rows_build( void ) {
    static const awl_notif_t* open[MAX_OPEN];
    static const awl_notif_past_t* past[MAX_PAST];
    rows_free();
    int k = awl_host->notif_list( open, MAX_OPEN );
    if (k > MAX_OPEN) k = MAX_OPEN;
    int np = awl_host->notif_history( past, MAX_PAST );
    if (np > MAX_PAST) np = MAX_PAST;
    size_t n = 4 + (size_t)np;
    for (int i = 0; i < k; i++) n += 1 + (size_t)open[i]->nactions;
    if (!(M.rows = calloc( n, sizeof(*M.rows) ))) return;
    row_add( ROW_DND, 0 );
    if (!k) {
        row_add( ROW_EMPTY, 0 );
        past_rows( past, np );
        return;
    }
    row_add( ROW_ALL, 0 );
    for (int i = 0; i < k; i++) {
        const awl_notif_t* o = open[i];
        row_t* t = row_add( ROW_TITLE, o->id );
        if (!title_set( t, o->summary, o->app )) continue;
        t->can_show = awl_notify_hidden( o->id );
        for (int a = 0; a < o->nactions; a++) {
            if (!strcmp( o->actions[2 * a], "default" )) {
                t->has_default = 1;
                continue;
            }
            row_t* r = row_add( ROW_ACTION, o->id );
            r->key = strdup( o->actions[2 * a] );
            r->text = one_line( o->actions[2 * a + 1] );
            if (!r->key || !r->text) {
                free( r->key );
                free( r->text );
                M.n--;
            }
        }
    }
    past_rows( past, np );
}

/* ---- the painter ---- */

static void fonts_release( void ) {
    if (M.bold) awl_host->font_release( M.bold );
    if (M.italic) awl_host->font_release( M.italic );
    M.bold = M.italic = NULL;
    M.font = NULL;
}

static void fonts_for( struct fcft_font* font ) {
    if (font == M.font) return;
    fonts_release();
    M.font = font;
    M.bold = awl_host->font_style( font, AWL_FONT_BOLD );
    M.italic = awl_host->font_style( font, AWL_FONT_ITALIC );
}

/** what row r has right of its title: its button, a past one's time */
static int row_extra( const row_t* r ) {
    if (!r->can_show) return 0;
    return M.colsp + M.show_w + (r->kind == ROW_PAST ? M.colsp + (int)awl_host->text_width( M.font, r->when ) : 0);
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    (void)data;
    if (!font || M.n < 1) return 0;
    M.scale = scale;
    fonts_for( font );
    const int fh = M.bold->height > font->height ? M.bold->height : font->height;
    M.pad = px( 6 );
    M.rowh = fh + 2 * px( 3 );
    M.indent = px( 16 );
    M.colsp = px( (float)awl_config()->popup.column_spacing );
    M.show_w = (int)awl_host->text_width( font, SHOW ) + 2 * px( 8 );
    const int gap = px( 4 );

    /* the rows that fit, with room for the last row when not all do */
    int y = 0, titles = 0;
    for (int i = 0; i < M.n; i++) {
        row_t* r = &M.rows[i];
        if ((r->kind == ROW_TITLE || r->kind == ROW_HISTORY) && i > 0) y += gap;
        r->y = y;
        r->h = M.rowh;
        y += r->h;
        titles += r->kind == ROW_TITLE || r->kind == ROW_PAST;
    }
    M.shown = M.n;
    M.more = 0;
    if (y > max_h) {
        y = 0;
        M.shown = 0;
        while (M.shown < M.n && M.rows[M.shown].y + M.rows[M.shown].h + M.rowh <= max_h) M.shown++;
        for (int i = 0; i < M.shown; i++) titles -= M.rows[i].kind == ROW_TITLE || M.rows[i].kind == ROW_PAST;
        M.more = titles;
        M.more_y = M.shown ? M.rows[M.shown - 1].y + M.rows[M.shown - 1].h : 0;
        y = M.more_y + M.rowh;
        if (y > max_h) return 0;
    }

    /* a fixed width, whatever the rows: what doesn't fit is cut */
    int width = px( (float)CFG->menu_width ) + 2 * M.pad;
    if (width > max_w) width = max_w;
    if (width < 1) return 0;
    M.w = width;
    *w = width;
    *h = y;
    return 1;
}

static int clickable( const row_t* r ) {
    return r->kind != ROW_EMPTY;
}

/** row i's "Show" button (a part of its own, M.n + i), if it has one */
static int show_box( int i, int* x, int* y, int* w, int* h ) {
    if (i < 0 || i >= M.shown || !M.rows[i].can_show) return 0;
    const row_t* r = &M.rows[i];
    *w = M.show_w;
    *x = M.w - M.pad - *w;
    if (r->kind == ROW_PAST) *x -= (int)awl_host->text_width( M.font, r->when ) + M.colsp;
    *y = r->y + px( 2 );
    *h = r->h - 2 * px( 2 );
    return 1;
}

static int hit( void* data, int x, int y ) {
    (void)data;
    if (x < 0 || x >= M.w) return -1;
    for (int i = 0; i < M.shown; i++) {
        if (y < M.rows[i].y || y >= M.rows[i].y + M.rows[i].h) continue;
        int bx, by, bw, bh;
        if (show_box( i, &bx, &by, &bw, &bh ) && x >= bx && x < bx + bw) return M.n + i;
        return clickable( &M.rows[i] ) ? i : -1;
    }
    return -1;
}

static int part_box( void* data, int part, int* x, int* y, int* w, int* h ) {
    (void)data;
    if (part >= M.n) return show_box( part - M.n, x, y, w, h );
    if (part < 0 || part >= M.shown) return 0;
    *x = 0;
    *y = M.rows[part].y;
    *w = M.w;
    *h = M.rows[part].h;
    return 1;
}

/** text in font at x of row r, its line centered in the row, cut at the
 * content's right edge less the padding (or at M.right) */
static void text_at( pixman_image_t* pix, struct fcft_font* f, int x0, int y0, int x, int y, int h,
                     const char* text, uint32_t fg ) {
    const int room = (M.right ? M.right : M.w - M.pad) - x;
    if (room <= 0) return;
    awl_host->text_over( pix, f, x0 + x, y0 + y + (h - f->height) / 2, (unsigned)room, (unsigned)f->height, text, fg );
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0, int hover ) {
    (void)data;
    (void)scale;
    const awl_config_t* c = awl_config();
    const struct awl_popup_menu_config_t mc = awl_popup_menu_config( &c->popup, c->blur.launcher );
    for (int i = 0; i < M.shown; i++) {
        const row_t* r = &M.rows[i];
        uint32_t fg = mc.fg;
        if (i == hover) {
            awl_fill_shape( pix, (float)x0, (float)(y0 + r->y), (float)M.w, (float)r->h, (float)r->h / 4, 0,
                            mc.highlight );
            fg = mc.highlight_fg;
        }
        const int x = M.pad;
        switch (r->kind) {
        case ROW_DND:
        case ROW_HISTORY: {
            /* the label cut short of the value at the right */
            const char* s = r->kind == ROW_DND ? dnd_text() : past_count;
            const int sw = (int)awl_host->text_width( font, s );
            text_at( pix, font, x0, y0, M.w - M.pad - sw, r->y, r->h, s, i == hover ? fg : mc.dim );
            M.right = M.w - M.pad - sw - M.colsp;
            if (M.right > x)
                text_at( pix, font, x0, y0, x, r->y, r->h, r->kind == ROW_DND ? "Do not disturb" : "Clear history",
                         fg );
            M.right = 0;
            break;
        }
        case ROW_ALL: text_at( pix, font, x0, y0, x, r->y, r->h, "Dismiss all", fg ); break;
        case ROW_EMPTY: text_at( pix, font, x0, y0, x, r->y, r->h, "No notifications", mc.dim ); break;
        case ROW_ACTION: text_at( pix, font, x0, y0, x + M.indent, r->y, r->h, r->text, fg ); break;
        case ROW_TITLE:
        case ROW_PAST: {
            /* the history's dim, its time at the right */
            int room = M.w - 2 * M.pad;
            if (r->kind == ROW_PAST) {
                if (i != hover) fg = mc.dim;
                const int tw = (int)awl_host->text_width( font, r->when );
                text_at( pix, font, x0, y0, M.w - M.pad - tw, r->y, r->h, r->when, fg );
            }
            if (r->can_show) {
                /* the button: outlined, filled under the pointer */
                int bx, by, bw, bh;
                show_box( i, &bx, &by, &bw, &bh );
                const int on = hover == M.n + i;
                awl_fill_shape( pix, (float)(x0 + bx), (float)(y0 + by), (float)bw, (float)bh, (float)bh / 4,
                                on ? 0 : (float)px( 1 ), on ? mc.highlight : fg );
                const int sw = (int)awl_host->text_width( font, SHOW );
                text_at( pix, font, x0, y0, bx + (bw - sw) / 2, r->y, r->h, SHOW, on ? mc.highlight_fg : fg );
                room -= row_extra( r );
                M.right = M.pad + (room > 0 ? room : 0);
            }
            /* the summary cut first, down to half the row, as the windows' */
            int sw = (int)awl_host->text_width( M.bold, r->text );
            if (r->app) {
                const int aw = (int)awl_host->text_width( M.italic, " " ) +
                               (int)awl_host->text_width( M.italic, r->app );
                if (sw > room - aw) sw = room - aw > room / 2 ? room - aw : room / 2;
                if (sw < 0) sw = 0;
                text_at( pix, M.italic, x0, y0, x + sw + (int)awl_host->text_width( M.italic, " " ), r->y, r->h,
                         r->app, i == hover ? fg : mc.dim );
            } else if (sw > room) {
                sw = room;
            }
            M.right = 0;
            if (sw > 0) {
                const int whole = sw == (int)awl_host->text_width( M.bold, r->text );
                awl_host->text_over( pix, M.bold, x0 + x, y0 + r->y + (r->h - M.bold->height) / 2,
                                     (unsigned)(sw + whole), (unsigned)M.bold->height, r->text, fg );
            }
            break;
        }
        }
    }
    if (M.shown < M.n) {
        char text[32];
        if (M.more) snprintf( text, sizeof text, "+%d more", M.more );
        else snprintf( text, sizeof text, "%s", AWL_WINDOW_ELLIPSIS );
        text_at( pix, font, x0, y0, M.pad, M.more_y, M.rowh, text, mc.dim );
    }
}

static const awl_window_painter_t painter = {
    .measure = measure, .paint = paint, .hit = hit, .box = part_box,
};

/* ---- input ---- */

static void click( awl_window_t* win, int row, int col, uint32_t button, void* data ) {
    (void)col;
    (void)data;
    if (row >= M.n && row - M.n < M.shown) {
        /* a "Show" button, left: the menu goes, so that the window is
         * seen; the other buttons are its row's */
        row -= M.n;
        if (button == BTN_LEFT) {
            const row_t r = M.rows[row];
            awl_host->window_hide( win );
            if (r.kind == ROW_PAST) awl_host->notif_show( r.seq );
            else awl_notify_reveal( r.id );
            return;
        }
    }
    if (row < 0 || row >= M.shown) return;
    const row_t* r = &M.rows[row];
    /* each case returns right after: what it calls may rebuild the rows */
    switch (r->kind) {
    case ROW_DND:
        if (button == BTN_LEFT) awl_notify_set_dnd( !awl_notify_dnd() );
        return;
    case ROW_ALL:
        if (button == BTN_LEFT) awl_notify_dismiss_all();
        return;
    case ROW_TITLE:
        if (button == BTN_RIGHT || (button == BTN_LEFT && !r->has_default)) {
            awl_host->notif_close( r->id, AWL_NOTIF_DISMISSED );
        } else if (button == BTN_LEFT) {
            awl_host->notif_invoke( r->id, "default", NULL );
            awl_host->window_hide( win );
        }
        return;
    case ROW_ACTION:
        if (button == BTN_RIGHT) {
            awl_host->notif_close( r->id, AWL_NOTIF_DISMISSED );
        } else if (button == BTN_LEFT) {
            awl_host->notif_invoke( r->id, r->key, NULL );
            awl_host->window_hide( win );
        }
        return;
    case ROW_HISTORY:
        if (button == BTN_LEFT) awl_host->notif_forget( 0 );
        return;
    case ROW_PAST:
        if (button == BTN_LEFT || button == BTN_RIGHT) awl_host->notif_forget( r->seq );
        return;
    case ROW_EMPTY: return;
    }
}

/** arms the leave timer once the pointer is off the menu and its source,
 * disarms it when it's back */
static void leave_check( void ) {
    if (!M.leave_timer) return;
    const int off = awl_notify_menu_is_open() && !M.inside && !M.on_source;
    if (off == M.leaving) return;
    M.leaving = off;
    awl_host->timer_set( M.leave_timer, off ? LEAVE_DELAY : 0 );
}

static void leave_fire( void* data ) {
    (void)data;
    M.leaving = 0;
    if (awl_notify_menu_is_open() && !M.inside && !M.on_source) {
        awl_vrb_printf( "notify: menu: pointer left, closing\n" );
        awl_host->window_hide( M.win );
    }
}

static void hover( awl_window_t* win, int row, int inside, void* data ) {
    (void)win;
    (void)row;
    (void)data;
    M.inside = inside;
    /* opened from "+k more": from now on only the menu counts */
    if (inside && M.on_source < 0) M.on_source = 0;
    leave_check();
}

void awl_notify_menu_pointer( int inside ) {
    if (M.on_source < 0 || M.on_source == !!inside) return;
    M.on_source = !!inside;
    leave_check();
}

/* ---- the window ---- */

/** closed, by a press elsewhere too */
static void hidden( awl_window_t* win, void* data ) {
    (void)win;
    (void)data;
    M.inside = M.on_source = 0;
    leave_check();
}

/** the window with the rows for now, NULL if it can't be made */
static awl_window_t* prepare( void ) {
    if (!M.win) {
        if (!(M.win = awl_host->window_create())) return NULL;
        awl_host->window_set_menu_style( M.win );
        awl_host->window_set_painter( M.win, &painter, NULL );
        awl_host->window_set_click( M.win, click, NULL );
        awl_host->window_set_hidden( M.win, hidden, NULL );
        awl_host->window_set_hover( M.win, hover, NULL );
    }
    if (!M.leave_timer) M.leave_timer = awl_host->timer_add( leave_fire, NULL );
    rows_build();
    awl_host->window_redraw( M.win );
    return M.win;
}

int awl_notify_menu_is_open( void ) {
    return M.win && awl_host->window_monitor( M.win );
}

void awl_notify_menu_toggle( Monitor* m, int center_x ) {
    if (awl_notify_menu_is_open()) {
        awl_host->window_hide( M.win );
    } else if (m && prepare()) {
        /* opened by a click on the widget */
        M.on_source = 1;
        awl_host->window_show( M.win, m, center_x );
        leave_check();
    }
}

void awl_notify_menu_at( Monitor* m, int at, int dx, int dy ) {
    if (awl_notify_menu_is_open()) {
        awl_host->window_hide( M.win );
    } else if (m && prepare()) {
        /* by a click on "+k more", under it now: until the pointer gets
         * into the menu, it stays */
        M.on_source = -1;
        awl_host->window_show_at( M.win, m, at, dx, dy );
        leave_check();
    }
}

void awl_notify_menu_changed( void ) {
    if (!awl_notify_menu_is_open()) return;
    rows_build();
    awl_host->window_redraw( M.win );
}

void awl_notify_menu_stop( void ) {
    awl_host->window_destroy( M.win );
    awl_host->timer_remove( M.leave_timer );
    rows_free();
    fonts_release();
    memset( &M, 0, sizeof M );
}
