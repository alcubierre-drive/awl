/** The plugins' defaults for awl_plugin_config_t (plugins.h), which
 * plugins_config.c's configure() changes, next to awl's for awl_config_t
 * (awl_defaults.c). Copy from here what to change. */
#include "plugins.h"

/** from the right edge in */
static const int widgets[] = {
    AwlWidgetClock, AwlWidgetTray, AwlWidgetVolume, AwlWidgetStats, AwlWidgetTemperature,
    AwlWidgetDock, AwlWidgetBattery, AwlWidgetNetwork, AwlWidgetBacklight,
};

void awl_plugin_config_defaults( awl_plugin_config_t* p ) {
    *p = (awl_plugin_config_t){
        .widgets = { widgets, LENGTH( widgets ) },
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
        },
        .tray = {
            .bg = 0x859394ff,
            .icon_size = 16,
            .icon_gap = 4,
        },
        .popup = {
            .dim = 0xf8f8f259,
            .calendar = {
                .today    = 0xb6ec52ff,
                .selected = 0xffffff33,
                .list_min_height = 120,
                .list_max_height = 200,
            },
            .menu = { 0 },
        },
        .desktop = {
            .bg     = 0x3c3c3c4c,
            .file   = 0xf8f8f2ff,
            .dir    = molokai_blue,
            .broken = molokai_red,
            .hidden_alpha = 0xa0,
            .margin = 10,
        },
        .temperature = {
            /* idles around 50, throttles at TjMax (110) */
            .coretemp = { 50, 100 },
            .thermal_zone = { 40, 90 },
        },
        .battery = {
            .dir = "/sys/class/power_supply/BAT0/",
        },
    };
}
