#pragma once

/* app_env.c's side the tray itself calls: main thread only, like
 * awl_tray_app_env() */

#ifdef __cplusplus
extern "C" {
#endif

/** the config's icon_theme (NULL or "": none); the next awl_tray_app_env()
 * makes it anew if it changed */
void awl_tray_app_set_theme( const char* theme );
/** removes what awl_tray_app_env() made */
void awl_tray_app_cleanup( void );

#ifdef __cplusplus
}
#endif
