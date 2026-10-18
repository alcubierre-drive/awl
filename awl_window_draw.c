#include "awl_window_draw.h"

#include <limits.h>
#include <math.h>
#include <string.h>

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
 * column of AWL_WINDOW_MAX_COLUMNS on, the rest of the row is one cell */
static void each_cell( const char* text, void (*f)( void* data, int row, int col, const char* s, size_t n ),
                       void* data ) {
    int row = 0, col = 0;
    for (const char* p = text;;) {
        size_t n = strcspn( p, col == AWL_WINDOW_MAX_COLUMNS - 1 ? "\n" : "\t\n" );
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

/** each_cell()'s data: the layout being made or painted, and how */
typedef struct {
    awl_window_layout_t* L;
    const awl_window_layout_t* cL;
    pixman_image_t* pix;
    struct fcft_font* font;
    uint32_t fg;
} cells_t;

static void measure_cell( void* data, int row, int col, const char* s, size_t n ) {
    cells_t* C = data;
    awl_window_layout_t* L = C->L;
    char buf[1024];
    if (row >= L->maxrows) return;
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy( buf, s, n );
    buf[n] = 0;
    int w = (int)awl_draw_text_width( C->font, buf );
    if (w > L->widths[col]) L->widths[col] = w;
    if (col + 1 > L->ncols) L->ncols = col + 1;
    if (row + 1 > L->nrows) L->nrows = row + 1;
}

static void draw_cell( void* data, int row, int col, const char* s, size_t n ) {
    cells_t* C = data;
    const awl_window_layout_t* L = C->cL;
    char buf[1024];
    if (!n || row >= L->maxrows) return;
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy( buf, s, n );
    buf[n] = 0;
    int x = L->x0;
    for (int c = 0; c < col; c++) x += L->widths[c] + L->colsp;
    int h = C->font->height;
    awl_draw_text_over( C->pix, C->font, x, L->y0 + row * (h + L->rowsp), (unsigned)L->widths[col], (unsigned)h,
                        buf, C->fg );
}

int awl_window_layout( awl_window_layout_t* L, const char* text, const awl_window_style_t* style,
                       struct fcft_font* font, float s, int max_w, int max_h ) {
    *L = (awl_window_layout_t){ .maxrows = INT_MAX };
    if (!font) return 0;
    if (!text) text = "";
    const int fh = L->fh = font->height;
    L->colsp = (int)lroundf( AWL_WINDOW_COLUMN_SPACING * s );
    L->rowsp = (int)lroundf( AWL_WINDOW_ROW_SPACING * s );
    L->border_width = (int)lroundf( fmaxf( (float)style->border_width, 0 ) * s );
    L->radius = fmaxf( (float)style->radius, 0 ) * s;
    L->x0 = L->y0 = awl_window_inset( style, s );

    /* taller than allowed: the rows that fit, the last of them an ellipsis */
    int rows = 1;
    for (const char* p = text; *p; p++) rows += *p == '\n';
    const int fit = (max_h - 2 * L->y0 + L->rowsp) / (fh + L->rowsp);
    if (fit < 1) return 0;
    L->cut = rows > fit;
    if (L->cut) L->maxrows = fit - 1;
    cells_t C = { .L = L, .font = font };
    each_cell( text, measure_cell, &C );
    if (L->cut) {
        int ew = (int)awl_draw_text_width( font, AWL_WINDOW_ELLIPSIS );
        if (L->ncols < 1) L->ncols = 1;
        if (ew > L->widths[0]) L->widths[0] = ew;
        L->nrows = fit;
    }

    int width = 2 * L->x0 + (L->ncols - 1) * L->colsp;
    for (int c = 0; c < L->ncols; c++) width += L->widths[c];
    int height = 2 * L->y0 + L->nrows * fh + (L->nrows - 1) * L->rowsp;
    /* wider than allowed: the last column is cut off */
    if (width > max_w) {
        L->widths[L->ncols - 1] -= width - max_w;
        if (L->widths[L->ncols - 1] < 0) L->widths[L->ncols - 1] = 0;
        width = max_w;
    }
    if (width <= 0 || height <= 0) return 0;
    L->width = width;
    L->height = height;
    return 1;
}

int awl_window_inset( const awl_window_style_t* style, float s ) {
    return (int)lroundf( fmaxf( (float)style->border_width, 0 ) * s ) +
           (int)lroundf( fmaxf( (float)style->padding, 0 ) * s );
}

int awl_window_layout_frame( awl_window_layout_t* L, const awl_window_style_t* style, float s,
                             int content_w, int content_h ) {
    *L = (awl_window_layout_t){ 0 };
    if (content_w <= 0 || content_h <= 0) return 0;
    L->border_width = (int)lroundf( fmaxf( (float)style->border_width, 0 ) * s );
    L->radius = fmaxf( (float)style->radius, 0 ) * s;
    L->x0 = L->y0 = awl_window_inset( style, s );
    L->width = content_w + 2 * L->x0;
    L->height = content_h + 2 * L->y0;
    return 1;
}

void awl_window_paint_frame( const awl_window_layout_t* L, const awl_window_style_t* style,
                             uint32_t* data, int32_t stride ) {
    draw_frame( data, (size_t)stride, L->width, L->height, (float)L->border_width, L->radius,
                style->bg, style->border );
}

/** c over the w x h box at x, y of data, clipped to the image */
static void blend_rect( uint32_t* data, int32_t stride, int iw, int ih, int x, int y, int w, int h,
                        uint32_t c ) {
    float f[4];
    unpack( c, f );
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < ih; yy++) {
        uint32_t* row = (uint32_t*)((char*)data + (size_t)yy * stride);
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < iw; xx++) {
            uint32_t d = row[xx], out = 0;
            /* ARGB8888, premultiplied: out = c + d * (1 - c.alpha) */
            const int shift[4] = { 16, 8, 0, 24 };
            for (int k = 0; k < 4; k++) {
                float v = f[k] * 255 + (float)((d >> shift[k]) & 0xff) * (1 - f[3]);
                out |= (uint32_t)lroundf( fminf( v, 255 ) ) << shift[k];
            }
            row[xx] = out;
        }
    }
}

