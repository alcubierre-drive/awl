/* PlutoSVG's and plutovg's implementation (see svg.h), as one file. Its code
 * isn't ours: neither are warnings in it. It parses untrusted SVGs, so it is
 * compiled without -ffast-math (whose assumption that no number is NaN or
 * infinite such input breaks). Embedded images are only PNGs and JPEGs
 * (which is all PlutoSVG takes), as large as plugins/icon.h allows; text
 * isn't drawn, so the font code never looks for fonts. */

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS 1024
#define PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"

#include "svg.h"
#include "plutovg-1.3.3/source/plutovg-blend.c"
#include "plutovg-1.3.3/source/plutovg-canvas.c"
#include "plutovg-1.3.3/source/plutovg-font.c"
#include "plutovg-1.3.3/source/plutovg-ft-math.c"
#include "plutovg-1.3.3/source/plutovg-ft-raster.c"
#include "plutovg-1.3.3/source/plutovg-ft-stroker.c"
#include "plutovg-1.3.3/source/plutovg-matrix.c"
#include "plutovg-1.3.3/source/plutovg-paint.c"
#include "plutovg-1.3.3/source/plutovg-path.c"
#include "plutovg-1.3.3/source/plutovg-rasterize.c"
#include "plutovg-1.3.3/source/plutovg-surface.c"
/* plutovg-font.c's, which plutosvg.c defines again */
#undef MAX_NAME
#include "plutosvg-0.0.8/source/plutosvg.c"
