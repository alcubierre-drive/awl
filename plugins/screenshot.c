/* Screenshots, see screenshot.h: the grab and the capture on the main
 * thread, encoding and saving on the screenshots' thread, the clipboard
 * and the notification on the main thread again. */

#include "screenshot.h"
#include "png.h"
#include "thread.h"
#include "../icons/icon.h"
#include "../plugins.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define CFG (&s->p->plugin_config.screenshot)

/** a side of the notification's image without a file at most, pixels: a few
 * times the notifications' image_size, and little memory (RGBA) */
#define THUMB_PX 256
#define COLORS (&s->p->plugin_config.colors)

/** a screenshot on its way: the main thread fills in what to do, the thread
 * what came of it */
typedef struct job_t {
    struct job_t* next;
    /** the image, awl's (:c:member:`awl_host_t.capture`); the thread's to
     * unref */
    pixman_image_t* img;
    int w, h;
    /** where to save it, NULL: not at all; the file's name without ".png"
     * (made unique with "-2", "-3", ...) */
    char* dir, * name;
    /** whether the PNG is wanted back, for the clipboard */
    bool clip;

    /** the thread's: the PNG (if clip), the file it went to (NULL if none),
     * what went wrong ("" nothing) */
    unsigned char* png;
    size_t len;
    char* path;
    /** without a file (path NULL): the image fitted into THUMB_PX, for the
     * notification, straight RGBA rows packed; NULL none */
    unsigned char* thumb;
    int tw, th;
    char err[PATH_MAX + 256];
} job_t;

struct awl_screenshot {
    awl_plugin_data_t* p;

    /** a grab holds (the pointer chooses) */
    bool grabbing;
    /** the button held since the grab's press, 0 none; where it was pressed */
    uint32_t button;
    double px, py;
    /** the press moved further than drag_px: a rectangle */
    bool dragging;
    /** Shift was held at the right press */
    bool all;
    /** the pointer */
    double x, y;

    awl_thread_t thread;
    pthread_mutex_t lock;
    /** under lock: the jobs for the thread, oldest first; those it is done
     * with, for the main thread */
    job_t* todo, * done;
};

static void job_free( job_t* j ) {
    if (!j) return;
    if (j->img) pixman_image_unref( j->img );
    free( j->dir );
    free( j->name );
    free( j->png );
    free( j->path );
    free( j->thumb );
    free( j );
}

static void push( job_t** list, job_t* j ) {
    j->next = NULL;
    while (*list) list = &(*list)->next;
    *list = j;
}

static job_t* pop( job_t** list ) {
    job_t* j = *list;
    if (j) *list = j->next;
    return j;
}

/* ---- the thread ---- */

