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
enum { MAX_OPEN = 24, MAX_PAST = 24, LEAVE_DELAY = 300 };

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
     * hides it (:c:func:`awl_notify_hidden`) */
    int can_show;
    /** ROW_PAST: its entry and when it closed */
    uint64_t seq;
    char when[16];
    /* measure()'s, physical pixels from the content's top */
    int y, h;
} row_t;

struct awl_notify_menu {
    /** the notifications, whose menu it is */
    awl_notify_t* ns;
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
    /** the history's count, as its row shows it */
    char past_count[16];
};

static int px( awl_notify_menu_t* mn, float v ) {
    return (int)lroundf( v * mn->scale );
}

static const char* dnd_text( void ) {
    return awl_notify_dnd() ? "on" : "off";
}

static const char SHOW[] = "Show";

/* ---- the rows ---- */

static void rows_free( awl_notify_menu_t* mn ) {
    for (int i = 0; i < mn->n; i++) {
        free( mn->rows[i].text );
        free( mn->rows[i].app );
        free( mn->rows[i].key );
    }
    free( mn->rows );
    mn->rows = NULL;
    mn->n = 0;
}

/** s on one line: control characters (a summary's newlines) as spaces */
static char* one_line( const char* s ) {
    char* t = strdup( s );
    if (t)
        for (char* c = t; *c; c++)
            if ((unsigned char)*c < 0x20 || *c == 0x7f) *c = ' ';
    return t;
}

static row_t* row_add( awl_notify_menu_t* mn, kind_t kind, uint32_t id ) {
    row_t* r = &mn->rows[mn->n++];
    *r = (row_t){ .kind = kind, .id = id };
    return r;
}

/** row t's title: summary, else app; "(app)" after a summary */
static int title_set( awl_notify_menu_t* mn, row_t* t, const char* summary, const char* app ) {
    t->text = one_line( *summary ? summary : app );
    if (*summary && *app && (t->app = malloc( strlen( app ) + 3 ))) sprintf( t->app, "(%s)", app );
    if (t->text) return 1;
    free( t->app );
    mn->n--;
    return 0;
}

/** the history's rows after the open ones */
static void past_rows( awl_notify_menu_t* mn, const awl_notif_past_t* const* past, int k ) {
    if (!k) return;
    snprintf( mn->past_count, sizeof mn->past_count, "%d", k );
    row_add( mn, ROW_HISTORY, 0 );
    const time_t now = time( NULL );
    struct tm today, then;
    localtime_r( &now, &today );
    for (int i = 0; i < k; i++) {
        row_t* r = row_add( mn, ROW_PAST, past[i]->id );
        if (!title_set( mn, r, past[i]->summary, past[i]->app )) continue;
        r->seq = past[i]->seq;
        r->can_show = 1;
        const time_t t = (time_t)past[i]->closed;
        localtime_r( &t, &then );
        const int same_day = then.tm_year == today.tm_year && then.tm_yday == today.tm_yday;
        strftime( r->when, sizeof r->when, same_day ? "%H:%M" : "%b %d", &then );
    }
}

/** the rows for what the server keeps open now, then its history */
static void rows_build( awl_notify_menu_t* mn ) {
    const awl_notif_t* open[MAX_OPEN];
    const awl_notif_past_t* past[MAX_PAST];
    rows_free( mn );
    int k = awl_host->notif_list( awl_host->srv, open, MAX_OPEN );
    if (k > MAX_OPEN) k = MAX_OPEN;
    /* the volume's and brightness's feedback isn't for the menu */
    int kept = 0;
    for (int i = 0; i < k; i++)
        if (!(open[i]->flags & AWL_NOTIF_OSD)) open[kept++] = open[i];
    k = kept;
    int np = awl_host->notif_history( awl_host->srv, past, MAX_PAST );
    if (np > MAX_PAST) np = MAX_PAST;
    size_t n = 4 + (size_t)np;
    for (int i = 0; i < k; i++) n += 1 + (size_t)open[i]->nactions;
    if (!(mn->rows = calloc( n, sizeof(*mn->rows) ))) return;
    row_add( mn, ROW_DND, 0 );
    if (!k) {
        row_add( mn, ROW_EMPTY, 0 );
        past_rows( mn, past, np );
        return;
    }
    row_add( mn, ROW_ALL, 0 );
    for (int i = 0; i < k; i++) {
        const awl_notif_t* o = open[i];
        row_t* t = row_add( mn, ROW_TITLE, o->id );
        if (!title_set( mn, t, o->summary, o->app )) continue;
        t->can_show = awl_notify_hidden( mn->ns, o->id );
        for (int a = 0; a < o->nactions; a++) {
            if (!strcmp( o->actions[2 * a], "default" )) {
                t->has_default = 1;
                continue;
            }
            row_t* r = row_add( mn, ROW_ACTION, o->id );
            r->key = strdup( o->actions[2 * a] );
            r->text = one_line( o->actions[2 * a + 1] );
            if (!r->key || !r->text) {
                free( r->key );
                free( r->text );
                mn->n--;
            }
        }
    }
    past_rows( mn, past, np );
}

