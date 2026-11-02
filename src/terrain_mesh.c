#include "terrain_mesh.h"

#include "sim_math.h"
#include "terrain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static unsigned int CellHash(int x, int z)
{
    unsigned int h = (unsigned int)x * 2246822519u ^ (unsigned int)z * 3266489917u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h;
}

static Color LerpColor(Color a, Color b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    Color c;
    c.r = (unsigned char)((float)a.r + ((float)b.r - (float)a.r) * t);
    c.g = (unsigned char)((float)a.g + ((float)b.g - (float)a.g) * t);
    c.b = (unsigned char)((float)a.b + ((float)b.b - (float)a.b) * t);
    c.a = 255;
    return c;
}

static Color ApplyHaze(Color c, float fog);

static Color GroundColor(float x, float z, float h, float slopeY, float fog)
{
    Color grassA = { 86, 166, 74, 255 };
    Color grassB = { 58, 132, 62, 255 };
    Color forestA = { 34, 92, 48, 255 };
    Color forestB = { 24, 70, 40, 255 };
    Color sand = { 214, 196, 138, 255 };
    Color rock = { 142, 138, 128, 255 };
    Color snow = { 236, 242, 246, 255 };
    Color hedge = { 58, 104, 52, 255 };
    Color asphalt = { 74, 78, 84, 255 };
    Color crops[5] = {
        { 206, 176, 64, 255 },
        { 78, 154, 64, 255 },
        { 158, 106, 64, 255 },
        { 138, 174, 78, 255 },
        { 220, 196, 62, 255 }
    };
    Color blocks[3] = {
        { 168, 164, 156, 255 },
        { 186, 178, 166, 255 },
        { 150, 156, 162, 255 }
    };

    float patch = (float)(CellHash((int)floorf(x * 0.07f), (int)floorf(z * 0.07f)) & 255) / 255.0f;
    int zone = Terrain_Zone(x, z);
    Color c = LerpColor(grassA, grassB, patch);

    if (zone == TERRAIN_ZONE_FOREST) c = LerpColor(forestA, forestB, patch);
    if (zone == TERRAIN_ZONE_HIGHLAND) c = LerpColor(grassB, forestA, 0.35f);

    if (zone == TERRAIN_ZONE_FARM && h > TERRAIN_WATER + 1.2f) {
        float gx = floorf(x / TERRAIN_FIELD);
        float gz = floorf(z / TERRAIN_FIELD);
        float u = x / TERRAIN_FIELD - gx;
        float v = z / TERRAIN_FIELD - gz;
        if (u < 0.07f || v < 0.07f) {
            c = hedge;
        } else {
            c = crops[CellHash((int)gx, (int)gz) % 5u];
        }
    }

    if (zone == TERRAIN_ZONE_CITY && h > TERRAIN_WATER + 1.0f) {
        float bx = floorf(x / TERRAIN_CITY_BLOCK);
        float bz = floorf(z / TERRAIN_CITY_BLOCK);
        float u = x / TERRAIN_CITY_BLOCK - bx;
        float v = z / TERRAIN_CITY_BLOCK - bz;
        if (u < 0.18f || v < 0.18f) {
            c = asphalt;
        } else if ((CellHash((int)bx, (int)bz) % 11u) == 0u) {
            c = LerpColor(grassA, grassB, 0.4f);
        } else {
            c = blocks[CellHash((int)bx + 3, (int)bz - 1) % 3u];
        }
    }

    if (zone != TERRAIN_ZONE_FARM && zone != TERRAIN_ZONE_CITY && h < TERRAIN_WATER + 2.4f && h >= TERRAIN_WATER) {
        c = LerpColor(sand, c, (h - TERRAIN_WATER) / 2.4f);
    }
    if (zone == TERRAIN_ZONE_HIGHLAND && slopeY < 0.88f) {
        float rockT = (0.88f - slopeY) / 0.30f;
        c = LerpColor(c, rock, Clampf(rockT, 0.0f, 1.0f));
    } else if (zone != TERRAIN_ZONE_FARM && zone != TERRAIN_ZONE_CITY && slopeY < 0.70f) {
        float rockT = (0.70f - slopeY) / 0.28f;
        c = LerpColor(c, rock, Clampf(rockT, 0.0f, 1.0f));
    }
    if (h > 52.0f) {
        float snowT = (h - 52.0f) / 18.0f;
        c = LerpColor(c, snow, Clampf(snowT, 0.0f, 1.0f));
    }
    if (h < TERRAIN_WATER) c = (Color){ 46, 110, 150, 255 };
    return ApplyHaze(c, fog);
}

