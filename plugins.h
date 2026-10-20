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
#include "plugins/loop.h"
#include "plugins/wallpaper.h"
#include "plugins/sni.h"

#include "plugins/colors.h"

/** the widgets the bar can show right of the task bar,
 * `awl_plugin_config_t.widgets` */
enum {
    AwlWidgetClock,       /* the time, a seconds meter; hover: the calendar */
    AwlWidgetTray,        /* the status notifier items (plugins/sni.h) */
    AwlWidgetVolume,      /* the default sink's volume (PulseAudio) */
    AwlWidgetStats,       /* graphs of CPU, I/O, memory and swap */
    AwlWidgetTemperature,
    AwlWidgetDock,        /* a Thunderbolt dock */
    AwlWidgetBattery,
    AwlWidgetNetwork,     /* the address; hover: all of them */
    AwlWidgetBacklight,   /* the backlight daemon's state */
};

/**
 * The bar's colors, `awl_plugin_config_t.colors` (0xRRGGBBAA) and as
 * pixman's `awl_plugin_data_t.colors`:
 * bg and fg: the layout symbol's, the clock's and the status widgets';
 * tag_bg, and over it in this order (its alpha says how much)
 * tag_occupied, tag_urgent, tag_selected: the tags;
 * selmon: the selected monitor's mark under the first tag;
 * pertag: the frame around a per-tag layout's symbol;
 * win_bg, win_focused, win_urgent, win_minimized and win_fg: the task bar's
 * windows (shown, focused, urgent, minimized) and their titles;
 * clock_meter: the clock's seconds;
 * stats_bg, cpu, io, mem and swap: the graphs;
 * good, warning and bad: the volume's, battery's, network's and
 * backlight's states.
 */
#define AWL_BAR_COLORS( X ) \
    X( bg ) X( fg ) X( tag_bg ) X( tag_occupied ) X( tag_urgent ) X( tag_selected ) X( selmon ) \
    X( pertag ) X( win_bg ) X( win_focused ) X( win_urgent ) X( win_minimized ) X( win_fg ) \
    X( clock_meter ) X( stats_bg ) X( cpu ) X( io ) X( mem ) X( swap ) X( good ) X( warning ) X( bad ) \
    X( full )

#define AWL_BAR_COLOR_U32( name ) uint32_t name;
#define AWL_BAR_COLOR_PIXMAN( name ) pixman_color_t name;

/**
 * What only the plugin library reads of the configuration: its defaults
 * (plugin_config_defaults.h) with plugins_config.c's changes, next to
 * `awl_config_t` (awl.h), which awl reads (the tray's, the popups' and the
 * desktop file list's look are there). Colors are 0xRRGGBBAA, sizes
 * logical pixels. Main thread.
 */
typedef struct awl_plugin_config_t {
    /** the bar's widgets right of the task bar, AwlWidget*, from the right
     * edge in; set with SETLIST() */
    struct { const int* v; size_t n; } widgets;
    struct { AWL_BAR_COLORS( AWL_BAR_COLOR_U32 ) } colors;
    /** the temperature's color scale, °C: of the CPU package sensor
     * (coretemp), else of ACPI's thermal_zone0 */
    struct {
        struct { float min, max; } coretemp, thermal_zone;
    } temperature;
    struct {
        /** the battery's sysfs directory, with a trailing '/' */
        const char* dir;
        /** its color scale, percent of the charge: colors.bad up to bad,
         * to colors.warning at warning, to colors.fg at ok and above
         * (linearly in between); colors.good while charging */
        struct { float bad, warning, ok, full; } scale;
    } battery;
    /** the launcher (plugins/launcher.h), a popup: styled as the others
     * (`awl_config_t.popup`, blurred with blur.launcher) */
    struct {
        /** AWL_LAUNCHER_LIST: a box with the query above a list of results;
         * AWL_LAUNCHER_BAR: a strip over the bar, the query left, the
         * results beside it in a row (as awesome's menubar) */
        int layout;
        /** result rows; the window's width; the icons' size (the bar's
         * height at most in AWL_LAUNCHER_BAR, which ignores rows, width, at
         * and dy) */
        int rows, width, icon_size;
        /** AWL_LAUNCHER_BAR: the query's width at least, logical pixels;
         * the results follow right after it */
        int input_width;
        /** AWL_LAUNCHER_BAR: where the strip is, whatever the bar's place:
         * AWL_WINDOW_AT_BAR over the bar, AWL_WINDOW_AT_STRIP_TOP or
         * AWL_WINDOW_AT_STRIP_BOTTOM along the monitor's edge (awl.h); as
         * high as the bar either way */
        int strip_at;
        /** what applications with Terminal=true run in: an argv (NULL
         * terminated) the command is appended to, e.g. { "foot", NULL } */
        const char* const* terminal;
        /** before the query */
        const char* prompt;
        /** where it shows: an AWL_WINDOW_AT_* (awl.h), moved down by dy */
        int at, dy;
        /** the characters the query matched; the description (GenericName),
         * 0: the popups' dim */
        uint32_t match, dim;
        /** the prompt, 0: dim; the query and the cursor, 0: the popups' fg */
        uint32_t prompt_color, query_color;
    } launcher;
} awl_plugin_config_t;

