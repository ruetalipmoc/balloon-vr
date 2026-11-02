#include "terrain.h"

#include "sim_math.h"

#include <math.h>

#define TERRAIN_SEED 0xB4110005u

static float Hash01(int x, int z)
{
    unsigned int h = TERRAIN_SEED;
    h ^= (unsigned int)x * 374761393u;
    h ^= (unsigned int)z * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0x00FFFFFFu) / (float)0x01000000u;
}

static float Fade(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

/* Value noise on the integer lattice. Frequency is applied by the caller. */
static float ValueNoise(float x, float z)
{
    float x0f = floorf(x);
    float z0f = floorf(z);
    int x0 = (int)x0f;
    int z0 = (int)z0f;
    float tx = Fade(x - x0f);
    float tz = Fade(z - z0f);
    float v00 = Hash01(x0, z0);
    float v10 = Hash01(x0 + 1, z0);
    float v01 = Hash01(x0, z0 + 1);
    float v11 = Hash01(x0 + 1, z0 + 1);
    float a = Lerpf(v00, v10, tx);
    float b = Lerpf(v01, v11, tx);
    return Lerpf(a, b, tz);
}

static float Fbm(float x, float z, float freq, float amp, int octaves)
{
    float sum = 0.0f;
    float norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += ValueNoise(x * freq, z * freq) * amp;
        norm += amp;
        freq *= 2.03f;
        amp *= 0.48f;
    }
    if (norm <= 0.0f) return 0.0f;
    return sum / norm;
}

float Terrain_Height(float x, float z)
{
    /* Broad hills plus a ridged octave so horizons read as low-poly mountains
     * even when a distant chunk is only a couple of triangles. */
    float hills = Fbm(x, z, 0.0038f, 1.0f, 5);
    float ridge = 1.0f - fabsf(Fbm(x + 400.0f, z - 250.0f, 0.0022f, 1.0f, 3) * 2.0f - 1.0f);
    float h = hills * 46.0f + ridge * ridge * 28.0f;
    h += (Fbm(x, z, 0.0007f, 1.0f, 2) - 0.5f) * 18.0f;
    if (h < 0.0f) h = 0.0f;
    return h;
}

float Terrain_Moisture(float x, float z)
{
    return Fbm(x + 90.0f, z - 40.0f, 0.0024f, 1.0f, 4);
}

void Terrain_Normal(float x, float z, float* nx, float* ny, float* nz)
{
    const float e = 1.6f;
    float hL = Terrain_Height(x - e, z);
    float hR = Terrain_Height(x + e, z);
    float hD = Terrain_Height(x, z - e);
    float hU = Terrain_Height(x, z + e);
    float vx = hL - hR;
    float vy = 2.0f * e;
    float vz = hD - hU;
    float len = sqrtf(vx * vx + vy * vy + vz * vz);
    if (len < 1.0e-5f) {
        *nx = 0.0f;
        *ny = 1.0f;
        *nz = 0.0f;
        return;
    }
    *nx = vx / len;
    *ny = vy / len;
    *nz = vz / len;
}

float Terrain_GroundAt(float x, float z)
{
    float h = Terrain_Height(x, z);
    const float o = 0.55f;
    h = Maxf(h, Terrain_Height(x + o, z));
    h = Maxf(h, Terrain_Height(x - o, z));
    h = Maxf(h, Terrain_Height(x, z + o));
    h = Maxf(h, Terrain_Height(x, z - o));
    return h;
}
