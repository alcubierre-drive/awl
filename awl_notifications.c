#include "awl_notifications.h"
#include "awl_bus.h"
#include "awl_config.h"
#include "awl_log.h"
#include "version.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <wayland-server-core.h>

#define NAME  "org.freedesktop.Notifications"
#define PATH  "/org/freedesktop/Notifications"
#define IFACE "org.freedesktop.Notifications"

enum {
    /** open at most; a new one closes the oldest */
    MAX_OPEN = 256,
    /** action pairs kept at most */
    MAX_ACTIONS = 16,
    /** the open ones' image data at most, bytes */
    MAX_IMAGES = 64 << 20,
    /** a side of image data at most, as icons/icon.h's AWL_ICON_MAX_DIM */
    MAX_DIM = 1024,
    /** bytes kept at most of the body (plugins/markup.h's AWL_MARKUP_MAX),
     * the summary, a path, the others */
    MAX_BODY = 16384, MAX_SUMMARY = 1024, MAX_PATH = 4096, MAX_SHORT = 256,
};

/** an open notification */
typedef struct {
    /** what the library sees; its strings are owned */
    awl_notif_t pub;
    /** pub.actions' strings */
    char** actions;
    /** pub.image_data.data's */
    uint8_t* pixels;
    size_t npixels;
    /** x-canonical-private-synchronous or x-dunst-stack-tag, "" none */
    char* tag;
    struct wl_event_source* timer;
    int held;
} notif_t;

static struct {
    struct wl_event_loop* loop;
    void (*changed)( void );
    struct wl_event_source* idle;
    awl_bus_t* conn;
    sd_bus* bus;
    sd_bus_slot* vtable;
    /** newest first */
    notif_t* open[MAX_OPEN];
    int nopen;
    /** the next new id; ids below it were given out */
    uint32_t next_id;
    uint64_t generation;
    size_t image_bytes;
} N = { .next_id = 1 };

/* ---- the store ---- */

static void changed_fire( void* data ) {
    (void)data;
    N.idle = NULL;
    if (N.changed) N.changed();
}

/** the library hears of it once this loop round is done */
static void changed( void ) {
    if (!N.idle && N.loop) N.idle = wl_event_loop_add_idle( N.loop, changed_fire, NULL );
}

static int find( uint32_t id ) {
    for (int i = 0; i < N.nopen; i++)
        if (N.open[i]->pub.id == id) return i;
    return -1;
}

/** s, at most max bytes of it, cut at a character */
static char* copy( const char* s, size_t max ) {
    size_t len = strnlen( s, max + 1 );
    if (len > max) {
        len = max;
        while (len && ((unsigned char)s[len] & 0xc0) == 0x80) len--;
    }
    return strndup( s, len );
}

/** frees what n holds, not n */
static void contents_free( notif_t* n ) {
    free( (char*)n->pub.app );
    free( (char*)n->pub.summary );
    free( (char*)n->pub.body );
    free( (char*)n->pub.image_path );
    free( (char*)n->pub.app_icon );
    free( (char*)n->pub.desktop_entry );
    free( (char*)n->pub.category );
    for (int i = 0; i < 2 * n->pub.nactions; i++) free( n->actions[i] );
    free( n->actions );
    free( n->pixels );
    free( n->tag );
}

static void notif_free( notif_t* n ) {
    if (!n) return;
    if (n->timer) wl_event_source_remove( n->timer );
    contents_free( n );
    free( n );
}

static int expire( void* data );

/** (re)starts n's timer, unless it is held or has no timeout */
static void arm( notif_t* n ) {
    if (n->held || n->pub.timeout <= 0) {
        if (n->timer) wl_event_source_timer_update( n->timer, 0 );
        return;
    }
    if (!n->timer) n->timer = wl_event_loop_add_timer( N.loop, expire, n );
    if (n->timer) wl_event_source_timer_update( n->timer, n->pub.timeout );
}

/** removes open[i], telling its clients why */
static void close_at( int i, awl_notif_reason_t reason ) {
    notif_t* n = N.open[i];
    awl_vrb_printf( "notifications: %u closed (%d)\n", n->pub.id, reason );
    /* awl's own have no client to tell */
    if (N.bus && !(n->pub.flags & AWL_NOTIF_AWL)) sd_bus_emit_signal( N.bus, PATH, IFACE, "NotificationClosed", "uu", n->pub.id, (uint32_t)reason );
    N.image_bytes -= n->npixels;
    memmove( &N.open[i], &N.open[i + 1], (size_t)(N.nopen - i - 1) * sizeof(*N.open) );
    N.nopen--;
    notif_free( n );
    changed();
}

