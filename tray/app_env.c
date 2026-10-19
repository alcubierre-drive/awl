/* The tray's icon theme for programs awl starts that ask for it
 * (awl_tray_app_env()): a dconf profile of their own, the one they would
 * have had otherwise plus a database that sets and locks
 * org.gnome.desktop.interface's icon-theme. A lock in a later database wins
 * over the values of the earlier ones, the user's included, so for them
 * that one key is the tray's and everything else stays as it is; the
 * desktop's setting isn't touched. GTK reads it from there (on Wayland,
 * without GTK_USE_PORTAL=1); a theme it lacks an icon in falls back as its
 * index.theme says, then to hicolor.
 *
 * Nothing goes to a file system: the profile and the database are sealed
 * in-memory files (memfd), close-on-exec in awl, which the program inherits
 * and opens as /proc/self/fd/N; they are gone once the last one holding
 * them exits. libawltray-env.so, preloaded into the program, keeps it from
 * handing them, and the variables, on to its children. Main thread only. */

#define _GNU_SOURCE /* memfd_create(), dladdr() */
#include "awl_tray.h"
#include "app_env.h"
#include "../awl_log.h"

#include <glib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define KEY "/org/gnome/desktop/interface/icon-theme"
#define SHIM "libawltray-env.so"

static struct {
    /** the config's icon_theme, NULL: none */
    char* theme;
    /** what awl_tray_app_env() made, for theme made_for (NULL: nothing yet);
     * env.env NULL: nothing to give */
    char* made_for;
    awl_tray_app_env_t env;
    char* vars[5];
} E = { .env.fds = { -1, -1 } };

void awl_tray_app_set_theme( const char* theme ) {
    g_free( E.theme );
    E.theme = theme && *theme ? g_strdup( theme ) : NULL;
}

/** forgets what awl_tray_app_env() made */
static void drop( void ) {
    for (int i = 0; i < 2; i++) {
        if (E.env.fds[i] >= 0) close( E.env.fds[i] );
        E.env.fds[i] = -1;
    }
    for (size_t i = 0; i < G_N_ELEMENTS( E.vars ); i++) {
        g_free( E.vars[i] );
        E.vars[i] = NULL;
    }
    E.env.env = NULL;
    g_free( E.made_for );
    E.made_for = NULL;
}

/** whether an icon theme name is installed where GTK looks for one */
static int installed( const char* name ) {
    char* dirs[] = { g_build_filename( g_get_user_data_dir(), "icons", NULL ),
                     g_build_filename( g_get_home_dir(), ".icons", NULL ) };
    int found = 0;
    for (size_t i = 0; i < G_N_ELEMENTS( dirs ); i++) {
        char* index = g_build_filename( dirs[i], name, "index.theme", NULL );
        found = found || g_file_test( index, G_FILE_TEST_EXISTS );
        g_free( index );
        g_free( dirs[i] );
    }
    for (const char* const* d = g_get_system_data_dirs(); *d && !found; d++) {
        char* index = g_build_filename( *d, "icons", name, "index.theme", NULL );
        found = g_file_test( index, G_FILE_TEST_EXISTS );
        g_free( index );
    }
    return found;
}

/** the dconf profile awl's programs get without us, as dconf finds it:
 * DCONF_PROFILE (a path, or a name), else "user", in /etc/dconf/profile
 * then the data dirs' dconf/profile; just the user's database if there is
 * none */
static char* base_profile( void ) {
    const char* name = g_getenv( "DCONF_PROFILE" );
    char* text = NULL;
    if (name && name[0] == '/') {
        g_file_get_contents( name, &text, NULL, NULL );
    } else {
        if (!name || !*name) name = "user";
        char* p = g_build_filename( "/etc/dconf/profile", name, NULL );
        g_file_get_contents( p, &text, NULL, NULL );
        g_free( p );
        for (const char* const* d = g_get_system_data_dirs(); *d && !text; d++) {
            p = g_build_filename( *d, "dconf", "profile", name, NULL );
            g_file_get_contents( p, &text, NULL, NULL );
            g_free( p );
        }
    }
    if (!text) return g_strdup( "user-db:user\n" );
    if (*text && !g_str_has_suffix( text, "\n" )) {
        char* t = g_strconcat( text, "\n", NULL );
        g_free( text );
        text = t;
    }
    return text;
}

