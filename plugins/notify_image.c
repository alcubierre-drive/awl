/* The notifications' images from files and icon names, see notify_image.h:
 * wishes go to the thread under a lock, what came of them back the same
 * way; the main thread polls for it (notify.c's timer). */

#include "notify_image.h"
#include "desktop_entry.h"
#include "thread.h"
#include "../icons/icon.h"
#include "../plugins.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** a notification's files: a 12 megapixel phone photo (4032 x 3024) or a
 * 4K screenshot, in up to 16 MiB; the decoder's memory fits a progressive
 * 4:2:0 JPEG that large (52 MiB) and refuses a progressive 4:4:4 one past
 * 7 megapixels. 128 MiB at the very most with the file and the pixels. */
static const awl_icon_limits_t LIMITS = {
    .bytes = 16u << 20, .dim = 8192, .pixels = 4096u * 3072, .work = 64u << 20,
};

/** decoded files kept for the next notifications (an app sends the same
 * icon each time): the most recently used, this many and bytes of pixels
 * at most */
enum { CACHE_N = 32, CACHE_BYTES = 8 << 20 };

/** a desktop entry's file read at most */
enum { MAX_DESKTOP_FILE = 64 << 10 };

typedef struct wish {
    awl_notify_image_t r;
    /** image_path, app_icon, desktop_entry; NULL none */
    char* src[3];
} wish_t;

/** a file decoded at px, known by what stat() says of it: a changed file
 * is decoded again; img NULL if it couldn't be */
typedef struct {
    char* path;
    int px;
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime;
    pixman_image_t* img;
    /** when last used, a count */
    uint64_t used;
} cached_t;

typedef struct {
    awl_thread_t thread;
    /** set before the thread starts, then read-only */
    char* icon_theme;
    pthread_mutex_t lock;
    /** under lock: main -> thread, newest first; thread -> main, oldest
     * first */
    wish_t *wanted, *done;
    /** the thread's alone */
    cached_t cache[CACHE_N];
    size_t cache_bytes;
    uint64_t uses;
} worker_t;

static worker_t* W;

static void wish_free( wish_t* w ) {
    for (int i = 0; i < 3; i++) free( w->src[i] );
    if (w->r.img) pixman_image_unref( w->r.img );
    free( w );
}

static void wishes_free( wish_t* w ) {
    for (wish_t* next; w; w = next) {
        next = (wish_t*)w->r.next;
        wish_free( w );
    }
}

/* ---- the thread ---- */

static int hex( char c ) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** a file:// URL's path, percent-decoded, into out; 0 for another host
 * than localhost, a bad escape or a NUL */
static int url_path( const char* url, char* out, size_t size ) {
    const char* p = url + strlen( "file://" );
    if (!strncmp( p, "localhost/", strlen( "localhost/" ) )) p += strlen( "localhost" );
    if (*p != '/') return 0;
    size_t n = 0;
    for (; *p; p++) {
        char c = *p;
        if (c == '%') {
            const int a = hex( p[1] ), b = a < 0 ? -1 : hex( p[2] );
            if (b < 0 || !(c = (char)(a << 4 | b))) return 0;
            p += 2;
        }
        if (n + 1 >= size) return 0;
        out[n++] = c;
    }
    out[n] = '\0';
    return 1;
}

static size_t image_bytes( pixman_image_t* img ) {
    return img ? (size_t)pixman_image_get_stride( img ) * (size_t)pixman_image_get_height( img ) : 0;
}

/** a copy of img: what the main thread gets is its own (pixman's reference
 * count isn't atomic, and it sets a transform on it) */
static pixman_image_t* image_copy( pixman_image_t* img ) {
    const int w = pixman_image_get_width( img ), h = pixman_image_get_height( img );
    pixman_image_t* out = pixman_image_create_bits( PIXMAN_a8r8g8b8, w, h, NULL, 0 );
    if (out && pixman_image_get_stride( out ) == pixman_image_get_stride( img ))
        memcpy( pixman_image_get_data( out ), pixman_image_get_data( img ), image_bytes( img ) );
    else if (out)
        pixman_image_composite32( PIXMAN_OP_SRC, img, NULL, out, 0, 0, 0, 0, 0, 0, w, h );
    return out;
}

static void cached_clear( worker_t* x, cached_t* c ) {
    x->cache_bytes -= image_bytes( c->img );
    if (c->img) pixman_image_unref( c->img );
    free( c->path );
    *c = (cached_t){ 0 };
}

static void cache_free( worker_t* x ) {
    for (int i = 0; i < CACHE_N; i++) cached_clear( x, &x->cache[i] );
}

/** path decoded at px, from the cache if the file is unchanged, else
 * decoded and kept; the caller's own image */
