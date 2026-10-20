#include "sni.h"
#include "sni_menu.h"
#include "../awl_host.h"
#include "../awl_plugin_abi.h"
#include "../plugins.h"
#include "tray_menu.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_PATH     "/StatusNotifierItem"
/** the items' interface, and what some (older ones) call it instead */
static const char* const item_ifaces[] = { "org.kde.StatusNotifierItem", "org.freedesktop.StatusNotifierItem" };
#define RETRY_USEC (5 * 1000000ull)
/** an item's New* signals come in bursts: one GetAll after the last */
#define DEBOUNCE_USEC (10 * 1000ull)
/** pixmaps kept of an item, and their bytes in all */
#define MAX_PIXMAPS 16
#define MAX_PIXMAP_BYTES (4 * (size_t)AWL_ICON_MAX_BYTES)

typedef struct item {
    uint64_t serial;
    /** "bus/path" as the watcher lists it */
    char *id, *bus_name, *path;
    sd_bus_slot* match;
    sd_event_source* debounce;
    /** item_ifaces' index the item answers to */
    int iface;
    /** GetAll answered once; one is out; again: one more once it is back */
    int ready, fetching, again;

    char *sni_id, *title, *tip, *icon_name, *att_name, *theme_path, *menu_path;
    int status, item_is_menu;
    awl_sni_blob_t *icon_pix, *att_pix;
    int n_icon_pix, n_att_pix;

    /** what the shown icon's file was looked up for, and found */
    char* looked_up;
    awl_sni_blob_t* file;
    unsigned file_flags;
    /** the pixmaps shown, if no file */
    awl_sni_blob_t* pix;
    int npix;
    uint32_t icon_gen;
    sni_menu_t* menu;
} item_t;

/** the main thread's requests */
enum { REQ_CLICK, REQ_SCROLL, REQ_PX, REQ_MENU_REQUEST, REQ_MENU_EVENT };
typedef struct req {
    struct req* next;
    int kind, what;
    uint64_t serial;
    uint32_t gen;
    int32_t id;
    char event[16];
    int x, y, delta, horizontal;
    char monitor[64];
    int center_x;
} req_t;

/** what goes back to the main thread */
enum { MAIL_SNAP, MAIL_GONE, MAIL_TOGGLE };
typedef struct mail {
    struct mail* next;
    int kind;
    awl_tray_menu_snap_t* snap;
    uint64_t serial;
    char monitor[64];
    int center_x;
} mail_t;

struct awl_sni_t {
    sd_event* event;

    /* the loop's */
    sd_bus* bus;
    sd_bus_slot *owner_match, *signal_match, *props_match, *listing, *disconnected;
    /** bus_lost()'s, armed while the bus is to be dropped */
    sd_event_source* lost;
    sd_event_source* retry;
    sd_event_source* wake;
    item_t** items;
    int nitems, cap;
    uint64_t next_serial;
    awl_icons_t* themes;
    int px;

    /* any thread's, under lock */
    pthread_mutex_t lock;
    awl_sni_snap_t* snap;
    req_t *reqs, **reqs_tail;
    mail_t *mail, **mail_tail;
    int wake_fd;
    atomic_int want_px;

    /* the main thread's: poll_timer takes the mail, every POLL_MS while a
     * menu is open or being opened and until poll_until (an Activate that
     * may fail and open its menu), so mail never waits for a bar to be
     * drawn (none is, while hidden or locked) */
    awl_timer_t* poll_timer;
    uint64_t poll_until;
};

enum {
    /** ms between looks at the mail while some is due */
    POLL_MS = 50,
    /** how long an Activate may take to fail: sd-bus's default timeout,
     * and a bit */
    ACTIVATE_WAIT_MS = 30000,
};

/** the one there is, for tray_menu_window.c's requests and sni_menu.c */
static awl_sni_t* the;

static void bus_lost( awl_sni_t* s );

/* ---- blobs and snapshots ---- */

static awl_sni_blob_t* blob_new( size_t len ) {
    awl_sni_blob_t* b = malloc( sizeof(*b) + len );
    if (!b) return NULL;
    atomic_init( &b->ref, 1 );
    b->len = len;
    return b;
}

static awl_sni_blob_t* blob_ref( awl_sni_blob_t* b ) {
    if (b) atomic_fetch_add( &b->ref, 1 );
    return b;
}

static void blob_unref( awl_sni_blob_t* b ) {
    if (b && atomic_fetch_sub( &b->ref, 1 ) == 1) free( b );
}

static awl_sni_snap_t* snap_new( int n ) {
    awl_sni_snap_t* snap = calloc( 1, sizeof(*snap) + (size_t)n * sizeof(snap->items[0]) );
    if (snap) atomic_init( &snap->ref, 1 );
    return snap;
}

void sni_snap_unref( awl_sni_snap_t* snap ) {
    if (!snap || atomic_fetch_sub( &snap->ref, 1 ) != 1) return;
    for (int i = 0; i < snap->n; i++) {
        free( snap->items[i].name );
        blob_unref( snap->items[i].file );
        blob_unref( snap->items[i].pixmaps );
    }
    free( snap );
}

