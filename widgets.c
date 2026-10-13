/** The bar widgets. Built into libawlplugins.so together with the plugins
 * whose data they show, so a reload swaps both at once; everything they need
 * from awl goes through the host table (awl_plugin_abi.h). */
#include "awl_plugin_abi.h"
#include "plugins.h"
#include "plugins/date.h"
#include "plugins/colors.h"
#include "tray/awl_tray.h"
#include "wuffs/wuffs.h"
#include "icons/eyes_closed.h"

#include <arpa/inet.h>
#include <stdatomic.h>

/* awl.h's TEXTW calls into awl directly */
#undef TEXTW
#define TEXTW(mon, text) (awl_host->font_getwidth((mon)->drw, text) + (mon)->lrpad)

static uint32_t tagwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void tagwidget_scroll( widget_t* w, uint32_t x, int amount );
static void tagwidget_click( widget_t* w, uint32_t x, int button );

static uint32_t layoutwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void layoutwidget_scroll( widget_t* w, uint32_t x, int amount );
static void layoutwidget_click( widget_t* w, uint32_t x, int button );

static uint32_t taskbarwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void taskbarwidget_scroll( widget_t* w, uint32_t x, int amount );
static void taskbarwidget_click( widget_t* w, uint32_t x, int button );

/** draw() of the right-hand widgets that show a single text (see textsnap_t) */
static uint32_t textsnap_draw( widget_t* w, uint32_t x, pixman_image_t* pix );

static uint32_t clockwidget_measure( widget_t* w );
static uint32_t clockwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void clockwidget_click( widget_t* w, uint32_t x, int button );
static void clockwidget_hover( widget_t* w );
static void clockwidget_leave( widget_t* w );
static void clockwidget_scroll( widget_t* w, uint32_t x, int amount );

static uint32_t pulsewidget_measure( widget_t* w );
static void pulsewidget_click( widget_t* w, uint32_t x, int button );
static void pulsewidget_scroll( widget_t* w, uint32_t x, int amount );

static uint32_t systray_measure( widget_t* w );
static uint32_t systray_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static uint32_t statuswidget_measure( widget_t* w );
static uint32_t statuswidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static uint32_t tempwidget_measure( widget_t* w );
static uint32_t tempwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static uint32_t batwidget_measure( widget_t* w );
static uint32_t dockwidget_measure( widget_t* w );
static uint32_t dockwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static uint32_t ipwidget_measure( widget_t* w );
static uint32_t ipwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void ipwidget_click( widget_t* w, uint32_t x, int button );
static void ipwidget_hover( widget_t* w );
static void ipwidget_leave( widget_t* w );

static uint32_t backlightwidget_measure( widget_t* w );
static uint32_t backlightwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix );
static void backlightwidget_click( widget_t* w, uint32_t x, int button );

void awl_widgets_create( awl_draw_t* drw ) {
    drw->widgets_left[drw->n_widgets_left++] = (widget_t){
        .bar = drw,
        .draw = &tagwidget_draw,
        .callback_click = &tagwidget_click,
        .callback_scroll = &tagwidget_scroll,
    };
    drw->widgets_left[drw->n_widgets_left++] = (widget_t){
        .bar = drw,
        .draw = &layoutwidget_draw,
        .callback_click = &layoutwidget_click,
        .callback_scroll = &layoutwidget_scroll,
    };

    drw->center_widget = (widget_t){
        .bar = drw,
        .draw = &taskbarwidget_draw,
        .callback_click = &taskbarwidget_click,
        .callback_scroll = &taskbarwidget_scroll,
    };
    drw->has_center_widget = 1;

    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &clockwidget_measure,
        .draw = &clockwidget_draw,
        .callback_click = &clockwidget_click,
        .callback_hover = &clockwidget_hover,
        .hover_delay_ms = 100,
        .callback_leave = &clockwidget_leave,
        .callback_scroll = &clockwidget_scroll,
        .leave_delay_ms = 20,
        .popup_namespace = "awl-calendar:",
        .free = free,
    };
    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &systray_measure,
        .draw = &systray_draw,
    };
    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &pulsewidget_measure,
        .draw = &textsnap_draw,
        .callback_click = &pulsewidget_click,
        .callback_scroll = &pulsewidget_scroll,
        .free = free,
    };
    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &statuswidget_measure,
        .draw = &statuswidget_draw,
        .free = free,
    };
    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &tempwidget_measure,
        .draw = &tempwidget_draw,
        .free = free,
    };
    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &dockwidget_measure,
        .draw = &dockwidget_draw,
        .free = free,
    };
    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &batwidget_measure,
        .draw = &textsnap_draw,
        .free = free,
    };
    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &ipwidget_measure,
        .draw = &ipwidget_draw,
        .callback_click = &ipwidget_click,
        .callback_hover = &ipwidget_hover,
        .hover_delay_ms = 100,
        .callback_leave = &ipwidget_leave,
        .leave_delay_ms = 20,
        .popup_namespace = "awl-popup:",
        .free = free,
    };
    drw->widgets_right[drw->n_widgets_right++] = (widget_t){
        .bar = drw,
        .measure = &backlightwidget_measure,
        .draw = &backlightwidget_draw,
        .callback_click = &backlightwidget_click,
        .free = free,
    };
}

