/** The desktop file list's content: which files, how they look, what clicks
 * do. awl_desktop.c owns the panels' buffers and scene nodes and asks for the
 * pixels through awl_plugin_api_t (see awl_plugin_abi.h). */

#include "plugins.h"
#include "plugins/colors.h"
#include "plugins/persistent.h"
#include "plugins/readdir.h"
#include "plugins/thread.h"

#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

/** the look, `awl_config_t.desktop`; hidden entries, "(empty)" and
 * "… N more" keep their color at its hidden_alpha */
#define DESK (&awl_config()->desktop)

#define NFILES (sizeof(((DesktopFiles*)0)->files) / sizeof(Filename))

/** the layout of the last measure, for drawing: the files, then the folders,
 * each spread evenly over `awl_desktop_config_t.columns` columns, the
 * folders from the first column on again below the files */
typedef struct {
    int idx[NFILES];   /* the entries shown, in order */
    int col[NFILES], row[NFILES];
    int n, shown;      /* visible ones, and how many of those fit */
    int col_x[NFILES], col_w[NFILES], ncols, row_h, pad, gap;
    /* per section, the one shown that is "… N more" in place of those that
     * don't fit (-1: all do) */
    int more_at[2];
    char more[2][32];
} PanelLayout;

/** Main thread state. The main thread never touches the file system: on a
 * hung (e.g. remote) mount any file system call can block indefinitely,
 * which must not freeze the compositor. */
static struct PanelState {
    /* main thread's copy of the scanner's last list, what the panels show */
    Filename files[NFILES];
    int n_files;
    uint64_t version; /* see desktop_version() */
    int scanned;      /* the scanner reported at least once */
    int show, hidden;

    PanelLayout lay;
} desk;

/** Shared with a scanner thread, which does all the file system access
 * (inotify_add_watch() resolves the path too). One per scanner: a scanner
 * stuck in the kernel is abandoned rather than waited for, together with its
 * Scanner, which is then never freed -- and since its code must stay
 * mapped too, the library stays loaded (api fini() reports it). */
typedef struct {
    char path[PATH_MAX];  /* set before the thread starts, then read-only */
    DesktopFiles files;   /* scanner only: its last list */
    /* scanner -> main: a copy of a changed list, NULL if there is none yet.
     * Handed over whole, so neither side ever waits for the other. */
    _Atomic(DesktopFiles*) pending;
    awl_thread_t thread;  /* waking it (main -> scanner) means rescan */
} Scanner;

/** the running scanner, NULL if none */
static Scanner* sc;

/** desk.show and desk.hidden as a string in awl's config dictionary, so they
 * survive reloads: "off", "on" or "all" (hidden files too). awl gives it a
 * default, the scanner loads and saves it in persistent.h's file. Off
 * doesn't keep hidden files' setting. */
static const char file_mode_key[] = "desktop_file_mode";

/** desk.show and desk.hidden from the dictionary; unchanged if it has no
 * mode; main thread */
static void readmode( void ) {
    awl_dict_val_t v;
    /* the string is the dictionary's until it's unlocked */
    AWL_HOST_CFG_LOCK( awl_host );
    if (AWL_HOST_CFG_GET( awl_host, file_mode_key, &v ) == AWL_DICT_OK
            && v.kind == AWL_DICT_KIND_STR) {
        if (!strcmp( v.str, "off" )) {
            desk.show = 0;
        } else if (!strcmp( v.str, "on" ) || !strcmp( v.str, "all" )) {
            desk.show = 1;
            desk.hidden = v.str[0] == 'a';
        }
    }
    AWL_HOST_CFG_UNLOCK( awl_host );
}

/** the dictionary's mode from desk.show and desk.hidden; main thread */
static void writemode( void ) {
    const char* mode = !desk.show ? "off" : desk.hidden ? "all" : "on";
    AWL_HOST_CFG_SET( awl_host, file_mode_key, AWL_DICT_STR( mode ) );
}

/** after an event, wait this long for more before scanning, so a burst
 * (copying many files) costs one scan; but at most scan_settle_max_ms */
static const int scan_settle_ms = 50, scan_settle_max_ms = 500;
/** without a watch (no directory yet, or it vanished), retry this often */
static const int rewatch_ms = 5000;

