/* The main thread's half of the items' menus (see menu.h): the menu open, in
 * one of awl's popups, kept across plugin reloads (window_create_kept). One
 * menu at a time; a submenu opens in its place, with a row back to its
 * parent first. Everything here runs on awl's main thread: what the tray
 * thread sends comes through awl_host->post(), awl_window's callbacks are
 * awl's. */

#include "menu.h"
#include "../awl_log.h"
#include "../awl_window_draw.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <linux/input-event-codes.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** a row shown: node -1 is the one back to the parent */
typedef struct {
    int node;
    /** from the content's top, physical pixels; y of rows above top is
     * meaningless */
    int y, h;
} row_t;

static struct {
    awl_window_t* win;
    /** the menu shown or being opened, 0: none */
    uint64_t serial;
    char monitor[64];
    int center_x;
    /** the request whose snapshots are taken */
    uint32_t gen;
    /** NULL while it is being opened */
    awl_tray_menu_snap_t* snap;
    /** the node whose children are shown; the first row shown, of more
     * than fit; scrolling not yet in rows */
    int level, top;
    double acc;
    /** the snapshot's icons decoded at icon_px, by node; tried: decoded or
     * failed */
    pixman_image_t** icons;
    char* tried;
    int icon_px;

    /* measure()'s layout, physical pixels */
    float scale;
    row_t* rows;
    int nrows, shown;
    int w, pad, gap, check_w, icon_w, label_w, arrow_w;
} M;

static int px( float v ) {
    return (int)lroundf( v * M.scale );
}

static int tw( struct fcft_font* font, const char* s ) {
    return (int)awl_host->text_width( font, s );
}

/* ---- drawing ---- */

/** how much of the pixel at x, y the w x h box with radius r covers (as
 * plugins/calpopup.c's) */
static float coverage( float x, float y, float w, float h, float r ) {
    if (w <= 0 || h <= 0) return 0;
    r = fminf( r, fminf( w, h ) / 2 );
    float qx = fabsf( x - w / 2 ) - (w / 2 - r), qy = fabsf( y - h / 2 ) - (h / 2 - r);
    float ox = fmaxf( qx, 0 ), oy = fmaxf( qy, 0 );
    float d = sqrtf( ox * ox + oy * oy ) + fminf( fmaxf( qx, qy ), 0 ) - r;
    return fminf( fmaxf( 0.5f - d, 0 ), 1 );
}

/** c (0xRRGGBBAA) over the rounded box, antialiased; with t > 0 only its
 * outline, t wide */
static void fill_shape( pixman_image_t* pix, float x, float y, float w, float h, float r, float t, uint32_t c ) {
    if (!pix || w <= 0 || h <= 0) return;
    uint32_t* data = pixman_image_get_data( pix );
    int stride = pixman_image_get_stride( pix ) / 4;
    int iw = pixman_image_get_width( pix ), ih = pixman_image_get_height( pix );
    float a = (float)(c & 0xff) / 255.f;
    float col[3] = { (float)(c >> 24) / 255.f * a, (float)((c >> 16) & 0xff) / 255.f * a,
                     (float)((c >> 8) & 0xff) / 255.f * a };
    for (int yy = (int)floorf( y ); yy < (int)ceilf( y + h ); yy++) {
        if (yy < 0 || yy >= ih) continue;
        for (int xx = (int)floorf( x ); xx < (int)ceilf( x + w ); xx++) {
            if (xx < 0 || xx >= iw) continue;
            float fx = (float)xx + 0.5f - x, fy = (float)yy + 0.5f - y;
            float k = coverage( fx, fy, w, h, r );
            if (t > 0) k -= coverage( fx - t, fy - t, w - 2 * t, h - 2 * t, fmaxf( r - t, 0 ) );
            if (k <= 0) continue;
            uint32_t d = data[yy * stride + xx];
            const int shift[4] = { 16, 8, 0, 24 };
            const float src[4] = { col[0] * k, col[1] * k, col[2] * k, a * k };
            uint32_t out = 0;
            for (int i = 0; i < 4; i++) {
                float v = src[i] * 255 + (float)((d >> shift[i]) & 0xff) * (1 - a * k);
                out |= (uint32_t)lroundf( fminf( v, 255 ) ) << shift[i];
            }
            data[yy * stride + xx] = out;
        }
    }
}

