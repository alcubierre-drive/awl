#pragma once

/**
 * Fuzzy matching for the launcher (launcher.h), fzf's way with typos on
 * top. The query's characters are found in order in the text, gaps
 * allowed, scored like fzf: a point for each character, a bonus where one
 * starts a word (after a space, '-', '_', '.', '/', at a camelCase hump)
 * or follows the one before, a small penalty for each gap. Case is ignored.
 *
 * Unlike fzf, a few typos still match, of every kind:
 *
 * - a wrong character ("firwfox"),
 * - one too many ("fiirefox"),
 * - two swapped ("fierfox"),
 * - one left out ("frefox") is just a gap, as in fzf.
 *
 * The first three cost an error each, from a budget by the query's length
 * (`awl_fuzzy_budget()`), and a penalty in the score, so a match without
 * one ranks first. A wrong or extra character only counts inside a run of
 * matched ones: right after the one before (a wrong first character at the
 * start of a word), and right before the next, unless it is the last, in
 * a stretch of the text with at least three characters matched exactly
 * (fewer only if the query has fewer); so errors can't glue unrelated parts
 * of a text together.
 *
 * One query at a time, on one thread: the matcher keeps its tables in
 * static memory.
 */

#include <stdint.h>

enum {
    /** characters of a query that count, the rest is left out */
    AWL_FUZZY_MAX_QUERY = 64,
    /** characters of a text that count, the rest is left out */
    AWL_FUZZY_MAX_TEXT = 256,
    /** errors a query may have at most, however long */
    AWL_FUZZY_MAX_ERRORS = 3,
};

/** a query, folded for matching */
typedef struct awl_fuzzy_query_t {
    uint32_t cp[AWL_FUZZY_MAX_QUERY];
    int n;
    /** errors allowed (`awl_fuzzy_budget()`) */
    int budget;
} awl_fuzzy_query_t;

/** the errors a query of n characters may have: none up to 2, one up to 4,
 * two up to 7, then `AWL_FUZZY_MAX_ERRORS` */
int awl_fuzzy_budget( int n );

/** Folds text (UTF-8) into q, its budget by its length. */
void awl_fuzzy_query( awl_fuzzy_query_t* q, const char* text );

/** Matches q against text (UTF-8). Returns 0 if it doesn't match, else 1
 * with its score in \*score (higher is better; the errors' penalty already
 * in it) and the errors in \*errors (either may be NULL). An empty query
 * matches everything with score 0. pos (NULL: not wanted) gets, for each
 * of q's characters, the byte offset in text of the character it matched,
 * -1 for one that is an error (wrong or extra); a swapped pair matched
 * both. */
int awl_fuzzy_match( const awl_fuzzy_query_t* q, const char* text, int* score, int* errors, int* pos );
