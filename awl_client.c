#include "awl_client.h"
#include "awl_bar.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_layout.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_popup.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BORDERCLOCK_PI 3.14159265358979323846

static void apply_bounds( Client *c, struct wlr_box *bbox );
static void apply_rules( awl_server_t *srv, Client *c );
static void attach_blur( const awl_config_t *cfg, Client *c );
static struct wlr_buffer *border_clock_buffer( awl_server_t *srv, int i, uint32_t color, int len, int fade );
static void border_clock_client( awl_server_t *srv, Client *c );
static void border_clock_mark( struct wlr_scene_buffer *r[2], struct wlr_buffer *buf, struct wlr_box box, int bw,
                             double a, int len );
static void border_clock_mon( awl_server_t *srv, Monitor *m, int show );
static void border_clock_place( awl_server_t *srv, struct wlr_scene_buffer *clock[2][2], struct wlr_box box, int bw );
static int border_clock_tick( void *data );
static void commit_notify( struct wl_listener *listener, void *data );
static void commit_popup( struct wl_listener *listener, void *data );
static void destroy_popup_listener( struct wl_listener *listener, void *data );
static void destroy_decoration( struct wl_listener *listener, void *data );
static void maximize_notify( struct wl_listener *listener, void *data );
static void request_decoration_mode( struct wl_listener *listener, void *data );
static void set_on_top( awl_server_t *srv, Client* c, int on_top );

void apply_bounds( Client *c, struct wlr_box *bbox ) {
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

void scene_buffer_set_opacity( struct wlr_scene_buffer *buffer, int sx, int sy, void *data ) {
    Client *c = data;
    (void)sx; (void)sy;
    if (c && c->one_minus_alpha != 0 && c->blur) {
        struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
        if (!scene_surface) return;

        struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(scene_surface->surface);
        if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
            wlr_scene_buffer_set_opacity(buffer, 1. - c->one_minus_alpha);

            // if (!wlr_subsurface_try_from_wlr_surface(xdg_surface->surface) && c->blur) {
            //     wlr_scene_blur_set_transparency_mask_source(c->blur, buffer);
            // }
        }
    }
}

void apply_rules( awl_server_t *srv, Client *c ) {
    /* rule matching */
    uint32_t new_tags = 0;
    Monitor *mon = srv->out.sel_mon;

    const char *appid = client_get_appid(c);
    const char *title = client_get_title(c);
    awl_log_printf( "SPAWN APPID '%s' @TITLE '%s'\n", appid, title );
    struct wlr_box rbox = {0};

    for (const Rule *r = srv->cfg->rules.v; r < srv->cfg->rules.v + srv->cfg->rules.n; r++) {
        if ((!r->title || strstr(title, r->title))
                && (!r->id || strstr(appid, r->id))) {
            c->is_floating = r->is_floating;
            new_tags |= r->tags;
            if (r->monitor >= 0 && r->monitor < srv->out.nmons)
                mon = srv->out.mons[r->monitor];
            if (c->is_floating || !selected_layout(mon)->arrange) {
                /* client is floating or in floating layout */
                if (r->w != 0 && r->h != 0)
                    rbox = (struct wlr_box){ mon->w.x + mon->w.width - r->w,
                            mon->w.y + mon->w.height - r->h, r->w, r->h };
            }
            c->one_minus_alpha = r->one_minus_alpha;
            c->inhibit_idle |= r->inhibit_idle;
            if (r->blur && !c->blur) attach_blur(srv->cfg, c);
        }
    }

    c->is_floating |= client_is_float_type(c);
    set_mon(srv, c, mon, new_tags);
    if (rbox.width) resize(srv, c, rbox, 1);
}

void attach_blur( const awl_config_t *cfg, Client *c ) {
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

void commit_notify( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, commit);
    awl_server_t *srv = c->srv;
    (void)data;

    if (c->surface.xdg->initial_commit) {
        /*
         * Get the monitor this client will be rendered on
         * Note that if the user set a rule in which the client is placed on
         * a different monitor based on its title, this will likely select
         * a wrong monitor.
         */
        apply_rules(srv, c);
        if (c->mon) {
            client_set_scale(client_surface(c), c->mon->wlr_output->scale);
        }
        set_mon(srv, c, NULL, 0); /* Make sure to reapply rules in map_notify() */

        wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
                WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
        if (c->decoration)
            request_decoration_mode(&c->set_decoration_mode, c->decoration);
        wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
        return;
    }

    resize(srv, c, c->geom, (c->is_floating && !c->is_fullscreen));

    /* mark a pending resize as completed */
    if (c->resize && c->resize <= c->surface.xdg->current.configure_serial)
        c->resize = 0;
}

