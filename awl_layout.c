#include "awl_layout.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_cursor.h"
#include "awl_input.h"
#include "awl_output.h"

static size_t pertagslot(const Monitor *m);
static struct PertagLayout *monlt(Monitor *m);
static const Layout *layoutmoved(awl_server_t *srv, const Layout *l, const Layout *old, size_t n_old);

void
arrange(awl_server_t *srv, Monitor *m)
{
    Client *c;

    if (!m->wlr_output->enabled)
        return;

    wl_list_for_each(c, &srv->clients.list, link) {
        if (c->mon == m) {
            wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON_ACTIVE(c, m));
            client_set_suspended(c, !VISIBLEON_ACTIVE(c, m));
        }
    }

    wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
            (c = focustop(srv, m)) && c->isfullscreen);

    strncpy(m->ltsymbol, sellayout(m)->symbol, LENGTH(m->ltsymbol)-1);

    /* We move all clients (except fullscreen and unmanaged) to LyrTile while
     * in floating layout to avoid "real" floating clients be always on top */
    wl_list_for_each(c, &srv->clients.list, link) {
        if (c->mon != m || c->scene->node.parent == srv->layers[LyrFS])
            continue;

        wlr_scene_node_reparent(&c->scene->node,
                c->isontop ? srv->layers[LyrTop] :
                (!sellayout(m)->arrange && c->isfloating)
                        ? srv->layers[LyrTile]
                        : (sellayout(m)->arrange && c->isfloating)
                                ? srv->layers[LyrFloat]
                                : c->scene->node.parent);
    }

    if (sellayout(m)->arrange)
        sellayout(m)->arrange(srv, m);
    motionnotify(srv, 0, NULL, 0, 0, 0, 0);
    checkidleinhibitor(srv, NULL);
    /* layout symbol, taskbar order and window states may have changed */
    barsdirty(srv);
    borderclock(srv);
}

void
gaplessgrid(awl_server_t *srv, Monitor *m)
{
    unsigned int n = 0, i = 0, ch, cw, cn, rn, rows, cols;
    Client *c;

    wl_list_for_each(c, &srv->clients.list, link)
        if (VISIBLEON_ACTIVE(c, m) && !c->isfloating && !c->ismaximized && !c->isfullscreen)
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

    rows = n / cols;

    /* window geometries */
    cw = cols ? (unsigned int)(m->w.width / cols) : (unsigned int)(m->w.width);
    cn = 0; /* current column number */
    rn = 0; /* current row number */
    wl_list_for_each(c, &srv->clients.list, link) {
        unsigned int cx, cy;
        if (!VISIBLEON_ACTIVE(c, m) || c->isfloating || c->ismaximized || c->isfullscreen)
            continue;

        if ((i / rows + 1) > (cols - n % cols))
            rows = n / cols + 1;
        ch = rows ? (unsigned int)(m->w.height / rows) : (unsigned int)(m->w.height);
        cx = m->w.x + cn * cw;
        cy = m->w.y + rn * ch;
        resize(srv, c, (struct wlr_box) { cx, cy, cw, ch}, 0);
        rn++;
        if (rn >= rows) {
            rn = 0;
            cn++;
        }
        i++;
    }
}

void
bstack(awl_server_t *srv, Monitor *m)
{
    int w, h, mh, mx, tx, ty, tw;
    int i, n = 0;
    Client *c;
    const struct PertagLayout *l = monlt(m);

    wl_list_for_each(c, &srv->clients.list, link)
        if (VISIBLEON_ACTIVE(c, m) && !c->isfloating && !c->ismaximized && !c->isfullscreen)
            n++;
    if (n == 0)
        return;

    if (n > l->nmaster) {
        mh = (int)round(l->nmaster ? l->mfact * m->w.height : 0);
        tw = m->w.width / (n - l->nmaster);
        ty = m->w.y + mh;
    } else {
        mh = m->w.height;
        tw = m->w.width;
        ty = m->w.y;
    }

    i = mx = 0;
    tx = m-> w.x;
    wl_list_for_each(c, &srv->clients.list, link) {
        if (!VISIBLEON_ACTIVE(c, m) || c->isfloating || c->ismaximized || c->isfullscreen)
            continue;
        if (i < l->nmaster) {
            w = (m->w.width - mx) / (MIN(n, l->nmaster) - i);
            resize(srv, c, (struct wlr_box) { .x = m->w.x + mx, .y = m->w.y, .width = w, .height = mh }, 0);
            mx += c->geom.width;
        } else {
            h = m->w.height - mh;
            resize(srv, c, (struct wlr_box) { .x = tx, .y = ty, .width = tw, .height = h }, 0);
            if (tw != m->w.width)
                tx += c->geom.width;
        }
        i++;
    }
}

void
incnmaster(awl_server_t *srv, const Arg *arg)
{
    if (!arg || !srv->out.selmon)
        return;
    monlt(srv->out.selmon)->nmaster = MAX(monlt(srv->out.selmon)->nmaster + arg->i, 0);
    arrange(srv, srv->out.selmon);
}

