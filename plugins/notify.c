/* The notifications' windows, see notify.h. Main thread only. */

#include "notify.h"
#include "../icons/icon.h"
#include "../plugins.h"
#include "markup.h"

#include <fcft/fcft.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG (&awl_plugin_config()->notify)

/** what the server keeps open at most (awl_notifications.c) */
enum { MAX_OPEN = 256 };
/** the action buttons at most (the server takes 16 pairs) */
enum { MAX_ACTIONS = 16 };

typedef struct { int x, y, w, h; } box_t;

/** a notification's window and what it shows, copied from its record */
typedef struct {
    uint32_t id;
    uint64_t generation;
    awl_window_t* win;
    awl_notif_urgency_t urgency;
    int value;
    /** awl's own look for it (all 0: the config's) */
    awl_notif_look_t look;
    char* summary;
    /** "{app}" after the summary (show_app), NULL none */
    char* app;
    awl_markup_t body;
    /** the buttons' keys and labels, "default" left out (it is the click
     * on the window): actions[2 * i] the key, [2 * i + 1] the label */
    char** actions;
    int nactions, has_default;
    /** the pointer is on it: its timeout is held (notif_hold) */
    int held;
    /** the image-data at img_px, NULL if none (or not yet converted) */
    pixman_image_t* img;
    int has_image, img_px;

    /* measure()'s, physical pixels from the content's top left */
    struct fcft_font* font;
    /** font in each style (AWL_MARKUP_FONT); [0] is font, not held */
    struct fcft_font* fonts[4];
    float scale;
    /** the body laid out for text_w and laid_lines at generation laid */
    awl_markup_layout_t layout;
    uint64_t laid;
    int laid_w, laid_lines;
    /** the content's height at most (restack(): what room is left), 0 any */
    int max_h;
    int w, h, img_sz, text_x, text_w, sum_h, body_y, bar_y, bar_h;
    box_t act[MAX_ACTIONS];
} note_t;

static struct {
    int started;
    /** newest first, as notif_list() has them */
    note_t* v[MAX_OPEN];
    int n;
    /** "+k more" under the shown ones */
    awl_window_t* more;
    /** restacks after awl hid a window (its monitor went) */
    awl_timer_t* timer;
    /** restack() runs: its own hides don't count */
    int restacking;
} N;

static int px( const note_t* n, float v ) {
    return (int)lroundf( v * n->scale );
}

/** id's record in what the server keeps open now, NULL if it closed */
static const awl_notif_t* record( uint32_t id ) {
    static const awl_notif_t* open[MAX_OPEN];
    int k = awl_host->notif_list( open, MAX_OPEN );
    for (int i = 0; i < k && i < MAX_OPEN; i++)
        if (open[i]->id == id) return open[i];
    return NULL;
}

static uint32_t or_( uint32_t c, uint32_t dflt ) {
    return c ? c : dflt;
}

static uint32_t fg_of( const note_t* n ) {
    return or_( n->look.fg, or_( CFG->urgency[n->urgency].fg, awl_config()->popup.fg ) );
}

static uint32_t frame_of( const note_t* n ) {
    return or_( n->look.frame, or_( CFG->urgency[n->urgency].frame, awl_config()->popup.border ) );
}

/** the body's lines at most, 0 any */
static int max_lines_of( const note_t* n ) {
    return n->look.max_lines < 0 ? 0 : n->look.max_lines ? n->look.max_lines : CFG->max_lines;
}

/* ---- fonts and the layout ---- */

static void fonts_release( note_t* n ) {
    for (int i = 1; i < 4; i++) {
        /* font itself too, when a style didn't load: a reference each */
        if (n->fonts[i]) awl_host->font_release( n->fonts[i] );
        n->fonts[i] = NULL;
    }
    n->fonts[0] = n->font = NULL;
}

/** n's fonts for font (the popups'), the styles from the registry */
static void fonts_for( note_t* n, struct fcft_font* font ) {
    if (font == n->font) return;
    fonts_release( n );
    n->font = n->fonts[0] = font;
    for (int i = 1; i < 4; i++) n->fonts[i] = awl_host->font_style( font, i );
    n->laid = 0;
}

