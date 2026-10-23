#include "colors.h"
#include <math.h>

#define MIN( A, B ) ( (A) < (B) ? (A) : (B) )

typedef struct {
    float r, g, b, a;
} fcolor_t;
typedef union {
    fcolor_t c;
    float f[4];
} fcolor_u;
static fcolor_t pixman2fcolor( pixman_color_t p ) {
    fcolor_u u;
    u.c = (fcolor_t){.r = p.red, .g = p.green, .b = p.blue, .a = p.alpha};
    for (int i=0; i<4; ++i) u.f[i] /= 0xFFFF;
    return u.c;
}
static pixman_color_t fcolor2pixman( fcolor_t c ) {
    pixman_color_t p;
    p.red = MIN(c.r * 0xFFFF, 0xFFFF);
    p.green = MIN(c.g * 0xFFFF, 0xFFFF);
    p.blue = MIN(c.b * 0xFFFF, 0xFFFF);
    p.alpha = MIN(c.a * 0xFFFF, 0xFFFF);
    return p;
}

pixman_color_t alpha_blend_16( pixman_color_t B, pixman_color_t A ) {
    fcolor_u uA, uB, uR = {0};
    uA.c = pixman2fcolor(A);
    uB.c = pixman2fcolor(B);
    uR.c.a = uA.c.a + uB.c.a * (1. - uA.c.a);
    for (int i=0; i<3; ++i)
        uR.f[i] = (uA.f[i] * uA.f[3] + uB.f[i] * uB.f[3] * (1.-uA.f[3])) / uR.f[3];
    return fcolor2pixman(uR.c);
}

pixman_color_t color_mix_16( pixman_color_t A, pixman_color_t B, float t ) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    fcolor_u uA, uB, uR;
    uA.c = pixman2fcolor(A);
    uB.c = pixman2fcolor(B);
    for (int i=0; i<4; ++i)
        uR.f[i] = uA.f[i] + (uB.f[i] - uA.f[i]) * t;
    return fcolor2pixman(uR.c);
}

uint32_t colormap_color( const awl_colormap_t* m, float x ) {
    const size_t n = m->n < AWL_COLORMAP_MAX ? m->n : AWL_COLORMAP_MAX;
    if (!n) return 0;
    if (x <= m->v[0].at) return m->v[0].color;
    for (size_t i=1; i<n; ++i) {
        const awl_colorstop_t* a = &m->v[i-1], * b = &m->v[i];
        if (x >= b->at) continue;
        // x < b->at here, so b->at > a->at: no division by zero (-Ofast)
        float t = (x - a->at) / (b->at - a->at);
        uint32_t c = 0;
        for (int shift=0; shift<32; shift+=8) {
            float ca = (a->color >> shift) & 0xff, cb = (b->color >> shift) & 0xff;
            c |= (uint32_t)lroundf( ca + (cb - ca) * t ) << shift;
        }
        return c;
    }
    return m->v[n-1].color;
}
