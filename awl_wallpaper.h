#pragma once

/**
 * The wallpaper's timer and actions, part of awl: when the wallpaper
 * changes and to which (next, previous, random), cfg_dict's
 * "wallpaper_mode" (awl_wallpaper.c). The library's thread picks and
 * decodes the image (plugins/wallpaper.c), awl_background.h shows it.
 * Main thread only.
 */

#include "awl_server.h"

/** the timer, unarmed until :c:func:`config_apply` */
void wallpaper_init( awl_server_t *srv );
void wallpaper_fini( awl_server_t *srv );
/** cfg_dict's "wallpaper_mode": :c:member:`cfg->wallpaper.mode <awl_wallpaper_config_t.mode>` */
void wallpaper_mode_init( awl_server_t *srv );
/** after plugins_config.c changed: its interval and mode */
void wallpaper_configure( awl_server_t *srv );
/** shows a wallpaper the library has decoded since, if any */
void wallpaper_take( awl_server_t *srv );
