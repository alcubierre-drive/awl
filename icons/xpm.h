#pragma once

/* XPM3 images (X11's pixmaps, a C array of strings) without libXpm, for
 * old applications' icons: their pixels as :c:func:`awl_icon_decode` takes
 * them, which decides by :c:func:`awl_xpm_is`. icons/xpm.c. */

#include "icon.h"

#include <stddef.h>
#include <stdint.h>

/** data begins (after white space) with the comment ``XPM`` (slash, star, XPM, star, slash) */
int awl_xpm_is( const uint8_t* data, size_t len );

/** data's pixels at their own size as PIXMAN_a8r8g8b8, each opaque or
 * transparent (so premultiplied as well), within lim's dim and pixels;
 * NULL if it is broken or too large, ``*err`` then says why */
pixman_image_t* awl_xpm_decode( const uint8_t* data, size_t len, const awl_icon_limits_t* lim, const char** err );
