#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_desktop.h"
#include "awl_layer.h"
#include "awl_layout.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_plugin_host.h"
#include "awl_popup.h"

static widget_t *barwidgetin(Monitor *m, double cx, unsigned int *xrel);
static widget_t *barwidgetatx(Monitor *m, double cx, double cy, unsigned int *xrel);
static void updateview(widget_t *w, unsigned int xrel);
static void setleavepending(int pending);
static int leavetimeout(void *data);
static int hovertimeout(void *data);
static void widget_wrap_scroll_callback( widget_t* w, uint32_t xrel, double delta );
static bool bar_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy);
static void buffer_destroy(struct wlr_buffer *wlr_buffer);
static bool buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
        void **data, uint32_t *format, size_t *stride);
static void buffer_end_data_ptr_access(struct wlr_buffer *buffer);
static void baroverflow(Monitor *m, uint32_t over);
static void barview(Monitor *m);
static void drawbar(Monitor *m);
static void redrawidle(void *data);
static Buffer *newbarbuffer(Monitor *m);
static Buffer *barbuffer(Monitor *m);
static int bardamage(const Buffer *buf, const Buffer *prev, pixman_region32_t *damage);
static void updatefont(Monitor *m, struct fcft_font **f, const char *font, int size, const char *what);
static void updatefonts(Monitor *m);

static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .begin_data_ptr_access = buffer_begin_data_ptr_access,
    .end_data_ptr_access = buffer_end_data_ptr_access
};

/** What the fonts were set up with, copied, to tell whether a reload
 * changed them */
static struct {
    char *font;
    int fontsize;
    char *emojifont;
    char *popupfont; /* cfg->popup's */
    int popupfontsize;
    char *desktopfont; /* cfg->desktop's */
    int desktopfontsize;
    /* cfg->bar's, -1 before the first */
    int show, top;
} applied = { .show = -1, .top = -1 };

/** Bar widget hover (:c:member:`widget_t.callback_hover`): hover_widget is the widget
 * under the pointer if it has a hover callback, hover_timer fires its
 * callback hover_delay_ms after the pointer got there. hover_active is the
 * widget whose hover fired and that has a callback_leave; leave_timer fires
 * that leave_delay_ms after the pointer left both it and its popup (at once
 * for a delay of 0). */
static struct wl_event_source *hover_timer, *leave_timer;
static widget_t *hover_widget, *hover_active;
static int leave_pending;
/** the widget with a callback_view the pointer is on (:c:member:`widget_t.callback_view`) */
static widget_t *view_widget;
/** :c:func:`barsdirty`'s redraw, NULL while none is due */
static struct wl_event_source *redraw_idle;

/** the widget of m's bar at layout x cx, if the bar is there; ``*xrel`` (may be
 * NULL) its x there, from the widget's left edge in buffer pixels. The one
 * place that knows how the bar is laid out: hovering, scrolling and clicks
 * all find their widget here. */
static widget_t *
barwidgetin(Monitor *m, double cx, unsigned int *xrel)
{
    unsigned int cursor_x, xpos = 0;
    awl_bar_t *d = &m->bar;
    int i;

    if (!d->m || cx < m->m.x)
        return NULL;
    cursor_x = (unsigned int)((cx - m->m.x) * m->wlr_output->scale);
#define AT(w) do { if (xrel) *xrel = cursor_x - xpos; return (w); } while (0)
    for (i = 0; i < d->n_widgets_left; ++i) {
        if (cursor_x >= xpos && cursor_x < xpos + d->widgets_left[i].width)
            AT(&d->widgets_left[i]);
        xpos += d->widgets_left[i].width;
    }
    xpos = d->center_widget_start;
    if (cursor_x >= xpos && cursor_x < xpos + d->center_widget_space)
        AT(d->has_center_widget ? &d->center_widget : NULL);
    xpos += d->center_widget_space;
    for (i = d->n_widgets_right - 1; i >= 0; --i) {
        if (cursor_x >= xpos && cursor_x < xpos + d->widgets_right[i].width)
            AT(&d->widgets_right[i]);
        xpos += d->widgets_right[i].width;
    }
#undef AT
    return NULL;
}

