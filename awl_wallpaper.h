#pragma once

/**
 * The wallpaper's timer and actions, part of awl: when the wallpaper
 * changes and to which (next, previous, random), cfgdict's
 * "wallpaper_mode" (awl_wallpaper.c). The library's thread picks and
 * decodes the image (plugins/wallpaper.c), awl_background.h shows it.
 * Main thread only.
 */

#include "awl_server.h"

/** the timer, unarmed until configapply() */
void wallpaperinit(void);
void wallpaperfini(void);
/** cfgdict's "wallpaper_mode": cfg->wallpaper.mode */
void wallpapermodeinit(void);
/** after plugins_config.c changed: its interval and mode */
void wallpaperconfigure(void);
/** shows a wallpaper the library has decoded since, if any */
void wallpapertake(void);
