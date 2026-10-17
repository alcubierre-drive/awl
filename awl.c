#include "awl.h"
#include "awl_log.h"
#include "awl_util.h"
#include "awl_draw.h"
#include "awl_dict.h"
#include "awl_plugin_host.h"
#include "awl_plugin_abi.h"
#include "awl_background.h"
#include "awl_desktop.h"
#include "plugins/colors.h" /* config_plugins.h */
#include "awl_redraw.h"
#include "awl_lock.h"
#include "awl_alert.h"
#include "awl_window.h"
#include "tray/awl_tray.h"
#include <fcntl.h>
#include <limits.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/interfaces/wlr_output.h>

/** vfork(2) is a glibc/BSD extension not declared under the strict
 * -D_POSIX_C_SOURCE=200809L this file is built with, though it's present
 * in libc regardless; declare it ourselves rather than widen the feature
 * test macros for the whole translation unit. Used instead of fork() when
 * spawning children after awl's background threads (plugin monitors, GPU
 * driver threads) are running, since fork() alone risks inheriting a lock
 * one of those threads held at the moment of the fork -- see spawn_pid(). */
extern pid_t vfork(void);

/* function declarations */
static void applybounds(Client *c, struct wlr_box *bbox);
static void applyrules(Client *c);
static void attachblur(Client *c);
static void scenebuffersetopacity(struct wlr_scene_buffer *buffer, int sx, int sy, void *data);
static void arrangelayer(Monitor *m, struct wl_list *list,
        struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void axisnotify(struct wl_listener *listener, void *data);
static bool bar_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy);
static int desktopat(double x, double y);
static void buffer_destroy(struct wlr_buffer *buffer);
static bool buffer_begin_data_ptr_access(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride);
static void buffer_end_data_ptr_access(struct wlr_buffer *buffer);
static void buttonpress(struct wl_listener *listener, void *data);
static void chvt(const Arg *arg);
static void checkidleinhibitor(struct wlr_surface *exclude);
static void cleanup(void);
static int in_cleanupmon = 0;
static void cleanupmon(struct wl_listener *listener, void *data);
static void cleanuplisteners(void);
static void closemon(Monitor *m);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void commitpopup(struct wl_listener *listener, void *data);
static void createdecoration(struct wl_listener *listener, void *data);
static void createidleinhibitor(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_keyboard *keyboard);
static KeyboardGroup *createkeyboardgroup(void);
static struct xkb_keymap *compilekeymap(void);
static void configapply(void);
static void wallpapermodeset(WallpaperMode mode);
static void configuse(void);
static void configurepointer(struct wlr_pointer *pointer);
static void trackinputdevice(struct wlr_input_device *device);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createlocksurface(struct wl_listener *listener, void *data);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_pointer *pointer);
static void createpointerconstraint(struct wl_listener *listener, void *data);
static void createpopup(struct wl_listener *listener, void *data);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void cursorframe(struct wl_listener *listener, void *data);
static void cursorwarptohint(void);
static void destroydecoration(struct wl_listener *listener, void *data);
static void destroydragicon(struct wl_listener *listener, void *data);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroylock(SessionLock *lock, int unlocked);
static void destroylocksurface(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void destroykeyboardgroup(struct wl_listener *listener, void *data);
static Monitor *nextmon(int add);
static void drawbar(Monitor *m);
static void drawbars(void);
static void drawalert(Monitor *m);
static void drawalerts(void);
static void textwinsupdate(Monitor *m);
static void textwinshideall(void);
static awl_window_t *textwinat(double x, double y);
static awl_window_t *textwinpointer(void);
static int textwinmotion(void);
static void textwinsdestroy(void);
static bool alert_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy);
static Buffer *barbuffer(Monitor *m);
static Buffer *newbarbuffer(Monitor *m);
static int bardamage(const Buffer *buf, const Buffer *prev, pixman_region32_t *damage);

static void wallpapertake(void);

/* the screen lock, see lockscreen(): lock requests and the logind thread's
 * state come in through awl_lock.h's eventfd */
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
static int lockfire(int fd, uint32_t mask, void *data);
static int logindtimeout(void *data);
static int coveredtimeout(void *data);
static void lockcovered(void);
static void logindupdate(void);
void awl_alert_notify(const char *text);

/** bars are redrawn on demand: see awl_redraw.h */
static struct wl_event_source* redraw_source = NULL;
static int redraw_fire( int fd, uint32_t mask, void* data ) {
    (void)fd; (void)mask; (void)data;
    awl_redraw_drain();
    drawbars();
    desktop_update_all();
    wallpapertake();
    drawalerts();
    return 0;
}

static void focusmon(const Arg *arg);
static void setselmon(Monitor *m);
static void focusstack(const Arg *arg);
static void movestack(const Arg *arg);
static Client *focustop(Monitor *m);
static void fullscreennotify(struct wl_listener *listener, void *data);
static void gpureset(struct wl_listener *listener, void *data);
static void bstack(Monitor* m);
static void gaplessgrid(Monitor *m);
static int handlesigchld(int signo, void *data);
static int handlesigquit(int signo, void *data);
static void incnmaster(const Arg *arg);
static void inputdevice(struct wl_listener *listener, void *data);
static int keybinding(uint32_t mods, xkb_keysym_t sym);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static int keyrepeat(void *data);
static void killclient(const Arg *arg);
static void lockscreen(const Arg *arg);
static void locksession(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void maximizenotify(struct wl_listener *listener, void *data);
static void monocle(Monitor *m);
static void motionabsolute(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
        double sy, double sx_unaccel, double sy_unaccel);
static void motionrelative(struct wl_listener *listener, void *data);
static void moveresize(const Arg *arg);
static void outputmgrapply(struct wl_listener *listener, void *data);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
static void outputmgrtest(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface,
        double sx, double sy, uint32_t time);
static void powermgrsetmode(struct wl_listener *listener, void *data);
static void quit(const Arg *arg);
static void rendermon(struct wl_listener *listener, void *data);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void requestmonstate(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int interact);
static void run(char *startup_cmd);
static void setcursor(struct wl_listener *listener, void *data);
static void setcursorshape(struct wl_listener *listener, void *data);
static void setfloating(Client *c, int floating);
static void setfullscreen(Client *c, int fullscreen);
static void setmfact(const Arg *arg);
static void setmon(Client *c, Monitor *m, uint32_t newtags);
static void setpsel(struct wl_listener *listener, void *data);
static void setsel(struct wl_listener *listener, void *data);
static void setup(void);
static void wlrlog(enum wlr_log_importance importance, const char *fmt, va_list ap);
static void spawn(const Arg *arg);
static pid_t spawn_pid(const Arg *arg);
static void autostart(const char **argv);
static void startdrag(struct wl_listener *listener, void *data);
static void tag(const Arg *arg);
static void tagmon(const Arg *arg);
static int testoutputadd(int signo, void *data);
static int testoutputremove(int signo, void *data);
static int testwinctl(int fd, uint32_t mask, void *data);
static void tile(Monitor *m);
static void togglebar_mon(Monitor* m);
static void togglebar(const Arg *arg);
static void wallpaper(const Arg *arg);
static void wallpapernext(WallpaperMode mode);
static void wallpapermode(const Arg *arg);
static void togglebw(const Arg *arg);
static void changebw(const Arg *arg);
static void togglefloating(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void toggletag(const Arg *arg);
static void unlocksession(struct wl_listener *listener, void *data);
static void unblocksignals(void);
static void stopchildren(void);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatemons(struct wl_listener *listener, void *data);
static void updatepluginpause(void);
static void updatebar(Monitor *m);
static void updatefonts(Monitor *m);
static void updatetitle(struct wl_listener *listener, void *data);
static void urgent(struct wl_listener *listener, void *data);
static void virtualkeyboard(struct wl_listener *listener, void *data);
static void virtualpointer(struct wl_listener *listener, void *data);
static Monitor *xytomon(double x, double y);
static void xytonode(double x, double y, struct wlr_surface **psurface,
        Client **pc, LayerSurface **pl, double *nx, double *ny);
static void setontop(Client *c, int ontop);
static void toggleontop(const Arg* arg);
static void togglepertag(const Arg *arg);
static struct PertagLayout *monlt(Monitor *m);
static const Layout *sellayout(Monitor *m);
static void plugin_restart(const Arg* arg);
static void minimize(const Arg* arg);
static void unminimize(const Arg* arg);
static void maximize(const Arg* arg);
static void cycle_layout(const Arg* arg);
static void setlayout(const Arg *arg);
static int layoutindex(const Layout *l);
static void cycle_view(const Arg* arg);
static void toggleview(const Arg *arg);
static void transluce(const Arg *arg);
static void view(const Arg *arg);

/** how long awl waits on exit for the tray thread and for its children */
#define TRAY_STOP_MS 5000
#define CHILD_STOP_MS 3000
/** how long the logind thread may not listen before the alert goes up
 * (logindupdate()), and the most a lock waits to cover every output
 * (lockcovered()) */
#define LOGIND_GRACE_MS 10000
#define LOCK_COVER_MS 2000
/** the alert (drawalert()): its font size relative to the bar's, its
 * distance from the bottom left corner in logical pixels, its opacity */
#define ALERT_FONT_SCALE 1.5
#define ALERT_MARGIN 12
#define ALERT_ALPHA 0.9f

/* variables */
static pid_t child_pid = -1;

static pid_t Autostarted_pids[256] = {0};
static int Autostarted_pids_sz = 0;

static int locked = 0;
/* key presses while locked, reset on unlock; nothing reads it yet (main
 * thread only, and only a count: never the keys, never their timing) */
static unsigned locked_keystrokes = 0;
static void *exclusive_focus;
static struct wl_display *dpy;
static struct wl_event_loop *event_loop;
static struct wlr_backend *backend;
static struct wlr_scene *scene;
static struct wlr_scene_tree *layers[NUM_LAYERS];
static struct wlr_scene_tree *drag_icon;
static struct wlr_scene_tree *desktop_tree;
static struct wlr_scene_tree *background_tree;
/** Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };
static struct wlr_renderer *drw;
static struct wlr_allocator *alloc;
static struct wlr_compositor *compositor;
static struct wlr_session *session;

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack;  /* focus order */
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
static struct wlr_keyboard_shortcuts_inhibit_manager_v1 *keyboard_shortcuts_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
static struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_output_power_manager_v1 *power_mgr;

static struct wlr_pointer_constraints_v1 *pointer_constraints;
static struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
static struct wlr_pointer_constraint_v1 *active_constraint;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;

static struct wlr_scene_rect *root_bg;
/** the alerts' tree, the topmost one: see drawalert() */
static struct wlr_scene_tree *alert_tree;
/** the plugins' text popups (awl_window.h, textwincreate()), TextWin.link;
 * their tree, right above LyrTop, is made with the first one: until a plugin
 * asks for a popup, none of this runs */
typedef struct {
    awl_window_t *win;
    struct wl_list link;
} TextWin;
static struct wl_list textwins = {&textwins, &textwins};
static struct wlr_scene_tree *textwin_tree;
/** AWL_TEST_WINDOWS' popups by number, see testwincmd(); textwindestroy()
 * clears a slot, so a plugin reload leaves none dangling */
static awl_window_t *testwins[8];
/** the popup the pointer is over, if it takes the pointer */
static awl_window_t *textwin_hovered;
static struct wlr_session_lock_manager_v1 *session_lock_mgr;
static struct wlr_scene_blur *locked_bg_blur = NULL;
static struct wlr_session_lock_v1 *cur_lock;

static struct wlr_seat *seat;
static KeyboardGroup *kb_group;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy; /* client-relative */

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;

static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .begin_data_ptr_access = buffer_begin_data_ptr_access,
    .end_data_ptr_access = buffer_end_data_ptr_access
};

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener gpu_reset = {.notify = gpureset};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
static struct wl_listener new_input_device = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
static struct wl_listener new_pointer_constraint = {.notify = createpointerconstraint};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_toplevel = {.notify = createnotify};
static struct wl_listener new_xdg_popup = {.notify = createpopup};
static struct wl_listener new_xdg_decoration = {.notify = createdecoration};
static struct wl_listener new_layer_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener output_power_mgr_set_mode = {.notify = powermgrsetmode};
static struct wl_listener request_activate = {.notify = urgent};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener new_session_lock = {.notify = locksession};

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void associatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void dissociatex11(struct wl_listener *listener, void *data);
static void sethints(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
#endif

/* configuration, allows nested code to access above variables */
#include "config_awl.h"
/* the defaults, for while no library (and so no config_plugins.h) is loaded */
#include "config_plugins.def.h"

/* attempt to encapsulate suck into one file */
#include "awl_client.h"

#ifdef SCENEFX_HAS_SHATTER
#define AWL_wlr_scene_blur_set_shatter(blur, shatter) \
    wlr_scene_blur_set_shatter((blur), (shatter))
#else
#define AWL_wlr_scene_blur_set_shatter(blur, shatter) {}
#endif

#define AWL_ACTION_INIT(name) .name = name,
const awl_actions_t awl_actions = { AWL_ACTIONS(AWL_ACTION_INIT) };
const awl_arranges_t awl_arranges = { AWL_ARRANGES(AWL_ACTION_INIT) };
#undef AWL_ACTION_INIT

/** config_plugins.h in effect: the library's, or while none is
 * loaded config_plugins.def.h. It points into the library, so it's only
 * good until the next reload; `pluginsdetach()` falls back to builtinconfig(). */
static const awl_config_t *cfg;
/** The config values that can change while awl runs, by name, for awl and the
 * library (`awl_host_t.cfg`). Lives from setup() to cleanup(), across reloads. */
static awl_dict_t *cfgdict;
/** cfg->bordercolors, copied */
static uint32_t borders[BorderLast];
/** What the keyboard and the bars' font were set up with, copied, to tell
 * whether a reload changed them */
static struct AppliedConfig {
    char *xkb[5]; /* rules, model, layout, variant, options */
    int repeat_rate, repeat_delay;
    char *font;
    int fontsize;
    char *popupfont; /* popup_config's */
    int popupfontsize;
    char *desktopfont; /* desktop_config's */
    int desktopfontsize;
    unsigned int borderpx;
    unsigned int wallpaper_interval;
    WallpaperMode wallpaper_mode;
    int wallpaper_mode_dict; /* cfgdict's "wallpaper_mode", -1: none */
} applied = { .wallpaper_mode_dict = -1 };

/** the keyboards and pointers, to apply a reloaded config_plugins.h to */
typedef struct {
    struct wlr_input_device *device;
    struct wl_listener destroy;
    struct wl_list link;
} InputDevice;
static struct wl_list inputdevices; /* InputDevice.link */
static struct wl_event_source *plugin_restart_source;

static void cfgdict_defaults(awl_dict_t* cfgdict)
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

    /* "wallpaper_dir": the folder the wallpapers come from, the built-in
     * config_plugins.h's wallpaper_config.dir, relative to $HOME unless absolute;
     * none for a NULL one, then there's no wallpaper */
    const char *wp = cfg->wallpaper->dir;
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
     * config_plugins.h's to begin with; awl keeps it current, and a plugin may set
     * it. plugins/wallpaper.c saves it in plugins/persistent.h's file and
     * at the start loads the saved one over this. */
    wallpapermodeset(cfg->wallpaper->mode);
    applied.wallpaper_mode = cfg->wallpaper->mode;

    /* "desktop_file_mode": what the desktop panels list, "off", "on" or
     * "all" (hidden files too); plugins_desktop_panel.c keeps it current
     * and, like "wallpaper_mode", saved */
    awl_dict_set(cfgdict, "desktop_file_mode", &AWL_DICT_STR("on"));

    /* "lock_on_suspend": config_awl.h's, for plugins/logind.c */
    awl_dict_set(cfgdict, "lock_on_suspend", &AWL_DICT_NUM(lock_on_suspend));
}

static const awl_config_t *
builtinconfig(void)
{
    static awl_config_t c;
    c = AWL_CONFIG_TABLE;
    return &c;
}

/* function implementations */
void
applybounds(Client *c, struct wlr_box *bbox)
{
    /* set minimum possible */
    c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
    c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);

    if (c->geom.x >= bbox->x + bbox->width)
        c->geom.x = bbox->x + bbox->width - c->geom.width;
    if (c->geom.y >= bbox->y + bbox->height)
        c->geom.y = bbox->y + bbox->height - c->geom.height;
    if (c->geom.x + c->geom.width <= bbox->x)
        c->geom.x = bbox->x;
    if (c->geom.y + c->geom.height <= bbox->y)
        c->geom.y = bbox->y;
}

void
scenebuffersetopacity(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    Client *c = data;
    if (c && c->one_minus_alpha != 0 && c->blur) {
        float opacity = 1. - c->one_minus_alpha;
        struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
        if (!scene_surface) return;

        struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(scene_surface->surface);
        if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
            wlr_scene_buffer_set_opacity(buffer, opacity);

            // if (!wlr_subsurface_try_from_wlr_surface(xdg_surface->surface) && c->blur) {
            //     wlr_scene_blur_set_transparency_mask_source(c->blur, buffer);
            // }
        }
    }
}

void
applyrules(Client *c)
{
    /* rule matching */
    const char *appid, *title;
    uint32_t newtags = 0;
    int i;
    const Rule *r;
    Monitor *mon = selmon, *m;

    appid = client_get_appid(c);
    title = client_get_title(c);
    awl_log_printf( "SPAWN APPID '%s' @TITLE '%s'\n", appid, title );
    int apply_resize = 0;
    struct wlr_box rbox;

    for (r = cfg->rules; r < cfg->rules + cfg->n_rules; r++) {
        if ((!r->title || strstr(title, r->title))
                && (!r->id || strstr(appid, r->id))) {
            c->isfloating = r->isfloating;
            newtags |= r->tags;
            i = 0;
            wl_list_for_each(m, &mons, link) {
                if (r->monitor == i++)
                    mon = m;
            }
            if (c->isfloating || !sellayout(mon)->arrange) {
                /* client is floating or in floating layout */
                if (r->w != 0 && r->h != 0) {
                    rbox.width = r->w;
                    rbox.height = r->h;
                    rbox.x = mon->w.x + mon->w.width - rbox.width;
                    rbox.y = mon->w.y + mon->w.height - rbox.height;
                    apply_resize = 1;
                }
            }
            c->one_minus_alpha = r->one_minus_alpha;
            if (r->blur && !c->blur) attachblur(c);
        }
    }

    c->isfloating |= client_is_float_type(c);
    setmon(c, mon, newtags);
    if (apply_resize) resize(c, rbox, 1);
}

void
attachblur(Client *c)
{
    if (!c) return;
    struct wlr_scene_tree* tree = c->one_minus_alpha == 0 ? c->scene : c->scene_surface;
    if (!tree) return;
    if (!c->blur) {
        c->blur = wlr_scene_blur_create(tree, 0, 0);
        wlr_scene_blur_set_size(c->blur, c->geom.width, c->geom.height);
        wlr_scene_blur_set_strength(c->blur, cfg->blur[0]);
        wlr_scene_blur_set_alpha(c->blur, cfg->blur[1]);
        wlr_scene_blur_set_should_only_blur_bottom_layer(c->blur, 0);
        wlr_scene_node_lower_to_bottom(&c->blur->node);
        wlr_scene_node_set_enabled(&c->blur->node, 1);
    }
}

void
arrange(Monitor *m)
{
    Client *c;

    if (!m->wlr_output->enabled)
        return;

    wl_list_for_each(c, &clients, link) {
        if (c->mon == m) {
            wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON_ACTIVE(c, m));
            client_set_suspended(c, !VISIBLEON_ACTIVE(c, m));
        }
    }

    wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
            (c = focustop(m)) && c->isfullscreen);

    strncpy(m->ltsymbol, sellayout(m)->symbol, LENGTH(m->ltsymbol)-1);

    /* We move all clients (except fullscreen and unmanaged) to LyrTile while
     * in floating layout to avoid "real" floating clients be always on top */
    wl_list_for_each(c, &clients, link) {
        if (c->mon != m || c->scene->node.parent == layers[LyrFS])
            continue;

        wlr_scene_node_reparent(&c->scene->node,
                c->isontop ? layers[LyrTop] :
                (!sellayout(m)->arrange && c->isfloating)
                        ? layers[LyrTile]
                        : (sellayout(m)->arrange && c->isfloating)
                                ? layers[LyrFloat]
                                : c->scene->node.parent);
    }

    if (sellayout(m)->arrange)
        sellayout(m)->arrange(m);
    motionnotify(0, NULL, 0, 0, 0, 0);
    checkidleinhibitor(NULL);
    /* layout symbol, taskbar order and window states may have changed */
    awl_redraw_request();
}

void
arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
    LayerSurface *l;
    struct wlr_box full_area = m->m;

    wl_list_for_each(l, list, link) {
        struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

        if (!layer_surface->initialized)
            continue;

        if (exclusive != (layer_surface->current.exclusive_zone > 0))
            continue;

        wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area, usable_area);
        wlr_scene_node_set_position(&l->popups->node, l->scene->node.x, l->scene->node.y);
    }
}

void
arrangelayers(Monitor *m)
{
    int i;
    struct wlr_box usable_area = m->m;
    LayerSurface *l;
    uint32_t layers_above_shell[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
    };
    if (!m->wlr_output->enabled)
        return;

    if (m->showbar) {
        usable_area.height -= m->b.real_height;
        usable_area.y += topbar ? m->b.real_height : 0;
    }

    /* Arrange exclusive surfaces from top->bottom */
    for (i = 3; i >= 0; i--)
        arrangelayer(m, &m->layers[i], &usable_area, 1);

    if (!wlr_box_equal(&usable_area, &m->w)) {
        m->w = usable_area;
        arrange(m);
    }

    /* Arrange non-exlusive surfaces from top->bottom */
    for (i = 3; i >= 0; i--)
        arrangelayer(m, &m->layers[i], &usable_area, 0);

    desktop_update(m);
    background_update(m);
    drawalert(m);
    textwinsupdate(m);

    /* a loop of its own: the focus search below stops at the first
     * keyboard-interactive surface (e.g. a launcher), which would leave
     * the blur of anything after it (a new notification) unsized */
    for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
        wl_list_for_each(l, &m->layers[layers_above_shell[i]], link) {
            if ((l->is_notification || l->is_launcher) && l->blur && l->layer_surface) {
                /* arrangelayers() runs on every layer-surface commit (e.g.
                 * every redrawn frame of a notification/launcher), not just
                 * on actual resizes; only touch the blur node's size when it
                 * actually changed, otherwise scenefx redoes its blur-region
                 * bookkeeping every frame, which shows up as flicker. */
                uint32_t desired_width = l->layer_surface->current.desired_width;
                uint32_t desired_height = l->layer_surface->current.desired_height;
                if ((uint32_t)l->geom.width != desired_width || (uint32_t)l->geom.height != desired_height) {
                    wlr_scene_blur_set_size(l->blur, desired_width, desired_height);
                    l->geom.width = desired_width;
                    l->geom.height = desired_height;
                }
            }
        }
    }

    /* Find topmost keyboard interactive layer, if such a layer exists */
    for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
        wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
            if (locked || !l->layer_surface->current.keyboard_interactive || !l->mapped)
                continue;
            focusclient(NULL, 0);
            exclusive_focus = l;
            client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
            return;
        }
    }
}

