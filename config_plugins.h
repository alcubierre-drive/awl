/* config_plugins.h: built into libawlplugins.so, so `make` and
 * plugin_restart (MOD+Ctrl+r) apply it to the running awl -- keymap, input
 * devices, font, colors, borders, blur and layouts included (monitor rules
 * are applied to outputs appearing afterwards). While no library is loaded,
 * awl uses config_plugins.def.h instead. What only a restart of awl applies
 * is in config_awl.h. */

/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }
/* If you want to use the windows key for MODKEY, use WLR_MODIFIER_LOGO */
#define MODKEY WLR_MODIFIER_LOGO
// #define MODKEY WLR_MODIFIER_ALT

/* appearance */
static const int sloppyfocus               = 1;  /* focus follows mouse */
static const int bypass_surface_visibility = 0;  /* 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible  */
static const int borderpx                  = 2; /* border pixel of windows */
static const char font[]                   = "monospace:size=";
static const int fontsize                  = 10;
static const float rootcolor[]             = COLOR(0x000000ff);
static const float locked_blur_config[]    = {1.0 /*strength*/, 1.0 /*alpha*/};
static const float fullscreen_bg[]         = {0.1f, 0.1f, 0.1f, 1.0f}; /* You can also use glsl colors */
static const int blur_notifications        = 1,
                 blur_notifications_radius = 15,
                 blur_launcher             = 1,
                 blur_launcher_radius      = 15,
                 blur_windowlist           = 0;

/* molokai_* are in plugins/colors.h */
static const uint32_t bordercolors[BorderLast] = {
    [BorderNorm] = molokai_gray,
    [BorderSel]  = molokai_blue,
    [BorderUrg]  = molokai_orange,
};

/* the tray: colors 0xRRGGBBAA */
static const awl_tray_config_t tray_config = {
    .bg = 0x859394ff,
    /* the icon theme of the tray and its menus; what it lacks comes from the
     * desktop's (NULL: the desktop's) */
    .icon_theme = "Papirus-Dark",
};
/* awl's own popups: the bar widgets' (the clock's calendar, the IP
 * addresses) and awl's; colors 0xRRGGBBAA, sizes logical pixels */
static const awl_popup_config_t popup_config = {
    .bg           = 0x3c3c3c4d, /* awl blurs what is behind it */
    .fg           = 0xf8f8f2ff,
    // .border       = molokai_blue,
    .border       = molokai_light_gray,
    .dim          = 0xf8f8f259, /* the calendar's neighbouring months' days */
    .hover        = 0xffffff26, /* the buttons' background */
    .pressed      = 0xffffff40,
    .border_width = borderpx,
    .radius       = blur_launcher_radius, /* the blur's corners */
    .padding      = 8,
    /* the gap to the bar and to the screen's sides (at most a quarter of
     * the screen) */
    .margin       = 6,
    /* the font and its size, like font and fontsize (NULL, 0: the bar's) */
    .font         = "sans:size=",
    .fontsize     = 10,
    .calendar = {
        .today    = 0xb6ec52ff,
        .selected = 0xffffff33, /* the selected day's background */
        /* the event list, which scrolls beyond the maximum (0: none) */
        .list_min_height = 120,
        .list_max_height = 200,
    },
    /* the tray's menus (all left out: the popups' look) */
    .menu = {
        .bg           = 0x3c3c3c4d, /* the popups' (bg, fg, dim, hover) */
        .fg           = 0xf8f8f2ff,
        .border       = 0x859394ff,
        .dim          = 0xf8f8f259, /* disabled entries, shortcuts, separators */
        .highlight    = 0xffffff26,
        .highlight_fg = 0, /* fg */
        .border_width = 2,
        .radius       = 4,
        .blur         = 1,
    },
};
/* the desktop file list's font and its size, like font and fontsize (NULL,
 * 0: the bar's); it is blurred like the launcher (blur_launcher*) */
static const awl_desktop_config_t desktop_config = {
    .font         = "sans:size=",
    .fontsize     = 10,
};

/* the wallpaper action ({.i = WallpaperNext, WallpaperPrev, WallpaperRand,
 * WallpaperBack, WallpaperTimerNext or WallpaperTimerBack; see awl.h})
 * shows one of dir's *.png files, scaled to cover each monitor; so does a
 * timer every interval seconds (0: never), going where mode says
 * (wallpapermode cycles it) */
static const WallpaperConfig wallpaper_config = {
    .dir = "Wallpapers", .fade_ms = 500, .interval = 7200, .mode = WallpaperRand,
};

/* NOTE: ALWAYS keep a rule declared even if you don't use rules (e.g leave at least one example) */
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
/* (x=-1, y=-1) is reserved as an "autoconfigure" monitor position indicator
 * WARNING: negative values other than (-1, -1) cause problems with Xwayland clients
 * https://gitlab.freedesktop.org/xorg/xserver/-/issues/899
*/
/* NOTE: ALWAYS add a fallback rule, even if you are completely sure it won't be used */
static const MonitorRule monrules[] = {
    /* name       mfact  nmaster scale layout       rotate/reflect                x    y  pertag*/
    /* example of a HiDPI laptop monitor:
    */
    /* defaults */
    { "eDP-1",    0.5f,  1,    1.5,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1, 0 },
    /* { "HDMI-A-1", 0.5f,  1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_90,       -1,  -1, 0 }, */
    { NULL,       0.5f,  1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1, 0 },
};

/* keyboard */
static const struct xkb_rule_names xkb_rules = {
    /* can specify fields: rules, model, layout, variant, options */
    /* example:
    .options = "ctrl:nocaps",
    */
    .model = "pc105",
    .layout = "de",
    .variant = "nodeadkeys",
    .options = NULL,
};

static const int repeat_rate = 25;
static const int repeat_delay = 600;

/* Trackpad */
static const int tap_to_click = 1;
static const int tap_and_drag = 1;
static const int drag_lock = 1;
static const int natural_scrolling = 0;
static const int disable_while_typing = 1;
static const int left_handed = 0;
static const int middle_button_emulation = 0;
/* You can choose between:
LIBINPUT_CONFIG_SCROLL_NO_SCROLL
LIBINPUT_CONFIG_SCROLL_2FG
LIBINPUT_CONFIG_SCROLL_EDGE
LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
*/
static const enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;

/* You can choose between:
LIBINPUT_CONFIG_CLICK_METHOD_NONE
LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
*/
static const enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;

/* You can choose between:
LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
*/
static const uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

/* You can choose between:
LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
*/
static const enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
static const double accel_speed = 0.0;

/* You can choose between:
LIBINPUT_CONFIG_TAP_MAP_LRM -- 1/2/3 finger tap maps to left/right/middle
LIBINPUT_CONFIG_TAP_MAP_LMR -- 1/2/3 finger tap maps to left/middle/right
*/
static const enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

#define TAGKEYS(KEY,SKEY,TAG) \
    { MODKEY,                    KEY,            view,            {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_CTRL,  KEY,            toggleview,      {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_SHIFT, SKEY,           tag,             {.ui = 1ul << TAG} }, \
    { MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,SKEY,toggletag, {.ui = 1ul << TAG} }

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
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_XF86Switch_VT_##n, chvt, {.ui = (n)} }
    CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
    CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

static const Button buttons[] = {
    { ClkClient,   MODKEY,             BTN_LEFT,   moveresize,     {.ui = CurMove} },
    { ClkClient,   MODKEY,             BTN_MIDDLE, togglefloating, {0} },
    { ClkClient,   MODKEY,             BTN_RIGHT,  moveresize,     {.ui = CurResize} },
};