/* ---- the painter ---- */

static void fonts_release( awl_notify_menu_t* mn ) {
    if (mn->bold) awl_host->font_release( mn->bold );
    if (mn->italic) awl_host->font_release( mn->italic );
    mn->bold = mn->italic = NULL;
    mn->font = NULL;
}

static void fonts_for( awl_notify_menu_t* mn, struct fcft_font* font ) {
    if (font == mn->font) return;
    fonts_release( mn );
    mn->font = font;
    mn->bold = awl_host->font_style( font, AWL_FONT_BOLD );
    mn->italic = awl_host->font_style( font, AWL_FONT_ITALIC );
}

/** what row r has right of its title: its button, a past one's time */
static int row_extra( awl_notify_menu_t* mn, const row_t* r ) {
    if (!r->can_show) return 0;
    return mn->colsp + mn->show_w + (r->kind == ROW_PAST ? mn->colsp + (int)awl_host->text_width( mn->font, r->when ) : 0);
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    awl_notify_menu_t* mn = data;
    if (!font || mn->n < 1) return 0;
    mn->scale = scale;
    fonts_for( mn, font );
    const int fh = mn->bold->height > font->height ? mn->bold->height : font->height;
    mn->pad = px( mn, 6 );
    mn->rowh = fh + 2 * px( mn, 3 );
    mn->indent = px( mn, 16 );
    mn->colsp = px( mn, (float)awl_config()->popup.column_spacing );
    mn->show_w = (int)awl_host->text_width( font, SHOW ) + 2 * px( mn, 8 );
    const int gap = px( mn, 4 );

    /* the rows that fit, with room for the last row when not all do */
    int y = 0, titles = 0;
    for (int i = 0; i < mn->n; i++) {
        row_t* r = &mn->rows[i];
        if ((r->kind == ROW_TITLE || r->kind == ROW_HISTORY) && i > 0) y += gap;
        r->y = y;
        r->h = mn->rowh;
        y += r->h;
        titles += r->kind == ROW_TITLE || r->kind == ROW_PAST;
    }
    mn->shown = mn->n;
    mn->more = 0;
    if (y > max_h) {
        y = 0;
        mn->shown = 0;
        while (mn->shown < mn->n && mn->rows[mn->shown].y + mn->rows[mn->shown].h + mn->rowh <= max_h) mn->shown++;
        for (int i = 0; i < mn->shown; i++) titles -= mn->rows[i].kind == ROW_TITLE || mn->rows[i].kind == ROW_PAST;
        mn->more = titles;
        mn->more_y = mn->shown ? mn->rows[mn->shown - 1].y + mn->rows[mn->shown - 1].h : 0;
        y = mn->more_y + mn->rowh;
        if (y > max_h) return 0;
    }

    /* a fixed width, whatever the rows: what doesn't fit is cut */
    int width = px( mn, (float)CFG->menu_width ) + 2 * mn->pad;
    if (width > max_w) width = max_w;
    if (width < 1) return 0;
    mn->w = width;
    *w = width;
    *h = y;
    return 1;
}

static int clickable( const row_t* r ) {
    return r->kind != ROW_EMPTY;
}

/** row i's "Show" button (a part of its own, mn->n + i), if it has one */
static int show_box( awl_notify_menu_t* mn, int i, int* x, int* y, int* w, int* h ) {
    if (i < 0 || i >= mn->shown || !mn->rows[i].can_show) return 0;
    const row_t* r = &mn->rows[i];
    *w = mn->show_w;
    *x = mn->w - mn->pad - *w;
    if (r->kind == ROW_PAST) *x -= (int)awl_host->text_width( mn->font, r->when ) + mn->colsp;
    *y = r->y + px( mn, 2 );
    *h = r->h - 2 * px( mn, 2 );
    return 1;
}

