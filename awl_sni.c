#include "awl_sni.h"
#include "awl_log.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <systemd/sd-bus.h>
#include <wayland-server-core.h>

#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_PATH     "/StatusNotifierItem"
#define RETRY_MS 5000
/** registrations beyond this are refused: a misbehaving client */
#define MAX_ITEMS 256

/** an item registered: "bus/path" as listed, and the bus name watched */
typedef struct {
    char* id;
    char* bus;
} item_t;

static struct {
    struct wl_event_loop* loop;
    sd_bus* bus;
    sd_bus_slot *vtable, *owner_match;
    struct wl_event_source *io, *timeout, *retry;
    int failed;
    item_t items[MAX_ITEMS];
    int nitems;
} W;

static void bus_update( void );

static void items_changed( void ) {
    sd_bus_emit_properties_changed( W.bus, WATCHER_PATH, WATCHER_IFACE, "RegisteredStatusNotifierItems", NULL );
}

static int find( const char* id ) {
    for (int i = 0; i < W.nitems; i++)
        if (!strcmp( W.items[i].id, id )) return i;
    return -1;
}

static void item_remove( int i ) {
    awl_vrb_printf( "sni watcher: item %s gone\n", W.items[i].id );
    sd_bus_emit_signal( W.bus, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierItemUnregistered", "s", W.items[i].id );
    free( W.items[i].id );
    free( W.items[i].bus );
    /* the rest keep their order, the registrations': hosts list them so */
    memmove( &W.items[i], &W.items[i + 1], (size_t)(W.nitems - i - 1) * sizeof(*W.items) );
    W.nitems--;
}

static void items_clear( void ) {
    for (int i = 0; i < W.nitems; i++) {
        free( W.items[i].id );
        free( W.items[i].bus );
    }
    W.nitems = 0;
}

/** NameOwnerChanged(s name, s old_owner, s new_owner): an item's name gone */
static int on_owner_changed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read( m, "sss", &name, &old_owner, &new_owner ) < 0 || *new_owner) return 0;
    int removed = 0;
    for (int i = W.nitems - 1; i >= 0; i--)
        if (!strcmp( W.items[i].bus, name )) {
            item_remove( i );
            removed = 1;
        }
    if (removed) items_changed();
    return 0;
}

/** GetNameOwner of a name just registered: it may have gone before the
 * match saw it */
static int on_name_owner( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    char* id = userdata;
    int i = sd_bus_message_is_method_error( m, NULL ) ? find( id ) : -1;
    if (i >= 0) {
        item_remove( i );
        items_changed();
    }
    free( id );
    return 0;
}

/** RegisterStatusNotifierItem(s service): a bus name, or an object path on
 * the caller's */
