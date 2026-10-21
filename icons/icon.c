/* Icons without GTK: see icon.h. The lookup follows the Icon Theme
 * Specification, sizes in pixels (Size times Scale) so that a fractional
 * scale finds the closest file; the folders each theme's directories have
 * are found once, when the theme is read, so a lookup only tries the files
 * that may exist. */

#define _GNU_SOURCE
#include "icon.h"
#include "../awl_log.h"
#include "../vendor/wuffs/wuffs.h"
#include "../vendor/plutosvg/svg.h"

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    /** themes inheriting themes, at most this deep */
    MAX_DEPTH = 16,
    /** lookups remembered, the oldest forgotten first */
    CACHE = 256,
};

/** one of a theme's directories, and the folders it is in */
typedef struct {
    int size, scale, min, max, threshold;
    /** 'F'ixed, 'S'calable, 'T'hreshold */
    char type;
    char** paths;
    int npaths;
} dir_t;

typedef struct {
    char* name;
    dir_t* dirs;
    int ndirs;
} theme_t;

typedef struct {
    /** "px name\nextra\n...", NULL: unused */
    char* key;
    char* path;
    unsigned flags;
} hit_t;

struct awl_icons {
    /** where themes are, in order */
    char** bases;
    int nbases;
    theme_t* themes;
    int nthemes;
    /** hicolor's, for extra's folders; NULL if it isn't installed */
    const theme_t* hicolor;
    pthread_mutex_t lock;
    hit_t cache[CACHE];
    int next;
};

static void* grow( void* p, int n, size_t size ) {
    void* q = realloc( p, (size_t)(n + 1) * size );
    if (!q) abort();
    return q;
}

static int is_dir( const char* path ) {
    struct stat st;
    return !stat( path, &st ) && S_ISDIR( st.st_mode );
}

static int has_suffix( const char* s, const char* suffix ) {
    size_t n = strlen( s ), m = strlen( suffix );
    return n >= m && !strcmp( s + n - m, suffix );
}

static void add_base( awl_icons_t* ic, const char* dir, const char* sub ) {
    if (!dir || dir[0] != '/') return;
    char* path;
    if (asprintf( &path, "%s%s", dir, sub ) < 0) return;
    for (int i = 0; i < ic->nbases; i++)
        if (!strcmp( ic->bases[i], path )) {
            free( path );
            return;
        }
    ic->bases = grow( ic->bases, ic->nbases, sizeof(*ic->bases) );
    ic->bases[ic->nbases++] = path;
}

/* ---- index.theme ---- */

/** an index.theme's entries, by section */
typedef struct {
    char *section, *key, *value;
} entry_t;

static char* trim( char* s ) {
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen( s );
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = 0;
    return s;
}

static entry_t* read_index( const char* path, int* n ) {
    *n = 0;
    FILE* f = fopen( path, "re" );
    if (!f) return NULL;
    entry_t* e = NULL;
    char *line = NULL, *section = NULL;
    size_t cap = 0;
    while (getline( &line, &cap, f ) > 0) {
        char* s = trim( line );
        if (*s == '[') {
            char* end = strchr( s, ']' );
            if (end) *end = 0;
            free( section );
            section = strdup( s + 1 );
            continue;
        }
        char* eq = strchr( s, '=' );
        /* localized keys aren't of interest */
        if (!section || *s == '#' || !eq || memchr( s, '[', (size_t)(eq - s) )) continue;
        *eq = 0;
        e = grow( e, *n, sizeof(*e) );
        e[*n] = (entry_t){ strdup( section ), strdup( trim( s ) ), strdup( trim( eq + 1 ) ) };
        (*n)++;
    }
    free( line );
    free( section );
    fclose( f );
    return e;
}

static const char* value( const entry_t* e, int n, const char* section, const char* key ) {
    for (int i = 0; i < n; i++)
        if (!strcmp( e[i].section, section ) && !strcmp( e[i].key, key )) return e[i].value;
    return NULL;
}

static int number( const entry_t* e, int n, const char* section, const char* key, int dflt ) {
    const char* v = value( e, n, section, key );
    return v && *v ? atoi( v ) : dflt;
}

static const char* dir_name( const dir_t* d );

