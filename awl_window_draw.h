#pragma once

/**
 * What an awl_window (awl_window.h) shows and where, apart from the scene
 * graph and the monitors: laying its text out, painting it into memory and
 * placing it. No wlroots calls, so test/window_unit.c runs it without a
 * compositor; awl_window.c feeds it from a Monitor.
 */

#include "awl_draw.h"

#include <math.h> /* awl_shape_coverage() */
#include <stdint.h>

/** between columns and rows, logical pixels */
#define AWL_WINDOW_COLUMN_SPACING 12
#define AWL_WINDOW_ROW_SPACING 2
/** at most this many columns; the rest of a row goes into the last one */
#define AWL_WINDOW_MAX_COLUMNS 16
/** the last row of a window with more rows than fit */
#define AWL_WINDOW_ELLIPSIS "…"

/** config_plugins.h's popup_config: how awl's own popups (awl_window.h)
 * look, the bar widgets' (the clock's calendar, the IP addresses) and awl's.
 * Colors 0xRRGGBBAA, sizes logical pixels. */
typedef struct awl_popup_config_t {
    uint32_t bg, fg, border;
    /** what matters less: the calendar's neighbouring months' days, the IP
     * popup's missing routes */
    uint32_t dim;
    /** hover and pressed: background of the buttons */
    uint32_t hover, pressed;
    int border_width, radius, padding;
    /** the gap to the bar and to the monitor's sides, see awl_window_margin() */
    int margin;
    /** the font, as config_plugins.h's font and fontsize (size appended to
     * font); NULL and 0 each take the bar's */
    const char *font;
    int fontsize;
    /** the clock's calendar */
    struct awl_popup_calendar_config_t {
        /** today's date */
        uint32_t today;
        /** background of the selected day */
        uint32_t selected;
        /** list_min_height and list_max_height: height of the event list,
         * which scrolls beyond the maximum; a maximum of 0 means none */
        int list_min_height, list_max_height;
    } calendar;
    /** the tray's menus, their own look; what is 0 (left out) is the
     * popups': bg, fg, border, dim, highlight (hover), highlight_fg (the
     * menu's fg), border_width, radius and blur (blur_launcher). A negative
     * border_width, radius or blur is none. */
    struct awl_popup_menu_config_t {
        uint32_t bg, fg, border;
        /** disabled entries, shortcuts, separators */
        uint32_t dim;
        /** the entry under the pointer, the one whose submenu is open: its
         * background and its text */
        uint32_t highlight, highlight_fg;
        int border_width, radius;
        /** blur what is behind them, as the launcher */
        int blur;
    } menu;
} awl_popup_config_t;

/** c's menu look with what is left out filled in from the popups' (see
 * menu), negative sizes and blur as 0; blur_launcher is the popups' blur */
static inline struct awl_popup_menu_config_t awl_popup_menu_config( const awl_popup_config_t* c, int blur_launcher ) {
    struct awl_popup_menu_config_t m = c->menu;
    if (!m.bg) m.bg = c->bg;
    if (!m.fg) m.fg = c->fg;
    if (!m.border) m.border = c->border;
    if (!m.dim) m.dim = c->dim;
    if (!m.highlight) m.highlight = c->hover;
    if (!m.highlight_fg) m.highlight_fg = m.fg;
    m.border_width = m.border_width < 0 ? 0 : m.border_width ? m.border_width : c->border_width;
    m.radius = m.radius < 0 ? 0 : m.radius ? m.radius : c->radius;
    m.blur = m.blur < 0 ? 0 : m.blur ? m.blur : blur_launcher;
    return m;
}

/** How much of the pixel centered at x, y the w x h box with corner radius
 * r at the origin covers, antialiased over a pixel: the frame's, and the
 * painters' rounded shapes (the menus' rows, the calendar's days). */
static inline float awl_shape_coverage( float x, float y, float w, float h, float r ) {
    if (w <= 0 || h <= 0) return 0;
    r = fminf( r, fminf( w, h ) / 2 );
    float qx = fabsf( x - w / 2 ) - (w / 2 - r), qy = fabsf( y - h / 2 ) - (h / 2 - r);
    float ox = fmaxf( qx, 0 ), oy = fmaxf( qy, 0 );
    float d = sqrtf( ox * ox + oy * oy ) + fminf( fmaxf( qx, qy ), 0 ) - r;
    return fminf( fmaxf( 0.5f - d, 0 ), 1 );
}

/** c (0xRRGGBBAA) over pix (premultiplied ARGB) in the w x h box at x, y
 * with corner radius r, antialiased; with t > 0 only its outline, t wide.
 * Physical pixels, for a painter's paint(). */
