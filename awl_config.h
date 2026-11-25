#pragma once

/**
 * The configuration, part of awl: :c:type:`awl_config_t` (awl.h), awl's defaults (awl_config_defaults.h) as
 * plugins_config.c changes them in the plugin library, anew on every reload
 * (awl_config.c). Main thread only.
 */

#include "awl_server.h"
#include "awl_plugin_abi.h"
#include "awl_dict.h"


/** fills in c with awl's defaults (awl_config_defaults.h), what they leave
 * unset still unset (:c:func:`awl_config_fill_defaults`) */
void config_defaults( awl_config_t *c );
/** awl's defaults (awl_config_defaults.h), filled in
 * (:c:func:`awl_config_fill_defaults`), constant; made once, in proc
 * (:c:member:`awl_process_t.config`) */
const awl_config_t *builtin_config( awl_process_t *proc );
/** creates cfg_dict with its defaults (see there); NULL out of memory, then
 * every lookup fails */
void config_init( awl_server_t *srv );
void config_fini( awl_server_t *srv );
/** the config dictionary's "home" (the user's home folder, which awl takes
 * from there rather than $HOME) into out, n bytes; 0, or -1 if there is
 * none or it doesn't fit */
int cfg_home( awl_server_t *srv, char *out, size_t n );
/** switches to the loaded library's config, or awl's defaults,
 * and brings everything set up from it up to date */
void config_use( awl_server_t *srv );
/** Hands DISPLAY, WAYLAND_DISPLAY and XDG_CURRENT_DESKTOP to systemd's and
 * the session bus' environment (:c:func:`awl_bus_import_environment`), once they
 * are set at the start; after that, every reload that changes one of them
 * does it again. Blocks for up to a few seconds. What it did is proc's
 * (:c:member:`awl_process_t.config`). */
void environment_import( awl_process_t *proc );
/** With :c:member:`cfg->systemd_session <awl_config_t.systemd_session>`,
 * starts graphical-session.target (:c:func:`awl_bus_session`), once, after
 * :c:func:`environment_import`. Blocks for up to half a second. */
void session_start( awl_process_t *proc, bool systemd_session );
/** stops it again, if session_start() started it */
void session_stop( awl_process_t *proc );
/** wlroots' log level: :c:member:`cfg->wlr_log_level <awl_config_t.wlr_log_level>`, WLR_DEBUG with awl -d; at
 * startup and after every config change */
void wlr_log_setup( awl_server_t *srv );
/** No bar is visible while locked or with every output off (unplugged or
 * powered down), so the plugins' 1 s polling can stop. */
void update_plugin_pause( awl_server_t *srv );

/* the functions the config binds keys and buttons to (`Key.func`,
 * `Button.func`) and arranges its layouts with (`Layout.arrange`), defined
 * in the modules they belong to */
#define AWL_ACTION_REGISTER(name) void name(awl_server_t *srv, const Arg *arg);
AWL_ACTIONS
#undef AWL_ACTION_REGISTER
#define AWL_LAYOUT_REGISTER(name) void name(awl_server_t *srv, Monitor *m);
AWL_ARRANGES
#undef AWL_LAYOUT_REGISTER
