#include "awl_screensaver.h"
#include "awl_bus.h"
#include "awl_config.h"
#include "awl_input.h"
#include "awl_log.h"
#include "awl_util.h"
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <wayland-server-core.h>

#define NAME  "org.freedesktop.ScreenSaver"
#define IFACE "org.freedesktop.ScreenSaver"

/** an Inhibit's cookie, until UnInhibit or its caller leaves the bus */
typedef struct cookie_t {
    struct wl_list link;
    awl_screensaver_t* S;
    uint32_t id;
    /** the caller, on the bus */
    sd_bus_track* track;
    char* app;
} cookie_t;

struct awl_screensaver_t {
    awl_server_t* srv;
    awl_bus_t* conn;
    /** the object at /org/freedesktop/ScreenSaver and at /ScreenSaver */
    sd_bus_slot* vtable[2];
    /** :c:type:`cookie_t`, the newest first */
    struct wl_list cookies;
    uint32_t last_id;
};

/** drops c, logged with why; idle is checked again */
static void release( cookie_t* c, const char* why ) {
    awl_screensaver_t* S = c->S;
    awl_log_printf( "screensaver: %s no longer inhibits idle (%s)\n", c->app, why );
    wl_list_remove( &c->link );
    sd_bus_track_unref( c->track );
    free( c->app );
    free( c );
    /* not while stopping: then the server is going, or checks itself */
    if (S->srv->screensaver == S) checkidleinhibitor( S->srv, NULL );
}

/** the caller of a cookie left the bus */
static int on_gone( sd_bus_track* track, void* userdata ) {
    (void)track;
    release( userdata, "gone from the bus" );
    return 0;
}

/** Inhibit(ss) -> u: application name, reason; the cookie */
static int method_inhibit( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    awl_screensaver_t* S = userdata;
    const char *app, *reason;
    cookie_t* c;
    int r;
    (void)ret_error;
    if ((r = sd_bus_message_read( m, "ss", &app, &reason )) < 0) return r;
    c = ecalloc( 1, sizeof(*c) );
    if ((r = sd_bus_track_new( sd_bus_message_get_bus( m ), &c->track, on_gone, c )) < 0 ||
        (r = sd_bus_track_add_sender( c->track, m )) < 0) {
        sd_bus_track_unref( c->track );
        free( c );
        return r;
    }
    c->S = S;
    if (!++S->last_id) ++S->last_id; /* 0 isn't a cookie */
    c->id = S->last_id;
    c->app = strdup( app );
    wl_list_insert( &S->cookies, &c->link );
    awl_log_printf( "screensaver: %s inhibits idle (%s)\n", app, reason );
    checkidleinhibitor( S->srv, NULL );
    return sd_bus_reply_method_return( m, "u", c->id );
}

/** UnInhibit(u): the cookie; an unknown one is ignored */
static int method_uninhibit( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    awl_screensaver_t* S = userdata;
    cookie_t* c;
    uint32_t id;
    int r;
    (void)ret_error;
    if ((r = sd_bus_message_read( m, "u", &id )) < 0) return r;
    wl_list_for_each( c, &S->cookies, link ) {
        if (c->id == id) {
            release( c, "UnInhibit" );
            break;
        }
    }
    return sd_bus_reply_method_return( m, "" );
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START( 0 ),
    SD_BUS_METHOD( "Inhibit", "ss", "u", method_inhibit, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "UnInhibit", "u", "", method_uninhibit, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_VTABLE_END
};

/** RequestName -> u: 1 the owner, 2 queued behind another screensaver */
static int on_name( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    awl_screensaver_t* S = userdata;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    uint32_t r = 0;
    (void)ret_error;
    if (e) {
        awl_err_printf( "screensaver: can't have " NAME ": %s\n", e->message ? e->message : e->name );
        awl_bus_fail( S->conn );
    } else if (sd_bus_message_read( m, "u", &r ) >= 0 && r == 2) {
        awl_wrn_printf( "screensaver: another one has " NAME ", waiting for it\n" );
    }
    return 0;
}

/** NameAcquired(s): the name is ours, at once or after the queue */
static int on_acquired( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    const char* name;
    (void)userdata; (void)ret_error;
    if (sd_bus_message_read( m, "s", &name ) >= 0 && !strcmp( name, NAME ))
        awl_log_printf( "screensaver: running\n" );
    return 0;
}

/** awl_bus_ops_t.setup */
static int setup( sd_bus* bus, void* data ) {
    awl_screensaver_t* S = data;
    int r;
    if ((r = sd_bus_add_object_vtable( bus, &S->vtable[0], "/org/freedesktop/ScreenSaver", IFACE, vtable, S )) < 0 ||
        (r = sd_bus_add_object_vtable( bus, &S->vtable[1], "/ScreenSaver", IFACE, vtable, S )) < 0 ||
        (r = sd_bus_match_signal( bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameAcquired", on_acquired, NULL )) < 0)
        return r;
    return sd_bus_request_name_async( bus, NULL, NAME, SD_BUS_NAME_QUEUE, on_name, S );
}

/** awl_bus_ops_t.dropped: the cookies go with the connection */
static void dropped( void* data ) {
    awl_screensaver_t* S = data;
    cookie_t *c, *tmp;
    wl_list_for_each_safe( c, tmp, &S->cookies, link )
        release( c, "the bus connection is gone" );
    for (int i = 0; i < 2; i++)
        S->vtable[i] = sd_bus_slot_unref( S->vtable[i] );
}

static const awl_bus_ops_t ops = {
    .name = "screensaver",
    .description = "awl-screensaver",
    .setup = setup,
    .dropped = dropped,
};

void awl_screensaver_configure( awl_server_t* srv ) {
    awl_screensaver_t* S;
    if (!srv->cfg->screensaver_inhibit) {
        if (srv->screensaver) {
            awl_screensaver_stop( srv );
            checkidleinhibitor( srv, NULL );
        }
        return;
    }
    if (srv->screensaver) return;
    S = ecalloc( 1, sizeof(*S) );
    S->srv = srv;
    wl_list_init( &S->cookies );
    if (!(S->conn = awl_bus_start( srv->event_loop, &ops, S ))) {
        free( S );
        return;
    }
    srv->screensaver = S;
}

void awl_screensaver_stop( awl_server_t* srv ) {
    awl_screensaver_t* S = srv->screensaver;
    if (!S) return;
    srv->screensaver = NULL;
    awl_bus_stop( S->conn );
    free( S );
}

bool awl_screensaver_inhibited( const awl_screensaver_t* S ) {
    return S && !wl_list_empty( &S->cookies );
}
