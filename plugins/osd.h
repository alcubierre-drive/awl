#pragma once

/**
 * The notification a volume or brightness change brings
 * (:c:member:`awl_plugin_config_t.osd`): the new value as a bar beside an icon, no
 * text, one each for the volume and the brightness, replaced in place
 * while the changes go on. Main thread.
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

/** the timer for a value that doesn't change; returns 0 */
int awl_osd_start( void );
/** gives it back (the plugin library's fini) */
void awl_osd_stop( void );
/** the volume, or a mute, was just asked to change */
void awl_osd_volume( void );
/** the brightness was just asked to change by delta (1 is all of it,
 * :c:func:`backlight_adjust`): shows what that should come to right away,
 * and once the daemon has answered all such asks, what it came to if that
 * looks different. Without a value to start from, it waits for the daemon
 * as the volume does. */
void awl_osd_brightness( float delta );
/** :c:member:`awl_plugin_api_t.redraw`: sends what changed in answer to the asks,
 * corrects the brightness's guess, and sends a change nobody here asked for
 * by its cause */
void awl_osd_redraw( void );
