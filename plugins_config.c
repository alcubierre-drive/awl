/** The configuration, built into libawlplugins.so, so `make` and
 * plugin_restart (MOD+Ctrl+r) apply it to the running awl -- keymap, input
 * devices, font, colors, borders, blur, layouts and the bar's widgets
 * included (monitor rules are applied to outputs appearing afterwards).
 * While no library is loaded, awl runs with its defaults; what only a
 * restart of awl applies are config.mk's compile-time switches.
 *
 * Only the part between the CUSTOM markers is yours to change: configure()
 * gets awl's defaults (awl_config_defaults.h,
 * `awl_host_t.config_defaults`) in c and the plugins'
 * (plugin_config_defaults.h) in p, and changes what it wants;
 * awl_config_t in awl.h and awl_plugin_config_t in plugins.h list
 * everything there is. The rest of this file is the library's: the
 * functions the config binds to keys and layouts are awl's, here
 * wrappers calling them through the host table (awl_action_<name>,
 * awl_layout_<name>: `AWL_ACTION()`, `AWL_LAYOUT()`), next to the
 * library's own actions. None of them is static, so the config may leave
 * any of them unbound; the library exports none. */
#include "awl.h"
#include "awl_plugin_abi.h"
#include "plugins.h"
#include "plugins/colors.h"
#include "plugin_config_defaults.h"

#define AWL_ACTION_REGISTER( name ) \
    void AWL_ACTION( name )( const Arg* arg ) { awl_host->actions->name( arg ); }
AWL_ACTIONS
#undef AWL_ACTION_REGISTER

#define AWL_LAYOUT_REGISTER( name ) \
    void AWL_LAYOUT( name )( Monitor* m ) { awl_host->arranges->name( m ); }
AWL_ARRANGES
#undef AWL_LAYOUT_REGISTER

void AWL_ACTION( notifyconfig )( const Arg* arg ) { awl_notify_config(); }
void AWL_ACTION( backlighttoggle )( const Arg* arg ) { awl_backlight_toggle(); }

/* ======================================================================
 * CUSTOM: the user's configuration, from here to "CUSTOM end".
 *
 * The bindings, rules, layouts, monitor rules, autostarts and the bar's
 * widgets are lists: to change one, copy its array from
 * awl_config_defaults.h (or plugin_config_defaults.h) here, change it, and
 * point the list at it in configure(), e.g. SETLIST(c->keys, keys) or
 * SETLIST(p->widgets, widgets).
 * In bindings, MODKEY is c->modkey. They can call AWL_ACTION(name) of
 * what AWL_ACTIONS lists (awl_plugin_abi.h) and of the library's actions
 * above (notifyconfig, backlighttoggle), and functions defined here by
 * their own name; layouts AWL_LAYOUT(name) of what AWL_ARRANGES lists. The
 * molokai_* colors are in plugins/colors.h.
 * ====================================================================== */

static const Rule rules[] = {
    /* app_id             title       tags mask     isfloating   monitor w h blur 1-alpha*/
    // { NULL,               "nomacs_garfield", 0,     1,           -1,   0,   0, 0, 0 },
    { "python3",          "Figure",   0,            1,           -1,   0,   0, 0, 0 },
    { "wdisplays",        NULL,       0,            1,           -1,   0,   0, 0, 0 },

    { "zoom",             NULL,       0,            1,           -1,   0,   0, 0, 0 },
    { "Zoom",             NULL,       0,            1,           -1,   0,   0, 0, 0 },

    { "org.telegram.desktop",NULL,    1<<7,         0,           -1,   0,   0, 0, 0 },
    { "signal",           "Signal",   1<<7,         0,           -1,   0,   0, 0, 0 },
    { "mattermost-desktop","Mattermost Desktop App",1<<7,0,      -1,   0,   0, 0, 0 },

    { "org.gnome.Evolution",NULL,     1<<8,         0,           -1,   0,   0, 0, 0 },
    { "evolution-alarm-notify", NULL, 1<<8,         1,           -1,   0,   0, 0, 0 },
    { "org.pulseaudio.pavucontrol","Volume Control",0,1,         -1,   0,   0, 0, 0 },
    // { "kitty",            NULL,       0,            0,           -1,   0,   0, 0, 0 },
};

