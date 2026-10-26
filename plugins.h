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
 * :c:member:`awl_plugin_config_t.widgets` */
enum {
    AwlWidgetClock,       /* the time, a seconds meter; hover: the calendar */
    AwlWidgetTray,        /* the status notifier items (plugins/sni.h) */
    AwlWidgetVolume,      /* the default sink's volume (PulseAudio) */
    AwlWidgetStats,       /* graphs of CPU, I/O, memory and swap */
    AwlWidgetTemperature,
    AwlWidgetDock,        /* a Thunderbolt dock, AC or battery */
    AwlWidgetBattery,
    AwlWidgetNetwork,     /* the address; hover: all of them */
    AwlWidgetBacklight,   /* the backlight daemon's state */
    /** a bell, a dot while any are open; left: do-not-disturb, right: the
     * menu, middle: dismiss all; hover: how many are unread */
    AwlWidgetNotifications,
};

/** :c:member:`awl_plugin_config_t.colors`: the colors only the plugins draw with,
 * 0xRRGGBBAA (awl's are :c:type:`awl_colors_t`, awl.h, whose popup_* the plugins'
 * popups use too). The optional ones (:c:type:`awl_opt_color_t`) say what they
 * are unset; :c:func:`awl_plugin_config_fill_defaults` fills that in. */
typedef struct awl_plugin_colors_t {
    /** the bar: the layout symbol's, the clock's and the status widgets'
     * background and text */
    uint32_t bar_bg, bar_fg;
    /** the tags: tag_bg, and over it in this order (its alpha says how
     * much) tag_occupied, tag_urgent, tag_selected */
    uint32_t tag_bg, tag_occupied, tag_urgent, tag_selected;
    /** the selected monitor's mark under the first tag; the frame around a
     * per-tag layout's symbol */
    uint32_t selmon, pertag;
    /** the task bar's windows (shown, focused, urgent, minimized) and
     * their titles */
    uint32_t win_bg, win_focused, win_urgent, win_minimized, win_fg;
    /** the clock's seconds */
    uint32_t clock_meter;
    /** the graphs: their background, the CPU's, I/O's, memory's, swap's */
    uint32_t stats_bg, cpu, io, mem, swap;
    /** the volume's, network's, backlight's and notifications' states */
    uint32_t good, warning, bad;
    /** the temperature's color, 0 to 1 over :c:type:`awl_plugin_config_t`'s
     * temperature range */
    awl_colormap_t temperature;
    /** the battery's color by its charge, 0 to 1: running on it, and
     * charging */
    awl_colormap_t battery, battery_charging;
    /** the tray widget's background */
    uint32_t tray_bg;
    /** the clock's calendar: today's date, the selected day's background */
    uint32_t calendar_today, calendar_selected;
    /** the launcher (plugins/launcher.h): the characters the query
     * matched, unset: popup_fg; the description (GenericName), unset:
     * popup_dim; the prompt, unset: launcher_dim; the query and the cursor,
     * unset: popup_fg */
    awl_opt_color_t launcher_match, launcher_dim, launcher_prompt, launcher_query;
    /** the notifications' windows (plugins/notify.h) by urgency: text,
     * background and frame, each unset: the popups' */
    awl_opt_color_t notify_low_fg, notify_low_bg, notify_low_frame;
    awl_opt_color_t notify_normal_fg, notify_normal_bg, notify_normal_frame;
    awl_opt_color_t notify_critical_fg, notify_critical_bg, notify_critical_frame;
    /** the bar of a notification with a value, its filled part; unset: its
     * frame's, which only the notification knows, so it stays unset here */
    awl_opt_color_t notify_progress;
    /** the OSD's bar (plugins/osd.h); unset: notify_progress, else
     * notify_low_fg (the OSD's urgency's text) */
    awl_opt_color_t osd_progress;
    /** the OSD's bar for a change awl didn't ask for, in place of
     * osd_progress */
    uint32_t osd_unasked;
    /** the desktop file list (awl_desktop.h): its background, the names of
     * files, folders and broken links */
    uint32_t desktop_bg, desktop_file, desktop_dir, desktop_broken;
} awl_plugin_colors_t;

/** the bar's colors of :c:type:`awl_plugin_colors_t`, which
 * :c:member:`awl_plugin_data_t.colors` has as pixman's */
#define AWL_BAR_COLORS( X ) \
    X( bar_bg ) X( bar_fg ) X( tag_bg ) X( tag_occupied ) X( tag_urgent ) X( tag_selected ) X( selmon ) \
    X( pertag ) X( win_bg ) X( win_focused ) X( win_urgent ) X( win_minimized ) X( win_fg ) \
    X( clock_meter ) X( stats_bg ) X( cpu ) X( io ) X( mem ) X( swap ) X( good ) X( warning ) X( bad )

