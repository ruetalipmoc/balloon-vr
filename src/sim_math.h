/* Tiny math helpers shared by the simulation. Kept free of raylib so the
 * physics tests can link without a GPU or an X11 display. */
#ifndef BALLOON_SIM_MATH_H
#define BALLOON_SIM_MATH_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float Clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float Lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static inline float Minf(float a, float b) { return a < b ? a : b; }
static inline float Maxf(float a, float b) { return a > b ? a : b; }

#endif
