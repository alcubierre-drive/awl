#include "sysmon.h"
#include "plugins.h"
#include "plugins/colors.h"
#include "plugins/spline.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define N AWL_STATS_HIST
/** the graphs' logical pixels a sample (a second) */
#define PX_PER_SAMPLE 1
#define NSENSORS 16
/** the points a column's area is taken at besides the samples in it:
 * SUB + 1 from edge to edge, where the curve bends between samples */
#define SUB 4
/** all of a column's points: those, and the samples (about one a pixel) */
#define NODES (SUB + 9)

/** what a panel shows: a header (title, the number), its detail lines, its
 * graph */
enum { K_CPU, K_MEM, K_SWP, K_TEMP };

typedef struct {
    int kind;
    /** K_TEMP's sensor */
    int sensor;
    /** its header's top and its graph's, from the content's top */
    int y, gy;
} panel_t;

/** the popup; layout in physical pixels, from the content's top left */
static struct {
    awl_window_t* win;

    /* the snapshot measure() took: the histories by age (0 the newest),
     * the first filled (tfilled) of them real */
    unsigned samples;
    int filled, tfilled;
    float cpu[N], io[N], sys[N], load[N], mem[N], swp[N];
    /** cpu + io, the CPU graph's top */
    float cpuio[N];
    float temp[NSENSORS][N];
    int ntemps;
    float tmin[NSENSORS], tmax[NSENSORS];
    char tlabel[NSENSORS][16];
    uint64_t mem_total, swp_total;
    float load5, load15;
    int running, tasks, has_swap;

    /* measure()'s layout: w is the graphs' width too, one column a pixel */
    float scale;
    int w, min_w, bh, gh, gap, foot_y;
    /** the graphs' width, the popup's; and how many samples fit in it at
     * PX_PER_SAMPLE logical pixels each (at most :c:macro:`AWL_STATS_HIST`),
     * the newest of which the graphs and their numbers show */
    int gw, vis;
    panel_t panel[3 + NSENSORS];
    int npanels;
} S;

static int px( float v ) {
    return (int)lroundf( v * S.scale );
}

static int detail_lines( int kind ) {
    return kind == K_CPU ? 2 : 1;
}

/** the histories into S */
static void snap( void ) {
    awl_plugin_data_t* P = awl_plugin_get();
    S.filled = S.tfilled = 0;
    S.ntemps = 0;
    if (!P) return;
    awl_stats_t* m = P->stats;
    if (m) {
        unsigned samples = S.samples = atomic_load( &m->samples );
        S.filled = stats_filled( samples );
        for (int a = 0; a < S.filled; a++) {
            S.cpu[a] = stats_sample( m->cpu, samples, a );
            S.io[a] = stats_sample( m->io, samples, a );
            S.sys[a] = stats_sample( m->sys, samples, a );
            S.load[a] = stats_sample( m->load, samples, a );
            S.mem[a] = stats_sample( m->mem, samples, a );
            S.swp[a] = stats_sample( m->swp, samples, a );
            S.cpuio[a] = S.cpu[a] + S.io[a];
        }
        S.mem_total = atomic_load( &m->mem_total );
        S.swp_total = atomic_load( &m->swp_total );
        S.load5 = atomic_load( &m->load5 );
        S.load15 = atomic_load( &m->load15 );
        S.running = atomic_load( &m->running );
        S.tasks = atomic_load( &m->tasks );
        S.has_swap = atomic_load( &m->has_swap );
    }
    awl_temperature_t* T = P->temp;
    if (T) {
        unsigned samples = atomic_load( &T->samples );
        S.tfilled = stats_filled( samples );
        S.ntemps = T->f_ntemps < NSENSORS ? T->f_ntemps : NSENSORS;
        for (int i = 0; i < S.ntemps; i++) {
            for (int a = 0; a < S.tfilled; a++) S.temp[i][a] = stats_sample( T->hist[i], samples, a );
            S.tmin[i] = T->f_t_min[i];
            S.tmax[i] = T->f_t_max[i];
            snprintf( S.tlabel[i], sizeof S.tlabel[i], "%s", T->f_labels[i] );
        }
    }
}

