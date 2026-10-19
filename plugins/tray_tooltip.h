#pragma once

/**
 * The tray items' tooltips: the item's name, in one of awl's text popups
 * (awl_window.h) under the icon, shown after the pointer rests on the icon
 * for a moment, hidden once it leaves, on a click, or when a menu opens. The
 * bar's tray widget (plugins/sni.h) calls these on awl's main thread.
 */

#include <stdint.h>

/** hides the tooltip, if one is shown or about to be */
void awl_tray_tooltip_hide( void );
/** the tooltip text for token (the caller's, nonzero), shown after a moment
 * (at once if another one is) */
void awl_tray_tooltip_show( uint32_t token, const char* monitor, int center_x, const char* text );
/** hides token's tooltip, if it is the one shown or waiting */
void awl_tray_tooltip_drop( uint32_t token );
/** gives back its window and timer (the plugin library's copy, before it is
 * unloaded) */
void awl_tray_tooltip_fini( void );
