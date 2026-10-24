/* XPM3 without libXpm: see xpm.h. Only what icons use: a pixel's color by
 * "c" (else "g", "g4", "m") as #hex, an X11 name or None, up to MAX_CPP
 * characters a pixel; the hotspot and extensions are ignored. The sizes
 * and the number of colors are checked against the limits and the data's
 * length before anything is allocated for them. Refused: a string or
 * comment the data doesn't close, a row shorter than the width, a pixel
 * of no color, fewer rows than the height. */

#include "xpm.h"
#include "../awl_log.h"

#include <stdlib.h>
#include <string.h>

#include "xpm_colors.h"

enum {
    /** characters a pixel at most (libXpm takes more, no icon has them) */
    MAX_CPP = 4,
    /** a color's name or value at most, without its spaces */
    MAX_NAME = 64,
};

typedef struct {
    const uint8_t *p, *end;
} cursor_t;

typedef struct {
    uint32_t key, argb;
} color_t;

static int is_space( uint8_t c ) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int awl_xpm_is( const uint8_t* d, size_t len ) {
    const uint8_t *p = d, *end = d + len;
    while (p < end && is_space( *p )) p++;
    if (end - p < 2 || p[0] != '/' || p[1] != '*') return 0;
    for (p += 2; p < end && is_space( *p ); p++);
    if (end - p < 3 || memcmp( p, "XPM", 3 )) return 0;
    for (p += 3; p < end && is_space( *p ); p++);
    return end - p >= 2 && p[0] == '*' && p[1] == '/';
}

/** the next string's contents, without its quotes (no escapes: XPM has
 * none); 1, 0 at the array's or data's end, -1 if a string or comment
 * isn't closed */
static int next_string( cursor_t* c, const uint8_t** s, size_t* n ) {
    const uint8_t *p = c->p, *end = c->end;
    while (p < end) {
        if (*p == '"') {
            const uint8_t* q = p + 1;
            while (q < end && *q != '"' && *q != '\n') q++;
            if (q >= end || *q != '"') return -1;
            *s = p + 1;
            *n = (size_t)(q - p - 1);
            c->p = q + 1;
            return 1;
        }
        if (*p == '/' && end - p >= 2 && p[1] == '*') {
            const uint8_t* q = p + 2;
            while (end - q >= 2 && !(q[0] == '*' && q[1] == '/')) q++;
            if (end - q < 2) return -1;
            p = q + 2;
        } else if (*p == '/' && end - p >= 2 && p[1] == '/') {
            while (p < end && *p != '\n') p++;
        } else if (*p == '}') {
            break;
        } else p++;
    }
    c->p = end;
    return 0;
}

/** a decimal number of at most 7 digits at *p, after white space; -1 if
 * there's none */
static long number( const uint8_t** p, const uint8_t* end ) {
    const uint8_t* q = *p;
    while (q < end && is_space( *q )) q++;
    long v = 0;
    int digits = 0;
    for (; q < end && *q >= '0' && *q <= '9'; q++)
        if (++digits > 7) return -1;
        else v = v * 10 + (*q - '0');
    *p = q;
    return digits ? v : -1;
}

static int hex( uint8_t c ) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int by_name( const void* key, const void* entry ) {
    return strcmp( key, *(const char* const*)entry );
}

/** name (lower case, without spaces) as opaque ARGB or 0 for None; -1 if
 * it is no color known */
static int64_t color_value( const char* name ) {
    size_t n = strlen( name );
    if (!strcmp( name, "none" )) return 0;
    if (name[0] == '#') {
        /* 1 to 4 hex digits a channel, their top 8 bits */
        size_t d = (n - 1) / 3;
        if (n < 4 || (n - 1) % 3 || d > 4) return -1;
        uint32_t argb = 0xff000000;
        for (int ch = 0; ch < 3; ch++) {
            uint32_t v = 0;
            for (size_t i = 0; i < d; i++) {
                int h = hex( (uint8_t)name[1 + ch * d + i] );
                if (h < 0) return -1;
                v = v << 4 | (uint32_t)h;
            }
            v = d == 1 ? v * 17 : v >> (4 * d - 8);
            argb |= v << (16 - 8 * ch);
        }
        return argb;
    }
    const xpm_color_t* hit = bsearch( name, xpm_colors, sizeof xpm_colors / sizeof xpm_colors[0],
                                      sizeof xpm_colors[0], by_name );
    return hit ? (int64_t)(0xff000000 | hit->rgb) : -1;
}

/** a color line after its key: the value of "c", else "g", "g4", "m" ("s",
 * the symbolic name, isn't a color), its words joined in lower case into
 * name; 0 if there's none or it is too long */
