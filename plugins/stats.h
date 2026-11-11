#pragma once

#include <stdatomic.h>
#include <stdint.h>

/** The samples each history keeps, one per tick of the poller (a second,
 * while the bar is shown): fifteen minutes. The bar's graphs show their
 * newest few, the system monitor popup (plugins/sysmon.h) as many as its
 * width takes at a logical pixel each. */
#define AWL_STATS_HIST 900

/**
 * The CPU, memory and swap histories, and what goes with them. The
 * histories are ring buffers of :c:macro:`AWL_STATS_HIST` samples, read with
 * :c:func:`stats_sample`: the newest is at index
 * ``samples % AWL_STATS_HIST``. It is stored before
 * :c:member:`~awl_stats_t.samples` moves on, so a reader racing an update
 * can at most see the oldest sample replaced by the newest.
 */
typedef struct awl_stats_t {
    /** busy (all but idle and I/O wait), memory and swap in use, 0 to 1 */
    _Atomic float cpu[AWL_STATS_HIST], mem[AWL_STATS_HIST], swp[AWL_STATS_HIST];
    /** waiting for I/O, on top of cpu */
    _Atomic float io[AWL_STATS_HIST];
    /** the part of cpu spent in the kernel (system, irq, softirq) */
    _Atomic float sys[AWL_STATS_HIST];
    /** the 1 min load average */
    _Atomic float load[AWL_STATS_HIST];
    /** number of updates so far */
    atomic_uint samples;
    /** 0 if the system has no swap (SwapTotal 0); then the bar leaves out
     * the swap graph. 1 until the first update. */
    atomic_int has_swap;
    /** the latest update's: memory and swap in total, KiB */
    _Atomic uint64_t mem_total, swp_total;
    /** the 5 and 15 min load averages */
    _Atomic float load5, load15;
    /** runnable tasks and tasks in all (/proc/loadavg) */
    atomic_int running, tasks;
    /** ncpu, nmem and nswp: how many of the newest samples the bar's graphs
     * show; set by :c:func:`stats_init`, then read-only */
    int ncpu, nmem, nswp;

    /** poller thread only */
    uint64_t* sizes_table;
} awl_stats_t;

/** The bar's graphs showing the given numbers of samples, at most
 * :c:macro:`AWL_STATS_HIST` each. */
awl_stats_t* stats_init( int nval_cpu, int nval_mem, int nval_swp );
/** Adds a sample to each history. Always changes what the bar shows. */
void stats_update( awl_stats_t* st );
void stats_free( awl_stats_t* st );

/** Sample i (0: newest) of the history g at head ``samples`` (one
 * ``atomic_load()`` of ``st->samples``, shared by all reads of a frame). */
static inline float stats_sample( _Atomic float* g, unsigned samples, int i ) {
    return atomic_load( &g[(samples % AWL_STATS_HIST + AWL_STATS_HIST - (unsigned)i % AWL_STATS_HIST) % AWL_STATS_HIST] );
}

/** how many samples of the histories are real: fewer than
 * :c:macro:`AWL_STATS_HIST` in the first fifteen minutes */
static inline int stats_filled( unsigned samples ) {
    return samples < AWL_STATS_HIST ? (int)samples : AWL_STATS_HIST;
}
