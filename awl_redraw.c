#include "awl_redraw.h"
#include "awl_process.h"
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/eventfd.h>


int awl_redraw_init( awl_process_t* proc ) {
    int fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    atomic_store( &proc->redraw_fd, fd );
    return fd;
}

void awl_redraw_fini( awl_process_t* proc ) {
    int fd = atomic_exchange( &proc->redraw_fd, -1 );
    if (fd >= 0) close( fd );
}

void awl_redraw_request( awl_process_t* proc ) {
    int fd = atomic_load( &proc->redraw_fd );
    uint64_t one = 1;
    /* EAGAIN only happens if the counter would overflow, i.e. a redraw is
     * pending anyway */
    if (fd >= 0 && write( fd, &one, sizeof one ) < 0) {}
}

void awl_redraw_drain( awl_process_t* proc ) {
    int fd = atomic_load( &proc->redraw_fd );
    uint64_t count;
    if (fd >= 0 && read( fd, &count, sizeof count ) < 0) {}
}

int awl_redraw_fd( awl_process_t* proc ) {
    return atomic_load( &proc->redraw_fd );
}
