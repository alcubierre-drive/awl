#pragma once

/**
 * Picks and decodes the wallpaper: lists a folder's ``*.png`` files, picks one
 * and decodes it (Wuffs, wuffs/). A thread does that, the main thread only
 * asks for it and takes the result (see plugins_desktop_panel.c on why the
 * main thread never touches the file system); awl_background.c shows it.
 */

#include <stdint.h>

/** premultiplied ARGB8888 (``DRM_FORMAT_ARGB8888``), rows of stride bytes;
 * one ``malloc()``, freed by whoever has it last */
typedef struct awl_image_t {
    int width, height, stride;
    uint8_t data[];
} awl_image_t;

/** Starts the thread on the config dictionary's ``wallpaper_dir``, if it's set; awl sets it from
 * config_plugins.h's ``wallpaper_config.dir``, a plugin may change it before this. The thread starts with the
 * wallpaper the config dictionary's ``wallpaper_index`` names; at awl's start, before anything
 * set that, the one persistent.h's file names; then, also once per awl run, the file's
 * ``wallpaper_mode`` replaces the dictionary's (`awl_persistent_load_str()`). Logs if it
 * can't start. */
void awl_wallpaper_start( void );
/** Stops the thread, which first writes the index and ``wallpaper_mode`` to persistent.h's
 * file if they changed.
 * Returns nonzero if it had to be left running (see thread.h). */
int awl_wallpaper_stop( void );
/** steps forward (negative: back) in the sorted list; main thread */
void awl_wallpaper_step( int steps );
/** any other one; main thread */
void awl_wallpaper_random( void );
/** the one shown before the current one (one more each time), as long as
 * the thread remembers; main thread */
void awl_wallpaper_back( void );
/** where the list stood at the last change: the current index, its length
 * and the index the next random change goes to. 0 if not known yet. Main
 * thread. */
int awl_wallpaper_position( int* cur, int* n, int* rand_next );
/** like `awl_wallpaper_position()`, but 0 also while a step or random change
 * asked for isn't done yet. Main thread. */
int awl_wallpaper_settled( int* cur, int* n );
/** the newest decoded wallpaper not taken yet, or NULL; the caller owns it.
 * A redraw request announces one. Main thread. */
awl_image_t* awl_wallpaper_take( void );
