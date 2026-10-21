/*
 * drwl - https://codeberg.org/sewn/drwl
 * See LICENSE for copyright and license details.
 */
#pragma once

/*
 * Two parts: text, drawn with a font from the font registry into any
 * pixman image (awl_draw_text() & co.), and the bar, `awl_draw_t`, which
 * is part of its Monitor (Monitor.drw) and has its own font and widgets.
 * No wlroots here; awl.h includes this for Monitor.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcft/fcft.h>
#include <pixman-1/pixman.h>

#define BETWEEN(X, A, B) ((A) <= (X) && (X) <= (B))


typedef struct widget_t widget_t;
typedef struct awl_draw_t awl_draw_t;
typedef struct Client Client;
typedef struct Monitor Monitor;
typedef struct awl_window awl_window_t;

/**
 * A part of the bar: the tags, the layout symbol, the task bar, the clock,
 * the graphs... Each bar has its own, made by `awl_plugin_api_t.bar_widgets`
 * (plugins_widgets.c); everything but `bar` and `width` is the library's.
 */
struct widget_t {
    /** what the last draw used, set by awl */
    uint32_t width;
    /**
     * Right-hand widgets are laid out from the bar's right edge, so their
     * width has to be known before anything is drawn: measure() takes a
     * snapshot of whatever the widget shows and returns its exact width, then
     * draw() renders that snapshot at x into exactly that width (its return
     * value is ignored). Left and center widgets only have draw(), which
     * returns the width it used.
     */
    uint32_t (*measure)(widget_t* this);
    /** see `measure` */
    uint32_t (*draw)(widget_t* this, uint32_t x, pixman_image_t* pix);
    /** pointer motion over the widget, x_rel from its left edge (buffer
     * pixels); -1 once it left the widget (for one that isn't, or awl's
     * popups, a client over the bar), and before the plugins detach */
    void (*callback_view)(widget_t* this, int32_t x_rel);
    /** a click, button as ``BTN_*`` */
    void (*callback_click)(widget_t* this, uint32_t x_rel, int button);
    /** a scroll, amount 1 or -1 (see `scroll_amount`) */
    void (*callback_scroll)(widget_t* this, uint32_t x_rel, int amount);
    /** called once the pointer has rested on the widget for hover_delay_ms;
     * leaving it before then cancels, and it fires again only after the
     * pointer has left and come back */
    void (*callback_hover)(widget_t* this);
    /** see `callback_hover` */
    uint32_t hover_delay_ms;
    /** called leave_delay_ms after the pointer has left a widget whose
     * callback_hover fired, unless it came back first (0: right away). While
     * the pointer is inside popup_window (the popup the hover opened), it
     * counts as still on the widget. A press on the widget calls it at
     * once (and cancels a hover not yet fired); hovering fires again only
     * after the pointer has left and come back. */
    void (*callback_leave)(widget_t* this);
    /** see `callback_leave` */
    uint32_t leave_delay_ms;
    /** see `callback_leave`: the widget's awl_window (awl_host_t's
     * window_create()), if its popup is one; NULL if none */
    awl_window_t* popup_window;
    /** awl's sum of scroll deltas since the last `callback_scroll`, which
     * comes once it is large enough */
    double scroll_amount;
    /** the widget's own state, freed with `free` */
    void* userdata;
    /** frees `userdata`, when the widget goes away */
    void (*free)( void* userdata );
    /** the bar it is on */
    awl_draw_t* bar;
};

/** A window as the task bar shows it, a snapshot taken by awl. */
typedef struct awl_draw_window_t {
    char name[255];
    struct { uint8_t
        floating:1,
        urgent:1,
        focused:1,
        visible:1,
        maximized:1,
        fullscreen:1,
        ontop:1;
    };
    Client* c;
} awl_draw_window_t;

/** A bar, Monitor.drw: its font, its widgets and what awl tells them,
 * which awl sets before every redraw. */
struct awl_draw_t {
    /** the bar's image while awl draws it (between awl_draw_prepare_drawing()
     * and awl_draw_finish_drawing()), else NULL */
    pixman_image_t *pix;
    /** the bar's font, at its monitor's scale; NULL if none loads */
    struct fcft_font *font;
    /** its emoji font (`awl_config_t.emoji_font`) at the same size and
     * scale, color emoji (awl_font_get_emoji()); NULL if there is none or
     * it doesn't load: then the widgets show text instead of emoji */
    struct fcft_font *emojifont;

    widget_t widgets_left[32];
    int n_widgets_left;
    widget_t widgets_right[32];
    int n_widgets_right;
    widget_t center_widget;
    uint32_t center_widget_space;
    uint32_t center_widget_start;
    int has_center_widget;

