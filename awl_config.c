#include "awl_config.h"
#include "awl_config_defaults.h"
#include "awl_alert.h"
#include "awl_bar.h"
#include "awl_bus.h"
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
#include "awl_process.h"
#include "awl_wallpaper.h"
#include "tray/awl_tray.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void configapply(awl_server_t *srv);
static void pluginsdetach(awl_server_t *srv);
static void pluginsattach(awl_server_t *srv);
static void pluginrestart(void *data);
static void cfgdictdefaults(awl_server_t *srv);

#define AWL_ACTION_REGISTER(name) .name = name,
#define AWL_LAYOUT_REGISTER(name) .name = name,
const awl_actions_t awl_actions = { AWL_ACTIONS };
const awl_arranges_t awl_arranges = { AWL_ARRANGES };
#undef AWL_ACTION_REGISTER
#undef AWL_LAYOUT_REGISTER

/** The config's state (:c:member:`awl_server_t.conf`), from :c:func:`configinit` to
 * configfini() */
struct awl_conf {
    /** the library's, cfg while it is loaded */
    awl_config_t loaded;
    /** :c:func:`plugin_restart`'s, until it runs */
    struct wl_event_source *restart_source;
};

void
configinit(awl_server_t *srv)
{
    srv->conf = ecalloc(1, sizeof *srv->conf);
    /* NULL is fine: every lookup then fails with AWL_DICT_ERR_INVALID */
    if (!(srv->cfgdict = awl_dict_new()))
        awl_err_printf("out of memory for the config dictionary\n");
    cfgdictdefaults(srv);
}

void
configfini(awl_server_t *srv)
{
    awl_dict_destroy(srv->cfgdict);
    srv->cfgdict = NULL;
    /* cfg is builtinconfig()'s by now, the event loop gone */
    free(srv->conf);
    srv->conf = NULL;
}

/** the config dictionary's keys awl sets when it starts */
static void
cfgdictdefaults(awl_server_t *srv)
{
    /* "t_awl_started": when awl started, in cfgdict_stamp()'s format; the
     * library sets "plugins_loaded" alike (plugins.c) */
    char buf[32];
    if (!awl_localtime(buf))
        awl_dict_set(srv->cfgdict, "t_awl_started", &AWL_DICT_STR(buf));

    /* "desktop_dir": the folder the desktop panels list, $HOME/Desktop */
    char dir[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(dir, sizeof(dir), "%s/Desktop", home ? home : "/tmp");
    awl_dict_set(srv->cfgdict, "desktop_dir", &AWL_DICT_STR(dir));

    /* "wallpaper_dir": the folder the wallpapers come from, the default
     * cfg->wallpaper.dir, relative to $HOME unless absolute;
     * none for a NULL one, then there's no wallpaper */
    const char *wp = srv->cfg->wallpaper.dir;
    if (wp) {
        if (wp[0] == '/')
            snprintf(dir, sizeof(dir), "%s", wp);
        else
            snprintf(dir, sizeof(dir), "%s/%s", home ? home : "/tmp", wp);
        awl_dict_set(srv->cfgdict, "wallpaper_dir", &AWL_DICT_STR(dir));
    }

    /* "config_home": $XDG_CONFIG_HOME, else $HOME/.config; the spec says
     * to ignore an empty or relative $XDG_CONFIG_HOME */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/')
        snprintf(dir, sizeof(dir), "%s", xdg);
    else
        snprintf(dir, sizeof(dir), "%s/.config", home ? home : "/tmp");
    awl_dict_set(srv->cfgdict, "config_home", &AWL_DICT_STR(dir));

    /* "state_home": $XDG_STATE_HOME, else $HOME/.local/state, likewise */
    xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] == '/')
        snprintf(dir, sizeof(dir), "%s", xdg);
    else
        snprintf(dir, sizeof(dir), "%s/.local/state", home ? home : "/tmp");
    awl_dict_set(srv->cfgdict, "state_home", &AWL_DICT_STR(dir));

    /* "wallpaper_mode": the timer's mode by name (wallpaper_mode_names),
     * cfg->wallpaper.mode to begin with; awl keeps it current, and a plugin may set
     * it. plugins/wallpaper.c saves it in plugins/persistent.h's file and
     * at the start loads the saved one over this. */
    wallpapermodeinit(srv);

    /* "desktop_file_mode": what the desktop panels list, "off", "on" or
     * "all" (hidden files too); plugins_desktop_panel.c keeps it current
     * and, like "wallpaper_mode", saved */
    awl_dict_set(srv->cfgdict, "desktop_file_mode", &AWL_DICT_STR("on"));

    /* "lock_on_suspend": cfg->lock.on_suspend, for plugins/logind.c; the
     * library sets it from its own config before its logind thread starts */
    awl_dict_set(srv->cfgdict, "lock_on_suspend", &AWL_DICT_NUM(srv->cfg->lock.on_suspend));
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
wlrloginit(awl_server_t *srv)
{
    int level = srv->cfg->wlr_log_level;
    if (awl_log_level() >= awl_loglevel_vrb) /* awl -d */
        level = WLR_DEBUG;
    wlr_log_init(level < WLR_SILENT ? WLR_SILENT : level > WLR_DEBUG ? WLR_DEBUG : level, wlrlog);
}

