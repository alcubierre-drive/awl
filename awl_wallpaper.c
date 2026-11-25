#include "awl_wallpaper.h"
#include "awl_background.h"
#include "awl_config.h"
#include "awl_plugin_host.h"
#include <limits.h>

static int wallpaper_mode_dict( awl_server_t *srv );
static void wallpaper_mode_set( awl_server_t *srv, WallpaperMode mode );
static void wallpaper_mode_sync( awl_server_t *srv );
static void wallpaper_next( awl_process_t *proc, WallpaperMode mode, int by_timer );
static void wallpaper_arm( awl_server_t *srv );
static int wallpaper_timeout( void *data );

/** The wallpaper timer's state (:c:member:`awl_server_t.wallpaper`), from
 * :c:func:`wallpaper_init` to wallpaper_fini() */
struct awl_wallpaper {
    /** What cfg->wallpaper had, copied, to tell whether a
     * reload changed it; and cfg_dict's "wallpaper_mode" as awl last set or saw
     * it (-1: none) */
    struct {
        unsigned int interval;
        WallpaperMode mode;
        int mode_dict;
    } applied;
    /** :c:member:`cfg->wallpaper.interval <awl_wallpaper_config_t.interval>` */
    struct wl_event_source *timer;
    /** what the timer switches to: :c:member:`cfg->wallpaper.mode <awl_wallpaper_config_t.mode>` or cfg_dict's "wallpaper_mode",
     * until :c:func:`wallpaper_mode` */
    WallpaperMode mode;
};

/** cfg_dict's "wallpaper_mode" values, the timer modes' names */
static const char *const wallpaper_mode_names[WallpaperModeCount] = {
    [WallpaperNext] = "next", [WallpaperPrev] = "prev", [WallpaperRand] = "random",
};

/** cfg_dict's "wallpaper_mode" as a mode, -1 if it's missing or no mode's
 * name */
static int wallpaper_mode_dict( awl_server_t *srv ) {
    int mode = -1;
    char name[16];
    if (awl_dict_strncpy(srv->cfg_dict, "wallpaper_mode", name, sizeof(name)) == AWL_DICT_OK)
        for (int i = 0; i < WallpaperModeCount; i++)
            if (!strcmp(name, wallpaper_mode_names[i]))
                mode = i;
    return mode;
}

/** sets the timer's mode, and cfg_dict's "wallpaper_mode" to match */
static void wallpaper_mode_set( awl_server_t *srv, WallpaperMode mode ) {
    if (mode >= WallpaperModeCount)
        return;
    srv->wallpaper->mode = mode;
    awl_dict_set(srv->cfg_dict, "wallpaper_mode", &AWL_DICT_STR(wallpaper_mode_names[mode]));
    srv->wallpaper->applied.mode_dict = mode;
}

/** takes a cfg_dict "wallpaper_mode" set since: a plugin's, or the saved one
 * plugins/wallpaper.c loads at the start */
static void wallpaper_mode_sync( awl_server_t *srv ) {
    int mode = wallpaper_mode_dict(srv);
    if (mode >= 0 && mode != srv->wallpaper->applied.mode_dict)
        srv->wallpaper->mode = mode;
    srv->wallpaper->applied.mode_dict = mode;
}

/** the library's thread does it (plugins/wallpaper.c), off the main thread;
 * proc's library */
void wallpaper_next( awl_process_t *proc, WallpaperMode mode, int by_timer ) {
    const awl_plugin_api_t *api = awl_plugins_api(proc);
    if (api)
        api->wallpaper(mode, by_timer);
}

/** the wallpaper action: switches to the wallpaper :c:member:`arg->i <Arg.i>` says (a
 * :c:enum:`WallpaperMode`; the timer's next or back: as the timer's mode
 * goes) */
