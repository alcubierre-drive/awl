#include "awl_client.h"
#include "awl_bar.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_layout.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_popup.h"

static void applybounds(Client *c, struct wlr_box *bbox);
static void applyrules(Client *c);
static void attachblur(Client *c);
static void commitnotify(struct wl_listener *listener, void *data);
static void commitpopup(struct wl_listener *listener, void *data);
static void destroypopuplistener(struct wl_listener *listener, void *data);
static void destroydecoration(struct wl_listener *listener, void *data);
static void maximizenotify(struct wl_listener *listener, void *data);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void setontop(Client* c, int ontop);

struct wl_list clients;
struct wl_list fstack;
void *exclusive_focus;
uint32_t borders[BorderLast];
/** the border width plugins_config.c had, to tell whether a reload changed
 * it */
static unsigned int applied_borderpx;
void
applybounds(Client *c, struct wlr_box *bbox)
{
    /* set minimum possible */
    c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
    c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);

    if (c->geom.x >= bbox->x + bbox->width)
        c->geom.x = bbox->x + bbox->width - c->geom.width;
    if (c->geom.y >= bbox->y + bbox->height)
        c->geom.y = bbox->y + bbox->height - c->geom.height;
    if (c->geom.x + c->geom.width <= bbox->x)
        c->geom.x = bbox->x;
    if (c->geom.y + c->geom.height <= bbox->y)
        c->geom.y = bbox->y;
}

void
scenebuffersetopacity(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    Client *c = data;
    if (c && c->one_minus_alpha != 0 && c->blur) {
        float opacity = 1. - c->one_minus_alpha;
        struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
        if (!scene_surface) return;

        struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(scene_surface->surface);
        if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
            wlr_scene_buffer_set_opacity(buffer, opacity);

            // if (!wlr_subsurface_try_from_wlr_surface(xdg_surface->surface) && c->blur) {
            //     wlr_scene_blur_set_transparency_mask_source(c->blur, buffer);
            // }
        }
    }
}

void
applyrules(Client *c)
{
    /* rule matching */
    const char *appid, *title;
    uint32_t newtags = 0;
    int i;
    const Rule *r;
    Monitor *mon = selmon, *m;

    appid = client_get_appid(c);
    title = client_get_title(c);
    awl_log_printf( "SPAWN APPID '%s' @TITLE '%s'\n", appid, title );
    int apply_resize = 0;
    struct wlr_box rbox;

    for (r = cfg->rules.v; r < cfg->rules.v + cfg->rules.n; r++) {
        if ((!r->title || strstr(title, r->title))
                && (!r->id || strstr(appid, r->id))) {
            c->isfloating = r->isfloating;
            newtags |= r->tags;
            i = 0;
            wl_list_for_each(m, &mons, link) {
                if (r->monitor == i++)
                    mon = m;
            }
            if (c->isfloating || !sellayout(mon)->arrange) {
                /* client is floating or in floating layout */
                if (r->w != 0 && r->h != 0) {
                    rbox.width = r->w;
                    rbox.height = r->h;
                    rbox.x = mon->w.x + mon->w.width - rbox.width;
                    rbox.y = mon->w.y + mon->w.height - rbox.height;
                    apply_resize = 1;
                }
            }
            c->one_minus_alpha = r->one_minus_alpha;
            if (r->blur && !c->blur) attachblur(c);
        }
    }

    c->isfloating |= client_is_float_type(c);
    setmon(c, mon, newtags);
    if (apply_resize) resize(c, rbox, 1);
}

void
attachblur(Client *c)
{
    if (!c) return;
    struct wlr_scene_tree* tree = c->one_minus_alpha == 0 ? c->scene : c->scene_surface;
    if (!tree) return;
    if (!c->blur) {
        c->blur = wlr_scene_blur_create(tree, 0, 0);
        wlr_scene_blur_set_size(c->blur, c->geom.width, c->geom.height);
        wlr_scene_blur_set_strength(c->blur, cfg->blur.strength);
        wlr_scene_blur_set_alpha(c->blur, cfg->blur.alpha);
        wlr_scene_blur_set_should_only_blur_bottom_layer(c->blur, 0);
        wlr_scene_node_lower_to_bottom(&c->blur->node);
        wlr_scene_node_set_enabled(&c->blur->node, 1);
    }
}