#define TEXT( width, string, fg, bg ) \
    awl_host->text( w->bar, x, -2, width, w->bar->m->b.height, w->bar->m->lrpad/2, string, \
            fg, bg )

static uint32_t tagwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return 0;
    int width = 0;
    for (int t=0; t<w->bar->ntags; ++t) {
        char num[16] = ""; snprintf( num, sizeof(num)-1, "%i", t+1 );
        int ww = TEXTW(w->bar->m, "0");
        pixman_color_t bg_color = color_8bit_to_16bit(molokai_dark_gray),
                       fg_color = P->awl_colors.fg_lay;
        pixman_color_t bg_add;
        if (w->bar->occ & (1 << t)) {
            bg_add = color_8bit_to_16bit(molokai_orange);
            bg_add.alpha = 0xaaaa;
            bg_color = alpha_blend_16(bg_color, bg_add);
        }
        if (w->bar->urg & (1 << t)) {
            bg_add = color_8bit_to_16bit(molokai_red);
            bg_add.alpha = 0xaaaa;
            bg_color = alpha_blend_16(bg_color, bg_add);
        }
        if (w->bar->sel & (1 << t)) {
            bg_add = color_8bit_to_16bit(molokai_purple);
            bg_add.alpha = 0xaaaa;
            bg_color = alpha_blend_16(bg_color, bg_add);
            // if (w->bar->selmon) fg_color = P->awl_colors.fg_stats_cpu;
        }
        TEXT( ww, num, fg_color, bg_color );
        /* the selected monitor: a bar under the "1", where TEXT() put it */
        if (t == 0 && w->bar->selmon && w->bar->selmon != -1) {
            int hh = w->bar->m->b.height;
            int h = hh / 8;
            if (h < 2) h = 2;
            int w = (ww < hh ? ww : hh)/2;
            if (w < 2) w = 2;
            pixman_color_t c = P->awl_colors.fg_stats_cpu;
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
        TEXT( space, "", P->awl_colors.fg_win, (pixman_color_t){0} );
        return 0;
    }

    // calculate space per window
    uint32_t spaces[n_windows];
    for (int wi=0; wi<n_windows; ++wi)
        spaces[wi] = 0;
    for (uint32_t s=0; s<space; ++s)
        spaces[s%n_windows]++;
    int nospace = 0;
    for (int wi=0; wi<n_windows; ++wi)
        if (spaces[wi] <= 20)
            nospace = 1;

    if (nospace)
        return TEXT( space, "+++", P->awl_colors.fg_win, P->awl_colors.bg_win_urg );

    awl_draw_window_t* windows = w->bar->tagwindows;

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
        pixman_color_t bg = windows[wi].focused ? P->awl_colors.bg_win_act :
                            windows[wi].urgent  ? P->awl_colors.bg_win_urg :
                            windows[wi].visible ? P->awl_colors.bg_win     :
                                                  P->awl_colors.bg_win_min;
        /* frosted: awl blurs what is behind the window list (drawbar()) */
        if (awl_config()->blur_windowlist && !windows[wi].focused && !windows[wi].urgent)
            bg.alpha = 0x8888;
        TEXT( spaces[wi], txt, P->awl_colors.fg_win, bg );
        x += spaces[wi];
    }
    return w->bar->center_widget_space;
}

