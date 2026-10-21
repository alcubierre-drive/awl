#include "markup.h"

#include <fcft/fcft.h>
#include <stdlib.h>
#include <string.h>

/* ---- parsing ---- */

/** the tags known, with the style they add; `<br>` and `<img>` have no
 * close tag */
static const struct {
    const char* name;
    int style;
} tags[] = {
    { "b", AWL_MARKUP_BOLD },
    { "i", AWL_MARKUP_ITALIC },
    { "u", AWL_MARKUP_UNDERLINE },
    { "t", 0 },
    { "span", 0 },
    { "a", 0 },
    { "s", 0 },
    { "tt", 0 },
    { "sub", 0 },
    { "sup", 0 },
    { "small", 0 },
    { "big", 0 },
    { "br", 0 },
    { "img", 0 },
};
#define NTAGS (sizeof( tags ) / sizeof( tags[0] ))

/** a tag as parsed: its index in tags, the values of the attributes awl
 * reads (NULL if not there) */
typedef struct {
    int tag, close, selfclose;
    const char *color, *alt;
    size_t colorlen, altlen;
} tag_t;

/** an open tag and what is in effect inside it */
typedef struct {
    int tag, style;
    uint32_t color;
} frame_t;

typedef struct {
    awl_markup_t* m;
    unsigned int cap;
    frame_t stack[AWL_MARKUP_DEPTH];
    int depth;
    int ok;
} parser_t;

static int lower( int c ) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int alpha( int c ) {
    return lower( c ) >= 'a' && lower( c ) <= 'z';
}

static int digit( int c ) {
    return c >= '0' && c <= '9';
}

static int space( int c ) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int hexval( int c ) {
    return digit( c ) ? c - '0' : lower( c ) >= 'a' && lower( c ) <= 'f' ? lower( c ) - 'a' + 10 : -1;
}

/** s[0 .. n) is name, case-insensitive */
static int named( const char* s, size_t n, const char* name ) {
    size_t i;
    for (i = 0; i < n && name[i]; i++)
        if (lower( (unsigned char)s[i] ) != name[i]) return 0;
    return i == n && !name[i];
}

int awl_markup_color( const char* s, size_t len, uint32_t* color ) {
    uint32_t c = 0;
    if (!s || len < 1 || s[0] != '#' || (len != 4 && len != 7 && len != 9)) return 0;
    for (size_t i = 1; i < len; i++) {
        int v = hexval( (unsigned char)s[i] );
        if (v < 0) return 0;
        c = c << 4 | (uint32_t)v;
    }
    if (len == 4) /* #rgb: each digit doubled, opaque */
        c = (c >> 8 & 0xf) * 0x11000000u | (c >> 4 & 0xf) * 0x110000u | (c & 0xf) * 0x1100u | 0xff;
    else if (len == 7)
        c = c << 8 | 0xff;
    *color = c;
    return 1;
}

/** cp as UTF-8 into out; its length */
static int utf8encode( uint32_t cp, char* out ) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/** the entity at s[0 .. n) (s[0] is '&'), decoded into out (at most 4
 * bytes, no more than the entity's own length); its length in s, 0 if it
 * is none (then '&' is text) */
