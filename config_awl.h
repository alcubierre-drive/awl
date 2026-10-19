/* config_awl.h: what awl reads once, at startup; restart awl to apply
 * changes. The rest is in config_plugins.h, which `make` and
 * plugin_restart (MOD+Ctrl+r) apply to the running awl. */

/* appearance */
static const int showbar                   = 1; /* 0 means no bar */
static const int topbar                    = 0; /* 0 means bottom bar */
static const bool locked_blur              = true;

/* tagging - TAGCOUNT must be no greater than 31 */
#define NTAGS 9
static char *tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

/* logging */
static int log_level = WLR_ERROR;

static const char* Autostarts[][8] = {
    { "fnott", NULL },
    { TRAY_ICONS, "nm-applet", NULL },
    { "blueman-applet", NULL },
    { "system-config-printer-applet", NULL },
    { "Telegram", NULL },
    { "evolution", NULL },
};
static const char* locker[] = { "swaylock", "-c", "00000000", "-p", NULL };
static const int lock_at_start   = 1;
static const int lock_on_suspend = 1;

#define SCENEFX_HAS_SHATTER
