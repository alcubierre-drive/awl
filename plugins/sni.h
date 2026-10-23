#pragma once

/**
 * The tray's host (StatusNotifierHost): follows awl's watcher (awl_sni.h)
 * and every item it lists, on the session bus, from a loop of its own
 * (loop.h, plugins.c starts it): sd-bus attached to its sd-event loop, and
 * only asynchronous calls.
 * For each item it keeps its properties (re-read on the item's New*
 * signals), picks the icon to show (the attention one while the item needs
 * attention, if it has one) and reads its file, looked up in the item's
 * IconThemePath and the tray's themes (:c:member:`awl_config_t.tray`'s icon_theme, the
 * desktop's, Adwaita, hicolor; icons/icon.h), or takes its pixmaps. What
 * the bar shows of it all is an :c:type:`awl_sni_snap_t`, immutable once made: the
 * bar widget (plugins_widgets.c) takes the newest one on the main thread
 * and decodes the icons there at its size, so the loop never decodes and
 * the main thread never touches a file.
 *
 * The items' menus (com.canonical.dbusmenu) are plugins/sni_menu.c's, on
 * the same loop and connection, shown by tray_menu_window.c (tray_menu.h). The main thread's requests (clicks, scrolls,
 * the menus' requests and events) are queued for the loop; what goes back
 * (the menus' layouts) waits for a timer of the main thread
 * (:c:func:`sni_main_poll`), never on a pointer into the library that could
 * outlive it.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <systemd/sd-event.h>
#include "../icons/icon.h"

/** an item's Status */
enum { AWL_SNI_PASSIVE, AWL_SNI_ACTIVE, AWL_SNI_ATTENTION };

/** bytes shared by snapshots, freed with the last reference */
typedef struct awl_sni_blob_t {
    atomic_int ref;
    size_t len;
    unsigned char data[];
} awl_sni_blob_t;

/** an item, as the bar shows it */
typedef struct awl_sni_item_t {
    /** the item's while it is registered, never reused */
    uint64_t serial;
    /** its menu's serial for tray_menu_window.c (tray_menu.h), 0 if it has none */
    uint64_t menu;
    /** ItemIsMenu: a left click opens the menu instead of Activate */
    int item_is_menu;
    /** AWL_SNI_* */
    int status;
    /** what the tooltip says: ToolTip's title, else Title, else Id; never NULL */
    char* name;
    /** changes whenever the icon does */
    uint32_t icon_gen;
    /** the icon's file (a PNG, or an SVG with :c:enumerator:`AWL_ICON_SVG` in file_flags,
     * :c:enumerator:`AWL_ICON_SYMBOLIC` too if it is one), else its pixmaps: the blob starts
     * with npixmaps :c:type:`awl_icon_pixmap_t`, whose data points behind them; both
     * NULL: none */
    awl_sni_blob_t* file;
    unsigned file_flags;
    awl_sni_blob_t* pixmaps;
    int npixmaps;
} awl_sni_item_t;

/** the items, in the order they registered */
typedef struct awl_sni_snap_t {
    atomic_int ref;
    int n;
    awl_sni_item_t items[];
} awl_sni_snap_t;

/** the tray's host: its bus, its loop and the items; opaque */
typedef struct awl_sni_t awl_sni_t;

/** On event's loop, which isn't running yet; it connects from the loop. NULL
 * on failure. One at a time. */
awl_sni_t* sni_new( sd_event* event );
/** After the loop has stopped, and after :c:func:`awl_tray_menu_window_fini` (whose
 * "closed" it still sends); main thread. */
void sni_free( awl_sni_t* s );

/* ---- the main thread ---- */

/** the newest snapshot, a reference to give back with :c:func:`sni_snap_unref`;
 * never NULL */
awl_sni_snap_t* sni_snap_get( awl_sni_t* s );
/** gives a reference back; the last one frees it. NULL is fine */
void sni_snap_unref( awl_sni_snap_t* snap );
/** the size (physical pixels) the bar draws icons at, which the lookups
 * pick their files for; the largest of the bars' */
void sni_set_icon_px( awl_sni_t* s, int px );

/** what a click asks of an item */
enum { AWL_SNI_ACTIVATE, AWL_SNI_SECONDARY, AWL_SNI_CONTEXT };
/** item serial's Activate, SecondaryActivate or ContextMenu at x, y
 * (layout coordinates); an Activate that fails opens its menu instead, on
 * the monitor of the output named monitor, centered on center_x */
void sni_click( awl_sni_t* s, uint64_t serial, int what, int x, int y, const char* monitor, int center_x );
/** item serial's Scroll( delta, "vertical" or "horizontal" ) */
void sni_scroll( awl_sni_t* s, uint64_t serial, int delta, int horizontal );
/** Hands what the loop has for the main thread (the menus' layouts) to
 * tray_menu_window.c, soon, from a timer of its own. That timer also runs by
 * itself while a menu is open or being opened, or an Activate may still
 * open one, so nothing waits for a bar to be drawn; the bar widget calls
 * this on every measure too. */
void sni_main_poll( awl_sni_t* s );