static struct fcft_font* bold( const note_t* n ) {
    return n->fonts[AWL_MARKUP_BOLD] ? n->fonts[AWL_MARKUP_BOLD] : n->font;
}

static struct fcft_font* italic( const note_t* n ) {
    return n->fonts[AWL_MARKUP_ITALIC] ? n->fonts[AWL_MARKUP_ITALIC] : n->font;
}

/** the image-data fitted into the image's square, from the record */
static void image_for( note_t* n ) {
    if (!n->has_image || (n->img && n->img_px == n->img_sz)) return;
    if (n->img) pixman_image_unref( n->img );
    n->img = NULL;
    n->img_px = n->img_sz;
    const awl_notif_t* r = record( n->id );
    if (!r || !r->image_data.data) return;
    const int s = r->image_data.stride;
    n->img = awl_icon_from_rgba( r->image_data.data, (size_t)s * (size_t)r->image_data.h, r->image_data.w,
                                 r->image_data.h, s, r->image_data.has_alpha, 8, r->image_data.channels,
                                 n->img_sz, n->img_sz );
}

/* ---- the painter ---- */

/** the parts' places below the summary, the body cut at lines (0 any, < 0
 * none); returns the content's height, -1 when out of memory */
static int stack( note_t* n, struct fcft_font* font, int lines ) {
    const int gap = px( n, 6 );
    int y = n->sum_h;
    n->body_y = 0;
    if (n->body.len && lines >= 0) {
        if (n->laid != n->generation || n->laid_w != n->text_w || n->laid_lines != lines) {
            if (!awl_markup_layout( &n->layout, &n->body, n->fonts, awl_host->text_width, n->text_w, lines, 0 ))
                return -1;
            n->laid = n->generation;
            n->laid_w = n->text_w;
            n->laid_lines = lines;
        }
        n->body_y = y ? y + px( n, 2 ) : 0;
        y = n->body_y + n->layout.h;
    } else {
        /* none drawn */
        awl_markup_layout_free( &n->layout );
        n->laid = 0;
    }
    n->bar_h = 0;
    if (n->value >= 0) {
        n->bar_h = px( n, (float)CFG->progress_height );
        if (n->bar_h < 1) n->bar_h = 1;
        n->bar_y = y ? y + gap : 0;
        y = n->bar_y + n->bar_h;
    }
    /* the buttons, in rows as wide as the text */
    const int bh = font->height + px( n, 6 ), bpad = px( n, 8 );
    int bx = 0, by = y ? y + gap : 0;
    for (int i = 0; i < n->nactions; i++) {
        int bw = (int)awl_host->text_width( font, n->actions[2 * i + 1] ) + 2 * bpad;
        if (bw > n->text_w) bw = n->text_w;
        if (bx && bx + bw > n->text_w) {
            bx = 0;
            by += bh + gap;
        }
        n->act[i] = (box_t){ n->text_x + bx, by, bw, bh };
        bx += bw + gap;
        y = by + bh;
    }
    if (!y) y = font->height;
    return y > n->img_sz ? y : n->img_sz;
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    note_t* n = data;
    if (!font) return 0;
    if (scale != n->scale) n->laid = 0;
    n->scale = scale;
    fonts_for( n, font );
    const int pad = px( n, (float)awl_config()->popup.padding );
    if (n->max_h > 0 && n->max_h < max_h) max_h = n->max_h;

    n->w = px( n, (float)or_( (uint32_t)n->look.width, (uint32_t)CFG->width ) );
    if (n->w > max_w) n->w = max_w;
    n->img_sz = n->has_image ? px( n, (float)or_( (uint32_t)n->look.image_size, (uint32_t)CFG->image_size ) ) : 0;
    if (n->img_sz > max_h) n->img_sz = max_h;
    image_for( n );
    if (!n->img) n->img_sz = 0;
    n->text_x = n->img_sz ? n->img_sz + pad : 0;
    n->text_w = n->w - n->text_x;
    if (n->text_w < 1) return 0;
    /* the title's line: the summary in bold, the app in italic */
    n->sum_h = *n->summary ? bold( n )->height : 0;
    if (n->app && italic( n )->height > n->sum_h) n->sum_h = italic( n )->height;

    int y = stack( n, font, max_lines_of( n ) );
    if (y > max_h && n->layout.nlines) {
        /* too high: the body's lines that fit, the last ending in "…" */
        const int keep = n->layout.h - (y - max_h);
        int k = (int)n->layout.nlines;
        while (k > 0 && n->layout.lines[k - 1].y + n->layout.lines[k - 1].h > keep) k--;
        y = stack( n, font, k ? k : -1 );
    }
    if (y < 0) return 0;
    n->h = y > max_h ? max_h : y;
    *w = n->w;
    *h = n->h;
    return 1;
}