#define AWL_BAR_COLOR_PIXMAN( name ) pixman_color_t name;

/**
 * What only the plugin library reads of the configuration: its defaults
 * (plugins_config_defaults.h) with plugins_config.c's changes, next to
 * :c:type:`awl_config_t` (awl.h), which awl reads (the tray's, the popups' and the
 * desktop file list's look are there). Colors are 0xRRGGBBAA, sizes
 * logical pixels. Main thread.
 */
typedef struct awl_plugin_config_t {
    /** the bar's widgets right of the task bar, AwlWidget*, from the right
     * edge in; set with :c:macro:`SETLIST() <SETLIST>` */
    struct { const int* v; size_t n; } widgets;
    /** the widgets' padding on either side of each of their cells
     * (:c:macro:`AWL_PADDING_DEFAULT`, almost all of them), in the bar font's heights */
    float widgets_padding;
    /** the library's colors; awl's are :c:member:`awl_config_t.colors` */
    awl_plugin_colors_t colors;
    /** the temperature's range, °C, over which its colors go (colors'
     * temperature): of the CPU package sensor (coretemp), else of ACPI's
     * thermal_zone0 */
    struct {
        struct { float min, max; } coretemp, thermal_zone;
    } temperature;
    struct {
        /** the battery's and the AC adapter's sysfs directories, with a
         * trailing '/' */
        const char* dir;
        const char* ac_dir;
        /** the charge in percent below which the battery is low (the
         * icons' battery_low; its colors are :c:type:`awl_plugin_colors_t`'s
         * battery) */
        float low;
        /** the dock widget's icons from the icon theme, as
         * :c:member:`awl_plugin_config_t.notify`'s bell_icon: what the machine sits
         * on (laptop, docked) and what it runs on (ac, battery, and
         * battery_low below low). In ac's, battery's and
         * battery_low's, "%d" is the charge down to its ten, 0 to 100
         * (Adwaita's battery-level-N icons). NULL, "" or none found: the
         * glyph (💻, 🖥️, ⚡, 🔋, 🪫). */
        struct { const char* laptop, * docked, * ac, * battery, * battery_low; } icons;
    } battery;
    /** the volume (PulseAudio's default sink) */
    struct {
        /** what the volume goes up to at most, 1 is 100% (above 100% the
         * widget's number is in colors.bad) */
        float max;
        /** a sink to mute on its own (the speakermute action), e.g. the
         * built-in speaker's while headphones play on another
         * (``pactl list short sinks`` names them); the widget's speaker is in
         * colors.bad while it is muted, even with the default sink muted
         * (whose mute is colors.warning, all of the widget otherwise).
         * NULL or "": none. */
        const char* speaker;
        /** what a click on the widget runs, an argv (NULL terminated);
         * NULL: nothing */
        const char* const* mixer;
    } volume;
    /** the clock's calendar (plugins/calpopup.h) */
    struct {
        /** what its button runs, an argv (NULL terminated), and the
         * button's text (NULL: app[0]); the button shows while the
         * calendar has events (Evolution's data server answers). NULL: no
         * button. */
        const char* const* app;
        const char* app_label;
    } calendar;
    /** the launcher (plugins/launcher.h), a popup: styled as the others
     * (:c:member:`awl_config_t.popup`, blurred with blur.popups; its own colors are
     * colors.launcher_*) */
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
         * :c:enumerator:`AWL_WINDOW_AT_BAR` over the bar, AWL_WINDOW_AT_STRIP_TOP or
         * AWL_WINDOW_AT_STRIP_BOTTOM along the monitor's edge (awl.h); as
         * high as the bar either way */
        int strip_at;
        /** what applications with Terminal=true run in: an argv (NULL
         * terminated) the command is appended to, e.g. { "foot", NULL } */
        const char* const* terminal;
        /** before the query; the run mode's. With markup (plugins/markup.h:
         * <b>, <i>, <u>, <t color="#rrggbb">; &lt; for a '<'), drawn in
         * colors.launcher_prompt where it sets no color */
        const char* prompt;
        const char* run_prompt;
        /** where it shows: an AWL_WINDOW_AT_* (awl.h), moved down by dy */
        int at, dy;
    } launcher;
    /** the notifications' windows (plugins/notify.h), persistent popups
     * styled as the others (their colors by urgency are colors.notify_*); how long they stay is awl's
     * (:c:member:`awl_config_t.notifications`) */
    struct {
        /** where they stack from, an AWL_WINDOW_AT_* (awl.h; the top ones
         * stack down, the others up), moved by dx, dy; gap between them */
        int at, dx, dy, gap;
        /** the content's width (the frame's padding around it); the
         * image's square left of the text */
        int width, image_size;
        /** the menu's content's width (notify_menu.h; its padding around
         * it), whatever it lists, at most the screen's */
        int menu_width;
        /** the body's lines at most (<= 0: any); the windows shown at
         * most, a "+k more" under them for the rest */
        int max_lines, max_shown;
        /** the bar of a notification with a value: its height */
        int progress_height;
        /** the app's name after the summary, in italic and braces:
         * "summary {app}" */
        int show_app;
        /** the bar widget's bell from the icon theme (:c:member:`awl_config_t.tray`'s
         * icon_theme first, then the desktop's), without and with
         * do-not-disturb: icon names or paths, ";" between them, the
         * first found wins; looked up and decoded on the image thread
         * (notify_image.h), as large as the tray's icons. NULL, "" or none
         * found: the glyph (🔔, 🔕). */
        const char* bell_icon;
        const char* bell_icon_dnd;
    } notify;
    /** the notification a volume or brightness change brings
     * (plugins/osd.h): the new value as a bar beside an icon, no text,
     * replaced in place while the changes go on; in colors.notify_low_*,
     * the bar in colors.osd_progress, or for a change awl didn't ask for
     * in colors.osd_unasked with its cause as the summary; above 100%
     * the volume's bar stays full and fills again over it in colors.bad */
    struct {
        /** ms it stays after the last change */
        int timeout;
        /** its window's content width and its icon's size, logical pixels
         * (unset: notify's) */
        awl_opt_int_t width, image_size;
        /** the icons by the volume (muted or 0, up to a third, two thirds,
         * 100%, above; only the built-in speaker muted) and by the
         * brightness (0, up to a third, two thirds, more), as notify's
         * bell_icon; none found: no icon */
        struct { const char* muted, * low, * medium, * high, * over, * headphones; } volume_icons;
        struct { const char* off, * low, * medium, * high; } brightness_icons;
        /** the summary of a change awl didn't ask for, by its cause; NULL:
         * such a change doesn't show. The brightness's by the daemon's
         * ``LastSource`` (unknown: a daemon without it; start: the update
         * at its start, which shows only if awl knew the brightness
         * before), the volume's by :c:type:`pulse_cause_t` */
        struct { const char* unknown, * start, * timer, * update, * adjust; } brightness_unasked;
        struct { const char* connect, * other, * sink, * port; } volume_unasked;
    } osd;
} awl_plugin_config_t;

