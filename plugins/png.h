#pragma once

/**
 * A PNG encoder for the screenshots (plugins/screenshot.h): 8 bits per
 * channel, RGB when every pixel is opaque, RGBA otherwise. Each row is
 * filtered by what it looks like: a row where most pixels repeat the one on
 * their left (screen content: text, flat colors) stays as it is, which
 * deflates best; any other (a photo, a gradient) gets the Paeth filter. The
 * rows are deflated by sdefl (vendor/sdefl/sdefl.h). Any thread; no state.
 */

#include <stddef.h>
#include <stdint.h>

/** sdefl's levels, 0 to 8: above 5 they get much slower for little */
#define AWL_PNG_LEVEL_DEFAULT 5

/**
 * The PNG of the w x h pixels at px, rows stride bytes apart, each a
 * premultiplied 0xAARRGGBB in native order (pixman's a8r8g8b8; opaque
 * x8r8g8b8 is fine if its top byte is 0xff). level is sdefl's, 0 to 8.
 * Returns the file's bytes, which the caller frees, and their length in len;
 * NULL if out of memory, w or h isn't positive, or the image is too large
 * (its rows over 1 GiB).
 */
unsigned char* awl_png_encode( const uint32_t* px, int w, int h, int stride, int level, size_t* len );