static int inside( box_t b, int x, int y ) {
    return x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h;
}

/** part 0 is the notification, part i + 1 its action i's button */
static int hit( void* data, int x, int y ) {
    const note_t* n = data;
    for (int i = 0; i < n->nactions; i++)
        if (inside( n->act[i], x, y )) return i + 1;
    return 0;
}

static int part_box( void* data, int part, int* x, int* y, int* w, int* h ) {
    const note_t* n = data;
    box_t b;
    if (part == 0) b = (box_t){ 0, 0, n->w, n->h };
    else if (part > 0 && part <= n->nactions) b = n->act[part - 1];
    else return 0;
    *x = b.x;
    *y = b.y;
    *w = b.w;
    *h = b.h;
    return 1;
}

static void paint_body( note_t* n, pixman_image_t* pix, int x0, int y0, uint32_t fg ) {
    const awl_markup_layout_t* L = &n->layout;
    for (unsigned l = 0; l < L->nlines; l++) {
        const awl_markup_line_t* line = &L->lines[l];
        const int top = y0 + n->body_y + line->y;
        if (n->body_y + line->y >= n->h) break;
        for (unsigned k = line->first; k < line->first + line->n; k++) {
            const awl_markup_piece_t* p = &L->pieces[k];
            struct fcft_font* f = awl_markup_font( n->fonts, p->style );
            const uint32_t c = p->style & AWL_MARKUP_COLOR ? p->color : fg;
            const int x = x0 + n->text_x + p->x;
            const int room = n->text_w - p->x;
            if (room <= 0) continue;
            awl_host->text_over( pix, f, x, top + line->baseline - f->ascent, (unsigned)room, (unsigned)f->height,
                                 p->text, c );
            if (p->style & AWL_MARKUP_UNDERLINE) {
                const int t = f->underline.thickness > 0 ? f->underline.thickness : 1;
                awl_fill_shape( pix, (float)x, (float)(top + line->baseline - f->underline.position), (float)p->w,
                                (float)t, 0, 0, c );
            }
        }
    }
}

/** the summary in bold, then the app in italic, on one baseline; the
 * summary cut first, down to half the line */