static uint32_t layoutwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return 0;
    int ww = TEXTW(w->bar->m, "XXX");
    TEXT( ww, w->bar->m->ltsymbol, P->awl_colors.fg_lay, P->awl_colors.bg_lay );
    /* each tag has its own layout: a red frame around the symbol */
    if (w->bar->pertag) {
        int hh = w->bar->m->b.height;
        int t = hh / 8;
        if (t < 1) t = 1;
        pixman_color_t c = color_8bit_to_16bit( molokai_red );
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
 * `textsnap_draw()` renders exactly that, so width and pixels always agree. */
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
    return s && *s->text ? TEXTW( w->bar->m, s->text ) : 0;
}

static uint32_t textsnap_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    textsnap_t* s = w->userdata;
    if (!s) return 0;
    TEXT( w->width, s->text, s->fg, s->bg );
    return w->width;
}

typedef struct {
    char timestr[16];
    int sec; // -1: unknown
} clockwidget_userdata_t;

/** how far the seconds meter moves left into the time's right padding */
static const float clock_meter_pad = 2.5f;

static uint32_t clockwidget_measure( widget_t* w ) {
    if (!w->userdata) w->userdata = calloc(1, sizeof(clockwidget_userdata_t));
    clockwidget_userdata_t* u = w->userdata;
    awl_plugin_data_t* P = awl_plugin_get();
    int secs = P && P->date ? atomic_load( &P->date->secs ) : -1;
    if (secs < 0) {
        strcpy(u->timestr, "--:--");
        u->sec = -1;
    } else {
        snprintf(u->timestr, sizeof(u->timestr), "%02d:%02d", secs / 3600, secs / 60 % 60);
        u->sec = secs % 60;
    }
    // time text plus the seconds meter, which eats into the text's padding
    int mw = 3 * w->bar->m->wlr_output->scale + 0.5f;
    if (mw < 2) mw = 2;
    return TEXTW( w->bar->m, "--:--" ) + mw - (int)(clock_meter_pad * w->bar->m->wlr_output->scale + 0.5f);
}

static uint32_t clockwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P) return 0;
    clockwidget_userdata_t* u = w->userdata;

    int mw = 3 * w->bar->m->wlr_output->scale + 0.5f;
    if (mw < 2) mw = 2;
    int ww = w->width - mw;
    TEXT( ww, u->timestr, P->awl_colors.fg_lay, P->awl_colors.bg_lay );

    // seconds meter right of the time: empty at :00, full at :59
    int sec = u->sec;
    int bar_height = w->bar->m->b.height;
    int ydiv = sec < 0 ? bar_height : bar_height - (sec * bar_height + 29) / 59;
    pixman_box32_t bg = { .x1 = x+ww, .x2 = x+ww+mw, .y1 = 0, .y2 = ydiv },
                   fg = { .x1 = x+ww, .x2 = x+ww+mw, .y1 = ydiv, .y2 = bar_height };
    pixman_image_fill_boxes( PIXMAN_OP_SRC, pix, &P->awl_colors.bg_lay, 1, &bg );
    pixman_color_t fgcolor = color_8bit_to_16bit( molokai_green );
    pixman_image_fill_boxes( PIXMAN_OP_SRC, pix, &fgcolor, 1, &fg );
    return w->width;
}

static uint32_t systray_measure( widget_t* w ) {
    return awl_host->tray_width( w->bar->m->wlr_output->name );
}

static uint32_t systray_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    // the tray is a separate window; put it where the bar left room, and
    // paint the slot in its color for the sub-pixel it doesn't cover
    awl_host->tray_set_widget_x( w->bar->m->wlr_output->name, x );
    pixman_color_t bg = color_8bit_to_16bit( awl_config()->tray->bg );
    pixman_image_fill_boxes( PIXMAN_OP_SRC, pix, &bg, 1, &(pixman_box32_t){
        .x1 = x, .x2 = x + w->width, .y1 = 0, .y2 = w->bar->m->b.height } );
    return w->width;
}

static uint32_t pulsewidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->pulse) return 0;
    textsnap_t* s = textsnap( w );
    float val = atomic_load( &P->pulse->value );
    int muted = atomic_load( &P->pulse->muted );
    snprintf( s->text, sizeof(s->text), "♫%3.0f%%", val * 100.0f );
    s->fg = muted ? color_8bit_to_16bit(molokai_orange) :
            lround(val*100.0) > 100 ? color_8bit_to_16bit(molokai_red) :
                                      P->awl_colors.fg_lay;
    s->bg = P->awl_colors.bg_lay;
    // fixed width so the bar doesn't jitter while the volume changes
    return TEXTW( w->bar->m, "V___%" );
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
    for (int i=0; i<n; ++i) xx += w->bar->m->wlr_output->scale;
    return xx;
}

