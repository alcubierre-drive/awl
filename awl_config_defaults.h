#pragma once
/**
 * awl's defaults for :c:type:`awl_config_t` (awl.h): what awl runs with while no
 * plugin library is loaded, and what plugins_config.c starts from and
 * changes (:c:member:`awl_host_t.config_defaults`). Only awl_config.c includes it:
 * every file that did would get its own copy. Every setting is here with its
 * default; one left zero is a comment saying what zero means. Copy from here
 * what to change into plugins_config.c's :c:func:`configure`. The plugins' own
 * defaults are in plugins_config_defaults.h.
 */
#include "awl_config.h"
#include "awl_lockscreen.h"

/* commands */
static const char *const term_cmd[] = { "foot", NULL };
static const char *const menu_cmd[] = { "wmenu-run", NULL };
/* the screen lock's locker: awlock (lock/), part of awl (AWL_LOCKER,
 * awl_lockscreen.h) */
static const char *const locker[] = { AWL_LOCKER, NULL };

static void move_to_monitor_follow( awl_server_t *srv, const Arg *arg ) { AWL_ACTION(move_to_monitor)(srv, arg); AWL_ACTION(focus_monitor)(srv, arg); }

/* what runs after idle_seconds, if they are set */
static const IdleAction on_idle[] = {
    { AWL_ACTION(lock_screen), {0} },
};

