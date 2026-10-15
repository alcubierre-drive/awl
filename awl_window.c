#include "awl_window.h"
#include "awl_draw.h"
#include "awl_util.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/** between columns and rows, logical pixels (tray/textpopup.cpp's grid) */
#define COLUMN_SPACING 12
#define ROW_SPACING 2
/** at most this many columns; the rest of a row goes into the last one */
#define MAX_COLUMNS 16

struct awl_window {
    struct wlr_scene_tree* tree; /* at the window's top left */
    struct wlr_scene_blur* blur;
    struct wlr_scene_buffer* buffer;
    awl_window_style_t style;
    char* text;
    /** where it is shown, NULL while hidden */
    Monitor* m;
    int center_x, top;
    /** what the buffer was drawn for; drawn = 0 forces a redraw */
    int drawn;
    float scale;
    struct fcft_font* font;
    /** its size, logical pixels */
    int lw, lh;
};

/** like the bar's buffers in awl.c: read-only for the renderer */
static void buffer_destroy( struct wlr_buffer* wb ) {
    Buffer* b = wl_container_of( wb, b, base );
    free( b );
}

static bool buffer_begin( struct wlr_buffer* wb, uint32_t flags, void** data,
                          uint32_t* format, size_t* stride ) {
    Buffer* b = wl_container_of( wb, b, base );
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
    *data = b->data;
    *stride = b->stride;
    *format = DRM_FORMAT_ARGB8888;
    return true;
}

static void buffer_end( struct wlr_buffer* wb ) {
}

static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .begin_data_ptr_access = buffer_begin,
    .end_data_ptr_access = buffer_end,
};

/** clicks and the pointer go through */
static bool accepts_input( struct wlr_scene_buffer* buffer, double* sx, double* sy ) {
    return false;
}

/** (re)does the blur, under the buffer, from win->style */
static void configure( awl_window_t* win ) {
    if (!win->style.blur) {
        if (win->blur) wlr_scene_node_destroy( &win->blur->node );
        win->blur = NULL;
        return;
    }
    if (!win->blur) {
        win->blur = wlr_scene_blur_create( win->tree, win->lw, win->lh );
        wlr_scene_node_lower_to_bottom( &win->blur->node );
        wlr_scene_blur_set_should_only_blur_bottom_layer( win->blur, 0 );
    }
    wlr_scene_blur_set_corner_radius( win->blur, win->style.radius );
    wlr_scene_blur_set_strength( win->blur, win->style.blur_strength );
    wlr_scene_blur_set_alpha( win->blur, win->style.blur_alpha );
}

awl_window_t* awl_window_create( struct wlr_scene_tree* parent, const awl_window_style_t* style ) {
    awl_window_t* win = ecalloc( 1, sizeof(*win) );
    win->style = *style;
    win->tree = wlr_scene_tree_create( parent );
    win->buffer = wlr_scene_buffer_create( win->tree, NULL );
    win->buffer->point_accepts_input = accepts_input;
    configure( win );
    wlr_scene_node_set_enabled( &win->tree->node, 0 );
    return win;
}

void awl_window_destroy( awl_window_t* win ) {
    if (!win) return;
    wlr_scene_node_destroy( &win->tree->node );
    free( win->text );
    free( win );
}

