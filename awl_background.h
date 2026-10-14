#pragma once

/**
 * The wallpaper, shown by awl itself: plugins/wallpaper.c picks and decodes
 * it on its thread, this side puts it on every monitor, scaled to cover it,
 * and fades from the previous one. Without the library, the last one stays.
 * Main thread only.
 *
 * The images live in their own scene tree right below ``LyrBg``, so
 * background layer surfaces still go over them; it's not part of
 * ``layers[]``, so ``xytonode()`` and clicks go right through them.
 */

#include "awl.h"
#include "plugins/wallpaper.h"

/** tree: an empty scene tree, already placed in the stacking order; loop
 * runs the fades */
void background_init( struct wlr_scene_tree* tree, struct wl_event_loop* loop );
/** drops the current image and stops a fade */
void background_fini( void );

/** shows img (taking it) instead of the current one, fading in over fade_ms */
void background_set( awl_image_t* img, unsigned int fade_ms );

/** puts the current image on m */
void background_addmon( Monitor* m );
/** takes it off m again */
void background_removemon( Monitor* m );
/** follows m's position, size and enabled state; cheap if they're unchanged */
void background_update( Monitor* m );
