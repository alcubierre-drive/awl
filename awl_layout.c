#include "awl_layout.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_cursor.h"
#include "awl_input.h"
#include "awl_output.h"

static size_t per_tag_slot( const Monitor *m );
static struct PertagLayout *mon_tag_layout( Monitor *m );
static const Layout *layout_moved( const awl_config_t *cfg, const Layout *l, const Layout *old, size_t n_old );

void arrange( awl_server_t *srv, Monitor *m ) {
    Client *c;

    if (!m->wlr_output->enabled)
        return;

    wl_list_for_each(c, &srv->clients.list, link) {
        if (c->mon == m) {
            wlr_scene_node_set_enabled(&c->scene->node, VISIBLE_ON_ACTIVE(c, m));
            client_set_suspended(c, !VISIBLE_ON_ACTIVE(c, m));
        }
    }

    wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
            (c = focus_top(srv, m)) && c->is_fullscreen);

    strncpy(m->layout_symbol, selected_layout(m)->symbol, LENGTH(m->layout_symbol)-1);

    /* We move all clients (except fullscreen and unmanaged) to LyrTile while
     * in floating layout to avoid "real" floating clients be always on top */
    wl_list_for_each(c, &srv->clients.list, link) {
        if (c->mon != m || c->scene->node.parent == srv->layers[LyrFS])
            continue;

        wlr_scene_node_reparent(&c->scene->node,
                c->is_on_top ? srv->layers[LyrTop] :
                (!selected_layout(m)->arrange && c->is_floating)
                        ? srv->layers[LyrTile]
                        : (selected_layout(m)->arrange && c->is_floating)
                                ? srv->layers[LyrFloat]
                                : c->scene->node.parent);
    }

    if (selected_layout(m)->arrange)
        selected_layout(m)->arrange(srv, m);
    motion_notify(srv, 0, NULL, 0, 0, 0, 0);
    check_idle_inhibitor(srv, NULL);
    /* layout symbol, taskbar order and window states may have changed */
    bars_dirty(srv);
    border_clock(srv);
}

void gapless_grid( awl_server_t *srv, Monitor *m ) {
    unsigned int n = 0, i = 0, cols;
    Client *c;

    wl_list_for_each(c, &srv->clients.list, link)
        if (VISIBLE_ON_ACTIVE(c, m) && !c->is_floating && !c->is_maximized && !c->is_fullscreen)
            n++;
    if (n == 0)
        return;

    /* grid dimensions */
    for (cols = 0; cols <= (n / 2); cols++) {
        if ((cols * cols) >= n)
            break;
    }

    if (n == 5) { /* set layout against the general calculation: not 1:2:2, but 2:3 */
        cols = 2;
    }

    /* widescreen is better if 3 columns */
    if (n >= 3 && n <= 6 && (m->w.width / m->w.height) > 1) {
        cols = 3;
    }

    unsigned int rows = n / cols;

    /* window geometries */
    unsigned int cw = cols ? (unsigned int)(m->w.width / cols) : (unsigned int)(m->w.width);
    unsigned int cn = 0; /* current column number */
    unsigned int rn = 0; /* current row number */
    wl_list_for_each(c, &srv->clients.list, link) {
        if (!VISIBLE_ON_ACTIVE(c, m) || c->is_floating || c->is_maximized || c->is_fullscreen)
            continue;

        if ((i / rows + 1) > (cols - n % cols))
            rows = n / cols + 1;
        unsigned int ch = rows ? (unsigned int)(m->w.height / rows) : (unsigned int)(m->w.height);
        unsigned int cx = m->w.x + cn * cw;
        unsigned int cy = m->w.y + rn * ch;
        resize(srv, c, (struct wlr_box) { cx, cy, cw, ch}, 0);
        rn++;
        if (rn >= rows) {
            rn = 0;
            cn++;
        }
        i++;
    }
}

