/*
 * drwl - https://codeberg.org/sewn/drwl
 * See LICENSE for copyright and license details.
 */
#pragma once

/*
 * Text, drawn with a font from the font registry into any pixman image
 * (awl_draw_text() & co.), and the colors and shapes around it. The bar
 * that draws with it is awl_bar_type.h's. No wlroots here.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcft/fcft.h>
#include <pixman-1/pixman.h>

#define BETWEEN(X, A, B) ((A) <= (X) && (X) <= (B))


#define UTF_INVALID 0xFFFD
#define UTF_SIZ     4

static const unsigned char utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static const unsigned char utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static const uint32_t utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static const uint32_t utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

static inline uint32_t
utf8decodebyte(const char c, size_t *i)
{
    for (*i = 0; *i < (UTF_SIZ + 1); ++(*i))
        if (((unsigned char)c & utfmask[*i]) == utfbyte[*i])
            return (unsigned char)c & ~utfmask[*i];
    return 0;
}

static inline size_t
utf8decode(const char *c, uint32_t *u)
{
    size_t i, j, len, type;
    uint32_t udecoded;

    *u = UTF_INVALID;
    udecoded = utf8decodebyte(c[0], &len);
    if (!BETWEEN(len, 1, UTF_SIZ))
        return 1;
    for (i = 1, j = 1; i < UTF_SIZ && j < len; ++i, ++j) {
        udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
        if (type)
            return j;
    }
    if (j < len)
        return 0;
    *u = udecoded;
    if (!BETWEEN(*u, utfmin[len], utfmax[len]) || BETWEEN(*u, 0xD800, 0xDFFF))
        *u = UTF_INVALID;
    for (i = 1; *u > utfmax[i]; ++i)
        ;
    return len;
}

/** 0xRRGGBBAA as pixman's color */
static inline pixman_color_t
color_8bit_to_16bit(uint32_t c)
{
    return (pixman_color_t){
        ((c >> 24) & 0xFF) * 0x101,
        ((c >> 16) & 0xFF) * 0x101,
        ((c >> 8) & 0xFF) * 0x101,
        (c & 0xFF) * 0x101
    };
}

/** pixman's color as 0xRRGGBBAA */
static inline uint32_t
color_16bit_to_8bit(pixman_color_t c)
{
    return (uint32_t)(c.red >> 8) << 24 | (c.green >> 8) << 16 | (c.blue >> 8) << 8 | (c.alpha >> 8);
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

static inline int
awl_draw_stride(unsigned int width)
{
    return (((PIXMAN_FORMAT_BPP(PIXMAN_a8r8g8b8) * width + 7) / 8 + 4 - 1) & -4);
}

/** fcft's setup; once, later calls return the first's result */
int awl_draw_init(void);

/** :c:func:`awl_font_get`'s last resort, as :c:member:`awl_config_t.font` and fontsize */
#define AWL_FONT_FALLBACK "monospace:size="
#define AWL_FONT_FALLBACK_SIZE 10

/** the registry's size: at most this many fonts loaded (or failed) */
#define AWL_FONT_SLOTS 32
/** the longest font spec (name with size), NUL included */
#define AWL_FONT_SPEC_MAX 128

/**
 * The font registry, every font awl draws with: one fcft instance per
 * font, size and scale, shared by everyone asking for it. name is as
 * :c:member:`awl_config_t.font` (size gets appended, as ":size=" unless name ends
 * in '='), at size times scale and dpi 192 * scale. A size or scale that
 * isn't positive and finite takes 10 or 1; huge ones are clamped.
 *
 * A static ring of :c:macro:`AWL_FONT_SLOTS`: past that the oldest font nobody holds
 * is unloaded (one in use never is). A font given back stays loaded until
 * then. One that doesn't load is logged once and not tried again until
 * :c:func:`awl_font_retry`.
 *
 * Returns that font, else :c:macro:`AWL_FONT_FALLBACK` at size, else at
 * AWL_FONT_FALLBACK_SIZE; NULL only when fontconfig has no font at all, or
 * all slots are in use (then awl draws no text). Give each back with
 * :c:func:`awl_font_release`. Main thread; loading a new font blocks on fontconfig
 * and the font files.
 */
struct fcft_font *awl_font_get(const char *name, float size, float scale);
/** like :c:func:`awl_font_get`, but NULL instead of a fallback */
struct fcft_font *awl_font_get_exact(const char *name, float size, float scale);
/** like :c:func:`awl_font_get_exact`, but in emoji presentation (emoji with a text
 * default, like 🖥, in color too), and NULL if the font draws emoji in black
 * and white (then a text font's would come out too), not in color */
struct fcft_font *awl_font_get_emoji(const char *name, float size, float scale);
/** :c:func:`awl_font_style`'s styles, a mask: bold */
#define AWL_FONT_BOLD 1
/** italic */
#define AWL_FONT_ITALIC 2
/**
 * font (from the registry) in style besides its own: the same name, size
 * and scale with fontconfig's ":weight=bold", ":slant=italic". Fontconfig
 * picks the family's Bold or Italic face, else synthesizes one (embolden, an
 * oblique matrix) by its usual config; fcft applies both. A weight or slant
 * in the font's own name comes first in the pattern and wins.
 *
 * Takes a reference like :c:func:`awl_font_get`: give it back with
 * :c:func:`awl_font_release`. If the styled font doesn't load, for style 0, a style
 * font already has, and for emoji fonts, font itself comes back (with a new
 * reference), so callers needn't fall back. NULL only for a font that isn't
 * the registry's (or NULL). Main thread.
 */
struct fcft_font *awl_font_style(struct fcft_font *font, int style);
/** gives back a font from :c:func:`awl_font_get`; NULL, or a font that isn't the
 * registry's (logged), does nothing */
void awl_font_release(struct fcft_font *font);
/** makes the next get try the fonts that didn't load again (after a config
 * reload: a font may have been installed meanwhile) */
void awl_font_retry(void);

/** an image to draw into: w x h pixels of premultiplied ARGB at bits, stride
 * bytes a row, clipped to them. Give it back with pixman_image_unref(). */
pixman_image_t *awl_draw_image(unsigned int w, unsigned int h, uint32_t *bits, int stride);
/** fills the box at x, y of pix with bg and draws text into it in font, lpad
 * from its left edge, in fg, cut with an ellipsis at w; returns the x after
 * the box. NULL pix or font draws nothing. */
int awl_draw_text(pixman_image_t *pix, struct fcft_font *font, int x, int y,
        unsigned int w, unsigned int h, unsigned int lpad, const char *text,
        pixman_color_t fg, pixman_color_t bg);
/** like :c:func:`awl_draw_text`, but without padding and over what is already
 * there instead of a filled box; fg is 0xRRGGBBAA */
int awl_draw_text_over(pixman_image_t *pix, struct fcft_font *font, int x, int y,
        unsigned int w, unsigned int h, const char *text, uint32_t fg);
/** width of text in font in pixels, kerning included; 0 without a font */
unsigned int awl_draw_text_width(struct fcft_font *font, const char *text);

/** fcft's teardown */
void awl_draw_fini(void);
