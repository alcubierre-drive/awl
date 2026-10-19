#pragma once

/**
 * The tray items' menus (com.canonical.dbusmenu), drawn by awl as one of its
 * own popups (awl_window.h). Two halves, one per thread, that share nothing
 * but the snapshots below and serial numbers:
 *
 * - sni_menu.c, on the tray's loop (plugins/sni.h): one menu per item with
 *   a menu. It talks D-Bus (AboutToShow, GetLayout, Event, LayoutUpdated)
 *   and turns each layout into an `awl_tray_menu_snap_t`, with the icons'
 *   files already read (icons/icon.h's lookup for icon-name, the PNG of
 *   icon-data), and hands it to the main thread through sni.c's mailbox.
 * - tray_menu_window.c, on awl's main thread: shows the newest snapshot in
 *   an awl_window, decodes the icons at the monitor's scale (off the loop,
 *   but never touching a file), and sends clicks back to the loop by serial.
 *
 * Neither half keeps a pointer into the other: a menu the loop drops
 * meanwhile (its item left) is looked up by serial and simply not found.
 */

#include <stddef.h>
#include <stdint.h>

/** a menu entry's flags */
enum {
    AWL_TRAY_MENU_SEPARATOR = 1 << 0,
    AWL_TRAY_MENU_DISABLED = 1 << 1,
    /** a check box or a radio button, CHECKED or MIXED if so */
    AWL_TRAY_MENU_CHECK = 1 << 2,
    AWL_TRAY_MENU_RADIO = 1 << 3,
    AWL_TRAY_MENU_CHECKED = 1 << 4,
    AWL_TRAY_MENU_MIXED = 1 << 5,
    /** opens its children instead of being activated */
    AWL_TRAY_MENU_SUBMENU = 1 << 6,
    /** its icon is a symbolic one: only its shape counts, drawn in the
     * entry's text color (as GTK does; Papirus-Dark's, for one, are dark
     * grey as they are) */
    AWL_TRAY_MENU_SYMBOLIC = 1 << 7,
    /** its icon is a file's, from icon-name, so it may be an SVG; icon-data
     * has to be a PNG */
    AWL_TRAY_MENU_FILE = 1 << 8,
};

/** One entry; the root (node 0, id 0) is the menu itself. Its children are
 * nodes first .. first + n - 1, hidden ones left out, separators collapsed
 * (none first, last or twice). */
typedef struct awl_tray_menu_node_t {
    int32_t id;
    uint32_t flags;
    /** plain text without mnemonics (never NULL); shortcut e.g. "Ctrl+Q", or
     * NULL */
    char *label, *shortcut;
    /** the icon's PNG, or SVG if AWL_TRAY_MENU_FILE, icon_len bytes (at
     * most AWL_ICON_MAX_BYTES, icons/icon.h), or NULL */
    uint8_t* icon;
    size_t icon_len;
    int parent, first, n;
} awl_tray_menu_node_t;

/** one layout of a menu, for the main thread; immutable once handed over */
typedef struct awl_tray_menu_snap_t {
    /** the menu's (sni_menu.h), and the request it answers */
    uint64_t serial;
    uint32_t gen;
    /** GetLayout's revision */
    uint32_t revision;
    awl_tray_menu_node_t* nodes;
    int nnodes;
} awl_tray_menu_snap_t;

void awl_tray_menu_snap_free( awl_tray_menu_snap_t* snap );

/* ---- any thread, run on the tray's loop (sni.c) ---- */

/** the main thread wants a snapshot of serial's menu for request gen */
void awl_tray_menu_request( uint64_t serial, uint32_t gen );
/** sends Event( id, event ) ("opened", "closed", "clicked") for serial's menu;
 * "opened" and "closed" of id 0 also tell the tray whether it is open */
void awl_tray_menu_event( uint64_t serial, int32_t id, const char* event );

/* ---- awl's main thread (tray_menu_window.c) ---- */

/** A click on the item: opens serial's menu on the monitor of the output
 * named monitor, centered on center_x (logical pixels from its left edge),
 * or closes it if it is open there. on_icon: the pointer is on the item. */
void awl_tray_menu_window_toggle( uint64_t serial, const char* monitor, int center_x, int on_icon );
/** The pointer came onto the item (inside) or left it: the menu open
 * closes once the pointer is off both it and the item for a moment. */
void awl_tray_menu_window_pointer( uint64_t serial, int inside );
/** a snapshot for the main thread, which takes it */
void awl_tray_menu_window_snap( awl_tray_menu_snap_t* snap );
/** serial's menu is gone: closes it, without events */
void awl_tray_menu_window_gone( uint64_t serial );
/** whether a menu is open, or being opened */
int awl_tray_menu_window_is_open( void );
/** whether serial's menu is open, or being opened */
int awl_tray_menu_window_shows( uint64_t serial );
/** closes it and gives back its windows and timers (the plugin library's
 * copy, before it is unloaded) */
void awl_tray_menu_window_fini( void );
