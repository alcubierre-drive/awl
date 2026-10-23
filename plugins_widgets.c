/** The bar widgets. Built into libawlplugins.so together with the plugins
 * whose data they show, so a reload swaps both at once; everything they need
 * from awl goes through the host table (awl_plugin_abi.h). */
#include "awl_plugin_abi.h"
#include "plugins.h"
#include "plugins/date.h"
#include "plugins/calpopup.h"
#include "plugins/notify.h"
#include "plugins/notify_menu.h"
#include "plugins/colors.h"
#include "plugins/tray_menu.h"
#include "plugins/tray_tooltip.h"
#include "plugins/sni.h"
#include "vendor/wuffs/wuffs.h"
#include "icons/eyes_closed.h"

#include <arpa/inet.h>
#include <stdatomic.h>


static uint32_t tagwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void tagwidget_scroll( widget_t* w, uint32_t x, int amount );
static void tagwidget_click( widget_t* w, uint32_t x, int button );

static uint32_t layoutwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void layoutwidget_scroll( widget_t* w, uint32_t x, int amount );
static void layoutwidget_click( widget_t* w, uint32_t x, int button );

static uint32_t taskbarwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void taskbarwidget_scroll( widget_t* w, uint32_t x, int amount );
static void taskbarwidget_click( widget_t* w, uint32_t x, int button );

/** draw() of the right-hand widgets that show a single text (see :c:type:`textsnap_t`) */
static uint32_t textsnap_draw( widget_t* w, uint32_t x, pixman_image_t* pix );

static uint32_t clockwidget_measure( widget_t* w );
static uint32_t clockwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void clockwidget_hover( widget_t* w );
static void clockwidget_leave( widget_t* w );
static void clockwidget_scroll( widget_t* w, uint32_t x, int amount );

static uint32_t pulsewidget_measure( widget_t* w );
static void pulsewidget_click( widget_t* w, uint32_t x, int button );
static void pulsewidget_scroll( widget_t* w, uint32_t x, int amount );

static uint32_t sniwidget_measure( widget_t* w );
static uint32_t sniwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void sniwidget_click( widget_t* w, uint32_t x, int button );
static void sniwidget_scroll( widget_t* w, uint32_t x, int amount );
static void sniwidget_view( widget_t* w, int32_t x );
static void sniwidget_free( void* userdata );
static uint32_t statuswidget_measure( widget_t* w );
static uint32_t statuswidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static uint32_t tempwidget_measure( widget_t* w );
static uint32_t tempwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static uint32_t batwidget_measure( widget_t* w );
static uint32_t pulsewidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static uint32_t dockwidget_measure( widget_t* w );
static uint32_t dockwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void dockwidget_free( void* userdata );
static uint32_t ipwidget_measure( widget_t* w );
static uint32_t ipwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void ipwidget_hover( widget_t* w );
static void ipwidget_leave( widget_t* w );
static void ipwidget_free( void* userdata );

static uint32_t backlightwidget_measure( widget_t* w );
static uint32_t backlightwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void backlightwidget_click( widget_t* w, uint32_t x, int button );
static void backlightwidget_scroll( widget_t* w, uint32_t x, int amount );

static uint32_t notifywidget_measure( widget_t* w );
static uint32_t notifywidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void notifywidget_click( widget_t* w, uint32_t x, int button );
static void notifywidget_view( widget_t* w, int32_t x );
static void notifywidget_hover( widget_t* w );
static void notifywidget_leave( widget_t* w );
static void notifywidget_free( void* userdata );

/** the usual padding, :c:member:`awl_plugin_config_t.widgets_padding` either side */
#define PAD_DEFAULT .pad_left = AWL_PADDING_DEFAULT, .pad_right = AWL_PADDING_DEFAULT

/** the widgets right of the task bar, by :c:member:`awl_plugin_config_t.widgets`' kind */
static const widget_t right_widgets[] = {
    [AwlWidgetClock] = {
        PAD_DEFAULT,
        .measure = &clockwidget_measure,
        .draw = &clockwidget_draw,
        .callback_hover = &clockwidget_hover,
        .hover_delay_ms = 100,
        .callback_leave = &clockwidget_leave,
        .callback_scroll = &clockwidget_scroll,
        .leave_delay_ms = 20,
        .free = free,
    },
    [AwlWidgetTray] = {
        PAD_DEFAULT,
        .measure = &sniwidget_measure,
        .draw = &sniwidget_draw,
        .callback_click = &sniwidget_click,
        .callback_scroll = &sniwidget_scroll,
        .callback_view = &sniwidget_view,
        .free = &sniwidget_free,
    },
    [AwlWidgetVolume] = {
        PAD_DEFAULT,
        .measure = &pulsewidget_measure,
        .draw = &pulsewidget_draw,
        .callback_click = &pulsewidget_click,
        .callback_scroll = &pulsewidget_scroll,
        .free = free,
    },
    [AwlWidgetStats] = {
        .pad_left = 0, .pad_right = 0,
        .measure = &statuswidget_measure,
        .draw = &statuswidget_draw,
        .free = free,
    },
    [AwlWidgetTemperature] = {
        PAD_DEFAULT,
        .measure = &tempwidget_measure,
        .draw = &tempwidget_draw,
        .free = free,
    },
    [AwlWidgetDock] = {
        /* right against the battery's percent */
        .pad_left = -2, .pad_right = AWL_PADDING_DEFAULT,
        .measure = &dockwidget_measure,
        .draw = &dockwidget_draw,
        .free = &dockwidget_free,
    },
    [AwlWidgetBattery] = {
        PAD_DEFAULT,
        .measure = &batwidget_measure,
        .draw = &textsnap_draw,
        .free = free,
    },
    [AwlWidgetNetwork] = {
        PAD_DEFAULT,
        .measure = &ipwidget_measure,
        .draw = &ipwidget_draw,
        .callback_hover = &ipwidget_hover,
        .hover_delay_ms = 100,
        .callback_leave = &ipwidget_leave,
        .leave_delay_ms = 20,
        .free = &ipwidget_free,
    },
    [AwlWidgetBacklight] = {
        PAD_DEFAULT,
        .measure = &backlightwidget_measure,
        .draw = &backlightwidget_draw,
        .callback_click = &backlightwidget_click,
        .callback_scroll = &backlightwidget_scroll,
        .free = free,
    },
    [AwlWidgetNotifications] = {
        .pad_left = 0, .pad_right = 0,
        .measure = &notifywidget_measure,
        .draw = &notifywidget_draw,
        .callback_click = &notifywidget_click,
        .callback_view = &notifywidget_view,
        .callback_hover = &notifywidget_hover,
        .hover_delay_ms = 100,
        .callback_leave = &notifywidget_leave,
        .leave_delay_ms = 20,
        .free = &notifywidget_free,
    },
};

void awl_widgets_create( awl_bar_t* bar ) {
    bar->widgets_left[bar->n_widgets_left++] = (widget_t){
        .bar = bar,
        /* each tag a button about as wide as the bar is high */
        .pad_left = AWL_PADDING_FONT, .pad_right = AWL_PADDING_FONT,
        .draw = &tagwidget_draw,
        .callback_click = &tagwidget_click,
        .callback_scroll = &tagwidget_scroll,
    };
    bar->widgets_left[bar->n_widgets_left++] = (widget_t){
        .bar = bar,
        PAD_DEFAULT,
        .draw = &layoutwidget_draw,
        .callback_click = &layoutwidget_click,
        .callback_scroll = &layoutwidget_scroll,
    };

    bar->center_widget = (widget_t){
        .bar = bar,
        PAD_DEFAULT,
        .draw = &taskbarwidget_draw,
        .callback_click = &taskbarwidget_click,
        .callback_scroll = &taskbarwidget_scroll,
    };
    bar->has_center_widget = 1;

    const awl_plugin_config_t* pc = awl_plugin_config();
    for (size_t i = 0; i < pc->widgets.n; i++) {
        int k = pc->widgets.v[i];
        if (k < 0 || (size_t)k >= LENGTH( right_widgets ) ||
            bar->n_widgets_right >= (int)LENGTH( bar->widgets_right ))
            continue;
        widget_t* w = &bar->widgets_right[bar->n_widgets_right++];
        *w = right_widgets[k];
        w->bar = bar;
    }
}

/* Every widget is made of cells with its padding on either side
 * (`widget_t.pad_left`, `pad_right`, mostly `awl_plugin_config_t.widgets_padding`):
 * cell_width() says how wide one is, cell_text() draws a text one,
 * cell_pads() a picture's padding. */

/** one side's padding, pad (a :c:type:`widget_t`'s), in the bar's pixels */
static int pad_px( const widget_t* w, int pad ) {
    if (pad == AWL_PADDING_FONT) return w->bar->lrpad / 2;
    if (pad == AWL_PADDING_DEFAULT)
        return (int)lroundf( (float)w->bar->lrpad * awl_plugin_config()->widgets_padding );
    return (int)lroundf( (float)pad * w->bar->scale );
}

/** w's padding, left and right, in the bar's pixels */
static void widget_padding( const widget_t* w, int* l, int* r ) {
    *l = pad_px( w, w->pad_left );
    *r = pad_px( w, w->pad_right );
}

/** a cell of w around content pixels, 0 if the padding leaves nothing */
static uint32_t cell_width( const widget_t* w, uint32_t content ) {
    int l, r;
    widget_padding( w, &l, &r );
    long cw = (long)content + l + r;
    return cw > 0 ? (uint32_t)cw : 0;
}

/** a cell of w around text in font */
static uint32_t text_cell_width( const widget_t* w, struct fcft_font* font, const char* text ) {
    return cell_width( w, awl_host->text_width( font, text ) );
}

/** Draws a cell of w at x, cw wide: bg, then text in font between the
 * paddings, cut off with an ellipsis if it doesn't fit. Negative padding
 * puts the text over the neighbours. */
