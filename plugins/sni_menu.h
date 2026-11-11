#pragma once

/**
 * The items' menus over sd-bus (com.canonical.dbusmenu), the loop's half of
 * tray_menu.h: plugins/sni.c makes one per item with a menu,
 * tray_menu_window.c (the main thread's half) asks for its layouts through
 * sni.c's queue. Each layout becomes an :c:type:`awl_tray_menu_snap_t`, its icons'
 * files already read. Calls are asynchronous and
 * find their menu by serial when they're answered (:c:func:`sni_call_method_async`):
 * one gone by then is simply not found. Loop thread only.
 */

#include <stdint.h>
#include <systemd/sd-bus.h>
#include "../icons/icon.h"
#include "tray_menu.h"
#include "sni.h"

/** an item's com.canonical.dbusmenu: its layout and the requests for it; opaque */
typedef struct sni_menu sni_menu_t;

/** the menus of a tray (awl_sni_t), kept in it */
typedef struct sni_menus_t {
    sni_menu_t* head;
    /** the next menu's serial */
    uint64_t next_serial;
} sni_menus_t;

/** s's menu at path on bus_name, its icons also looked up in theme_path
 * (the item's IconThemePath; NULL or "": none); NULL out of memory */
sni_menu_t* sni_menu_new( awl_sni_t* s, sd_bus* bus, const char* bus_name, const char* path, const char* theme_path );
/** tells the main thread it is gone; NULL is fine */
void sni_menu_free( sni_menu_t* menu );
uint64_t sni_menu_serial( const sni_menu_t* menu );
const char* sni_menu_path( const sni_menu_t* menu );
void sni_menu_set_theme_path( sni_menu_t* menu, const char* theme_path );
/** serial's menu of s, NULL if there's none */
sni_menu_t* sni_menu_find( awl_sni_t* s, uint64_t serial );
/** tray_menu_window.c's :c:func:`awl_tray_menu_request`, on the loop */
void sni_menu_request( sni_menu_t* menu, uint32_t gen );
/** tray_menu_window.c's :c:func:`awl_tray_menu_event`, on the loop */
void sni_menu_event( sni_menu_t* menu, int32_t id, const char* event );

/* ---- plugins/sni.c's, for the menus ---- */

/** s's menus */
sni_menus_t* sni_menus( awl_sni_t* s );
/** s's icon themes, current */
awl_icons_t* sni_themes( awl_sni_t* s );
/** hand snap, or that serial's menu is gone, to the main thread
 * (tray_menu_window.c) */
void sni_to_main_snap( awl_sni_t* s, awl_tray_menu_snap_t* snap );
void sni_to_main_gone( awl_sni_t* s, uint64_t serial );

/** what an asynchronous call's callback gets as its userdata: the tray,
 * and a number to find what the call was for (a serial, say), which may
 * be gone by the time it is answered */
typedef struct sni_cookie_t {
    awl_sni_t* s;
    uint64_t v;
} sni_cookie_t;

/** sd_bus_call_method_async() with a cookie of s and v as callback's
 * userdata, freed with the call (answered or not) */
int sni_call_method_async( awl_sni_t* s, uint64_t v, sd_bus* bus, const char* destination, const char* path,
                           const char* interface, const char* member, sd_bus_message_handler_t callback,
                           const char* types, ... );
/** sd_bus_call_async() of call, alike */
int sni_call_async( awl_sni_t* s, uint64_t v, sd_bus* bus, sd_bus_message* call, sd_bus_message_handler_t callback );