awl_sni_snap_t* sni_snap_get( awl_sni_t* s ) {
    pthread_mutex_lock( &s->lock );
    awl_sni_snap_t* snap = s->snap;
    atomic_fetch_add( &snap->ref, 1 );
    pthread_mutex_unlock( &s->lock );
    return snap;
}

static const char* item_name( const item_t* it ) {
    if (it->tip && *it->tip) return it->tip;
    if (it->title && *it->title) return it->title;
    return it->sni_id ? it->sni_id : "";
}

/** the items as they are now, for the bar */
static void publish( awl_sni_t* s ) {
    int n = 0;
    for (int i = 0; i < s->nitems; i++) n += s->items[i]->ready;
    awl_sni_snap_t* snap = snap_new( n );
    if (!snap) return;
    for (int i = 0; i < s->nitems; i++) {
        item_t* it = s->items[i];
        if (!it->ready) continue;
        awl_sni_item_t* o = &snap->items[snap->n];
        if (!(o->name = strdup( item_name( it ) ))) continue;
        o->serial = it->serial;
        o->menu = it->menu ? sni_menu_serial( it->menu ) : 0;
        o->item_is_menu = it->item_is_menu;
        o->status = it->status;
        o->icon_gen = it->icon_gen;
        o->file = blob_ref( it->file );
        o->file_flags = it->file_flags;
        o->pixmaps = it->file ? NULL : blob_ref( it->pix );
        o->npixmaps = it->file ? 0 : it->npix;
        snap->n++;
    }
    pthread_mutex_lock( &s->lock );
    awl_sni_snap_t* old = s->snap;
    s->snap = snap;
    pthread_mutex_unlock( &s->lock );
    sni_snap_unref( old );
    awl_host->redraw_request();
}

/* ---- to the main thread ---- */

/** the main thread's poll_timer takes it */
static void mail( awl_sni_t* s, mail_t* m ) {
    pthread_mutex_lock( &s->lock );
    *s->mail_tail = m;
    s->mail_tail = &m->next;
    pthread_mutex_unlock( &s->lock );
}

void sni_to_main_snap( awl_tray_menu_snap_t* snap ) {
    mail_t* m = calloc( 1, sizeof(*m) );
    if (!m || !the) {
        free( m );
        awl_tray_menu_snap_free( snap );
        return;
    }
    m->kind = MAIL_SNAP;
    m->snap = snap;
    mail( the, m );
}

void sni_to_main_gone( uint64_t serial ) {
    mail_t* m = calloc( 1, sizeof(*m) );
    if (!m || !the) {
        free( m );
        return;
    }
    m->kind = MAIL_GONE;
    m->serial = serial;
    mail( the, m );
}

static void to_main_toggle( uint64_t serial, const char* monitor, int center_x ) {
    mail_t* m = calloc( 1, sizeof(*m) );
    if (!m || !the) {
        free( m );
        return;
    }
    m->kind = MAIL_TOGGLE;
    m->serial = serial;
    snprintf( m->monitor, sizeof m->monitor, "%s", monitor );
    m->center_x = center_x;
    mail( the, m );
}

static void poll_fire( void* data );

static mail_t* take_mail( awl_sni_t* s ) {
    pthread_mutex_lock( &s->lock );
    mail_t* m = s->mail;
    s->mail = NULL;
    s->mail_tail = &s->mail;
    pthread_mutex_unlock( &s->lock );
    return m;
}

