/**
 * awl's defaults for awl_config_t (awl.h): what awl runs with while no
 * plugin library is loaded, and what plugins_config.c starts from and
 * changes (`awl_host_t.config_defaults`). Copy from here what to change; the
 * plugins' own defaults are in plugins_defaults.c.
 */
#include "awl_config.h"

/* commands */
static const char *termcmd[] = { "foot", NULL };
static const char *menucmd[] = { "wmenu-run", NULL };
/* the screen lock's locker */
static const char *locker[] = { "swaylock", "-c", "00000000", NULL };

static void tagmonf(const Arg *arg) { tagmon(arg); focusmon(arg); }

static const Key keys[] = {
    /* Note that Shift changes certain key codes: c -> C, 2 -> at, etc. */
    /* modifier                  key                 function          argument */
    { MODKEY,                    XKB_KEY_p,          spawn,            {.v = menucmd} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Return,     spawn,            {.v = termcmd} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_G,          lockscreen,       {0} },
    { MODKEY,                    XKB_KEY_w,          wallpaper,        {.i = WallpaperTimerNext} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_W,          wallpaper,        {.i = WallpaperTimerBack} },
    { MODKEY,                    XKB_KEY_b,          togglebar,        {0} },
    { MODKEY,                    XKB_KEY_j,          focusstack,       {.i = +1} },
    { MODKEY,                    XKB_KEY_k,          focusstack,       {.i = -1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_J,          movestack,        {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_K,          movestack,        {.i = -1} },
    { MODKEY,                    XKB_KEY_i,          incnmaster,       {.i = +1} },
    { MODKEY,                    XKB_KEY_d,          incnmaster,       {.i = -1} },
    { MODKEY,                    XKB_KEY_h,          setmfact,         {.f = -0.05f} },
    { MODKEY,                    XKB_KEY_l,          setmfact,         {.f = +0.05f} },
    { MODKEY,                    XKB_KEY_Tab,        view,             {0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_C,          killclient,       {0} },
    { MODKEY,                    XKB_KEY_space,      cycle_layout,     {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_space,      cycle_layout,     {.i = -1} },
    { MODKEY,                    XKB_KEY_f,          setlayout,        {.i = 1} }, /* floating */
    { MODKEY,                    XKB_KEY_g,          setlayout,        {.i = -1} }, /* the previous one */
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_g,          togglepertag,     {0} }, /* per-tag layouts */
    { MODKEY,                    XKB_KEY_Right,      cycle_view,       {.i = +1} },
    { MODKEY,                    XKB_KEY_Left,       cycle_view,       {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_space,      togglefloating,   {0} },
    { MODKEY,                    XKB_KEY_e,          togglefullscreen, {0} },
    { MODKEY,                    XKB_KEY_m,          maximize,         {0} },
    { MODKEY,                    XKB_KEY_t,          toggleontop,      {0} },
    { MODKEY,                    XKB_KEY_n,          minimize,         {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_n,          unminimize,       {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_r,          plugin_restart,   {0} },
    { MODKEY,                    XKB_KEY_0,          view,             {.ui = ~0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_parenright, tag,              {.ui = ~0} },
    { MODKEY,                    XKB_KEY_comma,      focusmon,         {.i = -1} },
    { MODKEY,                    XKB_KEY_period,     focusmon,         {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_less,       tagmonf,          {.i = -1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_greater,    tagmonf,          {.i = +1} },
    TAGKEYS(          XKB_KEY_1, XKB_KEY_exclam,                     0),
    TAGKEYS(          XKB_KEY_2, XKB_KEY_at,                         1),
    TAGKEYS(          XKB_KEY_3, XKB_KEY_numbersign,                 2),
    TAGKEYS(          XKB_KEY_4, XKB_KEY_dollar,                     3),
    TAGKEYS(          XKB_KEY_5, XKB_KEY_percent,                    4),
    TAGKEYS(          XKB_KEY_6, XKB_KEY_asciicircum,                5),
    TAGKEYS(          XKB_KEY_7, XKB_KEY_ampersand,                  6),
    TAGKEYS(          XKB_KEY_8, XKB_KEY_asterisk,                   7),
    TAGKEYS(          XKB_KEY_9, XKB_KEY_parenleft,                  8),
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Q,          quit,             {0} },

    /* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
    { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_Terminate_Server, quit, {0} },
    /* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
     * do not remove them. */
    CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
    CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

static const Button buttons[] = {
    { ClkClient,   MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
    { ClkClient,   MODKEY, BTN_MIDDLE, togglefloating, {0} },
    { ClkClient,   MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
};

static const Layout layouts[] = {
    /* symbol     arrange function */
    { "[]=",      tile },
    { "><>",      NULL },    /* no layout function means floating behavior */
    { "[M]",      monocle },
    { "###",      gaplessgrid },
    { "TTT",      bstack },
};

static const MonitorRule monrules[] = {
    /* name       mfact  nmaster scale layout rotate/reflect                x    y   pertag */
    /* example of a HiDPI laptop monitor:
    { "eDP-1",    0.5f,  1,      2,    0,     WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1,  0 },
    */
    { NULL,       0.55f, 1,      1,    0,     WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1,  0 },
};

/* rules, e.g.:
 *   app_id       title  tags mask  isfloating  monitor  w    h    blur  1-alpha
 * { "Gimp",      NULL,  0,         1,          -1,      0,   0,   0,    0 },   floating
 * { "firefox",   NULL,  1 << 8,    0,          -1,      0,   0,   0,    0 },   on tag 9 only
 * { "foot",      NULL,  0,         1,          -1,      400, 300, 1,    0.2 }, floating 400x300, blurred, 80% opaque
 */

static const awl_config_t defaults = {
    .modkey = WLR_MODIFIER_ALT,
    .keys = { keys, LENGTH(keys) },
    .buttons = { buttons, LENGTH(buttons) },
    .layouts = { layouts, LENGTH(layouts) },
    .monrules = { monrules, LENGTH(monrules) },

    .bar = { .show = 1, .top = 1 },
    .ntags = 9,
    .sloppyfocus = 1,
    .bypass_surface_visibility = 0,
    .borderpx = 1,
    .bordercolors = {
        [BorderNorm] = 0x444444ff,
        [BorderSel]  = 0x005577ff,
        [BorderUrg]  = 0x770000ff,
    },
    .font = "monospace:size=",
    .fontsize = 10,
    .rootcolor = 0x000000ff,
    .fullscreen_bg = 0x1a1a1aff,
    .blur = {
        .strength = 1.0f, .alpha = 1.0f,
        .notifications = 1, .notifications_radius = 15,
        .launcher = 1, .launcher_radius = 15,
        .windowlist = 1,
    },
    .keyboard = {
        .xkb = { .options = NULL },
        .repeat_rate = 25,
        .repeat_delay = 600,
    },
    .pointer = {
        .tap_to_click = 1,
        .tap_and_drag = 1,
        .drag_lock = 1,
        .natural_scrolling = 0,
        .disable_while_typing = 1,
        .left_handed = 0,
        .middle_button_emulation = 0,
        .scroll_method = LIBINPUT_CONFIG_SCROLL_2FG,
        .click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS,
        .send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED,
        .accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE,
        .accel_speed = 0.0,
        .button_map = LIBINPUT_CONFIG_TAP_MAP_LRM,
    },

    .lock = {
        .locker = locker,
        .at_start = 0,
        .on_suspend = 1,
        .blur = 1,
    },
    /* none; e.g. static const char *autostart[][8] = { { "fnott", NULL },
     * { TRAY_ICONS, "nm-applet", NULL } }; SETLIST(c->autostart, autostart) */
    .autostart = { NULL, 0 },
    .wlr_log_level = WLR_ERROR,

    .alert = {
        .fg = 0xffffffff, .bg = 0xc00000ff,
        .opacity = 0.9f, .font_scale = 1.5f,
        .margin = 12,
    },

    .tray = {
        .icon_theme = NULL,
    },
    .popup = {
        .bg           = 0x3c3c3c4d, /* awl blurs what is behind it */
        .fg           = 0xf8f8f2ff,
        .border       = 0xa6e22eff,
        .hover        = 0xffffff26, /* the row under the pointer, the buttons */
        .border_width = 1,
        .radius       = 15, /* the blur's corners */
        .padding      = 8,
        /* the gap to the bar and to the screen's sides (at most a quarter of
         * the screen) */
        .margin       = 6,
        .column_spacing = AWL_WINDOW_COLUMN_SPACING,
        .row_spacing  = AWL_WINDOW_ROW_SPACING,
        .font         = NULL,
        .fontsize     = 0,
        /* the tray's menus: all left out, so the popups' */
        .menu = { 0 },
    },
    .desktop = {
        .font = NULL,
        .fontsize = 0,
    },
    .wallpaper = {
        .dir = "Wallpapers", .fade_ms = 500, .interval = 7200, .mode = WallpaperRand,
    },
};

void
configdefaults(awl_config_t *c)
{
    *c = defaults;
}

const awl_config_t *
builtinconfig(void)
{
    return &defaults;
}
