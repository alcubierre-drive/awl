#include "awl_config.h"
#include "awl_config_defaults.h"
#include "awl_alert.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_desktop.h"
#include "awl_input.h"
#include "awl_layer.h"
#include "awl_layout.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_plugin_host.h"
#include "awl_popup.h"
#include "awl_wallpaper.h"
#include "tray/awl_tray.h"
#include <limits.h>

static void configapply(void);
static void pluginsdetach(void);
static void pluginsattach(void);
static void pluginrestart(void *data);
static void cfgdictdefaults(void);

#define AWL_ACTION_REGISTER(name) .name = name,
#define AWL_LAYOUT_REGISTER(name) .name = name,
const awl_actions_t awl_actions = { AWL_ACTIONS };
const awl_arranges_t awl_arranges = { AWL_ARRANGES };
#undef AWL_ACTION_REGISTER
#undef AWL_LAYOUT_REGISTER

const awl_config_t *cfg;
awl_dict_t *cfgdict;
/** the library's, cfg while it is loaded */
static awl_config_t loaded;
static struct wl_event_source *plugin_restart_source;

void
configinit(void)
{
    /* NULL is fine: every lookup then fails with AWL_DICT_ERR_INVALID */
    if (!(cfgdict = awl_dict_new()))
        awl_err_printf("out of memory for the config dictionary\n");
    cfgdictdefaults();
}

void
configfini(void)
{
    awl_dict_destroy(cfgdict);
    cfgdict = NULL;
}

static void
cfgdictdefaults(void)
{
    /* "t_awl_started": when awl started, in cfgdict_stamp()'s format; the
     * library sets "plugins_loaded" alike (plugins.c) */
    char buf[32];
    if (!awl_localtime(buf))
        awl_dict_set(cfgdict, "t_awl_started", &AWL_DICT_STR(buf));

    /* "desktop_dir": the folder the desktop panels list, $HOME/Desktop */
    char dir[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(dir, sizeof(dir), "%s/Desktop", home ? home : "/tmp");
    awl_dict_set(cfgdict, "desktop_dir", &AWL_DICT_STR(dir));

    /* "wallpaper_dir": the folder the wallpapers come from, the default
     * cfg->wallpaper.dir, relative to $HOME unless absolute;
     * none for a NULL one, then there's no wallpaper */
    const char *wp = cfg->wallpaper.dir;
    if (wp) {
        if (wp[0] == '/')
            snprintf(dir, sizeof(dir), "%s", wp);
        else
            snprintf(dir, sizeof(dir), "%s/%s", home ? home : "/tmp", wp);
        awl_dict_set(cfgdict, "wallpaper_dir", &AWL_DICT_STR(dir));
    }

    /* "config_home": $XDG_CONFIG_HOME, else $HOME/.config; the spec says
     * to ignore an empty or relative $XDG_CONFIG_HOME */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/')
        snprintf(dir, sizeof(dir), "%s", xdg);
    else
        snprintf(dir, sizeof(dir), "%s/.config", home ? home : "/tmp");
    awl_dict_set(cfgdict, "config_home", &AWL_DICT_STR(dir));

    /* "state_home": $XDG_STATE_HOME, else $HOME/.local/state, likewise */
    xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] == '/')
        snprintf(dir, sizeof(dir), "%s", xdg);
    else
        snprintf(dir, sizeof(dir), "%s/.local/state", home ? home : "/tmp");
    awl_dict_set(cfgdict, "state_home", &AWL_DICT_STR(dir));

    /* "wallpaper_mode": the timer's mode by name (wallpaper_mode_names),
     * cfg->wallpaper.mode to begin with; awl keeps it current, and a plugin may set
     * it. plugins/wallpaper.c saves it in plugins/persistent.h's file and
     * at the start loads the saved one over this. */
    wallpapermodeinit();

    /* "desktop_file_mode": what the desktop panels list, "off", "on" or
     * "all" (hidden files too); plugins_desktop_panel.c keeps it current
     * and, like "wallpaper_mode", saved */
    awl_dict_set(cfgdict, "desktop_file_mode", &AWL_DICT_STR("on"));

    /* "lock_on_suspend": cfg->lock.on_suspend, for plugins/logind.c; the
     * library sets it from its own config before its logind thread starts */
    awl_dict_set(cfgdict, "lock_on_suspend", &AWL_DICT_NUM(cfg->lock.on_suspend));
}

