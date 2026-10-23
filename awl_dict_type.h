/* awl_dict_type.h - minimal string -> tagged value hash map with a stable, portable C ABI. */
#pragma once

#include <stdint.h>

/** a string -> tagged value hash map (awl_dict.h); opaque */
typedef struct awl_dict_t awl_dict_t;

/** what the functions return */
enum {
    /** done, found */
    AWL_DICT_OK = 0,
    /** no such key, or the iteration is over */
    AWL_DICT_NOT_FOUND = 1,
    /** a NULL dict or key, a bad value */
    AWL_DICT_ERR_INVALID = -2,
    /** out of memory */
    AWL_DICT_ERR_NOMEM = -1,
};

/** what an :c:type:`awl_dict_val_t` holds */
enum {
    /** ptr: a plain pointer, never touched by the dict */
    AWL_DICT_KIND_PTR = 0,
    /** str: a string; set copies it, the dict owns the copy */
    AWL_DICT_KIND_STR = 1,
    /** owned: ptr plus the function the dict frees it with when the entry is
     * overwritten or removed, or the dict destroyed */
    AWL_DICT_KIND_OWNED = 2,
    /** num: a plain integer, passed explicitly by value */
    AWL_DICT_KIND_NUM = 3,
    /** frac: a plain double, passed explicitly by value */
    AWL_DICT_KIND_FRAC = 4,
};

/** a value: kind says which member of the union is valid */
typedef struct {
    int32_t kind;
    union {
        void* ptr;
        char* str;
        struct {
            void* ptr;
            void (*free)(void*);
        } owned;
        int64_t num;
        double frac;
    };
} awl_dict_val_t;

/** :c:type:`awl_dict_val_t` literals, e.g. ``awl_dict_set(d, "k", &AWL_DICT_STR("v"))`` */
#define AWL_DICT_PTR(p) ((awl_dict_val_t){ .kind = AWL_DICT_KIND_PTR, .ptr = (p) })
#define AWL_DICT_STR(s) ((awl_dict_val_t){ .kind = AWL_DICT_KIND_STR, .str = (char*)(s) })
#define AWL_DICT_OWNED(p, f) \
    ((awl_dict_val_t){ .kind = AWL_DICT_KIND_OWNED, .owned = { .ptr = (p), .free = (f) } })
#define AWL_DICT_NUM(n) ((awl_dict_val_t){ .kind = AWL_DICT_KIND_NUM, .num = (n) })
#define AWL_DICT_FRAC(f) ((awl_dict_val_t){ .kind = AWL_DICT_KIND_FRAC, .frac = (f) })

/** wrap to a 'class' to make it actually ABI-stable and usable in a host/client
 * data+function table setup. See awl_dict.h for the API, and there for
 * threads: every call is atomic on its own, s_lock/s_unlock group several */
typedef struct {
    awl_dict_t* s;
    awl_dict_t* (*s_new)(void);
    void (*s_destroy)(awl_dict_t*);
    int32_t (*s_set)(awl_dict_t*, const char*, const awl_dict_val_t*);
    int32_t (*s_get)(const awl_dict_t*, const char*, awl_dict_val_t*);
    int32_t (*s_del)(awl_dict_t*, const char*);
    uint64_t (*s_len)(const awl_dict_t*);
    int32_t (*s_next)(const awl_dict_t*, uint64_t*, const char**, awl_dict_val_t*);
    int32_t (*s_iter)(const awl_dict_t*, int32_t (*)(const char*, const awl_dict_val_t*, void*), void*);
    void (*s_lock)(const awl_dict_t*);
    void (*s_unlock)(const awl_dict_t*);
} awl_dict_c;