static void add_dir( awl_icons_t* ic, theme_t* t, const entry_t* e, int n, const char* name ) {
    for (int i = 0; i < t->ndirs; i++)
        if (!strcmp( dir_name( &t->dirs[i] ), name )) return;
    const char* type = value( e, n, name, "Type" );
    dir_t d = { 0 };
    d.size = number( e, n, name, "Size", 0 );
    if (d.size <= 0) return;
    d.scale = number( e, n, name, "Scale", 1 );
    if (d.scale <= 0) d.scale = 1;
    d.min = number( e, n, name, "MinSize", d.size );
    d.max = number( e, n, name, "MaxSize", d.size );
    d.threshold = number( e, n, name, "Threshold", 2 );
    d.type = type && !strcmp( type, "Fixed" ) ? 'F' : type && !strcmp( type, "Scalable" ) ? 'S' : 'T';
    for (int i = 0; i < ic->nbases; i++) {
        char* path;
        if (asprintf( &path, "%s/%s/%s", ic->bases[i], t->name, name ) < 0) continue;
        if (!is_dir( path )) {
            free( path );
            continue;
        }
        d.paths = grow( d.paths, d.npaths, sizeof(*d.paths) );
        d.paths[d.npaths++] = path;
    }
    /* hicolor's directories are also looked for in extra's folders */
    if (!d.npaths && strcmp( t->name, "hicolor" )) return;
    d.paths = grow( d.paths, d.npaths, sizeof(*d.paths) );
    /* the directory's own name, last, past npaths */
    d.paths[d.npaths] = strdup( name );
    t->dirs = grow( t->dirs, t->ndirs, sizeof(*t->dirs) );
    t->dirs[t->ndirs++] = d;
}

static const char* dir_name( const dir_t* d ) {
    return d->paths[d->npaths];
}

static int loaded( const awl_icons_t* ic, const char* name ) {
    for (int i = 0; i < ic->nthemes; i++)
        if (!strcmp( ic->themes[i].name, name )) return 1;
    return 0;
}

/** theme name, then what it inherits (but hicolor, which comes last) */
static void add_theme( awl_icons_t* ic, const char* name, int depth ) {
    if (!name || !*name || strchr( name, '/' ) || depth > MAX_DEPTH || loaded( ic, name )) return;
    if (depth && !strcmp( name, "hicolor" )) return;
    entry_t* e = NULL;
    int n = 0;
    for (int i = 0; i < ic->nbases && !e; i++) {
        char* path;
        if (asprintf( &path, "%s/%s/index.theme", ic->bases[i], name ) < 0) continue;
        e = read_index( path, &n );
        free( path );
    }
    if (!e) return;
    theme_t t = { .name = strdup( name ) };
    /* the directories, those for scales above 1 too */
    const char* lists[] = { value( e, n, "Icon Theme", "Directories" ), value( e, n, "Icon Theme", "ScaledDirectories" ) };
    for (int l = 0; l < 2; l++) {
        char* copy = lists[l] ? strdup( lists[l] ) : NULL;
        char* save = NULL;
        for (char* d = copy ? strtok_r( copy, ",", &save ) : NULL; d; d = strtok_r( NULL, ",", &save ) ) {
            d = trim( d );
            if (*d) add_dir( ic, &t, e, n, d );
        }
        free( copy );
    }
    ic->themes = grow( ic->themes, ic->nthemes, sizeof(*ic->themes) );
    ic->themes[ic->nthemes++] = t;
    const char* inherits = value( e, n, "Icon Theme", "Inherits" );
    char* copy = inherits ? strdup( inherits ) : NULL;
    char* save = NULL;
    for (char* p = copy ? strtok_r( copy, ",", &save ) : NULL; p; p = strtok_r( NULL, ",", &save ) )
        add_theme( ic, trim( p ), depth + 1 );
    free( copy );
    for (int i = 0; i < n; i++) {
        free( e[i].section );
        free( e[i].key );
        free( e[i].value );
    }
    free( e );
}

awl_icons_t* awl_icons_new( const char* const* themes ) {
    awl_icons_t* ic = calloc( 1, sizeof(*ic) );
    if (!ic) return NULL;
    pthread_mutex_init( &ic->lock, NULL );
    const char* home = getenv( "HOME" );
    add_base( ic, home, "/.icons" );
    const char* data = getenv( "XDG_DATA_HOME" );
    if (data && *data) add_base( ic, data, "/icons" );
    else add_base( ic, home, "/.local/share/icons" );
    const char* dirs = getenv( "XDG_DATA_DIRS" );
    char* copy = strdup( dirs && *dirs ? dirs : "/usr/local/share:/usr/share" );
    char* save = NULL;
    for (char* d = copy ? strtok_r( copy, ":", &save ) : NULL; d; d = strtok_r( NULL, ":", &save ) )
        add_base( ic, d, "/icons" );
    free( copy );
    for (; themes && *themes; themes++) add_theme( ic, *themes, 0 );
    add_theme( ic, "hicolor", 0 );
    for (int i = 0; i < ic->nthemes; i++)
        if (!strcmp( ic->themes[i].name, "hicolor" )) ic->hicolor = &ic->themes[i];
    /* the icons outside themes are looked for in the folders themes are in,
     * then here */
    add_base( ic, "/usr/share/pixmaps", "" );
    return ic;
}