/* ---- the numbers ---- */

/* The graphs span the history, the newest at the right: sample age a is
 * at x = S.gw - (a + 0.5) * k, k = PX_PER_SAMPLE logical pixels a
 * sample, in physical ones not a whole number at most scales; the oldest
 * that fit, S.vis of them, are shown. A graph is the curve through every
 * sample, monotone cubic pieces (plugins/spline.h): nothing is averaged
 * away, a spike keeps its height, and the curve never overshoots its
 * samples. A pixel gets exactly the area under the curve that falls in it
 * (column()), so the samples needn't line up with the pixels: no stripes
 * at any scale, and edges antialiased whichever way they slope. */

static double sample_k( void ) {
    return (double)PX_PER_SAMPLE * S.scale;
}

static double age_x( double a ) {
    return (double)S.gw - (a + 0.5) * sample_k();
}

/** the sample at column c's middle: the moment the pointer picks */
static int col_age( int c ) {
    int a = (int)floor( ((double)S.gw - c - 0.5) / sample_k() );
    return a < 0 ? 0 : a >= S.vis ? S.vis - 1 : a;
}

/** h's sample of age a into *v; 0 if there is none (not yet, or
 * unreadable) */
static int sample( const float* h, int filled, int a, double* v ) {
    if (a < 0 || a >= filled || h[a] == AWL_TEMP_NONE) return 0;
    *v = h[a];
    return 1;
}

/** the curve's slope at h's sample of age a, which it has */
static double slope_at( const float* h, int filled, int a ) {
    const double x[3] = { age_x( a + 1 ), age_x( a ), age_x( a - 1 ) };
    double y[3] = { 0 };
    const int prev = sample( h, filled, a + 1, &y[0] );
    sample( h, filled, a, &y[1] );
    const int next = sample( h, filled, a - 1, &y[2] );
    return spline_slope( x, y, prev, next );
}

/** h's curve at x into *v; 0 where it has no samples. For half a sample
 * past its ends, and on either side of a gap, it stays level. */
static int curve_at( const float* h, int filled, double x, double* v ) {
    const double af = ((double)S.gw - x) / sample_k() - 0.5;
    const int r = (int)floor( af ), l = r + 1; /* the samples right and left of x */
    double yr = 0, yl = 0;
    const int hr = sample( h, filled, r, &yr ), hl = sample( h, filled, l, &yl );
    if (hr && hl) {
        const spline_segment_t seg = spline_segment( age_x( l ), yl, slope_at( h, filled, l ), age_x( r ), yr,
                                                     slope_at( h, filled, r ) );
        *v = spline_segment_eval( &seg, x );
        return 1;
    }
    if (hr && af - r <= 0.5) return *v = yr, 1;
    if (hl && l - af <= 0.5) return *v = yl, 1;
    return 0;
}

/** the mean of h's real samples of ages lo to hi (exclusive, clamped to
 * filled) into *v; 0 if there are none */
static int window_mean( const float* h, int filled, int lo, int hi, float* v ) {
    if (lo < 0) lo = 0;
    if (hi > filled) hi = filled;
    double sum = 0;
    int n = 0;
    for (int a = lo; a < hi; a++)
        if (h[a] != AWL_TEMP_NONE) sum += h[a], n++;
    if (!n) return 0;
    *v = (float)(sum / n);
    return 1;
}

/** h at column c (the sample under its middle), all of h's real samples'
 * mean if c is -1, the newest sample if c is -2; into *v, 0 if there are
 * none */
static int mean( const float* h, int filled, int c, float* v ) {
    if (c == -1) return window_mean( h, filled, 0, filled, v );
    if (c == -2) return window_mean( h, filled, 0, 1, v );
    const int a = col_age( c );
    return window_mean( h, filled, a, a + 1, v );
}

/** h's real samples' extremes into *lo, *hi; 0 if there are none */
static int extremes( const float* h, int filled, float* lo, float* hi ) {
    int any = 0;
    for (int a = 0; a < filled; a++) {
        if (h[a] == AWL_TEMP_NONE) continue;
        if (!any || h[a] < *lo) *lo = h[a];
        if (!any || h[a] > *hi) *hi = h[a];
        any = 1;
    }
    return any;
}

