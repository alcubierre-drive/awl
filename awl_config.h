#pragma once

/**
 * The configuration, part of awl: `awl_config_t` (awl.h), awl's defaults (awl_config_defaults.h) as
 * plugins_config.c changes them in the plugin library, anew on every reload
 * (awl_config.c). Main thread only.
 */

#include "awl_server.h"
#include "awl_plugin_abi.h"
#include "awl_dict.h"

/** The config in effect: the library's (awl's defaults with
 * plugins_config.c's changes), or while none is loaded the defaults. The
 * library's points into it, so it's only good until the next reload;
 * `pluginsdetach()` falls back to the defaults. */
extern const awl_config_t *cfg;
/** The config values that can change while awl runs, by name, for awl and the
 * library (`awl_host_t.cfg`). Lives from setup() to cleanup(), across reloads. */
extern awl_dict_t *cfgdict;

/** fills in c with awl's defaults (awl_config_defaults.h) */
void configdefaults(awl_config_t *c);
/** awl's defaults themselves, constant (awl_config_defaults.h) */
const awl_config_t *builtinconfig(void);
/** creates cfgdict with its defaults (see there); NULL out of memory, then
 * every lookup fails */
void configinit(void);
void configfini(void);
/** switches to the loaded library's config, or awl's defaults,
 * and brings everything set up from it up to date */
void configuse(void);
/** wlroots' log level: cfg->wlr_log_level, WLR_DEBUG with awl -d; at
 * startup and after every config change */
void wlrloginit(void);
/** No bar is visible while locked or with every output off (unplugged or
 * powered down), so the plugins' 1 s polling can stop. */
void updatepluginpause(void);

/* the functions the config binds keys and buttons to (`Key.func`,
 * `Button.func`) and arranges its layouts with (`Layout.arrange`), defined
 * in the modules they belong to */
#define AWL_ACTION_REGISTER(name) void name(const Arg *arg);
AWL_ACTIONS
#undef AWL_ACTION_REGISTER
#define AWL_LAYOUT_REGISTER(name) void name(Monitor *m);
AWL_ARRANGES
#undef AWL_LAYOUT_REGISTER
