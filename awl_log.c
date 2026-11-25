#include "awl_log.h"
#include "awl_process.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* the file starts over once it grows past this */
#define AWL_LOG_FILE_MAX (1024*128)


static const struct { const char *name, *color; } levels[] = {
    [awl_loglevel_err] = { "error",   "\033[31;1m" },
    [awl_loglevel_wrn] = { "warn",    "\033[33;1m" },
    [awl_loglevel_log] = { "log",     "\033[34;1m" },
    [awl_loglevel_vrb] = { "verbose", "\033[38;5;248;1m" },
};

/** The process whose log the printf functions write: the core's only
 * global (awl_process.h). They are called from anywhere, any thread, with
 * nothing to pass; NULL before :c:func:`awl_log_init`, when they write to
 * stderr at the default level. Set before any thread starts, never reset:
 * the process outlives every caller. */
static awl_process_t* logged;

void awl_log_init( awl_process_t* proc, const char* path ) {
    logged = proc;
    proc->log.colors = isatty( STDERR_FILENO );
    if (!path || !*path) return;
    /* O_APPEND: after the truncation below writes start over at 0;
     * O_CLOEXEC: spawned programs don't inherit it */
    proc->log.fd = open( path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644 );
    if (proc->log.fd < 0)
        awl_wrn_printf( "can't open %s (%s), logging to stderr only\n", path, strerror( errno ) );
}

void awl_log_set_level( awl_process_t* proc, awl_loglevel_t l ) {
    proc->log.level = l;
}

awl_loglevel_t awl_log_level( const awl_process_t* proc ) {
    return proc->log.level;
}

/* one write() of "<color>[awl:level]<reset> message" */
static void emit( int fd, awl_loglevel_t l, int color, const char* msg, size_t mlen ) {
    char line[1152];
    int n = snprintf( line, sizeof(line), "%s[awl:%s]%s %.*s", color ? levels[l].color : "",
            levels[l].name, color ? "\033[0m" : "", (int)mlen, msg );
    if (n < 0) return;
    (void)!write( fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1 );
}

void awl_dbg_vprintf( awl_loglevel_t l, const char* fmt, va_list ap ) {
    awl_process_t* proc = logged;
    if (l > (proc ? proc->log.level : awl_loglevel_log)) return;
    if (l < awl_loglevel_err) l = awl_loglevel_err;
    if (l > awl_loglevel_vrb) l = awl_loglevel_vrb;

    char msg[1024];
    int ret = vsnprintf( msg, sizeof(msg), fmt, ap );
    if (ret < 0) return;
    /* vsnprintf returns what it would have written, not what it did */
    size_t len = (size_t)ret < sizeof(msg)-1 ? (size_t)ret : sizeof(msg)-1;
    if (len == sizeof(msg)-1) msg[len-1] = '\n';

    emit( STDERR_FILENO, l, proc && proc->log.colors, msg, len );
    if (!proc || proc->log.fd < 0) return;
    pthread_mutex_lock( &proc->log.mtx );
    /* the prefix is at most 20 bytes, close enough for the size cap */
    proc->log.size += len + 20;
    if (proc->log.size >= AWL_LOG_FILE_MAX && !ftruncate( proc->log.fd, 0 )) proc->log.size = len + 20;
    emit( proc->log.fd, l, 0, msg, len );
    pthread_mutex_unlock( &proc->log.mtx );
}

void awl_dbg_printf( awl_loglevel_t l, const char* fmt, ... ) {
    va_list ap;
    va_start( ap, fmt );
    awl_dbg_vprintf( l, fmt, ap );
    va_end( ap );
}
