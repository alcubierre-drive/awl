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
#include "awl_screensaver.h"
#include "awl_wallpaper.h"
#include "awl_trayapp_env.h"
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void config_apply( awl_server_t *srv );
static void plugins_detach( awl_server_t *srv );
static void plugins_attach( awl_server_t *srv );
static void plugin_restart_idle( void *data );
static void cfg_dict_defaults( awl_server_t *srv );

#define AWL_ACTION_REGISTER(name) .name = name,
#define AWL_LAYOUT_REGISTER(name) .name = name,
const awl_actions_t awl_actions = { AWL_ACTIONS };
const awl_arranges_t awl_arranges = { AWL_ARRANGES };
#undef AWL_ACTION_REGISTER
#undef AWL_LAYOUT_REGISTER

/** The config's state (:c:member:`awl_server_t.conf`), from :c:func:`config_init` to
 * config_fini() */
struct awl_conf {
    /** the library's, cfg while it is loaded */
    awl_config_t loaded;
    /** :c:func:`plugin_restart`'s, until it runs */
    struct wl_event_source *restart_source;
};

void config_init( awl_server_t *srv ) {
    srv->conf = calloc_or_die(1, sizeof *srv->conf);
    /* NULL is fine: every lookup then fails with AWL_DICT_ERR_INVALID */
    if (!(srv->cfg_dict = awl_dict_new()))
        awl_err_printf("out of memory for the config dictionary\n");
    cfg_dict_defaults(srv);
}

void config_fini( awl_server_t *srv ) {
    awl_dict_destroy(srv->cfg_dict);
    srv->cfg_dict = NULL;
    /* cfg is builtin_config()'s by now, the event loop gone */
    free(srv->conf);
    srv->conf = NULL;
}

int cfg_home( awl_server_t *srv, char *out, size_t n ) {
    return awl_dict_strncpy(srv->cfg_dict, "home", out, n) == AWL_DICT_OK && *out ? 0 : -1;
}

/** the config dictionary's keys awl sets when it starts */
static void cfg_dict_defaults( awl_server_t *srv ) {
    /* "t_awl_started": when awl started, in cfg_dict_stamp()'s format; the
     * library sets "plugins_loaded" alike (plugins.c) */
    char buf[32];
    if (!awl_localtime(buf))
        awl_dict_set(srv->cfg_dict, "t_awl_started", &AWL_DICT_STR(buf));

    /* "home": the user's home folder, $HOME, else the password database's;
     * none without either. awl and the library take it from here, not
     * from the environment. */
    const char *home = getenv("HOME");
    if (!home || !*home) {
        const struct passwd *pw = getpwuid(getuid());
        home = pw && pw->pw_dir && *pw->pw_dir ? pw->pw_dir : NULL;
    }
    if (home)
        awl_dict_set(srv->cfg_dict, "home", &AWL_DICT_STR(home));

    /* "desktop_dir": the folder the desktop panels list, $HOME/Desktop */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/Desktop", home ? home : "/tmp");
    awl_dict_set(srv->cfg_dict, "desktop_dir", &AWL_DICT_STR(dir));

    /* "wallpaper_dir": the folder the wallpapers come from, the default
     * cfg->wallpaper.dir, relative to $HOME unless absolute;
     * none for a NULL one, then there's no wallpaper */
    const char *wp = srv->cfg->wallpaper.dir;
    if (wp) {
        if (wp[0] == '/')
            snprintf(dir, sizeof(dir), "%s", wp);
        else
            snprintf(dir, sizeof(dir), "%s/%s", home ? home : "/tmp", wp);
        awl_dict_set(srv->cfg_dict, "wallpaper_dir", &AWL_DICT_STR(dir));
    }

    /* "config_home": $XDG_CONFIG_HOME, else $HOME/.config; the spec says
     * to ignore an empty or relative $XDG_CONFIG_HOME */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/')
        snprintf(dir, sizeof(dir), "%s", xdg);
    else
        snprintf(dir, sizeof(dir), "%s/.config", home ? home : "/tmp");
    awl_dict_set(srv->cfg_dict, "config_home", &AWL_DICT_STR(dir));

    /* "state_home": $XDG_STATE_HOME, else $HOME/.local/state, likewise */
    xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] == '/')
        snprintf(dir, sizeof(dir), "%s", xdg);
    else
        snprintf(dir, sizeof(dir), "%s/.local/state", home ? home : "/tmp");
    awl_dict_set(srv->cfg_dict, "state_home", &AWL_DICT_STR(dir));

    /* "wallpaper_mode": the timer's mode by name (wallpaper_mode_names),
     * cfg->wallpaper.mode to begin with; awl keeps it current, and a plugin may set
     * it. plugins/wallpaper.c saves it in plugins/persistent.h's file and
     * at the start loads the saved one over this. */
    wallpaper_mode_init(srv);

    /* "desktop_file_mode": what the desktop panels list, "off", "on" or
     * "all" (hidden files too); plugins_desktop_panel.c keeps it current
     * and, like "wallpaper_mode", saved */
    awl_dict_set(srv->cfg_dict, "desktop_file_mode", &AWL_DICT_STR("on"));

    /* "lock_on_suspend": cfg->lock.on_suspend, for plugins/logind.c; the
     * library sets it from its own config before its logind thread starts */
    awl_dict_set(srv->cfg_dict, "lock_on_suspend", &AWL_DICT_NUM(srv->cfg->lock.on_suspend));
}

