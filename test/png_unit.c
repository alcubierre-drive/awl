/* Unit test of plugins/png.c, the screenshots' PNG encoder: what it writes
 * is decoded by Wuffs with every checksum checked (CRCs, Adler-32), and
 * must give back the pixels: RGB for opaque images, RGBA un-premultiplied
 * otherwise; rows of both filters, padded strides, a 1x1, an image larger
 * than sdefl's blocks, noise sdefl stores; what is refused; and sdefl
 * itself on input that makes the most sequences per block (vendor/sdefl's
 * awl: change), inflated again by Wuffs. Built and run by
 * meson test; prints ok/FAIL per check, the exit status is the number of
 * failures. */
#include "plugins/png.h"
#include "vendor/sdefl/sdefl.h"
#include "vendor/wuffs/wuffs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK( cond ) do { \
        if (cond) printf( "ok    %s\n", #cond ); \
        else { printf( "FAIL  %s (line %d)\n", #cond, __LINE__ ); fails++; } \
    } while (0)

/** what the encoder should write for premultiplied p: straight RGBA */
static void straight( uint32_t p, uint8_t out[4] ) {
    unsigned a = p >> 24, c[3] = { (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff };
    for (int i = 0; i < 3; i++) {
        unsigned v = a ? (c[i] * 255 + a / 2) / a : 0;
        out[i] = (uint8_t)(v > 255 ? 255 : v);
    }
    out[3] = (uint8_t)a;
}

/** png decoded to straight RGBA (w * h * 4 bytes, which the caller frees),
 * checksums checked; its color type in *type; NULL if Wuffs refuses it */
static uint8_t* decode( const uint8_t* png, size_t len, int w, int h, int* type ) {
    uint8_t* out = NULL, * work = NULL;
    wuffs_png__decoder* dec = malloc( sizeof__wuffs_png__decoder() );
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader( (uint8_t*)png, len, true );
    wuffs_base__image_config ic;
    wuffs_base__pixel_buffer pb;
    wuffs_base__status st;
    *type = len > 25 ? png[25] : -1;
    st = wuffs_png__decoder__initialize( dec, sizeof__wuffs_png__decoder(), WUFFS_VERSION, 0 );
    if (!wuffs_base__status__is_ok( &st )) goto out;
    st = wuffs_png__decoder__decode_image_config( dec, &ic, &src );
    if (!wuffs_base__status__is_ok( &st )) goto out;
    if (wuffs_base__pixel_config__width( &ic.pixcfg ) != (uint32_t)w
            || wuffs_base__pixel_config__height( &ic.pixcfg ) != (uint32_t)h)
        goto out;
    wuffs_base__pixel_config__set( &ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
                                   WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, (uint32_t)w, (uint32_t)h );
    out = malloc( (size_t)w * h * 4 );
    st = wuffs_base__pixel_buffer__set_from_slice( &pb, &ic.pixcfg,
                                                   wuffs_base__make_slice_u8( out, (size_t)w * h * 4 ) );
    if (!wuffs_base__status__is_ok( &st )) goto fail;
    uint64_t wlen = wuffs_png__decoder__workbuf_len( dec ).max_incl;
    work = malloc( wlen ? wlen : 1 );
    st = wuffs_png__decoder__decode_frame( dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
                                           wuffs_base__make_slice_u8( work, wlen ), NULL );
    if (wuffs_base__status__is_ok( &st )) goto out;
fail:
    printf( "      wuffs: %s\n", wuffs_base__status__message( &st ) );
    free( out );
    out = NULL;
out:
    free( work );
    free( dec );
    return out;
}

/** encodes px (w x h, rows stride bytes apart), decodes it and compares;
 * returns whether all matched, and the color type it got in *type */
static int roundtrip( const char* what, const uint32_t* px, int w, int h, int stride, int* type ) {
    size_t len = 0;
    uint8_t* png = awl_png_encode( px, w, h, stride, AWL_PNG_LEVEL_DEFAULT, &len );
    uint8_t* got;
    int ok = 1;
    if (!png) {
        printf( "      %s: not encoded\n", what );
        return 0;
    }
    got = decode( png, len, w, h, type );
    if (!got) {
        printf( "      %s: not decoded (%zu bytes)\n", what, len );
        free( png );
        return 0;
    }
    for (int y = 0; y < h && ok; y++)
        for (int x = 0; x < w && ok; x++) {
            uint8_t want[4];
            straight( ((const uint32_t*)((const char*)px + (size_t)y * stride))[x], want );
            if (memcmp( want, got + ((size_t)y * w + x) * 4, 4 )) {
                printf( "      %s: pixel %d,%d differs\n", what, x, y );
                ok = 0;
            }
        }
    printf( "      %s: %dx%d, %zu bytes\n", what, w, h, len );
    free( got );
    free( png );
    return ok;
}

static uint32_t rnd( void ) {
    static uint32_t s = 2463534242u;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return s;
}

/** p premultiplied by alpha a, from straight r, g, b */
static uint32_t premul( unsigned a, unsigned r, unsigned g, unsigned b ) {
    return a << 24 | ((r * a + 127) / 255) << 16 | ((g * a + 127) / 255) << 8 | (b * a + 127) / 255;
}

/** zlib's stream z inflated by Wuffs into out (n bytes expected): whether it
 * gives exactly n bytes, the Adler-32 checked */
static int inflate( const uint8_t* z, size_t zn, uint8_t* out, size_t n ) {
    wuffs_zlib__decoder* dec = malloc( sizeof__wuffs_zlib__decoder() );
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader( (uint8_t*)z, zn, true );
    wuffs_base__io_buffer dst = wuffs_base__ptr_u8__writer( out, n );
    uint64_t wlen = wuffs_zlib__decoder__workbuf_len( dec ).max_incl;
    uint8_t* work = malloc( wlen ? wlen : 1 );
    wuffs_base__status st = wuffs_zlib__decoder__initialize( dec, sizeof__wuffs_zlib__decoder(), WUFFS_VERSION, 0 );
    if (wuffs_base__status__is_ok( &st ))
        st = wuffs_zlib__decoder__transform_io( dec, &dst, &src, wuffs_base__make_slice_u8( work, wlen ) );
    if (!wuffs_base__status__is_ok( &st )) printf( "      wuffs: %s\n", wuffs_base__status__message( &st ) );
    free( work );
    free( dec );
    return wuffs_base__status__is_ok( &st ) && dst.meta.wi == n;
}

/** sdefl on cells of 5 bytes, a random one and a copy of the cell before's
 * first 4: a literal and a match each, two sequences per 5 bytes */
static int sdefl_most_sequences( void ) {
    int n = 1 << 20, ok = 1;
    uint8_t* in = malloc( (size_t)n ), * z = malloc( (size_t)sdefl_bound( n ) + 6 ), * back = malloc( (size_t)n );
    struct sdefl* s = calloc( 1, sizeof(*s) );
    for (int i = 0; i < n; i++) in[i] = (i < 6 || i % 5 == 0) ? (uint8_t)(rnd() >> 24) : in[i - 6];
    for (int lvl = 0; lvl <= 8 && ok; lvl += 4) {
        int zn = zsdeflate( s, z, in, n, lvl );
        memset( back, 0, (size_t)n );
        ok = inflate( z, (size_t)zn, back, (size_t)n ) && !memcmp( in, back, (size_t)n );
        printf( "      sdefl level %d: %d bytes\n", lvl, zn );
    }
    free( s );
    free( back );
    free( z );
    free( in );
    return ok;
}

int main( void ) {
    int type;
    size_t len;

    {
        uint32_t one = 0xff336699;
        CHECK( roundtrip( "1x1", &one, 1, 1, 4, &type ) && type == 2 );
    }
    {
        /* noise: every row Paeth's */
        int w = 37, h = 23;
        uint32_t* px = malloc( (size_t)w * h * 4 );
        for (int i = 0; i < w * h; i++) px[i] = 0xff000000u | (rnd() & 0xffffff);
        CHECK( roundtrip( "noise", px, w, h, w * 4, &type ) && type == 2 );
        free( px );
    }
    {
        /* "text": flat rows (unfiltered) with marks, and gradient rows
         * (Paeth) between them, rows padded */
        int w = 301, h = 97, stride = (w + 3) * 4;
        uint32_t* px = calloc( (size_t)h, (size_t)stride );
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t* p = (uint32_t*)((char*)px + (size_t)y * stride) + x;
                if (y % 3 == 2) *p = 0xff000000u | (uint32_t)(x * 255 / w) << 16 | (uint32_t)y << 8 | (uint32_t)(x ^ y);
                else *p = (x % 17 == 3 && y % 5) ? 0xffeeeeee : 0xff204080;
            }
        CHECK( roundtrip( "text and gradients, padded", px, w, h, stride, &type ) && type == 2 );
        free( px );
    }
    {
        /* alpha: 0, 255 and everything between, premultiplied */
        int w = 64, h = 33;
        uint32_t* px = malloc( (size_t)w * h * 4 );
        for (int i = 0; i < w * h; i++) {
            uint32_t r = rnd();
            px[i] = premul( i % 7 == 0 ? 0 : i % 7 == 1 ? 255 : r >> 24, r & 0xff, (r >> 8) & 0xff, (r >> 16) & 0xff );
        }
        CHECK( roundtrip( "alpha", px, w, h, w * 4, &type ) && type == 6 );
        free( px );
    }
    {
        /* a screen's worth, over sdefl's 256 KiB blocks: flat with noise
         * stripes (stored blocks for them) */
        int w = 1920, h = 1080;
        uint32_t* px = malloc( (size_t)w * h * 4 );
        for (int i = 0; i < w * h; i++)
            px[i] = (i / w) % 200 < 20 ? 0xff000000u | (rnd() & 0xffffff) : 0xff204080;
        CHECK( roundtrip( "1920x1080", px, w, h, w * 4, &type ) && type == 2 );
        /* flat: deflates to little */
        for (int i = 0; i < w * h; i++) px[i] = 0xff204080;
        uint8_t* png = awl_png_encode( px, w, h, w * 4, AWL_PNG_LEVEL_DEFAULT, &len );
        CHECK( png && len < 20000 );
        free( png );
        free( px );
    }
    {
        uint32_t px[4] = { 0xff000000, 0xff000000, 0xff000000, 0xff000000 };
        CHECK( !awl_png_encode( px, 0, 1, 4, 5, &len ) );
        CHECK( !awl_png_encode( px, 2, 2, 4, 5, &len ) ); /* stride < w * 4 */
        CHECK( !awl_png_encode( NULL, 1, 1, 4, 5, &len ) );
        CHECK( !awl_png_encode( px, 1 << 20, 1 << 20, 4 << 20, 5, &len ) ); /* too large */
    }

    CHECK( sdefl_most_sequences() );

    printf( "%d failed\n", fails );
    return fails;
}
