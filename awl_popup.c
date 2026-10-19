#include "awl_popup.h"
#include "awl_bar.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_test.h"

static awl_window_style_t popupstyle(int menu);
static Popup *popupfind(awl_window_t *win);

struct wl_list popups = {&popups, &popups};
/** the popups' tree, made with the first one */
static struct wlr_scene_tree *popup_tree;
/** the popup the pointer is over, if it takes the pointer */
static awl_window_t *popup_hovered;

int
pointerinpopup(awl_window_t *win)
{
    /* the plugin may have destroyed it without clearing popup_window */
    return popupfind(win) && awl_window_contains(win, cursor->x, cursor->y);
}

/** the popups' look, from config_plugins.h; a menu's its own (popup_config.menu) */
static awl_window_style_t
popupstyle(int menu)
{
    awl_window_style_t s = {
            .bg = cfg->popup->bg, .fg = cfg->popup->fg,
            .border = cfg->popup->border,
            .highlight = cfg->popup->hover,
            .border_width = cfg->popup->border_width,
            .radius = cfg->popup->radius, .padding = cfg->popup->padding,
            .margin = cfg->popup->margin,
            .blur = cfg->blur_launcher,
            .blur_strength = cfg->blur[0], .blur_alpha = cfg->blur[1] };
    if (menu) {
        struct awl_popup_menu_config_t c = awl_popup_menu_config(cfg->popup, cfg->blur_launcher);
        s.bg = c.bg;
        s.fg = c.fg;
        s.border = c.border;
        s.highlight = c.highlight;
        s.border_width = c.border_width;
        s.radius = c.radius;
        s.blur = c.blur;
    }
    return s;
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
        wlr_scene_node_place_above(&popup_tree->node, &layers[LyrTop]->node);
    }
    style = popupstyle(0);
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
}

void
popupshow(awl_window_t *win, Monitor *m, int center_x)
{
    Popup *p = popupfind(win), *o;
    if (!m || !p)
        return;
    /* another popup (the calendar the clock's hover opens) closes the menu
     * open at once, instead of covering it until it notices the pointer
     * left (its hidden callback closes its submenus too) */
    if (!p->menu)
        wl_list_for_each(o, &popups, link)
            if (o->menu && awl_window_monitor(o->win))
                awl_window_hide(o->win);
    awl_window_show(win, m, center_x, topbar);
}

awl_window_t *
popupat(double x, double y)
{
    Popup *p;
    /* the newest is on top */
    wl_list_for_each_reverse(p, &popups, link)
        if (awl_window_takes_pointer(p->win, x, y))
            return p->win;
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
        awl_window_hide(p->win);
}

const awl_popup_config_t *
popupconfig(void)
{
    return cfg->popup;
}

void
popupshowbeside(awl_window_t *win, Monitor *m, int x, int y, int w, int h, int left)
{
    if (m && win)
        awl_window_show_beside(win, m, &(struct wlr_box){x, y, w, h}, left);
}

void
popupsetmenustyle(awl_window_t *win)
{
    Popup *p = popupfind(win);
    awl_window_style_t style;
    if (!p)
        return;
    p->menu = 1;
    style = popupstyle(1);
    awl_window_set_style(win, &style);
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
}

void
popupsrestyle(void)
{
    Popup *p;
    awl_window_style_t style;
    wl_list_for_each(p, &popups, link) {
        style = popupstyle(p->menu);
        awl_window_set_style(p->win, &style);
    }
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
    popup_tree = NULL;
}