/** the bar widget at cx, cy, if m's bar is what is there; ``*xrel`` as for
 * barwidgetin() */
static widget_t *
barwidgetatx(Monitor *m, double cx, double cy, unsigned int *xrel)
{
    struct wlr_scene_node *node;
    struct wlr_scene_buffer *buffer;

    if (!m->bar.m || !(node = wlr_scene_node_at(&layers[LyrBottom]->node, cx, cy, NULL, NULL))
            || !(buffer = wlr_scene_buffer_from_node(node)) || buffer != m->scene_buffer)
        return NULL;
    return barwidgetin(m, cx, xrel);
}

/** the pointer is on w at xrel (NULL: on no widget): callback_view tells
 * the widget it left -1 first */
static void
updateview(widget_t *w, unsigned int xrel)
{
    widget_t *left = view_widget;

    view_widget = w && w->callback_view ? w : NULL;
    if (left && left != view_widget)
        left->callback_view(left, -1);
    if (view_widget)
        view_widget->callback_view(view_widget, (int32_t)xrel);
}

static int leavetimeout(void *data);

/** a leave_delay_ms of 0 calls callback_leave right away */
static void
setleavepending(int pending)
{
    if (pending == leave_pending)
        return;
    if (pending && !hover_active->leave_delay_ms) {
        leavetimeout(NULL);
        return;
    }
    leave_pending = pending;
    wl_event_source_timer_update(leave_timer, pending ? (int)hover_active->leave_delay_ms : 0);
}

static int
leavetimeout(void *data)
{
    widget_t *w = hover_active;

    (void)data;
    hover_active = NULL;
    leave_pending = 0;
    if (w)
        w->callback_leave(w);
    return 0;
}

static int
hovertimeout(void *data)
{
    (void)data;
    /* not while a layer surface or a popup (e.g. a launcher) holds the
     * keyboard */
    if (!hover_widget || locked || exclusive_focus || popupkeyboard())
        return 0;
    if (hover_active && hover_active != hover_widget) {
        setleavepending(0);
        leavetimeout(NULL);
    }
    if (hover_widget->callback_leave)
        hover_active = hover_widget;
    hover_widget->callback_hover(hover_widget);
    return 0;
}

void
updatehover(Client *c)
{
    widget_t *w = NULL;
    Monitor *m;
    unsigned int xrel = 0;

    if (!hover_timer)
        return;
    if (!c && !locked && cursor_mode == CurNormal
            && (m = xytomon(cursor->x, cursor->y)))
        w = barwidgetatx(m, cursor->x, cursor->y, &xrel);
    updateview(w, xrel);
    if (w && !w->callback_hover)
        w = NULL;
    if (hover_active)
        setleavepending(w != hover_active && !pointerinpopup(hover_active->popup_window));
    if (w == hover_widget)
        return;
    hover_widget = w;
    /* a delay of 0 would disarm the timer */
    wl_event_source_timer_update(hover_timer, w ? (w->hover_delay_ms ? (int)w->hover_delay_ms : 1) : 0);
}

static const double SCROLL_LIMIT = 10.0;
static void widget_wrap_scroll_callback( widget_t* w, uint32_t xrel, double delta ) {
    if (w->callback_scroll) {
        w->scroll_amount += delta;
        if (fabs(w->scroll_amount) > SCROLL_LIMIT) {
            (*w->callback_scroll)( w, xrel, (w->scroll_amount > 0) ? 1 : (w->scroll_amount < 0) ? -1 : 0 );
            w->scroll_amount = 0;
        }
    }
}

bool
bar_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    return true;
}

void
buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    Buffer *buf;
    buf = wl_container_of(wlr_buffer, buf, base);
    free(buf);
}

bool
buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
                             void **data, uint32_t *format, size_t *stride)
{
    Buffer *buf;
    buf = wl_container_of(wlr_buffer, buf, base);

    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;

    *data   = buf->data;
    *stride = buf->stride;
    *format = DRM_FORMAT_ARGB8888;

    return true;
}