void
commitnotify(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, commit);

    if (c->surface.xdg->initial_commit) {
        /*
         * Get the monitor this client will be rendered on
         * Note that if the user set a rule in which the client is placed on
         * a different monitor based on its title, this will likely select
         * a wrong monitor.
         */
        applyrules(c);
        if (c->mon) {
            client_set_scale(client_surface(c), c->mon->wlr_output->scale);
        }
        setmon(c, NULL, 0); /* Make sure to reapply rules in mapnotify() */

        wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
        if (c->decoration)
            requestdecorationmode(&c->set_decoration_mode, c->decoration);
        wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
        return;
    }

    resize(c, c->geom, (c->isfloating && !c->isfullscreen));

    /* mark a pending resize as completed */
    if (c->resize && c->resize <= c->surface.xdg->current.configure_serial)
        c->resize = 0;
}

void
commitpopup(struct wl_listener *listener, void *data)
{
    struct wlr_surface *surface = data;
    struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
    LayerSurface *l = NULL;
    Client *c = NULL;
    struct wlr_box box;
    int type = -1;

    if (!popup || !popup->base->initial_commit)
        return;

    type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
    if (type < 0)
        return;

    if ((type == XDGShell && (!c || !c->mon || !c->scene)) ||
        (type == LayerShell && (!l || !l->mon || !l->scene))) {
        wlr_xdg_popup_destroy(popup);
        return;
    }

    struct wlr_scene_tree *parent_tree = NULL;
    struct wlr_xdg_surface *parent_xdg = wlr_xdg_surface_try_from_wlr_surface(popup->parent);

    if (parent_xdg && parent_xdg->surface->data) {
        struct wlr_scene_node *parent_node = parent_xdg->surface->data;
        parent_tree = wlr_scene_tree_from_node(parent_node);
    }

    // fallback
    if (!parent_tree)
        /* l->popups (not l->scene) is the tree commitlayersurfacenotify()
         * and createlayersurface() deliberately keep pinned to LyrTop (or
         * higher) even when the layer surface itself lives at LyrBottom/Bg,
         * so that popups (e.g. the tray's context menu) render above the
         * float layer instead of inheriting their parent's lower stacking
         * position. */
        parent_tree = (type == LayerShell) ? l->popups : c->scene;

    if (!parent_tree) {
        wlr_xdg_popup_destroy(popup);
        return;
    }

    // create popup in scene graph
    struct wlr_scene_tree *popup_tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
    if (!popup_tree) {
        wlr_xdg_popup_destroy(popup);
        return;
    }

    // save pointer for submenus
    popup->base->surface->data = &popup_tree->node;

    box = type == LayerShell ? l->mon->m : c->mon->w;
    box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x);
    box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y);
    wlr_xdg_popup_unconstrain_from_box(popup, &box);

    // do *NOT* free/remove from list, because there's a listener for that
    // (multiple allocation inhibited through the !popup->base->initial_commit
    // check
}

void
createdecoration(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    Client *c = deco->toplevel->base->data;
    c->decoration = deco;

    LISTEN(&deco->events.request_mode, &c->set_decoration_mode, requestdecorationmode);
    LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

    requestdecorationmode(&c->set_decoration_mode, deco);
}

