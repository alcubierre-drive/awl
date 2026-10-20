/* Unit test of plugins/fuzzy.c, the launcher's matcher. Built and run by
 * make check; prints ok/FAIL per check, the exit status is the number of
 * failures. */
#include "plugins/fuzzy.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK( cond ) do { \
        if (cond) printf( "ok    %s\n", #cond ); \
        else { printf( "FAIL  %s (line %d)\n", #cond, __LINE__ ); fails++; } \
    } while (0)

/* q against text: the score, -1000 if it doesn't match; errors into *e */
static int sc( const char* q, const char* text, int* e ) {
    awl_fuzzy_query_t fq;
    int s, err;
    awl_fuzzy_query( &fq, q );
    if (!awl_fuzzy_match( &fq, text, &s, &err, NULL )) return -1000;
    if (e) *e = err;
    return s;
}

static int matches( const char* q, const char* text ) {
    return sc( q, text, NULL ) > -1000;
}

static int errors( const char* q, const char* text ) {
    int e = -1;
    sc( q, text, &e );
    return e;
}

/* the matched characters of text marked: "Fire[f]ox" style, one char each */
static const char* marks( const char* q, const char* text ) {
    static char out[256];
    awl_fuzzy_query_t fq;
    int pos[AWL_FUZZY_MAX_QUERY], k = 0;
    awl_fuzzy_query( &fq, q );
    if (!awl_fuzzy_match( &fq, text, NULL, NULL, pos )) return "(none)";
    for (int i = 0; text[i]; i++) {
        int hit = 0;
        for (int j = 0; j < fq.n; j++) hit |= pos[j] == i;
        out[k++] = hit ? (char)(text[i] - (text[i] >= 'a' && text[i] <= 'z' ? 32 : 0)) : text[i];
    }
    out[k] = 0;
    return out;
}

int main( void ) {
    /* the budget */
    CHECK( awl_fuzzy_budget( 2 ) == 0 );
    CHECK( awl_fuzzy_budget( 3 ) == 1 );
    CHECK( awl_fuzzy_budget( 5 ) == 2 );
    CHECK( awl_fuzzy_budget( 30 ) == AWL_FUZZY_MAX_ERRORS );

    /* fzf's matches */
    CHECK( matches( "", "anything" ) );
    CHECK( matches( "ff", "Firefox" ) );
    CHECK( errors( "frfx", "Firefox" ) == 0 );
    CHECK( matches( "FIREFOX", "firefox" ) );
    CHECK( !matches( "xf", "Firefox" ) );
    CHECK( !matches( "zz", "Firefox" ) );

    /* each kind of typo */
    CHECK( errors( "firwfox", "Firefox" ) == 1 );  /* wrong */
    CHECK( errors( "fiirefox", "Firefox" ) == 1 ); /* extra */
    CHECK( errors( "fierfox", "Firefox" ) == 1 );  /* swapped */
    CHECK( errors( "frefox", "Firefox" ) == 0 );   /* left out: a gap */
    CHECK( errors( "girefox", "Firefox" ) == 1 );  /* wrong first one */
    /* and several in one query */
    CHECK( errors( "fierfxo", "Firefox" ) == 2 );
    CHECK( errors( "thunderbrid", "Thunderbird" ) == 1 );
    CHECK( errors( "libreofice writr", "LibreOffice Writer" ) == 0 );
    CHECK( errors( "lbireoffice", "LibreOffice" ) == 1 );
    CHECK( errors( "kepassxc", "KeePassXC" ) == 0 );
    CHECK( errors( "keepasscx", "KeePassXC" ) == 1 );

    /* but not more than the budget */
    CHECK( matches( "fx", "Fix" ) );
    CHECK( !matches( "fo", "Fa" ) );          /* 2 characters: no errors */
    CHECK( errors( "fxrefqx", "Firefox" ) == 2 );
    CHECK( !matches( "abcdefgh", "Firefox" ) );
    /* an error doesn't glue far apart pieces together: a wrong character
     * only right after a match */
    CHECK( !matches( "vqz", "vim and some text" ) );
    /* nor a wrong first character a gap away from the rest, nor an extra
     * one after a gap */
    CHECK( !matches( "fierfox", "system, process, task, resources, monitoring, performance" ) );
    CHECK( !matches( "xfire", "a fire" ) );
    CHECK( errors( "xfire", "x fire" ) == 0 );
    CHECK( errors( "firexfox", "Firefox" ) == 1 );
    /* nor an error next to a lone character, or a short run */
    CHECK( !matches( "term", "Text Editor" ) );
    CHECK( !matches( "term", "Document Viewer" ) );
    CHECK( !matches( "term", "Calendar, Event, Reminder" ) );
    CHECK( errors( "term", "System Monitor" ) == 1 );

    /* ranking: exact before typos, word starts before the middle */
    CHECK( sc( "vim", "Vim", NULL ) > sc( "vim", "Neovim", NULL ) );
    CHECK( sc( "vim", "Neovim", NULL ) > sc( "vim", "Video Player", NULL ) );
    CHECK( sc( "term", "Terminal", NULL ) >= sc( "term", "Xfce Settings Terminal", NULL ) );
    CHECK( sc( "firefox", "Firefox", NULL ) > sc( "fierfox", "Firefox", NULL ) );
    CHECK( sc( "gimp", "GNU Image Manipulation Program", NULL ) > -1000 );
    CHECK( sc( "ff", "Firefox", NULL ) < sc( "fi", "Firefox", NULL ) );
    CHECK( sc( "ts", "Telegram Settings", NULL ) > sc( "ts", "Thunar Bulk Rename tests", NULL ) );

    /* positions */
    CHECK( !strcmp( marks( "frfx", "firefox" ), "FiReFoX" ) );
    CHECK( !strcmp( marks( "fierfox", "firefox" ), "FIREFOX" ) );
    CHECK( !strcmp( marks( "firwfox", "firefox" ), "FIReFOX" ) );
    CHECK( !strcmp( marks( "gimp", "gnu image manipulation program" ), "Gnu Image ManiPulation program" ) );

    /* UTF-8 and case beyond ASCII */
    CHECK( matches( "über", "Übersicht" ) );
    CHECK( matches( "écran", "ÉCRAN" ) );
    CHECK( errors( "дискт", "Диски" ) == 1 );

    printf( "%d failed\n", fails );
    return fails;
}