/** of KiB: "5.2" with unit "GiB", or MiB below one GiB */
static void size_text( char* buf, size_t n, double used, double total ) {
    if (total >= 1024.0 * 1024.0)
        snprintf( buf, n, "%.1f of %.1f GiB", used / 1048576.0, total / 1048576.0 );
    else
        snprintf( buf, n, "%.0f of %.0f MiB", used / 1024.0, total / 1024.0 );
}

static const char* temp_title( int i ) {
    return *S.tlabel[i] ? S.tlabel[i] : "Temperature";
}

/** panel p's line (0 the header, then the detail lines) at column c (-2
 * now): left and right text; 0 if there is nothing to say */
static int panel_text( const panel_t* p, int line, int c, char* left, char* right, size_t n ) {
    float v = 0, io = 0, sys = 0, lo = 0, hi = 0;
    *left = *right = 0;
    /* before the history begins: the title and a dash */
    const int none = p->kind == K_TEMP ? !mean( S.temp[p->sensor], S.tfilled, c, &v )
                                       : !mean( p->kind == K_CPU ? S.cpu : p->kind == K_MEM ? S.mem : S.swp,
                                                S.filled, c, &v );
    if (none && line > 0) return 0;
    if (none) {
        snprintf( left, n, "%s", p->kind == K_CPU ? "CPU" : p->kind == K_MEM ? "Memory"
                                 : p->kind == K_SWP ? "Swap" : temp_title( p->sensor ) );
        snprintf( right, n, "–" );
        return 1;
    }
    switch (p->kind) {
    case K_CPU:
        mean( S.io, S.filled, c, &io );
        mean( S.sys, S.filled, c, &sys );
        if (line == 0) {
            snprintf( left, n, "CPU" );
            snprintf( right, n, "%.0f%%", v * 100.0f );
        } else if (line == 1) {
            float user = v - sys;
            snprintf( left, n, "user %.0f%% · sys %.0f%% · iowait %.0f%%", (user > 0 ? user : 0) * 100.0f,
                      sys * 100.0f, io * 100.0f );
        } else if (c == -2) {
            snprintf( left, n, "load %.2f %.2f %.2f", S.filled ? S.load[0] : 0.0f, S.load5, S.load15 );
            snprintf( right, n, "%d/%d tasks", S.running, S.tasks );
        } else {
            float l = 0;
            mean( S.load, S.filled, c, &l );
            snprintf( left, n, "load %.2f", l );
        }
        return 1;
    case K_MEM:
    case K_SWP: {
        const int mem = p->kind == K_MEM;
        if (line == 0) {
            snprintf( left, n, "%s", mem ? "Memory" : "Swap" );
            snprintf( right, n, "%.0f%%", v * 100.0f );
        } else {
            double total = (double)(mem ? S.mem_total : S.swp_total);
            size_text( left, n, v * total, total );
        }
        return 1;
    }
    case K_TEMP:
        if (line == 0) {
            snprintf( left, n, "%s", temp_title( p->sensor ) );
            snprintf( right, n, "%.0f°C", v );
        } else if (extremes( S.temp[p->sensor], S.tfilled, &lo, &hi )) {
            mean( S.temp[p->sensor], S.tfilled, -1, &v );
            snprintf( left, n, "min %.0f · mean %.0f · max %.0f°C", lo, v, hi );
        }
        return 1;
    }
    return 0;
}

/** the footer's middle at column c: how long ago */
static void ago_text( char* buf, size_t n, int c ) {
    const int a0 = col_age( c );
    if (a0 == 0) snprintf( buf, n, "now" );
    else snprintf( buf, n, "%d:%02d ago", a0 / 60, a0 % 60 );
}

/* ---- the painter ---- */