/** Bar widget hover (widget_t.callback_hover): hover_widget is the widget
 * under the pointer if it has a hover callback, hover_timer fires its
 * callback hover_delay_ms after the pointer got there. hover_active is the
 * widget whose hover fired and that has a callback_leave; leave_timer fires
 * that leave_delay_ms after the pointer left both it and its popup (at once
 * for a delay of 0). */
static struct wl_event_source *hover_timer, *leave_timer;
/** config_plugins.h's wallpaper_config.interval */
static struct wl_event_source *wallpaper_timer;
/** what the timer switches to: config_plugins.h's mode or cfgdict's "wallpaper_mode",
 * until `wallpapermode()` */
static WallpaperMode wallpaper_mode;
static widget_t *hover_widget, *hover_active;
static int leave_pending;

static widget_t *
barwidgetat(Monitor *m, double cx, double cy)
{
    struct wlr_scene_node *node;
    struct wlr_scene_buffer *buffer;
    unsigned int cursor_x, xpos = 0;
    awl_draw_t *d = &m->drw;
    int i;

    if (!d->m || !(node = wlr_scene_node_at(&layers[LyrBottom]->node, cx, cy, NULL, NULL))
            || !(buffer = wlr_scene_buffer_from_node(node)) || buffer != m->scene_buffer)
        return NULL;
    cursor_x = (unsigned int)((cx - m->m.x) * m->wlr_output->scale);
    for (i = 0; i < d->n_widgets_left; ++i) {
        if (cursor_x >= xpos && cursor_x < xpos + d->widgets_left[i].width)
            return &d->widgets_left[i];
        xpos += d->widgets_left[i].width;
    }
    xpos = d->center_widget_start;
    if (cursor_x >= xpos && cursor_x < xpos + d->center_widget_space)
        return d->has_center_widget ? &d->center_widget : NULL;
    xpos += d->center_widget_space;
    for (i = d->n_widgets_right - 1; i >= 0; --i) {
        if (cursor_x >= xpos && cursor_x < xpos + d->widgets_right[i].width)
            return &d->widgets_right[i];
        xpos += d->widgets_right[i].width;
    }
    return NULL;
}

static int leavetimeout(void *data);

/** a leave_delay_ms of 0 calls callback_leave right away */
static void
setleavepending(int pending)
{
    if (pending == leave_pending)
        return;
    if (pending && !hover_active->leave_delay_ms) {
        leavetimeout(NULL);
        return;
    }
    leave_pending = pending;
    wl_event_source_timer_update(leave_timer, pending ? (int)hover_active->leave_delay_ms : 0);
}

static int
leavetimeout(void *data)
{
    widget_t *w = hover_active;

    (void)data;
    hover_active = NULL;
    leave_pending = 0;
    if (w)
        w->callback_leave(w);
    return 0;
}

static int
hovertimeout(void *data)
{
    (void)data;
    /* not while a layer surface (e.g. a launcher) holds the keyboard */
    if (!hover_widget || locked || exclusive_focus)
        return 0;
    if (hover_active && hover_active != hover_widget) {
        setleavepending(0);
        leavetimeout(NULL);
    }
    if (hover_widget->callback_leave)
        hover_active = hover_widget;
    hover_widget->callback_hover(hover_widget);
    return 0;
}

/** whether the pointer is inside a mapped layer surface whose namespace
 * starts with ns */
static int
pointerinpopup(const char *ns)
{
    Monitor *m;
    LayerSurface *l;
    size_t i;
    int lx, ly;

    wl_list_for_each(m, &mons, link) {
        for (i = 0; i < LENGTH(m->layers); i++) {
            wl_list_for_each(l, &m->layers[i], link) {
                if (!l->mapped || !l->layer_surface->namespace
                        || strncmp(l->layer_surface->namespace, ns, strlen(ns)))
                    continue;
                wlr_scene_node_coords(&l->scene->node, &lx, &ly);
                struct wlr_box box = { lx, ly, l->layer_surface->surface->current.width,
                        l->layer_surface->surface->current.height };
                if (wlr_box_contains_point(&box, cursor->x, cursor->y))
                    return 1;
            }
        }
    }
    return 0;
}

static TextWin *textwinfind(awl_window_t *win);

/** whether the pointer is over win, one of awl's own popups (textwins) */
static int
pointerintextwin(awl_window_t *win)
{
    /* the plugin may have destroyed it without clearing popup_window */
    return textwinfind(win) && awl_window_contains(win, cursor->x, cursor->y);
}

/** c is the client under the pointer, which may cover the bar. Exclusive
 * focus (e.g. a launcher) deliberately doesn't reset the hover
 * state. */
static void
updatehover(Client *c)
{
    widget_t *w = NULL;
    Monitor *m;

    if (!hover_timer)
        return;
    if (!c && !locked && cursor_mode == CurNormal
            && (m = xytomon(cursor->x, cursor->y)))
        w = barwidgetat(m, cursor->x, cursor->y);
    if (w && !w->callback_hover)
        w = NULL;
    if (hover_active)
        setleavepending(w != hover_active && !(hover_active->popup_namespace
                && pointerinpopup(hover_active->popup_namespace))
                && !pointerintextwin(hover_active->popup_window));
    if (w == hover_widget)
        return;
    hover_widget = w;
    /* a delay of 0 would disarm the timer */
    wl_event_source_timer_update(hover_timer, w ? (w->hover_delay_ms ? (int)w->hover_delay_ms : 1) : 0);
}

static const double SCROLL_LIMIT = 10.0;
static void widget_wrap_scroll_callback( widget_t* w, uint32_t xrel, double delta ) {
    if (w->callback_scroll) {
        w->scroll_amount += delta;
        if (fabs(w->scroll_amount) > SCROLL_LIMIT) {
            (*w->callback_scroll)( w, xrel, (w->scroll_amount > 0) ? 1 : (w->scroll_amount < 0) ? -1 : 0 );
            w->scroll_amount = 0;
        }
    }
}

void
axisnotify(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits an axis event,
     * for example when you move the scroll wheel. */
    struct wlr_pointer_axis_event *event = data;
    wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

    Client* c = NULL;
    struct wlr_scene_buffer *buffer;
    struct wlr_scene_node *node;
    awl_window_t *twin;
    int row, col;

    /* A popup that takes the pointer gets the scrolling, as it gets the
     * presses; one with only a click callback drops it: its motion took the
     * pointer focus from what is below anyway. An open xdg_popup's grab
     * comes first, as for the buttons. */
    if (!wlr_seat_pointer_has_grab(seat) && (twin = textwinpointer())) {
        awl_dbg_printf(awl_loglevel_vrb, "textwin %p: axis %d %g (%d/120) at %.0f,%.0f\n", (void *)twin,
                event->orientation, event->delta, event->delta_discrete, cursor->x, cursor->y);
        /* the callback may destroy it */
        awl_window_pointer_axis(twin, cursor->x, cursor->y, event->orientation, event->delta,
                event->delta_discrete, &row, &col);
        awl_dbg_printf(awl_loglevel_vrb, "textwin %p: that is row %d, column %d\n", (void *)twin, row, col);
        return;
    }
    xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
    if (!c && !exclusive_focus &&
        (node = wlr_scene_node_at(&layers[LyrBottom]->node, cursor->x, cursor->y, NULL, NULL)) &&
        (buffer = wlr_scene_buffer_from_node(node)) && buffer == selmon->scene_buffer
        && !locked) {
        unsigned int cursor_x = cursor->x, cursor_y = cursor->y;
        cursor_x -= selmon->m.x;
        cursor_x *= selmon->wlr_output->scale;
        cursor_y *= selmon->wlr_output->scale;
        (void)cursor_y;
        unsigned int xpos = 0;
        for (int i=0; i<selmon->drw.n_widgets_left; ++i) {
            if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.widgets_left[i].width) {
                widget_wrap_scroll_callback( &selmon->drw.widgets_left[i], cursor_x - xpos, event->delta );
                return;
            }
            xpos += selmon->drw.widgets_left[i].width;
        }
        xpos = selmon->drw.center_widget_start;
        if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.center_widget_space)
            if (selmon->drw.has_center_widget) {
                widget_wrap_scroll_callback( &selmon->drw.center_widget, cursor_x - xpos, event->delta );
                return;
            }
        xpos += selmon->drw.center_widget_space;
        for (int i=selmon->drw.n_widgets_right-1; i>=0; --i) {
            if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.widgets_right[i].width) {
                widget_wrap_scroll_callback( &selmon->drw.widgets_right[i], cursor_x - xpos, event->delta );
                return;
            }
            xpos += selmon->drw.widgets_right[i].width;
        }
    }
    /* TODO: allow usage of scroll whell for mousebindings, it can be implemented
     * checking the event's orientation and the delta of the event */
    /* Notify the client with pointer focus of the axis event. */
    wlr_seat_pointer_notify_axis(seat,
            event->time_msec, event->orientation, event->delta,
            event->delta_discrete, event->source, event->relative_direction);
}

bool
bar_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    return true;
}

void
buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    Buffer *buf;
    buf = wl_container_of(wlr_buffer, buf, base);
    free(buf);
}

bool
buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
                             void **data, uint32_t *format, size_t *stride)
{
    Buffer *buf;
    buf = wl_container_of(wlr_buffer, buf, base);

    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;

    *data   = buf->data;
    *stride = buf->stride;
    *format = DRM_FORMAT_ARGB8888;

    return true;
}

void
buffer_end_data_ptr_access(struct wlr_buffer *buffer)
{
}

int
desktopat(double x, double y)
{
    /* only the wallpaper, background layer surfaces (LyrBg) and the desktop
     * panels, which aren't in layers[], are below; LyrBlock only matters
     * while locked */
    int i;
    for (i = LyrBottom; i < LyrBlock; i++)
        if (wlr_scene_node_at(&layers[i]->node, x, y, NULL, NULL))
            return 0;
    return xytomon(x, y) != NULL;
}

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
    applied.wallpaper_mode_dict = mode;
}

/** takes a cfgdict "wallpaper_mode" set since: a plugin's, or the saved one
 * plugins/wallpaper.c loads at the start */
static void
wallpapermodesync(void)
{
    int mode = wallpapermodedict();
    if (mode >= 0 && mode != applied.wallpaper_mode_dict)
        wallpaper_mode = mode;
    applied.wallpaper_mode_dict = mode;
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

/** shows a wallpaper the library has decoded since, if any */
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
buttonpress(struct wl_listener *listener, void *data)
{
    unsigned int click;
    struct wlr_pointer_button_event *event = data;
    struct wlr_keyboard *keyboard;
    struct wlr_scene_node *node;
    struct wlr_scene_buffer *buffer;
    uint32_t mods;
    Client *c;
    const Button *b;
    awl_window_t *twin;
    static int swallow_release;

    wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

    /* a press an awl window took (below) swallows its release too, so
     * nothing below sees half a click */
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && swallow_release) {
        swallow_release = 0;
        return;
    }
    /* An open xdg_popup (a menu, e.g. the tray's) holds a pointer grab and
     * closes itself on a press outside its client; that only works if the
     * press reaches the grab, so the bar, the desktop and the bindings don't
     * get to see it first. */
    if (wlr_seat_pointer_has_grab(seat)) {
        /* the press that opened the menu (or started a drag) set this */
        if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && cursor_mode == CurPressed)
            cursor_mode = CurNormal;
        wlr_seat_pointer_notify_button(seat,
                event->time_msec, event->button, event->state);
        return;
    }
    /* a popup that takes the pointer gets the press, and its release */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && !locked
            && (twin = textwinat(cursor->x, cursor->y))) {
        int row, col;
        awl_dbg_printf(awl_loglevel_vrb, "textwin %p: button %u at %.0f,%.0f\n", (void *)twin,
                event->button, cursor->x, cursor->y);
        /* the callback may destroy it */
        awl_window_pointer_button(twin, cursor->x, cursor->y, event->button, &row, &col);
        awl_dbg_printf(awl_loglevel_vrb, "textwin %p: that is row %d, column %d\n", (void *)twin, row, col);
        swallow_release = 1;
        return;
    }
    /* the other popups take no input; a press anywhere but on the bar (whose
     * widgets toggle them) closes them, and goes on to what is below */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && !wl_list_empty(&textwins)
            && !((node = wlr_scene_node_at(&layers[LyrBottom]->node, cursor->x, cursor->y, NULL, NULL))
                && (buffer = wlr_scene_buffer_from_node(node)) && buffer == selmon->scene_buffer))
        textwinshideall();

    click = ClkRoot; // TODO we don't treat these cases
    xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
    if (c)
        click = ClkClient;

    if (!c && !exclusive_focus &&
        (node = wlr_scene_node_at(&layers[LyrBottom]->node, cursor->x, cursor->y, NULL, NULL)) &&
        (buffer = wlr_scene_buffer_from_node(node)) && buffer == selmon->scene_buffer
        && event->state == WL_POINTER_BUTTON_STATE_PRESSED && !locked) {
        /* a click on the hovered widget takes over from the hover, so
         * whatever it opens doesn't close on leave */
        if (hover_active && barwidgetat(selmon, cursor->x, cursor->y) == hover_active) {
            setleavepending(0);
            hover_active = NULL;
        }
        unsigned int cursor_x = cursor->x, cursor_y = cursor->y;
        cursor_x -= selmon->m.x;
        cursor_x *= selmon->wlr_output->scale;
        cursor_y *= selmon->wlr_output->scale;
        (void)cursor_y;
        unsigned int xpos = 0;
        for (int i=0; i<selmon->drw.n_widgets_left; ++i) {
            if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.widgets_left[i].width) {
                click = ClkBar;
                if (selmon->drw.widgets_left[i].callback_click) {
                    (*selmon->drw.widgets_left[i].callback_click)(&selmon->drw.widgets_left[i],
                            cursor_x - xpos, event->button);
                    return;
                }
            }
            xpos += selmon->drw.widgets_left[i].width;
        }
        xpos = selmon->drw.center_widget_start;
        if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.center_widget_space) {
            click = ClkBar;
            if (selmon->drw.has_center_widget) {
                if (selmon->drw.center_widget.callback_click) {
                    (*selmon->drw.center_widget.callback_click)(&selmon->drw.center_widget,
                            cursor_x - xpos, event->button);
                    return;
                }
            }
        }
        xpos += selmon->drw.center_widget_space;
        for (int i=selmon->drw.n_widgets_right-1; i>=0; --i) {
            if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.widgets_right[i].width) {
                click = ClkBar;
                if (selmon->drw.widgets_right[i].callback_click) {
                    (*selmon->drw.widgets_right[i].callback_click)(&selmon->drw.widgets_right[i],
                            cursor_x - xpos, event->button);
                    return;
                }
            }
            xpos += selmon->drw.widgets_right[i].width;
        }
    }

    switch (event->state) {
    case WL_POINTER_BUTTON_STATE_PRESSED:
        cursor_mode = CurPressed;
        setselmon(xytomon(cursor->x, cursor->y));
        if (locked)
            break;

        /* Change focus if the button was _pressed_ over a client */
        xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
        if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
            focusclient(c, 1);

        keyboard = wlr_seat_get_keyboard(seat);
        mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
        for (b = cfg->buttons; b < cfg->buttons + cfg->n_buttons; b++) {
            if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
                    event->button == b->button && b->func) {
                if (b->click == click) {
                    b->func(&b->arg);
                    return;
                }
            }
        }
        /* the library declines the modified clicks it has no use for */
        if (click == ClkRoot && !exclusive_focus && desktopat(cursor->x, cursor->y)
                && desktop_click(event->button, CLEANMASK(mods)))
            return;
        break;
    case WL_POINTER_BUTTON_STATE_RELEASED:
        /* If you released any buttons, we exit interactive move/resize mode. */
        /* TODO: should reset to the pointer focus's current setcursor */
        if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
            wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
            cursor_mode = CurNormal;
            /* Drop the window off on its new monitor */
            selmon = xytomon(cursor->x, cursor->y);
            setmon(grabc, selmon, 0);
            grabc = NULL;
            return;
        }
        cursor_mode = CurNormal;
        break;
    }

    /* If the event wasn't handled by the compositor, notify the client with
     * pointer focus that a button press has occurred */
    wlr_seat_pointer_notify_button(seat,
            event->time_msec, event->button, event->state);
}

void
chvt(const Arg *arg)
{
    wlr_session_change_vt(session, arg->ui);
}

void
checkidleinhibitor(struct wlr_surface *exclude)
{
    int inhibited = 0, unused_lx, unused_ly;
    struct wlr_idle_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
        struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
        struct wlr_scene_tree *tree = surface->data;
        if (exclude != surface && (cfg->bypass_surface_visibility || (!tree
                || wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
            inhibited = 1;
            break;
        }
    }

    wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

void
cleanup(void)
{
    struct timespec start, now;
    sigset_t quitsigs;

    if (redraw_source) wl_event_source_remove(redraw_source);
    if (lock_source) wl_event_source_remove(lock_source);
    if (logind_timer) wl_event_source_remove(logind_timer);
    if (covered_timer) wl_event_source_remove(covered_timer);
    lock_source = logind_timer = covered_timer = NULL;
    if (hover_timer) {
        wl_event_source_remove(hover_timer);
        hover_timer = NULL;
        hover_widget = NULL;
    }
    if (leave_timer) {
        wl_event_source_remove(leave_timer);
        leave_timer = NULL;
        hover_active = NULL;
    }
    if (wallpaper_timer) {
        wl_event_source_remove(wallpaper_timer);
        wallpaper_timer = NULL;
    }
    background_fini();
    redraw_source = NULL;

    /* Tear down the tray's own client connection to us before we start
     * force-destroying clients below, so its GTK/GDK thread shuts down
     * cleanly instead of racing wl_display_destroy_clients(). */
    awl_tray_shutdown();
    /* awl_tray_shutdown() only requests the tray thread to quit; it can't
     * safely block here itself. wl_display_run() (above, in run()) already
     * returned once quit() called wl_display_terminate(), so nothing is
     * dispatching our Wayland event loop any more -- if the tray thread
     * needs a response from us to finish tearing down its own Wayland
     * client connection cleanly (a frame callback, a configure ack, a
     * buffer release), a plain pthread_join() would deadlock forever
     * waiting for an answer nobody is left to send. Keep servicing the
     * event loop ourselves until the tray thread actually confirms it's
     * done -- wl_display_flush_clients() is required here too, not just
     * dispatch: replies generated while handling a client's request are
     * only queued into that client's write buffer, and are normally
     * flushed out by wl_display_run()'s own loop, which stopped running.
     * Without an explicit flush here, a client blocked in a synchronous
     * roundtrip (e.g. GTK's gtk_main()-exit gdk_flush()) waits forever on
     * a reply that was generated but never actually written to its
     * socket. */
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!awl_tray_join()) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000
                >= TRAY_STOP_MS) {
            /* Tearing the display down under a GTK thread that still uses
             * it could crash anywhere; the kernel cleans up just as well. */
            awl_wrn_printf("tray thread did not stop within %d ms, exiting without cleanup\n",
                    TRAY_STOP_MS);
            stopchildren();
            _exit(EXIT_FAILURE);
        }
        wl_display_flush_clients(dpy);
        wl_event_loop_dispatch(event_loop, 10);
    }

    /* That was the last time the event loop ran, so nothing reads the
     * signalfd any more: let SIGINT and SIGTERM end awl the usual way again
     * instead of leaving it to SIGKILL should anything below hang. */
    sigemptyset(&quitsigs);
    sigaddset(&quitsigs, SIGINT);
    sigaddset(&quitsigs, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &quitsigs, NULL);

    cleanuplisteners();
#ifdef XWAYLAND
    wlr_xwayland_destroy(xwayland);
    xwayland = NULL;
#endif
    wl_display_destroy_clients(dpy);
    stopchildren();
    wlr_xcursor_manager_destroy(cursor_mgr);

    destroykeyboardgroup(&kb_group->destroy, NULL);

    /* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
     * Destroy it until it's fixed on the wlroots side */
    wlr_backend_destroy(backend);

    wl_display_destroy(dpy);
    /* Destroy after the wayland display (when the monitors are already destroyed)
       to avoid destroying them with an invalid scene output. */
    textwinsdestroy();
    textwin_tree = NULL;
    wlr_scene_node_destroy(&scene->tree.node);
    awl_draw_fini();
    cfg = builtinconfig();
    awl_plugins_unload();
    awl_dict_destroy(cfgdict);
    cfgdict = NULL;
    /* only now nothing can request a redraw or a lock anymore */
    awl_redraw_fini();
    awl_lock_fini();
}

void
cleanupmon(struct wl_listener *listener, void *data)
{
    in_cleanupmon = 1;
    Monitor *m = wl_container_of(listener, m, destroy);
    LayerSurface *l, *tmp;
    TextWin *tw;
    size_t i;

    /* Tear down this monitor's own tray overlay window (if it ever got
     * one -- see tray/awl_tray_bridge.cpp), before m->wlr_output's name
     * becomes unavailable below. */
    awl_tray_remove_monitor(m->wlr_output->name);

    /* m->layers[i] are intentionally not unlinked */
    for (i = 0; i < LENGTH(m->layers); i++) {
        wl_list_for_each_safe(l, tmp, &m->layers[i], link)
            wlr_layer_surface_v1_destroy(l->layer_surface);
    }
    /* hover_widget points into m->drw */
    if (hover_widget && (char *)hover_widget >= (char *)&m->drw
            && (char *)hover_widget < (char *)(&m->drw + 1)) {
        hover_widget = NULL;
        wl_event_source_timer_update(hover_timer, 0);
    }
    if (hover_active && (char *)hover_active >= (char *)&m->drw
            && (char *)hover_active < (char *)(&m->drw + 1)) {
        setleavepending(0);
        hover_active = NULL;
    }
    awl_font_release(m->popupfont);
    m->popupfont = NULL;
    awl_font_release(m->desktopfont);
    m->desktopfont = NULL;
    /* closemon() below checks m->drw.m to decide whether to touch the bar */
    awl_draw_teardown(&m->drw);

    wl_list_remove(&m->destroy.link);
    wl_list_remove(&m->frame.link);
    wl_list_remove(&m->link);
    wl_list_remove(&m->request_state.link);
    if (m->lock_surface)
        destroylocksurface(&m->destroy_lock_surface, NULL);
    m->wlr_output->data = NULL;
    wlr_output_layout_remove(output_layout, m->wlr_output);
    wlr_scene_output_destroy(m->scene_output);

    closemon(m);
    wlr_scene_node_destroy(&m->fullscreen_bg->node);
    wlr_scene_node_destroy(&m->bar_blur->node);
    wlr_scene_node_destroy(&m->scene_buffer->node);
    desktop_removemon(m);
    background_removemon(m);
    wl_list_for_each(tw, &textwins, link)
        awl_window_removemon(tw->win, m);
    wlr_scene_node_destroy(&m->alert->node);
    for (i = 0; i < LENGTH(m->bar_bufs); i++)
        if (m->bar_bufs[i])
            wlr_buffer_drop(&m->bar_bufs[i]->base);
    free(m);
    in_cleanupmon = 0;
}

