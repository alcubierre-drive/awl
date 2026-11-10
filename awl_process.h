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
    /** made for emoji: emoji presentation (see :c:func:`awl_font_get_emoji`) */
    int emoji;
    /** :c:macro:`AWL_FONT_BOLD`, AWL_FONT_ITALIC (see :c:func:`awl_font_style`) */
    int style;
    /** NULL: it didn't load (failed), or :c:func:`awl_font_retry` asked to try again */
    struct fcft_font *font;
    int failed;
    int refs;
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
        int colors;
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
        /** cfg->autostart's, the first ``nautostarted`` */
        pid_t autostarted[256];
        int nautostarted;
    } spawn;

    /** awl_redraw.c's eventfd, -1 before awl_redraw_init(); written from any
     * thread */
    _Atomic int redraw_fd;

    /** awl_lockscreen.c's lock requests and the logind thread's state: the
     * eventfd that wakes the main loop, and what it finds when it does
     * (lockfire()); requests before then coalesce into one. Any thread */
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
        int fcft_ready;
    } fonts;

    /** awl_plugin_host.c's: the one library a process loads; main thread */
    struct {
        /** what the library gets as ``awl_host``, from its template in
         * awl_plugin_host.c on the first load; ``host.srv`` is the server
         * that loaded it */
        awl_host_t host;
        /** dlopen()'s, NULL: none loaded */
        void *handle;
        /** the memfd it was loaded from, see libopen() */
        int handle_fd;
        const awl_plugin_api_t *api;
        int paused, covered;
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
        /** environmentimport() ran: from then on changes to its variables
         * go to systemd's and the bus' environment too */
        int envimportstarted;
        /** one of them changed since */
        int envimportdirty;
        /** builtinconfig()'s, filled on its first call */
        awl_config_t builtin;
        int builtin_filled;
    } config;

    /** tray/app_env.c's icon theme for the programs awl starts; main thread */
    struct {
        /** the config's icon_theme, NULL: none */
        char *theme;
        /** what :c:func:`awl_tray_app_env` made, for theme made_for (NULL:
         * nothing yet); env.env NULL: nothing to give */
        char *made_for;
        awl_tray_app_env_t env;
        char *vars[5];
    } tray;

    /** :c:enum:`awl_warned`; main thread */
    unsigned warned;
} awl_process_t;

/** the process' state, the core's only global (awl_process.c) */
extern awl_process_t awl_proc;
