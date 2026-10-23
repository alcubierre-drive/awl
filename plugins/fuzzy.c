#include "fuzzy.h"

#include <limits.h>
#include <string.h>

/* fzf's scores, in its units: a character is worth MATCH, a gap costs
 * GAP_START for its first character and GAP_EXT for each further one */
enum {
    MATCH = 16,
    GAP_START = -3,
    GAP_EXT = -1,
    /** a character starting a word, or the text */
    BONUS_BOUNDARY = 8,
    /** an upper case letter after a lower case one, a digit after a letter */
    BONUS_CAMEL = 7,
    /** at least this for a character right after the one before */
    BONUS_CONSECUTIVE = 4,
    /** the query's first character counts its bonus this many times */
    FIRST_MULTIPLIER = 2,
    /** an error's penalty, on top of the MATCH it misses */
    ERROR = 12,
};

#define NEG (INT_MIN / 4)

/** how a cell was reached, for the trace back to the positions */
enum {
    B_NONE,
    /* D, L: q[i] matches at j */
    B_START,      /* q[0] */
    B_CONSEC_D,   /* right after q[i - 1]'s (D) */
    B_CONSEC_L,   /* right after q[i - 1]'s (L) */
    B_MENDS,      /* right after an error (O), which this closes */
    B_SWAP_START, /* q[0], q[1] swapped here and before */
    B_SWAP_D,     /* q[i - 1], q[i] swapped, right after q[i - 2]'s at j - 2 (D) */
    B_SWAP_L,     /* the same, after an L */
    B_GAP,        /* (L) after a gap (G) */
    /* O: q[i] is an error at j */
    B_SUB_START,  /* q[0] is wrong here, at the start of a word */
    B_SUB,        /* q[i] is wrong here, right after q[i - 1]'s */
    B_EXTRA,      /* q[i] is one too many, after q[i - 1]'s here */
};

/* Each q[0..i] done, ending at j: D the last one matched in a run (the
 * query's first one, or right after another); L matched after a gap, alone
 * so far; O an error, which only a match right after it closes (no gap, no
 * second error). Errors only follow D: an error has a run on both sides. G:
 * q[0..i] done before j, j a gap. One layer per number of errors. */
#define CELLS ((AWL_FUZZY_MAX_ERRORS + 1) * AWL_FUZZY_MAX_QUERY * AWL_FUZZY_MAX_TEXT)
static int D[CELLS], L[CELLS], O[CELLS], G[CELLS];
static uint8_t BD[CELLS], BL[CELLS], BO[CELLS], BG[CELLS];
#define AT( e, i, j ) (((e) * AWL_FUZZY_MAX_QUERY + (i)) * AWL_FUZZY_MAX_TEXT + (j))

/** which table a cell is in, for the trace */
enum { IN_D, IN_L, IN_O, IN_G };

/** An error has to be in a segment (characters placed without a gap
 * between them) of at least this many exact ones, as many as the query has
 * if fewer: "term" doesn't match "Document Viewer" as "t", a gap, "er" and
 * an extra "m". */
#define SEGMENT_MIN 3

/** Traces the alignment ending at cell e, n - 1, j of table in back: the
 * byte offset each query character matched at into pos (off: the text's
 * characters' offsets), -1 an error. Returns whether every segment with an
 * error in it has enough exact characters (SEGMENT_MIN). */
