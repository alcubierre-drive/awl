#pragma once

/*
 * The bar as awl and the plugin library share it: `awl_bar_t`, part of its
 * Monitor (Monitor.bar), with its fonts, its widgets (`widget_t`) and what
 * awl tells them. awl_bar.h is what awl does with it. No wlroots here;
 * awl.h includes this for Monitor.
 */

#include "awl_draw.h"

#include <limits.h>

/** `widget_t.pad_left`, `pad_right`: half the bar font's height */
#define AWL_PADDING_FONT INT_MIN
/** `widget_t.pad_left`, `pad_right`: the configuration's padding, the same
 * for (almost) every widget */
#define AWL_PADDING_DEFAULT (INT_MIN + 1)

typedef struct widget_t widget_t;
typedef struct awl_bar_t awl_bar_t;
typedef struct Client Client;
typedef struct Monitor Monitor;
typedef struct awl_window awl_window_t;

/**
 * A part of the bar: the tags, the layout symbol, the task bar, the clock,
 * the graphs... Each bar has its own, made by `awl_plugin_api_t.bar_widgets`
 * (plugins_widgets.c); everything but `bar` and `width` is the library's.
 */
struct widget_t {
    /** what the last draw used, set by awl */
    uint32_t width;
    /**
     * Right-hand widgets are laid out from the bar's right edge, so their
     * width has to be known before anything is drawn: measure() takes a
     * snapshot of whatever the widget shows and returns its exact width, then
     * draw() renders that snapshot at x into exactly that width (its return
     * value is ignored). Left and center widgets only have draw(), which
     * returns the width it used.
     */
    uint32_t (*measure)(widget_t* this);
    /** see `measure` */
    uint32_t (*draw)(widget_t* this, uint32_t x, pixman_image_t* pix);
    /** pointer motion over the widget, x_rel from its left edge (buffer
     * pixels); -1 once it left the widget (for one that isn't, or awl's
     * popups, a client over the bar), and before the plugins detach */
    void (*callback_view)(widget_t* this, int32_t x_rel);
    /** a click, button as ``BTN_*`` */
    void (*callback_click)(widget_t* this, uint32_t x_rel, int button);
    /** a scroll, amount 1 or -1 (see `scroll_amount`) */
    void (*callback_scroll)(widget_t* this, uint32_t x_rel, int amount);
    /** called once the pointer has rested on the widget for hover_delay_ms;
     * leaving it before then cancels, and it fires again only after the
     * pointer has left and come back */
    void (*callback_hover)(widget_t* this);
    /** see `callback_hover` */
    uint32_t hover_delay_ms;
    /** called leave_delay_ms after the pointer has left a widget whose
     * callback_hover fired, unless it came back first (0: right away). While
     * the pointer is inside popup_window (the popup the hover opened), it
     * counts as still on the widget. A press on the widget calls it at
     * once (and cancels a hover not yet fired); hovering fires again only
     * after the pointer has left and come back. */
    void (*callback_leave)(widget_t* this);
    /** see `callback_leave` */
    uint32_t leave_delay_ms;
    /** see `callback_leave`: the widget's awl_window (awl_host_t's
     * window_create()), if its popup is one; NULL if none */
    awl_window_t* popup_window;
    /** awl's sum of scroll deltas since the last `callback_scroll`, which
     * comes once it is large enough */
    double scroll_amount;
    /** the widget's own state, freed with `free` */
    void* userdata;
    /** frees `userdata`, when the widget goes away */
    void (*free)( void* userdata );
    /** logical pixels left and right of each of its cells (each tag, task
     * bar window, temperature...: see plugins_widgets.c), negative ones
     * overlapping the neighbours; AWL_PADDING_FONT: half the font's height,
     * AWL_PADDING_DEFAULT: the configuration's */
    int pad_left, pad_right;
    /** the bar it is on */
    awl_bar_t* bar;
};

/** A window as the task bar shows it, a snapshot taken by awl. */
typedef struct awl_bar_window_t {
    char name[255];
    struct { uint8_t
        floating:1,
        urgent:1,
        focused:1,
        visible:1,
        maximized:1,
        fullscreen:1,
        ontop:1;
    };
    Client* c;
} awl_bar_window_t;

/** A bar, Monitor.bar: its font, its widgets and what awl tells them,
 * which awl sets before every redraw. */
struct awl_bar_t {
    /** the bar's image while awl draws it (between awl_bar_prepare_drawing()
     * and awl_bar_finish_drawing()), else NULL */
    pixman_image_t *pix;
    /** the bar's font, at its monitor's scale; NULL if none loads */
    struct fcft_font *font;
    /** its emoji font (`awl_config_t.emoji_font`) at the same size and
     * scale, color emoji (awl_font_get_emoji()); NULL if there is none or
     * it doesn't load: then the widgets show text instead of emoji */
    struct fcft_font *emojifont;

    widget_t widgets_left[32];
    int n_widgets_left;
    widget_t widgets_right[32];
    int n_widgets_right;
    widget_t center_widget;
    uint32_t center_widget_space;
    uint32_t center_widget_start;
    int has_center_widget;

    /** the current tag's windows */
    awl_bar_window_t tagwindows[128];
    int n_tagwindows;
    /** occ, urg and sel: tag masks of the occupied, urgent and selected
     * tags */
    uint32_t occ, urg, sel;
    int ntags;
    /** nonzero on the bar of the selected monitor, -1 if only one monitor exists */
    int selmon;
    /** nonzero if the monitor gives each tag its own layout (Monitor.pertag) */
    int pertag;
    /** the monitor's layout symbol (Monitor.ltsymbol) */
    char ltsymbol[16];
    /** what the widgets need of the monitor, so they never read it: its
     * output's name (valid while the bar is) and scale, the bar's height
     * and text padding (left and right together; updatebar() sets it with
     * the font) in buffer pixels, and where the monitor is in the layout
     * (logical pixels) */
    const char *output;
    float scale;
    int height, lrpad;
    int x, y;

    /** the bar's monitor, opaque to the library (to hand back to awl, e.g.
     * `awl_host_t.window_show`); NULL before awl_bar_setup() and after
     * awl_bar_teardown(), while the monitor has no bar */
    Monitor* m;
};
