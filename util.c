/* See LICENSE for copyright and license details. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "awl_log.h"

#include "util.h"

void
die(const char *fmt, ...) {
    va_list ap;
    char msg[1024];
    int err = errno;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (fmt[0] && fmt[strlen(fmt)-1] == ':')
        awl_err_printf("%s %s\n", msg, strerror(err));
    else
        awl_err_printf("%s\n", msg);

    exit(1);
}

void *
ecalloc(size_t nmemb, size_t size)
{
    void *p;

    if (!(p = calloc(nmemb, size)))
        die("calloc:");
    return p;
}

int
fd_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        awl_err_printf("fcntl(F_GETFL): %s\n", strerror(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        awl_err_printf("fcntl(F_SETFL): %s\n", strerror(errno));
        return -1;
    }

    return 0;
}
