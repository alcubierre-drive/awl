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
void configdefaults(awl_config_t *c);
/** awl's defaults (awl_config_defaults.h), filled in
 * (:c:func:`awl_config_fill_defaults`), constant */
const awl_config_t *builtinconfig(void);
/** creates cfgdict with its defaults (see there); NULL out of memory, then
 * every lookup fails */
void configinit(awl_server_t *srv);
void configfini(awl_server_t *srv);
/** switches to the loaded library's config, or awl's defaults,
 * and brings everything set up from it up to date */
void configuse(awl_server_t *srv);
/** Hands DISPLAY, WAYLAND_DISPLAY and XDG_CURRENT_DESKTOP to systemd's and
 * the session bus' environment (:c:func:`awl_bus_import_environment`), once they
 * are set at the start; after that, every reload that changes one of them
 * does it again. Blocks for up to a few seconds. */
void environmentimport(void);
/** wlroots' log level: cfg->wlr_log_level, WLR_DEBUG with awl -d; at
 * startup and after every config change */
void wlrloginit(awl_server_t *srv);
/** No bar is visible while locked or with every output off (unplugged or
 * powered down), so the plugins' 1 s polling can stop. */
void updatepluginpause(awl_server_t *srv);

/* the functions the config binds keys and buttons to (`Key.func`,
 * `Button.func`) and arranges its layouts with (`Layout.arrange`), defined
 * in the modules they belong to */
#define AWL_ACTION_REGISTER(name) void name(const Arg *arg);
AWL_ACTIONS
#undef AWL_ACTION_REGISTER
#define AWL_LAYOUT_REGISTER(name) void name(Monitor *m);
AWL_ARRANGES
#undef AWL_LAYOUT_REGISTER