/** the parts are the graphs' columns, the pointer picking a moment */
static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    (void)data;
    if (!font) return 0;
    S.scale = scale;
    snap();
    S.bh = font->height + px( 2 );
    S.gh = px( 44 );
    S.gap = px( 10 );

    /* the panels: CPU and memory, swap if there is any, each sensor that
     * has read anything */
    S.npanels = 0;
    S.panel[S.npanels++] = (panel_t){ .kind = K_CPU };
    S.panel[S.npanels++] = (panel_t){ .kind = K_MEM };
    if (S.has_swap && S.swp_total) S.panel[S.npanels++] = (panel_t){ .kind = K_SWP };
    for (int i = 0; i < S.ntemps; i++) {
        float lo, hi;
        if (extremes( S.temp[i], S.tfilled, &lo, &hi ))
            S.panel[S.npanels++] = (panel_t){ .kind = K_TEMP, .sensor = i };
    }

    /* as wide as the widest line now, at least five minutes of graph;
     * never narrower while shown, so it doesn't jitter. The graphs fill
     * it, showing as much of the history as that takes. */
    char left[128], right[128];
    const int sep = (int)awl_host->text_width( font, "    " );
    int W = px( PX_PER_SAMPLE * 300 );
    for (int p = 0; p < S.npanels; p++)
        for (int l = 0; l <= detail_lines( S.panel[p].kind ); l++) {
            panel_text( &S.panel[p], l, -2, left, right, sizeof left );
            int lw = (int)awl_host->text_width( font, left ), rw = (int)awl_host->text_width( font, right );
            if (lw + (rw ? sep + rw : 0) > W) W = lw + (rw ? sep + rw : 0);
        }
    if (W < S.min_w) W = S.min_w;
    S.min_w = W;
    if (W > max_w) W = max_w;
    S.w = S.gw = W;
    S.vis = (int)ceil( W / sample_k() );
    if (S.vis > N) S.vis = N;
    if (S.vis < 1) S.vis = 1;
    /* what isn't shown doesn't count, the temperatures' extremes neither */
    if (S.filled > S.vis) S.filled = S.vis;
    if (S.tfilled > S.vis) S.tfilled = S.vis;

    /* top to bottom; sensors that don't fit are left out */
    int y = 0, kept = 0;
    for (int p = 0; p < S.npanels; p++) {
        const int ph = (1 + detail_lines( S.panel[p].kind )) * S.bh + px( 3 ) + S.gh;
        const int next = y + (p ? S.gap : 0);
        if (next + ph + S.bh > max_h && p > 0) break;
        S.panel[p].y = next;
        S.panel[p].gy = next + (1 + detail_lines( S.panel[p].kind )) * S.bh + px( 3 );
        y = S.panel[p].gy + S.gh;
        kept = p + 1;
    }
    S.npanels = kept;
    S.foot_y = y + px( 2 );
    *w = W;
    *h = S.foot_y + S.bh;
    return W > 0;
}

/** text in the box x, y, w, S.bh (offset by x0, y0): align 0 left, 1
 * centered, 2 right; cut with an ellipsis if wider */
static void text_at( pixman_image_t* pix, struct fcft_font* font, int x0, int y0, int x, int y, int w, const char* s,
                     uint32_t fg, int align ) {
    if (!*s || w <= 0) return;
    int tw = (int)awl_host->text_width( font, s );
    if (tw < w) x += align == 1 ? (w - tw) / 2 : align == 2 ? w - tw : 0;
    awl_host->text_over( pix, font, x0 + x, y0 + y, (unsigned)(tw < w ? tw + 1 : w), (unsigned)S.bh, s, fg );
}

/** the secondary text's color and the graphs' boxes': halfway from the
 * popups' dim color to their text's */
static uint32_t secondary( void ) {
    const awl_colors_t* k = &awl_config()->colors;
    return color_mix_8( k->popup_dim, k->popup_fg, 0.5f );
}

static uint32_t with_alpha( uint32_t c, unsigned a ) {
    return (c & 0xffffff00u) | (((c & 0xff) * a / 255) & 0xff);
}

/** the antiderivative of a row's coverage at height t: the row from b0
 * to b0 + 1 (pixels from the graph's bottom) covered up to t */
static double row_antideriv( double t, double b0 ) {
    t -= b0;
    return t <= 0 ? 0 : t >= 1 ? t - 0.5 : t * t / 2;
}

