/* awl_dict.h - minimal string -> tagged value hash map with a stable, portable C ABI. */
#pragma once

#include "awl_dict_type.h"

/**
 * The dict's functions; the types are awl_dict_type.h's.
 *
 * Threads: any thread may call any function but :c:func:`awl_dict_destroy`. Each call
 * holds the dict's lock while it runs, so on its own it is atomic. Whatever
 * has to see the dict unchanged across several calls holds the lock itself,
 * with :c:func:`awl_dict_lock`/:c:func:`awl_dict_unlock`:
 *
 * - using a string or owned pointer from :c:func:`awl_dict_get` or :c:func:`awl_dict_next`,
 *   which another thread could otherwise free by overwriting or removing
 *   the entry (plain pointers and numbers are copies, they need nothing),
 * - a read-modify-write, like incrementing a number,
 * - iterating with :c:func:`awl_dict_next` (:c:func:`awl_dict_iter` holds it for you).
 *
 * The lock is recursive, so the functions can still be called under it. It
 * is a plain mutex otherwise: hold it briefly, and don't wait for anything
 * under it, least of all another thread that may want it. An owned value's
 * free function runs under it, too.
 */

/** Create an empty :c:type:`awl_dict_t`. Returns NULL on allocation failure. */
awl_dict_t *awl_dict_new(void);

/** Free the :c:type:`awl_dict_t`, its key copies and the values it owns (strings, and
 * owned values through their free function). Plain pointers and numbers need
 * nothing. NULL is a no-op. No other thread may use the dict anymore, or
 * still hold its lock. */
void awl_dict_destroy(awl_dict_t *d);

/** Insert or overwrite. The key (NUL-terminated) and a string value are
 * copied; an owned value is taken over. The value it replaces is released as
 * by :c:func:`awl_dict_del`, unless it is the same owned pointer. On an error nothing
 * changes and an owned value stays the caller's.
 * Returns :c:enumerator:`AWL_DICT_OK`, :c:enumerator:`AWL_DICT_ERR_NOMEM` or :c:enumerator:`AWL_DICT_ERR_INVALID` (also for an
 * unknown kind, a NULL string or an owned value without free function). */
int32_t awl_dict_set(awl_dict_t *d, const char *key, const awl_dict_val_t *val);

/** Look up a key. On :c:enumerator:`AWL_DICT_OK` copies the value to ``*out`` (if out is non-NULL).
 * A string or owned pointer stays the dict's: it is valid until the entry is
 * overwritten or removed.
 * Returns :c:enumerator:`AWL_DICT_OK`, :c:enumerator:`AWL_DICT_NOT_FOUND` or :c:enumerator:`AWL_DICT_ERR_INVALID`. */
int32_t awl_dict_get(const awl_dict_t *d, const char *key, awl_dict_val_t *out);

/** Remove a key, freeing a string or owned value.
 * Returns :c:enumerator:`AWL_DICT_OK`, :c:enumerator:`AWL_DICT_NOT_FOUND` or :c:enumerator:`AWL_DICT_ERR_INVALID`. */
int32_t awl_dict_del(awl_dict_t *d, const char *key);

/** Number of live entries (0 for NULL). */
uint64_t awl_dict_len(const awl_dict_t *d);

/** Iterate. Start with ``*cursor = 0`` and call until it returns :c:enumerator:`AWL_DICT_NOT_FOUND`::
 *
 *   uint64_t c = 0; const char *k; awl_dict_val_t v;
 *   while (awl_dict_next(d, &c, &k, &v) == AWL_DICT_OK) { ... }
 *
 * The returned key pointer is owned by the :c:type:`awl_dict_t` and is valid until the
 * entry is removed or the :c:type:`awl_dict_t` is modified. Don't modify the :c:type:`awl_dict_t` while
 * iterating. */
int32_t awl_dict_next(const awl_dict_t *d, uint64_t *cursor, const char **key, awl_dict_val_t *val);

/** iterate, and call ``(*iter)(key, val, ctx)`` for each entry, abort iteration if
 * its return value != :c:enumerator:`AWL_DICT_OK`, and return that value.
 * returns :c:enumerator:`AWL_DICT_OK` on successful iteration of all slots. Holds the lock
 * throughout; iter must not modify the dict. */
int32_t awl_dict_iter(const awl_dict_t *d, int32_t (*iter)(const char* key, const awl_dict_val_t* val, void* ctx), void* ctx);

/** Take the dict's lock (see the top of this file); calls nest.
 * NULL is a no-op. */
void awl_dict_lock(const awl_dict_t *d);
/** Release it */
void awl_dict_unlock(const awl_dict_t *d);
