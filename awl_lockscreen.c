#include "awl_lockscreen.h"
#include "awl_alert.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_lockhelper.h"
#include "awl_log.h"
#include "awl_popup.h"
#include "awl_output.h"
#include "awl_plugin_host.h"
#include "awl_process.h"
#include "awl_spawn.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#ifdef SCENEFX_HAS_SHATTER
#define AWL_wlr_scene_blur_set_shatter(blur, shatter) \
    wlr_scene_blur_set_shatter((blur), (shatter))
#else
#define AWL_wlr_scene_blur_set_shatter(blur, shatter) {}
#endif

/** how long the logind thread may not listen before the alert goes up
 * (:c:func:`logindupdate`), ms */
#define LOGIND_GRACE_MS 10000
/** the most a lock waits to cover every output, ms (:c:func:`lockcovered`) */
#define LOCK_COVER_MS 2000
/** the most a locker may take to lock, ms; then it is stopped
 * (:c:func:`lockerlate`) */
#define LOCKER_START_MS 10000
/** the most lockers awl starts on its own while the session is locked
 * without a lock (:c:func:`lockrecover`), until the next unlock */
#define LOCKER_RESTARTS 5

typedef struct {
    struct wlr_scene_tree *scene;

    struct wlr_session_lock_v1 *lock;
    struct wl_listener new_surface;
    struct wl_listener unlock;
    struct wl_listener destroy;
    awl_server_t *srv;
} SessionLock;

static void createlocksurface(struct wl_listener *listener, void *data);
static void destroylock(awl_server_t *srv, SessionLock *lock, int unlock);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void lockcovered(awl_server_t *srv);
static void lockergone(awl_server_t *srv);
static int lockerlate(void *data);
static void lockerstarted(awl_server_t *srv);
static void lockrecover(void *data);
static void lockrecoversoon(awl_server_t *srv);
static int coveredtimeout(void *data);
static int lockfire(int fd, uint32_t mask, void *data);
static void lockwake(awl_process_t *proc);
static unsigned logindneeded(awl_server_t *srv);
static void logindupdate(awl_server_t *srv);
static int logindtimeout(void *data);
static void unlocksession(struct wl_listener *listener, void *data);

/* lock requests and the logind thread's state, from any thread (see
 * awl_lock_request()), are :c:member:`awl_process_t.lock` */

/** The screen lock's state (:c:member:`awl_server_t.lock`), main thread only */
struct awl_lock {
    /** for ``locker_destroy`` */
    awl_server_t *srv;
    /* key presses while locked, reset on unlock; nothing reads it yet (only
     * a count: never the keys, never their timing) */
    unsigned keystrokes;
    struct wlr_scene_blur *blur;
    /** the lock in place, or NULL */
    struct wlr_session_lock_v1 *cur;
    /** :c:member:`awl_process_t.lock`'s fd's event handler */
    struct wl_event_source *source;
    /** the logind alert's grace period, see :c:func:`logindupdate` */
    struct wl_event_source *logind_timer;
    /** the most a lock waits for its first frame on every output */
    struct wl_event_source *covered_timer;
    /** the most the locker takes to lock, :c:macro:`LOCKER_START_MS` */
    struct wl_event_source *start_timer;
    /** :c:func:`lockrecover`, due; NULL if not */
    struct wl_event_source *recover_idle;
    /** lockers :c:func:`lockrecover` started since the last unlock */
    unsigned restarts;
    /** the locker :c:func:`lockscreen` started, until it exits; 0 if none
     * or awlock (``locker_client``) */
    pid_t locker_pid;
    /** awlock, the lock helper's (awl_lockhelper.h): its Wayland client,
     * until that goes; NULL if none */
    struct wl_client *locker_client;
    /** ``locker_client``'s destroy */
    struct wl_listener locker_destroy;
    /** its argv[0], as :c:member:`cfg->lock.locker <awl_config_t.lock.locker>` was then */
    char locker_name[64];
    /** what the logind thread last said it listens for, :c:enum:`awl_logind_flags` */
    unsigned logind_flags;
    /** the locker has locked the session at least once */
    bool locker_locked:1;
    /** the logind thread doesn't listen for all it should; the grace period runs since then */
    bool logind_missing:1;
    /** a lock waits for each output to show it, see :c:func:`lockcovered` */
    bool covered_pending:1;
    /** the locker was stopped for taking too long (:c:func:`lockerlate`) */
    bool locker_late:1;
    /** :c:func:`lockfini` ran: no more lockers */
    bool closing:1;
};