void
createnotify(struct wl_listener *listener, void *data)
{
    /* This event is raised when a client creates a new toplevel (application window). */
    struct wlr_xdg_toplevel *toplevel = data;
    Client *c = NULL;

    /* Allocate a Client for this surface */
    c = toplevel->base->data = ecalloc(1, sizeof(*c));
    c->surface.xdg = toplevel->base;
    c->bw = cfg->borderpx;
    c->isvisible = 1;

    LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
    LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
    LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
    LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
    LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
    LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
    LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

// helper struct to free the dynamical listener upon surface destruction
struct popup_listener {
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void
destroypopuplistener(struct wl_listener *listener, void *data)
{
    struct popup_listener *p_listener = wl_container_of(listener, p_listener, destroy);
    wl_list_remove(&p_listener->commit.link);
    wl_list_remove(&p_listener->destroy.link);
    free(p_listener);
}

void
createpopup(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_popup *popup = data;

    struct popup_listener *p_listener = calloc(1, sizeof*p_listener);
    if (!p_listener) return;

    p_listener->commit.notify = commitpopup;
    wl_signal_add(&popup->base->surface->events.commit, &p_listener->commit);

    p_listener->destroy.notify = destroypopuplistener;
    wl_signal_add(&popup->base->surface->events.destroy, &p_listener->destroy);
}

void
destroydecoration(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, destroy_decoration);

    wl_list_remove(&c->destroy_decoration.link);
    wl_list_remove(&c->set_decoration_mode.link);
}

void
destroynotify(struct wl_listener *listener, void *data)
{
    /* Called when the xdg_toplevel is destroyed. */
    Client *c = wl_container_of(listener, c, destroy);
    wl_list_remove(&c->destroy.link);
    wl_list_remove(&c->set_title.link);
    wl_list_remove(&c->fullscreen.link);
#ifdef XWAYLAND
    if (c->type != XDGShell) {
        wl_list_remove(&c->activate.link);
        wl_list_remove(&c->associate.link);
        wl_list_remove(&c->configure.link);
        wl_list_remove(&c->dissociate.link);
        wl_list_remove(&c->set_hints.link);
    } else
#endif
    {
        wl_list_remove(&c->commit.link);
        wl_list_remove(&c->map.link);
        wl_list_remove(&c->unmap.link);
        wl_list_remove(&c->maximize.link);
    }
    free(c);
}

void
focusclient(Client *c, int lift)
{
    struct wlr_surface *old = seat->keyboard_state.focused_surface;
    int unused_lx, unused_ly, old_client_type;
    Client *old_c = NULL;
    LayerSurface *old_l = NULL;

    if (locked)
        return;

    /* Raise client in stacking order if requested */
    if (c && lift)
        wlr_scene_node_raise_to_top(&c->scene->node);

    if (c && client_surface(c) == old)
        return;

    if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
        struct wlr_xdg_popup *popup, *tmp;
        wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
            wlr_xdg_popup_destroy(popup);
    }

    /* Put the new client atop the focus stack and select its monitor */
    if (c && !client_is_unmanaged(c)) {
        wl_list_remove(&c->flink);
        wl_list_insert(&fstack, &c->flink);
        selmon = c->mon;
        c->isurgent = 0;

        /* Don't change border color if there is an exclusive focus or we are
         * handling a drag operation */
        if (!exclusive_focus && !seat->drag)
            client_set_border_color(c, (float[])RGBA(borders[BorderSel]));
    }

    /* Deactivate old client if focus is changing */
    if (old && (!c || client_surface(c) != old)) {
        /* If an overlay is focused, don't focus or activate the client,
         * but only update its position in fstack to render its border with its color
         * and focus it after the overlay is closed. */
        if (old_client_type == LayerShell && wlr_scene_node_coords(
                    &old_l->scene->node, &unused_lx, &unused_ly)
                && old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
            return;
        } else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
            return;
        /* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
         * and probably other clients */
        } else if (old_c && !client_is_unmanaged(old_c) && (!c || !client_wants_focus(c))) {
            client_set_border_color(old_c, (float[])RGBA(borders[BorderNorm]));
            client_activate_surface(old, 0);
        }
    }
    barsdirty();

    if (!c) {
        /* With no client, all we have left is to clear focus */
        wlr_seat_keyboard_notify_clear_focus(seat);
        return;
    }

    /* a popup has the keyboard (the launcher): c gets it once that is
     * closed (popupsgrabupdate()) */
    if (popupkeyboard())
        return;

    /* Change cursor surface */
    motionnotify(0, NULL, 0, 0, 0, 0);

    /* Have a client, so focus its top-level wlr_surface */
    client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

    /* Activate the new client */
    client_activate_surface(client_surface(c), 1);
}

void
focusstack(const Arg *arg)
{
    /* Focus the next or previous client (in tiling order) on selmon */
    Client *c, *sel = focustop(selmon);
    if (!sel || (sel->isfullscreen && !client_has_children(sel)))
        return;
    if (arg->i > 0) {
        wl_list_for_each(c, &sel->link, link) {
            if (&c->link == &clients)
                continue; /* wrap past the sentinel node */
            if (VISIBLEON_ACTIVE(c, selmon))
                break; /* found it */
        }
    } else {
        wl_list_for_each_reverse(c, &sel->link, link) {
            if (&c->link == &clients)
                continue; /* wrap past the sentinel node */
            if (VISIBLEON_ACTIVE(c, selmon))
                break; /* found it */
        }
    }
    /* If only one client is visible on selmon, then c == sel */
    focusclient(c, 1);
}

