/* The launcher, see launcher.h: the index thread first, then the main
 * thread's window. */

#define _DEFAULT_SOURCE /* d_type */
#include "launcher.h"
#include "desktop_entry.h"
#include "fuzzy.h"
#include "markup.h"
#include "thread.h"
#include "../plugins.h"
#include "../icons/icon.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define CFG (&la->p->plugin_config.launcher)
#define COLORS (&la->p->plugin_config.colors)

/** the launcher's limits and times */
enum launcher_limits {
    /** after an inotify event, wait this long for more before scanning */
    SETTLE_MS = 200,
    /** checks this often whether a directory that was missing is there */
    REWATCH_MS = 30000,
    /** subdirectories of applications/ this deep count */
    MAX_DEPTH = 4,
    /** .desktop files larger than this aren't ours */
    MAX_FILE = 256 * 1024,
    /** the main thread looks for icons this often while it waits for them */
    ICON_POLL_MS = 30,
    /** icons kept decoded, the oldest dropped beyond */
    MAX_ICONS = 256,
    /** results shown at most in the bar layout */
    MAX_SHOWN = 64,
    /** command lines the run mode remembers, the least used forgotten */
    MAX_HISTORY = 500,
};

/* ---- the index, made on the thread, read on the main thread ---- */

/** an application: its desktop entry and how often it was launched */
typedef struct {
    /** the desktop file id ("org.gnome.Nautilus.desktop"), its file */
    char *id, *file;
    awl_desktop_entry_t e;
    /** the command's name, argv[0] without its directory, to match */
    char* command;
    /** launches, weighted by how recent */
    double frecency;
} entry_t;

/** the run mode's: an executable in $PATH or a command line run before */
typedef struct {
    char* line;
    double frecency;
} run_t;

/** what the launcher matches against: the applications and the run mode's
 * lines */
typedef struct {
    entry_t* v;
    int n;
    run_t* run;
    int nrun;
} index_t;

static void index_free( index_t* idx ) {
    if (!idx) return;
    for (int i = 0; i < idx->n; i++) {
        free( idx->v[i].id );
        free( idx->v[i].file );
        free( idx->v[i].command );
        awl_desktop_entry_clear( &idx->v[i].e );
    }
    for (int i = 0; i < idx->nrun; i++) free( idx->run[i].line );
    free( idx->v );
    free( idx->run );
    free( idx );
}

/** main -> thread: an icon wanted, an id launched or a command line run;
 * thread -> main: an icon (NULL if there is none) */
typedef struct msg_t {
    struct msg_t* next;
    char* name;
    int px;
    pixman_image_t* img;
} msg_t;

static void msgs_free( msg_t* m ) {
    for (msg_t* next; m; m = next) {
        next = m->next;
        free( m->name );
        if (m->img) pixman_image_unref( m->img );
        free( m );
    }
}

/** Shared with the thread. Like the desktop panel's Scanner, a thread
 * stuck in the kernel is left behind with this, never freed. */
typedef struct {
    awl_thread_t thread;
    /** set before the thread starts, then read-only */
    char* icon_theme, * state_home;
    /** thread -> main: a new index, NULL if none; handed over whole */
    _Atomic(index_t*) pending;
    pthread_mutex_t lock;
    /** under lock: icons wanted, ids launched and lines run (main ->
     * thread), icons found (thread -> main) */
    msg_t *icons_wanted, *launched, *ran, *icons_found;
} indexer_t;


/* ---- the thread: launch counts ---- */

/** an application's or a command line's launches */
typedef struct {
    char* id;
    long count;
    long long last;
    /** :c:func:`counts_add`'s sort key: the frecency at its now */
    double key;
} count_t;

/** the launch counts, as kept in the state file */
typedef struct {
    count_t* v;
    int n, cap;
} counts_t;

static count_t* count_of( counts_t* c, const char* id ) {
    for (int i = 0; i < c->n; i++)
        if (!strcmp( c->v[i].id, id )) return &c->v[i];
    return NULL;
}

static count_t* count_add( counts_t* c, const char* id ) {
    count_t* k = count_of( c, id );
    if (k) return k;
    if (c->n == c->cap) {
        int cap = c->cap ? 2 * c->cap : 32;
        count_t* v = realloc( c->v, (size_t)cap * sizeof(*v) );
        if (!v) return NULL;
        c->v = v;
        c->cap = cap;
    }
    char* s = strdup( id );
    if (!s) return NULL;
    c->v[c->n] = (count_t){ .id = s };
    return &c->v[c->n++];
}

/** launches, weighted by how long ago the last was */
static double frecency( const count_t* k, long long now ) {
    if (!k || k->count <= 0) return 0;
    const long long age = now - k->last;
    const double w = age < 86400 ? 4 : age < 7 * 86400 ? 2 : age < 30 * 86400 ? 1 : 0.5;
    return (double)k->count * w;
}

/** the file of the counts called name: "launcher" the applications', "run"
 * the run mode's */
static void counts_path( const indexer_t* x, const char* name, char* path, size_t n ) {
    snprintf( path, n, "%s/awl/%s", x->state_home ? x->state_home : "", name );
}

/** "count last id" lines */
static void counts_load( const indexer_t* x, const char* name, counts_t* c ) {
    char path[PATH_MAX], line[1024];
    if (!x->state_home) return;
    counts_path( x, name, path, sizeof path );
    FILE* f = fopen( path, "re" );
    if (!f) return;
    while (fgets( line, sizeof line, f )) {
        long count;
        long long last;
        int off = 0;
        if (sscanf( line, "%ld %lld %n", &count, &last, &off ) < 2 || !off) continue;
        line[strcspn( line, "\n" )] = '\0';
        count_t* k = line[off] ? count_add( c, line + off ) : NULL;
        if (k) {
            k->count = count;
            k->last = last;
        }
    }
    fclose( f );
}

/** mkdir -p of path's directory */
static void mkdirs( char* path ) {
    for (char* p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir( path, 0700 );
        *p = '/';
    }
}

/** the whole file anew, renamed over the old */
static void counts_save( const indexer_t* x, const char* name, const counts_t* c ) {
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    if (!x->state_home) return;
    counts_path( x, name, path, sizeof path );
    snprintf( tmp, sizeof tmp, "%s.tmp", path );
    mkdirs( tmp );
    FILE* f = fopen( tmp, "we" );
    if (!f) {
        awl_err_printf( "launcher: can't write %s: %s\n", tmp, strerror( errno ) );
        return;
    }
    for (int i = 0; i < c->n; i++) fprintf( f, "%ld %lld %s\n", c->v[i].count, c->v[i].last, c->v[i].id );
    if (fclose( f ) || rename( tmp, path )) {
        awl_err_printf( "launcher: can't write %s: %s\n", path, strerror( errno ) );
        unlink( tmp );
    }
}

/* ---- the thread: scanning ---- */

/** the applications directories, in precedence order; ``*n`` how many */
static char** app_dirs( int* n ) {
    char** dirs = NULL;
    *n = 0;
    const char* home = getenv( "HOME" );
    const char* data_home = getenv( "XDG_DATA_HOME" );
    const char* data_dirs = getenv( "XDG_DATA_DIRS" );
    char buf[PATH_MAX];
    if (!data_dirs || !*data_dirs) data_dirs = "/usr/local/share:/usr/share";
    if (data_home && *data_home) snprintf( buf, sizeof buf, "%s/applications", data_home );
    else if (home) snprintf( buf, sizeof buf, "%s/.local/share/applications", home );
    else buf[0] = '\0';
    size_t cap = 2;
    for (const char* p = data_dirs; *p; p++) cap += *p == ':';
    if (!(dirs = calloc( cap, sizeof(*dirs) ))) return NULL;
    if (buf[0]) dirs[(*n)++] = strdup( buf );
    for (const char* p = data_dirs; *p;) {
        size_t k = strcspn( p, ":" );
        if (k && k < sizeof buf - 16) {
            snprintf( buf, sizeof buf, "%.*s/applications", (int)k, p );
            dirs[(*n)++] = strdup( buf );
        }
        p += k + (p[k] == ':');
    }
    return dirs;
}

