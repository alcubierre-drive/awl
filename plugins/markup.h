#pragma once

/**
 * Notification markup: a body's text with ``<b>``, ``<i>``, ``<u>`` and
 * ``<t color="…">`` (awl's own) parsed into runs of one style and color, and
 * those runs wrapped into lines in their styles' fonts.
 *
 * Parsing (:c:func:`awl_markup_parse`) never fails, since clients that don't
 * escape their text are common:
 *
 * - Tags: ``<b>``, ``<i>``, ``<u>``; ``<t color>`` and Pango's
 *   ``<span foreground|fgcolor|color>``, the color as ``#rgb``, ``#rrggbb`` or
 *   ``#rrggbbaa`` (an invalid one keeps the outer color). They nest; a close
 *   tag closes everything up to its open tag, one without an open tag is
 *   dropped, unclosed ones end with the text. ``<a href>`` keeps its text,
 *   ``<img alt>`` becomes its alt text, ``<br>``, ``<br/>`` a line break.
 *   Pango's ``<s> <tt> <sub> <sup> <small> <big>`` are dropped, their text
 *   kept. Names are case-insensitive.
 * - Anything else that starts with ``<`` is text: an unknown name (``a<b2>``,
 *   ``x <y and z> w``), a malformed tag, a lone ``<``.
 * - Entities: ``&amp; &lt; &gt; &quot; &apos; &nbsp;``, ``&#N;``, ``&#xN;``;
 *   an unknown or invalid one stays as it is.
 * - Tabs become spaces; carriage returns and other control characters but
 *   newlines are dropped.
 *
 * Layout (:c:func:`awl_markup_layout`) wraps the runs into lines of at most max_w
 * pixels: at spaces (dropped at a wrap) and after '-'; a word may span runs
 * (``a<b>b</b>c`` is one word); a word longer than a line breaks between
 * characters; '\n' always breaks. After max_lines lines the last one ends
 * in an ellipsis. Runs on a line share a baseline.
 */

#include <stddef.h>
#include <stdint.h>

struct fcft_font;

/** a run's style, a mask; bold and italic are :c:func:`awl_font_style`'s
 * (awl_draw.h), so style & AWL_MARKUP_FONT indexes the layout's fonts */
enum {
    AWL_MARKUP_BOLD = 1,
    AWL_MARKUP_ITALIC = 2,
    AWL_MARKUP_UNDERLINE = 4,
    /** the run's color is set (else the caller's) */
    AWL_MARKUP_COLOR = 8,
    AWL_MARKUP_FONT = AWL_MARKUP_BOLD | AWL_MARKUP_ITALIC,
};

enum {
    /** bytes of markup parsed at most; the rest is left out */
    AWL_MARKUP_MAX = 16384,
    /** tags open at once at most; a further one is dropped */
    AWL_MARKUP_DEPTH = 16,
};

/** a stretch of the text in one style and color */
typedef struct awl_markup_run_t {
    /** the run's text: text[off .. off + len) */
    unsigned int off, len;
    int style;
    /** 0xRRGGBBAA, if style has :c:enumerator:`AWL_MARKUP_COLOR` */
    uint32_t color;
} awl_markup_run_t;

/** parsed markup: the plain text and its runs (:c:func:`awl_markup_parse`) */
typedef struct awl_markup_t {
    /** the text without markup, entities decoded; NUL-terminated */
    char* text;
    unsigned int len;
    /** in order, none empty; neighbours differ in style or color */
    awl_markup_run_t* runs;
    unsigned int nruns;
} awl_markup_t;

/** Parses src (UTF-8, up to AWL_MARKUP_MAX bytes) into m; NULL is the empty
 * text. markup 0 takes src as plain text (a summary): one run, only tabs
 * and control characters handled. Returns 0 only when out of memory (then
 * m is empty). Free m with :c:func:`awl_markup_free`. */
int awl_markup_parse( awl_markup_t* m, const char* src, int markup );
/** frees what m holds (not m) and empties it */
void awl_markup_free( awl_markup_t* m );

/** "#rgb", "#rrggbb", "#rrggbbaa" (case-insensitive) as 0xRRGGBBAA into
 * ``*color``; 0 if s is none of them */
int awl_markup_color( const char* s, size_t len, uint32_t* color );

/** a piece of a line: one run's text on it */
typedef struct awl_markup_piece_t {
    /** NUL-terminated, in the layout's buffer */
    const char* text;
    /** from the line's left edge */
    int x, w;
    /** the run's */
    int style;
    uint32_t color;
} awl_markup_piece_t;

typedef struct awl_markup_line_t {
    /** the line's top from the layout's, its height (its highest font's)
     * and its baseline from its top (its fonts' highest ascent) */
    int y, h, baseline;
    int w;
    /** its pieces: pieces[first .. first + n) */
    unsigned int first, n;
} awl_markup_line_t;

/** parsed markup wrapped into lines (:c:func:`awl_markup_layout`) */
typedef struct awl_markup_layout_t {
    awl_markup_piece_t* pieces;
    unsigned int npieces;
    awl_markup_line_t* lines;
    unsigned int nlines;
    /** the widest line's width, all lines' height (with spacing) */
    int w, h;
    /** max_lines cut it */
    int cut;
    /** the pieces' text */
    char* buf;
} awl_markup_layout_t;

/** a font's text width in pixels, as :c:func:`awl_draw_text_width` */
typedef unsigned int ( *awl_markup_width_t )( struct fcft_font* font, const char* text );

/** Lays m out into L (zeroed first, or a layout to redo: it is freed
 * first), lines at most max_w wide, spacing pixels apart, at most max_lines
 * (<= 0: any). fonts[style & AWL_MARKUP_FONT] draws a run; a NULL one is
 * fonts[0]'s, which must be set. Returns 0 only when out of memory. */
int awl_markup_layout( awl_markup_layout_t* L, const awl_markup_t* m, struct fcft_font* const fonts[4],
                       awl_markup_width_t width, int max_w, int max_lines, int spacing );
void awl_markup_layout_free( awl_markup_layout_t* L );

/** a piece's font: fonts[style & AWL_MARKUP_FONT], else fonts[0] */
struct fcft_font* awl_markup_font( struct fcft_font* const fonts[4], int style );

/** the ellipsis a cut layout ends in */
#define AWL_MARKUP_ELLIPSIS "…"
