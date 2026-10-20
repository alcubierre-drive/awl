#include "loop.h"
#include "thread.h"
#include <stdlib.h>
#include <sys/epoll.h>

struct awl_loop_t {
    sd_event* event;
    awl_thread_t thread;
    int started;
    /* loop thread only (once started) */
    sd_event_source* wake;
};

static int wake_callback( sd_event_source* e, int fd, uint32_t revents, void* userdata ) {
    (void)fd; (void)revents;
    awl_loop_t* l = userdata;
    if (awl_thread_woken( &l->thread ))
        sd_event_exit( sd_event_source_get_event( e ), 0 );
    return 0;
}

static void* loop_run( void* arg ) {
    awl_loop_t* l = arg;
    if (sd_event_add_io( l->event, &l->wake, l->thread.wake_fd, EPOLLIN, wake_callback, l ) < 0)
        return NULL;
    sd_event_loop( l->event );
    return NULL;
}

awl_loop_t* awl_loop_new( void ) {
    awl_loop_t* l = calloc( 1, sizeof(*l) );
    if (!l) return NULL;
    if (sd_event_new( &l->event ) < 0) {
        free( l );
        return NULL;
    }
    return l;
}

sd_event* awl_loop_event( awl_loop_t* l ) {
    return l->event;
}

int awl_loop_start( awl_loop_t* l, const char* name ) {
    if (awl_thread_start( &l->thread, name, loop_run, l )) return -1;
    l->started = 1;
    return 0;
}

int awl_loop_stop( awl_loop_t* l, int timeout_ms ) {
    if (!l || !l->started) return 0;
    if (awl_thread_stop( &l->thread, timeout_ms )) return 1;
    l->started = 0;
    return 0;
}

void awl_loop_free( awl_loop_t* l ) {
    if (!l) return;
    sd_event_source_disable_unref( l->wake );
    sd_event_unref( l->event );
    free( l );
}
