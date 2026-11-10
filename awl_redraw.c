#include "awl_redraw.h"
#include "awl_process.h"
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/eventfd.h>


int awl_redraw_init( void ) {
    int fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    atomic_store( &awl_proc.redraw_fd, fd );
    return fd;
}

void awl_redraw_fini( void ) {
    int fd = atomic_exchange( &awl_proc.redraw_fd, -1 );
    if (fd >= 0) close( fd );
}

void awl_redraw_request( void ) {
    int fd = atomic_load( &awl_proc.redraw_fd );
    uint64_t one = 1;
    /* EAGAIN only happens if the counter would overflow, i.e. a redraw is
     * pending anyway */
    if (fd >= 0 && write( fd, &one, sizeof one ) < 0) {}
}

void awl_redraw_drain( void ) {
    int fd = atomic_load( &awl_proc.redraw_fd );
    uint64_t count;
    if (fd >= 0 && read( fd, &count, sizeof count ) < 0) {}
}

int awl_redraw_fd( void ) {
    return atomic_load( &awl_proc.redraw_fd );
}
