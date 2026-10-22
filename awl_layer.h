#pragma once

/**
 * Layer-shell surfaces, part of awl: panels, notifications, launchers. Each
 * monitor keeps its own in ``Monitor.layers``; arrangelayers() places them,
 * gives what is left to the windows, and hands the keyboard to a keyboard
 * interactive one (awl_layer.c). Main thread only.
 */

#include "awl_server.h"

/** the layer shell's new surfaces */
void createlayersurface(struct wl_listener *listener, void *data);
/** places m's layer surfaces; arranges m if the window area changed */
void arrangelayers(Monitor *m);
