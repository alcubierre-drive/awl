#include "awl_output.h"
#include "awl_alert.h"
#include "awl_background.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_desktop.h"
#include "awl_layer.h"
#include "awl_layout.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_notifications.h"
#include "awl_popup.h"

static void cleanup_mon( struct wl_listener *listener, void *data );
static void close_mon( awl_server_t *srv, Monitor *m );
static void output_mgr_apply_or_test( awl_server_t *srv, struct wlr_output_configuration_v1 *config, int test );
static void render_mon( struct wl_listener *listener, void *data );
static void request_mon_state( struct wl_listener *listener, void *data );
static void outputs_update( awl_server_t *srv );


void cleanup_mon( struct wl_listener *listener, void *data ) {
    Monitor *m = wl_container_of(listener, m, destroy);
    awl_server_t *srv = m->srv;
    LayerSurface *l, *tmp;
    (void)data;

    srv->out.in_cleanup_mon = 1;

    /* m->layers[i] are intentionally not unlinked */
    for (size_t i = 0; i < LENGTH(m->layers); i++) {
        wl_list_for_each_safe(l, tmp, &m->layers[i], link)
            wlr_layer_surface_v1_destroy(l->layer_surface);
    }
    /* close_mon() below checks m->bar.m to decide whether to touch the bar */
    bar_remove_mon(srv, m);

    wl_list_remove(&m->destroy.link);
    wl_list_remove(&m->frame.link);
    for (int i = 0; i < srv->out.nmons; i++)
        if (srv->out.mons[i] == m)
            memmove(&srv->out.mons[i], &srv->out.mons[i + 1], (size_t)(--srv->out.nmons - i) * sizeof(m));
    wl_list_remove(&m->request_state.link);
    if (m->lock_surface)
        destroy_lock_surface(&m->destroy_lock_surface, NULL);
    m->wlr_output->data = NULL;
    wlr_output_layout_remove(srv->out.layout, m->wlr_output);
    wlr_scene_output_destroy(m->scene_output);

    close_mon(srv, m);
    wlr_scene_node_destroy(&m->fullscreen_bg->node);
    for (int i = 0; i < 4; i++)
        if (m->clock[i / 2][i % 2])
            wlr_scene_node_destroy(&m->clock[i / 2][i % 2]->node);
    wlr_scene_node_destroy(&m->bar_blur->node);
    wlr_scene_node_destroy(&m->scene_buffer->node);
    desktop_removemon(srv->desktop, m);
    background_removemon(srv->background, m);
    popups_remove_mon(srv, m);
    wlr_scene_node_destroy(&m->alert->node);
    for (size_t i = 0; i < LENGTH(m->bar_bufs); i++)
        if (m->bar_bufs[i])
            wlr_buffer_drop(&m->bar_bufs[i]->base);
    free(m);
    srv->out.in_cleanup_mon = 0;
}

void close_mon( awl_server_t *srv, Monitor *m ) {
    if (m && m->bar.m && m->show_bar) { toggle_bar_mon(srv, m); m->closed_bar=1; }
    /* update sel_mon if needed and
     * move closed monitor's clients to the focused one */
    Client *c;
    if (m == srv->out.sel_mon || !srv->out.nmons)
        srv->out.sel_mon = next_mon(srv, 0); /* the first enabled one, or NULL */

    wl_list_for_each(c, &srv->clients.list, link) {
        if (c->is_floating && c->geom.x > m->m.width)
            resize(srv, c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
                    .width = c->geom.width, .height = c->geom.height}, 0);
        if (c->mon == m)
            set_mon(srv, c, srv->out.sel_mon, c->tags);
    }
    focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
    bars_dirty(srv);
}

