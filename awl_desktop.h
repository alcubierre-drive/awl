#pragma once

/**
 * The files in the config dictionary's ``desktop_dir`` (``$HOME/Desktop``
 * unless a plugin changes it), listed in a panel over the wallpaper on
 * every monitor. This side only places the panels and owns their buffers;
 * what's in them -- the file list, its scanner thread (all file system access
 * is there, so a hung mount can't block the compositor), the layout, the look
 * and what clicks do -- is plugins_desktop_panel.c in ``libawlplugins.so``,
 * so it reloads with the plugins. Without the library, no panel. Main thread only.
 *
 * The panels live in their own scene tree right above ``LyrBg``, so they
 * cover the wallpaper (awl_background.h) and background layer surfaces but
 * nothing else, and they're not part of ``layers[]``, so ``xytonode()`` and
 * clicks go right through them.
 */

#include "awl_core.h"

/** How the panels look; awl fills it from cfg->blur (the launcher's). */
typedef struct {
    /** blur the wallpaper behind the panel */
    int blur;
    /** blur_strength and blur_alpha: the blur's strength and opacity */
    float blur_strength, blur_alpha;
    /** corner radius, logical pixels */
    int radius;
} desktop_style_t;

/** tree: an empty scene tree, already placed in the stacking order */
void desktop_init( struct wlr_scene_tree* tree, const desktop_style_t* cfg );

/** plugins_config.c was reloaded */
void desktop_configure( const desktop_style_t* cfg );

/** m's bar must have its font loaded (m->bar.font); the panel draws in
 * m->desktopfont, else in that */
void desktop_addmon( Monitor* m );
/** drops m's panel */
void desktop_removemon( Monitor* m );
/** redraws m's panel if anything it shows changed (the library's content,
 * usable area, scale), hides it if m is disabled; cheap otherwise */
void desktop_update( Monitor* m );
/** m's bar font (m->bar.font) or m->desktopfont was reloaded (scale or
 * config change). Not part of
 * `desktop_update()`'s check: a new font can land at the old one's address,
 * and on a scale change the layout is arranged before ``updatebar()``
 * reloads it */
void desktop_fontchanged( Monitor* m );
/** `desktop_update()` for every monitor; on each redraw request, which is
 * also how the library reports changed files */
void desktop_update_all( void );
/** after the library was (re)loaded */
void desktop_reloaded( void );

/** a click on the bare desktop, mods as ``WLR_MODIFIER_*`` without the
 * ignored ones; returns whether it was taken (forwards to
 * `awl_plugin_api_t.desktop_click`) */
int desktop_click( int button, uint32_t mods );
