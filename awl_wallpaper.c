#include "awl_wallpaper.h"
#include "awl_background.h"
#include "awl_config.h"
#include "awl_plugin_host.h"
#include <limits.h>

static int wallpapermodedict(awl_server_t *srv);
static void wallpapermodeset(awl_server_t *srv, WallpaperMode mode);
static void wallpapermodesync(awl_server_t *srv);
static void wallpapernext(WallpaperMode mode, int by_timer);
static void wallpaperarm(awl_server_t *srv);
static int wallpapertimeout(void *data);

/** The wallpaper timer's state (:c:member:`awl_server_t.wallpaper`), from
 * wallpaperinit() to wallpaperfini() */
struct awl_wallpaper {
    /** What cfg->wallpaper had, copied, to tell whether a
     * reload changed it; and cfgdict's "wallpaper_mode" as awl last set or saw
     * it (-1: none) */
    struct {
        unsigned int interval;
        WallpaperMode mode;
        int mode_dict;
    } applied;
    /** cfg->wallpaper.interval */
    struct wl_event_source *timer;
    /** what the timer switches to: cfg->wallpaper.mode or cfgdict's "wallpaper_mode",
     * until :c:func:`wallpapermode` */
    WallpaperMode mode;
};

/** cfgdict's "wallpaper_mode" values, the timer modes' names */
static const char *const wallpaper_mode_names[WallpaperModeCount] = {
    [WallpaperNext] = "next", [WallpaperPrev] = "prev", [WallpaperRand] = "random",
};

/** cfgdict's "wallpaper_mode" as a mode, -1 if it's missing or no mode's
 * name */
static int
wallpapermodedict(awl_server_t *srv)
{
    int mode = -1, i;
    awl_dict_val_t v;

    awl_dict_lock(srv->cfgdict);
    if (awl_dict_get(srv->cfgdict, "wallpaper_mode", &v) == AWL_DICT_OK
            && v.kind == AWL_DICT_KIND_STR)
        for (i = 0; i < WallpaperModeCount; i++)
            if (!strcmp(v.str, wallpaper_mode_names[i]))
                mode = i;
    awl_dict_unlock(srv->cfgdict);
    return mode;
}

/** sets the timer's mode, and cfgdict's "wallpaper_mode" to match */
static void
wallpapermodeset(awl_server_t *srv, WallpaperMode mode)
{
    if (mode >= WallpaperModeCount)
        return;
    srv->wallpaper->mode = mode;
    awl_dict_set(srv->cfgdict, "wallpaper_mode", &AWL_DICT_STR(wallpaper_mode_names[mode]));
    srv->wallpaper->applied.mode_dict = mode;
}

/** takes a cfgdict "wallpaper_mode" set since: a plugin's, or the saved one
 * plugins/wallpaper.c loads at the start */
static void
wallpapermodesync(awl_server_t *srv)
{
    int mode = wallpapermodedict(srv);
    if (mode >= 0 && mode != srv->wallpaper->applied.mode_dict)
        srv->wallpaper->mode = mode;
    srv->wallpaper->applied.mode_dict = mode;
}

/** the library's thread does it (plugins/wallpaper.c), off the main thread */
void
wallpapernext(WallpaperMode mode, int by_timer)
{
    const awl_plugin_api_t *api = awl_plugins_api();
    if (api)
        api->wallpaper(mode, by_timer);
}

/** the wallpaper action: switches to the wallpaper arg->i says (a
 * :c:enum:`WallpaperMode`; the timer's next or back: as the timer's mode
 * goes) */
void
wallpaper(awl_server_t *srv, const Arg *arg)
{
    WallpaperMode mode = arg->i;
    wallpapermodesync(srv);
    if (mode == WallpaperTimerNext)
        mode = srv->wallpaper->mode;
    else if (mode == WallpaperTimerBack)
        mode = srv->wallpaper->mode == WallpaperNext ? WallpaperPrev
             : srv->wallpaper->mode == WallpaperPrev ? WallpaperNext : WallpaperBack;
    wallpapernext(mode, 0);
}

void
wallpapertake(awl_server_t *srv)
{
    const awl_plugin_api_t *api = awl_plugins_api();
    awl_image_t *img = api ? api->wallpaper_take() : NULL;
    if (img)
        background_set(srv->background, img, srv->cfg->wallpaper.fade_ms);
}

/** cycles the timer's mode by arg->i (+1: random, next, previous, random...);
 * the library tells the user */
void
wallpapermode(awl_server_t *srv, const Arg *arg)
{
    static const WallpaperMode order[] = { WallpaperRand, WallpaperNext, WallpaperPrev };
    const awl_plugin_api_t *api = awl_plugins_api();
    const int n = LENGTH(order);
    int i;

    wallpapermodesync(srv);
    for (i = 0; i < n && order[i] != srv->wallpaper->mode; i++);
    wallpapermodeset(srv, order[((i + arg->i) % n + n) % n]);
    if (api)
        api->wallpaper_mode(srv->wallpaper->mode);
}

/** (re)starts the countdown to the next timed wallpaper change */
static void
wallpaperarm(awl_server_t *srv)
{
    unsigned int s = MIN(srv->cfg->wallpaper.interval, (unsigned int)INT_MAX / 1000);
    if (srv->wallpaper->timer)
        wl_event_source_timer_update(srv->wallpaper->timer, (int)(s * 1000));
}

static int
wallpapertimeout(void *data)
{
    awl_server_t *srv = data;
    wallpapermodesync(srv);
    wallpapernext(srv->wallpaper->mode, 1);
    wallpaperarm(srv);
    return 0;
}

void
wallpaperinit(awl_server_t *srv)
{
    srv->wallpaper = ecalloc(1, sizeof *srv->wallpaper);
    srv->wallpaper->applied.mode_dict = -1;
    srv->wallpaper->timer = wl_event_loop_add_timer(srv->event_loop, wallpapertimeout, srv);
}

void
wallpaperfini(awl_server_t *srv)
{
    if (srv->wallpaper->timer)
        wl_event_source_remove(srv->wallpaper->timer);
    free(srv->wallpaper);
    srv->wallpaper = NULL;
}

void
wallpapermodeinit(awl_server_t *srv)
{
    wallpapermodeset(srv, srv->cfg->wallpaper.mode);
    srv->wallpaper->applied.mode = srv->cfg->wallpaper.mode;
}

void
wallpaperconfigure(awl_server_t *srv)
{
    int mode;

    /* a new interval starts counting now; an unchanged one keeps counting */
    if (srv->cfg->wallpaper.interval != srv->wallpaper->applied.interval) {
        srv->wallpaper->applied.interval = srv->cfg->wallpaper.interval;
        wallpaperarm(srv);
    }
    /* a new plugins_config.c mode replaces wallpapermode()'s, and goes to cfgdict;
     * an unchanged one doesn't. So does a new cfgdict "wallpaper_mode" (the
     * library's init() sets it before this runs), which wins when both
     * change. */
    mode = wallpapermodedict(srv);
    if (srv->cfg->wallpaper.mode != srv->wallpaper->applied.mode) {
        srv->wallpaper->applied.mode = srv->cfg->wallpaper.mode;
        if (mode < 0 || mode == srv->wallpaper->applied.mode_dict)
            wallpapermodeset(srv, srv->cfg->wallpaper.mode);
    }
    wallpapermodesync(srv);
}