/** path's directories, as mkdir -p; 0 or -1 with errno */
static int mkdir_p( const char* path ) {
    char tmp[PATH_MAX];
    if (snprintf( tmp, sizeof(tmp), "%s", path ) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (char* c = tmp + 1; ; c++) {
        if (*c && *c != '/') continue;
        char end = *c;
        *c = 0;
        if (mkdir( tmp, 0755 ) && errno != EEXIST) return -1;
        if (!end) return 0;
        *c = end;
    }
}

/** all of len bytes of data to fd; 0 or -1 with errno */
static int write_all( int fd, const unsigned char* data, size_t len ) {
    while (len) {
        ssize_t n = write( fd, data, len );
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/** j's PNG into j->dir: written to a hidden file, then linked to the first
 * free name (so a file of that name is never overwritten, nor seen half
 * written); j->path, or j->err */
static void save( job_t* j ) {
    char tmp[PATH_MAX], path[PATH_MAX];
    int fd, i;
    if (mkdir_p( j->dir )) {
        snprintf( j->err, sizeof(j->err), "can't make %s: %s", j->dir, strerror( errno ) );
        return;
    }
    if (snprintf( tmp, sizeof(tmp), "%s/.%s.png.XXXXXX", j->dir, j->name ) >= (int)sizeof(tmp)
            || (fd = mkstemp( tmp )) < 0) {
        snprintf( j->err, sizeof(j->err), "can't write in %s: %s", j->dir, strerror( errno ) );
        return;
    }
    if (write_all( fd, j->png, j->len ) || fsync( fd ) || close( fd )) {
        snprintf( j->err, sizeof(j->err), "can't write %s: %s", tmp, strerror( errno ) );
        unlink( tmp );
        return;
    }
    chmod( tmp, 0644 ); /* mkstemp()'s is 0600 */
    for (i = 1; i < 1000; i++) {
        if (i == 1) snprintf( path, sizeof(path), "%s/%s.png", j->dir, j->name );
        else snprintf( path, sizeof(path), "%s/%s-%d.png", j->dir, j->name, i );
        if (!link( tmp, path )) {
            j->path = strdup( path );
            break;
        }
        if (errno != EEXIST) {
            snprintf( j->err, sizeof(j->err), "can't save %s: %s", path, strerror( errno ) );
            break;
        }
    }
    if (i == 1000) snprintf( j->err, sizeof(j->err), "no free name for %s in %s", j->name, j->dir );
    unlink( tmp );
}

/** img fitted into THUMB_PX (never larger) into j's thumb; none if out of
 * memory */
static void thumb( job_t* j, pixman_image_t* img ) {
    int w = j->w < THUMB_PX ? j->w : THUMB_PX, h = j->h < THUMB_PX ? j->h : THUMB_PX;
    pixman_image_t* t = awl_icon_fit( pixman_image_ref( img ), w, h );
    if (!t) return;
    const int tw = pixman_image_get_width( t ), th = pixman_image_get_height( t ),
              stride = pixman_image_get_stride( t ) / 4;
    const uint32_t* px = pixman_image_get_data( t );
    unsigned char* o = j->thumb = malloc( (size_t)tw * th * 4 );
    if (o) {
        j->tw = tw;
        j->th = th;
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++, o += 4) {
                uint32_t p = px[(size_t)y * stride + x];
                unsigned a = p >> 24;
                for (int c = 0; c < 3; c++) {
                    unsigned v = p >> (16 - 8 * c) & 0xff;
                    v = a == 255 ? v : a ? (v * 255 + a / 2) / a : 0;
                    o[c] = (unsigned char)(v > 255 ? 255 : v);
                }
                o[3] = (unsigned char)a;
            }
    }
    pixman_image_unref( t );
}

/** encodes j's image, saves it if it has a dir, keeps the PNG if it wants
 * it back; a thumbnail for the notification if there is no file */
static void run( job_t* j ) {
    pixman_image_t* img = j->img;
    struct timespec t0, t1;
    j->img = NULL;
    clock_gettime( CLOCK_MONOTONIC, &t0 );
    j->png = awl_png_encode( pixman_image_get_data( img ), j->w, j->h, pixman_image_get_stride( img ),
                             AWL_PNG_LEVEL_DEFAULT, &j->len );
    if (!j->png) {
        snprintf( j->err, sizeof(j->err), "can't encode the PNG (%dx%d)", j->w, j->h );
        pixman_image_unref( img );
        return;
    }
    clock_gettime( CLOCK_MONOTONIC, &t1 );
    awl_vrb_printf( "screenshot: %dx%d encoded in %ld ms, %zu bytes\n", j->w, j->h,
                    (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000), j->len );
    if (j->dir) save( j );
    if (!j->path) thumb( j, img );
    pixman_image_unref( img );
    if (!j->clip) {
        free( j->png );
        j->png = NULL;
        j->len = 0;
    }
}

static void* worker( void* data ) {
    awl_screenshot_t* s = data;
    struct pollfd fd = { .fd = s->thread.wake_fd, .events = POLLIN };
    job_t* j;
    while (1) {
        pthread_mutex_lock( &s->lock );
        j = pop( &s->todo );
        pthread_mutex_unlock( &s->lock );
        if (j) {
            run( j );
            pthread_mutex_lock( &s->lock );
            push( &s->done, j );
            pthread_mutex_unlock( &s->lock );
            awl_host->redraw_request( awl_host->srv );
            continue;
        }
        if (poll( &fd, 1, -1 ) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (awl_thread_woken( &s->thread )) break;
    }
    return NULL;
}

/* ---- the main thread ---- */

/** the config's dir: "~/" is the home folder's (the config dictionary's
 * "home"), NULL or "" the dictionary's desktop_dir; NULL without either
 * where it needs it */
static char* screenshot_dir( awl_screenshot_t* s ) {
    const char* dir = CFG->dir;
    if (!dir || !*dir) return awl_cfg_strdup( "desktop_dir" );
    if (strncmp( dir, "~/", 2 )) return strdup( dir );
    char* home = awl_cfg_strdup( "home" ), buf[PATH_MAX];
    int n = home ? snprintf( buf, sizeof(buf), "%s/%s", home, dir + 2 ) : -1;
    free( home );
    return n >= 0 && n < (int)sizeof(buf) ? strdup( buf ) : NULL;
}

static void fail( const char* what ) {
    awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
    awl_err_printf( "screenshot: %s\n", what );
    n.summary = "Screenshot failed";
    n.body = what;
    n.urgency = AWL_NOTIF_CRITICAL;
    n.timeout = 5000;
    awl_notify_send( &n );
}

/** captures box and hands it to the thread */
static void shoot( awl_screenshot_t* s, awl_box_t box ) {
    char name[256];
    time_t now = time( NULL );
    struct tm tm;
    job_t* j;
    pixman_image_t* img;

    if (box.w <= 0 || box.h <= 0) return;
    if (!(img = awl_host->capture( awl_host->srv, &box, 0 ))) {
        fail( "awl couldn't render it" );
        return;
    }
    if (!(j = calloc( 1, sizeof(*j) ))) {
        pixman_image_unref( img );
        return;
    }
    j->img = img;
    j->w = pixman_image_get_width( img );
    j->h = pixman_image_get_height( img );
    j->clip = CFG->clipboard;
    if (CFG->save) {
        localtime_r( &now, &tm );
        if (!strftime( name, sizeof(name), CFG->name && *CFG->name ? CFG->name : "%Y-%m-%d_%H-%M-%S", &tm )
                || strchr( name, '/' ))
            snprintf( name, sizeof(name), "screenshot" );
        j->name = strdup( name );
        if (!(j->dir = screenshot_dir( s ))) snprintf( j->err, sizeof(j->err), "no directory to save in" );
    }
    awl_vrb_printf( "screenshot: %dx%d at %d,%d: %dx%d\n", box.w, box.h, box.x, box.y, j->w, j->h );
    pthread_mutex_lock( &s->lock );
    push( &s->todo, j );
    pthread_mutex_unlock( &s->lock );
    awl_thread_wake( &s->thread );
}

/** s with '&' and '<' as entities, for a body's markup; NULL if out of
 * memory */
static char* escaped( const char* s ) {
    char* out = malloc( strlen( s ) * 5 + 1 ), * o = out;
    if (!out) return NULL;
    for (; *s; s++) {
        if (*s == '&') o = stpcpy( o, "&amp;" );
        else if (*s == '<') o = stpcpy( o, "&lt;" );
        else *o++ = *s;
    }
    *o = 0;
    return out;
}

void awl_screenshot_redraw( awl_screenshot_t* s ) {
    job_t* j;
    char body[PATH_MAX + 128];
    if (!s) return;
    while (1) {
        pthread_mutex_lock( &s->lock );
        j = pop( &s->done );
        pthread_mutex_unlock( &s->lock );
        if (!j) break;
        bool copied = j->png && awl_host->clipboard_set( awl_host->srv, "image/png", j->png, j->len );
        if (*j->err && !copied && !j->path) {
            fail( j->err );
        } else {
            awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
            if (*j->err) awl_err_printf( "screenshot: %s\n", j->err );
            /* the body has markup: a path's '<' and '&' are escaped */
            char* path = j->path ? escaped( j->path ) : NULL;
            snprintf( body, sizeof(body), "%d×%d%s%s%s%s", j->w, j->h,
                      copied ? ", copied" : "", path ? "\n" : "", path ? path : "",
                      *j->err ? "\n(not saved)" : "" );
            free( path );
            n.summary = "Screenshot";
            n.body = body;
            n.image = j->path;
            if (!j->path && j->thumb) {
                n.image_data.data = j->thumb;
                n.image_data.w = j->tw;
                n.image_data.h = j->th;
                n.image_data.stride = j->tw * 4;
                n.image_data.channels = 4;
                n.image_data.has_alpha = 1;
            }
            n.flags = AWL_NOTIF_TRANSIENT;
            n.timeout = CFG->notify_timeout;
            if (CFG->notify) awl_notify_send( &n );
            awl_vrb_printf( "screenshot: %s%s\n", j->path ? j->path : "not saved", copied ? ", copied" : "" );
        }
        job_free( j );
    }
}

/* ---- choosing with the pointer ---- */

/** what the pointer chooses now (as a release would), 0 if nothing */
static int chosen( awl_screenshot_t* s, awl_box_t* box ) {
    if (s->button == BTN_LEFT && s->dragging) {
        double x0 = s->px < s->x ? s->px : s->x, y0 = s->py < s->y ? s->py : s->y;
        double x1 = s->px < s->x ? s->x : s->px, y1 = s->py < s->y ? s->y : s->py;
        /* the pointer is between pixels where it is at x.0: round */
        *box = (awl_box_t){ (int)lround( x0 ), (int)lround( y0 ), 0, 0 };
        box->w = (int)lround( x1 ) - box->x;
        box->h = (int)lround( y1 ) - box->y;
        return box->w > 0 && box->h > 0;
    }
    if (s->button == BTN_RIGHT) {
        Monitor* m = s->all ? NULL : awl_host->monitor_at( awl_host->srv, s->x, s->y );
        if (!m && !s->all) return 0;
        return awl_host->monitor_box( awl_host->srv, m, box );
    }
    return awl_host->client_box( awl_host->srv, awl_host->client_at( awl_host->srv, s->x, s->y ),
                                 CFG->border, box );
}

/** marks what the pointer would choose now, or shades everything */
static void mark( awl_screenshot_t* s ) {
    awl_box_t box = {0};
    if (!chosen( s, &box )) box = (awl_box_t){0};
    awl_host->grab_mark( awl_host->srv, &box, COLORS->screenshot_shade, COLORS->screenshot_frame,
                         CFG->frame_width );
}

static void end( awl_screenshot_t* s ) {
    s->grabbing = 0;
    s->button = 0;
    awl_host->grab_end( awl_host->srv );
}

static void grab_motion( double x, double y, void* data ) {
    awl_screenshot_t* s = data;
    s->x = x;
    s->y = y;
    if (s->button == BTN_LEFT && !s->dragging
            && (fabs( x - s->px ) > CFG->drag_px || fabs( y - s->py ) > CFG->drag_px))
        s->dragging = 1;
    mark( s );
}

static void grab_button( uint32_t button, int pressed, uint32_t mods, void* data ) {
    awl_screenshot_t* s = data;
    awl_box_t box;
    if (pressed) {
        /* a second button while one is held, or another than these, takes
         * none */
        if (s->button || (button != BTN_LEFT && button != BTN_RIGHT)) {
            end( s );
            return;
        }
        s->button = button;
        s->px = s->x;
        s->py = s->y;
        s->dragging = 0;
        s->all = button == BTN_RIGHT && (mods & WLR_MODIFIER_SHIFT);
        mark( s );
        return;
    }
    /* a release of the press that started the grab (a binding's button) */
    if (button != s->button) return;
    int ok = chosen( s, &box );
    end( s );
    if (ok) shoot( s, box );
}

static int grab_key( uint32_t sym, uint32_t mods, void* data ) {
    awl_screenshot_t* s = data;
    /* the modifiers alone choose nothing, and Shift turns a held right
     * press into all monitors */
    switch (sym) {
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R:
        if (s->button == BTN_RIGHT) {
            s->all = 1;
            mark( s );
        }
        return 1;
    case XKB_KEY_Control_L: case XKB_KEY_Control_R: case XKB_KEY_Alt_L: case XKB_KEY_Alt_R:
    case XKB_KEY_Super_L: case XKB_KEY_Super_R:
        return 1;
    default:
        end( s );
        return 1;
    }
}

static void grab_cancel( void* data ) {
    awl_screenshot_t* s = data;
    s->grabbing = 0;
    s->button = 0;
}

static const awl_grab_t grab = {
    .motion = grab_motion,
    .button = grab_button,
    .key = grab_key,
    .cancel = grab_cancel,
};

void awl_screenshot( awl_screenshot_t* s, int mode ) {
    awl_box_t box;
    Monitor* m;
    if (!s) return;
    if (s->grabbing) {
        end( s );
        return;
    }
    switch (mode) {
    case AWL_SCREENSHOT_WINDOW:
        if (awl_host->client_box( awl_host->srv, awl_host->client_focused( awl_host->srv ), CFG->border, &box ))
            shoot( s, box );
        break;
    case AWL_SCREENSHOT_MONITOR:
        if ((m = awl_host->monitor_selected( awl_host->srv )) && awl_host->monitor_box( awl_host->srv, m, &box ))
            shoot( s, box );
        break;
    case AWL_SCREENSHOT_ALL:
        if (awl_host->monitor_box( awl_host->srv, NULL, &box )) shoot( s, box );
        break;
    default:
        s->button = 0;
        s->dragging = s->all = 0;
        /* set before: the grab reports the pointer right away */
        s->grabbing = 1;
        if (!awl_host->grab_start( awl_host->srv, &grab, s, "crosshair" )) s->grabbing = 0;
        break;
    }
}

awl_screenshot_t* awl_screenshot_start( awl_plugin_data_t* p ) {
    awl_screenshot_t* s = calloc( 1, sizeof(*s) );
    if (!s) return NULL;
    s->p = p;
    pthread_mutex_init( &s->lock, NULL );
    if (awl_thread_start( &s->thread, "screenshot", worker, s )) {
        pthread_mutex_destroy( &s->lock );
        free( s );
        return NULL;
    }
    return s;
}

int awl_screenshot_stop( awl_screenshot_t* s ) {
    job_t* j;
    if (!s) return 0;
    if (s->grabbing) end( s );
    if (awl_thread_stop( &s->thread, 2000 )) {
        awl_wrn_printf( "screenshot: thread stuck (saving?), not waiting for it\n" );
        return -1; /* s stays allocated, the thread may still use it */
    }
    while ((j = pop( &s->todo ))) job_free( j );
    while ((j = pop( &s->done ))) job_free( j );
    pthread_mutex_destroy( &s->lock );
    free( s );
    return 0;
}