void create_mon( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_output);
    /* This event is raised by the backend when a new output (aka a display or
     * monitor) becomes available. */
    struct wlr_output *wlr_output = data;
    struct wlr_output_state state;

    if (srv->out.nmons == AWL_MAX_MONS) {
        awl_wrn_printf("output %s: awl takes %d outputs at most, left alone\n", wlr_output->name, AWL_MAX_MONS);
        return;
    }
    if (!wlr_output_init_render(wlr_output, srv->alloc, srv->renderer))
        return;

    Monitor *m = wlr_output->data = calloc_or_die(1, sizeof(*m));
    m->srv = srv;
    m->wlr_output = wlr_output;

    for (size_t i = 0; i < LENGTH(m->layers); i++)
        wl_list_init(&m->layers[i]);

    wlr_output_state_init(&state);
    /* Initialize monitor state using configured rules */
    m->tagset[0] = m->tagset[1] = 1;
    /* if no rule matches */
    m->m.x = m->m.y = -1;
    m->tag_layouts[0].master_factor = 0.55f;
    m->tag_layouts[0].master_count = 1;
    m->tag_layouts[0].lt[0] = m->tag_layouts[0].lt[1] = &srv->cfg->layouts.v[0];
    for (const MonitorRule *r = srv->cfg->mon_rules.v; r < srv->cfg->mon_rules.v + srv->cfg->mon_rules.n; r++) {
        if (!r->name || strstr(wlr_output->name, r->name)) {
            m->m.x = r->x;
            m->m.y = r->y;
            m->tag_layouts[0].master_factor = r->master_factor;
            m->tag_layouts[0].master_count = r->master_count;
            size_t li = r->layout > 0 && (size_t)r->layout < srv->cfg->layouts.n ? (size_t)r->layout : 0;
            m->tag_layouts[0].lt[0] = &srv->cfg->layouts.v[li];
            m->tag_layouts[0].lt[1] = &srv->cfg->layouts.v[srv->cfg->layouts.n > 1 && li != 1];
            m->per_tag = !!r->per_tag;
            wlr_output_state_set_scale(&state, r->scale);
            wlr_output_state_set_transform(&state, r->rr);
            break;
        }
    }
    /* every view starts with the monitor's layout */
    for (size_t i = 1; i < LENGTH(m->tag_layouts); i++)
        m->tag_layouts[i] = m->tag_layouts[0];
    strncpy(m->layout_symbol, selected_layout(m)->symbol, LENGTH(m->layout_symbol)-1);

    /* The mode is a tuple of (width, height, refresh rate), and each
     * monitor supports only a specific set of modes. We just pick the
     * monitor's preferred mode; a more sophisticated compositor would let
     * the user configure it. */
    wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr_output));

    /* Set up event listeners */
    LISTEN(&wlr_output->events.frame, &m->frame, render_mon);
    LISTEN(&wlr_output->events.destroy, &m->destroy, cleanup_mon);
    LISTEN(&wlr_output->events.request_state, &m->request_state, request_mon_state);

    wlr_output_state_set_enabled(&state, 1);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    bar_add_mon(srv, m);
    desktop_addmon(srv->desktop, m);
    background_addmon(srv->background, m);
    awl_alert_addmon(srv, m);

    memmove(&srv->out.mons[1], &srv->out.mons[0], (size_t)srv->out.nmons++ * sizeof(m));
    srv->out.mons[0] = m;

    /* The xdg-protocol specifies:
     *
     * If the fullscreened surface is not opaque, the compositor must make
     * sure that other screen content not part of the same surface tree (made
     * up of subsurfaces, popups or similarly coupled surfaces) are not
     * visible below the fullscreened surface.
     *
     */
    /* outputs_update() will resize and set correct position */
    m->fullscreen_bg = wlr_scene_rect_create(srv->layers[LyrFS], 0, 0, (float[4])RGBA(srv->cfg->colors.fullscreen));
    wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

    /* Adds this to the output layout in the order it was configured.
     *
     * The output layout utility automatically adds a wl_output global to the
     * display, which Wayland clients can see to find out information about the
     * output (such as DPI, scale factor, manufacturer, etc).
     */
    m->scene_output = wlr_scene_output_create(srv->scene, wlr_output);
    if (m->m.x == -1 && m->m.y == -1)
        wlr_output_layout_add_auto(srv->out.layout, wlr_output);
    else
        wlr_output_layout_add(srv->out.layout, wlr_output, m->m.x, m->m.y);

}

