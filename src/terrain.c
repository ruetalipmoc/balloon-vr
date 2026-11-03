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

/* Zone weights only. The cover map samples this, so it must not also cut
 * rivers and ridges. */
static void MasksAt(float x, float z, float* city, float* farm, float* forest, float* highland)
{
    float elev = Fbm(x, z, 0.0016f, 1.0f, 3);
    float habit = Fbm(x + 1400.0f, z - 900.0f, 0.00125f, 1.0f, 3);
    float wet = Fbm(x - 500.0f, z + 700.0f, 0.0019f, 1.0f, 3);
    float c = Ramp(habit, 0.57f, 0.66f) * Ramp(elev, 0.14f, 0.26f) * (1.0f - Ramp(elev, 0.68f, 0.80f));
    float fa = Ramp(habit, 0.40f, 0.50f) * (1.0f - Ramp(habit, 0.62f, 0.72f));
    fa *= Ramp(elev, 0.12f, 0.24f) * (1.0f - Ramp(elev, 0.58f, 0.70f)) * (1.0f - c);
    float hi = Ramp(elev, 0.60f, 0.72f) * (1.0f - c);
    float fo = Ramp(wet, 0.54f, 0.64f) * (1.0f - c) * (1.0f - fa) * (1.0f - hi);
    *city = c;
    *farm = fa;
    *forest = fo;
    *highland = hi;
}

/* Drainage on a fixed grid. A vertex belongs to one tile, and its carved
 * height is a function of the base relief only, so the tile that stores it
 * and the tile next door agree on the shared edge. Rivers are the cells
 * that collect the most uphill rain. A hollow with nowhere to go is a lake. */
#define DRAIN_CELL 64.0f
#define DRAIN_N 4
#define DRAIN_MARGIN 12
#define DRAIN_WIN (DRAIN_N + 2 * DRAIN_MARGIN)
#define DRAIN_CACHE 4096

typedef struct DrainTile {
    int tx, tz;
    int live;
    float h[DRAIN_N * DRAIN_N];
    unsigned char wet[DRAIN_N * DRAIN_N];
} DrainTile;

static DrainTile g_drain[DRAIN_CACHE];
static float g_focusX = 0.0f;
static float g_focusZ = 0.0f;

static int DivFloor(int a, int b)
{
    int q = a / b;
    int r = a % b;
    if (r != 0 && ((a < 0 && b > 0) || (a > 0 && b < 0))) q--;
    return q;
}

static float BaseRelief(int ix, int iz)
{
    float x = (float)ix * DRAIN_CELL;
    float z = (float)iz * DRAIN_CELL;
    float macro = Fbm(x, z, 0.00115f, 1.0f, 3);
    float detail = Fbm(x + 80.0f, z - 40.0f, 0.0048f, 1.0f, 2);
    float ridgeN = Fbm(x + 520.0f, z - 280.0f, 0.0024f, 1.0f, 2);
    float ridge = 1.0f - fabsf(ridgeN * 2.0f - 1.0f);
    return 9.0f + macro * 38.0f + (detail - 0.5f) * 12.0f + ridge * ridge * 16.0f;
}