static void* scanner( void* data ) {
    Scanner* sc = data;
    int ifd = inotify_init1( IN_NONBLOCK | IN_CLOEXEC ), watch = -1, unsent = 1;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    /* at awl's start, the saved mode; the main thread reads it with the
     * first list */
    awl_persistent_load_str( file_mode_key );
    while (1) {
        /* the directory may have been (re)created since */
        if (ifd >= 0 && watch < 0)
            watch = inotify_add_watch( ifd, sc->path,
                    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                    IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR );
        /* the first result goes out even if it's empty, which findfiles()
         * doesn't count as a change */
        unsent |= findfiles( &sc->files, sc->path );
        if (unsent) {
            DesktopFiles* copy = malloc( sizeof(*copy) );
            if (copy) {
                *copy = sc->files;
                /* replaces one the main thread hasn't taken yet */
                free( atomic_exchange( &sc->pending, copy ) );
                awl_host->redraw_request();
                unsent = 0;
            }
        }

        struct pollfd fds[2] = {
            { .fd = sc->thread.wake_fd, .events = POLLIN },
            { .fd = ifd, .events = POLLIN },
        };
        int r = poll( fds, 2, watch < 0 ? rewatch_ms : -1 );
        if (r < 0 && errno != EINTR) break;
        if ((fds[0].revents & POLLIN) && awl_thread_woken( &sc->thread )) break;
        if (fds[1].revents & POLLIN) {
            for (int waited = 0; waited < scan_settle_max_ms; waited += scan_settle_ms) {
                ssize_t len;
                while ((len = read( ifd, buf, sizeof(buf) )) > 0) {
                    for (char* p = buf; p < buf + len; ) {
                        const struct inotify_event* ev = (const struct inotify_event*)p;
                        /* the directory is gone, the kernel dropped the watch */
                        if (ev->mask & IN_IGNORED) watch = -1;
                        p += sizeof(*ev) + ev->len;
                    }
                }
                if (poll( &fds[1], 1, scan_settle_ms ) <= 0) break;
            }
        }
    }
    if (ifd >= 0) close( ifd );
    awl_persistent_save_str( file_mode_key );
    return NULL;
}

/** asks the scanner to look again, e.g. for a directory that appeared */
static void rescan( void ) {
    if (sc) awl_thread_wake( &sc->thread );
}

void awl_desktop_start( void ) {
    memset( &desk, 0, sizeof(desk) );
    desk.show = 1;
    desk.version = 1;
    readmode();

    Scanner* n = calloc( 1, sizeof(*n) );
    if (!n) return;
    /* the string is the dictionary's until it's unlocked */
    awl_dict_val_t dir;
    AWL_HOST_CFG_LOCK( awl_host );
    if (AWL_HOST_CFG_GET( awl_host, "desktop_dir", &dir ) == AWL_DICT_OK
            && dir.kind == AWL_DICT_KIND_STR)
        snprintf( n->path, sizeof(n->path), "%s", dir.str );
    AWL_HOST_CFG_UNLOCK( awl_host );
    if (!n->path[0]) {
        awl_wrn_printf( "desktop: no desktop_dir in the config dictionary\n" );
        free( n );
        return;
    }
    atomic_init( &n->pending, NULL );
    if (awl_thread_start( &n->thread, "desktop", scanner, n )) {
        free( n );
        return;
    }
    sc = n;
}

int awl_desktop_stop( void ) {
    if (!sc) return 0;
    Scanner* old = sc;
    sc = NULL;

    /* a scanner blocked on a dead mount can't be woken or cancelled; leave
     * it behind instead of hanging awl */
    if (awl_thread_stop( &old->thread, 500 )) {
        awl_wrn_printf( "desktop: scanner thread stuck (in %s?), not waiting for it\n",
                 old->path );
        return -1; /* old stays allocated, the thread may still use it */
    }
    free( atomic_load( &old->pending ) );
    free( old );
    return 0;
}

uint64_t awl_desktop_version( void ) {
    DesktopFiles* f = sc ? atomic_exchange( &sc->pending, NULL ) : NULL;
    if (f) {
        int changed = desk.n_files != f->n_files
                   || memcmp( desk.files, f->files, sizeof(desk.files) );
        if (changed) {
            memcpy( desk.files, f->files, sizeof(desk.files) );
            desk.n_files = f->n_files;
        }
        free( f );
        if (changed || !desk.scanned) desk.version++;
        if (!desk.scanned) readmode(); /* the scanner may have loaded one */
        desk.scanned = 1;
    }
    /* nothing until the first scan, rather than a wrong "(empty)" */
    return desk.scanned && desk.show ? desk.version : 0;
}

static int visible( const Filename* f ) {
    return desk.hidden || !f->ishidden;
}

static void label( const Filename* f, char* out, size_t n ) {
    snprintf( out, n, "%s%s", f->name, f->isdir ? "/" : "" );
}

