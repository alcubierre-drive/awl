#pragma once

/**
 * The tray items' tooltips: the item's name, in one of awl's text popups
 * (awl_window.h) under the icon, shown after the pointer rests on the icon
 * for a moment, hidden once it leaves, on a click, or when a menu opens. The
 * bar's tray widget (plugins/sni.h) calls these on awl's main thread.
 */

#include <stdint.h>

#include "handles.h"

/** the tooltip's state, its window made on first use; p is the root, whose
 * menus' window (tray_menu.h) it stays hidden under. NULL if out of memory;
 * the functions below take NULL as a no-op. */
awl_tray_tooltip_t* awl_tray_tooltip_new( awl_plugin_data_t* p );
/** hides the tooltip, if one is shown or about to be */
void awl_tray_tooltip_hide( awl_tray_tooltip_t* t );
/** the tooltip text for token (the caller's, nonzero), shown after a moment
 * (at once if another one is) */
void awl_tray_tooltip_show( awl_tray_tooltip_t* t, uint32_t token, const char* monitor, int center_x,
                            const char* text );
/** hides token's tooltip, if it is the one shown or waiting */
void awl_tray_tooltip_drop( awl_tray_tooltip_t* t, uint32_t token );
/** gives back its window and timer and frees t (the plugin library's fini) */
void awl_tray_tooltip_fini( awl_tray_tooltip_t* t );
