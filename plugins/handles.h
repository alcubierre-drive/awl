#pragma once

/**
 * The plugin library's state, by type name only. Its root is
 * :c:type:`awl_plugin_data_t` (plugins.h), made by the library's init and
 * freed by its fini; :c:func:`awl_plugin_get` gives it to the entry points
 * (the api, the actions, the bar's widgets), which have no other way to it.
 * Each module's state is a struct defined in its .c, which its start
 * allocates and its stop frees; the root holds a pointer to each, and
 * everything else is passed: a module's functions take its handle, its
 * callbacks get it as their data, and it keeps the root (``->p``) where it
 * reaches other modules. Its fields are private to the module.
 *
 * What stays a file-scoped global is the library's ABI root, ``awl_host``
 * (awl_host.h, set by the entry point), the root's pointer itself, the
 * config (plugins_config.c: made by init, read-only after, by every
 * thread, including one that is stuck past fini) and process state whose
 * lifetime isn't init to fini (each says why where it is defined).
 */

/** plugins/osd.h */
typedef struct awl_osd awl_osd_t;
/** plugins/batpopup.h */
typedef struct awl_batpopup awl_batpopup_t;
/** plugins/sysmon.h */
typedef struct awl_sysmon awl_sysmon_t;
/** plugins/calendar.h, the calendar popup's (plugins/calpopup.h) thread */
typedef struct awl_calendar awl_calendar_t;
/** plugins/calpopup.h */
typedef struct awl_calpopup awl_calpopup_t;
/** plugins/notify_image.h, the notifications' (plugins/notify.h) image thread */
typedef struct awl_notify_images awl_notify_images_t;
/** plugins/notify.h */
typedef struct awl_notify awl_notify_t;
/** plugins/notify_menu.h, the notifications' (plugins/notify.h) menu */
typedef struct awl_notify_menu awl_notify_menu_t;
/** plugins/wallpaper.h, the wallpaper's thread (awl's own wallpaper,
 * awl_wallpaper.c, is ``struct awl_wallpaper``) */
typedef struct awl_wallpaper_thread awl_wallpaper_thread_t;
/** plugins_desktop_panel.c (plugins.h), the desktop file list's contents */
typedef struct awl_desktop_panel awl_desktop_panel_t;
/** plugins/logind.h, the logind thread */
typedef struct awl_logind awl_logind_t;
/** plugins/launcher.c (plugins.h), the launcher */
typedef struct awl_launcher awl_launcher_t;
/** plugins/sni.h, the tray's host: its bus, its loop and the items */
typedef struct awl_sni_t awl_sni_t;
/** plugins/tray_menu.h, the tray menus' window (tray_menu_window.c) */
typedef struct awl_tray_menu_window awl_tray_menu_window_t;
/** plugins/tray_tooltip.h */
typedef struct awl_tray_tooltip awl_tray_tooltip_t;
/** plugins_widgets.c (plugins.h), what the bar's widgets share */
typedef struct awl_widgets awl_widgets_t;

/* the root, plugins.h */
typedef struct awl_plugin_data_t awl_plugin_data_t;
