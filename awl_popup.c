#include "awl_popup.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_output.h"

static awl_window_style_t popup_style( const awl_config_t *cfg, const Popup *p );
static Popup *popup_find( awl_server_t *srv, awl_window_t *win );

int pointer_in_popup( awl_server_t *srv, awl_window_t *win ) {
    /* the plugin may have destroyed it without clearing popup_window */
    return popup_find(srv, win) && awl_window_contains(win, srv->cursor.wlr->x, srv->cursor.wlr->y);
}

int popup_shown( awl_server_t *srv, awl_window_t *win ) {
    return popup_find(srv, win) && awl_window_monitor(win);
}

/** p's look, from cfg->popup and cfg->colors' popup_*; a menu's its own
 * (cfg->colors' menu_*, cfg->popup.menu); p's colors over either (NULL: none) */
static awl_window_style_t popup_style( const awl_config_t *cfg, const Popup *p ) {
    int menu = p && p->menu;
    awl_window_style_t s = {
            .bg = cfg->colors.popup_bg, .fg = cfg->colors.popup_fg,
            .border = cfg->colors.popup_border,
            .highlight = cfg->colors.popup_hover,
            .border_width = cfg->popup.border_width,
            .radius = cfg->popup.radius, .padding = cfg->popup.padding,
            .margin = cfg->popup.margin,
            .column_spacing = cfg->popup.column_spacing,
            .row_spacing = cfg->popup.row_spacing,
            .blur = cfg->blur.popups,
            .blur_strength = cfg->blur.strength, .blur_alpha = cfg->blur.alpha };
    if (menu) {
        s.bg = cfg->colors.menu_bg.v;
        s.fg = cfg->colors.menu_fg.v;
        s.border = cfg->colors.menu_border.v;
        s.highlight = cfg->colors.menu_highlight.v;
        s.border_width = cfg->popup.menu.border_width.v;
        s.radius = cfg->popup.menu.radius.v;
        s.blur = cfg->popup.menu.blur.v;
    }
    if (p && p->bg)
        s.bg = p->bg;
    if (p && p->fg)
        s.fg = p->fg;
    if (p && p->border)
        s.border = p->border;
    if (p && p->border_width >= 0)
        s.border_width = p->border_width;
    return s;
}

/** whether a popup other than a persistent one is shown (a menu, a hover
 * popup, a tooltip) */
static int popups_shown( awl_server_t *srv ) {
    Popup *p;
    wl_list_for_each(p, &srv->popups.list, link)
        if (!p->persistent && awl_window_monitor(p->win))
            return 1;
    return 0;
}

/** puts the trees where they belong: the popups' right above LyrTop, the
 * persistent ones' right above it or above LyrOverlay; while another popup
 * is shown, the popups' right above the persistent ones', so that it covers
 * them (the notification center, a hover popup over the notifications) */
static void popups_place( awl_server_t *srv ) {
    struct wlr_scene_node *persistent_below = srv->cfg->popup.persistent_overlay
            ? &srv->layers[LyrOverlay]->node : &srv->layers[LyrTop]->node;
    if (srv->popups.persistent_tree && srv->popups.tree && popups_shown(srv)) {
        wlr_scene_node_place_above(&srv->popups.persistent_tree->node, persistent_below);
        wlr_scene_node_place_above(&srv->popups.tree->node, &srv->popups.persistent_tree->node);
        return;
    }
    if (srv->popups.tree)
        wlr_scene_node_place_above(&srv->popups.tree->node, &srv->layers[LyrTop]->node);
    if (srv->popups.persistent_tree)
        wlr_scene_node_place_above(&srv->popups.persistent_tree->node, srv->cfg->popup.persistent_overlay
                ? persistent_below : srv->popups.tree ? &srv->popups.tree->node : persistent_below);
}

