#pragma once

/**
 * The notifications' menu: a popup with a row each for do-not-disturb
 * (:c:func:`awl_notify_dnd`), "Dismiss all", and every notification the server
 * keeps open, newest first, its title followed by its actions, indented.
 * Then the server's history (:c:type:`awl_notif_past_t`), dimmed, each with the
 * time it closed, under "Clear history" and its count. It is as wide as
 * :c:member:`awl_plugin_config_t.notify`'s menu_width, whatever it lists: a title is
 * cut with an ellipsis, its summary first, as the windows', left of its
 * "Show" button and time, which stay at the right; the other rows' text
 * too.
 * On a title, left click: its default action, else dismiss; right click:
 * dismiss; its "Show" button, while do-not-disturb hides it
 * (:c:func:`awl_notify_hidden`): shows its window (:c:func:`awl_notify_reveal`) and
 * closes the menu. On an action: that action, which closes the menu; the rest leave
 * it open, and it follows what changes. "Clear history", left: clears it;
 * a past one, left or right: forgets it; its "Show" button: shows it again
 * (:c:member:`awl_host_t.notif_show`) and closes the menu. Rows that don't fit the window area
 * end in one that says how many more there are. LEAVE_DELAY after the
 * pointer left the menu and what opened it (the widget; for "+k more",
 * once the pointer was in the menu), it closes.
 *
 * Opened by the bar's notification widget and by the "+k more" window
 * (notify.h); it covers the notifications' windows, which stay (awl's
 * popups: an open menu over the persistent ones). Everything is copied out of the server's records, as
 * the windows do. Main thread only.
 */

#include "../awl_plugin_abi.h"

/** opens the menu on m for a bar widget centered on center_x (logical
 * pixels from m's left edge), or closes it if it is open */
void awl_notify_menu_toggle( Monitor* m, int center_x );
/** opens the menu on m at a spot, an AWL_WINDOW_AT_* moved by dx, dy (the
 * notifications' corner), or closes it if it is open */
void awl_notify_menu_at( Monitor* m, int at, int dx, int dy );
/** the pointer is on the bar's widget (inside), or left it */
void awl_notify_menu_pointer( int inside );
/** whether it is open */
int awl_notify_menu_is_open( void );
/** the notifications or do-not-disturb changed: an open menu follows */
void awl_notify_menu_changed( void );
/** destroys the menu */
void awl_notify_menu_stop( void );