void
awl_lock_fini(awl_server_t *srv)
{
    int fd = atomic_exchange(&srv->proc->lock.fd, -1);
    if (fd >= 0) close(fd);
    free(srv->lock);
    srv->lock = NULL;
}

void
awl_lock_logind_state(awl_process_t *proc, unsigned flags)
{
    atomic_store(&proc->lock.logind_flags, flags);
    lockwake(proc);
}

void
awl_lock_request(awl_process_t *proc)
{
    atomic_store(&proc->lock.requested, 1);
    lockwake(proc);
}

/** :c:func:`motionnotify` without a motion: the pointer enters the surface
 * under it */
static void
lockpointer(void *data)
{
    motionnotify(data, 0, NULL, 0, 0, 0, 0);
}

/** A lock surface was mapped. The pointer only enters a surface when it
 * moves, so one that appeared under a still pointer wouldn't know where the
 * pointer is; awlock would draw its logo centred until it moved. It enters
 * once the commit is done (:c:func:`lockpointer`, idle): the map comes during
 * the commit, before the scene shows the surface. */
static void
maplocksurface(struct wl_listener *listener, void *data)
{
    Monitor *m = wl_container_of(listener, m, map_lock_surface);
    wl_event_loop_add_idle(m->srv->event_loop, lockpointer, m->srv);
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
    SessionLock *lock = wl_container_of(listener, lock, new_surface);
    awl_server_t *srv = lock->srv;
    struct wlr_session_lock_surface_v1 *lock_surface = data;
    Monitor *m = lock_surface->output->data;
    struct wlr_scene_tree *scene_tree = lock_surface->surface->data
            = wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
    m->lock_surface = lock_surface;

    wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
    wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

    LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);
    LISTEN(&lock_surface->surface->events.map, &m->map_lock_surface, maplocksurface);

    if (m == srv->out.selmon)
        client_notify_enter(srv, lock_surface->surface, wlr_seat_get_keyboard(srv->seat));
}

void
destroylock(awl_server_t *srv, SessionLock *lock, int unlock)
{
    wlr_seat_keyboard_notify_clear_focus(srv->seat);
    /* a crashed locker leaves the screen locked, but only under awl's blur:
     * a suspend now waits for the next locker */
    srv->lock->covered_pending = 0;
    /* gone after lockfini(): a lock still in place then goes with its client */
    if (srv->lock->covered_timer)
        wl_event_source_timer_update(srv->lock->covered_timer, 0);
    awl_plugins_set_covered(srv->proc, 0);
    if ((srv->locked = !unlock)) {
        lockrecoversoon(srv);
        goto destroy;
    }
    srv->lock->restarts = 0;
    popupslocked(srv);
    srv->lock->keystrokes = 0;
    updatepluginpause(srv);

    if (srv->lock->blur) {
        wlr_scene_node_set_enabled(&srv->lock->blur->node, 0);
        AWL_wlr_scene_blur_set_shatter(srv->lock->blur, 0.01);
    }

    focusclient(srv, focustop(srv, srv->out.selmon), 0);
    motionnotify(srv, 0, NULL, 0, 0, 0, 0);

    /* catch up on everything skipped while locked */
    barsdirty(srv);

destroy:
    wl_list_remove(&lock->new_surface.link);
    wl_list_remove(&lock->unlock.link);
    wl_list_remove(&lock->destroy.link);

    wlr_scene_node_destroy(&lock->scene->node);
    srv->lock->cur = NULL;
    free(lock);
}