static int expire( void* data ) {
    notif_t* n = data;
    for (int i = 0; i < N.nopen; i++)
        if (N.open[i] == n) close_at( i, AWL_NOTIF_EXPIRED );
    return 0;
}

int awl_notifications_list( const awl_notif_t** out, int max ) {
    for (int i = 0; i < N.nopen && i < max; i++) out[i] = &N.open[i]->pub;
    return N.nopen;
}

void awl_notifications_close( uint32_t id, awl_notif_reason_t reason ) {
    int i = find( id );
    if (i >= 0) close_at( i, reason );
}

void awl_notifications_invoke( uint32_t id, const char* key, const char* token ) {
    int i = find( id );
    if (i < 0 || !key) return;
    notif_t* n = N.open[i];
    int a = 0;
    while (a < n->pub.nactions && strcmp( n->actions[2 * a], key )) a++;
    if (a == n->pub.nactions) {
        awl_wrn_printf( "notifications: %u has no action '%s'\n", id, key );
        return;
    }
    awl_vrb_printf( "notifications: %u action '%s'\n", id, key );
    if (N.bus) {
        if (token) sd_bus_emit_signal( N.bus, PATH, IFACE, "ActivationToken", "us", id, token );
        sd_bus_emit_signal( N.bus, PATH, IFACE, "ActionInvoked", "us", id, key );
    }
    if (!(n->pub.flags & AWL_NOTIF_RESIDENT)) close_at( i, AWL_NOTIF_DISMISSED );
}

void awl_notifications_hold( uint32_t id, int held ) {
    int i = find( id );
    if (i < 0 || N.open[i]->held == !!held) return;
    N.open[i]->held = !!held;
    arm( N.open[i] );
}

/* ---- Notify ---- */

/** an integer of any D-Bus type (clients differ: value comes as i, u or
 * y) into *v, clamped to int; 0 and nothing read if it is none */
static int read_int( sd_bus_message* m, const char* type, int* v ) {
    union { uint8_t y; int b; int16_t n; uint16_t q; int32_t i; uint32_t u; int64_t x; uint64_t t; } u;
    if (strlen( type ) != 1 || !strchr( "ybnqiuxt", *type )) return 0;
    if (sd_bus_message_read_basic( m, *type, &u ) < 0) return -1;
    int64_t x;
    switch (*type) {
    case 'y': x = u.y; break;
    case 'b': x = u.b; break;
    case 'n': x = u.n; break;
    case 'q': x = u.q; break;
    case 'i': x = u.i; break;
    case 'u': x = u.u; break;
    case 'x': x = u.x; break;
    default: x = u.t > INT64_MAX ? INT64_MAX : (int64_t)u.t; break;
    }
    *v = x > INT32_MAX ? INT32_MAX : x < INT32_MIN ? INT32_MIN : (int)x;
    return 1;
}

/** a string hint into *out (replacing what was there), at most max bytes */
static int read_str( sd_bus_message* m, const char* type, char** out, size_t max ) {
    const char* s;
    if (strcmp( type, "s" )) return 0;
    int r = sd_bus_message_read_basic( m, 's', &s );
    if (r < 0) return r;
    char* c = copy( s, max );
    if (!c) return -ENOMEM;
    free( *out );
    *out = c;
    return 1;
}

/** image-data's (iiibiiay) into n, rows packed, if it is one awl takes
 * (then *taken) */
