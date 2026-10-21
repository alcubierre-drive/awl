/* Unit test of plugins/markup.c, notification markup: parsing, and the
 * layout against fake fonts (a regular character is 10 px wide, a bold one
 * 12). Ends with a fuzz run over random markup. Built and run by meson
 * test; prints ok/FAIL per check, the exit status is the number of
 * failures. */
#include "plugins/markup.h"

#include <fcft/fcft.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK( cond ) do { \
        if (cond) printf( "ok    %s\n", #cond ); \
        else { printf( "FAIL  %s (line %d)\n", #cond, __LINE__ ); fails++; } \
    } while (0)

/* a fake regular, bold (taller), italic and bold italic font */
static struct fcft_font F[4] = {
    { .height = 10, .ascent = 8 },
    { .height = 14, .ascent = 11 },
    { .height = 10, .ascent = 8 },
    { .height = 14, .ascent = 11 },
};
static struct fcft_font* const fonts[4] = { &F[0], &F[1], &F[2], &F[3] };

/* characters (UTF-8 lead bytes) times 10, 12 in bold */
static unsigned int width( struct fcft_font* font, const char* text ) {
    unsigned int n = 0;
    for (; *text; text++)
        if (((unsigned char)*text & 0xc0) != 0x80) n++;
    return n * (font == &F[1] || font == &F[3] ? 12 : 10);
}

/* m's runs: plain text as it is, a styled run as {flags#color:text}, flags
 * b, i, u */
static const char* runs( const awl_markup_t* m ) {
    static char out[4096];
    size_t n = 0;
    out[0] = '\0';
    for (unsigned int r = 0; r < m->nruns; r++) {
        const awl_markup_run_t* R = &m->runs[r];
        if (R->style)
            n += (size_t)snprintf( out + n, sizeof( out ) - n, "{%s%s%s", R->style & AWL_MARKUP_BOLD ? "b" : "",
                                   R->style & AWL_MARKUP_ITALIC ? "i" : "", R->style & AWL_MARKUP_UNDERLINE ? "u" : "" );
        if (R->style & AWL_MARKUP_COLOR) n += (size_t)snprintf( out + n, sizeof( out ) - n, "#%08x", R->color );
        if (R->style) n += (size_t)snprintf( out + n, sizeof( out ) - n, ":" );
        n += (size_t)snprintf( out + n, sizeof( out ) - n, "%.*s%s", (int)R->len, m->text + R->off, R->style ? "}" : "" );
    }
    return out;
}

/* src parsed with markup, as runs() */
static const char* parse( const char* src ) {
    awl_markup_t m;
    static char out[4096];
    if (!awl_markup_parse( &m, src, 1 )) return "(out of memory)";
    snprintf( out, sizeof( out ), "%s", runs( &m ) );
    awl_markup_free( &m );
    return out;
}

static int is( const char* src, const char* want ) {
    const char* got = parse( src );
    if (strcmp( got, want )) printf( "      %s: got \"%s\", want \"%s\"\n", src, got, want );
    return !strcmp( got, want );
}

static void test_tags( void ) {
    printf( "-- tags\n" );
    CHECK( is( "plain", "plain" ) );
    CHECK( is( "", "" ) );
    CHECK( is( NULL, "" ) );
    CHECK( is( "a <b>bold</b> c", "a {b:bold} c" ) );
    CHECK( is( "<i>x</i><u>y</u>", "{i:x}{u:y}" ) );
    CHECK( is( "<B>up</B>", "{b:up}" ) );
    /* nested, and a close tag closes what is open inside it */
    CHECK( is( "<b>a<i>b</i>c</b>", "{b:a}{bi:b}{b:c}" ) );
    CHECK( is( "<b>a<i>b</b>c", "{b:a}{bi:b}c" ) );
    /* unclosed ones end with the text, stray closes are dropped */
    CHECK( is( "<b>open", "{b:open}" ) );
    CHECK( is( "x</b>y", "xy" ) );
    /* self-closed ones do nothing */
    CHECK( is( "<b/>x", "x" ) );
    /* the same style on both sides of a tag is one run */
    CHECK( is( "<b>a</b><b>b</b>", "{b:ab}" ) );
    CHECK( is( "a<a href=\"https://x\">link</a>b", "alinkb" ) );
    CHECK( is( "a<br>b<br/>c<BR />d", "a\nb\nc\nd" ) );
    CHECK( is( "<img src=\"x.png\" alt=\"[pic &amp; more]\"/> after", "[pic & more] after" ) );
    CHECK( is( "<img src='x.png'>x", "x" ) );
    CHECK( is( "<s>gone</s> <tt>mono</tt> <sub>1</sub>", "gone mono 1" ) );
}

static void test_colors( void ) {
    uint32_t c = 0;
    printf( "-- colors\n" );
    CHECK( awl_markup_color( "#f80", 4, &c ) && c == 0xff8800ff );
    CHECK( awl_markup_color( "#1a2B3c", 7, &c ) && c == 0x1a2b3cff );
    CHECK( awl_markup_color( "#1a2b3c80", 9, &c ) && c == 0x1a2b3c80 );
    CHECK( !awl_markup_color( "#1a2b3", 6, &c ) );
    CHECK( !awl_markup_color( "red", 3, &c ) );
    CHECK( !awl_markup_color( "#ggg", 4, &c ) );
    CHECK( !awl_markup_color( NULL, 0, &c ) );
    CHECK( is( "<t color=\"#ff0000\">red</t>", "{#ff0000ff:red}" ) );
    CHECK( is( "<t color='#f00'>r<b>b</b></t>", "{#ff0000ff:r}{b#ff0000ff:b}" ) );
    CHECK( is( "<span foreground=\"#00ff00\">g</span>", "{#00ff00ff:g}" ) );
    CHECK( is( "<span color=#00f>b</span>", "{#0000ffff:b}" ) );
    /* an invalid color keeps the outer one */
    CHECK( is( "<t color=\"#f00\">a<t color=\"red\">b</t>c</t>", "{#ff0000ff:abc}" ) );
    CHECK( is( "<t color=\"#f00\">a<t color=\"#0f0\">b</t>c</t>", "{#ff0000ff:a}{#00ff00ff:b}{#ff0000ff:c}" ) );
}

static void test_entities( void ) {
    printf( "-- entities\n" );
    CHECK( is( "&lt;b&gt; &amp; &quot;q&quot; &apos;", "<b> & \"q\" '" ) );
    CHECK( is( "&#65;&#x42;&#X43;", "ABC" ) );
    CHECK( is( "&#x1F600;", "\xf0\x9f\x98\x80" ) );
    CHECK( is( "a&nbsp;b", "a\xc2\xa0" "b" ) );
    /* unknown or invalid ones stay */
    CHECK( is( "&foo; &amp &#; &#xZZ; &#0; &#xD800; &#x110000;", "&foo; &amp &#; &#xZZ; &#0; &#xD800; &#x110000;" ) );
    CHECK( is( "AT&T", "AT&T" ) );
    CHECK( is( "<b>&lt;</b>", "{b:<}" ) );
    /* control characters as entities: a tab is a space, others nothing */
    CHECK( is( "a&#9;b&#1;c&#10;d", "a b" "c\nd" ) );
}

static void test_unescaped( void ) {
    printf( "-- unescaped\n" );
    CHECK( is( "1 < 2 and 3 > 2", "1 < 2 and 3 > 2" ) );
    CHECK( is( "a<b2>c", "a<b2>c" ) );
    CHECK( is( "x <y and z> w", "x <y and z> w" ) );
    CHECK( is( "<", "<" ) );
    CHECK( is( "a <b", "a <b" ) );
    CHECK( is( "<b", "<b" ) );
    CHECK( is( "<b x=\"unterminated>bold", "<b x=\"unterminated>bold" ) );
    CHECK( is( "</b x>y", "</b x>y" ) );
    CHECK( is( "<<b>>", "<{b:>}" ) );
    CHECK( is( "<3 <b>love</b>", "<3 {b:love}" ) );
    CHECK( is( "a\tb\rc\x01" "d", "a bcd" ) );
}

static void test_plain( void ) {
    awl_markup_t m;
    printf( "-- plain\n" );
    CHECK( awl_markup_parse( &m, "<b>not &amp; bold</b>\tx", 0 ) );
    CHECK( !strcmp( runs( &m ), "<b>not &amp; bold</b> x" ) );
    awl_markup_free( &m );
}

static void test_limits( void ) {
    awl_markup_t m;
    char* big = malloc( AWL_MARKUP_MAX + 16 );
    char* deep = malloc( 4 * (AWL_MARKUP_DEPTH + 4) + 2 );
    size_t n = 0;

    printf( "-- limits\n" );
    /* cut at AWL_MARKUP_MAX, at a character's start */
    memset( big, 'a', AWL_MARKUP_MAX - 1 );
    memcpy( big + AWL_MARKUP_MAX - 1, "\xc3\xa4tail", 7 );
    CHECK( awl_markup_parse( &m, big, 1 ) && m.len == AWL_MARKUP_MAX - 1 );
    awl_markup_free( &m );
    /* tags past AWL_MARKUP_DEPTH are dropped */
    for (int i = 0; i < AWL_MARKUP_DEPTH + 4; i++, n += 3)
        memcpy( deep + n, "<i>", 3 );
    deep[n++] = 'x';
    deep[n] = '\0';
    CHECK( awl_markup_parse( &m, deep, 1 ) && !strcmp( runs( &m ), "{i:x}" ) );
    awl_markup_free( &m );
    free( big );
    free( deep );
}

/* L's lines, their pieces' text joined by '|' and lines by '/' */
static const char* lines( const awl_markup_layout_t* L ) {
    static char out[4096];
    size_t n = 0;
    out[0] = '\0';
    for (unsigned int l = 0; l < L->nlines; l++) {
        if (l) n += (size_t)snprintf( out + n, sizeof( out ) - n, "/" );
        for (unsigned int p = 0; p < L->lines[l].n; p++)
            n += (size_t)snprintf( out + n, sizeof( out ) - n, "%s%s", p ? "|" : "", L->pieces[L->lines[l].first + p].text );
    }
    return out;
}

static int laid( const char* src, int max_w, int max_lines, const char* want ) {
    awl_markup_t m;
    awl_markup_layout_t L = { 0 };
    int ok;
    awl_markup_parse( &m, src, 1 );
    ok = awl_markup_layout( &L, &m, fonts, width, max_w, max_lines, 2 ) && !strcmp( lines( &L ), want );
    if (!ok) printf( "      %s at %d: got \"%s\", want \"%s\"\n", src, max_w, lines( &L ), want );
    awl_markup_layout_free( &L );
    awl_markup_free( &m );
    return ok;
}

static void test_wrap( void ) {
    printf( "-- wrap\n" );
    CHECK( laid( "one two three", 1000, 0, "one two three" ) );
    CHECK( laid( "one two three", 70, 0, "one two/three" ) );
    CHECK( laid( "one two three", 69, 0, "one/two/three" ) );
    /* spaces at a wrap are dropped, inside a line kept */
    CHECK( laid( "one   two", 50, 0, "one/two" ) );
    CHECK( laid( "a  b", 1000, 0, "a  b" ) );
    /* '\n' breaks, empty lines stay */
    CHECK( laid( "a\nb\n\nc", 1000, 0, "a/b//c" ) );
    /* after '-', but not a leading one */
    CHECK( laid( "well-known word", 60, 0, "well-/known/word" ) );
    CHECK( laid( "run -v now", 60, 0, "run -v/now" ) );
    /* too long: between characters */
    CHECK( laid( "abcdefghij x", 40, 0, "abcd/efgh/ij x" ) );
    CHECK( laid( "\xc3\xa4\xc3\xb6\xc3\xbc\xc3\x9f", 20, 0, "\xc3\xa4\xc3\xb6/\xc3\xbc\xc3\x9f" ) );
    /* at least a character per line */
    CHECK( laid( "abc", 5, 0, "a/b/c" ) );
    /* a word spans runs: a<b>b</b>c is one, 10 + 12 + 10 wide */
    CHECK( laid( "xx a<b>b</b>c", 40, 0, "xx/a|b|c" ) );
    CHECK( laid( "xx a<b>b</b>c", 61, 0, "xx/a|b|c" ) );
    CHECK( laid( "xx a<b>b</b>c", 62, 0, "xx a|b|c" ) );
}

static void test_cut( void ) {
    printf( "-- cut\n" );
    CHECK( laid( "one two three four", 70, 2, "one two/three|\xe2\x80\xa6" ) );
    CHECK( laid( "one two three", 70, 2, "one two/three" ) );
    /* only spaces and newlines after: not cut */
    CHECK( laid( "one\ntwo\n \n", 70, 2, "one/two" ) );
    CHECK( laid( "one\ntwo\nthree", 70, 2, "one/two|\xe2\x80\xa6" ) );
    /* the ellipsis must fit: the line gives way */
    CHECK( laid( "abcdefg hij", 70, 1, "abcde|\xe2\x80\xa6" ) );
    /* the ellipsis measured in the widest font (12, bold) */
    CHECK( laid( "<b>bold</b> rest", 50, 1, "bol|\xe2\x80\xa6" ) );
}

static void test_metrics( void ) {
    awl_markup_t m;
    awl_markup_layout_t L = { 0 };
    printf( "-- metrics\n" );
    awl_markup_parse( &m, "a <b>B</b> <u>u</u>\nplain", 1 );
    CHECK( awl_markup_layout( &L, &m, fonts, width, 1000, 0, 3 ) );
    CHECK( L.nlines == 2 );
    /* the bold font is taller: the line's baseline is its ascent */
    CHECK( L.lines[0].y == 0 && L.lines[0].baseline == 11 && L.lines[0].h == 14 );
    CHECK( L.lines[1].y == 14 + 3 && L.lines[1].baseline == 8 && L.lines[1].h == 10 );
    CHECK( L.h == 14 + 3 + 10 && L.w == 10 + 10 + 12 + 10 + 10 );
    CHECK( L.lines[0].n == 4 );
    CHECK( L.pieces[1].x == 20 && L.pieces[1].w == 12 && (L.pieces[1].style & AWL_MARKUP_BOLD) );
    CHECK( L.pieces[3].x == 42 && (L.pieces[3].style & AWL_MARKUP_UNDERLINE) );
    CHECK( !L.cut );
    /* redone in place */
    CHECK( awl_markup_layout( &L, &m, fonts, width, 30, 1, 3 ) && L.cut && L.nlines == 1 );
    awl_markup_layout_free( &L );
    awl_markup_free( &m );
    /* a NULL font is the regular one */
    struct fcft_font* const some[4] = { &F[0], NULL, NULL, NULL };
    awl_markup_parse( &m, "<b>x</b>", 1 );
    CHECK( awl_markup_layout( &L, &m, some, width, 100, 0, 0 ) && L.w == 10 && L.h == 10 );
    awl_markup_layout_free( &L );
    awl_markup_free( &m );
}

/* random markup: the parse and the layout hold up, and their invariants */
static void test_fuzz( void ) {
    static const char* parts[] = {
        "<b>", "</b>", "<i>", "</i>", "<u>", "</u>", "<t color=\"#f00\">", "</t>", "<t color='", "<br/>", "<img alt=\"",
        "\">", "<", ">", "&", "&amp;", "&#x", ";", "&#", "\"", "'", " ", "  ", "-", "\n", "\t", "a", "word",
        "\xc3\xa4", "\xe2\x80\xa6", "\xff", "\x01", "<span color=#0f0>", "</span>", "</", "/>", "=",
    };
    char src[512];
    int bad = 0, runs_ok = 1, fit_ok = 1, lines_ok = 1;

    printf( "-- fuzz\n" );
    srand( 1 );
    for (int iter = 0; iter < 20000; iter++) {
        size_t n = 0;
        int k = rand() % 40;
        awl_markup_t m;
        awl_markup_layout_t L = { 0 };
        for (int i = 0; i < k; i++) {
            const char* p = parts[rand() % (int)(sizeof( parts ) / sizeof( parts[0] ))];
            if (n + strlen( p ) >= sizeof( src )) break;
            memcpy( src + n, p, strlen( p ) );
            n += strlen( p );
        }
        src[n] = '\0';
        if (!awl_markup_parse( &m, src, rand() % 4 != 0 )) {
            bad++;
            continue;
        }
        /* runs: in order, back to back, none empty, covering the text */
        unsigned int at = 0;
        for (unsigned int r = 0; r < m.nruns; r++) {
            if (m.runs[r].off != at || !m.runs[r].len) runs_ok = 0;
            at += m.runs[r].len;
        }
        if (at != m.len || m.len > n || strlen( m.text ) != m.len) runs_ok = 0;
        for (unsigned int i = 0; i < m.len; i++)
            if ((unsigned char)m.text[i] < 0x20 && m.text[i] != '\n') runs_ok = 0;
        int max_w = 5 + rand() % 120, max_lines = rand() % 4;
        if (!awl_markup_layout( &L, &m, fonts, width, max_w, max_lines, 1 )) {
            bad++;
        } else {
            if (max_lines && L.nlines > (unsigned int)max_lines) lines_ok = 0;
            /* lines fit, but for one character that doesn't (or one plus the ellipsis) */
            for (unsigned int l = 0; l < L.nlines; l++)
                if (L.lines[l].w > max_w && L.lines[l].w > 12 * (L.cut && l + 1 == L.nlines ? 2 : 1)) fit_ok = 0;
        }
        awl_markup_layout_free( &L );
        awl_markup_free( &m );
    }
    CHECK( bad == 0 );
    CHECK( runs_ok );
    CHECK( lines_ok );
    CHECK( fit_ok );
}

int main( void ) {
    test_tags();
    test_colors();
    test_entities();
    test_unescaped();
    test_plain();
    test_limits();
    test_wrap();
    test_cut();
    test_metrics();
    test_fuzz();
    printf( "%d failed\n", fails );
    return fails;
}