void
destroylocksurface(struct wl_listener *listener, void *data)
{
    Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
    awl_server_t *srv = m->srv;
    struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

    m->lock_surface = NULL;
    wl_list_remove(&m->destroy_lock_surface.link);
    wl_list_remove(&m->map_lock_surface.link);

    if (lock_surface->surface != srv->seat->keyboard_state.focused_surface)
        return;

    if (srv->locked && srv->lock->cur && !wl_list_empty(&srv->lock->cur->surfaces)) {
        surface = wl_container_of(srv->lock->cur->surfaces.next, surface, link);
        client_notify_enter(srv, surface->surface, wlr_seat_get_keyboard(srv->seat));
    } else if (!srv->locked) {
        focusclient(srv, focustop(srv, srv->out.selmon), 1);
    } else {
        wlr_seat_keyboard_clear_focus(srv->seat);
    }
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
    SessionLock *lock = wl_container_of(listener, lock, destroy);
    awl_server_t *srv = lock->srv;
    destroylock(srv, lock, 0);
}

/** A lock covers the screen once every enabled output has shown a frame
 * with the locker's surface on it, or after :c:macro:`LOCK_COVER_MS` anyway (a locker
 * that never draws, an output that doesn't render). The logind thread waits
 * for that before it lets a suspend go ahead, so the last image before
 * sleep, and the first after it, is the locker's. */
void
lockcovered(awl_server_t *srv)
{
    Monitor *m;
    if (!srv->lock->covered_pending)
        return;
    wl_list_for_each(m, &srv->out.mons, link)
        if (m->wlr_output->enabled && !m->lock_shown)
            return;
    srv->lock->covered_pending = 0;
    if (srv->lock->covered_timer)
        wl_event_source_timer_update(srv->lock->covered_timer, 0);
    awl_plugins_set_covered(srv->proc, 1);
}

int
coveredtimeout(void *data)
{
    awl_server_t *srv = data;
    if (srv->lock->covered_pending)
        awl_wrn_printf("lock: not drawn on every output after %d ms, covered anyway\n",
                LOCK_COVER_MS);
    srv->lock->covered_pending = 0;
    awl_plugins_set_covered(srv->proc, 1);
    return 0;
}

/** :c:member:`awl_process_t.lock`'s fd's event handler: a lock request, a
 * new logind thread state */
int
lockfire(int fd, uint32_t mask, void *data)
{
    awl_server_t *srv = data;
    uint64_t count;
    unsigned flags;
    /* drained first: whatever is stored after this wakes the loop again */
    if (read(fd, &count, sizeof count) < 0) {}
    flags = atomic_load(&srv->proc->lock.logind_flags);
    if (atomic_exchange(&srv->proc->lock.requested, 0))
        lockscreen(srv, NULL);
    if (flags != srv->lock->logind_flags) {
        srv->lock->logind_flags = flags;
        logindupdate(srv);
    }
    return 0;
}

#ifdef AWL_AWLOCK
/** awlock's client is gone: it exited, or never started */
static void
awlockgone(struct wl_listener *listener, void *data)
{
    struct awl_lock *lock = wl_container_of(listener, lock, locker_destroy);
    wl_list_remove(&listener->link);
    lock->locker_client = NULL;
    lockergone(lock->srv);
}

/** awlock's configuration from awl's (:c:member:`awl_config_t.lock`, the
 * lock_* colors); a logo file name too long is logged, and awlock's own
 * logo drawn */
static void
awlockconfig(const awl_config_t *c, awlock_config_t *a)
{
    const awl_lock_colors_t *k[AWLOCK_STATES] = {
        &c->colors.lock_waiting, &c->colors.lock_typing, &c->colors.lock_caps_lock,
        &c->colors.lock_validating, &c->colors.lock_wrong,
    };
    memset(a, 0, sizeof *a);
    for (int i = 0; i < AWLOCK_STATES; i++)
        a->colors[i] = (awlock_colors_t){ k[i]->border, k[i]->fill };
    a->show_logo = !c->lock.logo || c->lock.logo[0];
    if (c->lock.logo && strlen(c->lock.logo) >= sizeof a->logo)
        awl_err_printf("lock: logo %s: longer than %d bytes, awlock's own instead\n",
                c->lock.logo, AWLOCK_LOGO_MAX - 1);
    else if (c->lock.logo)
        strcpy(a->logo, c->lock.logo);
    a->logo_scale = c->lock.logo_scale;
    a->fingerprint = c->lock.fingerprint;
    a->inactive_alpha = c->lock.inactive_alpha;
    a->active_alpha = c->lock.active_alpha;
    a->fingerprint_retry_ms = abs(c->lock.fingerprint_retry_ms);
}