static int pick_value( const uint8_t* s, const uint8_t* end, char name[MAX_NAME] ) {
    /* by preference */
    static const char* const keywords[] = { "c", "g", "g4", "m", "s" };
    enum { S = 4 };
    char found[5][MAX_NAME];
    size_t used[5] = { 0 };
    int current = -1;
    while (s < end) {
        while (s < end && is_space( *s )) s++;
        const uint8_t* w = s;
        while (s < end && !is_space( *s )) s++;
        size_t n = (size_t)(s - w);
        if (!n) break;
        int kw = -1;
        for (int i = 0; i < 5; i++)
            if (strlen( keywords[i] ) == n && !memcmp( w, keywords[i], n )) kw = i;
        /* a keyword starts a value, unless it would be one's first word */
        if (kw >= 0 && (current < 0 || used[current])) {
            current = kw;
            used[kw] = 0;
            continue;
        }
        if (current < 0) return 0;
        /* the symbolic name only has to have a word */
        if (current == S) {
            used[S] = 1;
            continue;
        }
        if (used[current] + n >= MAX_NAME) return 0;
        for (size_t i = 0; i < n; i++)
            found[current][used[current]++] = (char)(w[i] >= 'A' && w[i] <= 'Z' ? w[i] - 'A' + 'a' : w[i]);
        found[current][used[current]] = 0;
    }
    for (int i = 0; i < S; i++)
        if (used[i]) {
            memcpy( name, found[i], used[i] + 1 );
            return 1;
        }
    return 0;
}

static int by_key( const void* a_, const void* b_ ) {
    const color_t *a = a_, *b = b_;
    return a->key < b->key ? -1 : a->key > b->key;
}

static uint32_t key_of( const uint8_t* s, int cpp ) {
    uint32_t k = 0;
    for (int i = 0; i < cpp; i++) k = k << 8 | s[i];
    return k;
}

pixman_image_t* awl_xpm_decode( const uint8_t* data, size_t len, const awl_icon_limits_t* lim, const char** err ) {
    cursor_t c = { data, data + len };
    const uint8_t* s;
    size_t n;
    color_t* colors = NULL;
    pixman_image_t* img = NULL;
    int unknown = 0;
    *err = NULL;

    if (!awl_xpm_is( data, len )) {
        *err = "not an XPM";
        return NULL;
    }
    /* "width height colors chars-a-pixel [hotspot] [XPMEXT]" */
    if (next_string( &c, &s, &n ) != 1) {
        *err = "no values";
        return NULL;
    }
    const uint8_t* v = s;
    long w = number( &v, s + n ), h = number( &v, s + n ), nc = number( &v, s + n ), cpp = number( &v, s + n );
    if (w <= 0 || h <= 0 || nc <= 0 || cpp <= 0) {
        *err = "broken values";
        return NULL;
    }
    if (w > lim->dim || h > lim->dim || (uint64_t)w * (uint64_t)h > lim->pixels) {
        *err = "too large";
        return NULL;
    }
    /* each color's line has its key, a keyword and a value at least; no
     * more colors than keys */
    if (cpp > MAX_CPP || (uint64_t)nc > len / (size_t)(cpp + 3) || (cpp < 4 && nc > 1L << (8 * cpp))) {
        *err = "too many colors or characters a pixel";
        return NULL;
    }
    colors = malloc( (size_t)nc * sizeof(*colors) );
    if (!colors) {
        *err = "out of memory";
        return NULL;
    }
    for (long i = 0; i < nc; i++) {
        char name[MAX_NAME];
        if (next_string( &c, &s, &n ) != 1 || n < (size_t)cpp) {
            *err = "colors missing";
            goto out;
        }
        if (!pick_value( s + cpp, s + n, name )) {
            *err = "a color without a value";
            goto out;
        }
        int64_t argb = color_value( name );
        if (argb < 0) {
            if (!unknown++) awl_vrb_printf( "icon: XPM: unknown color '%s', black instead\n", name );
            argb = 0xff000000;
        }
        colors[i] = (color_t){ key_of( s, (int)cpp ), (uint32_t)argb };
    }
    qsort( colors, (size_t)nc, sizeof(*colors), by_key );

    img = pixman_image_create_bits( PIXMAN_a8r8g8b8, (int)w, (int)h, NULL, 0 );
    if (!img) {
        *err = "out of memory";
        goto out;
    }
    uint32_t* px = pixman_image_get_data( img );
    const color_t* last = &colors[0];
    for (long y = 0; y < h; y++) {
        if (next_string( &c, &s, &n ) != 1 || n < (size_t)(w * cpp)) {
            *err = "rows missing or short";
            goto out;
        }
        for (long x = 0; x < w; x++, s += cpp) {
            color_t k = { key_of( s, (int)cpp ), 0 };
            /* runs of one color are common */
            if (last->key != k.key) last = bsearch( &k, colors, (size_t)nc, sizeof(*colors), by_key );
            if (!last) {
                *err = "a pixel of no color";
                goto out;
            }
            px[y * w + x] = last->argb;
        }
    }
out:
    if (*err && img) {
        pixman_image_unref( img );
        img = NULL;
    }
    free( colors );
    return img;
}
