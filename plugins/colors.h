#pragma once

#include <stddef.h>
#include <stdint.h>
#include <pixman-1/pixman.h>
#include "../awl_draw.h" /* color_8bit_to_16bit() & co. */

static const pixman_color_t white = {.red = 0xFFFF, .green = 0xFFFF, .blue = 0xFFFF, .alpha = 0xFFFF};

/** a stop of :c:type:`awl_colormap_t`: color at the value at */
typedef struct awl_colorstop_t {
    float at;
    uint32_t color;
} awl_colorstop_t;

#define AWL_COLORMAP_MAX 16

/** A color map over 0 to 1: v[0] to v[n - 1] by ascending at, linearly in each channel
 * (alpha too) between two, the first's color below it and the last's above;
 * two stops at the same value make a step. Set with :c:macro:`COLORMAP() <COLORMAP>`. */
typedef struct awl_colormap_t {
    awl_colorstop_t v[AWL_COLORMAP_MAX];
    size_t n;
} awl_colormap_t;

/** the color map of the stops {at, color}, ... (at most AWL_COLORMAP_MAX),
 * as an initializer; (:c:type:`awl_colormap_t`)COLORMAP(...) to assign one */
#define COLORMAP( ... ) { .v = { __VA_ARGS__ }, \
    .n = sizeof( (awl_colorstop_t[]){ __VA_ARGS__ } ) / sizeof( awl_colorstop_t ) }

/** m's color (0xRRGGBBAA) at x, 0 to 1; 0 if m has no stops */
uint32_t colormap_color( const awl_colormap_t* m, float x );

/** A over B */
pixman_color_t alpha_blend_16( pixman_color_t B, pixman_color_t A );
/** A at t = 0 to B at t = 1, linearly in each channel; t is clipped to 0..1 */
pixman_color_t color_mix_16( pixman_color_t A, pixman_color_t B, float t );

