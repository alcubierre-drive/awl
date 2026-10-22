/* The notifications' menu, see notify_menu.h. Main thread only. */

#include "notify_menu.h"
#include "notify.h"
#include "../plugins.h"

#include <fcft/fcft.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG (&awl_plugin_config()->notify)

/** what the server keeps open at most (awl_notifications.c) */
enum { MAX_OPEN = 256 };

typedef enum { ROW_DND, ROW_ALL, ROW_TITLE, ROW_ACTION, ROW_EMPTY } kind_t;

/** a row, copied out of the records */
typedef struct {
    kind_t kind;
    uint32_t id;
    /** ROW_TITLE: the summary, "(app)" after it (NULL none), and whether
     * it has a default action; ROW_ACTION: the label and the key */
    char *text, *app, *key;
    int has_default;
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
} M;

static int px( float v ) {
    return (int)lroundf( v * M.scale );
}

static const char* dnd_text( void ) {
    return awl_notify_dnd() ? "on" : "off";
}

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

/** the rows for what the server keeps open now */
static void rows_build( void ) {
    static const awl_notif_t* open[MAX_OPEN];
    rows_free();
    int k = awl_host->notif_list( open, MAX_OPEN );
    if (k > MAX_OPEN) k = MAX_OPEN;
    size_t n = 3;
    for (int i = 0; i < k; i++) n += 1 + (size_t)open[i]->nactions;
    if (!(M.rows = calloc( n, sizeof(*M.rows) ))) return;
    row_add( ROW_DND, 0 );
    if (!k) {
        row_add( ROW_EMPTY, 0 );
        return;
    }
    row_add( ROW_ALL, 0 );
    for (int i = 0; i < k; i++) {
        const awl_notif_t* o = open[i];
        row_t* t = row_add( ROW_TITLE, o->id );
        t->text = one_line( *o->summary ? o->summary : o->app );
        if (*o->summary && *o->app && (t->app = malloc( strlen( o->app ) + 3 )))
            sprintf( t->app, "(%s)", o->app );
        if (!t->text) {
            free( t->app );
            M.n--;
            continue;
        }
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

/** how wide row r's content is */
static int row_width( const row_t* r ) {
    switch (r->kind) {
    case ROW_DND:
        return (int)awl_host->text_width( M.font, "Do not disturb" ) + M.colsp +
               (int)awl_host->text_width( M.font, "off" );
    case ROW_ALL: return (int)awl_host->text_width( M.font, "Dismiss all" );
    case ROW_EMPTY: return (int)awl_host->text_width( M.font, "No notifications" );
    case ROW_ACTION: return M.indent + (int)awl_host->text_width( M.font, r->text );
    case ROW_TITLE:
        return (int)awl_host->text_width( M.bold, r->text ) +
               (r->app ? (int)awl_host->text_width( M.italic, " " ) + (int)awl_host->text_width( M.italic, r->app )
                       : 0);
    }
    return 0;
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
    const int gap = px( 4 );

    /* the rows that fit, with room for the last row when not all do */
    int y = 0, titles = 0;
    for (int i = 0; i < M.n; i++) {
        row_t* r = &M.rows[i];
        if (r->kind == ROW_TITLE && i > 0) y += gap;
        r->y = y;
        r->h = M.rowh;
        y += r->h;
        titles += r->kind == ROW_TITLE;
    }
    M.shown = M.n;
    M.more = 0;
    if (y > max_h) {
        y = 0;
        M.shown = 0;
        while (M.shown < M.n && M.rows[M.shown].y + M.rows[M.shown].h + M.rowh <= max_h) M.shown++;
        for (int i = 0; i < M.shown; i++) titles -= M.rows[i].kind == ROW_TITLE;
        M.more = titles;
        M.more_y = M.shown ? M.rows[M.shown - 1].y + M.rows[M.shown - 1].h : 0;
        y = M.more_y + M.rowh;
        if (y > max_h) return 0;
    }

    int width = 0;
    for (int i = 0; i < M.shown; i++) {
        const int rw = row_width( &M.rows[i] );
        if (rw > width) width = rw;
    }
    width += 2 * M.pad;
    const int cap = px( (float)CFG->width );
    if (width > cap) width = cap;
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

static int hit( void* data, int x, int y ) {
    (void)data;
    if (x < 0 || x >= M.w) return -1;
    for (int i = 0; i < M.shown; i++)
        if (y >= M.rows[i].y && y < M.rows[i].y + M.rows[i].h) return clickable( &M.rows[i] ) ? i : -1;
    return -1;
}

static int part_box( void* data, int part, int* x, int* y, int* w, int* h ) {
    (void)data;
    if (part < 0 || part >= M.shown) return 0;
    *x = 0;
    *y = M.rows[part].y;
    *w = M.w;
    *h = M.rows[part].h;
    return 1;
}

/** text in font at x of row r, its line centered in the row, cut at the
 * content's right edge less the padding */
static void text_at( pixman_image_t* pix, struct fcft_font* f, int x0, int y0, int x, int y, int h,
                     const char* text, uint32_t fg ) {
    const int room = M.w - M.pad - x;
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
        case ROW_DND: {
            text_at( pix, font, x0, y0, x, r->y, r->h, "Do not disturb", fg );
            const char* s = dnd_text();
            const int sw = (int)awl_host->text_width( font, s );
            text_at( pix, font, x0, y0, M.w - M.pad - sw, r->y, r->h, s, i == hover ? fg : mc.dim );
            break;
        }
        case ROW_ALL: text_at( pix, font, x0, y0, x, r->y, r->h, "Dismiss all", fg ); break;
        case ROW_EMPTY: text_at( pix, font, x0, y0, x, r->y, r->h, "No notifications", mc.dim ); break;
        case ROW_ACTION: text_at( pix, font, x0, y0, x + M.indent, r->y, r->h, r->text, fg ); break;
        case ROW_TITLE: {
            /* the summary cut first, down to half the row, as the windows' */
            const int room = M.w - 2 * M.pad;
            int sw = (int)awl_host->text_width( M.bold, r->text );
            if (r->app) {
                const int aw = (int)awl_host->text_width( M.italic, " " ) +
                               (int)awl_host->text_width( M.italic, r->app );
                if (sw > room - aw) sw = room - aw > room / 2 ? room - aw : room / 2;
                text_at( pix, M.italic, x0, y0, x + sw + (int)awl_host->text_width( M.italic, " " ), r->y, r->h,
                         r->app, i == hover ? fg : mc.dim );
            }
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
    case ROW_EMPTY: return;
    }
}

/* ---- the window ---- */

/** closed, by a press elsewhere too: the notifications' windows again */
static void hidden( awl_window_t* win, void* data ) {
    (void)win;
    (void)data;
    awl_notify_restack();
}

/** the window with the rows for now, NULL if it can't be made */
static awl_window_t* prepare( void ) {
    if (!M.win) {
        if (!(M.win = awl_host->window_create())) return NULL;
        awl_host->window_set_menu_style( M.win );
        awl_host->window_set_painter( M.win, &painter, NULL );
        awl_host->window_set_click( M.win, click, NULL );
        awl_host->window_set_hidden( M.win, hidden, NULL );
    }
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
        awl_host->window_show( M.win, m, center_x );
        awl_notify_restack();
    }
}

void awl_notify_menu_at( Monitor* m, int at, int dx, int dy ) {
    if (awl_notify_menu_is_open()) {
        awl_host->window_hide( M.win );
    } else if (m && prepare()) {
        awl_host->window_show_at( M.win, m, at, dx, dy );
        awl_notify_restack();
    }
}

void awl_notify_menu_changed( void ) {
    if (!awl_notify_menu_is_open()) return;
    rows_build();
    awl_host->window_redraw( M.win );
}

void awl_notify_menu_stop( void ) {
    /* no restack from what is being torn down */
    if (M.win) awl_host->window_set_hidden( M.win, NULL, NULL );
    awl_host->window_destroy( M.win );
    rows_free();
    fonts_release();
    memset( &M, 0, sizeof M );
}