void
cleanuplisteners(void)
{
    wl_list_remove(&cursor_axis.link);
    wl_list_remove(&cursor_button.link);
    wl_list_remove(&cursor_frame.link);
    wl_list_remove(&cursor_motion.link);
    wl_list_remove(&cursor_motion_absolute.link);
    wl_list_remove(&gpu_reset.link);
    wl_list_remove(&new_idle_inhibitor.link);
    wl_list_remove(&layout_change.link);
    wl_list_remove(&new_input_device.link);
    wl_list_remove(&new_virtual_keyboard.link);
    wl_list_remove(&new_virtual_pointer.link);
    wl_list_remove(&new_pointer_constraint.link);
    wl_list_remove(&new_output.link);
    wl_list_remove(&new_xdg_toplevel.link);
    wl_list_remove(&new_xdg_decoration.link);
    wl_list_remove(&new_xdg_popup.link);
    wl_list_remove(&new_layer_surface.link);
    wl_list_remove(&output_mgr_apply.link);
    wl_list_remove(&output_mgr_test.link);
    wl_list_remove(&output_power_mgr_set_mode.link);
    wl_list_remove(&request_activate.link);
    wl_list_remove(&request_cursor.link);
    wl_list_remove(&request_set_psel.link);
    wl_list_remove(&request_set_sel.link);
    wl_list_remove(&request_set_cursor_shape.link);
    wl_list_remove(&request_start_drag.link);
    wl_list_remove(&start_drag.link);
    wl_list_remove(&new_session_lock.link);
#ifdef XWAYLAND
    /* Only registered in setup() if wlr_xwayland_create() succeeded;
     * removing an unregistered listener dereferences its NULL link. */
    if (xwayland) {
        wl_list_remove(&new_xwayland_surface.link);
        wl_list_remove(&xwayland_ready.link);
    }
#endif
}

void
closemon(Monitor *m)
{
    if (m && m->drw.m && m->showbar) { togglebar_mon(m); m->closedbar=1; }
    /* update selmon if needed and
     * move closed monitor's clients to the focused one */
    Client *c;
    int i = 0, nmons = wl_list_length(&mons);
    if (!nmons) {
        selmon = NULL;
    } else if (m == selmon) {
        do /* don't switch to disabled mons */
            selmon = wl_container_of(mons.next, selmon, link);
        while (!selmon->wlr_output->enabled && i++ < nmons);

        if (!selmon->wlr_output->enabled)
            selmon = NULL;
    }

    wl_list_for_each(c, &clients, link) {
        if (c->isfloating && c->geom.x > m->m.width)
            resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
                    .width = c->geom.width, .height = c->geom.height}, 0);
        if (c->mon == m)
            setmon(c, selmon, c->tags);
    }
    focusclient(focustop(selmon), 1);
    drawbars();
}

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
    LayerSurface *l = wl_container_of(listener, l, surface_commit);
    struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
    struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->current.layer]];
    struct wlr_layer_surface_v1_state old_state;

    if (l->layer_surface->initial_commit) {
        client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

        /* Temporarily set the layer's current state to pending
         * so that we can easily arrange it */
        old_state = l->layer_surface->current;
        l->layer_surface->current = l->layer_surface->pending;
        arrangelayers(l->mon);
        l->layer_surface->current = old_state;
        return;
    }

    if (layer_surface->current.committed == 0 && l->mapped == layer_surface->surface->mapped)
        return;
    l->mapped = layer_surface->surface->mapped;

    if (scene_layer != l->scene->node.parent) {
        wlr_scene_node_reparent(&l->scene->node, scene_layer);
        wl_list_remove(&l->link);
        wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
        wlr_scene_node_reparent(&l->popups->node, (layer_surface->current.layer
                < ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer));
    }

    arrangelayers(l->mon);
}

void
commitnotify(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, commit);

    if (c->surface.xdg->initial_commit) {
        /*
         * Get the monitor this client will be rendered on
         * Note that if the user set a rule in which the client is placed on
         * a different monitor based on its title, this will likely select
         * a wrong monitor.
         */
        applyrules(c);
        if (c->mon) {
            client_set_scale(client_surface(c), c->mon->wlr_output->scale);
        }
        setmon(c, NULL, 0); /* Make sure to reapply rules in mapnotify() */

        wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
        if (c->decoration)
            requestdecorationmode(&c->set_decoration_mode, c->decoration);
        wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
        return;
    }

    resize(c, c->geom, (c->isfloating && !c->isfullscreen));

    /* mark a pending resize as completed */
    if (c->resize && c->resize <= c->surface.xdg->current.configure_serial)
        c->resize = 0;
}

void
commitpopup(struct wl_listener *listener, void *data)
{
    struct wlr_surface *surface = data;
    struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
    LayerSurface *l = NULL;
    Client *c = NULL;
    struct wlr_box box;
    int type = -1;

    if (!popup || !popup->base->initial_commit)
        return;

    type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
    if (type < 0)
        return;

    if ((type == XDGShell && (!c || !c->mon || !c->scene)) ||
        (type == LayerShell && (!l || !l->mon || !l->scene))) {
        wlr_xdg_popup_destroy(popup);
        return;
    }

    struct wlr_scene_tree *parent_tree = NULL;
    struct wlr_xdg_surface *parent_xdg = wlr_xdg_surface_try_from_wlr_surface(popup->parent);

    if (parent_xdg && parent_xdg->surface->data) {
        struct wlr_scene_node *parent_node = parent_xdg->surface->data;
        parent_tree = wlr_scene_tree_from_node(parent_node);
    }

    // fallback
    if (!parent_tree)
        /* l->popups (not l->scene) is the tree commitlayersurfacenotify()
         * and createlayersurface() deliberately keep pinned to LyrTop (or
         * higher) even when the layer surface itself lives at LyrBottom/Bg,
         * so that popups (e.g. the tray's context menu) render above the
         * float layer instead of inheriting their parent's lower stacking
         * position. */
        parent_tree = (type == LayerShell) ? l->popups : c->scene;

    if (!parent_tree) {
        wlr_xdg_popup_destroy(popup);
        return;
    }

    // create popup in scene graph
    struct wlr_scene_tree *popup_tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
    if (!popup_tree) {
        wlr_xdg_popup_destroy(popup);
        return;
    }

    // save pointer for submenus
    popup->base->surface->data = &popup_tree->node;

    box = type == LayerShell ? l->mon->m : c->mon->w;
    box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x);
    box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y);
    wlr_xdg_popup_unconstrain_from_box(popup, &box);

    // do *NOT* free/remove from list, because there's a listener for that
    // (multiple allocation inhibited through the !popup->base->initial_commit
    // check
}


void
createdecoration(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    Client *c = deco->toplevel->base->data;
    c->decoration = deco;

    LISTEN(&deco->events.request_mode, &c->set_decoration_mode, requestdecorationmode);
    LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

    requestdecorationmode(&c->set_decoration_mode, deco);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
    struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
    LISTEN_STATIC(&idle_inhibitor->events.destroy, destroyidleinhibitor);

    checkidleinhibitor(NULL);
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
    /* Set the keymap to match the group keymap */
    wlr_keyboard_set_keymap(keyboard, kb_group->wlr_group->keyboard.keymap);

    /* Add the new keyboard to the group */
    wlr_keyboard_group_add_keyboard(kb_group->wlr_group, keyboard);
}

struct xkb_keymap *
compilekeymap(void)
{
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context,
            cfg->xkb_rules, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    xkb_context_unref(context);
    return keymap;
}

KeyboardGroup *
createkeyboardgroup(void)
{
    KeyboardGroup *group = ecalloc(1, sizeof(*group));
    struct xkb_keymap *keymap;

    group->wlr_group = wlr_keyboard_group_create();
    group->wlr_group->data = group;

    /* Prepare an XKB keymap and assign it to the keyboard group: the one in
     * effect, which is config_plugins.h's unless a reload brought a broken one */
    if (kb_group)
        keymap = xkb_keymap_ref(kb_group->wlr_group->keyboard.keymap);
    else if (!(keymap = compilekeymap()))
        die("failed to compile keymap");

    wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
    xkb_keymap_unref(keymap);

    wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard,
            kb_group ? kb_group->wlr_group->keyboard.repeat_info.rate : cfg->repeat_rate,
            kb_group ? kb_group->wlr_group->keyboard.repeat_info.delay : cfg->repeat_delay);

    /* Set up listeners for keyboard events */
    LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
    LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

    group->key_repeat_source = wl_event_loop_add_timer(event_loop, keyrepeat, group);

    /* A seat can only have one keyboard, but this is a limitation of the
     * Wayland protocol - not wlroots. We assign all connected keyboards to the
     * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
     * all of them. Set this combined wlr_keyboard as the seat keyboard.
     */
    wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
    return group;
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
    struct wlr_layer_surface_v1 *layer_surface = data;
    LayerSurface *l;
    struct wlr_surface *surface = layer_surface->surface;
    struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->pending.layer]];

    /* The tray (tray/awl_tray_bridge.cpp) opens one layer-shell window per
     * monitor and requests no output of its own -- instead it encodes which
     * monitor it belongs to in its namespace as "awl-tray:<monitor_id>"
     * (monitor_id being m->wlr_output->name) so we can bind it to exactly
     * that output here, before the generic "no output requested -> selmon"
     * fallback below would otherwise land every monitor's tray window on
     * whichever one happens to be selmon. */
    if (!layer_surface->output && layer_surface->namespace &&
            !strncmp(layer_surface->namespace, "awl-tray:", 9)) {
        const char *want = strchr(layer_surface->namespace, ':') + 1;
        Monitor *tm;
        wl_list_for_each(tm, &mons, link) {
            if (!strcmp(tm->wlr_output->name, want)) {
                layer_surface->output = tm->wlr_output;
                break;
            }
        }
    }

    if (!layer_surface->output
            && !(layer_surface->output = selmon ? selmon->wlr_output : NULL)) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }
    l = layer_surface->data = ecalloc(1, sizeof(*l));
    l->is_notification = cfg->blur_notifications && !strcmp(layer_surface->namespace, "notifications");
    l->is_launcher = cfg->blur_launcher && !strcmp(layer_surface->namespace, "launcher");
    l->type = LayerShell;
    LISTEN(&surface->events.commit, &l->surface_commit, commitlayersurfacenotify);
    LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);
    LISTEN(&layer_surface->events.destroy, &l->destroy, destroylayersurfacenotify);

    l->layer_surface = layer_surface;
    l->mon = layer_surface->output->data;
    l->scene_layer = wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
    l->scene = l->scene_layer->tree;
    if (l->is_notification || l->is_launcher) {
        l->blur = wlr_scene_blur_create(l->scene, l->scene->node.x, l->scene->node.y);
        if (l->is_launcher) wlr_scene_blur_set_corner_radius(l->blur, cfg->blur_launcher_radius);
        if (l->is_notification) wlr_scene_blur_set_corner_radius(l->blur, cfg->blur_notifications_radius);
        wlr_scene_blur_set_size(l->blur, l->layer_surface->current.desired_width, l->layer_surface->current.desired_height);
        wlr_scene_blur_set_strength(l->blur, cfg->blur[0]);
        wlr_scene_blur_set_alpha(l->blur, cfg->blur[1]);
        wlr_scene_blur_set_should_only_blur_bottom_layer(l->blur, 0);
        wlr_scene_node_set_enabled(&l->blur->node, 1);
        wlr_scene_node_lower_to_bottom(&l->blur->node);
    }

    l->popups = surface->data = wlr_scene_tree_create(layer_surface->current.layer
            < ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer);
    l->scene->node.data = l->popups->node.data = l;

    wl_list_insert(&l->mon->layers[layer_surface->pending.layer],&l->link);
    wlr_surface_send_enter(surface, layer_surface->output);
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
createmon(struct wl_listener *listener, void *data)
{
    /* This event is raised by the backend when a new output (aka a display or
     * monitor) becomes available. */
    struct wlr_output *wlr_output = data;
    const MonitorRule *r;
    size_t i;
    struct wlr_output_state state;
    Monitor *m;

    if (!wlr_output_init_render(wlr_output, alloc, drw))
        return;

    m = wlr_output->data = ecalloc(1, sizeof(*m));
    m->wlr_output = wlr_output;

    for (i = 0; i < LENGTH(m->layers); i++)
        wl_list_init(&m->layers[i]);

    wlr_output_state_init(&state);
    /* Initialize monitor state using configured rules */
    m->tagset[0] = m->tagset[1] = 1;
    /* if no rule matches */
    m->m.x = m->m.y = -1;
    m->ptlt[0].mfact = 0.55f;
    m->ptlt[0].nmaster = 1;
    m->ptlt[0].lt[0] = m->ptlt[0].lt[1] = &cfg->layouts[0];
    for (r = cfg->monrules; r < cfg->monrules + cfg->n_monrules; r++) {
        if (!r->name || strstr(wlr_output->name, r->name)) {
            m->m.x = r->x;
            m->m.y = r->y;
            m->ptlt[0].mfact = r->mfact;
            m->ptlt[0].nmaster = r->nmaster;
            if (layoutindex(r->lt) >= 0)
                m->ptlt[0].lt[0] = r->lt;
            m->ptlt[0].lt[1] = &cfg->layouts[cfg->n_layouts > 1 && r->lt != &cfg->layouts[1]];
            m->pertag = !!r->pertag;
            wlr_output_state_set_scale(&state, r->scale);
            wlr_output_state_set_transform(&state, r->rr);
            break;
        }
    }
    /* every view starts with the monitor's layout */
    for (i = 1; i < LENGTH(m->ptlt); i++)
        m->ptlt[i] = m->ptlt[0];
    strncpy(m->ltsymbol, sellayout(m)->symbol, LENGTH(m->ltsymbol)-1);

    /* The mode is a tuple of (width, height, refresh rate), and each
     * monitor supports only a specific set of modes. We just pick the
     * monitor's preferred mode; a more sophisticated compositor would let
     * the user configure it. */
    wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr_output));

    /* Set up event listeners */
    LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
    LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
    LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);

    wlr_output_state_set_enabled(&state, 1);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    awl_draw_setup(&m->drw, m);
    awl_plugins_bar_widgets(&m->drw);

    /* LyrBottom sits below LyrTile/LyrFloat, so floating windows dragged
     * over the bar's area render on top of it -- intentional: the tray
     * overlay window (a real layer-shell client requesting the "bottom"
     * protocol layer -- see gtk_layer_set_layer() in
     * tray/awl_tray_bridge.cpp) also lands in LyrBottom via layermap[],
     * so floating windows render above both. */
    m->scene_buffer = wlr_scene_buffer_create(layers[LyrBottom], NULL);
    m->scene_buffer->point_accepts_input = bar_accepts_input;
    /* frosts the window list's translucent entries; drawbar() fits it and
     * enables it with cfg->blur_windowlist */
    m->bar_blur = wlr_scene_blur_create(layers[LyrBottom], 0, 0);
    wlr_scene_node_place_below(&m->bar_blur->node, &m->scene_buffer->node);
    wlr_scene_blur_set_strength(m->bar_blur, cfg->blur[0]);
    wlr_scene_blur_set_alpha(m->bar_blur, cfg->blur[1]);

    m->showbar = showbar;
    updatebar(m);
    drawbar(m);
    desktop_addmon(m);
    background_addmon(m);
    m->alert = wlr_scene_buffer_create(alert_tree, NULL);
    m->alert->point_accepts_input = alert_accepts_input;
    wlr_scene_buffer_set_opacity(m->alert, ALERT_ALPHA);
    wlr_scene_node_set_enabled(&m->alert->node, 0);
    m->alert_version = (unsigned int)-1;

    wl_list_insert(&mons, &m->link);

    /* The xdg-protocol specifies:
     *
     * If the fullscreened surface is not opaque, the compositor must make
     * sure that other screen content not part of the same surface tree (made
     * up of subsurfaces, popups or similarly coupled surfaces) are not
     * visible below the fullscreened surface.
     *
     */
    /* updatemons() will resize and set correct position */
    m->fullscreen_bg = wlr_scene_rect_create(layers[LyrFS], 0, 0, cfg->fullscreen_bg);
    wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

    /* Adds this to the output layout in the order it was configured.
     *
     * The output layout utility automatically adds a wl_output global to the
     * display, which Wayland clients can see to find out information about the
     * output (such as DPI, scale factor, manufacturer, etc).
     */
    m->scene_output = wlr_scene_output_create(scene, wlr_output);
    if (m->m.x == -1 && m->m.y == -1)
        wlr_output_layout_add_auto(output_layout, wlr_output);
    else
        wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);

}