void awl_window_set_style( awl_window_t* win, const awl_window_style_t* style ) {
    win->style = *style;
    configure( win );
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

void awl_window_set_text( awl_window_t* win, const char* text ) {
    if (!text) text = "";
    if (win->text && !strcmp( win->text, text )) return;
    free( win->text );
    win->text = strdup( text );
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

/** 0xRRGGBBAA as premultiplied floats, 0..1 */
static void unpack( uint32_t c, float out[4] ) {
    float a = (float)(c & 0xff) / 255.f;
    out[0] = (float)((c >> 24) & 0xff) / 255.f * a;
    out[1] = (float)((c >> 16) & 0xff) / 255.f * a;
    out[2] = (float)((c >> 8) & 0xff) / 255.f * a;
    out[3] = a;
}

/** how much of the pixel centered at x, y the w x h box with corner radius r
 * at the origin covers, antialiased over a pixel */
static float coverage( float x, float y, float w, float h, float r ) {
    if (w <= 0 || h <= 0) return 0;
    r = fminf( r, fminf( w, h ) / 2 );
    float qx = fabsf( x - w / 2 ) - (w / 2 - r), qy = fabsf( y - h / 2 ) - (h / 2 - r);
    float ox = fmaxf( qx, 0 ), oy = fmaxf( qy, 0 );
    float d = sqrtf( ox * ox + oy * oy ) + fminf( fmaxf( qx, qy ), 0 ) - r;
    return fminf( fmaxf( 0.5f - d, 0 ), 1 );
}

/** the background: bg inside the border, the border around it */
static void draw_frame( uint32_t* data, size_t stride, int w, int h, float bw, float r,
                        uint32_t bg, uint32_t border ) {
    float b[4], g[4];
    unpack( bg, g );
    unpack( border, b );
    float ri = fmaxf( r - bw, 0 );
    for (int y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)((char*)data + (size_t)y * stride);
        for (int x = 0; x < w; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float co = coverage( px, py, (float)w, (float)h, r );
            float ci = bw > 0 ? coverage( px - bw, py - bw, (float)w - 2 * bw, (float)h - 2 * bw, ri ) : co;
            float kb = co - ci, out[4];
            for (int k = 0; k < 4; k++)
                out[k] = fminf( b[k] * kb + g[k] * ci, 1 );
            row[x] = (uint32_t)lroundf( out[3] * 255 ) << 24 | (uint32_t)lroundf( out[0] * 255 ) << 16 |
                     (uint32_t)lroundf( out[1] * 255 ) << 8 | (uint32_t)lroundf( out[2] * 255 );
        }
    }
}

/** calls f for each cell of text with its row and column; from the last
 * column of MAX_COLUMNS on, the rest of the row is one cell */
static void each_cell( const char* text, void (*f)( void* data, int row, int col, const char* s, size_t n ),
                       void* data ) {
    int row = 0, col = 0;
    for (const char* p = text;;) {
        size_t n = strcspn( p, col == MAX_COLUMNS - 1 ? "\n" : "\t\n" );
        char sep = p[n];
        f( data, row, col, p, n );
        if (!sep) break;
        p += n + 1;
        if (sep == '\n') {
            row++;
            col = 0;
        } else
            col++;
    }
}

typedef struct {
    awl_draw_t* drw;
    int widths[MAX_COLUMNS];
    int ncols, nrows;
    /** drawing: where the rows and columns start, and the color */
    int x0, y0, colsp, rowsp;
    uint32_t fg;
} layout_t;

static void measure_cell( void* data, int row, int col, const char* s, size_t n ) {
    layout_t* L = data;
    char buf[1024];
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy( buf, s, n );
    buf[n] = 0;
    int w = (int)awl_draw_font_getwidth( L->drw, buf );
    if (w > L->widths[col]) L->widths[col] = w;
    if (col + 1 > L->ncols) L->ncols = col + 1;
    if (row + 1 > L->nrows) L->nrows = row + 1;
}

static void draw_cell( void* data, int row, int col, const char* s, size_t n ) {
    layout_t* L = data;
    char buf[1024];
    if (!n) return;
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy( buf, s, n );
    buf[n] = 0;
    int x = L->x0;
    for (int c = 0; c < col; c++) x += L->widths[c] + L->colsp;
    int h = L->drw->font->height;
    awl_draw_text_over( L->drw, x, L->y0 + row * (h + L->rowsp), (unsigned)L->widths[col], (unsigned)h,
                        buf, L->fg );
}

/** returns whether there is anything to show */
static int draw( awl_window_t* win ) {
    Monitor* m = win->m;
    awl_draw_t* drw = m->drw;
    const float s = m->wlr_output->scale;
    if (!drw || !drw->font) return 0;

    layout_t L = { .drw = drw };
    each_cell( win->text ? win->text : "", measure_cell, &L );
    L.colsp = (int)lroundf( COLUMN_SPACING * s );
    L.rowsp = (int)lroundf( ROW_SPACING * s );
    const int bw = (int)lroundf( fmaxf( (float)win->style.border_width, 0 ) * s );
    const int pad = (int)lroundf( fmaxf( (float)win->style.padding, 0 ) * s );
    L.x0 = L.y0 = bw + pad;
    L.fg = win->style.fg;

    int width = 2 * L.x0 + (L.ncols - 1) * L.colsp;
    for (int c = 0; c < L.ncols; c++) width += L.widths[c];
    int height = 2 * L.y0 + L.nrows * drw->font->height + (L.nrows - 1) * L.rowsp;
    /* wider than the screen: the last column is cut off */
    const int maxw = (int)(m->m.width * s);
    if (width > maxw) {
        L.widths[L.ncols - 1] -= width - maxw;
        if (L.widths[L.ncols - 1] < 0) L.widths[L.ncols - 1] = 0;
        width = maxw;
    }
    if (width <= 0 || height <= 0) return 0;

    int32_t stride = awl_draw_stride( width );
    Buffer* b = ecalloc( 1, sizeof(Buffer) + (size_t)stride * height );
    b->stride = stride;
    b->w = width;
    b->h = height;
    wlr_buffer_init( &b->base, &buffer_impl, width, height );

    draw_frame( b->data, b->stride, width, height, (float)bw,
                fmaxf( (float)win->style.radius, 0 ) * s, win->style.bg, win->style.border );
    awl_draw_prepare_drawing( drw, width, height, b->data, stride );
    each_cell( win->text ? win->text : "", draw_cell, &L );
    awl_draw_finish_drawing( drw );

    win->lw = (int)lroundf( width / s );
    win->lh = (int)lroundf( height / s );
    wlr_scene_buffer_set_buffer( win->buffer, &b->base );
    wlr_scene_buffer_set_dest_size( win->buffer, win->lw, win->lh );
    wlr_buffer_drop( &b->base ); /* the scene holds it now */
    if (win->blur) wlr_scene_blur_set_size( win->blur, win->lw, win->lh );
    return 1;
}

static void place( awl_window_t* win ) {
    Monitor* m = win->m;
    int x = win->center_x - win->lw / 2;
    if (x > m->m.width - win->lw) x = m->m.width - win->lw;
    if (x < 0) x = 0;
    int y = win->top ? m->w.y : m->w.y + m->w.height - win->lh;
    wlr_scene_node_set_position( &win->tree->node, m->m.x + x, y );
}

void awl_window_update( awl_window_t* win, Monitor* m ) {
    if (!win || win->m != m || !m) return;
    if (!m->wlr_output->enabled || !m->drw || !m->drw->font) {
        wlr_scene_node_set_enabled( &win->tree->node, 0 );
        return;
    }
    const float s = m->wlr_output->scale;
    if (!win->drawn || win->scale != s || win->font != m->drw->font) {
        win->drawn = draw( win );
        win->scale = s;
        win->font = m->drw->font;
    }
    if (!win->drawn) {
        wlr_scene_node_set_enabled( &win->tree->node, 0 );
        return;
    }
    place( win );
    wlr_scene_node_set_enabled( &win->tree->node, 1 );
}

void awl_window_show( awl_window_t* win, Monitor* m, int center_x, int top ) {
    if (win->m != m) win->drawn = 0;
    win->m = m;
    win->center_x = center_x;
    win->top = top;
    awl_window_update( win, m );
}

void awl_window_hide( awl_window_t* win ) {
    if (!win->m) return;
    win->m = NULL;
    wlr_scene_node_set_enabled( &win->tree->node, 0 );
}

Monitor* awl_window_monitor( const awl_window_t* win ) {
    return win->m;
}

void awl_window_removemon( awl_window_t* win, Monitor* m ) {
    if (win && win->m == m) awl_window_hide( win );
}
