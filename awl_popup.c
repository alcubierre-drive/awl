#include "awl_popup.h"
#include "awl_bar.h"
#include "awl_client.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_test.h"

static awl_window_style_t popupstyle(const Popup *p);
static Popup *popupfind(awl_window_t *win);

struct wl_list popups = {&popups, &popups};
/** the popups' tree, made with the first one; the persistent ones', made
 * with the first of them (popupsplace()) */
static struct wlr_scene_tree *popup_tree, *persistent_tree;
/** the popup the pointer is over, if it takes the pointer */
static awl_window_t *popup_hovered;
/** the popup that had the keyboard at the last :c:func:`popupsgrabupdate` */
static awl_window_t *popup_keyboard;

int
pointerinpopup(awl_window_t *win)
{
    /* the plugin may have destroyed it without clearing popup_window */
    return popupfind(win) && awl_window_contains(win, cursor->x, cursor->y);
}

/** p's look, from cfg->popup and cfg->colors' popup_*; a menu's its own
 * (:c:func:`awl_menu_style`); p's colors over either (NULL: none) */
static awl_window_style_t
popupstyle(const Popup *p)
{
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
        awl_menu_style_t c = awl_menu_style(&cfg->colors, &cfg->popup, cfg->blur.popups);
        s.bg = c.bg;
        s.fg = c.fg;
        s.border = c.border;
        s.highlight = c.highlight;
        s.border_width = c.border_width;
        s.radius = c.radius;
        s.blur = c.blur;
    }
    if (p && p->bg)
        s.bg = p->bg;
    if (p && p->fg)
        s.fg = p->fg;
    if (p && p->border)
        s.border = p->border;
    return s;
}

/** whether a popup other than a persistent one is shown (a menu, a hover
 * popup, a tooltip) */
static int
popupsshown(void)
{
    Popup *p;
    wl_list_for_each(p, &popups, link)
        if (!p->persistent && awl_window_monitor(p->win))
            return 1;
    return 0;
}

/** puts the trees where they belong: the popups' right above LyrTop, the
 * persistent ones' right above it or above LyrOverlay; while another popup
 * is shown, the popups' right above the persistent ones', so that it covers
 * them (the notification center, a hover popup over the notifications) */
static void
popupsplace(void)
{
    struct wlr_scene_node *persistent_below = cfg->popup.persistent_overlay
            ? &layers[LyrOverlay]->node : &layers[LyrTop]->node;
    if (persistent_tree && popup_tree && popupsshown()) {
        wlr_scene_node_place_above(&persistent_tree->node, persistent_below);
        wlr_scene_node_place_above(&popup_tree->node, &persistent_tree->node);
        return;
    }
    if (popup_tree)
        wlr_scene_node_place_above(&popup_tree->node, &layers[LyrTop]->node);
    if (persistent_tree)
        wlr_scene_node_place_above(&persistent_tree->node, cfg->popup.persistent_overlay
                ? persistent_below : popup_tree ? &popup_tree->node : persistent_below);
}

/** win's entry in popups, NULL if it is not (or no longer) one of them */
static Popup *
popupfind(awl_window_t *win)
{
    Popup *p;
    if (win)
        wl_list_for_each(p, &popups, link)
            if (p->win == win)
                return p;
    return NULL;
}

awl_window_t *
popupnew(int plugin)
{
    Popup *p;
    awl_window_style_t style;
    if (!scene)
        return NULL;
    /* over the bar and the windows, under fullscreen ones */
    if (!popup_tree) {
        popup_tree = wlr_scene_tree_create(&scene->tree);
        popupsplace();
    }
    style = popupstyle(NULL);
    p = ecalloc(1, sizeof(*p));
    p->win = awl_window_create(popup_tree, &style);
    p->plugin = plugin;
    wl_list_insert(popups.prev, &p->link);
    return p->win;
}

awl_window_t *
popupcreate(void)
{
    return popupnew(1);
}

void
popupdestroy(awl_window_t *win)
{
    Popup *p = popupfind(win);
    if (!p)
        return;
    testpopupdestroyed(win);
    if (popup_hovered == win)
        popup_hovered = NULL;
    wl_list_remove(&p->link);
    awl_window_destroy(p->win);
    free(p);
    popupsgrabupdate();
}