static size_t entity( const char* s, size_t n, char* out, int* outlen ) {
    static const struct {
        const char *name, *text;
    } named_ents[] = {
        { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" }, { "nbsp", " " },
    };
    size_t end;
    /* &#x10FFFF; is the longest worth reading */
    for (end = 1; end < n && end < 12 && s[end] != ';'; end++)
        ;
    if (end >= n || s[end] != ';' || end < 2) return 0;
    if (s[1] == '#') {
        int hex = end > 2 && lower( (unsigned char)s[2] ) == 'x';
        uint32_t cp = 0;
        size_t i = hex ? 3 : 2;
        if (i >= end) return 0;
        for (; i < end; i++) {
            int v = hex ? hexval( (unsigned char)s[i] ) : digit( (unsigned char)s[i] ) ? s[i] - '0' : -1;
            if (v < 0 || cp > 0x10ffff) return 0;
            cp = cp * (hex ? 16 : 10) + (uint32_t)v;
        }
        /* no NUL, no surrogates, nothing past Unicode */
        if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        *outlen = utf8encode( cp, out );
        return end + 1;
    }
    for (size_t k = 0; k < sizeof( named_ents ) / sizeof( named_ents[0] ); k++)
        if (end - 1 == strlen( named_ents[k].name ) && !memcmp( s + 1, named_ents[k].name, end - 1 )) {
            *outlen = (int)strlen( named_ents[k].text );
            memcpy( out, named_ents[k].text, (size_t)*outlen );
            return end + 1;
        }
    return 0;
}

/** appends text[0 .. n) to m in style and color */
static void emit( parser_t* P, const char* text, size_t n, int style, uint32_t color ) {
    awl_markup_t* m = P->m;
    awl_markup_run_t* r = m->nruns ? &m->runs[m->nruns - 1] : NULL;

    if (!n || !P->ok) return;
    if (!(style & AWL_MARKUP_COLOR)) color = 0;
    if (!r || r->style != style || r->color != color) {
        if (m->nruns == P->cap) {
            unsigned int cap = P->cap ? 2 * P->cap : 8;
            awl_markup_run_t* runs = realloc( m->runs, cap * sizeof( *runs ) );
            if (!runs) {
                P->ok = 0;
                return;
            }
            m->runs = runs;
            P->cap = cap;
        }
        r = &m->runs[m->nruns++];
        *r = (awl_markup_run_t){ .off = m->len, .style = style, .color = color };
    }
    memcpy( m->text + m->len, text, n );
    m->len += (unsigned int)n;
    r->len += (unsigned int)n;
    m->text[m->len] = '\0';
}

/** appends text[0 .. n), a run of text without markup: tabs become spaces,
 * control characters but newlines are left out; with markup, entities
 * decoded */
static void emit_text( parser_t* P, const char* text, size_t n, int markup, int style, uint32_t color ) {
    size_t i = 0, start = 0;
    char buf[4];
    int len;

    while (i < n) {
        unsigned char c = (unsigned char)text[i];
        size_t k;
        if (c == '\t') {
            emit( P, text + start, i - start, style, color );
            emit( P, " ", 1, style, color );
            start = ++i;
        } else if ((c < 0x20 && c != '\n') || c == 0x7f) {
            emit( P, text + start, i - start, style, color );
            start = ++i;
        } else if (markup && c == '&' && (k = entity( text + i, n - i, buf, &len ))) {
            emit( P, text + start, i - start, style, color );
            /* "&#9;" is a tab, "&#1;" nothing */
            if (len == 1 && buf[0] == '\t') buf[0] = ' ';
            if (len > 1 || ((unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) || buf[0] == '\n') emit( P, buf, (size_t)len, style, color );
            start = i += k;
        } else {
            i++;
        }
    }
    emit( P, text + start, i - start, style, color );
}

/** the tag at s[0 .. n) (s[0] is '<') into *t; its length, 0 if it isn't
 * one of tags, or not well-formed (then '<' is text) */
static size_t tag( const char* s, size_t n, tag_t* t ) {
    size_t i = 1, name;

    *t = (tag_t){ .tag = -1 };
    if (i < n && s[i] == '/') {
        t->close = 1;
        i++;
    }
    for (name = i; i < n && (alpha( (unsigned char)s[i] ) || (i > name && digit( (unsigned char)s[i] ))); i++)
        ;
    for (size_t k = 0; k < NTAGS; k++)
        if (named( s + name, i - name, tags[k].name )) t->tag = (int)k;
    if (t->tag < 0) return 0;
    for (;;) {
        size_t attr, val = 0, vlen = 0, ws = i;
        while (i < n && space( (unsigned char)s[i] ))
            i++;
        if (i >= n) return 0;
        if (s[i] == '>') return i + 1;
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '>' && !t->close) {
            t->selfclose = 1;
            return i + 2;
        }
        /* attributes only after a space, and not in a close tag */
        if (t->close || ws == i) return 0;
        for (attr = i; i < n && (alpha( (unsigned char)s[i] ) || strchr( "-_:", s[i] ) || (i > attr && digit( (unsigned char)s[i] ))); i++)
            ;
        if (i == attr) return 0;
        size_t alen = i - attr;
        while (i < n && space( (unsigned char)s[i] ))
            i++;
        if (i < n && s[i] == '=') {
            i++;
            while (i < n && space( (unsigned char)s[i] ))
                i++;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                for (val = i; i < n && s[i] != q && s[i] != '<'; i++)
                    ;
                if (i >= n || s[i] != q) return 0;
                vlen = i++ - val;
            } else {
                for (val = i; i < n && !space( (unsigned char)s[i] ) && !strchr( "\"'<>=`", s[i] ); i++)
                    ;
                if (i == val) return 0;
                vlen = i - val;
            }
        }
        if (!val) continue;
        if (named( s + attr, alen, "color" ) || named( s + attr, alen, "foreground" )
            || named( s + attr, alen, "fgcolor" )) {
            t->color = s + val;
            t->colorlen = vlen;
        } else if (named( s + attr, alen, "alt" )) {
            t->alt = s + val;
            t->altlen = vlen;
        }
    }
}