/* wlroots' (and scenefx') messages; wlroots drops those above its level */
static void wlr_log_sink( enum wlr_log_importance importance, const char *fmt, va_list ap ) {
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    awl_dbg_printf(importance == WLR_ERROR ? awl_loglevel_err
            : importance == WLR_INFO ? awl_loglevel_log : awl_loglevel_vrb, "wlr: %s\n", msg);
}

void wlr_log_setup( awl_server_t *srv ) {
    int level = srv->cfg->wlr_log_level;
    if (awl_log_level(srv->proc) >= awl_loglevel_vrb) /* awl -d */
        level = WLR_DEBUG;
    wlr_log_init(level < WLR_SILENT ? WLR_SILENT : level > WLR_DEBUG ? WLR_DEBUG : level, wlr_log_sink);
}

void update_plugin_pause( awl_server_t *srv ) {
    int visible = 0;
    Monitor *m;
    FOR_EACH_MON(m, srv)
        visible |= m->wlr_output->enabled;
    awl_plugins_set_paused(srv->proc, srv->locked || !visible);
}

/* what the config's environment changed, and builtin_config(), are
 * :c:member:`awl_process_t.config`: the environment is the process' */

static int env_configured( const awl_config_t *cfg, const char *name ) {
    for (size_t i = 0; i < cfg->environment.n; i++)
        if (cfg->environment.v[i].name && !strcmp(cfg->environment.v[i].name, name))
            return 1;
    return 0;
}

/** what goes to systemd's and the session bus' environment too */
static const char *const env_imported[] = {
    "DISPLAY", "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", NULL };

/** sets name to value (NULL: unsets it), if that changes it: other threads
 * may be reading the environment */
static void env_set( awl_process_t *proc, const char *name, const char *value ) {
    const char *now = getenv(name);
    const char *const *n;
    if (value ? now && !strcmp(now, value) : !now)
        return;
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
    for (n = env_imported; *n; n++)
        proc->config.env_import_dirty |= !strcmp(*n, name);
}

void environment_import( awl_process_t *proc ) {
    proc->config.env_import_started = 1;
    proc->config.env_import_dirty = 0;
    awl_bus_import_environment(env_imported);
}

void session_start( awl_process_t *proc, bool systemd_session ) {
    if (!systemd_session || proc->config.session_started) return;
    proc->config.session_started = 1;
    awl_bus_session(1);
}

void session_stop( awl_process_t *proc ) {
    if (!proc->config.session_started) return;
    proc->config.session_started = 0;
    awl_bus_session(0);
}

/** :c:member:`cfg->environment <awl_config_t.environment>` into awl's environment */
static void environment_apply( awl_server_t *srv ) {
    awl_process_t *proc = srv->proc;
    for (size_t i = 0; i < proc->config.n_env_saved; i++)
        if (!env_configured(srv->cfg, proc->config.env_saved[i].name))
            env_set(proc, proc->config.env_saved[i].name, proc->config.env_saved[i].orig);
    for (size_t i = 0; i < srv->cfg->environment.n; i++) {
        const EnvVar *e = &srv->cfg->environment.v[i];
        if (!e->name || !*e->name || strchr(e->name, '=')) {
            awl_wrn_printf("environment: no variable \"%s\"\n", e->name ? e->name : "(null)");
            continue;
        }
        size_t j = 0;
        while (j < proc->config.n_env_saved && strcmp(proc->config.env_saved[j].name, e->name))
            j++;
        if (j == proc->config.n_env_saved) {
            void *more = realloc(proc->config.env_saved, (proc->config.n_env_saved + 1) * sizeof(*proc->config.env_saved));
            const char *orig = getenv(e->name);
            if (!more)
                continue;
            proc->config.env_saved = more;
            proc->config.env_saved[j].name = strdup(e->name);
            proc->config.env_saved[j].orig = orig ? strdup(orig) : NULL;
            if (!proc->config.env_saved[j].name || (orig && !proc->config.env_saved[j].orig)) {
                free(proc->config.env_saved[j].name);
                free(proc->config.env_saved[j].orig);
                continue;
            }
            proc->config.n_env_saved++;
        }
        env_set(proc, e->name, e->value);
    }
    if (proc->config.env_import_started && proc->config.env_import_dirty)
        environment_import(proc);
}