static uint32_t statuswidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->stats) return 0;
    statuswidget_userdata_t* u = w->userdata;
    awl_stats_t* st = P->stats;

    const int n[3] = { st->ncpu, st->nmem, u->nswp };
    _Atomic float* graph[3] = { st->cpu, st->mem, st->swp };
    const pixman_color_t* fg[3] = { &P->awl_colors.fg_stats_cpu, &P->awl_colors.fg_stats_mem,
                                    &P->awl_colors.fg_stats_swp };

    unsigned samples = atomic_load( &st->samples );
    int bar_height = w->bar->m->b.height;
    float xx = 0;
    for (int g=0; g<3; ++g) {
        // newest sample first
        for (int i=0; i<n[g]; ++i) {
            float val = stats_sample( graph[g], n[g], samples, i );
            int ydiv = bar_height - val * bar_height;
            // the CPU graph stacks the time spent waiting for I/O on top
            float io = g == 0 ? stats_sample( st->io, n[g], samples, i ) : 0;
            int yio = bar_height - (val + io < 1 ? val + io : 1) * bar_height;
            float next_x = xx + w->bar->m->wlr_output->scale;
            u->bg[i] = (pixman_box32_t){.x1=x+(int32_t)xx,.x2=x+(int32_t)next_x,.y1=0, .y2=yio};
            u->io[i] = (pixman_box32_t){.x1=x+(int32_t)xx,.x2=x+(int32_t)next_x,.y1=yio,.y2=ydiv};
            u->fg[i] = (pixman_box32_t){.x1=x+(int32_t)xx,.x2=x+(int32_t)next_x,.y1=ydiv,.y2=bar_height};
            xx = next_x;
        }
        pixman_image_fill_boxes(PIXMAN_OP_SRC, pix, &P->awl_colors.bg_stats, n[g], u->bg);
        if (g == 0)
            pixman_image_fill_boxes(PIXMAN_OP_SRC, pix, &P->awl_colors.fg_stats_io, n[g], u->io);
        pixman_image_fill_boxes(PIXMAN_OP_SRC, pix, fg[g], n[g], u->fg);
    }

    return w->width;
}

/** the readings measure() saw, so draw() renders text of the same width */
typedef struct {
    float temps[16];
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
        width += TEXTW(w->bar->m, text);
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
                P->temp_color( u->temps[i], T->f_t_min[i], T->f_t_max[i] ) );
        uint32_t ww = TEXTW(w->bar->m, text);
        TEXT( ww, text, fgcolor, P->awl_colors.bg_status );
        x += ww;
    }
    return w->width;
}

static uint32_t batwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->bat) return 0;

    int charging = atomic_load( &P->bat->charging );
    float charge = atomic_load( &P->bat->charge );
    if (charging < 0) return 0;

    textsnap_t* s = textsnap( w );
    snprintf( s->text, sizeof(s->text), "%3.0f%%", charge * 100.0 );
    s->fg = P->awl_colors.fg_status;
    if (charge < 0.3)   s->fg = color_8bit_to_16bit( molokai_orange );
    if (charge < 0.15)  s->fg = color_8bit_to_16bit( molokai_red );
    if (charging)       s->fg = color_8bit_to_16bit( molokai_green );
    s->bg = P->awl_colors.bg_status;
    return textsnap_width( w );
}

/** Whether the bar font of w's monitor draws all n code points cps in color
 * (a monochrome fallback doesn't count). */
static int color_emoji( widget_t* w, const uint32_t* cps, size_t n ) {
    struct fcft_font* font = w->bar->m->drw->font;
    if (!font) return 0;
    for (size_t i = 0; i < n; ++i) {
        const struct fcft_glyph* g = fcft_rasterize_char_utf32( font, cps[i], FCFT_SUBPIXEL_DEFAULT );
        if (!g || pixman_image_get_format( g->pix ) != PIXMAN_a8r8g8b8) return 0;
    }
    return 1;
}

/** 🔌 when a Thunderbolt dock is attached, 💻 otherwise; "DOCK" or nothing
 * without color emoji, nothing at all without a Thunderbolt bus. No left
 * padding: it sits right against the battery. */