void commit_popup( struct wl_listener *listener, void *data ) {
    struct wlr_surface *surface = data;
    struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
    LayerSurface *l = NULL;
    Client *c = NULL;
    (void)listener;

    if (!popup || !popup->base->initial_commit)
        return;

    int type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
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
        /* l->popups (not l->scene) is the tree commit_layer_surface_notify()
         * and create_layer_surface() deliberately keep pinned to LyrTop (or
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

    struct wlr_box box = type == LayerShell ? l->mon->m : c->mon->w;
    box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x);
    box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y);
    wlr_xdg_popup_unconstrain_from_box(popup, &box);

    // do *NOT* free/remove from list, because there's a listener for that
    // (multiple allocation inhibited through the !popup->base->initial_commit
    // check
}

void create_decoration( struct wl_listener *listener, void *data ) {
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    Client *c = deco->toplevel->base->data;
    (void)listener;
    c->decoration = deco;

    LISTEN(&deco->events.request_mode, &c->set_decoration_mode, request_decoration_mode);
    LISTEN(&deco->events.destroy, &c->destroy_decoration, destroy_decoration);

    request_decoration_mode(&c->set_decoration_mode, deco);
}

void create_notify( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_xdg_toplevel);
    /* This event is raised when a client creates a new toplevel (application window). */
    struct wlr_xdg_toplevel *toplevel = data;
    Client *c = NULL;

    /* Allocate a Client for this surface */
    c = toplevel->base->data = calloc_or_die(1, sizeof(*c));
    c->srv = srv;
    c->surface.xdg = toplevel->base;
    c->bw = srv->cfg->border_px;
    c->is_visible = 1;

    LISTEN(&toplevel->base->surface->events.commit, &c->commit, commit_notify);
    LISTEN(&toplevel->base->surface->events.map, &c->map, map_notify);
    LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmap_notify);
    LISTEN(&toplevel->events.destroy, &c->destroy, destroy_notify);
    LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreen_notify);
    LISTEN(&toplevel->events.request_maximize, &c->maximize, maximize_notify);
    LISTEN(&toplevel->events.set_title, &c->set_title, update_title);
}

// helper struct to free the dynamical listener upon surface destruction
struct popup_listener {
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void destroy_popup_listener( struct wl_listener *listener, void *data ) {
    struct popup_listener *p_listener = wl_container_of(listener, p_listener, destroy);
    (void)data;
    wl_list_remove(&p_listener->commit.link);
    wl_list_remove(&p_listener->destroy.link);
    free(p_listener);
}

void create_popup( struct wl_listener *listener, void *data ) {
    struct wlr_xdg_popup *popup = data;
    (void)listener;

    struct popup_listener *p_listener = calloc(1, sizeof*p_listener);
    if (!p_listener) return;

    p_listener->commit.notify = commit_popup;
    wl_signal_add(&popup->base->surface->events.commit, &p_listener->commit);

    p_listener->destroy.notify = destroy_popup_listener;
    wl_signal_add(&popup->base->surface->events.destroy, &p_listener->destroy);
}

void destroy_decoration( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, destroy_decoration);
    (void)data;

    wl_list_remove(&c->destroy_decoration.link);
    wl_list_remove(&c->set_decoration_mode.link);
}

void destroy_notify( struct wl_listener *listener, void *data ) {
    /* Called when the xdg_toplevel is destroyed. */
    Client *c = wl_container_of(listener, c, destroy);
    (void)data;
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

void focus_client( awl_server_t *srv, Client *c, int lift ) {
    struct wlr_surface *old = srv->seat->keyboard_state.focused_surface;
    int old_client_type;
    Client *old_c = NULL;
    LayerSurface *old_l = NULL;

    if (srv->locked)
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
        wl_list_remove(&c->focus_link);
        wl_list_insert(&srv->clients.focus_order, &c->focus_link);
        srv->out.sel_mon = c->mon;
        c->is_urgent = 0;

        /* Don't change border color if there is an exclusive focus or we are
         * handling a drag operation */
        if (!srv->clients.exclusive_focus && !srv->seat->drag)
            client_set_border_color(c, (float[])RGBA(srv->clients.borders[BorderSel]));
    }

    /* Deactivate old client if focus is changing */
    if (old && (!c || client_surface(c) != old)) {
        int unused_lx, unused_ly;
        /* If an overlay is focused, don't focus or activate the client,
         * but only update its position in focus_order to render its border with its color
         * and focus it after the overlay is closed. */
        if (old_client_type == LayerShell && wlr_scene_node_coords(
                    &old_l->scene->node, &unused_lx, &unused_ly)
                && old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
            return;
        } else if (old_c && old_c == srv->clients.exclusive_focus && client_wants_focus(old_c)) {
            return;
        /* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
         * and probably other clients */
        } else if (old_c && !client_is_unmanaged(old_c) && (!c || !client_wants_focus(c))) {
            client_set_border_color(old_c, (float[])RGBA(srv->clients.borders[BorderNorm]));
            client_activate_surface(old, 0);
        }
    }
    bars_dirty(srv);
    border_clock(srv);

    if (!c) {
        /* With no client, all we have left is to clear focus */
        wlr_seat_keyboard_notify_clear_focus(srv->seat);
        return;
    }

    /* a popup has the keyboard (the launcher): c gets it once that is
     * closed (popups_grab_update()) */
    if (popup_keyboard(srv))
        return;

    /* Change cursor surface */
    motion_notify(srv, 0, NULL, 0, 0, 0, 0);

    /* Have a client, so focus its top-level wlr_surface */
    client_notify_enter(srv, client_surface(c), wlr_seat_get_keyboard(srv->seat));

    /* Activate the new client */
    client_activate_surface(client_surface(c), 1);
}

