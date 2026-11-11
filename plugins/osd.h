#pragma once

/**
 * The notification a volume or brightness change brings
 * (:c:member:`awl_plugin_config_t.osd`): the new value as a bar beside an icon, no
 * text, one each for the volume and the brightness, replaced in place
 * while the changes go on. The wallpaper's and the desktop files' are
 * further below. Main thread.
 *
 * The daemons answer on their own threads, which wake awl for a redraw
 * (:c:func:`awl_osd_redraw`): for a while after a change is asked for, what the
 * value changes to goes out then, and if it doesn't change (at its limit,
 * say) it goes out as it is after a moment. The brightness doesn't wait:
 * its daemon can be slow, so the value an ask should come to goes out at
 * once, and is corrected when all asks are answered. A change that wasn't
 * asked for goes out by its cause, with it as the summary and the bar in
 * colors.osd_unasked: the daemon's automatic brightness, an ``Update()``
 * or another client's ``Adjust()`` (the daemon's ``LastSource``), another
 * output or port; not a mixer's change, nor a (re)connection's.
 * :c:enumerator:`AWL_NOTIF_OSD`: through do-not-disturb, not in the history, the menu or
 * the bar widget's counts.
 */

#include "handles.h"

/** the OSD's state, with the timer for a value that doesn't change; p is
 * the root, whose daemons it reads; NULL if out of memory. Every function
 * below takes NULL as a no-op. */
awl_osd_t* awl_osd_start( awl_plugin_data_t* p );
/** gives it back (the plugin library's fini) */
void awl_osd_stop( awl_osd_t* o );
/** the volume, or a mute, was just asked to change */
void awl_osd_volume( awl_osd_t* o );
/** the brightness was just asked to change by delta (1 is all of it,
 * :c:func:`backlight_adjust`): shows what that should come to right away,
 * and once the daemon has answered all such asks, what it came to if that
 * looks different. Without a value to start from, it waits for the daemon
 * as the volume does. */
void awl_osd_brightness( awl_osd_t* o, float delta );
/** :c:member:`awl_plugin_api_t.redraw`: sends what changed in answer to the asks,
 * corrects the brightness's guess, and sends a change nobody here asked for
 * by its cause */
void awl_osd_redraw( awl_osd_t* o );

/* The wallpaper, the desktop files and do-not-disturb: no watch, their changes are events.
 * Each has one notification (by tag), replaced in place. */

/** the wallpaper's now: "i/N · mode" (the timer's mode, "(off)" without a
 * timer) above its position in the folder as a bar, osd.wallpaper_icon
 * beside it; the mode alone while a change is under way */
void awl_osd_wallpaper( awl_osd_t* o );
/** a wallpaper change was just asked for: :c:func:`awl_osd_wallpaper` follows once
 * the image it ends on is taken */
void awl_osd_wallpaper_ask( awl_osd_t* o );
/** awl took a wallpaper (:c:member:`awl_plugin_api_t.wallpaper_take`): shows it if a
 * change was asked for and all asked for are done */
void awl_osd_wallpaper_taken( awl_osd_t* o );
/** the desktop files' text alone, centered, osd.desktop_icon beside it */
void awl_osd_desktop( awl_osd_t* o, const char* text );
/** do-not-disturb was turned on or off (:c:func:`awl_notify_set_dnd`): "Do not
 * disturb" or "Notifications enabled" alone, centered, notify's bell_icon_dnd
 * or bell_icon beside it. It shows through do-not-disturb, as every OSD. */
void awl_osd_dnd( awl_osd_t* o, int on );
