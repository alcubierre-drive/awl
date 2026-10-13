#include "persistent.h"
#include "../plugins.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** a larger file isn't ours: it's left alone rather than read */
#define MAX_SIZE (64 * 1024)

/** one read-modify-write at a time */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int awl_persistent_path( char* out, size_t n ) {
    awl_dict_val_t home, rel;
    int r = -1;
    /* the strings are the dictionary's until it's unlocked */
    AWL_HOST_CFG_LOCK( awl_host );
    if (AWL_HOST_CFG_GET( awl_host, "state_home", &home ) == AWL_DICT_OK
            && home.kind == AWL_DICT_KIND_STR
            && AWL_HOST_CFG_GET( awl_host, AWL_PERSISTENT_KEY, &rel ) == AWL_DICT_OK
            && rel.kind == AWL_DICT_KIND_STR) {
        int len = snprintf( out, n, "%s/%s", home.str, rel.str );
        r = len >= 0 && (size_t)len < n ? 0 : -1;
    }
    AWL_HOST_CFG_UNLOCK( awl_host );
    return r;
}

/** the whole file in buf, NUL-terminated; its length, or -1 */
static ssize_t readall( const char* path, char* buf, size_t n ) {
    int fd = open( path, O_RDONLY | O_CLOEXEC );
    if (fd < 0) return -1;
    size_t len = 0;
    ssize_t r;
    while (len < n - 1 && (r = read( fd, buf + len, n - 1 - len )) != 0) {
        if (r < 0) {
            if (errno == EINTR) continue;
            close( fd );
            return -1;
        }
        len += (size_t)r;
    }
    /* still more: too large */
    char extra;
    r = len == n - 1 ? read( fd, &extra, 1 ) : 0;
    close( fd );
    if (r) {
        errno = EFBIG;
        return -1;
    }
    buf[len] = 0;
    return (ssize_t)len;
}

/** the line's value if it's key's, else NULL */
static const char* value( const char* line, const char* key ) {
    size_t n = strlen( key );
    return !strncmp( line, key, n ) && line[n] == '=' ? line + n + 1 : NULL;
}

int awl_persistent_get_str( const char* key, char* out, size_t n ) {
    char path[PATH_MAX];
    static char buf[MAX_SIZE + 1];
    int r = -1;
    if (awl_persistent_path( path, sizeof(path) )) return -1;

    pthread_mutex_lock( &lock );
    if (readall( path, buf, sizeof(buf) ) >= 0) {
        for (char* line = buf; line && *line; ) {
            char* next = strchr( line, '\n' );
            if (next) *next++ = 0;
            const char* val = value( line, key );
            if (val) {
                int len = snprintf( out, n, "%s", val );
                r = len >= 0 && (size_t)len < n ? 0 : -1;
                break;
            }
            line = next;
        }
    }
    pthread_mutex_unlock( &lock );
    return r;
}

int awl_persistent_get_num( const char* key, int64_t* v ) {
    char val[32], *end;
    if (awl_persistent_get_str( key, val, sizeof(val) )) return -1;
    errno = 0;
    long long num = strtoll( val, &end, 10 );
    if (end == val || *end || errno) return -1;
    *v = num;
    return 0;
}

/** creates path's directories, as far as they don't exist */
static void mkparents( char* path ) {
    for (char* p = strchr( path + 1, '/' ); p; p = strchr( p + 1, '/' )) {
        *p = 0;
        mkdir( path, 0755 ); /* existing ones fail, a real failure shows at open() */
        *p = '/';
    }
}

