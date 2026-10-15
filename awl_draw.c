#include "awl_draw.h"
#include "plugins/colors.h"

int awl_draw_init(void) {
    return fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
}

awl_draw_t * awl_draw_create(Monitor* m) {
    awl_draw_t *drw;

    if (!(drw = calloc(1, sizeof(awl_draw_t))))
        return NULL;
    drw->m = m;
    /* the widgets come from libawlplugins.so, see awl_plugin_host.h */
    return drw;
}

static void awl_draw_setfont(awl_draw_t *drw, struct fcft_font *font) {
    if (drw)
        drw->font = font;
}

struct fcft_font * awl_draw_load_font(awl_draw_t *drw, size_t fontcount, const char *fonts[static fontcount], const char *attributes) {
    struct fcft_font *font = fcft_from_name(fontcount, fonts, attributes);
    if (drw)
        awl_draw_setfont(drw, font);
    return font;
}

void awl_draw_destroy_font(struct fcft_font *font) {
    fcft_destroy(font);
}

void awl_draw_prepare_drawing(awl_draw_t *drw, unsigned int w, unsigned int h, uint32_t *bits, int stride) {
    pixman_region32_t clip;

    if (!drw)
        return;

    drw->pix = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, w, h, bits, stride);
    pixman_region32_init_rect(&clip, 0, 0, w, h);
    pixman_image_set_clip_region32(drw->pix, &clip);
    pixman_region32_fini(&clip);
}

static void awl_draw_rect_color(awl_draw_t *drw, int x, int y, unsigned int w, unsigned int h, uint32_t color) {
    pixman_color_t clr;
    if (!drw || !drw->pix)
        return;

    /* the bar buffer is premultiplied ARGB, color is not */
    clr = convert_color(color);
    clr.red = (uint32_t)clr.red * clr.alpha / 0xffff;
    clr.green = (uint32_t)clr.green * clr.alpha / 0xffff;
    clr.blue = (uint32_t)clr.blue * clr.alpha / 0xffff;
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, drw->pix, &clr, 1,
        &(pixman_rectangle16_t){x, y, w, h});
}

/* width of `text` in pixels, kerning included */
static unsigned int text_width(awl_draw_t *drw, const char *text) {
    uint32_t cp = 0, last_cp = 0;
    long x = 0, x_kern;
    const struct fcft_glyph *glyph;

    while (*text) {
        text += utf8decode(text, &cp);
        if (!(glyph = fcft_rasterize_char_utf32(drw->font, cp, FCFT_SUBPIXEL_DEFAULT)))
            continue;
        x_kern = 0;
        if (last_cp)
            fcft_kerning(drw->font, last_cp, cp, &x_kern, NULL);
        last_cp = cp;
        x += x_kern + glyph->advance.x;
    }
    return x > 0 ? (unsigned int)x : 0;
}

