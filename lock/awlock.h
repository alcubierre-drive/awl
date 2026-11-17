// awlock: what its three parts share.
//   awlock.c       the Wayland side: the lock, its surfaces, the logo, the
//                  seat and main(); the event loop
//   auth.c         passwords: typing them, and checking them with PAM in a
//                  child process
//   fingerprint.c  fingerprints, with fprintd
#ifndef AWLOCK_H
#define AWLOCK_H
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "awl_log.h"
#include "awlock_main.h"

// from meson (-D): LOCK_PAM_SERVICE

// The log: awl's own (awl_log.h's awl_err_printf() and the others), each
// line "awlock: ...". This fork has a copy of awl's memory, its log among
// it: the same stderr, file and level as awl's. Never a password, nor
// anything typed.

// The event loop (awlock.c): a few fds (the display's, the password child's,
// the system bus's) and timers

struct loop_timer;

void loop_add_fd(int fd, void (*callback)(void *data), void *data);
// also from an fd's callback
void loop_remove_fd(int fd);
// The timer is freed once it has run, or has been removed
struct loop_timer *loop_add_timer(int ms, void (*callback)(void *data),
		void *data);
// NULL is fine
void loop_remove_timer(struct loop_timer *timer);

// The state

// Indicator state: status of authentication attempt
enum auth_state {
	AUTH_STATE_IDLE, // nothing happening
	AUTH_STATE_VALIDATING, // currently validating password
	AUTH_STATE_INVALID, // displaying message: password was wrong
};

// The logo's state (logo_state() in awlock.c), which picks its colours
enum logo_state {
	LOGO_WAITING, // nothing typed (also listening for a fingerprint)
	LOGO_TYPING, // the password buffer holds something
	LOGO_CAPS_LOCK, // Caps Lock is on, typed or not
	LOGO_VALIDATING, // the password is being checked
	LOGO_WRONG, // the password was wrong, until auth_idle_timer
	LOGO_STATES,
};

// a state's colours (awlock_main.h): LOGO_STATES of them, in its order
_Static_assert(LOGO_STATES == AWLOCK_STATES, "awlock_config_t.colors");

// the password typed so far: UTF-8, NUL-terminated
#define PASSWORD_SIZE 1024 // its buffer's, the NUL included
struct awlock_password {
	size_t len;
	char *buffer; // mlock()ed (auth.c)
};

struct awlock_xkb {
	bool caps_lock;
	bool control;
	struct xkb_state *state;
	struct xkb_context *context;
};

struct awlock_surface;

struct awlock_state {
	struct loop_timer *auth_idle_timer; // timer to stop displaying AUTH_STATE_INVALID
	struct loop_timer *clear_password_timer;  // clears the password buffer
	struct wl_display *display;
	int wayland_fd; // display's socket, from awl; the PAM child closes it
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct wl_list surfaces; // struct awlock_surface::link, one per output
	awlock_config_t args; // awl's (awlock_main.h)
	struct awlock_password password;
	struct awlock_xkb xkb;
	enum auth_state auth_state; // state of the authentication attempt
	// locked: the compositor has said so; running: until unlocked (a right
	// password or finger)
	bool running, locked;
	struct ext_session_lock_manager_v1 *lock_manager;
	struct ext_session_lock_v1 *lock;
	// the lock surfaces' content: one transparent pixel (single-pixel
	// buffer), stretched to each surface's size (viewporter)
	struct wp_single_pixel_buffer_manager_v1 *single_pixel;
	struct wp_viewporter *viewporter;
	struct wl_buffer *transparent;
	// the pointer: the lock surface it is on, or NULL, and where
	// (surface-local, logical); pointer_seen: it has entered one at all,
	// before which the logo is centred and not dimmed everywhere
	struct awlock_surface *pointer_surface;
	double pointer_x, pointer_y;
	bool pointer_seen;
	struct awlock_surface *pointer_drawn; // the logo last placed there
};

// awlock.c: redraws every output's logo, as the state changed
void damage_state(struct awlock_state *state);

// auth.c: forks the PAM child and makes the password buffer; checked
// passwords' results come in on the event loop. Exits on failure.
void auth_start(struct awlock_state *state);
// auth.c: a key typed (keysym), as text (UTF-8, "" for none)
void auth_handle_key(struct awlock_state *state,
		xkb_keysym_t keysym, const char *text);
// auth.c: shows AUTH_STATE_INVALID, then idle again after a while
void schedule_auth_idle(struct awlock_state *state);

// fingerprint.c: starts verifying fingerprints with fprintd: a match unlocks
// (state->running = false). NULL without a system bus.
struct fingerprint *fingerprint_start(struct awlock_state *state);
void fingerprint_stop(struct fingerprint *fp);

#endif