void
monocle(awl_server_t *srv, Monitor *m)
{
    Client *c;
    int n = 0;

    wl_list_for_each(c, &srv->clients.list, link) {
        if (!VISIBLEON_ACTIVE(c, m) || c->isfloating || c->isfullscreen || c->ismaximized)
            continue;
        resize(srv, c, m->w, 0);
        n++;
    }
    if (n)
        snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[%d]", n);
    if ((c = focustop(srv, m)))
        wlr_scene_node_raise_to_top(&c->scene->node);
}

int
layoutindex(awl_server_t *srv, const Layout *l)
{
    uintptr_t p = (uintptr_t)l, first = (uintptr_t)srv->cfg->layouts.v;
    if (p < first || p >= first + srv->cfg->layouts.n * sizeof(Layout))
        return -1;
    return (int)((p - first) / sizeof(Layout));
}

/** m's ptlt[] slot for what it shows: 1+t for tag t alone, 0 for several */
static size_t
pertagslot(const Monitor *m)
{
    uint32_t ts = m->tagset[m->seltags];
    return ts && !(ts & (ts - 1)) ? (size_t)__builtin_ctz(ts) + 1 : 0;
}

/** m's current layout and its settings: with per-tag layouts those of the
 * view's ptlt[] slot, else ptlt[0] */
static struct PertagLayout *
monlt(Monitor *m)
{
    return &m->ptlt[m->pertag ? pertagslot(m) : 0];
}

const Layout *
sellayout(Monitor *m)
{
    struct PertagLayout *l = monlt(m);
    return l->lt[l->sellt];
}

/** per-tag layouts on selmon on or off (:c:member:`MonitorRule.pertag` sets where it
 * starts). Either way the current view keeps its layout, mfact and nmaster;
 * turned on, the other views get back the ones they had when it was last on
 * (except views of several tags: their ptlt[0] is also the pertag-off one),
 * off, all views share the current ones. */
void
togglepertag(awl_server_t *srv, const Arg *arg)
{
    size_t slot;
    (void)arg;
    if (!srv->out.selmon)
        return;
    slot = pertagslot(srv->out.selmon);
    srv->out.selmon->pertag = !srv->out.selmon->pertag;
    if (srv->out.selmon->pertag)
        srv->out.selmon->ptlt[slot] = srv->out.selmon->ptlt[0];
    else
        srv->out.selmon->ptlt[0] = srv->out.selmon->ptlt[slot];
    barsdirty(srv);
}

/** the next (:c:member:`arg->i <Arg.i>` > 0) or previous (< 0) of :c:member:`cfg->layouts <awl_config_t.layouts>` on selmon */
void
cycle_layout(awl_server_t *srv, const Arg* arg)
{
    int i, n = (int)srv->cfg->layouts.n;
    if (!srv->out.selmon || !arg || !arg->i)
        return;
    i = layoutindex(srv, sellayout(srv->out.selmon)) + (arg->i > 0 ? 1 : -1);
    setlayout(srv, &(Arg){.i = (i % n + n) % n});
}

/** like dwm: cfg->layouts.v[:c:member:`arg->i <Arg.i>`] for selmon; with a negative :c:member:`arg->i <Arg.i>` (or
 * the one it has), back to its previous layout */
void
setlayout(awl_server_t *srv, const Arg *arg)
{
    const Layout *l = NULL;
    struct PertagLayout *ml;
    if (!srv->out.selmon || !arg || arg->i >= (int)srv->cfg->layouts.n)
        return;
    if (arg->i >= 0)
        l = &srv->cfg->layouts.v[arg->i];
    ml = monlt(srv->out.selmon);
    if (!l || l != ml->lt[ml->sellt])
        ml->sellt ^= 1;
    if (l)
        ml->lt[ml->sellt] = l;
    arrange(srv, srv->out.selmon);
    barsdirty(srv);
}

/** l, pointing into the old layouts table, moved to the same index in the
 * new one, or its first */
static const Layout *
layoutmoved(awl_server_t *srv, const Layout *l, const Layout *old, size_t n_old)
{
    size_t idx = l >= old && l < old + n_old ? (size_t)(l - old) : 0;
    return &srv->cfg->layouts.v[MIN(idx, srv->cfg->layouts.n - 1)];
}

void
tagsconfigure(awl_server_t *srv)
{
    Monitor *m;
    Client *c;
    size_t i;
    wl_list_for_each(m, &srv->out.mons, link)
        for (i = 0; i < LENGTH(m->tagset); i++)
            if (!(m->tagset[i] &= TAGMASK(srv->cfg)))
                m->tagset[i] = 1;
    wl_list_for_each(c, &srv->clients.list, link)
        if (!(c->tags &= TAGMASK(srv->cfg)))
            c->tags = 1;
}

void
layoutsmoved(awl_server_t *srv, const Layout *old, size_t n_old)
{
    Monitor *m;
    size_t i, j;
    wl_list_for_each(m, &srv->out.mons, link) {
        for (j = 0; j < LENGTH(m->ptlt); j++)
            for (i = 0; i < LENGTH(m->ptlt[j].lt); i++)
                m->ptlt[j].lt[i] = layoutmoved(srv, m->ptlt[j].lt[i], old, n_old);
        arrange(srv, m);
    }
}

