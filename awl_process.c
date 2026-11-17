#include "awl_process.h"

#include <string.h>

void awl_process_init(awl_process_t *proc)
{
    memset(proc, 0, sizeof *proc);
    proc->log.level = awl_loglevel_log;
    proc->log.fd = -1;
    pthread_mutex_init(&proc->log.mtx, NULL);
    proc->spawn.startup = -1;
    proc->redraw_fd = -1;
    proc->lock.fd = -1;
    proc->lockhelper.fd = -1;
    proc->plugins.handle_fd = -1;
    wl_list_init(&proc->plugins.timers);
    proc->tray.env.fds[0] = proc->tray.env.fds[1] = -1;
}