static void GenerateTile(DrainTile* tile, int tx, int tz)
{
    const int win = DRAIN_WIN;
    const int orgX = tx * DRAIN_N - DRAIN_MARGIN;
    const int orgZ = tz * DRAIN_N - DRAIN_MARGIN;
    static float base[DRAIN_WIN * DRAIN_WIN];
    static float acc[DRAIN_WIN * DRAIN_WIN];
    static float nxt[DRAIN_WIN * DRAIN_WIN];
    static signed char down[DRAIN_WIN * DRAIN_WIN];
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    static const int DZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

    for (int j = 0; j < win; j++) {
        for (int i = 0; i < win; i++)
            base[j * win + i] = BaseRelief(orgX + i, orgZ + j);
    }
    for (int j = 0; j < win; j++) {
        for (int i = 0; i < win; i++) {
            int id = j * win + i;
            float h = base[id];
            int best = -1;
            float bestH = h - 0.05f;
            for (int k = 0; k < 8; k++) {
                int ni = i + DX[k];
                int nj = j + DZ[k];
                if (ni < 0 || nj < 0 || ni >= win || nj >= win) continue;
                float hn = base[nj * win + ni];
                if (hn < bestH) {
                    bestH = hn;
                    best = k;
                }
            }
            down[id] = (signed char)best;
            acc[id] = 1.0f;
        }
    }
    for (int step = 0; step < DRAIN_MARGIN; step++) {
        for (int i = 0; i < win * win; i++) nxt[i] = 1.0f;
        for (int j = 0; j < win; j++) {
            for (int i = 0; i < win; i++) {
                int id = j * win + i;
                int dir = down[id];
                if (dir < 0) continue;
                int ni = i + DX[dir];
                int nj = j + DZ[dir];
                if (ni < 0 || nj < 0 || ni >= win || nj >= win) continue;
                nxt[nj * win + ni] += acc[id];
            }
        }
        memcpy(acc, nxt, sizeof(acc));
    }

    tile->tx = tx;
    tile->tz = tz;
    tile->live = 1;
    for (int j = 0; j < DRAIN_N; j++) {
        for (int i = 0; i < DRAIN_N; i++) {
            int id = (j + DRAIN_MARGIN) * win + (i + DRAIN_MARGIN);
            float b = base[id];
            float a = acc[id];
            int dir = down[id];
            float nab = a;
            for (int k = 0; k < 8; k++) {
                int ni = i + DRAIN_MARGIN + DX[k];
                int nj = j + DRAIN_MARGIN + DZ[k];
                if (ni < 0 || nj < 0 || ni >= win || nj >= win) continue;
                float an = acc[nj * win + ni];
                if (an > nab) nab = an;
            }
            float x = (float)(tx * DRAIN_N + i) * DRAIN_CELL;
            float z = (float)(tz * DRAIN_N + j) * DRAIN_CELL;
            float city, farm, forest, highland;
            MasksAt(x, z, &city, &farm, &forest, &highland);
            float h = b;
            float wet = 0.0f;
            int lake = (dir < 0 && b < 34.0f && highland < 0.6f && city < 0.5f && a > 2.0f);
            int channel = (a > 11.0f || (nab > 18.0f && b < 28.0f));
            if (lake || (channel && city < 0.5f && highland < 0.72f)) {
                h = TERRAIN_WATER - (lake ? 1.8f : 0.7f);
                wet = 1.0f;
            } else if (a > 6.0f && city < 0.55f) {
                float steps = logf(a) / 0.693147f;
                float depth = Clampf((steps - 2.2f) * 1.3f, 0.0f, 4.5f);
                h = b - depth;
                wet = Clampf((a - 6.0f) / 12.0f, 0.0f, 0.65f);
            }
            if (city > 0.55f) h = Lerpf(h, Maxf(h, 12.0f), city);
            if (h < 0.0f) h = 0.0f;
            tile->h[j * DRAIN_N + i] = h;
            tile->wet[j * DRAIN_N + i] = (unsigned char)(wet * 255.0f + 0.5f);
        }
    }
}

static DrainTile* GetTile(int tx, int tz)
{
    unsigned int hash = (unsigned int)tx * 374761393u ^ (unsigned int)tz * 668265263u;
    int slot = (int)(hash % DRAIN_CACHE);
    int victim = slot;
    float worst = -1.0f;
    for (int n = 0; n < 48; n++) {
        int i = (slot + n) % DRAIN_CACHE;
        DrainTile* t = &g_drain[i];
        if (t->live && t->tx == tx && t->tz == tz) return t;
        if (!t->live) {
            GenerateTile(t, tx, tz);
            return t;
        }
        float dx = (float)t->tx * 256.0f - g_focusX;
        float dz = (float)t->tz * 256.0f - g_focusZ;
        float d = dx * dx + dz * dz;
        if (d > worst) {
            worst = d;
            victim = i;
        }
    }
    GenerateTile(&g_drain[victim], tx, tz);
    return &g_drain[victim];
}

static void VertexDrain(int ix, int iz, float* h, float* wet)
{
    int tx = DivFloor(ix, DRAIN_N);
    int tz = DivFloor(iz, DRAIN_N);
    int lx = ix - tx * DRAIN_N;
    int lz = iz - tz * DRAIN_N;
    DrainTile* t = GetTile(tx, tz);
    *h = t->h[lz * DRAIN_N + lx];
    *wet = (float)t->wet[lz * DRAIN_N + lx] / 255.0f;
}

static void SampleDrain(float x, float z, float* height, float* wet)
{
    g_focusX = x;
    g_focusZ = z;
    float gx = x / DRAIN_CELL;
    float gz = z / DRAIN_CELL;
    int ix = (int)floorf(gx);
    int iz = (int)floorf(gz);
    float tx = gx - (float)ix;
    float tz = gz - (float)iz;
    float h00, h10, h01, h11, w00, w10, w01, w11;
    VertexDrain(ix, iz, &h00, &w00);
    VertexDrain(ix + 1, iz, &h10, &w10);
    VertexDrain(ix, iz + 1, &h01, &w01);
    VertexDrain(ix + 1, iz + 1, &h11, &w11);
    float hx0 = Lerpf(h00, h10, tx);
    float hx1 = Lerpf(h01, h11, tx);
    *height = Lerpf(hx0, hx1, tz);
    float wx0 = Lerpf(w00, w10, tx);
    float wx1 = Lerpf(w01, w11, tx);
    *wet = Lerpf(wx0, wx1, tz);
}