void
createnotify(struct wl_listener *listener, void *data)
{
    /* This event is raised when a client creates a new toplevel (application window). */
    struct wlr_xdg_toplevel *toplevel = data;
    Client *c = NULL;

    /* Allocate a Client for this surface */
    c = toplevel->base->data = ecalloc(1, sizeof(*c));
    c->surface.xdg = toplevel->base;
    c->bw = cfg->borderpx;
    c->isvisible = 1;

    LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
    LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
    LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
    LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
    LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
    LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
    LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

void
configurepointer(struct wlr_pointer *pointer)
{
    struct libinput_device *device;
    if (wlr_input_device_is_libinput(&pointer->base)
            && (device = wlr_libinput_get_device_handle(&pointer->base))) {

        if (libinput_device_config_tap_get_finger_count(device)) {
            libinput_device_config_tap_set_enabled(device, cfg->tap_to_click);
            libinput_device_config_tap_set_drag_enabled(device, cfg->tap_and_drag);
            libinput_device_config_tap_set_drag_lock_enabled(device, cfg->drag_lock);
            libinput_device_config_tap_set_button_map(device, cfg->button_map);
        }

        if (libinput_device_config_scroll_has_natural_scroll(device))
            libinput_device_config_scroll_set_natural_scroll_enabled(device, cfg->natural_scrolling);

        if (libinput_device_config_dwt_is_available(device))
            libinput_device_config_dwt_set_enabled(device, cfg->disable_while_typing);

        if (libinput_device_config_left_handed_is_available(device))
            libinput_device_config_left_handed_set(device, cfg->left_handed);

        if (libinput_device_config_middle_emulation_is_available(device))
            libinput_device_config_middle_emulation_set_enabled(device, cfg->middle_button_emulation);

        if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
            libinput_device_config_scroll_set_method(device, cfg->scroll_method);

        if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
            libinput_device_config_click_set_method(device, cfg->click_method);

        if (libinput_device_config_send_events_get_modes(device))
            libinput_device_config_send_events_set_mode(device, cfg->send_events_mode);

        if (libinput_device_config_accel_is_available(device)) {
            libinput_device_config_accel_set_profile(device, cfg->accel_profile);
            libinput_device_config_accel_set_speed(device, cfg->accel_speed);
        }
    }
}

void
createpointer(struct wlr_pointer *pointer)
{
    configurepointer(pointer);
    wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
    PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
    pointer_constraint->constraint = data;
    LISTEN(&pointer_constraint->constraint->events.destroy,
            &pointer_constraint->destroy, destroypointerconstraint);
}

// helper struct to free the dynamical listener upon surface destruction
struct popup_listener {
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void
destroypopuplistener(struct wl_listener *listener, void *data)
{
    struct popup_listener *p_listener = wl_container_of(listener, p_listener, destroy);
    wl_list_remove(&p_listener->commit.link);
    wl_list_remove(&p_listener->destroy.link);
    free(p_listener);
}

void
createpopup(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_popup *popup = data;

    struct popup_listener *p_listener = calloc(1, sizeof*p_listener);
    if (!p_listener) return;

    p_listener->commit.notify = commitpopup;
    wl_signal_add(&popup->base->surface->events.commit, &p_listener->commit);

    p_listener->destroy.notify = destroypopuplistener;
    wl_signal_add(&popup->base->surface->events.destroy, &p_listener->destroy);
}


void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
    if (active_constraint == constraint)
        return;

    if (active_constraint)
        wlr_pointer_constraint_v1_send_deactivated(active_constraint);

    active_constraint = constraint;
    wlr_pointer_constraint_v1_send_activated(constraint);
}

void
cursorframe(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits a frame
     * event. Frame events are sent after regular pointer events to group
     * multiple events together. For instance, two axis events may happen at the
     * same time, in which case a frame event won't be sent in between. */
    /* Notify the client with pointer focus of the frame event. */
    wlr_seat_pointer_notify_frame(seat);
}

void
cursorwarptohint(void)
{
    Client *c = NULL;
    double sx = active_constraint->current.cursor_hint.x;
    double sy = active_constraint->current.cursor_hint.y;

    toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
    if (c && active_constraint->current.cursor_hint.enabled) {
        wlr_cursor_warp(cursor, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
        wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
    }
}

void
destroydecoration(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, destroy_decoration);

    wl_list_remove(&c->destroy_decoration.link);
    wl_list_remove(&c->set_decoration_mode.link);
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
    /* Focus enter isn't sent during drag, so refocus the focused node. */
    focusclient(focustop(selmon), 1);
    motionnotify(0, NULL, 0, 0, 0, 0);
    wl_list_remove(&listener->link);
    free(listener);
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
    /* `data` is the wlr_surface of the idle inhibitor being destroyed,
     * at this point the idle inhibitor is still in the list of the manager */
    checkidleinhibitor(wlr_surface_get_root_surface(data));
    wl_list_remove(&listener->link);
    free(listener);
}

void
destroylayersurfacenotify(struct wl_listener *listener, void *data)
{
    LayerSurface *l = wl_container_of(listener, l, destroy);

    wl_list_remove(&l->link);
    wl_list_remove(&l->destroy.link);
    wl_list_remove(&l->unmap.link);
    wl_list_remove(&l->surface_commit.link);
    wlr_scene_node_destroy(&l->scene->node);
    wlr_scene_node_destroy(&l->popups->node);
    free(l);
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

    /* catch up on everything skipped while locked (also re-places the tray
     * via systray_draw()) */
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
destroynotify(struct wl_listener *listener, void *data)
{
    /* Called when the xdg_toplevel is destroyed. */
    Client *c = wl_container_of(listener, c, destroy);
    wl_list_remove(&c->destroy.link);
    wl_list_remove(&c->set_title.link);
    wl_list_remove(&c->fullscreen.link);
#ifdef XWAYLAND
    if (c->type != XDGShell) {
        wl_list_remove(&c->activate.link);
        wl_list_remove(&c->associate.link);
        wl_list_remove(&c->configure.link);
        wl_list_remove(&c->dissociate.link);
        wl_list_remove(&c->set_hints.link);
    } else
#endif
    {
        wl_list_remove(&c->commit.link);
        wl_list_remove(&c->map.link);
        wl_list_remove(&c->unmap.link);
        wl_list_remove(&c->maximize.link);
    }
    free(c);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
    PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

    if (active_constraint == pointer_constraint->constraint) {
        cursorwarptohint();
        active_constraint = NULL;
    }

    wl_list_remove(&pointer_constraint->destroy.link);
    free(pointer_constraint);
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
    SessionLock *lock = wl_container_of(listener, lock, destroy);
    destroylock(lock, 0);
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
    KeyboardGroup *group = wl_container_of(listener, group, destroy);
    wl_event_source_remove(group->key_repeat_source);
    wl_list_remove(&group->key.link);
    wl_list_remove(&group->modifiers.link);
    wl_list_remove(&group->destroy.link);
    wlr_keyboard_group_destroy(group->wlr_group);
    free(group);
}

void
drawbar(Monitor *m)
{
    if (in_cleanupmon) return;
    /* nothing of the bar is visible under the lock screen, but every new
     * buffer still damages the output (and the full-screen locked_bg_blur
     * above it); destroylock() redraws all bars once on unlock */
    if (locked) return;
    if (!m) return;
    if (!m->drw.m) return;
    int x = 0;
    uint32_t occ = 0, urg = 0;
    Client *c;
    Buffer *buf;
    int oneoff;
    pixman_region32_t damage;

    Client* ct;

    memset( m->drw.tagwindows, 0, sizeof(m->drw.tagwindows) );
    m->drw.n_tagwindows = 0;

    if (!m->showbar)
        return;

    /* fall back to a throwaway buffer if the scene holds both ring slots */
    if ((oneoff = !(buf = barbuffer(m))))
        buf = newbarbuffer(m);
    memset(buf->data, 0, buf->stride * buf->h);

    awl_draw_prepare_drawing(&m->drw, m->b.width, m->b.height, buf->data, buf->stride);

    ct = focustop(m);
    wl_list_for_each(c, &clients, link) {
        if (c->mon != m)
            continue;
        occ |= c->tags;
        if (c->isurgent)
            urg |= c->tags;
        if ( (c->tags & m->tagset[m->seltags]) && (m->drw.n_tagwindows < (int)LENGTH(m->drw.tagwindows)) ) {
            strncpy( m->drw.tagwindows[m->drw.n_tagwindows].name, client_get_title(c),
                    sizeof(m->drw.tagwindows[m->drw.n_tagwindows].name)-1 );
            m->drw.tagwindows[m->drw.n_tagwindows].floating = c->isfloating;
            m->drw.tagwindows[m->drw.n_tagwindows].urgent = c->isurgent;
            m->drw.tagwindows[m->drw.n_tagwindows].fullscreen = c->isfullscreen;
            m->drw.tagwindows[m->drw.n_tagwindows].visible = c->isvisible;
            m->drw.tagwindows[m->drw.n_tagwindows].maximized = c->ismaximized;
            m->drw.tagwindows[m->drw.n_tagwindows].ontop = c->isontop;
            m->drw.tagwindows[m->drw.n_tagwindows].focused = (c == ct);
            m->drw.tagwindows[m->drw.n_tagwindows].c = c;
            m->drw.n_tagwindows++;
        }
    }
    m->drw.occ = occ;
    m->drw.urg = urg;
    m->drw.sel = m->tagset[m->seltags];
    m->drw.ntags = LENGTH(tags);
    m->drw.selmon = (m == selmon);
    if (wl_list_length(&mons) == 1) m->drw.selmon = -1;
    m->drw.pertag = m->pertag;

    for (int ww=0; ww<m->drw.n_widgets_left; ++ww) {
        if (m->drw.widgets_left[ww].draw)
            m->drw.widgets_left[ww].width = m->drw.widgets_left[ww].draw(
                    &m->drw.widgets_left[ww], x, m->drw.pix );
        x += m->drw.widgets_left[ww].width;
    }
    m->drw.center_widget_start = x;

    uint32_t x_end = m->b.width;
    for (int ww=0; ww<m->drw.n_widgets_right; ++ww) {
        widget_t *w = &m->drw.widgets_right[ww];
        /* width first, so the widget lands at its final position right away */
        w->width = w->measure ? w->measure(w) : 0;
        x_end = w->width < x_end ? x_end - w->width : 0;
        if (w->draw && w->width)
            w->draw(w, x_end, m->drw.pix);
    }

    m->drw.center_widget_space = x_end > m->drw.center_widget_start ? x_end - m->drw.center_widget_start : 0;
    if (m->drw.has_center_widget) {
        if (m->drw.center_widget.draw)
            m->drw.center_widget.width = m->drw.center_widget.draw( &m->drw.center_widget, x, m->drw.pix );
    }

    awl_draw_finish_drawing(&m->drw);
    wlr_scene_buffer_set_dest_size(m->scene_buffer,
        m->b.real_width, m->b.real_height);
    wlr_scene_node_set_position(&m->scene_buffer->node, m->m.x,
        m->m.y + (topbar ? 0 : m->m.height - m->b.real_height));
    /* the window list's box, from buffer to logical pixels */
    if (m->b.width) {
        uint32_t cx = m->drw.has_center_widget ? m->drw.center_widget_start : 0,
                 cw = m->drw.has_center_widget ? m->drw.center_widget.width : 0;
        int bx = (int)((uint64_t)cx * m->b.real_width / m->b.width),
            bw = (int)((uint64_t)(cx + cw) * m->b.real_width / m->b.width) - bx;
        wlr_scene_blur_set_size(m->bar_blur, bw, m->b.real_height);
        wlr_scene_node_set_position(&m->bar_blur->node, m->m.x + bx,
            m->m.y + (topbar ? 0 : m->m.height - m->b.real_height));
        wlr_scene_node_set_enabled(&m->bar_blur->node, cfg->blur_windowlist && bw > 0);
    }
    /* only hand the scene a new buffer if the pixels actually changed, and
     * only damage the output where they did (usually just the clock and the
     * graphs); buf stays unlocked otherwise and is reused next time */
    pixman_region32_init(&damage);
    if (bardamage(buf, m->bar_shown, &damage)) {
        wlr_scene_buffer_set_buffer_with_damage(m->scene_buffer, &buf->base,
                m->bar_shown ? &damage : NULL);
        /* a one-off buffer is gone once the scene has uploaded it */
        m->bar_shown = oneoff ? NULL : buf;
    }
    pixman_region32_fini(&damage);
    if (oneoff)
        wlr_buffer_drop(&buf->base);
}

/** the popups' look, from config_plugins.h */
static awl_window_style_t
textwinstyle(void)
{
    return (awl_window_style_t){
            .bg = cfg->popup->bg, .fg = cfg->popup->fg,
            .border = cfg->popup->border,
            /* fg, faint */
            .highlight = (cfg->popup->fg & 0xffffff00) | 0x30,
            .border_width = cfg->popup->border_width,
            .radius = cfg->popup->radius, .padding = cfg->popup->padding,
            .margin = cfg->popup->margin,
            .blur = cfg->blur_launcher,
            .blur_strength = cfg->blur[0], .blur_alpha = cfg->blur[1] };
}

/** win's entry in textwins, NULL if it is not (or no longer) one of them */
static TextWin *
textwinfind(awl_window_t *win)
{
    TextWin *tw;
    if (win)
        wl_list_for_each(tw, &textwins, link)
            if (tw->win == win)
                return tw;
    return NULL;
}

awl_window_t *
textwincreate(void)
{
    TextWin *tw;
    awl_window_style_t style;
    if (!scene)
        return NULL;
    /* over the bar and the windows, under fullscreen ones */
    if (!textwin_tree) {
        textwin_tree = wlr_scene_tree_create(&scene->tree);
        wlr_scene_node_place_above(&textwin_tree->node, &layers[LyrTop]->node);
    }
    style = textwinstyle();
    tw = ecalloc(1, sizeof(*tw));
    tw->win = awl_window_create(textwin_tree, &style);
    wl_list_insert(textwins.prev, &tw->link);
    return tw->win;
}

void
textwindestroy(awl_window_t *win)
{
    TextWin *tw = textwinfind(win);
    if (!tw)
        return;
    for (size_t i = 0; i < LENGTH(testwins); i++)
        if (testwins[i] == win)
            testwins[i] = NULL;
    if (textwin_hovered == win)
        textwin_hovered = NULL;
    wl_list_remove(&tw->link);
    awl_window_destroy(tw->win);
    free(tw);
}

void
textwinshow(awl_window_t *win, Monitor *m, int center_x, const char *text, int toggle)
{
    if (!m || !textwinfind(win))
        return;
    if (toggle && awl_window_monitor(win) == m) {
        awl_window_hide(win);
        return;
    }
    awl_window_set_text(win, text);
    awl_window_show(win, m, center_x, topbar);
}

void
textwinupdate(awl_window_t *win, const char *text)
{
    if (textwinfind(win) && awl_window_monitor(win))
        awl_window_set_text(win, text);
}

void
textwinhide(awl_window_t *win)
{
    if (textwinfind(win))
        awl_window_hide(win);
}

void
textwinsetclick(awl_window_t *win,
        void (*click)(awl_window_t *win, int row, int col, uint32_t button, void *data), void *data)
{
    if (textwinfind(win))
        awl_window_set_click(win, click, data);
}

void
textwinsetscroll(awl_window_t *win,
        void (*scroll)(awl_window_t *win, int row, int col, uint32_t orientation,
                double delta, int32_t value120, void *data), void *data)
{
    if (textwinfind(win))
        awl_window_set_scroll(win, scroll, data);
}

void
textwinsetpainter(awl_window_t *win, const awl_window_painter_t *painter, void *data)
{
    if (textwinfind(win))
        awl_window_set_painter(win, painter, data);
}

void
textwinredraw(awl_window_t *win)
{
    if (textwinfind(win))
        awl_window_redraw(win);
}

Monitor *
textwinmonitor(awl_window_t *win)
{
    return textwinfind(win) ? awl_window_monitor(win) : NULL;
}

/** the topmost popup that takes the pointer at x, y, NULL if none */
static awl_window_t *
textwinat(double x, double y)
{
    TextWin *tw;
    /* the newest is on top */
    wl_list_for_each_reverse(tw, &textwins, link)
        if (awl_window_takes_pointer(tw->win, x, y))
            return tw->win;
    return NULL;
}

/** the popup that has the pointer: textwinat() the cursor, unless the
 * screen is locked or the pointer busy (moving, resizing, pressed on a
 * client, dragging) */
static awl_window_t *
textwinpointer(void)
{
    if (cursor_mode != CurNormal || seat->drag || locked)
        return NULL;
    return textwinat(cursor->x, cursor->y);
}

/** The pointer moved: over a popup that takes it, that popup gets it and
 * nothing below does; returns whether it did. Logs (at verbose, -d) where
 * it goes. */
static int
textwinmotion(void)
{
    awl_window_t *win = textwinpointer();
    int row;

    if (textwin_hovered && textwin_hovered != win) {
        awl_window_pointer_leave(textwin_hovered);
        awl_dbg_printf(awl_loglevel_vrb, "textwin %p: pointer left\n", (void *)textwin_hovered);
    }
    if (!win) {
        textwin_hovered = NULL;
        return 0;
    }
    if (textwin_hovered != win) {
        wlr_seat_pointer_notify_clear_focus(seat);
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
        updatehover(NULL);
        textwin_hovered = win;
    }
    row = awl_window_hover_row(win);
    if (awl_window_pointer_motion(win, cursor->x, cursor->y) != row)
        awl_dbg_printf(awl_loglevel_vrb, "textwin %p: pointer on row %d at %.0f,%.0f\n", (void *)win,
                awl_window_hover_row(win), cursor->x, cursor->y);
    return 1;
}

/** m was arranged, or its font or scale changed */
static void
textwinsupdate(Monitor *m)
{
    TextWin *tw;
    wl_list_for_each(tw, &textwins, link)
        awl_window_update(tw->win, m);
}

static void
textwinshideall(void)
{
    TextWin *tw;
    wl_list_for_each(tw, &textwins, link)
        awl_window_hide(tw->win);
}

/** the ones the plugins did not destroy themselves */
static void
textwinsdestroy(void)
{
    TextWin *tw, *tmp;
    wl_list_for_each_safe(tw, tmp, &textwins, link)
        textwindestroy(tw->win);
}

void
drawalerts(void)
{
    Monitor *m;
    wl_list_for_each(m, &mons, link)
        drawalert(m);
}

/** clicks go through the alert */
static bool
alert_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    return false;
}

/** The alert (awl_alert.h) on m: white on red at ALERT_ALPHA, in the
 * bar's font at ALERT_FONT_SCALE times its size, in the bottom left corner,
 * ALERT_MARGIN logical pixels from the edges, above everything, the lock
 * screen included. Redrawn only when the alert, m's area or its scale
 * changed; called on every redraw request and whenever m is arranged. */
void
drawalert(Monitor *m)
{
    static const pixman_color_t fg = {0xffff, 0xffff, 0xffff, 0xffff},
                                bg = {0xc000, 0x0000, 0x0000, 0xffff};
    const char *text = awl_alert_text();
    const float s = m->wlr_output->scale;
    unsigned int version = awl_alert_version();
    struct fcft_font *font;
    pixman_image_t *pix;
    Buffer *buf;
    int pad, w, h;

    if (!m->alert) /* createmon() arranges before it makes it */
        return;
    if (m->alert_version == version && m->alert_scale == s
            && wlr_box_equal(&m->alert_area, &m->m))
        return;
    m->alert_version = version;
    m->alert_scale = s;
    m->alert_area = m->m;
    if (!text || !m->wlr_output->enabled || m->m.width <= 0) {
        wlr_scene_node_set_enabled(&m->alert->node, 0);
        return;
    }

    /* the font only while drawing; the registry keeps it loaded */
    if (!(font = awl_font_get(cfg->font, cfg->fontsize * ALERT_FONT_SCALE, s))) {
        wlr_scene_node_set_enabled(&m->alert->node, 0);
        return;
    }
    pad = font->height * 6 / 10;
    h = font->height + pad;
    w = (int)awl_draw_text_width(font, text) + 2 * pad;
    if (w > (int)(m->m.width * s) - 2 * (int)(ALERT_MARGIN * s))
        w = (int)(m->m.width * s) - 2 * (int)(ALERT_MARGIN * s);
    if (w <= 0) {
        awl_font_release(font);
        wlr_scene_node_set_enabled(&m->alert->node, 0);
        return;
    }

    buf = ecalloc(1, sizeof(Buffer) + (size_t)awl_draw_stride(w) * h);
    buf->stride = awl_draw_stride(w);
    buf->w = w;
    buf->h = h;
    wlr_buffer_init(&buf->base, &buffer_impl, w, h);
    if ((pix = awl_draw_image(w, h, buf->data, buf->stride))) {
        awl_draw_text(pix, font, 0, 0, w, h, pad, text, fg, bg);
        pixman_image_unref(pix);
    }
    awl_font_release(font);

    wlr_scene_buffer_set_buffer(m->alert, &buf->base);
    wlr_buffer_drop(&buf->base); /* the scene holds it now */
    wlr_scene_buffer_set_dest_size(m->alert, (int)(w / s), (int)(h / s));
    wlr_scene_node_set_position(&m->alert->node, m->m.x + ALERT_MARGIN,
            m->m.y + m->m.height - ALERT_MARGIN - (int)(h / s));
    wlr_scene_node_raise_to_top(&alert_tree->node);
    wlr_scene_node_set_enabled(&m->alert->node, 1);
}

Buffer *
newbarbuffer(Monitor *m)
{
    int32_t stride = awl_draw_stride(m->b.width);
    Buffer *buf = ecalloc(1, sizeof(Buffer) + (size_t)stride * m->b.height);
    buf->stride = stride;
    buf->w = m->b.width;
    buf->h = m->b.height;
    wlr_buffer_init(&buf->base, &buffer_impl, m->b.width, m->b.height);
    return buf;
}

Buffer *
barbuffer(Monitor *m)
{
    /* The scene locks a buffer from set_buffer until it has copied it into a
     * texture on the next rendered frame, then unlocks it again. Any unlocked
     * slot is therefore free to draw into, except the one shown: drawbar()
     * compares the new pixels with it. The slots are only dropped in
     * cleanupmon(). */
    size_t i;
    Buffer *buf;

    for (i = 0; i < LENGTH(m->bar_bufs); i++) {
        buf = m->bar_bufs[i];
        if (buf && (buf->base.n_locks || buf == m->bar_shown))
            continue;
        if (buf && (buf->w != (size_t)m->b.width || buf->h != (size_t)m->b.height)) {
            wlr_buffer_drop(&buf->base);
            buf = NULL;
        }
        if (!buf)
            buf = m->bar_bufs[i] = newbarbuffer(m);
        return buf;
    }
    return NULL;
}

int
bardamage(const Buffer *buf, const Buffer *prev, pixman_region32_t *damage)
{
    /* Collects the columns of buf that differ from prev, in BARTILE-pixel
     * steps and over the full bar height, into damage; returns whether
     * anything differs. Without a comparable prev, everything does. */
    enum { BARTILE = 32 };
    size_t x, y, x0, run = 0, w = buf->w, h = buf->h, ntiles = (w + BARTILE - 1) / BARTILE;
    uint8_t dirty[ntiles ? ntiles : 1];
    int any = 0;

    if (!prev || prev->w != w || prev->h != h || prev->stride != buf->stride) {
        pixman_region32_union_rect(damage, damage, 0, 0, w, h);
        return 1;
    }
    memset(dirty, 0, ntiles);
    for (y = 0; y < h; y++) {
        const uint32_t *a = (const uint32_t *)((const char *)buf->data + y * buf->stride);
        const uint32_t *b = (const uint32_t *)((const char *)prev->data + y * prev->stride);
        if (!memcmp(a, b, w * sizeof(*a)))
            continue;
        for (x = 0; x < ntiles; x++) {
            size_t n = x + 1 < ntiles ? BARTILE : w - x * BARTILE;
            if (!dirty[x] && memcmp(a + x * BARTILE, b + x * BARTILE, n * sizeof(*a)))
                dirty[x] = any = 1;
        }
    }
    /* one rectangle per run of changed tiles */
    for (x = 0; x <= ntiles; x++) {
        if (x < ntiles && dirty[x]) {
            run++;
            continue;
        }
        if (run) {
            x0 = (x - run) * BARTILE;
            pixman_region32_union_rect(damage, damage, x0, 0,
                    (x * BARTILE < w ? x * BARTILE : w) - x0, h);
            run = 0;
        }
    }
    return any;
}

void
drawbars(void)
{
    if (in_cleanupmon) return;
    Monitor *m = NULL;

    wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->enabled) drawbar(m);
    }
}

void
focusclient(Client *c, int lift)
{
    struct wlr_surface *old = seat->keyboard_state.focused_surface;
    int unused_lx, unused_ly, old_client_type;
    Client *old_c = NULL;
    LayerSurface *old_l = NULL;

    if (locked)
        return;

    /* Raise client in stacking order if requested */
    if (c && lift)
        wlr_scene_node_raise_to_top(&c->scene->node);

    if (c && client_surface(c) == old)
        return;

    if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
        struct wlr_xdg_popup *popup, *tmp;
        wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
            wlr_xdg_popup_destroy(popup);
    }

    /* Put the new client atop the focus stack and select its monitor */
    if (c && !client_is_unmanaged(c)) {
        wl_list_remove(&c->flink);
        wl_list_insert(&fstack, &c->flink);
        selmon = c->mon;
        c->isurgent = 0;

        /* Don't change border color if there is an exclusive focus or we are
         * handling a drag operation */
        if (!exclusive_focus && !seat->drag)
            client_set_border_color(c, (float[])COLOR(borders[BorderSel]));
    }

    /* Deactivate old client if focus is changing */
    if (old && (!c || client_surface(c) != old)) {
        /* If an overlay is focused, don't focus or activate the client,
         * but only update its position in fstack to render its border with its color
         * and focus it after the overlay is closed. */
        if (old_client_type == LayerShell && wlr_scene_node_coords(
                    &old_l->scene->node, &unused_lx, &unused_ly)
                && old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
            return;
        } else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
            return;
        /* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
         * and probably other clients */
        } else if (old_c && !client_is_unmanaged(old_c) && (!c || !client_wants_focus(c))) {
            client_set_border_color(old_c, (float[])COLOR(borders[BorderNorm]));
            client_activate_surface(old, 0);
        }
    }
    drawbars();

    if (!c) {
        /* With no client, all we have left is to clear focus */
        wlr_seat_keyboard_notify_clear_focus(seat);
        return;
    }

    /* Change cursor surface */
    motionnotify(0, NULL, 0, 0, 0, 0);

    /* Have a client, so focus its top-level wlr_surface */
    client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

    /* Activate the new client */
    client_activate_surface(client_surface(c), 1);
}

Monitor *
nextmon(int add) {
    int i = 0, i_sel = 1;

    int nmons = wl_list_length(&mons);
    Monitor* pmons[MAX(nmons,1)];

    Monitor* m = NULL;
    wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->enabled) {
            pmons[i] = m;
            if (m == selmon) i_sel = i;
            i++;
        }
    }

    if (!i) return NULL;
    return pmons[(i_sel + add + i)%i];
}

void
setselmon(Monitor *m)
{
    /* the bars mark the selected monitor (awl_draw_t.selmon) */
    if (m == selmon)
        return;
    selmon = m;
    drawbars();
}

void
focusmon(const Arg *arg)
{
    Monitor *m = nextmon(arg->i);
    /* nextmon() returns NULL when no monitor is enabled -- keep selmon as it
     * is rather than handing NULL to everything that dereferences it */
    if (!m)
        return;
    focusclient(focustop(selmon = m), 1);
}

void
focusstack(const Arg *arg)
{
    /* Focus the next or previous client (in tiling order) on selmon */
    Client *c, *sel = focustop(selmon);
    if (!sel || (sel->isfullscreen && !client_has_children(sel)))
        return;
    if (arg->i > 0) {
        wl_list_for_each(c, &sel->link, link) {
            if (&c->link == &clients)
                continue; /* wrap past the sentinel node */
            if (VISIBLEON_ACTIVE(c, selmon))
                break; /* found it */
        }
    } else {
        wl_list_for_each_reverse(c, &sel->link, link) {
            if (&c->link == &clients)
                continue; /* wrap past the sentinel node */
            if (VISIBLEON_ACTIVE(c, selmon))
                break; /* found it */
        }
    }
    /* If only one client is visible on selmon, then c == sel */
    focusclient(c, 1);
}