/** the desktop's icon theme as GTK's settings.ini has it, "" if it doesn't
 * (GNOME's dconf setting isn't read) */
static void desktop_theme( char* out, size_t size ) {
    char path[4096];
    const char* config = getenv( "XDG_CONFIG_HOME" );
    const char* home = getenv( "HOME" );
    *out = 0;
    if (config && *config) snprintf( path, sizeof path, "%s/gtk-3.0/settings.ini", config );
    else if (home) snprintf( path, sizeof path, "%s/.config/gtk-3.0/settings.ini", home );
    else return;
    FILE* f = fopen( path, "re" );
    if (!f) return;
    char line[512];
    while (fgets( line, sizeof line, f )) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp( p, "gtk-icon-theme-name", 19 )) continue;
        p += 19;
        while (*p == ' ' || *p == '\t') p++;
        if (*p++ != '=') continue;
        while (*p == ' ' || *p == '\t' || *p == '"') p++;
        size_t n = strcspn( p, "\"\r\n" );
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
        snprintf( out, size, "%.*s", (int)n, p );
    }
    fclose( f );
}

awl_icons_t* awl_icons_new_desktop( const char* theme ) {
    char desktop[256];
    desktop_theme( desktop, sizeof desktop );
    const char* names[4];
    int n = 0;
    if (theme && *theme) names[n++] = theme;
    if (*desktop) names[n++] = desktop;
    names[n++] = "Adwaita";
    names[n] = NULL;
    return awl_icons_new( names );
}

void awl_icons_free( awl_icons_t* ic ) {
    if (!ic) return;
    for (int i = 0; i < ic->nthemes; i++) {
        theme_t* t = &ic->themes[i];
        for (int j = 0; j < t->ndirs; j++) {
            for (int k = 0; k <= t->dirs[j].npaths; k++) free( t->dirs[j].paths[k] );
            free( t->dirs[j].paths );
        }
        free( t->dirs );
        free( t->name );
    }
    free( ic->themes );
    for (int i = 0; i < ic->nbases; i++) free( ic->bases[i] );
    free( ic->bases );
    for (int i = 0; i < CACHE; i++) {
        free( ic->cache[i].key );
        free( ic->cache[i].path );
    }
    pthread_mutex_destroy( &ic->lock );
    free( ic );
}

/* ---- the lookup ---- */

/** how far d is from px pixels, the spec's DirectorySizeDistance in pixels;
 * doubled, plus one if it is smaller, so that at a tie the larger one wins
 * (shrinking looks better than enlarging) */
static int distance( const dir_t* d, int px ) {
    int lo, hi;
    switch (d->type) {
    case 'F': lo = hi = d->size; break;
    case 'S': lo = d->min, hi = d->max; break;
    default: lo = d->size - d->threshold, hi = d->size + d->threshold;
    }
    lo *= d->scale;
    hi *= d->scale;
    return px < lo ? 2 * (lo - px) : px > hi ? 2 * (px - hi) + 1 : 0;
}

typedef struct {
    int dir, distance;
} order_t;

static int by_distance( const void* a_, const void* b_ ) {
    const order_t *a = a_, *b = b_;
    return a->distance != b->distance ? a->distance - b->distance : a->dir - b->dir;
}

/** name's files in folder, *flags what was found: at the exact size a PNG
 * first (drawn for it), else an SVG */
static char* try_files( const char* folder, const char* name, int png_first, unsigned* flags ) {
    char path[PATH_MAX];
    const char* exts[2] = { png_first ? ".png" : ".svg", png_first ? ".svg" : ".png" };
    for (int i = 0; i < 2; i++) {
        if (snprintf( path, sizeof path, "%s/%s%s", folder, name, exts[i] ) >= (int)sizeof path) return NULL;
        if (!access( path, R_OK )) {
            *flags = (exts[i][1] == 's' ? AWL_ICON_SVG : 0) | (has_suffix( path, "-symbolic.svg" ) ? AWL_ICON_SYMBOLIC : 0);
            return strdup( path );
        }
    }
    /* GTK's symbolic PNGs: x-symbolic as x.symbolic.png */
    size_t n = strlen( name );
    if (has_suffix( name, "-symbolic" ) &&
        snprintf( path, sizeof path, "%s/%.*s.symbolic.png", folder, (int)(n - 9), name ) < (int)sizeof path &&
        !access( path, R_OK )) {
        *flags = AWL_ICON_SYMBOLIC;
        return strdup( path );
    }
    return NULL;
}