/** a check box or radio button of flags, s x s at x, y */
static void draw_check( pixman_image_t* pix, uint32_t flags, float x, float y, float s, uint32_t fg ) {
    const float t = fmaxf( 1, roundf( s / 9 ) );
    const int radio = !!(flags & AWL_TRAY_MENU_RADIO);
    const float r = radio ? s / 2 : s / 5;
    fill_shape( pix, x, y, s, s, r, t, fg );
    const float in = 2 * t + 1;
    if (flags & AWL_TRAY_MENU_CHECKED)
        fill_shape( pix, x + in, y + in, s - 2 * in, s - 2 * in, radio ? (s - 2 * in) / 2 : r / 2, 0, fg );
    else if (flags & AWL_TRAY_MENU_MIXED)
        fill_shape( pix, x + in, y + s / 2 - t, s - 2 * in, 2 * t, 0, 0, fg );
}

/* ---- icons: decoded here, at the monitor's scale; read by the tray thread ---- */

static void size_prepared( GdkPixbufLoader* loader, int w, int h, gpointer data ) {
    int s = *(int*)data;
    if (w <= 0 || h <= 0) return;
    if (w >= h) gdk_pixbuf_loader_set_size( loader, s, h * s / w > 0 ? h * s / w : 1 );
    else gdk_pixbuf_loader_set_size( loader, w * s / h > 0 ? w * s / h : 1, s );
}

/** the icon file's contents as premultiplied ARGB, fit into s x s; NULL if
 * gdk-pixbuf can't read them */