/** a file found: its id and path, and the order it was found in */
typedef struct {
    char *id, *path;
    int order;
} found_t;

/** a scan's findings */
typedef struct {
    found_t* v;
    int n, cap;
    /** inotify, its watches on every directory read */
    int ifd;
} scan_t;

static void found_add( scan_t* s, const char* id, const char* path ) {
    if (s->n == s->cap) {
        int cap = s->cap ? 2 * s->cap : 256;
        found_t* v = realloc( s->v, (size_t)cap * sizeof(*v) );
        if (!v) return;
        s->v = v;
        s->cap = cap;
    }
    found_t f = { strdup( id ), strdup( path ), s->n };
    if (!f.id || !f.path) {
        free( f.id );
        free( f.path );
        return;
    }
    s->v[s->n++] = f;
}

/** the .desktop files under dir, their ids prefix-ed (a subdirectory's
 * name and '-' each) */
static void scan_dir( scan_t* s, const char* dir, const char* prefix, int depth ) {
    DIR* d = opendir( dir );
    if (!d) return;
    if (s->ifd >= 0)
        inotify_add_watch( s->ifd, dir, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
                                        IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR );
    struct dirent* de;
    char path[PATH_MAX], id[PATH_MAX];
    while ((de = readdir( d ))) {
        if (de->d_name[0] == '.') continue;
        if (snprintf( path, sizeof path, "%s/%s", dir, de->d_name ) >= (int)sizeof path) continue;
        const size_t len = strlen( de->d_name );
        int isdir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
            struct stat st;
            isdir = !stat( path, &st ) && S_ISDIR( st.st_mode );
        }
        if (isdir) {
            if (depth < MAX_DEPTH && snprintf( id, sizeof id, "%s%s-", prefix, de->d_name ) < (int)sizeof id)
                scan_dir( s, path, id, depth + 1 );
        } else if (len > 8 && !strcmp( de->d_name + len - 8, ".desktop" ) &&
                   snprintf( id, sizeof id, "%s%s", prefix, de->d_name ) < (int)sizeof id) {
            found_add( s, id, path );
        }
    }
    closedir( d );
}

static int found_cmp( const void* a, const void* b ) {
    const found_t *x = a, *y = b;
    int c = strcmp( x->id, y->id );
    return c ? c : x->order - y->order;
}

