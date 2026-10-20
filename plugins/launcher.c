/* The launcher, see launcher.h: the index thread first, then the main
 * thread's window. */

#define _DEFAULT_SOURCE /* d_type */
#include "launcher.h"
#include "desktop_entry.h"
#include "fuzzy.h"
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

#define CFG (&awl_plugin_config()->launcher)

enum {
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
};

/* ---- the index, made on the thread, read on the main thread ---- */

typedef struct {
    /** the desktop file id ("org.gnome.Nautilus.desktop"), its file */
    char *id, *file;
    awl_desktop_entry_t e;
    /** the command's name, argv[0] without its directory, to match */
    char* command;
    /** launches, weighted by how recent */
    double frecency;
} entry_t;

typedef struct {
    entry_t* v;
    int n;
} index_t;

static void index_free( index_t* idx ) {
    if (!idx) return;
    for (int i = 0; i < idx->n; i++) {
        free( idx->v[i].id );
        free( idx->v[i].file );
        free( idx->v[i].command );
        awl_desktop_entry_clear( &idx->v[i].e );
    }
    free( idx->v );
    free( idx );
}

/** main -> thread: an icon wanted, or an id launched; thread -> main: an
 * icon (NULL if there is none) */
typedef struct msg {
    struct msg* next;
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
    char* icon_theme;
    char* state_home;
    /** thread -> main: a new index, NULL if none; handed over whole */
    _Atomic(index_t*) pending;
    pthread_mutex_t lock;
    /** under lock: icons wanted and ids launched (main -> thread), icons
     * found (thread -> main) */
    msg_t *icons_wanted, *launched, *icons_found;
} indexer_t;

static indexer_t* ix;

/* ---- the thread: launch counts ---- */

typedef struct {
    char* id;
    long count;
    long long last;
} count_t;

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
    c->v[c->n] = (count_t){ s, 0, 0 };
    return &c->v[c->n++];
}

/** launches, weighted by how long ago the last was */
static double frecency( const count_t* k, long long now ) {
    if (!k || k->count <= 0) return 0;
    const long long age = now - k->last;
    const double w = age < 86400 ? 4 : age < 7 * 86400 ? 2 : age < 30 * 86400 ? 1 : 0.5;
    return (double)k->count * w;
}

static void counts_path( const indexer_t* x, char* path, size_t n ) {
    snprintf( path, n, "%s/awl/launcher", x->state_home ? x->state_home : "" );
}

