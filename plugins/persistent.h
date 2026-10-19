#pragma once

/**
 * What the plugins keep across awl's restarts: ``key=value`` lines in one
 * file, at the config dictionary's ``state_home`` joined with its
 * ``awl_persistent`` (which the library's ``init()`` sets). The config
 * dictionary itself only lasts as long as awl.
 *
 * Plugin threads only: these read and write the file, and the main thread
 * never touches the file system. Calls are serialized among themselves.
 */

#include <stddef.h>
#include <stdint.h>

/** the config dictionary's key the file's path relative to ``state_home``
 * is under */
#define AWL_PERSISTENT_KEY "awl_persistent"

/** the config dictionary's key that lists, space-separated, the keys
 * `awl_persistent_load_str()` has loaded in this awl run */
#define AWL_PERSISTENT_LOADED_KEY "awl_persistent_loaded"

/** Writes the file's path to out. Returns 0, or -1 if ``state_home`` or
 * `AWL_PERSISTENT_KEY` isn't a string or the path doesn't fit. */
int awl_persistent_path( char* out, size_t n );
/** Reads key's string into out. Returns 0, or -1 if the file or the key is
 * missing or the string doesn't fit. */
int awl_persistent_get_str( const char* key, char* out, size_t n );
/** Reads key's number into v. Returns 0, or -1 if the file, the key or a
 * number there is missing. */
int awl_persistent_get_num( const char* key, int64_t* v );
/** Stores v under key, keeping the file's other lines, and creates the file
 * and its directories if need be. The new file replaces the old one whole
 * (rename()), so a crash leaves one or the other. Returns 0, or -1
 * (logged). */
int awl_persistent_set_num( const char* key, int64_t v );
/** like `awl_persistent_set_num()`, for a string without newlines */
int awl_persistent_set_str( const char* key, const char* v );

/** For a string key awl gives a default (awl_config.c's ``cfgdictdefaults()``):
 * once per awl run, at the first call for key, replaces the dictionary's
 * value with the file's, so a saved one wins over the default. Later calls,
 * e.g. after a reload, leave the dictionary alone, its value is newer.
 * Returns 0 if it set key, else -1. */
int awl_persistent_load_str( const char* key );
/** Stores the dictionary's string under key, unless the file has it
 * already. Returns 0, or -1 if key isn't a string or it can't be written. */
int awl_persistent_save_str( const char* key );