/** name in theme t, the closest size first; in extra (a folder) instead of
 * t's own folders if not NULL */
static char* in_theme( const theme_t* t, const char* name, int px, const char* extra, unsigned* flags ) {
    order_t* o = malloc( (size_t)(t->ndirs ? t->ndirs : 1) * sizeof(*o) );
    if (!o) return NULL;
    for (int i = 0; i < t->ndirs; i++) o[i] = (order_t){ i, distance( &t->dirs[i], px ) };
    qsort( o, (size_t)t->ndirs, sizeof(*o), by_distance );
    char* found = NULL;
    for (int i = 0; i < t->ndirs && !found; i++) {
        const dir_t* d = &t->dirs[o[i].dir];
        int png_first = o[i].distance == 0 && d->type != 'S';
        if (extra) {
            char folder[PATH_MAX];
            if (snprintf( folder, sizeof folder, "%s/%s/%s", extra, t->name, dir_name( d ) ) < (int)sizeof folder)
                found = try_files( folder, name, png_first, flags );
        }
        for (int j = 0; !extra && j < d->npaths && !found; j++) found = try_files( d->paths[j], name, png_first, flags );
    }
    free( o );
    return found;
}

static char* find( const awl_icons_t* ic, const char* name, int px, const char* const* extra, unsigned* flags ) {
    char* found = NULL;
    for (const char* const* e = extra; e && *e && !found; e++) {
        if (!**e) continue;
        if (ic->hicolor) {
            char folder[PATH_MAX];
            if (snprintf( folder, sizeof folder, "%s/hicolor", *e ) < (int)sizeof folder && is_dir( folder ))
                found = in_theme( ic->hicolor, name, px, *e, flags );
        }
        if (!found) found = try_files( *e, name, 0, flags );
    }
    for (int i = 0; i < ic->nthemes && !found; i++) found = in_theme( &ic->themes[i], name, px, NULL, flags );
    for (int i = 0; i < ic->nbases && !found; i++) found = try_files( ic->bases[i], name, 0, flags );
    return found;
}

char* awl_icons_lookup( awl_icons_t* ic, const char* name, int px, const char* const* extra, unsigned* flags ) {
    unsigned f = 0;
    if (flags) *flags = 0;
    if (!ic || !name || !*name) return NULL;
    if (name[0] == '/') {
        if (access( name, R_OK )) return NULL;
        if (flags) *flags = (has_suffix( name, ".svg" ) ? AWL_ICON_SVG : 0) |
                            (has_suffix( name, "-symbolic.svg" ) || has_suffix( name, ".symbolic.png" ) ? AWL_ICON_SYMBOLIC : 0);
        return strdup( name );
    }
    /* never a path within a folder */
    if (strchr( name, '/' )) return NULL;

    char* key = NULL;
    size_t keylen = 0;
    FILE* k = open_memstream( &key, &keylen );
    if (!k) return NULL;
    fprintf( k, "%d %s", px, name );
    for (const char* const* e = extra; e && *e; e++) fprintf( k, "\n%s", *e );
    fclose( k );

    pthread_mutex_lock( &ic->lock );
    for (int i = 0; i < CACHE; i++)
        if (ic->cache[i].key && !strcmp( ic->cache[i].key, key )) {
            char* path = ic->cache[i].path ? strdup( ic->cache[i].path ) : NULL;
            if (flags) *flags = ic->cache[i].flags;
            pthread_mutex_unlock( &ic->lock );
            free( key );
            return path;
        }
    pthread_mutex_unlock( &ic->lock );

    char* found = find( ic, name, px, extra, &f );
    /* "x.png" as "x" */
    if (!found && (has_suffix( name, ".png" ) || has_suffix( name, ".svg" ) || has_suffix( name, ".xpm" ))) {
        char* stem = strndup( name, strlen( name ) - 4 );
        if (stem && *stem) found = find( ic, stem, px, extra, &f );
        free( stem );
    }
    if (!found) f = 0;

    pthread_mutex_lock( &ic->lock );
    hit_t* h = &ic->cache[ic->next];
    ic->next = (ic->next + 1) % CACHE;
    free( h->key );
    free( h->path );
    h->key = key;
    h->path = found ? strdup( found ) : NULL;
    h->flags = f;
    pthread_mutex_unlock( &ic->lock );
    if (flags) *flags = f;
    return found;
}

/* ---- decoding ---- */

