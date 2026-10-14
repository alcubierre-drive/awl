#pragma once

/**
 * awl's side of ``libawlplugins.so`` (see awl_plugin_abi.h): loads it, swaps
 * it for a freshly built one on reload, and forwards to it. Main thread only.
 *
 * The library is looked up next to the awl binary, or at ``$AWL_PLUGINS`` if
 * set. It is copied into a memfd before ``dlopen()``, so rebuilding it while
 * awl runs is safe and every load gets the current file, never a cached
 * handle.
 */

#include "awl_draw.h"
#include "awl_dict_type.h"

/** Loads the library and starts the plugins. cfg becomes `awl_host_t.cfg`'s
 * dictionary; it stays awl's and must outlive `awl_plugins_unload()`. Returns
 * 0 on success; on failure awl runs on with empty bars until a reload
 * succeeds. */
int awl_plugins_load( int paused, awl_dict_t* cfg );

/**
 * Loads the library from disk again and swaps it in. `detach()` must clear
 * every bar's widgets and drop any pointer to them (hover state); it runs
 * while the old library is still loaded, so widget callbacks may still be
 * called from it. `attach()` must give every bar its widgets again via
 * `awl_plugins_bar_widgets()`. If the new library doesn't load, the old one
 * stays and its plugins are just restarted. Returns 0 if the new library
 * was swapped in.
 */
int awl_plugins_reload( void (*detach)( void ), void (*attach)( void ) );

/** Stops the plugins and unloads the library. Every bar's widgets must have
 * been cleared before. */
void awl_plugins_unload( void );

/** `awl_plugin_api_t.set_paused`, if a library is loaded */
void awl_plugins_set_paused( int paused );

/** `awl_plugin_api_t.session_covered`, if a library is loaded; also given
 * to every library started later */
void awl_plugins_set_covered( int covered );

/** Fills an empty bar's widget lists; no-op without a loaded library. */
void awl_plugins_bar_widgets( awl_draw_t* bar );

typedef struct awl_actions_t awl_actions_t;
/** awl.c's functions config_plugins.h binds keys to, for the library */
extern const awl_actions_t awl_actions;
typedef struct awl_arranges_t awl_arranges_t;
/** awl.c's functions config_plugins.h's layouts arrange with, for the library */
extern const awl_arranges_t awl_arranges;

typedef struct awl_host_t awl_host_t;
/** awl's host table (awl_host.h), the one the library gets; for the tray's
 * `awl_tray_init()`. Lives as long as awl. */
const awl_host_t* awl_host_table( void );

typedef struct awl_plugin_api_t awl_plugin_api_t;
/** The loaded library's table, NULL if none. Changes on reload, so don't
 * keep it (or anything from it) across one. */
const awl_plugin_api_t* awl_plugins_api( void );
