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

static widget_t *bar_widget_in( Monitor *m, double cx, unsigned int *xrel );
static widget_t *bar_widget_at_x( awl_server_t *srv, Monitor *m, double cx, double cy, unsigned int *xrel );
static void update_view( struct awl_bars *b, widget_t *w, unsigned int xrel );
static void set_leave_pending( struct awl_bars *b, int pending );
static int leave_timeout( void *data );
static int hover_timeout( void *data );
static void widget_wrap_scroll_callback( widget_t* w, uint32_t xrel, double delta, uint32_t mods );
static bool bar_accepts_input( struct wlr_scene_buffer *buffer, double *sx, double *sy );
static void buffer_destroy( struct wlr_buffer *wlr_buffer );
static bool buffer_begin_data_ptr_access( struct wlr_buffer *wlr_buffer, uint32_t flags, void **data, uint32_t *format,
                                          size_t *stride );
static void buffer_end_data_ptr_access( struct wlr_buffer *buffer );
static void bar_overflow( Monitor *m, uint32_t over );
static void bar_view( Monitor *m );
static void overlays( widget_t *w, int n, pixman_image_t *pix );
static void draw_bar( awl_server_t *srv, Monitor *m );
static void redraw_idle( void *data );
static Buffer *new_bar_buffer( Monitor *m );
static Buffer *bar_buffer( Monitor *m );
static int bar_damage( const Buffer *buf, const Buffer *prev, pixman_region32_t *damage );
static void update_font( Monitor *m, struct fcft_font **f, const char *font, int size, const char *what );
static void update_fonts( awl_server_t *srv, Monitor *m );

static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .begin_data_ptr_access = buffer_begin_data_ptr_access,
    .end_data_ptr_access = buffer_end_data_ptr_access
};

/** The bars' state (:c:member:`awl_server_t.bars`), from bar_init() to bar_free() */
struct awl_bars {
    /** What the fonts were set up with, copied, to tell whether a reload
     * changed them */
    struct {
        char *font;
        int font_size;
        char *emoji_font;
        char *popup_font; /* cfg->popup's */
        int popup_font_size;
        char *desktop_font; /* cfg->desktop's */
        int desktop_font_size;
        /* cfg->bar's, -1 before the first */
        int show, top;
    } applied;

    /** Bar widget hover (:c:member:`widget_t.callback_hover`): hover_widget is the widget
     * under the pointer if it has a hover callback, hover_timer fires its
     * callback hover_delay_ms after the pointer got there. hover_active is the
     * widget whose hover fired and that has a callback_leave; leave_timer fires
     * that leave_delay_ms after the pointer left both it and its popup (at once
     * for a delay of 0); clicked: a click opened its popup, not the hover.
     * The timers are there from bar_init() to bar_fini(). */
    struct wl_event_source *hover_timer, *leave_timer;
    widget_t *hover_widget, *hover_active;
    bool leave_pending, clicked;
    /** the widget with a callback_view the pointer is on (:c:member:`widget_t.callback_view`) */
    widget_t *view_widget;
    /** :c:func:`bars_dirty`'s redraw, NULL while none is due */
    struct wl_event_source *redraw_idle;
};

/** the widget of m's bar at layout x cx, if the bar is there; ``*xrel`` (may be
 * NULL) its x there, from the widget's left edge in buffer pixels. The one
 * place that knows how the bar is laid out: hovering, scrolling and clicks
 * all find their widget here. */
static widget_t *bar_widget_in( Monitor *m, double cx, unsigned int *xrel ) {
    unsigned int xpos = 0;
    awl_bar_t *d = &m->bar;

    if (!d->m || cx < m->m.x)
        return NULL;
    unsigned int cursor_x = (unsigned int)((cx - m->m.x) * m->wlr_output->scale);
#define AT(w) do { if (xrel) *xrel = cursor_x - xpos; return (w); } while (0)
    for (int i = 0; i < d->n_widgets_left; ++i)
        if (cursor_x >= (xpos = d->widgets_left[i].x) && cursor_x < xpos + d->widgets_left[i].width)
            AT(&d->widgets_left[i]);
    if (cursor_x >= (xpos = d->center_widget_start) && cursor_x < xpos + d->center_widget_space)
        AT(d->has_center_widget ? &d->center_widget : NULL);
    for (int i = 0; i < d->n_widgets_right; ++i)
        if (cursor_x >= (xpos = d->widgets_right[i].x) && cursor_x < xpos + d->widgets_right[i].width)
            AT(&d->widgets_right[i]);
#undef AT
    return NULL;
}

/** the bar widget at cx, cy, if m's bar is what is there; ``*xrel`` as for
 * bar_widget_in() */
