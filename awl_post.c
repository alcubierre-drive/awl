#include "awl_post.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>

typedef struct post_t {
    void (*fn)( void* data );
    void* data;
    struct post_t* next;
} post_t;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
/** guarded by mtx: the fd, the queue (oldest first) */
static int post_fd = -1;
static post_t *head, **tail = &head;

int awl_post_init( void ) {
    int fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    pthread_mutex_lock( &mtx );
    post_fd = fd;
    pthread_mutex_unlock( &mtx );
    return fd;
}

void awl_post_fini( void ) {
    pthread_mutex_lock( &mtx );
    int fd = post_fd;
    post_t* p = head;
    post_fd = -1;
    head = NULL;
    tail = &head;
    pthread_mutex_unlock( &mtx );
    while (p) {
        post_t* next = p->next;
        free( p );
        p = next;
    }
    if (fd >= 0) close( fd );
}

void awl_post( void (*fn)( void* data ), void* data ) {
    post_t* p = malloc( sizeof(*p) );
    if (!p) return;
    *p = (post_t){ fn, data, NULL };
    uint64_t one = 1;
    pthread_mutex_lock( &mtx );
    if (post_fd < 0) {
        pthread_mutex_unlock( &mtx );
        free( p );
        return;
    }
    *tail = p;
    tail = &p->next;
    /* EAGAIN only if the counter would overflow, i.e. a wakeup is pending */
    if (write( post_fd, &one, sizeof one ) < 0) {}
    pthread_mutex_unlock( &mtx );
}

void awl_post_run( void ) {
    uint64_t count;
    pthread_mutex_lock( &mtx );
    post_t* p = head;
    head = NULL;
    tail = &head;
    if (post_fd >= 0 && read( post_fd, &count, sizeof count ) < 0) {}
    pthread_mutex_unlock( &mtx );
    while (p) {
        post_t* next = p->next;
        p->fn( p->data );
        free( p );
        p = next;
    }
}

int awl_post_fd( void ) {
    pthread_mutex_lock( &mtx );
    int fd = post_fd;
    pthread_mutex_unlock( &mtx );
    return fd;
}
