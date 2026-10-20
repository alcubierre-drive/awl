#pragma once
/** The plugins' defaults for awl_plugin_config_t (plugins.h), which
 * plugins_config.c's configure() changes, next to awl's for awl_config_t
 * (awl_config_defaults.h). Only plugins_config.c includes it: every file
 * that did would get its own copy. Copy from here what to change; plugins_config.c's
 * configure() lists them, commented out. */
#include "plugins.h"
#include "plugins/colors.h"

/** from the right edge in */
static const int default_widgets[] = {
    AwlWidgetClock, AwlWidgetTray, AwlWidgetVolume, AwlWidgetStats, AwlWidgetTemperature,
    AwlWidgetDock, AwlWidgetBattery, AwlWidgetNetwork, AwlWidgetBacklight,
};

static const char* const default_terminal[] = { "xterm", "-e", NULL };

static const awl_plugin_config_t awl_plugin_config_defaults = {
    .widgets = { default_widgets, LENGTH( default_widgets ) },
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
        .match = molokai_orange, .dim = 0,
    },
};