/** a file's text, NUL terminated, or NULL */
static char* slurp( const char* path ) {
    FILE* f = fopen( path, "re" );
    if (!f) return NULL;
    char* buf = malloc( MAX_FILE + 1 );
    size_t n = buf ? fread( buf, 1, MAX_FILE + 1, f ) : 0;
    fclose( f );
    if (!buf || n > MAX_FILE) {
        free( buf );
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/** whether TryExec's program is there: a path, or a name in $PATH */
static int try_exec( const char* prog ) {
    if (strchr( prog, '/' )) return !access( prog, X_OK );
    const char* p = getenv( "PATH" );
    char path[PATH_MAX];
    if (!p) p = "/usr/local/bin:/usr/bin:/bin";
    while (*p) {
        size_t k = strcspn( p, ":" );
        if (snprintf( path, sizeof path, "%.*s/%s", (int)k, k ? p : ".", prog ) < (int)sizeof path &&
            !access( path, X_OK ))
            return 1;
        p += k + (p[k] == ':');
    }
    return 0;
}

/** the command's name: Exec's first argument without its directory */
static char* command_of( const awl_desktop_entry_t* e ) {
    char** argv = awl_desktop_exec_argv( e->exec, e->name, NULL, NULL );
    char* name = NULL;
    if (argv) {
        const char* slash = strrchr( argv[0], '/' );
        name = strdup( slash ? slash + 1 : argv[0] );
    }
    awl_desktop_argv_free( argv );
    return name;
}

/** the desktops of $XDG_CURRENT_DESKTOP, NULL terminated, in buf */
static void desktops_of( char* buf, size_t n, const char** out, int max ) {
    const char* env = getenv( "XDG_CURRENT_DESKTOP" );
    int k = 0;
    snprintf( buf, n, "%s", env ? env : "" );
    for (char *save = NULL, *t = strtok_r( buf, ":", &save ); t && k < max - 1; t = strtok_r( NULL, ":", &save ))
        out[k++] = t;
    out[k] = NULL;
}

/** Reads every directory into a new index, watching them on ifd (-1:
 * none); ``*present`` gets which of them exist, a bit each. */
static index_t* scan( const counts_t* counts, int ifd, unsigned* present ) {
    char langs[AWL_DESKTOP_MAX_LANGS][64], dbuf[256];
    const char* desktops[16];
    const char* locale = getenv( "LC_ALL" );
    if (!locale || !*locale) locale = getenv( "LC_MESSAGES" );
    if (!locale || !*locale) locale = getenv( "LANG" );
    const int nlangs = awl_desktop_langs( locale, langs );
    desktops_of( dbuf, sizeof dbuf, desktops, 16 );

    scan_t s = { .ifd = ifd };
    int ndirs = 0;
    char** dirs = app_dirs( &ndirs );
    *present = 0;
    for (int i = 0; i < ndirs; i++) {
        struct stat st;
        if (i < 32 && dirs[i] && !stat( dirs[i], &st ) && S_ISDIR( st.st_mode )) *present |= 1u << i;
        if (dirs[i]) scan_dir( &s, dirs[i], "", 0 );
        free( dirs[i] );
    }
    free( dirs );
    /* the first of each id, in the directories' order, wins */
    qsort( s.v, (size_t)s.n, sizeof(*s.v), found_cmp );

    index_t* idx = calloc( 1, sizeof(*idx) );
    if (idx) idx->v = calloc( (size_t)(s.n ? s.n : 1), sizeof(*idx->v) );
    const long long now = time( NULL );
    for (int i = 0; i < s.n; i++) {
        const found_t* f = &s.v[i];
        if (!idx || !idx->v || (i && !strcmp( f->id, s.v[i - 1].id ))) continue;
        char* text = slurp( f->path );
        entry_t* en = &idx->v[idx->n];
        if (!text || awl_desktop_entry_parse( &en->e, text, langs, nlangs, desktops ) != 1) {
            free( text );
            continue;
        }
        free( text );
        if (en->e.hidden || (en->e.try_exec && !try_exec( en->e.try_exec ))) {
            awl_desktop_entry_clear( &en->e );
            continue;
        }
        en->id = strdup( f->id );
        en->file = strdup( f->path );
        en->command = command_of( &en->e );
        en->frecency = frecency( en->id ? count_of( (counts_t*)counts, en->id ) : NULL, now );
        if (!en->id || !en->file) {
            free( en->id );
            free( en->file );
            free( en->command );
            awl_desktop_entry_clear( &en->e );
            continue;
        }
        idx->n++;
    }
    for (int i = 0; i < s.n; i++) {
        free( s.v[i].id );
        free( s.v[i].path );
    }
    free( s.v );
    if (idx && !idx->v) {
        free( idx );
        idx = NULL;
    }
    return idx;
}

static int name_cmp( const void* a, const void* b ) {
    return strcmp( *(char* const*)a, *(char* const*)b );
}

static int run_cmp( const void* a, const void* b ) {
    return strcmp( ((const run_t*)a)->line, ((const run_t*)b)->line );
}

/** The run mode's list into idx: the executables in $PATH's directories
 * (watched on ifd), each name once, and the lines in ran that aren't one
 * of them, with their frecencies. */
static void scan_path( index_t* idx, const counts_t* ran, int ifd ) {
    const char* p = getenv( "PATH" );
    char** names = NULL;
    int n = 0, cap = 0;
    char dir[PATH_MAX];
    if (!p) p = "/usr/local/bin:/usr/bin:/bin";
    while (*p) {
        const size_t k = strcspn( p, ":" );
        DIR* d = k && k < sizeof dir ? (snprintf( dir, sizeof dir, "%.*s", (int)k, p ), opendir( dir )) : NULL;
        p += k + (p[k] == ':');
        if (!d) continue;
        if (ifd >= 0)
            inotify_add_watch( ifd, dir, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                         IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR );
        struct dirent* de;
        while ((de = readdir( d ))) {
            struct stat st;
            if (de->d_name[0] == '.' || de->d_type == DT_DIR) continue;
            if (fstatat( dirfd( d ), de->d_name, &st, 0 ) || !S_ISREG( st.st_mode ) ||
                faccessat( dirfd( d ), de->d_name, X_OK, 0 ))
                continue;
            if (n == cap) {
                cap = cap ? 2 * cap : 1024;
                char** v = realloc( names, (size_t)cap * sizeof(*v) );
                if (!v) break;
                names = v;
            }
            if ((names[n] = strdup( de->d_name ))) n++;
        }
        closedir( d );
    }
    if (names) qsort( names, (size_t)n, sizeof(*names), name_cmp );
    idx->run = calloc( (size_t)(n + ran->n + 1), sizeof(*idx->run) );
    /* against the last one kept: the one before may be a duplicate, freed */
    char* last = NULL;
    for (int i = 0; i < n; i++) {
        if (idx->run && (!last || strcmp( names[i], last )))
            idx->run[idx->nrun++] = (run_t){ last = names[i], 0 };
        else
            free( names[i] );
    }
    free( names );
    if (!idx->run) return;
    /* the lines run: an executable's frecency, or one of their own */
    const long long now = time( NULL );
    const int nexe = idx->nrun;
    for (int i = 0; i < ran->n; i++) {
        run_t key = { ran->v[i].id, 0 };
        run_t* r = bsearch( &key, idx->run, (size_t)nexe, sizeof(*idx->run), run_cmp );
        if (r) r->frecency = frecency( &ran->v[i], now );
        else if ((key.line = strdup( ran->v[i].id )))
            idx->run[idx->nrun++] = (run_t){ key.line, frecency( &ran->v[i], now ) };
    }
}

/** scans, and hands the index to the main thread; a new inotify fd in ``*ifd`` */
static void index_rescan( indexer_t* x, const counts_t* counts, const counts_t* ran, int* ifd, unsigned* present ) {
    if (*ifd >= 0) close( *ifd );
    *ifd = inotify_init1( IN_NONBLOCK | IN_CLOEXEC );
    index_t* idx = scan( counts, *ifd, present );
    if (!idx) return;
    scan_path( idx, ran, *ifd );
    awl_vrb_printf( "launcher: %d applications, %d commands\n", idx->n, idx->nrun );
    /* replaces one the main thread hasn't taken yet */
    index_free( atomic_exchange( &x->pending, idx ) );
}

/** which applications directories exist, as scan() reports them */
static unsigned present_now( void ) {
    int ndirs = 0;
    unsigned present = 0;
    char** dirs = app_dirs( &ndirs );
    for (int i = 0; i < ndirs; i++) {
        struct stat st;
        if (i < 32 && dirs[i] && !stat( dirs[i], &st ) && S_ISDIR( st.st_mode )) present |= 1u << i;
        free( dirs[i] );
    }
    free( dirs );
    return present;
}

/** by key, highest first */
static int count_by_key( const void* a, const void* b ) {
    const double fa = ((const count_t*)a)->key, fb = ((const count_t*)b)->key;
    return fa != fb ? (fa < fb ? 1 : -1) : 0;
}

/** counts the names in msgs (freed) as launched now, keeps max of them at
 * most (the least used dropped), and saves them as name */
static void counts_add( const indexer_t* x, const char* name, counts_t* c, msg_t* msgs, int max ) {
    const long long now = time( NULL );
    for (msg_t* m = msgs; m; m = m->next) {
        count_t* k = count_add( c, m->name );
        if (!k) continue;
        k->count++;
        k->last = now;
    }
    msgs_free( msgs );
    if (max && c->n > max) {
        for (int i = 0; i < c->n; i++) c->v[i].key = frecency( &c->v[i], now );
        qsort( c->v, (size_t)c->n, sizeof(*c->v), count_by_key );
        while (c->n > max) free( c->v[--c->n].id );
    }
    counts_save( x, name, c );
}

static void* indexer( void* data ) {
    indexer_t* x = data;
    counts_t counts = { 0 }, ran = { 0 };
    int ifd = -1;
    unsigned present = 0;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    awl_icons_t* icons = awl_icons_new_desktop( x->icon_theme );

    counts_load( x, "launcher", &counts );
    counts_load( x, "run", &ran );
    index_rescan( x, &counts, &ran, &ifd, &present );
    int dirty = 0;
    while (1) {
        struct pollfd fds[2] = {
            { .fd = x->thread.wake_fd, .events = POLLIN },
            { .fd = ifd, .events = POLLIN },
        };
        int r = poll( fds, 2, dirty ? SETTLE_MS : REWATCH_MS );
        if (r < 0 && errno != EINTR) break;
        if (!r) {
            /* settled, or a directory may have come (or gone) */
            if (dirty || present_now() != present) index_rescan( x, &counts, &ran, &ifd, &present );
            dirty = 0;
            continue;
        }
        if (fds[1].revents & POLLIN) {
            while (read( ifd, buf, sizeof buf ) > 0)
                ;
            dirty = 1;
        }
        if (!(fds[0].revents & POLLIN)) continue;
        if (awl_thread_woken( &x->thread )) break;

        pthread_mutex_lock( &x->lock );
        msg_t *wanted = x->icons_wanted, *launched = x->launched, *run = x->ran;
        x->icons_wanted = x->launched = x->ran = NULL;
        pthread_mutex_unlock( &x->lock );

        if (launched) counts_add( x, "launcher", &counts, launched, 0 );
        if (run) counts_add( x, "run", &ran, run, MAX_HISTORY );
        /* the new order */
        if (launched || run) index_rescan( x, &counts, &ran, &ifd, &present );
        /* the newest wish first: the rows shown now */
        msg_t* found = NULL;
        for (msg_t *m = wanted, *next; m; m = next) {
            next = m->next;
            char* path = icons ? awl_icons_lookup( icons, m->name, m->px, NULL, NULL ) : NULL;
            m->img = path ? awl_icon_load( path, m->px, m->px ) : NULL;
            free( path );
            m->next = found;
            found = m;
        }
        if (found) {
            pthread_mutex_lock( &x->lock );
            msg_t** tail = &found;
            while (*tail) tail = &(*tail)->next;
            *tail = x->icons_found;
            x->icons_found = found;
            pthread_mutex_unlock( &x->lock );
        }
    }
    if (ifd >= 0) close( ifd );
    awl_icons_free( icons );
    for (int i = 0; i < counts.n; i++) free( counts.v[i].id );
    for (int i = 0; i < ran.n; i++) free( ran.v[i].id );
    free( counts.v );
    free( ran.v );
    return NULL;
}

/* ---- the main thread ---- */

/** a result: an entry of the index, its score */
typedef struct {
    int entry, score;
    /** the rest of the order (result_cmp()), from here on: the entry's
     * frecency */
    double frecency;
    /** its name */
    const char* name;
    /** the name's length for a query, 0 without one */
    size_t len;
} result_t;

/** an icon by name at px: img NULL while it is wanted, or if there is none */
typedef struct {
    char* name;
    int px;
    bool done;
    pixman_image_t* img;
} icon_t;

/** the prompt's markup, parsed while it is the same text, and the styles'
 * fonts for the font it was last drawn in */
typedef struct {
    char* src;
    awl_markup_t m;
    struct fcft_font* font;
    struct fcft_font* fonts[4];
} prompt_t;

struct awl_launcher {
    /** the root */
    awl_plugin_data_t* p;
    awl_window_t* win;
    awl_timer_t* timer;
    index_t* idx;
    /** AWL_LAUNCHER_APPS or AWL_LAUNCHER_RUN */
    int mode;
    char query[256];
    result_t* res;
    int nres;
    /** the row chosen and the first shown, indices into res */
    int sel, top;
    icon_t icons[MAX_ICONS];
    int nicons, waiting;

    /* measure()'s layout, physical pixels */
    float scale;
    int w, pad, gap, input_h, sep_h, row_h, rows, icon_px;
    /* the bar layout's: its height, the query's width, the results shown
     * (from top on) and where each is */
    int h, input_w, nshown;
    int bx[MAX_SHOWN], bw[MAX_SHOWN];
    prompt_t pm;
    /** scrolling not yet turned into steps */
    double acc;
    /** the index thread, NULL if none */
    indexer_t* ix;
    /** the matcher's tables (plugins/fuzzy.h) */
    awl_fuzzy_tables_t* fz;
};

static int bar( const awl_launcher_t* la ) {
    return CFG->layout == AWL_LAUNCHER_BAR;
}

static int run_mode( awl_launcher_t* la ) {
    return la->mode == AWL_LAUNCHER_RUN;
}

/** what entry (of the index's list in this mode) is called, matched and
 * shown by: the application's Name, or the command line */
static const char* entry_name( awl_launcher_t* la, int entry ) {
    return run_mode( la ) ? la->idx->run[entry].line : la->idx->v[entry].e.name;
}

static double entry_frecency( awl_launcher_t* la, int entry ) {
    return run_mode( la ) ? la->idx->run[entry].frecency : la->idx->v[entry].frecency;
}

/** result r's name; its icon's, NULL if none */
static const char* name_of( awl_launcher_t* la, int r ) {
    return entry_name( la, la->res[r].entry );
}

static const char* icon_of( awl_launcher_t* la, int r ) {
    return run_mode( la ) ? NULL : la->idx->v[la->res[r].entry].e.icon;
}

static const char* prompt( awl_launcher_t* la ) {
    const char* p = run_mode( la ) ? CFG->run_prompt : CFG->prompt;
    return p ? p : "";
}


static void prompt_free( awl_launcher_t* la ) {
    for (int i = 1; i < 4; i++)
        if (la->pm.fonts[i]) awl_host->font_release( la->pm.fonts[i] );
    free( la->pm.src );
    awl_markup_free( &la->pm.m );
    memset( &la->pm, 0, sizeof la->pm );
}

/** the prompt's markup for font, ready; 0 if it couldn't be (out of memory) */
static int prompt_ready( awl_launcher_t* la, struct fcft_font* font ) {
    const char* p = prompt( la );
    if (!la->pm.src || strcmp( la->pm.src, p )) {
        free( la->pm.src );
        awl_markup_free( &la->pm.m );
        if (!(la->pm.src = strdup( p )) || !awl_markup_parse( &la->pm.m, p, 1 )) {
            free( la->pm.src );
            la->pm.src = NULL;
            return 0;
        }
    }
    if (font != la->pm.font) {
        for (int i = 1; i < 4; i++) {
            if (la->pm.fonts[i]) awl_host->font_release( la->pm.fonts[i] );
            la->pm.fonts[i] = awl_host->font_style( font, i );
        }
        la->pm.font = la->pm.fonts[0] = font;
    }
    return 1;
}

/** draws the prompt (into pix; NULL: only measures) at x in a line h high at
 * y, cut at right, its runs on font's baseline in their styles; returns its
 * width */
static int prompt_draw( awl_launcher_t* la, pixman_image_t* pix, struct fcft_font* font, int x, int right, int y, int h, uint32_t fg ) {
    if (!prompt_ready( la, font )) return 0;
    const int x0 = x, base = y + (h - font->height) / 2 + font->ascent;
    for (unsigned i = 0; i < la->pm.m.nruns; i++) {
        const awl_markup_run_t* r = &la->pm.m.runs[i];
        struct fcft_font* f = awl_markup_font( la->pm.fonts, r->style );
        const uint32_t c = r->style & AWL_MARKUP_COLOR ? r->color : fg;
        /* the run alone: the text is ours, cut it there for a moment */
        char* s = la->pm.m.text + r->off;
        const char end = s[r->len];
        s[r->len] = 0;
        const int w = (int)awl_host->text_width( f, s );
        if (pix && x < right)
            awl_host->text_over( pix, f, x, base - f->ascent, (unsigned)(right - x), (unsigned)f->height, s, c );
        if (pix && x < right && r->style & AWL_MARKUP_UNDERLINE) {
            const int t = f->underline.thickness > 0 ? f->underline.thickness : 1;
            const int uw = x + w > right ? right - x : w;
            awl_fill_shape( pix, (float)x, (float)(base - f->underline.position), (float)uw, (float)t, 0, 0, c );
        }
        s[r->len] = end;
        x += w;
    }
    return x - x0;
}

/** what shows without results */
static const char* empty_text( awl_launcher_t* la ) {
    if (!la->idx) return run_mode( la ) ? "reading $PATH…" : "reading the applications…";
    return run_mode( la ) ? "no match, Return runs it as typed" : "no match";
}

static void icons_clear( awl_launcher_t* la ) {
    for (int i = 0; i < la->nicons; i++) {
        free( la->icons[i].name );
        if (la->icons[i].img) pixman_image_unref( la->icons[i].img );
    }
    la->nicons = 0;
    la->waiting = 0;
}

/** the icon name at px: decoded, or NULL while the thread looks for it (or
 * if there is none); asks the thread the first time */
static pixman_image_t* icon( awl_launcher_t* la, const char* name, int px ) {
    for (int i = 0; i < la->nicons; i++)
        if (la->icons[i].px == px && !strcmp( la->icons[i].name, name )) return la->icons[i].img;
    if (!la->ix) return NULL;
    if (la->nicons == MAX_ICONS) {
        /* the oldest goes, unless it is still wanted */
        if (!la->icons[0].done) return NULL;
        free( la->icons[0].name );
        if (la->icons[0].img) pixman_image_unref( la->icons[0].img );
        memmove( la->icons, la->icons + 1, (MAX_ICONS - 1) * sizeof(*la->icons) );
        la->nicons--;
    }
    msg_t* m = calloc( 1, sizeof(*m) );
    char* copy = strdup( name );
    if (!m || !copy || !(m->name = strdup( name ))) {
        free( m );
        free( copy );
        return NULL;
    }
    m->px = px;
    la->icons[la->nicons++] = (icon_t){ copy, px, 0, NULL };
    pthread_mutex_lock( &la->ix->lock );
    m->next = la->ix->icons_wanted;
    la->ix->icons_wanted = m;
    pthread_mutex_unlock( &la->ix->lock );
    awl_thread_wake( &la->ix->thread );
    la->waiting++;
    awl_host->timer_set( la->timer, ICON_POLL_MS );
    return NULL;
}

/** what the thread found: into the cache; redraws if any is shown */
static void icons_take( awl_launcher_t* la ) {
    if (!la->ix) return;
    pthread_mutex_lock( &la->ix->lock );
    msg_t* found = la->ix->icons_found;
    la->ix->icons_found = NULL;
    pthread_mutex_unlock( &la->ix->lock );
    int any = 0;
    for (msg_t* m = found; m; m = m->next)
        for (int i = 0; i < la->nicons; i++)
            if (!la->icons[i].done && la->icons[i].px == m->px && !strcmp( la->icons[i].name, m->name )) {
                la->icons[i].done = 1;
                la->icons[i].img = m->img;
                m->img = NULL;
                la->waiting--;
                any = 1;
                break;
            }
    msgs_free( found );
    if (any) awl_host->window_redraw( awl_host->srv, la->win );
}

static void index_take( awl_launcher_t* la );
/** while it is open and waits: for icons, or for the first index */
static void timer_fire( void* data ) {
    awl_launcher_t* la = data;
    if (!awl_host->window_monitor( la->win )) return;
    if (la->ix && atomic_load( &la->ix->pending )) {
        index_take( la );
        awl_host->window_redraw( awl_host->srv, la->win );
    }
    icons_take( la );
    if (la->waiting > 0 || !la->idx) awl_host->timer_set( la->timer, ICON_POLL_MS );
}

/** the score of en for q, the best of its fields (Name's counts most);
 * returns 0 if none matches */
static int score_of( awl_fuzzy_tables_t* tb, const awl_fuzzy_query_t* q, const entry_t* en, int* score ) {
    /* what the other fields lose against Name */
    enum { OTHER = 10 };
    const char* fields[] = { en->e.name, en->e.generic, en->e.keywords, en->command };
    int best = 0, found = 0;
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        int s;
        if (!fields[i] || !awl_fuzzy_match( tb, q, fields[i], &s, NULL, NULL )) continue;
        if (i) s -= OTHER;
        if (!found || s > best) best = s;
        found = 1;
    }
    *score = best;
    return found;
}

/** frecency's part in the ranking: a few points, as much as a word start
 * at most */
static int frecency_bonus( double f ) {
    const int b = (int)lround( 4 * log2( 1 + f ) );
    return b < 16 ? b : 16;
}

static int result_cmp( const void* a, const void* b ) {
    const result_t *x = a, *y = b;
    if (x->score != y->score) return y->score - x->score;
    if (x->frecency != y->frecency) return x->frecency < y->frecency ? 1 : -1;
    /* for a query the shorter name (more of it matched), else by name */
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return strcasecmp( x->name, y->name );
}

/** matches the query against the index (the applications, or the run
 * mode's lines), the best first */
static void filter( awl_launcher_t* la ) {
    awl_fuzzy_query_t q;
    awl_fuzzy_query( &q, la->query );
    la->nres = 0;
    la->sel = la->top = 0;
    if (!la->idx) return;
    const int n = run_mode( la ) ? la->idx->nrun : la->idx->n;
    result_t* res = realloc( la->res, (size_t)(n ? n : 1) * sizeof(*res) );
    if (!res) return;
    la->res = res;
    for (int i = 0; i < n; i++) {
        int s;
        if (run_mode( la ) ? !awl_fuzzy_match( la->fz, &q, la->idx->run[i].line, &s, NULL, NULL )
                       : !score_of( la->fz, &q, &la->idx->v[i], &s ))
            continue;
        /* an empty query: the frecency alone, then the names */
        const double f = entry_frecency( la, i );
        const char* name = entry_name( la, i );
        la->res[la->nres++] = (result_t){ .entry = i, .score = q.n ? s + frecency_bonus( f ) : 0, .frecency = f,
                                          .name = name, .len = la->query[0] ? strlen( name ) : 0 };
    }
    qsort( la->res, (size_t)la->nres, sizeof(*la->res), result_cmp );
}

/** a new index from the thread, if there is one */
static void index_take( awl_launcher_t* la ) {
    index_t* idx = la->ix ? atomic_exchange( &la->ix->pending, NULL ) : NULL;
    if (!idx) return;
    index_free( la->idx );
    la->idx = idx;
    const int sel = la->sel;
    filter( la );
    la->sel = sel < la->nres ? sel : 0;
}

/* ---- the painter ---- */

static int px( awl_launcher_t* la, float v ) {
    return (int)lroundf( v * la->scale );
}

/** the icons' size, physical pixels; the cache is for the size shown */
static void icon_size( awl_launcher_t* la, int ipx ) {
    if (ipx == la->icon_px) return;
    icons_clear( la );
    la->icon_px = ipx;
}

/** whether result r gets room for an icon in the bar layout: it names one
 * that isn't known to be missing at the size shown (one still on its way
 * keeps its room, so the row only moves if it turns out to be missing) */
static int icon_slot( awl_launcher_t* la, int r ) {
    const char* name = icon_of( la, r );
    if (!name) return 0;
    for (int i = 0; i < la->nicons; i++)
        if (la->icons[i].px == la->icon_px && !strcmp( la->icons[i].name, name ))
            return !la->icons[i].done || la->icons[i].img;
    return 1;
}

/** result r's width in the bar layout: its icon and name, padded; its name
 * at most name_w */
static int item_w( awl_launcher_t* la, struct fcft_font* font, int r, int name_w ) {
    int nw = (int)awl_host->text_width( font, name_of( la, r ) );
    if (nw > name_w) nw = name_w;
    return 2 * la->pad + (icon_slot( la, r ) ? la->icon_px + la->gap : 0) + nw;
}

/** lays out the results from top on beside the query, as many as fit;
 * returns whether the chosen one is among them */
static int lay_out_bar( awl_launcher_t* la, struct fcft_font* font, int name_w, int spacing ) {
    int x = la->input_w;
    la->nshown = 0;
    for (int r = la->top; r < la->nres && la->nshown < MAX_SHOWN; r++) {
        int w = item_w( la, font, r, name_w );
        if (x + w > la->w) {
            if (la->nshown) break;
            w = la->w - x; /* one alone, cut */
        }
        la->bx[la->nshown] = x;
        la->bw[la->nshown++] = w;
        x += w + spacing;
    }
    return la->sel >= la->top && la->sel < la->top + la->nshown;
}

/* the bar layout: the bar's spot (AWL_WINDOW_AT_BAR); the query, then the
 * results in a row, a page of them: one that starts at the chosen one going
 * right, one that ends with it going left */
static int measure_bar( awl_launcher_t* la, struct fcft_font* font, int max_w, int max_h, int* w, int* h ) {
    la->w = max_w;
    la->h = max_h;
    la->pad = px( la, 6 );
    la->gap = px( la, 6 );
    const int ipx = px( la, (float)CFG->icon_size ), room = la->h - 2 * px( la, 2 );
    icon_size( la, ipx < room ? ipx : room );
    /* the query's width at least, more as it grows, the results right after */
    const int typed = 2 * la->pad + px( la, 3 ) + prompt_draw( la, NULL, font, 0, 0, 0, 0, 0 ) +
                      (int)awl_host->text_width( font, la->query );
    la->input_w = px( la, (float)CFG->input_width );
    if (la->input_w < typed) la->input_w = typed;
    if (la->input_w > la->w / 3) la->input_w = la->w / 3;
    if (la->w <= la->input_w || la->h < 1) return 0;
    const int name_w = (la->w - la->input_w) / 3, spacing = px( la, 4 );
    la->nshown = 0;
    if (la->nres) {
        if (la->sel < la->top) {
            /* the page before: as much as fits that ends with the chosen one */
            int x = la->input_w + item_w( la, font, la->sel, name_w );
            la->top = la->sel;
            while (la->top > 0 && (x += spacing + item_w( la, font, la->top - 1, name_w )) <= la->w) la->top--;
        }
        if (!lay_out_bar( la, font, name_w, spacing )) {
            la->top = la->sel;
            lay_out_bar( la, font, name_w, spacing );
        }
    }
    *w = la->w;
    *h = la->h;
    return 1;
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    awl_launcher_t* la = data;
    la->scale = scale;
    if (bar( la )) return measure_bar( la, font, max_w, max_h, w, h );
    const int fh = font->height;
    la->pad = px( la, 6 );
    la->gap = px( la, 10 );
    icon_size( la, px( la, (float)CFG->icon_size ) );
    la->input_h = fh + 2 * la->pad;
    la->sep_h = px( la, 7 );
    la->row_h = (fh > la->icon_px ? fh : la->icon_px) + 2 * px( la, 3 );
    la->w = px( la, (float)CFG->width );
    if (la->w > max_w) la->w = max_w;
    la->rows = CFG->rows;
    if (la->input_h + la->sep_h + la->rows * la->row_h > max_h) la->rows = (max_h - la->input_h - la->sep_h) / la->row_h;
    if (la->rows < 1) return 0;
    *w = la->w;
    *h = la->input_h + la->sep_h + la->rows * la->row_h;
    return 1;
}

/** text in fg, the characters at the byte offsets in pos (n of them,
 * -1 none) in match, from x on, at most to right; returns where it ended */
static int text_marked( pixman_image_t* pix, struct fcft_font* font, const char* text, const int* pos, int n, int x,
                        int y, int right, int h, uint32_t fg, uint32_t match ) {
    char run[512];
    const size_t len = strlen( text );
    for (size_t i = 0; i < len && x < right;) {
        /* a run of characters all matched, or all not */
        int on = 0;
        for (int k = 0; k < n; k++) on |= pos[k] == (int)i;
        size_t j = i;
        do {
            j++;
            while (j < len && ((unsigned char)text[j] & 0xc0) == 0x80) j++;
            int next_on = 0;
            for (int k = 0; k < n; k++) next_on |= pos[k] == (int)j;
            if (j >= len || next_on != on) break;
        } while (j - i < sizeof run - 8);
        snprintf( run, sizeof run, "%.*s", (int)(j - i), text + i );
        awl_host->text_over( pix, font, x, y, (unsigned)(right - x), (unsigned)h, run, on ? match : fg );
        x += (int)awl_host->text_width( font, run );
        i = j;
    }
    return x;
}

/** the prompt and the query from x to right in a line h high at y, the
 * query's tail if it is too long, and the cursor after it (in the query's
 * color) */
static void paint_query( awl_launcher_t* la, pixman_image_t* pix, struct fcft_font* font, int x, int right, int y, int h ) {
    const uint32_t fg = COLORS->launcher_query.v;
    x += prompt_draw( la, pix, font, x, right, y, h, COLORS->launcher_prompt.v );
    const int cursor_w = px( la, 1.5f ) > 1 ? px( la, 1.5f ) : 1;
    const char* q = la->query;
    while (*q && x + (int)awl_host->text_width( font, q ) + cursor_w > right) {
        q++;
        while (((unsigned char)*q & 0xc0) == 0x80) q++;
    }
    if (*q) awl_host->text_over( pix, font, x, y, (unsigned)(right - x), (unsigned)h, q, fg );
    x += (int)awl_host->text_width( font, q );
    awl_fill_shape( pix, (float)x + px( la, 1 ), (float)(y + (h - font->height) / 2), (float)cursor_w,
                    (float)font->height, 0, 0, fg );
}

/** result r's icon, if it has one, centered in an icon's width at x and h
 * at y */
static void paint_icon( awl_launcher_t* la, pixman_image_t* pix, int r, int x, int y, int h ) {
    const char* name = icon_of( la, r );
    pixman_image_t* img = name ? icon( la, name, la->icon_px ) : NULL;
    if (!img) return;
    const int iw = pixman_image_get_width( img ), ih = pixman_image_get_height( img );
    pixman_image_composite32( PIXMAN_OP_OVER, img, NULL, pix, 0, 0, 0, 0, x + (la->icon_px - iw) / 2, y + (h - ih) / 2,
                              iw, ih );
}

/** what the query matched in text, into pos; returns how many */
static int marks( awl_fuzzy_tables_t* tb, const awl_fuzzy_query_t* fq, const char* text, int* pos ) {
    return fq->n && awl_fuzzy_match( tb, fq, text, NULL, NULL, pos ) ? fq->n : 0;
}

static void paint_bar( awl_launcher_t* la, pixman_image_t* pix, struct fcft_font* font, int x0, int y0, uint32_t fg, uint32_t dim,
                       uint32_t match ) {
    const awl_popup_config_t* pc = &la->p->config.popup;
    paint_query( la, pix, font, x0 + la->pad, x0 + la->input_w - la->pad, y0, la->h );
    if (!la->nres) {
        awl_host->text_over( pix, font, x0 + la->input_w + la->pad, y0, (unsigned)(la->w - la->input_w - la->pad),
                             (unsigned)la->h, empty_text( la ), dim );
        return;
    }
    awl_fuzzy_query_t fq;
    awl_fuzzy_query( &fq, la->query );
    const int inset = px( la, 2 );
    for (int i = 0; i < la->nshown; i++) {
        const int r = la->top + i, x = x0 + la->bx[i];
        if (r == la->sel)
            awl_fill_shape( pix, (float)x, (float)(y0 + inset), (float)la->bw[i], (float)(la->h - 2 * inset),
                            (float)px( la, pc->radius < 6 ? (float)pc->radius : 6 ), 0, la->p->config.colors.popup_hover );
        int tx = x + la->pad;
        if (icon_slot( la, r )) {
            paint_icon( la, pix, r, tx, y0, la->h );
            tx += la->icon_px + la->gap;
        }
        int pos[AWL_FUZZY_MAX_QUERY];
        const int n = marks( la->fz, &fq, name_of( la, r ), pos );
        text_marked( pix, font, name_of( la, r ), pos, n, tx, y0, x + la->bw[i] - la->pad, la->h, fg, match );
    }
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0, int hover ) {
    awl_launcher_t* la = data;
    const awl_popup_config_t* pc = &la->p->config.popup;
    const awl_colors_t* ak = &la->p->config.colors;
    const awl_plugin_colors_t* k = COLORS;
    const uint32_t fg = ak->popup_fg, dim = k->launcher_dim.v, match = k->launcher_match.v;
    if (bar( la )) {
        paint_bar( la, pix, font, x0, y0, fg, dim, match );
        return;
    }

    int x, right;
    paint_query( la, pix, font, x0 + la->pad, x0 + la->w - la->pad, y0, la->input_h );

    const float t = fmaxf( 1, (float)px( la, 1 ) );
    awl_fill_shape( pix, (float)x0, (float)(y0 + la->input_h) + ((float)la->sep_h - t) / 2, (float)la->w, t, 0, 0, dim );

    const int ry0 = y0 + la->input_h + la->sep_h;
    if (!la->nres) {
        awl_host->text_over( pix, font, x0 + la->pad, ry0, (unsigned)(la->w - 2 * la->pad), (unsigned)la->row_h,
                             empty_text( la ), dim );
        return;
    }
    awl_fuzzy_query_t fq;
    awl_fuzzy_query( &fq, la->query );
    for (int i = 0; i < la->rows && la->top + i < la->nres; i++) {
        const int r = la->top + i, y = ry0 + i * la->row_h;
        if (r == la->sel)
            awl_fill_shape( pix, (float)x0, (float)y, (float)la->w, (float)la->row_h,
                            (float)px( la, pc->radius < 6 ? (float)pc->radius : 6 ), 0, la->p->config.colors.popup_hover );
        x = x0 + la->pad;
        /* the run mode has no icons, nor their column */
        if (!run_mode( la )) {
            paint_icon( la, pix, r, x, y, la->row_h );
            x += la->icon_px + la->gap;
        }
        right = x0 + la->w - la->pad;
        /* the name, what the query matched in it marked */
        int pos[AWL_FUZZY_MAX_QUERY];
        const int n = marks( la->fz, &fq, name_of( la, r ), pos );
        x = text_marked( pix, font, name_of( la, r ), pos, n, x, y, right, la->row_h, fg, match );
        const char* generic = run_mode( la ) ? NULL : la->idx->v[la->res[r].entry].e.generic;
        if (generic && x + la->gap < right)
            awl_host->text_over( pix, font, x + la->gap, y, (unsigned)(right - x - la->gap), (unsigned)la->row_h,
                                 generic, dim );
    }
}

/** the result row (the bar layout's: the result shown) at px, py; -1 the
 * query, or none */
static int hit( void* data, int x, int y ) {
    awl_launcher_t* la = data;
    if (bar( la )) {
        for (int i = 0; i < la->nshown; i++)
            if (x >= la->bx[i] && x < la->bx[i] + la->bw[i] && y >= 0 && y < la->h) return i;
        return -1;
    }
    const int ry0 = la->input_h + la->sep_h;
    if (x < 0 || x >= la->w || y < ry0) return -1;
    const int i = (y - ry0) / la->row_h;
    return i < la->rows && la->top + i < la->nres ? i : -1;
}

static int box( void* data, int part, int* x, int* y, int* w, int* h ) {
    awl_launcher_t* la = data;
    if (bar( la )) {
        if (part < 0 || part >= la->nshown) return 0;
        *x = la->bx[part];
        *y = 0;
        *w = la->bw[part];
        *h = la->h;
        return 1;
    }
    if (part < 0 || part >= la->rows) return 0;
    *x = 0;
    *y = la->input_h + la->sep_h + part * la->row_h;
    *w = la->w;
    *h = la->row_h;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- choosing and launching ---- */

/** moves the choice to row r, scrolled into view (the bar layout's paged
 * by measure_bar()) */
static void choose( awl_launcher_t* la, int r ) {
    if (!la->nres) return;
    if (r < 0) r = 0;
    if (r >= la->nres) r = la->nres - 1;
    la->sel = r;
    if (bar( la )) {
        awl_host->window_redraw( awl_host->srv, la->win );
        return;
    }
    if (la->sel < la->top) la->top = la->sel;
    if (la->rows > 0 && la->sel >= la->top + la->rows) la->top = la->sel - la->rows + 1;
    awl_host->window_redraw( awl_host->srv, la->win );
}

static void close_launcher( awl_launcher_t* la ) {
    awl_host->window_hide( awl_host->srv, la->win );
}

/** counts a launch of name on the thread: an application's id (run 0), or
 * a command line (run 1) */
static void count_launch( awl_launcher_t* la, const char* name, int run ) {
    msg_t* m = calloc( 1, sizeof(*m) );
    if (!la->ix || !m || !(m->name = strdup( name ))) {
        free( m );
        return;
    }
    pthread_mutex_lock( &la->ix->lock );
    msg_t** list = run ? &la->ix->ran : &la->ix->launched;
    m->next = *list;
    *list = m;
    pthread_mutex_unlock( &la->ix->lock );
    awl_thread_wake( &la->ix->thread );
}

/** starts result r: its argv, in the terminal if it wants one, in its
 * working directory if it has one (through sh: spawn has none) */
static void launch( awl_launcher_t* la, int r ) {
    if (r < 0 || r >= la->nres) return;
    entry_t* en = &la->idx->v[la->res[r].entry];
    char** argv = awl_desktop_exec_argv( en->e.exec, en->e.name, en->e.icon, en->file );
    if (!argv) {
        awl_err_printf( "launcher: %s: can't make sense of Exec=%s\n", en->id, en->e.exec );
        return;
    }
    int n = 0, nt = 0;
    while (argv[n]) n++;
    const char* const* term = en->e.terminal ? CFG->terminal : NULL;
    while (term && term[nt]) nt++;
    const char** full = calloc( (size_t)(4 + nt + n + 1), sizeof(*full) );
    if (full) {
        int k = 0;
        if (en->e.path) {
            full[k++] = "/bin/sh";
            full[k++] = "-c";
            full[k++] = "cd -- \"$0\" && exec \"$@\"";
            full[k++] = en->e.path;
        }
        for (int i = 0; i < nt; i++) full[k++] = term[i];
        for (int i = 0; i < n; i++) full[k++] = argv[i];
        awl_log_printf( "launcher: %s\n", en->id );
        awl_host->actions->spawn( awl_host->srv, &(Arg){ .v = full } );
        free( full );
    }
    awl_desktop_argv_free( argv );
    en->frecency += 4;
    count_launch( la, en->id, 0 );
    close_launcher( la );
}

/** runs line with sh -c, in the terminal if term, and remembers it */
static void run_line( awl_launcher_t* la, const char* line, int term ) {
    char buf[sizeof la->query];
    while (*line == ' ') line++;
    size_t n = strlen( line );
    while (n && line[n - 1] == ' ') n--;
    if (!n || n >= sizeof buf) return;
    memcpy( buf, line, n );
    buf[n] = '\0';
    int nt = 0;
    const char* const* t = term ? CFG->terminal : NULL;
    while (t && t[nt]) nt++;
    const char** full = calloc( (size_t)(nt + 4), sizeof(*full) );
    if (!full) return;
    int k = 0;
    for (int i = 0; i < nt; i++) full[k++] = t[i];
    full[k++] = "/bin/sh";
    full[k++] = "-c";
    full[k++] = buf;
    awl_log_printf( "launcher: runs %s\n", buf );
    awl_host->actions->spawn( awl_host->srv, &(Arg){ .v = full } );
    free( full );
    count_launch( la, buf, 1 );
    close_launcher( la );
}

/** Return: launches result r; the run mode runs its line, or the query as
 * typed if literal (or if nothing matched), in the terminal if term */
static void activate( awl_launcher_t* la, int r, int literal, int term ) {
    if (!run_mode( la )) launch( la, r );
    else run_line( la, literal || r < 0 || r >= la->nres ? la->query : name_of( la, r ), term );
}

/** the run mode's Tab: the chosen line into the query, to go on from */
static void complete( awl_launcher_t* la ) {
    if (la->sel < 0 || la->sel >= la->nres) return;
    snprintf( la->query, sizeof la->query, "%s", name_of( la, la->sel ) );
    filter( la );
    awl_host->window_redraw( awl_host->srv, la->win );
}

/** the query lost its last character (word: its last word, and the
 * blanks after it) */
static void query_back( awl_launcher_t* la, int word ) {
    size_t n = strlen( la->query );
    if (word)
        while (n && la->query[n - 1] == ' ') n--;
    do {
        while (n && ((unsigned char)la->query[n - 1] & 0xc0) == 0x80) n--;
        if (n) n--;
    } while (word && n && la->query[n - 1] != ' ');
    la->query[n] = '\0';
}

/** results a page up or down moves */
static int page( awl_launcher_t* la ) {
    const int n = bar( la ) ? la->nshown : la->rows;
    return n > 0 ? n : 1;
}

static int key( awl_window_t* win, uint32_t sym, const char* utf8, uint32_t mods, void* data ) {
    awl_launcher_t* la = data;
    const int ctrl = mods & WLR_MODIFIER_CTRL, other = mods & (WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
    if (other) return 0;
    if (bar( la ) && (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) {
        choose( la, la->sel + (sym == XKB_KEY_Left ? -1 : 1) );
        return 1;
    }
    switch (sym) {
    case XKB_KEY_Escape: close_launcher( la ); return 1;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter: activate( la, la->sel, mods & WLR_MODIFIER_SHIFT, ctrl ); return 1;
    case XKB_KEY_Up:
    case XKB_KEY_ISO_Left_Tab: choose( la, la->sel - 1 ); return 1;
    case XKB_KEY_Tab:
        if (run_mode( la )) complete( la );
        else choose( la, la->sel + 1 );
        return 1;
    case XKB_KEY_Down: choose( la, la->sel + 1 ); return 1;
    case XKB_KEY_Page_Up: choose( la, la->sel - page( la ) ); return 1;
    case XKB_KEY_Page_Down: choose( la, la->sel + page( la ) ); return 1;
    case XKB_KEY_BackSpace:
        query_back( la, ctrl );
        filter( la );
        awl_host->window_redraw( awl_host->srv, win );
        return 1;
    }
    if (ctrl) {
        switch (sym) {
        case XKB_KEY_p: choose( la, la->sel - 1 ); return 1;
        case XKB_KEY_n: choose( la, la->sel + 1 ); return 1;
        case XKB_KEY_w: query_back( la, 1 ); break;
        case XKB_KEY_u: la->query[0] = '\0'; break;
        default: return 0;
        }
        filter( la );
        awl_host->window_redraw( awl_host->srv, win );
        return 1;
    }
    if (!*utf8) return 0;
    const size_t n = strlen( la->query ), add = strlen( utf8 );
    if (n + add < sizeof la->query) {
        memcpy( la->query + n, utf8, add + 1 );
        filter( la );
        awl_host->window_redraw( awl_host->srv, win );
    }
    return 1;
}

static void click( awl_window_t* win, int row, int col, uint32_t button, void* data ) {
    awl_launcher_t* la = data;
    if (row >= 0 && button == BTN_LEFT) activate( la, la->top + row, 0, 0 );
}

static void scroll( awl_window_t* win, int row, int col, uint32_t orientation, double delta, int32_t value120,
                    void* data ) {
    awl_launcher_t* la = data;
    /* the bar layout's row goes sideways too */
    if (orientation != 0 && !bar( la )) return;
    la->acc += value120 ? value120 / 120.0 : delta / 10.0;
    const int steps = (int)la->acc;
    if (!steps) return;
    la->acc -= steps;
    choose( la, la->sel + steps );
}

static void hover( awl_window_t* win, int row, int inside, void* data ) {
    awl_launcher_t* la = data;
    if (row >= 0 && la->top + row != la->sel) choose( la, la->top + row );
}

static void hidden( awl_window_t* win, void* data ) {
    awl_launcher_t* la = data;
    la->query[0] = '\0';
    la->nres = la->sel = la->top = la->nshown = 0;
}

void awl_launcher_toggle( awl_launcher_t* la, int mode ) {
    if (!la) return;
    if (awl_host->window_monitor( la->win )) {
        if (mode == la->mode) {
            close_launcher( la );
            return;
        }
        /* the other mode's: the same query */
        la->mode = mode;
        filter( la );
        awl_host->window_redraw( awl_host->srv, la->win );
        return;
    }
    Monitor* m = awl_host->monitor_selected( awl_host->srv );
    if (!m) return;
    la->mode = mode;
    if (!la->win) {
        if (!(la->win = awl_host->window_create( awl_host->srv ))) return;
        awl_host->window_set_painter( awl_host->srv, la->win, &painter, la );
        awl_host->window_set_click( awl_host->srv, la->win, click, la );
        awl_host->window_set_scroll( awl_host->srv, la->win, scroll, la );
        awl_host->window_set_hover( la->win, hover, la );
        awl_host->window_set_hidden( la->win, hidden, la );
        awl_host->window_set_key( awl_host->srv, la->win, key, la );
    }
    if (!la->timer) la->timer = awl_host->timer_add( awl_host->srv, timer_fire, la );
    index_take( la );
    la->query[0] = '\0';
    filter( la );
    if (bar( la ))
        awl_host->window_show_at( awl_host->srv, la->win, m, CFG->strip_at, 0, 0 );
    else
        awl_host->window_show_at( awl_host->srv, la->win, m, CFG->at, 0, CFG->dy );
    /* no index yet: it comes while it is open */
    if (!la->idx) awl_host->timer_set( la->timer, ICON_POLL_MS );
}

awl_launcher_t* awl_launcher_start( awl_plugin_data_t* p ) {
    awl_launcher_t* la = calloc( 1, sizeof *la );
    if (!la) return NULL;
    la->p = p;
    if (!(la->fz = malloc( sizeof *la->fz ))) {
        free( la );
        return NULL;
    }
    indexer_t* x = calloc( 1, sizeof(*x) );
    awl_dict_val_t v;
    if (!x) return la;
    pthread_mutex_init( &x->lock, NULL );
    const char* theme = la->p->config.tray.icon_theme;
    x->icon_theme = theme ? strdup( theme ) : NULL;
    AWL_HOST_CFG_LOCK( awl_host );
    if (AWL_HOST_CFG_GET( awl_host, "state_home", &v ) == AWL_DICT_OK && v.kind == AWL_DICT_KIND_STR)
        x->state_home = strdup( v.str );
    AWL_HOST_CFG_UNLOCK( awl_host );
    if (awl_thread_start( &x->thread, "launcher", indexer, x )) {
        free( x->icon_theme );
        free( x->state_home );
        free( x );
        return la;
    }
    la->ix = x;
    return la;
}

int awl_launcher_stop( awl_launcher_t* la ) {
    if (!la) return 0;
    awl_host->window_destroy( awl_host->srv, la->win );
    awl_host->timer_remove( la->timer );
    icons_clear( la );
    index_free( la->idx );
    free( la->res );
    prompt_free( la );
    free( la->fz );
    indexer_t* x = la->ix;
    free( la );
    if (!x) return 0;
    /* reads files: may hang on a dead mount */
    if (awl_thread_stop( &x->thread, 500 )) return 1;
    index_free( atomic_load( &x->pending ) );
    msgs_free( x->icons_wanted );
    msgs_free( x->launched );
    msgs_free( x->ran );
    msgs_free( x->icons_found );
    pthread_mutex_destroy( &x->lock );
    free( x->icon_theme );
    free( x->state_home );
    free( x );
    return 0;
}