void focus_stack( awl_server_t *srv, const Arg *arg ) {
    /* Focus the next or previous client (in tiling order) on sel_mon */
    Client *c, *sel = focus_top(srv, srv->out.sel_mon);
    if (!sel || (sel->is_fullscreen && !client_has_children(sel)))
        return;
    if (arg->i > 0) {
        wl_list_for_each(c, &sel->link, link) {
            if (&c->link == &srv->clients.list)
                continue; /* wrap past the sentinel node */
            if (VISIBLE_ON_ACTIVE(c, srv->out.sel_mon))
                break; /* found it */
        }
    } else {
        wl_list_for_each_reverse(c, &sel->link, link) {
            if (&c->link == &srv->clients.list)
                continue; /* wrap past the sentinel node */
            if (VISIBLE_ON_ACTIVE(c, srv->out.sel_mon))
                break; /* found it */
        }
    }
    /* If only one client is visible on sel_mon, then c == sel */
    focus_client(srv, c, 1);
}

void move_stack( awl_server_t *srv, const Arg *arg ) {
    Client *c, *sel = focus_top(srv, srv->out.sel_mon);

    if (!sel || wl_list_length(&srv->clients.list) <= 1)
        return;

    if (arg->i > 0) {
        wl_list_for_each(c, &sel->link, link) {
            if (&c->link == &srv->clients.list) {
                c = wl_container_of(&srv->clients.list, c, link);
                break; /* wrap past the sentinel node */
            }
            if (VISIBLE_ON(c, srv->out.sel_mon) || &c->link == &srv->clients.list) {
                break; /* found it */
            }
        }
    } else {
        wl_list_for_each_reverse(c, &sel->link, link) {
            if (&c->link == &srv->clients.list) {
                c = wl_container_of(&srv->clients.list, c, link);
                break; /* wrap past the sentinel node */
            }
            if (VISIBLE_ON(c, srv->out.sel_mon) || &c->link == &srv->clients.list) {
                break; /* found it */
            }
        }
        /* backup one client */
        c = wl_container_of(c->link.prev, c, link);
    }

    wl_list_remove(&sel->link);
    wl_list_insert(&c->link, &sel->link);
    arrange(srv, srv->out.sel_mon);
}

Client *focus_top( awl_server_t *srv, Monitor *m ) {
    Client *c;
    wl_list_for_each(c, &srv->clients.focus_order, focus_link) {
        if (VISIBLE_ON_ACTIVE(c, m))
            return c;
    }
    return NULL;
}

void fullscreen_notify( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, fullscreen);
    awl_server_t *srv = c->srv;
    (void)data;
    set_fullscreen(srv, c, client_wants_fullscreen(c));
}

void kill_client( awl_server_t *srv, const Arg *arg ) {
    /* arg->i 0: asks the window to close, which a hung client never answers;
     * else: kills the client's process (SIGKILL), its windows go with it */
    Client *sel = focus_top(srv, srv->out.sel_mon);
    pid_t pid = 0;
    if (!sel)
        return;
    if (!arg->i) {
        client_send_close(sel);
        return;
    }
#ifdef XWAYLAND
    if (client_is_x11(sel)) {
        /* _NET_WM_PID or XRes, 0 if unknown; never Xwayland itself, that
         * would take every X11 window along */
        pid = sel->surface.xwayland->pid;
        if (srv->xwayland && srv->xwayland->server && pid == srv->xwayland->server->pid)
            pid = 0;
        if (pid <= 1 || pid == getpid()) {
            /* no process to kill: have the X server drop the window's
             * client (xkill), on a connection of our own for just that,
             * flushed, not waited for */
            xcb_connection_t *x = NULL;
            if (srv->xwayland && srv->xwayland->display_name)
                x = xcb_connect(srv->xwayland->display_name, NULL);
            if (!x || xcb_connection_has_error(x)) {
                awl_wrn_printf("kill_client: can't connect to Xwayland to kill \"%s\"\n",
                        client_get_title(sel));
            } else {
                awl_log_printf("kill_client: killing the X11 client of \"%s\"\n",
                        client_get_title(sel));
                xcb_kill_client(x, sel->surface.xwayland->window_id);
                xcb_flush(x);
            }
            if (x)
                xcb_disconnect(x);
            return;
        }
    } else
#endif
    {
        struct wl_client *wc = sel->surface.xdg->client->client;
        wl_client_get_credentials(wc, &pid, NULL, NULL);
        if (pid <= 1 || pid == getpid()) {
            /* not a process of its own: disconnect it instead */
            wl_client_destroy(wc);
            return;
        }
    }
    awl_log_printf("kill_client: killing %d (\"%s\")\n", (int)pid, client_get_title(sel));
    kill(pid, arg->i < 0 ? -arg->i : SIGKILL);
}