void wallpaper( awl_server_t *srv, const Arg *arg ) {
    WallpaperMode mode = arg->i;
    wallpaper_mode_sync(srv);
    if (mode == WallpaperTimerNext)
        mode = srv->wallpaper->mode;
    else if (mode == WallpaperTimerBack)
        mode = srv->wallpaper->mode == WallpaperNext ? WallpaperPrev
             : srv->wallpaper->mode == WallpaperPrev ? WallpaperNext : WallpaperBack;
    wallpaper_next(srv->proc, mode, 0);
}

void wallpaper_take( awl_server_t *srv ) {
    const awl_plugin_api_t *api = awl_plugins_api(srv->proc);
    awl_image_t *img = api ? api->wallpaper_take() : NULL;
    if (img)
        background_set(srv->background, img, srv->cfg->wallpaper.fade_ms);
}

/** cycles the timer's mode by :c:member:`arg->i <Arg.i>` (+1: random, next, previous, random...);
 * the library tells the user */
void wallpaper_mode( awl_server_t *srv, const Arg *arg ) {
    static const WallpaperMode order[] = { WallpaperRand, WallpaperNext, WallpaperPrev };
    const awl_plugin_api_t *api = awl_plugins_api(srv->proc);
    const int n = LENGTH(order);
    int i;

    wallpaper_mode_sync(srv);
    for (i = 0; i < n && order[i] != srv->wallpaper->mode; i++);
    wallpaper_mode_set(srv, order[((i + arg->i) % n + n) % n]);
    if (api)
        api->wallpaper_mode(srv->wallpaper->mode);
}

/** (re)starts the countdown to the next timed wallpaper change */
static void wallpaper_arm( awl_server_t *srv ) {
    unsigned int s = MIN(srv->cfg->wallpaper.interval, (unsigned int)INT_MAX / 1000);
    if (srv->wallpaper->timer)
        wl_event_source_timer_update(srv->wallpaper->timer, (int)(s * 1000));
}

static int wallpaper_timeout( void *data ) {
    awl_server_t *srv = data;
    wallpaper_mode_sync(srv);
    wallpaper_next(srv->proc, srv->wallpaper->mode, 1);
    wallpaper_arm(srv);
    return 0;
}

void wallpaper_init( awl_server_t *srv ) {
    srv->wallpaper = calloc_or_die(1, sizeof *srv->wallpaper);
    srv->wallpaper->applied.mode_dict = -1;
    srv->wallpaper->timer = wl_event_loop_add_timer(srv->event_loop, wallpaper_timeout, srv);
}

void wallpaper_fini( awl_server_t *srv ) {
    if (srv->wallpaper->timer)
        wl_event_source_remove(srv->wallpaper->timer);
    free(srv->wallpaper);
    srv->wallpaper = NULL;
}

void wallpaper_mode_init( awl_server_t *srv ) {
    wallpaper_mode_set(srv, srv->cfg->wallpaper.mode);
    srv->wallpaper->applied.mode = srv->cfg->wallpaper.mode;
}

void wallpaper_configure( awl_server_t *srv ) {
    /* a new interval starts counting now; an unchanged one keeps counting */
    if (srv->cfg->wallpaper.interval != srv->wallpaper->applied.interval) {
        srv->wallpaper->applied.interval = srv->cfg->wallpaper.interval;
        wallpaper_arm(srv);
    }
    /* a new plugins_config.c mode replaces wallpaper_mode()'s, and goes to cfg_dict;
     * an unchanged one doesn't. So does a new cfg_dict "wallpaper_mode" (the
     * library's init() sets it before this runs), which wins when both
     * change. */
    int mode = wallpaper_mode_dict(srv);
    if (srv->cfg->wallpaper.mode != srv->wallpaper->applied.mode) {
        srv->wallpaper->applied.mode = srv->cfg->wallpaper.mode;
        if (mode < 0 || mode == srv->wallpaper->applied.mode_dict)
            wallpaper_mode_set(srv, srv->cfg->wallpaper.mode);
    }
    wallpaper_mode_sync(srv);
}
