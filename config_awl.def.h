/* config_awl.h: what awl reads once, at startup; restart awl to apply
 * changes. The rest is in config_plugins.h, which `make` and
 * plugin_restart (MOD+Ctrl+r) apply to the running awl. */

/* appearance */
static const int showbar                   = 1; /* 0 means no bar */
static const int topbar                    = 1; /* 0 means bottom bar */
static const bool locked_blur              = true; /* blur the screen while locked */

/* tagging - TAGCOUNT must be no greater than 31 */
#define NTAGS 9
static char *tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

/* logging */
static int log_level = WLR_ERROR;

/* started with awl, and terminated when it exits */
static const char* Autostarts[][8] = {
    { NULL }, /* e.g. { "nm-applet", NULL }, */
};

/* the screen lock: locker is an ext-session-lock client, started on logind's
 * Lock (loginctl lock-session), before suspend and by the lockscreen action */
static const char* locker[] = { "swaylock", "-c", "00000000", NULL };
static const int lock_at_start   = 0; /* lock right after starting */
static const int lock_on_suspend = 1; /* suspend waits for the lock */