/* wlroots' (and scenefx') messages; wlroots drops those above its level */
static void
wlrlog(enum wlr_log_importance importance, const char *fmt, va_list ap)
{
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    awl_dbg_printf(importance == WLR_ERROR ? awl_loglevel_err
            : importance == WLR_INFO ? awl_loglevel_log : awl_loglevel_vrb, "wlr: %s\n", msg);
}

void
wlrloginit(void)
{
    int level = cfg->wlr_log_level;
    if (awl_log_level() >= awl_loglevel_vrb) /* awl -d */
        level = WLR_DEBUG;
    wlr_log_init(level < WLR_SILENT ? WLR_SILENT : level > WLR_DEBUG ? WLR_DEBUG : level, wlrlog);
}

void
updatepluginpause(void)
{
    Monitor *m;
    int visible = 0;
    wl_list_for_each(m, &mons, link)
        visible |= m->wlr_output->enabled;
    awl_plugins_set_paused(locked || !visible);
}

/** Brings everything that was set up from the config up to
 * date with cfg, after it changed. What's only read when it's needed (key
 * and button bindings, rules, focus behaviour, which new layer surfaces get
 * blurred) needs nothing here. */
void
configapply(void)
{
    Monitor *m;

    wlrloginit();
    inputconfigure();
    tagsconfigure();
    barconfigure();
    clientsconfigure();
    wlr_scene_rect_set_color(root_bg, (float[4])RGBA(cfg->colors.root));
    wl_list_for_each(m, &mons, link)
        wlr_scene_rect_set_color(m->fullscreen_bg, (float[4])RGBA(cfg->colors.fullscreen));
    lockconfigure();
    wallpaperconfigure();
    desktop_configure(&(desktop_style_t){
            .blur = cfg->blur.popups, .radius = cfg->desktop.radius,
            .blur_strength = cfg->blur.strength, .blur_alpha = cfg->blur.alpha });
    popupsrestyle();
    awl_alert_configure();

    barsdirty();
}

void
configuse(void)
{
    const awl_plugin_api_t *api = awl_plugins_api();
    const Layout *old = cfg->layouts.v;
    size_t n_old = cfg->layouts.n;
    if (api) {
        /* the old one's lists point into the library, so only its layouts'
         * address survives this, for layoutsmoved() */
        api->config(&loaded);
        cfg = &loaded;
    } else {
        cfg = builtinconfig();
    }
    layoutsmoved(old, n_old);
    configapply();
}

/** `awl_plugins_reload()`: the bars' widgets come from the library */
static void
pluginsdetach(void)
{
    const Layout *old = cfg->layouts.v;
    size_t n_old = cfg->layouts.n;

    barwidgetsclear();
    /* the popups stay until the library's fini gives them back (or
     * pluginsreap() destroys them): its painters and callbacks are still
     * there until then. cfg points into the library */
    cfg = builtinconfig();
    layoutsmoved(old, n_old);
}

static void
pluginsattach(void)
{
    configuse();
    barwidgetsattach();
    barsdirty();
    desktop_reloaded();
}

static void
pluginrestart(void *data)
{
    plugin_restart_source = NULL;
    /* picks up a rebuilt libawlplugins.so; with an unchanged (or broken)
     * one this is a plain restart of the plugin threads */
    awl_plugins_reload(pluginsdetach, pluginsattach);

    /* the reloaded config's tray icon theme */
    awl_tray_app_set_theme(cfg->tray.icon_theme);
}

/** The plugin_restart action: reloads ``libawlplugins.so`` (its tray widget
 * too), see `awl_plugins_reload()`. Deferred
 * to the event loop's next idle round, since the binding calling it is in the
 * library the reload unloads. */
void
plugin_restart(const Arg* arg)
{
    (void)arg;
    /* Not from here: this is usually called through a key binding, from the
     * library's code the reload unloads */
    if (!plugin_restart_source)
        plugin_restart_source = wl_event_loop_add_idle(event_loop, pluginrestart, NULL);
}

void
configdefaults(awl_config_t *c)
{
    *c = awl_config_defaults;
}

const awl_config_t *
builtinconfig(void)
{
    return &awl_config_defaults;
}
