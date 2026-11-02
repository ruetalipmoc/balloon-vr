#include "terrain.h"

#include "sim_math.h"

#include <math.h>
#include <string.h>

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

static float Smooth(float t)
{
    t = Clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* 0 below lo, 1 above hi. */
static float Ramp(float v, float lo, float hi)
{
    return Smooth((v - lo) / (hi - lo));
}

typedef struct Land {
    float height;
    float city, farm, forest, highland;
} Land;

/* One evaluation of every mask. Height, zone and colour all share it so a
 * river cannot drift off the blue bed, and a town cannot sit on a ridge. */
static Land LandAt(float x, float z)
{
    float elev = Fbm(x, z, 0.0016f, 1.0f, 3);
    float habit = Fbm(x + 1400.0f, z - 900.0f, 0.00125f, 1.0f, 3);
    float wet = Fbm(x - 500.0f, z + 700.0f, 0.0019f, 1.0f, 3);

    float city = Ramp(habit, 0.57f, 0.66f) * Ramp(elev, 0.14f, 0.26f) * (1.0f - Ramp(elev, 0.68f, 0.80f));
    float farm = Ramp(habit, 0.40f, 0.50f) * (1.0f - Ramp(habit, 0.62f, 0.72f));
    farm *= Ramp(elev, 0.12f, 0.24f) * (1.0f - Ramp(elev, 0.58f, 0.70f)) * (1.0f - city);
    float highland = Ramp(elev, 0.60f, 0.72f) * (1.0f - city);
    float forest = Ramp(wet, 0.54f, 0.64f) * (1.0f - city) * (1.0f - farm) * (1.0f - highland);

    float macro = Fbm(x, z, 0.00115f, 1.0f, 3);
    float detail = Fbm(x + 80.0f, z - 40.0f, 0.0052f, 1.0f, 3);
    float ridgeN = Fbm(x + 520.0f, z - 280.0f, 0.0026f, 1.0f, 3);
    float ridge = 1.0f - fabsf(ridgeN * 2.0f - 1.0f);

    float h = 8.0f + macro * 34.0f + (detail - 0.5f) * 18.0f + ridge * ridge * 20.0f;
    h += highland * (16.0f + ridge * 28.0f);

    /* Towns and fields follow the broad swell but lose the local bumps,
     * so the lots read as planted or paved rather than draped on a hill. */
    float farmPad = 9.0f + macro * 16.0f;
    float cityPad = 12.0f + macro * 8.0f;
    h = Lerpf(h, farmPad, farm * 0.9f);
    h = Lerpf(h, cityPad, city);

    float basin = Fbm(x + 160.0f, z - 240.0f, 0.0024f, 1.0f, 3);
    float lake = Ramp(basin, 0.70f, 0.80f) * (1.0f - city) * (1.0f - highland * 0.5f);
    h = Lerpf(h, TERRAIN_WATER - 2.8f, lake);

    /* A warped contour is a meandering valley. The wide term lowers the
     * banks; the narrow term cuts the bed down to the water. */
    float warpX = Fbm(x, z, 0.0018f, 1.0f, 2) - 0.5f;
    float warpZ = Fbm(x + 260.0f, z - 140.0f, 0.0018f, 1.0f, 2) - 0.5f;
    float rn = Fbm(x + warpX * 220.0f, z + warpZ * 220.0f, 0.0031f, 1.0f, 3);
    float d = fabsf(rn - 0.5f);
    float valley = 1.0f - Smooth(d / 0.075f);
    float channel = 1.0f - Smooth(d / 0.026f);
    float wetMask = (1.0f - city) * (1.0f - highland * 0.75f);
    float shelf = TERRAIN_WATER + 3.2f;
    h = Lerpf(h, Minf(h, shelf), valley * wetMask);
    h = Lerpf(h, TERRAIN_WATER - 1.1f, channel * wetMask);

    if (h < 0.0f) h = 0.0f;

    Land land;
    land.height = h;
    land.city = city;
    land.farm = farm;
    land.forest = forest;
    land.highland = highland;
    return land;
}

float Terrain_Height(float x, float z)
{
    return LandAt(x, z).height;
}

int Terrain_Zone(float x, float z)
{
    Land land = LandAt(x, z);
    float best = 0.0f;
    int zone = TERRAIN_ZONE_PLAINS;
    if (land.highland > best) { best = land.highland; zone = TERRAIN_ZONE_HIGHLAND; }
    if (land.forest > best) { best = land.forest; zone = TERRAIN_ZONE_FOREST; }
    if (land.farm > best) { best = land.farm; zone = TERRAIN_ZONE_FARM; }
    if (land.city > best) zone = TERRAIN_ZONE_CITY;
    return zone;
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
    if (h < TERRAIN_WATER) return TERRAIN_WATER;
    return h;
}

int Terrain_LodForDistance(float distMeters)
{
    if (distMeters < 90.0f) return 0;
    if (distMeters < 180.0f) return 1;
    if (distMeters < 320.0f) return 2;
    return 3;
}

int Terrain_GridForLod(int lod)
{
    switch (lod) {
        case 0: return 16;
        case 1: return 8;
        case 2: return 2;
        default: return 1;
    }
}

static int ChunkLod(int cx, int cz, float viewX, float viewZ)
{
    float mx = ((float)cx + 0.5f) * TERRAIN_CHUNK_SIZE;
    float mz = ((float)cz + 0.5f) * TERRAIN_CHUNK_SIZE;
    return Terrain_LodForDistance(hypotf(mx - viewX, mz - viewZ));
}

/* Lerp this border sample onto the coarser edge running along Z (x fixed)
 * or along X (z fixed). Returns 0 when the sample already is a coarse vertex. */
static int SnapToCoarse(float fixed, float along, float origin, float size, int coarse,
                        int alongX, TerrainSample* s)
{
    float cstep = size / (float)coarse;
    float u = (along - origin) / cstep;
    int i = (int)floorf(u);
    if (i < 0) i = 0;
    if (i >= coarse) i = coarse - 1;
    float f = u - (float)i;
    if (f < 0.0001f || f > 0.9999f) return 0;
    float a = origin + (float)i * cstep;
    float b = a + cstep;
    if (alongX) {
        s->ax = a;
        s->az = fixed;
        s->bx = b;
        s->bz = fixed;
    } else {
        s->ax = fixed;
        s->az = a;
        s->bx = fixed;
        s->bz = b;
    }
    s->t = f;
    s->y = Lerpf(Terrain_Height(s->ax, s->az), Terrain_Height(s->bx, s->bz), f);
    s->snapped = 1;
    return 1;
}

TerrainSample Terrain_Sample(int jx, int jz, int grid, float x0, float z0, float size,
                             int cx, int cz, float viewX, float viewZ)
{
    TerrainSample s;
    memset(&s, 0, sizeof(s));
    float step = size / (float)grid;
    s.x = x0 + (float)jx * step;
    s.z = z0 + (float)jz * step;
    s.y = Terrain_Height(s.x, s.z);

    int onX = (jx == 0 || jx == grid);
    int onZ = (jz == 0 || jz == grid);
    /* Corners belong to every grid, so they already coincide. */
    if (!onX && !onZ) return s;
    if (onX && onZ) return s;

    int neighbor = grid;
    if (jx == 0) neighbor = Terrain_GridForLod(ChunkLod(cx - 1, cz, viewX, viewZ));
    else if (jx == grid) neighbor = Terrain_GridForLod(ChunkLod(cx + 1, cz, viewX, viewZ));
    else if (jz == 0) neighbor = Terrain_GridForLod(ChunkLod(cx, cz - 1, viewX, viewZ));
    else neighbor = Terrain_GridForLod(ChunkLod(cx, cz + 1, viewX, viewZ));
    if (neighbor >= grid) return s;

    if (onX) SnapToCoarse(s.x, s.z, z0, size, neighbor, 0, &s);
    else SnapToCoarse(s.z, s.x, x0, size, neighbor, 1, &s);
    return s;
}