/** awlock, through the lock helper, on a socketpair whose other end is its
 * client; false if it can't. argv is the config's locker: awlock takes no
 * arguments, any are logged */
static bool
startawlock(awl_server_t *srv, const char *const *argv)
{
    int sv[2];
    struct wl_client *client;
    awlock_config_t cfg;
    if (argv[1])
        awl_wrn_printf("lock: %s takes no arguments (%s ...): lock.* and colors.lock_* "
                "configure it\n", argv[0], argv[1]);
    awlockconfig(srv->cfg, &cfg);
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) {
        awl_err_printf("lock: no socket for %s: %s\n", argv[0], strerror(errno));
        return false;
    }
    if (!(client = wl_client_create(srv->dpy, sv[0]))) {
        awl_err_printf("lock: no client for %s\n", argv[0]);
        close(sv[0]);
        close(sv[1]);
        return false;
    }
    if (!lockhelperrun(srv->proc, &cfg, sv[1])) {
        close(sv[1]);
        wl_client_destroy(client);
        return false;
    }
    close(sv[1]);
    srv->lock->locker_client = client;
    srv->lock->locker_destroy.notify = awlockgone;
    wl_client_add_destroy_listener(client, &srv->lock->locker_destroy);
    lockerstarted(srv);
    return true;
}
#endif

/** Starts :c:member:`cfg->lock.locker <awl_config_t.lock.locker>`, unless a lock is in place (a locker holds
 * it) or the one started last is still running and yet to lock. While
 * locked without a lock (the locker crashed), a new locker takes over;
 * awl starts one itself then too (:c:func:`lockrecover`). A locker that
 * doesn't lock within :c:macro:`LOCKER_START_MS` is stopped (:c:func:`lockerlate`).
 * awlock, :c:macro:`AWL_LOCKER`, is part of awl: a fork of the lock helper
 * (awl_lockhelper.h), on a socket awl made its client of. Any other locker
 * is looked for in ``PATH``. */
void
lockscreen(awl_server_t *srv, const Arg *arg)
{
    const char *const *argv = srv->cfg->lock.locker;
    pid_t pid;
    if (srv->lock->cur || srv->lock->closing)
        return;
    if (srv->lock->locker_pid > 0 || srv->lock->locker_client) {
        awl_log_printf("lock: %s is still starting\n", srv->lock->locker_name);
        return;
    }
    if (!argv || !argv[0]) {
        awl_alert_set(srv, AwlAlertLocker, "no locker configured");
        return;
    }
    snprintf(srv->lock->locker_name, sizeof(srv->lock->locker_name), "%s", argv[0]);
#ifdef AWL_AWLOCK
    if (!strcmp(argv[0], AWL_LOCKER)) {
        if (!startawlock(srv, argv))
            awl_alert_set(srv, AwlAlertLocker, "can't start %s", argv[0]);
        return;
    }
#endif
    pid = spawn_pid(srv->proc, &(Arg){.v = argv});
    if (pid > 0) {
        srv->lock->locker_pid = pid;
        lockerstarted(srv);
    } else {
        awl_alert_set(srv, AwlAlertLocker, "can't start %s", argv[0]);
    }
}

/** wakes the main loop, which then looks at proc's lock: requested and
 * logind_flags (:c:func:`lockfire`); any thread */
void
lockwake(awl_process_t *proc)
{
    int fd = atomic_load(&proc->lock.fd);
    uint64_t one = 1;
    /* EAGAIN only happens if the counter would overflow, i.e. the main loop
     * is due to look anyway */
    if (fd >= 0 && write(fd, &one, sizeof one) < 0) {}
}