static void cell_text( widget_t* w, pixman_image_t* pix, int x, uint32_t cw,
        struct fcft_font* font, const char* text, pixman_color_t fg, pixman_color_t bg ) {
    int l, r, h = (int)w->bar->height;
    widget_padding( w, &l, &r );
    long room = (long)cw - l - r;
    awl_host->text( pix, font, x, -2, cw, h, 0, "", fg, bg );
    if (room <= 0 || !*text) return;
    if (l >= 0 && r >= 0)
        awl_host->text( pix, font, x + l, -2, (unsigned)room, h, 0, text, fg, bg );
    else // over what is there, also outside the cell
        awl_host->text_over( pix, font, x + l, -2, (unsigned)room, h, text,
                             color_16bit_to_8bit( fg ) );
}

/** fills the padding of w's picture cell at x, cw wide, with bg */
static void cell_pads( widget_t* w, pixman_image_t* pix, int x, uint32_t cw, pixman_color_t bg ) {
    int l, r;
    widget_padding( w, &l, &r );
    if (l > (int)cw) l = (int)cw;
    if (r > (int)cw) r = (int)cw;
    pixman_box32_t pads[2];
    int n = 0;
    if (l > 0) pads[n++] = (pixman_box32_t){ x, 0, x + l, (int)w->bar->height };
    if (r > 0) pads[n++] = (pixman_box32_t){ x + (int)cw - r, 0, x + (int)cw, (int)w->bar->height };
    if (n) pixman_image_fill_boxes( PIXMAN_OP_SRC, pix, &bg, n, pads );
}

static uint32_t tagwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return 0;
    int width = 0;
    for (int t=0; t<w->bar->ntags; ++t) {
        char num[16] = ""; snprintf( num, sizeof(num)-1, "%i", t+1 );
        int ww = text_cell_width( w, w->bar->font, "0" );
        pixman_color_t bg_color = P->colors.tag_bg,
                       fg_color = P->colors.bar_fg;
        if (w->bar->occ & (1 << t))
            bg_color = alpha_blend_16( bg_color, P->colors.tag_occupied );
        if (w->bar->urg & (1 << t))
            bg_color = alpha_blend_16( bg_color, P->colors.tag_urgent );
        if (w->bar->sel & (1 << t))
            bg_color = alpha_blend_16( bg_color, P->colors.tag_selected );
        cell_text( w, pix, x, ww, w->bar->font, num, fg_color, bg_color );
        /* the selected monitor: a mark in the "1"'s corner */
        if (t == 0 && w->bar->selmon && w->bar->selmon != -1) {
            int hh = w->bar->height;
            int h = hh / 8;
            if (h < 2) h = 2;
            int w = (ww < hh ? ww : hh)/2;
            if (w < 2) w = 2;
            pixman_color_t c = P->colors.selmon;
            pixman_rectangle16_t rects[] = {
                { .x=x, .y=hh-h, .width=w, .height=h },
                { .x=x, .y=hh-w, .width=h, .height=w },
            };
            pixman_image_fill_rectangles( PIXMAN_OP_OVER, pix, &c, 2, rects );
        }
        x += ww;
        width += ww;
    }
    return width;
}

static uint32_t taskbarwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    uint32_t space = w->bar->center_widget_space;
    if (space == 0) return 0;

    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return 0;

    int n_windows = w->bar->n_tagwindows;
    if (n_windows <= 0) {
        cell_text( w, pix, x, space, w->bar->font, "", P->colors.win_fg, (pixman_color_t){0} );
        return 0;
    }

    // calculate space per window
    // the first space % n_windows get one pixel more
    uint32_t spaces[n_windows];
    for (int wi=0; wi<n_windows; ++wi)
        spaces[wi] = space / n_windows + ((uint32_t)wi < space % n_windows);
    int nospace = 0;
    for (int wi=0; wi<n_windows; ++wi)
        if (spaces[wi] <= 20)
            nospace = 1;

    if (nospace) {
        cell_text( w, pix, x, space, w->bar->font, "+++", P->colors.win_fg, P->colors.win_urgent );
        return space;
    }

    awl_bar_window_t* windows = w->bar->tagwindows;

    // max, float, top
    for (int wi=0; wi<n_windows; ++wi) {
        /* prefix is at most "[FMT] ", name is a fixed-size NUL-terminated
         * field -- size txt so both always fit */
        char txt[sizeof(windows[wi].name) + 8] = {0};
        if (windows[wi].floating || windows[wi].maximized || windows[wi].ontop) {
            strcat( txt, "[" );
            if (windows[wi].floating) strcat( txt, "F" );
            if (windows[wi].maximized) strcat( txt, "M" );
            if (windows[wi].ontop) strcat( txt, "T" );
            strcat( txt, "] " );
        }
        strcat( txt, windows[wi].name );
        pixman_color_t bg = windows[wi].focused ? P->colors.win_focused :
                            windows[wi].urgent  ? P->colors.win_urgent :
                            windows[wi].visible ? P->colors.win_bg     :
                                                  P->colors.win_minimized;
        /* frosted: awl blurs what is behind the window list (drawbar()) */
        if (awl_config()->blur.windowlist && !windows[wi].focused && !windows[wi].urgent)
            bg.alpha = 0x8888;
        cell_text( w, pix, x, spaces[wi], w->bar->font, txt, P->colors.win_fg, bg );
        x += spaces[wi];
    }
    return w->bar->center_widget_space;
}

static uint32_t layoutwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return 0;
    int ww = text_cell_width( w, w->bar->font, "XXX" );
    cell_text( w, pix, x, ww, w->bar->font, w->bar->ltsymbol, P->colors.bar_fg, P->colors.bar_bg );
    /* each tag has its own layout: a red frame around the symbol */
    if (w->bar->pertag) {
        int hh = w->bar->height;
        int t = hh / 8;
        if (t < 1) t = 1;
        pixman_color_t c = P->colors.pertag;
        pixman_rectangle16_t rects[] = {
            { .x=x,        .y=0,    .width=ww, .height=t },
            { .x=x,        .y=hh-t, .width=ww, .height=t },
            { .x=x,        .y=0,    .width=t,  .height=hh },
            { .x=x+ww-t,   .y=0,    .width=t,  .height=hh },
        };
        pixman_image_fill_rectangles( PIXMAN_OP_SRC, pix, &c, 4, rects );
    }
    return ww;
}

/** A right-hand widget showing one text: measure() fills it in, then
 * :c:func:`textsnap_draw` renders exactly that, so width and pixels always agree. */
typedef struct {
    char text[128];
    pixman_color_t fg, bg;
} textsnap_t;

static textsnap_t* textsnap( widget_t* w ) {
    if (!w->userdata) w->userdata = calloc(1, sizeof(textsnap_t));
    return w->userdata;
}

static uint32_t textsnap_width( widget_t* w ) {
    textsnap_t* s = w->userdata;
    return s && *s->text ? text_cell_width( w, w->bar->font, s->text ) : 0;
}

static uint32_t textsnap_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    textsnap_t* s = w->userdata;
    if (!s) return 0;
    cell_text( w, pix, x, w->width, w->bar->font, s->text, s->fg, s->bg );
    return w->width;
}

typedef struct {
    char timestr[16];
    int sec; // -1: unknown
    /** where draw() put the widget, in bar pixels */
    uint32_t x;
} clockwidget_userdata_t;

static uint32_t clockwidget_measure( widget_t* w ) {
    if (!w->userdata) w->userdata = calloc(1, sizeof(clockwidget_userdata_t));
    clockwidget_userdata_t* u = w->userdata;
    awl_plugin_data_t* P = awl_plugin_get();
    // events fetched, or a new day: the calendar popup follows
    awl_calpopup_poll();
    int secs = P && P->date ? atomic_load( &P->date->secs ) : -1;
    if (secs < 0) {
        strcpy(u->timestr, "--:--");
        u->sec = -1;
    } else {
        snprintf(u->timestr, sizeof(u->timestr), "%02d:%02d", secs / 3600, secs / 60 % 60);
        u->sec = secs % 60;
    }
    // the time's cell, then the seconds meter
    int mw = 3 * w->bar->scale + 0.5f;
    if (mw < 2) mw = 2;
    return text_cell_width( w, w->bar->font, "--:--" ) + mw;
}

static uint32_t clockwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return 0;
    clockwidget_userdata_t* u = w->userdata;
    u->x = x;

    int mw = 3 * w->bar->scale + 0.5f;
    if (mw < 2) mw = 2;
    int ww = w->width - mw;
    cell_text( w, pix, x, ww, w->bar->font, u->timestr, P->colors.bar_fg, P->colors.bar_bg );

    // seconds meter right of the time: empty at :00, full at :59
    int sec = u->sec;
    int bar_height = w->bar->height;
    int ydiv = sec < 0 ? bar_height : bar_height - (sec * bar_height + 29) / 59;
    pixman_box32_t bg = { .x1 = x+ww, .x2 = x+ww+mw, .y1 = 0, .y2 = ydiv },
                   fg = { .x1 = x+ww, .x2 = x+ww+mw, .y1 = ydiv, .y2 = bar_height };
    pixman_image_fill_boxes( PIXMAN_OP_SRC, pix, &P->colors.bar_bg, 1, &bg );
    pixman_image_fill_boxes( PIXMAN_OP_SRC, pix, &P->colors.clock_meter, 1, &fg );
    return w->width;
}

/* ---- the tray (plugins/sni.h) ---- */


/** an icon decoded at px, for the item of serial in the state of gen */
typedef struct {
    uint64_t serial;
    uint32_t gen;
    int px;
    pixman_image_t* img;
} sni_icon_t;

/** a tray widget: the items it shows, their icons and where they are */
typedef struct sniwidget_t {
    /** what measure() saw, draw() draws */
    awl_sni_snap_t* snap;
    sni_icon_t* icons;
    int nicons;
    /** physical pixels: the icons' size, the gap between them, the left
     * padding; x: where draw() put the widget */
    int px, gap, lpad;
    uint32_t x;
    /** the icon under the pointer, -1 none; whether it is on the widget */
    int hovered, inside;
    uint32_t tip;
    /** sniwidgets, every bar's */
    struct sniwidget_t* next;
} sniwidget_t;