void
movestack(const Arg *arg)
{
    Client *c, *sel = focustop(selmon);

    if (!sel || wl_list_length(&clients) <= 1)
        return;

    if (arg->i > 0) {
        wl_list_for_each(c, &sel->link, link) {
            if (&c->link == &clients) {
                c = wl_container_of(&clients, c, link);
                break; /* wrap past the sentinel node */
            }
            if (VISIBLEON(c, selmon) || &c->link == &clients) {
                break; /* found it */
            }
        }
    } else {
        wl_list_for_each_reverse(c, &sel->link, link) {
            if (&c->link == &clients) {
                c = wl_container_of(&clients, c, link);
                break; /* wrap past the sentinel node */
            }
            if (VISIBLEON(c, selmon) || &c->link == &clients) {
                break; /* found it */
            }
        }
        /* backup one client */
        c = wl_container_of(c->link.prev, c, link);
    }

    wl_list_remove(&sel->link);
    wl_list_insert(&c->link, &sel->link);
    arrange(selmon);
}

Client *
focustop(Monitor *m)
{
    Client *c;
    wl_list_for_each(c, &fstack, flink) {
        if (VISIBLEON_ACTIVE(c, m))
            return c;
    }
    return NULL;
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, fullscreen);
    setfullscreen(c, client_wants_fullscreen(c));
}

void
killclient(const Arg *arg)
{
    Client *sel = focustop(selmon);
    if (sel)
        client_send_close(sel);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
    /* Called when the surface is mapped, or ready to display on-screen. */
    Client *p = NULL;
    Client *w, *c = wl_container_of(listener, c, map);
    Monitor *m;
    int i;

    /* Create scene tree for this client and its border */
    c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
    /* Enabled later by a call to arrange() */
    wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
    c->scene_surface = c->type == XDGShell
            ? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
            : wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
    c->scene->node.data = c->scene_surface->node.data = c;

    client_get_geometry(c, &c->geom);

    /* Handle unmanaged clients first so we can return prior create borders */
    if (client_is_unmanaged(c)) {
        /* Unmanaged clients always are floating */
        wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
        wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
        client_set_size(c, c->geom.width, c->geom.height);
        if (client_wants_focus(c)) {
            focusclient(c, 1);
            exclusive_focus = c;
        }
        goto unset_fullscreen;
    }

    for (i = 0; i < 4; i++) {
        c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
            (float[])RGBA(borders[c->isurgent ? BorderUrg : BorderNorm]));
        c->border[i]->node.data = c;
    }

    /* Initialize client geometry with room for border */
    client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
    c->geom.width += 2 * c->bw;
    c->geom.height += 2 * c->bw;

    /* Insert this client into client lists. */
    wl_list_insert(&clients, &c->link);
    wl_list_insert(&fstack, &c->flink);

    /* Set initial monitor, tags, floating status, and focus:
     * we always consider floating, clients that have parent and thus
     * we set the same tags and monitor as its parent.
     * If there is no parent, apply rules */
    if ((p = client_get_parent(c))) {
        c->isfloating = 1;
        setmon(c, p->mon, p->tags);
    } else {
        applyrules(c);
    }
    barsdirty();

unset_fullscreen:
    m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
    wl_list_for_each(w, &clients, link) {
        if (w != c && w != p && w->isfullscreen && m == w->mon && (w->tags & c->tags))
            setfullscreen(w, 0);
    }
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
    /* This event is raised when a client would like to maximize itself,
     * typically because the user clicked on the maximize button on
     * client-side decorations. awl doesn't support maximization, but
     * to conform to xdg-shell protocol we still must send a configure.
     * Since xdg-shell protocol v5 we should ignore request of unsupported
     * capabilities, just schedule a empty configure when the client uses <5
     * protocol version
     * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
    Client *c = wl_container_of(listener, c, maximize);
    if (c->surface.xdg->initialized
            && wl_resource_get_version(c->surface.xdg->toplevel->resource)
                    < XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
        wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

void
requestdecorationmode(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, set_decoration_mode);
    if (c->surface.xdg->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
                WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
    struct wlr_box *bbox;
    struct wlr_box clip;
    int old_width, old_height;

    if (!c->mon || !client_surface(c)->mapped)
        return;

    bbox = interact ? &sgeom : &c->mon->w;

    old_width = c->geom.width;
    old_height = c->geom.height;

    client_set_bounds(c, geo.width, geo.height);
    c->geom = geo;
    applybounds(c, bbox);

    /* Update scene-graph, including borders */
    wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
    wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
    /* resize() runs on every commit, not just actual resizes (e.g. every
     * redrawn frame of a client with c->blur set); only touch the blur
     * node's size when it actually changed, otherwise scenefx redoes its
     * blur-region/damage bookkeeping every single frame for no reason,
     * which is visible as flicker. */
    if (c->blur && (c->geom.width != old_width || c->geom.height != old_height)) {
        wlr_scene_blur_set_size(c->blur, c->geom.width, c->geom.height);
    }
    wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
    wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
    wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
    wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
    wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
    wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
    wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);

    /* this is a no-op if size hasn't changed */
    c->resize = client_set_size(c, c->geom.width - 2 * c->bw,
            c->geom.height - 2 * c->bw);
    client_get_clip(c, &clip);
    wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void
