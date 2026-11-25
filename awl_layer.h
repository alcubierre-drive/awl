#pragma once

/**
 * Layer-shell surfaces, part of awl: panels, notifications, launchers. Each
 * monitor keeps its own in :c:member:`Monitor.layers`; :c:func:`arrange_layers` places them,
 * gives what is left to the windows, and hands the keyboard to a keyboard
 * interactive one (awl_layer.c). Main thread only.
 */

#include "awl_server.h"

/** the layer shell's new surfaces */
void create_layer_surface( struct wl_listener *listener, void *data );
/** places m's layer surfaces; arranges m if the window area changed */
void arrange_layers( awl_server_t *srv, Monitor *m );
