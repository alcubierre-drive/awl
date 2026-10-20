/* Unit test of awl_window_draw.c, the part of awl's popups that needs no
 * compositor: layout, painting into memory and placement, with a real fcft
 * font. Built and run by make check; prints ok/FAIL per check, the exit
 * status is the number of failures. */
#include "awl_window_draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK( cond ) do { \
        if (cond) printf( "ok    %s\n", #cond ); \
        else { printf( "FAIL  %s (line %d)\n", #cond, __LINE__ ); fails++; } \
    } while (0)

static struct fcft_font* font;
static int fh;

static const awl_window_style_t style = {
    .bg = 0x101010ff, .fg = 0xffffffff, .border = 0x00ff00ff, .highlight = 0xff000080,
    .border_width = 2, .radius = 8, .padding = 4,
    .column_spacing = AWL_WINDOW_COLUMN_SPACING, .row_spacing = AWL_WINDOW_ROW_SPACING,
};

static int gw( const char* s ) {
    return (int)awl_draw_text_width( font, s );
}

/* n rows "row K\tvalue" in a malloc()ed string */
static char* rows( int n ) {
    char* t = calloc( (size_t)n, 32 );
    for (int i = 0; i < n; i++)
        sprintf( t + strlen( t ), "%srow %d\tvalue", i ? "\n" : "", i );
    return t;
}

static uint32_t pixel( const uint32_t* data, int32_t stride, int x, int y ) {
    return ((const uint32_t*)((const char*)data + (size_t)y * stride))[x];
}

static void test_layout( void ) {
    awl_window_layout_t L;
    printf( "-- layout\n" );

    CHECK( awl_window_layout( &L, "a\tbb\nccc\td", &style, font, 1, 10000, 10000 ) );
    CHECK( L.ncols == 2 && L.nrows == 2 && !L.cut );
    CHECK( L.widths[0] == gw( "ccc" ) && L.widths[1] == gw( "bb" ) );
    CHECK( L.x0 == 6 && L.y0 == 6 && L.colsp == AWL_WINDOW_COLUMN_SPACING && L.rowsp == AWL_WINDOW_ROW_SPACING );
    CHECK( L.width == 2 * L.x0 + L.colsp + gw( "ccc" ) + gw( "bb" ) );
    CHECK( L.height == 2 * L.y0 + 2 * fh + L.rowsp );

    /* a blank box, one row high */
    CHECK( awl_window_layout( &L, "", &style, font, 1, 10000, 10000 ) );
    CHECK( L.ncols == 1 && L.nrows == 1 && L.width == 2 * L.x0 && L.height == 2 * L.y0 + fh );
    CHECK( awl_window_layout( &L, NULL, &style, font, 1, 10000, 10000 ) && L.width == 2 * L.x0 );

    char cols[256] = "";
    for (int i = 1; i <= 30; i++) sprintf( cols + strlen( cols ), "c%d\t", i );
    CHECK( awl_window_layout( &L, cols, &style, font, 1, 100000, 10000 ) );
    CHECK( L.ncols == AWL_WINDOW_MAX_COLUMNS );

    /* scale: spacing, border and padding scale, the font is the caller's */
    CHECK( awl_window_layout( &L, "a\tb", &style, font, 2, 10000, 10000 ) );
    CHECK( L.x0 == 12 && L.colsp == 2 * AWL_WINDOW_COLUMN_SPACING && L.border_width == 4 && L.radius == 16 );
    CHECK( awl_window_layout( &L, "a\tb", &style, font, 1.25f, 10000, 10000 ) );
    CHECK( L.x0 == 8 && L.colsp == 15 && L.border_width == 3 );
}

static void test_cut( void ) {
    awl_window_layout_t L;
    printf( "-- too wide, too tall\n" );

    char wide[512];
    memset( wide, 'x', sizeof wide - 1 );
    wide[sizeof wide - 1] = 0;
    CHECK( awl_window_layout( &L, wide, &style, font, 1, 200, 10000 ) );
    CHECK( L.width == 200 && L.widths[0] == 200 - 2 * L.x0 );
    CHECK( awl_window_layout( &L, "first\tsecond column, long", &style, font, 1, 100, 10000 ) );
    CHECK( L.width == 100 && L.widths[0] == gw( "first" ) && L.widths[1] == 100 - 2 * L.x0 - L.colsp - gw( "first" ) );

    /* exactly n rows fit into max_h */
    const int n = 10, max_h = 2 * 6 + n * fh + (n - 1) * AWL_WINDOW_ROW_SPACING;
    char* t = rows( n );
    CHECK( awl_window_layout( &L, t, &style, font, 1, 10000, max_h ) );
    CHECK( !L.cut && L.nrows == n && L.height == max_h );
    free( t );
    t = rows( n + 1 );
    CHECK( awl_window_layout( &L, t, &style, font, 1, 10000, max_h ) );
    CHECK( L.cut && L.nrows == n && L.maxrows == n - 1 && L.height == max_h );
    free( t );
    t = rows( 1000 );
    CHECK( awl_window_layout( &L, t, &style, font, 1, 10000, max_h ) );
    CHECK( L.cut && L.nrows == n && L.height <= max_h );
    /* the hidden rows don't count for the widths */
    CHECK( L.widths[0] == gw( "row 8" ) );
    free( t );
    /* one row fits: only the ellipsis */
    t = rows( 5 );
    CHECK( awl_window_layout( &L, t, &style, font, 1, 10000, 2 * 6 + fh ) );
    CHECK( L.cut && L.nrows == 1 && L.maxrows == 0 && L.widths[0] == gw( AWL_WINDOW_ELLIPSIS ) );
    free( t );
    /* not even one */
    CHECK( !awl_window_layout( &L, "x", &style, font, 1, 10000, 2 * 6 + fh - 1 ) );
}

static void test_paint( void ) {
    awl_window_layout_t L;
    printf( "-- paint\n" );

    CHECK( awl_window_layout( &L, "Hello\tworld\nsecond\trow", &style, font, 1, 10000, 10000 ) );
    int32_t stride = awl_draw_stride( (unsigned)L.width );
    uint32_t* data = calloc( (size_t)L.height, (size_t)stride );
    awl_window_paint( &L, "Hello\tworld\nsecond\trow", &style, font, -1, data, stride );

    /* ARGB8888 premultiplied */
    CHECK( pixel( data, stride, 0, 0 ) >> 24 == 0 );                    /* round corner */
    CHECK( pixel( data, stride, L.width - 1, L.height - 1 ) >> 24 == 0 );
    CHECK( pixel( data, stride, L.width / 2, 0 ) == 0xff00ff00 );       /* border */
    CHECK( pixel( data, stride, 0, L.height / 2 ) == 0xff00ff00 );
    CHECK( pixel( data, stride, L.border_width, L.height / 2 ) == 0xff101010 ); /* padding */
    int lit = 0;
    for (int y = L.y0; y < L.y0 + fh; y++)
        for (int x = L.x0; x < L.x0 + L.widths[0]; x++)
            lit += (pixel( data, stride, x, y ) & 0xff) > 0x80;
    CHECK( lit > 20 );                                                   /* "Hello" */
    lit = 0;
    for (int y = L.y0; y < L.height - L.y0; y++)
        for (int x = L.x0 + L.widths[0]; x < L.x0 + L.widths[0] + L.colsp; x++)
            lit += (pixel( data, stride, x, y ) & 0xff) > 0x80;
    CHECK( lit == 0 );                                                   /* between the columns */
    free( data );

    /* the ellipsis row is painted */
    char* t = rows( 50 );
    CHECK( awl_window_layout( &L, t, &style, font, 1, 10000, 2 * 6 + 3 * fh + 2 * AWL_WINDOW_ROW_SPACING ) );
    stride = awl_draw_stride( (unsigned)L.width );
    data = calloc( (size_t)L.height, (size_t)stride );
    awl_window_paint( &L, t, &style, font, -1, data, stride );
    lit = 0;
    int y0 = L.y0 + 2 * (fh + L.rowsp);
    for (int y = y0; y < y0 + fh; y++)
        for (int x = L.x0; x < L.x0 + L.widths[0]; x++)
            lit += (pixel( data, stride, x, y ) & 0xff) > 0x80;
    CHECK( L.cut && lit > 0 );
    free( data );
    free( t );
}

static void test_input( void ) {
    awl_window_layout_t L;
    int r, c, x, y, w, h;
    printf( "-- input: cells, highlight\n" );

    CHECK( awl_window_layout( &L, "a\tbb\nccc\td", &style, font, 1, 10000, 10000 ) );
    CHECK( awl_window_cell_at( &L, L.x0, L.y0, &r, &c ) && r == 0 && c == 0 );
    CHECK( awl_window_cell_at( &L, L.x0 + L.widths[0] + L.colsp, L.y0, &r, &c ) && r == 0 && c == 1 );
    /* the gaps count to the cell before them */
    CHECK( awl_window_cell_at( &L, L.x0 + L.widths[0] + 1, L.y0 + L.fh, &r, &c ) && r == 0 && c == 0 );
    CHECK( awl_window_cell_at( &L, L.width - L.x0 - 1, L.height - L.y0 - 1, &r, &c ) && r == 1 && c == 1 );
    /* padding, frame, outside */
    CHECK( !awl_window_cell_at( &L, L.x0 - 1, L.y0, &r, &c ) && r == -1 && c == -1 );
    CHECK( !awl_window_cell_at( &L, L.x0, L.height - L.y0, &r, &c ) && r == -1 );
    CHECK( !awl_window_cell_at( &L, 0, 0, &r, &c ) && !awl_window_cell_at( &L, -5, 3, &r, &c ) );
    CHECK( !awl_window_cell_at( &L, L.width + 3, L.y0, &r, &c ) );
    CHECK( !awl_window_cell_box( &L, 2, 0, &x, &y, &w, &h ) && !awl_window_cell_box( &L, 0, 2, &x, &y, &w, &h ) );

    /* every point of every cell's box maps back to it */
    const float scales[] = { 1, 1.5f, 2 };
    for (int k = 0; k < 3; k++) {
        CHECK( awl_window_layout( &L, "one\ttwo\tthree\nfour\tfive\tsix\nseven\teight\tnine", &style, font,
                                  scales[k], 10000, 10000 ) );
        int bad = 0;
        for (int rr = 0; rr < L.nrows; rr++)
            for (int cc = 0; cc < L.ncols; cc++) {
                if (!awl_window_cell_box( &L, rr, cc, &x, &y, &w, &h )) { bad++; continue; }
                for (int py = y; py < y + h; py++)
                    for (int px = x; px < x + w; px++)
                        if (!awl_window_cell_at( &L, px, py, &r, &c ) || r != rr || c != cc) bad++;
            }
        CHECK( bad == 0 );
    }

    /* the ellipsis row */
    char* t = rows( 50 );
    CHECK( awl_window_layout( &L, t, &style, font, 1, 10000, 2 * 6 + 3 * fh + 2 * AWL_WINDOW_ROW_SPACING ) );
    CHECK( awl_window_cell_at( &L, L.x0, L.height - L.y0 - 1, &r, &c ) && r == L.maxrows && r == 2 );
    free( t );

    /* the highlight covers its row, in the padding too, and nothing else */
    CHECK( awl_window_layout( &L, "a\tb\nc\td\ne\tf", &style, font, 1, 10000, 10000 ) );
    int32_t stride = awl_draw_stride( (unsigned)L.width );
    uint32_t* data = calloc( (size_t)L.height, (size_t)stride );
    awl_window_paint( &L, "a\tb\nc\td\ne\tf", &style, font, 1, data, stride );
    awl_window_cell_box( &L, 1, 0, &x, &y, &w, &h );
    const int px = L.x0 - 1;  /* in the padding, left of the text */
    CHECK( pixel( data, stride, px, y + L.fh / 2 ) != 0xff101010 );
    awl_window_cell_box( &L, 0, 0, &x, &y, &w, &h );
    CHECK( pixel( data, stride, px, y + L.fh / 2 ) == 0xff101010 );
    awl_window_cell_box( &L, 2, 0, &x, &y, &w, &h );
    CHECK( pixel( data, stride, px, y + L.fh / 2 ) == 0xff101010 );
    CHECK( pixel( data, stride, 0, 0 ) >> 24 == 0 );
    free( data );
}

static void test_place( void ) {
    int x, y;
    printf( "-- place\n" );
    /* a second monitor at x 1280, the bar at the bottom */
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 640, 0, 0, &x, &y );
    CHECK( x == 1280 + 590 && y == 637 );
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 640, 1, 0, &x, &y );
    CHECK( y == 0 );
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 10, 0, 0, &x, &y );
    CHECK( x == 1280 );
    awl_window_place( 1280, 1280, 0, 687, 100, 50, -500, 0, 0, &x, &y );
    CHECK( x == 1280 );
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 1275, 0, 0, &x, &y );
    CHECK( x == 1280 + 1180 );
    awl_window_place( 1280, 1280, 33, 687, 100, 50, 640, 1, 0, &x, &y );
    CHECK( y == 33 );
    /* wider than the monitor: its left edge */
    awl_window_place( 1280, 1280, 0, 687, 2000, 50, 640, 0, 0, &x, &y );
    CHECK( x == 1280 );

    printf( "-- place, with a margin\n" );
    CHECK( awl_window_margin( 6, 1280 ) == 6 && awl_window_margin( -1, 1280 ) == 0 );
    CHECK( awl_window_margin( 30, 100 ) == 25 && awl_window_margin( 6, 0 ) == 0 );
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 640, 0, 6, &x, &y );
    CHECK( x == 1280 + 590 && y == 687 - 50 - 6 );
    awl_window_place( 1280, 1280, 33, 687, 100, 50, 640, 1, 6, &x, &y );
    CHECK( y == 33 + 6 );
    /* off the monitor's sides */
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 10, 0, 6, &x, &y );
    CHECK( x == 1280 + 6 );
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 1275, 0, 6, &x, &y );
    CHECK( x == 1280 + 1280 - 100 - 6 );
    /* what is left of it beside a window almost as wide, as tall */
    awl_window_place( 1280, 1280, 0, 687, 1274, 684, 0, 0, 6, &x, &y );
    CHECK( x == 1280 + 3 && y == 0 );
    awl_window_place( 1280, 1280, 0, 687, 1274, 684, 0, 1, 6, &x, &y );
    CHECK( y == 3 );
    awl_window_place( 1280, 1280, 0, 687, 1280, 687, 0, 1, 6, &x, &y );
    CHECK( x == 1280 && y == 0 );
    /* none beside one that does not fit */
    awl_window_place( 1280, 1280, 0, 687, 2000, 700, 640, 0, 6, &x, &y );
    CHECK( x == 1280 && y == 687 - 700 );
    awl_window_place( 1280, 1280, 0, 687, 2000, 700, 640, 1, 6, &x, &y );
    CHECK( y == 0 );
    /* negative: none; huge: a quarter of the monitor, of the area */
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 10, 0, -6, &x, &y );
    CHECK( x == 1280 && y == 637 );
    awl_window_place( 1280, 1280, 0, 687, 100, 50, 10, 0, 100000, &x, &y );
    CHECK( x == 1280 + 320 && y == 687 - 50 - 171 );

    printf( "-- place beside\n" );
    /* right of a menu at 1280 + 100, 120 wide, the row at y 40 */
    awl_window_place_beside( 1280, 1280, 0, 687, 100, 50, 1380, 40, 120, 0, 6, &x, &y );
    CHECK( x == 1500 && y == 40 );
    /* no room on the right: on the left */
    awl_window_place_beside( 1280, 1280, 0, 687, 100, 50, 2400, 40, 120, 0, 6, &x, &y );
    CHECK( x == 2300 );
    /* room on neither side: the right, which has more, as far as it goes */
    awl_window_place_beside( 1280, 1280, 0, 687, 700, 50, 1280 + 400, 40, 300, 0, 6, &x, &y );
    CHECK( x == 1280 + 1280 - 6 - 700 );
    /* the left has more */
    awl_window_place_beside( 1280, 1280, 0, 687, 700, 50, 1280 + 600, 40, 300, 0, 6, &x, &y );
    CHECK( x == 1280 + 6 );
    /* kept within the area, at its bottom, at its top */
    awl_window_place_beside( 1280, 1280, 33, 654, 100, 50, 1380, 670, 120, 0, 6, &x, &y );
    CHECK( y == 33 + 654 - 6 - 50 );
    awl_window_place_beside( 1280, 1280, 33, 654, 100, 50, 1380, 0, 120, 0, 6, &x, &y );
    CHECK( y == 33 + 6 );
    /* left first: there if it fits, else the right */
    awl_window_place_beside( 1280, 1280, 0, 687, 100, 50, 1500, 40, 120, 1, 6, &x, &y );
    CHECK( x == 1400 );
    awl_window_place_beside( 1280, 1280, 0, 687, 100, 50, 1300, 40, 120, 1, 6, &x, &y );
    CHECK( x == 1420 );
    /* taller than the area: its top */
    awl_window_place_beside( 1280, 1280, 33, 654, 100, 900, 1380, 100, 120, 0, 6, &x, &y );
    CHECK( y == 33 );

    printf( "-- place at\n" );
    /* a 1280 x 654 area under a 33 px bar, on the second monitor */
    awl_window_place_at( 1280, 1280, 33, 654, 400, 300, AWL_WINDOW_AT_TOP, 0, 0, 6, &x, &y );
    CHECK( x == 1280 + 440 && y == 33 + 6 );
    awl_window_place_at( 1280, 1280, 33, 654, 400, 300, AWL_WINDOW_AT_CENTER, 0, 0, 6, &x, &y );
    CHECK( x == 1280 + 440 && y == 33 + 177 );
    awl_window_place_at( 1280, 1280, 33, 654, 400, 100, AWL_WINDOW_AT_TOP_RIGHT, 0, 0, 6, &x, &y );
    CHECK( x == 1280 + 1280 - 6 - 400 && y == 33 + 6 );
    awl_window_place_at( 1280, 1280, 33, 654, 400, 100, AWL_WINDOW_AT_BOTTOM_LEFT, 0, 0, 6, &x, &y );
    CHECK( x == 1280 + 6 && y == 33 + 654 - 6 - 100 );
    /* moved: notifications stacked down from a corner */
    awl_window_place_at( 1280, 1280, 33, 654, 400, 100, AWL_WINDOW_AT_TOP_RIGHT, 0, 110, 6, &x, &y );
    CHECK( y == 33 + 6 + 110 );
    /* but kept within the area and the monitor */
    awl_window_place_at( 1280, 1280, 33, 654, 400, 100, AWL_WINDOW_AT_TOP_RIGHT, 50, 600, 6, &x, &y );
    CHECK( x == 1280 + 1280 - 400 && y == 33 + 654 - 100 );
    awl_window_place_at( 1280, 1280, 33, 654, 2000, 1000, AWL_WINDOW_AT_CENTER, 0, 0, 6, &x, &y );
    CHECK( x == 1280 && y == 33 );
}

int main( void ) {
    /* 96 dpi: monospace:size=10 at a scale of 1/2 */
    if (!awl_draw_init() || !(font = awl_font_get_exact( "monospace:size=", 20, 0.5f ))) {
        fprintf( stderr, "no font monospace:size=10\n" );
        return 100;
    }
    fh = font->height;
    test_layout();
    test_cut();
    test_paint();
    test_input();
    test_place();
    awl_font_release( font );
    awl_draw_fini();
    printf( "%d failed\n", fails );
    return fails;
}