setfloating(Client *c, int floating)
{
    Client *p = client_get_parent(c);
    c->isfloating = floating;
    /* If in floating layout do not change the client's layer */
    if (!c->mon || !client_surface(c)->mapped || !sellayout(c->mon)->arrange)
        return;
    wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
            (p && p->isfullscreen) ? LyrFS
            : c->isfloating ? LyrFloat : LyrTile]);
    arrange(c->mon);
    barsdirty();
}

void
setfullscreen(Client *c, int fullscreen)
{
    c->isfullscreen = fullscreen;
    if (!c->mon || !client_surface(c)->mapped)
        return;
    c->bw = fullscreen ? 0 : cfg->borderpx;
    client_set_fullscreen(c, fullscreen);
    wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
            ? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

    if (fullscreen) {
        c->prev = c->geom;
        resize(c, c->mon->m, 0);
    } else {
        /* restore previous size instead of arrange for floating windows since
         * client positions are set by the user and cannot be recalculated */
        resize(c, c->prev, 0);
    }
    arrange(c->mon);
    barsdirty();
}

void
transluce(const Arg *arg)
{
    if (!selmon) return;
    if (!arg) return;
    float change = arg->i > 0 ? 0.1 : -0.1;
    Client *c = focustop(selmon);
    if (!c) return;
    c->one_minus_alpha -= change;
    c->one_minus_alpha = MAX(c->one_minus_alpha, 0.0);
    c->one_minus_alpha = MIN(c->one_minus_alpha, 1.0);
    if (!c->blur) attachblur(c);
}

void
setmon(Client *c, Monitor *m, uint32_t newtags)
{
    Monitor *oldmon = c->mon;

    if (oldmon == m)
        return;
    c->mon = m;
    c->prev = c->geom;

    /* Scene graph sends surface leave/enter events on move and resize */
    if (oldmon)
        arrange(oldmon);
    if (m) {
        /* Make sure window actually overlaps with the monitor */
        resize(c, c->geom, 0);
        newtags &= TAGMASK;
        c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
        setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
        setfloating(c, c->isfloating);
    }
    focusclient(focustop(selmon), 1);
}

void
togglebw(const Arg *arg)
{
    Client* sel = focustop(selmon);
    if (sel && !sel->isfullscreen) {
        sel->bw = sel->bw ? 0 : cfg->borderpx;
        arrange(selmon);
    }
}

void
changebw(const Arg *arg)
{
    Client* sel = focustop(selmon);
    if (sel && !sel->isfullscreen) {
        sel->bw += arg->i;
        arrange(selmon);
    }
}

void
togglefloating(const Arg *arg)
{
    Client *sel = focustop(selmon);
    /* return if fullscreen */
    if (sel && !sel->isfullscreen)
        setfloating(sel, !sel->isfloating);
}

void
togglefullscreen(const Arg *arg)
{
    Client *sel = focustop(selmon);
    if (sel)
        setfullscreen(sel, !sel->isfullscreen);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
    /* Called when the surface is unmapped, and should no longer be shown. */
    Client *c = wl_container_of(listener, c, unmap);
    if (c == grabc) {
        cursor_mode = CurNormal;
        grabc = NULL;
    }

    if (client_is_unmanaged(c)) {
        if (c == exclusive_focus) {
            exclusive_focus = NULL;
            focusclient(focustop(selmon), 1);
        }
    } else {
        wl_list_remove(&c->link);
        setmon(c, NULL, 0);
        wl_list_remove(&c->flink);
    }

    wlr_scene_node_destroy(&c->scene->node);
    barsdirty();
    motionnotify(0, NULL, 0, 0, 0, 0);
}

void
updatetitle(struct wl_listener *listener, void *data)
{
    Client *c = wl_container_of(listener, c, set_title);
    /* the task bar lists every window of its monitor's current tags */
    if (c->mon && (c->tags & c->mon->tagset[c->mon->seltags]))
        barsdirty();
}

