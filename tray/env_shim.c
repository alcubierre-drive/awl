/* libawltray-env.so: preloaded into a program awl starts with TRAY_ICONS
 * (app_env.c), so that only it, not what it starts in turn, gets the
 * tray's icon theme. Before anything else in it runs, it takes what awl
 * gave it out of its environment: DCONF_PROFILE, which it keeps for dconf
 * (answering dconf's g_getenv() for it) and replaces by awl's own,
 * AWL_TRAY_DCONF_PROFILE, if awl had one, itself out of LD_PRELOAD, and
 * AWL_TRAY_ENV_FDS, the in-memory files the profile is in, which it makes
 * close-on-exec. Its children then get neither the variables nor the
 * files. Only for programs linked to GLib dynamically, as GTK ones are; in
 * others it just cleans up. Needs nothing but libc. */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

/** awl's DCONF_PROFILE, NULL: none */
static char* profile;

/** LD_PRELOAD without this library (whatever else was in it stays) */
static void drop_self( void ) {
    const char* v = getenv( "LD_PRELOAD" );
    Dl_info info;
    if (!v || !dladdr( &profile, &info ) || !info.dli_fname) return;
    const char* self = info.dli_fname;
    const char* base = strrchr( self, '/' );
    base = base ? base + 1 : self;
    char *copy = strdup( v ), *out = calloc( 1, strlen( v ) + 1 ), *save = NULL;
    if (!copy || !out) goto done;
    /* ld.so separates them by colons or spaces */
    for (char* t = strtok_r( copy, ": ", &save ); t; t = strtok_r( NULL, ": ", &save )) {
        const char* tb = strrchr( t, '/' );
        if (!strcmp( t, self ) || !strcmp( tb ? tb + 1 : t, base )) continue;
        if (*out) strcat( out, ":" );
        strcat( out, t );
    }
    if (*out) setenv( "LD_PRELOAD", out, 1 );
    else unsetenv( "LD_PRELOAD" );
done:
    free( copy );
    free( out );
}

/** AWL_TRAY_ENV_FDS' descriptors close-on-exec */
static void keep_fds( void ) {
    const char* v = getenv( "AWL_TRAY_ENV_FDS" );
    for (char* end; v && *v; v = *end ? end + 1 : end) {
        long fd = strtol( v, &end, 10 );
        if (end == v) break;
        if (fd > 2) {
            int flags = fcntl( (int)fd, F_GETFD );
            if (flags >= 0) fcntl( (int)fd, F_SETFD, flags | FD_CLOEXEC );
        }
    }
    unsetenv( "AWL_TRAY_ENV_FDS" );
}

/* the program's still single threaded: changing the environment is safe */
__attribute__( (constructor) ) static void init( void ) {
    const char* p = getenv( "DCONF_PROFILE" );
    if (p) profile = strdup( p );
    const char* was = getenv( "AWL_TRAY_DCONF_PROFILE" );
    if (was) setenv( "DCONF_PROFILE", was, 1 );
    else unsetenv( "DCONF_PROFILE" );
    unsetenv( "AWL_TRAY_DCONF_PROFILE" );
    keep_fds();
    drop_self();
}

/* GLib's, which dconf asks for DCONF_PROFILE */
const char* g_getenv( const char* name ) {
    static const char* (*real)( const char* );
    if (profile && name && !strcmp( name, "DCONF_PROFILE" )) return profile;
    const char* (*fn)( const char* ) = __atomic_load_n( &real, __ATOMIC_ACQUIRE );
    if (!fn) {
        void* sym = dlsym( RTLD_NEXT, "g_getenv" );
        if (!sym) return getenv( name );
        memcpy( &fn, &sym, sizeof fn );
        __atomic_store_n( &real, fn, __ATOMIC_RELEASE );
    }
    return fn( name );
}
