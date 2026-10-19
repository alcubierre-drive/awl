#include "awl_lockscreen.h"
#include "awl_alert.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_plugin_host.h"
#include "awl_spawn.h"

#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/eventfd.h>

#ifdef SCENEFX_HAS_SHATTER
#define AWL_wlr_scene_blur_set_shatter(blur, shatter) \
    wlr_scene_blur_set_shatter((blur), (shatter))
#else
#define AWL_wlr_scene_blur_set_shatter(blur, shatter) {}
#endif

/** how long the logind thread may not listen before the alert goes up
 * (logindupdate()), and the most a lock waits to cover every output
 * (lockcovered()) */
#define LOGIND_GRACE_MS 10000
#define LOCK_COVER_MS 2000

typedef struct {
    struct wlr_scene_tree *scene;

    struct wlr_session_lock_v1 *lock;
    struct wl_listener new_surface;
    struct wl_listener unlock;
    struct wl_listener destroy;
} SessionLock;

static void createlocksurface(struct wl_listener *listener, void *data);
static void destroylock(SessionLock *lock, int unlock);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void lockcovered(void);
static int coveredtimeout(void *data);
static int lockfire(int fd, uint32_t mask, void *data);
static void lockwake(void);
static unsigned logindneeded(void);
static void logindupdate(void);
static int logindtimeout(void *data);
static void unlocksession(struct wl_listener *listener, void *data);

int locked = 0;
/* key presses while locked, reset on unlock; nothing reads it yet (main
 * thread only, and only a count: never the keys, never their timing) */
static unsigned locked_keystrokes = 0;
static struct wlr_scene_blur *locked_bg_blur = NULL;
static struct wlr_session_lock_v1 *cur_lock;

/* lock requests and the logind thread's state, from any thread (see
 * awl_lock_request()): the eventfd that wakes the main loop, and what it
 * finds when it does (lockfire()); requests before then coalesce into one */
static _Atomic int lock_fd = -1;
static atomic_int lock_requested;
static atomic_uint lock_logind_flags;

/* the screen lock, see lockscreen(): lock_fd's event handler */
static struct wl_event_source *lock_source = NULL;
/** the logind alert's grace period, see logindupdate() */
static struct wl_event_source *logind_timer = NULL;
/** the most a lock waits for its first frame on every output */
static struct wl_event_source *covered_timer = NULL;
/** the locker lockscreen() started, until it exits; 0 if none */
static pid_t locker_pid = 0;
/** it has locked the session at least once */
static int locker_locked = 0;
/** what the logind thread last said it listens for, awl_logind_flags */
static unsigned logind_flags = 0;
/** it doesn't listen for all it should; the grace period runs since then */
static int logind_missing = 1;
/** a lock waits for each output to show it, see lockcovered() */
static int covered_pending = 0;

void
awl_lock_fini(void)
{
    int fd = atomic_exchange(&lock_fd, -1);
    if (fd >= 0) close(fd);
}

void
awl_lock_logind_state(unsigned flags)
{
    atomic_store(&lock_logind_flags, flags);
    lockwake();
}

void
awl_lock_request(void)
{
    atomic_store(&lock_requested, 1);
    lockwake();
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
    SessionLock *lock = wl_container_of(listener, lock, new_surface);
    struct wlr_session_lock_surface_v1 *lock_surface = data;
    Monitor *m = lock_surface->output->data;
    struct wlr_scene_tree *scene_tree = lock_surface->surface->data
            = wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
    m->lock_surface = lock_surface;

    wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
    wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

    LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);

    if (m == selmon)
        client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(seat));
}

void
destroylock(SessionLock *lock, int unlock)
{
    wlr_seat_keyboard_notify_clear_focus(seat);
    /* a crashed locker leaves the screen locked, but only under awl's blur:
     * a suspend now waits for the next locker */
    covered_pending = 0;
    wl_event_source_timer_update(covered_timer, 0);
    awl_plugins_set_covered(0);
    if ((locked = !unlock))
        goto destroy;
    locked_keystrokes = 0;
    updatepluginpause();

    if (locked_bg_blur) {
        wlr_scene_node_set_enabled(&locked_bg_blur->node, 0);
        AWL_wlr_scene_blur_set_shatter(locked_bg_blur, 0.01);
    }

    focusclient(focustop(selmon), 0);
    motionnotify(0, NULL, 0, 0, 0, 0);

    /* catch up on everything skipped while locked */
    drawbars();

destroy:
    wl_list_remove(&lock->new_surface.link);
    wl_list_remove(&lock->unlock.link);
    wl_list_remove(&lock->destroy.link);

    wlr_scene_node_destroy(&lock->scene->node);
    cur_lock = NULL;
    free(lock);
}

