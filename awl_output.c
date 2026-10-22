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
#include "awl_popup.h"

static void cleanupmon(struct wl_listener *listener, void *data);
static void closemon(Monitor *m);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
static void rendermon(struct wl_listener *listener, void *data);
static void requestmonstate(struct wl_listener *listener, void *data);

struct wl_list mons;
Monitor *selmon;
struct wlr_box sgeom;
struct wlr_output_layout *output_layout;
struct wlr_output_manager_v1 *output_mgr;
int in_cleanupmon = 0;

void
cleanupmon(struct wl_listener *listener, void *data)
{
    in_cleanupmon = 1;
    Monitor *m = wl_container_of(listener, m, destroy);
    LayerSurface *l, *tmp;
    size_t i;

    /* m->layers[i] are intentionally not unlinked */
    for (i = 0; i < LENGTH(m->layers); i++) {
        wl_list_for_each_safe(l, tmp, &m->layers[i], link)
            wlr_layer_surface_v1_destroy(l->layer_surface);
    }
    /* closemon() below checks m->bar.m to decide whether to touch the bar */
    barremovemon(m);

    wl_list_remove(&m->destroy.link);
    wl_list_remove(&m->frame.link);
    wl_list_remove(&m->link);
    wl_list_remove(&m->request_state.link);
    if (m->lock_surface)
        destroylocksurface(&m->destroy_lock_surface, NULL);
    m->wlr_output->data = NULL;
    wlr_output_layout_remove(output_layout, m->wlr_output);
    wlr_scene_output_destroy(m->scene_output);

    closemon(m);
    wlr_scene_node_destroy(&m->fullscreen_bg->node);
    wlr_scene_node_destroy(&m->bar_blur->node);
    wlr_scene_node_destroy(&m->scene_buffer->node);
    desktop_removemon(m);
    background_removemon(m);
    popupsremovemon(m);
    wlr_scene_node_destroy(&m->alert->node);
    for (i = 0; i < LENGTH(m->bar_bufs); i++)
        if (m->bar_bufs[i])
            wlr_buffer_drop(&m->bar_bufs[i]->base);
    free(m);
    in_cleanupmon = 0;
}

void
closemon(Monitor *m)
{
    if (m && m->bar.m && m->showbar) { togglebar_mon(m); m->closedbar=1; }
    /* update selmon if needed and
     * move closed monitor's clients to the focused one */
    Client *c;
    int i = 0, nmons = wl_list_length(&mons);
    if (!nmons) {
        selmon = NULL;
    } else if (m == selmon) {
        do /* don't switch to disabled mons */
            selmon = wl_container_of(mons.next, selmon, link);
        while (!selmon->wlr_output->enabled && i++ < nmons);

        if (!selmon->wlr_output->enabled)
            selmon = NULL;
    }

    wl_list_for_each(c, &clients, link) {
        if (c->isfloating && c->geom.x > m->m.width)
            resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
                    .width = c->geom.width, .height = c->geom.height}, 0);
        if (c->mon == m)
            setmon(c, selmon, c->tags);
    }
    focusclient(focustop(selmon), 1);
    barsdirty();
}