static uint64_t now_ms( void ) {
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/** poll_timer in ms */
static void poll_arm( awl_sni_t* s, int ms ) {
    if (!s->poll_timer) s->poll_timer = awl_host->timer_add( poll_fire, s );
    awl_host->timer_set( s->poll_timer, ms );
}

static void poll_fire( void* data ) {
    awl_sni_t* s = data;
    for (mail_t* m = take_mail( s ), *next; m; m = next) {
        next = m->next;
        switch (m->kind) {
        case MAIL_SNAP: awl_tray_menu_window_snap( m->snap ); break;
        case MAIL_GONE: awl_tray_menu_window_gone( m->serial ); break;
        case MAIL_TOGGLE: awl_tray_menu_window_toggle( m->serial, m->monitor, m->center_x ); break;
        }
        free( m );
    }
    if (awl_tray_menu_window_is_open() || now_ms() < s->poll_until) poll_arm( s, POLL_MS );
}

void sni_main_poll( awl_sni_t* s ) {
    pthread_mutex_lock( &s->lock );
    int any = s->mail != NULL;
    pthread_mutex_unlock( &s->lock );
    /* not from within the bar's drawing: from the loop, right after */
    if (any) poll_arm( s, 1 );
}

/* ---- the main thread's requests ---- */

static void request( awl_sni_t* s, req_t* r ) {
    pthread_mutex_lock( &s->lock );
    *s->reqs_tail = r;
    s->reqs_tail = &r->next;
    pthread_mutex_unlock( &s->lock );
    uint64_t one = 1;
    /* EAGAIN only if the counter would overflow: the loop is due anyway */
    if (write( s->wake_fd, &one, sizeof one ) < 0) {}
}

static req_t* req_new( int kind, uint64_t serial ) {
    req_t* r = calloc( 1, sizeof(*r) );
    if (r) {
        r->kind = kind;
        r->serial = serial;
    }
    return r;
}

void sni_click( awl_sni_t* s, uint64_t serial, int what, int x, int y, const char* monitor, int center_x ) {
    req_t* r = req_new( REQ_CLICK, serial );
    if (!r) return;
    r->what = what;
    r->x = x;
    r->y = y;
    snprintf( r->monitor, sizeof r->monitor, "%s", monitor ? monitor : "" );
    r->center_x = center_x;
    request( s, r );
    /* if it fails, its menu opens */
    if (what == AWL_SNI_ACTIVATE) {
        s->poll_until = now_ms() + ACTIVATE_WAIT_MS;
        poll_arm( s, POLL_MS );
    }
}

void sni_scroll( awl_sni_t* s, uint64_t serial, int delta, int horizontal ) {
    req_t* r = req_new( REQ_SCROLL, serial );
    if (!r) return;
    r->delta = delta;
    r->horizontal = horizontal;
    request( s, r );
}

void sni_set_icon_px( awl_sni_t* s, int px ) {
    if (px < 1) px = 1;
    if (px > AWL_ICON_MAX_DIM) px = AWL_ICON_MAX_DIM;
    if (atomic_exchange( &s->want_px, px ) == px) return;
    req_t* r = req_new( REQ_PX, 0 );
    if (r) request( s, r );
}

/* tray_menu_window.c's, on the main thread (tray_menu.h) */
void awl_tray_menu_request( uint64_t serial, uint32_t gen ) {
    req_t* r = the ? req_new( REQ_MENU_REQUEST, serial ) : NULL;
    if (!r) return;
    r->gen = gen;
    request( the, r );
    /* its layouts come by mail, as long as it is open */
    poll_arm( the, POLL_MS );
}

void awl_tray_menu_event( uint64_t serial, int32_t id, const char* event ) {
    req_t* r = the ? req_new( REQ_MENU_EVENT, serial ) : NULL;
    if (!r) return;
    r->id = id;
    snprintf( r->event, sizeof r->event, "%s", event );
    request( the, r );
}

/* ---- the icons ---- */

awl_icons_t* sni_themes( void ) {
    awl_sni_t* s = the;
    if (!s) return NULL;
    if (!s->themes) {
        s->themes = awl_icons_new_desktop( awl_config()->tray.icon_theme );
    }
    return s->themes;
}

/** sets *b to nb unless they hold the same bytes; returns whether it changed */
static int blob_set( awl_sni_blob_t** b, awl_sni_blob_t* nb ) {
    if (*b == nb) return 0;
    if (*b && nb && (*b)->len == nb->len && !memcmp( (*b)->data, nb->data, nb->len )) {
        blob_unref( nb );
        return 0;
    }
    blob_unref( *b );
    *b = nb;
    return 1;
}

/** the icon to show, looked up anew if what it depends on changed */
static void resolve( awl_sni_t* s, item_t* it ) {
    int att = it->status == AWL_SNI_ATTENTION && ((it->att_name && *it->att_name) || it->att_pix);
    const char* name = att ? it->att_name : it->icon_name;
    awl_sni_blob_t* pix = att ? it->att_pix : it->icon_pix;
    int npix = att ? it->n_att_pix : it->n_icon_pix;
    int changed = 0;

    char key[4096];
    snprintf( key, sizeof key, "%d\n%s\n%s", s->px, name ? name : "", it->theme_path ? it->theme_path : "" );
    if (!it->looked_up || strcmp( it->looked_up, key )) {
        free( it->looked_up );
        it->looked_up = strdup( key );
        awl_sni_blob_t* file = NULL;
        unsigned flags = 0;
        awl_icons_t* themes = sni_themes();
        if (name && *name && themes) {
            const char* extra[] = { it->theme_path, NULL };
            unsigned f = 0;
            char* path = awl_icons_lookup( themes, name, s->px, it->theme_path && *it->theme_path ? extra : NULL, &f );
            size_t len = 0;
            void* data = path ? awl_icon_read( path, &len ) : NULL;
            if (data && (file = blob_new( len ))) {
                memcpy( file->data, data, len );
                flags = f & (AWL_ICON_SVG | AWL_ICON_SYMBOLIC);
            }
            if (!data) awl_vrb_printf( "sni: item %s: icon '%s' not found\n", it->id, name );
            free( data );
            free( path );
        }
        changed |= blob_set( &it->file, file ) || flags != it->file_flags;
        it->file_flags = flags;
    }
    if (it->pix != pix) {
        blob_unref( it->pix );
        it->pix = blob_ref( pix );
        changed |= !it->file;
    }
    it->npix = npix;
    if (changed) it->icon_gen++;
}

/* ---- the items ---- */

static item_t* item_find( awl_sni_t* s, uint64_t serial ) {
    for (int i = 0; i < s->nitems; i++)
        if (s->items[i]->serial == serial) return s->items[i];
    return NULL;
}

static void item_free( awl_sni_t* s, item_t* it ) {
    sd_bus_slot_unref( it->match );
    sd_event_source_disable_unref( it->debounce );
    sni_menu_free( it->menu );
    free( it->id );
    free( it->bus_name );
    free( it->path );
    free( it->sni_id );
    free( it->title );
    free( it->tip );
    free( it->icon_name );
    free( it->att_name );
    free( it->theme_path );
    free( it->menu_path );
    free( it->looked_up );
    blob_unref( it->icon_pix );
    blob_unref( it->att_pix );
    blob_unref( it->file );
    blob_unref( it->pix );
    free( it );
}

/** a property's string (s or o) into *p */
static int read_str( sd_bus_message* m, const char* sig, char** p ) {
    const char* contents = NULL;
    if (sd_bus_message_peek_type( m, NULL, &contents ) <= 0 || !contents || strcmp( contents, sig ))
        return sd_bus_message_skip( m, "v" );
    const char* v;
    int r = sd_bus_message_read( m, "v", sig, &v );
    if (r < 0) return r;
    char* c = strdup( v );
    if (!c) return -ENOMEM;
    free( *p );
    *p = c;
    return r;
}

/** a(iiay), the variant's or ToolTip's (entered), as a blob of
 * `awl_icon_pixmap_t`s and their data; only those whose data matches
 * their size */
static int read_pixmaps( sd_bus_message* m, awl_sni_blob_t** out, int* nout ) {
    struct { int32_t w, h; const void* data; size_t len; } p[MAX_PIXMAPS];
    int n = 0, r;
    size_t total = 0;
    if ((r = sd_bus_message_enter_container( m, 'a', "(iiay)" )) <= 0) return r;
    while ((r = sd_bus_message_enter_container( m, 'r', "iiay" )) > 0) {
        int32_t w, h;
        const void* data;
        size_t len;
        if ((r = sd_bus_message_read( m, "ii", &w, &h )) < 0 ||
            (r = sd_bus_message_read_array( m, 'y', &data, &len )) < 0 ||
            (r = sd_bus_message_exit_container( m )) < 0)
            return r;
        if (n < MAX_PIXMAPS && w > 0 && h > 0 && w <= AWL_ICON_MAX_DIM && h <= AWL_ICON_MAX_DIM &&
            len == (size_t)w * (size_t)h * 4 && total + len <= MAX_PIXMAP_BYTES) {
            p[n].w = w;
            p[n].h = h;
            p[n].data = data;
            p[n].len = len;
            total += len;
            n++;
        }
    }
    if (r < 0 || (r = sd_bus_message_exit_container( m )) < 0) return r;
    awl_sni_blob_t* b = NULL;
    if (n && (b = blob_new( (size_t)n * sizeof(awl_icon_pixmap_t) + total ))) {
        awl_icon_pixmap_t* pm = (awl_icon_pixmap_t*)b->data;
        unsigned char* d = b->data + (size_t)n * sizeof(awl_icon_pixmap_t);
        for (int i = 0; i < n; i++) {
            memcpy( d, p[i].data, p[i].len );
            pm[i] = (awl_icon_pixmap_t){ p[i].w, p[i].h, d, p[i].len };
            d += p[i].len;
        }
    }
    blob_unref( *out );
    *out = b;
    *nout = b ? n : 0;
    return 1;
}

/** ToolTip (sa(iiay)ss): its title */
static int read_tooltip( sd_bus_message* m, char** tip ) {
    const char* contents = NULL;
    if (sd_bus_message_peek_type( m, NULL, &contents ) <= 0 || !contents || strcmp( contents, "(sa(iiay)ss)" ))
        return sd_bus_message_skip( m, "v" );
    const char *icon, *title, *text;
    int r;
    if ((r = sd_bus_message_enter_container( m, 'v', "(sa(iiay)ss)" )) <= 0 ||
        (r = sd_bus_message_enter_container( m, 'r', "sa(iiay)ss" )) <= 0 ||
        (r = sd_bus_message_read( m, "s", &icon )) < 0 || (r = sd_bus_message_skip( m, "a(iiay)" )) < 0 ||
        (r = sd_bus_message_read( m, "ss", &title, &text )) < 0 ||
        (r = sd_bus_message_exit_container( m )) < 0 || (r = sd_bus_message_exit_container( m )) < 0)
        return r;
    char* c = strdup( title );
    if (!c) return -ENOMEM;
    free( *tip );
    *tip = c;
    return 1;
}

static int status_of( const char* s ) {
    if (!s || !strcmp( s, "Active" )) return AWL_SNI_ACTIVE;
    if (!strcmp( s, "Passive" )) return AWL_SNI_PASSIVE;
    if (!strcmp( s, "NeedsAttention" )) return AWL_SNI_ATTENTION;
    return AWL_SNI_ACTIVE;
}

static void fetch( awl_sni_t* s, item_t* it );

/** the item's menu, made anew if its path changed */
static void set_menu( awl_sni_t* s, item_t* it ) {
    const char* path = it->menu_path;
    int none = !path || !*path || !strcmp( path, "/" );
    if (it->menu && !none && !strcmp( sni_menu_path( it->menu ), path )) {
        sni_menu_set_theme_path( it->menu, it->theme_path );
        return;
    }
    sni_menu_free( it->menu );
    it->menu = none ? NULL : sni_menu_new( s->bus, it->bus_name, path, it->theme_path );
}

/** GetAll -> a{sv} */
static int on_props( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_sni_t* s = the;
    item_t* it = s ? item_find( s, (uint64_t)(uintptr_t)userdata ) : NULL;
    if (!it) return 0;
    it->fetching = 0;
    int nprops = 0, r = 0;
    char* status = NULL;
    if (!sd_bus_message_is_method_error( m, NULL ) && sd_bus_message_enter_container( m, 'a', "{sv}" ) > 0) {
        while ((r = sd_bus_message_enter_container( m, 'e', "sv" )) > 0) {
            const char* key;
            if ((r = sd_bus_message_read( m, "s", &key )) < 0) break;
            nprops++;
            const char* contents = NULL;
            if (!strcmp( key, "Id" )) r = read_str( m, "s", &it->sni_id );
            else if (!strcmp( key, "Title" )) r = read_str( m, "s", &it->title );
            else if (!strcmp( key, "Status" )) r = read_str( m, "s", &status );
            else if (!strcmp( key, "IconName" )) r = read_str( m, "s", &it->icon_name );
            else if (!strcmp( key, "AttentionIconName" )) r = read_str( m, "s", &it->att_name );
            else if (!strcmp( key, "IconThemePath" )) r = read_str( m, "s", &it->theme_path );
            else if (!strcmp( key, "Menu" )) r = read_str( m, "o", &it->menu_path );
            else if (!strcmp( key, "ToolTip" )) r = read_tooltip( m, &it->tip );
            else if ((!strcmp( key, "IconPixmap" ) || !strcmp( key, "AttentionIconPixmap" )) &&
                     sd_bus_message_peek_type( m, NULL, &contents ) > 0 && contents && !strcmp( contents, "a(iiay)" )) {
                int att = key[0] == 'A';
                if ((r = sd_bus_message_enter_container( m, 'v', "a(iiay)" )) > 0 &&
                    (r = read_pixmaps( m, att ? &it->att_pix : &it->icon_pix, att ? &it->n_att_pix : &it->n_icon_pix )) >= 0)
                    r = sd_bus_message_exit_container( m );
            } else if (!strcmp( key, "ItemIsMenu" ) &&
                       sd_bus_message_peek_type( m, NULL, &contents ) > 0 && contents && !strcmp( contents, "b" )) {
                int b = 0;
                r = sd_bus_message_read( m, "v", "b", &b );
                it->item_is_menu = b;
            } else
                r = sd_bus_message_skip( m, "v" );
            if (r < 0 || (r = sd_bus_message_exit_container( m )) < 0) break;
        }
    }
    if (r < 0) awl_wrn_printf( "sni: item %s: broken properties\n", it->id );
    if (!nprops && !it->ready && it->iface + 1 < (int)(sizeof item_ifaces / sizeof *item_ifaces)) {
        /* nothing under the KDE name: the freedesktop one */
        it->iface++;
        free( status );
        fetch( s, it );
        return 0;
    }
    if (status) it->status = status_of( status );
    free( status );
    if (nprops) {
        if (!it->ready) awl_vrb_printf( "sni: item %s ('%s') is there\n", it->id, item_name( it ) );
        it->ready = 1;
        set_menu( s, it );
        resolve( s, it );
        publish( s );
    } else if (!it->ready) {
        awl_vrb_printf( "sni: item %s doesn't answer\n", it->id );
    }
    if (it->again) {
        it->again = 0;
        fetch( s, it );
    }
    return 0;
}

static void fetch( awl_sni_t* s, item_t* it ) {
    if (it->fetching) {
        it->again = 1;
        return;
    }
    if (sd_bus_call_method_async( s->bus, NULL, it->bus_name, it->path, "org.freedesktop.DBus.Properties", "GetAll",
                                  on_props, (void*)(uintptr_t)it->serial, "s", item_ifaces[it->iface] ) < 0)
        bus_lost( s );
    else
        it->fetching = 1;
}

static int debounce_fire( sd_event_source* e, uint64_t usec, void* userdata ) {
    (void)usec;
    awl_sni_t* s = the;
    item_t* it = s ? item_find( s, (uint64_t)(uintptr_t)userdata ) : NULL;
    sd_event_source_disable_unref( e );
    if (!it) return 0;
    it->debounce = NULL;
    fetch( s, it );
    return 0;
}

/** any signal of the item: its New* ones re-read the properties */
static int on_item_signal( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_sni_t* s = the;
    item_t* it = s ? item_find( s, (uint64_t)(uintptr_t)userdata ) : NULL;
    const char* member = sd_bus_message_get_member( m );
    if (!it || !member || (strncmp( member, "New", 3 ) && strncmp( member, "XAyatanaNew", 11 ))) return 0;
    if (!it->debounce && sd_event_add_time_relative( s->event, &it->debounce, CLOCK_MONOTONIC, DEBOUNCE_USEC, 0,
                                                      debounce_fire, userdata ) < 0)
        it->debounce = NULL;
    return 0;
}

static item_t* item_new( awl_sni_t* s, const char* id ) {
    item_t* it = calloc( 1, sizeof(*it) );
    if (!it) return NULL;
    const char* slash = strchr( id, '/' );
    it->serial = ++s->next_serial;
    it->status = AWL_SNI_ACTIVE;
    it->id = strdup( id );
    it->bus_name = slash ? strndup( id, (size_t)(slash - id) ) : strdup( id );
    it->path = strdup( slash ? slash : ITEM_PATH );
    if (!it->id || !it->bus_name || !it->path || !sd_bus_service_name_is_valid( it->bus_name ) ||
        !sd_bus_object_path_is_valid( it->path )) {
        if (it->id) awl_wrn_printf( "sni: watcher lists a broken item '%s'\n", id );
        item_free( s, it );
        return NULL;
    }
    char rule[1024];
    snprintf( rule, sizeof rule, "type='signal',sender='%s',path='%s'", it->bus_name, it->path );
    if (sd_bus_add_match_async( s->bus, &it->match, rule, on_item_signal, NULL, (void*)(uintptr_t)it->serial ) < 0)
        bus_lost( s );
    /* after the match: a change between the two isn't missed */
    fetch( s, it );
    return it;
}

/** the items the watcher lists now: new ones added at the end, gone ones
 * dropped */
static void sync_items( awl_sni_t* s, char** ids ) {
    int changed = 0;
    for (int i = s->nitems - 1; i >= 0; i--) {
        char** id = ids;
        for (; id && *id && strcmp( *id, s->items[i]->id ); id++)
            ;
        if (id && *id) continue;
        awl_vrb_printf( "sni: item %s gone\n", s->items[i]->id );
        changed |= s->items[i]->ready;
        item_free( s, s->items[i] );
        memmove( &s->items[i], &s->items[i + 1], (size_t)(s->nitems - i - 1) * sizeof(*s->items) );
        s->nitems--;
    }
    for (char** id = ids; id && *id; id++) {
        int i = 0;
        for (; i < s->nitems && strcmp( s->items[i]->id, *id ); i++)
            ;
        if (i < s->nitems) continue;
        if (s->nitems == s->cap) {
            int cap = s->cap ? 2 * s->cap : 16;
            item_t** items = realloc( s->items, (size_t)cap * sizeof(*items) );
            if (!items) break;
            s->items = items;
            s->cap = cap;
        }
        item_t* it = item_new( s, *id );
        if (it) s->items[s->nitems++] = it;
    }
    if (changed) publish( s );
}

static void free_strv( char** v ) {
    for (char** p = v; p && *p; p++) free( *p );
    free( v );
}

/** Get( RegisteredStatusNotifierItems ) -> v(as) */
static int on_listing( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_sni_t* s = userdata;
    s->listing = sd_bus_slot_unref( s->listing );
    char** ids = NULL;
    if (sd_bus_message_is_method_error( m, NULL ) || sd_bus_message_enter_container( m, 'v', "as" ) <= 0 ||
        sd_bus_message_read_strv( m, &ids ) < 0) {
        const sd_bus_error* e = sd_bus_message_get_error( m );
        awl_wrn_printf( "sni: the watcher's items: %s\n", e && e->message ? e->message : "broken" );
        return 0;
    }
    sync_items( s, ids );
    free_strv( ids );
    return 0;
}

/** asks the watcher for its items; a listing out already is dropped */
static void list_items( awl_sni_t* s ) {
    s->listing = sd_bus_slot_unref( s->listing );
    if (sd_bus_call_method_async( s->bus, &s->listing, WATCHER_NAME, WATCHER_PATH, "org.freedesktop.DBus.Properties",
                                  "Get", on_listing, s, "ss", WATCHER_IFACE, "RegisteredStatusNotifierItems" ) < 0)
        bus_lost( s );
}

static void watcher_there( awl_sni_t* s ) {
    const char* me = NULL;
    sd_bus_get_unique_name( s->bus, &me );
    if (me)
        sd_bus_call_method_async( s->bus, NULL, WATCHER_NAME, WATCHER_PATH, WATCHER_IFACE,
                                  "RegisterStatusNotifierHost", NULL, NULL, "s", me );
    list_items( s );
}

static void watcher_gone( awl_sni_t* s ) {
    s->listing = sd_bus_slot_unref( s->listing );
    sync_items( s, NULL );
}

/** the watcher's ItemRegistered, ItemUnregistered and PropertiesChanged:
 * the list anew */
static int on_watcher_signal( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)m; (void)ret_error;
    list_items( userdata );
    return 0;
}