static uint32_t dockwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->bat) return 0;
    int docked = atomic_load( &P->bat->docked );
    if (docked < 0) return 0;

    static const uint32_t cps[] = { 0x1F50C, 0x1F4BB };
    textsnap_t* s = textsnap( w );
    if (color_emoji( w, cps, sizeof(cps) / sizeof(*cps) ))
        strcpy( s->text, docked ? "🔌" : "💻" );
    else
        strcpy( s->text, docked ? "DOCK" : "" );
    s->fg = P->awl_colors.fg_status;
    s->bg = P->awl_colors.bg_status;
    if (!*s->text) return 0;
    return TEXTW( w->bar->m, s->text ) - w->bar->m->lrpad/2;
}

static uint32_t dockwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    textsnap_t* s = w->userdata;
    if (!s) return 0;
    awl_host->text( w->bar, x, -2, w->width, w->bar->m->b.height, 0, s->text, s->fg, s->bg );
    return w->width;
}

/** the IP widget: the bar shows the primary address (`awl_ipaddr_t.best`:
 * NetworkManager's, else the one the kernel would use for the internet),
 * the popup all of them */
typedef struct {
    textsnap_t snap; // first: textsnap_draw() reads it
    /** the popup's table, see `awl_tray_popup_show()` */
    char popup[1024];
    /** where draw() put the widget, in bar pixels */
    uint32_t x;
} ipwidget_userdata_t;

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
    s->fg = best < 0 || !strncmp( host[best], "127.", 4 ) ? color_8bit_to_16bit(molokai_red) :
            online ? color_8bit_to_16bit(molokai_green) :
            connectivity == AWL_NM_CONNECTIVITY_NONE ? color_8bit_to_16bit(molokai_red) :
            color_8bit_to_16bit(molokai_orange);
    s->bg = P->awl_colors.bg_status;

    // the popup: interface, address, default route metric; the routed one bold
    char popup[sizeof u->popup];
    size_t len = 0;
    *popup = 0;
    for (int i=0; i<n; ++i) {
        char metric[64];
        if (e[i].metric >= 0) snprintf( metric, sizeof metric, "metric %d", e[i].metric );
        else snprintf( metric, sizeof metric, "<span alpha=\"50%%\">no default route</span>" );
        // interface names are at most 15 plain characters, but may hold & or <
        char ifname[6*sizeof e[i].ifname] = "";
        for (size_t k=0, o=0; e[i].ifname[k] && o+6 < sizeof ifname; ++k) {
            const char* esc = e[i].ifname[k] == '&' ? "&amp;" : e[i].ifname[k] == '<' ? "&lt;" :
                              e[i].ifname[k] == '>' ? "&gt;" : NULL;
            if (esc) { strcpy( ifname+o, esc ); o += strlen(esc); }
            else { ifname[o++] = e[i].ifname[k]; ifname[o] = 0; }
        }
        int c = i == best && (routed || by_nm)
            ? snprintf( popup+len, sizeof popup - len, "%s<b>%s</b>\t<b>%s</b>\t<b>%s</b>",
                        len ? "\n" : "", ifname, host[i], metric )
            : snprintf( popup+len, sizeof popup - len, "%s%s\t%s\t%s",
                        len ? "\n" : "", ifname, host[i], metric );
        if (c < 0 || (size_t)c >= sizeof popup - len) break;
        len += (size_t)c;
    }
    if (!n) snprintf( popup, sizeof popup, "no IPv4 address" );
    // NetworkManager's verdict, as a last row
    if (connectivity >= 0) {
        static const char* const words[] = { "unknown", "none", "captive portal", "limited", "full" };
        len = strlen( popup );
        snprintf( popup+len, sizeof popup - len, "\n<span alpha=\"50%%\">connectivity</span>\t%s",
                  words[connectivity <= AWL_NM_CONNECTIVITY_FULL ? connectivity : 0] );
    }
    // an open popup follows the addresses and routes
    if (strcmp( popup, u->popup )) {
        strcpy( u->popup, popup );
        awl_host->popup_update( u->popup );
    }
    return textsnap_width( w );
}

static uint32_t ipwidget_draw( widget_t* w, uint32_t x, pixman_image_t* pix ) {
    ipwidget_userdata_t* u = w->userdata;
    if (u) u->x = x;
    return textsnap_draw( w, x, pix );
}

/* the popup centered under the widget, in logical pixels */
static void ipwidget_popup( widget_t* w, int toggle ) {
    ipwidget_userdata_t* u = w->userdata;
    if (!u) return;
    Monitor* m = w->bar->m;
    float center = (float)u->x + (float)w->width / 2.0f;
    awl_host->popup_show( m->wlr_output->name, (int)(center / m->wlr_output->scale + 0.5f),
                          u->popup, toggle );
}

