// awlock's entry point, for awl_lockhelper.c: awlock is part of awl, a
// fork of awl's lock helper, not a program of its own. It has no command
// line: awl's config (awl_config_t.lock) is its configuration.
#ifndef AWLOCK_MAIN_H
#define AWLOCK_MAIN_H

#include <stdbool.h>
#include <stdint.h>

/** the logo's states, each with its colors in :c:struct:`awlock_config_t`:
 * waiting, typing, Caps Lock, validating, wrong */
#define AWLOCK_STATES 5
/** the longest :c:member:`awlock_config_t.logo`, its NUL included */
#define AWLOCK_LOGO_MAX 1024

/** a state's colors, 0xRRGGBBAA: the logo's border and fill pixels */
typedef struct awlock_colors_t {
    uint32_t border, fill;
} awlock_colors_t;

/** awlock's configuration, from awl's (:c:member:`awl_config_t.lock`,
 * :c:type:`awl_colors_t`'s lock_*); plain data, it goes to the lock helper
 * as it is */
typedef struct awlock_config_t {
    /** by state, in :c:macro:`AWLOCK_STATES`' order */
    awlock_colors_t colors[AWLOCK_STATES];
    /** an XPM file instead of awlock's own logo (lock/README.md); "" its
     * own */
    char logo[AWLOCK_LOGO_MAX];
    /** logical pixels per logo pixel, 1 to 16 */
    uint32_t logo_scale;
    /** draws the logo */
    bool show_logo;
    /** fprintd's fingerprints too, if there is a reader */
    bool fingerprint;
    /** the logo's opacity, 0 to 1 (times its colors'), on the outputs
     * without the pointer, once the pointer has been on one */
    float inactive_alpha;
    /** the logo's opacity, 0 to 1 (times its colors'), on the pointer's
     * output, and on every output before the pointer has been on one */
    float active_alpha;
    /** timeout to retry on failed fprintd/already taken/... */
    uint32_t fingerprint_retry_ms;
} awlock_config_t;

/** awlock's main(), in a fork of awl's lock helper (awl_lockhelper.h),
 * which logs as awl does (awl_log.h: the fork's copy of awl's log, at
 * awl's level); cfg its configuration; wayland_fd its connection to awl
 * (a socket ``wl_client_create()`` took the other end of); its exit
 * status */
int awlock_main( const awlock_config_t *cfg, int wayland_fd );

#endif