static sniwidget_t* sniwidgets;

static sniwidget_t* sniwidget( widget_t* w ) {
    sniwidget_t* u = w->userdata;
    if (!u && (u = w->userdata = calloc( 1, sizeof(sniwidget_t) ))) {
        u->hovered = -1;
        u->next = sniwidgets;
        sniwidgets = u;
    }
    return u;
}

/** the item's icon at u->px: the one decoded before, if it's still current */
static pixman_image_t* sniwidget_icon( sniwidget_t* u, const awl_sni_item_t* it, sni_icon_t* old, int nold ) {
    for (int i = 0; i < nold; i++)
        if (old[i].serial == it->serial && old[i].img) {
            if (old[i].gen == it->icon_gen && old[i].px == u->px) {
                pixman_image_t* img = old[i].img;
                old[i].img = NULL;
                return img;
            }
            break;
        }
    if (it->file) {
        char source[128];
        snprintf( source, sizeof source, "tray item '%s' (its file)", it->name );
        return awl_icon_decode( it->file->data, it->file->len, !!(it->file_flags & AWL_ICON_SVG), u->px, u->px, source );
    }
    if (it->pixmaps)
        return awl_icon_from_pixmaps( (const awl_icon_pixmap_t*)it->pixmaps->data, it->npixmaps, u->px, u->px );
    return NULL;
}

static uint32_t sniwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    sniwidget_t* u = sniwidget( w );
    if (!P || !P->sni || !u) return 0;
    sni_main_poll( P->sni );
    float scale = w->bar->scale;
    int h = (int)w->bar->height;
    u->px = (int)lroundf( awl_config()->tray.icon_size * scale );
    if (u->px > h - 2) u->px = h - 2;
    if (u->px < 1) u->px = 1;
    u->gap = (int)lroundf( awl_config()->tray.icon_gap * scale );
    /* the lookups go by the largest bar's of those there are */
    int largest = 0;
    for (const sniwidget_t* o = sniwidgets; o; o = o->next)
        if (o->px > largest) largest = o->px;
    sni_set_icon_px( P->sni, largest );

    awl_sni_snap_t* snap = sni_snap_get( P->sni );
    sni_icon_t* icons = snap->n ? calloc( (size_t)snap->n, sizeof(*icons) ) : NULL;
    for (int i = 0; icons && i < snap->n; i++) {
        const awl_sni_item_t* it = &snap->items[i];
        icons[i] = (sni_icon_t){ it->serial, it->icon_gen, u->px, sniwidget_icon( u, it, u->icons, u->nicons ) };
    }
    for (int i = 0; i < u->nicons; i++)
        if (u->icons[i].img) pixman_image_unref( u->icons[i].img );
    free( u->icons );
    u->icons = icons;
    u->nicons = icons ? snap->n : 0;
    if (u->snap) sni_snap_unref( u->snap );
    u->snap = snap;
    if (u->hovered >= u->nicons) u->hovered = -1;
    if (!u->nicons) return 0;
    int r;
    widget_padding( w, &u->lpad, &r );
    return cell_width( w, (uint32_t)(u->nicons * u->px + (u->nicons - 1) * u->gap) );
}

static uint32_t sniwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    sniwidget_t* u = w->userdata;
    if (!P || !u) return 0;
    u->x = x;
    int h = (int)w->bar->height;
    pixman_color_t bg = color_8bit_to_16bit( awl_plugin_config()->colors.tray_bg );
    pixman_image_fill_boxes( PIXMAN_OP_SRC, pix, &bg, 1, &(pixman_box32_t){
        .x1 = x, .x2 = x + w->width, .y1 = 0, .y2 = h } );
    for (int i = 0; i < u->nicons; i++) {
        pixman_image_t* img = u->icons[i].img;
        if (!img) continue;
        int iw = pixman_image_get_width( img ), ih = pixman_image_get_height( img );
        int ix = (int)x + u->lpad + i * (u->px + u->gap) + (u->px - iw) / 2;
        int iy = (h - ih) / 2;
        if (u->snap->items[i].file && (u->snap->items[i].file_flags & AWL_ICON_SYMBOLIC)) {
            /* its shape in the text's color */
            pixman_image_t* solid = pixman_image_create_solid_fill( &P->colors.bar_fg );
            pixman_image_composite32( PIXMAN_OP_OVER, solid, img, pix, 0, 0, 0, 0, ix, iy, iw, ih );
            pixman_image_unref( solid );
        } else {
            pixman_image_composite32( PIXMAN_OP_OVER, img, NULL, pix, 0, 0, 0, 0, ix, iy, iw, ih );
        }
    }
    return w->width;
}

/** the icon at x (from the widget's left edge; the gaps split between
 * their icons), -1 none */
static int sniwidget_at( const sniwidget_t* u, int32_t x ) {
    x -= u ? u->lpad - u->gap / 2 : 0;
    if (!u || x < 0 || u->px + u->gap <= 0) return -1;
    int i = x / (u->px + u->gap);
    return i < u->nicons ? i : -1;
}

/** icon i's center, logical pixels from the monitor's left edge */
static int sniwidget_center( widget_t* w, const sniwidget_t* u, int i ) {
    return (int)lround( ((int)u->x + u->lpad + i * (u->px + u->gap) + u->px / 2.0) / w->bar->scale );
}

/** Right: the menu. Left: what the item does when activated, unless it says
 * it is a menu (ItemIsMenu), then its menu too (plugins/sni.c opens it if
 * Activate fails). Middle: SecondaryActivate. An item without a menu draws
 * its own on ContextMenu. With its menu open, any press only closes it. */
static void sniwidget_click( widget_t* w, uint32_t x, int button ) {
    awl_plugin_data_t* P = awl_plugin_get();
    sniwidget_t* u = w->userdata;
    int i = sniwidget_at( u, (int32_t)x );
    if (!P || !P->sni || i < 0) return;
    const awl_sni_item_t* it = &u->snap->items[i];
    awl_tray_tooltip_drop( u->tip );
    u->tip = 0;
    const awl_bar_t* bar = w->bar;
    const char* mon = bar->output;
    int cx = sniwidget_center( w, u, i );
    if (awl_tray_menu_window_shows( it->menu )) {
        awl_tray_menu_window_toggle( it->menu, mon, cx );
        return;
    }
    if ((button == BTN_LEFT && it->item_is_menu) || button == BTN_RIGHT) {
        if (it->menu) awl_tray_menu_window_toggle( it->menu, mon, cx );
        else sni_click( P->sni, it->serial, AWL_SNI_CONTEXT, bar->x + cx, bar->y, mon, cx );
    } else if (button == BTN_LEFT) {
        sni_click( P->sni, it->serial, AWL_SNI_ACTIVATE, bar->x + cx, bar->y, mon, cx );
    } else if (button == BTN_MIDDLE) {
        sni_click( P->sni, it->serial, AWL_SNI_SECONDARY, bar->x + cx, bar->y, mon, cx );
    }
}

static void sniwidget_scroll( widget_t* w, uint32_t x, int amount ) {
    awl_plugin_data_t* P = awl_plugin_get();
    sniwidget_t* u = w->userdata;
    int i = sniwidget_at( u, (int32_t)x );
    if (P && P->sni && i >= 0 && amount) sni_scroll( P->sni, u->snap->items[i].serial, amount, 0 );
}

/** the tooltip of the icon under the pointer; the menu open stays while
 * the pointer is anywhere on the tray */
static void sniwidget_view( widget_t* w, int32_t x ) {
    static uint32_t next_tip;
    sniwidget_t* u = w->userdata;
    if (!u || !u->snap) return;
    int inside = x >= 0;
    if (inside != u->inside) {
        u->inside = inside;
        for (int k = 0; k < u->snap->n; k++)
            if (u->snap->items[k].menu) awl_tray_menu_window_pointer( u->snap->items[k].menu, inside );
    }
    int i = inside ? sniwidget_at( u, x ) : -1;
    if (i == u->hovered) return;
    u->hovered = i;
    awl_tray_tooltip_drop( u->tip );
    u->tip = 0;
    if (i < 0 || !*u->snap->items[i].name) return;
    if (!++next_tip) next_tip = 1;
    u->tip = next_tip;
    awl_tray_tooltip_show( u->tip, w->bar->output, sniwidget_center( w, u, i ),
                           u->snap->items[i].name );
}

static void sniwidget_free( void* userdata ) {
    sniwidget_t* u = userdata;
    if (!u) return;
    for (sniwidget_t** p = &sniwidgets; *p; p = &(*p)->next)
        if (*p == u) {
            *p = u->next;
            break;
        }
    for (int i = 0; i < u->nicons; i++)
        if (u->icons[i].img) pixman_image_unref( u->icons[i].img );
    free( u->icons );
    if (u->snap) sni_snap_unref( u->snap );
    free( u );
}

/* the volume: a speaker filled from below as far as the volume goes (all
 * of it above 100 %), the rest dim, then the percentage; in the warning
 * color while muted, in the bad one above 100 %; the speaker alone in the
 * bad color while the config's volume.speaker sink is muted, the default
 * sink muted or not */
typedef struct {
    textsnap_t text;
    pixman_color_t speaker;
    float level;
    /** the speaker's size, its top and its gap to the text, in the bar's
     * pixels */
    int sw, sh, sy, gap;
} pulsesnap_t;