static void ipwidget_click( widget_t* w, uint32_t x, int button ) {
    (void)x; (void)button;
    ipwidget_popup( w, 1 );
}

static void ipwidget_hover( widget_t* w ) {
    ipwidget_popup( w, 0 );
}

static void ipwidget_leave( widget_t* w ) {
    (void)w;
    awl_host->popup_hide();
}

static void tagwidget_scroll( widget_t* w, uint32_t x, int amount ) {
    (void)w;
    (void)x;
    awl_host->actions->cycle_view( &(Arg){.i=amount} );
}
static void tagwidget_click( widget_t* w, uint32_t x, int button ) {
    int t = (double)x / (double)w->width * NTAGS;
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
    awl_draw_window_t* windows = w->bar->tagwindows;
    int win_idx = (double)x / (double)w->width * (double)w->bar->n_tagwindows;
    if (win_idx >= w->bar->n_tagwindows || win_idx < 0) return;

    if (!windows[win_idx].visible) {
        windows[win_idx].c->isvisible = 1;
        awl_host->focusclient(windows[win_idx].c, 1);
        goto arrange;
    }
    if (!windows[win_idx].focused) {
        awl_host->focusclient(windows[win_idx].c, 1);
        goto arrange;
    }
    windows[win_idx].c->isvisible = 0;
arrange:
    awl_host->arrange(windows[win_idx].c->mon);
}

static void pulsewidget_click( widget_t* w, uint32_t x, int button ) {
    (void)w; (void)x; (void)button;
    awl_host->actions->spawn( &(Arg){.v=(const char*[]){"pavucontrol", NULL}} );
}
static void pulsewidget_scroll( widget_t* w, uint32_t x, int amount ) {
    (void)w; (void)x;
    if (amount < 0) {
        awl_host->actions->spawn( &(Arg){.v=(const char*[]){"pactl", "set-sink-volume", "@DEFAULT_SINK@", "+2.5%", NULL }} );
    } else {
        awl_host->actions->spawn( &(Arg){.v=(const char*[]){"pactl", "set-sink-volume", "@DEFAULT_SINK@", "-2.5%", NULL }} );
    }
}

static void clockwidget_click( widget_t* w, uint32_t x, int button ) {
    (void)x; (void)button;
    awl_host->calendar_toggle( w->bar->m->wlr_output->name );
}

static void clockwidget_hover( widget_t* w ) {
    awl_host->calendar_show( w->bar->m->wlr_output->name );
}

static void clockwidget_leave( widget_t* w ) {
    (void)w;
    awl_host->calendar_hide();
}

/* pages the calendar the hover opened; does nothing while it is closed */
static void clockwidget_scroll( widget_t* w, uint32_t x, int amount ) {
    (void)w; (void)x;
    awl_host->calendar_scroll( amount );
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

/** Whether the bar font of w's monitor draws all of the backlight widget's
 * emoji in color. A monochrome fallback doesn't count: Adwaita Mono has the
 * moons, but gets two of them wrong, and draws them inverted on a dark bar. */
static int backlight_emoji( widget_t* w ) {
    static const uint32_t cps[] = { 0x1F311, 0x1F312, 0x1F313, 0x1F314, 0x1F315,
                                    0x1F316, 0x1F317, 0x1F318, 0x1F440, 0x1F6D1 };
    return color_emoji( w, cps, sizeof(cps) / sizeof(*cps) );
}

/** the gap between the moon and the eyes, in pixels at scale 1 */
static const float backlight_emoji_gap = 4.0f;
/** how much of the right padding (half of lrpad) the emoji leave out, in
 * pixels at scale 1: they fill the whole line height and need less air */
static const float backlight_emoji_rpad = 5.0f;

/** the backlight widget's snapshot: `text` is "BR"/"DK" without color
 * emoji; with them, moon (empty if the brightness is unknown, a stop sign if
 * it makes no sense) and mode, which `closed` draws as closed eyes in its
 * place */
typedef struct {
    textsnap_t text;
    backlight_moons_t moons;
    const char* mode;
    int closed;
    int gap, rpad;
} backlightsnap_t;

static uint32_t backlightwidget_measure( widget_t* w ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!P || !P->backlight) return 0;
    if (!w->userdata) w->userdata = calloc(1, sizeof(backlightsnap_t));
    backlightsnap_t* s = w->userdata;
    if (!s) return 0;

    const int enabled = atomic_load( &P->backlight->enabled );
    const int screen = atomic_load( &P->backlight->screen );
    s->text.fg = enabled ? P->awl_colors.fg_status : color_8bit_to_16bit(molokai_orange);
    s->text.bg = P->awl_colors.bg_status;
    if (!backlight_emoji( w )) {
        s->mode = NULL;
        strcpy( s->text.text, enabled ? "BR" : "DK" );
        return textsnap_width( w );
    }

    // the moon if the brightness is known, a stop sign if the daemon's
    // number is nonsense, then open eyes for automatic updates or closed
    // ones (as wide as the open ones) for manual brightness
    Monitor* m = w->bar->m;
    float scale = m->wlr_output->scale;
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
    s->rpad = (int)(backlight_emoji_rpad * scale + 0.5f);
    if (s->rpad > m->lrpad/2) s->rpad = m->lrpad/2;
    uint32_t w1 = TEXTW( m, s->moons.m1 ), w2 = TEXTW( m, s->moons.m2 );
    return (w1 > w2 ? w1 : w2) + s->gap + TEXTW( m, s->mode ) - m->lrpad - s->rpad;
}