void bottom_stack( awl_server_t *srv, Monitor *m ) {
    int n = 0;
    Client *c;
    const struct PertagLayout *l = mon_tag_layout(m);

    wl_list_for_each(c, &srv->clients.list, link)
        if (VISIBLE_ON_ACTIVE(c, m) && !c->is_floating && !c->is_maximized && !c->is_fullscreen)
            n++;
    if (n == 0)
        return;

    int mh = m->w.height, tw = m->w.width, ty = m->w.y;
    if (n > l->master_count) {
        mh = (int)round(l->master_count ? l->master_factor * m->w.height : 0);
        tw = m->w.width / (n - l->master_count);
        ty = m->w.y + mh;
    }

    int i = 0, mx = 0, tx = m->w.x;
    wl_list_for_each(c, &srv->clients.list, link) {
        if (!VISIBLE_ON_ACTIVE(c, m) || c->is_floating || c->is_maximized || c->is_fullscreen)
            continue;
        if (i < l->master_count) {
            int w = (m->w.width - mx) / (MIN(n, l->master_count) - i);
            resize(srv, c, (struct wlr_box) { .x = m->w.x + mx, .y = m->w.y, .width = w, .height = mh }, 0);
            mx += c->geom.width;
        } else {
            int h = m->w.height - mh;
            resize(srv, c, (struct wlr_box) { .x = tx, .y = ty, .width = tw, .height = h }, 0);
            if (tw != m->w.width)
                tx += c->geom.width;
        }
        i++;
    }
}

void change_master_count( awl_server_t *srv, const Arg *arg ) {
    if (!arg || !srv->out.sel_mon)
        return;
    mon_tag_layout(srv->out.sel_mon)->master_count = MAX(mon_tag_layout(srv->out.sel_mon)->master_count + arg->i, 0);
    arrange(srv, srv->out.sel_mon);
}

void monocle( awl_server_t *srv, Monitor *m ) {
    Client *c;
    int n = 0;

    wl_list_for_each(c, &srv->clients.list, link) {
        if (!VISIBLE_ON_ACTIVE(c, m) || c->is_floating || c->is_fullscreen || c->is_maximized)
            continue;
        resize(srv, c, m->w, 0);
        n++;
    }
    if (n)
        snprintf(m->layout_symbol, LENGTH(m->layout_symbol), "[%d]", n);
    if ((c = focus_top(srv, m)))
        wlr_scene_node_raise_to_top(&c->scene->node);
}

int layout_index( const awl_config_t *cfg, const Layout *l ) {
    uintptr_t p = (uintptr_t)l, first = (uintptr_t)cfg->layouts.v;
    if (p < first || p >= first + cfg->layouts.n * sizeof(Layout))
        return -1;
    return (int)((p - first) / sizeof(Layout));
}

/** m's tag_layouts[] slot for what it shows: 1+t for tag t alone, 0 for several */
static size_t per_tag_slot( const Monitor *m ) {
    uint32_t ts = m->tagset[m->sel_tags];
    return ts && !(ts & (ts - 1)) ? (size_t)__builtin_ctz(ts) + 1 : 0;
}

/** m's current layout and its settings: with per-tag layouts those of the
 * view's tag_layouts[] slot, else tag_layouts[0] */
static struct PertagLayout *mon_tag_layout( Monitor *m ) {
    return &m->tag_layouts[m->per_tag ? per_tag_slot(m) : 0];
}

const Layout *selected_layout( Monitor *m ) {
    struct PertagLayout *l = mon_tag_layout(m);
    return l->lt[l->sel_layout_index];
}

/** per-tag layouts on sel_mon on or off (:c:member:`MonitorRule.per_tag` sets where it
 * starts). Either way the current view keeps its layout, master_factor and master_count;
 * turned on, the other views get back the ones they had when it was last on
 * (except views of several tags: their tag_layouts[0] is also the per_tag-off one),
 * off, all views share the current ones. */
void toggle_per_tag( awl_server_t *srv, const Arg *arg ) {
    (void)arg;
    if (!srv->out.sel_mon)
        return;
    size_t slot = per_tag_slot(srv->out.sel_mon);
    srv->out.sel_mon->per_tag = !srv->out.sel_mon->per_tag;
    if (srv->out.sel_mon->per_tag)
        srv->out.sel_mon->tag_layouts[slot] = srv->out.sel_mon->tag_layouts[0];
    else
        srv->out.sel_mon->tag_layouts[0] = srv->out.sel_mon->tag_layouts[slot];
    bars_dirty(srv);
}