void
destroylocksurface(struct wl_listener *listener, void *data)
{
    Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
    struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

    m->lock_surface = NULL;
    wl_list_remove(&m->destroy_lock_surface.link);

    if (lock_surface->surface != seat->keyboard_state.focused_surface)
        return;

    if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
        surface = wl_container_of(cur_lock->surfaces.next, surface, link);
        client_notify_enter(surface->surface, wlr_seat_get_keyboard(seat));
    } else if (!locked) {
        focusclient(focustop(selmon), 1);
    } else {
        wlr_seat_keyboard_clear_focus(seat);
    }
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
    SessionLock *lock = wl_container_of(listener, lock, destroy);
    destroylock(lock, 0);
}

/** A lock covers the screen once every enabled output has shown a frame
 * with the locker's surface on it, or after LOCK_COVER_MS anyway (a locker
 * that never draws, an output that doesn't render). The logind thread waits
 * for that before it lets a suspend go ahead, so the last image before
 * sleep, and the first after it, is the locker's. */
void
lockcovered(void)
{
    Monitor *m;
    if (!covered_pending)
        return;
    wl_list_for_each(m, &mons, link)
        if (m->wlr_output->enabled && !m->lock_shown)
            return;
    covered_pending = 0;
    wl_event_source_timer_update(covered_timer, 0);
    awl_plugins_set_covered(1);
}

int
coveredtimeout(void *data)
{
    if (covered_pending)
        awl_wrn_printf("lock: not drawn on every output after %d ms, covered anyway\n",
                LOCK_COVER_MS);
    covered_pending = 0;
    awl_plugins_set_covered(1);
    return 0;
}

/** lock_fd's event handler: a lock request, a new logind thread state */
int
lockfire(int fd, uint32_t mask, void *data)
{
    uint64_t count;
    unsigned flags;
    /* drained first: whatever is stored after this wakes the loop again */
    if (read(fd, &count, sizeof count) < 0) {}
    flags = atomic_load(&lock_logind_flags);
    if (atomic_exchange(&lock_requested, 0))
        lockscreen(NULL);
    if (flags != logind_flags) {
        logind_flags = flags;
        logindupdate();
    }
    return 0;
}

/** Starts config_awl.h's locker, unless a lock is in place (a locker holds
 * it) or the one started last is still running and yet to lock. While
 * locked without a lock (the locker crashed), a new locker takes over. */
void
lockscreen(const Arg *arg)
{
    pid_t pid;
    if (cur_lock)
        return;
    if (locker_pid > 0) {
        awl_log_printf("lock: %s (%d) is still starting\n", locker[0], (int)locker_pid);
        return;
    }
    if ((pid = spawn_pid(&(Arg){.v = locker})) > 0) {
        locker_pid = pid;
        locker_locked = 0;
    } else {
        awl_alert_set(AwlAlertLocker, "can't start %s", locker[0]);
    }
}

/** what the logind thread has to listen for */
static /** wakes the main loop, which then looks at lock_requested and
 * lock_logind_flags (lockfire()); any thread */
void
lockwake(void)
{
    int fd = atomic_load(&lock_fd);
    uint64_t one = 1;
    /* EAGAIN only happens if the counter would overflow, i.e. the main loop
     * is due to look anyway */
    if (fd >= 0 && write(fd, &one, sizeof one) < 0) {}
}

unsigned
logindneeded(void)
{
    return AWL_LOGIND_LOCK | (lock_on_suspend ? AWL_LOGIND_SLEEP : 0);
}

/** The logind alert: up once the logind thread hasn't listened for all of
 * logindneeded() for LOGIND_GRACE_MS (it starts and reconnects within that,
 * a plugin reload too), down as soon as it does. That covers a thread that
 * never reports at all, e.g. without a plugin library. */
void
logindupdate(void)
{
    if (!(logindneeded() & ~logind_flags)) {
        logind_missing = 0;
        wl_event_source_timer_update(logind_timer, 0);
        awl_alert_clear(AwlAlertLogind);
    } else if (awl_alert_up(AwlAlertLogind)) {
        logindtimeout(NULL); /* the text may change */
    } else if (!logind_missing) {
        /* only from when it stopped: a thread that keeps reconnecting and
         * failing must not keep the alert down */
        logind_missing = 1;
        wl_event_source_timer_update(logind_timer, LOGIND_GRACE_MS);
    }
}

int
logindtimeout(void *data)
{
    unsigned missing = logindneeded() & ~logind_flags;
    if (missing == (AWL_LOGIND_LOCK | AWL_LOGIND_SLEEP))
        awl_alert_set(AwlAlertLogind, "Not listening to logind: NO LOCK ON SUSPEND");
    else if (missing & AWL_LOGIND_SLEEP)
        awl_alert_set(AwlAlertLogind, "logind: NO LOCK ON SUSPEND");
    else if (missing)
        awl_alert_set(AwlAlertLogind, "logind: loginctl lock-session does nothing");
    return 0;
}

