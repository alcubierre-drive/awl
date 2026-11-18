#include "awl_lockhelper.h"
#include "awl_lockscreen.h" /* AWL_LOCKER */

#ifndef AWL_AWLOCK
/* awlock isn't built in (meson option lock): no helper, and awl never asks
 * one, it looks for a program by that name instead (awl_lockscreen.c) */

void lockhelperstart(awl_process_t *proc) {}
bool lockhelperrun(awl_process_t *proc, const awlock_config_t *cfg, int fd) {return false;}
void lockhelperexited(awl_process_t *proc, pid_t pid) {}

#else
#include "awl_log.h"
#include "awl_process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

/** the fd in msg's SCM_RIGHTS, -1 if none; any more are closed */
static int
recvfd(struct msghdr *msg)
{
    int fd = -1;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(msg); h; h = CMSG_NXTHDR(msg, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS)
            continue;
        size_t n = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < n; i++) {
            int got;
            memcpy(&got, CMSG_DATA(h) + i * sizeof(int), sizeof got);
            if (fd < 0)
                fd = got;
            else
                close(got);
        }
    }
    return fd;
}

/** a locker: awlock with cfg on fd; never returns */
static _Noreturn void
locker(const awlock_config_t *cfg, int fd)
{
    /* no core dump, no ptrace by the user's other programs: it holds the
     * password, and its password child (a fork) inherits this */
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0))
        awl_wrn_printf("lock helper: awlock stays dumpable: %s\n", strerror(errno));
    /* the helper ignores it to reap its lockers; awlock waits for its PAM
     * child */
    signal(SIGCHLD, SIG_DFL);
    /* as awl_spawn.c's children: a group of its own */
    setsid();
    /* for ps and pkill: awlock, as when it was a program */
    prctl(PR_SET_NAME, AWL_LOCKER);
    exit(awlock_main(cfg, fd));
}

/** the helper: forks a locker for each request, until awl is gone */
static _Noreturn void
serve(int sock)
{
    /* the kernel reaps the lockers */
    signal(SIGCHLD, SIG_IGN);
    for (;;) {
        awlock_config_t cfg;
        union {
            struct cmsghdr h;
            char b[CMSG_SPACE(sizeof(int))];
        } ctl;
        struct iovec iov = { &cfg, sizeof cfg };
        struct msghdr msg = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = ctl.b, .msg_controllen = sizeof ctl.b,
        };
        ssize_t n = recvmsg(sock, &msg, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            _exit(n < 0);
        int fd = recvfd(&msg);
        /* awl's own request: anything else is a bug in awl */
        if (fd < 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || (size_t)n != sizeof cfg
                || !memchr(cfg.logo, '\0', sizeof cfg.logo)) {
            awl_err_printf("lock helper: a malformed request\n");
            if (fd >= 0)
                close(fd);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(sock);
            locker(&cfg, fd);
        }
        if (pid < 0)
            awl_err_printf("lock helper: can't fork awlock: %s\n", strerror(errno));
        /* a locker that never was: awl sees its client go */
        close(fd);
    }
}

void
lockhelperstart(awl_process_t *proc)
{
    int sv[2];
    /* SEQPACKET: one request per message, the fd with it */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
        awl_err_printf("lock helper: no socket: %s; no awlock\n", strerror(errno));
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        awl_err_printf("lock helper: can't fork: %s; no awlock\n", strerror(errno));
        close(sv[0]);
        close(sv[1]);
        return;
    }
    if (pid == 0) {
        /* not awl to ps and pkill (pkill -x awl) */
        prctl(PR_SET_NAME, "awl-lockhelper");
        close(sv[0]);
        /* stdin /dev/null, not closed: a locker's socket would get fd 0;
         * stdout to stderr, as awl_spawn.c's children */
        int null = open("/dev/null", O_RDONLY);
        if (null > STDIN_FILENO) {
            dup2(null, STDIN_FILENO);
            close(null);
        }
        dup2(STDERR_FILENO, STDOUT_FILENO);
        serve(sv[1]);
    }
    close(sv[1]);
    /* awl never waits on it: a request that doesn't fit now fails */
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    proc->lockhelper.fd = sv[0];
    proc->lockhelper.pid = pid;
}

bool
lockhelperrun(awl_process_t *proc, const awlock_config_t *cfg, int fd)
{
    if (proc->lockhelper.fd < 0) {
        awl_err_printf("lock: no lock helper, can't start awlock\n");
        return false;
    }
    union {
        struct cmsghdr h;
        char b[CMSG_SPACE(sizeof(int))];
    } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct iovec iov = { (void *)cfg, sizeof *cfg };
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctl.b, .msg_controllen = sizeof ctl.b,
    };
    struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    if (sendmsg(proc->lockhelper.fd, &msg, MSG_NOSIGNAL) < 0) {
        awl_err_printf("lock: the lock helper didn't take awlock: %s\n", strerror(errno));
        return false;
    }
    return true;
}

void
lockhelperexited(awl_process_t *proc, pid_t pid)
{
    if (pid != proc->lockhelper.pid)
        return;
    awl_err_printf("lock: the lock helper (%d) exited; no more awlock\n", (int)pid);
    proc->lockhelper.pid = 0;
    close(proc->lockhelper.fd);
    proc->lockhelper.fd = -1;
}
#endif