/** the area of that row under a straight piece from height fa to fb, w wide */
static double row_area( double fa, double fb, double w, double b0 ) {
    if (fabs( fb - fa ) < 1e-9) {
        const double g = fa - b0;
        return w * (g <= 0 ? 0 : g >= 1 ? 1 : g);
    }
    return w * (row_antideriv( fb, b0 ) - row_antideriv( fa, b0 )) / (fb - fa);
}

/** column c's points: its edges, SUB - 1 between, and its samples, by x */
static int column_nodes( int c, double* xs ) {
    int n = 0;
    for (int j = 0; j <= SUB; j++) xs[n++] = c + (double)j / SUB;
    const double k = sample_k();
    for (int a = (int)ceil( ((double)S.gw - c - 1) / k - 0.5 ); a <= (int)floor( ((double)S.gw - c) / k - 0.5 ) &&
                                                                n < NODES;
         a++) {
        const double x = age_x( a );
        if (x <= c || x >= c + 1) continue;
        int i = n++;
        while (i > 0 && xs[i - 1] > x) xs[i] = xs[i - 1], i--;
        xs[i] = x;
    }
    return n;
}

/** Column x of a graph whose top is gy: n stacked segments from the
 * bottom, at the nn points xs segment i up to top[j][i] (pixels from the
 * bottom, ascending), straight between them, in color c[i]
 * (0xRRGGBBAA). Each pixel gets each segment by the area of it that it
 * covers, in one write: segments drawn one over the other would let the
 * background through where they meet. */
static void column( pixman_image_t* pix, int x, int gy, int nn, const double* xs, double top[][3], int n,
                    const uint32_t* c ) {
    uint32_t* data = pixman_image_get_data( pix );
    const int stride = pixman_image_get_stride( pix ) / 4;
    const int iw = pixman_image_get_width( pix ), ih = pixman_image_get_height( pix );
    if (!data || x < 0 || x >= iw || n <= 0 || nn < 2) return;
    const double g = S.gh;
    double hi = 0;
    for (int j = 0; j < nn; j++) {
        double below = 0;
        for (int i = 0; i < n; i++) {
            double t = top[j][i] < below ? below : top[j][i] > g ? g : top[j][i];
            top[j][i] = below = t;
        }
        if (below > hi) hi = below;
    }
    double col[3][4];
    for (int i = 0; i < n; i++) {
        const double a = (double)(c[i] & 0xff) / 255.;
        col[i][0] = (double)(c[i] >> 24) / 255. * a;
        col[i][1] = (double)((c[i] >> 16) & 0xff) / 255. * a;
        col[i][2] = (double)((c[i] >> 8) & 0xff) / 255. * a;
        col[i][3] = a;
    }
    const double width = xs[nn - 1] - xs[0];
    for (int r = (int)floor( g - hi ); r < S.gh; r++) {
        const int y = gy + r;
        if (y < 0 || y >= ih) continue;
        const double b0 = g - r - 1;
        double acc[4] = { 0 }, under = 0;
        for (int i = 0; i < n; i++) {
            double area = 0;
            for (int j = 0; j + 1 < nn; j++) area += row_area( top[j][i], top[j + 1][i], xs[j + 1] - xs[j], b0 );
            area /= width;
            const double own = area - under;
            under = area;
            if (own <= 0) continue;
            for (int ch = 0; ch < 4; ch++) acc[ch] += col[i][ch] * own;
        }
        if (acc[3] <= 0) continue;
        uint32_t* d = &data[y * stride + x];
        const int shift[4] = { 16, 8, 0, 24 };
        uint32_t out = 0;
        for (int i = 0; i < 4; i++) {
            double v = acc[i] * 255 + (double)((*d >> shift[i]) & 0xff) * (1 - acc[3]);
            out |= (uint32_t)lround( fmin( fmax( v, 0 ), 255 ) ) << shift[i];
        }
        *d = out;
    }
}