Monitor *monitor_named( awl_server_t *srv, const char *name ) {
    Monitor *m;
    if (name)
        FOR_EACH_MON(m, srv)
            if (!strcmp(m->wlr_output->name, name))
                return m;
    return NULL;
}

Monitor *monitor_selected( awl_server_t *srv ) {
    return srv->out.sel_mon;
}

float monitor_scale( Monitor *m ) {
    return m && m->wlr_output ? m->wlr_output->scale : 1;
}

Monitor *next_mon( awl_server_t *srv, int add ) {
    Monitor *on[AWL_MAX_MONS], *m;
    int n = 0, sel = 0;
    FOR_EACH_MON(m, srv) {
        if (m == srv->out.sel_mon) sel = n;
        if (m->wlr_output->enabled) on[n++] = m;
    }
    return n ? on[((sel + add) % n + n) % n] : NULL;
}

void set_sel_mon( awl_server_t *srv, Monitor *m ) {
    /* the bars mark the selected monitor (awl_bar_t.sel_mon) */
    if (m == srv->out.sel_mon)
        return;
    srv->out.sel_mon = m;
    bars_dirty(srv);
}

void focus_monitor( awl_server_t *srv, const Arg *arg ) {
    Monitor *m = next_mon(srv, arg->i);
    /* next_mon() returns NULL when no monitor is enabled -- keep sel_mon as it
     * is rather than handing NULL to everything that dereferences it */
    if (!m)
        return;
    focus_client(srv, focus_top(srv, srv->out.sel_mon = m), 1);
}

void output_mgr_apply( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.output_mgr_apply);
    struct wlr_output_configuration_v1 *config = data;
    output_mgr_apply_or_test(srv, config, 0);
}

void output_mgr_apply_or_test( awl_server_t *srv, struct wlr_output_configuration_v1 *config, int test ) {
    /*
     * Called when a client such as wlr-randr requests a change in output
     * configuration. This is only one way that the layout can be changed,
     * so any Monitor information should be updated by outputs_update() after an
     * output_layout.change event, not here.
     */
    struct wlr_output_configuration_head_v1 *config_head;
    int ok = 1;

    wl_list_for_each(config_head, &config->heads, link) {
        struct wlr_output *wlr_output = config_head->state.output;
        Monitor *m = wlr_output->data;
        struct wlr_output_state state;

        /* Ensure displays previously disabled by wlr-output-power-management-v1
         * are properly handled*/
        m->asleep = 0;

        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, config_head->state.enabled);
        if (!config_head->state.enabled)
            goto apply_or_test;

        if (config_head->state.mode)
            wlr_output_state_set_mode(&state, config_head->state.mode);
        else
            wlr_output_state_set_custom_mode(&state,
                    config_head->state.custom_mode.width,
                    config_head->state.custom_mode.height,
                    config_head->state.custom_mode.refresh);

        wlr_output_state_set_transform(&state, config_head->state.transform);
        wlr_output_state_set_scale(&state, config_head->state.scale);
        wlr_output_state_set_adaptive_sync_enabled(&state,
                config_head->state.adaptive_sync_enabled);

apply_or_test:
        ok &= test ? wlr_output_test_state(wlr_output, &state)
                : wlr_output_commit_state(wlr_output, &state);

        /* Don't move monitors if position wouldn't change. This avoids
         * wlroots marking the output as manually configured.
         * wlr_output_layout_add does not like disabled outputs */
        if (!test && wlr_output->enabled && (m->m.x != config_head->state.x || m->m.y != config_head->state.y))
            wlr_output_layout_add(srv->out.layout, wlr_output,
                    config_head->state.x, config_head->state.y);

        wlr_output_state_finish(&state);
    }

    if (ok)
        wlr_output_configuration_v1_send_succeeded(config);
    else
        wlr_output_configuration_v1_send_failed(config);
    wlr_output_configuration_v1_destroy(config);

    /* https://codeberg.org/dwl/dwl/issues/577 */
    outputs_update(srv);
}