static pixman_image_t* decode( const uint8_t* bytes, size_t len, int s ) {
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    g_signal_connect( loader, "size-prepared", G_CALLBACK( size_prepared ), &s );
    gboolean ok = gdk_pixbuf_loader_write( loader, bytes, len, NULL );
    ok = gdk_pixbuf_loader_close( loader, NULL ) && ok;
    GdkPixbuf* pb = ok ? gdk_pixbuf_loader_get_pixbuf( loader ) : NULL;
    pixman_image_t* img = NULL;
    if (pb && gdk_pixbuf_get_bits_per_sample( pb ) == 8 && gdk_pixbuf_get_n_channels( pb ) >= 3) {
        int w = gdk_pixbuf_get_width( pb ), h = gdk_pixbuf_get_height( pb );
        int n = gdk_pixbuf_get_n_channels( pb ), rs = gdk_pixbuf_get_rowstride( pb );
        int alpha = gdk_pixbuf_get_has_alpha( pb );
        const guint8* src = gdk_pixbuf_read_pixels( pb );
        img = pixman_image_create_bits( PIXMAN_a8r8g8b8, w, h, NULL, 0 );
        uint32_t* dst = img ? pixman_image_get_data( img ) : NULL;
        int ds = img ? pixman_image_get_stride( img ) / 4 : 0;
        for (int y = 0; dst && y < h; y++)
            for (int x = 0; x < w; x++) {
                const guint8* p = src + y * rs + x * n;
                uint32_t a = alpha ? p[3] : 255;
                dst[y * ds + x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 | (p[2] * a / 255);
            }
    }
    g_object_unref( loader );
    return img;
}

static void icons_clear( void ) {
    if (M.icons && M.snap)
        for (int i = 0; i < M.snap->nnodes; i++)
            if (M.icons[i]) pixman_image_unref( M.icons[i] );
    free( M.icons );
    free( M.tried );
    M.icons = NULL;
    M.tried = NULL;
}

/** node i's icon at M.icon_px, NULL if it has none */
static pixman_image_t* icon( int i ) {
    awl_tray_menu_node_t* n = &M.snap->nodes[i];
    if (!n->icon || M.icon_px <= 0) return NULL;
    if (!M.icons) {
        M.icons = calloc( (size_t)M.snap->nnodes, sizeof(*M.icons) );
        M.tried = calloc( (size_t)M.snap->nnodes, 1 );
    }
    if (!M.tried[i]) {
        M.tried[i] = 1;
        M.icons[i] = decode( n->icon, n->icon_len, M.icon_px );
        if (!M.icons[i]) awl_vrb_printf( "tray: menu entry '%s': can't decode its icon\n", n->label );
    }
    return M.icons[i];
}

/* ---- the painter ---- */

static const char BACK[] = "‹", MORE[] = "›";

static const char* row_label( const row_t* r ) {
    return M.snap->nodes[r->node < 0 ? M.level : r->node].label;
}

static uint32_t row_flags( const row_t* r ) {
    return r->node < 0 ? 0 : M.snap->nodes[r->node].flags;
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    if (!M.snap) return 0;
    M.scale = scale;
    const awl_tray_menu_node_t* lv = &M.snap->nodes[M.level];
    const int fh = font->height;
    const int rh = fh + 2 * px( 3 ), sep_h = px( 9 );

    free( M.rows );
    M.nrows = lv->n + (M.level != 0);
    M.rows = calloc( (size_t)(M.nrows ? M.nrows : 1), sizeof(*M.rows) );
    int k = 0;
    if (M.level != 0) M.rows[k++] = (row_t){ -1, 0, rh };
    int checks = 0, icons = 0, subs = 0, label_w = 0, shortcut_w = 0;
    for (int i = 0; i < lv->n; i++) {
        const awl_tray_menu_node_t* n = &M.snap->nodes[lv->first + i];
        int sep = !!(n->flags & AWL_TRAY_MENU_SEPARATOR);
        M.rows[k++] = (row_t){ lv->first + i, 0, sep ? sep_h : rh };
        if (sep) continue;
        checks |= !!(n->flags & (AWL_TRAY_MENU_CHECK | AWL_TRAY_MENU_RADIO));
        icons |= !!n->icon;
        subs |= !!(n->flags & AWL_TRAY_MENU_SUBMENU);
        int lw = tw( font, n->label );
        if (lw > label_w) label_w = lw;
        if (n->shortcut && (lw = tw( font, n->shortcut )) > shortcut_w) shortcut_w = lw;
    }
    if (!M.nrows) return 0;

    M.pad = px( 6 );
    M.gap = px( 8 );
    M.check_w = checks ? (int)lroundf( fh * 0.7f ) + M.gap : 0;
    if (M.icon_px != fh) {
        icons_clear();
        M.icon_px = fh;
    }
    M.icon_w = icons ? M.icon_px + M.gap : 0;
    M.arrow_w = subs ? tw( font, MORE ) + M.gap : 0;
    if (M.level != 0) {
        int bw = tw( font, BACK ) + M.gap + tw( font, lv->label ) - M.check_w - M.icon_w;
        if (bw > label_w) label_w = bw;
    }
    M.label_w = label_w;
    int W = 2 * M.pad + M.check_w + M.icon_w + label_w + (shortcut_w ? 2 * M.gap + shortcut_w : 0) + M.arrow_w;
    if (W < px( 120 )) W = px( 120 );
    if (W > max_w) W = max_w;
    M.w = W;

    /* as many rows as fit from top on, top as low as lets the last one in */
    int total = 0;
    for (int i = 0; i < M.nrows; i++) total += M.rows[i].h;
    if (total <= max_h) M.top = 0;
    else {
        int last_top = M.nrows - 1, hh = M.rows[last_top].h;
        while (last_top > 0 && hh + M.rows[last_top - 1].h <= max_h) hh += M.rows[--last_top].h;
        if (M.top > last_top) M.top = last_top;
        if (M.top < 0) M.top = 0;
    }
    int y = 0;
    M.shown = 0;
    for (int i = M.top; i < M.nrows && y + M.rows[i].h <= max_h; i++, M.shown++) {
        M.rows[i].y = y;
        y += M.rows[i].h;
    }
    if (!M.shown) return 0;
    *w = W;
    *h = y;
    return 1;
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover ) {
    const awl_popup_config_t* c = awl_host->popup_config();
    for (int i = M.top; i < M.top + M.shown; i++) {
        const row_t* r = &M.rows[i];
        const uint32_t flags = row_flags( r );
        const int y = y0 + r->y;
        if (flags & AWL_TRAY_MENU_SEPARATOR) {
            float t = fmaxf( 1, (float)px( 1 ) );
            fill_shape( pix, (float)(x0 + M.pad), (float)y + ((float)r->h - t) / 2, (float)(M.w - 2 * M.pad), t, 0, 0,
                        c->dim );
            continue;
        }
        if (i == hover) fill_shape( pix, (float)x0, (float)y, (float)M.w, (float)r->h, (float)px( 4 ), 0, c->hover );
        const uint32_t fg = (flags & AWL_TRAY_MENU_DISABLED) ? c->dim : c->fg;
        int x = x0 + M.pad;
        if (r->node < 0) {
            /* back to the parent: the arrow where checks and icons go */
            int bw = tw( font, BACK );
            awl_host->text_over( pix, font, x, y, (unsigned)bw, (unsigned)r->h, BACK, c->fg );
            x += bw + M.gap;
            awl_host->text_over( pix, font, x, y, (unsigned)(x0 + M.w - M.pad - x), (unsigned)r->h, row_label( r ),
                                 c->fg );
            continue;
        }
        if (M.check_w && (flags & (AWL_TRAY_MENU_CHECK | AWL_TRAY_MENU_RADIO))) {
            float s = (float)(M.check_w - M.gap);
            draw_check( pix, flags, (float)x, (float)y + ((float)r->h - s) / 2, s, fg );
        }
        x += M.check_w;
        pixman_image_t* img = M.icon_w ? icon( r->node ) : NULL;
        if (img) {
            int iw = pixman_image_get_width( img ), ih = pixman_image_get_height( img );
            pixman_image_composite32( PIXMAN_OP_OVER, img, NULL, pix, 0, 0, 0, 0, x + (M.icon_px - iw) / 2,
                                      y + (r->h - ih) / 2, iw, ih );
        }
        x += M.icon_w;
        const awl_tray_menu_node_t* n = &M.snap->nodes[r->node];
        int right = x0 + M.w - M.pad - M.arrow_w;
        if (n->shortcut) {
            int sw = tw( font, n->shortcut );
            if (right - sw > x + M.gap) {
                awl_host->text_over( pix, font, right - sw, y, (unsigned)sw, (unsigned)r->h, n->shortcut, c->dim );
                right -= sw + 2 * M.gap;
            }
        }
        if (right > x) awl_host->text_over( pix, font, x, y, (unsigned)(right - x), (unsigned)r->h, n->label, fg );
        if (flags & AWL_TRAY_MENU_SUBMENU) {
            int aw = tw( font, MORE );
            awl_host->text_over( pix, font, x0 + M.w - M.pad - aw, y, (unsigned)aw, (unsigned)r->h, MORE, fg );
        }
    }
}

/** the row at px, py, -1 none or one that can't be chosen */
static int hit( void* data, int x, int y ) {
    if (x < 0 || x >= M.w) return -1;
    for (int i = M.top; i < M.top + M.shown; i++) {
        const row_t* r = &M.rows[i];
        if (y >= r->y && y < r->y + r->h)
            return (row_flags( r ) & (AWL_TRAY_MENU_SEPARATOR | AWL_TRAY_MENU_DISABLED)) ? -1 : i;
    }
    return -1;
}

static int box( void* data, int part, int* x, int* y, int* w, int* h ) {
    if (part < M.top || part >= M.top + M.shown) return 0;
    *x = 0;
    *y = M.rows[part].y;
    *w = M.w;
    *h = M.rows[part].h;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- the menu open ---- */

static void reset( void ) {
    icons_clear();
    awl_tray_menu_snap_free( M.snap );
    free( M.rows );
    M.snap = NULL;
    M.rows = NULL;
    M.nrows = M.shown = 0;
    M.serial = 0;
    M.level = M.top = 0;
    M.acc = 0;
}

/** hides it; with events, tells the menu it closed if it was open */
static void close_menu( int events ) {
    uint64_t serial = M.serial;
    int open = M.snap != NULL;
    /* first: hiding calls hidden() */
    reset();
    if (M.win) awl_host->window_hide( M.win );
    if (events && open) awl_tray_menu_event( serial, 0, "closed" );
}

/** awl hides its popups by itself too (a press elsewhere, the monitor
 * going away); close_menu() has reset everything before it hides it */
static void hidden( awl_window_t* win, void* data ) {
    if (M.snap) awl_tray_menu_event( M.serial, 0, "closed" );
    reset();
}

static void show_level( int level ) {
    M.level = level;
    M.top = 0;
    M.acc = 0;
    /* setting it again forgets the hovered row, another one's now; redraws */
    awl_host->window_set_painter( M.win, &painter, NULL );
}

static void click( awl_window_t* win, int row, int col, uint32_t button, void* data ) {
    if (!M.snap || row < 0 || row >= M.nrows || (button != BTN_LEFT && button != BTN_RIGHT)) return;
    const row_t* r = &M.rows[row];
    if (r->node < 0) {
        awl_tray_menu_event( M.serial, M.snap->nodes[M.level].id, "closed" );
        show_level( M.snap->nodes[M.level].parent );
        return;
    }
    const awl_tray_menu_node_t* n = &M.snap->nodes[r->node];
    if (n->flags & (AWL_TRAY_MENU_SEPARATOR | AWL_TRAY_MENU_DISABLED)) return;
    if (n->flags & AWL_TRAY_MENU_SUBMENU) {
        awl_tray_menu_event( M.serial, n->id, "opened" );
        show_level( r->node );
        return;
    }
    awl_tray_menu_event( M.serial, n->id, "clicked" );
    close_menu( 1 );
}

static void scroll( awl_window_t* win, int row, int col, uint32_t orientation, double delta, int32_t value120,
                    void* data ) {
    if (!M.snap || orientation != 0) return;
    M.acc += value120 ? value120 / 120.0 : delta / 10.0;
    int steps = (int)M.acc;
    if (!steps) return;
    M.acc -= steps;
    M.top += steps;
    if (M.top < 0) M.top = 0;
    awl_host->window_redraw( M.win );
}

/** the window, made on first use; NULL before awl is set up */
static awl_window_t* window( void ) {
    if (M.win) return M.win;
    if (!(M.win = awl_host->window_create_kept())) return NULL;
    awl_host->window_set_painter( M.win, &painter, NULL );
    awl_host->window_set_click( M.win, click, NULL );
    awl_host->window_set_scroll( M.win, scroll, NULL );
    awl_host->window_set_hidden( M.win, hidden, NULL );
    return M.win;
}

void awl_tray_menu_window_toggle( uint64_t serial, const char* monitor, int center_x ) {
    static uint32_t gen;
    awl_vrb_printf( "tray: menu %llu: toggle on %s at %d (open: %llu)\n", (unsigned long long)serial, monitor,
                    center_x, (unsigned long long)M.serial );
    if (M.serial == serial && !strcmp( M.monitor, monitor )) {
        close_menu( 1 );
        return;
    }
    close_menu( 1 );
    if (!window()) return;
    M.serial = serial;
    snprintf( M.monitor, sizeof M.monitor, "%s", monitor );
    M.center_x = center_x;
    M.gen = ++gen;
    awl_tray_menu_request( serial, M.gen );
}

void awl_tray_menu_window_snap( awl_tray_menu_snap_t* snap ) {
    awl_vrb_printf( "tray: menu %llu: layout %u of request %u, %d entries (open: %llu, request %u)\n",
                    (unsigned long long)snap->serial, snap->revision, snap->gen, snap->nnodes,
                    (unsigned long long)M.serial, M.gen );
    if (!M.serial || snap->serial != M.serial || snap->gen != M.gen) {
        awl_tray_menu_snap_free( snap );
        return;
    }
    const int opening = !M.snap;
    /* a new layout of the menu shown: the same submenu, if it still has it */
    int32_t level_id = opening ? 0 : M.snap->nodes[M.level].id;
    icons_clear();
    awl_tray_menu_snap_free( M.snap );
    M.snap = snap;
    M.level = 0;
    for (int i = 0; i < snap->nnodes; i++)
        if (snap->nodes[i].id == level_id && (i == 0 || (snap->nodes[i].flags & AWL_TRAY_MENU_SUBMENU))) {
            M.level = i;
            break;
        }
    if (!opening) {
        awl_host->window_redraw( M.win );
        return;
    }
    Monitor* m = awl_host->monitor_named( M.monitor );
    if (!m || !snap->nodes[0].n) {
        if (m) awl_log_printf( "tray: menu has no entries\n" );
        reset();
        return;
    }
    awl_host->window_redraw( M.win );
    awl_host->window_show( M.win, m, M.center_x, NULL, 0 );
    awl_tray_menu_event( M.serial, 0, "opened" );
}

void awl_tray_menu_window_gone( uint64_t serial ) {
    awl_vrb_printf( "tray: menu %llu: gone\n", (unsigned long long)serial );
    if (serial == M.serial) close_menu( 0 );
}
