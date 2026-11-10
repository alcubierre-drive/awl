#include "awl_sni.h"
#include "awl_bus.h"
#include "awl_log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_PATH     "/StatusNotifierItem"
/** registrations beyond this are refused: a misbehaving client */
#define MAX_ITEMS 256

/** an item registered: "bus/path" as listed, and the bus name watched */
typedef struct {
    char* id;
    char* bus;
} item_t;

struct awl_sni_watcher {
    awl_bus_t* conn;
    sd_bus* bus;
    sd_bus_slot *vtable, *owner_match;
    item_t items[MAX_ITEMS];
    int nitems;
};

/** on_name_owner()'s: the watcher, and the item it checks */
typedef struct {
    awl_sni_watcher_t* w;
    char id[];
} check_t;

static void items_changed( awl_sni_watcher_t* w ) {
    sd_bus_emit_properties_changed( w->bus, WATCHER_PATH, WATCHER_IFACE, "RegisteredStatusNotifierItems", NULL );
}

static int find( awl_sni_watcher_t* w, const char* id ) {
    for (int i = 0; i < w->nitems; i++)
        if (!strcmp( w->items[i].id, id )) return i;
    return -1;
}

static void item_remove( awl_sni_watcher_t* w, int i ) {
    awl_vrb_printf( "sni watcher: item %s gone\n", w->items[i].id );
    sd_bus_emit_signal( w->bus, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierItemUnregistered", "s", w->items[i].id );
    free( w->items[i].id );
    free( w->items[i].bus );
    /* the rest keep their order, the registrations': hosts list them so */
    memmove( &w->items[i], &w->items[i + 1], (size_t)(w->nitems - i - 1) * sizeof(*w->items) );
    w->nitems--;
}

static void items_clear( awl_sni_watcher_t* w ) {
    for (int i = 0; i < w->nitems; i++) {
        free( w->items[i].id );
        free( w->items[i].bus );
    }
    w->nitems = 0;
}

/** NameOwnerChanged(s name, s old_owner, s new_owner): an item's name gone */
static int on_owner_changed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    awl_sni_watcher_t* w = userdata;
    (void)ret_error;
    const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read( m, "sss", &name, &old_owner, &new_owner ) < 0 || *new_owner) return 0;
    int removed = 0;
    for (int i = w->nitems - 1; i >= 0; i--)
        if (!strcmp( w->items[i].bus, name )) {
            item_remove( w, i );
            removed = 1;
        }
    if (removed) items_changed( w );
    return 0;
}

/** GetNameOwner of a name just registered: it may have gone before the
 * match saw it */
static int on_name_owner( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    check_t* c = userdata;
    awl_sni_watcher_t* w = c->w;
    int i = sd_bus_message_is_method_error( m, NULL ) ? find( w, c->id ) : -1;
    if (i >= 0) {
        item_remove( w, i );
        items_changed( w );
    }
    free( c );
    return 0;
}

/** RegisterStatusNotifierItem(s service): a bus name, or an object path on
 * the caller's */
static int method_register_item( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    awl_sni_watcher_t* w = userdata;
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
    if (find( w, id ) >= 0) {
        free( id );
        return sd_bus_reply_method_return( m, NULL );
    }
    if (w->nitems == MAX_ITEMS) {
        free( id );
        return sd_bus_error_setf( ret_error, SD_BUS_ERROR_LIMITS_EXCEEDED, "too many items" );
    }
    char* watched = strdup( bus );
    check_t* check = malloc( sizeof(*check) + len );
    if (check) {
        check->w = w;
        memcpy( check->id, id, len );
    }
    if (!watched || !check) {
        free( id );
        free( watched );
        free( check );
        return -ENOMEM;
    }
    w->items[w->nitems++] = (item_t){ id, watched };
    awl_vrb_printf( "sni watcher: item %s registered\n", id );
    if (sd_bus_call_method_async( w->bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "GetNameOwner", on_name_owner, check, "s", bus ) < 0)
        free( check );
    sd_bus_emit_signal( w->bus, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierItemRegistered", "s", id );
    items_changed( w );
    return sd_bus_reply_method_return( m, NULL );
}

/** RegisterStatusNotifierHost(s): accepted, but awl is the host anyway */
static int method_register_host( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)userdata; (void)ret_error;
    return sd_bus_reply_method_return( m, NULL );
}

