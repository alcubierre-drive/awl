#pragma once

/**
 * The tray items' tooltips: the item's name, in one of awl's text popups
 * (awl_window.h) under the icon, shown after the pointer rests on the icon
 * for a moment, hidden once it leaves, on a click, or when a menu opens.
 * tooltip.c: the tray thread posts what it learns (awl_tray_tooltip_enter(),
 * awl_tray_tooltip_leave()) to awl's main thread, which shows it; the bar's
 * own tray widget (plugins/sni.h), on the main thread already, calls
 * awl_tray_tooltip_show() and _drop() itself.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the tray's GTK thread ---- */

/** The pointer came onto an icon on the monitor of the output named monitor,
 * centered on center_x (logical pixels from its left edge), whose item is
 * called text. Returns what awl_tray_tooltip_leave() takes. */
uint32_t awl_tray_tooltip_enter( const char* monitor, int center_x, const char* text );
/** it left the icon (or clicked it) enter() returned token for */
void awl_tray_tooltip_leave( uint32_t token );

/* ---- awl's main thread ---- */

/** hides the tooltip, if one is shown or about to be */
void awl_tray_tooltip_hide( void );
/** what awl_tray_tooltip_enter() hands over: the tooltip text for token,
 * shown after a moment (at once if another one is) */
void awl_tray_tooltip_show( uint32_t token, const char* monitor, int center_x, const char* text );
/** what awl_tray_tooltip_leave() hands over */
void awl_tray_tooltip_drop( uint32_t token );
/** gives back its window and timer (the plugin library's copy, before it is
 * unloaded) */
void awl_tray_tooltip_fini( void );

#ifdef __cplusplus
}
#endif
