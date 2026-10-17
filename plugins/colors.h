#pragma once

#include <stdint.h>
#include <pixman-1/pixman.h>

/** colors, 0xRRGGBBAA */
static const uint32_t molokai_blue = 0x66d9efff;
static const uint32_t molokai_red = 0xf92672ff;
static const uint32_t molokai_green = 0xa6e22eff;
static const uint32_t molokai_orange = 0xfd971fff;
static const uint32_t molokai_purple = 0xae81ffff;
static const uint32_t molokai_gray = 0x232526ff;
static const uint32_t molokai_dark_gray = 0x1b1d1eff;
static const uint32_t molokai_light_gray = 0x455354ff;

static const pixman_color_t white = {.red = 0xFFFF, .green = 0xFFFF, .blue = 0xFFFF, .alpha = 0xFFFF};

/** pixman's color as 0xRRGGBBAA */
static inline uint32_t color_16bit_to_8bit( pixman_color_t c ) {
    return (uint32_t)(c.red >> 8) << 24 |
           (c.green >> 8) << 16 |
           (c.blue >> 8) << 8 |
           (c.alpha >> 8) << 0;
}

/** 0xRRGGBBAA as pixman's color */
static inline pixman_color_t color_8bit_to_16bit( uint32_t c ) {
    uint8_t red = (c & 0xFF000000) >> 24,
            green = (c & 0x00FF0000) >> 16,
            blue = (c & 0x0000FF00) >> 8,
            alpha = c & 0x000000FF;
    pixman_color_t r = { .red = ((uint16_t)red) << 8 | red,
                         .green = ((uint16_t)green) << 8 | green,
                         .blue = ((uint16_t)blue) << 8 | blue,
                         .alpha = ((uint16_t)alpha) << 8 | alpha, };
    return r;
}

/** A over B */
pixman_color_t alpha_blend_16( pixman_color_t B, pixman_color_t A );

/** The bar's colors, as ``awl_colors()`` makes them from the palette above. */
struct awl_colors {
    pixman_color_t bg_tags, bg_tags_occ, bg_tags_act, bg_tags_urg, fg_tags,
                   /* bg_lay, fg_lay are also bg/fg for all standard widgets */
                   bg_lay, fg_lay,
                   bg_status, fg_status,
                   bg_win, bg_win_min, bg_win_act, bg_win_urg, fg_win,
                   bg_stats, fg_stats_cpu, fg_stats_io, fg_stats_mem, fg_stats_swp;
};
struct awl_colors awl_colors( void );

