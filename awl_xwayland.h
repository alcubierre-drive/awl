#pragma once

/**
 * XWayland, part of awl if built with XWAYLAND: the X server, started with
 * the first X client, and its windows as clients (awl_client.h); the
 * client_* helpers there tell them apart (awl_xwayland.c). Main thread
 * only.
 */

#include "awl_server.h"

#ifdef XWAYLAND

/** sets it up and $DISPLAY to it; awl runs on without it if that fails */
void xwaylandinit(awl_server_t *srv);
void xwaylandfini(awl_server_t *srv);
#endif