/** another popup than a menu (the calendar the clock's hover opens)
 * closes the menu open at once, instead of covering it until it notices the
 * pointer left (its hidden callback closes its submenus too) */
static void
closemenus(Popup *p)
{
    Popup *o;
    if (!p->menu && !p->persistent)
        wl_list_for_each(o, &popups, link)
            if (o->menu && awl_window_monitor(o->win))
                awl_window_hide(o->win);
}

void
popupshow(awl_window_t *win, Monitor *m, int center_x)
{
    Popup *p = popupfind(win);
    if (!m || !p)
        return;
    closemenus(p);
    awl_window_show(win, m, center_x, cfg->bar.top);
    popupsgrabupdate();
}

void
popupshowat(awl_window_t *win, Monitor *m, int at, int dx, int dy)
{
    Popup *p = popupfind(win);
    if (!m || !p)
        return;
    closemenus(p);
    awl_window_show_at(win, m, at, dx, dy);
    popupsgrabupdate();
}

void
popuphide(awl_window_t *win)
{
    awl_window_hide(win);
    popupsgrabupdate();
}

void
popupsetkey(awl_window_t *win, awl_window_key_t key, void *data)
{
    awl_window_set_key(win, key, data);
    popupsgrabupdate();
}

awl_window_t *
popupkeyboard(void)
{
    Popup *p;
    wl_list_for_each_reverse(p, &popups, link)
        if (awl_window_takes_keys(p->win) && !(locked && p->persistent))
            return p->win;
    return NULL;
}

void
popupsgrabupdate(void)
{
    awl_window_t *win = popupkeyboard();
    /* a popup shown or hidden: its place over the persistent ones */
    popupsplace();
    if (!win == !popup_keyboard) {
        popup_keyboard = win;
        return;
    }
    popup_keyboard = win;
    awl_dbg_printf(awl_loglevel_vrb, "popup %p: %s the keyboard\n", (void *)win,
            win ? "takes" : "gives back");
    if (locked)
        return;
    if (win)
        wlr_seat_keyboard_notify_clear_focus(seat);
    else if (!exclusive_focus)
        focusclient(focustop(selmon), 0);
}

/** whether something above win's tree takes the pointer at x, y: a
 * fullscreen client over the popups, a layer surface over the persistent
 * ones (the popups themselves never do, awl_window.c) */
static int
popupcovered(awl_window_t *win, int persistent, double x, double y)
{
    struct wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node, x, y, NULL, NULL);
    struct wlr_scene_node *ours = persistent ? &persistent_tree->node : &popup_tree->node;
    struct wlr_scene_node *it;
    if (!node)
        return 0;
    /* the scene's child that has it, then whether that is above ours */
    while (node->parent && node->parent != &scene->tree)
        node = &node->parent->node;
    for (it = ours; &it->link != &scene->tree.children;
            it = wl_container_of(it->link.next, it, link))
        if (it == node)
            return it != ours;
    return 0;
}

awl_window_t *
popupat(double x, double y)
{
    Popup *p;
    /* the top tree's first (popupsplace()), then the newest */
    const int top = !popupsshown();
    for (int i = 0; i < 2; i++)
        wl_list_for_each_reverse(p, &popups, link)
            if (p->persistent == (i ? !top : top) && awl_window_takes_pointer(p->win, x, y))
                return popupcovered(p->win, p->persistent, x, y) ? NULL : p->win;
    return NULL;
}

awl_window_t *
popuppointer(void)
{
    if (cursor_mode != CurNormal || seat->drag || locked)
        return NULL;
    return popupat(cursor->x, cursor->y);
}

int
popupmotion(void)
{
    awl_window_t *win = popuppointer();
    int row;

    if (popup_hovered && popup_hovered != win) {
        awl_window_pointer_leave(popup_hovered);
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: pointer left\n", (void *)popup_hovered);
    }
    if (!win) {
        popup_hovered = NULL;
        return 0;
    }
    if (popup_hovered != win) {
        wlr_seat_pointer_notify_clear_focus(seat);
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
        updatehover(NULL);
        popup_hovered = win;
    }
    row = awl_window_hover_row(win);
    if (awl_window_pointer_motion(win, cursor->x, cursor->y) != row)
        awl_dbg_printf(awl_loglevel_vrb, "popup %p: pointer on row %d at %.0f,%.0f\n", (void *)win,
                awl_window_hover_row(win), cursor->x, cursor->y);
    return 1;
}