void
buffer_end_data_ptr_access(struct wlr_buffer *buffer)
{
}

/** Logs it when m's right-hand widgets don't fit beside its left-hand ones
 * (they're then drawn over them, and clicks land up to over pixels right of
 * what's shown), or no longer do so, once per change. */
static void
baroverflow(Monitor *m, uint32_t over)
{
    if (m->bar_over == over)
        return;
    if (over)
        awl_err_printf("bar on %s: the widgets don't fit, %u pixels too wide\n",
                m->wlr_output->name, over);
    else
        awl_log_printf("bar on %s: the widgets fit again\n", m->wlr_output->name);
    m->bar_over = over;
}

/** what m's widgets need of m, into its :c:type:`awl_bar_t` */
static void
barview(Monitor *m)
{
    awl_bar_t *d = &m->bar;

    memcpy(d->ltsymbol, m->ltsymbol, sizeof(d->ltsymbol));
    d->output = m->wlr_output->name;
    d->scale = m->wlr_output->scale;
    d->height = m->bar_area.height;
    d->x = m->m.x;
    d->y = m->m.y;
}

static void
drawbar(Monitor *m)
{
    if (in_cleanupmon) return;
    /* nothing of the bar is visible under the lock screen, but every new
     * buffer still damages the output (and the full-screen locked_bg_blur
     * above it); destroylock() redraws all bars once on unlock */
    if (locked) return;
    if (!m) return;
    if (!m->bar.m) return;
    int x = 0;
    uint32_t occ = 0, urg = 0;
    Client *c;
    Buffer *buf;
    int oneoff;
    pixman_region32_t damage;

    Client* ct;

    memset( m->bar.tagwindows, 0, sizeof(m->bar.tagwindows) );
    m->bar.n_tagwindows = 0;
    barview(m);

    if (!m->showbar)
        return;

    /* fall back to a throwaway buffer if the scene holds both ring slots */
    if ((oneoff = !(buf = barbuffer(m))))
        buf = newbarbuffer(m);
    memset(buf->data, 0, buf->stride * buf->h);

    awl_bar_prepare_drawing(&m->bar, m->bar_area.width, m->bar_area.height, buf->data, buf->stride);

    ct = focustop(m);
    wl_list_for_each(c, &clients, link) {
        if (c->mon != m)
            continue;
        occ |= c->tags;
        if (c->isurgent)
            urg |= c->tags;
        if ( (c->tags & m->tagset[m->seltags]) && (m->bar.n_tagwindows < (int)LENGTH(m->bar.tagwindows)) ) {
            strncpy( m->bar.tagwindows[m->bar.n_tagwindows].name, client_get_title(c),
                    sizeof(m->bar.tagwindows[m->bar.n_tagwindows].name)-1 );
            m->bar.tagwindows[m->bar.n_tagwindows].floating = c->isfloating;
            m->bar.tagwindows[m->bar.n_tagwindows].urgent = c->isurgent;
            m->bar.tagwindows[m->bar.n_tagwindows].fullscreen = c->isfullscreen;
            m->bar.tagwindows[m->bar.n_tagwindows].visible = c->isvisible;
            m->bar.tagwindows[m->bar.n_tagwindows].maximized = c->ismaximized;
            m->bar.tagwindows[m->bar.n_tagwindows].ontop = c->isontop;
            m->bar.tagwindows[m->bar.n_tagwindows].focused = (c == ct);
            m->bar.tagwindows[m->bar.n_tagwindows].c = c;
            m->bar.n_tagwindows++;
        }
    }
    m->bar.occ = occ;
    m->bar.urg = urg;
    m->bar.sel = m->tagset[m->seltags];
    m->bar.ntags = TAGCOUNT;
    m->bar.selmon = (m == selmon);
    if (wl_list_length(&mons) == 1) m->bar.selmon = -1;
    m->bar.pertag = m->pertag;

    for (int ww=0; ww<m->bar.n_widgets_left; ++ww) {
        if (m->bar.widgets_left[ww].draw)
            m->bar.widgets_left[ww].width = m->bar.widgets_left[ww].draw(
                    &m->bar.widgets_left[ww], x, m->bar.pix );
        x += m->bar.widgets_left[ww].width;
    }
    m->bar.center_widget_start = x;

    uint32_t x_end = m->bar_area.width;
    for (int ww=0; ww<m->bar.n_widgets_right; ++ww) {
        widget_t *w = &m->bar.widgets_right[ww];
        /* width first, so the widget lands at its final position right away */
        w->width = w->measure ? w->measure(w) : 0;
        x_end = w->width < x_end ? x_end - w->width : 0;
        if (w->draw && w->width)
            w->draw(w, x_end, m->bar.pix);
    }

    m->bar.center_widget_space = x_end > m->bar.center_widget_start ? x_end - m->bar.center_widget_start : 0;
    baroverflow(m, x_end < m->bar.center_widget_start ? m->bar.center_widget_start - x_end : 0);
    if (m->bar.has_center_widget) {
        if (m->bar.center_widget.draw)
            m->bar.center_widget.width = m->bar.center_widget.draw( &m->bar.center_widget, x, m->bar.pix );
    }

    awl_bar_finish_drawing(&m->bar);
    wlr_scene_buffer_set_dest_size(m->scene_buffer,
        m->bar_area.real_width, m->bar_area.real_height);
    wlr_scene_node_set_position(&m->scene_buffer->node, m->m.x,
        m->m.y + (cfg->bar.top ? 0 : m->m.height - m->bar_area.real_height));
    /* the window list's box, from buffer to logical pixels */
    if (m->bar_area.width) {
        uint32_t cx = m->bar.has_center_widget ? m->bar.center_widget_start : 0,
                 cw = m->bar.has_center_widget ? m->bar.center_widget.width : 0;
        int bx = (int)((uint64_t)cx * m->bar_area.real_width / m->bar_area.width),
            bw = (int)((uint64_t)(cx + cw) * m->bar_area.real_width / m->bar_area.width) - bx;
        wlr_scene_blur_set_size(m->bar_blur, bw, m->bar_area.real_height);
        wlr_scene_node_set_position(&m->bar_blur->node, m->m.x + bx,
            m->m.y + (cfg->bar.top ? 0 : m->m.height - m->bar_area.real_height));
        wlr_scene_node_set_enabled(&m->bar_blur->node, cfg->blur.windowlist && bw > 0);
    }
    /* only hand the scene a new buffer if the pixels actually changed, and
     * only damage the output where they did (usually just the clock and the
     * graphs); buf stays unlocked otherwise and is reused next time */
    pixman_region32_init(&damage);
    if (bardamage(buf, m->bar_shown, &damage)) {
        wlr_scene_buffer_set_buffer_with_damage(m->scene_buffer, &buf->base,
                m->bar_shown ? &damage : NULL);
        /* a one-off buffer is gone once the scene has uploaded it */
        m->bar_shown = oneoff ? NULL : buf;
    }
    pixman_region32_fini(&damage);
    if (oneoff)
        wlr_buffer_drop(&buf->base);
}

