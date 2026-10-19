#include "awl_log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* the file starts over once it grows past this */
#define AWL_LOGFILE_MAX (1024*128)

static awl_loglevel_t loglevel = awl_loglevel_log;
static int colors = 0;
static int logfd = -1;
static size_t logsize = 0;
static pthread_mutex_t logmtx = PTHREAD_MUTEX_INITIALIZER;

static const struct { const char *name, *color; } levels[] = {
    [awl_loglevel_err] = { "error",   "\033[31;1m" },
    [awl_loglevel_wrn] = { "warn",    "\033[33;1m" },
    [awl_loglevel_log] = { "log",     "\033[34;1m" },
    [awl_loglevel_vrb] = { "verbose", "\033[38;5;248;1m" },
};

void awl_log_init( const char* path ) {
    colors = isatty( STDERR_FILENO );
    if (!path || !*path) return;
    /* O_APPEND: after the truncation below writes start over at 0;
     * O_CLOEXEC: spawned programs don't inherit it */
    logfd = open( path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644 );
    if (logfd < 0)
        awl_wrn_printf( "can't open %s (%s), logging to stderr only\n", path, strerror( errno ) );
}

void awl_log_set_level( awl_loglevel_t l ) {
    loglevel = l;
}

awl_loglevel_t awl_log_level( void ) {
    return loglevel;
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
    if (l > loglevel) return;
    if (l < awl_loglevel_err) l = awl_loglevel_err;
    if (l > awl_loglevel_vrb) l = awl_loglevel_vrb;

    char msg[1024];
    int ret = vsnprintf( msg, sizeof(msg), fmt, ap );
    if (ret < 0) return;
    /* vsnprintf returns what it would have written, not what it did */
    size_t len = (size_t)ret < sizeof(msg)-1 ? (size_t)ret : sizeof(msg)-1;
    if (len == sizeof(msg)-1) msg[len-1] = '\n';

    emit( STDERR_FILENO, l, colors, msg, len );
    if (logfd < 0) return;
    pthread_mutex_lock( &logmtx );
    /* the prefix is at most 20 bytes, close enough for the size cap */
    logsize += len + 20;
    if (logsize >= AWL_LOGFILE_MAX && !ftruncate( logfd, 0 )) logsize = len + 20;
    emit( logfd, l, 0, msg, len );
    pthread_mutex_unlock( &logmtx );
}

void awl_dbg_printf( awl_loglevel_t l, const char* fmt, ... ) {
    va_list ap;
    va_start( ap, fmt );
    awl_dbg_vprintf( l, fmt, ap );
    va_end( ap );
}