static int trace( const int* off, int e, int n, int j, int in, int* pos ) {
    /* per query character: an error (wrong or extra, or in a swapped
     * pair), and whether a gap comes before it */
    char err[AWL_FUZZY_MAX_QUERY], gap[AWL_FUZZY_MAX_QUERY];
    int i = n - 1;
    memset( err, 0, sizeof err );
    memset( gap, 0, sizeof gap );
    while (i >= 0) {
        const int at = AT( e, i, j );
        if (in == IN_G) {
            in = BG[at] == 1 ? IN_D : BG[at] == 2 ? IN_L : IN_G;
            j--;
            continue;
        }
        const int how = in == IN_D ? BD[at] : in == IN_L ? BL[at] : BO[at];
        switch (how) {
        case B_START:
            pos[i] = off[j];
            i = -1;
            break;
        case B_CONSEC_D:
        case B_CONSEC_L:
        case B_MENDS:
        case B_GAP:
            in = how == B_CONSEC_D ? IN_D : how == B_CONSEC_L ? IN_L : how == B_MENDS ? IN_O : IN_G;
            gap[i] = how == B_GAP;
            pos[i--] = off[j--];
            break;
        case B_SWAP_START:
        case B_SWAP_D:
        case B_SWAP_L:
            in = how == B_SWAP_L ? IN_L : IN_D;
            if (how != B_SWAP_START) e--;
            pos[i] = off[j - 1];
            pos[i - 1] = off[j];
            err[i] = err[i - 1] = 2;
            i -= 2;
            j -= 2;
            break;
        case B_SUB_START:
            pos[i] = -1;
            err[i] = 1;
            i = -1;
            break;
        case B_SUB:
        case B_EXTRA:
            in = IN_D;
            pos[i] = -1;
            err[i--] = 1;
            e--;
            j -= how == B_SUB;
            break;
        default: /* can't happen */
            while (i >= 0) pos[i--] = -1;
            return 0;
        }
    }
    /* each segment: its exact characters, whether it has an error */
    int exact_all = 0;
    for (int k = 0; k < n; k++) exact_all += err[k] != 1;
    const int need = exact_all < SEGMENT_MIN ? exact_all : SEGMENT_MIN;
    for (int k = 0; k < n;) {
        int exact = 0, bad = 0, l = k;
        do {
            exact += err[l] != 1;
            bad |= err[l];
        } while (++l < n && !gap[l]);
        if (bad && exact < need) return 0;
        k = l;
    }
    return 1;
}

/** the length of the longest common subsequence of a (n) and b (m <=
 * AWL_FUZZY_MAX_TEXT), bit-parallel (Hyyrö): V's zero bits count it */
static int lcs( const uint32_t* a, int n, const uint32_t* b, int m ) {
    enum { W = (AWL_FUZZY_MAX_TEXT + 63) / 64 };
    const int words = (m + 63) / 64;
    uint64_t V[W], M[W], U[W];
    for (int w = 0; w < words; w++) V[w] = ~(uint64_t)0;
    for (int i = 0; i < n; i++) {
        memset( M, 0, sizeof M );
        for (int j = 0; j < m; j++)
            if (b[j] == a[i]) M[j / 64] |= (uint64_t)1 << (j % 64);
        /* V = (V + U) | (V - U), U = V & M, across the words */
        uint64_t carry = 0, borrow = 0;
        for (int w = 0; w < words; w++) {
            U[w] = V[w] & M[w];
            const uint64_t sum = V[w] + U[w] + carry;
            carry = sum < V[w] || (carry && sum == V[w]);
            const uint64_t diff = V[w] - U[w] - borrow;
            borrow = V[w] < U[w] || (borrow && V[w] == U[w]);
            V[w] = sum | diff;
        }
    }
    int zeros = 0;
    for (int j = 0; j < m; j++) zeros += !(V[j / 64] >> (j % 64) & 1);
    return zeros;
}

int awl_fuzzy_budget( int n ) {
    return n <= 2 ? 0 : n <= 4 ? 1 : n <= 7 ? 2 : AWL_FUZZY_MAX_ERRORS;
}

/** the next code point of ``*s``, advancing it; a stray byte stands for itself */
static uint32_t utf8_next( const char** s ) {
    const unsigned char* p = (const unsigned char*)*s;
    uint32_t c = p[0];
    int len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (len <= 1) {
        *s += 1;
        return c;
    }
    c &= 0x7f >> len;
    for (int k = 1; k < len; k++) {
        if ((p[k] & 0xc0) != 0x80) {
            *s += 1;
            return p[0];
        }
        c = c << 6 | (p[k] & 0x3f);
    }
    *s += len;
    return c;
}