Buffer *
buffercreate(int width, int height)
{
    int32_t stride = awl_draw_stride(width);
    Buffer *buf = ecalloc(1, sizeof(Buffer) + (size_t)stride * height);
    buf->stride = stride;
    buf->w = width;
    buf->h = height;
    wlr_buffer_init(&buf->base, &buffer_impl, width, height);
    return buf;
}

Buffer *
newbarbuffer(Monitor *m)
{
    return buffercreate(m->bar_area.width, m->bar_area.height);
}

Buffer *
barbuffer(Monitor *m)
{
    /* The scene locks a buffer from set_buffer until it has copied it into a
     * texture on the next rendered frame, then unlocks it again. Any unlocked
     * slot is therefore free to draw into, except the one shown: drawbar()
     * compares the new pixels with it. The slots are only dropped in
     * cleanupmon(). */
    size_t i;
    Buffer *buf;

    for (i = 0; i < LENGTH(m->bar_bufs); i++) {
        buf = m->bar_bufs[i];
        if (buf && (buf->base.n_locks || buf == m->bar_shown))
            continue;
        if (buf && (buf->w != (size_t)m->bar_area.width || buf->h != (size_t)m->bar_area.height)) {
            wlr_buffer_drop(&buf->base);
            buf = NULL;
        }
        if (!buf)
            buf = m->bar_bufs[i] = newbarbuffer(m);
        return buf;
    }
    return NULL;
}

