/** The configuration, built into libawlplugins.so, so ``ninja -C build`` and
 * plugin_restart (MOD+Ctrl+r) apply it to the running awl -- keymap, input
 * devices, font, colors, borders, blur, layouts and the bar's widgets
 * included (monitor rules are applied to outputs appearing afterwards).
 * While no library is loaded, awl runs with its defaults; what only a
 * restart of awl applies are meson.options' compile-time switches.
 *
 * Only the part between the CUSTOM markers is yours to change: configure()
 * gets awl's defaults (awl_config_defaults.h,
 * :c:member:`awl_host_t.config_defaults`) in c and the plugins'
 * (plugins_config_defaults.h) in p, and changes what it wants; those two
 * list every setting with its default, :c:type:`awl_config_t` in awl.h and
 * :c:type:`awl_plugin_config_t` in plugins.h what each means. The rest of this file is the library's: the
 * functions the config binds to keys and layouts are awl's, here
 * wrappers calling them through the host table (awl_action_<name>,
 * awl_layout_<name>: :c:macro:`AWL_ACTION() <AWL_ACTION>`, :c:macro:`AWL_LAYOUT() <AWL_LAYOUT>`), next to the
 * library's own actions. None of them is static, so the config may leave
 * any of them unbound; the library exports none. */
#include "awl.h"
#include "awl_plugin_abi.h"
#include "plugins.h"
#include "plugins/colors.h"
#include "plugins_config_defaults.h"
#include "plugins/logind.h"

#define AWL_ACTION_REGISTER( name ) \
    void AWL_ACTION( name )( const Arg* arg ) { awl_host->actions->name( arg ); }
AWL_ACTIONS
#undef AWL_ACTION_REGISTER

#define AWL_LAYOUT_REGISTER( name ) \
    void AWL_LAYOUT( name )( Monitor* m ) { awl_host->arranges->name( m ); }
AWL_ARRANGES
#undef AWL_LAYOUT_REGISTER

/** the notifications' config popup (:c:func:`awl_notify_config`) */
static void AWL_ACTION( notifyconfig )( const Arg* arg ) {
    awl_notify_config();
}

/** opens or closes the launcher, arg->i its mode (:c:enumerator:`AWL_LAUNCHER_APPS`, :c:enumerator:`AWL_LAUNCHER_RUN`) (:c:func:`awl_launcher_toggle`) */
static void AWL_ACTION( launcher )( const Arg* arg ) {
    awl_launcher_toggle( arg->i );
}

/** switches the backlight daemon's automatic updates on or off (:c:func:`backlight_toggle`) */
static void AWL_ACTION( backlighttoggle )( const Arg* arg ) {
    awl_plugin_data_t* p = awl_plugin_get();
    if (p) backlight_toggle( p->backlight );
}

/** changes the screen's brightness by arg->f (:c:func:`awl_brightness_adjust`) */
static void AWL_ACTION( brightness )( const Arg* arg ) {
    awl_brightness_adjust( arg->f );
}

/** changes the volume by arg->f (:c:func:`awl_volume_adjust`) */
static void AWL_ACTION( volume )( const Arg* arg ) {
    awl_volume_adjust( arg->f );
}

/** mutes or unmutes the sink (:c:func:`awl_volume_mute_toggle`) */
static void AWL_ACTION( volumemute )( const Arg* arg ) {
    awl_volume_mute_toggle();
}

/** mutes or unmutes the source (:c:func:`awl_mic_mute_toggle`) */
static void AWL_ACTION( micmute )( const Arg* arg ) {
    awl_mic_mute_toggle();
}

/** mutes or unmutes the speaker port only (:c:func:`awl_speaker_mute_toggle`) */
static void AWL_ACTION( speakermute )( const Arg* arg ) {
    awl_speaker_mute_toggle();
}

/** suspends through logind (:c:func:`awl_logind_suspend`) */
static void AWL_ACTION( suspend )( const Arg* arg ) {
    awl_logind_suspend();
}

/** sends the window to the monitor arg->i over and follows it there */
static void AWL_ACTION( tagmonf )( const Arg* arg ) {
    AWL_ACTION(tagmon)(arg);
    AWL_ACTION(focusmon)(arg);
}