static widget_t *bar_widget_at_x( awl_server_t *srv, Monitor *m, double cx, double cy, unsigned int *xrel ) {
    struct wlr_scene_node *node;
    struct wlr_scene_buffer *buffer;

    if (!m->bar.m || !(node = wlr_scene_node_at(&srv->layers[LyrBottom]->node, cx, cy, NULL, NULL))
            || !(buffer = wlr_scene_buffer_from_node(node)) || buffer != m->scene_buffer)
        return NULL;
    return bar_widget_in(m, cx, xrel);
}

/** the pointer is on w at xrel (NULL: on no widget): callback_view tells
 * the widget it left -1 first */
static void update_view( struct awl_bars *b, widget_t *w, unsigned int xrel ) {
    widget_t *left = b->view_widget;

    b->view_widget = w && w->callback_view ? w : NULL;
    if (left && left != b->view_widget)
        left->callback_view(left, -1);
    if (b->view_widget)
        b->view_widget->callback_view(b->view_widget, (int32_t)xrel);
}

static int leave_timeout( void *data );

/** a leave_delay_ms of 0 calls callback_leave right away */
static void set_leave_pending( struct awl_bars *b, int pending ) {
    if (pending == b->leave_pending)
        return;
    if (pending && !b->hover_active->leave_delay_ms) {
        leave_timeout(b);
        return;
    }
    b->leave_pending = pending;
    wl_event_source_timer_update(b->leave_timer, pending ? (int)b->hover_active->leave_delay_ms : 0);
}

static int leave_timeout( void *data ) {
    struct awl_bars *b = data;
    widget_t *w = b->hover_active;

    b->hover_active = NULL;
    b->leave_pending = 0;
    if (w)
        w->callback_leave(w);
    return 0;
}

static int hover_timeout( void *data ) {
    awl_server_t *srv = data;
    /* not while a layer surface or a popup (e.g. a launcher) holds the
     * keyboard */
    if (!srv->bars->hover_widget || srv->locked || srv->clients.exclusive_focus || popup_keyboard(srv))
        return 0;
    /* its popup is still open */
    if (srv->bars->hover_active == srv->bars->hover_widget)
        return 0;
    if (srv->bars->hover_active) {
        set_leave_pending(srv->bars, 0);
        leave_timeout(srv->bars);
    }
    srv->bars->clicked = 0;
    if (srv->bars->hover_widget->callback_leave)
        srv->bars->hover_active = srv->bars->hover_widget;
    srv->bars->hover_widget->callback_hover(srv->bars->hover_widget);
    return 0;
}

void update_hover( awl_server_t *srv, Client *c ) {
    widget_t *w = NULL;
    Monitor *m;
    unsigned int xrel = 0;

    if (!srv->bars->hover_timer)
        return;
    if (!c && !srv->locked && srv->cursor.mode == CurNormal
            && (m = xy_to_mon(srv, srv->cursor.wlr->x, srv->cursor.wlr->y)))
        w = bar_widget_at_x(srv, m, srv->cursor.wlr->x, srv->cursor.wlr->y, &xrel);
    update_view(srv->bars, w, xrel);
    if (w && !w->callback_hover)
        w = NULL;
    if (srv->bars->hover_active)
        set_leave_pending(srv->bars, w != srv->bars->hover_active && !pointer_in_popup(srv, srv->bars->hover_active->popup_window));
    if (w == srv->bars->hover_widget)
        return;
    srv->bars->hover_widget = w;
    /* a delay of 0 would disarm the timer */
    wl_event_source_timer_update(srv->bars->hover_timer, w ? (w->hover_delay_ms ? (int)w->hover_delay_ms : 1) : 0);
}

static const double SCROLL_LIMIT = 10.0;
static void widget_wrap_scroll_callback( widget_t* w, uint32_t xrel, double delta, uint32_t mods ) {
    if (w->callback_scroll) {
        w->scroll_amount += delta;
        if (fabs(w->scroll_amount) > SCROLL_LIMIT) {
            (*w->callback_scroll)( w, xrel, (w->scroll_amount > 0) ? 1 : (w->scroll_amount < 0) ? -1 : 0, mods );
            w->scroll_amount = 0;
        }
    }
}

bool bar_accepts_input( struct wlr_scene_buffer *buffer, double *sx, double *sy ) {
    (void)buffer; (void)sx; (void)sy;
    return true;
}

void buffer_destroy( struct wlr_buffer *wlr_buffer ) {
    Buffer *buf = wl_container_of(wlr_buffer, buf, base);
    free(buf);
}