/** :c:member:`awl_plugin_config_t.launcher`'s layout */
enum {
    /** a list of results under the query */
    AWL_LAUNCHER_LIST,
    /** a strip as high as the bar, the results beside the query */
    AWL_LAUNCHER_BAR,
};
/** :c:func:`awl_launcher_toggle`'s modes: the applications, or $PATH and the
 * command lines run before (plugins/launcher.h) */
enum {
    /** the applications */
    AWL_LAUNCHER_APPS,
    /** $PATH and the command lines run before */
    AWL_LAUNCHER_RUN,
};

/** The running plugins' state, see :c:func:`awl_plugin_get`. */
typedef struct awl_plugin_data_t {
    float refresh_sec;

    awl_ipaddr_t* ip;
    awl_stats_t* stats;
    awl_temperature_t* temp;
    awl_battery_t* bat;
    awl_date_t* date;
    pulse_test_t* pulse;
    awl_backlight_t* backlight;
    awl_poller_t* poller;
    /** the tray's host, and the loop it runs on (plugins/loop.h) */
    awl_sni_t* sni;
    awl_loop_t* tray_loop;
    int paused;

    /** :c:type:`awl_plugin_colors_t`'s bar colors (:c:macro:`AWL_BAR_COLORS`) as pixman's */
    struct { AWL_BAR_COLORS( AWL_BAR_COLOR_PIXMAN ) } colors;
} awl_plugin_data_t;

/** The running plugins' state; NULL while the plugins aren't running. */
awl_plugin_data_t* awl_plugin_get( void );

/** plugins.c: awl's own notification (:c:func:`awl_notify_send`) of title and
 * body (with markup) as AWL_NOTIF_SEND_INIT has it; main thread */
void awl_notify( const char* title, const char* body );
/** plugins.c: n as awl's own notification (:c:member:`awl_host_t.notif_send`), what
 * D-Bus has no say in included; logs a failure; returns its id, 0 if none.
 * awl's notifications all go through here. Main thread. */