/* marks awl's own tokens (wlr_xdg_activation_token_v1.data) */
static char activationmark;

const char *
activationtoken(void)
{
    struct wlr_xdg_activation_token_v1 *t;
    if (!activation || !(t = wlr_xdg_activation_token_v1_create(activation)))
        return NULL;
    t->data = &activationmark;
    return wlr_xdg_activation_token_v1_get_name(t);
}

void
urgent(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_activation_v1_request_activate_event *event = data;
    Client *c = NULL;
    toplevel_from_wlr_surface(event->surface, &c, NULL);
    if (!c || c == focustop(selmon))
        return;

    /* the user asked for it (activationtoken()): to the window, on its
     * monitor's view of its tags, back if it was minimized; unless the
     * config has it only urgent */
    if (event->token->data == &activationmark && c->mon && !locked
            && !cfg->notifications.activate_urgent) {
        if (!VISIBLEON(c, c->mon) && (c->tags & TAGMASK)) {
            c->mon->seltags ^= 1;
            c->mon->tagset[c->mon->seltags] = c->tags & TAGMASK;
        }
        c->isvisible = 1;
        selmon = c->mon;
        focusclient(c, 1);
        arrange(c->mon);
        barsdirty();
        return;
    }

    c->isurgent = 1;
    barsdirty();

    if (client_surface(c)->mapped)
        client_set_border_color(c, (float[])RGBA(borders[BorderUrg]));
}

void
setontop(Client* c, int ontop)
{
    c->isontop = ontop;
    if (!c->mon) return;
    wlr_scene_node_reparent(&c->scene->node, c->isontop ? layers[LyrTop] : c->isfloating ? layers[LyrFloat] : layers[LyrTile]);
    arrange(c->mon);
    barsdirty();
}

void
toggleontop(const Arg* arg)
{
    (void)arg;
    Client* sel = focustop(selmon);
    if (sel && !sel->isfullscreen) setontop(sel, !sel->isontop);
}

void
clientsetvisible(Client *c, int visible)
{
    c->isvisible = !!visible;
    if (c->mon)
        arrange(c->mon);
}

void
minimize(const Arg* arg)
{
    (void)arg;
    Client* sel = focustop(selmon);
    if (!sel) return;
    sel->isvisible = 0;
    focusclient(focustop(selmon), 1);
    arrange(sel->mon);
    barsdirty();
}

void
unminimize(const Arg* arg)
{
    Client *c = NULL;
    int found_client = 0;
    wl_list_for_each(c, &fstack, flink) {
        if (VISIBLEON(c, selmon) && !c->isvisible) {
            c->isvisible = 1;
            found_client = 1;
            break;
        }
    }
    if (!found_client) return;
    focusclient(c, 1);
    arrange(selmon);
    barsdirty();
}

void
maximize(const Arg* arg)
{
    Client* sel = focustop(selmon);
    if (!sel) return;
    sel->ismaximized = !sel->ismaximized;
    if (sel->ismaximized)
        sel->geom = sel->mon->w;
    arrange(sel->mon);
    barsdirty();
}

void
clientsconfigure(void)
{
    Monitor *m;
    Client *c, *sel = focustop(selmon);

    /* borders: those at the old default width get the new one */
    if (cfg->borderpx != applied_borderpx) {
        wl_list_for_each(c, &clients, link) {
            if (c->isfullscreen || c->bw != applied_borderpx)
                continue;
            c->bw = cfg->borderpx;
            if (c->mon)
                resize(c, c->geom, 0);
        }
        applied_borderpx = cfg->borderpx;
        wl_list_for_each(m, &mons, link)
            arrange(m);
    }

    borders[BorderNorm] = cfg->colors.border;
    borders[BorderSel] = cfg->colors.border_focused;
    borders[BorderUrg] = cfg->colors.border_urgent;
    wl_list_for_each(c, &clients, link)
        if (c->border[0])
            client_set_border_color(c, (float[])RGBA(borders[c->isurgent ? BorderUrg
                    : c == sel ? BorderSel : BorderNorm]));

    /* blur */
    wl_list_for_each(c, &clients, link) {
        if (!c->blur)
            continue;
        wlr_scene_blur_set_strength(c->blur, cfg->blur.strength);
        wlr_scene_blur_set_alpha(c->blur, cfg->blur.alpha);
    }
}