bool buffer_begin_data_ptr_access( struct wlr_buffer *wlr_buffer, uint32_t flags, void **data, uint32_t *format,
                                   size_t *stride ) {
    Buffer *buf = wl_container_of(wlr_buffer, buf, base);

    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;

    *data   = buf->data;
    *stride = buf->stride;
    *format = DRM_FORMAT_ARGB8888;

    return true;
}

void buffer_end_data_ptr_access( struct wlr_buffer *buffer ) {
    (void)buffer;
}

/** Logs it when m's right-hand widgets don't fit beside its left-hand ones
 * (they're then drawn over them, and clicks land up to over pixels right of
 * what's shown), or no longer do so, once per change. */
static void bar_overflow( Monitor *m, uint32_t over ) {
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
static void bar_view( Monitor *m ) {
    awl_bar_t *d = &m->bar;

    memcpy(d->layout_symbol, m->layout_symbol, sizeof(d->layout_symbol));
    d->output = m->wlr_output->name;
    d->scale = m->wlr_output->scale;
    d->height = m->bar_area.height;
    d->x = m->m.x;
    d->y = m->m.y;
}

/** the :c:member:`~widget_t.overlay` of each of the n widgets from w that has one and was drawn */
static void overlays( widget_t *w, int n, pixman_image_t *pix ) {
    for (; n-- > 0; ++w)
        if (w->overlay && w->width) w->overlay( w, pix );
}

static void draw_bar( awl_server_t *srv, Monitor *m ) {
    if (srv->out.in_cleanup_mon) return;
    /* nothing of the bar is visible under the lock screen, but every new
     * buffer still damages the output (and the full-screen locked_bg_blur
     * above it); destroy_lock() redraws all bars once on unlock */
    if (srv->locked) return;
    if (!m) return;
    if (!m->bar.m) return;
    int x = 0;
    uint32_t occ = 0, urg = 0;
    Client *c;
    Buffer *buf;
    int one_off;
    pixman_region32_t damage;

    memset( m->bar.tag_windows, 0, sizeof(m->bar.tag_windows) );
    m->bar.n_tag_windows = 0;
    bar_view(m);

    if (!m->show_bar)
        return;

    /* fall back to a throwaway buffer if the scene holds both ring slots */
    if ((one_off = !(buf = bar_buffer(m))))
        buf = new_bar_buffer(m);
    memset(buf->data, 0, buf->stride * buf->h);

    awl_bar_prepare_drawing(&m->bar, m->bar_area.width, m->bar_area.height, buf->data, buf->stride);

    Client* ct = focus_top(srv, m);
    wl_list_for_each(c, &srv->clients.list, link) {
        if (c->mon != m)
            continue;
        occ |= c->tags;
        if (c->is_urgent)
            urg |= c->tags;
        if ( (c->tags & m->tagset[m->sel_tags]) && (m->bar.n_tag_windows < (int)LENGTH(m->bar.tag_windows)) ) {
            strncpy( m->bar.tag_windows[m->bar.n_tag_windows].name, client_get_title(c),
                    sizeof(m->bar.tag_windows[m->bar.n_tag_windows].name)-1 );
            m->bar.tag_windows[m->bar.n_tag_windows].floating = c->is_floating;
            m->bar.tag_windows[m->bar.n_tag_windows].urgent = c->is_urgent;
            m->bar.tag_windows[m->bar.n_tag_windows].fullscreen = c->is_fullscreen;
            m->bar.tag_windows[m->bar.n_tag_windows].visible = c->is_visible;
            m->bar.tag_windows[m->bar.n_tag_windows].maximized = c->is_maximized;
            m->bar.tag_windows[m->bar.n_tag_windows].on_top = c->is_on_top;
            m->bar.tag_windows[m->bar.n_tag_windows].focused = (c == ct);
            m->bar.tag_windows[m->bar.n_tag_windows].c = c;
            m->bar.n_tag_windows++;
        }
    }
    m->bar.occ = occ;
    m->bar.urg = urg;
    m->bar.sel = m->tagset[m->sel_tags];
    m->bar.ntags = TAG_COUNT(srv->cfg);
    m->bar.sel_mon = (m == srv->out.sel_mon);
    if (srv->out.nmons == 1) m->bar.sel_mon = -1;
    m->bar.per_tag = m->per_tag;

    for (int ww=0; ww<m->bar.n_widgets_left; ++ww) {
        widget_t *w = &m->bar.widgets_left[ww];
        w->x = (uint32_t)x;
        if (w->draw) w->width = w->draw(w, w->x, m->bar.pix);
        x += w->width;
    }
    m->bar.center_widget_start = x;

    uint32_t x_end = m->bar_area.width;
    for (int ww=0; ww<m->bar.n_widgets_right; ++ww) {
        widget_t *w = &m->bar.widgets_right[ww];
        /* width first, so the widget lands at its final position right away */
        w->width = w->measure ? w->measure(w) : 0;
        w->x = x_end = w->width < x_end ? x_end - w->width : 0;
        if (w->draw && w->width)
            w->draw(w, x_end, m->bar.pix);
    }

    m->bar.center_widget_space = x_end > m->bar.center_widget_start ? x_end - m->bar.center_widget_start : 0;
    bar_overflow(m, x_end < m->bar.center_widget_start ? m->bar.center_widget_start - x_end : 0);
    if (m->bar.has_center_widget) {
        m->bar.center_widget.x = (uint32_t)x;
        if (m->bar.center_widget.draw)
            m->bar.center_widget.width = m->bar.center_widget.draw( &m->bar.center_widget, x, m->bar.pix );
    }
    /* the overlays, once every widget is drawn */
    overlays( m->bar.widgets_left, m->bar.n_widgets_left, m->bar.pix );
    overlays( m->bar.widgets_right, m->bar.n_widgets_right, m->bar.pix );
    overlays( &m->bar.center_widget, m->bar.has_center_widget, m->bar.pix );

    awl_bar_finish_drawing(&m->bar);
    wlr_scene_buffer_set_dest_size(m->scene_buffer,
        m->bar_area.real_width, m->bar_area.real_height);
    wlr_scene_node_set_position(&m->scene_buffer->node, m->m.x,
        m->m.y + (srv->cfg->bar.top ? 0 : m->m.height - m->bar_area.real_height));
    /* the window list's box, from buffer to logical pixels */
    if (m->bar_area.width) {
        uint32_t cx = m->bar.has_center_widget ? m->bar.center_widget_start : 0,
                 cw = m->bar.has_center_widget ? m->bar.center_widget.width : 0;
        int bx = (int)((uint64_t)cx * m->bar_area.real_width / m->bar_area.width),
            bw = (int)((uint64_t)(cx + cw) * m->bar_area.real_width / m->bar_area.width) - bx;
        wlr_scene_blur_set_size(m->bar_blur, bw, m->bar_area.real_height);
        wlr_scene_node_set_position(&m->bar_blur->node, m->m.x + bx,
            m->m.y + (srv->cfg->bar.top ? 0 : m->m.height - m->bar_area.real_height));
        wlr_scene_node_set_enabled(&m->bar_blur->node, srv->cfg->blur.window_list && bw > 0);
    }
    /* only hand the scene a new buffer if the pixels actually changed, and
     * only damage the output where they did (usually just the clock and the
     * graphs); buf stays unlocked otherwise and is reused next time */
    pixman_region32_init(&damage);
    if (bar_damage(buf, m->bar_shown, &damage)) {
        wlr_scene_buffer_set_buffer_with_damage(m->scene_buffer, &buf->base,
                m->bar_shown ? &damage : NULL);
        /* a one-off buffer is gone once the scene has uploaded it */
        m->bar_shown = one_off ? NULL : buf;
    }
    pixman_region32_fini(&damage);
    if (one_off)
        wlr_buffer_drop(&buf->base);
}

Buffer *buffer_create( int width, int height ) {
    int32_t stride = awl_draw_stride(width);
    Buffer *buf = calloc_or_die(1, sizeof(Buffer) + (size_t)stride * height);
    buf->stride = stride;
    buf->w = width;
    buf->h = height;
    wlr_buffer_init(&buf->base, &buffer_impl, width, height);
    return buf;
}

Buffer *new_bar_buffer( Monitor *m ) {
    return buffer_create(m->bar_area.width, m->bar_area.height);
}

Buffer *bar_buffer( Monitor *m ) {
    /* The scene locks a buffer from set_buffer until it has copied it into a
     * texture on the next rendered frame, then unlocks it again. Any unlocked
     * slot is therefore free to draw into, except the one shown: draw_bar()
     * compares the new pixels with it. The slots are only dropped in
     * cleanup_mon(). */
    for (size_t i = 0; i < LENGTH(m->bar_bufs); i++) {
        Buffer *buf = m->bar_bufs[i];
        if (buf && (buf->base.n_locks || buf == m->bar_shown))
            continue;
        if (buf && (buf->w != (size_t)m->bar_area.width || buf->h != (size_t)m->bar_area.height)) {
            wlr_buffer_drop(&buf->base);
            buf = NULL;
        }
        if (!buf)
            buf = m->bar_bufs[i] = new_bar_buffer(m);
        return buf;
    }
    return NULL;
}

int bar_damage( const Buffer *buf, const Buffer *prev, pixman_region32_t *damage ) {
    /* Collects the columns of buf that differ from prev, in BARTILE-pixel
     * steps and over the full bar height, into damage; returns whether
     * anything differs. Without a comparable prev, everything does. */
    enum { BARTILE = 32 };
    size_t run = 0, w = buf->w, h = buf->h, ntiles = (w + BARTILE - 1) / BARTILE;
    uint8_t dirty[ntiles ? ntiles : 1];
    int any = 0;

    if (!prev || prev->w != w || prev->h != h || prev->stride != buf->stride) {
        pixman_region32_union_rect(damage, damage, 0, 0, w, h);
        return 1;
    }
    memset(dirty, 0, ntiles);
    for (size_t y = 0; y < h; y++) {
        const uint32_t *a = (const uint32_t *)((const char *)buf->data + y * buf->stride);
        const uint32_t *b = (const uint32_t *)((const char *)prev->data + y * prev->stride);
        if (!memcmp(a, b, w * sizeof(*a)))
            continue;
        for (size_t x = 0; x < ntiles; x++) {
            size_t n = x + 1 < ntiles ? BARTILE : w - x * BARTILE;
            if (!dirty[x] && memcmp(a + x * BARTILE, b + x * BARTILE, n * sizeof(*a)))
                dirty[x] = any = 1;
        }
    }
    /* one rectangle per run of changed tiles */
    for (size_t x = 0; x <= ntiles; x++) {
        if (x < ntiles && dirty[x]) {
            run++;
            continue;
        }
        if (run) {
            size_t x0 = (x - run) * BARTILE;
            pixman_region32_union_rect(damage, damage, x0, 0,
                    (x * BARTILE < w ? x * BARTILE : w) - x0, h);
            run = 0;
        }
    }
    return any;
}

void draw_bars( awl_server_t *srv ) {
    if (srv->out.in_cleanup_mon) return;
    Monitor *m = NULL;

    FOR_EACH_MON(m, srv) {
        if (m->wlr_output->enabled) draw_bar(srv, m);
    }
}

static void redraw_idle( void *data ) {
    awl_server_t *srv = data;
    (void)data;
    srv->bars->redraw_idle = NULL;
    draw_bars(srv);
}

void bars_dirty( awl_server_t *srv ) {
    /* hover_timer: between bar_init() and bar_fini() */
    if (!srv->bars->redraw_idle && srv->bars->hover_timer)
        srv->bars->redraw_idle = wl_event_loop_add_idle(srv->event_loop, redraw_idle, srv);
}

void toggle_bar( awl_server_t *srv, const Arg *arg ) { (void)arg; toggle_bar_mon(srv, srv->out.sel_mon); }
void toggle_bar_mon( awl_server_t *srv, Monitor* m ) {
    if (!m) return;
    m->show_bar = !m->show_bar;
    wlr_scene_node_set_enabled(&m->scene_buffer->node, m->show_bar);
    /* draw_bar() enables it again, if the window list has a width */
    if (!m->show_bar)
        wlr_scene_node_set_enabled(&m->bar_blur->node, 0);
    arrange_layers(srv, m);
}

/** (re)loads ``*f``, a font of m's other than the bar's, at m's scale: font
 * and size (filled in, :c:func:`awl_config_fill_defaults`); NULL if it doesn't
 * load (what says whose, for the warning) */
static void update_font( Monitor *m, struct fcft_font **f, const char *font, int size, const char *what ) {
    awl_process_t *proc = m->srv->proc;
    awl_font_release(proc, *f);
    *f = NULL;
    if (!font)
        return;
    if (!(*f = awl_font_get_exact(proc, font, (float)size, m->wlr_output->scale)))
        awl_wrn_printf("%s font %s not found, using the bar's\n", what, font);
}

/** (re)loads m's bar's emoji font (:c:member:`awl_bar_t.emoji_font`), at the bar's size */
static void update_emoji_font( awl_server_t *srv, Monitor *m ) {
    awl_font_release(srv->proc, m->bar.emoji_font);
    m->bar.emoji_font = NULL;
    if (!srv->cfg->emoji_font)
        return;
    if (!(m->bar.emoji_font = awl_font_get_emoji(srv->proc, srv->cfg->emoji_font, (float)srv->cfg->font_size,
                m->wlr_output->scale)))
        awl_wrn_printf("emoji font %s not found or not in color, the bar shows text instead\n",
                srv->cfg->emoji_font);
}

/** (re)loads m's emoji, popup and desktop fonts (:c:member:`awl_bar_t.emoji_font`,
 * :c:member:`Monitor.popup_font`, .desktop_font) */
static void update_fonts( awl_server_t *srv, Monitor *m ) {
    update_emoji_font(srv, m);
    update_font(m, &m->popup_font, srv->cfg->popup.font, srv->cfg->popup.font_size.v, "popup");
    update_font(m, &m->desktop_font, srv->cfg->desktop.font, srv->cfg->desktop.font_size.v, "desktop");
}

void update_bar( awl_server_t *srv, Monitor *m ) {
    int rw, rh;

    wlr_output_transformed_resolution(m->wlr_output, &rw, &rh);
    m->bar_area.width = rw;
    m->bar_area.real_width = (int)((float)m->bar_area.width / m->wlr_output->scale);

    if (m->bar_area.scale == m->wlr_output->scale && m->bar.m) {
        bar_view(m);
        return;
    }

    /* falls back to monospace; keeps the old font if even that fails */
    awl_bar_set_font(srv->proc, &m->bar, srv->cfg->font, (float)srv->cfg->font_size, m->wlr_output->scale, 1);

    m->bar_area.scale = m->wlr_output->scale;
    /* no font at all: an empty bar of a monospace:size=10's height */
    m->bar.lr_pad = m->bar.font ? m->bar.font->height
            : (int)lroundf(AWL_FONT_FALLBACK_SIZE * 2 * m->wlr_output->scale);
    m->bar_area.height = m->bar.lr_pad + 2;
    m->bar_area.real_height = (int)((float)m->bar_area.height / m->wlr_output->scale);
    update_fonts(srv, m);
    desktop_fontchanged(srv->desktop, m);
    popups_update(srv, m);
    bar_view(m);
}

void bar_init( awl_server_t *srv ) {
    srv->bars = calloc_or_die(1, sizeof *srv->bars);
    srv->bars->applied.show = srv->bars->applied.top = -1;
    srv->bars->hover_timer = wl_event_loop_add_timer(srv->event_loop, hover_timeout, srv);
    srv->bars->leave_timer = wl_event_loop_add_timer(srv->event_loop, leave_timeout, srv->bars);
}

void bar_fini( awl_server_t *srv ) {
    if (srv->bars->redraw_idle) {
        wl_event_source_remove(srv->bars->redraw_idle);
        srv->bars->redraw_idle = NULL;
    }
    if (srv->bars->hover_timer) {
        wl_event_source_remove(srv->bars->hover_timer);
        srv->bars->hover_timer = NULL;
        srv->bars->hover_widget = NULL;
        srv->bars->view_widget = NULL;
    }
    if (srv->bars->leave_timer) {
        wl_event_source_remove(srv->bars->leave_timer);
        srv->bars->leave_timer = NULL;
        srv->bars->hover_active = NULL;
    }
}

void bar_free( awl_server_t *srv ) {
    if (!srv->bars)
        return;
    free(srv->bars->applied.font);
    free(srv->bars->applied.emoji_font);
    free(srv->bars->applied.popup_font);
    free(srv->bars->applied.desktop_font);
    free(srv->bars);
    srv->bars = NULL;
}

void bar_add_mon( awl_server_t *srv, Monitor *m ) {
    awl_bar_setup(&m->bar, m);
    awl_plugins_bar_widgets(srv->proc, &m->bar);

    /* LyrBottom sits below LyrTile/LyrFloat, so floating windows dragged
     * over the bar's area render on top of it -- intentional */
    m->scene_buffer = wlr_scene_buffer_create(srv->layers[LyrBottom], NULL);
    m->scene_buffer->point_accepts_input = bar_accepts_input;
    /* frosts the window list's translucent entries; draw_bar() fits it and
     * enables it with cfg->blur.window_list */
    m->bar_blur = wlr_scene_blur_create(srv->layers[LyrBottom], 0, 0);
    wlr_scene_node_place_below(&m->bar_blur->node, &m->scene_buffer->node);
    wlr_scene_blur_set_strength(m->bar_blur, srv->cfg->blur.strength);
    wlr_scene_blur_set_alpha(m->bar_blur, srv->cfg->blur.alpha);

    m->show_bar = !!srv->cfg->bar.show;
    update_bar(srv, m);
    bars_dirty(srv);
}

void bar_remove_mon( awl_server_t *srv, Monitor *m ) {
    /* hover_widget points into m->bar, as may view_widget */
    if (srv->bars->view_widget && (char *)srv->bars->view_widget >= (char *)&m->bar
            && (char *)srv->bars->view_widget < (char *)(&m->bar + 1))
        srv->bars->view_widget = NULL;
    if (srv->bars->hover_widget && (char *)srv->bars->hover_widget >= (char *)&m->bar
            && (char *)srv->bars->hover_widget < (char *)(&m->bar + 1)) {
        srv->bars->hover_widget = NULL;
        wl_event_source_timer_update(srv->bars->hover_timer, 0);
    }
    if (srv->bars->hover_active && (char *)srv->bars->hover_active >= (char *)&m->bar
            && (char *)srv->bars->hover_active < (char *)(&m->bar + 1)) {
        set_leave_pending(srv->bars, 0);
        srv->bars->hover_active = NULL;
    }
    awl_font_release(srv->proc, m->popup_font);
    m->popup_font = NULL;
    awl_font_release(srv->proc, m->desktop_font);
    m->desktop_font = NULL;
    awl_bar_teardown(srv->proc, &m->bar);
}

void bar_widgets_clear( awl_server_t *srv ) {
    Monitor *m;

    /* a hover popup gets its leave callback while that still exists */
    if (srv->bars->hover_active) {
        set_leave_pending(srv->bars, 0);
        leave_timeout(srv->bars);
    }
    srv->bars->hover_widget = NULL;
    if (srv->bars->hover_timer)
        wl_event_source_timer_update(srv->bars->hover_timer, 0);
    /* it hears that the pointer left, while it still can */
    update_view(srv->bars, NULL, 0);
    FOR_EACH_MON(m, srv)
        if (m->bar.m)
            awl_bar_widgets_clear(&m->bar);
}

void bar_widgets_attach( awl_server_t *srv ) {
    Monitor *m;

    FOR_EACH_MON(m, srv)
        if (m->bar.m)
            awl_plugins_bar_widgets(srv->proc, &m->bar);
}

void bar_configure( awl_server_t *srv ) {
    Monitor *m;

    /* the bars' font, and with it their height */
    awl_font_retry(srv->proc);
    if (!streq_null(srv->cfg->font, srv->bars->applied.font) || srv->cfg->font_size != srv->bars->applied.font_size) {
        free(srv->bars->applied.font);
        srv->bars->applied.font = strdup_null(srv->cfg->font);
        srv->bars->applied.font_size = srv->cfg->font_size;
        FOR_EACH_MON(m, srv) {
            if (!m->bar.m)
                continue;
            m->bar_area.scale = 0; /* update_bar() reloads it */
            update_bar(srv, m);
            arrange_layers(srv, m);
        }
    }
    /* the bars' place, and whether they're shown: every bar follows a
     * change (toggle_bar's state otherwise stays) */
    if (srv->bars->applied.top >= 0 && !!srv->cfg->bar.top != srv->bars->applied.top)
        FOR_EACH_MON(m, srv)
            if (m->bar.m) {
                arrange_layers(srv, m);
                popups_update(srv, m);
            }
    srv->bars->applied.top = !!srv->cfg->bar.top;
    if (srv->bars->applied.show >= 0 && !!srv->cfg->bar.show != srv->bars->applied.show)
        FOR_EACH_MON(m, srv)
            if (m->bar.m && m->show_bar != !!srv->cfg->bar.show)
                toggle_bar_mon(srv, m);
    srv->bars->applied.show = !!srv->cfg->bar.show;

    /* the bars' emoji (update_bar() above reloaded them too, no harm) */
    if (!streq_null(srv->cfg->emoji_font, srv->bars->applied.emoji_font)) {
        free(srv->bars->applied.emoji_font);
        srv->bars->applied.emoji_font = strdup_null(srv->cfg->emoji_font);
        FOR_EACH_MON(m, srv)
            if (m->bar.m)
                update_emoji_font(srv, m);
        bars_dirty(srv);
    }
    /* the popups' (update_bar() above reloaded it too, no harm) */
    if (!streq_null(srv->cfg->popup.font, srv->bars->applied.popup_font)
            || srv->cfg->popup.font_size.v != srv->bars->applied.popup_font_size) {
        free(srv->bars->applied.popup_font);
        srv->bars->applied.popup_font = strdup_null(srv->cfg->popup.font);
        srv->bars->applied.popup_font_size = srv->cfg->popup.font_size.v;
        FOR_EACH_MON(m, srv) {
            if (!m->bar.m || !m->bar.font)
                continue;
            update_font(m, &m->popup_font, srv->cfg->popup.font, srv->cfg->popup.font_size.v, "popup");
            popups_update(srv, m);
        }
    }
    /* the desktop file list's, the same way */
    if (!streq_null(srv->cfg->desktop.font, srv->bars->applied.desktop_font)
            || srv->cfg->desktop.font_size.v != srv->bars->applied.desktop_font_size) {
        free(srv->bars->applied.desktop_font);
        srv->bars->applied.desktop_font = strdup_null(srv->cfg->desktop.font);
        srv->bars->applied.desktop_font_size = srv->cfg->desktop.font_size.v;
        FOR_EACH_MON(m, srv) {
            if (!m->bar.m || !m->bar.font)
                continue;
            update_font(m, &m->desktop_font, srv->cfg->desktop.font, srv->cfg->desktop.font_size.v, "desktop");
            desktop_fontchanged(srv->desktop, m);
        }
    }
    FOR_EACH_MON(m, srv) {
        wlr_scene_blur_set_strength(m->bar_blur, srv->cfg->blur.strength);
        wlr_scene_blur_set_alpha(m->bar_blur, srv->cfg->blur.alpha);
    }
}

int pointer_on_bar( awl_server_t *srv ) {
    struct wlr_scene_node *node;
    struct wlr_scene_buffer *buffer;

    return (node = wlr_scene_node_at(&srv->layers[LyrBottom]->node, srv->cursor.wlr->x, srv->cursor.wlr->y, NULL, NULL))
            && (buffer = wlr_scene_buffer_from_node(node)) && buffer == srv->out.sel_mon->scene_buffer;
}

int bar_axis( awl_server_t *srv, double delta, uint32_t mods ) {
    unsigned int xrel;
    widget_t *w;

    if (!pointer_on_bar(srv) || !(w = bar_widget_in(srv->out.sel_mon, srv->cursor.wlr->x, &xrel)))
        return 0;
    widget_wrap_scroll_callback(w, xrel, delta, mods);
    return 1;
}

int bar_button( awl_server_t *srv, uint32_t button, uint32_t mods, unsigned int *click ) {
    unsigned int xrel = 0;

    if (!pointer_on_bar(srv))
        return 0;
    /* hover popups are hover only: a press on the widget closes what
     * its hover opened, or keeps it from opening, until the pointer
     * leaves and comes back; what a click opened is the click's to close */
    widget_t *pressed = bar_widget_in(srv->out.sel_mon, srv->cursor.wlr->x, &xrel);
    if (pressed && pressed == srv->bars->hover_widget)
        wl_event_source_timer_update(srv->bars->hover_timer, 0);
    if (srv->bars->hover_active && pressed == srv->bars->hover_active) {
        set_leave_pending(srv->bars, 0);
        if (srv->bars->clicked)
            srv->bars->hover_active = NULL;
        else
            leave_timeout(srv->bars);
    }
    *click = ClkBar;
    if (!pressed || !pressed->callback_click)
        return 0;
    pressed->callback_click(pressed, xrel, button, mods);
    /* and that closes as a hover's would */
    if (pressed->callback_leave && popup_shown(srv, pressed->popup_window)) {
        if (srv->bars->hover_active && srv->bars->hover_active != pressed) {
            set_leave_pending(srv->bars, 0);
            leave_timeout(srv->bars);
        }
        srv->bars->hover_active = pressed;
        srv->bars->clicked = 1;
    }
    return 1;
}

/* awl_bar_t, awl_bar_type.h */

void awl_bar_setup( awl_bar_t *bar, Monitor *m ) {
    *bar = (awl_bar_t){ .m = m };
    /* the widgets come from libawlplugins.so, see awl_plugin_host.h */
}

struct fcft_font *awl_bar_set_font( awl_process_t *proc, awl_bar_t *bar, const char *name, float size, float scale,
                                    int fallback ) {
    struct fcft_font *font = fallback ? awl_font_get(proc, name, size, scale)
                                      : awl_font_get_exact(proc, name, size, scale);
    if (!bar || !font) {
        awl_font_release(proc, font);
        return NULL;
    }
    /* the new one first: if it is the same, the old reference keeps it */
    awl_font_release(proc, bar->font);
    bar->font = font;
    return font;
}

void awl_bar_prepare_drawing( awl_bar_t *bar, unsigned int w, unsigned int h, uint32_t *bits, int stride ) {
    if (!bar)
        return;
    awl_bar_finish_drawing(bar);
    bar->pix = awl_draw_image(w, h, bits, stride);
}

void awl_bar_finish_drawing( awl_bar_t *bar ) {
    if (bar && bar->pix) {
        pixman_image_unref(bar->pix);
        /* awl_bar_teardown() and the next awl_bar_prepare_drawing() unref
         * a bar->pix that is left; a dangling one would be a double unref */
        bar->pix = NULL;
    }
}

void awl_bar_teardown( awl_process_t *proc, awl_bar_t *bar ) {
    if (!bar)
        return;
    awl_bar_finish_drawing(bar);
    awl_font_release(proc, bar->font);
    awl_font_release(proc, bar->emoji_font);
    awl_bar_widgets_clear(bar);
    *bar = (awl_bar_t){ 0 };
}

void awl_bar_widgets_clear( awl_bar_t *bar ) {
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