static int read_image( sd_bus_message* m, const char* type, notif_t* n, int* taken ) {
    int32_t w, h, stride, bits, channels;
    int alpha;
    const void* data;
    size_t size;
    int r;
    if (strcmp( type, "(iiibiiay)" )) return 0;
    if ((r = sd_bus_message_enter_container( m, 'r', "iiibiiay" )) < 0 ||
        (r = sd_bus_message_read( m, "iiibii", &w, &h, &stride, &alpha, &bits, &channels )) < 0 ||
        (r = sd_bus_message_read_array( m, 'y', &data, &size )) < 0 ||
        (r = sd_bus_message_exit_container( m )) < 0)
        return r;
    if (bits != 8 || channels != (alpha ? 4 : 3) || w < 1 || h < 1 || w > MAX_DIM || h > MAX_DIM ||
        stride < w * channels || (uint64_t)(h - 1) * (uint64_t)stride + (uint64_t)(w * channels) > size) {
        awl_vrb_printf( "notifications: image data %dx%d, stride %d, %d bits, %d channels, %zu bytes refused\n",
                        w, h, stride, bits, channels, size );
        return 1;
    }
    size_t row = (size_t)(w * channels), len = row * (size_t)h;
    if (N.image_bytes + len > MAX_IMAGES) {
        awl_wrn_printf( "notifications: too much image data open, one dropped\n" );
        return 1;
    }
    uint8_t* pixels = malloc( len );
    if (!pixels) return -ENOMEM;
    for (int y = 0; y < h; y++)
        memcpy( pixels + (size_t)y * row, (const uint8_t*)data + (size_t)y * (size_t)stride, row );
    free( n->pixels );
    n->pixels = pixels;
    n->npixels = len;
    n->pub.image_data.data = pixels;
    n->pub.image_data.w = w;
    n->pub.image_data.h = h;
    n->pub.image_data.stride = (int)row;
    n->pub.image_data.has_alpha = !!alpha;
    n->pub.image_data.channels = channels;
    *taken = 1;
    return 1;
}

/** one hint, its value's type type, into n; what isn't taken is skipped.
 * *rank is the image data's so far: image-data beats image_data beats
 * icon_data. */
static int hint( sd_bus_message* m, const char* key, const char* type, notif_t* n, int* rank ) {
    static const char* images[] = { "icon_data", "image_data", "image-data" };
    int r = 0, v, image = -1, taken = 0;
    for (int i = 0; i < 3; i++)
        if (!strcmp( key, images[i] )) image = i;
    if (image >= 0) {
        if (image >= *rank && (r = read_image( m, type, n, &taken )) > 0 && taken) *rank = image + 1;
    } else if (!strcmp( key, "urgency" )) {
        if ((r = read_int( m, type, &v )) > 0)
            n->pub.urgency = v <= 0 ? AWL_NOTIF_LOW : v == 1 ? AWL_NOTIF_NORMAL : AWL_NOTIF_CRITICAL;
    } else if (!strcmp( key, "value" )) {
        if ((r = read_int( m, type, &v )) > 0) n->pub.value = v < 0 ? 0 : v > 100 ? 100 : v;
    } else if (!strcmp( key, "transient" ) || !strcmp( key, "resident" )) {
        int flag = *key == 't' ? AWL_NOTIF_TRANSIENT : AWL_NOTIF_RESIDENT;
        if ((r = read_int( m, type, &v )) > 0) n->pub.flags = v ? n->pub.flags | flag : n->pub.flags & ~flag;
    } else if (!strcmp( key, "category" )) {
        r = read_str( m, type, (char**)&n->pub.category, MAX_SHORT );
    } else if (!strcmp( key, "desktop-entry" )) {
        r = read_str( m, type, (char**)&n->pub.desktop_entry, MAX_SHORT );
    } else if (!strcmp( key, "image-path" ) || !strcmp( key, "image_path" )) {
        r = read_str( m, type, (char**)&n->pub.image_path, MAX_PATH );
    } else if (!strcmp( key, "x-canonical-private-synchronous" ) || !strcmp( key, "x-dunst-stack-tag" )) {
        r = read_str( m, type, &n->tag, MAX_SHORT );
    }
    if (r < 0) return r;
    return r ? 0 : sd_bus_message_skip( m, type );
}

static int read_hints( sd_bus_message* m, notif_t* n ) {
    int r, rank = 0;
    if ((r = sd_bus_message_enter_container( m, 'a', "{sv}" )) < 0) return r;
    while ((r = sd_bus_message_enter_container( m, 'e', "sv" )) > 0) {
        const char *key, *type;
        if ((r = sd_bus_message_read_basic( m, 's', &key )) < 0 ||
            (r = sd_bus_message_peek_type( m, NULL, &type )) < 0 ||
            (r = sd_bus_message_enter_container( m, 'v', type )) < 0 ||
            (r = hint( m, key, type, n, &rank )) < 0 ||
            (r = sd_bus_message_exit_container( m )) < 0 ||
            (r = sd_bus_message_exit_container( m )) < 0)
            return r;
    }
    return r < 0 ? r : sd_bus_message_exit_container( m );
}