/** Brings everything that was set up from the config up to
 * date with cfg, after it changed. What's only read when it's needed (key
 * and button bindings, rules, focus behaviour, which new layer surfaces get
 * blurred) needs nothing here. */
void config_apply( awl_server_t *srv ) {
    Monitor *m;

    environment_apply(srv);
    wlr_log_setup(srv);
    input_configure(srv);
    awl_screensaver_configure(srv);
    tags_configure(srv);
    bar_configure(srv);
    clients_configure(srv);
    wlr_scene_rect_set_color(srv->root_bg, (float[4])RGBA(srv->cfg->colors.root));
    FOR_EACH_MON(m, srv)
        wlr_scene_rect_set_color(m->fullscreen_bg, (float[4])RGBA(srv->cfg->colors.fullscreen));
    lock_configure(srv);
    wallpaper_configure(srv);
    desktop_configure(srv->desktop, &(desktop_style_t){
            .blur = srv->cfg->blur.popups, .radius = srv->cfg->desktop.radius,
            .blur_strength = srv->cfg->blur.strength, .blur_alpha = srv->cfg->blur.alpha });
    popups_restyle(srv);
    awl_alert_configure(srv);

    bars_dirty(srv);
}

void config_use( awl_server_t *srv ) {
    const awl_plugin_api_t *api = awl_plugins_api(srv->proc);
    const Layout *old = srv->cfg->layouts.v;
    size_t n_old = srv->cfg->layouts.n;
    if (api) {
        /* the old one's lists point into the library, so only its layouts'
         * address survives this, for layouts_moved() */
        api->config(&srv->conf->loaded);
        /* the library fills it in itself; filling it twice changes nothing */
        awl_config_fill_defaults(&srv->conf->loaded);
        srv->cfg = &srv->conf->loaded;
    } else {
        srv->cfg = builtin_config(srv->proc);
    }
    layouts_moved(srv, old, n_old);
    config_apply(srv);
}

/** :c:func:`awl_plugins_reload`: the bars' widgets come from the library */
static void plugins_detach( awl_server_t *srv ) {
    const Layout *old = srv->cfg->layouts.v;
    size_t n_old = srv->cfg->layouts.n;

    bar_widgets_clear(srv);
    /* the popups stay until the library's fini gives them back (or
     * plugins_reap() destroys them): its painters and callbacks are still
     * there until then. cfg points into the library */
    srv->cfg = builtin_config(srv->proc);
    layouts_moved(srv, old, n_old);
}

/** :c:func:`awl_plugins_reload`: the new library's config (:c:func:`config_use`),
 * the bars' widgets from it, the desktop panels redrawn */
static void plugins_attach( awl_server_t *srv ) {
    config_use(srv);
    bar_widgets_attach(srv);
    bars_dirty(srv);
    desktop_reloaded(srv->desktop);
}

static void plugin_restart_idle( void *data ) {
    awl_server_t *srv = data;
    srv->conf->restart_source = NULL;
    /* picks up a rebuilt libawlplugins.so; with an unchanged (or broken)
     * one this is a plain restart of the plugin threads */
    awl_plugins_reload(srv, plugins_detach, plugins_attach);

    /* the reloaded config's tray icon theme */
    {
        char home[PATH_MAX];
        awl_tray_app_set_theme(srv->proc, srv->cfg->tray.icon_theme,
                cfg_home(srv, home, sizeof(home)) ? NULL : home);
    }
}

/** The plugin_restart action: reloads ``libawlplugins.so`` (its tray widget
 * too), see :c:func:`awl_plugins_reload`. Deferred
 * to the event loop's next idle round, since the binding calling it is in the
 * library the reload unloads. */
void plugin_restart( awl_server_t *srv, const Arg* arg ) {
    (void)arg;
    /* Not from here: this is usually called through a key binding, from the
     * library's code the reload unloads */
    if (!srv->conf->restart_source)
        srv->conf->restart_source = wl_event_loop_add_idle(srv->event_loop, plugin_restart_idle, srv);
}

void config_defaults( awl_config_t *c ) {
    *c = awl_config_defaults;
}

const awl_config_t *builtin_config( awl_process_t *proc ) {
    if (!proc->config.builtin_filled) {
        proc->config.builtin = awl_config_defaults;
        awl_config_fill_defaults(&proc->config.builtin);
        proc->config.builtin_filled = 1;
    }
    return &proc->config.builtin;
}
