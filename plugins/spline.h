#pragma once

/**
 * Monotone cubic interpolation (M. Steffen, A&A 239, 443, 1990), with no
 * allocation: piecewise cubic Hermite through the points, each point's
 * slope from its neighbours alone, limited so that between two points the
 * curve never leaves the range of their values (no overshoot, a spike
 * stays a spike of its own height, a flat stretch stays flat). C1.
 *
 * Built from segments: :c:func:`spline_slope` needs only a point and its
 * neighbours, :c:func:`spline_segment` only the two ends of a piece, so a
 * caller can evaluate a curve through data it has anywhere, a ring buffer
 * say, without copying it. :c:func:`spline_slopes` and :c:func:`spline_eval`
 * do the same over arrays, into storage the caller gives. Header only.
 */

#include <math.h>

/** one piece of the curve: y0 + b t + c t² + d t³ at t = x - x0 */
typedef struct spline_segment_t {
    double x0, y0, b, c, d;
} spline_segment_t;

static inline double spline_sign_( double v ) {
    return (v > 0) - (v < 0);
}

/** The slope at the point (x[1], y[1]), its neighbours (x[0], y[0]) and
 * (x[2], y[2]), x ascending; prev and next say whether those exist (at an
 * end of the data, or next to a gap, one doesn't: the slope is then the
 * secant's towards the other, 0 with neither). */
static inline double spline_slope( const double x[3], const double y[3], int prev, int next ) {
    if (prev && next) {
        const double h0 = x[1] - x[0], h1 = x[2] - x[1];
        const double s0 = (y[1] - y[0]) / h0, s1 = (y[2] - y[1]) / h1;
        const double p = (s0 * h1 + s1 * h0) / (h0 + h1);
        return (spline_sign_( s0 ) + spline_sign_( s1 )) * fmin( fmin( fabs( s0 ), fabs( s1 ) ), 0.5 * fabs( p ) );
    }
    if (prev) return (y[1] - y[0]) / (x[1] - x[0]);
    if (next) return (y[2] - y[1]) / (x[2] - x[1]);
    return 0;
}

/** the piece from (x0, y0) to (x1, y1), x0 < x1, with slopes m0 and m1 there */
static inline spline_segment_t spline_segment( double x0, double y0, double m0, double x1, double y1, double m1 ) {
    const double h = x1 - x0, s = (y1 - y0) / h;
    return (spline_segment_t){
        .x0 = x0,
        .y0 = y0,
        .b = m0,
        .c = (3 * s - 2 * m0 - m1) / h,
        .d = (m0 + m1 - 2 * s) / (h * h),
    };
}

static inline double spline_segment_eval( const spline_segment_t* s, double x ) {
    const double t = x - s->x0;
    return s->y0 + t * (s->b + t * (s->c + t * s->d));
}

/** each of the n points' slope into m[0] to m[n - 1] (x strictly ascending) */
static inline void spline_slopes( int n, const double* x, const double* y, double* m ) {
    for (int i = 0; i < n; i++) {
        const int p = i > 0, q = i < n - 1;
        const double xs[3] = { p ? x[i - 1] : 0, x[i], q ? x[i + 1] : 0 };
        const double ys[3] = { p ? y[i - 1] : 0, y[i], q ? y[i + 1] : 0 };
        m[i] = spline_slope( xs, ys, p, q );
    }
}

/** the curve through the n points with slopes m (:c:func:`spline_slopes`)
 * at x; the end values beyond the ends */
static inline double spline_eval( int n, const double* x, const double* y, const double* m, double at ) {
    if (n <= 0) return 0;
    if (n == 1 || at <= x[0]) return y[0];
    if (at >= x[n - 1]) return y[n - 1];
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (at >= x[mid]) lo = mid;
        else hi = mid;
    }
    const spline_segment_t s = spline_segment( x[lo], y[lo], m[lo], x[hi], y[hi], m[hi] );
    return spline_segment_eval( &s, at );
}