void output_mgr_test( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.output_mgr_test);
    struct wlr_output_configuration_v1 *config = data;
    output_mgr_apply_or_test(srv, config, 1);
}

static int output_matches( struct wlr_output *o, const char *match ) {
    return match && (strstr(o->name, match)
            || (o->description && strstr(o->description, match)));
}

/** the first of p's settings that matches o, NULL if none */
static const OutputSetting *profile_setting( const OutputProfile *p, struct wlr_output *o ) {
    for (const OutputSetting *s = p->outputs.v; s < p->outputs.v + p->outputs.n; s++)
        if (output_matches(o, s->match))
            return s;
    return NULL;
}

static void profile_refused( awl_server_t *srv, const char *name, const char *why, const char *what ) {
    char body[256];
    awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
    snprintf(body, sizeof(body), why, what);
    awl_wrn_printf("output_profile %s: %s\n", name ? name : "(none)", body);
    n.summary = "Output profile not applied";
    n.body = body;
    n.tag = "awl-outputprofile";
    awl_notifications_send(srv, &n);
}

/** The output_profile action: applies the :c:type:`OutputProfile` named arg->v. */
void output_profile( awl_server_t *srv, const Arg *arg ) {
    const char *name = arg->v;
    const OutputProfile *p = NULL;
    const OutputSetting *s;
    Monitor *m;
    int on = 0;

    for (size_t i = 0; name && i < srv->cfg->output_profiles.n; i++)
        if (srv->cfg->output_profiles.v[i].name && !strcmp(srv->cfg->output_profiles.v[i].name, name))
            p = &srv->cfg->output_profiles.v[i];
    if (!p) {
        profile_refused(srv, name, "there is no profile \"%s\"", name ? name : "");
        return;
    }
    /* everything it turns on has to be there */
    for (s = p->outputs.v; s < p->outputs.v + p->outputs.n; s++) {
        if (!s->on)
            continue;
        int found = 0;
        FOR_EACH_MON(m, srv)
            found |= output_matches(m->wlr_output, s->match);
        if (!found) {
            profile_refused(srv, name, "no output matches \"%s\"", s->match ? s->match : "");
            return;
        }
    }
    FOR_EACH_MON(m, srv) {
        s = profile_setting(p, m->wlr_output);
        on += s ? s->on : m->wlr_output->enabled || m->asleep;
    }
    if (!on) {
        profile_refused(srv, name, "%s", "it would turn every output off");
        return;
    }

    /* the ones going off first: they free what the others may need */
    FOR_EACH_MON(m, srv) {
        struct wlr_output_state state;
        if (!(s = profile_setting(p, m->wlr_output)) || s->on)
            continue;
        m->asleep = 0;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, 0);
        if (!wlr_output_commit_state(m->wlr_output, &state))
            awl_wrn_printf("output_profile %s: can't turn %s off\n", name, m->wlr_output->name);
        wlr_output_state_finish(&state);
    }
    FOR_EACH_MON(m, srv) {
        struct wlr_output_state state;
        struct wlr_output_layout_output *lo;
        if (!(s = profile_setting(p, m->wlr_output)) || !s->on)
            continue;
        m->asleep = 0;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, 1);
        if (!m->wlr_output->enabled)
            wlr_output_state_set_mode(&state, wlr_output_preferred_mode(m->wlr_output));
        if (s->scale > 0)
            wlr_output_state_set_scale(&state, s->scale);
        wlr_output_state_set_transform(&state, s->rr);
        if (!wlr_output_commit_state(m->wlr_output, &state)) {
            awl_wrn_printf("output_profile %s: can't turn %s on\n", name, m->wlr_output->name);
        } else if (!(lo = wlr_output_layout_get(srv->out.layout, m->wlr_output))
                || lo->x != s->x || lo->y != s->y) {
            /* moves it if it is in the layout already */
            wlr_output_layout_add(srv->out.layout, m->wlr_output, s->x, s->y);
        }
        wlr_output_state_finish(&state);
    }
    outputs_update(srv);
}