/* ======================================================================
 * CUSTOM: the user's configuration, from here to "CUSTOM end".
 *
 * The bindings, rules, layouts, monitor rules, output profiles, the
 * environment, autostarts and the bar's widgets are lists: to change one, copy its array from
 * awl_config_defaults.h (or plugins_config_defaults.h) here, change it, and
 * point the list at it in configure(), e.g. SETLIST(c->keys, keys) or
 * SETLIST(p->widgets, widgets).
 * In bindings, MODKEY is c->modkey. They can call AWL_ACTION(name) of
 * what AWL_ACTIONS lists (awl_plugin_abi.h) and of the library's actions
 * above (notifyconfig, launcher, backlighttoggle), and functions defined here by
 * their own name; layouts AWL_LAYOUT(name) of what AWL_ARRANGES lists. The
 * molokai_* colors are in awl.h. A setting that may be left unset (an
 * awl_opt_int_t or awl_opt_color_t) is set with AWL_SET(setting, value).
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
    { "eDP-1",    0.5f,  1,    1.5,    0,           WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1, 0 },
    /* { "HDMI-A-1", 0.5f,  1,      1,    0,           WL_OUTPUT_TRANSFORM_90,       -1,  -1, 0 }, */
    { NULL,       0.5f,  1,      1,    0,           WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1, 0 },
};

/* output profiles (the outputprofile action): the laptop alone, or docked at
 * WUE with the two Fujitsus, or with the laptop below them as a third */
#define WUE_LEFT  "Fujitsu Siemens Computers GmbH B24-9 WS YVDJ005432"
#define WUE_RIGHT "Fujitsu Siemens Computers GmbH B24-9 WS YVDJ004877"
#define LAPTOP    "Samsung Display Corp. 0x419F"
static const OutputSetting profile_laptop[] = {
    /* match       on   x     y     scale  rotate/reflect */
    { LAPTOP,      1,   0,    0,    1.5,   WL_OUTPUT_TRANSFORM_NORMAL },
    { WUE_LEFT,    0,   0,    0,    0,     WL_OUTPUT_TRANSFORM_NORMAL },
    { WUE_RIGHT,   0,   0,    0,    0,     WL_OUTPUT_TRANSFORM_NORMAL },
};
static const OutputSetting profile_docked[] = {
    { LAPTOP,      0,   0,    0,    0,     WL_OUTPUT_TRANSFORM_NORMAL },
    { WUE_LEFT,    1,   0,    0,    1,     WL_OUTPUT_TRANSFORM_NORMAL },
    { WUE_RIGHT,   1,   1920, 0,    1,     WL_OUTPUT_TRANSFORM_NORMAL },
};
static const OutputSetting profile_triple[] = {
    { LAPTOP,      1,   0,    1200, 1.5,   WL_OUTPUT_TRANSFORM_NORMAL },
    { WUE_LEFT,    1,   0,    0,    1,     WL_OUTPUT_TRANSFORM_NORMAL },
    { WUE_RIGHT,   1,   1920, 0,    1,     WL_OUTPUT_TRANSFORM_NORMAL },
};
static const OutputProfile outputprofiles[] = {
    { "laptop", { profile_laptop, LENGTH(profile_laptop) } },
    { "docked", { profile_docked, LENGTH(profile_docked) } },
    { "triple", { profile_triple, LENGTH(profile_triple) } },
};

/* awl's environment, for what it starts (NULL unsets) */
static const EnvVar environment[] = {
    { "XDG_CURRENT_DESKTOP",         "wlroots" },
    { "MOZ_ENABLE_WAYLAND",          "1" },
    { "QT_STYLE_OVERRIDE",           "kvantum" },
    { "DESKTOP_SESSION",             "kde" },
    { "QT_AUTO_SCREEN_SCALE_FACTOR", "0" },
    { "EDITOR",                      "nvim" },
    { "SYSTEMD_EDITOR",              "/usr/bin/nvim" },
    { "SSH_AUTH_SOCK",               "1" },
    { "NO_AT_BRIDGE",                "1" },
};

/* commands */
static const char *termcmd[] = { "kitty", "-d", "$HOME", NULL };
static const char *const launcher_terminal[] = { "kitty", NULL };
static const char *hyprcopy[] = { "hyprcopy", NULL };
static const char *grimslurp[] = { "grim_slurp", NULL };
static const char *wdisplays[] = { "wdisplays", NULL };

/* started with awl (only at its start), terminated when it exits;
 * TRAY_ICONS first: in the tray's icon theme */
static const char* autostart[][8] = {
    { TRAY_ICONS, "nm-applet", NULL },
    { "blueman-applet", NULL },
    { "system-config-printer-applet", NULL },
    { "Telegram", NULL },
    { "evolution", NULL },
};
static const char* locker[] = { "swaylock", "-c", "00000000", "-p", NULL };