void
createmon(struct wl_listener *listener, void *data)
{
    /* This event is raised by the backend when a new output (aka a display or
     * monitor) becomes available. */
    struct wlr_output *wlr_output = data;
    const MonitorRule *r;
    size_t i, li;
    struct wlr_output_state state;
    Monitor *m;

    if (!wlr_output_init_render(wlr_output, alloc, renderer))
        return;

    m = wlr_output->data = ecalloc(1, sizeof(*m));
    m->wlr_output = wlr_output;

    for (i = 0; i < LENGTH(m->layers); i++)
        wl_list_init(&m->layers[i]);

    wlr_output_state_init(&state);
    /* Initialize monitor state using configured rules */
    m->tagset[0] = m->tagset[1] = 1;
    /* if no rule matches */
    m->m.x = m->m.y = -1;
    m->ptlt[0].mfact = 0.55f;
    m->ptlt[0].nmaster = 1;
    m->ptlt[0].lt[0] = m->ptlt[0].lt[1] = &cfg->layouts.v[0];
    for (r = cfg->monrules.v; r < cfg->monrules.v + cfg->monrules.n; r++) {
        if (!r->name || strstr(wlr_output->name, r->name)) {
            m->m.x = r->x;
            m->m.y = r->y;
            m->ptlt[0].mfact = r->mfact;
            m->ptlt[0].nmaster = r->nmaster;
            li = r->layout > 0 && (size_t)r->layout < cfg->layouts.n ? (size_t)r->layout : 0;
            m->ptlt[0].lt[0] = &cfg->layouts.v[li];
            m->ptlt[0].lt[1] = &cfg->layouts.v[cfg->layouts.n > 1 && li != 1];
            m->pertag = !!r->pertag;
            wlr_output_state_set_scale(&state, r->scale);
            wlr_output_state_set_transform(&state, r->rr);
            break;
        }
    }
    /* every view starts with the monitor's layout */
    for (i = 1; i < LENGTH(m->ptlt); i++)
        m->ptlt[i] = m->ptlt[0];
    strncpy(m->ltsymbol, sellayout(m)->symbol, LENGTH(m->ltsymbol)-1);

    /* The mode is a tuple of (width, height, refresh rate), and each
     * monitor supports only a specific set of modes. We just pick the
     * monitor's preferred mode; a more sophisticated compositor would let
     * the user configure it. */
    wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr_output));

    /* Set up event listeners */
    LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
    LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
    LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);

    wlr_output_state_set_enabled(&state, 1);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    baraddmon(m);
    desktop_addmon(m);
    background_addmon(m);
    awl_alert_addmon(m);

    wl_list_insert(&mons, &m->link);

    /* The xdg-protocol specifies:
     *
     * If the fullscreened surface is not opaque, the compositor must make
     * sure that other screen content not part of the same surface tree (made
     * up of subsurfaces, popups or similarly coupled surfaces) are not
     * visible below the fullscreened surface.
     *
     */
    /* updatemons() will resize and set correct position */
    m->fullscreen_bg = wlr_scene_rect_create(layers[LyrFS], 0, 0, (float[4])RGBA(cfg->colors.fullscreen));
    wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

    /* Adds this to the output layout in the order it was configured.
     *
     * The output layout utility automatically adds a wl_output global to the
     * display, which Wayland clients can see to find out information about the
     * output (such as DPI, scale factor, manufacturer, etc).
     */
    m->scene_output = wlr_scene_output_create(scene, wlr_output);
    if (m->m.x == -1 && m->m.y == -1)
        wlr_output_layout_add_auto(output_layout, wlr_output);
    else
        wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);

}

Monitor *
monitornamed(const char *name)
{
    Monitor *m;
    if (name)
        wl_list_for_each(m, &mons, link)
            if (!strcmp(m->wlr_output->name, name))
                return m;
    return NULL;
}

Monitor *
monitorselected(void)
{
    return selmon;
}

float
monitorscale(Monitor *m)
{
    return m && m->wlr_output ? m->wlr_output->scale : 1;
}

Monitor *
nextmon(int add) {
    int i = 0, i_sel = 1;

    int nmons = wl_list_length(&mons);
    Monitor* pmons[MAX(nmons,1)];

    Monitor* m = NULL;
    wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->enabled) {
            pmons[i] = m;
            if (m == selmon) i_sel = i;
            i++;
        }
    }

    if (!i) return NULL;
    return pmons[(i_sel + add + i)%i];
}

void
setselmon(Monitor *m)
{
    /* the bars mark the selected monitor (awl_bar_t.selmon) */
    if (m == selmon)
        return;
    selmon = m;
    barsdirty();
}

void
focusmon(const Arg *arg)
{
    Monitor *m = nextmon(arg->i);
    /* nextmon() returns NULL when no monitor is enabled -- keep selmon as it
     * is rather than handing NULL to everything that dereferences it */
    if (!m)
        return;
    focusclient(focustop(selmon = m), 1);
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
    struct wlr_output_configuration_v1 *config = data;
    outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
    /*
     * Called when a client such as wlr-randr requests a change in output
     * configuration. This is only one way that the layout can be changed,
     * so any Monitor information should be updated by updatemons() after an
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
            wlr_output_layout_add(output_layout, wlr_output,
                    config_head->state.x, config_head->state.y);

        wlr_output_state_finish(&state);
    }

    if (ok)
        wlr_output_configuration_v1_send_succeeded(config);
    else
        wlr_output_configuration_v1_send_failed(config);
    wlr_output_configuration_v1_destroy(config);

    /* https://codeberg.org/dwl/dwl/issues/577 */
    updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
    struct wlr_output_configuration_v1 *config = data;
    outputmgrapplyortest(config, 1);
}

