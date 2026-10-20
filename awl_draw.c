#include "awl_draw.h"

#include "awl_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The font registry: every font awl draws with comes from here, one fcft
 * instance per spec (name with size) and dpi, shared and counted. A static
 * ring of AWL_FONT_SLOTS entries, oldest at head: a new spec takes the next
 * free slot, and with none left evicts from the beginning, skipping (and
 * moving to the end) what is still in use. A font given back stays loaded
 * until evicted, so asking again costs nothing. A spec that doesn't load
 * stays as a failed entry (logged once) until awl_font_retry(). */
typedef struct {
    char spec[AWL_FONT_SPEC_MAX];
    char attrs[24];
    /** NULL: it didn't load (failed), or awl_font_retry() asked to try again */
    struct fcft_font *font;
    int failed;
    int refs;
} font_entry_t;

static font_entry_t ring[AWL_FONT_SLOTS];
static unsigned head, count;
static int fcft_ready;

/** the i-th oldest entry */
#define SLOT(i) (&ring[(head + (i)) % AWL_FONT_SLOTS])

int awl_draw_init(void) {
    if (!fcft_ready)
        fcft_ready = fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
    return fcft_ready;
}

/** name with size appended (after its "...size=", else as ":size=") into
 * spec; 0 if it doesn't fit */
static int font_spec(char spec[static AWL_FONT_SPEC_MAX], const char *name, float size) {
    size_t n = strlen(name);
    const char *sep = n && name[n - 1] == '=' ? "" : ":size=";
    int len = snprintf(spec, AWL_FONT_SPEC_MAX, "%s%s%.0f", name, sep, size);
    return len > 0 && len < AWL_FONT_SPEC_MAX;
}

static void entry_clear(font_entry_t *e) {
    if (e->font)
        fcft_destroy(e->font);
    *e = (font_entry_t){0};
}

/** a slot for a new entry, cleared: a free one, else the oldest one not in
 * use (those in use before it move to the end); NULL if all are in use */
static font_entry_t *slot_take(void) {
    static int warned;
    font_entry_t *e;

    if (count < AWL_FONT_SLOTS)
        return SLOT(count++);
    for (unsigned i = 0; i < AWL_FONT_SLOTS; i++) {
        e = SLOT(0);
        /* the head becomes the tail either way: freed and reused, or kept */
        head = (head + 1) % AWL_FONT_SLOTS;
        if (e->refs > 0)
            continue;
        entry_clear(e);
        return e;
    }
    if (!warned++)
        awl_err_printf("all %d fonts in use, loading no other\n", AWL_FONT_SLOTS);
    return NULL;
}

/** spec at dpi from the registry, loading it if new or to be retried; NULL
 * if it doesn't load (now or before). Takes a reference on success. */
static struct fcft_font *font_lookup(const char *name, float size, float scale) {
    char spec[AWL_FONT_SPEC_MAX], attrs[24];
    font_entry_t *e = NULL;
    const char *names[1];

    if (!name || !*name)
        return NULL;
    if (!font_spec(spec, name, size * scale)) {
        awl_wrn_printf("font name %.40s... is too long\n", name);
        return NULL;
    }
    snprintf(attrs, sizeof(attrs), "dpi=%.2f", 96. * 2. * scale);
    for (unsigned i = 0; i < count && !e; i++)
        if (!strcmp(SLOT(i)->spec, spec) && !strcmp(SLOT(i)->attrs, attrs))
            e = SLOT(i);
    if (!e) {
        if (!(e = slot_take()))
            return NULL;
        memcpy(e->spec, spec, sizeof(spec));
        memcpy(e->attrs, attrs, sizeof(attrs));
    }
    if (!e->font && !e->failed) {
        names[0] = e->spec;
        if (awl_draw_init())
            e->font = fcft_from_name(1, names, e->attrs);
        if ((e->failed = !e->font))
            awl_wrn_printf("font %s (%s) didn't load\n", e->spec, e->attrs);
    }
    if (!e->font)
        return NULL;
    e->refs++;
    return e->font;
}

/** a size or scale fcft can take: finite, positive, not absurd */
static float font_sane(float v, float dflt, float max) {
    return isfinite(v) && v > 0 ? (v > max ? max : v) : dflt;
}

struct fcft_font *awl_font_get_exact(const char *name, float size, float scale) {
    return font_lookup(name, font_sane(size, AWL_FONT_FALLBACK_SIZE, 200),
            font_sane(scale, 1, 16));
}

struct fcft_font *awl_font_get(const char *name, float size, float scale) {
    static int warned;
    struct fcft_font *font;

