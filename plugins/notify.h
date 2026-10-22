#pragma once

#include <pixman-1/pixman.h>
#include <stdint.h>

/**
 * The notifications' windows: one persistent popup (awl_host_t's
 * window_set_persistent) per notification awl's server keeps open
 * (awl_host_t's notif_list), stacked from a corner of the selected monitor
 * (`awl_plugin_config_t.notify`), newest first, as far as the monitor's
 * window area has room: the last one that fits is cut to it (the body's
 * lines, the last ending in "…"), the rest behind "+k more". A
 * notification's own look (`awl_notif_look_t`, awl's own) beats the
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
 * `awl_notify_reveal()`); the others stay open in the server and in the
 * menu.
 *
 * Everything is copied out of the server's records when they change, so
 * nothing here outlives a list. Main thread only.
 */

/** The bar's bell from the icon theme (`awl_plugin_config_t.notify`'s
 * bell_icon, with dnd bell_icon_dnd) at px pixels, *symbolic whether its
 * shape is to be drawn in the text's color; NULL while the image thread
 * looks for it (it redraws when done), if none of the names was found, or
 * none is configured: the glyph then. The image is kept here, valid until
 * the next call: take a reference to keep it. */
pixman_image_t* awl_notify_bell( int px, int dnd, int* symbolic );

/** makes the windows for what is open already (a reload) */
void awl_notify_start( void );
/** destroys the windows, releasing a hold, and stops the image thread
 * (notify_image.h); returns nonzero if that is stuck */
int awl_notify_stop( void );
/** `awl_plugin_api_t.notifications_changed` */
void awl_notify_changed( void );
/** whether do-not-disturb is on */
int awl_notify_dnd( void );
/** turns do-not-disturb on or off: the windows and the menu follow */
void awl_notify_set_dnd( int on );
/** closes every open notification, as the user's dismissal */
void awl_notify_dismiss_all( void );
/** how many notifications are open, how many of them critical into
 * *critical (unless NULL); 0 while the windows aren't started */
int awl_notify_count( int* critical );
/** how many open notifications are unread: those not asked for (shown
 * again from the history, AWL_NOTIF_SHOWN, or revealed); 0 while the
 * windows aren't started */
int awl_notify_unread( void );
/** whether do-not-disturb hides open notification id (not critical, not
 * asked for); its image not come yet doesn't count */
int awl_notify_hidden( uint32_t id );
/** shows open notification id though do-not-disturb, its timeout started
 * over; a reload forgets that */
void awl_notify_reveal( uint32_t id );