/** what is in effect at the top of P's stack */
static void top( const parser_t* P, int* style, uint32_t* color ) {
    *style = P->depth ? P->stack[P->depth - 1].style : 0;
    *color = P->depth ? P->stack[P->depth - 1].color : 0;
}

static void apply( parser_t* P, const tag_t* t ) {
    int style;
    uint32_t color;

    top( P, &style, &color );
    if (t->close) {
        /* up to its open tag; none: dropped */
        for (int d = P->depth - 1; d >= 0; d--)
            if (P->stack[d].tag == t->tag) {
                P->depth = d;
                break;
            }
        return;
    }
    if (!strcmp( tags[t->tag].name, "br" )) {
        emit( P, "\n", 1, style, color );
        return;
    }
    if (!strcmp( tags[t->tag].name, "img" )) {
        if (t->alt) emit_text( P, t->alt, t->altlen, 1, style, color );
        return;
    }
    if (t->selfclose || P->depth == AWL_MARKUP_DEPTH) return;
    style |= tags[t->tag].style;
    if (awl_markup_color( t->color, t->colorlen, &color )) style |= AWL_MARKUP_COLOR;
    P->stack[P->depth++] = (frame_t){ .tag = t->tag, .style = style, .color = color };
}

int awl_markup_parse( awl_markup_t* m, const char* src, int markup ) {
    parser_t P = { .m = m, .ok = 1 };
    size_t n = src ? strlen( src ) : 0, i = 0, start = 0;

    *m = (awl_markup_t){ 0 };
    if (n > AWL_MARKUP_MAX) /* cut at a character's start */
        for (n = AWL_MARKUP_MAX; n && ((unsigned char)src[n] & 0xc0) == 0x80; n--)
            ;
    /* the text never grows: an entity, a tag, an alt text is at least as
     * long as what it becomes */
    if (!(m->text = malloc( n + 1 ))) return 0;
    m->text[0] = '\0';
    while (markup && i < n) {
        tag_t t;
        size_t k;
        int style;
        uint32_t color;
        if (src[i] != '<' || !(k = tag( src + i, n - i, &t ))) {
            i++;
            continue;
        }
        top( &P, &style, &color );
        emit_text( &P, src + start, i - start, 1, style, color );
        apply( &P, &t );
        start = i += k;
    }
    int style;
    uint32_t color;
    top( &P, &style, &color );
    emit_text( &P, src ? src + start : "", n - start, markup, style, color );
    if (!P.ok) {
        awl_markup_free( m );
        return 0;
    }
    return 1;
}

void awl_markup_free( awl_markup_t* m ) {
    free( m->text );
    free( m->runs );
    *m = (awl_markup_t){ 0 };
}

/* ---- layout ---- */

typedef struct {
    awl_markup_layout_t* L;
    const awl_markup_t* m;
    struct fcft_font* const* fonts;
    awl_markup_width_t width;
    /** a copy of a piece, to measure */
    char* scratch;
    /** break positions */
    unsigned int* cand;
    unsigned int cap_pieces, cap_lines;
    size_t buflen;
} layout_t;

struct fcft_font* awl_markup_font( struct fcft_font* const fonts[4], int style ) {
    struct fcft_font* f = fonts[style & AWL_MARKUP_FONT];
    return f ? f : fonts[0];
}

