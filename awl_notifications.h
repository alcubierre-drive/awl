#pragma once

/**
 * The notification server, part of awl: org.freedesktop.Notifications on
 * the session bus (the Desktop Notifications Specification 1.2), on awl's
 * main loop, with sd-bus (awl_bus.h). It lives in awl itself, not in the
 * plugin library, like the tray's watcher (awl_sni.h): the name, the ids,
 * the open notifications and the actions clients wait on outlive a plugin
 * reload, and clients that block on Notify get their answer even without
 * the library. It keeps what came (`awl_notif_t`, awl_host.h), expires
 * it and tells the clients what became of it; showing it is the
 * library's (`awl_host_t.notif_list` and the `notifications_changed`
 * hook).
 *
 * - Notify: replaces_id replaces an open one in place (same id, place and
 *   generation + 1); a closed id it gave out comes back as a new one with
 *   that id. The hints x-canonical-private-synchronous and
 *   x-dunst-stack-tag replace the open one with the same tag and app.
 *   Hints taken: urgency, category, desktop-entry, image-data (image_data,
 *   icon_data), image-path (image_path), transient, resident, value; the
 *   rest is ignored. Image data is checked and copied, rows packed; a bad
 *   one is dropped, the notification still comes.
 * - The timeout: the client's, the config's by urgency for -1
 *   (`awl_config_t.notifications`), none for 0 and for critical ones.
 * - Capped: 256 open (a new one closes the oldest that isn't critical,
 *   else the oldest, reason 4), 64 MiB of image data (beyond, the image
 *   is dropped), the strings' lengths.
 * - awl's own (`awl_notifications_send()`, `awl_host_t.notif_send`) skip
 *   D-Bus: they come even without a bus or the name, with what a client has
 *   no say in (`awl_notif_look_t`, a timeout for critical ones), flagged
 *   AWL_NOTIF_AWL. No client replaces (by id or tag) or closes them, no
 *   signal tells of them; they replace each other by tag.
 * - The history (`awl_host_t.notif_history`) keeps the closed ones'
 *   contents but their actions (no client waits on those), image data up
 *   to 16 MiB for all of them (the oldest's goes first).
 *   `awl_notifications_show()` opens one again under a new id, flagged
 *   AWL_NOTIF_SHOWN: as awl's own, no client replaces or closes it, no
 *   signal tells of it; its timeout starts over.
 * - The name is requested with SD_BUS_NAME_QUEUE: another server that has
 *   it keeps it (logged), awl's waits behind it. A lost bus is connected to
 *   again; the notifications stay.
 *
 * Main thread only.
 */

#include <stdint.h>
#include "awl_host.h"

struct wl_event_loop;

/** Starts it on loop; it connects from the loop. changed, if not NULL, is
 * called once per loop round after the open notifications changed.
 * Returns 0, or -1 (logged). */
int awl_notifications_start( struct wl_event_loop* loop, void (*changed)( void ) );
/** Stops it, letting go of the name; the open notifications are dropped
 * without a signal. Nothing if it isn't running. */
void awl_notifications_stop( void );

/** `awl_host_t.notif_list` */
int awl_notifications_list( const awl_notif_t** out, int max );
/** `awl_host_t.notif_close` */
void awl_notifications_close( uint32_t id, awl_notif_reason_t reason );
/** `awl_host_t.notif_invoke` */
void awl_notifications_invoke( uint32_t id, const char* key, const char* token );
/** `awl_host_t.notif_send`: awl's own notifications, the library's and
 * the core's (awl_alert.c) */
uint32_t awl_notifications_send( const awl_notif_send_t* send );
/** `awl_host_t.notif_history` */
int awl_notifications_history( const awl_notif_past_t** out, int max );
/** `awl_host_t.notif_forget` */
void awl_notifications_forget( uint64_t seq );
/** `awl_host_t.notif_show` */
uint32_t awl_notifications_show( uint64_t seq );
/** `awl_host_t.notif_hold` */
void awl_notifications_hold( uint32_t id, int held );
/** a key or the pointer moved: timeouts stopped for idleness
 * (`awl_config_t.notifications.idle_pause`) resume. Cheap enough for
 * every motion. */
void awl_notifications_activity( void );
/** as if idle_pause had passed now (the test FIFO's nidle): the timeouts
 * stop until the next activity */
void awl_notifications_idle( void );