/** NameOwnerChanged(s name, s old_owner, s new_owner) of the watcher */
static int on_owner_changed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_sni_t* s = userdata;
    const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read( m, "sss", &name, &old_owner, &new_owner ) < 0) return 0;
    awl_vrb_printf( "sni: the watcher %s\n", *new_owner ? "is there" : "is gone" );
    if (*new_owner) watcher_there( s );
    else watcher_gone( s );
    return 0;
}

/** GetNameOwner(s) -> s; NameHasNoOwner while there is no watcher */
static int on_owner( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_sni_t* s = userdata;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    if (!e) watcher_there( s );
    else if (sd_bus_error_has_name( e, SD_BUS_ERROR_NAME_HAS_NO_OWNER ))
        /* awl's own may just not have its name yet */
        awl_vrb_printf( "sni: no watcher yet (awl's, awl_sni.h), waiting for one\n" );
    else
        bus_lost( s );
    return 0;
}

static int on_subscribed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_sni_t* s = userdata;
    if (sd_bus_message_is_method_error( m, NULL )) bus_lost( s );
    return 0;
}

/* ---- the requests, on the loop ---- */

typedef struct {
    uint64_t serial;
    char monitor[64];
    int center_x;
} activate_t;

/** An item that says it is no menu (ItemIsMenu false) but can't be
 * activated after all (no Activate, or it fails): its menu, as for one that
 * is; one gone or hanging: nothing. */
