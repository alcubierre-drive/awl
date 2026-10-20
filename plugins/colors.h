#pragma once

#include <stdint.h>
#include <pixman-1/pixman.h>
#include "../awl_draw.h" /* color_8bit_to_16bit() & co. */

/** colors, 0xRRGGBBAA */
#define molokai_blue 0x66d9efffu
#define molokai_red 0xf92672ffu
#define molokai_green 0xa6e22effu
#define molokai_orange 0xfd971fffu
#define molokai_purple 0xae81ffffu
#define molokai_gray 0x232526ffu
#define molokai_dark_gray 0x1b1d1effu
#define molokai_light_gray 0x455354ffu

static const pixman_color_t white = {.red = 0xFFFF, .green = 0xFFFF, .blue = 0xFFFF, .alpha = 0xFFFF};

/** A over B */
pixman_color_t alpha_blend_16( pixman_color_t B, pixman_color_t A );
/** A at t = 0 to B at t = 1, linearly in each channel; t is clipped to 0..1 */
pixman_color_t color_mix_16( pixman_color_t A, pixman_color_t B, float t );