void map_notify( struct wl_listener *listener, void *data ) {
    /* Called when the surface is mapped, or ready to display on-screen. */
    Client *p = NULL;
    Client *w, *c = wl_container_of(listener, c, map);
    awl_server_t *srv = c->srv;
    Monitor *m;
    (void)data;

    /* Create scene tree for this client and its border */
    c->scene = client_surface(c)->data = wlr_scene_tree_create(srv->layers[LyrTile]);
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
        wlr_scene_node_reparent(&c->scene->node, srv->layers[LyrFloat]);
        wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
        client_set_size(c, c->geom.width, c->geom.height);
        if (client_wants_focus(c)) {
            focus_client(srv, c, 1);
            srv->clients.exclusive_focus = c;
        }
        goto unset_fullscreen;
    }

    for (int i = 0; i < 4; i++) {
        c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
            (float[])RGBA(srv->clients.borders[c->is_urgent ? BorderUrg : BorderNorm]));
        c->border[i]->node.data = c;
    }
    for (int i = 0; i < 4; i++) {
        struct wlr_scene_buffer **r = &c->clock[i / 2][i % 2];
        *r = wlr_scene_buffer_create(c->scene, NULL);
        (*r)->node.data = c;
        wlr_scene_node_set_enabled(&(*r)->node, 0);
    }

    /* Initialize client geometry with room for border */
    client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
    c->geom.width += 2 * c->bw;
    c->geom.height += 2 * c->bw;

    /* Insert this client into client lists. */
    wl_list_insert(&srv->clients.list, &c->link);
    wl_list_insert(&srv->clients.focus_order, &c->focus_link);

    /* Set initial monitor, tags, floating status, and focus:
     * we always consider floating, clients that have parent and thus
     * we set the same tags and monitor as its parent.
     * If there is no parent, apply rules */
    if ((p = client_get_parent(c))) {
        c->is_floating = 1;
        set_mon(srv, c, p->mon, p->tags);
    } else {
        apply_rules(srv, c);
    }
    bars_dirty(srv);

unset_fullscreen:
    m = c->mon ? c->mon : xy_to_mon(srv, c->geom.x, c->geom.y);
    wl_list_for_each(w, &srv->clients.list, link) {
        if (w != c && w != p && w->is_fullscreen && m == w->mon && (w->tags & c->tags))
            set_fullscreen(srv, w, 0);
    }
}

