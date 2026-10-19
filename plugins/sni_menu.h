#pragma once

/**
 * The items' menus over sd-bus (com.canonical.dbusmenu), the loop's half of
 * tray/menu.h in this library: plugins/sni.c makes one per item with a
 * menu, menu_window.c (the main thread's half) asks for its layouts through
 * sni.c's queue. Each layout becomes an `awl_tray_menu_snap_t`, its icons'
 * files already read, as tray/menu.c makes them. Calls are asynchronous and
 * find their menu by serial when they're answered: one gone by then is
 * simply not found. Loop thread only.
 */

#include <stdint.h>
#include <systemd/sd-bus.h>
#include "icon.h"
#include "../tray/menu.h"

typedef struct sni_menu sni_menu_t;

/** the menu at path on bus_name, its icons also looked up in theme_path
 * (the item's IconThemePath; NULL or "": none); NULL out of memory */
sni_menu_t* sni_menu_new( sd_bus* bus, const char* bus_name, const char* path, const char* theme_path );
/** tells the main thread it is gone; NULL is fine */
void sni_menu_free( sni_menu_t* menu );
uint64_t sni_menu_serial( const sni_menu_t* menu );
const char* sni_menu_path( const sni_menu_t* menu );
void sni_menu_set_theme_path( sni_menu_t* menu, const char* theme_path );
/** serial's menu, NULL if there's none */
sni_menu_t* sni_menu_find( uint64_t serial );
/** menu_window.c's awl_tray_menu_request(), on the loop */
void sni_menu_request( sni_menu_t* menu, uint32_t gen );
/** menu_window.c's awl_tray_menu_event(), on the loop */
void sni_menu_event( sni_menu_t* menu, int32_t id, const char* event );

/* ---- plugins/sni.c's, for the menus ---- */

/** the tray's icon themes, current */
awl_icons_t* sni_themes( void );
/** hand snap, or that serial's menu is gone, to the main thread
 * (menu_window.c) */
void sni_to_main_snap( awl_tray_menu_snap_t* snap );
void sni_to_main_gone( uint64_t serial );
