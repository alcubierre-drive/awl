#pragma once

/**
 * Icons without GTK: freedesktop icon theme lookup (the Icon Theme
 * Specification, as GTK does it) and decoding into pixman images, PNG
 * through Wuffs (vendor/wuffs/), SVG through PlutoSVG
 * (vendor/plutosvg/svg.h), right into the image's pixels. PlutoSVG has no
 * stylesheets, but the icon themes' color scheme (KDE's, which Papirus and
 * Breeze follow: <style> rules
 * ".ColorScheme-Text { color: ... }" for the elements of that class) is
 * applied before; masks, patterns and filters (a few app icons, Adwaita's
 * full color ones now and then) are left out. Icons are untrusted input (a
 * tray item's files and data), so anything unrealistically large is
 * refused before it is decoded: `AWL_ICON_MAX_BYTES`, `AWL_ICON_MAX_DIM`.
 *
 * Any thread: an `awl_icons_t` is immutable once made, apart from its
 * lookup cache, which has a lock.
 */

#include <stddef.h>
#include <stdint.h>
#include <pixman-1/pixman.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** files and data larger than this aren't read or decoded */
    AWL_ICON_MAX_BYTES = 2 << 20,
    /** nor PNGs and pixmaps wider or higher than this, nor any icon asked
     * for at more */
    AWL_ICON_MAX_DIM = 1024,
};

/** what `awl_icons_lookup()` found */
enum {
    /** a symbolic icon, by GTK's rule (-symbolic.svg, .symbolic.png): only
     * its shape counts, to be drawn in the text's color */
    AWL_ICON_SYMBOLIC = 1 << 0,
    /** an SVG file (by its name; `awl_icon_decode()` goes by the content) */
    AWL_ICON_SVG = 1 << 1,
};

typedef struct awl_icons awl_icons_t;

/** The themes named, in order (NULL terminated; NULL itself, NULL and ""
 * entries: none), each followed by what it inherits, then hicolor, looked
 * up in the usual places: ~/.icons, $XDG_DATA_HOME/icons, each of
 * $XDG_DATA_DIRS' icons, and, for icons outside any theme,
 * /usr/share/pixmaps. Themes that aren't installed are left out. Reads the
 * themes' index.theme files, so they are best made once and kept; a theme
 * changed on disk takes a new one. */
awl_icons_t* awl_icons_new( const char* const* themes );
/** The themes as GTK has them: theme (NULL or "": none, e.g.
 * `awl_config_t.tray`'s icon_theme), the desktop's (gtk-3.0/settings.ini's
 * gtk-icon-theme-name; GNOME's dconf setting isn't read), then Adwaita. */
awl_icons_t* awl_icons_new_desktop( const char* theme );
/** NULL is fine */
void awl_icons_free( awl_icons_t* icons );

/** The file of the icon name at px pixels (the logical size times the
 * scale), malloc()ed, or NULL if there is none. A path is taken as it is.
 * Else, if extra (a NULL terminated list of folders, an item's
 * IconThemePath; may be NULL) has them, hicolor's sizes there, then the
 * folders' own files; then the themes, the closest size first, and the
 * icons outside them. A name with an extension (.png, .svg) is looked up
 * without too. *flags (may be NULL): AWL_ICON_* of what was found. */
char* awl_icons_lookup( awl_icons_t* icons, const char* name, int px, const char* const* extra, unsigned* flags );

/** A file's contents, malloc()ed, its length in ``*len``; NULL if it can't be read
 * or is larger than `AWL_ICON_MAX_BYTES`. */
void* awl_icon_read( const char* path, size_t* len );

/** A PNG, or an SVG if svg, fitted into max_w x max_h keeping its aspect
 * ratio (scaled up as well as down) as premultiplied PIXMAN_a8r8g8b8; NULL
 * if it can't be decoded or is too large. source names the data in the
 * verbose log line each decode writes (its path, or e.g. "tray item 'x'";
 * NULL: "data"). */
pixman_image_t* awl_icon_decode( const void* data, size_t len, int svg, int max_w, int max_h, const char* source );
/** `awl_icon_read()` and `awl_icon_decode()` of path, SVG allowed */
pixman_image_t* awl_icon_load( const char* path, int max_w, int max_h );

/** one of an item's pixmaps (StatusNotifierItem's IconPixmap, a(iiay)): w x h
 * pixels of non-premultiplied ARGB, 4 bytes each in network byte order */
typedef struct awl_icon_pixmap_t {
    int w, h;
    const uint8_t* data;
    size_t len;
} awl_icon_pixmap_t;

/** Of n pixmaps, the smallest that covers max_w x max_h (else the largest),
 * fitted into it as `awl_icon_decode()` does; pixmaps whose data doesn't
 * match their size, or larger than `AWL_ICON_MAX_DIM`, don't count. NULL if
 * none does. */
pixman_image_t* awl_icon_from_pixmaps( const awl_icon_pixmap_t* pixmaps, int n, int max_w, int max_h );

#ifdef __cplusplus
}
#endif
