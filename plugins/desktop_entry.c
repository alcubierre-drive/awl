#include "desktop_entry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a growing string ---- */

typedef struct {
    char* s;
    size_t n, cap;
    int oom;
} buf_t;

static void put( buf_t* b, char c ) {
    if (b->oom) return;
    if (b->n + 2 > b->cap) {
        size_t cap = b->cap ? 2 * b->cap : 32;
        char* s = realloc( b->s, cap );
        if (!s) {
            b->oom = 1;
            return;
        }
        b->s = s;
        b->cap = cap;
    }
    b->s[b->n++] = c;
    b->s[b->n] = '\0';
}

static void puts_( buf_t* b, const char* s ) {
    while (s && *s) put( b, *s++ );
}

/** b's string, "" if empty; NULL if out of memory */
static char* take( buf_t* b ) {
    if (b->oom) {
        free( b->s );
        return NULL;
    }
    return b->s ? b->s : strdup( "" );
}

/* ---- locales ---- */

int awl_desktop_langs( const char* locale, char out[AWL_DESKTOP_MAX_LANGS][64] ) {
    char lang[64] = "", country[64] = "", modifier[64] = "";
    if (!locale || !*locale || !strcmp( locale, "C" ) || !strcmp( locale, "POSIX" )) return 0;
    /* lang_COUNTRY.ENCODING@MODIFIER, all but lang optional */
    const char* p = locale;
    size_t k = strcspn( p, "_.@" );
    snprintf( lang, sizeof lang, "%.*s", (int)k, p );
    p += k;
    if (*p == '_') {
        k = strcspn( ++p, ".@" );
        snprintf( country, sizeof country, "%.*s", (int)k, p );
        p += k;
    }
    if (*p == '.') p += strcspn( p, "@" );
    if (*p == '@') snprintf( modifier, sizeof modifier, "%s", p + 1 );
    if (!*lang || !strcmp( lang, "C" )) return 0;

    int n = 0;
    /* real ones are short: at most 20 characters a part, so they fit */
    if (*country && *modifier) snprintf( out[n++], 64, "%.20s_%.20s@%.20s", lang, country, modifier );
    if (*country) snprintf( out[n++], 64, "%.20s_%.20s", lang, country );
    if (*modifier) snprintf( out[n++], 64, "%.20s@%.20s", lang, modifier );
    snprintf( out[n++], 64, "%s", lang );
    return n;
}

/* ---- parsing ---- */

/** v unescaped as a string value (\s \n \t \r \\); with list, split at the
 * ';' not escaped, the parts joined by sep (a trailing ';' ends the last) */
static char* unescape( const char* v, size_t len, const char* list_sep ) {
    buf_t b = { 0 };
    const char* end = v + len;
    for (; v < end; v++) {
        if (*v == '\\' && v + 1 < end) {
            switch (*++v) {
            case 's': put( &b, ' ' ); break;
            case 'n': put( &b, '\n' ); break;
            case 't': put( &b, '\t' ); break;
            case 'r': put( &b, '\r' ); break;
            default: put( &b, *v ); break; /* \\, and a list's \; */
            }
        } else if (*v == ';' && list_sep) {
            if (v + 1 < end) puts_( &b, list_sep );
        } else {
            put( &b, *v );
        }
    }
    return take( &b );
}

/** whether the ';' list v (len bytes) has one of desktops */
static int list_has( const char* v, size_t len, const char* const* desktops ) {
    for (const char* const* d = desktops; d && *d; d++) {
        const size_t dl = strlen( *d );
        if (!dl) continue;
        for (const char* p = v; p < v + len;) {
            size_t k = 0;
            while (p + k < v + len && p[k] != ';') k++;
            if (k == dl && !strncmp( p, *d, dl )) return 1;
            p += k + 1;
        }
    }
    return 0;
}

static int is_true( const char* v, size_t len ) {
    return len == 4 && !strncmp( v, "true", 4 );
}

/** a localized key's string, if it is a better language than ``*rank``'s */
static void localized( char** dst, int* rank, int r, const char* v, size_t len, const char* sep ) {
    if (r < 0 || r >= *rank) return;
    char* s = unescape( v, len, sep );
    if (!s) return;
    free( *dst );
    *dst = s;
    *rank = r;
}

