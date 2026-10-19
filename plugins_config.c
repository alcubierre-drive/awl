/** The configuration, built into libawlplugins.so, so `make` and
 * plugin_restart (MOD+Ctrl+r) apply it to the running awl -- keymap, input
 * devices, font, colors, borders, blur, layouts and the bar's widgets
 * included (monitor rules are applied to outputs appearing afterwards).
 * While no library is loaded, awl runs with its defaults; what only a
 * restart of awl applies are config.mk's compile-time switches.
 *
 * Only the part between the CUSTOM markers is yours to change: configure()
 * gets awl's defaults (awl_defaults.c, `awl_host_t.config_defaults`) in c
 * and the plugins' (plugins_defaults.c) in p, and changes what it wants;
 * awl_config_t in awl.h and awl_plugin_config_t in plugins.h list
 * everything there is. The rest of this file is the library's: the
 * functions the config binds to keys and layouts are awl's, here
 * same-named wrappers calling them through the host table, next to the
 * library's own actions. None of them is static, so the config may leave
 * any of them unbound; the library exports none. */
#include "awl.h"
#include "awl_plugin_abi.h"
#include "plugins.h"
#include "plugins/colors.h"

#define AWL_ACTION_WRAPPER( name ) \
    void name( const Arg* arg ) { awl_host->actions->name( arg ); }
AWL_ACTIONS( AWL_ACTION_WRAPPER )
#undef AWL_ACTION_WRAPPER

#define AWL_ARRANGE_WRAPPER( name ) \
    void name( Monitor* m ) { awl_host->arranges->name( m ); }
AWL_ARRANGES( AWL_ARRANGE_WRAPPER )
#undef AWL_ARRANGE_WRAPPER

void notifyconfig( const Arg* arg ) { awl_notify_config(); }
void backlighttoggle( const Arg* arg ) { awl_backlight_toggle(); }

/* ======================================================================
 * CUSTOM: the user's configuration, from here to "CUSTOM end".
 *
 * The bindings, rules, layouts, monitor rules, autostarts and the bar's
 * widgets are lists: to change one, copy its array from awl_defaults.c (or
 * plugins_defaults.c) here, change it, and point the list at it in
 * configure(), e.g. SETLIST(c->keys, keys) or SETLIST(p->widgets, widgets).
 * In bindings, MODKEY is c->modkey. They can call what AWL_ACTIONS lists
 * (awl_plugin_abi.h), the library's actions above (notifyconfig,
 * backlighttoggle), and functions defined here. The molokai_* colors are
 * in plugins/colors.h.
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
    { "[|]",      tile },
    { "[·]",      NULL },    /* no layout function means floating behavior */
    { "[M]",      monocle },
    { "[×]",      gaplessgrid },
    { "[–]",      bstack },
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

static void tagmonf( const Arg* arg ) { tagmon(arg); focusmon(arg); }