static pixman_image_t* load( worker_t* x, const char* path, int px ) {
    struct stat st;
    /* the decode checks it is a regular file once opened, the cache's key
     * is told by this */
    if (stat( path, &st ) || !S_ISREG( st.st_mode )) return NULL;
    cached_t* c = NULL;
    for (int i = 0; i < CACHE_N && !c; i++) {
        cached_t* k = &x->cache[i];
        if (k->path && k->px == px && !strcmp( k->path, path )) c = k;
    }
    if (c && c->dev == st.st_dev && c->ino == st.st_ino && c->size == st.st_size &&
        c->mtime.tv_sec == st.st_mtim.tv_sec && c->mtime.tv_nsec == st.st_mtim.tv_nsec) {
        c->used = ++x->uses;
        awl_vrb_printf( "notify: image %s at %d from the cache\n", path, px );
        return c->img ? image_copy( c->img ) : NULL;
    }
    if (c) cached_clear( x, c );
    pixman_image_t* img = awl_icon_load_limited( path, px, px, &LIMITS );
    const size_t bytes = image_bytes( img );
    char* key = bytes <= CACHE_BYTES ? strdup( path ) : NULL;
    if (!key) return img;
    pixman_image_t* mine = img ? image_copy( img ) : NULL;
    if (img && !mine) {
        free( key );
        return img;
    }
    /* room: the least recently used go, a free slot is taken */
    while (1) {
        cached_t* lru = NULL;
        int n = 0;
        for (int i = 0; i < CACHE_N; i++) {
            cached_t* k = &x->cache[i];
            if (!k->path) {
                if (!c) c = k;
                continue;
            }
            n++;
            if (!lru || k->used < lru->used) lru = k;
        }
        if (!lru || (n < CACHE_N && x->cache_bytes + bytes <= CACHE_BYTES)) break;
        cached_clear( x, lru );
    }
    *c = (cached_t){ .path = key, .px = px, .dev = st.st_dev, .ino = st.st_ino, .size = st.st_size,
                     .mtime = st.st_mtim, .img = img, .used = ++x->uses };
    x->cache_bytes += bytes;
    return mine;
}

/** src's image at px: a path, a file:// URL, or an icon name in icons */
static pixman_image_t* resolve( worker_t* x, awl_icons_t* icons, const char* src, int px, int* symbolic ) {
    char path[PATH_MAX];
    unsigned flags = 0;
    *symbolic = 0;
    if (!strncmp( src, "file://", strlen( "file://" ) )) {
        if (!url_path( src, path, sizeof path )) return NULL;
        src = path;
    }
    char* found = icons ? awl_icons_lookup( icons, src, px, NULL, &flags ) : NULL;
    if (!found && *src == '/') found = strdup( src );
    if (!found) return NULL;
    pixman_image_t* img = load( x, found, px );
    free( found );
    *symbolic = img && (flags & AWL_ICON_SYMBOLIC);
    return img;
}

/** the Icon of the desktop entry id (with or without ".desktop"): the
 * first applications directory's file of that name, malloc()ed; NULL if
 * there is none, or id isn't a plain file name */
static char* desktop_icon( const char* id ) {
    size_t k = strlen( id );
    if (k > 8 && !strcmp( id + k - 8, ".desktop" )) k -= 8;
    if (!k || k > NAME_MAX - 8 || *id == '.' || memchr( id, '/', k )) return NULL;
    const char* home = getenv( "HOME" );
    const char* data_home = getenv( "XDG_DATA_HOME" );
    const char* data_dirs = getenv( "XDG_DATA_DIRS" );
    char path[PATH_MAX], first[PATH_MAX];
    if (!data_dirs || !*data_dirs) data_dirs = "/usr/local/share:/usr/share";
    if (data_home && *data_home) snprintf( first, sizeof first, "%s", data_home );
    else if (home) snprintf( first, sizeof first, "%s/.local/share", home );
    else first[0] = '\0';
    for (const char* p = data_dirs;;) {
        const char* dir;
        size_t dk;
        if (first[0]) {
            dir = first;
            dk = strlen( first );
        } else if (*p) {
            dir = p;
            dk = strcspn( p, ":" );
            p += dk + (p[dk] == ':');
        } else {
            return NULL;
        }
        const int n = snprintf( path, sizeof path, "%.*s/applications/%.*s.desktop", (int)dk, dir, (int)k, id );
        first[0] = '\0';
        if (!dk || n < 0 || (size_t)n >= sizeof path) continue;
        size_t len;
        char* data = awl_icon_read( path, &len );
        if (!data) continue;
        char* text = len <= MAX_DESKTOP_FILE ? malloc( len + 1 ) : NULL;
        if (text) {
            memcpy( text, data, len );
            text[len] = '\0';
        }
        free( data );
        awl_desktop_entry_t e;
        char* icon = NULL;
        if (text && awl_desktop_entry_parse( &e, text, NULL, 0, NULL ) == 1) {
            icon = e.icon;
            e.icon = NULL;
            awl_desktop_entry_clear( &e );
        }
        free( text );
        /* the first file of the id is the entry, an icon or not */
        return icon;
    }
}

