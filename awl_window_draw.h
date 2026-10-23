#pragma once

/**
 * What an awl_window (awl_window.h) shows and where, apart from the scene
 * graph and the monitors: laying its text out, painting it into memory and
 * placing it. No wlroots calls, so test/window_unit.c runs it without a
 * compositor; awl_window.c feeds it from a Monitor.
 */

#include "awl.h" /* awl_popup_config_t, awl_window_painter_t, AWL_WINDOW_ELLIPSIS */

#include <stdint.h>

/** at most this many columns; the rest of a row goes into the last one */
#define AWL_WINDOW_MAX_COLUMNS 16

/** How a window looks; awl fills it from cfg->popup. Colors are
 * 0xRRGGBBAA, sizes logical pixels. */
typedef struct awl_window_style_t {
    /** highlight: over the row under the pointer, if the window takes it */
    uint32_t bg, fg, border, highlight;
    int border_width, radius, padding;
    /** the gap to the bar (the window area's edge) and to the monitor's
     * sides, see :c:func:`awl_window_margin` */
    int margin;
    /** between columns and rows */
    int column_spacing, row_spacing;
    /** blur what is behind it, with blur_strength and blur_alpha */
    int blur;
    float blur_strength, blur_alpha;
} awl_window_style_t;

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

/** Lays out a painted window (:c:type:`awl_window_painter_t`): its frame around
 * content_w x content_h pixels at scale, the content at L->x0, L->y0; no
 * cells. Returns whether there is anything to show. */
int awl_window_layout_frame( awl_window_layout_t* L, const awl_window_style_t* style, float scale,
                             int content_w, int content_h );

/** the border's width plus the padding at scale, physical pixels: how far
 * the content is from each edge of the image */
int awl_window_inset( const awl_window_style_t* style, float scale );

/** Paints only L's frame (background and border) into data, as
 * :c:func:`awl_window_paint` does before the text. */
void awl_window_paint_frame( const awl_window_layout_t* L, const awl_window_style_t* style,
                             uint32_t* data, int32_t stride );

/** The cell at px, py (pixels from the image's top left) into ``*row`` and ``*col``:
 * the gap after a row or column counts to it, the ellipsis row is row
 * L->maxrows. Outside the text (the frame, the padding) both are -1, and it
 * returns 0. */
int awl_window_cell_at( const awl_window_layout_t* L, int px, int py, int* row, int* col );

/** Where cell row, col is in the image, the gaps after it included (the
 * area :c:func:`awl_window_cell_at` maps to it); 0 if there is no such cell. */
int awl_window_cell_box( const awl_window_layout_t* L, int row, int col, int* x, int* y, int* w, int* h );

/** How much of margin (logical pixels) a window keeps to each end of extent
 * (the monitor's width, the window area's height): none if negative, at
 * most a quarter of extent, so the window keeps at least half of it. */
int awl_window_margin( int margin, int extent );

/** Where a lw x lh window (logical pixels) goes on a monitor at mon_x, mon_w
 * with the window area area_y, area_h (layout coordinates): centered on
 * center_x (from the monitor's left edge) but within the monitor, at the
 * area's top if top, else its bottom, :c:func:`awl_window_margin` of margin away
 * from the monitor's sides and the area's edge. Less of the margin where
 * the window would not fit with it, none where it does not fit at all: then
 * at the monitor's left edge, the area's top or bottom edge. */
void awl_window_place( int mon_x, int mon_w, int area_y, int area_h, int lw, int lh,
                       int center_x, int top, int margin, int* x, int* y );

/** Where a lw x lh window (logical pixels) goes beside the box ax, ay, aw
 * wide (layout coordinates; a submenu's beside the menu, at the row that
 * opened it): right of the box (left of it if left) if it fits there within
 * the monitor at mon_x, mon_w, :c:func:`awl_window_margin` of margin away from its
 * side, else on the other side if it fits there, else on the side with more
 * room, as far from the box as the monitor allows. Its top at ay, but
 * within the window area area_y, area_h as for :c:func:`awl_window_place`. */
void awl_window_place_beside( int mon_x, int mon_w, int area_y, int area_h, int lw, int lh,
                              int ax, int ay, int aw, int left, int margin, int* x, int* y );

/** Where a lw x lh window (logical pixels) goes at spot at (an
 * AWL_WINDOW_AT_*) of the window area: the monitor at mon_x, mon_w across,
 * area_y, area_h down (layout coordinates), :c:func:`awl_window_margin` of margin
 * away from the edges it is at, then moved by dx, dy, but kept within the
 * monitor and the area (at its top left where it doesn't fit). */
void awl_window_place_at( int mon_x, int mon_w, int area_y, int area_h, int lw, int lh,
                          int at, int dx, int dy, int margin, int* x, int* y );