/** what the logind thread has to listen for */
unsigned
logindneeded(awl_server_t *srv)
{
    return AWL_LOGIND_LOCK | (srv->cfg->lock.on_suspend ? AWL_LOGIND_SLEEP : 0);
}

/** The logind alert: up once the logind thread hasn't listened for all of
 * logindneeded() for :c:macro:`LOGIND_GRACE_MS` (it starts and reconnects within that,
 * a plugin reload too), down as soon as it does. That covers a thread that
 * never reports at all, e.g. without a plugin library. */
void
logindupdate(awl_server_t *srv)
{
    if (!(logindneeded(srv) & ~srv->lock->logind_flags)) {
        srv->lock->logind_missing = 0;
        wl_event_source_timer_update(srv->lock->logind_timer, 0);
        awl_alert_clear(srv, AwlAlertLogind);
    } else if (awl_alert_up(srv, AwlAlertLogind)) {
        logindtimeout(srv); /* the text may change */
    } else if (!srv->lock->logind_missing) {
        /* only from when it stopped: a thread that keeps reconnecting and
         * failing must not keep the alert down */
        srv->lock->logind_missing = 1;
        wl_event_source_timer_update(srv->lock->logind_timer, LOGIND_GRACE_MS);
    }
}

int
logindtimeout(void *data)
{
    awl_server_t *srv = data;
    unsigned missing = logindneeded(srv) & ~srv->lock->logind_flags;
    if (missing == (AWL_LOGIND_LOCK | AWL_LOGIND_SLEEP))
        awl_alert_set(srv, AwlAlertLogind, "Not listening to logind: NO LOCK ON SUSPEND");
    else if (missing & AWL_LOGIND_SLEEP)
        awl_alert_set(srv, AwlAlertLogind, "logind: NO LOCK ON SUSPEND");
    else if (missing)
        awl_alert_set(srv, AwlAlertLogind, "logind: loginctl lock-session does nothing");
    return 0;
}

void
locksession(struct wl_listener *listener, void *data)
{
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_session_lock);
    struct wlr_session_lock_v1 *session_lock = data;
    SessionLock *lock;
    Monitor *m;
    if (srv->lock->blur && srv->cfg->lock.blur) {
        wlr_scene_node_set_enabled(&srv->lock->blur->node, 1);
        AWL_wlr_scene_blur_set_shatter(srv->lock->blur, 0.01);
    }
    if (srv->lock->cur) {
        wlr_session_lock_v1_destroy(session_lock);
        return;
    }
    lock = session_lock->data = ecalloc(1, sizeof(*lock));
    lock->srv = srv;
    focusclient(srv, NULL, 0);
    /* the launcher drops the keyboard, the menus go: they'd only wait under
     * the locker */
    popupshideall(srv);

    lock->scene = wlr_scene_tree_create(srv->layers[LyrBlock]);
    srv->lock->cur = lock->lock = session_lock;
    srv->locked = 1;
    popupslocked(srv);
    updatepluginpause(srv);
    if (srv->lock->locker_pid > 0 || srv->lock->locker_client)
        srv->lock->locker_locked = 1;
    if (srv->lock->start_timer)
        wl_event_source_timer_update(srv->lock->start_timer, 0);
    awl_alert_clear(srv, AwlAlertLocker);

    LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
    LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
    LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

    wlr_session_lock_v1_send_locked(session_lock);

    /* covered once every output has shown a frame of it */
    wl_list_for_each(m, &srv->out.mons, link)
        m->lock_shown = 0;
    srv->lock->covered_pending = 1;
    wl_event_source_timer_update(srv->lock->covered_timer, LOCK_COVER_MS);
    lockcovered(srv);
}

void
unlocksession(struct wl_listener *listener, void *data)
{
    SessionLock *lock = wl_container_of(listener, lock, unlock);
    awl_server_t *srv = lock->srv;
    destroylock(srv, lock, 1);
}