static Land LandAt(float x, float z)
{
    float city, farm, forest, highland;
    MasksAt(x, z, &city, &farm, &forest, &highland);
    float h = 0.0f, wet = 0.0f;
    SampleDrain(x, z, &h, &wet);
    (void)wet;
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

void Terrain_Cover(float x, float z, float* city, float* farm, float* forest, float* highland)
{
    MasksAt(x, z, city, farm, forest, highland);
}

void Terrain_HeightCover(float x, float z, float* height, float* city, float* farm, float* forest, float* highland)
{
    Land land = LandAt(x, z);
    *height = land.height;
    *city = land.city;
    *farm = land.farm;
    *forest = land.forest;
    *highland = land.highland;
}

float Terrain_Moisture(float x, float z)
{
    float h = 0.0f, wet = 0.0f;
    SampleDrain(x, z, &h, &wet);
    return Clampf(wet, 0.0f, 1.0f);
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

static float g_baseView = TERRAIN_VIEW_DEFAULT;
static float g_baseLod[4] = { 100.0f, 200.0f, 340.0f, 520.0f };
static float g_viewDistance = TERRAIN_VIEW_DEFAULT;
static float g_lod[4] = { 100.0f, 200.0f, 340.0f, 520.0f };
static int g_rings = TERRAIN_RINGS_DEFAULT;

float Terrain_SetViewDistance(float meters)
{
    if (meters < TERRAIN_VIEW_MIN) meters = TERRAIN_VIEW_MIN;
    if (meters > TERRAIN_VIEW_MAX) meters = TERRAIN_VIEW_MAX;
    g_baseView = meters;
    g_viewDistance = meters;
    return g_viewDistance;
}

float Terrain_ViewDistance(void)
{
    return g_viewDistance;
}

int Terrain_SetRingCount(int count)
{
    if (count < TERRAIN_RINGS_MIN) count = TERRAIN_RINGS_MIN;
    if (count > TERRAIN_RINGS_MAX) count = TERRAIN_RINGS_MAX;
    g_rings = count;
    return g_rings;
}

int Terrain_RingCount(void)
{
    return g_rings;
}

float Terrain_RingSpacing(int level)
{
    if (level < 0) level = 0;
    if (level > TERRAIN_RINGS_MAX - 1) level = TERRAIN_RINGS_MAX - 1;
    float s = g_viewDistance / (float)TERRAIN_CLIP_CELLS;
    return s * (float)(1 << level);
}

float Terrain_RingRadius(int level)
{
    return Terrain_RingSpacing(level) * (float)TERRAIN_CLIP_CELLS;
}

float Terrain_OuterRadius(void)
{
    return Terrain_RingRadius(g_rings - 1);
}

float Terrain_FarPlane(void)
{
    float far = Terrain_OuterRadius() * 1.35f;
    if (far < 8000.0f) far = 8000.0f;
    return far;
}

void Terrain_SetLodThresholds(float d0, float d1, float d2, float d3)
{
    g_baseLod[0] = g_lod[0] = d0;
    g_baseLod[1] = g_lod[1] = d1;
    g_baseLod[2] = g_lod[2] = d2;
    g_baseLod[3] = g_lod[3] = d3;
}

void Terrain_ApplyAltitude(float agl)
{
    if (agl < 0.0f) agl = 0.0f;
    /* At or below the reference the typed radius is exact. Above it the
     * rings grow so the ground past the rail keeps the same angle. The
     * factor is stepped by 1/8, otherwise every metre of climb would
     * rebuild a ring. */
    float q = 1.0f;
    if (agl > TERRAIN_ALT_REF) {
        q = agl / TERRAIN_ALT_REF;
        q = floorf(q * 8.0f + 0.5f) / 8.0f;
        if (q < 1.0f) q = 1.0f;
    }
    float steps = 1.0f;
    for (int i = 1; i < g_rings; i++) steps *= 2.0f;
    float radius = g_baseView * q;
    if (radius * steps > TERRAIN_OUTER_MAX) radius = TERRAIN_OUTER_MAX / steps;
    g_viewDistance = radius;
}

float Terrain_LodThreshold(int band)
{
    if (band < 0) band = 0;
    if (band > 3) band = 3;
    return g_lod[band];
}

int Terrain_LodForDistance(float distMeters)
{
    /* Bands should be wider than a chunk (64 m) so a border steps by one
     * level. Closer bands still stitch, they just change more abruptly. */
    if (distMeters < g_lod[0]) return 0;
    if (distMeters < g_lod[1]) return 1;
    if (distMeters < g_lod[2]) return 2;
    if (distMeters < g_lod[3]) return 3;
    return 4;
}

int Terrain_GridForLod(int lod)
{
    /* Nested by halves: a coarse edge is a subset of every finer grid.
     * The last level is a single quad; haze, not geometry, does the rest. */
    switch (lod) {
        case 0: return 16;
        case 1: return 8;
        case 2: return 4;
        case 3: return 2;
        default: return 1;
    }
}

/* Deterministic, so the answer is cached for the life of the process. */
#define WATER_CACHE 4096
static int g_wcX[WATER_CACHE];
static int g_wcZ[WATER_CACHE];
static unsigned char g_wcV[WATER_CACHE]; /* 0 empty, 1 dry, 2 wet */

static int ChunkTouchesWater(int cx, int cz)
{
    unsigned int h = (unsigned int)(cx * 374761393) ^ (unsigned int)(cz * 668265263);
    int slot = (int)(h % WATER_CACHE);
    for (int n = 0; n < 24; n++) {
        int i = (slot + n) % WATER_CACHE;
        if (g_wcV[i] == 0) break;
        if (g_wcX[i] == cx && g_wcZ[i] == cz) return g_wcV[i] == 2;
    }

    const int n = 8;
    float x0 = (float)cx * TERRAIN_CHUNK_SIZE;
    float z0 = (float)cz * TERRAIN_CHUNK_SIZE;
    float step = TERRAIN_CHUNK_SIZE / (float)n;
    float minY = 1.0e9f;
    float maxY = -1.0e9f;
    for (int j = 0; j <= n; j++) {
        for (int i = 0; i <= n; i++) {
            float y = Terrain_Height(x0 + (float)i * step, z0 + (float)j * step);
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    /* Deep water and dry hills do not own the shoreline. A bank does. */
    int wet = (minY < TERRAIN_WATER + 2.0f && maxY > TERRAIN_WATER - 0.4f) ? 2 : 1;

    for (int ntry = 0; ntry < 24; ntry++) {
        int i = (slot + ntry) % WATER_CACHE;
        if (g_wcV[i] == 0 || (g_wcX[i] == cx && g_wcZ[i] == cz)) {
            g_wcX[i] = cx;
            g_wcZ[i] = cz;
            g_wcV[i] = (unsigned char)wet;
            break;
        }
    }
    return wet == 2;
}

static int NeighborhoodWater(int cx, int cz)
{
    if (ChunkTouchesWater(cx, cz)) return 1;
    if (ChunkTouchesWater(cx - 1, cz) || ChunkTouchesWater(cx + 1, cz)) return 1;
    if (ChunkTouchesWater(cx, cz - 1) || ChunkTouchesWater(cx, cz + 1)) return 1;
    return 0;
}

int Terrain_EffectiveGrid(int cx, int cz, int lod, float viewX, float viewZ)
{
    int g = Terrain_GridForLod(lod);
    float mx = ((float)cx + 0.5f) * TERRAIN_CHUNK_SIZE;
    float mz = ((float)cz + 0.5f) * TERRAIN_CHUNK_SIZE;
    float dist = hypotf(mx - viewX, mz - viewZ);
    /* 4 m cells only within 240 m, 8 m out to 480 m. Past that the shore
     * is small on screen and the coarse grid is enough. */
    if (dist > 480.0f) return g;
    if (!NeighborhoodWater(cx, cz)) return g;
    if (dist <= 240.0f && g < 16) return 16;
    if (g < 8) return 8;
    return g;
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

    int ncx = cx;
    int ncz = cz;
    if (jx == 0) ncx = cx - 1;
    else if (jx == grid) ncx = cx + 1;
    else if (jz == 0) ncz = cz - 1;
    else ncz = cz + 1;
    int neighbor = Terrain_EffectiveGrid(ncx, ncz, ChunkLod(ncx, ncz, viewX, viewZ), viewX, viewZ);
    if (neighbor >= grid) return s;

    if (onX) SnapToCoarse(s.x, s.z, z0, size, neighbor, 0, &s);
    else SnapToCoarse(s.z, s.x, x0, size, neighbor, 1, &s);
    return s;
}
