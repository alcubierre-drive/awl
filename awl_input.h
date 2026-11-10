#pragma once

/**
 * The seat's input devices, part of awl: keyboards (one group for the real
 * ones, one each for virtual ones), the key bindings, pointer devices and
 * their libinput settings, the selections, idle inhibitors (awl_input.c).
 * Main thread only.
 */

#include "awl_server.h"

/** the modifier mask without Caps Lock, as keys and buttons are matched */
#define CLEANMASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
/** a binding's mod (:c:member:`Key.mod`, :c:member:`Button.mod`) with :c:macro:`MODKEY` as cfg->modkey */
uint32_t bindmods(awl_server_t *srv, uint32_t mod);


/** the keyboard group of the real keyboards, before the first one; and the
 * list of devices configapply() reconfigures */
void inputinit(awl_server_t *srv);
void inputfini(awl_server_t *srv);
/** frees :c:member:`awl_server_t.input`; once the backend, and with it
 * every device, is gone */
void inputfree(awl_server_t *srv);
/** after the config changed: the keymap, key repeat, and the
 * pointers' libinput settings */
void inputconfigure(awl_server_t *srv);

/* the backend's new devices, virtual ones, the seat's selection requests,
 * new idle inhibitors */
void inputdevice(struct wl_listener *listener, void *data);
void virtualkeyboard(struct wl_listener *listener, void *data);
void virtualpointer(struct wl_listener *listener, void *data);
void setpsel(struct wl_listener *listener, void *data);
void setsel(struct wl_listener *listener, void *data);
void createidleinhibitor(struct wl_listener *listener, void *data);

/** whether an idle inhibitor whose surface is visible (any, with
 * bypass_surface_visibility) keeps the outputs on; exclude is going */
void checkidleinhibitor(awl_server_t *srv, struct wlr_surface *exclude);