static int prop_items( sd_bus* bus, const char* path, const char* iface, const char* prop,
                       sd_bus_message* reply, void* userdata, sd_bus_error* ret_error ) {
    awl_sni_watcher_t* w = userdata;
    (void)bus; (void)path; (void)iface; (void)prop; (void)ret_error;
    int r = sd_bus_message_open_container( reply, 'a', "s" );
    for (int i = 0; r >= 0 && i < w->nitems; i++)
        r = sd_bus_message_append( reply, "s", w->items[i].id );
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
    awl_sni_watcher_t* w = userdata;
    (void)ret_error;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    uint32_t r = 0;
    if (e) {
        awl_err_printf( "sni watcher: can't have " WATCHER_NAME ": %s\n", e->message ? e->message : e->name );
        awl_bus_fail( w->conn );
    } else if (sd_bus_message_read( m, "u", &r ) >= 0 && r == 2) {
        awl_wrn_printf( "sni watcher: another watcher has " WATCHER_NAME ", waiting for it\n" );
    }
    return 0;
}

/** NameAcquired(s): the name is ours, at once or after the queue */
static int on_acquired( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    awl_sni_watcher_t* w = userdata;
    (void)ret_error;
    const char* name;
    if (sd_bus_message_read( m, "s", &name ) < 0 || strcmp( name, WATCHER_NAME )) return 0;
    awl_log_printf( "sni watcher: running\n" );
    sd_bus_emit_signal( w->bus, WATCHER_PATH, WATCHER_IFACE, "StatusNotifierHostRegistered", "" );
    return 0;
}

static int on_subscribed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    awl_sni_watcher_t* w = userdata;
    (void)ret_error;
    if (sd_bus_message_is_method_error( m, NULL )) awl_bus_fail( w->conn );
    return 0;
}

/** awl_bus_ops_t.dropped */
static void dropped( void* data ) {
    awl_sni_watcher_t* w = data;
    w->vtable = sd_bus_slot_unref( w->vtable );
    w->owner_match = sd_bus_slot_unref( w->owner_match );
    w->bus = NULL;
    /* the items register again with the next watcher */
    items_clear( w );
}

/** awl_bus_ops_t.setup */
static int setup( sd_bus* bus, void* data ) {
    awl_sni_watcher_t* w = data;
    int r;
    w->bus = bus;
    if ((r = sd_bus_add_object_vtable( bus, &w->vtable, WATCHER_PATH, WATCHER_IFACE, watcher_vtable, w )) < 0 ||
        (r = sd_bus_add_match_async( bus, &w->owner_match,
                "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged'",
                on_owner_changed, on_subscribed, w )) < 0 ||
        (r = sd_bus_match_signal( bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameAcquired", on_acquired, w )) < 0)
        return r;
    return sd_bus_request_name_async( bus, NULL, WATCHER_NAME, SD_BUS_NAME_QUEUE, on_name, w );
}

static const awl_bus_ops_t ops = {
    .name = "sni watcher",
    .description = "awl-sni-watcher",
    .setup = setup,
    .dropped = dropped,
};

awl_sni_watcher_t* awl_sni_watcher_start( struct wl_event_loop* loop ) {
    awl_sni_watcher_t* w = calloc( 1, sizeof(*w) );
    if (!w) {
        awl_err_printf( "sni watcher: out of memory\n" );
        return NULL;
    }
    if (!(w->conn = awl_bus_start( loop, &ops, w ))) {
        free( w );
        return NULL;
    }
    return w;
}

void awl_sni_watcher_stop( awl_sni_watcher_t* w ) {
    if (!w) return;
    awl_bus_stop( w->conn );
    items_clear( w );
    free( w );
}
