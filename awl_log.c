#include "awl_log.h"
#include "awl_process.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* the file starts over once it grows past this */
#define AWL_LOGFILE_MAX (1024*128)


static const struct { const char *name, *color; } levels[] = {
    [awl_loglevel_err] = { "error",   "\033[31;1m" },
    [awl_loglevel_wrn] = { "warn",    "\033[33;1m" },
    [awl_loglevel_log] = { "log",     "\033[34;1m" },
    [awl_loglevel_vrb] = { "verbose", "\033[38;5;248;1m" },
};

void awl_log_init( const char* path ) {
    awl_proc.log.colors = isatty( STDERR_FILENO );
    if (!path || !*path) return;
    /* O_APPEND: after the truncation below writes start over at 0;
     * O_CLOEXEC: spawned programs don't inherit it */
    awl_proc.log.fd = open( path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644 );
    if (awl_proc.log.fd < 0)
        awl_wrn_printf( "can't open %s (%s), logging to stderr only\n", path, strerror( errno ) );
}

void awl_log_set_level( awl_loglevel_t l ) {
    awl_proc.log.level = l;
}

awl_loglevel_t awl_log_level( void ) {
    return awl_proc.log.level;
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
    if (l > awl_proc.log.level) return;
    if (l < awl_loglevel_err) l = awl_loglevel_err;
    if (l > awl_loglevel_vrb) l = awl_loglevel_vrb;

    char msg[1024];
    int ret = vsnprintf( msg, sizeof(msg), fmt, ap );
    if (ret < 0) return;
    /* vsnprintf returns what it would have written, not what it did */
    size_t len = (size_t)ret < sizeof(msg)-1 ? (size_t)ret : sizeof(msg)-1;
    if (len == sizeof(msg)-1) msg[len-1] = '\n';

    emit( STDERR_FILENO, l, awl_proc.log.colors, msg, len );
    if (awl_proc.log.fd < 0) return;
    pthread_mutex_lock( &awl_proc.log.mtx );
    /* the prefix is at most 20 bytes, close enough for the size cap */
    awl_proc.log.size += len + 20;
    if (awl_proc.log.size >= AWL_LOGFILE_MAX && !ftruncate( awl_proc.log.fd, 0 )) awl_proc.log.size = len + 20;
    emit( awl_proc.log.fd, l, 0, msg, len );
    pthread_mutex_unlock( &awl_proc.log.mtx );
}

void awl_dbg_printf( awl_loglevel_t l, const char* fmt, ... ) {
    va_list ap;
    va_start( ap, fmt );
    awl_dbg_vprintf( l, fmt, ap );
    va_end( ap );
}
