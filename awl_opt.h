/* awl_opt.h - values a config may leave unset, filled in once from what they default to. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * A setting that may be left unset, as all-zero (static or calloc'd) is:
 * then it takes what its documentation says, filled in once
 * (:c:func:`awl_config_fill_defaults`, :c:func:`awl_plugin_config_fill_defaults`,
 * a notification's look when it's shown), so whoever reads it after that
 * reads v. Set one with :c:macro:`AWL_SET`, or :c:macro:`AWL_SOME` in an
 * initializer.
 */
typedef struct awl_opt_int_t {
    int v;
    bool set;
} awl_opt_int_t;

/** a 0xRRGGBBAA color, as :c:type:`awl_opt_int_t`; any color, 0 (transparent) too */
typedef struct awl_opt_color_t {
    uint32_t v;
    bool set;
} awl_opt_color_t;

/** an initializer setting an :c:type:`awl_opt_int_t` or :c:type:`awl_opt_color_t` to x */
#define AWL_SOME( x ) { .v = (x), .set = true }
/** sets the option o to x */
#define AWL_SET( o, x ) ((o) = (__typeof__( o )){ .v = (x), .set = true })
/** o's value if it is set, else dflt */
#define AWL_OR( o, dflt ) ((o).set ? (o).v : (dflt))
/** sets o to dflt unless it is set */
#define AWL_FILL( o, dflt ) \
    do { \
        if (!(o).set) AWL_SET( o, dflt ); \
    } while (0)
