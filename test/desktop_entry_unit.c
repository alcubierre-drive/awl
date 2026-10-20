/* Unit test of plugins/desktop_entry.c, the launcher's .desktop files and
 * Exec lines. Built and run by meson test; prints ok/FAIL per check, the
 * exit status is the number of failures. */
#include "plugins/desktop_entry.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK( cond ) do { \
        if (cond) printf( "ok    %s\n", #cond ); \
        else { printf( "FAIL  %s (line %d)\n", #cond, __LINE__ ); fails++; } \
    } while (0)

/* exec's argv joined by '|', "(null)" for none */
static const char* argv_of( const char* exec ) {
    static char out[512];
    char** v = awl_desktop_exec_argv( exec, "My App", "my-icon", "/a/b.desktop" );
    if (!v) return "(null)";
    out[0] = '\0';
    for (char** a = v; *a; a++) {
        if (a != v) strcat( out, "|" );
        strcat( out, *a );
    }
    awl_desktop_argv_free( v );
    return out;
}

static const char* const desktops[] = { "wlroots", NULL };

int main( void ) {
    char langs[AWL_DESKTOP_MAX_LANGS][64];
    awl_desktop_entry_t e;

    /* languages */
    CHECK( awl_desktop_langs( "de_DE.UTF-8@euro", langs ) == 4 );
    CHECK( !strcmp( langs[0], "de_DE@euro" ) && !strcmp( langs[1], "de_DE" ) && !strcmp( langs[2], "de@euro" ) &&
           !strcmp( langs[3], "de" ) );
    CHECK( awl_desktop_langs( "de_DE.UTF-8", langs ) == 2 && !strcmp( langs[0], "de_DE" ) );
    CHECK( awl_desktop_langs( "C.UTF-8", langs ) == 0 );
    CHECK( awl_desktop_langs( "POSIX", langs ) == 0 );
    CHECK( awl_desktop_langs( NULL, langs ) == 0 );

    /* an application, localized */
    int n = awl_desktop_langs( "de_DE.UTF-8", langs );
    CHECK( awl_desktop_entry_parse( &e,
        "# comment\n"
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Files\n"
        "Name[de]=Dateien\n"
        "Name[fr]=Fichiers\n"
        "GenericName = File Manager\n"
        "Keywords=folder;manager;explore\\;disk;\n"
        "Keywords[de_DE]=Ordner;Verwaltung;\n"
        "Icon=org.gnome.Nautilus\n"
        "Exec=nautilus --new-window %U\n"
        "Terminal=false\n"
        "\n"
        "[Desktop Action new-window]\n"
        "Name=New Window\n"
        "Exec=nautilus --new-window\n", langs, n, desktops ) == 1 );
    CHECK( !strcmp( e.name, "Dateien" ) );
    CHECK( !strcmp( e.generic, "File Manager" ) );
    CHECK( !strcmp( e.keywords, "Ordner, Verwaltung" ) );
    CHECK( !strcmp( e.icon, "org.gnome.Nautilus" ) );
    CHECK( !strcmp( e.exec, "nautilus --new-window %U" ) );
    CHECK( !e.terminal && !e.hidden && !e.path && !e.try_exec );
    awl_desktop_entry_clear( &e );
    /* unlocalized */
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\r\nType=Application\r\nName[de]=Dateien\r\n"
                                        "Name=Files\r\nExec=x\r\nKeywords=a;b\\;c;d\r\n",
                                    langs, 0, desktops ) == 1 );
    CHECK( !strcmp( e.name, "Files" ) && !strcmp( e.keywords, "a, b;c, d" ) );
    awl_desktop_entry_clear( &e );

    /* hidden in their ways */
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\nType=Application\nName=A\nExec=a\nNoDisplay=true\n",
                                    langs, n, desktops ) == 1 && e.hidden );
    awl_desktop_entry_clear( &e );
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\nType=Application\nName=A\nExec=a\nHidden=true\n",
                                    langs, n, desktops ) == 1 && e.hidden );
    awl_desktop_entry_clear( &e );
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\nType=Application\nName=A\nExec=a\nOnlyShowIn=GNOME;KDE;\n",
                                    langs, n, desktops ) == 1 && e.hidden );
    awl_desktop_entry_clear( &e );
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\nType=Application\nName=A\nExec=a\nOnlyShowIn=GNOME;wlroots;\n",
                                    langs, n, desktops ) == 1 && !e.hidden );
    awl_desktop_entry_clear( &e );
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\nType=Application\nName=A\nExec=a\nNotShowIn=wlroots\n",
                                    langs, n, desktops ) == 1 && e.hidden );
    awl_desktop_entry_clear( &e );
    /* not applications */
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\nType=Link\nName=A\nURL=http://x\n", langs, n, desktops ) == 0 );
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\nType=Application\nName=A\n", langs, n, desktops ) == 0 );
    CHECK( awl_desktop_entry_parse( &e, "[Other]\nType=Application\nName=A\nExec=a\n", langs, n, desktops ) == 0 );
    CHECK( awl_desktop_entry_parse( &e, "", langs, n, desktops ) == 0 );
    /* the rest */
    CHECK( awl_desktop_entry_parse( &e, "[Desktop Entry]\nType=Application\nName=A\nExec=htop\nTerminal=true\n"
                                        "Path=/tmp\nTryExec=htop\nIcon=\n", langs, n, desktops ) == 1 );
    CHECK( e.terminal && !strcmp( e.path, "/tmp" ) && !strcmp( e.try_exec, "htop" ) && !e.icon );
    awl_desktop_entry_clear( &e );

    /* Exec */
    CHECK( !strcmp( argv_of( "firefox %u" ), "firefox" ) );
    CHECK( !strcmp( argv_of( "  a  b\tc " ), "a|b|c" ) );
    CHECK( !strcmp( argv_of( "app --x=%f" ), "app|--x=" ) );
    CHECK( !strcmp( argv_of( "app %F %U %d %D %n %N %v %m" ), "app" ) );
    CHECK( !strcmp( argv_of( "app %i" ), "app|--icon|my-icon" ) );
    CHECK( !strcmp( argv_of( "app --title=%c %k" ), "app|--title=My App|/a/b.desktop" ) );
    CHECK( !strcmp( argv_of( "app 100%%" ), "app|100%" ) );
    CHECK( !strcmp( argv_of( "\"/opt/My App/bin/app\" \"a b\" \"\"" ), "/opt/My App/bin/app|a b|" ) );
    CHECK( !strcmp( argv_of( "sh -c \"echo \\\"hi\\\" \\$HOME \\\\ \\`x\\`\"" ), "sh|-c|echo \"hi\" $HOME \\ `x`" ) );
    CHECK( !strcmp( argv_of( "app \"unterminated" ), "(null)" ) );
    CHECK( !strcmp( argv_of( "" ), "(null)" ) );
    CHECK( !strcmp( argv_of( "%U" ), "(null)" ) );

    printf( "%d failed\n", fails );
    return fails;
}