static void paint_title( note_t* n, pixman_image_t* pix, int x0, int y0, uint32_t fg ) {
    struct fcft_font *b = bold( n ), *i = italic( n );
    const int asc = n->app && i->ascent > b->ascent ? i->ascent : b->ascent;
    int sum_w = *n->summary ? (int)awl_host->text_width( b, n->summary ) : 0;
    int app_w = 0, sep = 0;
    if (n->app) {
        app_w = (int)awl_host->text_width( i, n->app );
        if (sum_w) sep = (int)awl_host->text_width( i, " " );
        const int room = n->text_w - app_w - sep;
        if (sum_w > room) sum_w = room > n->text_w / 2 ? room : n->text_w / 2;
    } else if (sum_w > n->text_w) {
        sum_w = n->text_w;
    }
    const int x = x0 + n->text_x;
    /* text_over() cuts at a width it doesn't exceed: one more when whole */
    const int whole = *n->summary && sum_w == (int)awl_host->text_width( b, n->summary );
    if (sum_w > 0)
        awl_host->text_over( pix, b, x, y0 + asc - b->ascent, (unsigned)(sum_w + whole), (unsigned)b->height,
                             n->summary, fg );
    const int ax = (sum_w > 0 ? sum_w : 0) + sep, left = n->text_w - ax;
    if (n->app && left > 0)
        awl_host->text_over( pix, i, x + ax, y0 + asc - i->ascent, (unsigned)left, (unsigned)i->height, n->app,
                             fg );
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0, int hover ) {
    note_t* n = data;
    const awl_config_t* c = awl_config();
    const uint32_t fg = fg_of( n );
    if (n->img) {
        const int iw = pixman_image_get_width( n->img ), ih = pixman_image_get_height( n->img );
        pixman_image_composite32( PIXMAN_OP_OVER, n->img, NULL, pix, 0, 0, 0, 0, x0 + (n->img_sz - iw) / 2,
                                  y0 + (n->img_sz - ih) / 2, iw, ih );
    }
    if (n->sum_h) paint_title( n, pix, x0, y0, fg );
    if (n->body.len) paint_body( n, pix, x0, y0, fg );
    if (n->bar_h) {
        const float x = (float)(x0 + n->text_x), y = (float)(y0 + n->bar_y), r = (float)n->bar_h / 2;
        awl_fill_shape( pix, x, y, (float)n->text_w, (float)n->bar_h, r, 0, c->popup.dim );
        if (n->value > 0)
            awl_fill_shape( pix, x, y, (float)n->text_w * (float)n->value / 100.f, (float)n->bar_h, r, 0,
                            or_( n->look.progress, or_( CFG->progress_color, frame_of( n ) ) ) );
    }
    for (int i = 0; i < n->nactions; i++) {
        const box_t b = n->act[i];
        const float r = (float)b.h / 4;
        awl_fill_shape( pix, (float)(x0 + b.x), (float)(y0 + b.y), (float)b.w, (float)b.h, r, 0, c->popup.hover );
        if (hover == i + 1)
            awl_fill_shape( pix, (float)(x0 + b.x), (float)(y0 + b.y), (float)b.w, (float)b.h, r, 0,
                            c->popup.hover );
        const char* label = n->actions[2 * i + 1];
        const int lw = (int)awl_host->text_width( font, label );
        const int lx = lw < b.w ? b.x + (b.w - lw) / 2 : b.x;
        awl_host->text_over( pix, font, x0 + lx, y0 + b.y, (unsigned)(lw < b.w ? lw + 1 : b.w), (unsigned)b.h,
                             label, fg );
    }
    (void)scale;
}

static const awl_window_painter_t painter = {
    .measure = measure, .paint = paint, .hit = hit, .box = part_box,
};

/* ---- input ---- */

static void dismiss_all( void ) {
    uint32_t ids[MAX_OPEN];
    int k = 0;
    /* copied first: a close changes the list */
    for (int i = 0; i < N.n; i++) ids[k++] = N.v[i]->id;
    for (int i = 0; i < k; i++) awl_host->notif_close( ids[i], AWL_NOTIF_DISMISSED );
}

static void click( awl_window_t* win, int row, int col, uint32_t button, void* data ) {
    note_t* n = data;
    (void)win;
    (void)col;
    if (button == BTN_MIDDLE) dismiss_all();
    else if (button == BTN_RIGHT) awl_host->notif_close( n->id, AWL_NOTIF_DISMISSED );
    else if (button != BTN_LEFT) return;
    else if (row >= 1 && row <= n->nactions) awl_host->notif_invoke( n->id, n->actions[2 * (row - 1)], NULL );
    else if (n->has_default) awl_host->notif_invoke( n->id, "default", NULL );
    else awl_host->notif_close( n->id, AWL_NOTIF_DISMISSED );
}

static void hover( awl_window_t* win, int row, int in, void* data ) {
    note_t* n = data;
    (void)win;
    (void)row;
    if (in == n->held) return;
    n->held = in;
    awl_host->notif_hold( n->id, in );
}

/* ---- the windows ---- */

static void restack( void );

/** awl hid a window (its monitor went): placed again once it is done */
static void hidden( awl_window_t* win, void* data ) {
    (void)win;
    (void)data;
    if (!N.restacking && N.timer) awl_host->timer_set( N.timer, 1 );
}

static void restack_timer( void* data ) {
    (void)data;
    restack();
}