void
locksession(struct wl_listener *listener, void *data)
{
    struct wlr_session_lock_v1 *session_lock = data;
    SessionLock *lock;
    Monitor *m;
    if (locked_bg_blur) {
        wlr_scene_node_set_enabled(&locked_bg_blur->node, 1);
        AWL_wlr_scene_blur_set_shatter(locked_bg_blur, 0.01);
    }
    if (cur_lock) {
        wlr_session_lock_v1_destroy(session_lock);
        return;
    }
    lock = session_lock->data = ecalloc(1, sizeof(*lock));
    focusclient(NULL, 0);

    lock->scene = wlr_scene_tree_create(layers[LyrBlock]);
    cur_lock = lock->lock = session_lock;
    locked = 1;
    updatepluginpause();
    if (locker_pid > 0)
        locker_locked = 1;
    awl_alert_clear(AwlAlertLocker);

    LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
    LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
    LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

    wlr_session_lock_v1_send_locked(session_lock);

    /* covered once every output has shown a frame of it */
    wl_list_for_each(m, &mons, link)
        m->lock_shown = 0;
    covered_pending = 1;
    wl_event_source_timer_update(covered_timer, LOCK_COVER_MS);
    lockcovered();
}

void
unlocksession(struct wl_listener *listener, void *data)
{
    SessionLock *lock = wl_container_of(listener, lock, unlock);
    destroylock(lock, 1);
}

void
lockinit(void)
{
    if (locked_blur) {
        locked_bg_blur = wlr_scene_blur_create( layers[LyrBlock], sgeom.width, sgeom.height );
        wlr_scene_node_set_position(&locked_bg_blur->node, sgeom.x, sgeom.y);
        wlr_scene_blur_set_size(locked_bg_blur, sgeom.width, sgeom.height);
        wlr_scene_blur_set_strength( locked_bg_blur, cfg->blur[0] );
        wlr_scene_blur_set_alpha( locked_bg_blur, cfg->blur[1] );
        wlr_scene_blur_set_should_only_blur_bottom_layer( locked_bg_blur, 0 );
        wlr_scene_node_set_enabled(&locked_bg_blur->node, 0);
        wlr_scene_node_lower_to_bottom(&locked_bg_blur->node);
    }
    /* without it, requests and states are dropped (the logind alert stays) */
    atomic_store(&lock_fd, eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (atomic_load(&lock_fd) >= 0)
        lock_source = wl_event_loop_add_fd(event_loop, atomic_load(&lock_fd),
                WL_EVENT_READABLE, lockfire, NULL);
    covered_timer = wl_event_loop_add_timer(event_loop, coveredtimeout, NULL);
    /* the logind thread has until then to start listening (logind_missing) */
    logind_timer = wl_event_loop_add_timer(event_loop, logindtimeout, NULL);
    wl_event_source_timer_update(logind_timer, LOGIND_GRACE_MS);
}

void
lockfini(void)
{
    if (lock_source) wl_event_source_remove(lock_source);
    if (logind_timer) wl_event_source_remove(logind_timer);
    if (covered_timer) wl_event_source_remove(covered_timer);
    lock_source = logind_timer = covered_timer = NULL;
}

void
lockshown(Monitor *m)
{
    /* the locker's own surface, not just the blur behind it */
    if (covered_pending && m->lock_surface && wlr_surface_has_buffer(m->lock_surface->surface)) {
        m->lock_shown = 1;
        lockcovered();
    }
}

void
lockresize(void)
{
    if (locked_bg_blur) {
        wlr_scene_node_set_position(&locked_bg_blur->node, sgeom.x, sgeom.y);
        wlr_scene_blur_set_size(locked_bg_blur, sgeom.width, sgeom.height);
    }
}

void
lockkeystroke(void)
{
    locked_keystrokes++;
    if (locked_bg_blur)
        AWL_wlr_scene_blur_set_shatter(locked_bg_blur, (float)locked_keystrokes/20.0);
}

void
lockerexited(pid_t pid)
{
    if (pid != locker_pid)
        return;
    locker_pid = 0;
    if (!locker_locked)
        awl_alert_set(AwlAlertLocker, "%s exited without locking", locker[0]);
}

void
lockconfigure(void)
{
    if (locked_bg_blur) {
        wlr_scene_blur_set_strength(locked_bg_blur, cfg->blur[0]);
        wlr_scene_blur_set_alpha(locked_bg_blur, cfg->blur[1]);
    }
}
