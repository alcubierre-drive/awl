/* PlutoSVG's and plutovg's implementation (see svg.h), as one file. Its code
 * isn't ours: neither are warnings in it. It parses untrusted SVGs, so it is
 * compiled without -ffast-math (whose assumption that no number is NaN or
 * infinite such input breaks). Embedded images are only PNGs and JPEGs
 * (which is all PlutoSVG takes), as large as icons/icon.h allows; text
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

/* awl: an SVG's embedded image (a data: URL), decoded by stb_image, is
 * logged as icons/icon.c logs its decodes: plutosvg.c's one call of this
 * goes through here */
#include "../../awl_log.h"
static plutovg_surface_t* awl_svg_image( const char* data, int length )
{
    plutovg_surface_t* image = plutovg_surface_load_from_image_base64( data, length );
    /* base64 of PNG's signature */
    const char* kind = length >= 11 && !memcmp( data, "iVBORw0KGgo", 11 ) ? "PNG" : "JPEG";
    if (image)
        awl_vrb_printf( "icon: decoded SVG's embedded %s (data: URL, %d base64 bytes): %dx%d\n", kind, length,
                        plutovg_surface_get_width( image ), plutovg_surface_get_height( image ) );
    else
        awl_vrb_printf( "icon: SVG's embedded %s (data: URL, %d base64 bytes): can't decode it\n", kind, length );
    return image;
}
#define plutovg_surface_load_from_image_base64 awl_svg_image

#include "plutosvg-0.0.8/source/plutosvg.c"