/** copies r into n; returns 0 when out of memory */
static int fill( note_t* n, const awl_notif_t* r ) {
    char* summary = strdup( r->summary );
    char* app = NULL;
    if (CFG->show_app && *r->app && (app = malloc( strlen( r->app ) + 3 )))
        sprintf( app, "(%s)", r->app );
    char** actions = calloc( (size_t)(r->nactions ? r->nactions : 1) * 2, sizeof(*actions) );
    awl_markup_t body;
    if (!summary || !actions || !awl_markup_parse( &body, r->body, 1 )) {
        free( summary );
        free( app );
        free( actions );
        return 0;
    }
    int k = 0, has_default = 0;
    for (int i = 0; i < r->nactions && k < MAX_ACTIONS; i++) {
        if (!strcmp( r->actions[2 * i], "default" )) {
            has_default = 1;
            continue;
        }
        actions[2 * k] = strdup( r->actions[2 * i] );
        actions[2 * k + 1] = strdup( r->actions[2 * i + 1] );
        if (!actions[2 * k] || !actions[2 * k + 1]) {
            free( actions[2 * k] );
            free( actions[2 * k + 1] );
            continue;
        }
        k++;
    }
    free( n->summary );
    free( n->app );
    for (int i = 0; i < 2 * n->nactions; i++) free( n->actions[i] );
    free( n->actions );
    awl_markup_free( &n->body );
    n->summary = summary;
    n->app = app;
    n->actions = actions;
    n->nactions = k;
    n->has_default = has_default;
    n->body = body;
    n->generation = r->generation;
    n->urgency = r->urgency <= AWL_NOTIF_CRITICAL ? r->urgency : AWL_NOTIF_NORMAL;
    n->value = r->value;
    n->look = r->look;
    n->has_image = r->image_data.data != NULL;
    if (n->img) pixman_image_unref( n->img );
    n->img = NULL;
    n->img_px = 0;
    n->laid = 0;
    awl_host->window_set_colors( n->win, or_( n->look.bg, CFG->urgency[n->urgency].bg ), fg_of( n ),
                                 or_( n->look.frame, CFG->urgency[n->urgency].frame ) );
    return 1;
}

static void note_free( note_t* n ) {
    if (!n) return;
    if (n->held) awl_host->notif_hold( n->id, 0 );
    awl_host->window_destroy( n->win );
    fonts_release( n );
    awl_markup_layout_free( &n->layout );
    awl_markup_free( &n->body );
    free( n->summary );
    free( n->app );
    for (int i = 0; i < 2 * n->nactions; i++) free( n->actions[i] );
    free( n->actions );
    if (n->img) pixman_image_unref( n->img );
    free( n );
}

static note_t* note_new( const awl_notif_t* r ) {
    note_t* n = calloc( 1, sizeof(*n) );
    if (!n || !(n->win = awl_host->window_create())) {
        free( n );
        return NULL;
    }
    n->id = r->id;
    n->scale = 1;
    if (!fill( n, r )) {
        awl_host->window_destroy( n->win );
        free( n );
        return NULL;
    }
    awl_host->window_set_persistent( n->win, 1 );
    awl_host->window_set_painter( n->win, &painter, n );
    awl_host->window_set_click( n->win, click, n );
    awl_host->window_set_hover( n->win, hover, n );
    awl_host->window_set_hidden( n->win, hidden, NULL );
    return n;
}

/** whether at stacks down (the top's spots) */
static int downward( int at ) {
    return at == AWL_WINDOW_AT_TOP || at == AWL_WINDOW_AT_TOP_LEFT || at == AWL_WINDOW_AT_TOP_RIGHT;
}

/** the "+k more" window's height for the newest k, shown; 0 if it can't */
static int more_show( Monitor* m, int k, int off ) {
    char text[32];
    int x, y, w, h;
    if (!N.more && !(N.more = awl_host->window_create())) return 0;
    snprintf( text, sizeof text, "+%d more", k );
    awl_host->window_set_persistent( N.more, 1 );
    awl_host->window_set_text( N.more, text );
    awl_host->window_show_at( N.more, m, CFG->at, CFG->dx, CFG->dy + (downward( CFG->at ) ? off : -off) );
    return awl_host->window_box( N.more, &x, &y, &w, &h ) ? h : 0;
}

/** caps n's window to h logical pixels, its frame included, shown at off;
 * returns whether anything of it fits */