/* layout(s); setlayout/cycle_layout switch between them. After a reload
 * every monitor keeps the layout at the same index. */
static const Layout layouts[] = {
    /* symbol     arrange function */
    { "[|]",      AWL_LAYOUT(tile) },
    { "[·]",      NULL },    /* no layout function means floating behavior */
    { "[M]",      AWL_LAYOUT(monocle) },
    { "[×]",      AWL_LAYOUT(gaplessgrid) },
    { "[–]",      AWL_LAYOUT(bstack) },
};

/* monitors */
static const MonitorRule monrules[] = {
    /* name       mfact  nmaster scale layout       rotate/reflect                x    y  pertag*/
    /* example of a HiDPI laptop monitor:
    */
    /* defaults */
    { "eDP-1",    0.5f,  1,    1.5,    0,           WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1, 0 },
    /* { "HDMI-A-1", 0.5f,  1,      1,    0,           WL_OUTPUT_TRANSFORM_90,       -1,  -1, 0 }, */
    { NULL,       0.5f,  1,      1,    0,           WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1, 0 },
};

/* commands */
static const char *termcmd[] = { "kitty", "-d", "$HOME", NULL };
static const char *menucmd[] = { "fuzzel", NULL };
static const char *suspend[] = { "systemctl", "suspend", NULL };
static const char *hyprcopy[] = { "hyprcopy", NULL };
static const char *brightp[] = { "backlight-tooler", "-v5", NULL };
static const char *brightm[] = { "backlight-tooler", "-v-5", NULL };
static const char *volp[] = { "pactl", "set-sink-volume", "@DEFAULT_SINK@", "+5%", NULL };
static const char *volm[] = { "pactl", "set-sink-volume", "@DEFAULT_SINK@", "-5%", NULL };
static const char *volt[] = { "pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", NULL };
static const char *volT[] = { "pactl", "set-source-mute", "@DEFAULT_SOURCE@", "toggle", NULL };
static const char *speakerT[] = { "pulse_port_switch", NULL };
static const char *grimslurp[] = { "grim_slurp", NULL };
static const char *wdisplays[] = { "wdisplays", NULL };
static const char *docked_r[] = { "docked", "reset", NULL };
static const char *docked_d[] = { "docked", "dock", NULL };
static const char *docked_z[] = { "docked", "zoom", NULL };
static const char *notification_action[] = { "fnottctl", "actions", NULL };
// static const char *garfield[] = { "garfield", NULL };

/* started with awl (only at its start), terminated when it exits;
 * TRAY_ICONS first: in the tray's icon theme */
static const char* autostart[][8] = {
    { "fnott", NULL },
    { TRAY_ICONS, "nm-applet", NULL },
    { "blueman-applet", NULL },
    { "system-config-printer-applet", NULL },
    { "Telegram", NULL },
    { "evolution", NULL },
};
static const char* locker[] = { "swaylock", "-c", "00000000", "-p", NULL };

static void tagmonf( const Arg* arg ) { AWL_ACTION(tagmon)(arg); AWL_ACTION(focusmon)(arg); }