/** the next (:c:member:`arg->i <Arg.i>` > 0) or previous (< 0) of :c:member:`cfg->layouts <awl_config_t.layouts>` on sel_mon */
void cycle_layout( awl_server_t *srv, const Arg* arg ) {
    int i, n = (int)srv->cfg->layouts.n;
    if (!srv->out.sel_mon || !arg || !arg->i)
        return;
    i = layout_index(srv->cfg, selected_layout(srv->out.sel_mon)) + (arg->i > 0 ? 1 : -1);
    set_layout(srv, &(Arg){.i = (i % n + n) % n});
}

/** like dwm: cfg->layouts.v[:c:member:`arg->i <Arg.i>`] for sel_mon; with a negative :c:member:`arg->i <Arg.i>` (or
 * the one it has), back to its previous layout */
void set_layout( awl_server_t *srv, const Arg *arg ) {
    const Layout *l = NULL;
    if (!srv->out.sel_mon || !arg || arg->i >= (int)srv->cfg->layouts.n)
        return;
    if (arg->i >= 0)
        l = &srv->cfg->layouts.v[arg->i];
    struct PertagLayout *ml = mon_tag_layout(srv->out.sel_mon);
    if (!l || l != ml->lt[ml->sel_layout_index])
        ml->sel_layout_index ^= 1;
    if (l)
        ml->lt[ml->sel_layout_index] = l;
    arrange(srv, srv->out.sel_mon);
    bars_dirty(srv);
}

/** l, pointing into the old layouts table, moved to the same index in the
 * new one, or its first */
static const Layout *layout_moved( const awl_config_t *cfg, const Layout *l, const Layout *old, size_t n_old ) {
    size_t idx = l >= old && l < old + n_old ? (size_t)(l - old) : 0;
    return &cfg->layouts.v[MIN(idx, cfg->layouts.n - 1)];
}

void tags_configure( awl_server_t *srv ) {
    Monitor *m;
    Client *c;
    FOR_EACH_MON(m, srv)
        for (size_t i = 0; i < LENGTH(m->tagset); i++)
            if (!(m->tagset[i] &= TAG_MASK(srv->cfg)))
                m->tagset[i] = 1;
    wl_list_for_each(c, &srv->clients.list, link)
        if (!(c->tags &= TAG_MASK(srv->cfg)))
            c->tags = 1;
}

void layouts_moved( awl_server_t *srv, const Layout *old, size_t n_old ) {
    Monitor *m;
    FOR_EACH_MON(m, srv) {
        for (size_t j = 0; j < LENGTH(m->tag_layouts); j++)
            for (size_t i = 0; i < LENGTH(m->tag_layouts[j].lt); i++)
                m->tag_layouts[j].lt[i] = layout_moved(srv->cfg, m->tag_layouts[j].lt[i], old, n_old);
        arrange(srv, m);
    }
}

/** arg > 1.0 will set master_factor absolutely */
void set_master_factor( awl_server_t *srv, const Arg *arg ) {
    if (!arg || !srv->out.sel_mon || !selected_layout(srv->out.sel_mon)->arrange)
        return;
    float f = arg->f < 1.0f ? arg->f + mon_tag_layout(srv->out.sel_mon)->master_factor : arg->f - 1.0f;
    if (f < 0.1 || f > 0.9)
        return;
    mon_tag_layout(srv->out.sel_mon)->master_factor = f;
    arrange(srv, srv->out.sel_mon);
}

void tag( awl_server_t *srv, const Arg *arg ) {
    Client *sel = focus_top(srv, srv->out.sel_mon);
    if (!sel || (arg->ui & TAG_MASK(srv->cfg)) == 0)
        return;

    sel->tags = arg->ui & TAG_MASK(srv->cfg);
    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    arrange(srv, srv->out.sel_mon);
    bars_dirty(srv);
}

