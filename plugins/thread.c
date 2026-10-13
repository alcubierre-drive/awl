/* gettid(), pthread_setname_np(), pthread_timedjoin_np() */
#define _GNU_SOURCE
#include "thread.h"
#include "../awl_log.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>

static void* trampoline( void* data ) {
    awl_thread_t* t = data;
    awl_log_printf( "%s thread is tid %d\n", t->name, (int)gettid() );
    return t->fn( t->arg );
}

int awl_thread_start( awl_thread_t* t, const char* name, void* (*fn)( void* ), void* arg ) {
    t->name = name;
    t->fn = fn;
    t->arg = arg;
    atomic_init( &t->quit, 0 );
    t->wake_fd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
    if (t->wake_fd < 0) {
        awl_err_printf( "%s: eventfd: %s\n", name, strerror( errno ) );
        return -1;
    }

    sigset_t all, old;
    sigfillset( &all );
    pthread_sigmask( SIG_SETMASK, &all, &old );
    int err = pthread_create( &t->thread, NULL, trampoline, t );
    pthread_sigmask( SIG_SETMASK, &old, NULL );
    if (err) {
        awl_err_printf( "can't start the %s thread: %s\n", name, strerror( err ) );
        close( t->wake_fd );
        return -1;
    }
    char comm[16];
    snprintf( comm, sizeof(comm), "awl-%s", name );
    pthread_setname_np( t->thread, comm );
    return 0;
}

void awl_thread_wake( awl_thread_t* t ) {
    uint64_t one = 1;
    /* only fails if the counter is full, i.e. the thread is due to wake anyway */
    if (write( t->wake_fd, &one, sizeof one ) < 0) {}
}

int awl_thread_woken( awl_thread_t* t ) {
    uint64_t n;
    if (read( t->wake_fd, &n, sizeof n ) < 0) {}
    return atomic_load( &t->quit );
}

int awl_thread_stop( awl_thread_t* t, int timeout_ms ) {
    atomic_store( &t->quit, 1 );
    awl_thread_wake( t );
    if (timeout_ms < 0) {
        pthread_join( t->thread, NULL );
    } else {
        struct timespec until;
        clock_gettime( CLOCK_REALTIME, &until );
        until.tv_sec += timeout_ms / 1000;
        until.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
        if (until.tv_nsec >= 1000000000) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000;
        }
        if (pthread_timedjoin_np( t->thread, NULL, &until )) {
            pthread_detach( t->thread );
            return -1;
        }
    }
    close( t->wake_fd );
    return 0;
}