/** the actions' key, label pairs, at most MAX_ACTIONS; an odd one out is
 * dropped */
static int read_actions( sd_bus_message* m, notif_t* n ) {
    const char* s[2 * MAX_ACTIONS];
    int r, k = 0;
    if ((r = sd_bus_message_enter_container( m, 'a', "s" )) < 0) return r;
    const char* x;
    while ((r = sd_bus_message_read_basic( m, 's', &x )) > 0)
        if (k < 2 * MAX_ACTIONS) s[k++] = x;
    if (r < 0 || (r = sd_bus_message_exit_container( m )) < 0) return r;
    k &= ~1;
    if (!k) return 0;
    if (!(n->actions = calloc( (size_t)k, sizeof(*n->actions) ))) return -ENOMEM;
    for (int i = 0; i < k; i++) {
        if (!(n->actions[i] = copy( s[i], MAX_SHORT ))) {
            n->pub.nactions = (i + 1) / 2;
            return -ENOMEM;
        }
    }
    n->pub.nactions = k / 2;
    n->pub.actions = (const char* const*)n->actions;
    return 0;
}

/** fills in n's strings that are still NULL with "" */
static int strings_set( notif_t* n ) {
    char** s[] = { (char**)&n->pub.app, (char**)&n->pub.summary, (char**)&n->pub.body,
                   (char**)&n->pub.image_path, (char**)&n->pub.app_icon, (char**)&n->pub.desktop_entry,
                   (char**)&n->pub.category, &n->tag };
    for (size_t i = 0; i < sizeof(s) / sizeof(*s); i++)
        if (!*s[i] && !(*s[i] = strdup( "" ))) return -ENOMEM;
    return 0;
}

/** whether open[i] is of n's kind: both awl's own or both a client's */
static int same_kind( int i, const notif_t* n ) {
    return !((N.open[i]->pub.flags ^ n->pub.flags) & AWL_NOTIF_AWL);
}

/** the open one that n replaces: replaces_id's, else the one with n's tag
 * from n's app; only one of n's kind (no client replaces awl's own) */
static int replaced( const notif_t* n, uint32_t replaces_id ) {
    if (replaces_id) {
        int i = find( replaces_id );
        return i >= 0 && same_kind( i, n ) ? i : -1;
    }
    for (int i = 0; *n->tag && i < N.nopen; i++)
        if (same_kind( i, n ) && !strcmp( N.open[i]->tag, n->tag ) && !strcmp( N.open[i]->pub.app, n->pub.app ))
            return i;
    return -1;
}

/** an id for a new one: replaces_id if awl gave it out and it is closed,
 * else the next, never 0 or an open one's */
static uint32_t new_id( uint32_t replaces_id ) {
    if (replaces_id && replaces_id < N.next_id && find( replaces_id ) < 0) return replaces_id;
    while (!N.next_id || find( N.next_id ) >= 0) N.next_id++;
    return N.next_id++;
}

/** the timeout for urgency when the sender leaves it to the server */
static int default_timeout( awl_notif_urgency_t urgency ) {
    return urgency == AWL_NOTIF_CRITICAL ? 0
         : urgency == AWL_NOTIF_LOW ? cfg->notifications.timeout_low
         : cfg->notifications.timeout_normal;
}

/** opens n (complete, with its generation), or replaces the open one it
 * replaces with it; returns its id. n is the store's now. */
static uint32_t post( notif_t* n, uint32_t replaces_id ) {
    int i = replaced( n, replaces_id );
    if (i >= 0) {
        /* in place: its id, place, timer (its data is old) and hold stay */
        notif_t* old = N.open[i];
        N.image_bytes = N.image_bytes - old->npixels + n->npixels;
        n->pub.id = old->pub.id;
        n->timer = old->timer;
        n->held = old->held;
        contents_free( old );
        *old = *n;
        free( n );
        n = old;
        awl_vrb_printf( "notifications: %u replaced\n", n->pub.id );
    } else {
        if (N.nopen == MAX_OPEN) {
            int o = N.nopen - 1;
            while (o > 0 && N.open[o]->pub.urgency == AWL_NOTIF_CRITICAL) o--;
            if (N.open[o]->pub.urgency == AWL_NOTIF_CRITICAL) o = N.nopen - 1;
            close_at( o, AWL_NOTIF_UNDEFINED );
        }
        n->pub.id = new_id( replaces_id );
        N.image_bytes += n->npixels;
        memmove( &N.open[1], &N.open[0], (size_t)N.nopen * sizeof(*N.open) );
        N.open[0] = n;
        N.nopen++;
        awl_vrb_printf( "notifications: %u from '%s'\n", n->pub.id, n->pub.app );
    }
    arm( n );
    changed();
    return n->pub.id;
}