void* awl_icon_read( const char* path, size_t* len ) {
    *len = 0;
    FILE* f = fopen( path, "re" );
    if (!f) return NULL;
    struct stat st;
    void* data = NULL;
    if (fstat( fileno( f ), &st ) || !S_ISREG( st.st_mode )) goto out;
    if (st.st_size > AWL_ICON_MAX_BYTES) {
        awl_vrb_printf( "icon: %s: too large (%lld bytes)\n", path, (long long)st.st_size );
        goto out;
    }
    data = malloc( st.st_size ? (size_t)st.st_size : 1 );
    if (data && fread( data, 1, (size_t)st.st_size, f ) == (size_t)st.st_size) {
        *len = (size_t)st.st_size;
    } else {
        free( data );
        data = NULL;
    }
out:
    fclose( f );
    return data;
}

/** w x h fitted into max_w x max_h, keeping the aspect ratio */
static void fit( double w, double h, int max_w, int max_h, int* tw, int* th ) {
    if (w * max_h >= h * max_w) {
        *tw = max_w;
        *th = (int)lround( h * max_w / w );
    } else {
        *th = max_h;
        *tw = (int)lround( w * max_h / h );
    }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

/** img fitted into max_w x max_h, Lanczos3 as fcft scales its bitmaps; takes
 * img */
static pixman_image_t* scaled( pixman_image_t* img, int max_w, int max_h ) {
    int w = pixman_image_get_width( img ), h = pixman_image_get_height( img ), tw, th;
    fit( w, h, max_w, max_h, &tw, &th );
    if (tw == w && th == h) return img;
    pixman_image_t* out = pixman_image_create_bits( PIXMAN_a8r8g8b8, tw, th, NULL, 0 );
    if (!out) {
        pixman_image_unref( img );
        return NULL;
    }
    double sx = (double)w / tw, sy = (double)h / th;
    pixman_transform_t t;
    pixman_transform_init_scale( &t, pixman_double_to_fixed( sx ), pixman_double_to_fixed( sy ) );
    pixman_image_set_transform( img, &t );
    /* the edges' own pixels beyond them, not transparency: no faded rim */
    pixman_image_set_repeat( img, PIXMAN_REPEAT_PAD );
    int n = 0;
    pixman_fixed_t* params = pixman_filter_create_separable_convolution(
        &n, pixman_double_to_fixed( sx > 1 ? sx : 1 ), pixman_double_to_fixed( sy > 1 ? sy : 1 ),
        PIXMAN_KERNEL_IMPULSE, PIXMAN_KERNEL_IMPULSE, PIXMAN_KERNEL_LANCZOS3, PIXMAN_KERNEL_LANCZOS3, 4, 4 );
    if (params) {
        pixman_image_set_filter( img, PIXMAN_FILTER_SEPARABLE_CONVOLUTION, params, n );
        free( params );
    } else {
        pixman_image_set_filter( img, PIXMAN_FILTER_BILINEAR, NULL, 0 );
    }
    pixman_image_composite32( PIXMAN_OP_SRC, img, NULL, out, 0, 0, 0, 0, 0, 0, tw, th );
    pixman_image_unref( img );
    return out;
}

/** a PNG's or JPEG's pixels through dec, a Wuffs decoder made for it; kind
 * names it in the log */
static pixman_image_t* wuffs_image( wuffs_base__image_decoder* dec, const uint8_t* data, size_t len, const char* kind ) {
    const char* err = NULL;
    void* work = NULL;
    pixman_image_t* img = NULL;
    /* the reader doesn't write */
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader( (uint8_t*)data, len, true );
    wuffs_base__image_config ic;
    wuffs_base__status st = wuffs_base__image_decoder__decode_image_config( dec, &ic, &src );
    if (!wuffs_base__status__is_ok( &st )) goto out;
    uint32_t w = wuffs_base__pixel_config__width( &ic.pixcfg ), h = wuffs_base__pixel_config__height( &ic.pixcfg );
    if (!w || !h || w > AWL_ICON_MAX_DIM || h > AWL_ICON_MAX_DIM) {
        err = "too large";
        goto out;
    }
    /* what the decoder needs besides the pixels: little for a PNG, a
     * progressive JPEG's coefficients (2 bytes a sample) for a JPEG; known
     * before anything is allocated */
    uint64_t wlen = wuffs_base__image_decoder__workbuf_len( dec ).max_incl;
    if (wlen > AWL_ICON_MAX_WORK) {
        err = "needs too much memory";
        goto out;
    }
    wuffs_base__pixel_config__set( &ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
                                   WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h );
    /* pixman's rows of 32 bits pixels have no padding */
    img = pixman_image_create_bits( PIXMAN_a8r8g8b8, (int)w, (int)h, NULL, 0 );
    if (!img) goto out;
    wuffs_base__pixel_buffer pb;
    st = wuffs_base__pixel_buffer__set_from_slice(
        &pb, &ic.pixcfg, wuffs_base__make_slice_u8( (uint8_t*)pixman_image_get_data( img ), (size_t)w * h * 4 ) );
    if (!wuffs_base__status__is_ok( &st )) goto out;
    work = malloc( wlen ? wlen : 1 );
    if (!work) goto out;
    st = wuffs_base__image_decoder__decode_frame( dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
                                                  wuffs_base__make_slice_u8( work, wlen ), NULL );
out:
    if (!err && !wuffs_base__status__is_ok( &st )) err = wuffs_base__status__message( &st );
    if (err) {
        awl_vrb_printf( "icon: %s: %s\n", kind, err );
        if (img) pixman_image_unref( img );
        img = NULL;
    }
    free( work );
    return img;
}

static pixman_image_t* png( const uint8_t* data, size_t len ) {
    pixman_image_t* img = NULL;
    wuffs_png__decoder* dec = malloc( sizeof__wuffs_png__decoder() );
    if (!dec) return NULL;
    wuffs_base__status st = wuffs_png__decoder__initialize( dec, sizeof__wuffs_png__decoder(), WUFFS_VERSION,
                                                            WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED );
    if (wuffs_base__status__is_ok( &st )) {
        wuffs_png__decoder__set_quirk( dec, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, 1 );
        img = wuffs_image( wuffs_png__decoder__upcast_as__wuffs_base__image_decoder( dec ), data, len, "PNG" );
    }
    free( dec );
    return img;
}

/** baseline and progressive, CMYK too; not arithmetic coded, lossless or
 * 12 bits (Wuffs has no decoder for those). The EXIF orientation isn't
 * applied. */
static pixman_image_t* jpeg( const uint8_t* data, size_t len ) {
    pixman_image_t* img = NULL;
    wuffs_jpeg__decoder* dec = malloc( sizeof__wuffs_jpeg__decoder() );
    if (!dec) return NULL;
    wuffs_base__status st = wuffs_jpeg__decoder__initialize( dec, sizeof__wuffs_jpeg__decoder(), WUFFS_VERSION,
                                                             WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED );
    if (wuffs_base__status__is_ok( &st ))
        img = wuffs_image( wuffs_jpeg__decoder__upcast_as__wuffs_base__image_decoder( dec ), data, len, "JPEG" );
    free( dec );
    return img;
}

/* ---- SVG ---- */

/** one rule of an icon theme's color scheme: ".name { color: color }" */
typedef struct {
    char name[48], color[48];
} rule_t;

enum { MAX_RULES = 16 };

static const char* find_text( const char* s, const char* end, const char* what ) {
    size_t n = strlen( what );
    for (; s && end - s >= (ptrdiff_t)n; s++)
        if (!memcmp( s, what, n )) return s;
    return NULL;
}

static int name_char( char c ) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/** of [s, end) without the spaces around it, at most size - 1 chars to out;
 * 0 if longer, or if it has chars that can't be in an attribute's value */
static int copy_trimmed( const char* s, const char* end, char* out, size_t size ) {
    while (s < end && strchr( " \t\r\n", *s )) s++;
    while (end > s && strchr( " \t\r\n", end[-1] )) end--;
    if (s == end || (size_t)(end - s) >= size) return 0;
    for (const char* c = s; c < end; c++)
        if (*c == '"' || *c == '\'' || *c == '<' || *c == '>' || *c == '&') return 0;
    memcpy( out, s, (size_t)(end - s) );
    out[end - s] = 0;
    return 1;
}

/** The icon themes' color scheme (KDE's convention, which Papirus and Breeze
 * follow): <style> rules ".ColorScheme-Text { color: #dfdfdf; }" for the
 * elements of that class, which draw in currentColor. Rules of any other
 * form are skipped. Returns how many it found. */
static int scheme( const char* s, const char* end, rule_t* rules ) {
    int n = 0;
    for (const char* style = find_text( s, end, "<style" ); style && n < MAX_RULES; style = find_text( style + 1, end, "<style" )) {
        const char* p = memchr( style, '>', (size_t)(end - style) );
        const char* close = p ? find_text( p, end, "</style>" ) : NULL;
        if (!close) break;
        for (p++; p < close && n < MAX_RULES;) {
            const char* open = memchr( p, '{', (size_t)(close - p) );
            const char* shut = open ? memchr( open, '}', (size_t)(close - open) ) : NULL;
            if (!shut) break;
            /* the selector: one class */
            const char* sel = p;
            while (sel < open && !name_char( *sel ) && *sel != '.') sel++;
            const char* name = sel < open && *sel == '.' ? sel + 1 : NULL;
            const char* name_end = name;
            while (name_end && name_end < open && name_char( *name_end )) name_end++;
            const char* rest = name_end;
            while (rest && rest < open && strchr( " \t\r\n", *rest )) rest++;
            rule_t r;
            if (name && rest == open && copy_trimmed( name, name_end, r.name, sizeof r.name )) {
                /* its color declaration */
                for (const char* d = open + 1; d < shut;) {
                    const char* semi = memchr( d, ';', (size_t)(shut - d) );
                    const char* d_end = semi ? semi : shut;
                    const char* colon = memchr( d, ':', (size_t)(d_end - d) );
                    char key[16];
                    if (colon && copy_trimmed( d, colon, key, sizeof key ) && !strcmp( key, "color" ) &&
                        copy_trimmed( colon + 1, d_end, r.color, sizeof r.color )) {
                        rules[n++] = r;
                        break;
                    }
                    d = d_end + 1;
                }
            }
            p = shut + 1;
        }
        style = close;
    }
    return n;
}

/** data with the color scheme's colors as the attribute color="..." of the
 * elements of their classes, which PlutoSVG understands: malloc()ed, *len
 * its length; NULL if there is no scheme, or no memory */
static char* apply_scheme( const char* data, size_t* len ) {
    const char* end = data + *len;
    rule_t rules[MAX_RULES];
    int n = scheme( data, end, rules );
    if (!n) return NULL;
    char* out = NULL;
    size_t out_len = 0;
    FILE* f = open_memstream( &out, &out_len );
    if (!f) return NULL;
    const char* done = data;
    for (const char* c = find_text( data, end, "class=" ); c; c = find_text( c + 6, end, "class=" )) {
        /* an attribute: after a space, its value quoted */
        if (c == data || !strchr( " \t\r\n", c[-1] ) || c + 6 >= end || (c[6] != '"' && c[6] != '\'')) continue;
        const char* v = c + 7;
        const char* v_end = memchr( v, c[6], (size_t)(end - v) );
        if (!v_end) break;
        const char* color = NULL;
        for (const char* t = v; t < v_end && !color;) {
            while (t < v_end && !name_char( *t )) t++;
            const char* t_end = t;
            while (t_end < v_end && name_char( *t_end )) t_end++;
            for (int i = 0; i < n && !color; i++)
                if (strlen( rules[i].name ) == (size_t)(t_end - t) && !memcmp( rules[i].name, t, (size_t)(t_end - t) ))
                    color = rules[i].color;
            t = t_end;
        }
        if (!color) continue;
        fwrite( done, 1, (size_t)(c - done), f );
        fprintf( f, "color=\"%s\" ", color );
        done = c;
    }
    fwrite( done, 1, (size_t)(end - done), f );
    if (fclose( f )) {
        free( out );
        return NULL;
    }
    *len = out_len;
    return out;
}

/** drawn at the size it is fitted to: an SVG has no pixels of its own */
static pixman_image_t* svg( const uint8_t* data, size_t len, int max_w, int max_h, const char* source ) {
    /* the file's own length for the log: the color scheme changes len */
    size_t in_len = len;
    char* themed = apply_scheme( (const char*)data, &len );
    const char* text = themed ? themed : (const char*)data;
    pixman_image_t* img = NULL;
    /* PlutoSVG reads no other file, and no image but a data: URL's PNG or
     * JPEG */
    plutosvg_document_t* doc = plutosvg_document_load_from_data( text, (int)len, -1, -1, NULL, NULL );
    if (!doc) {
        awl_vrb_printf( "icon: SVG: can't parse it\n" );
        goto out;
    }
    float w = plutosvg_document_get_width( doc ), h = plutosvg_document_get_height( doc );
    if (!(w > 0) || !(h > 0) || !isfinite( w ) || !isfinite( h )) {
        awl_vrb_printf( "icon: SVG: no size\n" );
        goto out;
    }
    int tw, th;
    fit( w, h, max_w, max_h, &tw, &th );
    img = pixman_image_create_bits( PIXMAN_a8r8g8b8, tw, th, NULL, 0 );
    if (!img) goto out;
    /* plutovg's pixels are pixman's a8r8g8b8: it draws right into img */
    plutovg_surface_t* surface = plutovg_surface_create_for_data( (unsigned char*)pixman_image_get_data( img ), tw, th,
                                                                  pixman_image_get_stride( img ) );
    plutovg_canvas_t* canvas = surface ? plutovg_canvas_create( surface ) : NULL;
    int ok = 0;
    if (canvas) {
        plutovg_canvas_scale( canvas, (float)tw / w, (float)th / h );
        ok = plutosvg_document_render( doc, NULL, canvas, NULL, NULL, NULL );
    }
    if (canvas) plutovg_canvas_destroy( canvas );
    if (surface) plutovg_surface_destroy( surface );
    if (!ok) {
        awl_vrb_printf( "icon: SVG: can't draw it\n" );
        pixman_image_unref( img );
        img = NULL;
    } else {
        awl_vrb_printf( "icon: decoded SVG %s (%zu bytes): %gx%g, drawn at %dx%d (scale %.3g) into %dx%d\n",
                        source, in_len, (double)w, (double)h, tw, th, (double)tw / w, max_w, max_h );
    }
out:
    if (doc) plutosvg_document_destroy( doc );
    free( themed );
    return img;
}

static int sane( int max_w, int max_h ) {
    return max_w > 0 && max_h > 0 && max_w <= AWL_ICON_MAX_DIM && max_h <= AWL_ICON_MAX_DIM;
}

pixman_image_t* awl_icon_decode( const void* data, size_t len, int allow_svg, int max_w, int max_h, const char* source ) {
    static const uint8_t magic[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (!data || !len || !sane( max_w, max_h )) return NULL;
    if (!source) source = "data";
    if (len > AWL_ICON_MAX_BYTES) {
        awl_vrb_printf( "icon: %s: too large (%zu bytes)\n", source, len );
        return NULL;
    }
    int is_png = len >= sizeof magic && !memcmp( data, magic, sizeof magic );
    /* SOI, then a marker */
    int is_jpeg = len >= 3 && !memcmp( data, "\xff\xd8\xff", 3 );
    if (is_png || is_jpeg) {
        pixman_image_t* img = is_png ? png( data, len ) : jpeg( data, len );
        if (!img) return NULL;
        int w = pixman_image_get_width( img ), h = pixman_image_get_height( img );
        img = scaled( img, max_w, max_h );
        if (img)
            awl_vrb_printf( "icon: decoded %s %s (%zu bytes): %dx%d, fitted to %dx%d into %dx%d\n",
                            is_png ? "PNG" : "JPEG", source, len, w, h, pixman_image_get_width( img ),
                            pixman_image_get_height( img ), max_w, max_h );
        return img;
    }
    return allow_svg ? svg( data, len, max_w, max_h, source ) : NULL;
}

pixman_image_t* awl_icon_load( const char* path, int max_w, int max_h ) {
    size_t len;
    void* data = awl_icon_read( path, &len );
    pixman_image_t* img = data ? awl_icon_decode( data, len, 1, max_w, max_h, path ) : NULL;
    if (data && !img) awl_vrb_printf( "icon: can't decode %s\n", path );
    free( data );
    return img;
}

pixman_image_t* awl_icon_from_pixmaps( const awl_icon_pixmap_t* p, int n, int max_w, int max_h ) {
    if (!sane( max_w, max_h )) return NULL;
    int best = -1, best_covers = 0;
    long best_area = 0;
    for (int i = 0; i < n; i++) {
        if (p[i].w <= 0 || p[i].h <= 0 || p[i].w > AWL_ICON_MAX_DIM || p[i].h > AWL_ICON_MAX_DIM || !p[i].data ||
            p[i].len != (size_t)p[i].w * (size_t)p[i].h * 4)
            continue;
        int tw, th;
        fit( p[i].w, p[i].h, max_w, max_h, &tw, &th );
        int covers = p[i].w >= tw && p[i].h >= th;
        long area = (long)p[i].w * p[i].h;
        /* the smallest that covers it, else the largest */
        if (best < 0 || (covers && (!best_covers || area < best_area)) || (!covers && !best_covers && area > best_area)) {
            best = i;
            best_covers = covers;
            best_area = area;
        }
    }
    if (best < 0) return NULL;
    pixman_image_t* img = pixman_image_create_bits( PIXMAN_a8r8g8b8, p[best].w, p[best].h, NULL, 0 );
    if (!img) return NULL;
    uint32_t* dst = pixman_image_get_data( img );
    int ds = pixman_image_get_stride( img ) / 4;
    const uint8_t* src = p[best].data;
    for (int y = 0; y < p[best].h; y++)
        for (int x = 0; x < p[best].w; x++, src += 4) {
            uint32_t a = src[0];
            dst[y * ds + x] = a << 24 | (src[1] * a / 255) << 16 | (src[2] * a / 255) << 8 | (src[3] * a / 255);
        }
    return scaled( img, max_w, max_h );
}