static uint32_t pulsewidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->pulse) return 0;
    if (!w->userdata) w->userdata = calloc( 1, sizeof(pulsesnap_t) );
    pulsesnap_t* s = w->userdata;
    if (!s) return 0;
    float val = atomic_load( &P->pulse->value );
    int muted = atomic_load( &P->pulse->muted );
    snprintf( s->text.text, sizeof(s->text.text), "%3.0f%%", val * 100.0f );
    s->text.fg = muted ? P->colors.warning :
                 lround(val*100.0) > 100 ? P->colors.bad :
                                           P->colors.bar_fg;
    s->text.bg = P->colors.bar_bg;
    s->speaker = atomic_load( &P->pulse->speaker_muted ) ? P->colors.bad : s->text.fg;
    s->level = val < 0 ? 0 : val > 1 ? 1 : val;
    /* as tall as the digits, on their line as text() places them */
    struct fcft_font* font = w->bar->font;
    const int baseline = ((int)w->bar->height - font->height) / 2 - 2 + font->ascent;
    const struct fcft_glyph* g = fcft_rasterize_char_utf32( font, U'0', FCFT_SUBPIXEL_NONE );
    s->sh = g && g->height > 0 ? g->height : font->ascent;
    s->sy = baseline - (g && g->height > 0 ? g->y : font->ascent);
    s->sw = (int)lroundf( (float)s->sh * 0.8f );
    s->gap = (int)lroundf( 2.0f * w->bar->scale );
    // fixed width so the bar doesn't jitter while the volume changes
    return cell_width( w, (uint32_t)(s->sw + s->gap) + awl_host->text_width( w->bar->font, "___%" ) );
}

/** the speaker's shape, sw x sh, into an a8 mask: the box at the back, the
 * cone flaring to the right */
static pixman_image_t* speaker_mask( int sw, int sh ) {
    pixman_image_t* mask = pixman_image_create_bits( PIXMAN_a8, sw, sh, NULL, 0 );
    if (!mask) return NULL;
    const double w = sw, h = sh, bx = 0.3 * w, by0 = 0.3 * h, by1 = 0.7 * h;
#define PT( x, y ) { pixman_double_to_fixed( x ), pixman_double_to_fixed( y ) }
    const pixman_triangle_t tris[] = {
        { PT( 0, by0 ), PT( bx, by0 ), PT( bx, by1 ) },
        { PT( 0, by0 ), PT( bx, by1 ), PT( 0, by1 ) },
        { PT( bx, by0 ), PT( w, 0 ), PT( w, h ) },
        { PT( bx, by0 ), PT( w, h ), PT( bx, by1 ) },
    };
#undef PT
    pixman_add_triangles( mask, 0, 0, LENGTH( tris ), tris );
    return mask;
}

static uint32_t pulsewidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    pulsesnap_t* s = w->userdata;
    if (!s) return 0;
    int l, r, h = (int)w->bar->height;
    widget_padding( w, &l, &r );
    awl_host->text( pix, w->bar->font, x, -2, w->width, h, 0, "", s->text.fg, s->text.bg );
    const int sx = (int)x + l, sy = s->sy;
    pixman_image_t* mask = s->sw > 0 && s->sh > 0 ? speaker_mask( s->sw, s->sh ) : NULL;
    if (mask) {
        pixman_color_t dim = s->speaker;
        dim.red /= 3, dim.green /= 3, dim.blue /= 3, dim.alpha /= 3;
        const int filled = (int)lroundf( s->level * (float)s->sh );
        pixman_image_t* solid = pixman_image_create_solid_fill( &dim );
        pixman_image_composite32( PIXMAN_OP_OVER, solid, mask, pix, 0, 0, 0, 0, sx, sy, s->sw, s->sh - filled );
        pixman_image_unref( solid );
        solid = pixman_image_create_solid_fill( &s->speaker );
        pixman_image_composite32( PIXMAN_OP_OVER, solid, mask, pix, 0, 0, 0, s->sh - filled, sx, sy + s->sh - filled,
                                  s->sw, filled );
        pixman_image_unref( solid );
        pixman_image_unref( mask );
    }
    const long room = (long)w->width - l - r - s->sw - s->gap;
    if (room > 0)
        awl_host->text( pix, w->bar->font, sx + s->sw + s->gap, -2, (unsigned)room, h, 0, s->text.text, s->text.fg,
                        s->text.bg );
    return w->width;
}

typedef struct {
    pixman_box32_t fg[AWL_STATS_MAX], bg[AWL_STATS_MAX], io[AWL_STATS_MAX]; // one graph at a time
    int nswp; // the swap samples measure() made room for: 0 without swap
} statuswidget_userdata_t;

static uint32_t statuswidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->stats) return 0;

    if (!w->userdata)
        w->userdata = calloc(1, sizeof(statuswidget_userdata_t));
    awl_stats_t* st = P->stats;
    statuswidget_userdata_t* u = w->userdata;
    u->nswp = atomic_load( &st->has_swap ) ? st->nswp : 0;

    // one column per sample, accumulated the same way draw() places them
    float xx = 0;
    int n = st->ncpu + st->nmem + u->nswp;
    for (int i=0; i<n; ++i) xx += w->bar->scale;
    return cell_width( w, xx );
}

static uint32_t statuswidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->stats) return 0;
    statuswidget_userdata_t* u = w->userdata;
    awl_stats_t* st = P->stats;

    const int n[3] = { st->ncpu, st->nmem, u->nswp };
    _Atomic float* graph[3] = { st->cpu, st->mem, st->swp };
    const pixman_color_t* fg[3] = { &P->colors.cpu, &P->colors.mem,
                                    &P->colors.swap };

    unsigned samples = atomic_load( &st->samples );
    int bar_height = w->bar->height;
    cell_pads( w, pix, x, w->width, P->colors.bar_bg );
    // the graphs between the paddings; with a negative one, the columns
    // outside the bar are left out (pixman doesn't clip fills)
    int l, r, x0, pix_w = pixman_image_get_width( pix );
    widget_padding( w, &l, &r );
    x0 = (int)x + l;
    float xx = 0;
    for (int g=0; g<3; ++g) {
        int m = 0;
        // newest sample first
        for (int i=0; i<n[g]; ++i) {
            float val = stats_sample( graph[g], n[g], samples, i );
            int ydiv = bar_height - val * bar_height;
            // the CPU graph stacks the time spent waiting for I/O on top
            float io = g == 0 ? stats_sample( st->io, n[g], samples, i ) : 0;
            int yio = bar_height - (val + io < 1 ? val + io : 1) * bar_height;
            float next_x = xx + w->bar->scale;
            int x1 = x0 + (int32_t)xx, x2 = x0 + (int32_t)next_x;
            xx = next_x;
            if (x1 < 0 || x2 > pix_w) continue;
            u->bg[m] = (pixman_box32_t){.x1=x1,.x2=x2,.y1=0, .y2=yio};
            u->io[m] = (pixman_box32_t){.x1=x1,.x2=x2,.y1=yio,.y2=ydiv};
            u->fg[m] = (pixman_box32_t){.x1=x1,.x2=x2,.y1=ydiv,.y2=bar_height};
            m++;
        }
        pixman_image_fill_boxes(PIXMAN_OP_SRC, pix, &P->colors.stats_bg, m, u->bg);
        if (g == 0)
            pixman_image_fill_boxes(PIXMAN_OP_SRC, pix, &P->colors.io, m, u->io);
        pixman_image_fill_boxes(PIXMAN_OP_SRC, pix, fg[g], m, u->fg);
    }

    return w->width;
}

/** the readings measure() saw and their texts' widths, so draw() renders
 * text of the same width */
typedef struct {
    float temps[16];
    uint32_t widths[16];
} tempwidget_userdata_t;

static void tempwidget_text( const awl_temperature_t* T, int i, float t, char* text, size_t size ) {
    // only put the label if the string is set
    if (*T->f_labels[i])
        snprintf( text, size, "%s:%.0f°C", T->f_labels[i], t );
    else
        snprintf( text, size, "%3.0f°C", t );
}

static uint32_t tempwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->temp) return 0;

    if (!w->userdata) w->userdata = calloc(1,sizeof(tempwidget_userdata_t));
    tempwidget_userdata_t* u = w->userdata;

    uint32_t width = 0;
    for (int i=0; i<P->temp->f_ntemps; ++i) {
        u->temps[i] = atomic_load( &P->temp->temps[i] );
        if (u->temps[i] == AWL_TEMP_NONE) continue; // unreadable
        char text[128];
        tempwidget_text( P->temp, i, u->temps[i], text, sizeof(text) );
        u->widths[i] = text_cell_width( w, w->bar->font, text );
        width += u->widths[i];
    }
    return width;
}

static uint32_t tempwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->temp) return 0;
    const awl_temperature_t* T = P->temp;
    tempwidget_userdata_t* u = w->userdata;

    for (int i=0; i<T->f_ntemps; ++i) {
        if (u->temps[i] == AWL_TEMP_NONE) continue;
        char text[128];
        tempwidget_text( T, i, u->temps[i], text, sizeof(text) );
        pixman_color_t fgcolor = color_8bit_to_16bit(
                colormap_color( &awl_plugin_config()->colors.temperature,
                                (u->temps[i] - T->f_t_min[i]) / (T->f_t_max[i] - T->f_t_min[i]) ) );
        cell_text( w, pix, x, u->widths[i], w->bar->font, text, fgcolor, P->colors.bar_bg );
        x += u->widths[i];
    }
    return w->width;
}


/** The battery's color at its charge (0 to 1), :c:type:`awl_plugin_colors_t`'s
 * battery or battery_charging */
static pixman_color_t batwidget_color( float charge, int charging ) {
    const awl_plugin_colors_t* k = &awl_plugin_config()->colors;
    return color_8bit_to_16bit( colormap_color( charging ? &k->battery_charging : &k->battery, charge ) );
}

static uint32_t batwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->bat) return 0;

    int charging = atomic_load( &P->bat->charging );
    float charge = atomic_load( &P->bat->charge );
    if (charging < 0) return 0;

    textsnap_t* s = textsnap( w );
    snprintf( s->text, sizeof(s->text), "%3.0f%%", charge * 100.0 );
    s->fg = batwidget_color( charge, charging );
    s->bg = P->colors.bar_bg;
    return textsnap_width( w );
}

/* Emoji are drawn in the bar's emoji font (awl_bar_t.emojifont, color
 * emoji); without one, the widgets show text instead. Icons from the icon
 * theme (`awl_notify_bar_icon()`) replace either where configured. */