int awl_persistent_set_str( const char* key, const char* v ) {
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    static char buf[MAX_SIZE + 1];
    if (strchr( v, '\n' )) {
        awl_wrn_printf( "persistent: %s's value has a newline, not writing it\n", key );
        return -1;
    }
    if (awl_persistent_path( path, sizeof(path) )) {
        awl_err_printf( "persistent: state_home or " AWL_PERSISTENT_KEY " unset\n" );
        return -1;
    }
    snprintf( tmp, sizeof(tmp), "%s.tmp", path );

    pthread_mutex_lock( &lock );
    ssize_t len = readall( path, buf, sizeof(buf) );
    if (len < 0 && errno != ENOENT) {
        /* unreadable or too large: rather keep it than lose its other lines */
        awl_err_printf( "persistent: can't read %s, not writing it\n", path );
        pthread_mutex_unlock( &lock );
        return -1;
    }
    if (len < 0) {
        buf[0] = 0;
        mkparents( path );
    }

    FILE* f = NULL;
    int fd = open( tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 );
    if (fd >= 0 && !(f = fdopen( fd, "w" ))) close( fd );
    if (!f) goto fail;
    /* the other keys' lines as they were, key's own last */
    for (char* line = buf; *line; ) {
        char* next = strchr( line, '\n' );
        if (next) *next++ = 0;
        if (*line && !value( line, key )) fprintf( f, "%s\n", line );
        if (!next) break;
        line = next;
    }
    fprintf( f, "%s=%s\n", key, v );
    /* on disk before the rename, or a crash could leave an empty file */
    if (fflush( f ) || fsync( fileno( f ) )) {
        fclose( f );
        goto fail;
    }
    if (fclose( f ) || rename( tmp, path )) goto fail;
    pthread_mutex_unlock( &lock );
    return 0;

fail:
    awl_err_printf( "persistent: can't write %s: %s\n", path, strerror( errno ) );
    unlink( tmp );
    pthread_mutex_unlock( &lock );
    return -1;
}

int awl_persistent_set_num( const char* key, int64_t v ) {
    char val[32];
    snprintf( val, sizeof(val), "%" PRId64, v );
    return awl_persistent_set_str( key, val );
}

/** whether key is a word of the space-separated list; dictionary locked */
static int listed( const char* key ) {
    awl_dict_val_t v;
    if (AWL_HOST_CFG_GET( awl_host, AWL_PERSISTENT_LOADED_KEY, &v ) != AWL_DICT_OK
            || v.kind != AWL_DICT_KIND_STR) return 0;
    size_t n = strlen( key );
    for (const char* p = v.str; (p = strstr( p, key )); p += n)
        if ((p == v.str || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
    return 0;
}

int awl_persistent_load_str( const char* key ) {
    char val[256];
    AWL_HOST_CFG_LOCK( awl_host );
    int done = listed( key );
    AWL_HOST_CFG_UNLOCK( awl_host );
    if (done) return -1;

    /* the file's read outside the lock, the main thread takes it too */
    int r = awl_persistent_get_str( key, val, sizeof(val) );
    AWL_HOST_CFG_LOCK( awl_host );
    if (!listed( key )) { /* another thread may have been quicker */
        awl_dict_val_t v;
        char list[1024] = "";
        if (AWL_HOST_CFG_GET( awl_host, AWL_PERSISTENT_LOADED_KEY, &v ) == AWL_DICT_OK
                && v.kind == AWL_DICT_KIND_STR)
            snprintf( list, sizeof(list), "%s ", v.str );
        size_t len = strlen( list );
        snprintf( list + len, sizeof(list) - len, "%s", key );
        AWL_HOST_CFG_SET( awl_host, AWL_PERSISTENT_LOADED_KEY, AWL_DICT_STR( list ) );
        if (!r) AWL_HOST_CFG_SET( awl_host, key, AWL_DICT_STR( val ) );
    } else {
        r = -1;
    }
    AWL_HOST_CFG_UNLOCK( awl_host );
    return r;
}

int awl_persistent_save_str( const char* key ) {
    char val[256], saved[256];
    awl_dict_val_t v;
    int r = -1;
    /* the string is the dictionary's until it's unlocked */
    AWL_HOST_CFG_LOCK( awl_host );
    if (AWL_HOST_CFG_GET( awl_host, key, &v ) == AWL_DICT_OK && v.kind == AWL_DICT_KIND_STR) {
        int len = snprintf( val, sizeof(val), "%s", v.str );
        r = len >= 0 && (size_t)len < sizeof(val) ? 0 : -1;
    }
    AWL_HOST_CFG_UNLOCK( awl_host );
    if (r) return -1;
    if (!awl_persistent_get_str( key, saved, sizeof(saved) ) && !strcmp( saved, val ))
        return 0;
    return awl_persistent_set_str( key, val );
}
