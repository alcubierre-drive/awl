#pragma once

#include <stdint.h>
#include <pixman-1/pixman.h>
#include "../awl_draw.h" /* color_8bit_to_16bit() & co. */

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

/** A over B */
pixman_color_t alpha_blend_16( pixman_color_t B, pixman_color_t A );