void
popupsupdate(Monitor *m)
{
    Popup *p;
    wl_list_for_each(p, &popups, link)
        awl_window_update(p->win, m);
}

void
popupshideall(void)
{
    Popup *p;
    wl_list_for_each(p, &popups, link)
        if (!p->persistent)
            awl_window_hide(p->win);
    popupsgrabupdate();
}

void
popupshowbeside(awl_window_t *win, Monitor *m, int x, int y, int w, int h, int left)
{
    if (m && win)
        awl_window_show_beside(win, m, &(struct wlr_box){x, y, w, h}, left);
    popupsgrabupdate();
}

void
popupsetmenustyle(awl_window_t *win)
{
    Popup *p = popupfind(win);
    awl_window_style_t style;
    if (!p)
        return;
    p->menu = 1;
    style = popupstyle(p);
    awl_window_set_style(win, &style);
}

void
popupsetpersistent(awl_window_t *win, int persistent)
{
    Popup *p = popupfind(win);
    if (!p || p->persistent == !!persistent)
        return;
    p->persistent = !!persistent;
    if (p->persistent && !persistent_tree) {
        persistent_tree = wlr_scene_tree_create(&scene->tree);
        popupsplace();
        popupslocked();
    }
    awl_window_reparent(win, p->persistent ? persistent_tree : popup_tree);
}

void
popupsetcolors(awl_window_t *win, uint32_t bg, uint32_t fg, uint32_t border)
{
    Popup *p = popupfind(win);
    awl_window_style_t style;
    if (!p || (p->bg == bg && p->fg == fg && p->border == border))
        return;
    p->bg = bg;
    p->fg = fg;
    p->border = border;
    style = popupstyle(p);
    awl_window_set_style(win, &style);
}

int
popupbox(awl_window_t *win, int *x, int *y, int *w, int *h)
{
    struct wlr_box box = {0};
    int visible = popupfind(win) && awl_window_box(win, &box);
    *x = box.x;
    *y = box.y;
    *w = box.width;
    *h = box.height;
    return visible;
}

void
popuproom(awl_window_t *win, Monitor *m, int *x, int *y, int *w, int *h)
{
    struct wlr_box box = {0};
    if (popupfind(win) && m)
        box = awl_window_room(win, m);
    *x = box.x;
    *y = box.y;
    *w = box.width;
    *h = box.height;
}

int
popuppersistent(awl_window_t *win)
{
    Popup *p = popupfind(win);
    return p && p->persistent;
}

int
popuppersistentlayer(int *visible)
{
    *visible = persistent_tree && persistent_tree->node.enabled;
    if (!persistent_tree)
        return -1;
    return persistent_tree->node.link.prev == &layers[LyrOverlay]->node.link ? LyrOverlay : LyrTop;
}

void
popupslocked(void)
{
    if (persistent_tree)
        wlr_scene_node_set_enabled(&persistent_tree->node, !locked);
}

int
popuppartbox(awl_window_t *win, int part, int *x, int *y, int *w, int *h)
{
    struct wlr_box box;
    if (!win || !awl_window_part_box(win, part, &box))
        return 0;
    *x = box.x;
    *y = box.y;
    *w = box.width;
    *h = box.height;
    return 1;
}

void
popupsremovemon(Monitor *m)
{
    Popup *p;
    wl_list_for_each(p, &popups, link)
        awl_window_removemon(p->win, m);
    popupsgrabupdate();
}

void
popupsrestyle(void)
{
    Popup *p;
    awl_window_style_t style;
    wl_list_for_each(p, &popups, link) {
        style = popupstyle(p);
        awl_window_set_style(p->win, &style);
    }
    popupsplace();
}

int
popupsreap(void)
{
    Popup *p, *tmp;
    int n = 0;

    /* AWL_TEST_WINDOWS' go too: a reload destroys every popup */
    wl_list_for_each_safe(p, tmp, &popups, link) {
        n += p->plugin;
        popupdestroy(p->win);
    }
    return n;
}

void
popupsfini(void)
{
    popup_tree = persistent_tree = NULL;
    popup_keyboard = NULL;
}