int awl_desktop_entry_parse( awl_desktop_entry_t* e, const char* text, char langs[][64], int n,
                             const char* const* desktops ) {
    memset( e, 0, sizeof(*e) );
    /* the rank of what each localized key holds: the index in langs, n
     * unlocalized, n + 1 none yet */
    int rank_name = n + 1, rank_generic = n + 1, rank_keywords = n + 1;
    int in_main = 0, app = 0;
    for (const char* line = text; line && *line;) {
        const char* eol = strchr( line, '\n' );
        size_t len = eol ? (size_t)(eol - line) : strlen( line );
        const char* next = eol ? eol + 1 : NULL;
        if (len && line[len - 1] == '\r') len--;
        if (len && line[0] == '[') {
            in_main = len == 15 && !strncmp( line, "[Desktop Entry]", 15 );
            line = next;
            continue;
        }
        const char* eq = memchr( line, '=', len );
        if (!in_main || line[0] == '#' || !eq) {
            line = next;
            continue;
        }
        /* the key, spaces before '=' and after it left out */
        size_t klen = (size_t)(eq - line);
        while (klen && line[klen - 1] == ' ') klen--;
        const char* v = eq + 1;
        size_t vlen = len - (size_t)(v - line);
        while (vlen && *v == ' ') v++, vlen--;
        /* Key[locale]: r its rank, n unlocalized, -1 a language not wanted */
        size_t base = klen;
        int r = n;
        const char* lb = memchr( line, '[', klen );
        if (lb && line[klen - 1] == ']') {
            base = (size_t)(lb - line);
            const size_t ll = klen - base - 2;
            r = -1;
            for (int i = 0; i < n && r < 0; i++)
                if (strlen( langs[i] ) == ll && !strncmp( lb + 1, langs[i], ll )) r = i;
        }
#define KEY( k ) (base == sizeof(k) - 1 && !strncmp( line, k, base ))
        if (KEY( "Name" )) localized( &e->name, &rank_name, r, v, vlen, NULL );
        else if (KEY( "GenericName" )) localized( &e->generic, &rank_generic, r, v, vlen, NULL );
        else if (KEY( "Keywords" )) localized( &e->keywords, &rank_keywords, r, v, vlen, ", " );
        else if (r != n) ; /* other keys aren't localized here */
        else if (KEY( "Type" )) app = vlen == 11 && !strncmp( v, "Application", 11 );
        else if (KEY( "Icon" )) free( e->icon ), e->icon = unescape( v, vlen, NULL );
        else if (KEY( "Exec" )) free( e->exec ), e->exec = unescape( v, vlen, NULL );
        else if (KEY( "TryExec" )) free( e->try_exec ), e->try_exec = unescape( v, vlen, NULL );
        else if (KEY( "Path" )) free( e->path ), e->path = unescape( v, vlen, NULL );
        else if (KEY( "Terminal" )) e->terminal = is_true( v, vlen );
        else if (KEY( "Hidden" ) || KEY( "NoDisplay" )) e->hidden |= is_true( v, vlen );
        else if (KEY( "OnlyShowIn" )) e->hidden |= !list_has( v, vlen, desktops );
        else if (KEY( "NotShowIn" )) e->hidden |= list_has( v, vlen, desktops );
#undef KEY
        line = next;
    }
    if (!app || !e->name || !*e->name || !e->exec || !*e->exec) {
        awl_desktop_entry_clear( e );
        return 0;
    }
    /* empty ones are none */
    char** opt[] = { &e->generic, &e->keywords, &e->icon, &e->try_exec, &e->path };
    for (size_t i = 0; i < sizeof opt / sizeof *opt; i++)
        if (*opt[i] && !**opt[i]) {
            free( *opt[i] );
            *opt[i] = NULL;
        }
    return 1;
}

void awl_desktop_entry_clear( awl_desktop_entry_t* e ) {
    free( e->name );
    free( e->generic );
    free( e->keywords );
    free( e->icon );
    free( e->exec );
    free( e->try_exec );
    free( e->path );
    memset( e, 0, sizeof(*e) );
}

/* ---- Exec ---- */

typedef struct {
    char** v;
    int n, cap, oom;
} argv_t;

static void push( argv_t* a, char* s ) {
    if (!s) {
        a->oom = 1;
        return;
    }
    if (a->n + 2 > a->cap) {
        int cap = a->cap ? 2 * a->cap : 8;
        char** v = realloc( a->v, (size_t)cap * sizeof(*v) );
        if (!v) {
            free( s );
            a->oom = 1;
            return;
        }
        a->v = v;
        a->cap = cap;
    }
    a->v[a->n++] = s;
    a->v[a->n] = NULL;
}

char** awl_desktop_exec_argv( const char* exec, const char* name, const char* icon, const char* file ) {
    argv_t a = { 0 };
    const char* p = exec;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        buf_t b = { 0 };
        /* quoted: an argument even if empty; fields: had a field code
         * that expands to nothing, other: anything else (without, it goes) */
        int quoted = 0, fields = 0, other = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
            if (*p == '"') {
                quoted = 1;
                for (p++; *p && *p != '"'; p++) {
                    if (*p == '\\' && p[1] && strchr( "\"`$\\", p[1] )) p++;
                    put( &b, *p );
                }
                if (!*p) {
                    free( b.s );
                    awl_desktop_argv_free( a.v );
                    return NULL;
                }
                p++;
                continue;
            }
            if (*p != '%') {
                put( &b, *p++ );
                other = 1;
                continue;
            }
            const char code = p[1];
            p += code ? 2 : 1;
            switch (code) {
            case '%': put( &b, '%' ); other = 1; break;
            case 'c': puts_( &b, name ); other = 1; break;
            case 'k': puts_( &b, file ); other = 1; break;
            case 'i':
                /* on its own only: two arguments */
                if (!b.n && !other && (!*p || *p == ' ' || *p == '\t')) {
                    if (icon && *icon) {
                        push( &a, strdup( "--icon" ) );
                        push( &a, strdup( icon ) );
                    }
                }
                fields = 1;
                break;
            default: /* %f %F %u %U and the deprecated ones, and unknown */
                fields = 1;
                break;
            }
        }
        if (b.n || quoted || !fields) push( &a, take( &b ) );
        else free( b.s );
    }
    if (a.oom || !a.n) {
        awl_desktop_argv_free( a.v );
        return NULL;
    }
    return a.v;
}

void awl_desktop_argv_free( char** argv ) {
    if (!argv) return;
    for (char** a = argv; *a; a++) free( *a );
    free( argv );
}