/* ---- the database: GLib's gvdb format (gvdb-format.h), as `dconf compile`
 * writes it, for one key and its lock ---- */

typedef struct {
    guint32 hash, parent, key_start;
    guint16 key_size;
    gchar type, unused;
    guint32 start, end;
} gvdb_item_t;

/** gvdb's key hash (djb) */
static guint32 gvdb_hash( const char* key ) {
    guint32 h = 5381;
    for (; *key; key++) h = h * 33 + (guint32)(signed char)*key;
    return h;
}

static void put32( GByteArray* b, gsize at, guint32 v ) {
    v = GUINT32_TO_LE( v );
    memcpy( b->data + at, &v, 4 );
}

static gsize append( GByteArray* b, const void* data, gsize n, gsize align ) {
    static const guint8 zero[8];
    while (b->len % align) g_byte_array_append( b, zero, 1 );
    gsize at = b->len;
    if (n) g_byte_array_append( b, data, (guint)n );
    return at;
}

/** a hash table of n items at its place, all in one bucket and without a
 * bloom filter: returns where it starts, *end where it ends */
static gsize table( GByteArray* b, int n, gsize* end ) {
    const guint32 header[3] = { 0, GUINT32_TO_LE( 1 ), 0 }; /* no bloom words, 1 bucket starting at item 0 */
    gsize at = append( b, header, sizeof header, 4 );
    gvdb_item_t blank = { 0 };
    for (int i = 0; i < n; i++) append( b, &blank, sizeof blank, 1 );
    *end = b->len;
    return at;
}

static void item( GByteArray* b, gsize table_at, int i, const char* key, gsize key_at, char type, gsize start,
                  gsize end ) {
    gsize at = table_at + 12 + (gsize)i * sizeof( gvdb_item_t );
    put32( b, at, gvdb_hash( key ) );
    put32( b, at + 4, 0xffffffffu ); /* no parent: the whole key is its own */
    put32( b, at + 8, (guint32)key_at );
    guint16 size = GUINT16_TO_LE( (guint16)strlen( key ) );
    memcpy( b->data + at + 12, &size, 2 );
    b->data[at + 14] = (guint8)type;
    put32( b, at + 16, (guint32)start );
    put32( b, at + 20, (guint32)end );
}

static gsize value( GByteArray* b, GVariant* v, gsize* end ) {
    GVariant* boxed = g_variant_ref_sink( g_variant_new_variant( v ) );
    gsize at = append( b, g_variant_get_data( boxed ), g_variant_get_size( boxed ), 8 );
    *end = b->len;
    g_variant_unref( boxed );
    return at;
}

/** KEY set to theme, and locked: the root table holds it and .locks, a
 * table with the lock */
static GByteArray* database( const char* theme ) {
    GByteArray* b = g_byte_array_new();
    const guint32 header[6] = { GUINT32_TO_LE( 0x72615647 ), GUINT32_TO_LE( 0x746e6169 ) }; /* "GVariant", version 0 */
    append( b, header, sizeof header, 1 );
    gsize root_end, locks_end, theme_end, lock_end;
    gsize root = table( b, 2, &root_end );
    gsize locks = table( b, 1, &locks_end );
    gsize locks_key = append( b, ".locks", 6, 1 );
    gsize key = append( b, KEY, strlen( KEY ), 1 );
    gsize theme_at = value( b, g_variant_new_string( theme ), &theme_end );
    gsize lock_at = value( b, g_variant_new_string( "" ), &lock_end );
    put32( b, 16, (guint32)root );
    put32( b, 20, (guint32)root_end );
    item( b, root, 0, ".locks", locks_key, 'H', locks, locks_end );
    item( b, root, 1, KEY, key, 'v', theme_at, theme_end );
    item( b, locks, 0, KEY, key, 'v', lock_at, lock_end );
    return b;
}