uint32_t awl_notify_send( const awl_notif_send_t* n );
/** plugins.c: a notification "AWL config" listing the config dictionary,
 * one ``key=value`` line per entry, sorted by key; an owned value (one the
 * dictionary frees) as ``key=pointer(free function)``; its lines aren't
 * capped (but by the screen). Main thread. */
void awl_notify_config( void );
/** plugins.c: a notification "x/N" once the wallpaper all changes asked for
 * end on is shown; main thread */
void awl_notify_wallpaper_shown( const char* title );

/** plugins.c: the default sink's volume up (delta > 0) or down by delta (1
 * is 100%), up no further than the config's volume.max
 * (:c:func:`pulse_volume_adjust`); for a
 * binding's action, e.g. ``.f = 0.05f``; the osd's notification follows
 * (plugins/osd.h). Main thread, returns at once. */
void awl_volume_adjust( float delta );
/** plugins.c: mutes the default sink or unmutes it (:c:func:`pulse_mute_toggle`);
 * the osd's notification follows */
void awl_volume_mute_toggle( void );
/** plugins.c: mutes the default source or unmutes it
 * (:c:func:`pulse_mic_mute_toggle`) */
void awl_mic_mute_toggle( void );
/** plugins.c: mutes the config's volume.speaker sink or unmutes it
 * (:c:func:`pulse_speaker_mute_toggle`); the osd's notification follows */
void awl_speaker_mute_toggle( void );
/** plugins.c: the screen brightness up or down by delta (1 is all of it),
 * through the backlight daemon (:c:func:`backlight_adjust`), which this starts if
 * it isn't running; the osd's notification shows the expected value at once
 * and corrects it if the daemon's differs. Main thread, returns at once. */
void awl_brightness_adjust( float delta );

/** plugins_widgets.c: :c:member:`awl_plugin_api_t.bar_widgets` */
void awl_widgets_create( awl_bar_t* bar );

/** plugins_desktop_panel.c: starts the scanner thread */
void awl_desktop_start( void );
/** plugins_desktop_panel.c: stops the scanner thread; returns nonzero if it
 * had to be left running */
int awl_desktop_stop( void );
/** plugins_desktop_panel.c: :c:member:`awl_plugin_api_t.desktop_version` */
uint64_t awl_desktop_version( void );
/** plugins_desktop_panel.c: :c:member:`awl_plugin_api_t.desktop_measure` */
void awl_desktop_measure( struct fcft_font* font, int avail_w, int avail_h, int r, float scale,
                          int* x, int* y, int* w, int* h );
/** plugins_desktop_panel.c: :c:member:`awl_plugin_api_t.desktop_draw` */
void awl_desktop_draw( pixman_image_t* pix, struct fcft_font* font, uint32_t* data, int stride,
                       int w, int h, int r, float scale );
/** plugins_desktop_panel.c: :c:member:`awl_plugin_api_t.desktop_click` */
int awl_desktop_click( int button, uint32_t mods );

/** plugins/launcher.c: starts the launcher's index thread */
void awl_launcher_start( void );
/** plugins/launcher.c: closes the launcher, stops its thread; returns
 * nonzero if it had to be left running */
int awl_launcher_stop( void );
/** plugins/launcher.c: opens the launcher on the selected monitor in mode
 * (AWL_LAUNCHER_APPS or AWL_LAUNCHER_RUN), switches it to mode if it is
 * open in the other, or closes it (the launcher action, its .i the mode) */
void awl_launcher_toggle( int mode );

/** plugins_config.c: makes both configs, awl's defaults
 * (:c:member:`awl_host_t.config_defaults`) and the plugins' with configure() (the
 * CUSTOM part of plugins_config.c) applied; first thing in :c:member:`awl_plugin_api_t.init`, so before
 * anything reads them */
void awl_config_init( void );
/** plugins_config.c: the config :c:func:`awl_config_init` made; valid until the
 * library is unloaded */
const awl_config_t* awl_config( void );
/** plugins_config.c: the plugins' config :c:func:`awl_config_init` made */
const awl_plugin_config_t* awl_plugin_config( void );
/** plugins_config.c: fills in what p leaves unset (:c:type:`awl_opt_color_t`,
 * :c:type:`awl_opt_int_t`) with what it defaults to, from p and c, c filled in
 * already (:c:func:`awl_config_fill_defaults`); once, in :c:func:`awl_config_init` */
void awl_plugin_config_fill_defaults( awl_plugin_config_t* p, const awl_config_t* c );
/** plugins_config.c: :c:member:`awl_plugin_api_t.config`, a copy of :c:func:`awl_config` */
void awl_config_fill( awl_config_t* c );
