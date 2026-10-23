#pragma once

#include <stdatomic.h>
#include <stdint.h>

/** The most samples a graph keeps. */
#define AWL_STATS_MAX 128

/**
 * The CPU, memory and swap graphs. They are ring buffers, read with
 * :c:func:`stats_sample`: the newest sample of a graph with n samples is at index
 * ``samples % n``. It is stored before :c:member:`~awl_stats_t.samples` moves on, so a reader racing
 * an update can at most see the oldest sample replaced by the newest.
 */
typedef struct awl_stats_t {
    _Atomic float cpu[AWL_STATS_MAX], mem[AWL_STATS_MAX], swp[AWL_STATS_MAX];
    /** waiting for I/O, on top of cpu; ncpu samples */
    _Atomic float io[AWL_STATS_MAX];
    /** number of updates so far */
    atomic_uint samples;
    /** 0 if the system has no swap (SwapTotal 0); then the bar leaves out
     * the swap graph. 1 until the first update. */
    atomic_int has_swap;
    /** ncpu, nmem and nswp: the graphs' numbers of samples; set by
     * :c:func:`stats_init`, then read-only */
    int ncpu, nmem, nswp;

    /** poller thread only */
    uint64_t* sizes_table;
} awl_stats_t;

/** Graphs of the given numbers of samples, at most :c:macro:`AWL_STATS_MAX` each. */
awl_stats_t* stats_init( int nval_cpu, int nval_mem, int nval_swp );
/** Adds a sample to each graph. Always changes what the bar shows. */
void stats_update( awl_stats_t* st );
void stats_free( awl_stats_t* st );

/** Sample i (0: newest) of the graph g with n samples, at head ``samples``
 * (one ``atomic_load()`` of ``st->samples``, shared by all reads of a
 * frame). */
static inline float stats_sample( _Atomic float* g, int n, unsigned samples, int i ) {
    return atomic_load( &g[(samples % n + n - i) % n] );
}