void
powermgrsetmode(struct wl_listener *listener, void *data)
{
    struct wlr_output_power_v1_set_mode_event *event = data;
    struct wlr_output_state state = {0};
    Monitor *m = event->output->data;

    if (!m)
        return;

    m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the ouput */
    wlr_output_state_set_enabled(&state, event->mode);
    wlr_output_commit_state(m->wlr_output, &state);

    m->asleep = !event->mode;
    updatemons(NULL, NULL);
}

void
rendermon(struct wl_listener *listener, void *data)
{
    /* This function is called every time an output is ready to display a frame,
     * generally at the output's refresh rate (e.g. 60Hz). */
    Monitor *m = wl_container_of(listener, m, frame);
    Client *c;
    struct wlr_output_state pending = {0};
    struct timespec now;

    /* Render if no XDG clients have an outstanding resize and are visible on
     * this monitor. */
    wl_list_for_each(c, &clients, link) {
        if (c->resize && !c->isfloating && client_is_rendered_on_mon(c, m) && !client_is_stopped(c))
            goto skip;
    }

    /* make clients transparent before the commit; they change it back again otherwise */
    wl_list_for_each(c, &clients, link) {
        if (c->scene_surface && c->one_minus_alpha != 0) {
            wlr_scene_node_for_each_buffer(&c->scene_surface->node, scenebuffersetopacity, c);
        }
    }
    if (wlr_scene_output_commit(m->scene_output, NULL))
        lockshown(m);

skip:
    /* Let clients know a frame has been rendered */
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(m->scene_output, &now);
    wlr_output_state_finish(&pending);
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
    struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(event->output, event->state);
    updatemons(NULL, NULL);
}

void
updatemons(struct wl_listener *listener, void *data)
{
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
    struct wlr_output_configuration_head_v1 *config_head;
    Monitor *m;

    /* First remove from the layout the disabled monitors */
    wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->enabled || m->asleep)
            continue;
        config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
        config_head->state.enabled = 0;
        /* Remove this output from the layout to avoid cursor enter inside it */
        wlr_output_layout_remove(output_layout, m->wlr_output);
        closemon(m);
        m->m = m->w = (struct wlr_box){0};
        desktop_update(m);
        background_update(m);
        awl_alert_draw(m);
        popupsupdate(m);
    }
    /* Insert outputs that need to */
    wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->enabled
                && !wlr_output_layout_get(output_layout, m->wlr_output))
            wlr_output_layout_add_auto(output_layout, m->wlr_output);
    }

    /* Now that we update the output layout we can get its box */
    wlr_output_layout_get_box(output_layout, NULL, &sgeom);

    wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
    wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

    /* Make sure the clients are hidden when awl is locked */
    lockresize();

    wl_list_for_each(m, &mons, link) {
        if (!m->wlr_output->enabled)
            continue;
        config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

        /* Get the effective monitor geometry to use for surfaces */
        wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
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
        arrangelayers(m);
        /* Don't move clients to the left output when plugging monitors */
        arrange(m);
        /* make sure fullscreen clients have the right size */
        if ((c = focustop(m)) && c->isfullscreen)
            resize(c, m->m, 0);

        /* Try to re-set the gamma LUT when updating monitors,
         * it's only really needed when enabling a disabled output, but meh. */
        m->gamma_lut_changed = 1;

        config_head->state.x = m->m.x;
        config_head->state.y = m->m.y;

        if (!selmon) {
            selmon = m;
        }
    }

    if (selmon && selmon->wlr_output->enabled) {
        wl_list_for_each(c, &clients, link) {
            if (!c->mon && client_surface(c)->mapped)
                setmon(c, selmon, c->tags);
        }
        focusclient(focustop(selmon), 1);
        if (selmon->lock_surface) {
            client_notify_enter(selmon->lock_surface->surface,
                    wlr_seat_get_keyboard(seat));
            client_activate_surface(selmon->lock_surface->surface, 1);
        }
    }

    wl_list_for_each(m, &mons, link) {
        if (!m->wlr_output->enabled) continue;
        if (m->closedbar) {
            m->showbar = 0;
            m->closedbar = 0;
            togglebar_mon(m);
        }
        updatebar(m);
    }
    barsdirty();

    /* FIXME: figure out why the cursor image is at 0,0 after turning all
     * the monitors on.
     * Move the cursor image where it used to be. It does not generate a
     * wl_pointer.motion event for the clients, it's only the image what it's
     * at the wrong position after all. */
    wlr_cursor_move(cursor, NULL, 0, 0);

    wlr_output_manager_v1_set_configuration(output_mgr, config);
    updatepluginpause();
}

Monitor *
xytomon(double x, double y)
{
    struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
    return o ? o->data : NULL;
}
