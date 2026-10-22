#pragma once

/**
 * The notifications' images from files and icon names (notify.h), decoded
 * on a thread of their own: started with the first wish, stopped with the
 * windows. A wish names a notification's sources in the spec's order,
 * image_path, app_icon, then desktop_entry's Icon. Each source is an
 * absolute path, a ``file://`` URL (percent-decoded) or an icon name
 * looked up in the desktop's themes (`awl_config_t.tray`'s icon_theme
 * first). The first source that yields an image wins. Files may be
 * photos and screenshots, larger than icons: `awl_icon_limits_t` allows
 * them more here, and only here (12 megapixels, 16 MiB), turned as their
 * EXIF orientation says. What a file came out as at a size is kept (32
 * of them, 8 MiB of pixels, the least recently used go) until the file
 * changes (its inode, size or modification time): an app sends the same
 * icon with each notification.
 *
 * The bar's bell (`awl_plugin_config_t.notify`'s bell_icon) comes from
 * here too, so no file system is touched on the main thread for it.
 *
 * Everything a client names is untrusted: a path that is no regular file
 * (a FIFO, a device) is refused without being read, a desktop entry id is
 * a file name in the applications directories, nothing else.
 *
 * Main thread only, apart from the thread itself.
 */

#include <pixman-1/pixman.h>
#include <stdint.h>

/** what came of a wish, `awl_notify_image_take()`'s */
typedef struct awl_notify_image_t {
    struct awl_notify_image_t* next;
    /** the wish's: the notification, which of its sources, the size */
    uint32_t id;
    uint64_t serial;
    int px;
    /** fitted into px x px; NULL if no source yielded one */
    pixman_image_t* img;
    /** a symbolic icon: its shape is to be drawn in the text's color */
    int symbolic;
} awl_notify_image_t;

/** Asks for the first image of the sources (NULL and "" for none) at px
 * pixels, for notification id's serial. Returns 0 if it can't (no thread,
 * no memory, no source): nothing will come of it then. */
int awl_notify_image_want( uint32_t id, uint64_t serial, int px, const char* image_path, const char* app_icon,
                           const char* desktop_entry );
/** what is done since the last call, a list, oldest first; NULL none */
awl_notify_image_t* awl_notify_image_take( void );
/** Asks for the first of names (the config's: icon names or paths, ";"
 * between them) the themes have, at px pixels, for the bar; key is the
 * caller's, handed back as the result's serial (its id is 0). The thread
 * asks for a redraw when it is done. Returns 0 if it can't: nothing will
 * come of it then. */
int awl_notify_image_icon_want( uint64_t key, int px, const char* names );
/** what `awl_notify_image_icon_want()` asked for that is done since the
 * last call, like `awl_notify_image_take()`'s */
awl_notify_image_t* awl_notify_image_icon_take( void );
/** frees a list of what came, the images with it */
void awl_notify_image_free( awl_notify_image_t* list );
/** Stops the thread, dropping what is wished for and done. Returns nonzero
 * if it is stuck (in a file system): it is left behind with what it has. */
int awl_notify_image_stop( void );