/* Aerial perspective. Colour drains toward a hazy blue, then the last of it
 * meets the sky horizon so the far edge of the world does not draw a line. */
static Color ApplyHaze(Color c, float fog)
{
    if (fog <= 0.0f) return c;
    Color haze = { 150, 188, 214, 255 };
    Color horizon = { 232, 210, 180, 255 };
    Color washed = LerpColor(c, haze, fog);
    float intoSky = (fog - 0.75f) / 0.25f;
    if (intoSky <= 0.0f) return washed;
    if (intoSky > 1.0f) intoSky = 1.0f;
    return LerpColor(washed, horizon, intoSky);
}

static float FogAmount(float dist)
{
    /* Crisp through the near rings. The far LOD (past ~520 m) is where the
     * ground gives up its colour and turns blue. */
    float t = (dist - 220.0f) / 620.0f;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static void VertexShade(float x, float y, float z, float viewX, float viewZ,
                        float* nx, float* ny, float* nz, Color* c)
{
    Terrain_Normal(x, z, nx, ny, nz);
    *c = GroundColor(x, z, y, *ny, FogAmount(hypotf(x - viewX, z - viewZ)));
}

/* Border samples that were pulled onto a coarser edge inherit that edge's
 * colour and normal, so the two LODs shade as one surface. */
static void SampleShade(const TerrainSample* s, float viewX, float viewZ,
                        float* nx, float* ny, float* nz, Color* c)
{
    if (!s->snapped) {
        VertexShade(s->x, s->y, s->z, viewX, viewZ, nx, ny, nz, c);
        return;
    }
    float nax, nay, naz, nbx, nby, nbz;
    Color ca, cb;
    VertexShade(s->ax, Terrain_Height(s->ax, s->az), s->az, viewX, viewZ, &nax, &nay, &naz, &ca);
    VertexShade(s->bx, Terrain_Height(s->bx, s->bz), s->bz, viewX, viewZ, &nbx, &nby, &nbz, &cb);
    float vx = nax + (nbx - nax) * s->t;
    float vy = nay + (nby - nay) * s->t;
    float vz = naz + (nbz - naz) * s->t;
    float len = sqrtf(vx * vx + vy * vy + vz * vz);
    if (len < 1.0e-5f) len = 1.0f;
    *nx = vx / len;
    *ny = vy / len;
    *nz = vz / len;
    *c = LerpColor(ca, cb, s->t);
}

static void PushVert(float* v, float* n, unsigned char* col, float* uv, int i,
                      float x, float y, float z, float nx, float ny, float nz, Color c)
{
    v[i * 3 + 0] = x;
    v[i * 3 + 1] = y;
    v[i * 3 + 2] = z;
    n[i * 3 + 0] = nx;
    n[i * 3 + 1] = ny;
    n[i * 3 + 2] = nz;
    col[i * 4 + 0] = c.r;
    col[i * 4 + 1] = c.g;
    col[i * 4 + 2] = c.b;
    col[i * 4 + 3] = c.a;
    uv[i * 2 + 0] = 0.0f;
    uv[i * 2 + 1] = 0.0f;
}

static int UploadFlat(Mesh* mesh, int vertCount, float* v, float* nrm, unsigned char* col, float* uv)
{
    memset(mesh, 0, sizeof(*mesh));
    mesh->vertexCount = vertCount;
    mesh->triangleCount = vertCount / 3;
    mesh->vertices = v;
    mesh->normals = nrm;
    mesh->colors = col;
    mesh->texcoords = uv;
    UploadMesh(mesh, 0);
    return 1;
}

static void PushProp(ChunkMesh* out, int kind, float x, float y, float z, float yaw, float scale)
{
    if (out->propCount >= CHUNK_MAX_PROPS) return;
    Prop* p = &out->props[out->propCount++];
    p->kind = kind;
    p->x = x;
    p->y = y;
    p->z = z;
    p->yaw = yaw;
    p->scale = scale;
}

static float FadeScale(float x, float z, float viewX, float viewZ, float begin, float end)
{
    float u = (hypotf(x - viewX, z - viewZ) - begin) / (end - begin);
    if (u <= 0.0f) return 1.0f;
    if (u >= 1.0f) return 0.0f;
    float s = u * u * (3.0f - 2.0f * u);
    return 1.0f - s;
}

/* Blocks whose centres fall in this chunk. Parks keep a tree instead of a roof.
 * Scale eases off with distance so a building does not vanish on a ring. */
static void AddCity(float x0, float z0, float size, float viewX, float viewZ, ChunkMesh* out)
{
    const float block = TERRAIN_CITY_BLOCK;
    int i0 = (int)floorf(x0 / block);
    int i1 = (int)floorf((x0 + size - 0.01f) / block);
    int k0 = (int)floorf(z0 / block);
    int k1 = (int)floorf((z0 + size - 0.01f) / block);
    for (int iz = k0; iz <= k1; iz++) {
        for (int ix = i0; ix <= i1; ix++) {
            if (out->propCount >= CHUNK_MAX_PROPS) return;
            float px = ((float)ix + 0.5f) * block;
            float pz = ((float)iz + 0.5f) * block;
            if (px < x0 || pz < z0 || px >= x0 + size || pz >= z0 + size) continue;
            if (Terrain_Zone(px, pz) != TERRAIN_ZONE_CITY) continue;
            float py = Terrain_Height(px, pz);
            if (py < TERRAIN_WATER + 1.0f) continue;
            unsigned int h = CellHash(ix, iz);
            if ((h % 11u) == 0u) {
                float fade = FadeScale(px, pz, viewX, viewZ, 110.0f, 260.0f);
                if (fade < 0.05f) continue;
                PushProp(out, 0, px, py, pz, 0.0f, 1.35f * fade);
                continue;
            }
            float fade = FadeScale(px, pz, viewX, viewZ, 180.0f, 320.0f);
            if (fade < 0.05f) continue;
            int tower = (h % 5u) == 0u;
            float scale = 1.1f + (float)(h % 100u) / 100.0f * 1.2f;
            if (tower) scale += 0.7f;
            PushProp(out, tower ? 6 : 5, px, py, pz, 0.0f, scale * fade);
        }
    }
}

static void AddProps(int cx, int cz, float x0, float z0, float size, int lod,
                     float viewX, float viewZ, ChunkMesh* out)
{
    (void)cx;
    (void)cz;
    out->propCount = 0;
    AddCity(x0, z0, size, viewX, viewZ, out);
    if (lod > 2) return;

    const int cells = 5;
    float cell = size / (float)cells;
    for (int jz = 0; jz < cells; jz++) {
        for (int jx = 0; jx < cells; jx++) {
            if (out->propCount >= CHUNK_MAX_PROPS) return;
            int wx = (int)floorf(x0 / cell) + jx;
            int wz = (int)floorf(z0 / cell) + jz;
            unsigned int h = CellHash(wx, wz);
            float px = x0 + ((float)jx + 0.18f + (float)(h & 255) / 255.0f * 0.64f) * cell;
            float pz = z0 + ((float)jz + 0.18f + (float)((h >> 8) & 255) / 255.0f * 0.64f) * cell;
            float py = Terrain_Height(px, pz);
            if (py < TERRAIN_WATER + 0.6f) continue;
            float nx, ny, nz;
            Terrain_Normal(px, pz, &nx, &ny, &nz);
            int zone = Terrain_Zone(px, pz);
            float fade = FadeScale(px, pz, viewX, viewZ, 110.0f, 260.0f);
            if (fade < 0.05f) continue;
            float scale = (0.8f + (float)((h >> 12) % 100u) / 100.0f * 0.85f) * fade;

            if (zone == TERRAIN_ZONE_CITY) continue;
            if (zone == TERRAIN_ZONE_FARM) {
                if (ny < 0.9f) continue;
                if ((h % 53u) == 0u) PushProp(out, 4, px, py, pz, 0.0f, 2.6f * fade);
                else if ((h % 17u) == 0u) PushProp(out, 0, px, py, pz, (float)(h % 360u), scale);
                continue;
            }
            if (ny < 0.84f) {
                if (zone == TERRAIN_ZONE_HIGHLAND && (h % 3u) == 0u)
                    PushProp(out, 2, px, py, pz, (float)(h % 360u), 0.7f + scale);
                continue;
            }
            if (zone == TERRAIN_ZONE_FOREST) {
                if ((h % 5u) == 0u) continue;
                float tree = (2.4f + (float)((h >> 12) % 100u) / 100.0f * 1.8f) * fade;
                PushProp(out, (h % 3u) == 0u ? 1 : 0, px, py, pz, (float)(h % 360u), tree);
                continue;
            }
            if (zone == TERRAIN_ZONE_HIGHLAND) {
                if ((h % 2u) != 0u) continue;
                PushProp(out, (h % 4u) == 0u ? 2 : 1, px, py, pz, (float)(h % 360u), scale);
                continue;
            }
            /* Open country: a few trees, the odd farmhouse. */
            if ((h % 29u) == 0u && py < 36.0f) {
                PushProp(out, 3, px, py, pz, 0.0f, fade);
                continue;
            }
            if ((h % 4u) != 0u) continue;
            PushProp(out, 0, px, py, pz, (float)(h % 360u), scale);
        }
    }
}

/* 0 through the inner part of this level's ring, 1 at the outer edge,
 * where the next chunk is already the coarser grid. */
static float LodMorph(int lod, float dist)
{
    float start = 0.0f;
    float end = 100.0f;
    if (lod == 1) { start = 100.0f; end = 200.0f; }
    else if (lod == 2) { start = 200.0f; end = 340.0f; }
    else if (lod == 3) { start = 340.0f; end = 520.0f; }
    else if (lod >= 4) return 0.0f;
    float t = (dist - start) / (end - start);
    float u = (t - 0.55f) / 0.45f;
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    return u * u * (3.0f - 2.0f * u);
}

static int OnChunkBorder(float x, float z, float x0, float z0, float size)
{
    const float e = 0.08f;
    return (x <= x0 + e || x >= x0 + size - e || z <= z0 + e || z >= z0 + size - e);
}

/* Height and normal of the next-coarser grid. A fully morphed vertex lands
 * on the same surface the neighbouring chunk already draws. */
static void CoarseSurface(float x, float z, float x0, float z0, float size, int coarse,
                          float* y, float* nx, float* ny, float* nz)
{
    if (coarse < 1) coarse = 1;
    float step = size / (float)coarse;
    float fx = (x - x0) / step;
    float fz = (z - z0) / step;
    int ix = (int)floorf(fx);
    int iz = (int)floorf(fz);
    if (ix < 0) ix = 0;
    if (iz < 0) iz = 0;
    if (ix >= coarse) ix = coarse - 1;
    if (iz >= coarse) iz = coarse - 1;
    float tx = Clampf(fx - (float)ix, 0.0f, 1.0f);
    float tz = Clampf(fz - (float)iz, 0.0f, 1.0f);
    float xA = x0 + (float)ix * step;
    float zA = z0 + (float)iz * step;
    float h00 = Terrain_Height(xA, zA);
    float h10 = Terrain_Height(xA + step, zA);
    float h01 = Terrain_Height(xA, zA + step);
    float h11 = Terrain_Height(xA + step, zA + step);
    *y = Lerpf(Lerpf(h00, h10, tx), Lerpf(h01, h11, tx), tz);
    float dhdx = Lerpf(h10 - h00, h11 - h01, tz) / step;
    float dhdz = Lerpf(h01 - h00, h11 - h10, tx) / step;
    float vx = -dhdx;
    float vy = 1.0f;
    float vz = -dhdz;
    float len = sqrtf(vx * vx + vy * vy + vz * vz);
    if (len < 1.0e-5f) len = 1.0f;
    *nx = vx / len;
    *ny = vy / len;
    *nz = vz / len;
}

/* Interior only. Border vertices stay put: the fine side is snapped onto
 * them, and moving either side would open a crack. */
static void MorphCorner(TerrainSample* s, int lod, int grid, float x0, float z0, float size,
                        float viewX, float viewZ, float* nx, float* ny, float* nz)
{
    if (OnChunkBorder(s->x, s->z, x0, z0, size)) return;
    int coarse = grid / 2;
    if (coarse < 1) return;
    float m = LodMorph(lod, hypotf(s->x - viewX, s->z - viewZ));
    if (m <= 0.001f) return;
    float cy, cnx, cny, cnz;
    CoarseSurface(s->x, s->z, x0, z0, size, coarse, &cy, &cnx, &cny, &cnz);
    s->y = Lerpf(s->y, cy, m);
    if (!nx) return;
    float vx = Lerpf(*nx, cnx, m);
    float vy = Lerpf(*ny, cny, m);
    float vz = Lerpf(*nz, cnz, m);
    float len = sqrtf(vx * vx + vy * vy + vz * vz);
    if (len < 1.0e-5f) len = 1.0f;
    *nx = vx / len;
    *ny = vy / len;
    *nz = vz / len;
}

int ChunkMesh_Build(int cx, int cz, int lod, float viewX, float viewZ, ChunkMesh* out)
{
    memset(out, 0, sizeof(*out));
    int grid = Terrain_GridForLod(lod);
    float size = TERRAIN_CHUNK_SIZE;
    float x0 = (float)cx * size;
    float z0 = (float)cz * size;

    int quads = grid * grid;
    int groundVerts = quads * 6;
    float* gv = (float*)MemAlloc((unsigned int)groundVerts * 3 * sizeof(float));
    float* gn = (float*)MemAlloc((unsigned int)groundVerts * 3 * sizeof(float));
    unsigned char* gc = (unsigned char*)MemAlloc((unsigned int)groundVerts * 4);
    float* gu = (float*)MemAlloc((unsigned int)groundVerts * 2 * sizeof(float));

    int waterQuads = 0;
    for (int jz = 0; jz < grid; jz++) {
        for (int jx = 0; jx < grid; jx++) {
            TerrainSample s00 = Terrain_Sample(jx, jz, grid, x0, z0, size, cx, cz, viewX, viewZ);
            TerrainSample s10 = Terrain_Sample(jx + 1, jz, grid, x0, z0, size, cx, cz, viewX, viewZ);
            TerrainSample s11 = Terrain_Sample(jx + 1, jz + 1, grid, x0, z0, size, cx, cz, viewX, viewZ);
            TerrainSample s01 = Terrain_Sample(jx, jz + 1, grid, x0, z0, size, cx, cz, viewX, viewZ);
            MorphCorner(&s00, lod, grid, x0, z0, size, viewX, viewZ, NULL, NULL, NULL);
            MorphCorner(&s10, lod, grid, x0, z0, size, viewX, viewZ, NULL, NULL, NULL);
            MorphCorner(&s11, lod, grid, x0, z0, size, viewX, viewZ, NULL, NULL, NULL);
            MorphCorner(&s01, lod, grid, x0, z0, size, viewX, viewZ, NULL, NULL, NULL);
            if (s00.y < TERRAIN_WATER || s10.y < TERRAIN_WATER || s11.y < TERRAIN_WATER || s01.y < TERRAIN_WATER)
                waterQuads++;
        }
    }

    int gi = 0;
    for (int jz = 0; jz < grid; jz++) {
        for (int jx = 0; jx < grid; jx++) {
            TerrainSample corner[4] = {
                Terrain_Sample(jx, jz, grid, x0, z0, size, cx, cz, viewX, viewZ),
                Terrain_Sample(jx + 1, jz, grid, x0, z0, size, cx, cz, viewX, viewZ),
                Terrain_Sample(jx + 1, jz + 1, grid, x0, z0, size, cx, cz, viewX, viewZ),
                Terrain_Sample(jx, jz + 1, grid, x0, z0, size, cx, cz, viewX, viewZ)
            };
            float nx[4], ny[4], nz[4];
            Color col[4];
            for (int k = 0; k < 4; k++) {
                SampleShade(&corner[k], viewX, viewZ, &nx[k], &ny[k], &nz[k], &col[k]);
                MorphCorner(&corner[k], lod, grid, x0, z0, size, viewX, viewZ, &nx[k], &ny[k], &nz[k]);
            }
            /* Winding faces up (right-handed Y-up). The other order is culled
             * from the basket, which sits above the mesh. */
            int tris[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
            for (int t = 0; t < 2; t++) {
                for (int k = 0; k < 3; k++) {
                    int id = tris[t][k];
                    PushVert(gv, gn, gc, gu, gi, corner[id].x, corner[id].y, corner[id].z,
                             nx[id], ny[id], nz[id], col[id]);
                    gi++;
                }
            }
        }
    }
    UploadFlat(&out->ground, groundVerts, gv, gn, gc, gu);

    if (waterQuads > 0) {
        int wvCount = waterQuads * 6;
        float* wv = (float*)MemAlloc((unsigned int)wvCount * 3 * sizeof(float));
        float* wn = (float*)MemAlloc((unsigned int)wvCount * 3 * sizeof(float));
        unsigned char* wc = (unsigned char*)MemAlloc((unsigned int)wvCount * 4);
        float* wu = (float*)MemAlloc((unsigned int)wvCount * 2 * sizeof(float));
        int wi = 0;
        for (int jz = 0; jz < grid; jz++) {
            for (int jx = 0; jx < grid; jx++) {
                TerrainSample corner[4] = {
                    Terrain_Sample(jx, jz, grid, x0, z0, size, cx, cz, viewX, viewZ),
                    Terrain_Sample(jx + 1, jz, grid, x0, z0, size, cx, cz, viewX, viewZ),
                    Terrain_Sample(jx + 1, jz + 1, grid, x0, z0, size, cx, cz, viewX, viewZ),
                    Terrain_Sample(jx, jz + 1, grid, x0, z0, size, cx, cz, viewX, viewZ)
                };
                for (int k = 0; k < 4; k++)
                    MorphCorner(&corner[k], lod, grid, x0, z0, size, viewX, viewZ, NULL, NULL, NULL);
                if (!(corner[0].y < TERRAIN_WATER || corner[1].y < TERRAIN_WATER ||
                      corner[2].y < TERRAIN_WATER || corner[3].y < TERRAIN_WATER))
                    continue;
                int tris[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
                for (int t = 0; t < 2; t++) {
                    for (int k = 0; k < 3; k++) {
                        int id = tris[t][k];
                        float fog = FogAmount(hypotf(corner[id].x - viewX, corner[id].z - viewZ));
                        Color water = { 64, 150, 196, 255 };
                        water = ApplyHaze(water, fog);
                        water.a = 170;
                        PushVert(wv, wn, wc, wu, wi, corner[id].x, TERRAIN_WATER, corner[id].z,
                                 0, 1, 0, water);
                        wi++;
                    }
                }
            }
        }
        UploadFlat(&out->water, wvCount, wv, wn, wc, wu);
        out->hasWater = 1;
    }

    if (lod <= 2) AddProps(cx, cz, x0, z0, size, lod, viewX, viewZ, out);
    out->ready = 1;
    return 1;
}

void ChunkMesh_Free(ChunkMesh* chunk)
{
    if (!chunk->ready) return;
    UnloadMesh(chunk->ground);
    if (chunk->hasWater) UnloadMesh(chunk->water);
    memset(chunk, 0, sizeof(*chunk));
}