/** the first run that ends after pos */
static unsigned int run_at( const awl_markup_t* m, unsigned int pos ) {
    unsigned int lo = 0, hi = m->nruns;
    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2;
        if (m->runs[mid].off + m->runs[mid].len <= pos) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/** text[s .. e) in each run's font, its width */
static int measure( layout_t* C, unsigned int s, unsigned int e ) {
    const awl_markup_t* m = C->m;
    int w = 0;
    for (unsigned int r = run_at( m, s ); r < m->nruns && m->runs[r].off < e; r++) {
        unsigned int a = s > m->runs[r].off ? s : m->runs[r].off;
        unsigned int b = e < m->runs[r].off + m->runs[r].len ? e : m->runs[r].off + m->runs[r].len;
        if (a >= b) continue;
        memcpy( C->scratch, m->text + a, b - a );
        C->scratch[b - a] = '\0';
        w += (int)C->width( awl_markup_font( C->fonts, m->runs[r].style ), C->scratch );
    }
    return w;
}

/** the last of cand[0 .. k) at which text[s .. cand) fits limit (they fit
 * fewer as they go, about: kerning), galloping then bisecting; -1 if the
 * first doesn't */
static int fit( layout_t* C, unsigned int s, const unsigned int* cand, int k, int limit ) {
    int lo, hi = 1;
    if (!k || measure( C, s, cand[0] ) > limit) return -1;
    lo = 0;
    while (hi < k && measure( C, s, cand[hi] ) <= limit) {
        lo = hi;
        hi *= 2;
    }
    if (hi > k) hi = k;
    /* cand[lo] fits, cand[hi] doesn't (or is past the end) */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (measure( C, s, cand[mid] ) <= limit) lo = mid;
        else hi = mid;
    }
    return lo;
}

/** the end of a line from s in its paragraph, which ends at pe: the last
 * break that fits limit, else the last character that does (at least one) */
static unsigned int line_end( layout_t* C, unsigned int s, unsigned int pe, int limit ) {
    const char* t = C->m->text;
    int k = 0, i;

    /* before a space that follows text, after a '-' inside a word ("-v",
     * "a -b" stay whole), the paragraph's end */
    for (unsigned int e = s + 1; e < pe; e++)
        if ((t[e] == ' ' && t[e - 1] != ' ')
            || (t[e - 1] == '-' && t[e] != ' ' && e >= s + 2 && t[e - 2] != ' '))
            C->cand[k++] = e;
    C->cand[k++] = pe;
    if ((i = fit( C, s, C->cand, k, limit )) >= 0) return C->cand[i];
    /* the first word is too long: between characters */
    unsigned int word = C->cand[0];
    k = 0;
    for (unsigned int e = s + 1; e <= word; e++)
        if (e == word || ((unsigned char)t[e] & 0xc0) != 0x80) C->cand[k++] = e;
    i = fit( C, s, C->cand, k, limit );
    return C->cand[i > 0 ? i : 0];
}

/** a new piece of the current line: text (n bytes), in style and color */
static int piece( layout_t* C, const char* text, size_t n, int x, int style, uint32_t color ) {
    awl_markup_layout_t* L = C->L;
    if (L->npieces == C->cap_pieces) {
        unsigned int cap = C->cap_pieces ? 2 * C->cap_pieces : 16;
        awl_markup_piece_t* p = realloc( L->pieces, cap * sizeof( *p ) );
        if (!p) return -1;
        L->pieces = p;
        C->cap_pieces = cap;
    }
    char* dst = L->buf + C->buflen;
    memcpy( dst, text, n );
    dst[n] = '\0';
    C->buflen += n + 1;
    int w = (int)C->width( awl_markup_font( C->fonts, style ), dst );
    L->pieces[L->npieces++] = (awl_markup_piece_t){ .text = dst, .x = x, .w = w, .style = style, .color = color };
    return w;
}