void
updatepluginpause(awl_server_t *srv)
{
    Monitor *m;
    int visible = 0;
    wl_list_for_each(m, &srv->out.mons, link)
        visible |= m->wlr_output->enabled;
    awl_plugins_set_paused(srv->locked || !visible);
}

/* what the config's environment changed, and builtinconfig(), are
 * :c:member:`awl_process_t.config`: the environment is the process' */

static int
envconfigured(awl_server_t *srv, const char *name)
{
    size_t i;
    for (i = 0; i < srv->cfg->environment.n; i++)
        if (srv->cfg->environment.v[i].name && !strcmp(srv->cfg->environment.v[i].name, name))
            return 1;
    return 0;
}

/** what goes to systemd's and the session bus' environment too */
static const char *const envimported[] = {
    "DISPLAY", "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", NULL };

/** sets name to value (NULL: unsets it), if that changes it: other threads
 * may be reading the environment */
static void
envset(const char *name, const char *value)
{
    const char *now = getenv(name);
    const char *const *n;
    if (value ? now && !strcmp(now, value) : !now)
        return;
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
    for (n = envimported; *n; n++)
        awl_proc.config.envimportdirty |= !strcmp(*n, name);
}

void
environmentimport(void)
{
    awl_proc.config.envimportstarted = 1;
    awl_proc.config.envimportdirty = 0;
    awl_bus_import_environment(envimported);
}

/** :c:member:`cfg->environment <awl_config_t.environment>` into awl's environment */
static void
environmentapply(awl_server_t *srv)
{
    size_t i, j;
    for (i = 0; i < awl_proc.config.nenvsaved; i++)
        if (!envconfigured(srv, awl_proc.config.envsaved[i].name))
            envset(awl_proc.config.envsaved[i].name, awl_proc.config.envsaved[i].orig);
    for (i = 0; i < srv->cfg->environment.n; i++) {
        const EnvVar *e = &srv->cfg->environment.v[i];
        if (!e->name || !*e->name || strchr(e->name, '=')) {
            awl_wrn_printf("environment: no variable \"%s\"\n", e->name ? e->name : "(null)");
            continue;
        }
        for (j = 0; j < awl_proc.config.nenvsaved && strcmp(awl_proc.config.envsaved[j].name, e->name); j++)
            ;
        if (j == awl_proc.config.nenvsaved) {
            void *more = realloc(awl_proc.config.envsaved, (awl_proc.config.nenvsaved + 1) * sizeof(*awl_proc.config.envsaved));
            const char *orig = getenv(e->name);
            if (!more)
                continue;
            awl_proc.config.envsaved = more;
            awl_proc.config.envsaved[j].name = strdup(e->name);
            awl_proc.config.envsaved[j].orig = orig ? strdup(orig) : NULL;
            if (!awl_proc.config.envsaved[j].name || (orig && !awl_proc.config.envsaved[j].orig)) {
                free(awl_proc.config.envsaved[j].name);
                free(awl_proc.config.envsaved[j].orig);
                continue;
            }
            awl_proc.config.nenvsaved++;
        }
        envset(e->name, e->value);
    }
    /* grim's default folder: the desktop's, unless the config sets it */
    if (!envconfigured(srv, "GRIM_DEFAULT_DIR")) {
        awl_dict_val_t dir;
        char path[PATH_MAX] = "";
        awl_dict_lock(srv->cfgdict);
        if (awl_dict_get(srv->cfgdict, "desktop_dir", &dir) == AWL_DICT_OK
                && dir.kind == AWL_DICT_KIND_STR)
            snprintf(path, sizeof(path), "%s", dir.str);
        awl_dict_unlock(srv->cfgdict);
        if (*path)
            envset("GRIM_DEFAULT_DIR", path);
    }
    if (awl_proc.config.envimportstarted && awl_proc.config.envimportdirty)
        environmentimport();
}

