#pragma once

/**
 * Popups that awl draws itself, part of awl: a box of text next to the bar,
 * looking like the tray's calendar and text popups (config_plugins.h's
 * ``tray_config.calendar``: colors, border, corner radius, padding; blurred
 * behind like the launcher), in the bar's font. Not a client: no Wayland
 * surface, no keyboard focus. It takes no input either, clicks and the
 * pointer go through to what is below. Main thread only.
 *
 * A window is a scene tree in the parent awl gives it, shown on one monitor
 * at a time.
 */

#include "awl.h"

/** How a window looks; awl fills it from config_plugins.h. Colors are
 * 0xRRGGBBAA, sizes logical pixels. */
typedef struct awl_window_style_t {
    uint32_t bg, fg, border;
    int border_width, radius, padding;
    /** blur what is behind it, with blur_strength and blur_alpha */
    int blur;
    float blur_strength, blur_alpha;
} awl_window_style_t;

typedef struct awl_window awl_window_t;

/** A hidden, empty window in parent, an empty scene tree already placed in
 * the stacking order. */
awl_window_t* awl_window_create( struct wlr_scene_tree* parent, const awl_window_style_t* style );
void awl_window_destroy( awl_window_t* win );

/** config_plugins.h was reloaded */
void awl_window_set_style( awl_window_t* win, const awl_window_style_t* style );

/** Replaces the text: rows separated by '\n', columns by '\t', plain text
 * (no markup). Columns are as wide as their widest cell. */
void awl_window_set_text( awl_window_t* win, const char* text );

/** Shows the window on m, or moves it there: on the bar's side of m's window
 * area (the top if top, else the bottom), centered on center_x (logical
 * pixels from m's left edge) but kept within m. m->drw's font must be
 * loaded. */
void awl_window_show( awl_window_t* win, Monitor* m, int center_x, int top );
void awl_window_hide( awl_window_t* win );
/** the monitor it is shown on, NULL while hidden */
Monitor* awl_window_monitor( const awl_window_t* win );

/** m was arranged, or its font or scale changed: places and redraws the
 * window if it is shown there (cheap if nothing changed), hides it if m is
 * disabled */
void awl_window_update( awl_window_t* win, Monitor* m );
/** m is going away: hides the window if it is shown there */
void awl_window_removemon( awl_window_t* win, Monitor* m );