    size = font_sane(size, AWL_FONT_FALLBACK_SIZE, 200);
    scale = font_sane(scale, 1, 16);
    if ((font = font_lookup(name, size, scale)))
        return font;
    if ((font = font_lookup(AWL_FONT_FALLBACK, size, scale))
            || (font = font_lookup(AWL_FONT_FALLBACK, AWL_FONT_FALLBACK_SIZE, scale))
            || (font = font_lookup(AWL_FONT_FALLBACK, AWL_FONT_FALLBACK_SIZE, 1))) {
        awl_wrn_printf("font %s: using " AWL_FONT_FALLBACK "\n", name ? name : "(none)");
        return font;
    }
    if (!warned++)
        awl_err_printf("no font loads, not even " AWL_FONT_FALLBACK "%d: drawing no text\n",
                AWL_FONT_FALLBACK_SIZE);
    return NULL;
}

void awl_font_release(struct fcft_font *font) {
    if (!font)
        return;
    for (unsigned i = 0; i < count; i++) {
        font_entry_t *e = SLOT(i);
        if (e->font != font)
            continue;
        if (e->refs > 0)
            e->refs--;
        else
            awl_wrn_printf("font %s released once too often\n", e->spec);
        return;
    }
    awl_wrn_printf("font %p is not awl's, not releasing it\n", (void *)font);
}

void awl_font_retry(void) {
    for (unsigned i = 0; i < count; i++)
        SLOT(i)->failed = 0;
}

void awl_draw_setup(awl_draw_t *bar, Monitor *m) {
    *bar = (awl_draw_t){ .m = m };
    /* the widgets come from libawlplugins.so, see awl_plugin_host.h */
}

struct fcft_font *awl_draw_set_font(awl_draw_t *bar, const char *name, float size, float scale,
        int fallback) {
    struct fcft_font *font = fallback ? awl_font_get(name, size, scale)
                                      : awl_font_get_exact(name, size, scale);
    if (!bar || !font) {
        awl_font_release(font);
        return NULL;
    }
    /* the new one first: if it is the same, the old reference keeps it */
    awl_font_release(bar->font);
    bar->font = font;
    return font;
}

pixman_image_t *awl_draw_image(unsigned int w, unsigned int h, uint32_t *bits, int stride) {
    pixman_region32_t clip;
    pixman_image_t *pix = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, w, h, bits, stride);

    if (!pix)
        return NULL;
    pixman_region32_init_rect(&clip, 0, 0, w, h);
    pixman_image_set_clip_region32(pix, &clip);
    pixman_region32_fini(&clip);
    return pix;
}

void awl_draw_prepare_drawing(awl_draw_t *bar, unsigned int w, unsigned int h, uint32_t *bits, int stride) {
    if (!bar)
        return;
    awl_draw_finish_drawing(bar);
    bar->pix = awl_draw_image(w, h, bits, stride);
}

static void rect_color(pixman_image_t *pix, int x, int y, unsigned int w, unsigned int h, uint32_t color) {
    pixman_color_t clr;

    /* the image is premultiplied ARGB, color is not */
    clr = color_8bit_to_16bit(color);
    clr.red = (uint32_t)clr.red * clr.alpha / 0xffff;
    clr.green = (uint32_t)clr.green * clr.alpha / 0xffff;
    clr.blue = (uint32_t)clr.blue * clr.alpha / 0xffff;
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &clr, 1,
        &(pixman_rectangle16_t){x, y, w, h});
}

/* width of `text` in pixels, kerning included */
static unsigned int text_width(struct fcft_font *font, const char *text) {
    uint32_t cp = 0, last_cp = 0;
    long x = 0, x_kern;
    const struct fcft_glyph *glyph;

    while (*text) {
        text += utf8decode(text, &cp);
        if (!(glyph = fcft_rasterize_char_utf32(font, cp, FCFT_SUBPIXEL_DEFAULT)))
            continue;
        x_kern = 0;
        if (last_cp)
            fcft_kerning(font, last_cp, cp, &x_kern, NULL);
        last_cp = cp;
        x += x_kern + glyph->advance.x;
    }
    return x > 0 ? (unsigned int)x : 0;
}