void power_mgr_set_mode( struct wl_listener *listener, void *data ) {
    struct wlr_output_power_v1_set_mode_event *event = data;
    struct wlr_output_state state = {0};
    Monitor *m = event->output->data;
    (void)listener;

    if (!m)
        return;

    m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the ouput */
    wlr_output_state_set_enabled(&state, event->mode);
    wlr_output_commit_state(m->wlr_output, &state);

    m->asleep = !event->mode;
    outputs_update(m->srv);
}

/** whether c has kept its resize unanswered for :c:macro:`RESIZE_WAIT_MS`
 * by now; logs it the first time */
static int resize_late( Client *c, const struct timespec *now ) {
    int64_t ms = (int64_t)now->tv_sec * 1000 + now->tv_nsec / 1000000;
    if (ms - c->resize_sent < RESIZE_WAIT_MS)
        return 0;
    if (!c->resize_late) {
        c->resize_late = 1;
        awl_wrn_printf("client '%s': no answer to a resize in %d ms, drawing without it\n",
                client_get_appid(c), RESIZE_WAIT_MS);
    }
    return 1;
}

void render_mon( struct wl_listener *listener, void *data ) {
    /* This function is called every time an output is ready to display a frame,
     * generally at the output's refresh rate (e.g. 60Hz). */
    Monitor *m = wl_container_of(listener, m, frame);
    awl_server_t *srv = m->srv;
    Client *c;
    struct wlr_output_state pending = {0};
    struct timespec now;
    (void)data;

    clock_gettime(CLOCK_MONOTONIC, &now);
    /* Render if no XDG clients have an outstanding resize and are visible on
     * this monitor, or one has kept us waiting for RESIZE_WAIT_MS. */
    wl_list_for_each(c, &srv->clients.list, link) {
        if (c->resize && !c->is_floating && client_is_rendered_on_mon(c, m) && !client_is_stopped(c)
                && !resize_late(c, &now))
            goto skip;
    }

    /* make clients transparent before the commit; they change it back again otherwise */
    wl_list_for_each(c, &srv->clients.list, link) {
        if (c->scene_surface && c->one_minus_alpha != 0) {
            wlr_scene_node_for_each_buffer(&c->scene_surface->node, scene_buffer_set_opacity, c);
        }
    }
    if (wlr_scene_output_commit(m->scene_output, NULL))
        lock_shown(srv, m);

skip:
    /* Let clients know a frame has been rendered */
    wlr_scene_output_send_frame_done(m->scene_output, &now);
    wlr_output_state_finish(&pending);
}

void request_mon_state( struct wl_listener *listener, void *data ) {
    Monitor *m = wl_container_of(listener, m, request_state);
    struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(event->output, event->state);
    outputs_update(m->srv);
}

void update_mons( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.layout_change);
    (void)data;
    outputs_update(srv);
}

