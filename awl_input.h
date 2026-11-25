#pragma once

/**
 * The seat's input devices, part of awl: keyboards (one group for the real
 * ones, one each for virtual ones), the key bindings, pointer devices and
 * their libinput settings, the selections, idle inhibitors (awl_input.c).
 * Main thread only.
 */

#include "awl_server.h"

/** the modifier mask without Caps Lock, as keys and buttons are matched */
#define CLEAN_MASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
/** a binding's mod (:c:member:`Key.mod`, :c:member:`Button.mod`) with :c:macro:`MODKEY` as :c:member:`cfg->mod_key <awl_config_t.mod_key>` */
uint32_t bind_mods( const awl_config_t *cfg, uint32_t mod );


/** the keyboard group of the real keyboards, before the first one; and the
 * list of devices :c:func:`config_apply` reconfigures */
void input_init( awl_server_t *srv );
void input_fini( awl_server_t *srv );
/** frees :c:member:`awl_server_t.input`; once the backend, and with it
 * every device, is gone */
void input_free( awl_server_t *srv );
/** after the config changed: the keymap, key repeat, the
 * pointers' libinput settings, and the idle timer
 * (:c:member:`awl_config_t.idle_seconds`) */
void input_configure( awl_server_t *srv );

/* the backend's new devices, virtual ones, the seat's selection requests,
 * new idle inhibitors */
void input_device( struct wl_listener *listener, void *data );
void virtual_keyboard( struct wl_listener *listener, void *data );
void virtual_pointer( struct wl_listener *listener, void *data );
void set_primary_selection( struct wl_listener *listener, void *data );
void set_selection( struct wl_listener *listener, void *data );
void create_idle_inhibitor( struct wl_listener *listener, void *data );

/** whether an idle inhibitor, or a :c:type:`Rule` inhibit_idle window, that is visible (any, with
 * :c:member:`~awl_config_t.bypass_surface_visibility`), or an
 * org.freedesktop.ScreenSaver cookie (awl_screensaver.h) keeps the outputs on, and holds off
 * :c:member:`awl_config_t.on_idle`; exclude is going */
void check_idle_inhibitor( awl_server_t *srv, struct wlr_surface *exclude );
/** the :c:enum:`awl_idle_mode` :c:member:`arg->i <Arg.i>`, or with -1 the
 * next one; any other value is ignored */
void toggle_idle_inhibit( awl_server_t *srv, const Arg *arg );
/** the :c:enum:`awl_idle_mode` :c:func:`toggle_idle_inhibit` set */
awl_idle_mode_t idle_mode_get( awl_server_t *srv );
/** a key or the pointer: for the idle notifier's clients, the
 * notifications' timeouts (:c:func:`awl_notifications_activity`) and
 * :c:member:`awl_config_t.idle_seconds` */
void activity( awl_server_t *srv );