/* fill: the box gets bg first, else the text goes over what is there */
static int text_color(pixman_image_t *pix, struct fcft_font *font, int x, int y,
        unsigned int w, unsigned int h, unsigned int lpad, const char *text,
        uint32_t fg, uint32_t bg, int fill) {
    int ty;
    int utf8charlen;
    long x_kern;
    uint32_t cp = 0, last_cp = 0;
    pixman_color_t clr;
    pixman_image_t *fg_pix;
    int noellipsis = 0;
    const struct fcft_glyph *glyph, *eg;
    int fcft_subpixel_mode = FCFT_SUBPIXEL_DEFAULT;

    if (!pix || !w || !text || !font)
        return 0;

    /* pixman's colors are premultiplied, fg is not (a translucent fg
     * would come out too bright) */
    clr = color_8bit_to_16bit(fg);
    clr.red = (uint32_t)clr.red * clr.alpha / 0xffff;
    clr.green = (uint32_t)clr.green * clr.alpha / 0xffff;
    clr.blue = (uint32_t)clr.blue * clr.alpha / 0xffff;
    fg_pix = pixman_image_create_solid_fill(&clr);

    if (fill)
        rect_color(pix, x, y, w, y<0?h-y:h, bg);

    x += lpad;
    w -= lpad;

    if (!fill || (bg & 0xFF) != 0xFF)
        fcft_subpixel_mode = FCFT_SUBPIXEL_NONE;

    // U+2026 == …
    eg = fcft_rasterize_char_utf32(font, 0x2026, fcft_subpixel_mode);

    while (*text) {
        utf8charlen = utf8decode(text, &cp);

        glyph = fcft_rasterize_char_utf32(font, cp, fcft_subpixel_mode);
        if (!glyph) {
            text += utf8charlen;
            continue;
        }

        x_kern = 0;
        if (last_cp)
            fcft_kerning(font, last_cp, cp, &x_kern, NULL);
        last_cp = cp;

        ty = y + (h - font->height) / 2 + font->ascent;

        /* draw ellipsis if remaining text doesn't fit */
        if (!noellipsis && x_kern + glyph->advance.x + eg->advance.x > w && text[utf8charlen] != '\0') {
            if ((int)text_width(font, text) - glyph->advance.x < eg->advance.x) {
                noellipsis = 1;
            } else {
                w -= eg->advance.x;
                pixman_image_composite32(
                    PIXMAN_OP_OVER, fg_pix, eg->pix, pix, 0, 0, 0, 0,
                    x + eg->x, ty - eg->y, eg->width, eg->height);
            }
        }

        if ((x_kern + glyph->advance.x) > w)
            break;

        x += x_kern;

        if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8)
            // pre-rendered glyphs (eg. emoji)
            pixman_image_composite32(
                PIXMAN_OP_OVER, glyph->pix, NULL, pix, 0, 0, 0, 0,
                x + glyph->x, ty - glyph->y, glyph->width, glyph->height);
        else
            pixman_image_composite32(
                PIXMAN_OP_OVER, fg_pix, glyph->pix, pix, 0, 0, 0, 0,
                x + glyph->x, ty - glyph->y, glyph->width, glyph->height);

        text += utf8charlen;
        x += glyph->advance.x;
        w -= glyph->advance.x;
    }

    pixman_image_unref(fg_pix);

    return x + w;
}

int awl_draw_text(pixman_image_t *pix, struct fcft_font *font, int x, int y,
        unsigned int w, unsigned int h, unsigned int lpad, const char *text,
        pixman_color_t fg, pixman_color_t bg) {
    return text_color(pix, font, x, y, w, h, lpad, text,
            color_16bit_to_8bit(fg), color_16bit_to_8bit(bg), 1);
}

int awl_draw_text_over(pixman_image_t *pix, struct fcft_font *font, int x, int y,
        unsigned int w, unsigned int h, const char *text, uint32_t fg) {
    return text_color(pix, font, x, y, w, h, 0, text, fg, 0, 0);
}

unsigned int awl_draw_text_width(struct fcft_font *font, const char *text) {
    if (!font || !text)
        return 0;
    return text_width(font, text);
}

void awl_draw_finish_drawing(awl_draw_t *bar) {
    if (bar && bar->pix) {
        pixman_image_unref(bar->pix);
        /* awl_draw_teardown() and the next awl_draw_prepare_drawing() unref
         * a bar->pix that is left; a dangling one would be a double unref */
        bar->pix = NULL;
    }
}

void awl_draw_teardown(awl_draw_t *bar) {
    if (!bar)
        return;
    awl_draw_finish_drawing(bar);
    awl_font_release(bar->font);
    awl_draw_widgets_clear(bar);
    *bar = (awl_draw_t){ 0 };
}

void awl_draw_widgets_clear(awl_draw_t *bar) {
    for (int l=0; l<bar->n_widgets_left; ++l)
        if (bar->widgets_left[l].free)
            bar->widgets_left[l].free( bar->widgets_left[l].userdata );
    for (int r=0; r<bar->n_widgets_right; ++r)
        if (bar->widgets_right[r].free)
            bar->widgets_right[r].free( bar->widgets_right[r].userdata );

    if (bar->has_center_widget)
        if (bar->center_widget.free)
            bar->center_widget.free( bar->center_widget.userdata );

    bar->n_widgets_left = bar->n_widgets_right = 0;
    bar->has_center_widget = 0;
    bar->center_widget = (widget_t){0};
}

void awl_draw_fini(void) {
    int left = 0;

    for (unsigned i = 0; i < count; i++) {
        left += SLOT(i)->refs;
        entry_clear(SLOT(i));
    }
    head = count = 0;
    if (left)
        awl_wrn_printf("%d font references left at exit\n", left);
    if (fcft_ready)
        fcft_fini();
    fcft_ready = 0;
}