/** win's entry in popups, NULL if it is not (or no longer) one of them */
static Popup *popup_find( awl_server_t *srv, awl_window_t *win ) {
    Popup *p;
    if (win)
        wl_list_for_each(p, &srv->popups.list, link)
            if (p->win == win)
                return p;
    return NULL;
}

awl_window_t *popup_new( awl_server_t *srv, int plugin ) {
    if (!srv->scene)
        return NULL;
    /* over the bar and the windows, under fullscreen ones */
    if (!srv->popups.tree) {
        srv->popups.tree = wlr_scene_tree_create(&srv->scene->tree);
        popups_place(srv);
    }
    awl_window_style_t style = popup_style(srv->cfg, NULL);
    Popup *p = calloc_or_die(1, sizeof(*p));
    p->win = awl_window_create(srv->popups.tree, &style);
    p->plugin = plugin;
    p->border_width = -1;
    wl_list_insert(srv->popups.list.prev, &p->link);
    return p->win;
}

awl_window_t *popup_create( awl_server_t *srv ) {
    return popup_new(srv, 1);
}

void popup_destroy( awl_server_t *srv, awl_window_t *win ) {
    Popup *p = popup_find(srv, win);
    if (!p)
        return;
    if (srv->popups.hovered == win)
        srv->popups.hovered = NULL;
    wl_list_remove(&p->link);
    awl_window_destroy(p->win);
    free(p);
    popups_grab_update(srv);
}

/** another popup than a menu (the calendar the clock's hover opens)
 * closes the menu open at once, instead of covering it until it notices the
 * pointer left (its hidden callback closes its submenus too) */
static void close_menus( awl_server_t *srv, Popup *p ) {
    Popup *o;
    if (!p->menu && !p->persistent)
        wl_list_for_each(o, &srv->popups.list, link)
            if (o->menu && awl_window_monitor(o->win))
                awl_window_hide(o->win);
}

void popup_show( awl_server_t *srv, awl_window_t *win, Monitor *m, int center_x ) {
    Popup *p = popup_find(srv, win);
    if (!m || !p)
        return;
    close_menus(srv, p);
    awl_window_show(srv, win, m, center_x, srv->cfg->bar.top);
    popups_grab_update(srv);
}

void popup_show_at( awl_server_t *srv, awl_window_t *win, Monitor *m, int at, int dx, int dy ) {
    Popup *p = popup_find(srv, win);
    if (!m || !p)
        return;
    close_menus(srv, p);
    awl_window_show_at(srv, win, m, at, dx, dy);
    popups_grab_update(srv);
}

void popup_hide( awl_server_t *srv, awl_window_t *win ) {
    awl_window_hide(win);
    popups_grab_update(srv);
}

void popup_set_key( awl_server_t *srv, awl_window_t *win, awl_window_key_t key, void *data ) {
    awl_window_set_key(win, key, data);
    popups_grab_update(srv);
}

awl_window_t *popup_keyboard( awl_server_t *srv ) {
    Popup *p;
    wl_list_for_each_reverse(p, &srv->popups.list, link)
        if (awl_window_takes_keys(p->win) && !(srv->locked && p->persistent))
            return p->win;
    return NULL;
}

void popups_grab_update( awl_server_t *srv ) {
    awl_window_t *win = popup_keyboard(srv);
    /* a popup shown or hidden: its place over the persistent ones */
    popups_place(srv);
    if (!win == !srv->popups.keyboard) {
        srv->popups.keyboard = win;
        return;
    }
    srv->popups.keyboard = win;
    awl_dbg_printf(awl_loglevel_vrb, "popup %p: %s the keyboard\n", (void *)win,
            win ? "takes" : "gives back");
    if (srv->locked)
        return;
    if (win)
        wlr_seat_keyboard_notify_clear_focus(srv->seat);
    else if (!srv->clients.exclusive_focus)
        focus_client(srv, focus_top(srv, srv->out.sel_mon), 0);
}

