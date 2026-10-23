#pragma once

/**
 * What awl's core keeps of the tray: the icon theme of the programs awl
 * starts with :c:macro:`TRAY_ICONS` (awl.h), :c:member:`awl_config_t.tray` (awl.h)'s
 * icon_theme (app_env.c, libawltray-env.so from env_shim.c). The bar's tray
 * widget is the library's (plugins/sni.h). Main thread only.
 */

#include <stdint.h>

/** What a program awl starts that asks for the tray's icon theme
 * (:c:macro:`TRAY_ICONS`, awl.h) gets: a dconf profile of its own, the one it
 * would have had, but with org.gnome.desktop.interface's icon-theme set to
 * the config's icon_theme (and locked, over the user's value), so GTK
 * programs draw their icons, the ones they send the tray as images too, in
 * it; the desktop's setting and everything else stay as they are. The
 * profile and its database are in-memory files, nothing is written to
 * disk. libawltray-env.so, preloaded, takes all of it back out of the
 * program's environment, so what it starts gets none of it. */
typedef struct awl_tray_app_env_t {
    /** "NAME=value" entries to put in place of the environment's, NULL
     * terminated: DCONF_PROFILE, LD_PRELOAD (the shim first, then
     * awl's), AWL_TRAY_ENV_FDS, and AWL_TRAY_DCONF_PROFILE, awl's own
     * DCONF_PROFILE, if it has one */
    const char *const *env;
    /** the in-memory files, close-on-exec in awl: the child, between
     * vfork() and exec, makes them inheritable */
    int fds[2];
} awl_tray_app_env_t;

/** The above for icon_theme, NULL if it is unset or not installed, or it
 * couldn't be made (the shim missing): start the program as it is. Made on
 * first use, and anew after a reload changed icon_theme, for the programs
 * started from then on. It lives until the next call or
 * :c:func:`awl_tray_app_cleanup`. */
const awl_tray_app_env_t *awl_tray_app_env(void);

/** the config's icon_theme (NULL or "": none), at startup and after each
 * plugin reload; the next :c:func:`awl_tray_app_env` makes it anew if it changed */
void awl_tray_app_set_theme(const char *theme);
/** removes what :c:func:`awl_tray_app_env` made */
void awl_tray_app_cleanup(void);
