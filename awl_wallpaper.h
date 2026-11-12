#pragma once

/**
 * The wallpaper's timer and actions, part of awl: when the wallpaper
 * changes and to which (next, previous, random), cfgdict's
 * "wallpaper_mode" (awl_wallpaper.c). The library's thread picks and
 * decodes the image (plugins/wallpaper.c), awl_background.h shows it.
 * Main thread only.
 */

#include "awl_server.h"

/** the timer, unarmed until :c:func:`configapply` */
void wallpaperinit(awl_server_t *srv);
void wallpaperfini(awl_server_t *srv);
/** cfgdict's "wallpaper_mode": :c:member:`cfg->wallpaper.mode <awl_wallpaper_config_t.mode>` */
void wallpapermodeinit(awl_server_t *srv);
/** after plugins_config.c changed: its interval and mode */
void wallpaperconfigure(awl_server_t *srv);
/** shows a wallpaper the library has decoded since, if any */
void wallpapertake(awl_server_t *srv);