static const Key keys[] = {
    /* Note that Shift changes certain key codes: c -> C, 2 -> at, etc. */
    /* modifier                  key                 function                    argument */
    { MODKEY,                    XKB_KEY_p,          AWL_ACTION(launcher),         {.i = AWL_LAUNCHER_APPS} },
    { MODKEY,                    XKB_KEY_r,          AWL_ACTION(launcher),         {.i = AWL_LAUNCHER_RUN} },
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
    { MODKEY,                    XKB_KEY_b,          AWL_ACTION(togglebw),         {0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_B,          AWL_ACTION(changebw),         {.i=+1} },
    { MODKEY|WLR_MODIFIER_SHIFT|WLR_MODIFIER_CTRL,XKB_KEY_B, AWL_ACTION(changebw), {.i=-1} },
    { MODKEY|WLR_MODIFIER_SHIFT|WLR_MODIFIER_CTRL,XKB_KEY_S, AWL_ACTION(suspend),  {0} },

    { MODKEY,                    XKB_KEY_n,          AWL_ACTION(minimize),         {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_n,          AWL_ACTION(unminimize),       {0} },
    { MODKEY,                    XKB_KEY_c,          AWL_ACTION(spawn),            {.v = hyprcopy} },
    { 0, XKB_KEY_XF86MonBrightnessUp,                AWL_ACTION(brightness),       {.f =  0.05f} },
    { 0, XKB_KEY_XF86MonBrightnessDown,              AWL_ACTION(brightness),       {.f = -0.05f} },
    { 0, XKB_KEY_XF86AudioRaiseVolume,               AWL_ACTION(volume),           {.f =  0.05f} },
    { 0, XKB_KEY_XF86AudioLowerVolume,               AWL_ACTION(volume),           {.f = -0.05f} },
    { 0, XKB_KEY_XF86AudioMute,                      AWL_ACTION(volumemute),       {0} },
    { 0, XKB_KEY_XF86AudioMicMute,                   AWL_ACTION(micmute),          {0} },
    { MODKEY,                    XKB_KEY_F1,         AWL_ACTION(speakermute),      {0} },
    { 0,                         XKB_KEY_Print,      AWL_ACTION(spawn),            {.v=grimslurp} },
    { MODKEY,                    XKB_KEY_d,          AWL_ACTION(spawn),            {.v=wdisplays} },
    { 0,                         XKB_KEY_XF86Display,AWL_ACTION(spawn),            {.v=wdisplays} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_G,          AWL_ACTION(lockscreen),       {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_z,          AWL_ACTION(transluce),        {.i=+1} },
    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,XKB_KEY_Z,AWL_ACTION(transluce), {.i=-1} },

    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,XKB_KEY_D, AWL_ACTION(outputprofile), {.v="laptop"} },
    { MODKEY|WLR_MODIFIER_CTRL,     XKB_KEY_d,         AWL_ACTION(outputprofile), {.v="docked"} },
    { MODKEY|WLR_MODIFIER_SHIFT,    XKB_KEY_D,         AWL_ACTION(outputprofile), {.v="triple"} },
    /*{ MODKEY,                    XKB_KEY_0,          AWL_ACTION(view),           {.ui = ~0} },*/
    /*{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_parenright, AWL_ACTION(tag),            {.ui = ~0} },*/

    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_h,          AWL_ACTION(incnmaster),     {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_l,          AWL_ACTION(incnmaster),     {.i = +1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_j,          AWL_ACTION(focusmon),       {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_k,          AWL_ACTION(focusmon),       {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_O,          AWL_ACTION(tagmonf),        {.i = -1} },
    { MODKEY|WLR_MODIFIER_SHIFT|WLR_MODIFIER_CTRL,XKB_KEY_O,AWL_ACTION(tagmonf), {.i = +1} },
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

/* configure() changes what differs from the defaults; awl_config_defaults.h
 * and plugins_config_defaults.h list every setting with its default. */
static void
configure(awl_config_t *c, awl_plugin_config_t *p)
{
    /* bindings and lists */
    c->modkey = WLR_MODIFIER_LOGO;
    SETLIST(c->keys, keys);
    SETLIST(c->rules, rules);
    SETLIST(c->layouts, layouts);
    SETLIST(c->monrules, monrules);
    SETLIST(c->outputprofiles, outputprofiles);
    SETLIST(c->environment, environment);
    SETLIST(c->autostart, autostart);

    /* the bar */
    c->bar.top = 0; /* the bar at the bottom */

    /* the screen lock */
    c->lock.locker = locker;
    c->lock.at_start = 1;

    /* colors */
    c->colors.border          = molokai_gray;
    c->colors.border_focused  = molokai_blue;
    c->colors.border_urgent   = molokai_orange;
    // c->colors.popup_border = molokai_blue;
    // c->colors.popup_border = molokai_light_gray;
    c->colors.popup_border    = 0x657374ff;
    /* the tray's menus */
    AWL_SET(c->colors.menu_bg,         0x3c3c3c4d);
    AWL_SET(c->colors.menu_fg,         0xf8f8f2ff);
    AWL_SET(c->colors.menu_border,     0x859394ff);
    AWL_SET(c->colors.menu_highlight,  0xffffff26);
    AWL_SET(c->colors.menu_dim,        0xf8f8f259);
    p->colors.tray_bg         = molokai_light_gray;
    AWL_SET(p->colors.launcher_prompt, color_mix_8(molokai_red, molokai_orange, 0.5));
    AWL_SET(p->colors.launcher_query,  molokai_orange);

    p->launcher.prompt = "<b>launch &gt; </b>";
    p->launcher.run_prompt = "<b>run $ </b>";

    /* appearance */
    c->borderpx = 2;

    /* blur */
    c->blur.windowlist = 0;

    /* the tray */
    c->tray.icon_theme = "Papirus-Dark";

    /* the notifications' bell, from the icon theme */
    p->notify.bell_icon = "notifications-symbolic;preferences-system-notifications-symbolic";
    p->notify.bell_icon_dnd = "notifications-disabled-symbolic";
    c->notifications.activate_urgent = 1;

    /* the popups */
    c->popup.border_width   = c->borderpx;
    c->popup.font           = "sans:size=";
    AWL_SET(c->popup.fontsize,       10);

    /* the tray's menus */
    AWL_SET(c->popup.menu.border_width, 2);
    AWL_SET(c->popup.menu.radius,       4);
    AWL_SET(c->popup.menu.blur,         1);

    /* the desktop file list */
    c->desktop.font         = "sans:size=";
    AWL_SET(c->desktop.fontsize,     10);
    c->desktop.margin       = 20;
    c->desktop.columns      = 2;

    /* keyboard */
    c->keyboard.xkb.model   = "pc105";
    c->keyboard.xkb.layout  = "de";
    c->keyboard.xkb.variant = "nodeadkeys";

    /* the built-in speaker, muted on its own (MOD+F1) */
    p->volume.speaker = "alsa_output.pci-0000_00_1f.3-platform-skl_hda_dsp_generic.HiFi__Speaker__sink";

    /* the battery's colors by its charge: {at, color}, at 0 to 1,
     * linearly in between */
    p->colors.battery = (awl_colormap_t)COLORMAP( { 0.1f, molokai_red }, { 0.2f, molokai_orange },
                                                  { 0.3f, 0xf5efffff } );

    /* the launcher (plugins/launcher.h) */
    p->launcher.layout       = AWL_LAUNCHER_BAR; /* a strip over the bar */
    p->launcher.strip_at     = AWL_WINDOW_AT_STRIP_TOP; /* or _AT_BAR, _AT_STRIP_BOTTOM */
    p->launcher.terminal     = launcher_terminal;
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
    awl_config_fill_defaults( &cfg );
    awl_plugin_config_fill_defaults( &plugin_cfg, &cfg );
}

void awl_plugin_config_fill_defaults( awl_plugin_config_t* p, const awl_config_t* c ) {
    awl_plugin_colors_t* k = &p->colors;
    const awl_colors_t* ak = &c->colors;
    AWL_FILL( k->launcher_match, ak->popup_fg );
    AWL_FILL( k->launcher_dim, ak->popup_dim );
    AWL_FILL( k->launcher_prompt, k->launcher_dim.v );
    AWL_FILL( k->launcher_query, ak->popup_fg );
    AWL_FILL( k->notify_low_fg, ak->popup_fg );
    AWL_FILL( k->notify_low_bg, ak->popup_bg );
    AWL_FILL( k->notify_low_frame, ak->popup_border );
    AWL_FILL( k->notify_normal_fg, ak->popup_fg );
    AWL_FILL( k->notify_normal_bg, ak->popup_bg );
    AWL_FILL( k->notify_normal_frame, ak->popup_border );
    AWL_FILL( k->notify_critical_fg, ak->popup_fg );
    AWL_FILL( k->notify_critical_bg, ak->popup_bg );
    AWL_FILL( k->notify_critical_frame, ak->popup_border );
    AWL_FILL( k->osd_progress, AWL_OR( k->notify_progress, k->notify_low_fg.v ) );
    AWL_FILL( p->osd.width, p->notify.width );
    AWL_FILL( p->osd.image_size, p->notify.image_size );
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
