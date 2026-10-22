#pragma once

#include <stdint.h>
#include <pixman-1/pixman.h>
#include "../awl_draw.h" /* color_8bit_to_16bit() & co. */

static const pixman_color_t white = {.red = 0xFFFF, .green = 0xFFFF, .blue = 0xFFFF, .alpha = 0xFFFF};

/** A over B */
pixman_color_t alpha_blend_16( pixman_color_t B, pixman_color_t A );
/** A at t = 0 to B at t = 1, linearly in each channel; t is clipped to 0..1 */
pixman_color_t color_mix_16( pixman_color_t A, pixman_color_t B, float t );