/** Notify(s app_name, u replaces_id, s app_icon, s summary, s body,
 * as actions, a{sv} hints, i expire_timeout) -> u id */
static int method_notify( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    const char *app, *icon, *summary, *body;
    uint32_t replaces_id;
    int32_t expire_timeout;
    int r;
    notif_t* n = calloc( 1, sizeof(*n) );
    if (!n) return -ENOMEM;
    n->pub.urgency = AWL_NOTIF_NORMAL;
    n->pub.value = -1;
    if ((r = sd_bus_message_read( m, "susss", &app, &replaces_id, &icon, &summary, &body )) < 0 ||
        (r = read_actions( m, n )) < 0 || (r = read_hints( m, n )) < 0 ||
        (r = sd_bus_message_read( m, "i", &expire_timeout )) < 0)
        goto fail;
    r = -ENOMEM;
    if (!(n->pub.app = copy( app, MAX_SHORT )) || !(n->pub.app_icon = copy( icon, MAX_PATH )) ||
        !(n->pub.summary = copy( summary, MAX_SUMMARY )) || !(n->pub.body = copy( body, MAX_BODY )) ||
        strings_set( n ) < 0)
        goto fail;
    n->pub.timeout = n->pub.urgency == AWL_NOTIF_CRITICAL ? 0
                   : expire_timeout >= 0 ? expire_timeout
                   : default_timeout( n->pub.urgency );
    n->pub.generation = ++N.generation;
    return sd_bus_reply_method_return( m, "u", post( n, replaces_id ) );
fail:
    notif_free( n );
    return r;
}

/** CloseNotification(u id): reason 3; nothing if it isn't open or is awl's
 * own */
static int method_close( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    uint32_t id;
    int r = sd_bus_message_read( m, "u", &id );
    if (r < 0) return r;
    int i = find( id );
    if (i >= 0 && !(N.open[i]->pub.flags & AWL_NOTIF_AWL)) close_at( i, AWL_NOTIF_CLOSED );
    return sd_bus_reply_method_return( m, NULL );
}

static int method_capabilities( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    sd_bus_message* reply;
    int r = sd_bus_message_new_method_return( m, &reply );
    if (r < 0) return r;
    r = sd_bus_message_append( reply, "as", 4, "body", "body-markup", "actions", "icon-static" );
    if (r >= 0) r = sd_bus_send( NULL, reply, NULL );
    sd_bus_message_unref( reply );
    return r;
}