int
bardamage(const Buffer *buf, const Buffer *prev, pixman_region32_t *damage)
{
    /* Collects the columns of buf that differ from prev, in BARTILE-pixel
     * steps and over the full bar height, into damage; returns whether
     * anything differs. Without a comparable prev, everything does. */
    enum { BARTILE = 32 };
    size_t x, y, x0, run = 0, w = buf->w, h = buf->h, ntiles = (w + BARTILE - 1) / BARTILE;
    uint8_t dirty[ntiles ? ntiles : 1];
    int any = 0;

    if (!prev || prev->w != w || prev->h != h || prev->stride != buf->stride) {
        pixman_region32_union_rect(damage, damage, 0, 0, w, h);
        return 1;
    }
    memset(dirty, 0, ntiles);
    for (y = 0; y < h; y++) {
        const uint32_t *a = (const uint32_t *)((const char *)buf->data + y * buf->stride);
        const uint32_t *b = (const uint32_t *)((const char *)prev->data + y * prev->stride);
        if (!memcmp(a, b, w * sizeof(*a)))
            continue;
        for (x = 0; x < ntiles; x++) {
            size_t n = x + 1 < ntiles ? BARTILE : w - x * BARTILE;
            if (!dirty[x] && memcmp(a + x * BARTILE, b + x * BARTILE, n * sizeof(*a)))
                dirty[x] = any = 1;
        }
    }
    /* one rectangle per run of changed tiles */
    for (x = 0; x <= ntiles; x++) {
        if (x < ntiles && dirty[x]) {
            run++;
            continue;
        }
        if (run) {
            x0 = (x - run) * BARTILE;
            pixman_region32_union_rect(damage, damage, x0, 0,
                    (x * BARTILE < w ? x * BARTILE : w) - x0, h);
            run = 0;
        }
    }
    return any;
}

void
drawbars(void)
{
    if (in_cleanupmon) return;
    Monitor *m = NULL;

    wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->enabled) drawbar(m);
    }
}

static void
redrawidle(void *data)
{
    (void)data;
    redraw_idle = NULL;
    drawbars();
}

void
barsdirty(void)
{
    /* hover_timer: between barinit() and barfini() */
    if (!redraw_idle && hover_timer)
        redraw_idle = wl_event_loop_add_idle(event_loop, redrawidle, NULL);
}

void togglebar(const Arg *arg) { togglebar_mon(selmon); }
void togglebar_mon(Monitor* m) {
    if (!m) return;
    m->showbar = !m->showbar;
    wlr_scene_node_set_enabled(&m->scene_buffer->node, m->showbar);
    /* drawbar() enables it again, if the window list has a width */
    if (!m->showbar)
        wlr_scene_node_set_enabled(&m->bar_blur->node, 0);
    arrangelayers(m);
}

/** (re)loads ``*f``, a font of m's other than the bar's, at m's scale: font
 * and size (NULL, 0: the bar's, cfg->font and ->fontsize); NULL if both are
 * the bar's, or it doesn't load (what says whose, for the warning) */
static void
updatefont(Monitor *m, struct fcft_font **f, const char *font, int size, const char *what)
{
    const char *name = font ? font : cfg->font;

    awl_font_release(*f);
    *f = NULL;
    if ((!font && size <= 0) || !name)
        return;
    if (!(*f = awl_font_get_exact(name, (float)(size > 0 ? size : cfg->fontsize),
                m->wlr_output->scale)))
        awl_wrn_printf("%s font %s not found, using the bar's\n", what, name);
}