void
lockinit(awl_server_t *srv)
{
    srv->lock = ecalloc(1, sizeof *srv->lock);
    srv->lock->srv = srv;
    /* until the logind thread says otherwise */
    srv->lock->logind_missing = 1;
    /* made either way, cfg->lock.blur can change with a reload */
    {
        srv->lock->blur = wlr_scene_blur_create( srv->layers[LyrBlock], srv->out.sgeom.width, srv->out.sgeom.height );
        wlr_scene_node_set_position(&srv->lock->blur->node, srv->out.sgeom.x, srv->out.sgeom.y);
        wlr_scene_blur_set_size(srv->lock->blur, srv->out.sgeom.width, srv->out.sgeom.height);
        wlr_scene_blur_set_strength( srv->lock->blur, srv->cfg->blur.strength );
        wlr_scene_blur_set_alpha( srv->lock->blur, srv->cfg->blur.alpha );
        wlr_scene_blur_set_should_only_blur_bottom_layer( srv->lock->blur, 0 );
        wlr_scene_node_set_enabled(&srv->lock->blur->node, 0);
        wlr_scene_node_lower_to_bottom(&srv->lock->blur->node);
    }
    /* without it, requests and states are dropped (the logind alert stays) */
    atomic_store(&srv->proc->lock.fd, eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (atomic_load(&srv->proc->lock.fd) >= 0)
        srv->lock->source = wl_event_loop_add_fd(srv->event_loop, atomic_load(&srv->proc->lock.fd),
                WL_EVENT_READABLE, lockfire, srv);
    srv->lock->covered_timer = wl_event_loop_add_timer(srv->event_loop, coveredtimeout, srv);
    srv->lock->start_timer = wl_event_loop_add_timer(srv->event_loop, lockerlate, srv);
    /* the logind thread has until then to start listening (logind_missing) */
    srv->lock->logind_timer = wl_event_loop_add_timer(srv->event_loop, logindtimeout, srv);
    wl_event_source_timer_update(srv->lock->logind_timer, LOGIND_GRACE_MS);
}

void
lockfini(awl_server_t *srv)
{
    /* the clients go after this: no alert for awlock then, and no new
     * locker for a lock that goes with its client */
    srv->lock->closing = 1;
    if (srv->lock->locker_client)
        wl_list_remove(&srv->lock->locker_destroy.link);
    srv->lock->locker_client = NULL;
    if (srv->lock->source) wl_event_source_remove(srv->lock->source);
    if (srv->lock->logind_timer) wl_event_source_remove(srv->lock->logind_timer);
    if (srv->lock->covered_timer) wl_event_source_remove(srv->lock->covered_timer);
    if (srv->lock->start_timer) wl_event_source_remove(srv->lock->start_timer);
    if (srv->lock->recover_idle) wl_event_source_remove(srv->lock->recover_idle);
    srv->lock->source = srv->lock->logind_timer = srv->lock->covered_timer = NULL;
    srv->lock->start_timer = srv->lock->recover_idle = NULL;
}

void
lockshown(awl_server_t *srv, Monitor *m)
{
    /* the locker's own surface, not just the blur behind it */
    if (srv->lock->covered_pending && m->lock_surface && wlr_surface_has_buffer(m->lock_surface->surface)) {
        m->lock_shown = 1;
        lockcovered(srv);
    }
}

void
lockresize(awl_server_t *srv)
{
    if (srv->lock->blur) {
        wlr_scene_node_set_position(&srv->lock->blur->node, srv->out.sgeom.x, srv->out.sgeom.y);
        wlr_scene_blur_set_size(srv->lock->blur, srv->out.sgeom.width, srv->out.sgeom.height);
    }
}

void
lockkeystroke(awl_server_t *srv)
{
    srv->lock->keystrokes++;
    if (srv->lock->blur)
        AWL_wlr_scene_blur_set_shatter(srv->lock->blur, (float)srv->lock->keystrokes/20.0);
}

/** a locker started: it has :c:macro:`LOCKER_START_MS` to lock */
static void
lockerstarted(awl_server_t *srv)
{
    srv->lock->locker_locked = 0;
    srv->lock->locker_late = 0;
    if (srv->lock->start_timer)
        wl_event_source_timer_update(srv->lock->start_timer, LOCKER_START_MS);
}

/** The locker hasn't locked in :c:macro:`LOCKER_START_MS`: it is stopped,
 * so the next lock request starts another instead of waiting for it.
 * awlock's client goes (it exits once it sees that; its pid is the lock
 * helper's), any other locker's process group gets SIGKILL. */
static int
lockerlate(void *data)
{
    awl_server_t *srv = data;
    struct awl_lock *lock = srv->lock;
    if (lock->locker_locked || (lock->locker_pid <= 0 && !lock->locker_client))
        return 0;
    awl_err_printf("lock: %s didn't lock in %d ms, stopping it\n",
            lock->locker_name, LOCKER_START_MS);
    lock->locker_late = 1;
    if (lock->locker_client)
        wl_client_destroy(lock->locker_client); /* awlockgone() */
    else if (kill(-lock->locker_pid, SIGKILL) && kill(lock->locker_pid, SIGKILL))
        awl_err_printf("lock: can't stop %s (%d): %s\n", lock->locker_name,
                (int)lock->locker_pid, strerror(errno));
    return 0;
}

/** the locker is gone: an alert if it never locked, and a new one if the
 * session is still locked (:c:func:`lockrecover`) */
static void
lockergone(awl_server_t *srv)
{
    if (srv->lock->closing)
        return;
    if (srv->lock->start_timer)
        wl_event_source_timer_update(srv->lock->start_timer, 0);
    if (srv->lock->locker_late)
        awl_alert_set(srv, AwlAlertLocker, "%s didn't lock in %d s", srv->lock->locker_name,
                LOCKER_START_MS / 1000);
    else if (!srv->lock->locker_locked)
        awl_alert_set(srv, AwlAlertLocker, "%s exited without locking", srv->lock->locker_name);
    lockrecoversoon(srv);
}

/** :c:func:`lockrecover` once this round of events is through: of a locker
 * that crashes, the lock and the client (or the process) go in either
 * order */
static void
lockrecoversoon(awl_server_t *srv)
{
    if (srv->lock->closing || srv->lock->recover_idle)
        return;
    srv->lock->recover_idle = wl_event_loop_add_idle(srv->event_loop, lockrecover, srv);
}

/** Locked without a lock and without a locker (it crashed, or was
 * stopped): the screen shows only the blur, and while locked no key binding
 * starts one. awl starts the next locker itself, up to
 * :c:macro:`LOCKER_RESTARTS` times until the next unlock; after that it
 * takes a lock request (``loginctl lock-session``, a suspend). */
static void
lockrecover(void *data)
{
    awl_server_t *srv = data;
    struct awl_lock *lock = srv->lock;
    lock->recover_idle = NULL;
    if (!srv->locked || lock->cur || lock->locker_pid > 0 || lock->locker_client)
        return;
    if (lock->restarts >= LOCKER_RESTARTS) {
        awl_err_printf("lock: %s gone, locked without a locker; %d started already, "
                "loginctl lock-session starts another\n", lock->locker_name, LOCKER_RESTARTS);
        awl_alert_set(srv, AwlAlertLocker, "%s keeps failing: loginctl lock-session",
                lock->locker_name);
        return;
    }
    lock->restarts++;
    awl_wrn_printf("lock: %s gone, locked without a locker; starting another (%u of %d)\n",
            lock->locker_name, lock->restarts, LOCKER_RESTARTS);
    lockscreen(srv, NULL);
}

void
lockerexited(awl_server_t *srv, pid_t pid)
{
    lockhelperexited(srv->proc, pid);
    if (pid != srv->lock->locker_pid)
        return;
    srv->lock->locker_pid = 0;
    lockergone(srv);
}

void
lockconfigure(awl_server_t *srv)
{
    if (srv->lock->blur) {
        wlr_scene_blur_set_strength(srv->lock->blur, srv->cfg->blur.strength);
        wlr_scene_blur_set_alpha(srv->lock->blur, srv->cfg->blur.alpha);
        if (srv->locked)
            wlr_scene_node_set_enabled(&srv->lock->blur->node, srv->cfg->lock.blur);
    }
}