int awl_window_cell_box( const awl_window_layout_t* L, int row, int col, int* x, int* y, int* w, int* h ) {
    if (row < 0 || row >= L->nrows || col < 0 || col >= L->ncols) return 0;
    *x = L->x0;
    for (int c = 0; c < col; c++) *x += L->widths[c] + L->colsp;
    *w = L->widths[col] + (col < L->ncols - 1 ? L->colsp : 0);
    *y = L->y0 + row * (L->fh + L->rowsp);
    *h = L->fh + (row < L->nrows - 1 ? L->rowsp : 0);
    return 1;
}

int awl_window_cell_at( const awl_window_layout_t* L, int px, int py, int* row, int* col ) {
    *row = *col = -1;
    if (px < L->x0 || py < L->y0 || px >= L->width - L->x0 || py >= L->height - L->y0) return 0;
    int r = (py - L->y0) / (L->fh + L->rowsp), c = 0, x = L->x0;
    if (r >= L->nrows) return 0;
    while (c < L->ncols - 1 && px >= x + L->widths[c] + L->colsp) x += L->widths[c++] + L->colsp;
    *row = r;
    *col = c;
    return 1;
}

void awl_window_paint( const awl_window_layout_t* L, const char* text, const awl_window_style_t* style,
                       struct fcft_font* font, int highlight_row, uint32_t* data, int32_t stride ) {
    int hx, hy, hw, hh;
    if (!text) text = "";
    draw_frame( data, (size_t)stride, L->width, L->height, (float)L->border_width, L->radius,
                style->bg, style->border );
    /* the row's band, across the padding up to halfway to the border and
     * half the row gap above and below */
    if (awl_window_cell_box( L, highlight_row, 0, &hx, &hy, &hw, &hh )) {
        const int inset = (L->border_width + L->x0) / 2;
        blend_rect( data, stride, L->width, L->height, inset, hy - L->rowsp / 2, L->width - 2 * inset,
                    L->fh + L->rowsp, style->highlight );
    }
    pixman_image_t* pix = font ? awl_draw_image( (unsigned)L->width, (unsigned)L->height, data, stride ) : NULL;
    if (!pix) return;
    cells_t C = { .cL = L, .pix = pix, .font = font, .fg = style->fg };
    each_cell( text, draw_cell, &C );
    if (L->cut)
        awl_draw_text_over( pix, font, L->x0, L->y0 + L->maxrows * (font->height + L->rowsp),
                            (unsigned)L->widths[0], (unsigned)font->height, AWL_WINDOW_ELLIPSIS, style->fg );
    pixman_image_unref( pix );
}

int awl_window_margin( int margin, int extent ) {
    if (margin < 0 || extent <= 0) return 0;
    return margin < extent / 4 ? margin : extent / 4;
}

void awl_window_place( int mon_x, int mon_w, int area_y, int area_h, int lw, int lh,
                       int center_x, int top, int margin, int* x, int* y ) {
    int mx = awl_window_margin( margin, mon_w ), my = awl_window_margin( margin, area_h );
    /* what fits beside, and between the area's edge and the window */
    if (mx > (mon_w - lw) / 2) mx = lw < mon_w ? (mon_w - lw) / 2 : 0;
    if (my > area_h - lh) my = lh < area_h ? area_h - lh : 0;
    int dx = center_x - lw / 2;
    if (dx > mon_w - lw - mx) dx = mon_w - lw - mx;
    if (dx < mx) dx = mx;
    *x = mon_x + dx;
    *y = top ? area_y + my : area_y + area_h - lh - my;
}

void awl_window_place_beside( int mon_x, int mon_w, int area_y, int area_h, int lw, int lh,
                              int ax, int ay, int aw, int left, int margin, int* x, int* y ) {
    int mx = awl_window_margin( margin, mon_w ), my = awl_window_margin( margin, area_h );
    if (mx > (mon_w - lw) / 2) mx = lw < mon_w ? (mon_w - lw) / 2 : 0;
    if (my > (area_h - lh) / 2) my = lh < area_h ? (area_h - lh) / 2 : 0;
    const int right = mon_x + mon_w - mx - (ax + aw);
    const int room_left = ax - (mon_x + mx);
    int go_left;
    if (lw <= (left ? room_left : right)) go_left = left;
    else if (lw <= (left ? right : room_left)) go_left = !left;
    else go_left = room_left > right;
    *x = go_left ? ax - lw : ax + aw;
    if (*x > mon_x + mon_w - mx - lw) *x = mon_x + mon_w - mx - lw;
    if (*x < mon_x + mx) *x = mon_x + mx;
    *y = ay;
    if (*y > area_y + area_h - my - lh) *y = area_y + area_h - my - lh;
    if (*y < area_y + my) *y = area_y + my;
}