static int on_activate( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    activate_t* a = userdata;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    item_t* it = the && e ? item_find( the, a->serial ) : NULL;
    if (it && !sd_bus_error_has_name( e, SD_BUS_ERROR_NO_REPLY ) &&
        !sd_bus_error_has_name( e, SD_BUS_ERROR_TIMEOUT )) {
        awl_vrb_printf( "sni: item %s: Activate failed (%s), its menu instead\n", it->id,
                        e->message ? e->message : e->name );
        if (it->menu) to_main_toggle( sni_menu_serial( it->menu ), a->monitor, a->center_x );
    }
    free( a );
    return 0;
}

static void run( awl_sni_t* s, req_t* r ) {
    if (r->kind == REQ_PX) {
        s->px = atomic_load( &s->want_px );
        for (int i = 0; i < s->nitems; i++)
            if (s->items[i]->ready) resolve( s, s->items[i] );
        publish( s );
        return;
    }
    if (r->kind == REQ_MENU_REQUEST || r->kind == REQ_MENU_EVENT) {
        sni_menu_t* menu = s->bus ? sni_menu_find( r->serial ) : NULL;
        if (!menu) {
            /* it went away before the main thread knew: let it know */
            if (r->kind == REQ_MENU_REQUEST) sni_to_main_gone( r->serial );
        } else if (r->kind == REQ_MENU_REQUEST) {
            sni_menu_request( menu, r->gen );
        } else {
            sni_menu_event( menu, r->id, r->event );
        }
        return;
    }
    item_t* it = s->bus ? item_find( s, r->serial ) : NULL;
    if (!it) return;
    const char* iface = item_ifaces[it->iface];
    if (r->kind == REQ_SCROLL) {
        sd_bus_call_method_async( s->bus, NULL, it->bus_name, it->path, iface, "Scroll", NULL, NULL, "is", r->delta,
                                  r->horizontal ? "horizontal" : "vertical" );
    } else if (r->what == AWL_SNI_ACTIVATE) {
        activate_t* a = calloc( 1, sizeof(*a) );
        if (!a) return;
        a->serial = it->serial;
        snprintf( a->monitor, sizeof a->monitor, "%s", r->monitor );
        a->center_x = r->center_x;
        if (sd_bus_call_method_async( s->bus, NULL, it->bus_name, it->path, iface, "Activate", on_activate, a, "ii",
                                      r->x, r->y ) < 0)
            free( a );
    } else {
        sd_bus_call_method_async( s->bus, NULL, it->bus_name, it->path, iface,
                                  r->what == AWL_SNI_SECONDARY ? "SecondaryActivate" : "ContextMenu", NULL, NULL,
                                  "ii", r->x, r->y );
    }
}

