#include "nm.h"
#include "pa_bus.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

/** NetworkManager's D-Bus API, read-only. Its PropertiesChanged signals for
 * the interfaces below restart the query chain (main object -> primary
 * active connection -> its IPv4 config and device), so a change to any of
 * them is seen; others (access point strength, ...) are not even received.
 * Every call is asynchronous, like plugins/backlight.c, so a slow bus never
 * holds up the other plugins on the loop. */
#define NM_NAME   "org.freedesktop.NetworkManager"
#define NM_PATH   "/org/freedesktop/NetworkManager"
#define NM_IFACE  "org.freedesktop.NetworkManager"
#define NM_ACTIVE NM_IFACE ".Connection.Active"
#define NM_IP4    NM_IFACE ".IP4Config"
#define NM_DEVICE NM_IFACE ".Device"
#define RETRY_USEC (60 * 1000000ull)

struct awl_nm_t {
    void (*changed)( void* );
    void* userdata;

    pa_bus_t* pb;
    /** pb's connection, NULL while there is none */
    sd_bus* bus;
    sd_bus_slot* props_match[3];
    /** the query in flight, at most one; unref'ing it drops its reply */
    sd_bus_slot* pending;

    /** the chain's progress */
    char device[256];
    awl_nm_state_t next, state;
};

static void query( awl_nm_t* s );

static void publish( awl_nm_t* s, const awl_nm_state_t* st ) {
    if (!memcmp( st, &s->state, sizeof *st )) return;
    s->state = *st;
    if (s->changed) s->changed( s->userdata );
}

static void set_gone( awl_nm_t* s ) {
    s->pending = sd_bus_slot_unref( s->pending );
    publish( s, &(awl_nm_state_t){0} );
}

/** the chain ends here: next is complete */
static void finish( awl_nm_t* s ) {
    s->next.present = 1;
    publish( s, &s->next );
}

static int reply_ok( sd_bus_message* m, awl_nm_t* s ) {
    const sd_bus_error* e = sd_bus_message_get_error( m );
    if (!e) return 1;
    /* gone in between: its NameOwnerChanged follows; an object that went
     * away mid-chain: a PropertiesChanged that restarts it follows */
    if (!sd_bus_error_has_name( e, SD_BUS_ERROR_SERVICE_UNKNOWN )
            && !sd_bus_error_has_name( e, SD_BUS_ERROR_UNKNOWN_OBJECT )
            && !sd_bus_error_has_name( e, SD_BUS_ERROR_UNKNOWN_METHOD ))
        pa_bus_fail( s->pb );
    return 0;
}

static void get( awl_nm_t* s, const char* path, const char* iface, const char* prop, sd_bus_message_handler_t cb ) {
    s->pending = sd_bus_slot_unref( s->pending );
    if (sd_bus_call_method_async( s->bus, &s->pending, NM_NAME, path, "org.freedesktop.DBus.Properties",
                                  prop ? "Get" : "GetAll", cb, s, prop ? "ss" : "s", iface, prop ) < 0)
        pa_bus_fail( s->pb );
}

/* 4. the device's IpInterface (s): where the address lives */
static int on_device( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_nm_t* s = userdata;
    s->pending = sd_bus_slot_unref( s->pending );
    const char* ifname = NULL;
    if (reply_ok( m, s ) && sd_bus_message_read( m, "v", "s", &ifname ) >= 0 && ifname)
        snprintf( s->next.ifname, sizeof s->next.ifname, "%s", ifname );
    finish( s );
    return 0;
}

/* 3. the IPv4 config's AddressData (aa{sv}): the first "address" */
static int on_ip4( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_nm_t* s = userdata;
    s->pending = sd_bus_slot_unref( s->pending );
    if (!reply_ok( m, s )) return 0;
    const char* address = NULL;
    if (sd_bus_message_enter_container( m, 'v', "aa{sv}" ) > 0 &&
        sd_bus_message_enter_container( m, 'a', "a{sv}" ) > 0 &&
        sd_bus_message_enter_container( m, 'a', "{sv}" ) > 0) {
        while (!address && sd_bus_message_enter_container( m, 'e', "sv" ) > 0) {
            const char* key = NULL;
            if (sd_bus_message_read( m, "s", &key ) < 0) break;
            int r = !strcmp( key, "address" ) ? sd_bus_message_read( m, "v", "s", &address )
                                              : sd_bus_message_skip( m, "v" );
            if (r < 0 || sd_bus_message_exit_container( m ) < 0) break;
        }
    }
    struct in_addr a;
    if (address && inet_pton( AF_INET, address, &a ) == 1) {
        s->next.has_addr = 1;
        s->next.addr = a.s_addr;
    }
    if (s->next.has_addr && *s->device)
        get( s, s->device, NM_DEVICE, "IpInterface", on_device );
    else
        finish( s );
    return 0;
}

