#pragma once

/* PlutoSVG (https://github.com/sammycage/plutosvg, MIT, plutosvg-0.0.8/) and
 * the rasterizer it draws with, plutovg (https://github.com/sammycage/plutovg,
 * MIT, plutovg-1.3.3/, the version PlutoSVG 0.0.8 pins). Their build files,
 * examples and PlutoSVG's FreeType bridge are left out, and three changes,
 * marked "awl:", make them safe for untrusted SVGs (found by fuzzing):
 *
 * - plutovg-rasterize.c: coordinates, stroke widths and miter limits are
 *   clamped to what FreeType's fixed point holds (NaN to 0), which would
 *   overflow otherwise;
 * - plutovg-path.c: a dash pattern that would cut a path into more than
 *   65536 dashes draws it solid (a 150 bytes SVG could ask for billions,
 *   until memory ran out and it crashed);
 * - plutosvg.c: one render does a bounded amount of work, RENDER_BUDGET
 *   (<use>s of <use>s of 2 KB took twenty minutes).
 *
 * awl.patch is them, for a newer release.
 *
 * plutovg's plutovg-ft-*.[ch] are FreeType's rasterizer
 * and stroker: portions of this software are copyright (c) The FreeType
 * Project (www.freetype.org), all rights reserved; FTL.TXT is its license.
 * svg.c compiles them into libawlplugins.so, which needs
 * plutovg-1.3.3/include on the include path.
 *
 * What it draws is SVG Tiny's static subset: shapes and paths, solid colors
 * and gradients, clip paths, <use>, and styles as attributes or style="".
 * It has no <style> elements and classes (icons/icon.c applies the icon
 * themes' one stylesheet itself), masks, patterns, filters or text. */

#define PLUTOSVG_BUILD_STATIC
#define PLUTOVG_BUILD_STATIC
#include "plutosvg-0.0.8/source/plutosvg.h"