void maximize_notify( struct wl_listener *listener, void *data ) {
    /* This event is raised when a client would like to maximize itself,
     * typically because the user clicked on the maximize button on
     * client-side decorations. awl doesn't support maximization, but
     * to conform to xdg-shell protocol we still must send a configure.
     * Since xdg-shell protocol v5 we should ignore request of unsupported
     * capabilities, just schedule a empty configure when the client uses <5
     * protocol version
     * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
    Client *c = wl_container_of(listener, c, maximize);
    (void)data;
    if (c->surface.xdg->initialized
            && wl_resource_get_version(c->surface.xdg->toplevel->resource)
                    < XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
        wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

void request_decoration_mode( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, set_decoration_mode);
    (void)data;
    if (c->surface.xdg->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
                WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/* a border clock mark's image: one row, read-only for the renderer */
typedef struct {
    struct wlr_buffer base;
    uint32_t data[];
} ClockBuffer;

static void clock_buffer_destroy( struct wlr_buffer *wb ) {
    ClockBuffer *b = wl_container_of(wb, b, base);
    free(b);
}

static bool clock_buffer_begin( struct wlr_buffer *wb, uint32_t flags, void **data, uint32_t *format, size_t *stride ) {
    ClockBuffer *b = wl_container_of(wb, b, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;
    *data = b->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)wb->width * 4;
    return true;
}

static void clock_buffer_end( struct wlr_buffer *wb ) {
    (void)wb;
}

static const struct wlr_buffer_impl clockbuffer_impl = {
    .destroy = clock_buffer_destroy,
    .begin_data_ptr_access = clock_buffer_begin,
    .end_data_ptr_access = clock_buffer_end,
};

/* srv's image for mark i (0 the hour, 1 the minute): len pixels in color
 * (0xRRGGBBAA), the last fade at either end fading out, the outermost the
 * faintest; premultiplied. Made again only when one of those changed;
 * NULL if out of memory. */
static struct wlr_buffer *border_clock_buffer( awl_server_t *srv, int i, uint32_t color, int len, int fade ) {
    awl_clients_t *cl = &srv->clients;
    uint32_t key[3] = {color, (uint32_t)len, (uint32_t)fade};
    double a = (color & 0xFF) / 255.0;
    ClockBuffer *b;

    if (cl->clock_buf[i] && !memcmp(cl->clock_buf_key[i], key, sizeof(key)))
        return cl->clock_buf[i];
    if (!(b = calloc(1, sizeof(*b) + (size_t)len * 4)))
        return NULL;
    wlr_buffer_init(&b->base, &clockbuffer_impl, len, 1);
    for (int x = 0; x < len; x++) {
        int edge = MIN(x, len - 1 - x);
        double k = edge < fade ? a * (edge + 0.5) / fade : a;
        uint32_t px = (uint32_t)lround(k * 255) << 24;
        for (int j = 1; j < 4; j++) /* r, g, b */
            px |= (uint32_t)lround(((color >> (8 * j)) & 0xFF) * k) << (8 * (j - 1));
        b->data[x] = px;
    }
    if (cl->clock_buf[i])
        wlr_buffer_drop(cl->clock_buf[i]);
    cl->clock_buf[i] = &b->base;
    memcpy(cl->clock_buf_key[i], key, sizeof(key));
    return cl->clock_buf[i];
}

/* r, a mark len long (logical pixels) where a hand at angle a (radians,
 * clockwise from twelve o'clock) from the middle of box crosses its edge, bw
 * wide (inside box), showing buf (a row, stretched to len, turned along the
 * edge). The border goes round clockwise from the top left corner in four
 * pieces, each owning the corner it starts at: the top, the right, the
 * bottom, the left; a mark reaching a corner goes on round it in r[1].
 * box is in r's parent's coordinates. */
static void border_clock_mark( struct wlr_scene_buffer *r[2], struct wlr_buffer *buf, struct wlr_box box, int bw,
                             double a, int len ) {
    static const enum wl_output_transform tr[4] = {WL_OUTPUT_TRANSFORM_NORMAL,
            WL_OUTPUT_TRANSFORM_90, WL_OUTPUT_TRANSFORM_180, WL_OUTPUT_TRANSFORM_270};
    int w = box.width, h = box.height, el[4] = {w - bw, h - bw, w - bw, h - bw};
    int per = 2 * (el[0] + el[1]), e, u, o, l, i, x, y;
    double dx = sin(a), dy = -cos(a), hw = w / 2.0, hh = h / 2.0;
    double tx = fabs(dx) > 1e-9 ? hw / fabs(dx) : INFINITY;
    double ty = fabs(dy) > 1e-9 ? hh / fabs(dy) : INFINITY;
    double s;

    len = MIN(len, MIN(el[0], el[1]));
    if (len <= 0 || bw <= 0) {
        for (i = 0; i < 2; i++)
            wlr_scene_node_set_enabled(&r[i]->node, 0);
        return;
    }
    /* the crossing's distance along the border, each side's own length
     * squeezed into its piece's */
    if (ty <= tx)
        s = dy < 0 ? (hw + ty * dx) * el[0] / w
                : el[0] + el[1] + (hw - ty * dx) * el[2] / w;
    else
        s = dx > 0 ? el[0] + (hh + tx * dy) * el[1] / h
                : 2 * el[0] + el[1] + (hh - tx * dy) * el[3] / h;
    o = (int)lround(s - len / 2.0);
    o = (o % per + per) % per;
    for (e = 0; o >= el[e]; e++)
        o -= el[e];
    double f = buf ? buf->width / (double)len : 1;

    /* u: where on side e the piece starts, o: where in the mark */
    for (i = 0, u = o, o = 0; i < 2; i++, e = (e + 1) % 4, u = 0) {
        l = MIN(len - o, el[e] - u);
        wlr_scene_node_set_enabled(&r[i]->node, l > 0);
        if (l <= 0)
            continue;
        if (r[i]->buffer != buf)
            wlr_scene_buffer_set_buffer(r[i], buf);
        wlr_scene_buffer_set_source_box(r[i], &(struct wlr_fbox){o * f, 0, l * f, 1});
        wlr_scene_buffer_set_transform(r[i], tr[e]);
        switch (e) {
        case 0: x = u; y = 0; break;
        case 1: x = w - bw; y = u; break;
        case 2: x = w - u - l; y = h - bw; break;
        default: x = 0; y = h - u - l; break;
        }
        wlr_scene_buffer_set_dest_size(r[i], e % 2 ? bw : l, e % 2 ? l : bw);
        wlr_scene_node_set_position(&r[i]->node, box.x + x, box.y + y);
        o += l;
    }
}

/* the marks clock[0] (the hour) and clock[1] (the minute) on box's edge at
 * the current time, in the configured colors */
static void border_clock_place( awl_server_t *srv, struct wlr_scene_buffer *clock[2][2], struct wlr_box box, int bw ) {
    const awl_config_t *cfg = srv->cfg;
    /* the hands' seconds per turn */
    static const double turn[2] = {12 * 3600, 3600};
    struct timespec ts;
    struct tm tm;
    int len[2] = {cfg->border_clock.hour_len, cfg->border_clock.minute_len};
    uint32_t color[2] = {cfg->colors.border_clock_hour, cfg->colors.border_clock_minute};

    clock_gettime(CLOCK_REALTIME, &ts);
    if (!localtime_r(&ts.tv_sec, &tm))
        return;
    double sec = tm.tm_min * 60 + tm.tm_sec + ts.tv_nsec / 1e9;
    for (int i = 0; i < 2; i++) {
        double a = ((i ? 0 : tm.tm_hour % 12 * 3600) + sec) / turn[i] * (2 * BORDERCLOCK_PI);
        border_clock_mark(clock[i], border_clock_buffer(srv, i, color[i], len[i],
                MAX(0, MIN(cfg->border_clock.fade, len[i] / 2))), box, bw, a, len[i]);
    }
}

/* c's own marks, at its size */
static void border_clock_client( awl_server_t *srv, Client *c ) {
    border_clock_place(srv, c->clock, (struct wlr_box){0, 0, c->geom.width, c->geom.height}, c->bw);
}

/* m's marks on its window area's edge, or hidden: off, or no window shown
 * there and not border_clock.screen_empty */
static void border_clock_mon( awl_server_t *srv, Monitor *m, int show ) {
    int any = srv->cfg->border_clock.screen_empty;

    if (show && !any) {
        Client *c;
        wl_list_for_each(c, &srv->clients.list, link) {
            if (VISIBLE_ON_ACTIVE(c, m)) {
                any = 1;
                break;
            }
        }
    }
    show = show && any;
    if (show && !m->clock[0][0])
        for (int i = 0; i < 4; i++)
            m->clock[i / 2][i % 2] = wlr_scene_buffer_create(srv->layers[LyrTile], NULL);
    if (!m->clock[0][0])
        return;
    for (int i = 0; i < 4; i++)
        wlr_scene_node_set_enabled(&m->clock[i / 2][i % 2]->node, show);
    if (!show)
        return;
    border_clock_place(srv, m->clock, m->w, (int)srv->cfg->border_px);
    /* right above the borders, which raising a window would cover */
    for (int i = 0; i < 4; i++)
        wlr_scene_node_raise_to_top(&m->clock[i / 2][i % 2]->node);
}

void border_clock_mode( awl_server_t *srv, const Arg *arg ) {
    int cur = srv->clients.clock_mode_set ? srv->clients.clock_mode
            : (int)srv->cfg->border_clock.mode;
    int mode = arg->i < 0 ? (cur + 1) % (BorderClockScreen + 1) : arg->i;

    if (mode > BorderClockScreen)
        return;
    srv->clients.clock_mode = mode;
    srv->clients.clock_mode_set = 1;
    border_clock(srv);
}

static int border_clock_tick( void *data ) {
    border_clock(data);
    return 0;
}

void border_clock( awl_server_t *srv ) {
    const awl_config_t *cfg = srv->cfg;
    enum awl_border_clock_mode mode = srv->clients.clock_mode_set
            ? (enum awl_border_clock_mode)srv->clients.clock_mode : cfg->border_clock.mode;
    Client *c, *sel = mode == BorderClockFocused ? focus_top(srv, srv->out.sel_mon) : NULL;
    Monitor *m;
    struct timespec ts;

    /* off, and hidden everywhere already */
    if (mode == BorderClockOff && !srv->clients.clock_on)
        return;
    srv->clients.clock_on = mode != BorderClockOff;

    wl_list_for_each(c, &srv->clients.list, link) {
        if (!c->clock[0][0])
            continue;
        int on = c == sel; /* without a border, 0 wide */
        for (int i = 0; i < 4; i++)
            wlr_scene_node_set_enabled(&c->clock[i / 2][i % 2]->node, on);
        if (on)
            border_clock_client(srv, c);
    }
    FOR_EACH_MON(m, srv)
        border_clock_mon(srv, m, mode == BorderClockScreen && m->wlr_output->enabled);

    if (mode == BorderClockOff) {
        if (srv->clients.clock_timer)
            wl_event_source_timer_update(srv->clients.clock_timer, 0);
        return;
    }
    if (!srv->clients.clock_timer)
        srv->clients.clock_timer = wl_event_loop_add_timer(srv->event_loop, border_clock_tick, srv);
    if (!srv->clients.clock_timer)
        return;
    /* just past the next minute; at least every 15 s, as the timer's clock
     * stands still while suspended */
    clock_gettime(CLOCK_REALTIME, &ts);
    long ms = 60000 - (ts.tv_sec % 60) * 1000 - ts.tv_nsec / 1000000 + 50;

    wl_event_source_timer_update(srv->clients.clock_timer, (int)MIN(ms, cfg->border_clock.min_update_time_ms));
}

void resize( awl_server_t *srv, Client *c, struct wlr_box geo, int interact ) {
    struct wlr_box clip;

    if (!c->mon || !client_surface(c)->mapped)
        return;

    struct wlr_box *bbox = interact ? &srv->out.screen_geom : &c->mon->w;

    int old_width = c->geom.width;
    int old_height = c->geom.height;

    client_set_bounds(c, geo.width, geo.height);
    c->geom = geo;
    apply_bounds(c, bbox);

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
    if (c->clock[0][0] && c->clock[0][0]->node.enabled)
        border_clock_client(srv, c);

    /* this is a no-op if size hasn't changed */
    uint32_t serial = client_set_size(c, c->geom.width - 2 * c->bw,
            c->geom.height - 2 * c->bw);
    /* the oldest unanswered one: render_mon() waits for it only so long */
    if (serial && !c->resize) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        c->resize_sent = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        c->resize_late = 0;
    }
    c->resize = serial;
    client_get_clip(c, &clip);
    wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void set_floating( awl_server_t *srv, Client *c, int floating ) {
    Client *p = client_get_parent(c);
    c->is_floating = floating;
    /* If in floating layout do not change the client's layer */
    if (!c->mon || !client_surface(c)->mapped || !selected_layout(c->mon)->arrange)
        return;
    wlr_scene_node_reparent(&c->scene->node, srv->layers[c->is_fullscreen ||
            (p && p->is_fullscreen) ? LyrFS
            : c->is_floating ? LyrFloat : LyrTile]);
    arrange(srv, c->mon);
    bars_dirty(srv);
}

void set_fullscreen( awl_server_t *srv, Client *c, int fullscreen ) {
    c->is_fullscreen = fullscreen;
    if (!c->mon || !client_surface(c)->mapped)
        return;
    c->bw = fullscreen ? 0 : srv->cfg->border_px;
    client_set_fullscreen(c, fullscreen);
    wlr_scene_node_reparent(&c->scene->node, srv->layers[c->is_fullscreen
            ? LyrFS : c->is_floating ? LyrFloat : LyrTile]);

    if (fullscreen) {
        c->prev = c->geom;
        resize(srv, c, c->mon->m, 0);
    } else {
        /* restore previous size instead of arrange for floating windows since
         * client positions are set by the user and cannot be recalculated */
        resize(srv, c, c->prev, 0);
    }
    arrange(srv, c->mon);
    bars_dirty(srv);
}

void change_opacity( awl_server_t *srv, const Arg *arg ) {
    if (!srv->out.sel_mon) return;
    if (!arg) return;
    float change = arg->i > 0 ? 0.1 : -0.1;
    Client *c = focus_top(srv, srv->out.sel_mon);
    if (!c) return;
    c->one_minus_alpha -= change;
    c->one_minus_alpha = MAX(c->one_minus_alpha, 0.0);
    c->one_minus_alpha = MIN(c->one_minus_alpha, 1.0);
    if (!c->blur) attach_blur(srv->cfg, c);
}

void set_mon( awl_server_t *srv, Client *c, Monitor *m, uint32_t new_tags ) {
    Monitor *old_mon = c->mon;

    if (old_mon == m)
        return;
    c->mon = m;
    c->prev = c->geom;

    /* Scene graph sends surface leave/enter events on move and resize */
    if (old_mon)
        arrange(srv, old_mon);
    if (m) {
        /* Make sure window actually overlaps with the monitor */
        resize(srv, c, c->geom, 0);
        new_tags &= TAG_MASK(srv->cfg);
        c->tags = new_tags ? new_tags : m->tagset[m->sel_tags]; /* assign tags of target monitor */
        set_fullscreen(srv, c, c->is_fullscreen); /* This will call arrange(c->mon) */
        set_floating(srv, c, c->is_floating);
    }
    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
}

void toggle_border( awl_server_t *srv, const Arg *arg ) {
    Client* sel = focus_top(srv, srv->out.sel_mon);
    (void)arg;
    if (sel && !sel->is_fullscreen) {
        sel->bw = sel->bw ? 0 : srv->cfg->border_px;
        arrange(srv, srv->out.sel_mon);
    }
}

void change_border_width( awl_server_t *srv, const Arg *arg ) {
    Client* sel = focus_top(srv, srv->out.sel_mon);
    if (sel && !sel->is_fullscreen) {
        sel->bw += arg->i;
        arrange(srv, srv->out.sel_mon);
    }
}

void toggle_floating( awl_server_t *srv, const Arg *arg ) {
    Client *sel = focus_top(srv, srv->out.sel_mon);
    (void)arg;
    /* return if fullscreen */
    if (sel && !sel->is_fullscreen)
        set_floating(srv, sel, !sel->is_floating);
}

void toggle_fullscreen( awl_server_t *srv, const Arg *arg ) {
    Client *sel = focus_top(srv, srv->out.sel_mon);
    (void)arg;
    if (sel)
        set_fullscreen(srv, sel, !sel->is_fullscreen);
}

void unmap_notify( struct wl_listener *listener, void *data ) {
    /* Called when the surface is unmapped, and should no longer be shown. */
    Client *c = wl_container_of(listener, c, unmap);
    awl_server_t *srv = c->srv;
    (void)data;
    if (c == srv->cursor.grab_client) {
        srv->cursor.mode = CurNormal;
        srv->cursor.grab_client = NULL;
    }

    if (client_is_unmanaged(c)) {
        if (c == srv->clients.exclusive_focus) {
            srv->clients.exclusive_focus = NULL;
            focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
        }
    } else {
        wl_list_remove(&c->link);
        set_mon(srv, c, NULL, 0);
        wl_list_remove(&c->focus_link);
    }

    wlr_scene_node_destroy(&c->scene->node);
    bars_dirty(srv);
    motion_notify(srv, 0, NULL, 0, 0, 0, 0);
}

void update_title( struct wl_listener *listener, void *data ) {
    Client *c = wl_container_of(listener, c, set_title);
    awl_server_t *srv = c->srv;
    (void)data;
    /* the task bar lists every window of its monitor's current tags */
    if (c->mon && (c->tags & c->mon->tagset[c->mon->sel_tags]))
        bars_dirty(srv);
}

/* marks awl's own tokens (wlr_xdg_activation_token_v1.data) */
static const char activation_mark;

const char *activation_token( awl_server_t *srv ) {
    struct wlr_xdg_activation_token_v1 *t;
    if (!srv->activation || !(t = wlr_xdg_activation_token_v1_create(srv->activation)))
        return NULL;
    t->data = (void *)&activation_mark;
    return wlr_xdg_activation_token_v1_get_name(t);
}

void urgent( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.request_activate);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;
    Client *c = NULL;
    toplevel_from_wlr_surface(event->surface, &c, NULL);
    if (!c || c == focus_top(srv, srv->out.sel_mon))
        return;

    /* the user asked for it (activation_token()): to the window, on its
     * monitor's view of its tags, back if it was minimized; unless the
     * config has it only urgent */
    if (event->token->data == &activation_mark && c->mon && !srv->locked
            && !srv->cfg->notifications.activate_urgent) {
        if (!VISIBLE_ON(c, c->mon) && (c->tags & TAG_MASK(srv->cfg))) {
            c->mon->sel_tags ^= 1;
            c->mon->tagset[c->mon->sel_tags] = c->tags & TAG_MASK(srv->cfg);
        }
        c->is_visible = 1;
        srv->out.sel_mon = c->mon;
        focus_client(srv, c, 1);
        arrange(srv, c->mon);
        bars_dirty(srv);
        return;
    }

    c->is_urgent = 1;
    bars_dirty(srv);

    if (client_surface(c)->mapped)
        client_set_border_color(c, (float[])RGBA(srv->clients.borders[BorderUrg]));
}

