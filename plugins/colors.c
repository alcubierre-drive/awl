#include "colors.h"

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