/** (re)loads m's bar's emoji font (:c:member:`awl_bar_t.emojifont`), at the bar's size */
static void
updateemojifont(Monitor *m)
{
    awl_font_release(m->bar.emojifont);
    m->bar.emojifont = NULL;
    if (!cfg->emoji_font)
        return;
    if (!(m->bar.emojifont = awl_font_get_emoji(cfg->emoji_font, (float)cfg->fontsize,
                m->wlr_output->scale)))
        awl_wrn_printf("emoji font %s not found or not in color, the bar shows text instead\n",
                cfg->emoji_font);
}

/** (re)loads m's emoji, popup and desktop fonts (:c:member:`awl_bar_t.emojifont`,
 * :c:member:`Monitor.popupfont`, .desktopfont) */
static void
updatefonts(Monitor *m)
{
    updateemojifont(m);
    updatefont(m, &m->popupfont, cfg->popup.font, cfg->popup.fontsize, "popup");
    updatefont(m, &m->desktopfont, cfg->desktop.font, cfg->desktop.fontsize, "desktop");
}

void
updatebar(Monitor *m)
{
    int rw, rh;

    wlr_output_transformed_resolution(m->wlr_output, &rw, &rh);
    m->bar_area.width = rw;
    m->bar_area.real_width = (int)((float)m->bar_area.width / m->wlr_output->scale);

    if (m->bar_area.scale == m->wlr_output->scale && m->bar.m) {
        barview(m);
        return;
    }

    /* falls back to monospace; keeps the old font if even that fails */
    awl_bar_set_font(&m->bar, cfg->font, (float)cfg->fontsize, m->wlr_output->scale, 1);

    m->bar_area.scale = m->wlr_output->scale;
    /* no font at all: an empty bar of a monospace:size=10's height */
    m->bar.lrpad = m->bar.font ? m->bar.font->height
            : (int)lroundf(AWL_FONT_FALLBACK_SIZE * 2 * m->wlr_output->scale);
    m->bar_area.height = m->bar.lrpad + 2;
    m->bar_area.real_height = (int)((float)m->bar_area.height / m->wlr_output->scale);
    updatefonts(m);
    desktop_fontchanged(m);
    popupsupdate(m);
    barview(m);
}

void
barinit(void)
{
    hover_timer = wl_event_loop_add_timer(event_loop, hovertimeout, NULL);
    leave_timer = wl_event_loop_add_timer(event_loop, leavetimeout, NULL);
}

void
barfini(void)
{
    if (redraw_idle) {
        wl_event_source_remove(redraw_idle);
        redraw_idle = NULL;
    }
    if (hover_timer) {
        wl_event_source_remove(hover_timer);
        hover_timer = NULL;
        hover_widget = NULL;
        view_widget = NULL;
    }
    if (leave_timer) {
        wl_event_source_remove(leave_timer);
        leave_timer = NULL;
        hover_active = NULL;
    }
}

void
baraddmon(Monitor *m)
{
    awl_bar_setup(&m->bar, m);
    awl_plugins_bar_widgets(&m->bar);

    /* LyrBottom sits below LyrTile/LyrFloat, so floating windows dragged
     * over the bar's area render on top of it -- intentional */
    m->scene_buffer = wlr_scene_buffer_create(layers[LyrBottom], NULL);
    m->scene_buffer->point_accepts_input = bar_accepts_input;
    /* frosts the window list's translucent entries; drawbar() fits it and
     * enables it with cfg->blur.windowlist */
    m->bar_blur = wlr_scene_blur_create(layers[LyrBottom], 0, 0);
    wlr_scene_node_place_below(&m->bar_blur->node, &m->scene_buffer->node);
    wlr_scene_blur_set_strength(m->bar_blur, cfg->blur.strength);
    wlr_scene_blur_set_alpha(m->bar_blur, cfg->blur.alpha);

    m->showbar = !!cfg->bar.show;
    updatebar(m);
    barsdirty();
}

