#include "wind.h"

#include "sim_math.h"

#include <math.h>

void Wind_AtAltitude(float altitude, WindSample* out)
{
    /* Several slow sine layers, each with its own phase, so direction and
     * strength shear with height instead of jumping at hard altitude bands. */
    float alt = altitude;
    if (alt < 0.0f) alt = 0.0f;
    float a = alt * 0.01f;

    float dir = 0.55f * sinf(a * 1.65f)
              + 1.25f * sinf(a * 0.38f + 1.15f)
              + 0.35f * sinf(a * 2.90f + 0.40f);
    float speed = 1.6f
                + 4.2f * (0.5f + 0.5f * sinf(a * 0.52f + 0.7f))
                + 2.4f * (0.5f + 0.5f * sinf(a * 1.85f + 2.1f));

    out->bearing = dir;
    out->speed = speed;
    out->x = sinf(dir) * speed;
    out->z = cosf(dir) * speed;
}

const char* Wind_Compass(float bearingRadians)
{
    static const char* names[8] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW"
    };
    float deg = bearingRadians * (180.0f / (float)M_PI);
    while (deg < 0.0f) deg += 360.0f;
    while (deg >= 360.0f) deg -= 360.0f;
    int idx = (int)((deg + 22.5f) / 45.0f) & 7;
    return names[idx];
}
