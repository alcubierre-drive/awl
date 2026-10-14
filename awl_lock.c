#include "awl_lock.h"
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/eventfd.h>

static _Atomic int lock_fd = -1;
static atomic_int requested;
static atomic_uint logind_flags;

int awl_lock_init( void ) {
    int fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    atomic_store( &lock_fd, fd );
    return fd;
}

void awl_lock_fini( void ) {
    int fd = atomic_exchange( &lock_fd, -1 );
    if (fd >= 0) close( fd );
}

static void wake( void ) {
    int fd = atomic_load( &lock_fd );
    uint64_t one = 1;
    /* EAGAIN only happens if the counter would overflow, i.e. the main loop
     * is due to look anyway */
    if (fd >= 0 && write( fd, &one, sizeof one ) < 0) {}
}

void awl_lock_request( void ) {
    atomic_store( &requested, 1 );
    wake();
}

void awl_lock_logind_state( unsigned flags ) {
    atomic_store( &logind_flags, flags );
    wake();
}

int awl_lock_fd( void ) {
    return atomic_load( &lock_fd );
}

int awl_lock_take( unsigned* flags ) {
    int fd = atomic_load( &lock_fd );
    uint64_t count;
    /* drained first: whatever is stored after this wakes the loop again */
    if (fd >= 0 && read( fd, &count, sizeof count ) < 0) {}
    *flags = atomic_load( &logind_flags );
    return atomic_exchange( &requested, 0 );
}