/** c at opacity a; pixman takes text colors premultiplied */
static uint32_t fade( uint32_t c, uint32_t a ) {
    uint32_t out = a;
    for (int s = 8; s < 32; s += 8)
        out |= (((c >> s) & 0xff) * a / 255) << s;
    return out;
}

static uint32_t color( const Filename* f ) {
    uint32_t c = f->isbroken ? DESK->broken : f->isdir ? DESK->dir : DESK->file;
    return f->ishidden ? fade( c, DESK->hidden_alpha ) : c;
}

/** "… N more" if the i-th shown stands for those that don't fit, else NULL */
static const char* more_of( const PanelLayout* l, int i ) {
    return i == l->more_at[0] ? l->more[0] : i == l->more_at[1] ? l->more[1] : NULL;
}

void awl_desktop_measure( struct fcft_font* font, int avail_w, int avail_h, int radius, float s,
                          int* x, int* y, int* w, int* h ) {
    PanelLayout* l = &desk.lay;
    const int fh = font->height,
              margin = (int)lroundf( DESK->margin * s ),
              cols = MAX( 1, MIN( DESK->columns, (int)NFILES ) );
    l->row_h = fh + (int)lroundf( 4 * s );
    l->pad = MAX( fh / 2, radius / 2 );
    l->gap = fh;
    avail_w -= 2 * margin;
    avail_h -= 2 * margin;
    const int max_rows = MAX( 1, (avail_h - 2 * l->pad) / l->row_h ),
              max_col_w = MAX( 1, (avail_w - 2 * l->pad - (cols - 1) * l->gap) / cols );

    /* the visible ones: the files, then the folders (findfiles()' order) */
    int vis[NFILES], k[2] = { 0, 0 };
    l->n = 0;
    for (int i = 0; i < desk.n_files; i++)
        if (visible( &desk.files[i] )) {
            vis[l->n++] = i;
            k[desk.files[i].isdir]++;
        }

    /* each section's rows; if both don't fit, the folders keep at least
     * half of them, the files the rest */
    int rows[2] = { (k[0] + cols - 1) / cols, (k[1] + cols - 1) / cols };
    if (rows[0] + rows[1] > max_rows) {
        rows[1] = MIN( rows[1], MAX( max_rows / 2, max_rows - rows[0] ) );
        rows[0] = max_rows - rows[1];
    }

    /* each section's first ones that fit, spread over the columns evenly:
     * the first columns one longer than the others */
    l->shown = l->ncols = 0;
    for (int sec = 0, first = 0, row0 = 0; sec < 2; first += k[sec++]) {
        const int m = MIN( k[sec], rows[sec] * cols ), base = m / cols, longer = m % cols;
        /* the last that fits stands for the ones that don't */
        l->more_at[sec] = m && m < k[sec] ? l->shown + m - 1 : -1;
        snprintf( l->more[sec], sizeof(l->more[sec]), "… %d more", k[sec] - m + 1 );
        for (int c = 0, j = 0; j < m; c++)
            for (int r = 0; r < base + (c < longer); r++, j++) {
                l->idx[l->shown] = vis[first + j];
                l->col[l->shown] = c;
                l->row[l->shown++] = row0 + r;
                l->ncols = MAX( l->ncols, c + 1 );
            }
        row0 += base + (longer > 0);
    }

    char buf[sizeof(desk.files[0].name) + 2];
    int nrows = 1;
    if (l->n == 0) {
        l->ncols = 1;
        l->col_w[0] = (int)awl_host->text_width( font, "(empty)" );
    } else {
        for (int c = 0; c < l->ncols; c++) l->col_w[c] = 0;
        for (int i = 0; i < l->shown; i++) {
            const char* t = more_of( l, i );
            label( &desk.files[l->idx[i]], buf, sizeof(buf) );
            if (!t) t = buf;
            l->col_w[l->col[i]] = MAX( l->col_w[l->col[i]], MIN( (int)awl_host->text_width( font, t ), max_col_w ) );
            nrows = MAX( nrows, l->row[i] + 1 );
        }
    }
    int width = l->pad;
    for (int c = 0; c < l->ncols; c++) {
        l->col_x[c] = width;
        width += l->col_w[c] + (c + 1 < l->ncols ? l->gap : 0);
    }

    *x = *y = margin;
    *w = width + l->pad;
    *h = 2 * l->pad + nrows * l->row_h;
}