static int hit( void* data, int x, int y ) {
    awl_notify_menu_t* mn = data;
    if (x < 0 || x >= mn->w) return -1;
    for (int i = 0; i < mn->shown; i++) {
        if (y < mn->rows[i].y || y >= mn->rows[i].y + mn->rows[i].h) continue;
        int bx, by, bw, bh;
        if (show_box( mn, i, &bx, &by, &bw, &bh ) && x >= bx && x < bx + bw) return mn->n + i;
        return clickable( &mn->rows[i] ) ? i : -1;
    }
    return -1;
}

static int part_box( void* data, int part, int* x, int* y, int* w, int* h ) {
    awl_notify_menu_t* mn = data;
    if (part >= mn->n) return show_box( mn, part - mn->n, x, y, w, h );
    if (part < 0 || part >= mn->shown) return 0;
    *x = 0;
    *y = mn->rows[part].y;
    *w = mn->w;
    *h = mn->rows[part].h;
    return 1;
}

/** text in font at x of row r, its line centered in the row, cut at the
 * content's right edge less the padding (or at mn->right) */
static void text_at( awl_notify_menu_t* mn, pixman_image_t* pix, struct fcft_font* f, int x0, int y0, int x, int y, int h,
                     const char* text, uint32_t fg ) {
    const int room = (mn->right ? mn->right : mn->w - mn->pad) - x;
    if (room <= 0) return;
    awl_host->text_over( pix, f, x0 + x, y0 + y + (h - f->height) / 2, (unsigned)room, (unsigned)f->height, text, fg );
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0, int hover ) {
    awl_notify_menu_t* mn = data;
    (void)scale;
    const awl_config_t* c = awl_config();
    const awl_colors_t* mc = &c->colors;
    for (int i = 0; i < mn->shown; i++) {
        const row_t* r = &mn->rows[i];
        uint32_t fg = mc->menu_fg.v;
        if (i == hover) {
            awl_fill_shape( pix, (float)x0, (float)(y0 + r->y), (float)mn->w, (float)r->h, (float)r->h / 4, 0,
                            mc->menu_highlight.v );
            fg = mc->menu_highlight_fg.v;
        }
        const int x = mn->pad;
        switch (r->kind) {
        case ROW_DND:
        case ROW_HISTORY: {
            /* the label cut short of the value at the right */
            const char* s = r->kind == ROW_DND ? dnd_text() : mn->past_count;
            const int sw = (int)awl_host->text_width( font, s );
            text_at( mn, pix, font, x0, y0, mn->w - mn->pad - sw, r->y, r->h, s, i == hover ? fg : mc->menu_dim.v );
            mn->right = mn->w - mn->pad - sw - mn->colsp;
            if (mn->right > x)
                text_at( mn, pix, font, x0, y0, x, r->y, r->h, r->kind == ROW_DND ? "Do not disturb" : "Clear history",
                         fg );
            mn->right = 0;
            break;
        }
        case ROW_ALL: text_at( mn, pix, font, x0, y0, x, r->y, r->h, "Dismiss all", fg ); break;
        case ROW_EMPTY: text_at( mn, pix, font, x0, y0, x, r->y, r->h, "No notifications", mc->menu_dim.v ); break;
        case ROW_ACTION: text_at( mn, pix, font, x0, y0, x + mn->indent, r->y, r->h, r->text, fg ); break;
        case ROW_TITLE:
        case ROW_PAST: {
            /* the history's dim, its time at the right */
            int room = mn->w - 2 * mn->pad;
            if (r->kind == ROW_PAST) {
                if (i != hover) fg = mc->menu_dim.v;
                const int tw = (int)awl_host->text_width( font, r->when );
                text_at( mn, pix, font, x0, y0, mn->w - mn->pad - tw, r->y, r->h, r->when, fg );
            }
            if (r->can_show) {
                /* the button: outlined, filled under the pointer */
                int bx, by, bw, bh;
                show_box( mn, i, &bx, &by, &bw, &bh );
                const int on = hover == mn->n + i;
                awl_fill_shape( pix, (float)(x0 + bx), (float)(y0 + by), (float)bw, (float)bh, (float)bh / 4,
                                on ? 0 : (float)px( mn, 1 ), on ? mc->menu_highlight.v : fg );
                const int sw = (int)awl_host->text_width( font, SHOW );
                text_at( mn, pix, font, x0, y0, bx + (bw - sw) / 2, r->y, r->h, SHOW, on ? mc->menu_highlight_fg.v : fg );
                room -= row_extra( mn, r );
                mn->right = mn->pad + (room > 0 ? room : 0);
            }
            /* the summary cut first, down to half the row, as the windows' */
            int sw = (int)awl_host->text_width( mn->bold, r->text );
            if (r->app) {
                const int aw = (int)awl_host->text_width( mn->italic, " " ) +
                               (int)awl_host->text_width( mn->italic, r->app );
                if (sw > room - aw) sw = room - aw > room / 2 ? room - aw : room / 2;
                if (sw < 0) sw = 0;
                text_at( mn, pix, mn->italic, x0, y0, x + sw + (int)awl_host->text_width( mn->italic, " " ), r->y, r->h,
                         r->app, i == hover ? fg : mc->menu_dim.v );
            } else if (sw > room) {
                sw = room;
            }
            mn->right = 0;
            if (sw > 0) {
                const int whole = sw == (int)awl_host->text_width( mn->bold, r->text );
                awl_host->text_over( pix, mn->bold, x0 + x, y0 + r->y + (r->h - mn->bold->height) / 2,
                                     (unsigned)(sw + whole), (unsigned)mn->bold->height, r->text, fg );
            }
            break;
        }
        }
    }
    if (mn->shown < mn->n) {
        char text[32];
        if (mn->more) snprintf( text, sizeof text, "+%d more", mn->more );
        else snprintf( text, sizeof text, "%s", AWL_WINDOW_ELLIPSIS );
        text_at( mn, pix, font, x0, y0, mn->pad, mn->more_y, mn->rowh, text, mc->menu_dim.v );
    }
}