/** lower case, for the scripts a desktop's names are mostly in (Latin,
 * Greek, Cyrillic), without the locale */
static uint32_t fold( uint32_t c ) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0xc0) return c;
    if (c <= 0xde && c != 0xd7) return c + 32;
    if (c >= 0x100 && c <= 0x137 && !(c & 1)) return c + 1;
    if (c >= 0x139 && c <= 0x148 && (c & 1)) return c + 1;
    if (c >= 0x14a && c <= 0x177 && !(c & 1)) return c + 1;
    if (c >= 0x179 && c <= 0x17e && (c & 1)) return c + 1;
    if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) return c + 32;
    if (c >= 0x410 && c <= 0x42f) return c + 32;
    if (c >= 0x400 && c <= 0x40f) return c + 80;
    return c;
}

static int is_upper( uint32_t c ) {
    return fold( c ) != c;
}

static int is_lower( uint32_t c ) {
    return (c >= 'a' && c <= 'z') || (c >= 0xdf && c <= 0xff && c != 0xf7);
}

static int is_digit( uint32_t c ) {
    return c >= '0' && c <= '9';
}

static int is_separator( uint32_t c ) {
    return c == ' ' || c == '\t' || c == '-' || c == '_' || c == '.' || c == '/' || c == ':' ||
           c == ',' || c == '(' || c == ')' || c == '[' || c == ']' || c == '+';
}

/** the bonus of a character cur after prev (0: none, at the start) */
static int bonus( uint32_t prev, uint32_t cur ) {
    if (is_separator( cur )) return 0;
    if (!prev || is_separator( prev )) return BONUS_BOUNDARY;
    if ((is_lower( prev ) && is_upper( cur )) || (!is_digit( prev ) && is_digit( cur ))) return BONUS_CAMEL;
    return 0;
}

void awl_fuzzy_query( awl_fuzzy_query_t* q, const char* text ) {
    q->n = 0;
    while (text && *text && q->n < AWL_FUZZY_MAX_QUERY)
        q->cp[q->n++] = fold( utf8_next( &text ) );
    q->budget = awl_fuzzy_budget( q->n );
}

static int max( int a, int b ) {
    return a > b ? a : b;
}

