#pragma once
/** The plugins' defaults for awl_plugin_config_t (plugins.h), which
 * plugins_config.c's configure() changes, next to awl's for awl_config_t
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

static const awl_plugin_config_t awl_plugin_config_defaults = {
    .widgets = { default_widgets, LENGTH( default_widgets ) },
    .widgets_bar_padding = 0, /* each widget's own (plugins_widgets.c) */
    .colors = {
        .bg            = molokai_light_gray,
        .fg            = 0xf5efffff,
        .tag_bg        = molokai_dark_gray,
        .tag_occupied  = 0xfd971faa, /* molokai_orange, two thirds */
        .tag_urgent    = 0xf92672aa, /* molokai_red */
        .tag_selected  = 0xae81ffaa, /* molokai_purple */
        .selmon        = molokai_blue,
        .pertag        = molokai_red,
        .win_bg        = 0x42375aff,
        .win_focused   = 0x7359a5ff,
        .win_urgent    = molokai_red,
        .win_minimized = molokai_dark_gray,
        .win_fg        = 0xf5efffff,
        .clock_meter   = molokai_green,
        .stats_bg      = molokai_dark_gray,
        .cpu           = molokai_blue,
        .io            = 0x336c77ff, /* the CPU's, half as bright */
        .mem           = molokai_orange,
        .swap          = molokai_green,
        .good          = molokai_green,
        .warning       = molokai_orange,
        .bad           = molokai_red,
        .full          = molokai_blue,
    },
    .temperature = {
        /* idles around 50, throttles at TjMax (110) */
        .coretemp = { 50, 100 },
        .thermal_zone = { 40, 90 },
    },
    .battery = {
        .dir = "/sys/class/power_supply/BAT0/",
        .ac_dir = "/sys/class/power_supply/AC/",
        .scale = { .bad = 15, .warning = 25, .ok = 30, .full = 90 },
    },
    .launcher = {
        .layout = AWL_LAUNCHER_LIST,
        .rows = 10, .width = 560, .icon_size = 24,
        .input_width = 120,
        .strip_at = AWL_WINDOW_AT_BAR,
        .terminal = default_terminal,
        .prompt = "> ", .run_prompt = "$ ",
        .at = AWL_WINDOW_AT_TOP, .dy = 0,
        .match = molokai_orange,
        .dim = 0, /* the popups' dim */
        /* .prompt_color: 0, the launcher's dim; .query_color: 0, the
         * popups' fg (the cursor's too) */
    },
    .notify = {
        .at = AWL_WINDOW_AT_TOP_RIGHT, .dx = 0, .dy = 0, .gap = 6,
        .width = 360, .image_size = 48,
        .max_lines = 14, .max_shown = 8,
        /* low and normal: the popups' colors */
        .urgency = {
            [AWL_NOTIF_LOW]      = { .frame = molokai_dark_gray },
            [AWL_NOTIF_CRITICAL] = { .frame = molokai_red },
        },
        .progress_color = 0, /* the frame's */
        .progress_height = 6,
        .show_app = 1,
    },
};
