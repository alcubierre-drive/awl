#pragma once
/** The plugins' defaults for :c:type:`awl_plugin_config_t` (plugins.h), which
 * plugins_config.c's configure() changes, next to awl's for :c:type:`awl_config_t`
 * (awl_config_defaults.h). Only plugins_config.c includes it: every file
 * that did would get its own copy. Every setting is here with its default;
 * one left zero is a comment saying what zero means. Copy from here what to
 * change into plugins_config.c's configure(). */
#include "plugins.h"
#include "plugins/colors.h"

/** from the right edge in */
static const int default_widgets[] = {
    AwlWidgetClock, AwlWidgetNotifications, AwlWidgetTray, AwlWidgetVolume, AwlWidgetStats, AwlWidgetTemperature,
    AwlWidgetDock, AwlWidgetBattery, AwlWidgetNetwork, AwlWidgetBacklight,
};

static const char* const default_terminal[] = { "xterm", "-e", NULL };
static const char* const default_mixer[] = { "pavucontrol", NULL };
static const char* const default_calendar_app[] = { "evolution", "-c", "calendar", NULL };

static const awl_plugin_config_t awl_plugin_config_defaults = {
    .widgets = { default_widgets, LENGTH( default_widgets ) },
    .widgets_padding = 0.25f, /* a widget's gap to the next: half the font's height */
    .colors = {
        .bar_bg         = molokai_light_gray,
        .bar_fg         = 0xf5efffff,
        .tag_bg         = molokai_dark_gray,
        .tag_occupied   = 0xfd971faa, /* molokai_orange, two thirds */
        .tag_urgent     = 0xf92672aa, /* molokai_red */
        .tag_selected   = 0xae81ffaa, /* molokai_purple */
        .selmon         = molokai_blue,
        .pertag         = molokai_red,
        .win_bg         = 0x42375aff,
        .win_focused    = 0x7359a5ff,
        .win_urgent     = molokai_red,
        .win_minimized  = molokai_dark_gray,
        .win_fg         = 0xf5efffff,
        .clock_meter    = molokai_green,
        .stats_bg       = molokai_dark_gray,
        .cpu            = molokai_blue,
        .io             = 0x336c77ff, /* the CPU's, half as bright */
        .mem            = molokai_orange,
        .swap           = molokai_green,
        .good           = molokai_green,
        .warning        = molokai_orange,
        .bad            = molokai_red,
        /* the color maps' stops: {at, color}, at 0 to 1, linearly in
         * between */
        .temperature    = COLORMAP( { 0, 0x40cfebff }, { 0.5f, 0xfd9e2fff }, { 1, 0xf9327aff } ),
        .battery        = COLORMAP( { 0.15f, molokai_red }, { 0.25f, molokai_orange },
                                    { 0.3f, 0xf5efffff /* bar_fg */ } ),
        .battery_charging = COLORMAP( { 0, molokai_green }, { 0.9f, molokai_blue } ),
        .tray_bg        = 0x859394ff,

        .calendar_today    = 0xb6ec52ff,
        .calendar_selected = 0xffffff33,
        .launcher_match = molokai_orange,
        /* .launcher_dim: 0, popup_dim; .launcher_prompt: 0, launcher_dim;
         * .launcher_query: 0, popup_fg (the cursor's too) */
        /* .notify_*: low and normal 0, the popups' */
        .notify_low_frame      = molokai_dark_gray,
        .notify_critical_frame = molokai_red,
        .notify_progress       = 0, /* the frame's */
        .desktop_bg     = 0x3c3c3c4c,
        .desktop_file   = 0xf8f8f2ff,
        .desktop_dir    = molokai_blue,
        .desktop_broken = molokai_red,
    },
    .temperature = {
        /* idles around 50, throttles at TjMax (110) */
        .coretemp = { 50, 100 },
        .thermal_zone = { 40, 90 },
    },
    .battery = {
        .dir = "/sys/class/power_supply/BAT0/",
        .ac_dir = "/sys/class/power_supply/AC/",
        .low = 15,
        /* NULL or none found: the glyphs */
        .icons = {
            /* Papirus: laptop, Breeze: computer-laptop, Adwaita has neither */
            .laptop = "laptop-symbolic;computer-laptop-symbolic;computer-symbolic",
            .docked = "video-display-symbolic;computer-symbolic",
            /* level 100 has no charging icon, but a charged one */
            .ac = "battery-level-%d-charging-symbolic;battery-level-100-charged-symbolic;ac-adapter-symbolic",
            .battery = "battery-level-%d-symbolic;battery-good-symbolic",
            .battery_low = "battery-level-%d-symbolic;battery-low-symbolic;battery-caution-symbolic",
        },
    },
    .launcher = {
        .layout = AWL_LAUNCHER_LIST,
        .rows = 10, .width = 560, .icon_size = 24,
        .input_width = 120,
        .strip_at = AWL_WINDOW_AT_BAR,
        .terminal = default_terminal,
        .prompt = "> ", .run_prompt = "$ ",
        .at = AWL_WINDOW_AT_TOP, .dy = 0,
    },
    .volume = {
        .max = 2.0f,
        .speaker = NULL,
        .mixer = default_mixer,
    },
    .calendar = {
        .app = default_calendar_app,
        .app_label = "Evolution",
    },
    .notify = {
        .at = AWL_WINDOW_AT_TOP_RIGHT, .dx = 0, .dy = 0, .gap = 6,
        .width = 360, .image_size = 48,
        .menu_width = 440, /* the windows' and a past one's "Show" and time */
        .max_lines = 14, .max_shown = 8,
        .progress_height = 6,
        .show_app = 1,
        /* NULL or none found: the glyphs (Adwaita, Breeze, Papirus have these) */
        .bell_icon = "notifications-symbolic;preferences-system-notifications-symbolic",
        .bell_icon_dnd = "notifications-disabled-symbolic",
    },
    .osd = {
        .timeout = 1500, .width = 200, .image_size = 32,
        /* Adwaita, Breeze, Papirus have the volume's; the brightness's are
         * Papirus', then Adwaita's and Breeze's single one */
        .volume_icons = {
            .muted = "audio-volume-muted-symbolic",
            .low = "audio-volume-low-symbolic",
            .medium = "audio-volume-medium-symbolic",
            .high = "audio-volume-high-symbolic",
            .over = "audio-volume-overamplified-symbolic;audio-volume-high-danger-symbolic;audio-volume-high-symbolic",
        },
        .brightness_icons = {
            .off = "display-brightness-off-symbolic;display-brightness-symbolic;video-display-brightness-symbolic",
            .low = "display-brightness-low-symbolic;display-brightness-symbolic;video-display-brightness-symbolic",
            .medium = "display-brightness-medium-symbolic;display-brightness-symbolic;video-display-brightness-symbolic",
            .high = "display-brightness-high-symbolic;display-brightness-symbolic;video-display-brightness-symbolic",
        },
    },
};
