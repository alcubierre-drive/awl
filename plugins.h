#pragma once

/** Internal to ``libawlplugins.so``; awl itself only sees awl_plugin_abi.h. */

#include "awl_plugin_abi.h"

#include "plugins/ipaddr.h"
#include "plugins/stats.h"
#include "plugins/temp.h"
#include "plugins/bat.h"
#include "plugins/backlight.h"
#include "plugins/date.h"
#include "plugins/pulsetest.h"
#include "plugins/poller.h"
#include "plugins/wallpaper.h"

#include "plugins/colors.h"

/** The running plugins' state, see `awl_plugin_get()`. */
typedef struct awl_plugin_data_t {
    float refresh_sec;

    awl_ipaddr_t* ip;
    awl_stats_t* stats;
    awl_temperature_t* temp;
    uint32_t (*temp_color)( float T, float min, float max );
    awl_battery_t* bat;
    awl_date_t* date;
    pulse_test_t* pulse;
    awl_backlight_t* backlight;
    awl_poller_t* poller;
    int paused;

    struct awl_colors awl_colors;
} awl_plugin_data_t;

/** The running plugins' state; NULL while the plugins aren't running. */
awl_plugin_data_t* awl_plugin_get( void );

/** plugins.c: a desktop notification (``notify-send``, spawned); main thread */
void awl_notify( const char* title, const char* body );
/** plugins.c: a notification "AWL config" listing the config dictionary,
 * one ``key=value`` line per entry, sorted by key; an owned value (one the
 * dictionary frees) as ``key=pointer(free function)``. Main thread. */
void awl_notify_config( void );
/** plugins.c: `backlight_toggle()` on the running plugins' daemon, if any
 * (the backlighttoggle action); main thread */
void awl_backlight_toggle( void );
/** plugins.c: a notification "x/N" once the wallpaper all changes asked for
 * end on is shown; main thread */
void awl_notify_wallpaper_shown( const char* title );

/** widgets.c: `awl_plugin_api_t.bar_widgets` */
void awl_widgets_create( awl_draw_t* bar );

/** desktop_panel.c: starts the scanner thread */
void awl_desktop_start( void );
/** desktop_panel.c: stops the scanner thread; returns nonzero if it had to
 * be left running */
int awl_desktop_stop( void );
/** desktop_panel.c: `awl_plugin_api_t.desktop_version` */
uint64_t awl_desktop_version( void );
/** desktop_panel.c: `awl_plugin_api_t.desktop_measure` */
void awl_desktop_measure( awl_draw_t* drw, int avail_w, int avail_h, int r, float scale,
                          int* x, int* y, int* w, int* h );
/** desktop_panel.c: `awl_plugin_api_t.desktop_draw` */
void awl_desktop_draw( awl_draw_t* drw, uint32_t* data, int stride, int w, int h, int r,
                       float scale );
/** desktop_panel.c: `awl_plugin_api_t.desktop_click` */
int awl_desktop_click( int button, uint32_t mods );

/** awl_config.c: `awl_plugin_api_t.config` */
const awl_config_t* awl_config( void );