/** Brings everything that was set up from the config up to
 * date with cfg, after it changed. What's only read when it's needed (key
 * and button bindings, rules, focus behaviour, which new layer surfaces get
 * blurred) needs nothing here. */
void
configapply(awl_server_t *srv)
{
    Monitor *m;

    environmentapply(srv);
    wlrloginit(srv);
    inputconfigure(srv);
    tagsconfigure(srv);
    barconfigure(srv);
    clientsconfigure(srv);
    wlr_scene_rect_set_color(srv->root_bg, (float[4])RGBA(srv->cfg->colors.root));
    wl_list_for_each(m, &srv->out.mons, link)
        wlr_scene_rect_set_color(m->fullscreen_bg, (float[4])RGBA(srv->cfg->colors.fullscreen));
    lockconfigure(srv);
    wallpaperconfigure(srv);
    desktop_configure(srv->desktop, &(desktop_style_t){
            .blur = srv->cfg->blur.popups, .radius = srv->cfg->desktop.radius,
            .blur_strength = srv->cfg->blur.strength, .blur_alpha = srv->cfg->blur.alpha });
    popupsrestyle(srv);
    awl_alert_configure(srv);

    barsdirty(srv);
}

void
configuse(awl_server_t *srv)
{
    const awl_plugin_api_t *api = awl_plugins_api();
    const Layout *old = srv->cfg->layouts.v;
    size_t n_old = srv->cfg->layouts.n;
    if (api) {
        /* the old one's lists point into the library, so only its layouts'
         * address survives this, for layoutsmoved() */
        api->config(&srv->conf->loaded);
        /* the library fills it in itself; filling it twice changes nothing */
        awl_config_fill_defaults(&srv->conf->loaded);
        srv->cfg = &srv->conf->loaded;
    } else {
        srv->cfg = builtinconfig();
    }
    layoutsmoved(srv, old, n_old);
    configapply(srv);
}

/** :c:func:`awl_plugins_reload`: the bars' widgets come from the library */
static void
pluginsdetach(awl_server_t *srv)
{
    const Layout *old = srv->cfg->layouts.v;
    size_t n_old = srv->cfg->layouts.n;

    barwidgetsclear(srv);
    /* the popups stay until the library's fini gives them back (or
     * pluginsreap() destroys them): its painters and callbacks are still
     * there until then. cfg points into the library */
    srv->cfg = builtinconfig();
    layoutsmoved(srv, old, n_old);
}

/** :c:func:`awl_plugins_reload`: the new library's config (:c:func:`configuse`),
 * the bars' widgets from it, the desktop panels redrawn */
static void
pluginsattach(awl_server_t *srv)
{
    configuse(srv);
    barwidgetsattach(srv);
    barsdirty(srv);
    desktop_reloaded(srv->desktop);
}

static void
pluginrestart(void *data)
{
    awl_server_t *srv = data;
    srv->conf->restart_source = NULL;
    /* picks up a rebuilt libawlplugins.so; with an unchanged (or broken)
     * one this is a plain restart of the plugin threads */
    awl_plugins_reload(srv, pluginsdetach, pluginsattach);

    /* the reloaded config's tray icon theme */
    awl_tray_app_set_theme(srv->cfg->tray.icon_theme);
}

/** The plugin_restart action: reloads ``libawlplugins.so`` (its tray widget
 * too), see :c:func:`awl_plugins_reload`. Deferred
 * to the event loop's next idle round, since the binding calling it is in the
 * library the reload unloads. */
void
plugin_restart(awl_server_t *srv, const Arg* arg)
{
    (void)arg;
    /* Not from here: this is usually called through a key binding, from the
     * library's code the reload unloads */
    if (!srv->conf->restart_source)
        srv->conf->restart_source = wl_event_loop_add_idle(srv->event_loop, pluginrestart, srv);
}

void
configdefaults(awl_config_t *c)
{
    *c = awl_config_defaults;
}

const awl_config_t *
builtinconfig(void)
{
    if (!awl_proc.config.builtin_filled) {
        awl_proc.config.builtin = awl_config_defaults;
        awl_config_fill_defaults(&awl_proc.config.builtin);
        awl_proc.config.builtin_filled = 1;
    }
    return &awl_proc.config.builtin;
}