void set_on_top( awl_server_t *srv, Client* c, int on_top ) {
    c->is_on_top = on_top;
    if (!c->mon) return;
    wlr_scene_node_reparent(&c->scene->node, c->is_on_top ? srv->layers[LyrTop] : c->is_floating ? srv->layers[LyrFloat] : srv->layers[LyrTile]);
    arrange(srv, c->mon);
    bars_dirty(srv);
}

void toggle_on_top( awl_server_t *srv, const Arg* arg ) {
    (void)arg;
    Client* sel = focus_top(srv, srv->out.sel_mon);
    if (sel && !sel->is_fullscreen) set_on_top(srv, sel, !sel->is_on_top);
}

void client_set_visible( awl_server_t *srv, Client *c, int visible ) {
    c->is_visible = !!visible;
    if (c->mon)
        arrange(srv, c->mon);
}

void minimize( awl_server_t *srv, const Arg* arg ) {
    (void)arg;
    Client* sel = focus_top(srv, srv->out.sel_mon);
    if (!sel) return;
    sel->is_visible = 0;
    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    arrange(srv, sel->mon);
    bars_dirty(srv);
}

void unminimize( awl_server_t *srv, const Arg* arg ) {
    Client *c = NULL;
    int found_client = 0;
    (void)arg;
    wl_list_for_each(c, &srv->clients.focus_order, focus_link) {
        if (VISIBLE_ON(c, srv->out.sel_mon) && !c->is_visible) {
            c->is_visible = 1;
            found_client = 1;
            break;
        }
    }
    if (!found_client) return;
    focus_client(srv, c, 1);
    arrange(srv, srv->out.sel_mon);
    bars_dirty(srv);
}

