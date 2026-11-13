#pragma once

/**
 * The process' state, :c:var:`awl_proc`: what belongs to awl the process,
 * not to the compositor (:c:type:`awl_server_t`, which ``main()`` makes and
 * passes in). It is the core's only global, and the place to look for what
 * a signal handler or another thread reaches without a parameter. Each
 * member says who touches it and from which thread; nothing in it points
 * into the server.
 *
 * Only the modules named below include this header, and each reads only its
 * own member. Everyone else goes through their functions.
 */

#include "awl_config.h"
#include "awl_draw.h"
#include "awl_log.h"
#include "tray/awl_tray.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>

/** One font of awl_draw.c's registry (:c:member:`awl_process_t.fonts`) */
typedef struct {
    char spec[AWL_FONT_SPEC_MAX];
    /** "dpi=...", then the style's ":weight=bold:slant=italic" */
    char attrs[48];
    /** :c:macro:`AWL_FONT_BOLD`, :c:macro:`AWL_FONT_ITALIC` (see :c:func:`awl_font_style`) */
    int style;
    int refs;
    /** made for emoji: emoji presentation (see :c:func:`awl_font_get_emoji`) */
    bool emoji:1;
    /** it didn't load */
    bool failed:1;
    /** NULL: it didn't load (failed), or :c:func:`awl_font_retry` asked to try again */
    struct fcft_font *font;
} awl_font_slot_t;

/** :c:member:`awl_process_t.warned`'s bits: what was logged once already */
enum awl_warned {
    /** every font slot is in use (awl_draw.c) */
    AwlWarnedFontSlots = 1 << 0,
    /** not even the fallback font loads (awl_draw.c) */
    AwlWarnedFontNone = 1 << 1,
};

/** true the first time for bit, an :c:enum:`awl_warned`, and false from then
 * on: ``if (AWL_ONCE(AwlWarnedFontSlots)) awl_err_printf(...)`` */
#define AWL_ONCE(bit) (!(awl_proc.warned & (bit)) && (awl_proc.warned |= (bit)))

/** The type of :c:var:`awl_proc` */
typedef struct {
    /** awl_log.c's: any thread. ``level`` and ``colors`` are set before the
     * threads start; ``mtx`` guards ``fd``'s writes and ``size`` */
    struct {
        awl_loglevel_t level;
        /** stderr is a terminal */
        bool colors;
        /** the log file, -1: none */
        int fd;
        /** about how much is in it, for its size cap */
        size_t size;
        pthread_mutex_t mtx;
    } log;

    /** awl_spawn.c's children, which it stops on exit; main thread */
    struct {
        /** the startup command (``-s``), -1: none */
        pid_t startup;
        /** :c:member:`cfg->autostart <awl_config_t.autostart>`'s, the first :c:member:`~awl_process_t.spawn.nautostarted` */
        pid_t autostarted[256];
        /** how many autostarted are set */
        int nautostarted;
    } spawn;

    /** awl_redraw.c's eventfd, -1 before :c:func:`awl_redraw_init`; written from any
     * thread */
    _Atomic int redraw_fd;

    /** awl_lockscreen.c's lock requests and the logind thread's state: the
     * eventfd that wakes the main loop, and what it finds when it does
     * (:c:func:`lockfire`); requests before then coalesce into one. Any thread */
    struct {
        _Atomic int fd;
        atomic_int requested;
        atomic_uint logind_flags;
    } lock;

    /** awl_draw.c's font registry, a ring of :c:macro:`AWL_FONT_SLOTS`
     * entries, oldest at ``head``; main thread (the library draws on it
     * through the host) */
    struct {
        awl_font_slot_t ring[AWL_FONT_SLOTS];
        unsigned head, count;
        /** fcft_init() ran */
        bool fcft_ready;
    } fonts;

    /** awl_plugin_host.c's: the one library a process loads; main thread */
    struct {
        /** what the library gets as ``awl_host``, from its template in
         * awl_plugin_host.c on the first load; ``host.srv`` is the server
         * that loaded it */
        awl_host_t host;
        /** dlopen()'s, NULL: none loaded */
        void *handle;
        /** the memfd it was loaded from, see :c:func:`libopen` */
        int handle_fd;
        const awl_plugin_api_t *api;
        bool paused:1, covered:1;
        /** :c:member:`awl_host_t.timer_add() <awl_host_t.timer_add>`'s
         * timers, reaped after the library stops */
        struct wl_list timers;
    } plugins;

    /** awl_config.c's: the process' environment and the built-in config;
     * main thread */
    struct {
        /** each variable the config's environment changed, with its value
         * from before (NULL: unset), to go back to once the config drops
         * it; kept for awl's life */
        struct { char *name, *orig; } *envsaved;
        size_t nenvsaved;
        /** :c:func:`environmentimport` ran: from then on changes to its variables
         * go to systemd's and the bus' environment too */
        bool envimportstarted:1;
        /** one of them changed since */
        bool envimportdirty:1;
        /** builtin is filled */
        bool builtin_filled:1;
        /** :c:func:`builtinconfig`'s, filled on its first call */
        awl_config_t builtin;
    } config;

    /** tray/app_env.c's icon theme for the programs awl starts; main thread */
    struct {
        /** the config's icon_theme, NULL: none */
        char *theme;
        /** the theme env was made for, NULL: nothing yet */
        char *made_for;
        /** what :c:func:`awl_tray_app_env` made; :c:member:`env.env <awl_tray_app_env_t.env>` NULL: nothing to give */
        awl_tray_app_env_t env;
        char *vars[5];
    } tray;

    /** :c:enum:`awl_warned`; main thread */
    unsigned warned;
} awl_process_t;

/** the process' state, the core's only global (awl_process.c) */
extern awl_process_t awl_proc;