/** "count last id" lines */
static void counts_load( const indexer_t* x, counts_t* c ) {
    char path[PATH_MAX], line[1024];
    if (!x->state_home) return;
    counts_path( x, path, sizeof path );
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
static void counts_save( const indexer_t* x, const counts_t* c ) {
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    if (!x->state_home) return;
    counts_path( x, path, sizeof path );
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

/** the applications directories, in precedence order; *n how many */
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
 * none); *present gets which of them exist, a bit each. */
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

/** scans, and hands the index to the main thread; a new inotify fd in *ifd */
static void rescan( indexer_t* x, const counts_t* counts, int* ifd, unsigned* present ) {
    if (*ifd >= 0) close( *ifd );
    *ifd = inotify_init1( IN_NONBLOCK | IN_CLOEXEC );
    index_t* idx = scan( counts, *ifd, present );
    if (!idx) return;
    awl_vrb_printf( "launcher: %d applications\n", idx->n );
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

static void* indexer( void* data ) {
    indexer_t* x = data;
    counts_t counts = { 0 };
    int ifd = -1;
    unsigned present = 0;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    awl_icons_t* icons = awl_icons_new_desktop( x->icon_theme );

    counts_load( x, &counts );
    rescan( x, &counts, &ifd, &present );
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
            if (dirty || present_now() != present) rescan( x, &counts, &ifd, &present );
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
        msg_t *wanted = x->icons_wanted, *launched = x->launched;
        x->icons_wanted = x->launched = NULL;
        pthread_mutex_unlock( &x->lock );

        if (launched) {
            const long long now = time( NULL );
            for (msg_t* m = launched; m; m = m->next) {
                count_t* k = count_add( &counts, m->name );
                if (!k) continue;
                k->count++;
                k->last = now;
            }
            counts_save( x, &counts );
            msgs_free( launched );
            /* the new order */
            rescan( x, &counts, &ifd, &present );
        }
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
    free( counts.v );
    return NULL;
}

/* ---- the main thread ---- */

/** a result: an entry of the index, its score */
typedef struct {
    int entry, score;
} result_t;

/** an icon by name at px: img NULL while it is wanted, or if there is none */
typedef struct {
    char* name;
    int px, done;
    pixman_image_t* img;
} icon_t;

static struct {
    awl_window_t* win;
    awl_timer_t* timer;
    index_t* idx;
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
} L;

static int bar( void ) {
    return CFG->layout == AWL_LAUNCHER_BAR;
}

static void icons_clear( void ) {
    for (int i = 0; i < L.nicons; i++) {
        free( L.icons[i].name );
        if (L.icons[i].img) pixman_image_unref( L.icons[i].img );
    }
    L.nicons = 0;
    L.waiting = 0;
}

/** the icon name at px: decoded, or NULL while the thread looks for it (or
 * if there is none); asks the thread the first time */
static pixman_image_t* icon( const char* name, int px ) {
    for (int i = 0; i < L.nicons; i++)
        if (L.icons[i].px == px && !strcmp( L.icons[i].name, name )) return L.icons[i].img;
    if (!ix) return NULL;
    if (L.nicons == MAX_ICONS) {
        /* the oldest goes, unless it is still wanted */
        if (!L.icons[0].done) return NULL;
        free( L.icons[0].name );
        if (L.icons[0].img) pixman_image_unref( L.icons[0].img );
        memmove( L.icons, L.icons + 1, (MAX_ICONS - 1) * sizeof(*L.icons) );
        L.nicons--;
    }
    msg_t* m = calloc( 1, sizeof(*m) );
    char* copy = strdup( name );
    if (!m || !copy || !(m->name = strdup( name ))) {
        free( m );
        free( copy );
        return NULL;
    }
    m->px = px;
    L.icons[L.nicons++] = (icon_t){ copy, px, 0, NULL };
    pthread_mutex_lock( &ix->lock );
    m->next = ix->icons_wanted;
    ix->icons_wanted = m;
    pthread_mutex_unlock( &ix->lock );
    awl_thread_wake( &ix->thread );
    L.waiting++;
    awl_host->timer_set( L.timer, ICON_POLL_MS );
    return NULL;
}

/** what the thread found: into the cache; redraws if any is shown */
static void icons_take( void ) {
    if (!ix) return;
    pthread_mutex_lock( &ix->lock );
    msg_t* found = ix->icons_found;
    ix->icons_found = NULL;
    pthread_mutex_unlock( &ix->lock );
    int any = 0;
    for (msg_t* m = found; m; m = m->next)
        for (int i = 0; i < L.nicons; i++)
            if (!L.icons[i].done && L.icons[i].px == m->px && !strcmp( L.icons[i].name, m->name )) {
                L.icons[i].done = 1;
                L.icons[i].img = m->img;
                m->img = NULL;
                L.waiting--;
                any = 1;
                break;
            }
    msgs_free( found );
    if (any) awl_host->window_redraw( L.win );
}

/** while it is open and waits: for icons, or for the first index */
static void index_take( void );
static void timer_fire( void* data ) {
    if (!awl_host->window_monitor( L.win )) return;
    if (ix && atomic_load( &ix->pending )) {
        index_take();
        awl_host->window_redraw( L.win );
    }
    icons_take();
    if (L.waiting > 0 || !L.idx) awl_host->timer_set( L.timer, ICON_POLL_MS );
}

/** the score of en for q, the best of its fields (Name's counts most);
 * returns 0 if none matches */
static int score_of( const awl_fuzzy_query_t* q, const entry_t* en, int* score ) {
    /* what the other fields lose against Name */
    enum { OTHER = 10 };
    const char* fields[] = { en->e.name, en->e.generic, en->e.keywords, en->command };
    int best = 0, found = 0;
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        int s;
        if (!fields[i] || !awl_fuzzy_match( q, fields[i], &s, NULL, NULL )) continue;
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
    const entry_t *ex = &L.idx->v[x->entry], *ey = &L.idx->v[y->entry];
    if (ex->frecency != ey->frecency) return ex->frecency < ey->frecency ? 1 : -1;
    /* for a query the shorter name (more of it matched), else by name */
    const size_t lx = strlen( ex->e.name ), ly = strlen( ey->e.name );
    if (L.query[0] && lx != ly) return lx < ly ? -1 : 1;
    return strcasecmp( ex->e.name, ey->e.name );
}

/** matches the query against the index, the best first */
static void filter( void ) {
    awl_fuzzy_query_t q;
    awl_fuzzy_query( &q, L.query );
    L.nres = 0;
    L.sel = L.top = 0;
    if (!L.idx) return;
    result_t* res = realloc( L.res, (size_t)(L.idx->n ? L.idx->n : 1) * sizeof(*res) );
    if (!res) return;
    L.res = res;
    for (int i = 0; i < L.idx->n; i++) {
        int s;
        if (!score_of( &q, &L.idx->v[i], &s )) continue;
        /* an empty query: the frecency alone, then the names */
        L.res[L.nres++] = (result_t){ i, q.n ? s + frecency_bonus( L.idx->v[i].frecency ) : 0 };
    }
    qsort( L.res, (size_t)L.nres, sizeof(*L.res), result_cmp );
}

/** a new index from the thread, if there is one */
static void index_take( void ) {
    index_t* idx = ix ? atomic_exchange( &ix->pending, NULL ) : NULL;
    if (!idx) return;
    index_free( L.idx );
    L.idx = idx;
    const int sel = L.sel;
    filter();
    L.sel = sel < L.nres ? sel : 0;
}

/* ---- the painter ---- */

static int px( float v ) {
    return (int)lroundf( v * L.scale );
}

/** the icons' size, physical pixels; the cache is for the size shown */
static void icon_size( int ipx ) {
    if (ipx == L.icon_px) return;
    icons_clear();
    L.icon_px = ipx;
}

/** whether en gets room for an icon in the bar layout: it names one that
 * isn't known to be missing at the size shown (one still on its way keeps
 * its room, so the row only moves if it turns out to be missing) */
static int icon_slot( const entry_t* en ) {
    if (!en->e.icon) return 0;
    for (int i = 0; i < L.nicons; i++)
        if (L.icons[i].px == L.icon_px && !strcmp( L.icons[i].name, en->e.icon ))
            return !L.icons[i].done || L.icons[i].img;
    return 1;
}

/** result r's width in the bar layout: its icon and name, padded; its name
 * at most name_w */
static int item_w( struct fcft_font* font, int r, int name_w ) {
    const entry_t* en = &L.idx->v[L.res[r].entry];
    int nw = (int)awl_host->text_width( font, en->e.name );
    if (nw > name_w) nw = name_w;
    return 2 * L.pad + (icon_slot( en ) ? L.icon_px + L.gap : 0) + nw;
}

/** lays out the results from top on beside the query, as many as fit;
 * returns whether the chosen one is among them */
static int lay_out_bar( struct fcft_font* font, int name_w, int spacing ) {
    int x = L.input_w;
    L.nshown = 0;
    for (int r = L.top; r < L.nres && L.nshown < MAX_SHOWN; r++) {
        int w = item_w( font, r, name_w );
        if (x + w > L.w) {
            if (L.nshown) break;
            w = L.w - x; /* one alone, cut */
        }
        L.bx[L.nshown] = x;
        L.bw[L.nshown++] = w;
        x += w + spacing;
    }
    return L.sel >= L.top && L.sel < L.top + L.nshown;
}

/* the bar layout: the bar's spot (AWL_WINDOW_AT_BAR); the query, then the
 * results in a row, a page of them: one that starts at the chosen one going
 * right, one that ends with it going left */
static int measure_bar( struct fcft_font* font, int max_w, int max_h, int* w, int* h ) {
    L.w = max_w;
    L.h = max_h;
    L.pad = px( 6 );
    L.gap = px( 6 );
    const int ipx = px( (float)CFG->icon_size ), room = L.h - 2 * px( 2 );
    icon_size( ipx < room ? ipx : room );
    /* the query's width at least, more as it grows, the results right after */
    const char* prompt = CFG->prompt ? CFG->prompt : "";
    const int typed = 2 * L.pad + px( 3 ) + (int)awl_host->text_width( font, prompt ) +
                      (int)awl_host->text_width( font, L.query );
    L.input_w = px( (float)CFG->input_width );
    if (L.input_w < typed) L.input_w = typed;
    if (L.input_w > L.w / 3) L.input_w = L.w / 3;
    if (L.w <= L.input_w || L.h < 1) return 0;
    const int name_w = (L.w - L.input_w) / 3, spacing = px( 4 );
    L.nshown = 0;
    if (L.nres) {
        if (L.sel < L.top) {
            /* the page before: as much as fits that ends with the chosen one */
            int x = L.input_w + item_w( font, L.sel, name_w );
            L.top = L.sel;
            while (L.top > 0 && (x += spacing + item_w( font, L.top - 1, name_w )) <= L.w) L.top--;
        }
        if (!lay_out_bar( font, name_w, spacing )) {
            L.top = L.sel;
            lay_out_bar( font, name_w, spacing );
        }
    }
    *w = L.w;
    *h = L.h;
    return 1;
}

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    L.scale = scale;
    if (bar()) return measure_bar( font, max_w, max_h, w, h );
    const int fh = font->height;
    L.pad = px( 6 );
    L.gap = px( 10 );
    icon_size( px( (float)CFG->icon_size ) );
    L.input_h = fh + 2 * L.pad;
    L.sep_h = px( 7 );
    L.row_h = (fh > L.icon_px ? fh : L.icon_px) + 2 * px( 3 );
    L.w = px( (float)CFG->width );
    if (L.w > max_w) L.w = max_w;
    L.rows = CFG->rows;
    if (L.input_h + L.sep_h + L.rows * L.row_h > max_h) L.rows = (max_h - L.input_h - L.sep_h) / L.row_h;
    if (L.rows < 1) return 0;
    *w = L.w;
    *h = L.input_h + L.sep_h + L.rows * L.row_h;
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
 * query's tail if it is too long, and the cursor after it */
static void paint_query( pixman_image_t* pix, struct fcft_font* font, int x, int right, int y, int h, uint32_t fg,
                         uint32_t dim ) {
    const char* prompt = CFG->prompt ? CFG->prompt : "";
    if (*prompt) awl_host->text_over( pix, font, x, y, (unsigned)(right - x), (unsigned)h, prompt, dim );
    x += (int)awl_host->text_width( font, prompt );
    const int cursor_w = px( 1.5f ) > 1 ? px( 1.5f ) : 1;
    const char* q = L.query;
    while (*q && x + (int)awl_host->text_width( font, q ) + cursor_w > right) {
        q++;
        while (((unsigned char)*q & 0xc0) == 0x80) q++;
    }
    if (*q) awl_host->text_over( pix, font, x, y, (unsigned)(right - x), (unsigned)h, q, fg );
    x += (int)awl_host->text_width( font, q );
    awl_fill_shape( pix, (float)x + px( 1 ), (float)(y + (h - font->height) / 2), (float)cursor_w,
                    (float)font->height, 0, 0, fg );
}

/** en's icon, if it has one, centered in an icon's width at x and h at y */
static void paint_icon( pixman_image_t* pix, const entry_t* en, int x, int y, int h ) {
    pixman_image_t* img = en->e.icon ? icon( en->e.icon, L.icon_px ) : NULL;
    if (!img) return;
    const int iw = pixman_image_get_width( img ), ih = pixman_image_get_height( img );
    pixman_image_composite32( PIXMAN_OP_OVER, img, NULL, pix, 0, 0, 0, 0, x + (L.icon_px - iw) / 2, y + (h - ih) / 2,
                              iw, ih );
}

/** what the query matched in en's name, into pos; returns how many */
static int marks( const awl_fuzzy_query_t* fq, const entry_t* en, int* pos ) {
    return fq->n && awl_fuzzy_match( fq, en->e.name, NULL, NULL, pos ) ? fq->n : 0;
}

static void paint_bar( pixman_image_t* pix, struct fcft_font* font, int x0, int y0, uint32_t fg, uint32_t dim,
                       uint32_t match ) {
    const awl_popup_config_t* pc = &awl_config()->popup;
    paint_query( pix, font, x0 + L.pad, x0 + L.input_w - L.pad, y0, L.h, fg, dim );
    if (!L.nres) {
        awl_host->text_over( pix, font, x0 + L.input_w + L.pad, y0, (unsigned)(L.w - L.input_w - L.pad),
                             (unsigned)L.h, L.idx ? "no match" : "reading the applications…", dim );
        return;
    }
    awl_fuzzy_query_t fq;
    awl_fuzzy_query( &fq, L.query );
    const int inset = px( 2 );
    for (int i = 0; i < L.nshown; i++) {
        const int r = L.top + i, x = x0 + L.bx[i];
        const entry_t* en = &L.idx->v[L.res[r].entry];
        if (r == L.sel)
            awl_fill_shape( pix, (float)x, (float)(y0 + inset), (float)L.bw[i], (float)(L.h - 2 * inset),
                            (float)px( pc->radius < 6 ? (float)pc->radius : 6 ), 0, pc->hover );
        int tx = x + L.pad;
        if (icon_slot( en )) {
            paint_icon( pix, en, tx, y0, L.h );
            tx += L.icon_px + L.gap;
        }
        int pos[AWL_FUZZY_MAX_QUERY];
        const int n = marks( &fq, en, pos );
        text_marked( pix, font, en->e.name, pos, n, tx, y0, x + L.bw[i] - L.pad, L.h, fg, match );
    }
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0, int hover ) {
    const awl_popup_config_t* pc = &awl_config()->popup;
    const uint32_t fg = pc->fg, dim = CFG->dim ? CFG->dim : pc->dim, match = CFG->match ? CFG->match : fg;
    if (bar()) {
        paint_bar( pix, font, x0, y0, fg, dim, match );
        return;
    }

    int x, right;
    paint_query( pix, font, x0 + L.pad, x0 + L.w - L.pad, y0, L.input_h, fg, dim );

    const float t = fmaxf( 1, (float)px( 1 ) );
    awl_fill_shape( pix, (float)x0, (float)(y0 + L.input_h) + ((float)L.sep_h - t) / 2, (float)L.w, t, 0, 0, dim );

    const int ry0 = y0 + L.input_h + L.sep_h;
    if (!L.nres) {
        awl_host->text_over( pix, font, x0 + L.pad, ry0, (unsigned)(L.w - 2 * L.pad), (unsigned)L.row_h,
                             L.idx ? "no match" : "reading the applications…", dim );
        return;
    }
    awl_fuzzy_query_t fq;
    awl_fuzzy_query( &fq, L.query );
    for (int i = 0; i < L.rows && L.top + i < L.nres; i++) {
        const int r = L.top + i, y = ry0 + i * L.row_h;
        const entry_t* en = &L.idx->v[L.res[r].entry];
        if (r == L.sel)
            awl_fill_shape( pix, (float)x0, (float)y, (float)L.w, (float)L.row_h,
                            (float)px( pc->radius < 6 ? (float)pc->radius : 6 ), 0, pc->hover );
        x = x0 + L.pad;
        paint_icon( pix, en, x, y, L.row_h );
        x += L.icon_px + L.gap;
        right = x0 + L.w - L.pad;
        /* the name, what the query matched in it marked */
        int pos[AWL_FUZZY_MAX_QUERY];
        const int n = marks( &fq, en, pos );
        x = text_marked( pix, font, en->e.name, pos, n, x, y, right, L.row_h, fg, match );
        if (en->e.generic && x + L.gap < right)
            awl_host->text_over( pix, font, x + L.gap, y, (unsigned)(right - x - L.gap), (unsigned)L.row_h,
                                 en->e.generic, dim );
    }
}

/** the result row (the bar layout's: the result shown) at px, py; -1 the
 * query, or none */
static int hit( void* data, int x, int y ) {
    if (bar()) {
        for (int i = 0; i < L.nshown; i++)
            if (x >= L.bx[i] && x < L.bx[i] + L.bw[i] && y >= 0 && y < L.h) return i;
        return -1;
    }
    const int ry0 = L.input_h + L.sep_h;
    if (x < 0 || x >= L.w || y < ry0) return -1;
    const int i = (y - ry0) / L.row_h;
    return i < L.rows && L.top + i < L.nres ? i : -1;
}

static int box( void* data, int part, int* x, int* y, int* w, int* h ) {
    if (bar()) {
        if (part < 0 || part >= L.nshown) return 0;
        *x = L.bx[part];
        *y = 0;
        *w = L.bw[part];
        *h = L.h;
        return 1;
    }
    if (part < 0 || part >= L.rows) return 0;
    *x = 0;
    *y = L.input_h + L.sep_h + part * L.row_h;
    *w = L.w;
    *h = L.row_h;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- choosing and launching ---- */

/** moves the choice to row r, scrolled into view (the bar layout's paged
 * by measure_bar()) */
static void choose( int r ) {
    if (!L.nres) return;
    if (r < 0) r = 0;
    if (r >= L.nres) r = L.nres - 1;
    L.sel = r;
    if (bar()) {
        awl_host->window_redraw( L.win );
        return;
    }
    if (L.sel < L.top) L.top = L.sel;
    if (L.rows > 0 && L.sel >= L.top + L.rows) L.top = L.sel - L.rows + 1;
    awl_host->window_redraw( L.win );
}

static void close_launcher( void ) {
    awl_host->window_hide( L.win );
}

/** counts a launch of id on the thread */
static void count_launch( const char* id ) {
    msg_t* m = calloc( 1, sizeof(*m) );
    if (!ix || !m || !(m->name = strdup( id ))) {
        free( m );
        return;
    }
    pthread_mutex_lock( &ix->lock );
    m->next = ix->launched;
    ix->launched = m;
    pthread_mutex_unlock( &ix->lock );
    awl_thread_wake( &ix->thread );
}

/** starts result r: its argv, in the terminal if it wants one, in its
 * working directory if it has one (through sh: spawn has none) */
static void launch( int r ) {
    if (r < 0 || r >= L.nres) return;
    entry_t* en = &L.idx->v[L.res[r].entry];
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
        awl_host->actions->spawn( &(Arg){ .v = full } );
        free( full );
    }
    awl_desktop_argv_free( argv );
    en->frecency += 4;
    count_launch( en->id );
    close_launcher();
}

/** the query lost its last character (word: its last word, and the
 * blanks after it) */
static void query_back( int word ) {
    size_t n = strlen( L.query );
    if (word)
        while (n && L.query[n - 1] == ' ') n--;
    do {
        while (n && ((unsigned char)L.query[n - 1] & 0xc0) == 0x80) n--;
        if (n) n--;
    } while (word && n && L.query[n - 1] != ' ');
    L.query[n] = '\0';
}

/** results a page up or down moves */
static int page( void ) {
    const int n = bar() ? L.nshown : L.rows;
    return n > 0 ? n : 1;
}

static int key( awl_window_t* win, uint32_t sym, const char* utf8, uint32_t mods, void* data ) {
    const int ctrl = mods & WLR_MODIFIER_CTRL, other = mods & (WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
    if (other) return 0;
    if (bar() && (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) {
        choose( L.sel + (sym == XKB_KEY_Left ? -1 : 1) );
        return 1;
    }
    switch (sym) {
    case XKB_KEY_Escape: close_launcher(); return 1;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter: launch( L.sel ); return 1;
    case XKB_KEY_Up:
    case XKB_KEY_ISO_Left_Tab: choose( L.sel - 1 ); return 1;
    case XKB_KEY_Down:
    case XKB_KEY_Tab: choose( L.sel + 1 ); return 1;
    case XKB_KEY_Page_Up: choose( L.sel - page() ); return 1;
    case XKB_KEY_Page_Down: choose( L.sel + page() ); return 1;
    case XKB_KEY_BackSpace:
        query_back( ctrl );
        filter();
        awl_host->window_redraw( win );
        return 1;
    }
    if (ctrl) {
        switch (sym) {
        case XKB_KEY_p: choose( L.sel - 1 ); return 1;
        case XKB_KEY_n: choose( L.sel + 1 ); return 1;
        case XKB_KEY_w: query_back( 1 ); break;
        case XKB_KEY_u: L.query[0] = '\0'; break;
        default: return 0;
        }
        filter();
        awl_host->window_redraw( win );
        return 1;
    }
    if (!*utf8) return 0;
    const size_t n = strlen( L.query ), add = strlen( utf8 );
    if (n + add < sizeof L.query) {
        memcpy( L.query + n, utf8, add + 1 );
        filter();
        awl_host->window_redraw( win );
    }
    return 1;
}

static void click( awl_window_t* win, int row, int col, uint32_t button, void* data ) {
    if (row >= 0 && button == BTN_LEFT) launch( L.top + row );
}

static void scroll( awl_window_t* win, int row, int col, uint32_t orientation, double delta, int32_t value120,
                    void* data ) {
    static double acc;
    /* the bar layout's row goes sideways too */
    if (orientation != 0 && !bar()) return;
    acc += value120 ? value120 / 120.0 : delta / 10.0;
    const int steps = (int)acc;
    if (!steps) return;
    acc -= steps;
    choose( L.sel + steps );
}

static void hover( awl_window_t* win, int row, int inside, void* data ) {
    if (row >= 0 && L.top + row != L.sel) choose( L.top + row );
}

static void hidden( awl_window_t* win, void* data ) {
    L.query[0] = '\0';
    L.nres = L.sel = L.top = L.nshown = 0;
}

void awl_launcher_toggle( void ) {
    if (awl_host->window_monitor( L.win )) {
        close_launcher();
        return;
    }
    Monitor* m = awl_host->monitor_selected();
    if (!m) return;
    if (!L.win) {
        if (!(L.win = awl_host->window_create())) return;
        awl_host->window_set_painter( L.win, &painter, NULL );
        awl_host->window_set_click( L.win, click, NULL );
        awl_host->window_set_scroll( L.win, scroll, NULL );
        awl_host->window_set_hover( L.win, hover, NULL );
        awl_host->window_set_hidden( L.win, hidden, NULL );
        awl_host->window_set_key( L.win, key, NULL );
    }
    if (!L.timer) L.timer = awl_host->timer_add( timer_fire, NULL );
    index_take();
    L.query[0] = '\0';
    filter();
    if (bar())
        awl_host->window_show_at( L.win, m, CFG->strip_at, 0, 0 );
    else
        awl_host->window_show_at( L.win, m, CFG->at, 0, CFG->dy );
    /* no index yet: it comes while it is open */
    if (!L.idx) awl_host->timer_set( L.timer, ICON_POLL_MS );
}

void awl_launcher_start( void ) {
    indexer_t* x = calloc( 1, sizeof(*x) );
    awl_dict_val_t v;
    if (!x) return;
    pthread_mutex_init( &x->lock, NULL );
    const char* theme = awl_config()->tray.icon_theme;
    x->icon_theme = theme ? strdup( theme ) : NULL;
    AWL_HOST_CFG_LOCK( awl_host );
    if (AWL_HOST_CFG_GET( awl_host, "state_home", &v ) == AWL_DICT_OK && v.kind == AWL_DICT_KIND_STR)
        x->state_home = strdup( v.str );
    AWL_HOST_CFG_UNLOCK( awl_host );
    if (awl_thread_start( &x->thread, "launcher", indexer, x )) {
        free( x->icon_theme );
        free( x->state_home );
        free( x );
        return;
    }
    ix = x;
}

int awl_launcher_stop( void ) {
    awl_host->window_destroy( L.win );
    awl_host->timer_remove( L.timer );
    L.win = NULL;
    L.timer = NULL;
    icons_clear();
    index_free( L.idx );
    L.idx = NULL;
    free( L.res );
    L.res = NULL;
    L.nres = 0;
    indexer_t* x = ix;
    ix = NULL;
    if (!x) return 0;
    /* reads files: may hang on a dead mount */
    if (awl_thread_stop( &x->thread, 500 )) return 1;
    index_free( atomic_load( &x->pending ) );
    msgs_free( x->icons_wanted );
    msgs_free( x->launched );
    msgs_free( x->icons_found );
    pthread_mutex_destroy( &x->lock );
    free( x->icon_theme );
    free( x->state_home );
    free( x );
    return 0;
}