/** Composites the one-glyph string `text` into dst at alpha (0..1), placed
 * as awl_draw_text_color() would place it with x being the pen position;
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
enum { eyes_closed_w = 136, eyes_closed_h = 128 };
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

out:
    if (err) fprintf( stderr, "backlight widget: can't decode the closed eyes: %s\n", err );
    free( work );
    free( dec );
    return state > 0;
}

/** Draws the closed eyes into dst where the font's 👀 would go, x being the
 * pen position as in backlight_glyph(); 0 if it can't. Scaled the way fcft
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
    if (!s->mode) return textsnap_draw( w, x, pix );
    // the moon with the left padding and the gap, then the mode filling the
    // rest; TEXT() pads by lrpad/2 on the left
    Monitor* m = w->bar->m;
    uint32_t w1 = awl_host->font_getwidth( w->bar, s->moons.m1 ),
             w2 = awl_host->font_getwidth( w->bar, s->moons.m2 );
    uint32_t mw = m->lrpad/2 + (w1 > w2 ? w1 : w2) + s->gap;
    if (mw > w->width) mw = w->width;
    struct fcft_font* font = w->bar->font;
    double a2 = s->moons.alpha_m2;
    if (!font || a2 <= 0.0 || a2 >= 1.0 || !strcmp( s->moons.m1, s->moons.m2 )) {
        TEXT( mw, a2 >= 1.0 ? s->moons.m2 : s->moons.m1, s->text.fg, s->text.bg );
    } else {
        // between two phases: the background, then both moons crossfaded in
        // a scratch image (m1 at 1-a, m2 at a, added up) laid over it, so
        // each pixel is the exact mix even where only one of them covers it
        TEXT( mw, "", s->text.fg, s->text.bg );
        int h = m->b.height, y = -2;
        int ty = y + (h - font->height) / 2 + font->ascent;
        pixman_image_t* tmp = mw ? pixman_image_create_bits( PIXMAN_a8r8g8b8, mw, h, NULL, 0 ) : NULL;
        if (tmp) {
            int px = m->lrpad/2;
            backlight_glyph( tmp, font, s->moons.m1, px, ty, 1.0 - a2 );
            backlight_glyph( tmp, font, s->moons.m2, px, ty, a2 );
            pixman_image_composite32( PIXMAN_OP_OVER, tmp, NULL, pix, 0, 0, 0, 0,
                    x, 0, mw, h );
            pixman_image_unref( tmp );
        }
    }
    // the closed eyes: the background, then the image where 👀 would be
    int closed = s->closed && font;
    awl_host->text( w->bar, x + mw, -2, w->width - mw, m->b.height, 0,
            closed ? "" : s->mode, s->text.fg, s->text.bg );
    if (closed) {
        int h = m->b.height, y = -2;
        int ty = y + (h - font->height) / 2 + font->ascent;
        if (!backlight_eyes_closed( pix, font, (int)(x + mw), ty ))
            awl_host->text( w->bar, x + mw, -2, w->width - mw, m->b.height, 0, s->mode,
                    s->text.fg, s->text.bg );
    }
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