void
barremovemon(Monitor *m)
{
    /* hover_widget points into m->bar, as may view_widget */
    if (view_widget && (char *)view_widget >= (char *)&m->bar
            && (char *)view_widget < (char *)(&m->bar + 1))
        view_widget = NULL;
    if (hover_widget && (char *)hover_widget >= (char *)&m->bar
            && (char *)hover_widget < (char *)(&m->bar + 1)) {
        hover_widget = NULL;
        wl_event_source_timer_update(hover_timer, 0);
    }
    if (hover_active && (char *)hover_active >= (char *)&m->bar
            && (char *)hover_active < (char *)(&m->bar + 1)) {
        setleavepending(0);
        hover_active = NULL;
    }
    awl_font_release(m->popupfont);
    m->popupfont = NULL;
    awl_font_release(m->desktopfont);
    m->desktopfont = NULL;
    awl_bar_teardown(&m->bar);
}

void
barwidgetsclear(void)
{
    Monitor *m;

    /* a hover popup gets its leave callback while that still exists */
    if (hover_active) {
        setleavepending(0);
        leavetimeout(NULL);
    }
    hover_widget = NULL;
    if (hover_timer)
        wl_event_source_timer_update(hover_timer, 0);
    /* it hears that the pointer left, while it still can */
    updateview(NULL, 0);
    wl_list_for_each(m, &mons, link)
        if (m->bar.m)
            awl_bar_widgets_clear(&m->bar);
}

void
barwidgetsattach(void)
{
    Monitor *m;

    wl_list_for_each(m, &mons, link)
        if (m->bar.m)
            awl_plugins_bar_widgets(&m->bar);
}

void
barconfigure(void)
{
    Monitor *m;

    /* the bars' font, and with it their height */
    awl_font_retry();
    if (!streqnull(cfg->font, applied.font) || cfg->fontsize != applied.fontsize) {
        free(applied.font);
        applied.font = strdupnull(cfg->font);
        applied.fontsize = cfg->fontsize;
        wl_list_for_each(m, &mons, link) {
            if (!m->bar.m)
                continue;
            m->bar_area.scale = 0; /* updatebar() reloads it */
            updatebar(m);
            arrangelayers(m);
        }
    }
    /* the bars' place, and whether they're shown: every bar follows a
     * change (togglebar's state otherwise stays) */
    if (applied.top >= 0 && !!cfg->bar.top != applied.top)
        wl_list_for_each(m, &mons, link)
            if (m->bar.m) {
                arrangelayers(m);
                popupsupdate(m);
            }
    applied.top = !!cfg->bar.top;
    if (applied.show >= 0 && !!cfg->bar.show != applied.show)
        wl_list_for_each(m, &mons, link)
            if (m->bar.m && m->showbar != !!cfg->bar.show)
                togglebar_mon(m);
    applied.show = !!cfg->bar.show;

    /* the bars' emoji (updatebar() above reloaded them too, no harm) */
    if (!streqnull(cfg->emoji_font, applied.emojifont)) {
        free(applied.emojifont);
        applied.emojifont = strdupnull(cfg->emoji_font);
        wl_list_for_each(m, &mons, link)
            if (m->bar.m)
                updateemojifont(m);
        barsdirty();
    }
    /* the popups' (updatebar() above reloaded it too, no harm) */
    if (!streqnull(cfg->popup.font, applied.popupfont)
            || cfg->popup.fontsize != applied.popupfontsize) {
        free(applied.popupfont);
        applied.popupfont = strdupnull(cfg->popup.font);
        applied.popupfontsize = cfg->popup.fontsize;
        wl_list_for_each(m, &mons, link) {
            if (!m->bar.m || !m->bar.font)
                continue;
            updatefont(m, &m->popupfont, cfg->popup.font, cfg->popup.fontsize, "popup");
            popupsupdate(m);
        }
    }
    /* the desktop file list's, the same way */
    if (!streqnull(cfg->desktop.font, applied.desktopfont)
            || cfg->desktop.fontsize != applied.desktopfontsize) {
        free(applied.desktopfont);
        applied.desktopfont = strdupnull(cfg->desktop.font);
        applied.desktopfontsize = cfg->desktop.fontsize;
        wl_list_for_each(m, &mons, link) {
            if (!m->bar.m || !m->bar.font)
                continue;
            updatefont(m, &m->desktopfont, cfg->desktop.font, cfg->desktop.fontsize, "desktop");
            desktop_fontchanged(m);
        }
    }
    wl_list_for_each(m, &mons, link) {
        wlr_scene_blur_set_strength(m->bar_blur, cfg->blur.strength);
        wlr_scene_blur_set_alpha(m->bar_blur, cfg->blur.alpha);
    }
}