static inline float clampf( float x ) {
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

/** Puts a rounded panel (antialiased) under what is already drawn into the
 * premultiplied ARGB buffer: dst = dst + src * (1 - dst.a). */
static void panel_under( uint32_t* data, int stride, int w, int h, float r, uint32_t bg ) {
    const float hw = w / 2.f, hh = h / 2.f;
    const float bga = (bg & 0xff) / 255.f;
    for (int y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)((char*)data + (size_t)y * stride);
        for (int x = 0; x < w; x++) {
            /* signed distance to the rounded rect's outline */
            float qx = fabsf( x + .5f - hw ) - (hw - r),
                  qy = fabsf( y + .5f - hh ) - (hh - r);
            float d = hypotf( fmaxf( qx, 0 ), fmaxf( qy, 0 ) ) + fminf( fmaxf( qx, qy ), 0 ) - r;
            float sa = bga * clampf( .5f - d );
            if (sa <= 0) continue;

            uint32_t px = row[x];
            float keep = 1 - (px >> 24) / 255.f;
            uint32_t out = 0;
            for (int s = 24; s >= 0; s -= 8) {
                /* channel s of bg as 0..1, premultiplied */
                float c = s == 24 ? sa : ((bg >> (s + 8)) & 0xff) / 255.f * sa;
                float v = ((px >> s) & 0xff) + c * keep * 255.f;
                out |= (uint32_t)(v > 255 ? 255 : v + .5f) << s;
            }
            row[x] = out;
        }
    }
}

static void text( pixman_image_t* pix, struct fcft_font* font, int x, int y, int w, int h,
                  const char* s, uint32_t fg ) {
    awl_host->text( pix, font, x, y, w, h, 0, s, color_8bit_to_16bit( fg ), color_8bit_to_16bit( 0 ) );
}

void awl_desktop_draw( pixman_image_t* pix, struct fcft_font* font, uint32_t* data, int stride,
                       int w, int h, int radius, float s ) {
    const PanelLayout* l = &desk.lay;
    char buf[sizeof(desk.files[0].name) + 2];

    if (l->n == 0)
        text( pix, font, l->pad, l->pad, l->col_w[0], l->row_h, "(empty)", fade( DESK->file, DESK->hidden_alpha ) );
    for (int i = 0; i < l->shown; i++) {
        const Filename* f = &desk.files[l->idx[i]];
        const char* more = more_of( l, i );
        label( f, buf, sizeof(buf) );
        text( pix, font, l->col_x[l->col[i]], l->pad + l->row[i] * l->row_h, l->col_w[l->col[i]], l->row_h,
              more ? more : buf, more ? fade( DESK->file, DESK->hidden_alpha ) : color( f ) );
    }
    panel_under( data, stride, w, h, radius, DESK->bg );
}

/* plain clicks change the wallpaper, Shift+clicks the panel and the timer */
/* what a toggle changed, from show/hidden before it to now */
static void notify_toggle( int show, int hidden ) {
    char body[128];
    int len = 0;
    if (show != desk.show)
        len += snprintf( body + len, sizeof(body) - len, "%svisible", desk.show ? "" : "not " );
    if (hidden != desk.hidden)
        snprintf( body + len, sizeof(body) - len, "%s%s hidden files",
                  len ? ", " : "", desk.hidden ? "show" : "hide" );
    awl_notify( "Desktop files", body );
}

int awl_desktop_click( int button, uint32_t mods ) {
    if (!mods) {
        WallpaperMode mode;
        switch (button) {
        case BTN_LEFT: mode = WallpaperPrev; break;
        case BTN_RIGHT: mode = WallpaperNext; break;
        case BTN_MIDDLE: mode = WallpaperRand; break;
        default: return 0;
        }
        awl_notify_wallpaper_shown( "Wallpaper" );
        if (mode == WallpaperRand) awl_wallpaper_random();
        else awl_wallpaper_step( mode == WallpaperNext ? +1 : -1 );
        return 1;
    } else if (mods == WLR_MODIFIER_CTRL) {
        // the button is ignored here, could be taken in the future.
        int cur, n;
        char body[32];
        if (awl_wallpaper_settled( &cur, &n )) {
            snprintf( body, sizeof(body), "%d/%d", cur + 1, n );
            awl_notify( "Wallpaper Info", body );
        }
        return 1;
    } else if (mods == WLR_MODIFIER_SHIFT) {
        int show = desk.show, hidden = desk.hidden;
        switch (button) {
        case BTN_LEFT:
            desk.show = !desk.show;
            break;
        case BTN_MIDDLE:
            /* also shows a hidden list */
            if (desk.show) desk.hidden = !desk.hidden;
            desk.show = 1;
            break;
        case BTN_RIGHT:
            awl_host->actions->wallpapermode( &(Arg){ .i = +1 } );
            return 1;
        default:
            return 0;
        }
        desk.version++;
        writemode();
        rescan();
        notify_toggle( show, hidden );
        return 1;
    } else {
        return 0;
    }
}