static const awl_window_painter_t painter = {
    .measure = measure, .paint = paint, .hit = hit, .box = part_box,
};

/* ---- input ---- */

static void click( awl_window_t* win, int row, int col, uint32_t button, void* data ) {
    awl_notify_menu_t* mn = data;
    (void)col;
    if (row >= mn->n && row - mn->n < mn->shown) {
        /* a "Show" button, left: the menu goes, so that the window is
         * seen; the other buttons are its row's */
        row -= mn->n;
        if (button == BTN_LEFT) {
            const row_t r = mn->rows[row];
            awl_host->window_hide( awl_host->srv, win );
            if (r.kind == ROW_PAST) awl_host->notif_show( awl_host->srv, r.seq );
            else awl_notify_reveal( mn->ns, r.id );
            return;
        }
    }
    if (row < 0 || row >= mn->shown) return;
    const row_t* r = &mn->rows[row];
    /* each case returns right after: what it calls may rebuild the rows */
    switch (r->kind) {
    case ROW_DND:
        if (button == BTN_LEFT) awl_notify_set_dnd( mn->ns, !awl_notify_dnd() );
        return;
    case ROW_ALL:
        if (button == BTN_LEFT) awl_notify_dismiss_all( mn->ns );
        return;
    case ROW_TITLE:
        if (button == BTN_RIGHT || (button == BTN_LEFT && !r->has_default)) {
            awl_host->notif_close( awl_host->srv, r->id, AWL_NOTIF_DISMISSED );
        } else if (button == BTN_LEFT) {
            awl_host->notif_invoke( awl_host->srv, r->id, "default" );
            awl_host->window_hide( awl_host->srv, win );
        }
        return;
    case ROW_ACTION:
        if (button == BTN_RIGHT) {
            awl_host->notif_close( awl_host->srv, r->id, AWL_NOTIF_DISMISSED );
        } else if (button == BTN_LEFT) {
            awl_host->notif_invoke( awl_host->srv, r->id, r->key );
            awl_host->window_hide( awl_host->srv, win );
        }
        return;
    case ROW_HISTORY:
        if (button == BTN_LEFT) awl_host->notif_forget( awl_host->srv, 0 );
        return;
    case ROW_PAST:
        if (button == BTN_LEFT || button == BTN_RIGHT) awl_host->notif_forget( awl_host->srv, r->seq );
        return;
    case ROW_EMPTY: return;
    }
}

/** arms the leave timer once the pointer is off the menu and its source,
 * disarms it when it's back */
