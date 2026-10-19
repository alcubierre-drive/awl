#include "awl_spawn.h"
#include "awl_config.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_xwayland.h"
#include "tray/awl_tray.h"
#include <fcntl.h>

/** vfork(2) is a glibc/BSD extension not declared under the strict
 * -D_POSIX_C_SOURCE=200809L this file is built with, though it's present
 * in libc regardless; declare it ourselves rather than widen the feature
 * test macros for the whole translation unit. Used instead of fork() when
 * spawning children after awl's background threads (plugin monitors, GPU
 * driver threads) are running, since fork() alone risks inheriting a lock
 * one of those threads held at the moment of the fork -- see spawn_pid(). */
extern pid_t vfork(void);

/** how long awl waits on exit for its children */
#define CHILD_STOP_MS 3000

static void autostart(const char **argv);
static void unblocksignals(void);

/** the startup command's */
static pid_t child_pid = -1;
static pid_t Autostarted_pids[256] = {0};
static int Autostarted_pids_sz = 0;

int
handlesigchld(int signo, void *data)
{
    siginfo_t in;
    /* wlroots reaps the Xwayland fork itself and gives up on Xwayland if
     * that fails, so look first (WNOWAIT) and leave that one alone */
    while (!waitid(P_ALL, 0, &in, WEXITED | WNOHANG | WNOWAIT) && in.si_pid) {
#ifdef XWAYLAND
        if (xwayland && xwayland->server && in.si_pid == xwayland->server->pid)
            break;
#endif
        waitpid(in.si_pid, NULL, 0);
        lockerexited(in.si_pid);
    }
    return 0;
}

pid_t
spawn_pid(const Arg *arg)
{
    char **argv = (char **)arg->v, **envp = NULL;
    const awl_tray_app_env_t *tray = NULL;
    pid_t pid;
    /* TRAY_ICONS first: the rest, with awl_tray_app_env()'s variables (the
     * child can't set them itself, it shares our memory until it execs) */
    if (argv[0] && !strcmp(argv[0], TRAY_ICONS)) {
        if (!*++argv)
            return -1;
        if ((tray = awl_tray_app_env()) && !(envp = envwith(tray->env))) {
            awl_err_printf("%s: out of memory, starting it in the desktop's icon theme\n", argv[0]);
            tray = NULL;
        }
    }
    /* on execvp failure, must call _exit() not exit()/die(): vfork()'s child
     * shares stdio buffers with the parent until it exits or execs. */
    pid = vfork();
    if (pid < 0) {
        awl_err_printf("vfork failed: %s\n", strerror(errno));
    } else if (pid == 0) {
        unblocksignals();
        close(STDIN_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        setsid();
        /* its own fd table: this doesn't touch ours */
        for (int i = 0; tray && i < (int)LENGTH(tray->fds); i++)
            if (tray->fds[i] >= 0)
                fcntl(tray->fds[i], F_SETFD, 0);
        if (envp)
            execvpenv(argv[0], argv, envp);
        else
            execvp(argv[0], argv);
        awl_err_printf("execvp %s failed: %s\n", argv[0], strerror(errno));
        _exit(1);
    }
    free(envp);
    return pid < 0 ? -1 : pid;
}

void spawn(const Arg *arg) { spawn_pid(arg); }

/** spawns argv and remembers it, so it is stopped when awl exits; refuses to
 * start it at all if there is no room left to remember it */
void
autostart(const char **argv)
{
    pid_t pid;
    if (Autostarted_pids_sz >= (int)LENGTH(Autostarted_pids)) {
        awl_wrn_printf("autostart: more than %d programs, not starting %s\n",
                (int)LENGTH(Autostarted_pids),
                argv[0] && !strcmp(argv[0], TRAY_ICONS) && argv[1] ? argv[1] : argv[0]);
        return;
    }
    /* a failed spawn returns -1; never let that reach kill()/waitpid() */
    if ((pid = spawn_pid(&(const Arg){.v=argv})) > 0)
        Autostarted_pids[Autostarted_pids_sz++] = pid;
}

void
stopchildren(void)
{
    pid_t pids[LENGTH(Autostarted_pids) + 1], r;
    struct timespec tick = {0, 20 * 1000 * 1000};
    int i, n = 0, left, waited;

    if (child_pid > 0)
        pids[n++] = child_pid;
    for (i = 0; i < Autostarted_pids_sz; i++)
        if (Autostarted_pids[i] > 0)
            pids[n++] = Autostarted_pids[i];
    child_pid = -1;
    Autostarted_pids_sz = 0;

    for (i = 0; i < n; i++)
        kill(-pids[i], SIGTERM);
    for (waited = 0;; waited += 20) {
        left = 0;
        for (i = 0; i < n; i++) {
            if (!pids[i])
                continue;
            /* ECHILD: handlesigchld() already reaped it */
            if ((r = waitpid(pids[i], NULL, WNOHANG)) == pids[i] || (r < 0 && errno == ECHILD))
                pids[i] = 0;
            else
                left++;
        }
        if (!left || waited >= CHILD_STOP_MS)
            break;
        nanosleep(&tick, NULL);
    }
    for (i = 0; i < n; i++) {
        if (!pids[i])
            continue;
        awl_wrn_printf("pid %d still running %d ms after SIGTERM, sending SIGKILL\n",
                (int)pids[i], CHILD_STOP_MS);
        kill(-pids[i], SIGKILL);
        waitpid(pids[i], NULL, 0);
    }
}

/** For a (v)forked child before exec, which would keep setup()'s signal mask
 * and ignored SIGPIPE. Both are the child's own even after `vfork()`. */
void
unblocksignals(void)
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
}

void
spawnstartup(const char *cmd)
{
    /* Uses vfork(), not fork(): by this point setup() has already started the
     * plugin/GPU driver threads, so a plain fork() risks inheriting a lock
     * (e.g. malloc's arena lock) held by one of those threads at the moment
     * of the fork -- see vfork() above. */
    if (!cmd)
        return;
    if ((child_pid = vfork()) < 0)
        die("startup: vfork:");
    if (child_pid == 0) {
        unblocksignals();
        close(STDIN_FILENO);
        setsid();
        execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
        awl_err_printf("startup: execl failed: %s\n", strerror(errno));
        _exit(1);
    }
}

void
autostartall(void)
{
    for (size_t i = 0; i < cfg->autostart.n; ++i) {
        if (!cfg->autostart.v[i][0]) continue; /* { NULL }: an empty entry */
        autostart(cfg->autostart.v[i]);
    }
}