static const Key keys[] = {
    /* Note that Shift changes certain key codes: c -> C, 2 -> at, etc. */
    /* modifier                  key                 function        argument */
    { MODKEY,                    XKB_KEY_p,          spawn,            {.v = menucmd} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_P,          notifyconfig,     {0} },
    { MODKEY,                    XKB_KEY_Return,     spawn,            {.v = termcmd} },
    { MODKEY,                    XKB_KEY_y,          backlighttoggle,  {0} },
    { MODKEY,                    XKB_KEY_i,          togglebar,        {0} },
    { MODKEY,                    XKB_KEY_j,          focusstack,       {.i = +1} },
    { MODKEY,                    XKB_KEY_k,          focusstack,       {.i = -1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_J,          movestack,        {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_K,          movestack,        {.i = -1} },
    /*{ MODKEY,                    XKB_KEY_i,          incnmaster,     {.i = +1} },*/
    /*{ MODKEY,                    XKB_KEY_d,          incnmaster,     {.i = -1} },*/
    { MODKEY,                    XKB_KEY_h,          setmfact,         {.f = -0.05f} },
    { MODKEY,                    XKB_KEY_l,          setmfact,         {.f = +0.05f} },
    { MODKEY,                    XKB_KEY_Tab,        view,             {0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_C,          killclient,       {0} },
    { MODKEY,                    XKB_KEY_m,          maximize,         {0} },
    { MODKEY,                    XKB_KEY_space,      cycle_layout,     {.i = +1} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_space,      cycle_layout,     {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,XKB_KEY_space,togglepertag,{0} },
    { MODKEY,                    XKB_KEY_Right,      cycle_view,       {.i = +1} },
    { MODKEY,                    XKB_KEY_Left,       cycle_view,       {.i = -1} },
    { MODKEY,                    XKB_KEY_f,          togglefullscreen, {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_space,      togglefloating,   {0} },
    { MODKEY,                    XKB_KEY_t,          toggleontop,      {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_r,          plugin_restart,   {0} },
    { MODKEY,                    XKB_KEY_w,          wallpaper,        {.i = WallpaperTimerNext} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_W,          wallpaper,        {.i = WallpaperTimerBack} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_N,          spawn,            {.v = notification_action} },
    // { MODKEY,                    XKB_KEY_g,          spawn,            {.v = garfield} },
    { MODKEY,                    XKB_KEY_b,          togglebw,         {0} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_B,          changebw,         {.i=+1} },
    { MODKEY|WLR_MODIFIER_SHIFT|WLR_MODIFIER_CTRL,XKB_KEY_B, changebw, {.i=-1} },
    { MODKEY|WLR_MODIFIER_SHIFT|WLR_MODIFIER_CTRL,XKB_KEY_S, spawn,    {.v = suspend} },

    { MODKEY,                    XKB_KEY_n,          minimize,         {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_n,          unminimize,       {0} },
    { MODKEY,                    XKB_KEY_c,          spawn,            {.v = hyprcopy} },
    { 0, XKB_KEY_XF86MonBrightnessUp,                spawn,            {.v=brightp} },
    { 0, XKB_KEY_XF86MonBrightnessDown,              spawn,            {.v=brightm} },
    { 0, XKB_KEY_XF86AudioRaiseVolume,               spawn,            {.v=volp} },
    { 0, XKB_KEY_XF86AudioLowerVolume,               spawn,            {.v=volm} },
    { 0, XKB_KEY_XF86AudioMute,                      spawn,            {.v=volt} },
    { 0, XKB_KEY_XF86AudioMicMute,                   spawn,            {.v=volT} },
    { MODKEY,                    XKB_KEY_F1,         spawn,            {.v = speakerT} },
    { 0,                         XKB_KEY_Print,      spawn,            {.v=grimslurp} },
    { MODKEY,                    XKB_KEY_d,          spawn,            {.v=wdisplays} },
    { 0,                         XKB_KEY_XF86Display,spawn,            {.v=wdisplays} },
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_G,          lockscreen,       {0} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_z,          transluce,        {.i=+1} },
    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,XKB_KEY_Z,transluce, {.i=-1} },

    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,XKB_KEY_D, spawn,   {.v=docked_r} },
    { MODKEY|WLR_MODIFIER_CTRL,     XKB_KEY_d,         spawn,         {.v=docked_d} },
    { MODKEY|WLR_MODIFIER_SHIFT,    XKB_KEY_D,         spawn,         {.v=docked_z} },
    /*{ MODKEY,                    XKB_KEY_0,          view,           {.ui = ~0} },*/
    /*{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_parenright, tag,            {.ui = ~0} },*/

    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_h,          incnmaster,     {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_l,          incnmaster,     {.i = +1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_j,          focusmon,       {.i = -1} },
    { MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_k,          focusmon,       {.i = +1} },
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
    { MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_equal,          quit,       {0} },

    /* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
    { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_Terminate_Server, quit, {0} },
    /* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
     * do not remove them.
     */
    CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
    CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

static void
configure(awl_config_t *c, awl_plugin_config_t *p)
{
    c->modkey = WLR_MODIFIER_LOGO;
    // c->modkey = WLR_MODIFIER_ALT;
    SETLIST(c->keys, keys);
    SETLIST(c->rules, rules);
    SETLIST(c->layouts, layouts);
    SETLIST(c->monrules, monrules);
    SETLIST(c->autostart, autostart);

    /* the bar at the bottom */
    c->bar.top = 0;
    /* the screen lock */
    c->lock.locker = locker;
    c->lock.at_start = 1;

    /* appearance */
    c->borderpx = 2;
    c->blur.windowlist = 0;
    /* molokai_* are in plugins/colors.h */
    c->bordercolors[BorderNorm] = molokai_gray;
    c->bordercolors[BorderSel]  = molokai_blue;
    c->bordercolors[BorderUrg]  = molokai_orange;

    /* the icon theme of the tray and its menus; what it lacks comes from the
     * desktop's (NULL: the desktop's) */
    c->tray.icon_theme = "Papirus-Dark";

    // c->popup.border = molokai_blue;
    // c->popup.border = molokai_light_gray;
    c->popup.border       = 0x657374ff;
    c->popup.border_width = c->borderpx;
    c->popup.font         = "sans:size=";
    c->popup.fontsize     = 10;
    /* the tray's menus */
    c->popup.menu.bg           = 0x3c3c3c4d; /* the popups' (bg, fg, hover) */
    c->popup.menu.fg           = 0xf8f8f2ff;
    c->popup.menu.border       = 0x859394ff;
    p->popup.menu.dim          = 0xf8f8f259; /* disabled entries, shortcuts, separators */
    c->popup.menu.highlight    = 0xffffff26;
    c->popup.menu.border_width = 2;
    c->popup.menu.radius       = 4;
    c->popup.menu.blur         = 1;

    c->desktop.font     = "sans:size=";
    c->desktop.fontsize = 10;

    /* keyboard */
    c->keyboard.xkb.model   = "pc105";
    c->keyboard.xkb.layout  = "de";
    c->keyboard.xkb.variant = "nodeadkeys";
}

/* ======================================================================
 * CUSTOM end: below is the library's again.
 * ====================================================================== */

static awl_config_t cfg;
static awl_plugin_config_t plugin_cfg;

void awl_config_init( void ) {
    awl_host->config_defaults( &cfg );
    awl_plugin_config_defaults( &plugin_cfg );
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
