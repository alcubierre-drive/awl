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

static widget_t *barwidgetatx(Monitor *m, double cx, double cy, unsigned int *xrel);
static widget_t *barwidgetat(Monitor *m, double cx, double cy);
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
    char *popupfont; /* cfg->popup's */
    int popupfontsize;
    char *desktopfont; /* cfg->desktop's */
    int desktopfontsize;
    /* cfg->bar's, -1 before the first */
    int show, top;
} applied = { .show = -1, .top = -1 };

/** Bar widget hover (widget_t.callback_hover): hover_widget is the widget
 * under the pointer if it has a hover callback, hover_timer fires its
 * callback hover_delay_ms after the pointer got there. hover_active is the
 * widget whose hover fired and that has a callback_leave; leave_timer fires
 * that leave_delay_ms after the pointer left both it and its popup (at once
 * for a delay of 0). */
static struct wl_event_source *hover_timer, *leave_timer;
static widget_t *hover_widget, *hover_active;
static int leave_pending;
/** the widget with a callback_view the pointer is on (widget_t.callback_view) */
static widget_t *view_widget;

/** the bar widget at cx, cy; *xrel (may be NULL) its x there, from the
 * widget's left edge in buffer pixels */
static widget_t *
barwidgetatx(Monitor *m, double cx, double cy, unsigned int *xrel)
{
    struct wlr_scene_node *node;
    struct wlr_scene_buffer *buffer;
    unsigned int cursor_x, xpos = 0;
    awl_draw_t *d = &m->drw;
    int i;

    if (!d->m || !(node = wlr_scene_node_at(&layers[LyrBottom]->node, cx, cy, NULL, NULL))
            || !(buffer = wlr_scene_buffer_from_node(node)) || buffer != m->scene_buffer)
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

static widget_t *
barwidgetat(Monitor *m, double cx, double cy)
{
    return barwidgetatx(m, cx, cy, NULL);
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
    /* not while a layer surface (e.g. a launcher) holds the keyboard */
    if (!hover_widget || locked || exclusive_focus)
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
    /* by monitor: a Monitor field would change the plugin ABI */
    static struct { Monitor *m; uint32_t over; } seen[16];
    size_t i, free_slot = LENGTH(seen);

    for (i = 0; i < LENGTH(seen) && seen[i].m != m; i++)
        if (!seen[i].m && free_slot == LENGTH(seen))
            free_slot = i;
    if (i == LENGTH(seen)) {
        if (!over || free_slot == LENGTH(seen))
            return;
        i = free_slot;
        seen[i].m = m;
        seen[i].over = 0;
    }
    if (seen[i].over == over)
        return;
    if (over)
        awl_err_printf("bar on %s: the widgets don't fit, %u pixels too wide\n",
                m->wlr_output->name, over);
    else
        awl_log_printf("bar on %s: the widgets fit again\n", m->wlr_output->name);
    seen[i].over = over;
    if (!over)
        seen[i].m = NULL;
}

void
drawbar(Monitor *m)
{
    if (in_cleanupmon) return;
    /* nothing of the bar is visible under the lock screen, but every new
     * buffer still damages the output (and the full-screen locked_bg_blur
     * above it); destroylock() redraws all bars once on unlock */
    if (locked) return;
    if (!m) return;
    if (!m->drw.m) return;
    int x = 0;
    uint32_t occ = 0, urg = 0;
    Client *c;
    Buffer *buf;
    int oneoff;
    pixman_region32_t damage;

    Client* ct;

    memset( m->drw.tagwindows, 0, sizeof(m->drw.tagwindows) );
    m->drw.n_tagwindows = 0;

    if (!m->showbar)
        return;

    /* fall back to a throwaway buffer if the scene holds both ring slots */
    if ((oneoff = !(buf = barbuffer(m))))
        buf = newbarbuffer(m);
    memset(buf->data, 0, buf->stride * buf->h);

    awl_draw_prepare_drawing(&m->drw, m->b.width, m->b.height, buf->data, buf->stride);

    ct = focustop(m);
    wl_list_for_each(c, &clients, link) {
        if (c->mon != m)
            continue;
        occ |= c->tags;
        if (c->isurgent)
            urg |= c->tags;
        if ( (c->tags & m->tagset[m->seltags]) && (m->drw.n_tagwindows < (int)LENGTH(m->drw.tagwindows)) ) {
            strncpy( m->drw.tagwindows[m->drw.n_tagwindows].name, client_get_title(c),
                    sizeof(m->drw.tagwindows[m->drw.n_tagwindows].name)-1 );
            m->drw.tagwindows[m->drw.n_tagwindows].floating = c->isfloating;
            m->drw.tagwindows[m->drw.n_tagwindows].urgent = c->isurgent;
            m->drw.tagwindows[m->drw.n_tagwindows].fullscreen = c->isfullscreen;
            m->drw.tagwindows[m->drw.n_tagwindows].visible = c->isvisible;
            m->drw.tagwindows[m->drw.n_tagwindows].maximized = c->ismaximized;
            m->drw.tagwindows[m->drw.n_tagwindows].ontop = c->isontop;
            m->drw.tagwindows[m->drw.n_tagwindows].focused = (c == ct);
            m->drw.tagwindows[m->drw.n_tagwindows].c = c;
            m->drw.n_tagwindows++;
        }
    }
    m->drw.occ = occ;
    m->drw.urg = urg;
    m->drw.sel = m->tagset[m->seltags];
    m->drw.ntags = TAGCOUNT;
    m->drw.selmon = (m == selmon);
    if (wl_list_length(&mons) == 1) m->drw.selmon = -1;
    m->drw.pertag = m->pertag;

    for (int ww=0; ww<m->drw.n_widgets_left; ++ww) {
        if (m->drw.widgets_left[ww].draw)
            m->drw.widgets_left[ww].width = m->drw.widgets_left[ww].draw(
                    &m->drw.widgets_left[ww], x, m->drw.pix );
        x += m->drw.widgets_left[ww].width;
    }
    m->drw.center_widget_start = x;

    uint32_t x_end = m->b.width;
    for (int ww=0; ww<m->drw.n_widgets_right; ++ww) {
        widget_t *w = &m->drw.widgets_right[ww];
        /* width first, so the widget lands at its final position right away */
        w->width = w->measure ? w->measure(w) : 0;
        x_end = w->width < x_end ? x_end - w->width : 0;
        if (w->draw && w->width)
            w->draw(w, x_end, m->drw.pix);
    }

    m->drw.center_widget_space = x_end > m->drw.center_widget_start ? x_end - m->drw.center_widget_start : 0;
    baroverflow(m, x_end < m->drw.center_widget_start ? m->drw.center_widget_start - x_end : 0);
    if (m->drw.has_center_widget) {
        if (m->drw.center_widget.draw)
            m->drw.center_widget.width = m->drw.center_widget.draw( &m->drw.center_widget, x, m->drw.pix );
    }

    awl_draw_finish_drawing(&m->drw);
    wlr_scene_buffer_set_dest_size(m->scene_buffer,
        m->b.real_width, m->b.real_height);
    wlr_scene_node_set_position(&m->scene_buffer->node, m->m.x,
        m->m.y + (cfg->bar.top ? 0 : m->m.height - m->b.real_height));
    /* the window list's box, from buffer to logical pixels */
    if (m->b.width) {
        uint32_t cx = m->drw.has_center_widget ? m->drw.center_widget_start : 0,
                 cw = m->drw.has_center_widget ? m->drw.center_widget.width : 0;
        int bx = (int)((uint64_t)cx * m->b.real_width / m->b.width),
            bw = (int)((uint64_t)(cx + cw) * m->b.real_width / m->b.width) - bx;
        wlr_scene_blur_set_size(m->bar_blur, bw, m->b.real_height);
        wlr_scene_node_set_position(&m->bar_blur->node, m->m.x + bx,
            m->m.y + (cfg->bar.top ? 0 : m->m.height - m->b.real_height));
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
    return buffercreate(m->b.width, m->b.height);
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
        if (buf && (buf->w != (size_t)m->b.width || buf->h != (size_t)m->b.height)) {
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

/** (re)loads *f, a font of m's other than the bar's, at m's scale: font
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

/** (re)loads m's popup and desktop fonts (Monitor.popupfont, .desktopfont) */
static void
updatefonts(Monitor *m)
{
    updatefont(m, &m->popupfont, cfg->popup.font, cfg->popup.fontsize, "popup");
    updatefont(m, &m->desktopfont, cfg->desktop.font, cfg->desktop.fontsize, "desktop");
}

void
updatebar(Monitor *m)
{
    int rw, rh;

    wlr_output_transformed_resolution(m->wlr_output, &rw, &rh);
    m->b.width = rw;
    m->b.real_width = (int)((float)m->b.width / m->wlr_output->scale);

    if (m->b.scale == m->wlr_output->scale && m->drw.m)
        return;

    /* falls back to monospace; keeps the old font if even that fails */
    awl_draw_set_font(&m->drw, cfg->font, (float)cfg->fontsize, m->wlr_output->scale, 1);

    m->b.scale = m->wlr_output->scale;
    /* no font at all: an empty bar of a monospace:size=10's height */
    m->lrpad = m->drw.font ? m->drw.font->height
            : (int)lroundf(AWL_FONT_FALLBACK_SIZE * 2 * m->wlr_output->scale);
    m->b.height = m->lrpad + 2;
    m->b.real_height = (int)((float)m->b.height / m->wlr_output->scale);
    updatefonts(m);
    desktop_fontchanged(m);
    popupsupdate(m);
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
    awl_draw_setup(&m->drw, m);
    awl_plugins_bar_widgets(&m->drw);

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
    drawbar(m);
}

void
barremovemon(Monitor *m)
{
    /* hover_widget points into m->drw, as may view_widget */
    if (view_widget && (char *)view_widget >= (char *)&m->drw
            && (char *)view_widget < (char *)(&m->drw + 1))
        view_widget = NULL;
    if (hover_widget && (char *)hover_widget >= (char *)&m->drw
            && (char *)hover_widget < (char *)(&m->drw + 1)) {
        hover_widget = NULL;
        wl_event_source_timer_update(hover_timer, 0);
    }
    if (hover_active && (char *)hover_active >= (char *)&m->drw
            && (char *)hover_active < (char *)(&m->drw + 1)) {
        setleavepending(0);
        hover_active = NULL;
    }
    awl_font_release(m->popupfont);
    m->popupfont = NULL;
    awl_font_release(m->desktopfont);
    m->desktopfont = NULL;
    awl_draw_teardown(&m->drw);
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
        if (m->drw.m)
            awl_draw_widgets_clear(&m->drw);
}

void
barwidgetsattach(void)
{
    Monitor *m;

    wl_list_for_each(m, &mons, link)
        if (m->drw.m)
            awl_plugins_bar_widgets(&m->drw);
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
            if (!m->drw.m)
                continue;
            m->b.scale = 0; /* updatebar() reloads it */
            updatebar(m);
            arrangelayers(m);
        }
    }
    /* the bars' place, and whether they're shown: every bar follows a
     * change (togglebar's state otherwise stays) */
    if (applied.top >= 0 && !!cfg->bar.top != applied.top)
        wl_list_for_each(m, &mons, link)
            if (m->drw.m) {
                arrangelayers(m);
                popupsupdate(m);
            }
    applied.top = !!cfg->bar.top;
    if (applied.show >= 0 && !!cfg->bar.show != applied.show)
        wl_list_for_each(m, &mons, link)
            if (m->drw.m && m->showbar != !!cfg->bar.show)
                togglebar_mon(m);
    applied.show = !!cfg->bar.show;

    /* the popups' (updatebar() above reloaded it too, no harm) */
    if (!streqnull(cfg->popup.font, applied.popupfont)
            || cfg->popup.fontsize != applied.popupfontsize) {
        free(applied.popupfont);
        applied.popupfont = strdupnull(cfg->popup.font);
        applied.popupfontsize = cfg->popup.fontsize;
        wl_list_for_each(m, &mons, link) {
            if (!m->drw.m || !m->drw.font)
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
            if (!m->drw.m || !m->drw.font)
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
    unsigned int cursor_x, xpos = 0;

    if (!pointeronbar())
        return 0;
    cursor_x = cursor->x;
    cursor_x -= selmon->m.x;
    cursor_x *= selmon->wlr_output->scale;
    for (int i=0; i<selmon->drw.n_widgets_left; ++i) {
        if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.widgets_left[i].width) {
            widget_wrap_scroll_callback( &selmon->drw.widgets_left[i], cursor_x - xpos, delta );
            return 1;
        }
        xpos += selmon->drw.widgets_left[i].width;
    }
    xpos = selmon->drw.center_widget_start;
    if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.center_widget_space)
        if (selmon->drw.has_center_widget) {
            widget_wrap_scroll_callback( &selmon->drw.center_widget, cursor_x - xpos, delta );
            return 1;
        }
    xpos += selmon->drw.center_widget_space;
    for (int i=selmon->drw.n_widgets_right-1; i>=0; --i) {
        if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.widgets_right[i].width) {
            widget_wrap_scroll_callback( &selmon->drw.widgets_right[i], cursor_x - xpos, delta );
            return 1;
        }
        xpos += selmon->drw.widgets_right[i].width;
    }
    return 0;
}

int
barbutton(uint32_t button, unsigned int *click)
{
    unsigned int cursor_x, xpos = 0;
    widget_t *pressed;

    if (!pointeronbar())
        return 0;
    /* hover popups are hover only: a press on the widget closes what
     * its hover opened, or keeps it from opening, until the pointer
     * leaves and comes back */
    pressed = barwidgetat(selmon, cursor->x, cursor->y);
    if (pressed && pressed == hover_widget)
        wl_event_source_timer_update(hover_timer, 0);
    if (hover_active && pressed == hover_active) {
        setleavepending(0);
        leavetimeout(NULL);
    }
    cursor_x = cursor->x;
    cursor_x -= selmon->m.x;
    cursor_x *= selmon->wlr_output->scale;
    for (int i=0; i<selmon->drw.n_widgets_left; ++i) {
        if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.widgets_left[i].width) {
            *click = ClkBar;
            if (selmon->drw.widgets_left[i].callback_click) {
                (*selmon->drw.widgets_left[i].callback_click)(&selmon->drw.widgets_left[i],
                        cursor_x - xpos, button);
                return 1;
            }
        }
        xpos += selmon->drw.widgets_left[i].width;
    }
    xpos = selmon->drw.center_widget_start;
    if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.center_widget_space) {
        *click = ClkBar;
        if (selmon->drw.has_center_widget) {
            if (selmon->drw.center_widget.callback_click) {
                (*selmon->drw.center_widget.callback_click)(&selmon->drw.center_widget,
                        cursor_x - xpos, button);
                return 1;
            }
        }
    }
    xpos += selmon->drw.center_widget_space;
    for (int i=selmon->drw.n_widgets_right-1; i>=0; --i) {
        if (cursor_x >= xpos && cursor_x < xpos + selmon->drw.widgets_right[i].width) {
            *click = ClkBar;
            if (selmon->drw.widgets_right[i].callback_click) {
                (*selmon->drw.widgets_right[i].callback_click)(&selmon->drw.widgets_right[i],
                        cursor_x - xpos, button);
                return 1;
            }
        }
        xpos += selmon->drw.widgets_right[i].width;
    }
    return 0;
}
