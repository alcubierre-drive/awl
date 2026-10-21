/* Unit test of icons/icon.c's decoding: PNG and JPEG through Wuffs, from
 * the files in test/data (the first argument): their pixels, what is
 * refused, and truncated and mutated copies, which must fail or decode but
 * never crash (best run under ASan); and notifications' raw RGB(A) image
 * data, awl_icon_from_rgba(). Built and run by meson test; prints
 * ok/FAIL per check, the exit status is the number of failures. */
#include "icons/icon.h"
#include "awl_host.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK( cond ) do { \
        if (cond) printf( "ok    %s\n", #cond ); \
        else { printf( "FAIL  %s (line %d)\n", #cond, __LINE__ ); fails++; } \
    } while (0)

/* icon.c logs through the host table, as in the library */
static int verbose;
static void log_to_stdout( awl_loglevel_t level, const char* fmt, ... ) {
    va_list ap;
    if (level == awl_loglevel_vrb && !verbose) return;
    va_start( ap, fmt );
    vprintf( fmt, ap );
    va_end( ap );
}
static awl_host_t host = { .log = log_to_stdout };
const awl_host_t* awl_host = &host;

static const char* dir;

static uint8_t* load( const char* name, size_t* len ) {
    char path[4096];
    snprintf( path, sizeof( path ), "%s/%s", dir, name );
    uint8_t* data = awl_icon_read( path, len );
    if (!data) printf( "      can't read %s\n", path );
    return data;
}

static pixman_image_t* decode( const char* name, int max_w, int max_h ) {
    size_t len;
    uint8_t* data = load( name, &len );
    pixman_image_t* img = data ? awl_icon_decode( data, len, 0, max_w, max_h, name ) : NULL;
    free( data );
    return img;
}

static uint32_t pixel( pixman_image_t* img, int x, int y ) {
    return pixman_image_get_data( img )[y * pixman_image_get_stride( img ) / 4 + x];
}

/* a within tol of b in each channel */
static int near( uint32_t a, uint32_t b, int tol ) {
    for (int s = 0; s < 32; s += 8) {
        int d = (int)(a >> s & 0xff) - (int)(b >> s & 0xff);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

/* red on the left, blue on the right, 64 x 48 */
static int red_blue( const char* name ) {
    pixman_image_t* img = decode( name, 64, 48 );
    int ok = img && pixman_image_get_width( img ) == 64 && pixman_image_get_height( img ) == 48
             && near( pixel( img, 8, 24 ), 0xffff0000, 12 ) && near( pixel( img, 56, 24 ), 0xff0000ff, 12 );
    if (img && !ok) printf( "      %s: %08x %08x\n", name, pixel( img, 8, 24 ), pixel( img, 56, 24 ) );
    if (img) pixman_image_unref( img );
    return ok;
}

static void test_decode( void ) {
    pixman_image_t* img;
    printf( "-- decode\n" );
    CHECK( red_blue( "baseline.jpg" ) );
    CHECK( red_blue( "progressive.jpg" ) );
    CHECK( red_blue( "444.jpg" ) );
    CHECK( red_blue( "cmyk.jpg" ) );
    /* gray: the same in each channel */
    img = decode( "gray.jpg", 64, 48 );
    CHECK( img && pixel( img, 8, 24 ) >> 24 == 0xff && (pixel( img, 8, 24 ) & 0xff) == (pixel( img, 8, 24 ) >> 16 & 0xff) );
    if (img) pixman_image_unref( img );
    /* PNG still: premultiplied, the right half blue at half alpha */
    img = decode( "rb.png", 64, 48 );
    CHECK( img && pixel( img, 8, 24 ) == 0xffff0000 && near( pixel( img, 56, 24 ), 0x80000080, 1 ) );
    if (img) pixman_image_unref( img );
    /* fitted: scaled down keeping the aspect */
    img = decode( "baseline.jpg", 32, 32 );
    CHECK( img && pixman_image_get_width( img ) == 32 && pixman_image_get_height( img ) == 24 );
    if (img) pixman_image_unref( img );
    /* the largest allowed, progressive 4:4:4: its work buffer under the cap */
    img = decode( "progressive-1024.jpg", 64, 64 );
    CHECK( img && pixman_image_get_width( img ) == 64 && near( pixel( img, 32, 32 ), 0xff00a000, 12 ) );
    if (img) pixman_image_unref( img );
}

static void test_refused( void ) {
    size_t len;
    printf( "-- refused\n" );
    /* Wuffs has no arithmetic decoding */
    CHECK( !decode( "arithmetic.jpg", 64, 48 ) );
    /* 1100 wide, past AWL_ICON_MAX_DIM */
    CHECK( !decode( "wide.jpg", 64, 48 ) );
    /* the magic alone, garbage */
    CHECK( !awl_icon_decode( "\xff\xd8\xff", 3, 0, 64, 64, "magic" ) );
    CHECK( !awl_icon_decode( "\xff\xd8\xff\xe0garbage", 11, 0, 64, 64, "garbage" ) );
    /* not a JPEG without SOI */
    uint8_t* data = load( "baseline.jpg", &len );
    CHECK( data && !awl_icon_decode( data + 2, len - 2, 0, 64, 64, "no SOI" ) );
    /* every truncation fails or decodes, but none may crash; the whole
     * file without its EOI is still an image */
    int decoded = 0;
    for (size_t n = 0; data && n < len; n++) {
        pixman_image_t* img = awl_icon_decode( data, n, 0, 64, 48, "truncated" );
        if (img) {
            decoded++;
            pixman_image_unref( img );
        }
    }
    CHECK( decoded < 8 );
    free( data );
}

/* each file with random bytes changed: decodes or not, never a crash */
static void test_mutated( void ) {
    static const char* names[] = { "baseline.jpg", "progressive.jpg", "cmyk.jpg", "gray.jpg", "rb.png" };
    int runs = 0;
    printf( "-- mutated\n" );
    srand( 1 );
    for (size_t f = 0; f < sizeof( names ) / sizeof( names[0] ); f++) {
        size_t len;
        uint8_t* orig = load( names[f], &len );
        uint8_t* data = orig ? malloc( len ) : NULL;
        for (int i = 0; data && i < 2000; i++) {
            memcpy( data, orig, len );
            for (int k = 1 + rand() % 4; k > 0; k--) {
                size_t at = (size_t)rand() % len;
                /* a bit, a byte, a likely length or marker */
                switch (rand() % 3) {
                case 0: data[at] ^= (uint8_t)(1 << rand() % 8); break;
                case 1: data[at] = (uint8_t)rand(); break;
                default: data[at] = rand() % 2 ? 0xff : 0x00; break;
                }
            }
            pixman_image_t* img = awl_icon_decode( data, len, 0, 64, 48, "mutated" );
            if (img) pixman_image_unref( img );
            runs++;
        }
        free( data );
        free( orig );
    }
    CHECK( runs == 5 * 2000 );
}

/* awl_icon_from_rgba(): the image-data hint's pixels, and what is refused */
static int rgba_refused( const uint8_t* data, size_t len, int w, int h, int stride, int alpha, int bits, int ch ) {
    pixman_image_t* img = awl_icon_from_rgba( data, len, w, h, stride, alpha, bits, ch, 64, 64 );
    if (img) pixman_image_unref( img );
    return !img;
}

static void test_rgba( void ) {
    pixman_image_t* img;
    printf( "-- rgba\n" );
    /* 2 x 2 RGBA: opaque red, half-alpha white, transparent green, half-alpha blue */
    static const uint8_t rgba[] = {
        0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x80,
        0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80,
    };
    img = awl_icon_from_rgba( rgba, sizeof rgba, 2, 2, 8, 1, 8, 4, 2, 2 );
    CHECK( img && pixman_image_get_width( img ) == 2 && pixman_image_get_height( img ) == 2 );
    CHECK( img && pixel( img, 0, 0 ) == 0xffff0000 && pixel( img, 1, 0 ) == 0x80808080 && pixel( img, 0, 1 ) == 0 &&
           pixel( img, 1, 1 ) == 0x80000080 );
    if (img) pixman_image_unref( img );
    /* RGB: opaque; rows padded to 8 bytes */
    static const uint8_t rgb[] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0xee, 0xee,
        0x70, 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xee, 0xee,
    };
    img = awl_icon_from_rgba( rgb, sizeof rgb, 2, 2, 8, 0, 8, 3, 2, 2 );
    CHECK( img && pixel( img, 0, 0 ) == 0xff102030 && pixel( img, 1, 0 ) == 0xff405060 &&
           pixel( img, 0, 1 ) == 0xff708090 && pixel( img, 1, 1 ) == 0xffa0b0c0 );
    if (img) pixman_image_unref( img );
    /* exactly the minimum: the last row without its padding; a byte less is refused */
    size_t min = 8 + 6;
    img = awl_icon_from_rgba( rgb, min, 2, 2, 8, 0, 8, 3, 2, 2 );
    CHECK( img && pixel( img, 1, 1 ) == 0xffa0b0c0 );
    if (img) pixman_image_unref( img );
    CHECK( rgba_refused( rgb, min - 1, 2, 2, 8, 0, 8, 3 ) );
    CHECK( rgba_refused( rgba, sizeof rgba - 1, 2, 2, 8, 1, 8, 4 ) );
    /* fitted: 40 x 20 of opaque red into 16 x 16 is 16 x 8 */
    uint8_t* big = malloc( 40 * 20 * 4 );
    for (int i = 0; i < 40 * 20; i++) memcpy( big + i * 4, "\xff\x00\x00\xff", 4 );
    img = awl_icon_from_rgba( big, 40 * 20 * 4, 40, 20, 40 * 4, 1, 8, 4, 16, 16 );
    CHECK( img && pixman_image_get_width( img ) == 16 && pixman_image_get_height( img ) == 8 &&
           near( pixel( img, 8, 4 ), 0xffff0000, 2 ) );
    if (img) pixman_image_unref( img );
    free( big );

    /* refused */
    static uint8_t zero[4 * 1025];
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, 8, 1, 16, 4 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, 8, 1, 8, 2 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, 8, 0, 8, 2 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, 8, 1, 8, 5 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, 8, 1, 8, 3 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, 8, 0, 8, 4 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, 7, 1, 8, 4 ) );
    CHECK( rgba_refused( rgb, sizeof rgb, 2, 2, 5, 0, 8, 3 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 0, 2, 8, 1, 8, 4 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 0, 8, 1, 8, 4 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, -2, 2, 8, 1, 8, 4 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, -2, 8, 1, 8, 4 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, 0, 1, 8, 4 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, -8, 1, 8, 4 ) );
    CHECK( rgba_refused( rgba, sizeof rgba, 2, 2, INT32_MIN, 1, 8, 4 ) );
    CHECK( rgba_refused( NULL, 0, 2, 2, 8, 1, 8, 4 ) );
    /* one row past AWL_ICON_MAX_DIM wide, one column high: the data is there */
    CHECK( rgba_refused( zero, sizeof zero, AWL_ICON_MAX_DIM + 1, 1, 4 * (AWL_ICON_MAX_DIM + 1), 1, 8, 4 ) );
    CHECK( rgba_refused( zero, sizeof zero, 1, AWL_ICON_MAX_DIM + 1, 4, 1, 8, 4 ) );
    CHECK( rgba_refused( zero, sizeof zero, INT32_MAX, 1, INT32_MAX, 1, 8, 4 ) );
    /* AWL_ICON_MAX_DIM itself is fine */
    img = awl_icon_from_rgba( zero, sizeof zero, AWL_ICON_MAX_DIM, 1, 4 * AWL_ICON_MAX_DIM, 1, 8, 4, 64, 64 );
    CHECK( img != NULL );
    if (img) pixman_image_unref( img );
    /* overflowing: (h - 1) * rowstride far past len (past SIZE_MAX on 32 bits) */
    CHECK( rgba_refused( zero, sizeof zero, 1, AWL_ICON_MAX_DIM, INT32_MAX, 1, 8, 4 ) );
    CHECK( rgba_refused( zero, sizeof zero, AWL_ICON_MAX_DIM, AWL_ICON_MAX_DIM, INT32_MAX, 0, 8, 3 ) );
    /* the fitted size asked for is checked too */
    img = awl_icon_from_rgba( rgba, sizeof rgba, 2, 2, 8, 1, 8, 4, 0, 64 );
    CHECK( img == NULL );
    img = awl_icon_from_rgba( rgba, sizeof rgba, 2, 2, 8, 1, 8, 4, 64, AWL_ICON_MAX_DIM + 1 );
    CHECK( img == NULL );
}

int main( int argc, char** argv ) {
    dir = argc > 1 ? argv[1] : "test/data";
    verbose = getenv( "VERBOSE" ) != NULL;
    test_decode();
    test_refused();
    test_mutated();
    test_rgba();
    printf( "%d failed\n", fails );
    return fails;
}
