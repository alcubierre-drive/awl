#pragma once

/**
 * The compositor's core objects, made by setup() (awl.c) and shared by all
 * of awl's modules: the display and its event loop, the backend, the
 * renderer, the scene with its layers, the seat. Each module's own state
 * is in its header (awl_output.h's monitors, awl_client.h's clients,
 * awl_cursor.h's cursor, ...). Main thread only.
 */

#include "awl.h"
#include "awl_util.h"

/** listens to E with L, calling H */
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
/** the same with a listener of its own, which H frees */
#define LISTEN_STATIC(E, H)     do { struct wl_listener *_l = ecalloc(1, sizeof*_l); _l->notify = (H); wl_signal_add((E), _l); } while (0)

extern struct wl_display *dpy;
extern struct wl_event_loop *event_loop;
extern struct wlr_backend *backend;
/** NULL without a session (nested, headless) */
extern struct wlr_session *session;
extern struct wlr_renderer *drw;
extern struct wlr_allocator *alloc;
extern struct wlr_compositor *compositor;

extern struct wlr_scene *scene;
/** the scene's layers, bottom to top, `SceneLayer`; the wallpaper and the
 * desktop panels are under LyrBg (awl_background.h, awl_desktop.h), awl's
 * popups right above LyrTop (awl_popup.h), the alerts above everything
 * (awl_alert.h) */
extern struct wlr_scene_tree *layers[NUM_LAYERS];
/** the root color behind everything, the size of all outputs */
extern struct wlr_scene_rect *root_bg;

extern struct wlr_seat *seat;
extern struct wlr_idle_notifier_v1 *idle_notifier;