int
pointeronbar(void)
{
    struct wlr_scene_node *node;
    struct wlr_scene_buffer *buffer;

    return (node = wlr_scene_node_at(&layers[LyrBottom]->node, cursor->x, cursor->y, NULL, NULL))
            && (buffer = wlr_scene_buffer_from_node(node)) && buffer == selmon->scene_buffer;
}

int
baraxis(double delta)
{
    unsigned int xrel;
    widget_t *w;

    if (!pointeronbar() || !(w = barwidgetin(selmon, cursor->x, &xrel)))
        return 0;
    widget_wrap_scroll_callback(w, xrel, delta);
    return 1;
}

int
barbutton(uint32_t button, unsigned int *click)
{
    unsigned int xrel = 0;
    widget_t *pressed;

    if (!pointeronbar())
        return 0;
    /* hover popups are hover only: a press on the widget closes what
     * its hover opened, or keeps it from opening, until the pointer
     * leaves and comes back */
    pressed = barwidgetin(selmon, cursor->x, &xrel);
    if (pressed && pressed == hover_widget)
        wl_event_source_timer_update(hover_timer, 0);
    if (hover_active && pressed == hover_active) {
        setleavepending(0);
        leavetimeout(NULL);
    }
    *click = ClkBar;
    if (!pressed || !pressed->callback_click)
        return 0;
    pressed->callback_click(pressed, xrel, button);
    return 1;
}

/* awl_bar_t, awl_bar_type.h */

void awl_bar_setup(awl_bar_t *bar, Monitor *m) {
    *bar = (awl_bar_t){ .m = m };
    /* the widgets come from libawlplugins.so, see awl_plugin_host.h */
}

struct fcft_font *awl_bar_set_font(awl_bar_t *bar, const char *name, float size, float scale,
        int fallback) {
    struct fcft_font *font = fallback ? awl_font_get(name, size, scale)
                                      : awl_font_get_exact(name, size, scale);
    if (!bar || !font) {
        awl_font_release(font);
        return NULL;
    }
    /* the new one first: if it is the same, the old reference keeps it */
    awl_font_release(bar->font);
    bar->font = font;
    return font;
}

void awl_bar_prepare_drawing(awl_bar_t *bar, unsigned int w, unsigned int h, uint32_t *bits, int stride) {
    if (!bar)
        return;
    awl_bar_finish_drawing(bar);
    bar->pix = awl_draw_image(w, h, bits, stride);
}

void awl_bar_finish_drawing(awl_bar_t *bar) {
    if (bar && bar->pix) {
        pixman_image_unref(bar->pix);
        /* awl_bar_teardown() and the next awl_bar_prepare_drawing() unref
         * a bar->pix that is left; a dangling one would be a double unref */
        bar->pix = NULL;
    }
}

void awl_bar_teardown(awl_bar_t *bar) {
    if (!bar)
        return;
    awl_bar_finish_drawing(bar);
    awl_font_release(bar->font);
    awl_font_release(bar->emojifont);
    awl_bar_widgets_clear(bar);
    *bar = (awl_bar_t){ 0 };
}

void awl_bar_widgets_clear(awl_bar_t *bar) {
    for (int l=0; l<bar->n_widgets_left; ++l)
        if (bar->widgets_left[l].free)
            bar->widgets_left[l].free( bar->widgets_left[l].userdata );
    for (int r=0; r<bar->n_widgets_right; ++r)
        if (bar->widgets_right[r].free)
            bar->widgets_right[r].free( bar->widgets_right[r].userdata );

    if (bar->has_center_widget)
        if (bar->center_widget.free)
            bar->center_widget.free( bar->center_widget.userdata );

    bar->n_widgets_left = bar->n_widgets_right = 0;
    bar->has_center_widget = 0;
    bar->center_widget = (widget_t){0};
}