/** the bar's icons' size: the tray's, inside the bar */
static int bar_icon_px( const widget_t* w ) {
    const int h = (int)w->bar->height;
    int px = (int)lroundf( awl_config()->tray.icon_size * w->bar->scale );
    return px > h - 2 ? h - 2 : px;
}

/** ``*slot`` holding a reference of its own to icon (NULL: none), the cache's
 * being valid only until its next call */
static void bar_icon_keep( pixman_image_t** slot, pixman_image_t* icon ) {
    if (icon == *slot) return;
    if (*slot) pixman_image_unref( *slot );
    *slot = icon ? pixman_image_ref( icon ) : NULL;
}

/** draws icon at x, centered in the bar's height: a symbolic one's shape
 * in fg; returns its top */
static int bar_icon_draw( const widget_t* w, pixman_image_t* pix, pixman_image_t* icon, int symbolic, int x,
                          pixman_color_t fg ) {
    const int iw = pixman_image_get_width( icon ), ih = pixman_image_get_height( icon );
    const int iy = ((int)w->bar->height - ih) / 2;
    if (symbolic) {
        pixman_image_t* solid = pixman_image_create_solid_fill( &fg );
        pixman_image_composite32( PIXMAN_OP_OVER, solid, icon, pix, 0, 0, 0, 0, x, iy, iw, ih );
        pixman_image_unref( solid );
    } else {
        pixman_image_composite32( PIXMAN_OP_OVER, icon, NULL, pix, 0, 0, 0, 0, x, iy, iw, ih );
    }
    return iy;
}

/** one of the dock widget's two parts: an icon, else a text; w wide */
typedef struct {
    pixman_image_t* icon;
    int symbolic;
    const char* text;
    uint32_t w;
} dockpart_t;

/** the dock widget's snapshot: its parts and the font their texts are in;
 * lead: what an icon first is moved right, out of the neighbour a
 * negative padding reaches into (it draws over it then) */
typedef struct {
    dockpart_t part[2];
    int lead;
    struct fcft_font* font;
    pixman_color_t fg, bg;
} docksnap_t;

/** part as the config's icon names at px, else glyph (in the emoji font),
 * else text; nothing at all without any */
static void dockpart_set( widget_t* w, dockpart_t* part, const char* names, int px, const char* glyph,
                          const char* text ) {
    docksnap_t* s = w->userdata;
    int symbolic;
    pixman_image_t* icon = names ? awl_notify_bar_icon( names, px, &symbolic ) : NULL;
    bar_icon_keep( &part->icon, icon );
    part->symbolic = icon ? symbolic : 0;
    part->text = icon ? "" : w->bar->emojifont ? glyph : text;
    part->w = icon ? (uint32_t)pixman_image_get_width( icon ) : awl_host->text_width( s->font, part->text );
}

/** names with each "%d" the charge at percent down to its ten, 0 to 100
 * (as GNOME picks Adwaita's battery-level-N icons), in buf; NULL if it
 * doesn't fit */
static const char* dock_level_names( const char* names, float percent, char* buf, size_t n ) {
    if (!names) return NULL;
    int level = (int)(percent / 10.0f) * 10;
    level = level < 0 ? 0 : level > 100 ? 100 : level;
    size_t k = 0;
    for (const char* c = names; *c; c++) {
        int len = 1;
        if (c[0] == '%' && c[1] == 'd') {
            len = snprintf( buf + k, n - k, "%d", level );
            c++;
        } else if (k < n) {
            buf[k] = *c;
        }
        if (len < 0 || (k += (size_t)len) >= n) return NULL;
    }
    buf[k] = 0;
    return buf;
}

/** Two parts, what the machine sits on and what it runs on: 🖥️ when a
 * Thunderbolt dock is attached, 💻 otherwise (neither without a Thunderbolt
 * bus); ⚡ on AC (by the adapter, else by whether the battery charges), 🔋 on
 * the battery, 🪫 below its low. "DOCK" and "AC" without the emoji
 * font; each the config's icon instead (:c:member:`awl_plugin_config_t.battery`'s
 * icons) if it has one, the power's following the charge. */
static uint32_t dockwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->bat) return 0;
    int docked = atomic_load( &P->bat->docked );
    int ac = atomic_load( &P->bat->ac );
    if (ac < 0) ac = atomic_load( &P->bat->charging );
    float percent = atomic_load( &P->bat->charge ) * 100.0f;

    if (!w->userdata) w->userdata = calloc(1, sizeof(docksnap_t));
    docksnap_t* s = w->userdata;
    if (!s) return 0;
    const awl_plugin_config_t* pc = awl_plugin_config();
    s->fg = P->colors.bar_fg;
    s->bg = P->colors.bar_bg;
    s->font = w->bar->emojifont ? w->bar->emojifont : w->bar->font;
    const int px = bar_icon_px( w );
    if (docked < 0) dockpart_set( w, &s->part[0], NULL, px, "", "" );
    else if (docked) dockpart_set( w, &s->part[0], pc->battery.icons.docked, px, "🖥️", "DOCK" );
    else dockpart_set( w, &s->part[0], pc->battery.icons.laptop, px, "💻", "" );
    /* the texts apart */
    const char* ac_text = *s->part[0].text && !s->part[0].icon ? " AC" : "AC";
    char names[256];
    if (ac < 0) dockpart_set( w, &s->part[1], NULL, px, "", "" );
    else if (ac)
        dockpart_set( w, &s->part[1], dock_level_names( pc->battery.icons.ac, percent, names, sizeof names ), px,
                      "⚡", ac_text );
    else if (percent < pc->battery.low)
        dockpart_set( w, &s->part[1], dock_level_names( pc->battery.icons.battery_low, percent, names, sizeof names ),
                      px, "🪫", "" );
    else
        dockpart_set( w, &s->part[1], dock_level_names( pc->battery.icons.battery, percent, names, sizeof names ),
                      px, "🔋", "" );
    const dockpart_t* first = s->part[0].w ? &s->part[0] : &s->part[1];
    int l, r;
    widget_padding( w, &l, &r );
    s->lead = first->icon && l < 0 ? -l : 0;
    const uint32_t content = s->part[0].w + s->part[1].w;
    return content ? cell_width( w, content + (uint32_t)s->lead ) : 0;
}

static uint32_t dockwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    docksnap_t* s = w->userdata;
    if (!s) return 0;
    int l, r, h = (int)w->bar->height;
    widget_padding( w, &l, &r );
    awl_host->text( pix, s->font, x, -2, w->width, h, 0, "", s->fg, s->bg );
    /* with a negative padding over the neighbour, but for an icon */
    int px = (int)x + l + s->lead;
    for (int i = 0; i < 2; i++) {
        const dockpart_t* part = &s->part[i];
        if (part->icon) bar_icon_draw( w, pix, part->icon, part->symbolic, px, s->fg );
        else if (*part->text)
            awl_host->text_over( pix, s->font, px, -2, part->w, h, part->text, color_16bit_to_8bit( s->fg ) );
        px += (int)part->w;
    }
    return w->width;
}

static void dockwidget_free( void* userdata ) {
    docksnap_t* s = userdata;
    if (!s) return;
    for (int i = 0; i < 2; i++)
        if (s->part[i].icon) pixman_image_unref( s->part[i].icon );
    free( s );
}

/** the IP widget: the bar shows the primary address (:c:member:`awl_ipaddr_t.best`:
 * NetworkManager's, else the one the kernel would use for the internet),
 * the popup all of them */
#define IPWIDGET_COLS 3
/** the popup's table: a row per address, the connectivity row */
#define IPWIDGET_ROWS (AWL_IP_MAX + 1)
typedef struct {
    char text[48];
    /** 0xRRGGBBAA */
    uint32_t fg;
} ipwidget_cell_t;

typedef struct {
    ipwidget_cell_t cell[IPWIDGET_ROWS][IPWIDGET_COLS];
    int nrows;
} ipwidget_table_t;

typedef struct {
    textsnap_t snap; // first: textsnap_draw() reads it
    /** the popup (awl_window.h), painted by ipwidget_painter from table */
    awl_window_t* win;
    ipwidget_table_t table;
    /** the table as the painter's measure() last laid it out, physical
     * pixels: column widths, columns, the rows painted above an ellipsis
     * row (if cut), the font's height, the spacing */
    int widths[IPWIDGET_COLS];
    int ncols, shown, cut, fh, colsp, rowsp;
    /** where draw() put the widget, in bar pixels */
    uint32_t x;
} ipwidget_userdata_t;

static void ipwidget_cell( ipwidget_table_t* t, int row, int col, uint32_t fg, const char* text ) {
    ipwidget_cell_t* c = &t->cell[row][col];
    snprintf( c->text, sizeof c->text, "%s", text );
    c->fg = fg;
}