/* fill: the box gets bg first, else the text goes over what is there */
static int awl_draw_text_color(awl_draw_t *drw, int x, int y, unsigned int w, unsigned int h,
        unsigned int lpad, const char *text, uint32_t fg, uint32_t bg, int fill) {
    int ty;
    int utf8charlen, render = x || y || w || h;
    long x_kern;
    uint32_t cp = 0, last_cp = 0;
    pixman_color_t clr;
    pixman_image_t *fg_pix = NULL;
    int noellipsis = 0;
    const struct fcft_glyph *glyph, *eg;
    int fcft_subpixel_mode = FCFT_SUBPIXEL_DEFAULT;

    if (!drw || (render && (!w || !drw->pix)) || !text || !drw->font)
        return 0;

    if (!render) {
        // w = ...;
    } else {
        clr = convert_color(fg);
        fg_pix = pixman_image_create_solid_fill(&clr);

        if (fill)
            awl_draw_rect_color(drw, x, y, w, y<0?h-y:h, bg);

        x += lpad;
        w -= lpad;
    }

    if (render && (!fill || (bg & 0xFF) != 0xFF))
        fcft_subpixel_mode = FCFT_SUBPIXEL_NONE;

    // U+2026 == …
    eg = fcft_rasterize_char_utf32(drw->font, 0x2026, fcft_subpixel_mode);

    while (*text) {
        utf8charlen = utf8decode(text, &cp);

        glyph = fcft_rasterize_char_utf32(drw->font, cp, fcft_subpixel_mode);
        if (!glyph) {
            text += utf8charlen;
            continue;
        }

        x_kern = 0;
        if (last_cp)
            fcft_kerning(drw->font, last_cp, cp, &x_kern, NULL);
        last_cp = cp;

        ty = y + (h - drw->font->height) / 2 + drw->font->ascent;

        /* draw ellipsis if remaining text doesn't fit */
        if (!noellipsis && x_kern + glyph->advance.x + eg->advance.x > w && *(text + 1) != '\0') {
            if ((int)text_width(drw, text) - glyph->advance.x < eg->advance.x) {
                noellipsis = 1;
            } else {
                w -= eg->advance.x;
                pixman_image_composite32(
                    PIXMAN_OP_OVER, fg_pix, eg->pix, drw->pix, 0, 0, 0, 0,
                    x + eg->x, ty - eg->y, eg->width, eg->height);
            }
        }

        if ((x_kern + glyph->advance.x) > w)
            break;

        x += x_kern;

        if (render && pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8)
            // pre-rendered glyphs (eg. emoji)
            pixman_image_composite32(
                PIXMAN_OP_OVER, glyph->pix, NULL, drw->pix, 0, 0, 0, 0,
                x + glyph->x, ty - glyph->y, glyph->width, glyph->height);
        else if (render)
            pixman_image_composite32(
                PIXMAN_OP_OVER, fg_pix, glyph->pix, drw->pix, 0, 0, 0, 0,
                x + glyph->x, ty - glyph->y, glyph->width, glyph->height);

        text += utf8charlen;
        x += glyph->advance.x;
        w -= glyph->advance.x;
    }

    if (render)
        pixman_image_unref(fg_pix);

    return x + (render ? w : 0);
}

int awl_draw_text_color2(awl_draw_t *drw, int x, int y, unsigned int w, unsigned int h,
        unsigned int lpad, const char *text, pixman_color_t fg, pixman_color_t bg) {
    return awl_draw_text_color(drw, x, y, w, h, lpad, text, color_16bit_to_8bit(fg), color_16bit_to_8bit(bg), 1);
}

int awl_draw_text_over(awl_draw_t *drw, int x, int y, unsigned int w, unsigned int h,
        const char *text, uint32_t fg) {
    return awl_draw_text_color(drw, x, y, w, h, 0, text, fg, 0, 0);
}

unsigned int awl_draw_font_getwidth(awl_draw_t *drw, const char *text) {
    if (!drw || !drw->font || !text)
        return 0;
    return text_width(drw, text);
}

void awl_draw_finish_drawing(awl_draw_t *drw) {
    if (drw && drw->pix) {
        pixman_image_unref(drw->pix);
        /* awl_draw_destroy() also checks drw->pix before unreffing it again on
         * monitor teardown; leaving this dangling causes a double-unref /
         * use-after-free on the pixman image (heap corruption that doesn't
         * crash until some later, unrelated allocation reuses the memory). */
        drw->pix = NULL;
    }
}

void awl_draw_destroy(awl_draw_t *drw) {
    if (drw->pix)
        pixman_image_unref(drw->pix);
    if (drw->font)
        awl_draw_destroy_font(drw->font);

    awl_draw_widgets_clear(drw);
    free(drw);
}

void awl_draw_widgets_clear(awl_draw_t *drw) {
    for (int l=0; l<drw->n_widgets_left; ++l)
        if (drw->widgets_left[l].free)
            drw->widgets_left[l].free( drw->widgets_left[l].userdata );
    for (int r=0; r<drw->n_widgets_right; ++r)
        if (drw->widgets_right[r].free)
            drw->widgets_right[r].free( drw->widgets_right[r].userdata );

    if (drw->has_center_widget)
        if (drw->center_widget.free)
            drw->center_widget.free( drw->center_widget.userdata );

    drw->n_widgets_left = drw->n_widgets_right = 0;
    drw->has_center_widget = 0;
    drw->center_widget = (widget_t){0};
}

void awl_draw_fini(void) {
    fcft_fini();
}