static int method_register_item( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata;
    const char* service;
    int r = sd_bus_message_read( m, "s", &service );
    if (r < 0) return r;
    const char* bus = service;
    const char* path = ITEM_PATH;
    if (*service == '/') {
        bus = sd_bus_message_get_sender( m );
        path = service;
    }
    if (!bus || !sd_bus_service_name_is_valid( bus ) || !sd_bus_object_path_is_valid( path ))
        return sd_bus_error_setf( ret_error, SD_BUS_ERROR_INVALID_ARGS, "not a valid item: '%s'", service );
    size_t len = strlen( bus ) + strlen( path ) + 1;
    char* id = malloc( len );
    if (!id) return -ENOMEM;
    snprintf( id, len, "%s%s", bus, path );
    if (find( id ) >= 0) {
        free( id );
        return sd_bus_reply_method_return( m, NULL );
    }
    if (W.nitems == MAX_ITEMS) {
        free( id );
        return sd_bus_error_setf( ret_error, SD_BUS_ERROR_LIMITS_EXCEEDED, "too many items" );
    }
    char* watched = strdup( bus );
    char* check = strdup( id );
    if (!watched || !check) {
        free( id );
        free( watched );
        free( check );
        return -ENOMEM;
    }
    W.items[W.nitems++] = (item_t){ id, watched };
    awl_vrb_printf( "sni watcher: item %s registered\n", id );
    if (sd_bus_call_method_async( W.bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "GetNameOwner", on_name_owner, check, "s", bus ) < 0)
        free( check );
    sd_bus_emit_signal( W.bus, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierItemRegistered", "s", id );
    items_changed();
    return sd_bus_reply_method_return( m, NULL );
}

/** RegisterStatusNotifierHost(s): accepted, but awl is the host anyway */
static int method_register_host( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    return sd_bus_reply_method_return( m, NULL );
}

static int prop_items( sd_bus* bus, const char* path, const char* iface, const char* prop,
                       sd_bus_message* reply, void* userdata, sd_bus_error* ret_error ) {
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    int r = sd_bus_message_open_container( reply, 'a', "s" );
    for (int i = 0; r >= 0 && i < W.nitems; i++)
        r = sd_bus_message_append( reply, "s", W.items[i].id );
    return r < 0 ? r : sd_bus_message_close_container( reply );
}

static int prop_true( sd_bus* bus, const char* path, const char* iface, const char* prop,
                      sd_bus_message* reply, void* userdata, sd_bus_error* ret_error ) {
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    return sd_bus_message_append( reply, "b", 1 );
}

static int prop_version( sd_bus* bus, const char* path, const char* iface, const char* prop,
                         sd_bus_message* reply, void* userdata, sd_bus_error* ret_error ) {
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    return sd_bus_message_append( reply, "i", 0 );
}

static const sd_bus_vtable watcher_vtable[] = {
    SD_BUS_VTABLE_START( 0 ),
    SD_BUS_METHOD( "RegisterStatusNotifierItem", "s", "", method_register_item, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "RegisterStatusNotifierHost", "s", "", method_register_host, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_PROPERTY( "RegisteredStatusNotifierItems", "as", prop_items, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE ),
    SD_BUS_PROPERTY( "IsStatusNotifierHostRegistered", "b", prop_true, 0, SD_BUS_VTABLE_PROPERTY_CONST ),
    SD_BUS_PROPERTY( "ProtocolVersion", "i", prop_version, 0, SD_BUS_VTABLE_PROPERTY_CONST ),
    SD_BUS_SIGNAL( "StatusNotifierItemRegistered", "s", 0 ),
    SD_BUS_SIGNAL( "StatusNotifierItemUnregistered", "s", 0 ),
    SD_BUS_SIGNAL( "StatusNotifierHostRegistered", "", 0 ),
    SD_BUS_SIGNAL( "StatusNotifierHostUnregistered", "", 0 ),
    SD_BUS_VTABLE_END
};

/** RequestName -> u: 1 the owner, 2 queued behind another watcher */
static int on_name( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    uint32_t r = 0;
    if (e) {
        awl_err_printf( "sni watcher: can't have " WATCHER_NAME ": %s\n", e->message ? e->message : e->name );
        W.failed = 1;
    } else if (sd_bus_message_read( m, "u", &r ) >= 0 && r == 2) {
        awl_wrn_printf( "sni watcher: another watcher has " WATCHER_NAME ", waiting for it\n" );
    }
    return 0;
}

/** NameAcquired(s): the name is ours, at once or after the queue */
static int on_acquired( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    const char* name;
    if (sd_bus_message_read( m, "s", &name ) < 0 || strcmp( name, WATCHER_NAME )) return 0;
    awl_log_printf( "sni watcher: running\n" );
    sd_bus_emit_signal( W.bus, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierHostRegistered", "" );
    return 0;
}

static int on_subscribed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    if (sd_bus_message_is_method_error( m, NULL )) W.failed = 1;
    return 0;
}

static void drop_bus( void ) {
    if (W.io) wl_event_source_remove( W.io );
    if (W.timeout) wl_event_source_remove( W.timeout );
    W.io = W.timeout = NULL;
    W.vtable = sd_bus_slot_unref( W.vtable );
    W.owner_match = sd_bus_slot_unref( W.owner_match );
    /* not sd_bus_flush_close_unref(): that blocks until the queue is out */
    if (W.bus) sd_bus_close( W.bus );
    W.bus = sd_bus_unref( W.bus );
    W.failed = 0;
    /* the items register again with the next watcher */
    items_clear();
}

static int bus_io( int fd, uint32_t mask, void* data ) {
    (void)fd; (void)mask; (void)data;
    bus_update();
    return 0;
}

static int bus_timeout( void* data ) {
    (void)data;
    bus_update();
    return 0;
}

static void connect_bus( void );

static int retry_fire( void* data ) {
    (void)data;
    connect_bus();
    return 0;
}

static void retry( int ms ) {
    if (!W.retry) W.retry = wl_event_loop_add_timer( W.loop, retry_fire, NULL );
    /* 0 would disarm it */
    if (W.retry) wl_event_source_timer_update( W.retry, ms > 0 ? ms : 1 );
}

static void connect_bus( void ) {
    int r;
    if ((r = sd_bus_open_user_with_description( &W.bus, "awl-sni-watcher" )) < 0 ||
        (r = sd_bus_add_object_vtable( W.bus, &W.vtable, WATCHER_PATH, WATCHER_IFACE, watcher_vtable, NULL )) < 0 ||
        (r = sd_bus_add_match_async( W.bus, &W.owner_match,
                "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged'",
                on_owner_changed, on_subscribed, NULL )) < 0 ||
        (r = sd_bus_match_signal( W.bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameAcquired", on_acquired, NULL )) < 0 ||
        (r = sd_bus_request_name_async( W.bus, NULL, WATCHER_NAME, SD_BUS_NAME_QUEUE, on_name, NULL )) < 0 ||
        !(W.io = wl_event_loop_add_fd( W.loop, sd_bus_get_fd( W.bus ), WL_EVENT_READABLE, bus_io, NULL ))) {
        awl_wrn_printf( "sni watcher: no session bus (%s), retrying\n", strerror( r < 0 ? -r : ENOMEM ) );
        drop_bus();
        retry( RETRY_MS );
        return;
    }
    bus_update();
}

/** Handles whatever the bus has, then waits for what it wants next. */
static void bus_update( void ) {
    int r;
    while ((r = sd_bus_process( W.bus, NULL )) > 0)
        ;
    if (r < 0 || W.failed) {
        awl_wrn_printf( "sni watcher: lost the session bus, reconnecting\n" );
        drop_bus();
        retry( RETRY_MS );
        return;
    }
    int events = sd_bus_get_events( W.bus );
    wl_event_source_fd_update( W.io, (events & POLLIN ? WL_EVENT_READABLE : 0) |
                                     (events & POLLOUT ? WL_EVENT_WRITABLE : 0) );
    uint64_t until;
    if (sd_bus_get_timeout( W.bus, &until ) < 0 || until == UINT64_MAX) {
        if (W.timeout) wl_event_source_timer_update( W.timeout, 0 );
        return;
    }
    /* until is on CLOCK_MONOTONIC */
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    int ms = until > now ? (int)((until - now + 999) / 1000) : 1;
    if (!W.timeout) W.timeout = wl_event_loop_add_timer( W.loop, bus_timeout, NULL );
    if (W.timeout) wl_event_source_timer_update( W.timeout, ms > 0 ? ms : 1 );
}

int awl_sni_watcher_start( struct wl_event_loop* loop ) {
    if (W.loop) return 0;
    W.loop = loop;
    /* the first connection from the loop: no socket work during setup */
    retry( 0 );
    if (!W.retry) {
        awl_err_printf( "sni watcher: can't start\n" );
        W.loop = NULL;
        return -1;
    }
    return 0;
}

void awl_sni_watcher_stop( void ) {
    if (!W.loop) return;
    drop_bus();
    if (W.retry) wl_event_source_remove( W.retry );
    W.retry = NULL;
    W.loop = NULL;
}