static const Key keys[] = {
    /* Note that Shift changes certain key codes: c -> C, 2 -> at, etc. */
    /* modifier                  key                 function                    argument */
    { MODKEY,                    XKB_KEY_p,          AWL_ACTION(spawn),            {.v = menucmd} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_P,          AWL_ACTION(notifyconfig),     {0} },
    { MODKEY,                    XKB_KEY_Return,     AWL_ACTION(spawn),            {.v = termcmd} },
    { MODKEY,                    XKB_KEY_y,          AWL_ACTION(backlighttoggle),  {0} },
    { MODKEY,                    XKB_KEY_i,          AWL_ACTION(togglebar),        {0} },
    { MODKEY,                    XKB_KEY_j,          AWL_ACTION(focusstack),       {.i = +1} },
    { MODKEY,                    XKB_KEY_k,          AWL_ACTION(focusstack),       {.i = -1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_J,          AWL_ACTION(movestack),        {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_K,          AWL_ACTION(movestack),        {.i = -1} },
    /*{ MODKEY,                    XKB_KEY_i,          AWL_ACTION(incnmaster),     {.i = +1} },*/
    /*{ MODKEY,                    XKB_KEY_d,          AWL_ACTION(incnmaster),     {.i = -1} },*/
    { MODKEY,                    XKB_KEY_h,          AWL_ACTION(setmfact),         {.f = -0.05f} },
    { MODKEY,                    XKB_KEY_l,          AWL_ACTION(setmfact),         {.f = +0.05f} },
    { MODKEY,                    XKB_KEY_Tab,        AWL_ACTION(view),             {0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_C,          AWL_ACTION(killclient),       {0} },
    { MODKEY,                    XKB_KEY_m,          AWL_ACTION(maximize),         {0} },
    { MODKEY,                    XKB_KEY_space,      AWL_ACTION(cycle_layout),     {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_space,      AWL_ACTION(cycle_layout),     {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,XKB_KEY_space,AWL_ACTION(togglepertag),{0} },
    { MODKEY,                    XKB_KEY_Right,      AWL_ACTION(cycle_view),       {.i = +1} },
    { MODKEY,                    XKB_KEY_Left,       AWL_ACTION(cycle_view),       {.i = -1} },
    { MODKEY,                    XKB_KEY_f,          AWL_ACTION(togglefullscreen), {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_space,      AWL_ACTION(togglefloating),   {0} },
    { MODKEY,                    XKB_KEY_t,          AWL_ACTION(toggleontop),      {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_r,          AWL_ACTION(plugin_restart),   {0} },
    { MODKEY,                    XKB_KEY_w,          AWL_ACTION(wallpaper),        {.i = WallpaperTimerNext} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_W,          AWL_ACTION(wallpaper),        {.i = WallpaperTimerBack} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_N,          AWL_ACTION(spawn),            {.v = notification_action} },
    // { MODKEY,                    XKB_KEY_g,          AWL_ACTION(spawn),            {.v = garfield} },
    { MODKEY,                    XKB_KEY_b,          AWL_ACTION(togglebw),         {0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_B,          AWL_ACTION(changebw),         {.i=+1} },
    { MODKEY|WLR_MODIFIER_SHIFT|WLR_MODIFIER_CTRL,XKB_KEY_B, AWL_ACTION(changebw), {.i=-1} },
    { MODKEY|WLR_MODIFIER_SHIFT|WLR_MODIFIER_CTRL,XKB_KEY_S, AWL_ACTION(spawn),    {.v = suspend} },

    { MODKEY,                    XKB_KEY_n,          AWL_ACTION(minimize),         {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_n,          AWL_ACTION(unminimize),       {0} },
    { MODKEY,                    XKB_KEY_c,          AWL_ACTION(spawn),            {.v = hyprcopy} },
    { 0, XKB_KEY_XF86MonBrightnessUp,                AWL_ACTION(spawn),            {.v=brightp} },
    { 0, XKB_KEY_XF86MonBrightnessDown,              AWL_ACTION(spawn),            {.v=brightm} },
    { 0, XKB_KEY_XF86AudioRaiseVolume,               AWL_ACTION(spawn),            {.v=volp} },
    { 0, XKB_KEY_XF86AudioLowerVolume,               AWL_ACTION(spawn),            {.v=volm} },
    { 0, XKB_KEY_XF86AudioMute,                      AWL_ACTION(spawn),            {.v=volt} },
    { 0, XKB_KEY_XF86AudioMicMute,                   AWL_ACTION(spawn),            {.v=volT} },
    { MODKEY,                    XKB_KEY_F1,         AWL_ACTION(spawn),            {.v = speakerT} },
    { 0,                         XKB_KEY_Print,      AWL_ACTION(spawn),            {.v=grimslurp} },
    { MODKEY,                    XKB_KEY_d,          AWL_ACTION(spawn),            {.v=wdisplays} },
    { 0,                         XKB_KEY_XF86Display,AWL_ACTION(spawn),            {.v=wdisplays} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_G,          AWL_ACTION(lockscreen),       {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_z,          AWL_ACTION(transluce),        {.i=+1} },
    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,XKB_KEY_Z,AWL_ACTION(transluce), {.i=-1} },

    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,XKB_KEY_D, AWL_ACTION(spawn),   {.v=docked_r} },
    { MODKEY|WLR_MODIFIER_CTRL,     XKB_KEY_d,         AWL_ACTION(spawn),         {.v=docked_d} },
    { MODKEY|WLR_MODIFIER_SHIFT,    XKB_KEY_D,         AWL_ACTION(spawn),         {.v=docked_z} },
    /*{ MODKEY,                    XKB_KEY_0,          AWL_ACTION(view),           {.ui = ~0} },*/
    /*{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_parenright, AWL_ACTION(tag),            {.ui = ~0} },*/

    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_h,          AWL_ACTION(incnmaster),     {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_l,          AWL_ACTION(incnmaster),     {.i = +1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_j,          AWL_ACTION(focusmon),       {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_k,          AWL_ACTION(focusmon),       {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_O,          tagmonf,        {.i = -1} },
    { MODKEY|WLR_MODIFIER_SHIFT|WLR_MODIFIER_CTRL,XKB_KEY_O,tagmonf, {.i = +1} },
    TAGKEYS(          XKB_KEY_1, XKB_KEY_exclam,                     0),
    TAGKEYS(          XKB_KEY_2, XKB_KEY_quotedbl,                   1),
    TAGKEYS(          XKB_KEY_3, XKB_KEY_section,                    2),
    TAGKEYS(          XKB_KEY_4, XKB_KEY_dollar,                     3),
    TAGKEYS(          XKB_KEY_5, XKB_KEY_percent,                    4),
    TAGKEYS(          XKB_KEY_6, XKB_KEY_ampersand,                  5),
    TAGKEYS(          XKB_KEY_7, XKB_KEY_slash,                      6),
    TAGKEYS(          XKB_KEY_8, XKB_KEY_parenleft,                  7),
    TAGKEYS(          XKB_KEY_9, XKB_KEY_parenright,                 8),
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_equal,          AWL_ACTION(quit),       {0} },

    /* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
    { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_Terminate_Server, AWL_ACTION(quit), {0} },
    /* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
     * do not remove them.
     */
    CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
    CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

/* configure() lists every setting: commented out with its default
 * (awl_config_defaults.h, plugin_config_defaults.h) where it is left as
 * it is. */
static void
configure(awl_config_t *c, awl_plugin_config_t *p)
{
    /* bindings and lists; the default arrays are awl_config_defaults.h's and
     * plugin_config_defaults.h's */
    // c->modkey = WLR_MODIFIER_ALT;
    c->modkey = WLR_MODIFIER_LOGO;
    SETLIST(c->keys, keys);
    // SETLIST(c->buttons, buttons);
    SETLIST(c->rules, rules); /* default: none */
    SETLIST(c->layouts, layouts);
    SETLIST(c->monrules, monrules);
    SETLIST(c->autostart, autostart); /* default: none */
    // SETLIST(p->widgets, default_widgets);

    /* the bar, tags, focus, logging */
    // c->bar.show = 1;
    c->bar.top = 0; /* the bar at the bottom */
    // c->ntags = 9;
    // c->sloppyfocus = 1;
    // c->bypass_surface_visibility = 0;
    // c->wlr_log_level = WLR_ERROR;

    /* the screen lock; awl_config_defaults.h's locker is
     * { "swaylock", "-c", "00000000", NULL } */
    c->lock.locker = locker;
    c->lock.at_start = 1;
    // c->lock.on_suspend = 1;
    // c->lock.blur = 1;

    /* appearance */
    c->borderpx = 2;
    c->bordercolors[BorderNorm] = molokai_gray;   /* 0x444444ff */
    c->bordercolors[BorderSel]  = molokai_blue;   /* 0x005577ff */
    c->bordercolors[BorderUrg]  = molokai_orange; /* 0x770000ff */
    // c->font = "monospace:size=";
    // c->fontsize = 10;
    // c->rootcolor = 0x000000ff;
    // c->fullscreen_bg = 0x1a1a1aff;

    /* blur */
    // c->blur.strength = 1.0f;
    // c->blur.alpha = 1.0f;
    // c->blur.notifications = 1;
    // c->blur.notifications_radius = 15;
    // c->blur.launcher = 1;
    // c->blur.launcher_radius = 15;
    c->blur.windowlist = 0;

    /* the bar's colors */
    // p->colors.bg            = molokai_light_gray;
    // p->colors.fg            = 0xf5efffff;
    // p->colors.tag_bg        = molokai_dark_gray;
    // p->colors.tag_occupied  = 0xfd971faa; /* molokai_orange, two thirds */
    // p->colors.tag_urgent    = 0xf92672aa; /* molokai_red */
    // p->colors.tag_selected  = 0xae81ffaa; /* molokai_purple */
    // p->colors.selmon        = molokai_blue;
    // p->colors.pertag        = molokai_red;
    // p->colors.win_bg        = 0x42375aff;
    // p->colors.win_focused   = 0x7359a5ff;
    // p->colors.win_urgent    = molokai_red;
    // p->colors.win_minimized = molokai_dark_gray;
    // p->colors.win_fg        = 0xf5efffff;
    // p->colors.clock_meter   = molokai_green;
    // p->colors.stats_bg      = molokai_dark_gray;
    // p->colors.cpu           = molokai_blue;
    // p->colors.io            = 0x336c77ff; /* the CPU's, half as bright */
    // p->colors.mem           = molokai_orange;
    // p->colors.swap          = molokai_green;
    // p->colors.good          = molokai_green;
    // p->colors.warning       = molokai_orange;
    // p->colors.bad           = molokai_red;

    /* the tray; what the icon theme lacks comes from the desktop's (NULL:
     * the desktop's) */
    c->tray.icon_theme = "Papirus-Dark"; /* NULL */
    // c->tray.bg = molokai_light_gray;
    c->tray.bg         = 0x657374ff;     /* 0x859394ff */
    // c->tray.icon_size  = 16;
    // c->tray.icon_gap   = 4;

    /* the popups */
    // c->popup.bg             = 0x3c3c3c4d; /* awl blurs what is behind it */
    // c->popup.fg             = 0xf8f8f2ff;
    // c->popup.border = molokai_blue;
    // c->popup.border = molokai_light_gray;
    c->popup.border         = 0x657374ff;  /* 0xa6e22eff */
    // c->popup.hover          = 0xffffff26;
    // c->popup.dim            = 0xf8f8f259;
    c->popup.border_width   = c->borderpx; /* 1 */
    // c->popup.radius         = 15;
    // c->popup.padding        = 8;
    // c->popup.margin         = 6;
    // c->popup.column_spacing = AWL_WINDOW_COLUMN_SPACING;
    // c->popup.row_spacing    = AWL_WINDOW_ROW_SPACING;
    c->popup.font           = "sans:size="; /* NULL: the bar's */
    c->popup.fontsize       = 10;           /* 0: the bar's */
    // c->popup.calendar.today    = 0xb6ec52ff;
    // c->popup.calendar.selected = 0xffffff33;
    // c->popup.calendar.list_min_height = 120;
    // c->popup.calendar.list_max_height = 200;

    /* the tray's menus; 0 is the popups' */
    c->popup.menu.bg           = 0x3c3c3c4d; /* the popups' (bg, fg, hover) */
    c->popup.menu.fg           = 0xf8f8f2ff;
    c->popup.menu.border       = 0x859394ff;
    c->popup.menu.highlight    = 0xffffff26;
    // c->popup.menu.highlight_fg = 0; /* the menu's fg */
    c->popup.menu.dim          = 0xf8f8f259; /* disabled entries, shortcuts, separators */
    c->popup.menu.border_width = 2;
    c->popup.menu.radius       = 4;
    c->popup.menu.blur         = 1;          /* 0: blur.launcher */

    /* the desktop file list */
    c->desktop.font         = "sans:size="; /* NULL: the bar's */
    c->desktop.fontsize     = 10;           /* 0: the bar's */
    // c->desktop.bg           = 0x3c3c3c4c;
    // c->desktop.file         = 0xf8f8f2ff;
    // c->desktop.dir          = molokai_blue;
    // c->desktop.broken       = molokai_red;
    // c->desktop.hidden_alpha = 0xa0;
    // c->desktop.margin       = 10;

    /* the wallpaper */
    // c->wallpaper.dir      = "Wallpapers";
    // c->wallpaper.fade_ms  = 500;
    // c->wallpaper.interval = 7200;
    // c->wallpaper.mode     = WallpaperRand;

    /* the alert */
    // c->alert.fg         = 0xffffffff;
    // c->alert.bg         = 0xc00000ff;
    // c->alert.opacity    = 0.9f;
    // c->alert.font_scale = 1.5f;
    // c->alert.margin     = 12;

    /* keyboard; NULL: the default */
    // c->keyboard.xkb.rules   = NULL;
    c->keyboard.xkb.model   = "pc105";
    c->keyboard.xkb.layout  = "de";
    c->keyboard.xkb.variant = "nodeadkeys";
    // c->keyboard.xkb.options = NULL;
    // c->keyboard.repeat_rate  = 25;
    // c->keyboard.repeat_delay = 600;

    /* pointer devices */
    // c->pointer.tap_to_click = 1;
    // c->pointer.tap_and_drag = 1;
    // c->pointer.drag_lock = 1;
    // c->pointer.natural_scrolling = 0;
    // c->pointer.disable_while_typing = 1;
    // c->pointer.left_handed = 0;
    // c->pointer.middle_button_emulation = 0;
    // c->pointer.scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;
    // c->pointer.click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
    // c->pointer.send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
    // c->pointer.accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
    // c->pointer.accel_speed = 0.0;
    // c->pointer.button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

    /* the temperature's color scale (°C) and the battery */
    // p->temperature.coretemp.min     = 50;
    // p->temperature.coretemp.max     = 100;
    // p->temperature.thermal_zone.min = 40;
    // p->temperature.thermal_zone.max = 90;
    // p->battery.dir = "/sys/class/power_supply/BAT0/";
    p->battery.scale.bad     = 10;
    p->battery.scale.warning = 20;
    p->battery.scale.ok      = 30;
}

/* ======================================================================
 * CUSTOM end: below is the library's again.
 * ====================================================================== */

static awl_config_t cfg;
static awl_plugin_config_t plugin_cfg;

void awl_config_init( void ) {
    awl_host->config_defaults( &cfg );
    plugin_cfg = awl_plugin_config_defaults;
    configure( &cfg, &plugin_cfg );
}

const awl_config_t* awl_config( void ) {
    return &cfg;
}

const awl_plugin_config_t* awl_plugin_config( void ) {
    return &plugin_cfg;
}

void awl_config_fill( awl_config_t* c ) {
    *c = *awl_config();
}
