#pragma once

/**
 * awl's own popups (awl_window.h), part of awl: the plugins' (the bar
 * widgets' hover popups, the tray's menus) and AWL_TEST_WINDOWS'
 * (awl_test.h), styled by config_plugins.h's popup_config, in a tree of
 * their own right above LyrTop; made with the first one, so until a plugin
 * asks for a popup none of this runs (awl_popup.c). A popup that takes the
 * pointer gets its motion, presses and wheel before anything below it.
 * Main thread only.
 */

#include "awl_server.h"
#include "awl_window.h"

/** a popup, in popups */
typedef struct {
    awl_window_t *win;
    struct wl_list link;
    /** looks like a menu (popupsetmenustyle()) */
    int menu;
    /** the plugins' (popupcreate()), not AWL_TEST_WINDOWS' */
    int plugin;
} Popup;
/** every popup, Popup.link, the newest last (on top) */
extern struct wl_list popups;

/** a new, hidden popup of the plugins' (plugin) or of AWL_TEST_WINDOWS';
 * NULL before setup() */
awl_window_t *popupnew(int plugin);
/** a new, hidden popup for the plugins, styled by
 * popup_config; NULL before setup(). The library gives back what it made
 * before it is unloaded; pluginsreap() destroys what it left. Several can be
 * shown at once. The host table hands the plugins the rest of
 * awl_window.h as it is. */
awl_window_t *popupcreate(void);
/** nothing if win is NULL */
void popupdestroy(awl_window_t *win);
/** shows win for a bar widget on m, centered on center_x (logical pixels
 * from m's left edge), on the bar's side; a popup that isn't a menu closes
 * the menus open. A press anywhere but on the bar hides every popup. */
void popupshow(awl_window_t *win, Monitor *m, int center_x);
/** shows win on m beside the box x, y, w, h, a row of another popup, on
 * its right unless left (`awl_window_show_beside()`) */
void popupshowbeside(awl_window_t *win, Monitor *m, int x, int y, int w, int h, int left);
/** makes win look like a menu, popup_config.menu, from now on */
void popupsetmenustyle(awl_window_t *win);
/** where part of win's painter is, 0 if it is not shown
 * (`awl_window_part_box()`) */
int popuppartbox(awl_window_t *win, int part, int *x, int *y, int *w, int *h);
typedef struct awl_popup_config_t awl_popup_config_t;
/** config_plugins.h's popup_config as it is now; valid until the next
 * config reload */
const awl_popup_config_t *popupconfig(void);

/** whether the pointer is over win, one of the popups */
int pointerinpopup(awl_window_t *win);
/** the topmost popup that takes the pointer at x, y, NULL if none */
awl_window_t *popupat(double x, double y);
/** the popup that has the pointer: popupat() the cursor, unless the
 * screen is locked or the pointer busy (moving, resizing, pressed on a
 * client, dragging) */
awl_window_t *popuppointer(void);
/** The pointer moved: over a popup that takes it, that popup gets it and
 * nothing below does; returns whether it did. Logs (at verbose, -d) where
 * it goes. */
int popupmotion(void);
/** m was arranged, or its font or scale changed */
void popupsupdate(Monitor *m);
void popupshideall(void);
/** m is going */
void popupsremovemon(Monitor *m);
/** after config_plugins.h changed: every popup's look */
void popupsrestyle(void);
/** destroys every popup (a reload, see pluginsreap()); returns how many were
 * the plugins' */
int popupsreap(void);
/** the scene is going */
void popupsfini(void);