/** arg > 1.0 will set mfact absolutely */
void
setmfact(awl_server_t *srv, const Arg *arg)
{
    float f;

    if (!arg || !srv->out.selmon || !sellayout(srv->out.selmon)->arrange)
        return;
    f = arg->f < 1.0f ? arg->f + monlt(srv->out.selmon)->mfact : arg->f - 1.0f;
    if (f < 0.1 || f > 0.9)
        return;
    monlt(srv->out.selmon)->mfact = f;
    arrange(srv, srv->out.selmon);
}

void
tag(awl_server_t *srv, const Arg *arg)
{
    Client *sel = focustop(srv, srv->out.selmon);
    if (!sel || (arg->ui & TAGMASK(srv->cfg)) == 0)
        return;

    sel->tags = arg->ui & TAGMASK(srv->cfg);
    focusclient(srv, focustop(srv, srv->out.selmon), 1);
    arrange(srv, srv->out.selmon);
    barsdirty(srv);
}

void
tagmon(awl_server_t *srv, const Arg *arg)
{
    Client *sel = focustop(srv, srv->out.selmon);
    if (sel)
        setmon(srv, sel, nextmon(srv, arg->i), 0);
}

void
tile(awl_server_t *srv, Monitor *m)
{
    unsigned int mw, my, ty;
    int i, n = 0;
    Client *c;
    const struct PertagLayout *l = monlt(m);

    wl_list_for_each(c, &srv->clients.list, link)
        if (VISIBLEON_ACTIVE(c, m) && !c->isfloating && !c->isfullscreen && !c->ismaximized)
            n++;
    if (n == 0)
        return;

    if (n > l->nmaster)
        mw = l->nmaster ? (int)roundf(m->w.width * l->mfact) : 0;
    else
        mw = m->w.width;
    i = my = ty = 0;
    wl_list_for_each(c, &srv->clients.list, link) {
        if (!VISIBLEON_ACTIVE(c, m) || c->isfloating || c->isfullscreen || c->ismaximized)
            continue;
        if (i < l->nmaster) {
            resize(srv, c, (struct wlr_box){.x = m->w.x, .y = m->w.y + my, .width = mw,
                .height = (m->w.height - my) / (MIN(n, l->nmaster) - i)}, 0);
            my += c->geom.height;
        } else {
            resize(srv, c, (struct wlr_box){.x = m->w.x + mw, .y = m->w.y + ty,
                .width = m->w.width - mw, .height = (m->w.height - ty) / (n - i)}, 0);
            ty += c->geom.height;
        }
        i++;
    }
}

void
toggletag(awl_server_t *srv, const Arg *arg)
{
    uint32_t newtags;
    Client *sel = focustop(srv, srv->out.selmon);
    if (!sel || !(newtags = sel->tags ^ (arg->ui & TAGMASK(srv->cfg))))
        return;

    sel->tags = newtags;
    focusclient(srv, focustop(srv, srv->out.selmon), 1);
    arrange(srv, srv->out.selmon);
    barsdirty(srv);
}

void
toggleview(awl_server_t *srv, const Arg *arg)
{
    uint32_t newtagset;
    if (!(newtagset = srv->out.selmon ? srv->out.selmon->tagset[srv->out.selmon->seltags] ^ (arg->ui & TAGMASK(srv->cfg)) : 0))
        return;

    srv->out.selmon->tagset[srv->out.selmon->seltags] = newtagset;
    focusclient(srv, focustop(srv, srv->out.selmon), 1);
    arrange(srv, srv->out.selmon);
    barsdirty(srv);
}

void
view(awl_server_t *srv, const Arg *arg)
{
    if (!srv->out.selmon || (arg->ui & TAGMASK(srv->cfg)) == srv->out.selmon->tagset[srv->out.selmon->seltags])
        return;
    srv->out.selmon->seltags ^= 1; /* toggle sel tagset */
    if (arg->ui & TAGMASK(srv->cfg))
        srv->out.selmon->tagset[srv->out.selmon->seltags] = arg->ui & TAGMASK(srv->cfg);
    focusclient(srv, focustop(srv, srv->out.selmon), 1);
    arrange(srv, srv->out.selmon);
    barsdirty(srv);
}

void
cycle_view(awl_server_t *srv, const Arg* arg)
{
    if (arg->i == 0) return;
    int tmax = -1;
    int tmin = 32;
    int nt = TAGCOUNT(srv->cfg);
    for (int t=0; t<nt; ++t)
        if (srv->out.selmon->tagset[srv->out.selmon->seltags] & (1 << t)) {
            tmax = MAX(tmax, t);
            tmin = MIN(tmin, t);
        }

    srv->out.selmon->seltags ^= 1; /* toggle sel tagset */
    int next = arg->i > 0 ? (tmax + 1)%nt : (tmin - 1 + nt)%nt;
    srv->out.selmon->tagset[srv->out.selmon->seltags] = (1 << next) & TAGMASK(srv->cfg);

    focusclient(srv, focustop(srv, srv->out.selmon), 1);
    arrange(srv, srv->out.selmon);
    barsdirty(srv);
}