static int method_information( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    return sd_bus_reply_method_return( m, "ssss", "awl", "awl", VERSION, "1.2" );
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START( 0 ),
    SD_BUS_METHOD( "Notify", "susssasa{sv}i", "u", method_notify, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "CloseNotification", "u", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "GetCapabilities", "", "as", method_capabilities, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "GetServerInformation", "", "ssss", method_information, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_SIGNAL( "NotificationClosed", "uu", 0 ),
    SD_BUS_SIGNAL( "ActionInvoked", "us", 0 ),
    SD_BUS_SIGNAL( "ActivationToken", "us", 0 ),
    SD_BUS_VTABLE_END
};

/* ---- awl's own ---- */

/** s, as copy() does, with what isn't UTF-8 replaced by '?' (D-Bus checks a
 * client's; awl's own may come from anywhere, the config's values) */
static char* copy_utf8( const char* s, size_t max ) {
    char* c = copy( s ? s : "", max );
    if (!c) return NULL;
    for (unsigned char* p = (unsigned char*)c; *p;) {
        int len = *p < 0x80 ? 1 : (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : (*p & 0xf8) == 0xf0 ? 4 : 0;
        int ok = len > 0;
        for (int k = 1; ok && k < len; k++) ok = (p[k] & 0xc0) == 0x80;
        if (ok && len > 1) {
            /* overlong, surrogates, beyond U+10FFFF */
            uint32_t cp = *p & (0x7f >> len);
            for (int k = 1; k < len; k++) cp = cp << 6 | (p[k] & 0x3f);
            ok = cp >= (len == 2 ? 0x80u : len == 3 ? 0x800u : 0x10000u) && cp <= 0x10ffff &&
                 (cp < 0xd800 || cp > 0xdfff);
        }
        if (!ok) {
            *p++ = '?';
            continue;
        }
        p += len;
    }
    return c;
}

uint32_t awl_notifications_send( const awl_notif_send_t* s ) {
    if (!N.loop || !s) return 0;
    notif_t* n = calloc( 1, sizeof(*n) );
    if (!n) return 0;
    n->pub.urgency = s->urgency <= AWL_NOTIF_LOW ? AWL_NOTIF_LOW
                   : s->urgency >= AWL_NOTIF_CRITICAL ? AWL_NOTIF_CRITICAL : AWL_NOTIF_NORMAL;
    n->pub.value = s->value < 0 ? -1 : s->value > 100 ? 100 : s->value;
    n->pub.flags = AWL_NOTIF_AWL;
    n->pub.look = s->look;
    n->pub.timeout = s->timeout >= 0 ? s->timeout : default_timeout( n->pub.urgency );
    if (!(n->pub.app = strdup( "awl" )) || !(n->pub.summary = copy_utf8( s->summary, MAX_SUMMARY )) ||
        !(n->pub.body = copy_utf8( s->body, MAX_BODY )) || !(n->pub.image_path = copy_utf8( s->image, MAX_PATH )) ||
        !(n->tag = copy_utf8( s->tag, MAX_SHORT )) || strings_set( n ) < 0) {
        notif_free( n );
        return 0;
    }
    n->pub.generation = ++N.generation;
    return post( n, 0 );
}

/* ---- the bus ---- */

/** RequestName -> u: 1 the owner, 2 queued behind another server */
static int on_name( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    uint32_t r = 0;
    if (e) {
        awl_err_printf( "notifications: can't have " NAME ": %s\n", e->message ? e->message : e->name );
        awl_bus_fail( N.conn );
    } else if (sd_bus_message_read( m, "u", &r ) >= 0 && r == 2) {
        awl_wrn_printf( "notifications: another server has " NAME ", waiting for it\n" );
    }
    return 0;
}

/** NameAcquired(s): the name is ours, at once or after the queue */
static int on_acquired( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    const char* name;
    if (sd_bus_message_read( m, "s", &name ) >= 0 && !strcmp( name, NAME ))
        awl_log_printf( "notifications: running\n" );
    return 0;
}

/** awl_bus_ops_t.setup */
static int setup( sd_bus* bus, void* data ) {
    (void)data;
    int r;
    N.bus = bus;
    if ((r = sd_bus_add_object_vtable( bus, &N.vtable, PATH, IFACE, vtable, NULL )) < 0 ||
        (r = sd_bus_match_signal( bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameAcquired", on_acquired, NULL )) < 0)
        return r;
    return sd_bus_request_name_async( bus, NULL, NAME, SD_BUS_NAME_QUEUE, on_name, NULL );
}

/** awl_bus_ops_t.dropped: the notifications stay for the next connection */
static void dropped( void* data ) {
    (void)data;
    N.vtable = sd_bus_slot_unref( N.vtable );
    N.bus = NULL;
}

static const awl_bus_ops_t ops = {
    .name = "notifications",
    .description = "awl-notifications",
    .setup = setup,
    .dropped = dropped,
};

int awl_notifications_start( struct wl_event_loop* loop, void (*changed)( void ) ) {
    if (N.loop) return 0;
    N.loop = loop;
    N.changed = changed;
    if (!(N.conn = awl_bus_start( loop, &ops, NULL ))) {
        N.loop = NULL;
        return -1;
    }
    return 0;
}

void awl_notifications_stop( void ) {
    if (!N.loop) return;
    awl_bus_stop( N.conn );
    N.conn = NULL;
    if (N.idle) wl_event_source_remove( N.idle );
    N.idle = NULL;
    for (int i = 0; i < N.nopen; i++) notif_free( N.open[i] );
    N.nopen = 0;
    N.image_bytes = 0;
    N.loop = NULL;
}
