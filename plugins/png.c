/* The screenshots' PNG encoder, see png.h. */

#include "png.h"
#include "../vendor/sdefl/sdefl.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** a row with at least this share of pixels like their left neighbors
 * stays unfiltered (measured on screenshots, photos and both mixed: lower
 * puts photo rows there, higher text rows to Paeth, either deflating worse) */
#define REPEATS_NONE 0.7

/** the PNG filter types png.c uses */
enum png_filter {
    FILTER_NONE = 0,
    FILTER_PAETH = 4,
};

/** the CRC-32 of PNG's chunks (and zlib's), by the byte; made once, by
 * whichever thread encodes first */
static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_init( void ) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32_of( const unsigned char* p, size_t n ) {
    uint32_t c = 0xffffffffu;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static unsigned char* be32( unsigned char* p, uint32_t v ) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
    return p + 4;
}

/** a chunk's CRC after its data, which starts at p (the type) and is n
 * bytes long with it */
static unsigned char* chunk_end( unsigned char* p, size_t n ) {
    return be32( p + n, crc32_of( p, n ) );
}

static int paeth( int a, int b, int c ) {
    int p = a + b - c, pa = abs( p - a ), pb = abs( p - b ), pc = abs( p - c );
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/** one row of pixels (premultiplied ARGB words) as bytes, bpp of them each:
 * RGB, or RGBA un-premultiplied */
static void row_bytes( unsigned char* o, const uint32_t* px, int w, int bpp ) {
    for (int x = 0; x < w; x++, o += bpp) {
        uint32_t p = px[x];
        unsigned a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
        if (bpp == 4 && a != 255) {
            if (a) {
                r = (r * 255 + a / 2) / a;
                g = (g * 255 + a / 2) / a;
                b = (b * 255 + a / 2) / a;
                r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
            } else {
                r = g = b = 0;
            }
        }
        o[0] = (unsigned char)r;
        o[1] = (unsigned char)g;
        o[2] = (unsigned char)b;
        if (bpp == 4) o[3] = (unsigned char)a;
    }
}

/** the filter for row px (see REPEATS_NONE) */
static int row_filter( const uint32_t* px, int w ) {
    int repeats = 0;
    for (int x = 1; x < w; x++) repeats += px[x] == px[x - 1];
    return repeats >= REPEATS_NONE * (w - 1) ? FILTER_NONE : FILTER_PAETH;
}

unsigned char* awl_png_encode( const uint32_t* px, int w, int h, int stride, int level, size_t* len ) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    unsigned char* out = NULL, * raw = NULL, * prev = NULL, * cur = NULL, * p;
    struct sdefl* s = NULL;
    int bpp = 3, zn;
    size_t cap;

    if (w <= 0 || h <= 0 || stride < w * 4 || !px || !len) return NULL;
    if (level < 0 || level > 8) level = AWL_PNG_LEVEL_DEFAULT;
    /* sdefl's lengths are ints, and its bound adds to them: the rows, as
     * RGBA at most, 1 GiB at most */
    if ((size_t)w * 4 + 1 > ((size_t)1 << 30) / (size_t)h) return NULL;
    pthread_once( &crc_once, crc_init );
    /* RGBA only if it needs it */
    for (int y = 0; y < h && bpp == 3; y++) {
        const uint32_t* r = (const uint32_t*)((const char*)px + (size_t)y * stride);
        for (int x = 0; x < w; x++)
            if (r[x] >> 24 != 0xff) {
                bpp = 4;
                break;
            }
    }
    size_t row = (size_t)w * bpp;
    size_t rawlen = (row + 1) * (size_t)h;

    if (!(raw = malloc( rawlen )) || !(prev = calloc( 1, row )) || !(cur = malloc( row ))
            || !(s = calloc( 1, sizeof(*s) ))) /* sdefl leaves its counts as they are (its issues 58, 61) */
        goto fail;
    for (int y = 0; y < h; y++) {
        const uint32_t* r = (const uint32_t*)((const char*)px + (size_t)y * stride);
        unsigned char* o = raw + (row + 1) * (size_t)y;
        int f = row_filter( r, w );
        row_bytes( cur, r, w, bpp );
        o[0] = (unsigned char)f;
        if (f == FILTER_NONE) {
            memcpy( o + 1, cur, row );
        } else {
            /* a: left, b: above, c: above left; 0 off the image */
            for (size_t i = 0; i < row; i++) {
                int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
                o[1 + i] = (unsigned char)(cur[i] - paeth( a, b, c ));
            }
        }
        unsigned char* t = prev;
        prev = cur, cur = t;
    }

    /* signature, IHDR (8 + 13 + 4), IDAT's head (8) and the zlib stream
     * written in place after it, its CRC, IEND (12) */
    cap = 8 + 25 + 8 + (size_t)sdefl_bound( (int)rawlen ) + 6 + 4 + 12;
    if (!(out = malloc( cap ))) goto fail;
    p = out;
    memcpy( p, sig, 8 );
    p += 8;
    p = be32( p, 13 );
    memcpy( p, "IHDR", 4 );
    be32( p + 4, (uint32_t)w );
    be32( p + 8, (uint32_t)h );
    p[12] = 8;                    /* bits per channel */
    p[13] = bpp == 4 ? 6 : 2;     /* RGBA, RGB */
    p[14] = p[15] = p[16] = 0;    /* deflate, adaptive filters, no interlace */
    p = chunk_end( p, 4 + 13 );

    zn = zsdeflate( s, p + 8, raw, (int)rawlen, level );
    p = be32( p, (uint32_t)zn );
    memcpy( p, "IDAT", 4 );
    p = chunk_end( p, 4 + (size_t)zn );

    p = be32( p, 0 );
    memcpy( p, "IEND", 4 );
    p = chunk_end( p, 4 );

    *len = (size_t)(p - out);
    free( raw );
    free( prev );
    free( cur );
    free( s );
    return out;
fail:
    free( out );
    free( raw );
    free( prev );
    free( cur );
    free( s );
    return NULL;
}