void
movestack(const Arg *arg)
{
    Client *c, *sel = focustop(selmon);

    if (!sel || wl_list_length(&clients) <= 1)
        return;

    if (arg->i > 0) {
        wl_list_for_each(c, &sel->link, link) {
            if (&c->link == &clients) {
                c = wl_container_of(&clients, c, link);
                break; /* wrap past the sentinel node */
            }
            if (VISIBLEON(c, selmon) || &c->link == &clients) {
                break; /* found it */
            }
        }
    } else {
        wl_list_for_each_reverse(c, &sel->link, link) {
            if (&c->link == &clients) {
                c = wl_container_of(&clients, c, link);
                break; /* wrap past the sentinel node */
            }
            if (VISIBLEON(c, selmon) || &c->link == &clients) {
                break; /* found it */
            }
        }
        /* backup one client */
        c = wl_container_of(c->link.prev, c, link);
    }

    wl_list_remove(&sel->link);
    wl_list_insert(&c->link, &sel->link);
    arrange(selmon);
}

/** We probably should change the name of this, it sounds like
 * will focus the topmost client of this mon, when actually will
 * only return that client */
Client *
focustop(Monitor *m)
{
    Client *c;
    wl_list_for_each(c, &fstack, flink) {
        if (VISIBLEON_ACTIVE(c, m))
            return c;
    }
    return NULL;
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, fullscreen);
    setfullscreen(c, client_wants_fullscreen(c));
}

void
gpureset(struct wl_listener *listener, void *data)
{
    struct wlr_renderer *old_drw = drw;
    struct wlr_allocator *old_alloc = alloc;
    struct Monitor *m;
    /* must match setup(): the scene holds scenefx-only nodes (blur,
     * per-node opacity) that a plain wlroots renderer cannot render */
    if (!(drw = fx_renderer_create(backend)))
        die("couldn't recreate renderer");

    if (!(alloc = wlr_allocator_autocreate(backend, drw)))
        die("couldn't recreate allocator");

    wl_list_remove(&gpu_reset.link);
    wl_signal_add(&drw->events.lost, &gpu_reset);

    wlr_compositor_set_renderer(compositor, drw);

    wl_list_for_each(m, &mons, link) {
        wlr_output_init_render(m->wlr_output, alloc, drw);
        /* the bar's texture dies with the old renderer, and the scene
         * no longer has the buffer to re-upload it from */
        m->bar_shown = NULL;
    }

    wlr_allocator_destroy(old_alloc);
    wlr_renderer_destroy(old_drw);
    awl_redraw_request();
}

int
handlesigchld(int signo, void *data)
{
    siginfo_t in;
    /* wlroots reaps the Xwayland fork itself and gives up on Xwayland if
     * that fails, so look first (WNOWAIT) and leave that one alone */
    while (!waitid(P_ALL, 0, &in, WEXITED | WNOHANG | WNOWAIT) && in.si_pid) {
#ifdef XWAYLAND
        if (xwayland && xwayland->server && in.si_pid == xwayland->server->pid)
            break;
#endif
        waitpid(in.si_pid, NULL, 0);
        if (in.si_pid == locker_pid) {
            locker_pid = 0;
            if (!locker_locked)
                awl_alert_set(AwlAlertLocker, "%s exited without locking", locker[0]);
        }
    }
    return 0;
}

int
handlesigquit(int signo, void *data)
{
    quit(NULL);
    return 0;
}

void
gaplessgrid(Monitor *m)
{
    unsigned int n = 0, i = 0, ch, cw, cn, rn, rows, cols;
    Client *c;

    wl_list_for_each(c, &clients, link)
        if (VISIBLEON_ACTIVE(c, m) && !c->isfloating && !c->ismaximized && !c->isfullscreen)
            n++;
    if (n == 0)
        return;

    /* grid dimensions */
    for (cols = 0; cols <= (n / 2); cols++) {
        if ((cols * cols) >= n)
            break;
    }

    if (n == 5) { /* set layout against the general calculation: not 1:2:2, but 2:3 */
        cols = 2;
    }

    /* widescreen is better if 3 columns */
    if (n >= 3 && n <= 6 && (m->w.width / m->w.height) > 1) {
        cols = 3;
    }

    rows = n / cols;

    /* window geometries */
    cw = cols ? (unsigned int)(m->w.width / cols) : (unsigned int)(m->w.width);
    cn = 0; /* current column number */
    rn = 0; /* current row number */
    wl_list_for_each(c, &clients, link) {
        unsigned int cx, cy;
        if (!VISIBLEON_ACTIVE(c, m) || c->isfloating || c->ismaximized || c->isfullscreen)
            continue;

        if ((i / rows + 1) > (cols - n % cols))
            rows = n / cols + 1;
        ch = rows ? (unsigned int)(m->w.height / rows) : (unsigned int)(m->w.height);
        cx = m->w.x + cn * cw;
        cy = m->w.y + rn * ch;
        resize(c, (struct wlr_box) { cx, cy, cw, ch}, 0);
        rn++;
        if (rn >= rows) {
            rn = 0;
            cn++;
        }
        i++;
    }
}

void
bstack(Monitor *m)
{
    int w, h, mh, mx, tx, ty, tw;
    int i, n = 0;
    Client *c;
    const struct PertagLayout *l = monlt(m);

    wl_list_for_each(c, &clients, link)
        if (VISIBLEON_ACTIVE(c, m) && !c->isfloating && !c->ismaximized && !c->isfullscreen)
            n++;
    if (n == 0)
        return;

    if (n > l->nmaster) {
        mh = (int)round(l->nmaster ? l->mfact * m->w.height : 0);
        tw = m->w.width / (n - l->nmaster);
        ty = m->w.y + mh;
    } else {
        mh = m->w.height;
        tw = m->w.width;
        ty = m->w.y;
    }

    i = mx = 0;
    tx = m-> w.x;
    wl_list_for_each(c, &clients, link) {
        if (!VISIBLEON_ACTIVE(c, m) || c->isfloating || c->ismaximized || c->isfullscreen)
            continue;
        if (i < l->nmaster) {
            w = (m->w.width - mx) / (MIN(n, l->nmaster) - i);
            resize(c, (struct wlr_box) { .x = m->w.x + mx, .y = m->w.y, .width = w, .height = mh }, 0);
            mx += c->geom.width;
        } else {
            h = m->w.height - mh;
            resize(c, (struct wlr_box) { .x = tx, .y = ty, .width = tw, .height = h }, 0);
            if (tw != m->w.width)
                tx += c->geom.width;
        }
        i++;
    }
}

void
incnmaster(const Arg *arg)
{
    if (!arg || !selmon)
        return;
    monlt(selmon)->nmaster = MAX(monlt(selmon)->nmaster + arg->i, 0);
    arrange(selmon);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
    /* This event is raised by the backend when a new input device becomes
     * available. */
    struct wlr_input_device *device = data;
    uint32_t caps;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        createkeyboard(wlr_keyboard_from_input_device(device));
        trackinputdevice(device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        createpointer(wlr_pointer_from_input_device(device));
        trackinputdevice(device);
        break;
    default:
        /* TODO handle other input device types */
        break;
    }

    /* We need to let the wlr_seat know what our capabilities are, which is
     * communiciated to the client. In awl we always have a cursor, even if
     * there are no pointer devices, so we always include that capability. */
    /* TODO do we actually require a cursor? */
    caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&kb_group->wlr_group->devices))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(seat, caps);
}

int
keybinding(uint32_t mods, xkb_keysym_t sym)
{
    /*
     * Here we handle compositor keybindings. This is when the compositor is
     * processing keys, rather than passing them on to the client for its own
     * processing.
     */
    const Key *k;
    for (k = cfg->keys; k < cfg->keys + cfg->n_keys; k++) {
        if (CLEANMASK(mods) == CLEANMASK(k->mod)
                && sym == k->keysym && k->func) {
            // awl_vrb_printf( "keybinding: (mod:%u key:%u func:%p arg:%li)\n", k->mod, k->keysym, k->func, k->arg.v );
            k->func(&k->arg);
            return 1;
        }
    }
    return 0;
}

void
keypress(struct wl_listener *listener, void *data)
{
    int i;
    /* This event is raised when a key is pressed or released. */
    KeyboardGroup *group = wl_container_of(listener, group, key);
    struct wlr_keyboard_key_event *event = data;

    /* Translate libinput keycode -> xkbcommon */
    uint32_t keycode = event->keycode + 8;
    /* Get a list of keysyms based on the keymap for this keyboard */
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
            group->wlr_group->keyboard.xkb_state, keycode, &syms);

    int handled = 0;
    uint32_t mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);

    wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

    if (locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        locked_keystrokes++;
        if (locked_bg_blur)
            AWL_wlr_scene_blur_set_shatter(locked_bg_blur, (float)locked_keystrokes/20.0);
    }

    /* On _press_ if there is no active screen locker,
     * attempt to process a compositor keybinding. */
    if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (i = 0; i < nsyms; i++)
            handled = keybinding(mods, syms[i]) || handled;
    }

    if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
        group->mods = mods;
        group->keysyms = syms;
        group->nsyms = nsyms;
        wl_event_source_timer_update(group->key_repeat_source,
                group->wlr_group->keyboard.repeat_info.delay);
    } else {
        group->nsyms = 0;
        wl_event_source_timer_update(group->key_repeat_source, 0);
    }

    if (handled)
        return;

    wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
    /* Pass unhandled keycodes along to the client. */
    wlr_seat_keyboard_notify_key(seat, event->time_msec,
            event->keycode, event->state);
}

void
keypressmod(struct wl_listener *listener, void *data)
{
    /* This event is raised when a modifier key, such as shift or alt, is
     * pressed. We simply communicate this to the client. */
    KeyboardGroup *group = wl_container_of(listener, group, modifiers);

    wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
    /* Send modifiers to the client. */
    wlr_seat_keyboard_notify_modifiers(seat,
            &group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
    KeyboardGroup *group = data;
    int i;
    if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
        return 0;

    wl_event_source_timer_update(group->key_repeat_source,
            1000 / group->wlr_group->keyboard.repeat_info.rate);

    for (i = 0; i < group->nsyms; i++)
        keybinding(group->mods, group->keysyms[i]);

    return 0;
}

void
killclient(const Arg *arg)
{
    Client *sel = focustop(selmon);
    if (sel)
        client_send_close(sel);
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

/** awl_lock.h's eventfd: a lock request, a new logind thread state */
int
lockfire(int fd, uint32_t mask, void *data)
{
    unsigned flags;
    if (awl_lock_take(&flags))
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
static unsigned
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

/** awl_alert.c's notification */
void
awl_alert_notify(const char *text)
{
    spawn(&(Arg){.v = (const char *[]){ "notify-send", "-a", "awl", "-u", "critical",
            "awl alert", text, NULL }});
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
mapnotify(struct wl_listener *listener, void *data)
{
    /* Called when the surface is mapped, or ready to display on-screen. */
    Client *p = NULL;
    Client *w, *c = wl_container_of(listener, c, map);
    Monitor *m;
    int i;

    /* Create scene tree for this client and its border */
    c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
    /* Enabled later by a call to arrange() */
    wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
    c->scene_surface = c->type == XDGShell
            ? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
            : wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
    c->scene->node.data = c->scene_surface->node.data = c;

    client_get_geometry(c, &c->geom);

    /* Handle unmanaged clients first so we can return prior create borders */
    if (client_is_unmanaged(c)) {
        /* Unmanaged clients always are floating */
        wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
        wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
        client_set_size(c, c->geom.width, c->geom.height);
        if (client_wants_focus(c)) {
            focusclient(c, 1);
            exclusive_focus = c;
        }
        goto unset_fullscreen;
    }

    for (i = 0; i < 4; i++) {
        c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
            (float[])COLOR(borders[c->isurgent ? BorderUrg : BorderNorm]));
        c->border[i]->node.data = c;
    }

    /* Initialize client geometry with room for border */
    client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
    c->geom.width += 2 * c->bw;
    c->geom.height += 2 * c->bw;

    /* Insert this client into client lists. */
    wl_list_insert(&clients, &c->link);
    wl_list_insert(&fstack, &c->flink);

    /* Set initial monitor, tags, floating status, and focus:
     * we always consider floating, clients that have parent and thus
     * we set the same tags and monitor as its parent.
     * If there is no parent, apply rules */
    if ((p = client_get_parent(c))) {
        c->isfloating = 1;
        setmon(c, p->mon, p->tags);
    } else {
        applyrules(c);
    }
    drawbars();

unset_fullscreen:
    m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
    wl_list_for_each(w, &clients, link) {
        if (w != c && w != p && w->isfullscreen && m == w->mon && (w->tags & c->tags))
            setfullscreen(w, 0);
    }
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
    /* This event is raised when a client would like to maximize itself,
     * typically because the user clicked on the maximize button on
     * client-side decorations. awl doesn't support maximization, but
     * to conform to xdg-shell protocol we still must send a configure.
     * Since xdg-shell protocol v5 we should ignore request of unsupported
     * capabilities, just schedule a empty configure when the client uses <5
     * protocol version
     * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
    Client *c = wl_container_of(listener, c, maximize);
    if (c->surface.xdg->initialized
            && wl_resource_get_version(c->surface.xdg->toplevel->resource)
                    < XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
        wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

void
monocle(Monitor *m)
{
    Client *c;
    int n = 0;

    wl_list_for_each(c, &clients, link) {
        if (!VISIBLEON_ACTIVE(c, m) || c->isfloating || c->isfullscreen || c->ismaximized)
            continue;
        resize(c, m->w, 0);
        n++;
    }
    if (n)
        snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[%d]", n);
    if ((c = focustop(m)))
        wlr_scene_node_raise_to_top(&c->scene->node);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits an _absolute_
     * motion event, from 0..1 on each axis. This happens, for example, when
     * wlroots is running under a Wayland window rather than KMS+DRM, and you
     * move the mouse over the window. You could enter the window from any edge,
     * so we have to warp the mouse there. Also, some hardware emits these events. */
    struct wlr_pointer_motion_absolute_event *event = data;
    double lx, ly, dx, dy;

    if (!event->time_msec) /* this is 0 with virtual pointers */
        wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

    wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
    dx = lx - cursor->x;
    dy = ly - cursor->y;
    motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
        double dx_unaccel, double dy_unaccel)
{
    double sx = 0, sy = 0, sx_confined, sy_confined;
    Client *c = NULL, *w = NULL;
    LayerSurface *l = NULL;
    struct wlr_surface *surface = NULL;
    struct wlr_pointer_constraint_v1 *constraint;

    /* Find the client under the pointer and send the event along. */
    xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

    /* time is 0 in internal calls meant to restore pointer focus. */
    if (time) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
                relative_pointer_mgr, seat, (uint64_t)time * 1000,
                dx, dy, dx_unaccel, dy_unaccel);

        wl_list_for_each(constraint, &pointer_constraints->constraints, link)
            cursorconstrain(constraint);

        if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
            toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
            if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
                sx = cursor->x - c->geom.x - c->bw;
                sy = cursor->y - c->geom.y - c->bw;
                if (wlr_region_confine(&active_constraint->region, sx, sy,
                        sx + dx, sy + dy, &sx_confined, &sy_confined)) {
                    dx = sx_confined - sx;
                    dy = sy_confined - sy;
                }

                if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
                    return;
            }
        }

        wlr_cursor_move(cursor, device, dx, dy);
        wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

        /* Update selmon (even while dragging a window) */
        if (cfg->sloppyfocus)
            setselmon(xytomon(cursor->x, cursor->y));
    }

    /* the cursor has moved: look again */
    xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

    if (cursor_mode == CurPressed && !seat->drag
            && surface != seat->pointer_state.focused_surface
            && toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &l) >= 0) {
        c = w;
        surface = seat->pointer_state.focused_surface;
        sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
        sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
    }

    /* Update drag icon's position */
    wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

    /* If we are currently grabbing the mouse, handle and return */
    if (cursor_mode == CurMove) {
        /* Move the grabbed client to the new position. */
        resize(grabc, (struct wlr_box){.x = (int)round(cursor->x) - grabcx, .y = (int)round(cursor->y) - grabcy,
            .width = grabc->geom.width, .height = grabc->geom.height}, 1);
        return;
    } else if (cursor_mode == CurResize) {
        resize(grabc, (struct wlr_box){.x = grabc->geom.x, .y = grabc->geom.y,
            .width = (int)round(cursor->x) - grabc->geom.x, .height = (int)round(cursor->y) - grabc->geom.y}, 1);
        return;
    }

    /* awl's popups that take the pointer keep it from what is below */
    if (textwinmotion())
        return;

    /* If there's no client surface under the cursor, set the cursor image to a
     * default. This is what makes the cursor image appear when you move it
     * off of a client or over its border. */
    if (!surface && !seat->drag)
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

    updatehover(c);
    pointerfocus(c, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
    /* This event is forwarded by the cursor when a pointer emits a _relative_
     * pointer motion event (i.e. a delta) */
    struct wlr_pointer_motion_event *event = data;
    /* The cursor doesn't move unless we tell it to. The cursor automatically
     * handles constraining the motion to the output layout, as well as any
     * special configuration applied for the specific input device which
     * generated the event. You can pass NULL for the device if you want to move
     * the cursor around without any input. */
    motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
    if (cursor_mode != CurNormal && cursor_mode != CurPressed)
        return;
    xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
    if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
        return;

    /* Float the window and tell motionnotify to grab it */
    setfloating(grabc, 1);
    switch (cursor_mode = arg->ui) {
    case CurMove:
        grabcx = (int)round(cursor->x) - grabc->geom.x;
        grabcy = (int)round(cursor->y) - grabc->geom.y;
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "all-scroll");
        break;
    case CurResize:
        /* Doesn't work for X11 output - the next absolute motion event
         * returns the cursor to where it started */
        wlr_cursor_warp_closest(cursor, NULL,
                grabc->geom.x + grabc->geom.width,
                grabc->geom.y + grabc->geom.height);
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
        break;
    }
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
    struct wlr_output_configuration_v1 *config = data;
    outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
    /*
     * Called when a client such as wlr-randr requests a change in output
     * configuration. This is only one way that the layout can be changed,
     * so any Monitor information should be updated by updatemons() after an
     * output_layout.change event, not here.
     */
    struct wlr_output_configuration_head_v1 *config_head;
    int ok = 1;

    wl_list_for_each(config_head, &config->heads, link) {
        struct wlr_output *wlr_output = config_head->state.output;
        Monitor *m = wlr_output->data;
        struct wlr_output_state state;

        /* Ensure displays previously disabled by wlr-output-power-management-v1
         * are properly handled*/
        m->asleep = 0;

        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, config_head->state.enabled);
        if (!config_head->state.enabled)
            goto apply_or_test;

        if (config_head->state.mode)
            wlr_output_state_set_mode(&state, config_head->state.mode);
        else
            wlr_output_state_set_custom_mode(&state,
                    config_head->state.custom_mode.width,
                    config_head->state.custom_mode.height,
                    config_head->state.custom_mode.refresh);

        wlr_output_state_set_transform(&state, config_head->state.transform);
        wlr_output_state_set_scale(&state, config_head->state.scale);
        wlr_output_state_set_adaptive_sync_enabled(&state,
                config_head->state.adaptive_sync_enabled);

apply_or_test:
        ok &= test ? wlr_output_test_state(wlr_output, &state)
                : wlr_output_commit_state(wlr_output, &state);

        /* Don't move monitors if position wouldn't change. This avoids
         * wlroots marking the output as manually configured.
         * wlr_output_layout_add does not like disabled outputs */
        if (!test && wlr_output->enabled && (m->m.x != config_head->state.x || m->m.y != config_head->state.y))
            wlr_output_layout_add(output_layout, wlr_output,
                    config_head->state.x, config_head->state.y);

        wlr_output_state_finish(&state);
    }

    if (ok)
        wlr_output_configuration_v1_send_succeeded(config);
    else
        wlr_output_configuration_v1_send_failed(config);
    wlr_output_configuration_v1_destroy(config);

    /* https://codeberg.org/dwl/dwl/issues/577 */
    updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
    struct wlr_output_configuration_v1 *config = data;
    outputmgrapplyortest(config, 1);
}

void
pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
        uint32_t time)
{
    struct timespec now;

    if (surface != seat->pointer_state.focused_surface &&
            cfg->sloppyfocus && time && c && !client_is_unmanaged(c))
        focusclient(c, 0);

    /* If surface is NULL, clear pointer focus */
    if (!surface) {
        wlr_seat_pointer_notify_clear_focus(seat);
        return;
    }

    if (!time) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
    }

    /* Let the client know that the mouse cursor has entered one
     * of its surfaces, and make keyboard focus follow if desired.
     * wlroots makes this a no-op if surface is already focused */
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void
powermgrsetmode(struct wl_listener *listener, void *data)
{
    struct wlr_output_power_v1_set_mode_event *event = data;
    struct wlr_output_state state = {0};
    Monitor *m = event->output->data;

    if (!m)
        return;

    m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the ouput */
    wlr_output_state_set_enabled(&state, event->mode);
    wlr_output_commit_state(m->wlr_output, &state);

    m->asleep = !event->mode;
    updatemons(NULL, NULL);
}

void
quit(const Arg *arg)
{
    wl_display_terminate(dpy);
}

void
rendermon(struct wl_listener *listener, void *data)
{
    /* This function is called every time an output is ready to display a frame,
     * generally at the output's refresh rate (e.g. 60Hz). */
    Monitor *m = wl_container_of(listener, m, frame);
    Client *c;
    struct wlr_output_state pending = {0};
    struct timespec now;

    /* Render if no XDG clients have an outstanding resize and are visible on
     * this monitor. */
    wl_list_for_each(c, &clients, link) {
        if (c->resize && !c->isfloating && client_is_rendered_on_mon(c, m) && !client_is_stopped(c))
            goto skip;
    }

    /* make clients transparent before the commit; they change it back again otherwise */
    wl_list_for_each(c, &clients, link) {
        if (c->scene_surface && c->one_minus_alpha != 0) {
            wlr_scene_node_for_each_buffer(&c->scene_surface->node, scenebuffersetopacity, c);
        }
    }
    /* the locker's own surface, not just the blur behind it */
    if (wlr_scene_output_commit(m->scene_output, NULL) && covered_pending
            && m->lock_surface && wlr_surface_has_buffer(m->lock_surface->surface)) {
        m->lock_shown = 1;
        lockcovered();
    }

skip:
    /* Let clients know a frame has been rendered */
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(m->scene_output, &now);
    wlr_output_state_finish(&pending);
}

