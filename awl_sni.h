#pragma once

/**
 * The StatusNotifierWatcher, part of awl: the registry of the tray items
 * (org.kde.StatusNotifierWatcher on the session bus), on awl's main loop,
 * with sd-bus. It lives in awl itself, not in the plugin library, so the
 * items' registrations outlive a plugin reload; the bar's tray widget
 * (plugins/sni.c) is the host and asks it for the items over the bus like
 * any other host would. awl is the host as long as the watcher runs:
 * IsStatusNotifierHostRegistered is true throughout, so items don't fall
 * back to something else while the plugins reload.
 *
 * An item registers by bus name (its path then is /StatusNotifierItem) or
 * by object path (its bus name the caller's); it is listed as bus name
 * and path, ":1.42/StatusNotifierItem", in the order they registered,
 * until that name goes away. If
 * another watcher has the name, awl's waits in the queue for it. A lost
 * bus is reconnected to after a few seconds. Main thread only.
 */

struct wl_event_loop;

/** Starts it on loop; it connects from the loop. Returns 0, or -1 (logged). */
int awl_sni_watcher_start( struct wl_event_loop* loop );
/** Stops it, letting go of the name. Nothing if it isn't running. */
void awl_sni_watcher_stop( void );
