#pragma once

/**
 * The boundary between awl and ``libawlplugins.so``, the live-reloadable part
 * of awl: the plugin threads (plugins.c, plugins/), the bar widgets
 * (plugins_widgets.c), the desktop file list (plugins_desktop_panel.c;
 * awl_desktop.c only places it), picking and decoding the wallpaper
 * (plugins/wallpaper.c; awl_background.c shows it) and the configuration
 * (plugins_config.c), which fills in `awl_config_t` (awl.h) over awl's
 * defaults.
 * awl loads the library with ``dlopen()`` and looks up a single symbol,
 * `AWL_PLUGIN_ENTRY`; everything else goes through two tables, `awl_host_t`
 * (awl_host.h) and `awl_plugin_api_t` below, so the library has no unresolved
 * references into awl (no ``-rdynamic``).
 *
 * The library still reads awl's structs directly (`awl_draw_t`, `widget_t`,
 * `Monitor`, `Client`, `awl_config_t`, `Arg`; all in awl.h), so it has to be
 * built from the same headers as the running awl. The entry point refuses a
 * host whose ABI version or struct sizes differ from its own. Bump `AWL_PLUGIN_ABI` whenever either table
 * changes.
 */

#include <stddef.h>
#include <stdint.h>
#include "awl.h"
#include "awl_dict_type.h"
#include "awl_host.h"

/** The version both sides compare; see the top of this file. */
#define AWL_PLUGIN_ABI 24
/** The name of the library's only exported symbol, an `awl_plugin_entry_t`. */
#define AWL_PLUGIN_ENTRY "awl_plugin_entry"

/**
 * Every awl function the configuration (plugins_config.c) can bind to a key or button, as an X macro.
 * The library binds them through same-named wrappers around
 * `awl_host_t.actions` (plugins_config.c).
 */
#define AWL_ACTIONS( X ) \
    X( changebw ) X( chvt ) X( cycle_layout ) X( cycle_view ) X( focusmon ) \
    X( focusstack ) X( incnmaster ) X( killclient ) X( lockscreen ) X( maximize ) X( minimize ) \
    X( moveresize ) X( movestack ) X( plugin_restart ) X( quit ) X( setlayout ) X( setmfact ) \
    X( spawn ) X( tag ) X( tagmon ) X( togglebar ) X( togglebw ) X( togglefloating ) \
    X( togglefullscreen ) X( toggleontop ) X( togglepertag ) X( toggletag ) X( toggleview ) \
    X( transluce ) X( unminimize ) X( view ) X( wallpaper ) X( wallpapermode )

#define AWL_ACTION_FIELD( name ) void (*name)( const Arg* arg );
/** awl's functions of `AWL_ACTIONS`, one ``void (*name)( const Arg* )`` each */
typedef struct awl_actions_t {
    AWL_ACTIONS( AWL_ACTION_FIELD )
} awl_actions_t;
#undef AWL_ACTION_FIELD

/** awl's layouts' arrange functions, likewise */
#define AWL_ARRANGES( X ) X( bstack ) X( gaplessgrid ) X( monocle ) X( tile )

#define AWL_ARRANGE_FIELD( name ) void (*name)( Monitor* m );
/** the functions of `AWL_ARRANGES`, one ``void (*name)( Monitor* )`` each */
typedef struct awl_arranges_t {
    AWL_ARRANGES( AWL_ARRANGE_FIELD )
} awl_arranges_t;
#undef AWL_ARRANGE_FIELD

/** What the library provides. Main thread only. */
typedef struct awl_plugin_api_t {
    /** `AWL_PLUGIN_ABI` as the library was built */
    uint32_t abi;
    /** makes the config (plugins_config.c's configure()), then starts the
     * plugin threads; before any other call but the entry point */
    void (*init)( int paused );
    /** stops and joins every plugin thread; afterwards no code of the library
     * runs anymore, except through widgets still attached to a bar. Returns
     * nonzero if a thread couldn't be joined (blocked in the kernel, e.g. on
     * a dead mount): it was left running, so the library must stay loaded. */
    int (*fini)( void );
    /** stops/resumes the 1 s polling while no bar is visible */
    void (*set_paused)( int paused );
    /** fills the bar's (empty) widget lists */
    void (*bar_widgets)( awl_draw_t* bar );

    /**
     * The desktop panel (this and the next three; all sizes in device
     * pixels; awl owns the buffer and the scene node, the library what goes
     * in it): changes whenever the panel's contents may have (files,
     * toggles); 0: show nothing. Cheap, called on every redraw request.
     */
    uint64_t (*desktop_version)( void );
    /** lays the panel out in font (Monitor.desktopfont,
     * else the bar's; never NULL) for the usable
     * area avail_w x avail_h with corner radius r; returns its position in
     * that area and its size (0: none) */
    void (*desktop_measure)( struct fcft_font* font, int avail_w, int avail_h, int r, float scale,
                             int* x, int* y, int* w, int* h );
    /** draws what the last measure laid out into data (premultiplied ARGB,
     * cleared), pix an image of it to draw text into, in the same font */
    void (*desktop_draw)( pixman_image_t* pix, struct fcft_font* font, uint32_t* data, int stride,
                          int w, int h, int r, float scale );
    /** a click on the bare desktop, mods as WLR_MODIFIER_* without the ignored
     * ones; returns whether it was taken */
    int (*desktop_click)( int button, uint32_t mods );

    /** changes the wallpaper (plugins/wallpaper.h); returns at once */
    void (*wallpaper)( WallpaperMode mode );
    /** the newest decoded wallpaper not taken yet, or NULL; the caller owns
     * it. A redraw request announces one. */
    awl_image_t* (*wallpaper_take)( void );
    /** the wallpaper timer's mode changed (the wallpapermode action) */
    void (*wallpaper_mode)( WallpaperMode mode );

    /** whether the screen is covered by the locker: from the first frame
     * that shows its surface on every output on, until it unlocks or
     * crashes (awl then stays locked under its blur, but not covered); right
     * after `init` too */
    void (*session_covered)( int covered );

    /** fills in c: awl's defaults (`awl_host_t.config_defaults`) with
     * plugins_config.c's changes; what it points to stays valid until the
     * library is unloaded */
    void (*config)( awl_config_t* c );
} awl_plugin_api_t;

/**
 * The type of the library's only exported symbol, `AWL_PLUGIN_ENTRY`.
 * Returns NULL if the library can't run against this host (ABI or struct
 * layout mismatch). The host table must stay valid as long as the library is
 * loaded.
 */
typedef const awl_plugin_api_t* (*awl_plugin_entry_t)( const awl_host_t* host );

