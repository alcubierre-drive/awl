/* awl_dict.c - implementation of awl_dict.h (open addressing, linear probing). */
#include "awl_dict.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;      /* NULL = empty slot, TOMBSTONE = deleted slot */
    awl_dict_val_t val;
} entry;

struct awl_dict_t {
    entry *slots;
    size_t cap;     /* always a power of two */
    size_t len;     /* live entries */
    size_t used;    /* live entries + tombstones */
    pthread_mutex_t lock;   /* recursive, see awl_dict.h */
};

/* the lock is no part of the dict's value, so the const functions take it */
#define LOCK(d) pthread_mutex_lock(&((awl_dict_t *)(d))->lock)
#define UNLOCK(d) pthread_mutex_unlock(&((awl_dict_t *)(d))->lock)

/* a deleted entry's key: only its address counts, nothing writes to it */
static const char tombstone_marker;
#define TOMBSTONE ((char *)&tombstone_marker)
#define LIVE(e) ((e)->key && (e)->key != TOMBSTONE)

/* FNV-1a */
static uint64_t hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Returns slot holding key, or the best empty/tombstone slot to insert into. */
static entry *find(entry *slots, size_t cap, const char *key) {
    size_t i = (size_t)(hash(key) & (cap - 1));
    entry *tomb = NULL;
    for (;;) {
        entry *e = &slots[i];
        if (!e->key) return tomb ? tomb : e;
        if (e->key == TOMBSTONE) { if (!tomb) tomb = e; }
        else if (strcmp(e->key, key) == 0) return e;
        i = (i + 1) & (cap - 1);
    }
}

/* frees what the dict owns of v */
static void release(const awl_dict_val_t *v) {
    switch (v->kind) {
    case AWL_DICT_KIND_STR: free(v->str); break;
    case AWL_DICT_KIND_OWNED: v->owned.free(v->owned.ptr); break;
    }
}

static int grow(awl_dict_t *d) {
    /* Double if genuinely full; otherwise just rehash to clear tombstones. */
    size_t ncap = d->len * 4 >= d->cap ? d->cap * 2 : d->cap;
    entry *ns = calloc(ncap, sizeof(entry));
    if (!ns) return -1;
    for (size_t i = 0; i < d->cap; i++)
        if (LIVE(&d->slots[i])) *find(ns, ncap, d->slots[i].key) = d->slots[i];
    free(d->slots);
    d->slots = ns; d->cap = ncap; d->used = d->len;
    return 0;
}

