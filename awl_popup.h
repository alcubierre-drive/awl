#pragma once

/**
 * awl's own popups (awl_window.h), part of awl: the plugins' (the bar
 * widgets' hover popups, the tray's menus, the notifications) and
 * AWL_TEST_WINDOWS' (awl_test.h), styled by cfg->popup, in a tree of
 * their own right above LyrTop; made with the first one, so until a plugin
 * asks for a popup none of this runs (awl_popup.c). A popup that takes the
 * pointer gets its motion, presses and wheel before anything below it,
 * unless something above it takes the pointer there (a fullscreen client).
 *
 * Persistent popups (popupsetpersistent(), the notifications) stay until
 * they are hidden: a press elsewhere, another popup shown and locking the
 * screen leave them (they are only out of sight while it is locked). They
 * are in a tree of their own above the others: right above LyrOverlay if
 * cfg->popup.persistent_overlay (over fullscreen clients), else right above
 * the others' tree. Main thread only.
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
    /** popupsetpersistent() */
    int persistent;
    /** popupsetcolors(), 0: the style's */
    uint32_t bg, fg, border;
} Popup;
/** every popup, Popup.link, the newest last (on top) */
extern struct wl_list popups;

/** a new, hidden popup of the plugins' (plugin) or of AWL_TEST_WINDOWS';
 * NULL before setup() */
awl_window_t *popupnew(int plugin);
/** a new, hidden popup for the plugins, styled by
 * cfg->popup; NULL before setup(). The library gives back what it made
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
/** shows win on m at a spot of its window area, an AWL_WINDOW_AT_*, moved
 * by dx, dy (`awl_window_show_at()`); closes the menus open, as popupshow() */
void popupshowat(awl_window_t *win, Monitor *m, int at, int dx, int dy);
/** hides win (`awl_window_hide()`) */
void popuphide(awl_window_t *win);
/** win takes the keyboard while shown (`awl_window_set_key()`) */
void popupsetkey(awl_window_t *win, awl_window_key_t key, void *data);
/** the popup that has the keyboard: the newest shown one with a key
 * callback, NULL if none. While there is one, no client has the keyboard
 * focus: keypress() hands it every key before the bindings. */
awl_window_t *popupkeyboard(void);
/** after popups were shown or hidden: takes the keyboard focus from the
 * clients when a popup takes the keys, gives it back (focustop(selmon))
 * when none does anymore */
void popupsgrabupdate(void);
/** shows win on m beside the box x, y, w, h, a row of another popup, on
 * its right unless left (`awl_window_show_beside()`) */
void popupshowbeside(awl_window_t *win, Monitor *m, int x, int y, int w, int h, int left);
/** makes win look like a menu, cfg->popup.menu, from now on */
void popupsetmenustyle(awl_window_t *win);
/** makes win persistent, or not (see above), from now on */
void popupsetpersistent(awl_window_t *win, int persistent);
/** win's own background, text and border colors (0xRRGGBBAA) over the
 * style's, 0 keeps the style's */
void popupsetcolors(awl_window_t *win, uint32_t bg, uint32_t fg, uint32_t border);
/** win's last placement (`awl_window_box()`) into x, y, w, h; returns
 * whether it is visible */
int popupbox(awl_window_t *win, int *x, int *y, int *w, int *h);
/** the room win has on m at a spot (`awl_window_room()`) into x, y, w, h;
 * all 0 for no popup or monitor */
void popuproom(awl_window_t *win, Monitor *m, int *x, int *y, int *w, int *h);
/** whether win is a persistent popup */
int popuppersistent(awl_window_t *win);
/** the layer the persistent popups' tree is above (LyrOverlay, or LyrTop
 * with the others' between), -1 before there is one; *visible whether they
 * are in sight */
int popuppersistentlayer(int *visible);
/** the screen was locked or unlocked (`locked`): the persistent popups go
 * out of sight while it is, and come back after */
void popupslocked(void);
/** where part of win's painter is, 0 if it is not shown
 * (`awl_window_part_box()`) */
int popuppartbox(awl_window_t *win, int part, int *x, int *y, int *w, int *h);
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
/** hides every popup but the persistent ones */
void popupshideall(void);
/** m is going */
void popupsremovemon(Monitor *m);
/** after plugins_config.c changed: every popup's look, and where the
 * persistent ones are */
void popupsrestyle(void);
/** destroys every popup (a reload, see pluginsreap()); returns how many were
 * the plugins' */
int popupsreap(void);
/** the scene is going */
void popupsfini(void);