int awl_fuzzy_match( const awl_fuzzy_query_t* q, const char* text, int* score, int* errors, int* pos ) {
    uint32_t c[AWL_FUZZY_MAX_TEXT], raw_prev = 0;
    int off[AWL_FUZZY_MAX_TEXT], b[AWL_FUZZY_MAX_TEXT];
    const int n = q->n;
    int m = 0;

    if (score) *score = 0;
    if (errors) *errors = 0;
    if (!n) return 1;

    for (const char* s = text; s && *s && m < AWL_FUZZY_MAX_TEXT; m++) {
        off[m] = (int)(s - text);
        uint32_t raw = utf8_next( &s );
        b[m] = bonus( raw_prev, raw );
        c[m] = fold( raw );
        raw_prev = raw;
    }

    /* every query character needs a text character, but the extra ones */
    const int K = q->budget;
    if (n > m + K) return 0;
    /* each error costs the longest common subsequence one character at
     * most: what has too short a one can't match, cheaply ruled out */
    if (lcs( q->cp, n, c, m ) < n - K) return 0;

/* v = t + add, how = h, if t is a score and that's better */
#define TRY( v, how, t, add, h ) do { \
        const int t_ = (t); \
        if (t_ > NEG && t_ + (add) > (v)) { \
            (v) = t_ + (add); \
            (how) = (h); \
        } \
    } while (0)
    for (int e = 0; e <= K; e++)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < m; j++) {
                const int at = AT( e, i, j );
                int d = NEG, dhow = B_NONE, l = NEG, lhow = B_NONE, o = NEG, ohow = B_NONE;
                if (c[j] == q->cp[i]) {
                    if (!i) {
                        d = MATCH + FIRST_MULTIPLIER * b[j];
                        dhow = B_START;
                    } else if (j) {
                        const int consec = MATCH + max( b[j], BONUS_CONSECUTIVE );
                        TRY( d, dhow, D[AT( e, i - 1, j - 1 )], consec, B_CONSEC_D );
                        TRY( d, dhow, L[AT( e, i - 1, j - 1 )], consec, B_CONSEC_L );
                        TRY( d, dhow, O[AT( e, i - 1, j - 1 )], consec, B_MENDS );
                        TRY( l, lhow, G[AT( e, i - 1, j - 1 )], MATCH + b[j], B_GAP );
                    }
                }
                if (e && i && j && q->cp[i - 1] != q->cp[i] && q->cp[i - 1] == c[j] && q->cp[i] == c[j - 1]) {
                    if (i == 1) {
                        TRY( d, dhow, 0, 2 * MATCH + FIRST_MULTIPLIER * b[j - 1], B_SWAP_START );
                    } else if (j >= 2) {
                        const int both = 2 * MATCH + max( b[j - 1], BONUS_CONSECUTIVE );
                        TRY( d, dhow, D[AT( e - 1, i - 2, j - 2 )], both, B_SWAP_D );
                        TRY( d, dhow, L[AT( e - 1, i - 2, j - 2 )], both, B_SWAP_L );
                    }
                }
                if (e && c[j] != q->cp[i]) {
                    if (!i && b[j] == BONUS_BOUNDARY) TRY( o, ohow, 0, 0, B_SUB_START );
                    else if (i && j) TRY( o, ohow, D[AT( e - 1, i - 1, j - 1 )], 0, B_SUB );
                }
                if (e && i) TRY( o, ohow, D[AT( e - 1, i - 1, j )], 0, B_EXTRA );
                D[at] = d;
                BD[at] = (uint8_t)dhow;
                L[at] = l;
                BL[at] = (uint8_t)lhow;
                O[at] = o;
                BO[at] = (uint8_t)ohow;

                /* a gap at j, after q[i]'s match (D, L) or another gap (G) */
                int g = NEG, ghow = 0;
                if (j) {
                    TRY( g, ghow, D[AT( e, i, j - 1 )], GAP_START, 1 );
                    TRY( g, ghow, L[AT( e, i, j - 1 )], GAP_START, 2 );
                    TRY( g, ghow, G[AT( e, i, j - 1 )], GAP_EXT, 3 );
                }
                G[at] = g;
                BG[at] = (uint8_t)ghow;
            }

    /* the best end of each number of errors: the last query character's,
     * anywhere, a match or an error; what follows it costs nothing (the
     * shorter text wins ties, the caller's business) */
    int best[AWL_FUZZY_MAX_ERRORS + 1], bj[AWL_FUZZY_MAX_ERRORS + 1], bin[AWL_FUZZY_MAX_ERRORS + 1];
    for (int e = 0; e <= K; e++) {
        best[e] = NEG;
        for (int j = 0; j < m; j++)
            for (int t = IN_D; t <= IN_O; t++) {
                const int v = (t == IN_D ? D : t == IN_L ? L : O)[AT( e, n - 1, j )];
                if (v > NEG && v - e * ERROR > best[e]) {
                    best[e] = v - e * ERROR;
                    bj[e] = j;
                    bin[e] = t;
                }
            }
    }
    /* the best of them whose errors are in long enough segments */
    int tried = 0, p[AWL_FUZZY_MAX_QUERY];
    while (1) {
        int e = -1;
        for (int k = 0; k <= K; k++)
            if (!(tried & 1 << k) && best[k] > NEG && (e < 0 || best[k] > best[e])) e = k;
        if (e < 0) return 0;
        tried |= 1 << e;
        if (!trace( off, e, n, bj[e], bin[e], pos ? pos : p )) continue;
        if (score) *score = best[e];
        if (errors) *errors = e;
        break;
    }
#undef TRY
    return 1;
}