/** `awl_plugin_config_t.launcher`'s layout */
enum { AWL_LAUNCHER_LIST, AWL_LAUNCHER_BAR };

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
    /** the tray's host, and the loop it runs on (plugins/loop.h) */
    awl_sni_t* sni;
    awl_loop_t* tray_loop;
    int paused;

    /** `awl_plugin_config_t.colors` as pixman's */
    struct { AWL_BAR_COLORS( AWL_BAR_COLOR_PIXMAN ) } colors;
} awl_plugin_data_t;

/** The running plugins' state; NULL while the plugins aren't running. */
awl_plugin_data_t* awl_plugin_get( void );

/** plugins.c: a desktop notification (``notify-send``, spawned); main thread */
void awl_notify( const char* title, const char* body );
/** plugins.c: a notification "AWL config" listing the config dictionary,
 * one ``key=value`` line per entry, sorted by key; an owned value (one the
 * dictionary frees) as ``key=pointer(free function)``. Main thread. */
void awl_notify_config( void );
/** plugins.c: a notification "x/N" once the wallpaper all changes asked for
 * end on is shown; main thread */
void awl_notify_wallpaper_shown( const char* title );

/** plugins_widgets.c: `awl_plugin_api_t.bar_widgets` */
void awl_widgets_create( awl_draw_t* bar );

/** plugins_desktop_panel.c: starts the scanner thread */
void awl_desktop_start( void );
/** plugins_desktop_panel.c: stops the scanner thread; returns nonzero if it
 * had to be left running */
int awl_desktop_stop( void );
/** plugins_desktop_panel.c: `awl_plugin_api_t.desktop_version` */
uint64_t awl_desktop_version( void );
/** plugins_desktop_panel.c: `awl_plugin_api_t.desktop_measure` */
void awl_desktop_measure( struct fcft_font* font, int avail_w, int avail_h, int r, float scale,
                          int* x, int* y, int* w, int* h );
/** plugins_desktop_panel.c: `awl_plugin_api_t.desktop_draw` */
void awl_desktop_draw( pixman_image_t* pix, struct fcft_font* font, uint32_t* data, int stride,
                       int w, int h, int r, float scale );
/** plugins_desktop_panel.c: `awl_plugin_api_t.desktop_click` */
int awl_desktop_click( int button, uint32_t mods );

/** plugins/launcher.c: starts the launcher's index thread */
void awl_launcher_start( void );
/** plugins/launcher.c: closes the launcher, stops its thread; returns
 * nonzero if it had to be left running */
int awl_launcher_stop( void );
/** plugins/launcher.c: opens the launcher on the selected monitor, or
 * closes it (the launcher action) */
void awl_launcher_toggle( void );

/** plugins_config.c: makes both configs, awl's defaults
 * (`awl_host_t.config_defaults`) and the plugins' with configure() (the
 * CUSTOM part of plugins_config.c) applied; first thing in `awl_plugin_api_t.init`, so before
 * anything reads them */
void awl_config_init( void );
/** plugins_config.c: the config `awl_config_init()` made; valid until the
 * library is unloaded */
const awl_config_t* awl_config( void );
/** plugins_config.c: the plugins' config `awl_config_init()` made */
const awl_plugin_config_t* awl_plugin_config( void );
/** plugins_config.c: `awl_plugin_api_t.config`, a copy of `awl_config()` */
void awl_config_fill( awl_config_t* c );