void move_to_monitor( awl_server_t *srv, const Arg *arg ) {
    Client *sel = focus_top(srv, srv->out.sel_mon);
    if (sel)
        set_mon(srv, sel, next_mon(srv, arg->i), 0);
}

void tile( awl_server_t *srv, Monitor *m ) {
    int n = 0;
    Client *c;
    const struct PertagLayout *l = mon_tag_layout(m);

    wl_list_for_each(c, &srv->clients.list, link)
        if (VISIBLE_ON_ACTIVE(c, m) && !c->is_floating && !c->is_fullscreen && !c->is_maximized)
            n++;
    if (n == 0)
        return;

    unsigned int mw = m->w.width, my = 0, ty = 0;
    int i = 0;
    if (n > l->master_count)
        mw = l->master_count ? (int)roundf(m->w.width * l->master_factor) : 0;
    wl_list_for_each(c, &srv->clients.list, link) {
        if (!VISIBLE_ON_ACTIVE(c, m) || c->is_floating || c->is_fullscreen || c->is_maximized)
            continue;
        if (i < l->master_count) {
            resize(srv, c, (struct wlr_box){.x = m->w.x, .y = m->w.y + my, .width = mw,
                .height = (m->w.height - my) / (MIN(n, l->master_count) - i)}, 0);
            my += c->geom.height;
        } else {
            resize(srv, c, (struct wlr_box){.x = m->w.x + mw, .y = m->w.y + ty,
                .width = m->w.width - mw, .height = (m->w.height - ty) / (n - i)}, 0);
            ty += c->geom.height;
        }
        i++;
    }
}

void toggle_tag( awl_server_t *srv, const Arg *arg ) {
    uint32_t new_tags;
    Client *sel = focus_top(srv, srv->out.sel_mon);
    if (!sel || !(new_tags = sel->tags ^ (arg->ui & TAG_MASK(srv->cfg))))
        return;

    sel->tags = new_tags;
    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    arrange(srv, srv->out.sel_mon);
    bars_dirty(srv);
}

void toggle_view( awl_server_t *srv, const Arg *arg ) {
    uint32_t new_tagset;
    if (!(new_tagset = srv->out.sel_mon ? srv->out.sel_mon->tagset[srv->out.sel_mon->sel_tags] ^ (arg->ui & TAG_MASK(srv->cfg)) : 0))
        return;

    srv->out.sel_mon->tagset[srv->out.sel_mon->sel_tags] = new_tagset;
    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    arrange(srv, srv->out.sel_mon);
    bars_dirty(srv);
}

void view( awl_server_t *srv, const Arg *arg ) {
    if (!srv->out.sel_mon || (arg->ui & TAG_MASK(srv->cfg)) == srv->out.sel_mon->tagset[srv->out.sel_mon->sel_tags])
        return;
    srv->out.sel_mon->sel_tags ^= 1; /* toggle sel tagset */
    if (arg->ui & TAG_MASK(srv->cfg))
        srv->out.sel_mon->tagset[srv->out.sel_mon->sel_tags] = arg->ui & TAG_MASK(srv->cfg);
    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    arrange(srv, srv->out.sel_mon);
    bars_dirty(srv);
}

void cycle_view( awl_server_t *srv, const Arg* arg ) {
    if (arg->i == 0) return;
    int tmax = -1;
    int tmin = 32;
    int nt = TAG_COUNT(srv->cfg);
    for (int t=0; t<nt; ++t)
        if (srv->out.sel_mon->tagset[srv->out.sel_mon->sel_tags] & (1 << t)) {
            tmax = MAX(tmax, t);
            tmin = MIN(tmin, t);
        }

    srv->out.sel_mon->sel_tags ^= 1; /* toggle sel tagset */
    int next = arg->i > 0 ? (tmax + 1)%nt : (tmin - 1 + nt)%nt;
    srv->out.sel_mon->tagset[srv->out.sel_mon->sel_tags] = (1 << next) & TAG_MASK(srv->cfg);

    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    arrange(srv, srv->out.sel_mon);
    bars_dirty(srv);
}