static void outputs_update( awl_server_t *srv ) {
    /*
     * Called whenever the output layout changes: adding or removing a
     * monitor, changing an output's mode or position, etc. This is where
     * the change officially happens and we update geometry, window
     * positions, focus, and the stored configuration in wlroots'
     * output-manager implementation.
     */
    struct wlr_output_configuration_v1 *config
            = wlr_output_configuration_v1_create();
    Client *c;
    Monitor *m;

    /* First remove from the layout the disabled monitors */
    FOR_EACH_MON(m, srv) {
        if (m->wlr_output->enabled || m->asleep)
            continue;
        struct wlr_output_configuration_head_v1 *config_head
                = wlr_output_configuration_head_v1_create(config, m->wlr_output);
        config_head->state.enabled = 0;
        /* Remove this output from the layout to avoid cursor enter inside it */
        wlr_output_layout_remove(srv->out.layout, m->wlr_output);
        close_mon(srv, m);
        m->m = m->w = (struct wlr_box){0};
        desktop_update(srv->desktop, m);
        background_update(srv->background, m);
        awl_alert_draw(srv, m);
        popups_remove_mon(srv, m);
    }
    /* Insert outputs that need to */
    FOR_EACH_MON(m, srv) {
        if (m->wlr_output->enabled
                && !wlr_output_layout_get(srv->out.layout, m->wlr_output))
            wlr_output_layout_add_auto(srv->out.layout, m->wlr_output);
    }

    /* Now that we update the output layout we can get its box */
    wlr_output_layout_get_box(srv->out.layout, NULL, &srv->out.screen_geom);

    wlr_scene_node_set_position(&srv->root_bg->node, srv->out.screen_geom.x, srv->out.screen_geom.y);
    wlr_scene_rect_set_size(srv->root_bg, srv->out.screen_geom.width, srv->out.screen_geom.height);

    /* Make sure the clients are hidden when awl is locked */
    lock_resize(srv);

    FOR_EACH_MON(m, srv) {
        if (!m->wlr_output->enabled)
            continue;
        struct wlr_output_configuration_head_v1 *config_head
                = wlr_output_configuration_head_v1_create(config, m->wlr_output);

        /* Get the effective monitor geometry to use for surfaces */
        wlr_output_layout_get_box(srv->out.layout, m->wlr_output, &m->m);
        m->w = m->m;
        wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

        wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
        wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

        if (m->lock_surface) {
            struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
            wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
            wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
        }

        /* Calculate the effective monitor geometry to use for clients */
        arrange_layers(srv, m);
        /* Don't move clients to the left output when plugging monitors */
        arrange(srv, m);
        /* make sure fullscreen clients have the right size */
        if ((c = focus_top(srv, m)) && c->is_fullscreen)
            resize(srv, c, m->m, 0);

        /* Try to re-set the gamma LUT when updating monitors,
         * it's only really needed when enabling a disabled output, but meh. */
        m->gamma_lut_changed = 1;

        config_head->state.x = m->m.x;
        config_head->state.y = m->m.y;

        if (!srv->out.sel_mon) {
            srv->out.sel_mon = m;
        }
    }

    if (srv->out.sel_mon && srv->out.sel_mon->wlr_output->enabled) {
        wl_list_for_each(c, &srv->clients.list, link) {
            if (!c->mon && client_surface(c)->mapped)
                set_mon(srv, c, srv->out.sel_mon, c->tags);
        }
        focus_client(srv, focus_top(srv, srv->out.sel_mon), 1);
        if (srv->out.sel_mon->lock_surface) {
            client_notify_enter(srv, srv->out.sel_mon->lock_surface->surface,
                    wlr_seat_get_keyboard(srv->seat));
            client_activate_surface(srv->out.sel_mon->lock_surface->surface, 1);
        }
    }

    FOR_EACH_MON(m, srv) {
        if (!m->wlr_output->enabled) continue;
        if (m->closed_bar) {
            m->show_bar = 0;
            m->closed_bar = 0;
            toggle_bar_mon(srv, m);
        }
        update_bar(srv, m);
    }
    bars_dirty(srv);

    /* wlr_cursor keeps the cursor image's position in each output's own
     * coordinates and only recomputes them when the cursor moves (or leaves
     * the layout): an output that moved in the layout, or was added or
     * turned on, would show the image where it was before, e.g. at 0,0. A
     * move by nothing recomputes them; it sends the clients no motion. */
    wlr_cursor_move(srv->cursor.wlr, NULL, 0, 0);

    wlr_output_manager_v1_set_configuration(srv->out.mgr, config);
    update_plugin_pause(srv);
}

Monitor *xy_to_mon( awl_server_t *srv, double x, double y ) {
    struct wlr_output *o = wlr_output_layout_output_at(srv->out.layout, x, y);
    return o ? o->data : NULL;
}
