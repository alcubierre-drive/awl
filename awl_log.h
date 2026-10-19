#pragma once

/**
 * awl's logging: every message goes to stderr as ``[awl:level] message`` (the
 * prefix colored if stderr is a terminal) and, uncolored, to the log file
 * (``-l``, by default `AWL_LOGFILE`). Messages above the log level (`awl_log_set_level()`; ``-d``
 * raises it to verbose) are dropped. Any thread may log; each message is one
 * ``write()`` per output.
 *
 * The implementation (awl_log.c) lives in awl. libawlplugins.so (built with
 * ``AWL_HOSTED``) reaches awl only through its host table, so there
 * `awl_dbg_printf()` is a macro for `awl_host_t.log` (awl_host.h), which is
 * awl's `awl_dbg_printf()`. wlroots' messages come here too (awl.c's
 * wlrlog(), as "wlr: ").
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

typedef enum {
    awl_loglevel_err = 0,
    awl_loglevel_wrn = 1,
    awl_loglevel_log = 2,
    awl_loglevel_vrb = 3,
} awl_loglevel_t;

#ifndef AWL_HOSTED
#ifndef AWL_LOGFILE
/** the log file without ``-l``; ``-DAWL_LOGFILE=...`` changes it */
#define AWL_LOGFILE "/tmp/awl.log"
#endif

/** opens the log file path (truncating it; NULL or "": none) and decides on
 * colors; before it, messages go to stderr only */
void awl_log_init( const char* path );
/** discards every message above loglevel */
void awl_log_set_level( awl_loglevel_t loglevel );

/** prints fmt with the prefix of loglevel; fmt ends in its own newline */
void awl_dbg_printf( awl_loglevel_t loglevel, const char* fmt, ... )
    __attribute__(( format( printf, 2, 3 ) ));
/** `awl_dbg_printf()` with a va_list */
void awl_dbg_vprintf( awl_loglevel_t loglevel, const char* fmt, va_list ap );
#else
#include "awl_host.h" /* awl_host */
#define awl_dbg_printf( ... ) awl_host->log( __VA_ARGS__ )
#endif

/** print error message (loglevel 0) */
#define awl_err_printf( ... ) awl_dbg_printf( awl_loglevel_err, __VA_ARGS__ )
/** print warning message (loglevel 1) */
#define awl_wrn_printf( ... ) awl_dbg_printf( awl_loglevel_wrn, __VA_ARGS__ )
/** print log message (loglevel 2) */
#define awl_log_printf( ... ) awl_dbg_printf( awl_loglevel_log, __VA_ARGS__ )
/** print verbose message (loglevel 3, off by default) */
#define awl_vrb_printf( ... ) awl_dbg_printf( awl_loglevel_vrb, __VA_ARGS__ )

#ifdef __cplusplus
}
#endif