static int wake_callback( sd_event_source* e, int fd, uint32_t revents, void* userdata ) {
    (void)e; (void)revents;
    awl_sni_t* s = userdata;
    uint64_t count;
    if (read( fd, &count, sizeof count ) < 0) {}
    pthread_mutex_lock( &s->lock );
    req_t* r = s->reqs;
    s->reqs = NULL;
    s->reqs_tail = &s->reqs;
    pthread_mutex_unlock( &s->lock );
    for (req_t* next; r; r = next) {
        next = r->next;
        run( s, r );
        free( r );
    }
    return 0;
}

/* ---- the connection ---- */

static void schedule_retry( awl_sni_t* s, uint64_t usec );

static void drop_bus( awl_sni_t* s ) {
    s->lost = sd_event_source_disable_unref( s->lost );
    for (int i = 0; i < s->nitems; i++) item_free( s, s->items[i] );
    s->nitems = 0;
    s->listing = sd_bus_slot_unref( s->listing );
    s->owner_match = sd_bus_slot_unref( s->owner_match );
    s->signal_match = sd_bus_slot_unref( s->signal_match );
    s->props_match = sd_bus_slot_unref( s->props_match );
    s->disconnected = sd_bus_slot_unref( s->disconnected );
    if (s->bus) {
        sd_bus_detach_event( s->bus );
        /* not sd_bus_flush_close_unref(): that blocks until the queue is out */
        sd_bus_close( s->bus );
    }
    s->bus = sd_bus_unref( s->bus );
}