/** a sealed in-memory file named name holding data, close-on-exec; -1 on
 * failure */
static int memfile( const char* name, const void* data, gsize n ) {
    int fd = memfd_create( name, MFD_CLOEXEC | MFD_ALLOW_SEALING );
    if (fd < 0) {
        awl_err_printf( "tray: memfd_create: %s\n", g_strerror( errno ) );
        return -1;
    }
    const guint8* p = data;
    for (gsize left = n; left;) {
        ssize_t w = write( fd, p + (n - left), left );
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            awl_err_printf( "tray: writing %s: %s\n", name, g_strerror( errno ) );
            close( fd );
            return -1;
        }
        left -= (gsize)w;
    }
    fcntl( fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL );
    return fd;
}

/** the shim, next to this library; NULL if it isn't there */
static char* shim( void ) {
    Dl_info info;
    /* any of this library's objects tells which it is */
    if (!dladdr( &E, &info ) || !info.dli_fname) return NULL;
    char* dir = g_path_get_dirname( info.dli_fname );
    char* rel = g_build_filename( dir, SHIM, NULL );
    char* path = g_canonicalize_filename( rel, NULL );
    g_free( rel );
    g_free( dir );
    if (access( path, R_OK )) {
        g_free( path );
        return NULL;
    }
    return path;
}

/** the profile, the database and the variables for E.theme */
static int make( void ) {
    char* lib = shim();
    if (!lib) {
        awl_wrn_printf( "tray: no " SHIM " next to libawltray.so, programs keep the desktop's icon theme\n" );
        return 0;
    }
    GByteArray* db = database( E.theme );
    E.env.fds[1] = memfile( "awl-tray-dconf-db", db->data, db->len );
    g_byte_array_unref( db );
    if (E.env.fds[1] >= 0) {
        char* base = base_profile();
        char* text = g_strdup_printf( "%sfile-db:/proc/self/fd/%d\n", base, E.env.fds[1] );
        E.env.fds[0] = memfile( "awl-tray-dconf-profile", text, strlen( text ) );
        g_free( text );
        g_free( base );
    }
    if (E.env.fds[0] < 0 || E.env.fds[1] < 0) {
        g_free( lib );
        return 0;
    }
    /* ours first, then what awl was started with */
    const char* preload = g_getenv( "LD_PRELOAD" );
    /* awl's own, for the shim to put back */
    const char* was = g_getenv( "DCONF_PROFILE" );
    E.vars[0] = g_strdup_printf( "DCONF_PROFILE=/proc/self/fd/%d", E.env.fds[0] );
    E.vars[1] = g_strdup_printf( "AWL_TRAY_ENV_FDS=%d,%d", E.env.fds[0], E.env.fds[1] );
    E.vars[2] = preload && *preload ? g_strdup_printf( "LD_PRELOAD=%s:%s", lib, preload )
                                    : g_strdup_printf( "LD_PRELOAD=%s", lib );
    if (was) E.vars[3] = g_strdup_printf( "AWL_TRAY_DCONF_PROFILE=%s", was );
    E.env.env = (const char* const*)E.vars;
    g_free( lib );
    return 1;
}

const awl_tray_app_env_t* awl_tray_app_env( void ) {
    if (!E.theme) return NULL;
    if (E.made_for && !strcmp( E.made_for, E.theme )) return E.env.env ? &E.env : NULL;
    drop();
    E.made_for = g_strdup( E.theme );
    if (!installed( E.theme )) {
        awl_wrn_printf( "tray: icon theme %s isn't installed, programs keep the desktop's\n", E.theme );
        return NULL;
    }
    if (!make()) {
        drop();
        E.made_for = g_strdup( E.theme );
        return NULL;
    }
    awl_log_printf( "tray: icon theme %s for the programs that ask\n", E.theme );
    return &E.env;
}

void awl_tray_app_cleanup( void ) {
    drop();
    g_free( E.theme );
    E.theme = NULL;
}