static uint32_t ipwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->ip) return 0;
    if (!w->userdata) w->userdata = calloc(1, sizeof(ipwidget_userdata_t));
    ipwidget_userdata_t* u = w->userdata;
    textsnap_t* s = &u->snap;

    awl_ipaddr_entry_t e[AWL_IP_MAX];
    int best = -1, routed = 0, by_nm = 0, connectivity = -1;
    int n = ip_snapshot( P->ip, e, &best, &routed, &by_nm, &connectivity );

    char host[AWL_IP_MAX][INET_ADDRSTRLEN];
    for (int i=0; i<n; ++i)
        if (!inet_ntop( AF_INET, &(struct in_addr){ .s_addr = e[i].addr }, host[i], sizeof host[i] ))
            strcpy( host[i], "?" );

    // green: online; orange: an address, but no (full) way out; red: none.
    // NetworkManager's connectivity check knows best; without one (not
    // running, or checks off) the kernel's route has to do.
    snprintf( s->text, sizeof s->text, "%s", best >= 0 ? host[best] : "disconnected" );
    int online = connectivity > AWL_NM_CONNECTIVITY_UNKNOWN ? connectivity == AWL_NM_CONNECTIVITY_FULL
                                                            : routed;
    s->fg = best < 0 || !strncmp( host[best], "127.", 4 ) ? P->colors.bad :
            online ? P->colors.good :
            connectivity == AWL_NM_CONNECTIVITY_NONE ? P->colors.bad :
            P->colors.warning;
    s->bg = P->colors.bar_bg;

    // the popup: interface, address, default route metric; the routed one
    // in the bar's color for it, what is missing dimmed
    const uint32_t fg = awl_config()->colors.popup_fg, dim = awl_config()->colors.popup_dim,
                   hi = color_16bit_to_8bit( s->fg );
    ipwidget_table_t t;
    memset( &t, 0, sizeof t );
    for (int i=0; i<n && t.nrows < IPWIDGET_ROWS - 1; ++i) {
        const uint32_t c = i == best && (routed || by_nm) ? hi : fg;
        // interface names are at most 15 plain characters, but no control characters here
        char ifname[sizeof e[i].ifname];
        snprintf( ifname, sizeof ifname, "%s", e[i].ifname );
        for (char* p = ifname; *p; ++p)
            if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = ' ';
        ipwidget_cell( &t, t.nrows, 0, c, ifname );
        ipwidget_cell( &t, t.nrows, 1, c, host[i] );
        if (e[i].metric >= 0) {
            char metric[32];
            snprintf( metric, sizeof metric, "metric %d", e[i].metric );
            ipwidget_cell( &t, t.nrows, 2, c, metric );
        } else {
            ipwidget_cell( &t, t.nrows, 2, dim, "no default route" );
        }
        t.nrows++;
    }
    if (!n) ipwidget_cell( &t, t.nrows++, 0, fg, "no IPv4 address" );
    // NetworkManager's verdict, as a last row
    if (connectivity >= 0) {
        static const char* const words[] = { "unknown", "none", "captive portal", "limited", "full" };
        ipwidget_cell( &t, t.nrows, 0, dim, "connectivity" );
        ipwidget_cell( &t, t.nrows, 1, fg, words[connectivity <= AWL_NM_CONNECTIVITY_FULL ? connectivity : 0] );
        t.nrows++;
    }
    // an open popup follows the addresses and routes
    if (memcmp( &t, &u->table, sizeof t )) {
        u->table = t;
        if (u->win) awl_host->window_redraw( u->win );
    }
    return textsnap_width( w );
}

static uint32_t ipwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    ipwidget_userdata_t* u = w->userdata;
    if (u) u->x = x;
    return textsnap_draw( w, x, pix );
}

/* the popup's painter (awl_window_draw.h): the table laid out like a text
 * window's, columns as wide as their widest cell, rows that do not fit
 * ending in an ellipsis row, the last columns cut to max_w */
static int ipwidget_window_measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h,
                                    int* w, int* h ) {
    ipwidget_userdata_t* u = data;
    const ipwidget_table_t* t = &u->table;
    if (!font || t->nrows < 1) return 0;
    u->fh = font->height;
    u->colsp = (int)lroundf( awl_config()->popup.column_spacing * scale );
    u->rowsp = (int)lroundf( awl_config()->popup.row_spacing * scale );
    const int fit = (max_h + u->rowsp) / (u->fh + u->rowsp);
    if (fit < 1) return 0;
    u->cut = t->nrows > fit;
    u->shown = u->cut ? fit - 1 : t->nrows;

    u->ncols = 0;
    for (int c=0; c<IPWIDGET_COLS; ++c) u->widths[c] = 0;
    for (int r=0; r<u->shown; ++r)
        for (int c=0; c<IPWIDGET_COLS; ++c) {
            if (!*t->cell[r][c].text) continue;
            int cw = (int)awl_host->text_width( font, t->cell[r][c].text );
            if (cw > u->widths[c]) u->widths[c] = cw;
            if (c >= u->ncols) u->ncols = c + 1;
        }
    if (u->cut) {
        int ew = (int)awl_host->text_width( font, AWL_WINDOW_ELLIPSIS );
        if (u->ncols < 1) u->ncols = 1;
        if (ew > u->widths[0]) u->widths[0] = ew;
    }
    if (u->ncols < 1) return 0;

    int width = (u->ncols - 1) * u->colsp;
    for (int c=0; c<u->ncols; ++c) width += u->widths[c];
    // wider than allowed: cut from the last column on
    for (int c=u->ncols-1; c>=0 && width > max_w; --c) {
        int cut = width - max_w < u->widths[c] ? width - max_w : u->widths[c];
        u->widths[c] -= cut;
        width -= cut;
    }
    if (width > max_w) width = max_w;
    const int rows = u->shown + u->cut;
    *w = width;
    *h = rows * u->fh + (rows - 1) * u->rowsp;
    return width > 0;
}

static void ipwidget_window_paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale,
                                   int x0, int y0, int hover ) {
    (void)scale; (void)hover;
    const ipwidget_userdata_t* u = data;
    const ipwidget_table_t* t = &u->table;
    for (int r=0; r<u->shown; ++r) {
        int x = x0, y = y0 + r * (u->fh + u->rowsp);
        for (int c=0; c<u->ncols; ++c) {
            const ipwidget_cell_t* cell = &t->cell[r][c];
            if (*cell->text && u->widths[c] > 0)
                awl_host->text_over( pix, font, x, y, (unsigned)u->widths[c], (unsigned)u->fh, cell->text, cell->fg );
            x += u->widths[c] + u->colsp;
        }
    }
    if (u->cut && u->widths[0] > 0)
        awl_host->text_over( pix, font, x0, y0 + u->shown * (u->fh + u->rowsp), (unsigned)u->widths[0],
                             (unsigned)u->fh, AWL_WINDOW_ELLIPSIS, awl_config()->colors.popup_fg );
}

/* the popup centered under the widget, in logical pixels */
static void ipwidget_hover( widget_t* w ) {
    ipwidget_userdata_t* u = w->userdata;
    if (!u) return;
    if (!u->win) {
        static const awl_window_painter_t painter = {
            .measure = &ipwidget_window_measure,
            .paint = &ipwidget_window_paint,
        };
        w->popup_window = u->win = awl_host->window_create();
        awl_host->window_set_painter( u->win, &painter, u );
    }
    float center = (float)u->x + (float)w->width / 2.0f;
    awl_host->window_show( u->win, w->bar->m, (int)(center / w->bar->scale + 0.5f) );
}

static void ipwidget_leave( widget_t* w ) {
    ipwidget_userdata_t* u = w->userdata;
    if (u && u->win) awl_host->window_hide( u->win );
}

/* with the bar: a monitor going away, or the plugins detaching */
static void ipwidget_free( void* userdata ) {
    ipwidget_userdata_t* u = userdata;
    if (u && u->win) awl_host->window_destroy( u->win );
    free( u );
}

static void tagwidget_scroll( widget_t* w, uint32_t x, int amount ) {
    (void)w;
    (void)x;
    awl_host->actions->cycle_view( &(Arg){.i=amount} );
}
static void tagwidget_click( widget_t* w, uint32_t x, int button ) {
    int t = (double)x / (double)w->width * w->bar->ntags;
    switch (button) {
        case BTN_LEFT: awl_host->actions->view( &(Arg){.ui = (1 << t)} ); break;
        case BTN_RIGHT: awl_host->actions->toggleview( &(Arg){.ui = (1 << t)} ); break;
        case BTN_MIDDLE: awl_host->actions->view( &(Arg){.ui = ~0} ); break;
        default: break;
    }
}

static void layoutwidget_scroll( widget_t* w, uint32_t x, int amount ) {
    (void)w; (void)x; awl_host->actions->cycle_layout( &(Arg){.i=amount} );
}
static void layoutwidget_click( widget_t* w, uint32_t x, int button ) {
    (void)w; (void)x; awl_host->actions->cycle_layout( &(Arg){.i=button==BTN_LEFT?1:-1} );
}

static void taskbarwidget_scroll( widget_t* w, uint32_t x, int amount ) {
    awl_host->actions->focusstack( &(Arg){.i=amount} );
}
static void taskbarwidget_click( widget_t* w, uint32_t x, int button ) {
    if (w->bar->n_tagwindows <= 0) return;
    awl_bar_window_t* windows = w->bar->tagwindows;
    int win_idx = (double)x / (double)w->width * (double)w->bar->n_tagwindows;
    if (win_idx >= w->bar->n_tagwindows || win_idx < 0) return;

    Client* c = windows[win_idx].c;
    // a minimized window comes back, an unfocused one gets the focus, the
    // focused one is minimized; client_set_visible() arranges
    if (!windows[win_idx].visible) {
        awl_host->client_set_visible(c, 1);
        awl_host->focusclient(c, 1);
    } else if (!windows[win_idx].focused) {
        awl_host->focusclient(c, 1);
        awl_host->client_set_visible(c, 1);
    } else {
        awl_host->client_set_visible(c, 0);
    }
}

static void pulsewidget_click( widget_t* w, uint32_t x, int button ) {
    (void)w; (void)x; (void)button;
    const char* const* mixer = awl_plugin_config()->volume.mixer;
    if (mixer && mixer[0]) awl_host->actions->spawn( &(Arg){.v=mixer} );
}
static void pulsewidget_scroll( widget_t* w, uint32_t x, int amount ) {
    (void)w; (void)x;
    awl_volume_adjust( amount < 0 ? 0.025f : -0.025f );
}

/* the calendar centered under the clock, in logical pixels from the
 * monitor's left edge (kept on the monitor, so at the right end it sits
 * against the right edge) */
static int clockwidget_center( widget_t* w ) {
    clockwidget_userdata_t* u = w->userdata;
    float center = (float)(u ? u->x : 0) + (float)w->width / 2.0f;
    return (int)(center / w->bar->scale + 0.5f);
}

static void clockwidget_hover( widget_t* w ) {
    awl_calpopup_show( w, clockwidget_center( w ) );
}

static void clockwidget_leave( widget_t* w ) {
    (void)w;
    awl_calpopup_hide();
}