static int lost_fire( sd_event_source* e, void* userdata ) {
    (void)e;
    awl_sni_t* s = userdata;
    awl_wrn_printf( "sni: lost the session bus, reconnecting\n" );
    drop_bus( s );
    publish( s );
    schedule_retry( s, RETRY_USEC );
    return 0;
}

/** The bus failed us (a call that couldn't be sent, a match refused, the
 * connection gone): it is dropped and connected anew, from the loop's next
 * round, not from within the sd-bus callback that tells it. */
static void bus_lost( awl_sni_t* s ) {
    if (!s->bus || s->lost) return;
    if (sd_event_add_defer( s->event, &s->lost, lost_fire, s ) < 0) s->lost = NULL;
}

/** the connection's end, which sd-bus tells as a local Disconnected signal */
static int on_message( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    if (sd_bus_message_is_signal( m, "org.freedesktop.DBus.Local", "Disconnected" )) bus_lost( userdata );
    return 0;
}

static void connect_bus( awl_sni_t* s ) {
    s->px = atomic_load( &s->want_px );
    /* sd-bus runs on the loop by itself (sd_bus_attach_event()). The bus
     * handles these in order: the matches are in place before the owner is
     * asked for, so no change slips through in between */
    if (sd_bus_open_user_with_description( &s->bus, "awl-sni-host" ) < 0 ||
        sd_bus_attach_event( s->bus, s->event, SD_EVENT_PRIORITY_NORMAL ) < 0 ||
        sd_bus_add_filter( s->bus, &s->disconnected, on_message, s ) < 0 ||
        sd_bus_add_match_async( s->bus, &s->owner_match,
                "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" WATCHER_NAME "'",
                on_owner_changed, on_subscribed, s ) < 0 ||
        sd_bus_add_match_async( s->bus, &s->signal_match,
                "type='signal',sender='" WATCHER_NAME "',path='" WATCHER_PATH "',interface='" WATCHER_IFACE "'",
                on_watcher_signal, on_subscribed, s ) < 0 ||
        sd_bus_add_match_async( s->bus, &s->props_match,
                "type='signal',sender='" WATCHER_NAME "',path='" WATCHER_PATH "',"
                "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                on_watcher_signal, on_subscribed, s ) < 0 ||
        sd_bus_call_method_async( s->bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetNameOwner", on_owner, s, "s", WATCHER_NAME ) < 0) {
        awl_wrn_printf( "sni: no session bus, retrying\n" );
        drop_bus( s );
        publish( s );
        schedule_retry( s, RETRY_USEC );
    }
}

static int retry_callback( sd_event_source* e, uint64_t usec, void* userdata ) {
    (void)usec;
    awl_sni_t* s = userdata;
    s->retry = sd_event_source_disable_unref( e );
    connect_bus( s );
    return 0;
}

static void schedule_retry( awl_sni_t* s, uint64_t usec ) {
    if (s->retry) return;
    if (sd_event_add_time_relative( s->event, &s->retry, CLOCK_MONOTONIC, usec, 0, retry_callback, s ) < 0)
        s->retry = NULL;
}

awl_sni_t* sni_new( sd_event* event ) {
    if (the) return NULL;
    awl_sni_t* s = calloc( 1, sizeof(*s) );
    if (!s) return NULL;
    s->event = sd_event_ref( event );
    s->wake_fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    pthread_mutex_init( &s->lock, NULL );
    s->reqs_tail = &s->reqs;
    s->mail_tail = &s->mail;
    atomic_init( &s->want_px, 16 );
    if (s->wake_fd < 0 || !(s->snap = snap_new( 0 )) ||
        sd_event_add_io( event, &s->wake, s->wake_fd, EPOLLIN, wake_callback, s ) < 0)
        goto fail;
    the = s;
    /* the first connection from the loop: no socket work on the caller's thread */
    schedule_retry( s, 0 );
    if (!s->retry) goto fail;
    return s;
fail:
    the = NULL;
    sd_event_source_disable_unref( s->wake );
    if (s->wake_fd >= 0) close( s->wake_fd );
    sd_event_unref( s->event );
    sni_snap_unref( s->snap );
    pthread_mutex_destroy( &s->lock );
    free( s );
    return NULL;
}

void sni_free( awl_sni_t* s ) {
    if (!s) return;
    awl_host->timer_remove( s->poll_timer );
    /* the loop has stopped (awl_loop_stop()), so the "closed" of a menu that
     * was open (tray_menu_window.c's fini queued it) goes out from here,
     * still on the bus: the item would think it open otherwise. Only
     * written out, if the socket takes it at once; the rest is dropped */
    for (req_t* r = s->reqs; r; r = r->next)
        if (r->kind == REQ_MENU_EVENT && !strcmp( r->event, "closed" )) run( s, r );
    drop_bus( s );
    sd_event_source_disable_unref( s->retry );
    sd_event_source_disable_unref( s->wake );
    close( s->wake_fd );
    for (mail_t* m = take_mail( s ), *next; m; m = next) {
        next = m->next;
        awl_tray_menu_snap_free( m->snap );
        free( m );
    }
    for (req_t* r = s->reqs, *next; r; r = next) {
        next = r->next;
        free( r );
    }
    awl_icons_free( s->themes );
    free( s->items );
    sni_snap_unref( s->snap );
    pthread_mutex_destroy( &s->lock );
    sd_event_unref( s->event );
    the = NULL;
    free( s );
}