static inline void awl_fill_shape( pixman_image_t* pix, float x, float y, float w, float h, float r, float t,
                                   uint32_t c ) {
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
            float k = awl_shape_coverage( fx, fy, w, h, r );
            if (t > 0) k -= awl_shape_coverage( fx - t, fy - t, w - 2 * t, h - 2 * t, fmaxf( r - t, 0 ) );
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

/** How a window looks; awl fills it from popup_config. Colors are
 * 0xRRGGBBAA, sizes logical pixels. */
typedef struct awl_window_style_t {
    /** highlight: over the row under the pointer, if the window takes it */
    uint32_t bg, fg, border, highlight;
    int border_width, radius, padding;
    /** the gap to the bar (the window area's edge) and to the monitor's
     * sides, see awl_window_margin() */
    int margin;
    /** blur what is behind it, with blur_strength and blur_alpha */
    int blur;
    float blur_strength, blur_alpha;
} awl_window_style_t;

/**
 * A window that draws what is inside its frame itself (a calendar, a menu),
 * awl_window_set_painter(): instead of text and cells, parts the painter
 * numbers itself. Everything in physical pixels, positions from the content's
 * top left (inside the border and the padding). Main thread; font is the
 * popups' of the monitor the window is on (Monitor.popupfont, else the
 * bar's), at that monitor's scale, never NULL.
 */
typedef struct awl_window_painter_t {
    /** the content's size in font at scale, at most max_w x max_h, into
     * *w, *h; returns 0 if there is nothing to show */
    int (*measure)( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h );
    /** paints the content as measure() last laid it out, its top left at
     * x0, y0 of pix (the window's image, the frame already in it); hover the
     * part the pointer is over, -1 none */
    void (*paint)( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover );
    /** the part at px, py, -1 none: what the pointer hovers, and the row
     * clicks and scrolls report (their column 0, or -1 with the row) */
    int (*hit)( void* data, int px, int py );
    /** where part is, for awl_window_cell_center(); 0 if there is no such
     * part. May be NULL. */
    int (*box)( void* data, int part, int* x, int* y, int* w, int* h );
} awl_window_painter_t;

/** Text laid out for painting; physical pixels. */
typedef struct awl_window_layout_t {
    /** the image */
    int width, height;
    /** each column's width, the last one cut if the image would be wider
     * than allowed */
    int widths[AWL_WINDOW_MAX_COLUMNS];
    int ncols;
    /** rows in the image, the ellipsis row included */
    int nrows;
    /** the text's rows painted, the ones above the ellipsis if cut */
    int maxrows;
    /** the text has more rows than fit: the last row is an ellipsis */
    int cut;
    /** the border's width and corner radius, the first cell's top left,
     * the spacing between columns and between rows */
    int border_width;
    float radius;
    int x0, y0, colsp, rowsp;
    /** the font's height, a row's */
    int fh;
} awl_window_layout_t;

/** Lays out text (rows separated by '\n', columns by '\t') in font with
 * style at scale, at most max_w x max_h pixels: more rows than fit end
 * in an ellipsis row, a wider image cuts its last column. Returns whether
 * there is anything to show (not if not even one row fits). */
int awl_window_layout( awl_window_layout_t* L, const char* text, const awl_window_style_t* style,
                       struct fcft_font* font, float scale, int max_w, int max_h );

/** Paints text as laid out in L into data (L->width x L->height, ARGB8888,
 * premultiplied): the frame, row highlight_row (-1: none) in
 * style->highlight, then the text in font, in style->fg. */
void awl_window_paint( const awl_window_layout_t* L, const char* text, const awl_window_style_t* style,
                       struct fcft_font* font, int highlight_row, uint32_t* data, int32_t stride );

/** Lays out a painted window (awl_window_painter_t): its frame around
 * content_w x content_h pixels at scale, the content at L->x0, L->y0; no
 * cells. Returns whether there is anything to show. */
int awl_window_layout_frame( awl_window_layout_t* L, const awl_window_style_t* style, float scale,
                             int content_w, int content_h );

/** the border's width plus the padding at scale, physical pixels: how far
 * the content is from each edge of the image */
int awl_window_inset( const awl_window_style_t* style, float scale );

/** Paints only L's frame (background and border) into data, as
 * awl_window_paint() does before the text. */
void awl_window_paint_frame( const awl_window_layout_t* L, const awl_window_style_t* style,
                             uint32_t* data, int32_t stride );

/** The cell at px, py (pixels from the image's top left) into *row and *col:
 * the gap after a row or column counts to it, the ellipsis row is row
 * L->maxrows. Outside the text (the frame, the padding) both are -1, and it
 * returns 0. */
int awl_window_cell_at( const awl_window_layout_t* L, int px, int py, int* row, int* col );

/** Where cell row, col is in the image, the gaps after it included (the
 * area awl_window_cell_at() maps to it); 0 if there is no such cell. */
int awl_window_cell_box( const awl_window_layout_t* L, int row, int col, int* x, int* y, int* w, int* h );

/** How much of margin (logical pixels) a window keeps to each end of extent
 * (the monitor's width, the window area's height): none if negative, at
 * most a quarter of extent, so the window keeps at least half of it. */
int awl_window_margin( int margin, int extent );

/** Where a lw x lh window (logical pixels) goes on a monitor at mon_x, mon_w
 * with the window area area_y, area_h (layout coordinates): centered on
 * center_x (from the monitor's left edge) but within the monitor, at the
 * area's top if top, else its bottom, awl_window_margin() of margin away
 * from the monitor's sides and the area's edge. Less of the margin where
 * the window would not fit with it, none where it does not fit at all: then
 * at the monitor's left edge, the area's top or bottom edge. */
void awl_window_place( int mon_x, int mon_w, int area_y, int area_h, int lw, int lh,
                       int center_x, int top, int margin, int* x, int* y );

/** Where a lw x lh window (logical pixels) goes beside the box ax, ay, aw
 * wide (layout coordinates; a submenu's beside the menu, at the row that
 * opened it): right of the box (left of it if left) if it fits there within
 * the monitor at mon_x, mon_w, awl_window_margin() of margin away from its
 * side, else on the other side if it fits there, else on the side with more
 * room, as far from the box as the monitor allows. Its top at ay, but
 * within the window area area_y, area_h as for awl_window_place(). */
void awl_window_place_beside( int mon_x, int mon_w, int area_y, int area_h, int lw, int lh,
                              int ax, int ay, int aw, int left, int margin, int* x, int* y );