/** whether something above the popups' tree (the persistent one's if
 * persistent) takes the pointer at x, y: a fullscreen client over the popups,
 * a layer surface over the persistent ones (the popups themselves never do,
 * awl_window.c) */
static int popup_covered( awl_server_t *srv, int persistent, double x, double y ) {
    struct wlr_scene_node *node = wlr_scene_node_at(&srv->scene->tree.node, x, y, NULL, NULL);
    struct wlr_scene_node *ours = persistent ? &srv->popups.persistent_tree->node : &srv->popups.tree->node;
    if (!node)
        return 0;
    /* the scene's child that has it, then whether that is above ours */
    while (node->parent && node->parent != &srv->scene->tree)
        node = &node->parent->node;
    for (struct wlr_scene_node *it = ours; &it->link != &srv->scene->tree.children;
            it = wl_container_of(it->link.next, it, link))
        if (it == node)
            return it != ours;
    return 0;
}

awl_window_t *popup_at( awl_server_t *srv, double x, double y ) {
    Popup *p;
    /* the top tree's first (popups_place()), then the newest */
    const int top = !popups_shown(srv);
    for (int i = 0; i < 2; i++)
        wl_list_for_each_reverse(p, &srv->popups.list, link)
            if (p->persistent == (i ? !top : top) && awl_window_takes_pointer(p->win, x, y))
                return popup_covered(srv, p->persistent, x, y) ? NULL : p->win;
    return NULL;
}

awl_window_t *popup_pointer( awl_server_t *srv ) {
    if (srv->cursor.mode != CurNormal || srv->seat->drag || srv->locked)
        return NULL;
    return popup_at(srv, srv->cursor.wlr->x, srv->cursor.wlr->y);
}

int popup_motion( awl_server_t *srv ) {
    awl_window_t *win = popup_pointer(srv);

    if (srv->popups.hovered && srv->popups.hovered != win) {
        awl_window_pointer_leave(srv, srv->popups.hovered);
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: pointer left\n", (void *)srv->popups.hovered);
    }
    if (!win) {
        srv->popups.hovered = NULL;
        return 0;
    }
    if (srv->popups.hovered != win) {
        wlr_seat_pointer_notify_clear_focus(srv->seat);
        wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr, "default");
        update_hover(srv, NULL);
        srv->popups.hovered = win;
    }
    int row = awl_window_hover_row(win);
    if (awl_window_pointer_motion(srv, win, srv->cursor.wlr->x, srv->cursor.wlr->y) != row)
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: pointer on row %d at %.0f,%.0f\n", (void *)win,
                awl_window_hover_row(win), srv->cursor.wlr->x, srv->cursor.wlr->y);
    return 1;
}

void popups_update( awl_server_t *srv, Monitor *m ) {
    Popup *p;
    wl_list_for_each(p, &srv->popups.list, link)
        awl_window_update(srv, p->win, m);
}

void popups_hide_all( awl_server_t *srv ) {
    Popup *p;
    wl_list_for_each(p, &srv->popups.list, link)
        if (!p->persistent)
            awl_window_hide(p->win);
    popups_grab_update(srv);
}

void popup_show_beside( awl_server_t *srv, awl_window_t *win, Monitor *m, int x, int y, int w, int h, int left ) {
    if (m && win)
        awl_window_show_beside(srv, win, m, &(struct wlr_box){x, y, w, h}, left);
    popups_grab_update(srv);
}

void popup_set_menu_style( awl_server_t *srv, awl_window_t *win ) {
    Popup *p = popup_find(srv, win);
    if (!p)
        return;
    p->menu = 1;
    awl_window_style_t style = popup_style(srv->cfg, p);
    awl_window_set_style(srv, win, &style);
}

