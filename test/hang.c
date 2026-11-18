/* hang: a window that stops answering, for rendermon()'s wait for a resize
 * (RESIZE_WAIT_MS, awl_core.h). An xdg toplevel filled with one colour,
 * answering every configure with a buffer of its size, until SIGUSR1 (or
 * SECONDS after it was first drawn): then it hangs, alive but reading
 * nothing from awl anymore, so a resize is never answered. Not stopped:
 * awl doesn't wait for a stopped client (client_is_stopped()), nor for one
 * that isn't its child, so start it from awl:
 *
 *   live.sh start -s 'exec build-dev/hang'
 *
 * Prints "drawn WxH" for each buffer it commits and "hung" when it hangs.
 *
 *   hang [RRGGBB [SECONDS]]
 *
 * Exit status: 0 if awl closed the window or went away, 2 on a setup error. */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct wl_surface* surface;
static uint32_t color = 0xff3060c0;
static int width = 640, height = 480, closed;

/* the hang: SIGUSR1's or SIGALRM's handler, which never returns */
static void hang( int sig ) {
    (void)sig;
    static const char msg[] = "hung\n";
    if (write( STDOUT_FILENO, msg, sizeof(msg) - 1 ) < 0) {}
    for (;;) pause();
}

/* a w by h buffer of color, on an unlinked POSIX shm object */
static struct wl_buffer* buffer( int w, int h ) {
    char name[64];
    int stride = w * 4, size = stride * h;
    snprintf( name, sizeof(name), "/awl-hang-%ld", (long)getpid() );
    int fd = shm_open( name, O_RDWR | O_CREAT | O_EXCL, 0600 );
    if (fd < 0) return NULL;
    shm_unlink( name );
    if (ftruncate( fd, size ) < 0) { close( fd ); return NULL; }
    uint32_t* px = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (px == MAP_FAILED) { close( fd ); return NULL; }
    for (int i = 0; i < w * h; i++) px[i] = color;
    munmap( px, size );
    struct wl_shm_pool* pool = wl_shm_create_pool( shm, fd, size );
    struct wl_buffer* b = wl_shm_pool_create_buffer( pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888 );
    wl_shm_pool_destroy( pool );
    close( fd );
    return b;
}

static void buffer_release( void* data, struct wl_buffer* b ) {
    (void)data;
    wl_buffer_destroy( b );
}
static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void wm_ping( void* data, struct xdg_wm_base* b, uint32_t serial ) {
    (void)data;
    xdg_wm_base_pong( b, serial );
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void surface_configure( void* data, struct xdg_surface* s, uint32_t serial ) {
    unsigned* seconds = data;
    xdg_surface_ack_configure( s, serial );
    struct wl_buffer* b = buffer( width, height );
    if (!b) { fprintf( stderr, "hang: no buffer\n" ); exit( 2 ); }
    wl_buffer_add_listener( b, &buffer_listener, NULL );
    wl_surface_attach( surface, b, 0, 0 );
    wl_surface_damage_buffer( surface, 0, 0, width, height );
    wl_surface_commit( surface );
    printf( "drawn %dx%d\n", width, height );
    fflush( stdout );
    if (*seconds) {
        alarm( *seconds );
        *seconds = 0;
    }
}
static const struct xdg_surface_listener surface_listener = { surface_configure };

static void toplevel_configure( void* data, struct xdg_toplevel* t, int32_t w, int32_t h,
        struct wl_array* states ) {
    (void)data; (void)t; (void)states;
    if (w > 0) width = w;
    if (h > 0) height = h;
}
static void toplevel_close( void* data, struct xdg_toplevel* t ) {
    (void)data; (void)t;
    closed = 1;
}
/* xdg_wm_base version 1: no configure_bounds or wm_capabilities */
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close };

static void global( void* data, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version ) {
    (void)data; (void)version;
    if (!strcmp( iface, wl_compositor_interface.name ))
        compositor = wl_registry_bind( r, name, &wl_compositor_interface, 4 );
    else if (!strcmp( iface, wl_shm_interface.name ))
        shm = wl_registry_bind( r, name, &wl_shm_interface, 1 );
    else if (!strcmp( iface, xdg_wm_base_interface.name ))
        wm_base = wl_registry_bind( r, name, &xdg_wm_base_interface, 1 );
}
static void global_remove( void* data, struct wl_registry* r, uint32_t name ) {
    (void)data; (void)r; (void)name;
}
static const struct wl_registry_listener registry_listener = { global, global_remove };

int main( int argc, char** argv ) {
    static unsigned seconds;
    if (argc > 1) color = 0xff000000 | (uint32_t)strtoul( argv[1], NULL, 16 );
    if (argc > 2) seconds = (unsigned)strtoul( argv[2], NULL, 10 );

    struct sigaction sa = { .sa_handler = hang };
    sigemptyset( &sa.sa_mask );
    sigaction( SIGUSR1, &sa, NULL );
    sigaction( SIGALRM, &sa, NULL );

    struct wl_display* dpy = wl_display_connect( NULL );
    if (!dpy) { fprintf( stderr, "hang: no Wayland display\n" ); return 2; }
    struct wl_registry* reg = wl_display_get_registry( dpy );
    wl_registry_add_listener( reg, &registry_listener, NULL );
    wl_display_roundtrip( dpy );
    if (!compositor || !shm || !wm_base) { fprintf( stderr, "hang: missing globals\n" ); return 2; }
    xdg_wm_base_add_listener( wm_base, &wm_listener, NULL );

    surface = wl_compositor_create_surface( compositor );
    struct xdg_surface* xs = xdg_wm_base_get_xdg_surface( wm_base, surface );
    xdg_surface_add_listener( xs, &surface_listener, &seconds );
    struct xdg_toplevel* top = xdg_surface_get_toplevel( xs );
    xdg_toplevel_add_listener( top, &toplevel_listener, NULL );
    xdg_toplevel_set_app_id( top, "awl-hang" );
    xdg_toplevel_set_title( top, "hang" );
    wl_surface_commit( surface );

    while (!closed && wl_display_dispatch( dpy ) != -1) {}
    return 0;
}
