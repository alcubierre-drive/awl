#include "stats.h"
/*#include "bar.h"*/
/*#include "init.h"*/
/*#include "../awl_log.h"*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

static float cpu_idle( uint64_t* sizes_table, float* io, float* sys );
static int getmem( float* mem, float* swp, uint64_t* mem_total, uint64_t* swp_total );
static void getload( awl_stats_t* st, float* load1 );

awl_stats_t* stats_init( int nval_cpu, int nval_mem, int nval_swp ) {
    /*P_awl_log_printf( "starting system monitor" );*/
    awl_stats_t* st = calloc(1,sizeof(awl_stats_t));
    if (!st) return NULL;
    st->ncpu = nval_cpu < AWL_STATS_HIST ? nval_cpu : AWL_STATS_HIST;
    st->nmem = nval_mem < AWL_STATS_HIST ? nval_mem : AWL_STATS_HIST;
    st->nswp = nval_swp < AWL_STATS_HIST ? nval_swp : AWL_STATS_HIST;
    for (int i=0; i<AWL_STATS_HIST; ++i) {
        atomic_init( &st->cpu[i], 0 );
        atomic_init( &st->mem[i], 0 );
        atomic_init( &st->swp[i], 0 );
        atomic_init( &st->io[i], 0 );
        atomic_init( &st->sys[i], 0 );
        atomic_init( &st->load[i], 0 );
    }
    atomic_init( &st->mem_total, 0 );
    atomic_init( &st->swp_total, 0 );
    atomic_init( &st->load5, 0 );
    atomic_init( &st->load15, 0 );
    atomic_init( &st->running, 0 );
    atomic_init( &st->tasks, 0 );
    atomic_init( &st->samples, 0 );
    atomic_init( &st->has_swap, 1 );
    st->sizes_table = calloc(20, sizeof(uint64_t));
    return st;
}

void stats_free( awl_stats_t* st ) {
    free( st->sizes_table );
    free( st );
}

void stats_update( awl_stats_t* st ) {
    float mem = 0, swp = 0;
    uint64_t mem_total = 0, swp_total = 0;
    int has_swap = getmem( &mem, &swp, &mem_total, &swp_total );
    float io = 0, sys = 0, load1 = stats_sample( st->load, atomic_load( &st->samples ), 0 );
    float cpu = 1. - cpu_idle(st->sizes_table, &io, &sys);
    getload( st, &load1 );
    atomic_store( &st->mem_total, mem_total );
    atomic_store( &st->swp_total, swp_total );
    // the slot after the head is the oldest sample; it becomes the newest
    unsigned next = atomic_load( &st->samples ) + 1, at = next % AWL_STATS_HIST;
    atomic_store( &st->cpu[at], cpu );
    atomic_store( &st->io[at], io );
    atomic_store( &st->sys[at], sys );
    atomic_store( &st->load[at], load1 );
    atomic_store( &st->mem[at], mem );
    atomic_store( &st->swp[at], swp );
    atomic_store( &st->samples, next );
    atomic_store( &st->has_swap, has_swap );
}

/* /proc/loadavg: "0.52 0.61 0.70 3/1234 5678"; left as they were if it
 * can't be read */
static void getload( awl_stats_t* st, float* load1 ) {
    char buf[128];
    int fd = open( "/proc/loadavg", O_RDONLY | O_CLOEXEC );
    if (fd < 0) return;
    ssize_t nread = read( fd, buf, sizeof(buf)-1 );
    close( fd );
    if (nread <= 0) return;
    buf[nread] = '\0';
    float l1, l5, l15;
    int running, tasks;
    if (sscanf( buf, "%f %f %f %d/%d", &l1, &l5, &l15, &running, &tasks ) != 5) return;
    *load1 = l1;
    atomic_store( &st->load5, l5 );
    atomic_store( &st->load15, l15 );
    atomic_store( &st->running, running );
    atomic_store( &st->tasks, tasks );
}

/* the idle fraction since the last call; *io: the part of it spent waiting
 * for I/O, *sys: the busy part spent in the kernel (both left 0 if
 * unknown) */
static float cpu_idle( uint64_t* sizes_table, float* io, float* sys ) {
    // copy old values
    memcpy( sizes_table+10, sizes_table, sizeof(uint64_t)*10 );
    // read only the aggregate "cpu " line (first line of /proc/stat)
    char buf[256];
    int fd = open( "/proc/stat", O_RDONLY | O_CLOEXEC );
    if (fd < 0) return 1.0;
    ssize_t nread = read( fd, buf, sizeof(buf)-1 );
    close( fd );
    if (nread <= 0) return 1.0;
    buf[nread] = '\0';
    uint64_t* s = sizes_table;
    sscanf(buf, "cpu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", s+0, s+1, s+2, s+3, s+4, s+5, s+6, s+7, s+8, s+9);

    // calculate differences: 0=user 1=nice 2=system 3=idle 4=iowait 5=irq
    // 6=softirq 7=steal; 8=guest 9=guest_nice are already part of user and
    // nice, so they stay out of the total. Waiting for I/O counts as idle.
    uint64_t total = 0;
    for (int i=0; i<8; ++i)
        total += s[i] - (s+10)[i];
    uint64_t idle = (s[3] - (s+10)[3]) + (s[4] - (s+10)[4]);
    // no tick since the last read: nothing to tell, call it idle
    if (total == 0) return 1.0;
    *io = (float)(s[4] - (s+10)[4])/(float)total;
    *sys = (float)((s[2] - (s+10)[2]) + (s[5] - (s+10)[5]) + (s[6] - (s+10)[6]))/(float)total;

    // return idle percentage
    float result = (float)idle/(float)total;
    return result > 0.0 ? (result < 1.0 ? result : 1.0) : 0.0;
}

/* returns whether the system has swap */
static int getmem( float* mem, float* swp, uint64_t* mem_total_kib, uint64_t* swp_total_kib ) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) { *mem = *swp = 0.0; return 1; }
    long unsigned mem_total = 0,
                  mem_avail = 0,
                  swp_total = 0,
                  swp_free = 0;
    ssize_t nread = 0;
    size_t len = 0;
    char* line = NULL;
    while ((nread = getline(&line, &len, f)) != -1) {
        long unsigned buf = 0;
        if (mem_total == 0 && sscanf(line, "MemTotal: %lu kB", &buf))
            mem_total = buf;
        else if (mem_avail == 0 && sscanf(line, "MemAvailable: %lu kB", &buf))
            mem_avail = buf;
        else if (swp_total == 0 && sscanf(line, "SwapTotal: %lu kB", &buf))
            swp_total = buf;
        else if (swp_free == 0 && sscanf(line, "SwapFree: %lu kB", &buf))
            swp_free = buf;
        if (mem_total && mem_avail && swp_total && swp_free)
            break;
    }
    free(line);
    fclose(f);
    *mem_total_kib = mem_total;
    *swp_total_kib = swp_total;
    if (mem_total == 0)
        *mem = 1.0;
    else
        *mem = 1.0 - (float)mem_avail/(float)mem_total;
    if (swp_total == 0)
        *swp = 0.0;
    else
        *swp = 1.0 - (float)swp_free/(float)swp_total;

    if (*mem < 0.0) *mem = 0.0;
    if (*mem > 1.0) *mem = 1.0;
    if (*swp < 0.0) *swp = 0.0;
    if (*swp > 1.0) *swp = 1.0;
    return swp_total != 0;
}