void
requestdecorationmode(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, set_decoration_mode);
    if (c->surface.xdg->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
                WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
    struct wlr_seat_request_start_drag_event *event = data;

    if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
            event->serial))
        wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
    else
        wlr_data_source_destroy(event->drag->source);
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
    struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(event->output, event->state);
    updatemons(NULL, NULL);
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
    struct wlr_box *bbox;
    struct wlr_box clip;
    int old_width, old_height;

    if (!c->mon || !client_surface(c)->mapped)
        return;

    bbox = interact ? &sgeom : &c->mon->w;

    old_width = c->geom.width;
    old_height = c->geom.height;

    client_set_bounds(c, geo.width, geo.height);
    c->geom = geo;
    applybounds(c, bbox);

    /* Update scene-graph, including borders */
    wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
    wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
    /* resize() runs on every commit, not just actual resizes (e.g. every
     * redrawn frame of a client with c->blur set); only touch the blur
     * node's size when it actually changed, otherwise scenefx redoes its
     * blur-region/damage bookkeeping every single frame for no reason,
     * which is visible as flicker. */
    if (c->blur && (c->geom.width != old_width || c->geom.height != old_height)) {
        wlr_scene_blur_set_size(c->blur, c->geom.width, c->geom.height);
    }
    wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
    wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
    wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
    wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
    wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
    wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
    wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);

    /* this is a no-op if size hasn't changed */
    c->resize = client_set_size(c, c->geom.width - 2 * c->bw,
            c->geom.height - 2 * c->bw);
    client_get_clip(c, &clip);
    wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void
run(char *startup_cmd)
{
    /* Add a Unix socket to the Wayland display. */
    const char *socket = wl_display_add_socket_auto(dpy);
    if (!socket)
        die("startup: display_add_socket_auto");
    setenv("WAYLAND_DISPLAY", socket, 1);
    setenv("XDG_CURRENT_DESKTOP", "wlroots", 1 );
    system("systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XDG_CURRENT_DESKTOP");
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    setenv("QT_STYLE_OVERRIDE","kvantum",1);
    setenv("DESKTOP_SESSION","kde",1);
    setenv("QT_AUTO_SCREEN_SCALE_FACTOR","0",1);
    setenv("EDITOR","nvim",1);
    setenv("SYSTEMD_EDITOR","/usr/bin/nvim",1);
    setenv("SSH_AUTH_SOCK","1",1);
    setenv("NO_AT_BRIDGE","1",1);
    /* setup() filled in "desktop_dir" and the library's init() may have
     * changed it since; later changes don't reach the environment */
    awl_dict_val_t dir;
    awl_dict_lock(cfgdict);
    if (awl_dict_get(cfgdict, "desktop_dir", &dir) == AWL_DICT_OK
            && dir.kind == AWL_DICT_KIND_STR)
        setenv("GRIM_DEFAULT_DIR", dir.str, 1);
    awl_dict_unlock(cfgdict);

    /* Only now does our own Wayland socket exist and WAYLAND_DISPLAY point
     * at it, which the tray's GTK/GDK thread needs to connect to us as a
     * client. Its startup runs in the background (it can't complete the
     * connection handshake until wl_display_run() below starts servicing
     * clients) -- awl_tray_init() must not block waiting for that. */
    awl_tray_init(awl_host_table(), cfg->tray);

    /* Start the backend. This will enumerate outputs and inputs, become the DRM
     * master, etc */
    if (!wlr_backend_start(backend))
        die("startup: backend_start");

    /* Now that the socket exists and the backend is started, run the startup command.
     * Uses vfork(), not fork(): by this point setup() has already started the
     * plugin/GPU driver threads, so a plain fork() risks inheriting a lock
     * (e.g. malloc's arena lock) held by one of those threads at the moment
     * of the fork -- see spawn_pid() for the full explanation. */
    if (startup_cmd) {
        if ((child_pid = vfork()) < 0)
            die("startup: vfork:");
        if (child_pid == 0) {
            unblocksignals();
            close(STDIN_FILENO);
            setsid();
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
            awl_err_printf("startup: execl failed: %s\n", strerror(errno));
            _exit(1);
        }
    }

    // autostart goes in here
    for (unsigned i=0; i<LENGTH(Autostarts); ++i) {
        if (!Autostarts[i][0]) continue; /* { NULL }: an empty list */
        autostart(Autostarts[i]);
    }

    /* Mark stdout as non-blocking to avoid the startup script
     * causing awl to freeze when a user neither closes stdin
     * nor consumes standard input in his startup script */

    if (fd_set_nonblock(STDOUT_FILENO) < 0)
        close(STDOUT_FILENO);

    drawbars();

    /* At this point the outputs are initialized, choose initial selmon based on
     * cursor position, and set default cursor image */
    selmon = xytomon(cursor->x, cursor->y);

    /* TODO hack to get cursor to display in its initial location (100, 100)
     * instead of (0, 0) and then jumping. Still may not be fully
     * initialized, as the image/coordinates are not transformed for the
     * monitor when displayed here */
    wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
    wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

    if (lock_at_start)
        lockscreen(NULL);

    /* Run the Wayland event loop. This does not return until you exit the
     * compositor. Starting the backend rigged up all of the necessary event
     * loop configuration to listen to libinput events, DRM events, generate
     * frame events at the refresh rate, and so on. */
    wl_display_run(dpy);
}

void
setcursor(struct wl_listener *listener, void *data)
{
    /* This event is raised by the seat when a client provides a cursor image */
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    /* If we're "grabbing" the cursor, don't use the client's image, we will
     * restore it after "grabbing" sending a leave event, followed by a enter
     * event, which will result in the client requesting set the cursor surface */
    if (cursor_mode != CurNormal && cursor_mode != CurPressed)
        return;
    /* This can be sent by any client, so we check to make sure this one
     * actually has pointer focus first. If so, we can tell the cursor to
     * use the provided surface as the cursor image. It will set the
     * hardware cursor on the output that it's currently on and continue to
     * do so as the cursor moves between outputs. */
    if (event->seat_client == seat->pointer_state.focused_client)
        wlr_cursor_set_surface(cursor, event->surface,
                event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    if (cursor_mode != CurNormal && cursor_mode != CurPressed)
        return;
    /* This can be sent by any client, so we check to make sure this one
     * actually has pointer focus first. If so, we can tell the cursor to
     * use the provided cursor shape. */
    if (event->seat_client == seat->pointer_state.focused_client)
        wlr_cursor_set_xcursor(cursor, cursor_mgr,
                wlr_cursor_shape_v1_name(event->shape));
}

void
setfloating(Client *c, int floating)
{
    Client *p = client_get_parent(c);
    c->isfloating = floating;
    /* If in floating layout do not change the client's layer */
    if (!c->mon || !client_surface(c)->mapped || !sellayout(c->mon)->arrange)
        return;
    wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
            (p && p->isfullscreen) ? LyrFS
            : c->isfloating ? LyrFloat : LyrTile]);
    arrange(c->mon);
    drawbars();
}

void
setfullscreen(Client *c, int fullscreen)
{
    c->isfullscreen = fullscreen;
    if (!c->mon || !client_surface(c)->mapped)
        return;
    c->bw = fullscreen ? 0 : cfg->borderpx;
    client_set_fullscreen(c, fullscreen);
    wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
            ? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

    if (fullscreen) {
        c->prev = c->geom;
        resize(c, c->mon->m, 0);
    } else {
        /* restore previous size instead of arrange for floating windows since
         * client positions are set by the user and cannot be recalculated */
        resize(c, c->prev, 0);
    }
    arrange(c->mon);
    drawbars();
}

/** l's index in config_plugins.h's layouts, -1 if it isn't one of them */
static int
layoutindex(const Layout *l)
{
    uintptr_t p = (uintptr_t)l, first = (uintptr_t)cfg->layouts;
    if (p < first || p >= first + cfg->n_layouts * sizeof(Layout))
        return -1;
    return (int)((p - first) / sizeof(Layout));
}

/** m's ptlt[] slot for what it shows: 1+t for tag t alone, 0 for several */
static size_t
pertagslot(const Monitor *m)
{
    uint32_t ts = m->tagset[m->seltags];
    return ts && !(ts & (ts - 1)) ? (size_t)__builtin_ctz(ts) + 1 : 0;
}

/** m's current layout and its settings: with per-tag layouts those of the
 * view's ptlt[] slot, else ptlt[0] */
static struct PertagLayout *
monlt(Monitor *m)
{
    return &m->ptlt[m->pertag ? pertagslot(m) : 0];
}

/** m's selected layout */
static const Layout *
sellayout(Monitor *m)
{
    struct PertagLayout *l = monlt(m);
    return l->lt[l->sellt];
}

/** per-tag layouts on selmon on or off (MonitorRule.pertag sets where it
 * starts). Either way the current view keeps its layout, mfact and nmaster;
 * turned on, the other views get back the ones they had when it was last on
 * (except views of several tags: their ptlt[0] is also the pertag-off one),
 * off, all views share the current ones. */
void
togglepertag(const Arg *arg)
{
    size_t slot;
    (void)arg;
    if (!selmon)
        return;
    slot = pertagslot(selmon);
    selmon->pertag = !selmon->pertag;
    if (selmon->pertag)
        selmon->ptlt[slot] = selmon->ptlt[0];
    else
        selmon->ptlt[0] = selmon->ptlt[slot];
    drawbar(selmon);
}

/** the next (arg->i > 0) or previous (< 0) of config_plugins.h's layouts on selmon */
void
cycle_layout(const Arg* arg)
{
    int i, n = (int)cfg->n_layouts;
    if (!selmon || !arg || !arg->i)
        return;
    i = layoutindex(sellayout(selmon)) + (arg->i > 0 ? 1 : -1);
    setlayout(&(Arg){.v = &cfg->layouts[(i % n + n) % n]});
}

/** like dwm: arg->v, one of config_plugins.h's layouts (&layouts[n]), for selmon;
 * without one (or with the one it has), back to its previous layout */
void
setlayout(const Arg *arg)
{
    const Layout *l = arg ? arg->v : NULL;
    struct PertagLayout *ml;
    if (!selmon || (l && layoutindex(l) < 0))
        return;
    ml = monlt(selmon);
    if (!l || l != ml->lt[ml->sellt])
        ml->sellt ^= 1;
    if (l)
        ml->lt[ml->sellt] = l;
    arrange(selmon);
    drawbar(selmon);
}

/** l, pointing into the old layouts table, moved to the same index in the
 * new one, or its first */
static const Layout *
layoutmoved(const Layout *l, const Layout *old, size_t n_old)
{
    size_t idx = l >= old && l < old + n_old ? (size_t)(l - old) : 0;
    return &cfg->layouts[MIN(idx, cfg->n_layouts - 1)];
}

/** After cfg changed: the monitors' layouts point into the old table (which
 * may be about to be unloaded); same index in the new one, or its first */
static void
layoutsmoved(const Layout *old, size_t n_old)
{
    Monitor *m;
    size_t i, j;
    wl_list_for_each(m, &mons, link) {
        for (j = 0; j < LENGTH(m->ptlt); j++)
            for (i = 0; i < LENGTH(m->ptlt[j].lt); i++)
                m->ptlt[j].lt[i] = layoutmoved(m->ptlt[j].lt[i], old, n_old);
        arrange(m);
    }
}

void
transluce(const Arg *arg)
{
    if (!selmon) return;
    if (!arg) return;
    float change = arg->i > 0 ? 0.1 : -0.1;
    Client *c = focustop(selmon);
    if (!c) return;
    c->one_minus_alpha -= change;
    c->one_minus_alpha = MAX(c->one_minus_alpha, 0.0);
    c->one_minus_alpha = MIN(c->one_minus_alpha, 1.0);
    if (!c->blur) attachblur(c);
}

/** arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
    float f;

    if (!arg || !selmon || !sellayout(selmon)->arrange)
        return;
    f = arg->f < 1.0f ? arg->f + monlt(selmon)->mfact : arg->f - 1.0f;
    if (f < 0.1 || f > 0.9)
        return;
    monlt(selmon)->mfact = f;
    arrange(selmon);
}

void
setmon(Client *c, Monitor *m, uint32_t newtags)
{
    Monitor *oldmon = c->mon;

    if (oldmon == m)
        return;
    c->mon = m;
    c->prev = c->geom;

    /* Scene graph sends surface leave/enter events on move and resize */
    if (oldmon)
        arrange(oldmon);
    if (m) {
        /* Make sure window actually overlaps with the monitor */
        resize(c, c->geom, 0);
        c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
        setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
        setfloating(c, c->isfloating);
    }
    focusclient(focustop(selmon), 1);
}

void
setpsel(struct wl_listener *listener, void *data)
{
    /* This event is raised by the seat when a client wants to set the selection,
     * usually when the user copies something. wlroots allows compositors to
     * ignore such requests if they so choose, but in awl we always honor them
     */
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
    /* This event is raised by the seat when a client wants to set the selection,
     * usually when the user copies something. wlroots allows compositors to
     * ignore such requests if they so choose, but in awl we always honor them
     */
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(seat, event->source, event->serial);
}

void
setup(void)
{
    int drm_fd, i;
    sigset_t handled;

    /* These signals are handled in the event loop (a signalfd). That
     * only works if no thread can take them, so block them here, before any
     * thread starts, and every thread inherits the mask; spawned children
     * unblock them again (see unblocksignals()). */
    sigemptyset(&handled);
    sigaddset(&handled, SIGCHLD);
    sigaddset(&handled, SIGINT);
    sigaddset(&handled, SIGTERM);
    if (getenv("AWL_TEST_OUTPUTS")) {
        sigaddset(&handled, SIGUSR1);
        sigaddset(&handled, SIGUSR2);
    }
    sigprocmask(SIG_BLOCK, &handled, NULL);
    /* a dead client must not kill awl (GIO ignores it anyway) */
    signal(SIGPIPE, SIG_IGN);

    /* until the library is loaded */
    cfg = builtinconfig();
    wl_list_init(&inputdevices);


    wlr_log_init(log_level, wlrlog);

    /* The Wayland display is managed by libwayland. It handles accepting
     * clients from the Unix socket, manging Wayland globals, and so on. */
    dpy = wl_display_create();
    event_loop = wl_display_get_event_loop(dpy);
    wl_event_loop_add_signal(event_loop, SIGCHLD, handlesigchld, NULL);
    wl_event_loop_add_signal(event_loop, SIGINT, handlesigquit, NULL);
    wl_event_loop_add_signal(event_loop, SIGTERM, handlesigquit, NULL);
    hover_timer = wl_event_loop_add_timer(event_loop, hovertimeout, NULL);
    leave_timer = wl_event_loop_add_timer(event_loop, leavetimeout, NULL);
    wallpaper_timer = wl_event_loop_add_timer(event_loop, wallpapertimeout, NULL);

    /* The backend is a wlroots feature which abstracts the underlying input and
     * output hardware. The autocreate option will choose the most suitable
     * backend based on the current environment, such as opening an X11 window
     * if an X11 server is running. */
    if (!(backend = wlr_backend_autocreate(event_loop, &session)))
        die("couldn't create backend");
    /* for testing (test/live.sh): SIGUSR1 adds an output, SIGUSR2 removes
     * the newest one; headless and nested (wayland) backends only */
    if (getenv("AWL_TEST_OUTPUTS")) {
        wl_event_loop_add_signal(event_loop, SIGUSR1, testoutputadd, NULL);
        wl_event_loop_add_signal(event_loop, SIGUSR2, testoutputremove, NULL);
    }
    /* for testing (test/live.sh's win, test/windows.sh): a FIFO of commands
     * for the text popups, see testwincmd(); O_RDWR, so it never reads EOF
     * while no writer has it open */
    if (getenv("AWL_TEST_WINDOWS")) {
        int fd = open(getenv("AWL_TEST_WINDOWS"), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            die("AWL_TEST_WINDOWS %s:", getenv("AWL_TEST_WINDOWS"));
        wl_event_loop_add_fd(event_loop, fd, WL_EVENT_READABLE, testwinctl, NULL);
    }

    /* Initialize the scene graph used to lay out windows */
    scene = wlr_scene_create();
    root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, cfg->rootcolor);
    for (i = 0; i < NUM_LAYERS; i++) {
        layers[i] = wlr_scene_tree_create(&scene->tree);
    }
    drag_icon = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);
    /* the wallpaper, under background layer surfaces (see awl_background.h) */
    background_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_place_below(&background_tree->node, &layers[LyrBg]->node);
    background_init(background_tree, event_loop);
    /* the desktop_dir panels, over the wallpaper (see awl_desktop.h) */
    desktop_tree = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_place_above(&desktop_tree->node, &layers[LyrBg]->node);
    desktop_init(desktop_tree, &(desktop_config_t){
            .blur = cfg->blur_launcher, .radius = cfg->blur_launcher_radius,
            .blur_strength = cfg->blur[0], .blur_alpha = cfg->blur[1] });
    /* the alerts, over everything (drawalert() keeps it on top) */
    alert_tree = wlr_scene_tree_create(&scene->tree);

    /* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
     * can also specify a renderer using the WLR_RENDERER env var.
     * The renderer is responsible for defining the various pixel formats it
     * supports for shared memory, this configures that for clients. */

    // TODO this is for scenefx
    // if (!(drw = wlr_renderer_autocreate(backend)))
    if (!(drw = fx_renderer_create(backend)))
        die("couldn't create renderer");
    wl_signal_add(&drw->events.lost, &gpu_reset);

    /* Create shm, drm and linux_dmabuf interfaces by ourselves.
     * The simplest way is to call:
     *      wlr_renderer_init_wl_display(drw);
     * but we need to create the linux_dmabuf interface manually to integrate it
     * with wlr_scene. */
    wlr_renderer_init_wl_shm(drw, dpy);

    if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
        wlr_drm_create(dpy, drw);
        wlr_scene_set_linux_dmabuf_v1(scene,
                wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
    }

    if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
            && backend->features.timeline)
        wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

    /* Autocreates an allocator for us.
     * The allocator is the bridge between the renderer and the backend. It
     * handles the buffer creation, allowing wlroots to render onto the
     * screen */
    if (!(alloc = wlr_allocator_autocreate(backend, drw)))
        die("couldn't create allocator");

    /* This creates some hands-off wlroots interfaces. The compositor is
     * necessary for clients to allocate surfaces and the data device manager
     * handles the clipboard. Each of these wlroots interfaces has room for you
     * to dig your fingers in and play with their behavior if you want. Note that
     * the clients cannot set the selection directly without compositor approval,
     * see the setsel() function. */
    compositor = wlr_compositor_create(dpy, 6, drw);
    wlr_subcompositor_create(dpy);
    wlr_data_device_manager_create(dpy);
    wlr_export_dmabuf_manager_v1_create(dpy);
    wlr_screencopy_manager_v1_create(dpy);
    wlr_ext_image_copy_capture_manager_v1_create(dpy, 1);
    wlr_ext_output_image_capture_source_manager_v1_create(dpy, 1);
    wlr_data_control_manager_v1_create(dpy);
    wlr_primary_selection_v1_device_manager_create(dpy);
    wlr_viewporter_create(dpy);
    wlr_single_pixel_buffer_manager_v1_create(dpy);
    wlr_fractional_scale_manager_v1_create(dpy, 1);
    wlr_presentation_create(dpy, backend, 2);
    wlr_alpha_modifier_v1_create(dpy);

    /* Initializes the interface used to implement urgency hints */
    activation = wlr_xdg_activation_v1_create(dpy);
    wl_signal_add(&activation->events.request_activate, &request_activate);

    wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(dpy));

    power_mgr = wlr_output_power_manager_v1_create(dpy);
    wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

    /* Creates an output layout, which is a wlroots utility for working with an
     * arrangement of screens in a physical layout. */
    output_layout = wlr_output_layout_create(dpy);
    wl_signal_add(&output_layout->events.change, &layout_change);

    wlr_xdg_output_manager_v1_create(dpy, output_layout);

    /* Configure a listener to be notified when new outputs are available on the
     * backend. */
    wl_list_init(&mons);
    wl_signal_add(&backend->events.new_output, &new_output);

    /* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
     * Wayland protocol which is used for application windows. For more
     * detail on shells, refer to the article:
     *
     * https://drewdevault.com/2018/07/29/Wayland-shells.html
     */
    wl_list_init(&clients);
    wl_list_init(&fstack);

    xdg_shell = wlr_xdg_shell_create(dpy, 6);
    wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
    wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

    layer_shell = wlr_layer_shell_v1_create(dpy, 3);
    wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

    idle_notifier = wlr_idle_notifier_v1_create(dpy);

    idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
    wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

    keyboard_shortcuts_inhibit_mgr = wlr_keyboard_shortcuts_inhibit_v1_create(dpy);
    session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
    wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);
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

    /* Use decoration protocols to negotiate server-side decorations */
    wlr_server_decoration_manager_set_default_mode(
            wlr_server_decoration_manager_create(dpy),
            WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
    wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

    pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
    wl_signal_add(&pointer_constraints->events.new_constraint, &new_pointer_constraint);

    relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

    /*
     * Creates a cursor, which is a wlroots utility for tracking the cursor
     * image shown on screen.
     */
    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, output_layout);

    /* Creates an xcursor manager, another wlroots utility which loads up
     * Xcursor themes to source cursor images from and makes sure that cursor
     * images are available at all scale factors on the screen (necessary for
     * HiDPI support). Scaled cursors will be loaded with each output. */
    cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    setenv("XCURSOR_SIZE", "24", 1);

    /*
     * wlr_cursor *only* displays an image on screen. It does not move around
     * when the pointer moves. However, we can attach input devices to it, and
     * it will generate aggregate events for all of them. In these events, we
     * can choose how we want to process them, forwarding them to clients and
     * moving the cursor around. More detail on this process is described in
     * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
     *
     * And more comments are sprinkled throughout the notify functions above.
     */
    wl_signal_add(&cursor->events.motion, &cursor_motion);
    wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
    wl_signal_add(&cursor->events.button, &cursor_button);
    wl_signal_add(&cursor->events.axis, &cursor_axis);
    wl_signal_add(&cursor->events.frame, &cursor_frame);

    cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
    wl_signal_add(&cursor_shape_mgr->events.request_set_shape, &request_set_cursor_shape);

    /*
     * Configures a seat, which is a single "seat" at which a user sits and
     * operates the computer. This conceptually includes up to one keyboard,
     * pointer, touch, and drawing tablet device. We also rig up a listener to
     * let us know when new input devices are available on the backend.
     */
    wl_signal_add(&backend->events.new_input, &new_input_device);
    virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
    wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
            &new_virtual_keyboard);
    virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
    wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
            &new_virtual_pointer);

    seat = wlr_seat_create(dpy, "seat0");
    /* there is always a cursor (see inputdevice()), even before the first
     * pointer device or with only a virtual one, which doesn't come through
     * inputdevice() */
    wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER);
    wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
    wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
    wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
    wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
    wl_signal_add(&seat->events.start_drag, &start_drag);

    kb_group = createkeyboardgroup();
    wl_list_init(&kb_group->destroy.link);

    output_mgr = wlr_output_manager_v1_create(dpy);
    wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
    wl_signal_add(&output_mgr->events.test, &output_mgr_test);

    /* NULL is fine: every lookup then fails with AWL_DICT_ERR_INVALID */
    if (!(cfgdict = awl_dict_new()))
        awl_err_printf("out of memory for the config dictionary\n");
    cfgdict_defaults(cfgdict);
    /* before the plugins: their threads wake these from the start, and a
     * wake without the eventfd is lost (the logind state then stays unseen
     * and its alert up) */
    if (awl_redraw_init() >= 0)
        redraw_source = wl_event_loop_add_fd(event_loop, awl_redraw_fd(),
                WL_EVENT_READABLE, redraw_fire, NULL);
    if (awl_lock_init() >= 0)
        lock_source = wl_event_loop_add_fd(event_loop, awl_lock_fd(),
                WL_EVENT_READABLE, lockfire, NULL);
    covered_timer = wl_event_loop_add_timer(event_loop, coveredtimeout, NULL);
    /* the logind thread has until then to start listening (logind_missing) */
    logind_timer = wl_event_loop_add_timer(event_loop, logindtimeout, NULL);
    wl_event_source_timer_update(logind_timer, LOGIND_GRACE_MS);
    awl_plugins_load(0, cfgdict);
    configuse();
    awl_draw_init();

    /* Make sure XWayland clients don't connect to the parent X server,
     * e.g when running in the x11 backend or the wayland backend and the
     * compositor has Xwayland support */
    unsetenv("DISPLAY");