void maximize( awl_server_t *srv, const Arg* arg ) {
    Client* sel = focus_top(srv, srv->out.sel_mon);
    (void)arg;
    if (!sel) return;
    sel->is_maximized = !sel->is_maximized;
    if (sel->is_maximized)
        sel->geom = sel->mon->w;
    arrange(srv, sel->mon);
    bars_dirty(srv);
}

void clients_configure( awl_server_t *srv ) {
    Client *c, *sel = focus_top(srv, srv->out.sel_mon);

    /* borders: those at the old default width get the new one */
    if (srv->cfg->border_px != srv->clients.applied_border_px) {
        Monitor *m;
        wl_list_for_each(c, &srv->clients.list, link) {
            if (c->is_fullscreen || c->bw != srv->clients.applied_border_px)
                continue;
            c->bw = srv->cfg->border_px;
            if (c->mon)
                resize(srv, c, c->geom, 0);
        }
        srv->clients.applied_border_px = srv->cfg->border_px;
        FOR_EACH_MON(m, srv)
            arrange(srv, m);
    }

    srv->clients.borders[BorderNorm] = srv->cfg->colors.border;
    srv->clients.borders[BorderSel] = srv->cfg->colors.border_focused;
    srv->clients.borders[BorderUrg] = srv->cfg->colors.border_urgent;
    wl_list_for_each(c, &srv->clients.list, link)
        if (c->border[0])
            client_set_border_color(c, (float[])RGBA(srv->clients.borders[c->is_urgent ? BorderUrg
                    : c == sel ? BorderSel : BorderNorm]));

    /* blur */
    wl_list_for_each(c, &srv->clients.list, link) {
        if (!c->blur)
            continue;
        wlr_scene_blur_set_strength(c->blur, srv->cfg->blur.strength);
        wlr_scene_blur_set_alpha(c->blur, srv->cfg->blur.alpha);
    }

    /* the config's border clock mode again */
    srv->clients.clock_mode_set = 0;
    border_clock(srv);
}