awl_dict_t *awl_dict_new(void) {
    awl_dict_t *d = malloc(sizeof *d);
    if (!d) return NULL;
    d->cap = 8; d->len = d->used = 0;
    d->slots = calloc(d->cap, sizeof(entry));
    if (!d->slots) { free(d); return NULL; }
    pthread_mutexattr_t attr;
    int err = pthread_mutexattr_init(&attr);
    if (!err) {
        err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
              pthread_mutex_init(&d->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    if (err) { free(d->slots); free(d); return NULL; }
    return d;
}

void awl_dict_destroy(awl_dict_t *d) {
    if (!d) return;
    for (size_t i = 0; i < d->cap; i++)
        if (LIVE(&d->slots[i])) { free(d->slots[i].key); release(&d->slots[i].val); }
    free(d->slots);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

static int32_t set(awl_dict_t *d, const char *key, const awl_dict_val_t *val) {
    awl_dict_val_t v = *val;
    switch (v.kind) {
    case AWL_DICT_KIND_PTR: break;
    case AWL_DICT_KIND_STR: if (!v.str) return AWL_DICT_ERR_INVALID; break;
    case AWL_DICT_KIND_OWNED: if (!v.owned.free) return AWL_DICT_ERR_INVALID; break;
    case AWL_DICT_KIND_NUM: break;
    case AWL_DICT_KIND_FRAC: break;
    default: return AWL_DICT_ERR_INVALID;
    }
    if ((d->used + 1) * 4 > d->cap * 3 && grow(d) < 0) return AWL_DICT_ERR_NOMEM;
    /* copied before the old value goes, which v.str may point into */
    if (v.kind == AWL_DICT_KIND_STR && !(v.str = dupstr(v.str))) return AWL_DICT_ERR_NOMEM;
    entry *e = find(d->slots, d->cap, key);
    if (LIVE(e)) {
        if (!(v.kind == AWL_DICT_KIND_OWNED && e->val.kind == AWL_DICT_KIND_OWNED &&
              v.owned.ptr == e->val.owned.ptr))
            release(&e->val);
        e->val = v;
        return AWL_DICT_OK;
    }
    char *k = dupstr(key);
    if (!k) {
        if (v.kind == AWL_DICT_KIND_STR) free(v.str);
        return AWL_DICT_ERR_NOMEM;
    }
    if (!e->key) d->used++;          /* reusing a tombstone doesn't add to used */
    e->key = k; e->val = v; d->len++;
    return AWL_DICT_OK;
}

static int32_t get(const awl_dict_t *d, const char *key, awl_dict_val_t *out) {
    entry *e = find(d->slots, d->cap, key);
    if (!LIVE(e)) return AWL_DICT_NOT_FOUND;
    if (out) *out = e->val;
    return AWL_DICT_OK;
}

static int32_t del(awl_dict_t *d, const char *key) {
    entry *e = find(d->slots, d->cap, key);
    if (!LIVE(e)) return AWL_DICT_NOT_FOUND;
    free(e->key);
    release(&e->val);
    e->key = TOMBSTONE; e->val = AWL_DICT_PTR(NULL); d->len--;
    return AWL_DICT_OK;
}

uint64_t awl_dict_len(const awl_dict_t *d) {
    if (!d) return 0;
    LOCK(d);
    uint64_t n = (uint64_t)d->len;
    UNLOCK(d);
    return n;
}

static int32_t next(const awl_dict_t *d, uint64_t *cursor, const char **key, awl_dict_val_t *val) {
    for (uint64_t i = *cursor; i < (uint64_t)d->cap; i++) {
        entry *e = &d->slots[i];
        if (LIVE(e)) {
            if (key) *key = e->key;
            if (val) *val = e->val;
            *cursor = i + 1;
            return AWL_DICT_OK;
        }
    }
    *cursor = (uint64_t)d->cap;
    return AWL_DICT_NOT_FOUND;
}

static int32_t iterate(const awl_dict_t *d, int32_t (*iter)(const char* key, const awl_dict_val_t* val, void* ctx), void* ctx) {
    for (uint64_t i=0; i<d->cap; i++) {
        entry *e = &d->slots[i];
        if (LIVE(e)) {
            int32_t retval = (*iter)(e->key, &e->val, ctx);
            if (retval != AWL_DICT_OK) return retval;
        }
    }
    return AWL_DICT_OK;
}

/* the public functions: check the arguments, then do it under the lock */

int32_t awl_dict_set(awl_dict_t *d, const char *key, const awl_dict_val_t *val) {
    if (!d || !key || !val) return AWL_DICT_ERR_INVALID;
    LOCK(d);
    int32_t r = set(d, key, val);
    UNLOCK(d);
    return r;
}

int32_t awl_dict_get(const awl_dict_t *d, const char *key, awl_dict_val_t *out) {
    if (!d || !key) return AWL_DICT_ERR_INVALID;
    LOCK(d);
    int32_t r = get(d, key, out);
    UNLOCK(d);
    return r;
}

int32_t awl_dict_del(awl_dict_t *d, const char *key) {
    if (!d || !key) return AWL_DICT_ERR_INVALID;
    LOCK(d);
    int32_t r = del(d, key);
    UNLOCK(d);
    return r;
}

int32_t awl_dict_next(const awl_dict_t *d, uint64_t *cursor, const char **key, awl_dict_val_t *val) {
    if (!d || !cursor) return AWL_DICT_ERR_INVALID;
    LOCK(d);
    int32_t r = next(d, cursor, key, val);
    UNLOCK(d);
    return r;
}

int32_t awl_dict_iter(const awl_dict_t *d, int32_t (*iter)(const char* key, const awl_dict_val_t* val, void* ctx), void* ctx) {
    if (!d || iter == NULL) return AWL_DICT_ERR_INVALID;
    LOCK(d);
    int32_t r = iterate(d, iter, ctx);
    UNLOCK(d);
    return r;
}

void awl_dict_lock(const awl_dict_t *d) {
    if (d) LOCK(d);
}

void awl_dict_unlock(const awl_dict_t *d) {
    if (d) UNLOCK(d);
}