/* 2. the primary active connection: Ip4Config (o), Devices (ao) */
static int on_active( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_nm_t* s = userdata;
    s->pending = sd_bus_slot_unref( s->pending );
    if (!reply_ok( m, s )) return 0;
    char ip4[256] = "";
    if (sd_bus_message_enter_container( m, 'a', "{sv}" ) > 0) {
        while (sd_bus_message_enter_container( m, 'e', "sv" ) > 0) {
            const char* key = NULL;
            int r = sd_bus_message_read( m, "s", &key );
            if (r >= 0 && !strcmp( key, "Ip4Config" )) {
                const char* o = NULL;
                if ((r = sd_bus_message_read( m, "v", "o", &o )) >= 0)
                    snprintf( ip4, sizeof ip4, "%s", o );
            } else if (r >= 0 && !strcmp( key, "Devices" )) {
                char** devs = NULL;
                if ((r = sd_bus_message_enter_container( m, 'v', "ao" )) > 0 &&
                    (r = sd_bus_message_read_strv( m, &devs )) >= 0) {
                    if (devs && devs[0]) snprintf( s->device, sizeof s->device, "%s", devs[0] );
                    r = sd_bus_message_exit_container( m );
                }
                if (devs) for (char** d = devs; *d; ++d) free( *d );
                free( devs );
            } else if (r >= 0) {
                r = sd_bus_message_skip( m, "v" );
            }
            if (r < 0 || sd_bus_message_exit_container( m ) < 0) break;
        }
    }
    if (*ip4 && strcmp( ip4, "/" ))
        get( s, ip4, NM_IP4, "AddressData", on_ip4 );
    else
        finish( s );
    return 0;
}

/* 1. the main object: Connectivity (u), PrimaryConnection (o) */
static int on_main( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)ret_error;
    awl_nm_t* s = userdata;
    s->pending = sd_bus_slot_unref( s->pending );
    if (!reply_ok( m, s )) return 0;
    char primary[256] = "";
    if (sd_bus_message_enter_container( m, 'a', "{sv}" ) > 0) {
        while (sd_bus_message_enter_container( m, 'e', "sv" ) > 0) {
            const char* key = NULL;
            int r = sd_bus_message_read( m, "s", &key );
            if (r >= 0 && !strcmp( key, "Connectivity" )) {
                uint32_t c = 0;
                if ((r = sd_bus_message_read( m, "v", "u", &c )) >= 0)
                    s->next.connectivity = c <= AWL_NM_CONNECTIVITY_FULL ? (int)c : 0;
            } else if (r >= 0 && !strcmp( key, "PrimaryConnection" )) {
                const char* o = NULL;
                if ((r = sd_bus_message_read( m, "v", "o", &o )) >= 0)
                    snprintf( primary, sizeof primary, "%s", o );
            } else if (r >= 0) {
                r = sd_bus_message_skip( m, "v" );
            }
            if (r < 0 || sd_bus_message_exit_container( m ) < 0) break;
        }
    }
    if (*primary && strcmp( primary, "/" ))
        get( s, primary, NM_ACTIVE, NULL, on_active );
    else
        finish( s );
    return 0;
}

/** (re)starts the chain; one already running is dropped */
static void query( awl_nm_t* s ) {
    memset( &s->next, 0, sizeof s->next );
    *s->device = 0;
    get( s, NM_PATH, NM_IFACE, NULL, on_main );
}

static int on_properties_changed( sd_bus_message* m, void* userdata, sd_bus_error* ret_error ) {
    (void)m; (void)ret_error;
    query( userdata );
    return 0;
}

static int add_props_match( awl_nm_t* s, sd_bus_slot** slot, const char* iface ) {
    char rule[512];
    snprintf( rule, sizeof rule, "type='signal',sender='" NM_NAME "',"
              "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='%s'", iface );
    return sd_bus_add_match_async( s->bus, slot, rule, on_properties_changed, NULL, s );
}

static int setup( sd_bus* bus, void* data ) {
    awl_nm_t* s = data;
    s->bus = bus;
    return add_props_match( s, &s->props_match[0], NM_IFACE ) < 0 ||
           add_props_match( s, &s->props_match[1], NM_ACTIVE ) < 0 ||
           add_props_match( s, &s->props_match[2], NM_IP4 ) < 0 ? -1 : 0;
}

static void appeared( void* s ) { query( s ); }
static void gone( void* s ) { set_gone( s ); }

/** with the connection (e.g. the system bus restarted) goes what it told */
static void dropped( void* data ) {
    awl_nm_t* s = data;
    for (int i=0; i<3; ++i)
        s->props_match[i] = sd_bus_slot_unref( s->props_match[i] );
    s->bus = NULL;
    set_gone( s );
}

static const pa_bus_ops_t ops = { .name = NM_NAME, .system = true, .retry_usec = RETRY_USEC,
    .setup = setup, .appeared = appeared, .gone = gone, .dropped = dropped };

awl_nm_t* nm_new( pa_mainloop_api* api, void (*changed)( void* userdata ), void* userdata ) {
    awl_nm_t* s = calloc( 1, sizeof *s );
    if (!s) return NULL;
    s->changed = changed;
    s->userdata = userdata;
    if ((s->pb = pa_bus_start( api, &ops, s ))) return s;
    free( s );
    return NULL;
}

const awl_nm_state_t* nm_state( const awl_nm_t* s ) {
    return &s->state;
}

void nm_free( awl_nm_t* s ) {
    if (!s) return;
    s->changed = NULL; /* the bus going says so, to no one */
    pa_bus_stop( s->pb );
    free( s );
}
