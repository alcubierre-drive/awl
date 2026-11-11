#pragma once

#include "handles.h"

#include <pixman-1/pixman.h>
#include <stdint.h>

/**
 * The notifications' windows: one persistent popup (:c:type:`awl_host_t`'s
 * window_set_persistent) per notification awl's server keeps open
 * (:c:type:`awl_host_t`'s notif_list), stacked from a corner of the selected monitor
 * (:c:member:`awl_plugin_config_t.notify`), newest first, as far as the monitor's
 * window area has room: the last one that fits is cut to it (the body's
 * lines, the last ending in "…"), the rest behind "+k more". A
 * notification's own look (:c:type:`awl_notif_look_t`, awl's own) beats the
 * config's.
 *
 * A window shows the image left of the text: the image-data
 * converted here, else the files or icon names the notification names,
 * decoded on a thread (notify_image.h) at the monitor's scale. A new
 * window waits for its image up to 100 ms before it shows, so it doesn't
 * grow right after; a later one is drawn when it comes. The summary in
 * bold, the body's markup (plugins/markup.h) wrapped under it, a bar for a
 * ``value`` and the actions as buttons. Left click: the action under the
 * pointer, else the default action, else dismiss; right click: dismiss;
 * middle click: dismiss all. The pointer on a window holds its timeout.
 * A click on "+k more" opens the menu (notify_menu.h) in the stack's
 * place; an open menu covers the windows, which stay.
 *
 * Do-not-disturb, the config dictionary's ``notify_dnd`` (so it survives
 * a reload): only critical notifications show, and those asked for (shown
 * again from the history, or shown with the menu's "Show",
 * :c:func:`awl_notify_reveal`); the others stay open in the server and in the
 * menu.
 *
 * Everything is copied out of the server's records when they change, so
 * nothing here outlives a list. Main thread only.
 */

/** The bar's bell from the icon theme (:c:member:`awl_plugin_config_t.notify`'s
 * bell_icon, with dnd bell_icon_dnd) at px pixels, ``*symbolic`` whether its
 * shape is to be drawn in the text's color; NULL while the image thread
 * looks for it (it redraws when done), if none of the names was found, or
 * none is configured: the glyph then. The image is kept here, valid until
 * the next call: take a reference to keep it. */
pixman_image_t* awl_notify_bell( awl_notify_t* ns, int px, int dnd, int* symbolic );
/** :c:func:`awl_notify_bell` for any of the config's icons: names (icon names or
 * paths, ";" between them, the first found wins) at px pixels, alike */
pixman_image_t* awl_notify_bar_icon( awl_notify_t* ns, const char* names, int px, int* symbolic );

/** the notifications' state, with the menu's (notify_menu.h); makes the
 * windows for what is open already (a reload). NULL if out of memory; the
 * functions here take NULL: nothing shows then, and they count none. */
awl_notify_t* awl_notify_start( awl_plugin_data_t* p );
/** destroys the windows, releasing a hold, and the menu, stops the image
 * thread (notify_image.h) and frees ns; returns nonzero if that is stuck */
int awl_notify_stop( awl_notify_t* ns );
/** ns's menu (notify_menu.h); NULL with ns NULL */
awl_notify_menu_t* awl_notify_menu( awl_notify_t* ns );
/** :c:member:`awl_plugin_api_t.notifications_changed` */
void awl_notify_changed( awl_notify_t* ns );
/** whether do-not-disturb is on (the config dictionary's, awl's) */
int awl_notify_dnd( void );
/** turns do-not-disturb on or off: the windows and the menu follow, an OSD
 * tells (:c:func:`awl_osd_dnd`) */
void awl_notify_set_dnd( awl_notify_t* ns, int on );
/** closes every open notification, as the user's dismissal */
void awl_notify_dismiss_all( awl_notify_t* ns );
/** how many notifications are open, how many of them critical into
 * ``*critical`` (unless NULL), :c:enumerator:`AWL_NOTIF_OSD` ones not counted; 0 while the
 * windows aren't started (ns NULL) */
int awl_notify_count( awl_notify_t* ns, int* critical );
/** how many open notifications are unread: those not asked for (shown
 * again from the history, :c:enumerator:`AWL_NOTIF_SHOWN`, or revealed) nor
 * :c:enumerator:`AWL_NOTIF_OSD`; 0 while the
 * windows aren't started (ns NULL) */
int awl_notify_unread( awl_notify_t* ns );
/** whether do-not-disturb hides open notification id (not critical, not
 * asked for); its image not come yet doesn't count */
int awl_notify_hidden( awl_notify_t* ns, uint32_t id );
/** shows open notification id though do-not-disturb, its timeout started
 * over; a reload forgets that */
void awl_notify_reveal( awl_notify_t* ns, uint32_t id );
