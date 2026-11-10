#include "awl_process.h"

awl_process_t awl_proc = {
    .log = {
        .level = awl_loglevel_log,
        .fd = -1,
        .mtx = PTHREAD_MUTEX_INITIALIZER,
    },
    .spawn.startup = -1,
    .redraw_fd = -1,
    .lock.fd = -1,
    .plugins = {
        .handle_fd = -1,
        .timers = { &awl_proc.plugins.timers, &awl_proc.plugins.timers },
    },
    .tray.env.fds = { -1, -1 },
};