static void graph( pixman_image_t* pix, int x0, int y0, const panel_t* p, int hover ) {
    const awl_colors_t* k = &awl_config()->colors;
    const awl_plugin_config_t* pc = awl_plugin_config();
    const int gy = y0 + p->gy;
    /* on the popup's own background (no box of the bar's stats_bg); the
     * kernel's part the CPU's color darkened toward it, opaque */
    const uint32_t kernel = color_mix_8( pc->colors.cpu, k->popup_bg | 0xff, 0.45f );
    const double g = S.gh;
    for (int c = 0; c < S.gw; c++) {
        double xs[NODES], top[NODES][3] = { { 0 } };
        const int nn = column_nodes( c, xs );
        int any = 0;
        for (int j = 0; j < nn; j++) {
            double v = 0, t = 0;
            switch (p->kind) {
            case K_CPU:
                if (!curve_at( S.cpu, S.filled, xs[j], &v )) continue;
                curve_at( S.sys, S.filled, xs[j], &t );
                top[j][0] = t * g;
                top[j][1] = v * g;
                t = v;
                curve_at( S.cpuio, S.filled, xs[j], &t );
                top[j][2] = t * g;
                any = 1;
                break;
            case K_MEM:
            case K_SWP:
                if (!curve_at( p->kind == K_MEM ? S.mem : S.swp, S.filled, xs[j], &v )) continue;
                top[j][0] = v * g;
                any = 1;
                break;
            case K_TEMP: {
                const int i = p->sensor;
                if (!curve_at( S.temp[i], S.tfilled, xs[j], &v )) continue;
                const double f = S.tmax[i] > S.tmin[i] ? (v - S.tmin[i]) / (S.tmax[i] - S.tmin[i]) : 0.5;
                /* a sliver at the range's bottom, to see there was a reading */
                top[j][0] = (f > 0.02 ? f : 0.02) * g;
                any = 1;
                break;
            }
            }
        }
        if (!any) continue;
        const int x = x0 + c;
        switch (p->kind) {
        case K_CPU:
            column( pix, x, gy, nn, xs, top, 3, (const uint32_t[]){ kernel, pc->colors.cpu, pc->colors.io } );
            break;
        case K_MEM:
        case K_SWP:
            column( pix, x, gy, nn, xs, top, 1, p->kind == K_MEM ? &pc->colors.mem : &pc->colors.swap );
            break;
        case K_TEMP: {
            /* the column in the color of its middle's value */
            const int i = p->sensor;
            double v = 0;
            curve_at( S.temp[i], S.tfilled, c + 0.5, &v );
            const float f = S.tmax[i] > S.tmin[i] ? (float)((v - S.tmin[i]) / (S.tmax[i] - S.tmin[i])) : 0.5f;
            column( pix, x, gy, nn, xs, top, 1, (const uint32_t[]){ colormap_color( &pc->colors.temperature, f ) } );
            break;
        }
        }
    }
    /* the quarters */
    /* lines a whole number of pixels thick, on whole pixels */
    const int line = (int)S.scale > 1 ? (int)S.scale : 1;
    const uint32_t grid = with_alpha( k->popup_dim, 0x50 );
    for (int q = 1; q < 4; q++)
        awl_fill_shape( pix, (float)x0, (float)(gy + S.gh * q / 4), (float)S.gw, (float)line, 0, 0, grid );
    /* the box around it */
    awl_fill_shape( pix, (float)x0, (float)gy, (float)S.gw, (float)S.gh, 0, (float)line, secondary() );
    if (hover >= 0 && hover < S.gw) {
        const int cx = hover + line > S.gw ? S.gw - line : hover;
        awl_fill_shape( pix, (float)(x0 + cx), (float)gy, (float)line, (float)S.gh, 0, 0, k->popup_fg );
    }
}