#ifdef XWAYLAND
    /*
     * Initialise the XWayland X server.
     * It will be started when the first X client is started.
     */
    if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
        wl_signal_add(&xwayland->events.ready, &xwayland_ready);
        wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

        setenv("DISPLAY", xwayland->display_name, 1);
    } else {
        awl_wrn_printf("failed to setup XWayland X server, continuing without it\n");
    }
#endif
}

static pid_t
spawn_pid(const Arg *arg)
{
    /* on execvp failure, must call _exit() not exit()/die(): vfork()'s child
     * shares stdio buffers with the parent until it exits or execs. */
    pid_t pid = vfork();
    if (pid < 0) {
        awl_err_printf("vfork failed: %s\n", strerror(errno));
        return -1;
    } else if (pid == 0) {
        unblocksignals();
        close(STDIN_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        setsid();
        execvp(((char **)arg->v)[0], (char **)arg->v);
        awl_err_printf("execvp %s failed: %s\n", ((char **)arg->v)[0], strerror(errno));
        _exit(1);
    } else {
        return pid;
    }
}

void spawn(const Arg *arg) { spawn_pid(arg); }

/** spawns argv and remembers it, so it is stopped when awl exits; refuses to
 * start it at all if there is no room left to remember it */
void
autostart(const char **argv)
{
    pid_t pid;
    if (Autostarted_pids_sz >= (int)LENGTH(Autostarted_pids)) {
        awl_wrn_printf("autostart: more than %d programs, not starting %s\n",
                (int)LENGTH(Autostarted_pids), argv[0]);
        return;
    }
    /* a failed spawn returns -1; never let that reach kill()/waitpid() */
    if ((pid = spawn_pid(&(const Arg){.v=argv})) > 0)
        Autostarted_pids[Autostarted_pids_sz++] = pid;
}

/** Sends SIGTERM to the startup command and every autostarted program (each
 * leads its own process group), then waits for them; whatever is still
 * running after CHILD_STOP_MS gets SIGKILL, so a child that ignores SIGTERM
 * can't keep awl from exiting. */
void
stopchildren(void)
{
    pid_t pids[LENGTH(Autostarted_pids) + 1], r;
    struct timespec tick = {0, 20 * 1000 * 1000};
    int i, n = 0, left, waited;

    if (child_pid > 0)
        pids[n++] = child_pid;
    for (i = 0; i < Autostarted_pids_sz; i++)
        if (Autostarted_pids[i] > 0)
            pids[n++] = Autostarted_pids[i];
    child_pid = -1;
    Autostarted_pids_sz = 0;

    for (i = 0; i < n; i++)
        kill(-pids[i], SIGTERM);
    for (waited = 0;; waited += 20) {
        left = 0;
        for (i = 0; i < n; i++) {
            if (!pids[i])
                continue;
            /* ECHILD: handlesigchld() already reaped it */
            if ((r = waitpid(pids[i], NULL, WNOHANG)) == pids[i] || (r < 0 && errno == ECHILD))
                pids[i] = 0;
            else
                left++;
        }
        if (!left || waited >= CHILD_STOP_MS)
            break;
        nanosleep(&tick, NULL);
    }
    for (i = 0; i < n; i++) {
        if (!pids[i])
            continue;
        awl_wrn_printf("pid %d still running %d ms after SIGTERM, sending SIGKILL\n",
                (int)pids[i], CHILD_STOP_MS);
        kill(-pids[i], SIGKILL);
        waitpid(pids[i], NULL, 0);
    }
}

void
startdrag(struct wl_listener *listener, void *data)
{
    struct wlr_drag *drag = data;
    if (!drag->icon)
        return;

    drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
    LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void
tag(const Arg *arg)
{
    Client *sel = focustop(selmon);
    if (!sel || (arg->ui & TAGMASK) == 0)
        return;

    sel->tags = arg->ui & TAGMASK;
    focusclient(focustop(selmon), 1);
    arrange(selmon);
    drawbars();
}

void
tagmon(const Arg *arg)
{
    Client *sel = focustop(selmon);
    if (sel)
        setmon(sel, nextmon(arg->i), 0);
}

static void
testoutputadd_backend(struct wlr_backend *b, void *data)
{
    if (wlr_backend_is_headless(b))
        wlr_headless_add_output(b, 1280, 720);
    else if (wlr_backend_is_wl(b))
        wlr_wl_output_create(b);
}

/** the AWL_TEST_OUTPUTS hooks, see setup() */
int
testoutputadd(int signo, void *data)
{
    wlr_multi_for_each_backend(backend, testoutputadd_backend, NULL);
    return 0;
}

int
testoutputremove(int signo, void *data)
{
    /* the newest monitor, createmon() inserts at the head; never the last one */
    Monitor *m;
    if (wl_list_length(&mons) > 1) {
        m = wl_container_of(mons.next, m, link);
        wlr_output_destroy(m->wlr_output);
    }
    return 0;
}

/** what testwincmd()'s dump reports as clicks and scrolls */
static char testclicks[2048];

/** input N on: N's click callback, data is N */
static void
testwinclick(awl_window_t *win, int row, int col, uint32_t button, void *data)
{
    size_t len = strlen(testclicks);
    snprintf(testclicks + len, sizeof(testclicks) - len, "click %d %d %d %u\n",
            (int)(intptr_t)data, row, col, button);
}

/** scroll N on: N's scroll callback, data is N */
static void
testwinscroll(awl_window_t *win, int row, int col, uint32_t orientation, double delta,
        int32_t value120, void *data)
{
    size_t len = strlen(testclicks);
    snprintf(testclicks + len, sizeof(testclicks) - len, "scroll %d %d %d %u %d %d\n",
            (int)(intptr_t)data, row, col, orientation, value120, delta > 0 ? 1 : delta < 0 ? -1 : 0);
}

/** \n, \t and \\ in s, in place */
static void
testunescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/** One line of AWL_TEST_WINDOWS' FIFO, driving the text popups the way a
 * plugin does through awl_host. N is a slot of testwins, MON an output's
 * name, X logical pixels from its left edge, TEXT the rest of the line with
 * \n, \t and \\ escapes:
 *
 *   show N MON X TEXT      textwinshow(), creating N first if it is empty
 *   toggle N MON X TEXT    the same with toggle
 *   update N TEXT | hide N | destroy N | hideall
 *   input N on|off         N takes the pointer, its presses are recorded
 *   scroll N on|off        N takes the pointer, its scrolling is recorded
 *   restart                the plugin_restart action (destroys them all)
 *   dump PATH              writes PATH.tmp, then renames it to PATH:
 *                            mon NAME X Y W H WX WY WW WH SCALE ENABLED
 *                            win N MON|- X Y W H VISIBLE HOVERROW
 *                            cell N ROW COL X Y (a cell's center, visible
 *                              windows' first 8 rows)
 *                            click N ROW COL BUTTON (pressed since the last
 *                              dump, if it takes the pointer)
 *                            scroll N ROW COL ORIENTATION VALUE120 SIGN
 *                              (likewise; SIGN the delta's, -1, 0 or 1)
 *                            pwin I MON|- X Y W H VISIBLE (the plugins')
 *                            hover MON NAMESPACE|- X Y (the center of a bar
 *                              widget with callback_hover)
 *                            margin PX (popup_config.margin)
 *                            errors COUNT (lines rejected so far)
 */
static void
testwincmd(char *line)
{
    static int errors;
    char cmd[16] = "", mon[64] = "", tmp[PATH_MAX], onoff[4] = "";
    double cx, cy;
    int n = -1, x = 0, off = 0, args = 0, toggle;
    Monitor *m = NULL, *it;
    struct wlr_box box;
    TextWin *tw;
    awl_draw_t *d;
    unsigned int xpos;
    int bx, by, i;
    FILE *f;

    if (sscanf(line, "%15s%n", cmd, &off) != 1)
        return;
    line += off;
    if (*line == ' ')
        line++;
    toggle = !strcmp(cmd, "toggle");
    if (toggle || !strcmp(cmd, "show")) {
        if (sscanf(line, "%d %63s %d%n", &n, mon, &x, &args) != 3)
            goto bad;
    } else if (!strcmp(cmd, "update") || !strcmp(cmd, "hide") || !strcmp(cmd, "destroy")) {
        if (sscanf(line, "%d%n", &n, &args) != 1)
            goto bad;
    } else if (!strcmp(cmd, "input") || !strcmp(cmd, "scroll")) {
        if (sscanf(line, "%d %3s%n", &n, onoff, &args) != 2
                || (strcmp(onoff, "on") && strcmp(onoff, "off")))
            goto bad;
    }
    if (n != -1 && (n < 0 || n >= (int)LENGTH(testwins)))
        goto bad;
    line += args;
    if (*line == ' ')
        line++;
    testunescape(line);

    if (toggle || !strcmp(cmd, "show")) {
        wl_list_for_each(it, &mons, link)
            if (!strcmp(it->wlr_output->name, mon))
                m = it;
        if (!m)
            goto bad;
        if (!testwins[n])
            testwins[n] = textwincreate();
        textwinshow(testwins[n], m, x, line, toggle);
    } else if (!strcmp(cmd, "update")) {
        textwinupdate(testwins[n], line);
    } else if (!strcmp(cmd, "hide")) {
        textwinhide(testwins[n]);
    } else if (!strcmp(cmd, "destroy")) {
        textwindestroy(testwins[n]);
    } else if (!strcmp(cmd, "input")) {
        if (!testwins[n])
            goto bad;
        textwinsetclick(testwins[n], strcmp(onoff, "on") ? NULL : testwinclick, (void *)(intptr_t)n);
    } else if (!strcmp(cmd, "scroll")) {
        if (!testwins[n])
            goto bad;
        textwinsetscroll(testwins[n], strcmp(onoff, "on") ? NULL : testwinscroll, (void *)(intptr_t)n);
    } else if (!strcmp(cmd, "hideall")) {
        textwinshideall();
    } else if (!strcmp(cmd, "restart")) {
        plugin_restart(NULL);
    } else if (!strcmp(cmd, "dump") && *line) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", line);
        if (!(f = fopen(tmp, "w")))
            goto bad;
        wl_list_for_each(it, &mons, link)
            fprintf(f, "mon %s %d %d %d %d %d %d %d %d %g %d\n", it->wlr_output->name,
                    it->m.x, it->m.y, it->m.width, it->m.height,
                    it->w.x, it->w.y, it->w.width, it->w.height,
                    it->wlr_output->scale, it->wlr_output->enabled);
        for (n = 0; n < (int)LENGTH(testwins); n++) {
            if (!testwins[n])
                continue;
            x = awl_window_box(testwins[n], &box);
            m = awl_window_monitor(testwins[n]);
            fprintf(f, "win %d %s %d %d %d %d %d %d\n", n, m ? m->wlr_output->name : "-",
                    box.x, box.y, box.width, box.height, x, awl_window_hover_row(testwins[n]));
            for (int r = 0; x && r < 8 && awl_window_cell_center(testwins[n], r, 0, &cx, &cy); r++)
                for (int c = 0; awl_window_cell_center(testwins[n], r, c, &cx, &cy); c++)
                    fprintf(f, "cell %d %d %d %.0f %.0f\n", n, r, c, cx, cy);
        }
        fputs(testclicks, f);
        testclicks[0] = 0;
        n = 0;
        wl_list_for_each(tw, &textwins, link) {
            for (i = 0; i < (int)LENGTH(testwins) && testwins[i] != tw->win; i++);
            if (i < (int)LENGTH(testwins))
                continue;
            x = awl_window_box(tw->win, &box);
            m = awl_window_monitor(tw->win);
            fprintf(f, "pwin %d %s %d %d %d %d %d\n", n++, m ? m->wlr_output->name : "-",
                    box.x, box.y, box.width, box.height, x);
        }
        /* barwidgetat()'s layout of the right-hand widgets */
        wl_list_for_each(it, &mons, link) {
            d = &it->drw;
            if (!d->m || !it->scene_buffer || !it->wlr_output->enabled)
                continue;
            wlr_scene_node_coords(&it->scene_buffer->node, &bx, &by);
            xpos = d->center_widget_start + d->center_widget_space;
            for (i = d->n_widgets_right - 1; i >= 0; --i) {
                if (d->widgets_right[i].callback_hover && d->widgets_right[i].width)
                    fprintf(f, "hover %s %s %d %d\n", it->wlr_output->name,
                            d->widgets_right[i].popup_namespace ? d->widgets_right[i].popup_namespace : "-",
                            it->m.x + (int)((xpos + d->widgets_right[i].width / 2) / it->wlr_output->scale),
                            by + it->b.real_height / 2);
                xpos += d->widgets_right[i].width;
            }
        }
        fprintf(f, "margin %d\n", cfg->popup->margin);
        fprintf(f, "errors %d\n", errors);
        fclose(f);
        rename(tmp, line);
    } else {
        goto bad;
    }
    return;
bad:
    errors++;
    awl_dbg_printf(awl_loglevel_err, "AWL_TEST_WINDOWS: bad command: %s %s\n", cmd, line);
}

/** AWL_TEST_WINDOWS' FIFO is readable: runs its complete lines */
int
testwinctl(int fd, uint32_t mask, void *data)
{
    static char buf[4096];
    static size_t len;
    char *line, *nl;
    ssize_t r;

    while ((r = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
        len += (size_t)r;
        buf[len] = 0;
        for (line = buf; (nl = strchr(line, '\n')); line = nl + 1) {
            *nl = 0;
            testwincmd(line);
        }
        len -= (size_t)(line - buf);
        memmove(buf, line, len);
        if (len == sizeof(buf) - 1) /* an overlong line */
            len = 0;
    }
    return 0;
}

void
tile(Monitor *m)
{
    unsigned int mw, my, ty;
    int i, n = 0;
    Client *c;
    const struct PertagLayout *l = monlt(m);

    wl_list_for_each(c, &clients, link)
        if (VISIBLEON_ACTIVE(c, m) && !c->isfloating && !c->isfullscreen && !c->ismaximized)
            n++;
    if (n == 0)
        return;

    if (n > l->nmaster)
        mw = l->nmaster ? (int)roundf(m->w.width * l->mfact) : 0;
    else
        mw = m->w.width;
    i = my = ty = 0;
    wl_list_for_each(c, &clients, link) {
        if (!VISIBLEON_ACTIVE(c, m) || c->isfloating || c->isfullscreen || c->ismaximized)
            continue;
        if (i < l->nmaster) {
            resize(c, (struct wlr_box){.x = m->w.x, .y = m->w.y + my, .width = mw,
                .height = (m->w.height - my) / (MIN(n, l->nmaster) - i)}, 0);
            my += c->geom.height;
        } else {
            resize(c, (struct wlr_box){.x = m->w.x + mw, .y = m->w.y + ty,
                .width = m->w.width - mw, .height = (m->w.height - ty) / (n - i)}, 0);
            ty += c->geom.height;
        }
        i++;
    }
}

void togglebar(const Arg *arg) { togglebar_mon(selmon); }
void togglebar_mon(Monitor* m) {
    if (!m) return;
    m->showbar = !m->showbar;
    wlr_scene_node_set_enabled(&m->scene_buffer->node, m->showbar);
    /* drawbar() enables it again, if the window list has a width */
    if (!m->showbar)
        wlr_scene_node_set_enabled(&m->bar_blur->node, 0);
    /* The tray is a separate, real layer-shell overlay window per monitor
     * (see tray/awl_tray_bridge.cpp), not something drawn into m's own bar
     * buffer, so hiding/showing that buffer's scene node above doesn't
     * affect it -- it has to be told explicitly, for this monitor's own
     * tray window specifically. */
    awl_tray_set_visible(m->wlr_output->name, m->showbar);
    arrangelayers(m);
}

void
togglebw(const Arg *arg)
{
    Client* sel = focustop(selmon);
    if (sel && !sel->isfullscreen) {
        sel->bw = sel->bw ? 0 : cfg->borderpx;
        arrange(selmon);
    }
}

void
changebw(const Arg *arg)
{
    Client* sel = focustop(selmon);
    if (sel && !sel->isfullscreen) {
        sel->bw += arg->i;
        arrange(selmon);
    }
}

void
togglefloating(const Arg *arg)
{
    Client *sel = focustop(selmon);
    /* return if fullscreen */
    if (sel && !sel->isfullscreen)
        setfloating(sel, !sel->isfloating);
}

void
togglefullscreen(const Arg *arg)
{
    Client *sel = focustop(selmon);
    if (sel)
        setfullscreen(sel, !sel->isfullscreen);
}

void
toggletag(const Arg *arg)
{
    uint32_t newtags;
    Client *sel = focustop(selmon);
    if (!sel || !(newtags = sel->tags ^ (arg->ui & TAGMASK)))
        return;

    sel->tags = newtags;
    focusclient(focustop(selmon), 1);
    arrange(selmon);
    drawbars();
}

void
toggleview(const Arg *arg)
{
    uint32_t newtagset;
    if (!(newtagset = selmon ? selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK) : 0))
        return;

    selmon->tagset[selmon->seltags] = newtagset;
    focusclient(focustop(selmon), 1);
    arrange(selmon);
    drawbars();
}

void
unlocksession(struct wl_listener *listener, void *data)
{
    SessionLock *lock = wl_container_of(listener, lock, unlock);
    destroylock(lock, 1);
}

/** For a (v)forked child before exec, which would keep setup()'s signal mask
 * and ignored SIGPIPE. Both are the child's own even after `vfork()`. */
void
unblocksignals(void)
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
    LayerSurface *l = wl_container_of(listener, l, unmap);

    l->mapped = 0;
    wlr_scene_node_set_enabled(&l->scene->node, 0);
    if (l == exclusive_focus)
        exclusive_focus = NULL;
    if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
        arrangelayers(l->mon);
    if (l->layer_surface->surface == seat->keyboard_state.focused_surface)
        focusclient(focustop(selmon), 1);
    motionnotify(0, NULL, 0, 0, 0, 0);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
    /* Called when the surface is unmapped, and should no longer be shown. */
    Client *c = wl_container_of(listener, c, unmap);
    if (c == grabc) {
        cursor_mode = CurNormal;
        grabc = NULL;
    }

    if (client_is_unmanaged(c)) {
        if (c == exclusive_focus) {
            exclusive_focus = NULL;
            focusclient(focustop(selmon), 1);
        }
    } else {
        wl_list_remove(&c->link);
        setmon(c, NULL, 0);
        wl_list_remove(&c->flink);
    }

    wlr_scene_node_destroy(&c->scene->node);
    drawbars();
    motionnotify(0, NULL, 0, 0, 0, 0);
}

/** No bar is visible while locked or with every output off (unplugged or
 * powered down), so the plugins' 1 s polling can stop. */
void
updatepluginpause(void)
{
    Monitor *m;
    int visible = 0;
    wl_list_for_each(m, &mons, link)
        visible |= m->wlr_output->enabled;
    awl_plugins_set_paused(locked || !visible);
}

void
updatemons(struct wl_listener *listener, void *data)
{
    /*
     * Called whenever the output layout changes: adding or removing a
     * monitor, changing an output's mode or position, etc. This is where
     * the change officially happens and we update geometry, window
     * positions, focus, and the stored configuration in wlroots'
     * output-manager implementation.
     */
    struct wlr_output_configuration_v1 *config
            = wlr_output_configuration_v1_create();
    Client *c;
    struct wlr_output_configuration_head_v1 *config_head;
    Monitor *m;

    /* First remove from the layout the disabled monitors */
    wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->enabled || m->asleep)
            continue;
        config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
        config_head->state.enabled = 0;
        /* Remove this output from the layout to avoid cursor enter inside it */
        wlr_output_layout_remove(output_layout, m->wlr_output);
        closemon(m);
        m->m = m->w = (struct wlr_box){0};
        desktop_update(m);
        background_update(m);
        drawalert(m);
        textwinsupdate(m);
    }
    /* Insert outputs that need to */
    wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->enabled
                && !wlr_output_layout_get(output_layout, m->wlr_output))
            wlr_output_layout_add_auto(output_layout, m->wlr_output);
    }

    /* Now that we update the output layout we can get its box */
    wlr_output_layout_get_box(output_layout, NULL, &sgeom);

    wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
    wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

    /* Make sure the clients are hidden when awl is locked */
    if (locked_bg_blur) {
        wlr_scene_node_set_position(&locked_bg_blur->node, sgeom.x, sgeom.y);
        wlr_scene_blur_set_size(locked_bg_blur, sgeom.width, sgeom.height);
    }

    wl_list_for_each(m, &mons, link) {
        if (!m->wlr_output->enabled)
            continue;
        config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

        /* Get the effective monitor geometry to use for surfaces */
        wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
        m->w = m->m;
        wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

        wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
        wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

        if (m->lock_surface) {
            struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
            wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
            wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
        }

        /* Calculate the effective monitor geometry to use for clients */
        arrangelayers(m);
        /* Don't move clients to the left output when plugging monitors */
        arrange(m);
        /* make sure fullscreen clients have the right size */
        if ((c = focustop(m)) && c->isfullscreen)
            resize(c, m->m, 0);

        /* Try to re-set the gamma LUT when updating monitors,
         * it's only really needed when enabling a disabled output, but meh. */
        m->gamma_lut_changed = 1;

        config_head->state.x = m->m.x;
        config_head->state.y = m->m.y;

        if (!selmon) {
            selmon = m;
        }
    }

    if (selmon && selmon->wlr_output->enabled) {
        wl_list_for_each(c, &clients, link) {
            if (!c->mon && client_surface(c)->mapped)
                setmon(c, selmon, c->tags);
        }
        focusclient(focustop(selmon), 1);
        if (selmon->lock_surface) {
            client_notify_enter(selmon->lock_surface->surface,
                    wlr_seat_get_keyboard(seat));
            client_activate_surface(selmon->lock_surface->surface, 1);
        }
    }

    wl_list_for_each(m, &mons, link) {
        if (!m->wlr_output->enabled) continue;
        if (m->closedbar) {
            m->showbar = 0;
            m->closedbar = 0;
            togglebar_mon(m);
        }
        updatebar(m), drawbar(m);
    }

    /* FIXME: figure out why the cursor image is at 0,0 after turning all
     * the monitors on.
     * Move the cursor image where it used to be. It does not generate a
     * wl_pointer.motion event for the clients, it's only the image what it's
     * at the wrong position after all. */
    wlr_cursor_move(cursor, NULL, 0, 0);

    wlr_output_manager_v1_set_configuration(output_mgr, config);
    updatepluginpause();
}

