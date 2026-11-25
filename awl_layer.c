#include "awl_layer.h"
#include "awl_alert.h"
#include "awl_background.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_desktop.h"
#include "awl_layout.h"
#include "awl_lockscreen.h"
#include "awl_output.h"
#include "awl_popup.h"

static void arrange_layer( Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive );
static void commit_layer_surface_notify( struct wl_listener *listener, void *data );
static void destroy_layer_surface_notify( struct wl_listener *listener, void *data );
static void unmap_layer_surface_notify( struct wl_listener *listener, void *data );

/** Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
static const int layer_map[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };

void arrange_layer( Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive ) {
    LayerSurface *l;
    struct wlr_box full_area = m->m;

    wl_list_for_each(l, list, link) {
        struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

        if (!layer_surface->initialized)
            continue;

        if (exclusive != (layer_surface->current.exclusive_zone > 0))
            continue;

        wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area, usable_area);
        wlr_scene_node_set_position(&l->popups->node, l->scene->node.x, l->scene->node.y);
    }
}

void arrange_layers( awl_server_t *srv, Monitor *m ) {
    struct wlr_box usable_area = m->m;
    LayerSurface *l;
    uint32_t layers_above_shell[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
    };
    if (!m->wlr_output->enabled)
        return;

    if (m->show_bar) {
        usable_area.height -= m->bar_area.real_height;
        usable_area.y += srv->cfg->bar.top ? m->bar_area.real_height : 0;
    }

    /* Arrange exclusive surfaces from top->bottom */
    for (int i = 3; i >= 0; i--)
        arrange_layer(m, &m->layers[i], &usable_area, 1);

    if (!wlr_box_equal(&usable_area, &m->w)) {
        m->w = usable_area;
        arrange(srv, m);
    }

    /* Arrange non-exlusive surfaces from top->bottom */
    for (int i = 3; i >= 0; i--)
        arrange_layer(m, &m->layers[i], &usable_area, 0);

    desktop_update(srv->desktop, m);
    background_update(srv->background, m);
    awl_alert_draw(srv, m);
    popups_update(srv, m);

    /* Find topmost keyboard interactive layer, if such a layer exists */
    for (int i = 0; i < (int)LENGTH(layers_above_shell); i++) {
        wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
            if (srv->locked || !l->layer_surface->current.keyboard_interactive || !l->mapped)
                continue;
            focus_client(srv, NULL, 0);
            srv->clients.exclusive_focus = l;
            client_notify_enter(srv, l->layer_surface->surface, wlr_seat_get_keyboard(srv->seat));
            return;
        }
    }
}

void commit_layer_surface_notify( struct wl_listener *listener, void *data ) {
    LayerSurface *l = wl_container_of(listener, l, surface_commit);
    awl_server_t *srv = l->srv;
    struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
    struct wlr_scene_tree *scene_layer = srv->layers[layer_map[layer_surface->current.layer]];
    (void)data;

    if (l->layer_surface->initial_commit) {
        client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

        /* Temporarily set the layer's current state to pending
         * so that we can easily arrange it */
        struct wlr_layer_surface_v1_state old_state = l->layer_surface->current;
        l->layer_surface->current = l->layer_surface->pending;
        arrange_layers(srv, l->mon);
        l->layer_surface->current = old_state;
        return;
    }

    if (layer_surface->current.committed == 0 && l->mapped == layer_surface->surface->mapped)
        return;
    l->mapped = layer_surface->surface->mapped;

    if (scene_layer != l->scene->node.parent) {
        wlr_scene_node_reparent(&l->scene->node, scene_layer);
        wl_list_remove(&l->link);
        wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
        wlr_scene_node_reparent(&l->popups->node, (layer_surface->current.layer
                < ZWLR_LAYER_SHELL_V1_LAYER_TOP ? srv->layers[LyrTop] : scene_layer));
    }

    arrange_layers(srv, l->mon);
}

void create_layer_surface( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_layer_surface);
    struct wlr_layer_surface_v1 *layer_surface = data;
    struct wlr_surface *surface = layer_surface->surface;
    struct wlr_scene_tree *scene_layer = srv->layers[layer_map[layer_surface->pending.layer]];

    if (!layer_surface->output
            && !(layer_surface->output = srv->out.sel_mon ? srv->out.sel_mon->wlr_output : NULL)) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }
    LayerSurface *l = layer_surface->data = calloc_or_die(1, sizeof(*l));
    l->srv = srv;
    l->type = LayerShell;
    LISTEN(&surface->events.commit, &l->surface_commit, commit_layer_surface_notify);
    LISTEN(&surface->events.unmap, &l->unmap, unmap_layer_surface_notify);
    LISTEN(&layer_surface->events.destroy, &l->destroy, destroy_layer_surface_notify);

    l->layer_surface = layer_surface;
    l->mon = layer_surface->output->data;
    l->scene_layer = wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
    l->scene = l->scene_layer->tree;

    l->popups = surface->data = wlr_scene_tree_create(layer_surface->current.layer
            < ZWLR_LAYER_SHELL_V1_LAYER_TOP ? srv->layers[LyrTop] : scene_layer);
    l->scene->node.data = l->popups->node.data = l;

    wl_list_insert(&l->mon->layers[layer_surface->pending.layer],&l->link);
    wlr_surface_send_enter(surface, layer_surface->output);
}

void destroy_layer_surface_notify( struct wl_listener *listener, void *data ) {
    LayerSurface *l = wl_container_of(listener, l, destroy);
    (void)data;

    wl_list_remove(&l->link);
    wl_list_remove(&l->destroy.link);
    wl_list_remove(&l->unmap.link);
    wl_list_remove(&l->surface_commit.link);
    wlr_scene_node_destroy(&l->scene->node);
    wlr_scene_node_destroy(&l->popups->node);
    free(l);
}

void unmap_layer_surface_notify( struct wl_listener *listener, void *data ) {
    LayerSurface *l = wl_container_of(listener, l, unmap);
    awl_server_t *srv = l->srv;
    (void)data;

    l->mapped = 0;
    wlr_scene_node_set_enabled(&l->scene->node, 0);
    if (l == srv->clients.exclusive_focus)
        srv->clients.exclusive_focus = NULL;
    if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
        arrange_layers(srv, l->mon);
    if (l->layer_surface->surface == srv->seat->keyboard_state.focused_surface)
        focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    motion_notify(srv, 0, NULL, 0, 0, 0, 0);
}