static uint32_t title_color( const panel_t* p, int c ) {
    const awl_plugin_config_t* pc = awl_plugin_config();
    float v;
    switch (p->kind) {
    case K_CPU: return pc->colors.cpu;
    case K_MEM: return pc->colors.mem;
    case K_SWP: return pc->colors.swap;
    default:
        if (!mean( S.temp[p->sensor], S.tfilled, c, &v )) return awl_config()->colors.popup_fg;
        return colormap_color( &pc->colors.temperature, S.tmax[p->sensor] > S.tmin[p->sensor]
                                   ? (v - S.tmin[p->sensor]) / (S.tmax[p->sensor] - S.tmin[p->sensor]) : 0.5f );
    }
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover ) {
    (void)data; (void)scale;
    const awl_colors_t* k = &awl_config()->colors;
    char left[128], right[128];
    const uint32_t dim = secondary();
    if (hover >= S.gw) hover = -1;
    const int c = hover >= 0 ? hover : -2;
    for (int p = 0; p < S.npanels; p++) {
        const panel_t* pn = &S.panel[p];
        for (int l = 0; l <= detail_lines( pn->kind ); l++) {
            if (!panel_text( pn, l, c, left, right, sizeof left )) continue;
            const int y = pn->y + l * S.bh;
            /* the right part only where it fits beside the left one; the
             * header's number always, the title cut to fit then */
            const int lw = (int)awl_host->text_width( font, left ), gap = px( 12 );
            int rw = *right ? (int)awl_host->text_width( font, right ) : 0;
            if (l && rw && lw + gap + rw > S.w) rw = 0;
            text_at( pix, font, x0, y0, 0, y, S.w - (rw ? rw + gap : 0), left, l ? dim : title_color( pn, -2 ),
                     0 );
            if (rw) text_at( pix, font, x0, y0, 0, y, S.w, right, l ? dim : k->popup_fg, 2 );
        }
        graph( pix, x0, y0, pn, hover );
    }
    /* the time axis */
    char buf[32];
    /* the graphs' left edge: whole minutes, rounded down */
    int span = (int)floor( S.gw / sample_k() + 0.5 );
    if (span > N) span = N;
    if (span >= 60) snprintf( buf, sizeof buf, "%d min ago", span / 60 );
    else snprintf( buf, sizeof buf, "%d s ago", span );
    text_at( pix, font, x0, y0, 0, S.foot_y, S.gw, buf, dim, 0 );
    if (hover >= 0) {
        ago_text( buf, sizeof buf, hover );
        text_at( pix, font, x0, y0, 0, S.foot_y, S.gw, buf, k->popup_fg, 1 );
    }
    text_at( pix, font, x0, y0, 0, S.foot_y, S.gw, "now", dim, 2 );
}

static int hit( void* data, int x, int y ) {
    (void)data;
    if (x < 0 || x >= S.gw || y < 0 || y >= S.foot_y + S.bh) return -1;
    return x;
}

static int box( void* data, int p, int* x, int* y, int* w, int* h ) {
    (void)data;
    if (p < 0 || p >= S.gw || !S.npanels) return 0;
    *x = p;
    *y = S.panel[0].gy;
    *w = 1;
    *h = S.gh;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/** nothing to click: it is there for the pointer, whose column the
 * painter gets */
static void click( awl_window_t* win, int part, int col, uint32_t button, void* data ) {
    (void)win; (void)part; (void)col; (void)button; (void)data;
}

/** the window, made on first use; NULL before awl is set up */
static awl_window_t* window( void ) {
    if (S.win) return S.win;
    if (!(S.win = awl_host->window_create( awl_host->srv ))) return NULL;
    awl_host->window_set_painter( awl_host->srv, S.win, &painter, NULL );
    awl_host->window_set_click( awl_host->srv, S.win, click, NULL );
    return S.win;
}

void awl_sysmon_show( widget_t* w, int center_x ) {
    awl_window_t* win = window();
    if (!win) return;
    w->popup_window = win;
    if (!awl_host->window_monitor( win )) S.min_w = 0;
    awl_host->window_redraw( awl_host->srv, win );
    awl_host->window_show( awl_host->srv, win, w->bar->m, center_x );
}

void awl_sysmon_hide( void ) {
    if (S.win) awl_host->window_hide( awl_host->srv, S.win );
}

void awl_sysmon_poll( void ) {
    awl_plugin_data_t* P = awl_plugin_get();
    if (!S.win || !awl_host->window_monitor( S.win ) || !P || !P->stats) return;
    if (atomic_load( &P->stats->samples ) != S.samples) awl_host->window_redraw( awl_host->srv, S.win );
}

void awl_sysmon_stop( void ) {
    if (S.win) awl_host->window_destroy( awl_host->srv, S.win );
    memset( &S, 0, sizeof S );
}