/** (re)loads *f, a font of m's other than the bar's, at m's scale: font
 * and size (NULL, 0: the bar's, cfg->font and ->fontsize); NULL if both are
 * the bar's, or it doesn't load (what says whose, for the warning) */
static void
updatefont(Monitor *m, struct fcft_font **f, const char *font, int size, const char *what)
{
    const char *name = font ? font : cfg->font;

    awl_font_release(*f);
    *f = NULL;
    if ((!font && size <= 0) || !name)
        return;
    if (!(*f = awl_font_get_exact(name, (float)(size > 0 ? size : cfg->fontsize),
                m->wlr_output->scale)))
        awl_wrn_printf("%s font %s not found, using the bar's\n", what, name);
}

/** (re)loads m's popup and desktop fonts (Monitor.popupfont, .desktopfont) */
static void
updatefonts(Monitor *m)
{
    updatefont(m, &m->popupfont, cfg->popup->font, cfg->popup->fontsize, "popup");
    updatefont(m, &m->desktopfont, cfg->desktop->font, cfg->desktop->fontsize, "desktop");
}

void
updatebar(Monitor *m)
{
    int rw, rh;

    wlr_output_transformed_resolution(m->wlr_output, &rw, &rh);
    m->b.width = rw;
    m->b.real_width = (int)((float)m->b.width / m->wlr_output->scale);

    if (m->b.scale == m->wlr_output->scale && m->drw.m) {
        /* x/y are monitor-local -- see the comment on the other call below. */
        awl_tray_set_bar_geometry(m->wlr_output->name, 0, (topbar ? 0 : m->m.height - m->b.real_height),
                m->b.real_width, m->b.real_height, m->b.scale);
        return;
    }

    /* falls back to monospace; keeps the old font if even that fails */
    awl_draw_set_font(&m->drw, cfg->font, (float)cfg->fontsize, m->wlr_output->scale, 1);

    m->b.scale = m->wlr_output->scale;
    /* no font at all: an empty bar of a monospace:size=10's height */
    m->lrpad = m->drw.font ? m->drw.font->height
            : (int)lroundf(AWL_FONT_FALLBACK_SIZE * 2 * m->wlr_output->scale);
    m->b.height = m->lrpad + 2;
    m->b.real_height = (int)((float)m->b.height / m->wlr_output->scale);
    updatefonts(m);
    desktop_fontchanged(m);
    textwinsupdate(m);

    /* x/y are monitor-LOCAL (0, and the bar's own vertical offset within
     * this monitor), not m->m.x/m->m.y's global output-layout position:
     * the tray's layer-shell surface is explicitly bound to this exact
     * output (see createlayersurface()'s "awl-tray:" namespace handling),
     * so wlr_scene_layer_surface_v1_configure() already adds this
     * monitor's own global origin on top of whatever margin we request --
     * adding it again here double-counts it, pushing every monitor except
     * the one at (0,0) off screen (confirmed: this is exactly what made a
     * second monitor's tray invisible before this fix). */
    awl_tray_set_bar_geometry(m->wlr_output->name, 0, (topbar ? 0 : m->m.height - m->b.real_height),
            m->b.real_width, m->b.real_height, m->b.scale);
}

void
updatetitle(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, set_title);
    if (c == focustop(c->mon))
        drawbars();
}

void
urgent(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_activation_v1_request_activate_event *event = data;
    Client *c = NULL;
    toplevel_from_wlr_surface(event->surface, &c, NULL);
    if (!c || c == focustop(selmon))
        return;

    c->isurgent = 1;
    drawbars();

    if (client_surface(c)->mapped)
        client_set_border_color(c, (float[])COLOR(borders[BorderUrg]));
}

void
view(const Arg *arg)
{
    if (!selmon || (arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
        return;
    selmon->seltags ^= 1; /* toggle sel tagset */
    if (arg->ui & TAGMASK)
        selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
    focusclient(focustop(selmon), 1);
    arrange(selmon);
    drawbars();
}

void
cycle_view(const Arg* arg)
{
    if (arg->i == 0) return;
    int tmax = -1;
    int tmin = 32;
    int nt = LENGTH(tags);
    for (int t=0; t<nt; ++t)
        if (selmon->tagset[selmon->seltags] & (1 << t)) {
            tmax = MAX(tmax, t);
            tmin = MIN(tmin, t);
        }

    selmon->seltags ^= 1; /* toggle sel tagset */
    int next = arg->i > 0 ? (tmax + 1)%nt : (tmin - 1 + nt)%nt;
    selmon->tagset[selmon->seltags] = (1 << next) & TAGMASK;

    focusclient(focustop(selmon), 1);
    arrange(selmon);
    drawbars();
}

void
setontop(Client* c, int ontop)
{
    c->isontop = ontop;
    if (!c->mon) return;
    wlr_scene_node_reparent(&c->scene->node, c->isontop ? layers[LyrTop] : c->isfloating ? layers[LyrFloat] : layers[LyrTile]);
    arrange(c->mon);
    drawbars();
}

void
toggleontop(const Arg* arg)
{
    (void)arg;
    Client* sel = focustop(selmon);
    if (sel && !sel->isfullscreen) setontop(sel, !sel->isontop);
}

void
minimize(const Arg* arg)
{
    (void)arg;
    Client* sel = focustop(selmon);
    if (!sel) return;
    sel->isvisible = 0;
    focusclient(focustop(selmon), 1);
    arrange(sel->mon);
    drawbars();
}

void
unminimize(const Arg* arg)
{
    Client *c = NULL;
    int found_client = 0;
    wl_list_for_each(c, &fstack, flink) {
        if (VISIBLEON(c, selmon) && !c->isvisible) {
            c->isvisible = 1;
            found_client = 1;
            break;
        }
    }
    if (!found_client) return;
    focusclient(c, 1);
    arrange(selmon);
    drawbars();
}

void
maximize(const Arg* arg)
{
    Client* sel = focustop(selmon);
    if (!sel) return;
    sel->ismaximized = !sel->ismaximized;
    if (sel->ismaximized)
        sel->geom = sel->mon->w;
    arrange(sel->mon);
    drawbars();
}

static int
streqnull(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

static char *
strdupnull(const char *s)
{
    return s ? strdup(s) : NULL;
}

/** Brings everything that was set up from config_plugins.h up to
 * date with cfg, after it changed. What's only read when it's needed (key
 * and button bindings, rules, focus behaviour, which new layer surfaces get
 * blurred) needs nothing here. */
void
configapply(void)
{
    Monitor *m;
    Client *c, *sel = focustop(selmon);
    LayerSurface *l;
    InputDevice *d;
    TextWin *tw;
    awl_window_style_t style;
    int mode;
    struct xkb_keymap *keymap;
    const char *xkb[5] = { cfg->xkb_rules->rules, cfg->xkb_rules->model,
        cfg->xkb_rules->layout, cfg->xkb_rules->variant, cfg->xkb_rules->options };
    int i, changed;

    /* keyboards: the virtual ones keep the keymaps their clients gave them */
    for (i = changed = 0; i < (int)LENGTH(xkb); i++)
        changed |= !streqnull(xkb[i], applied.xkb[i]);
    if (changed && !(keymap = compilekeymap())) {
        awl_wrn_printf("config_plugins.h: can't compile the keymap, keeping the old one\n");
    } else if (changed) {
        wlr_keyboard_set_keymap(&kb_group->wlr_group->keyboard, keymap);
        wl_list_for_each(d, &inputdevices, link)
            if (d->device->type == WLR_INPUT_DEVICE_KEYBOARD)
                wlr_keyboard_set_keymap(wlr_keyboard_from_input_device(d->device), keymap);
        xkb_keymap_unref(keymap);
        for (i = 0; i < (int)LENGTH(xkb); i++) {
            free(applied.xkb[i]);
            applied.xkb[i] = strdupnull(xkb[i]);
        }
    }
    if (cfg->repeat_rate != applied.repeat_rate || cfg->repeat_delay != applied.repeat_delay) {
        wlr_keyboard_set_repeat_info(&kb_group->wlr_group->keyboard,
                cfg->repeat_rate, cfg->repeat_delay);
        applied.repeat_rate = cfg->repeat_rate;
        applied.repeat_delay = cfg->repeat_delay;
    }

    wl_list_for_each(d, &inputdevices, link)
        if (d->device->type == WLR_INPUT_DEVICE_POINTER)
            configurepointer(wlr_pointer_from_input_device(d->device));

    /* the bars' font, and with it their height */
    awl_font_retry();
    if (!streqnull(cfg->font, applied.font) || cfg->fontsize != applied.fontsize) {
        free(applied.font);
        applied.font = strdupnull(cfg->font);
        applied.fontsize = cfg->fontsize;
        wl_list_for_each(m, &mons, link) {
            if (!m->drw.m)
                continue;
            m->b.scale = 0; /* updatebar() reloads it */
            updatebar(m);
            arrangelayers(m);
        }
    }
    /* the popups' (updatebar() above reloaded it too, no harm) */
    if (!streqnull(cfg->popup->font, applied.popupfont)
            || cfg->popup->fontsize != applied.popupfontsize) {
        free(applied.popupfont);
        applied.popupfont = strdupnull(cfg->popup->font);
        applied.popupfontsize = cfg->popup->fontsize;
        wl_list_for_each(m, &mons, link) {
            if (!m->drw.m || !m->drw.font)
                continue;
            updatefont(m, &m->popupfont, cfg->popup->font, cfg->popup->fontsize, "popup");
            textwinsupdate(m);
        }
    }
    /* the desktop file list's, the same way */
    if (!streqnull(cfg->desktop->font, applied.desktopfont)
            || cfg->desktop->fontsize != applied.desktopfontsize) {
        free(applied.desktopfont);
        applied.desktopfont = strdupnull(cfg->desktop->font);
        applied.desktopfontsize = cfg->desktop->fontsize;
        wl_list_for_each(m, &mons, link) {
            if (!m->drw.m || !m->drw.font)
                continue;
            updatefont(m, &m->desktopfont, cfg->desktop->font, cfg->desktop->fontsize, "desktop");
            desktop_fontchanged(m);
        }
    }

    /* borders: those at the old default width get the new one */
    if (cfg->borderpx != applied.borderpx) {
        wl_list_for_each(c, &clients, link) {
            if (c->isfullscreen || c->bw != applied.borderpx)
                continue;
            c->bw = cfg->borderpx;
            if (c->mon)
                resize(c, c->geom, 0);
        }
        applied.borderpx = cfg->borderpx;
        wl_list_for_each(m, &mons, link)
            arrange(m);
    }

    memcpy(borders, *cfg->bordercolors, sizeof(borders));
    wl_list_for_each(c, &clients, link)
        if (c->border[0])
            client_set_border_color(c, (float[])COLOR(borders[c->isurgent ? BorderUrg
                    : c == sel ? BorderSel : BorderNorm]));
    wlr_scene_rect_set_color(root_bg, cfg->rootcolor);

    /* blur */
    wl_list_for_each(c, &clients, link) {
        if (!c->blur)
            continue;
        wlr_scene_blur_set_strength(c->blur, cfg->blur[0]);
        wlr_scene_blur_set_alpha(c->blur, cfg->blur[1]);
    }
    wl_list_for_each(m, &mons, link) {
        wlr_scene_rect_set_color(m->fullscreen_bg, cfg->fullscreen_bg);
        wlr_scene_blur_set_strength(m->bar_blur, cfg->blur[0]);
        wlr_scene_blur_set_alpha(m->bar_blur, cfg->blur[1]);
        for (i = 0; i < (int)LENGTH(m->layers); i++) {
            wl_list_for_each(l, &m->layers[i], link) {
                if (!l->blur)
                    continue;
                wlr_scene_blur_set_strength(l->blur, cfg->blur[0]);
                wlr_scene_blur_set_alpha(l->blur, cfg->blur[1]);
                wlr_scene_blur_set_corner_radius(l->blur, l->is_notification
                        ? cfg->blur_notifications_radius : cfg->blur_launcher_radius);
            }
        }
    }
    if (locked_bg_blur) {
        wlr_scene_blur_set_strength(locked_bg_blur, cfg->blur[0]);
        wlr_scene_blur_set_alpha(locked_bg_blur, cfg->blur[1]);
    }
    /* a new interval starts counting now; an unchanged one keeps counting */
    if (cfg->wallpaper->interval != applied.wallpaper_interval) {
        applied.wallpaper_interval = cfg->wallpaper->interval;
        wallpaperarm();
    }
    /* a new config_plugins.h mode replaces wallpapermode()'s, and goes to cfgdict;
     * an unchanged one doesn't. So does a new cfgdict "wallpaper_mode" (the
     * library's init() sets it before this runs), which wins when both
     * change. */
    mode = wallpapermodedict();
    if (cfg->wallpaper->mode != applied.wallpaper_mode) {
        applied.wallpaper_mode = cfg->wallpaper->mode;
        if (mode < 0 || mode == applied.wallpaper_mode_dict)
            wallpapermodeset(cfg->wallpaper->mode);
    }
    wallpapermodesync();
    desktop_configure(&(desktop_config_t){
            .blur = cfg->blur_launcher, .radius = cfg->blur_launcher_radius,
            .blur_strength = cfg->blur[0], .blur_alpha = cfg->blur[1] });
    if (!wl_list_empty(&textwins)) {
        style = textwinstyle();
        wl_list_for_each(tw, &textwins, link)
            awl_window_set_style(tw->win, &style);
    }

    drawbars();
}

/** switches to the loaded library's config_plugins.h, or the built-in one */
static void
configuse(void)
{
    const awl_plugin_api_t *api = awl_plugins_api();
    const Layout *old = cfg->layouts;
    size_t n_old = cfg->n_layouts;
    cfg = api ? api->config() : builtinconfig();
    layoutsmoved(old, n_old);
    configapply();
}

static void
untrackinputdevice(struct wl_listener *listener, void *data)
{
    InputDevice *d = wl_container_of(listener, d, destroy);
    wl_list_remove(&d->destroy.link);
    wl_list_remove(&d->link);
    free(d);
}

void
trackinputdevice(struct wlr_input_device *device)
{
    InputDevice *d = ecalloc(1, sizeof(*d));
    d->device = device;
    LISTEN(&device->events.destroy, &d->destroy, untrackinputdevice);
    wl_list_insert(&inputdevices, &d->link);
}

/** `awl_plugins_reload()`: the bars' widgets come from the library */
static void
pluginsdetach(void)
{
    Monitor *m;

    /* a hover popup gets its leave callback while that still exists */
    if (hover_active) {
        setleavepending(0);
        leavetimeout(NULL);
    }
    hover_widget = NULL;
    if (hover_timer)
        wl_event_source_timer_update(hover_timer, 0);
    wl_list_for_each(m, &mons, link)
        if (m->drw.m)
            awl_draw_widgets_clear(&m->drw);
    /* the popups the plugins left: nothing would update them any more */
    textwinsdestroy();
    /* cfg points into the library */
    const Layout *old = cfg->layouts;
    size_t n_old = cfg->n_layouts;
    cfg = builtinconfig();
    layoutsmoved(old, n_old);
}

static void
pluginsattach(void)
{
    Monitor *m;

    configuse();
    wl_list_for_each(m, &mons, link)
        if (m->drw.m)
            awl_plugins_bar_widgets(&m->drw);
    drawbars();
    desktop_reloaded();
}

static void
pluginrestart(void *data)
{
    plugin_restart_source = NULL;
    /* picks up a rebuilt libawlplugins.so; with an unchanged (or broken)
     * one this is a plain restart of the plugin threads */
    awl_plugins_reload(pluginsdetach, pluginsattach);

    /* The tray's D-Bus registration (and with it every item's icons) can be
     * lost across suspend+wake, same class of problem the other plugin
     * threads already get restarted for above -- so reload it the same way.
     * Unlike those, this does NOT tear down and restart the tray's GTK
     * thread itself (see awl_tray_reload()'s own comment for why: gtkmm's
     * Gtk::Main cannot safely be constructed a second time in one process),
     * so it's fire-and-forget, not a blocking shutdown/join/init cycle.
     * It also applies the reloaded config_plugins.h's tray_config. */
    awl_tray_reload(cfg->tray);
}

/** The plugin_restart action: reloads ``libawlplugins.so`` and the tray's
 * D-Bus state, see `awl_plugins_reload()` and `awl_tray_reload()`. Deferred
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
virtualkeyboard(struct wl_listener *listener, void *data)
{
    struct wlr_virtual_keyboard_v1 *kb = data;
    /* virtual keyboards shouldn't share keyboard group */
    KeyboardGroup *group = createkeyboardgroup();
    /* Set the keymap to match the group keymap */
    wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
    LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

    /* Add the new keyboard to the group */
    wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    struct wlr_input_device *device = &event->new_pointer->pointer.base;

    wlr_cursor_attach_input_device(cursor, device);
    if (event->suggested_output)
        wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
}

Monitor *
xytomon(double x, double y)
{
    struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
    return o ? o->data : NULL;
}

void
xytonode(double x, double y, struct wlr_surface **psurface,
        Client **pc, LayerSurface **pl, double *nx, double *ny)
{
    struct wlr_scene_node *node, *pnode;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_surface *scene_surface = NULL;
    Client *c = NULL;
    LayerSurface *l = NULL;
    int layer;

    for (layer = NUM_LAYERS - 1; !surface && layer >= 0; layer--) {
        if (!(node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny)))
            continue;

        if (node->type == WLR_SCENE_NODE_BUFFER) {
            scene_surface = wlr_scene_surface_try_from_buffer(
                    wlr_scene_buffer_from_node(node));
            if (!scene_surface) continue;
            surface = scene_surface->surface;
        }
        /* Walk the tree to find a node that knows the client */
        for (pnode = node; pnode && !c; pnode = &pnode->parent->node)
            c = pnode->data;
        if (c && c->type == LayerShell) {
            c = NULL;
            l = pnode->data;
        }
    }

    if (psurface) *psurface = surface;
    if (pc) *pc = c;
    if (pl) *pl = l;
}

#ifdef XWAYLAND
void
activatex11(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, activate);

    /* Only "managed" windows can be activated */
    if (!client_is_unmanaged(c))
        wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void
associatex11(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, associate);

    LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
    LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

void
configurex11(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    if (!client_surface(c) || !client_surface(c)->mapped) {
        wlr_xwayland_surface_configure(c->surface.xwayland,
                event->x, event->y, event->width, event->height);
        return;
    }
    if (client_is_unmanaged(c)) {
        wlr_scene_node_set_position(&c->scene->node, event->x, event->y);
        wlr_xwayland_surface_configure(c->surface.xwayland,
                event->x, event->y, event->width, event->height);
        return;
    }
    if ((c->isfloating && c != grabc) || !sellayout(c->mon)->arrange) {
        resize(c, (struct wlr_box){.x = event->x - c->bw,
                .y = event->y - c->bw, .width = event->width + c->bw * 2,
                .height = event->height + c->bw * 2}, 0);
    } else {
        arrange(c->mon);
    }
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
    struct wlr_xwayland_surface *xsurface = data;
    Client *c;

    /* Allocate a Client for this surface */
    c = xsurface->data = ecalloc(1, sizeof(*c));
    c->surface.xwayland = xsurface;
    c->type = X11;
    c->bw = client_is_unmanaged(c) ? 0 : cfg->borderpx;
    c->isvisible = 1;

    /* Listen to the various events it can emit */
    LISTEN(&xsurface->events.associate, &c->associate, associatex11);
    LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
    LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
    LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
    LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
    LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
    LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
    LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

void
dissociatex11(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, dissociate);
    wl_list_remove(&c->map.link);
    wl_list_remove(&c->unmap.link);
}

void
sethints(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, set_hints);
    struct wlr_surface *surface = client_surface(c);
    if (c == focustop(selmon) || !c->surface.xwayland->hints)
        return;

    c->isurgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
    drawbars();

    if (c->isurgent && surface && surface->mapped)
        client_set_border_color(c, (float[])COLOR(borders[BorderUrg]));
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
    struct wlr_xcursor *xcursor;

    /* assign the one and only seat */
    wlr_xwayland_set_seat(xwayland, seat);

    /* Set the default XWayland cursor to match the rest of awl. */
    if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
        wlr_xwayland_set_cursor(xwayland, wlr_xcursor_image_get_buffer(xcursor->images[0]),
                xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}
#endif

/* wlroots' (and scenefx') messages; wlroots drops those above log_level */
static void
wlrlog(enum wlr_log_importance importance, const char *fmt, va_list ap)
{
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    awl_dbg_printf(importance == WLR_ERROR ? awl_loglevel_err
            : importance == WLR_INFO ? awl_loglevel_log : awl_loglevel_vrb, "wlr: %s\n", msg);
}

int
main(int argc, char *argv[])
{
    char *startup_cmd = NULL;
    const char *logfile = AWL_LOGFILE;
    int c;

    while ((c = getopt(argc, argv, "s:l:hdv")) != -1) {
        if (c == 's')
            startup_cmd = optarg;
        else if (c == 'l')
            logfile = optarg;
        else if (c == 'd') {
            log_level = WLR_DEBUG;
            awl_log_set_level(awl_loglevel_vrb);
        }
        else if (c == 'v') {
            puts("awl " VERSION);
            return EXIT_SUCCESS;
        }
        else
            goto usage;
    }
    if (optind < argc)
        goto usage;

    /* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
    if (!getenv("XDG_RUNTIME_DIR"))
        die("XDG_RUNTIME_DIR must be set");
    /* only now: awl -v or a usage error must not truncate a running awl's log */
    awl_log_init(logfile);

    setup();

    run(startup_cmd);
    cleanup();
    return EXIT_SUCCESS;

usage:
    fprintf(stderr, "Usage: %s [-v] [-d] [-l logfile] [-s startup command]\n", argv[0]);
    return EXIT_FAILURE;
}
