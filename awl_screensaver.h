#pragma once

/**
 * org.freedesktop.ScreenSaver's Inhibit and UnInhibit on the session bus, at
 * /org/freedesktop/ScreenSaver and /ScreenSaver, on awl's main loop, with
 * sd-bus (awl_bus.h), while :c:member:`awl_config_t.screensaver_inhibit` is
 * set: what players, browsers and call apps ask to keep the screen on with,
 * GTK apps through xdg-desktop-portal-gtk. While a cookie is held, idle is
 * inhibited (:c:func:`checkidleinhibitor`), visible or not: the caller has no
 * surface. A cookie ends with UnInhibit, with its caller leaving the bus, or
 * with the connection. The name is requested with SD_BUS_NAME_QUEUE: another
 * screensaver that has it keeps it. Main thread only.
 */

#include <stdbool.h>

typedef struct awl_server_t awl_server_t;
/** the service's state, :c:member:`awl_server_t.screensaver` */
typedef struct awl_screensaver_t awl_screensaver_t;

/** Starts or stops :c:member:`awl_server_t.screensaver` as
 * :c:member:`awl_config_t.screensaver_inhibit` says; it connects from the loop. */
void awl_screensaver_configure( awl_server_t* srv );
/** Stops it, letting go of the name and the cookies; none is fine. */
void awl_screensaver_stop( awl_server_t* srv );
/** whether a cookie is held; false for NULL */
bool awl_screensaver_inhibited( const awl_screensaver_t* S );
