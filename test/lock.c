/* lock: a minimal screen locker (ext-session-lock-v1), for test/lock.sh: it
 * locks, gives every output a lock surface with a plain shm buffer, prints
 * "locked" once awl confirms the lock and every surface has its first
 * buffer, and then holds the lock until awl goes away or it gets SIGTERM.
 * It never unlocks.
 *
 *   live.sh run test/lock
 *
 * Exit status: 0 once it locked and awl went away, 1 if the lock was refused
 * (finished) or never came, 2 on a setup error. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-session-lock-v1-client-protocol.h"

#define MAX_OUTPUTS 16

typedef struct {
    struct wl_output* output;
    struct wl_surface* surface;
    struct ext_session_lock_surface_v1* lock_surface;
    int drawn;
} Output;

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct ext_session_lock_manager_v1* manager;
static Output outputs[MAX_OUTPUTS];
static int noutputs, locked, finished, reported;

static struct wl_buffer* buffer( int w, int h ) {
    int stride = w * 4, size = stride * h;
    int fd = memfd_create( "lock", MFD_CLOEXEC );
    if (fd < 0 || ftruncate( fd, size ) < 0) return NULL;
    uint32_t* px = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (px == MAP_FAILED) return NULL;
    for (int i = 0; i < w * h; i++) px[i] = 0xff203040;
    munmap( px, size );
    struct wl_shm_pool* pool = wl_shm_create_pool( shm, fd, size );
    struct wl_buffer* b = wl_shm_pool_create_buffer( pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888 );
    wl_shm_pool_destroy( pool );
    close( fd );
    return b;
}

static void report( void ) {
    if (reported || !locked) return;
    for (int i = 0; i < noutputs; i++)
        if (!outputs[i].drawn) return;
    reported = 1;
    printf( "locked\n" );
    fflush( stdout );
}

static void surface_configure( void* data, struct ext_session_lock_surface_v1* ls, uint32_t serial,
                               uint32_t w, uint32_t h ) {
    Output* o = data;
    struct wl_buffer* b = buffer( (int)w, (int)h );
    ext_session_lock_surface_v1_ack_configure( ls, serial );
    if (!b) exit( 2 );
    wl_surface_attach( o->surface, b, 0, 0 );
    wl_surface_damage_buffer( o->surface, 0, 0, (int)w, (int)h );
    wl_surface_commit( o->surface );
    o->drawn = 1;
    report();
}

static const struct ext_session_lock_surface_v1_listener surface_listener = {
    .configure = surface_configure,
};

static void lock_locked( void* data, struct ext_session_lock_v1* lock ) {
    locked = 1;
    report();
}

static void lock_finished( void* data, struct ext_session_lock_v1* lock ) {
    finished = 1;
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = lock_locked,
    .finished = lock_finished,
};

static void global( void* data, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t version ) {
    if (!strcmp( iface, wl_compositor_interface.name ))
        compositor = wl_registry_bind( reg, name, &wl_compositor_interface, 4 );
    else if (!strcmp( iface, wl_shm_interface.name ))
        shm = wl_registry_bind( reg, name, &wl_shm_interface, 1 );
    else if (!strcmp( iface, ext_session_lock_manager_v1_interface.name ))
        manager = wl_registry_bind( reg, name, &ext_session_lock_manager_v1_interface, 1 );
    else if (!strcmp( iface, wl_output_interface.name ) && noutputs < MAX_OUTPUTS)
        outputs[noutputs++].output = wl_registry_bind( reg, name, &wl_output_interface, 1 );
}

static void global_remove( void* data, struct wl_registry* reg, uint32_t name ) {
}

static const struct wl_registry_listener registry_listener = {
    .global = global,
    .global_remove = global_remove,
};

int main( void ) {
    struct wl_display* dpy = wl_display_connect( NULL );
    if (!dpy) {
        fprintf( stderr, "lock: no display\n" );
        return 2;
    }
    wl_registry_add_listener( wl_display_get_registry( dpy ), &registry_listener, NULL );
    wl_display_roundtrip( dpy );
    if (!compositor || !shm || !manager || !noutputs) {
        fprintf( stderr, "lock: missing globals\n" );
        return 2;
    }
    struct ext_session_lock_v1* lock = ext_session_lock_manager_v1_lock( manager );
    ext_session_lock_v1_add_listener( lock, &lock_listener, NULL );
    for (int i = 0; i < noutputs; i++) {
        Output* o = &outputs[i];
        o->surface = wl_compositor_create_surface( compositor );
        o->lock_surface = ext_session_lock_v1_get_lock_surface( lock, o->surface, o->output );
        ext_session_lock_surface_v1_add_listener( o->lock_surface, &surface_listener, o );
    }
    /* until awl goes away (or refuses the lock) */
    while (!finished && wl_display_dispatch( dpy ) >= 0)
        ;
    if (finished) fprintf( stderr, "lock: refused\n" );
    return reported && !finished ? 0 : 1;
}