static void grant( worker_t* x, awl_icons_t* icons, wish_t* w ) {
    for (int i = 0; i < 3 && !w->r.img; i++) {
        if (!w->src[i]) continue;
        if (i < 2) {
            w->r.img = resolve( x, icons, w->src[i], w->r.px, &w->r.symbolic );
            continue;
        }
        char* icon = desktop_icon( w->src[i] );
        if (icon) w->r.img = resolve( x, icons, icon, w->r.px, &w->r.symbolic );
        free( icon );
    }
}

static void* worker( void* data ) {
    worker_t* x = data;
    awl_icons_t* icons = awl_icons_new_desktop( x->icon_theme );
    while (1) {
        struct pollfd fd = { .fd = x->thread.wake_fd, .events = POLLIN };
        if (poll( &fd, 1, -1 ) < 0 && errno != EINTR) break;
        if (!(fd.revents & POLLIN)) continue;
        if (awl_thread_woken( &x->thread )) break;
        pthread_mutex_lock( &x->lock );
        wish_t* wanted = x->wanted;
        x->wanted = NULL;
        pthread_mutex_unlock( &x->lock );
        /* oldest first; each handed back as it is done */
        wish_t* order = NULL;
        for (wish_t *w = wanted, *next; w; w = next) {
            next = (wish_t*)w->r.next;
            w->r.next = (awl_notify_image_t*)order;
            order = w;
        }
        for (wish_t *w = order, *next; w; w = next) {
            next = (wish_t*)w->r.next;
            if (atomic_load( &x->thread.quit )) {
                wish_free( w );
                continue;
            }
            grant( x, icons, w );
            w->r.next = NULL;
            pthread_mutex_lock( &x->lock );
            wish_t** tail = &x->done;
            while (*tail) tail = (wish_t**)&(*tail)->r.next;
            *tail = w;
            pthread_mutex_unlock( &x->lock );
        }
    }
    cache_free( x );
    awl_icons_free( icons );
    return NULL;
}

/* ---- the main thread ---- */

static worker_t* worker_get( void ) {
    if (W) return W;
    worker_t* x = calloc( 1, sizeof(*x) );
    if (!x) return NULL;
    const char* theme = awl_config()->tray.icon_theme;
    x->icon_theme = theme ? strdup( theme ) : NULL;
    pthread_mutex_init( &x->lock, NULL );
    if (awl_thread_start( &x->thread, "notifyimg", worker, x )) {
        pthread_mutex_destroy( &x->lock );
        free( x->icon_theme );
        free( x );
        return NULL;
    }
    return W = x;
}

static char* copy_src( const char* s ) {
    return s && *s ? strdup( s ) : NULL;
}

int awl_notify_image_want( uint32_t id, uint64_t serial, int px, const char* image_path, const char* app_icon,
                           const char* desktop_entry ) {
    const char* src[3] = { image_path, app_icon, desktop_entry };
    int any = 0;
    for (int i = 0; i < 3; i++) any |= src[i] && *src[i];
    if (!any || px < 1) return 0;
    if (px > AWL_ICON_MAX_DIM) px = AWL_ICON_MAX_DIM;
    worker_t* x = worker_get();
    wish_t* w = x ? calloc( 1, sizeof(*w) ) : NULL;
    if (!w) return 0;
    w->r = (awl_notify_image_t){ .id = id, .serial = serial, .px = px };
    for (int i = 0; i < 3; i++)
        if (src[i] && *src[i] && !(w->src[i] = copy_src( src[i] ))) {
            wish_free( w );
            return 0;
        }
    pthread_mutex_lock( &x->lock );
    w->r.next = (awl_notify_image_t*)x->wanted;
    x->wanted = w;
    pthread_mutex_unlock( &x->lock );
    awl_thread_wake( &x->thread );
    return 1;
}

awl_notify_image_t* awl_notify_image_take( void ) {
    if (!W) return NULL;
    pthread_mutex_lock( &W->lock );
    wish_t* done = W->done;
    W->done = NULL;
    pthread_mutex_unlock( &W->lock );
    /* the sources aren't the caller's business */
    for (wish_t* w = done; w; w = (wish_t*)w->r.next)
        for (int i = 0; i < 3; i++) {
            free( w->src[i] );
            w->src[i] = NULL;
        }
    return done ? &done->r : NULL;
}

void awl_notify_image_free( awl_notify_image_t* list ) {
    /* r is a wish's first member */
    wishes_free( (wish_t*)list );
}

int awl_notify_image_stop( void ) {
    worker_t* x = W;
    W = NULL;
    if (!x) return 0;
    /* reads files: may hang on a dead mount */
    if (awl_thread_stop( &x->thread, 500 )) return 1;
    wishes_free( x->wanted );
    wishes_free( x->done );
    pthread_mutex_destroy( &x->lock );
    free( x->icon_theme );
    free( x );
    return 0;
}
