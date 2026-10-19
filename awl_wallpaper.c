#include "awl_wallpaper.h"
#include "awl_background.h"
#include "awl_config.h"
#include "awl_plugin_host.h"
#include <limits.h>

static int wallpapermodedict(void);
static void wallpapermodeset(WallpaperMode mode);
static void wallpapermodesync(void);
static void wallpapernext(WallpaperMode mode);
static void wallpaperarm(void);
static int wallpapertimeout(void *data);

/** What config_plugins.h's wallpaper_config had, copied, to tell whether a
 * reload changed it; and cfgdict's "wallpaper_mode" as awl last set or saw
 * it (-1: none) */
static struct {
    unsigned int interval;
    WallpaperMode mode;
    int mode_dict;
} applied = { .mode_dict = -1 };
/** config_plugins.h's wallpaper_config.interval */
static struct wl_event_source *wallpaper_timer;
/** what the timer switches to: config_plugins.h's mode or cfgdict's "wallpaper_mode",
 * until `wallpapermode()` */
static WallpaperMode wallpaper_mode;

/** cfgdict's "wallpaper_mode" values, the timer modes' names */
static const char *const wallpaper_mode_names[WallpaperModeCount] = {
    [WallpaperNext] = "next", [WallpaperPrev] = "prev", [WallpaperRand] = "random",
};

/** cfgdict's "wallpaper_mode" as a mode, -1 if it's missing or no mode's
 * name */
static int
wallpapermodedict(void)
{
    int mode = -1, i;
    awl_dict_val_t v;

    awl_dict_lock(cfgdict);
    if (awl_dict_get(cfgdict, "wallpaper_mode", &v) == AWL_DICT_OK
            && v.kind == AWL_DICT_KIND_STR)
        for (i = 0; i < WallpaperModeCount; i++)
            if (!strcmp(v.str, wallpaper_mode_names[i]))
                mode = i;
    awl_dict_unlock(cfgdict);
    return mode;
}

/** sets the timer's mode, and cfgdict's "wallpaper_mode" to match */
static void
wallpapermodeset(WallpaperMode mode)
{
    if (mode >= WallpaperModeCount)
        return;
    wallpaper_mode = mode;
    awl_dict_set(cfgdict, "wallpaper_mode", &AWL_DICT_STR(wallpaper_mode_names[mode]));
    applied.mode_dict = mode;
}

/** takes a cfgdict "wallpaper_mode" set since: a plugin's, or the saved one
 * plugins/wallpaper.c loads at the start */
static void
wallpapermodesync(void)
{
    int mode = wallpapermodedict();
    if (mode >= 0 && mode != applied.mode_dict)
        wallpaper_mode = mode;
    applied.mode_dict = mode;
}

/** the library's thread does it (plugins/wallpaper.c), off the main thread */
void
wallpapernext(WallpaperMode mode)
{
    const awl_plugin_api_t *api = awl_plugins_api();
    if (api)
        api->wallpaper(mode);
}

void
wallpaper(const Arg *arg)
{
    WallpaperMode mode = arg->i;
    wallpapermodesync();
    if (mode == WallpaperTimerNext)
        mode = wallpaper_mode;
    else if (mode == WallpaperTimerBack)
        mode = wallpaper_mode == WallpaperNext ? WallpaperPrev
             : wallpaper_mode == WallpaperPrev ? WallpaperNext : WallpaperBack;
    wallpapernext(mode);
}

void
wallpapertake(void)
{
    const awl_plugin_api_t *api = awl_plugins_api();
    awl_image_t *img = api ? api->wallpaper_take() : NULL;
    if (img)
        background_set(img, cfg->wallpaper->fade_ms);
}

/** cycles the timer's mode by arg->i (+1: random, next, previous, random...);
 * the library tells the user */
void
wallpapermode(const Arg *arg)
{
    static const WallpaperMode order[] = { WallpaperRand, WallpaperNext, WallpaperPrev };
    const awl_plugin_api_t *api = awl_plugins_api();
    const int n = LENGTH(order);
    int i;

    wallpapermodesync();
    for (i = 0; i < n && order[i] != wallpaper_mode; i++);
    wallpapermodeset(order[((i + arg->i) % n + n) % n]);
    if (api)
        api->wallpaper_mode(wallpaper_mode);
}

/** (re)starts the countdown to the next timed wallpaper change */
static void
wallpaperarm(void)
{
    unsigned int s = MIN(cfg->wallpaper->interval, (unsigned int)INT_MAX / 1000);
    if (wallpaper_timer)
        wl_event_source_timer_update(wallpaper_timer, (int)(s * 1000));
}

static int
wallpapertimeout(void *data)
{
    wallpapermodesync();
    wallpapernext(wallpaper_mode);
    wallpaperarm();
    return 0;
}

void
wallpaperinit(void)
{
    wallpaper_timer = wl_event_loop_add_timer(event_loop, wallpapertimeout, NULL);
}

void
wallpaperfini(void)
{
    if (wallpaper_timer) {
        wl_event_source_remove(wallpaper_timer);
        wallpaper_timer = NULL;
    }
}

void
wallpapermodeinit(void)
{
    wallpapermodeset(cfg->wallpaper->mode);
    applied.mode = cfg->wallpaper->mode;
}

void
wallpaperconfigure(void)
{
    int mode;

    /* a new interval starts counting now; an unchanged one keeps counting */
    if (cfg->wallpaper->interval != applied.interval) {
        applied.interval = cfg->wallpaper->interval;
        wallpaperarm();
    }
    /* a new config_plugins.h mode replaces wallpapermode()'s, and goes to cfgdict;
     * an unchanged one doesn't. So does a new cfgdict "wallpaper_mode" (the
     * library's init() sets it before this runs), which wins when both
     * change. */
    mode = wallpapermodedict();
    if (cfg->wallpaper->mode != applied.mode) {
        applied.mode = cfg->wallpaper->mode;
        if (mode < 0 || mode == applied.mode_dict)
            wallpapermodeset(cfg->wallpaper->mode);
    }
    wallpapermodesync();
}