    /** the current tag's windows */
    awl_draw_window_t tagwindows[128];
    int n_tagwindows;
    /** occ, urg and sel: tag masks of the occupied, urgent and selected
     * tags */
    uint32_t occ, urg, sel;
    int ntags;
    /** nonzero on the bar of the selected monitor, -1 if only one monitor exists */
    int selmon;
    /** nonzero if the monitor gives each tag its own layout (Monitor.pertag) */
    int pertag;
    /** the monitor's layout symbol (Monitor.ltsymbol) */
    char ltsymbol[16];
    /** what the widgets need of the monitor, so they never read it: its
     * output's name (valid while the bar is) and scale, the bar's height
     * and text padding (Monitor.lrpad, left and right together) in buffer
     * pixels, and where the monitor is in the layout (logical pixels) */
    const char *output;
    float scale;
    int height, lrpad;
    int x, y;

    /** the bar's monitor, opaque to the library (to hand back to awl, e.g.
     * `awl_host_t.window_show`); NULL before awl_draw_setup() and after
     * awl_draw_teardown(), while the monitor has no bar */
    Monitor* m;
};

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

/** awl_font_get()'s last resort, as `awl_config_t.font` and fontsize */
#define AWL_FONT_FALLBACK "monospace:size="
#define AWL_FONT_FALLBACK_SIZE 10

/** the registry's size: at most this many fonts loaded (or failed) */
#define AWL_FONT_SLOTS 32
/** the longest font spec (name with size), NUL included */
#define AWL_FONT_SPEC_MAX 128

/**
 * The font registry, every font awl draws with: one fcft instance per
 * font, size and scale, shared by everyone asking for it. name is as
 * `awl_config_t.font` (size gets appended, as ":size=" unless name ends
 * in '='), at size times scale and dpi 192 * scale. A size or scale that
 * isn't positive and finite takes 10 or 1; huge ones are clamped.
 *
 * A static ring of AWL_FONT_SLOTS: past that the oldest font nobody holds
 * is unloaded (one in use never is). A font given back stays loaded until
 * then. One that doesn't load is logged once and not tried again until
 * awl_font_retry().
 *
 * Returns that font, else AWL_FONT_FALLBACK at size, else at
 * AWL_FONT_FALLBACK_SIZE; NULL only when fontconfig has no font at all, or
 * all slots are in use (then awl draws no text). Give each back with
 * awl_font_release(). Main thread; loading a new font blocks on fontconfig
 * and the font files.
 */
struct fcft_font *awl_font_get(const char *name, float size, float scale);
/** like awl_font_get(), but NULL instead of a fallback */
struct fcft_font *awl_font_get_exact(const char *name, float size, float scale);
/** like awl_font_get_exact(), but in emoji presentation (emoji with a text
 * default, like 🖥, in color too), and NULL if the font draws emoji in black
 * and white (then a text font's would come out too), not in color */
struct fcft_font *awl_font_get_emoji(const char *name, float size, float scale);
/** gives back a font from awl_font_get(); NULL, or a font that isn't the
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
/** like `awl_draw_text()`, but without padding and over what is already
 * there instead of a filled box; fg is 0xRRGGBBAA */
int awl_draw_text_over(pixman_image_t *pix, struct fcft_font *font, int x, int y,
        unsigned int w, unsigned int h, const char *text, uint32_t fg);
/** width of text in font in pixels, kerning included; 0 without a font */
unsigned int awl_draw_text_width(struct fcft_font *font, const char *text);

/** makes bar m's, without font or widgets (bar is m's drw, unused) */
void awl_draw_setup(awl_draw_t *bar, Monitor *m);
/** Makes the registry's font (awl_font_get(), or awl_font_get_exact() if
 * !fallback) bar's, giving back its old one. Returns it; NULL (bar keeps
 * its font) if none loads. */
struct fcft_font *awl_draw_set_font(awl_draw_t *bar, const char *name, float size, float scale,
        int fallback);
/** makes bar->pix an image of bits, see awl_draw_image() */
void awl_draw_prepare_drawing(awl_draw_t *bar, unsigned int w, unsigned int h, uint32_t *bits, int stride);
/** lets go of what `awl_draw_prepare_drawing()` pointed at */
void awl_draw_finish_drawing(awl_draw_t *bar);
/** gives back bar's font and image, frees its widgets and empties it: its
 * monitor has no bar anymore (bar->m is NULL) */
void awl_draw_teardown(awl_draw_t *bar);
/** Frees every widget's userdata and empties the widget lists, so nothing in
 * the bar points into ``libawlplugins.so`` anymore. */
void awl_draw_widgets_clear(awl_draw_t *bar);
/** fcft's teardown */
void awl_draw_fini(void);