static const Key keys[] = {
    /* Note that Shift changes certain key codes: c -> C, 2 -> at, etc. */
    /* modifier                  key                 function                      argument */
    { MODKEY,                    XKB_KEY_p,          AWL_ACTION(spawn),            {.v = menu_cmd} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Return,     AWL_ACTION(spawn),            {.v = term_cmd} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_G,          AWL_ACTION(lock_screen),       {0} },
    { MODKEY,                    XKB_KEY_w,          AWL_ACTION(wallpaper),        {.i = WallpaperTimerNext} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_W,          AWL_ACTION(wallpaper),        {.i = WallpaperTimerBack} },
    { MODKEY,                    XKB_KEY_b,          AWL_ACTION(toggle_bar),        {0} },
    { MODKEY,                    XKB_KEY_j,          AWL_ACTION(focus_stack),       {.i = +1} },
    { MODKEY,                    XKB_KEY_k,          AWL_ACTION(focus_stack),       {.i = -1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_J,          AWL_ACTION(move_stack),        {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_K,          AWL_ACTION(move_stack),        {.i = -1} },
    { MODKEY,                    XKB_KEY_i,          AWL_ACTION(change_master_count),       {.i = +1} },
    { MODKEY,                    XKB_KEY_d,          AWL_ACTION(change_master_count),       {.i = -1} },
    { MODKEY,                    XKB_KEY_h,          AWL_ACTION(set_master_factor),         {.f = -0.05f} },
    { MODKEY,                    XKB_KEY_l,          AWL_ACTION(set_master_factor),         {.f = +0.05f} },
    { MODKEY,                    XKB_KEY_Tab,        AWL_ACTION(view),             {0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_C,          AWL_ACTION(kill_client),       {0} },
    { MODKEY,                    XKB_KEY_space,      AWL_ACTION(cycle_layout),     {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_space,      AWL_ACTION(cycle_layout),     {.i = -1} },
    { MODKEY,                    XKB_KEY_f,          AWL_ACTION(set_layout),        {.i = 1} }, /* floating */
    { MODKEY,                    XKB_KEY_g,          AWL_ACTION(set_layout),        {.i = -1} }, /* the previous one */
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_g,          AWL_ACTION(toggle_per_tag),     {0} }, /* per-tag layouts */
    { MODKEY,                    XKB_KEY_Right,      AWL_ACTION(cycle_view),       {.i = +1} },
    { MODKEY,                    XKB_KEY_Left,       AWL_ACTION(cycle_view),       {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_space,      AWL_ACTION(toggle_floating),   {0} },
    { MODKEY,                    XKB_KEY_e,          AWL_ACTION(toggle_fullscreen), {0} },
    { MODKEY,                    XKB_KEY_m,          AWL_ACTION(maximize),         {0} },
    { MODKEY,                    XKB_KEY_t,          AWL_ACTION(toggle_on_top),      {0} },
    { MODKEY,                    XKB_KEY_n,          AWL_ACTION(minimize),         {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_n,          AWL_ACTION(unminimize),       {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_r,          AWL_ACTION(plugin_restart),   {0} },
    { MODKEY,                    XKB_KEY_0,          AWL_ACTION(view),             {.ui = ~0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_parenright, AWL_ACTION(tag),              {.ui = ~0} },
    { MODKEY,                    XKB_KEY_comma,      AWL_ACTION(focus_monitor),         {.i = -1} },
    { MODKEY,                    XKB_KEY_period,     AWL_ACTION(focus_monitor),         {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_less,       move_to_monitor_follow,          {.i = -1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_greater,    move_to_monitor_follow,          {.i = +1} },
    TAG_KEYS(          XKB_KEY_1, XKB_KEY_exclam,                     0),
    TAG_KEYS(          XKB_KEY_2, XKB_KEY_at,                         1),
    TAG_KEYS(          XKB_KEY_3, XKB_KEY_numbersign,                 2),
    TAG_KEYS(          XKB_KEY_4, XKB_KEY_dollar,                     3),
    TAG_KEYS(          XKB_KEY_5, XKB_KEY_percent,                    4),
    TAG_KEYS(          XKB_KEY_6, XKB_KEY_asciicircum,                5),
    TAG_KEYS(          XKB_KEY_7, XKB_KEY_ampersand,                  6),
    TAG_KEYS(          XKB_KEY_8, XKB_KEY_asterisk,                   7),
    TAG_KEYS(          XKB_KEY_9, XKB_KEY_parenleft,                  8),
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Q,          AWL_ACTION(quit),             {0} },

    /* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
    { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_Terminate_Server, AWL_ACTION(quit), {0} },
    /* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
     * do not remove them. */
    CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
    CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

static const Button buttons[] = {
    { ClkClient,   MODKEY, BTN_LEFT,   AWL_ACTION(move_resize),     {.ui = CurMove} },
    { ClkClient,   MODKEY, BTN_MIDDLE, AWL_ACTION(toggle_floating), {0} },
    { ClkClient,   MODKEY, BTN_RIGHT,  AWL_ACTION(move_resize),     {.ui = CurResize} },
};

static const Layout layouts[] = {
    /* symbol     arrange function */
    { "[]=",      AWL_LAYOUT(tile) },
    { "><>",      NULL },    /* no layout function means floating behavior */
    { "[M]",      AWL_LAYOUT(monocle) },
    { "###",      AWL_LAYOUT(gapless_grid) },
    { "TTT",      AWL_LAYOUT(bottom_stack) },
};

static const MonitorRule mon_rules[] = {
    /* name       master_factor  master_count scale layout rotate/reflect                x    y   per_tag */
    /* example of a HiDPI laptop monitor:
    { "eDP-1",    0.5f,  1,      2,    0,     WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1,  0 },
    */
    { NULL,       0.55f, 1,      1,    0,     WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1,  0 },
};

/* output profiles (the output_profile action, {.v = "docked"}), e.g.:
 * static const OutputSetting docked[] = {
 *     match                    on   x     y  scale  rotate/reflect
 *   { "eDP-1",                 0,   0,    0, 0,     WL_OUTPUT_TRANSFORM_NORMAL },
 *   { "Dell Inc. DELL U2722",  1,   0,    0, 1,     WL_OUTPUT_TRANSFORM_NORMAL },
 * };
 * static const OutputProfile output_profiles[] = {
 *   { "docked", { docked, LENGTH(docked) } },
 * };
 */

static const EnvVar environment[] = {
    { "XDG_CURRENT_DESKTOP", "wlroots" },
    { "MOZ_ENABLE_WAYLAND", "1" },
};

/* rules, e.g.:
 *   app_id       title  tags mask  is_floating  monitor  w    h    blur  1-alpha
 * { "Gimp",      NULL,  0,         1,          -1,      0,   0,   0,    0 },   floating
 * { "firefox",   NULL,  1 << 8,    0,          -1,      0,   0,   0,    0 },   on tag 9 only
 * { "foot",      NULL,  0,         1,          -1,      400, 300, 1,    0.2 }, floating 400x300, blurred, 80% opaque
 */

static const awl_config_t awl_config_defaults = {
    .mod_key = WLR_MODIFIER_ALT,
    .keys = { keys, LENGTH(keys) },
    .buttons = { buttons, LENGTH(buttons) },
    .layouts = { layouts, LENGTH(layouts) },
    .mon_rules = { mon_rules, LENGTH(mon_rules) },
    /* .output_profiles: none, see the example above */
    .environment = { environment, LENGTH(environment) },
    /* .rules: none, see the examples above */

    .bar = { .show = 1, .top = 1 },
    .ntags = 9,
    .sloppy_focus = 1,
    .bypass_surface_visibility = 0,
    .screensaver_inhibit = 0,
    .idle_seconds = 0, /* never idle */
    .on_idle = { on_idle, LENGTH(on_idle) },
    .systemd_session = 0,
    .colors = {
        .root           = 0x000000ff,
        .fullscreen     = 0x1a1a1aff,
        .border         = 0x444444ff,
        .border_focused = 0x005577ff,
        .border_urgent  = 0x770000ff,
        .border_clock_hour   = molokai_red,
        .border_clock_minute = molokai_orange,

        .popup_bg       = 0x3c3c3c4d, /* awl blurs what is behind it */
        .popup_fg       = 0xf8f8f2ff,
        .popup_border   = 0xa6e22eff,
        .popup_hover    = 0xffffff26, /* the row under the pointer, the buttons */
        .popup_dim      = 0xf8f8f259,
        /* .menu_*: all unset, the popups' */
        .alert_fg       = 0xffffffff,
        .alert_bg       = 0xc00000ff,
        /* awlock's logo: light gray cuts and outlines; the letters blue,
         * purple, orange (as awl's warnings), green, red */
        .lock_waiting    = { molokai_light_gray, molokai_blue },
        .lock_typing     = { molokai_light_gray, molokai_purple },
        .lock_caps_lock  = { molokai_light_gray, molokai_orange },
        .lock_validating = { molokai_light_gray, molokai_green },
        .lock_wrong      = { molokai_light_gray, molokai_red },
    },
    .border_px = 1,
    .font = "monospace:size=",
    .font_size = 10,
    .emoji_font = "emoji", /* NULL: text instead of emoji */
    .blur = {
        .strength = 1.0f, .alpha = 1.0f,
        .popups = 1,
        .window_list = 1,
    },
    .keyboard = {
        /* .xkb: .rules, .model, .layout, .variant, .options all NULL,
         * xkbcommon's defaults */
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
        .logo = NULL,    /* awlock's own (lock/logo.xpm) */
        .logo_scale = 3,
        .inactive_alpha = 0.4,
        .active_alpha = 1.0,
        .fingerprint_retry_ms = 1000,
        .fingerprint = 1,
        .at_start = 0,
        .on_suspend = 1,
        .blur = 1,
    },
    /* none; e.g. static const char *const autostart[][8] = {
     * { TRAY_ICONS, "nm-applet", NULL } }; SETLIST(c->autostart, autostart) */
    .autostart = { NULL, 0 },
    .wlr_log_level = WLR_ERROR,

    .alert = {
        .opacity = 0.9f, .font_scale = 1.5f,
        .margin = 12,
    },

    .tray = {
        .icon_theme = NULL, /* the desktop's */
        .icon_size = 16,
        .icon_gap = 4,
    },
    .popup = {
        .border_width = 1,
        .radius       = 15, /* the blur's corners */
        .padding      = 8,
        /* the gap to the bar and to the screen's sides (at most a quarter of
         * the screen) */
        .margin       = 6,
        .column_spacing = AWL_WINDOW_COLUMN_SPACING,
        .row_spacing  = AWL_WINDOW_ROW_SPACING,
        /* .font, .font_size: unset, the bar's */
        .calendar = {
            .list_min_height = 120,
            .list_max_height = 200,
        },
        /* .menu: the tray's and the notifications' menus, all unset, so
         * the popups' .border_width, .radius; .blur blur.popups */
        /* the notifications over fullscreen clients too */
        .persistent_overlay = 1,
    },
    .desktop = {
        /* .font, .font_size: unset, the bar's */
        .hidden_alpha = 0xa0,
        .margin = 10,
        .radius = 15,
        .columns = 3,
    },
    .notifications = {
        .timeout_low = 10000,
        .timeout_normal = 20000,
        .idle_pause = 10000,
        .idle_giveback = 0.33f,
        .idle_floor = 0.2f,
        .activate_urgent = false,
        .osd_skips_pause = true,
    },
    .wallpaper = {
        .dir = "Wallpapers", .fade_ms = 500, .interval = 7200, .mode = WallpaperRand,
    },
};
