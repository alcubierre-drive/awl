/* vptr x y [button...]: moves the pointer of the compositor at $WAYLAND_DISPLAY
 * to layout pixel x,y and clicks each button (left, middle, right or a code)
 * there, through wlr-virtual-pointer. Instead of a button, wheel:N scrolls N
 * wheel notches down (negative: up), hwheel:N right, finger:D a touchpad's
 * continuous D down (no notches). Absolute motion maps onto the whole output
 * layout, so pass its size as VPTR_EXTENT=WxH (default 1280x720). */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1* mgr;
static struct wl_seat* seat;

static void global( void* data, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver ) {
    if (!strcmp( iface, zwlr_virtual_pointer_manager_v1_interface.name ))
        mgr = wl_registry_bind( r, name, &zwlr_virtual_pointer_manager_v1_interface, 1 );
    else if (!strcmp( iface, wl_seat_interface.name ) && !seat)
        seat = wl_registry_bind( r, name, &wl_seat_interface, 1 );
}
static void global_remove( void* data, struct wl_registry* r, uint32_t name ) {}
static const struct wl_registry_listener reg = { global, global_remove };

/* libinput's degrees per wheel notch, what wlroots passes on as the delta */
#define NOTCH 15.0

/** a wheel:N, hwheel:N or finger:D argument; returns whether it was one */
static int scroll( struct zwlr_virtual_pointer_v1* p, uint32_t* t, const char* arg ) {
    double v;
    int n;
    if (sscanf( arg, "wheel:%d", &n ) == 1 || sscanf( arg, "hwheel:%d", &n ) == 1) {
        uint32_t axis = arg[0] == 'h' ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL;
        zwlr_virtual_pointer_v1_axis_source( p, WL_POINTER_AXIS_SOURCE_WHEEL );
        zwlr_virtual_pointer_v1_axis_discrete( p, (*t)++, axis, wl_fixed_from_double( n * NOTCH ), n );
        zwlr_virtual_pointer_v1_frame( p );
        return 1;
    }
    if (sscanf( arg, "finger:%lf", &v ) == 1) {
        zwlr_virtual_pointer_v1_axis_source( p, WL_POINTER_AXIS_SOURCE_FINGER );
        zwlr_virtual_pointer_v1_axis( p, (*t)++, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double( v ) );
        zwlr_virtual_pointer_v1_frame( p );
        zwlr_virtual_pointer_v1_axis_source( p, WL_POINTER_AXIS_SOURCE_FINGER );
        zwlr_virtual_pointer_v1_axis_stop( p, (*t)++, WL_POINTER_AXIS_VERTICAL_SCROLL );
        zwlr_virtual_pointer_v1_frame( p );
        return 1;
    }
    return 0;
}

int main( int argc, char** argv ) {
    if (argc < 3) { fprintf( stderr, "usage: vptr x y [left|middle|right|code|wheel:N|hwheel:N|finger:D...]\n" ); return 2; }
    unsigned ew = 1280, eh = 720;
    const char* e = getenv( "VPTR_EXTENT" );
    if (e && sscanf( e, "%ux%u", &ew, &eh ) != 2) { fprintf( stderr, "bad VPTR_EXTENT\n" ); return 2; }
    struct wl_display* d = wl_display_connect( NULL );
    if (!d) { fprintf( stderr, "no wayland display\n" ); return 1; }
    wl_registry_add_listener( wl_display_get_registry( d ), &reg, NULL );
    wl_display_roundtrip( d );
    if (!mgr) { fprintf( stderr, "no zwlr_virtual_pointer_manager_v1\n" ); return 1; }
    struct zwlr_virtual_pointer_v1* p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer( mgr, seat );
    uint32_t t = 1;
    zwlr_virtual_pointer_v1_motion_absolute( p, t++, (uint32_t)atoi( argv[1] ), (uint32_t)atoi( argv[2] ), ew, eh );
    zwlr_virtual_pointer_v1_frame( p );
    for (int i = 3; i < argc; i++) {
        if (scroll( p, &t, argv[i] )) continue;
        uint32_t b = !strcmp( argv[i], "left" ) ? BTN_LEFT : !strcmp( argv[i], "middle" ) ? BTN_MIDDLE
                   : !strcmp( argv[i], "right" ) ? BTN_RIGHT : (uint32_t)strtoul( argv[i], NULL, 0 );
        zwlr_virtual_pointer_v1_button( p, t++, b, WL_POINTER_BUTTON_STATE_PRESSED );
        zwlr_virtual_pointer_v1_frame( p );
        zwlr_virtual_pointer_v1_button( p, t++, b, WL_POINTER_BUTTON_STATE_RELEASED );
        zwlr_virtual_pointer_v1_frame( p );
    }
    wl_display_roundtrip( d );
    zwlr_virtual_pointer_v1_destroy( p );
    wl_display_roundtrip( d );
    wl_display_disconnect( d );
    return 0;
}
