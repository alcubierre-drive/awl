/* Unit test of awl_draw.c's font registry: styled fonts (awl_font_style()),
 * with real fcft fonts. Built and run by meson test; prints ok/FAIL per
 * check, the exit status is the number of failures. */
#include "awl_draw.h"

#include <stdio.h>

static int fails;

#define CHECK( cond ) do { \
        if (cond) printf( "ok    %s\n", #cond ); \
        else { printf( "FAIL  %s (line %d)\n", #cond, __LINE__ ); fails++; } \
    } while (0)

/* the ink of c in font: its glyph's alpha summed */
static unsigned long ink( struct fcft_font* font, uint32_t c ) {
    const struct fcft_glyph* g = fcft_rasterize_char_utf32( font, c, FCFT_SUBPIXEL_NONE );
    unsigned long sum = 0;
    if (!g) return 0;
    pixman_image_t* a = pixman_image_create_bits( PIXMAN_a8, g->width, g->height, NULL, 0 );
    pixman_image_composite32( PIXMAN_OP_SRC, g->pix, NULL, a, 0, 0, 0, 0, 0, 0, g->width, g->height );
    const uint8_t* p = (const uint8_t*)pixman_image_get_data( a );
    int stride = pixman_image_get_stride( a );
    for (int y = 0; y < g->height; y++)
        for (int x = 0; x < g->width; x++) sum += p[y * stride + x];
    pixman_image_unref( a );
    return sum;
}

/* the leftmost ink column of c's top row with ink, relative to its bottom
 * row's: positive for a slanted glyph */
static int lean( struct fcft_font* font, uint32_t c ) {
    const struct fcft_glyph* g = fcft_rasterize_char_utf32( font, c, FCFT_SUBPIXEL_NONE );
    int top = -1, bottom = -1;
    if (!g) return 0;
    pixman_image_t* a = pixman_image_create_bits( PIXMAN_a8, g->width, g->height, NULL, 0 );
    pixman_image_composite32( PIXMAN_OP_SRC, g->pix, NULL, a, 0, 0, 0, 0, 0, 0, g->width, g->height );
    const uint8_t* p = (const uint8_t*)pixman_image_get_data( a );
    int stride = pixman_image_get_stride( a );
    for (int y = 0; y < g->height; y++)
        for (int x = 0; x < g->width; x++)
            if (p[y * stride + x] > 128) {
                if (top < 0) top = x;
                bottom = x;
                break;
            }
    pixman_image_unref( a );
    return top - bottom;
}

static void test_style( struct fcft_font* f ) {
    printf( "-- style\n" );
    struct fcft_font* b = awl_font_style( f, AWL_FONT_BOLD );
    struct fcft_font* i = awl_font_style( f, AWL_FONT_ITALIC );
    struct fcft_font* bi = awl_font_style( f, AWL_FONT_BOLD | AWL_FONT_ITALIC );
    CHECK( b && i && bi );
    CHECK( b != f && i != f && bi != f && b != i && b != bi && i != bi );
    /* the same height class: same size and dpi */
    CHECK( b && b->height >= f->height - 2 && b->height <= f->height + 2 );
    /* bold has more ink, italic leans */
    CHECK( b && ink( b, 'm' ) > ink( f, 'm' ) );
    CHECK( i && lean( i, 'l' ) > lean( f, 'l' ) );

    /* shared: the same entry again */
    struct fcft_font* b2 = awl_font_style( f, AWL_FONT_BOLD );
    CHECK( b2 == b );
    awl_font_release( b2 );
    /* a style adds to the font's own */
    struct fcft_font* bi2 = awl_font_style( b, AWL_FONT_ITALIC );
    CHECK( bi2 == bi );
    awl_font_release( bi2 );
    struct fcft_font* b3 = awl_font_style( b, AWL_FONT_BOLD );
    CHECK( b3 == b );
    awl_font_release( b3 );
    /* style 0 and unknown bits: the font itself */
    struct fcft_font* f2 = awl_font_style( f, 0 );
    CHECK( f2 == f );
    awl_font_release( f2 );
    f2 = awl_font_style( f, 0x100 );
    CHECK( f2 == f );
    awl_font_release( f2 );
    /* not the registry's */
    CHECK( awl_font_style( NULL, AWL_FONT_BOLD ) == NULL );

    /* held fonts survive the ring filling up; their entries are found again */
    for (int n = 0; n < 2 * AWL_FONT_SLOTS; n++) {
        struct fcft_font* t = awl_font_get_exact( "monospace:size=", 6 + n, 0.5f );
        awl_font_release( t );
    }
    b2 = awl_font_style( f, AWL_FONT_BOLD );
    CHECK( b2 == b );
    awl_font_release( b2 );

    awl_font_release( b );
    awl_font_release( i );
    awl_font_release( bi );
}

static void test_emoji( void ) {
    printf( "-- emoji\n" );
    struct fcft_font* e = awl_font_get_emoji( "emoji:size=", 20, 0.5f );
    if (!e) {
        printf( "skip  no color emoji font\n" );
        return;
    }
    /* emoji fonts aren't styled */
    struct fcft_font* eb = awl_font_style( e, AWL_FONT_BOLD );
    CHECK( eb == e );
    awl_font_release( eb );
    awl_font_release( e );
}

int main( void ) {
    struct fcft_font* f;

    /* 96 dpi: monospace:size=10 at a scale of 1/2 */
    if (!awl_draw_init() || !(f = awl_font_get_exact( "monospace:size=", 20, 0.5f ))) {
        fprintf( stderr, "no font monospace:size=10\n" );
        return 100;
    }
    test_style( f );
    test_emoji();
    awl_font_release( f );
    awl_draw_fini();
    printf( "%d failed\n", fails );
    return fails;
}
