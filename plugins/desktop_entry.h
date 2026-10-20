#pragma once

/**
 * Desktop entries (the Desktop Entry Specification), for the launcher
 * (launcher.h): parsing an application's .desktop file and turning its Exec
 * line into an argv. Pure functions on strings, no file system, any thread
 * (test/desktop_entry_unit.c runs them).
 */

/** what the launcher needs of an application's .desktop file; every string
 * malloc()ed or NULL */
typedef struct awl_desktop_entry_t {
    /** Name, GenericName, Keywords (joined by ", "), in the best language
     * the file has */
    char *name, *generic, *keywords;
    /** Icon: a name or a path */
    char* icon;
    /** Exec, unescaped (`awl_desktop_exec_argv()` takes it so), TryExec,
     * Path (the working directory) */
    char *exec, *try_exec, *path;
    int terminal;
    /** Hidden, NoDisplay, or OnlyShowIn/NotShowIn ruling the desktop out:
     * not to be shown (Hidden still masks the id in later directories) */
    int hidden;
} awl_desktop_entry_t;

/** at most this many languages `awl_desktop_langs()` makes */
#define AWL_DESKTOP_MAX_LANGS 4

/** The languages a localized key matches for locale (``lang_COUNTRY.ENCODING@MODIFIER``,
 * as in $LC_MESSAGES), best first, as the spec orders them:
 * ``lang_COUNTRY@MODIFIER``, ``lang_COUNTRY``, ``lang@MODIFIER``, ``lang``.
 * Returns how many; none for NULL, "", "C" and "POSIX". */
int awl_desktop_langs( const char* locale, char out[AWL_DESKTOP_MAX_LANGS][64] );

/** Parses a .desktop file's text into e (zeroed first) for an application:
 * its [Desktop Entry] group, localized keys in the first of langs (n of
 * them) the file has. desktops (NULL terminated, $XDG_CURRENT_DESKTOP's)
 * decide OnlyShowIn and NotShowIn. Returns 1 for an application (Type
 * Application with a Name and an Exec), hidden or not, else 0 with e empty. */
int awl_desktop_entry_parse( awl_desktop_entry_t* e, const char* text, char langs[][64], int n,
                             const char* const* desktops );
/** frees what e holds and zeroes it */
void awl_desktop_entry_clear( awl_desktop_entry_t* e );

/** The argv of an Exec value (unescaped as the entry has it): split at
 * blanks, double quotes quoting (in them, a backslash escapes ", `, $ and
 * itself), field codes expanded: %% a '%', %c the name, %k file (the
 * .desktop file's path, NULL: none), %i as an argument of its own
 * "--icon" and the icon if there is one; the file and URL ones (%f %F %u
 * %U, the deprecated %d %D %n %N %v %m) dropped, an argument that was only
 * one of them with it. malloc()ed, NULL terminated, each argument
 * malloc()ed (`awl_desktop_argv_free()`); NULL if it has none, or for an
 * unterminated quote. */
char** awl_desktop_exec_argv( const char* exec, const char* name, const char* icon, const char* file );
void awl_desktop_argv_free( char** argv );
