#pragma once

/**
 * What a screenshot needs of awl, for the plugin library (awl_host.h's
 * screenshot entries), part of awl: a pointer grab (:c:enumerator:`CurGrab`)
 * that hands the pointer and the keys to the library, its mark (a shade
 * around a box, above everything), rendering the scene again for a box
 * into an image, and a clipboard of awl's own. Main thread only.
 */

#include "awl_server.h"
#include "awl_host.h"

/** :c:member:`awl_host_t.grab_start` */
int grabstart(awl_server_t *srv, const awl_grab_t *grab, void *data, const char *cursor);
/** :c:member:`awl_host_t.grab_end` */
void grabend(awl_server_t *srv);
/** Ends the grab for awl, the library's cancel told (the screen locks). */
void grabcancel(awl_server_t *srv);
/** Ends the grab the library left before it is unloaded, without telling it;
 * returns whether there was one. */
int grabreap(awl_server_t *srv);
/** :c:member:`awl_host_t.grab_mark` */
void grabmark(awl_server_t *srv, const awl_box_t *box, uint32_t dim, uint32_t frame, int frame_width);

/* awl_cursor.c's and awl_input.c's, while the mode is CurGrab: the
 * pointer is where it is now, a button changed, a key was pressed */
void grabmotion(awl_server_t *srv);
void grabbutton(awl_server_t *srv, uint32_t button, int pressed, uint32_t mods);
void grabkey(awl_server_t *srv, uint32_t sym, uint32_t mods);

/** :c:member:`awl_host_t.client_at` */
Client *clientat(awl_server_t *srv, double x, double y);
/** :c:member:`awl_host_t.client_focused` */
Client *clientfocused(awl_server_t *srv);
/** :c:member:`awl_host_t.client_box` */
int clientbox(awl_server_t *srv, Client *c, int border, awl_box_t *box);
/** :c:member:`awl_host_t.monitor_at` */
Monitor *monitorat(awl_server_t *srv, double x, double y);
/** :c:member:`awl_host_t.monitor_box` */
int monitorbox(awl_server_t *srv, Monitor *m, awl_box_t *box);

/** :c:member:`awl_host_t.capture` */
pixman_image_t *capture(awl_server_t *srv, const awl_box_t *box, float scale);
/** :c:member:`awl_host_t.clipboard_set` */
int clipboardset(awl_server_t *srv, const char *mime, const void *data, size_t len);

/** Frees what is left, after the display is destroyed. */
void capturefini(awl_server_t *srv);