static int cap( note_t* n, Monitor* m, int h, int off ) {
    int x, y, w, wh;
    if (!awl_host->window_box( n->win, &x, &y, &w, &wh )) return 0;
    /* the frame as drawn, physical pixels, around measure()'s content */
    const int frame = (int)lroundf( (float)wh * n->scale ) - n->h;
    n->max_h = (int)floorf( (float)h * n->scale ) - frame;
    if (n->max_h < 1) {
        n->max_h = 0;
        return 0;
    }
    awl_host->window_redraw( n->win );
    awl_host->window_show_at( n->win, m, CFG->at, CFG->dx, CFG->dy + (downward( CFG->at ) ? off : -off) );
    return 1;
}

/** shows the newest max_shown on the selected monitor, one after the
 * other from the corner, as many as its window area has room for: the
 * last one that fits cut to the room left (the body's lines; "…"), the
 * rest hidden behind "+k more", which always fits */
static void restack( void ) {
    Monitor* m = awl_host->monitor_selected();
    const int down = downward( CFG->at ), max = CFG->max_shown > 0 ? CFG->max_shown : MAX_OPEN;
    int off = 0, shown = 0, full = 0, base = 0, more_h = -1, x, y, w, h, rx = 0, ry = 0, rw = 0, rh = 0;
    N.restacking = 1;
    if (m && N.n) awl_host->window_room( N.v[0]->win, m, &rx, &ry, &rw, &rh );
    for (int i = 0; i < N.n; i++) {
        note_t* n = N.v[i];
        if (!m || shown >= max || full) {
            awl_host->window_hide( n->win );
            continue;
        }
        if (n->max_h) {
            /* whole again, unless it still doesn't fit */
            n->max_h = 0;
            awl_host->window_redraw( n->win );
        }
        awl_host->window_show_at( n->win, m, CFG->at, CFG->dx, CFG->dy + (down ? off : -off) );
        if (!awl_host->window_box( n->win, &x, &y, &w, &h )) continue;
        if (!shown) base = down ? y : y + h;
        /* from where it belongs (the placement keeps it inside, on top of
         * the others) to the room's far edge */
        const int room = down ? ry + rh - (base + off) : base - off - ry;
        int need = h;
        if (i + 1 < N.n) {
            /* what comes after it: at least "+k more" */
            if (more_h < 0) more_h = more_show( m, N.n, off );
            need += CFG->gap + more_h;
        }
        if (need > room) {
            full = 1;
            /* no less than a line of it (as high as "+k more") */
            if (more_h < 0) more_h = more_show( m, N.n, off );
            const int left = room - (need - h);
            if (left < more_h || !cap( n, m, left, off ) || !awl_host->window_box( n->win, &x, &y, &w, &h )) {
                awl_host->window_hide( n->win );
                continue;
            }
        }
        off += h + CFG->gap;
        shown++;
    }
    if (m && shown < N.n) more_show( m, N.n - shown, off );
    else if (N.more) awl_host->window_hide( N.more );
    N.restacking = 0;
}

void awl_notify_changed( void ) {
    static const awl_notif_t* open[MAX_OPEN];
    static note_t* next[MAX_OPEN];
    if (!N.started) return;
    int k = awl_host->notif_list( open, MAX_OPEN );
    if (k > MAX_OPEN) k = MAX_OPEN;
    int nn = 0;
    for (int i = 0; i < k; i++) {
        note_t* n = NULL;
        for (int j = 0; j < N.n; j++)
            if (N.v[j] && N.v[j]->id == open[i]->id) {
                n = N.v[j];
                N.v[j] = NULL;
                break;
            }
        if (!n) {
            if (!(n = note_new( open[i] ))) continue;
        } else if (n->generation != open[i]->generation) {
            if (fill( n, open[i] )) awl_host->window_redraw( n->win );
        }
        next[nn++] = n;
    }
    /* what is left closed */
    for (int j = 0; j < N.n; j++) note_free( N.v[j] );
    memcpy( N.v, next, (size_t)nn * sizeof(*next) );
    N.n = nn;
    restack();
}

void awl_notify_start( void ) {
    if (N.started) return;
    N.started = 1;
    N.timer = awl_host->timer_add( restack_timer, NULL );
    awl_notify_changed();
}

void awl_notify_stop( void ) {
    if (!N.started) return;
    for (int i = 0; i < N.n; i++) note_free( N.v[i] );
    awl_host->window_destroy( N.more );
    awl_host->timer_remove( N.timer );
    memset( &N, 0, sizeof N );
}