static void leave_check( awl_notify_menu_t* mn ) {
    if (!mn->leave_timer) return;
    const int off = awl_notify_menu_is_open( mn ) && !mn->inside && !mn->on_source;
    if (off == mn->leaving) return;
    mn->leaving = off;
    awl_host->timer_set( mn->leave_timer, off ? LEAVE_DELAY : 0 );
}

static void leave_fire( void* data ) {
    awl_notify_menu_t* mn = data;
    mn->leaving = 0;
    if (awl_notify_menu_is_open( mn ) && !mn->inside && !mn->on_source) {
        awl_vrb_printf( "notify: menu: pointer left, closing\n" );
        awl_host->window_hide( awl_host->srv, mn->win );
    }
}

static void hover( awl_window_t* win, int row, int inside, void* data ) {
    awl_notify_menu_t* mn = data;
    (void)win;
    (void)row;
    mn->inside = inside;
    /* opened from "+k more": from now on only the menu counts */
    if (inside && mn->on_source < 0) mn->on_source = 0;
    leave_check( mn );
}

void awl_notify_menu_pointer( awl_notify_menu_t* mn, int inside ) {
    if (!mn) return;
    if (mn->on_source < 0 || mn->on_source == !!inside) return;
    mn->on_source = !!inside;
    leave_check( mn );
}

/* ---- the window ---- */

/** closed, by a press elsewhere too */
static void hidden( awl_window_t* win, void* data ) {
    awl_notify_menu_t* mn = data;
    (void)win;
    mn->inside = mn->on_source = 0;
    leave_check( mn );
}

/** the window with the rows for now, NULL if it can't be made */
static awl_window_t* prepare( awl_notify_menu_t* mn ) {
    if (!mn->win) {
        if (!(mn->win = awl_host->window_create( awl_host->srv ))) return NULL;
        awl_host->window_set_menu_style( awl_host->srv, mn->win );
        awl_host->window_set_painter( awl_host->srv, mn->win, &painter, mn );
        awl_host->window_set_click( awl_host->srv, mn->win, click, mn );
        awl_host->window_set_hidden( mn->win, hidden, mn );
        awl_host->window_set_hover( mn->win, hover, mn );
    }
    if (!mn->leave_timer) mn->leave_timer = awl_host->timer_add( awl_host->srv, leave_fire, mn );
    rows_build( mn );
    awl_host->window_redraw( awl_host->srv, mn->win );
    return mn->win;
}

int awl_notify_menu_is_open( awl_notify_menu_t* mn ) {
    return mn && mn->win && awl_host->window_monitor( mn->win );
}

void awl_notify_menu_toggle( awl_notify_menu_t* mn, Monitor* m, int center_x ) {
    if (!mn) return;
    if (awl_notify_menu_is_open( mn )) {
        awl_host->window_hide( awl_host->srv, mn->win );
    } else if (m && prepare( mn )) {
        /* opened by a click on the widget */
        mn->on_source = 1;
        awl_host->window_show( awl_host->srv, mn->win, m, center_x );
        leave_check( mn );
    }
}

void awl_notify_menu_at( awl_notify_menu_t* mn, Monitor* m, int at, int dx, int dy ) {
    if (!mn) return;
    if (awl_notify_menu_is_open( mn )) {
        awl_host->window_hide( awl_host->srv, mn->win );
    } else if (m && prepare( mn )) {
        /* by a click on "+k more", under it now: until the pointer gets
         * into the menu, it stays */
        mn->on_source = -1;
        awl_host->window_show_at( awl_host->srv, mn->win, m, at, dx, dy );
        leave_check( mn );
    }
}

void awl_notify_menu_changed( awl_notify_menu_t* mn ) {
    if (!awl_notify_menu_is_open( mn )) return;
    rows_build( mn );
    awl_host->window_redraw( awl_host->srv, mn->win );
}

awl_notify_menu_t* awl_notify_menu_new( awl_notify_t* ns ) {
    awl_notify_menu_t* mn = calloc( 1, sizeof *mn );
    if (mn) mn->ns = ns;
    return mn;
}

void awl_notify_menu_stop( awl_notify_menu_t* mn ) {
    if (!mn) return;
    awl_host->window_destroy( awl_host->srv, mn->win );
    awl_host->timer_remove( mn->leave_timer );
    rows_free( mn );
    fonts_release( mn );
    free( mn );
}
