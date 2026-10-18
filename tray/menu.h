#pragma once

/**
 * The tray items' menus (com.canonical.dbusmenu), drawn by awl as one of its
 * own popups (awl_window.h) instead of a GTK menu. Two halves, one per
 * thread, that share nothing but the snapshots below and serial numbers:
 *
 * - menu.c, on the tray's GTK thread: one `awl_tray_menu_t` per item with a
 *   menu. It talks D-Bus (AboutToShow, GetLayout, Event, LayoutUpdated) and
 *   turns each layout into an `awl_tray_menu_snap_t`, with the icons' files
 *   already read (an icon theme lookup for icon-name, the PNG of icon-data),
 *   and hands it over with ``awl_host->post()``.
 * - menu_window.c, on awl's main thread: shows the newest snapshot in an
 *   awl_window, decodes the icons at the monitor's scale (off the tray thread,
 *   but never touching a file), and sends clicks back to the tray thread by
 *   serial (g_idle_add()).
 *
 * Neither half keeps a pointer into the other: a menu the tray drops meanwhile
 * (its item left) is looked up by serial and simply not found.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    /** the icon file's contents (PNG, SVG, ...), icon_len bytes, or NULL */
    uint8_t* icon;
    size_t icon_len;
    int parent, first, n;
} awl_tray_menu_node_t;

/** one layout of a menu, for the main thread; immutable once posted */
typedef struct awl_tray_menu_snap_t {
    /** the menu's (awl_tray_menu_serial()), and the request it answers */
    uint64_t serial;
    uint32_t gen;
    /** GetLayout's revision */
    uint32_t revision;
    awl_tray_menu_node_t* nodes;
    int nnodes;
} awl_tray_menu_snap_t;

void awl_tray_menu_snap_free( awl_tray_menu_snap_t* snap );

/* ---- the tray's GTK thread (menu.c) ---- */

typedef struct awl_tray_menu awl_tray_menu_t;

/** The menu at object path on bus name bus, its icons also looked up in
 * theme_path (the item's IconThemePath; NULL or "": none). Connects at once,
 * asynchronously. */
awl_tray_menu_t* awl_tray_menu_new( const char* bus, const char* path, const char* theme_path );
/** Closes it if it is open. NULL is fine. */
void awl_tray_menu_free( awl_tray_menu_t* menu );
void awl_tray_menu_set_theme_path( awl_tray_menu_t* menu, const char* theme_path );
/** A click on the item: opens the menu on the monitor of the output named
 * monitor, centered on center_x (logical pixels from its left edge), or
 * closes it if it is open there. on_icon: the pointer is on the item's tray
 * window (see awl_tray_menu_pointer()). */
void awl_tray_menu_toggle( awl_tray_menu_t* menu, const char* monitor, int center_x, int on_icon );
/** The pointer came onto the item's tray window (inside) or left it: the
 * menu open closes once the pointer is off both it and the tray for a
 * moment. */
void awl_tray_menu_pointer( awl_tray_menu_t* menu, int inside );
/** whether the main thread shows it */
int awl_tray_menu_is_open( const awl_tray_menu_t* menu );

/* ---- any thread, run on the tray's GTK thread ---- */

/** the main thread wants a snapshot of serial's menu for request gen */
void awl_tray_menu_request( uint64_t serial, uint32_t gen );
/** sends Event( id, event ) ("opened", "closed", "clicked") for serial's menu;
 * "opened" and "closed" of id 0 also tell the tray whether it is open */
void awl_tray_menu_event( uint64_t serial, int32_t id, const char* event );

/* ---- awl's main thread (menu_window.c), through awl_host->post() ---- */

/** the click awl_tray_menu_toggle() got */
void awl_tray_menu_window_toggle( uint64_t serial, const char* monitor, int center_x, int on_icon );
/** what awl_tray_menu_pointer() got */
void awl_tray_menu_window_pointer( uint64_t serial, int inside );
/** a snapshot for the main thread, which takes it */
void awl_tray_menu_window_snap( awl_tray_menu_snap_t* snap );
/** serial's menu is gone: closes it, without events */
void awl_tray_menu_window_gone( uint64_t serial );
/** whether a menu is open, or being opened */
int awl_tray_menu_window_is_open( void );

#ifdef __cplusplus
}
#endif