void popup_set_persistent( awl_server_t *srv, awl_window_t *win, int persistent ) {
    Popup *p = popup_find(srv, win);
    if (!p || p->persistent == !!persistent)
        return;
    p->persistent = !!persistent;
    if (p->persistent && !srv->popups.persistent_tree) {
        srv->popups.persistent_tree = wlr_scene_tree_create(&srv->scene->tree);
        popups_place(srv);
        popups_locked(srv);
    }
    awl_window_reparent(win, p->persistent ? srv->popups.persistent_tree : srv->popups.tree);
}

void popup_set_colors( awl_server_t *srv, awl_window_t *win, uint32_t bg, uint32_t fg, uint32_t border ) {
    Popup *p = popup_find(srv, win);
    if (!p || (p->bg == bg && p->fg == fg && p->border == border))
        return;
    p->bg = bg;
    p->fg = fg;
    p->border = border;
    awl_window_style_t style = popup_style(srv->cfg, p);
    awl_window_set_style(srv, win, &style);
}

void popup_set_border_width( awl_server_t *srv, awl_window_t *win, int width ) {
    Popup *p = popup_find(srv, win);
    if (width < 0)
        width = -1;
    if (!p || p->border_width == width)
        return;
    p->border_width = width;
    awl_window_style_t style = popup_style(srv->cfg, p);
    awl_window_set_style(srv, win, &style);
}

int popup_box( awl_server_t *srv, awl_window_t *win, int *x, int *y, int *w, int *h ) {
    struct wlr_box box = {0};
    int visible = popup_find(srv, win) && awl_window_box(win, &box);
    *x = box.x;
    *y = box.y;
    *w = box.width;
    *h = box.height;
    return visible;
}

void popup_room( awl_server_t *srv, awl_window_t *win, Monitor *m, int *x, int *y, int *w, int *h ) {
    struct wlr_box box = {0};
    if (popup_find(srv, win) && m)
        box = awl_window_room(win, m);
    *x = box.x;
    *y = box.y;
    *w = box.width;
    *h = box.height;
}

int popup_persistent( awl_server_t *srv, awl_window_t *win ) {
    Popup *p = popup_find(srv, win);
    return p && p->persistent;
}

int popup_persistent_layer( awl_server_t *srv, int *visible ) {
    *visible = srv->popups.persistent_tree && srv->popups.persistent_tree->node.enabled;
    if (!srv->popups.persistent_tree)
        return -1;
    return srv->popups.persistent_tree->node.link.prev == &srv->layers[LyrOverlay]->node.link ? LyrOverlay : LyrTop;
}

void popups_locked( awl_server_t *srv ) {
    if (srv->popups.persistent_tree)
        wlr_scene_node_set_enabled(&srv->popups.persistent_tree->node, !srv->locked);
}

int popup_part_box( awl_window_t *win, int part, int *x, int *y, int *w, int *h ) {
    struct wlr_box box;
    if (!win || !awl_window_part_box(win, part, &box))
        return 0;
    *x = box.x;
    *y = box.y;
    *w = box.width;
    *h = box.height;
    return 1;
}

void popups_remove_mon( awl_server_t *srv, Monitor *m ) {
    Popup *p;
    wl_list_for_each(p, &srv->popups.list, link)
        awl_window_removemon(p->win, m);
    popups_grab_update(srv);
}

void popups_restyle( awl_server_t *srv ) {
    Popup *p;
    wl_list_for_each(p, &srv->popups.list, link) {
        awl_window_style_t style = popup_style(srv->cfg, p);
        awl_window_set_style(srv, p->win, &style);
    }
    popups_place(srv);
}

int popups_reap( awl_server_t *srv ) {
    Popup *p, *tmp;
    int n = 0;

    /* only the plugins': AWL_TEST_WINDOWS' callbacks are awl's own */
    wl_list_for_each_safe(p, tmp, &srv->popups.list, link) {
        if (p->plugin) {
            n++;
            popup_destroy(srv, p->win);
        }
    }
    return n;
}

void popups_fini( awl_server_t *srv ) {
    srv->popups.tree = srv->popups.persistent_tree = NULL;
    srv->popups.keyboard = NULL;
}