/* pages the calendar the hover opened; does nothing while it is closed */
static void clockwidget_scroll( widget_t* w, uint32_t x, int amount ) {
    (void)w; (void)x;
    awl_calpopup_scroll( amount );
}

/** how close (per mille) the screen brightness has to come to full or empty
 * for the moon to pass through it and swap sides */
static const int backlight_moon_swap = 100;

/** The screen brightness (per mille) as a moon, new at 0 and full at 1000:
 * waxing (lit on the right) on the way up, waning (lit on the left) on the
 * way down, like the real one. The side only swaps when the brightness comes
 * within backlight_moon_swap of full or empty, so going back and forth in
 * between doesn't flip it. In between two phases, m2 is faded in over m1 by
 * alpha_m2. Shared by all bars. */
typedef struct {
    const char *m1, *m2;
    double alpha_m2;
} backlight_moons_t;
static backlight_moons_t backlight_moons( int permille ) {
    static const char* moons_r[] = { "🌑", "🌒", "🌓", "🌔", "🌕" };
    static const char* moons_l[] = { "🌑", "🌘", "🌗", "🌖", "🌕" };
    static const int n = sizeof(moons_r) / sizeof(*moons_r);
    static int lr_sel = 0;
    static int permille_prev = 0;
    static int dir_prev = 0;

    // every bar measures on every redraw, mostly with the brightness
    // unchanged: only a change has a direction
    permille = permille < 0 ? 0 : permille > 1000 ? 1000 : permille;
    if (permille != permille_prev) {
        int dir = permille > permille_prev;
        if ((abs(permille - 500) - (500 - backlight_moon_swap) > 0) && dir != dir_prev)
            lr_sel = !lr_sel;
        permille_prev = permille;
        dir_prev = dir;
    }
    const char** moons = lr_sel ? moons_l : moons_r;

    // n moons, n-1 steps between them over 0..1000
    double pval = permille * (n - 1) / 1000.;
    int i1 = (int)pval;
    if (i1 > n - 2) i1 = n - 2;
    double alpha_m2 = pval - i1;
    alpha_m2 = alpha_m2 < 0.0 ? 0.0 : alpha_m2 > 1.0 ? 1.0 : alpha_m2;
    return (backlight_moons_t) {
        .m1 = moons[i1],
        .m2 = moons[i1 + 1],
        .alpha_m2 = alpha_m2,
    };
}

/** the gap between the moon and the eyes, in pixels at scale 1 */
static const float backlight_emoji_gap = 4.0f;

/** the backlight widget's snapshot: ``text`` is "BR"/"DK" without the emoji
 * font; with it, moon (empty if the brightness is unknown, a stop sign if
 * it makes no sense) and mode, which ``closed`` draws as closed eyes in its
 * place */
typedef struct {
    textsnap_t text;
    backlight_moons_t moons;
    const char* mode;
    int closed;
    int gap;
} backlightsnap_t;

static uint32_t backlightwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->backlight) return 0;
    if (!w->userdata) w->userdata = calloc(1, sizeof(backlightsnap_t));
    backlightsnap_t* s = w->userdata;
    if (!s) return 0;

    const int enabled = atomic_load( &P->backlight->enabled );
    const int screen = atomic_load( &P->backlight->screen );
    s->text.fg = enabled ? P->colors.bar_fg : P->colors.warning;
    s->text.bg = P->colors.bar_bg;
    struct fcft_font* ef = w->bar->emojifont;
    if (!ef) {
        s->mode = NULL;
        strcpy( s->text.text, enabled ? "BR" : "DK" );
        return textsnap_width( w );
    }

    // the moon if the brightness is known, a stop sign if the daemon's
    // number is nonsense, then open eyes for automatic updates or closed
    // ones (as wide as the open ones) for manual brightness
    float scale = w->bar->scale;
    if (screen >= 0) {
        s->moons = backlight_moons(screen);
    } else if (screen == BACKLIGHT_INVALID) {
        s->moons = (backlight_moons_t){ "🛑", "🛑", 0.0 };
    } else {
        s->moons = (backlight_moons_t){ "", "", 0.0 };
    }
    s->mode = "👀";
    s->closed = !enabled;
    s->gap = *s->moons.m1 ? (int)(backlight_emoji_gap * scale + 0.5f) : 0;
    uint32_t w1 = awl_host->text_width( ef, s->moons.m1 ), w2 = awl_host->text_width( ef, s->moons.m2 );
    return cell_width( w, (w1 > w2 ? w1 : w2) + s->gap + awl_host->text_width( ef, s->mode ) );
}

/** Composites the one-glyph string ``text`` into dst at alpha (0..1), placed
 * as :c:func:`awl_draw_text` would place it with x being the pen position;
 * ADD, so two calls with alphas adding up to one crossfade two glyphs. */
static void backlight_glyph( pixman_image_t* dst, struct fcft_font* font,
        const char* text, int x, int ty, double alpha ) {
    uint32_t cp = 0;
    if (!*text || alpha <= 0.0) return;
    utf8decode( text, &cp );
    const struct fcft_glyph* g = fcft_rasterize_char_utf32( font, cp, FCFT_SUBPIXEL_NONE );
    if (!g || pixman_image_get_format( g->pix ) != PIXMAN_a8r8g8b8) return;
    pixman_color_t a = { .alpha = (uint16_t)(alpha * 0xffff + 0.5) };
    pixman_image_t* mask = pixman_image_create_solid_fill( &a );
    pixman_image_composite32( PIXMAN_OP_ADD, g->pix, mask, dst, 0, 0, 0, 0,
            x + g->x, ty - g->y, g->width, g->height );
    pixman_image_unref( mask );
}

/** The closed eyes for manual brightness: icons/eyes_closed.png, made for
 * Noto Color Emoji's 136x128 bitmaps (the box and palette of its 👀), and
 * decoded on first use. Static, so nothing outlives a reload. */
enum eyes_closed_size {
    /** pixels */
    eyes_closed_w = 136,
    /** pixels */
    eyes_closed_h = 128
};
/** BGRA premultiplied, i.e. PIXMAN_a8r8g8b8 */
static uint32_t eyes_closed_bits[eyes_closed_w * eyes_closed_h];
/** scaled to the last box drawn into, if no larger than the image */
static uint32_t eyes_scaled_bits[eyes_closed_w * eyes_closed_h];
static int eyes_scaled_w, eyes_scaled_h;

/** 1 if eyes_closed_bits holds the image, 0 if it can't be decoded */
static int eyes_closed_decode( void ) {
    static int state = 0; // 0 untried, 1 decoded, -1 failed
    if (state) return state > 0;
    state = -1;
    const char* err = NULL;
    void* work = NULL;
    wuffs_png__decoder* dec = malloc( sizeof__wuffs_png__decoder() );
    if (!dec) { err = "out of memory"; goto out; }
    wuffs_base__status st = wuffs_png__decoder__initialize( dec, sizeof__wuffs_png__decoder(),
            WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED );
    if (!wuffs_base__status__is_ok( &st )) { err = wuffs_base__status__message( &st ); goto out; }

    // the reader doesn't write
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader( (uint8_t*)icons_eyes_closed_png,
            icons_eyes_closed_png_len, true );
    wuffs_base__image_config ic;
    st = wuffs_png__decoder__decode_image_config( dec, &ic, &src );
    if (!wuffs_base__status__is_ok( &st )) { err = wuffs_base__status__message( &st ); goto out; }
    if (wuffs_base__pixel_config__width( &ic.pixcfg ) != eyes_closed_w
            || wuffs_base__pixel_config__height( &ic.pixcfg ) != eyes_closed_h) {
        err = "not 136x128";
        goto out;
    }
    wuffs_base__pixel_config__set( &ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
            WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, eyes_closed_w, eyes_closed_h );
    wuffs_base__pixel_buffer pb;
    st = wuffs_base__pixel_buffer__set_from_slice( &pb, &ic.pixcfg,
            wuffs_base__make_slice_u8( (uint8_t*)eyes_closed_bits, sizeof(eyes_closed_bits) ) );
    if (!wuffs_base__status__is_ok( &st )) { err = wuffs_base__status__message( &st ); goto out; }

    uint64_t wlen = wuffs_png__decoder__workbuf_len( dec ).max_incl;
    work = malloc( wlen ? wlen : 1 );
    if (!work) { err = "out of memory"; goto out; }
    st = wuffs_png__decoder__decode_frame( dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
            wuffs_base__make_slice_u8( work, wlen ), NULL );
    if (!wuffs_base__status__is_ok( &st )) { err = wuffs_base__status__message( &st ); goto out; }
    state = 1;
    awl_vrb_printf( "backlight widget: decoded PNG icons/eyes_closed.png (embedded, %u bytes): %dx%d\n",
                    (unsigned)icons_eyes_closed_png_len, eyes_closed_w, eyes_closed_h );

out:
    if (err) awl_err_printf( "backlight widget: can't decode the closed eyes: %s\n", err );
    free( work );
    free( dec );
    return state > 0;
}

/** Draws the closed eyes into dst where the font's 👀 would go, x being the
 * pen position as in :c:func:`backlight_glyph`; 0 if it can't. Scaled the way fcft
 * scales the font's bitmaps (Lanczos3, its default), so both look alike. */
