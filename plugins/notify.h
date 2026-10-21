#pragma once

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
 * A window shows the image (image-data only so far) left of the summary in
 * bold, the body's markup (plugins/markup.h) wrapped under it, a bar for a
 * ``value`` and the actions as buttons. Left click: the action under the
 * pointer, else the default action, else dismiss; right click: dismiss;
 * middle click: dismiss all. The pointer on a window holds its timeout.
 *
 * Everything is copied out of the server's records when they change, so
 * nothing here outlives a list. Main thread only.
 */

/** makes the windows for what is open already (a reload) */
void awl_notify_start( void );
/** destroys the windows, releasing a hold */
void awl_notify_stop( void );
/** `awl_plugin_api_t.notifications_changed` */
void awl_notify_changed( void );