/** a line of text[s .. e), its text's pieces, then the ellipsis if cut */
static int line( layout_t* C, unsigned int s, unsigned int e, int cut, int y ) {
    awl_markup_layout_t* L = C->L;
    const awl_markup_t* m = C->m;
    int x = 0, w, ascent = 0, descent = 0, style = 0;
    uint32_t color = 0;
    unsigned int first = L->npieces;

    if (L->nlines == C->cap_lines) {
        unsigned int cap = C->cap_lines ? 2 * C->cap_lines : 8;
        awl_markup_line_t* l = realloc( L->lines, cap * sizeof( *l ) );
        if (!l) return -1;
        L->lines = l;
        C->cap_lines = cap;
    }
    for (unsigned int r = run_at( m, s ); r < m->nruns && m->runs[r].off < e; r++) {
        unsigned int a = s > m->runs[r].off ? s : m->runs[r].off;
        unsigned int b = e < m->runs[r].off + m->runs[r].len ? e : m->runs[r].off + m->runs[r].len;
        style = m->runs[r].style;
        color = m->runs[r].color;
        if (a >= b) continue;
        if ((w = piece( C, m->text + a, b - a, x, style, color )) < 0) return -1;
        x += w;
    }
    if (cut) {
        /* in the style of the text it cuts */
        if (L->npieces == first && s < m->len) {
            unsigned int r = run_at( m, s );
            if (r < m->nruns) {
                style = m->runs[r].style;
                color = m->runs[r].color;
            }
        }
        if ((w = piece( C, AWL_MARKUP_ELLIPSIS, strlen( AWL_MARKUP_ELLIPSIS ), x, style, color )) < 0) return -1;
        x += w;
    }
    for (unsigned int p = first; p < L->npieces; p++) {
        struct fcft_font* f = awl_markup_font( C->fonts, L->pieces[p].style );
        if (f->ascent > ascent) ascent = f->ascent;
        if (f->height - f->ascent > descent) descent = f->height - f->ascent;
    }
    if (L->npieces == first) {
        ascent = C->fonts[0]->ascent;
        descent = C->fonts[0]->height - ascent;
    }
    L->lines[L->nlines++] = (awl_markup_line_t){
        .y = y, .h = ascent + descent, .baseline = ascent, .w = x, .first = first, .n = L->npieces - first,
    };
    if (x > L->w) L->w = x;
    return ascent + descent;
}

int awl_markup_layout( awl_markup_layout_t* L, const awl_markup_t* m, struct fcft_font* const fonts[4],
                       awl_markup_width_t width, int max_w, int max_lines, int spacing ) {
    layout_t C = { .L = L, .m = m, .fonts = fonts, .width = width };
    const char* t = m->text;
    unsigned int s = 0, len = m->len;
    int y = 0, h, ok = 0;

    awl_markup_layout_free( L );
    /* the pieces' text: each a part of m's or the ellipsis, NUL-terminated */
    C.scratch = malloc( len + 1 );
    C.cand = malloc( (len + 1) * sizeof( *C.cand ) );
    L->buf = malloc( 2 * (size_t)len + strlen( AWL_MARKUP_ELLIPSIS ) + 2 );
    if (!C.scratch || !C.cand || !L->buf) goto out;
    for (;;) {
        unsigned int pe = s, e, next;
        int last = max_lines > 0 && L->nlines + 1 == (unsigned int)max_lines, cut = 0;

        while (pe < len && t[pe] != '\n')
            pe++;
        e = s < pe ? line_end( &C, s, pe, max_w ) : s;
        next = e;
        while (next < pe && t[next] == ' ')
            next++;
        if (next == pe && pe < len) next++;
        if (last) {
            /* more to come, but no more lines: this one cut to fit an ellipsis */
            for (unsigned int k = next; k < len && !cut; k++)
                cut = t[k] != ' ' && t[k] != '\n';
            if (cut) {
                /* in whichever font it ends up: the widest */
                int ew = 0;
                for (int f = 0; f < 4; f++) {
                    int w = (int)width( awl_markup_font( fonts, f ), AWL_MARKUP_ELLIPSIS );
                    if (w > ew) ew = w;
                }
                e = s < pe ? line_end( &C, s, pe, max_w - ew ) : s;
                if (measure( &C, s, e ) > max_w - ew) e = s;
            }
        }
        /* no trailing spaces */
        while (e > s && t[e - 1] == ' ')
            e--;
        if ((h = line( &C, s, e, cut, y )) < 0) goto out;
        L->h = y + h;
        y += h + spacing;
        if (cut) L->cut = 1;
        if (last || next >= len) break;
        s = next;
    }
    ok = 1;
out:
    free( C.scratch );
    free( C.cand );
    if (!ok) awl_markup_layout_free( L );
    return ok;
}

void awl_markup_layout_free( awl_markup_layout_t* L ) {
    free( L->pieces );
    free( L->lines );
    free( L->buf );
    *L = (awl_markup_layout_t){ 0 };
}