static int backlight_eyes_closed( pixman_image_t* dst, struct fcft_font* font, int x, int ty ) {
    const struct fcft_glyph* g = fcft_rasterize_char_utf32( font, 0x1F440, FCFT_SUBPIXEL_NONE );
    if (!g || g->width <= 0 || g->height <= 0 || !eyes_closed_decode()) return 0;
    int cache = g->width <= eyes_closed_w && g->height <= eyes_closed_h;
    pixman_image_t* src = NULL;
    if (cache && g->width == eyes_scaled_w && g->height == eyes_scaled_h) {
        src = pixman_image_create_bits( PIXMAN_a8r8g8b8, g->width, g->height,
                eyes_scaled_bits, g->width * 4 );
    } else {
        src = pixman_image_create_bits( PIXMAN_a8r8g8b8, eyes_closed_w, eyes_closed_h,
                eyes_closed_bits, eyes_closed_w * 4 );
        if (!src) return 0;
        double sx = (double)eyes_closed_w / g->width, sy = (double)eyes_closed_h / g->height;
        pixman_transform_t t;
        pixman_transform_init_scale( &t, pixman_double_to_fixed( sx ), pixman_double_to_fixed( sy ) );
        pixman_image_set_transform( src, &t );
        int n = 0;
        pixman_fixed_t* params = pixman_filter_create_separable_convolution( &n,
                pixman_double_to_fixed( sx ), pixman_double_to_fixed( sy ),
                PIXMAN_KERNEL_IMPULSE, PIXMAN_KERNEL_IMPULSE,
                PIXMAN_KERNEL_LANCZOS3, PIXMAN_KERNEL_LANCZOS3, 4, 4 );
        if (params) {
            pixman_image_set_filter( src, PIXMAN_FILTER_SEPARABLE_CONVOLUTION, params, n );
            free( params );
        } else {
            pixman_image_set_filter( src, PIXMAN_FILTER_BILINEAR, NULL, 0 );
        }
        if (cache) {
            // scaled once per size: the filter is wide when shrinking a lot
            pixman_image_t* scaled = pixman_image_create_bits( PIXMAN_a8r8g8b8, g->width,
                    g->height, eyes_scaled_bits, g->width * 4 );
            if (scaled) {
                pixman_image_composite32( PIXMAN_OP_SRC, src, NULL, scaled, 0, 0, 0, 0,
                        0, 0, g->width, g->height );
                eyes_scaled_w = g->width;
                eyes_scaled_h = g->height;
                pixman_image_unref( src );
                src = scaled;
            }
        }
    }
    if (!src) return 0;
    pixman_image_composite32( PIXMAN_OP_OVER, src, NULL, dst, 0, 0, 0, 0,
            x + g->x, ty - g->y, g->width, g->height );
    pixman_image_unref( src );
    return 1;
}

static uint32_t backlightwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    backlightsnap_t* s = w->userdata;
    if (!s) return 0;
    struct fcft_font* font = w->bar->emojifont;
    if (!s->mode || !font) return textsnap_draw( w, x, pix );
    // the cell's background, the moon after the left padding, the gap, then
    // the mode; over the background, so a negative padding overlaps
    int h = w->bar->height, y = -2, l, r;
    widget_padding( w, &l, &r );
    int ty = y + (h - font->height) / 2 + font->ascent;
    uint32_t fg = color_16bit_to_8bit( s->text.fg );
    uint32_t w1 = awl_host->text_width( font, s->moons.m1 ),
             w2 = awl_host->text_width( font, s->moons.m2 ),
             mw = w1 > w2 ? w1 : w2, ew = awl_host->text_width( font, s->mode );
    int mx = (int)x + l, ex = mx + (int)mw + s->gap;
    awl_host->text( pix, font, x, y, w->width, h, 0, "", s->text.fg, s->text.bg );
    double a2 = s->moons.alpha_m2;
    if (a2 <= 0.0 || a2 >= 1.0 || !strcmp( s->moons.m1, s->moons.m2 )) {
        awl_host->text_over( pix, font, mx, y, mw, h, a2 >= 1.0 ? s->moons.m2 : s->moons.m1, fg );
    } else {
        // between two phases: both moons crossfaded in a scratch image (m1
        // at 1-a, m2 at a, added up) laid over the background, so each
        // pixel is the exact mix even where only one of them covers it
        pixman_image_t* tmp = mw ? pixman_image_create_bits( PIXMAN_a8r8g8b8, mw, h, NULL, 0 ) : NULL;
        if (tmp) {
            backlight_glyph( tmp, font, s->moons.m1, 0, ty, 1.0 - a2 );
            backlight_glyph( tmp, font, s->moons.m2, 0, ty, a2 );
            pixman_image_composite32( PIXMAN_OP_OVER, tmp, NULL, pix, 0, 0, 0, 0,
                    mx, 0, mw, h );
            pixman_image_unref( tmp );
        }
    }
    // the closed eyes: the image where 👀 would be
    if (!s->closed || !backlight_eyes_closed( pix, font, ex, ty ))
        awl_host->text_over( pix, font, ex, y, ew, h, s->mode, fg );
    return w->width;
}

static void backlightwidget_click( widget_t* w, uint32_t x, int button ) {
    (void)w; (void)x;
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->backlight) return;
    // both start the daemon if it isn't running
    if (button == BTN_LEFT)
        backlight_update( P->backlight );
    else
        backlight_toggle( P->backlight );
}

static void backlightwidget_scroll( widget_t* w, uint32_t x, int amount ) {
    (void)w; (void)x;
    awl_brightness_adjust( amount < 0 ? 0.025f : -0.025f );
}

/* the notifications: a bell (the config's icon, else in the emoji font,
 * else a letter), crossed out while do-not-disturb, with a dot in the bad color at its top right
 * while any are open; hover: how many are unread (open, not shown again
 * from the history) */
typedef struct {
    const char* bell;
    struct fcft_font* bell_font;
    int open;
    uint32_t bell_w, dot, x;
    /* the bell from the icon theme, a reference of our own; NULL: the
     * glyph */
    pixman_image_t* icon;
    int symbolic;
    /* the hover's popup, NULL until the first hover */
    awl_window_t* tip;
} notifysnap_t;

/* the hover popup's text for the open notifications now */
static void notifywidget_tiptext( notifysnap_t* s ) {
    char text[64];
    const int n = awl_notify_unread();
    if (n) snprintf( text, sizeof(text), "%d unread notification%s", n, n == 1 ? "" : "s" );
    else snprintf( text, sizeof(text), "No unread notifications" );
    awl_host->window_set_text( s->tip, text );
}

static uint32_t notifywidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return 0;
    if (!w->userdata) w->userdata = calloc( 1, sizeof(notifysnap_t) );
    notifysnap_t* s = w->userdata;
    if (!s) return 0;
    const int dnd = awl_notify_dnd();
    s->open = awl_notify_count( NULL ) > 0;
    /* a shown popup follows the count */
    int bx, by, bw, bh;
    if (s->tip && awl_host->window_box( s->tip, &bx, &by, &bw, &bh )) notifywidget_tiptext( s );
    bar_icon_keep( &s->icon, awl_notify_bell( bar_icon_px( w ), dnd, &s->symbolic ) );
    s->bell_font = w->bar->emojifont ? w->bar->emojifont : w->bar->font;
    s->bell = w->bar->emojifont ? (dnd ? "🔕" : "🔔") : (dnd ? "Z" : "N");
    s->bell_w = s->icon ? (uint32_t)pixman_image_get_width( s->icon ) : awl_host->text_width( s->bell_font, s->bell );
    s->dot = (uint32_t)lroundf( 6.0f * w->bar->scale );
    /* the dot sticks out of the bell by half its size, always: the width
     * doesn't change with it */
    return cell_width( w, s->bell_w + s->dot / 2 );
}

static uint32_t notifywidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    notifysnap_t* s = w->userdata;
    if (!P || !s) return 0;
    int h = (int)w->bar->height, l, r;
    widget_padding( w, &l, &r );
    s->x = x;
    awl_host->text( pix, w->bar->font, x, -2, w->width, h, 0, "", P->colors.bar_fg, P->colors.bar_bg );
    /* the dot at the bell's top: the icon's, or the glyph's line, centered
     * as text() centers it */
    float top;
    if (s->icon) {
        top = (float)bar_icon_draw( w, pix, s->icon, s->symbolic, (int)x + l, P->colors.bar_fg );
    } else {
        awl_host->text_over( pix, s->bell_font, (int)x + l, -2, s->bell_w, h, s->bell,
                             color_16bit_to_8bit( P->colors.bar_fg ) );
        top = (float)(h - s->bell_font->height) / 2.0f - 2.0f;
    }
    if (awl_notify_unread()) {
        const float d = (float)s->dot;
        const float dx = (float)x + (float)l + (float)s->bell_w + d / 2.0f - d;
        awl_fill_shape( pix, dx, top > 0 ? top : 0, d, d, d / 2.0f, 0, color_16bit_to_8bit( P->colors.bad ) );
    }
    return w->width;
}

static void notifywidget_click( widget_t* w, uint32_t x, int button ) {
    (void)x;
    notifysnap_t* s = w->userdata;
    if (button == BTN_RIGHT && s) {
        float center = (float)s->x + (float)w->width / 2.0f;
        awl_notify_menu_toggle( w->bar->m, (int)(center / w->bar->scale + 0.5f) );
    } else if (button == BTN_LEFT) {
        awl_notify_set_dnd( !awl_notify_dnd() );
    } else if (button == BTN_MIDDLE) {
        awl_notify_dismiss_all();
    }
}

/* the menu stays open while the pointer is on the widget */
static void notifywidget_view( widget_t* w, int32_t x ) {
    (void)w;
    awl_notify_menu_pointer( x >= 0 );
}

/* the count centered under the widget; not over the menu, which shows it */
static void notifywidget_hover( widget_t* w ) {
    notifysnap_t* s = w->userdata;
    if (!s || awl_notify_menu_is_open()) return;
    if (!s->tip) w->popup_window = s->tip = awl_host->window_create();
    if (!s->tip) return;
    notifywidget_tiptext( s );
    float center = (float)s->x + (float)w->width / 2.0f;
    awl_host->window_show( s->tip, w->bar->m, (int)(center / w->bar->scale + 0.5f) );
}

static void notifywidget_leave( widget_t* w ) {
    notifysnap_t* s = w->userdata;
    if (s && s->tip) awl_host->window_hide( s->tip );
}

/* with the bar: a monitor going away, or the plugins detaching */
static void notifywidget_free( void* userdata ) {
    notifysnap_t* s = userdata;
    if (s && s->tip) awl_host->window_destroy( s->tip );
    if (s && s->icon) pixman_image_unref( s->icon );
    free( s );
}
