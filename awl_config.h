#pragma once

/**
 * The configuration, part of awl: config_awl.h, which awl reads at startup,
 * and config_plugins.h, which comes with the plugin library and changes on
 * every reload (awl_config.c). Main thread only.
 */

#include "awl_server.h"
#include "awl_plugin_abi.h"
#include "awl_dict.h"

/** an argv's first word (an Autostarts entry's, a spawn binding's) that
 * starts the rest in the tray's icon theme instead of the desktop's, see
 * spawn_pid() */
#define TRAY_ICONS "awl:tray-icons"

/* config_awl.h, in every module that includes this: what one of them
 * doesn't use is no mistake */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "config_awl.h"
#pragma GCC diagnostic pop

/** config_plugins.h in effect: the library's, or while none is
 * loaded config_plugins.def.h. It points into the library, so it's only
 * good until the next reload; `pluginsdetach()` falls back to builtinconfig(). */
extern const awl_config_t *cfg;
/** The config values that can change while awl runs, by name, for awl and the
 * library (`awl_host_t.cfg`). Lives from setup() to cleanup(), across reloads. */
extern awl_dict_t *cfgdict;

/** config_plugins.def.h, built into awl */
const awl_config_t *builtinconfig(void);
/** creates cfgdict with its defaults (see there); NULL out of memory, then
 * every lookup fails */
void configinit(void);
void configfini(void);
/** switches to the loaded library's config_plugins.h, or the built-in one,
 * and brings everything set up from it up to date */
void configuse(void);
/** No bar is visible while locked or with every output off (unplugged or
 * powered down), so the plugins' 1 s polling can stop. */
void updatepluginpause(void);

/* the functions config_plugins.h binds keys and buttons to (`Key.func`,
 * `Button.func`) and arranges its layouts with (`Layout.arrange`), defined
 * in the modules they belong to */
#define AWL_ACTION_DECL(name) void name(const Arg *arg);
AWL_ACTIONS(AWL_ACTION_DECL)
#undef AWL_ACTION_DECL
#define AWL_ARRANGE_DECL(name) void name(Monitor *m);
AWL_ARRANGES(AWL_ARRANGE_DECL)
#undef AWL_ARRANGE_DECL
