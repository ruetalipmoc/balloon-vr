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

/* Packed into the vertex colour for the ground shader: r city, g farm,
 * b forest, a highland. The pixel shader paints the actual pattern. */
static Color CoverColor(float x, float z)
{
    float city, farm, forest, highland;
    Terrain_Cover(x, z, &city, &farm, &forest, &highland);
    Color c;
    c.r = (unsigned char)(Clampf(city, 0.0f, 1.0f) * 255.0f + 0.5f);
    c.g = (unsigned char)(Clampf(farm, 0.0f, 1.0f) * 255.0f + 0.5f);
    c.b = (unsigned char)(Clampf(forest, 0.0f, 1.0f) * 255.0f + 0.5f);
    c.a = (unsigned char)(Clampf(highland, 0.0f, 1.0f) * 255.0f + 0.5f);
    return c;
}

static void VertexShade(float x, float y, float z, float viewX, float viewZ, float step,
                        float* nx, float* ny, float* nz, Color* c)
{
    Terrain_Normal(x, z, nx, ny, nz);
    *c = CoverColor(x, z);
}

/* Border samples that were pulled onto a coarser edge inherit that edge's
 * colour and normal, so the two LODs shade as one surface. */
static void SampleShade(const TerrainSample* s, float viewX, float viewZ, float step,
                        float* nx, float* ny, float* nz, Color* c)
{
    if (!s->snapped) {
        VertexShade(s->x, s->y, s->z, viewX, viewZ, step, nx, ny, nz, c);
        return;
    }
    float nax, nay, naz, nbx, nby, nbz;
    Color ca, cb;
    VertexShade(s->ax, Terrain_Height(s->ax, s->az), s->az, viewX, viewZ, step, &nax, &nay, &naz, &ca);
    VertexShade(s->bx, Terrain_Height(s->bx, s->bz), s->bz, viewX, viewZ, step, &nbx, &nby, &nbz, &cb);
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

int ChunkMesh_Build(int cx, int cz, int lod, float viewX, float viewZ, ChunkMesh* out)
{
    memset(out, 0, sizeof(*out));
    int grid = Terrain_EffectiveGrid(cx, cz, lod);
    float size = TERRAIN_CHUNK_SIZE;
    float x0 = (float)cx * size;
    float z0 = (float)cz * size;

    int quads = grid * grid;
    int groundVerts = quads * 6;
    float* gv = (float*)MemAlloc((unsigned int)groundVerts * 3 * sizeof(float));
    float* gn = (float*)MemAlloc((unsigned int)groundVerts * 3 * sizeof(float));
    unsigned char* gc = (unsigned char*)MemAlloc((unsigned int)groundVerts * 4);
    float* gu = (float*)MemAlloc((unsigned int)groundVerts * 2 * sizeof(float));

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
            float step = size / (float)grid;
            for (int k = 0; k < 4; k++)
                SampleShade(&corner[k], viewX, viewZ, step, &nx[k], &ny[k], &nz[k], &col[k]);
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
    UploadFlat(&out->ground, gi, gv, gn, gc, gu);

    if (lod <= 2) AddProps(cx, cz, x0, z0, size, lod, viewX, viewZ, out);
    out->ready = 1;
    return 1;
}

void ChunkMesh_Free(ChunkMesh* chunk)
{
    if (!chunk->ready) return;
    UnloadMesh(chunk->ground);
    memset(chunk, 0, sizeof(*chunk));
}

/* A vertex on the outer edge of a fine band is lerped onto the next coarser
 * step, so the coarse band's straight edge occupies the same line. */
static float BandHeight(float x, float z, float oL, float oR, float oB, float oT, float snap)
{
    int onL = fabsf(x - oL) <= 0.2f;
    int onR = fabsf(x - oR) <= 0.2f;
    int onB = fabsf(z - oB) <= 0.2f;
    int onT = fabsf(z - oT) <= 0.2f;
    int edgeX = (onL || onR) && !(onB || onT);
    int edgeZ = (onB || onT) && !(onL || onR);
    if (!edgeX && !edgeZ) return Terrain_Height(x, z);

    float along = edgeX ? z : x;
    float fixed = edgeX ? x : z;
    float a = floorf(along / snap) * snap;
    float b = a + snap;
    float t = (along - a) / snap;
    if (t < 0.001f || t > 0.999f) return Terrain_Height(x, z);
    float h0 = edgeX ? Terrain_Height(fixed, a) : Terrain_Height(a, fixed);
    float h1 = edgeX ? Terrain_Height(fixed, b) : Terrain_Height(b, fixed);
    return Lerpf(h0, h1, t);
}

static float AlignDown(float v, float step)
{
    return floorf(v / step + 1.0e-4f) * step;
}

static float AlignUp(float v, float step)
{
    return ceilf(v / step - 1.0e-4f) * step;
}

typedef struct FarBuf {
    float* v;
    float* n;
    unsigned char* c;
    float* uv;
    int count;
    int cap;
} FarBuf;

static int FarPush(FarBuf* b, float x, float y, float z, float viewX, float viewZ, float step, Color* forced)
{
    if (b->count >= b->cap) {
        int cap = b->cap > 0 ? b->cap * 2 : 2048;
        b->v = (float*)MemRealloc(b->v, (unsigned int)cap * 3 * sizeof(float));
        b->n = (float*)MemRealloc(b->n, (unsigned int)cap * 3 * sizeof(float));
        b->c = (unsigned char*)MemRealloc(b->c, (unsigned int)cap * 4);
        b->uv = (float*)MemRealloc(b->uv, (unsigned int)cap * 2 * sizeof(float));
        if (!b->v || !b->n || !b->c || !b->uv) return 0;
        b->cap = cap;
    }
    float nx, ny, nz;
    Terrain_Normal(x, z, &nx, &ny, &nz);
    Color col = CoverColor(x, z);
    PushVert(b->v, b->n, b->c, b->uv, b->count, x, y, z, nx, ny, nz, col);
    b->count++;
    return 1;
}

static void FarTri(FarBuf* b, float x0, float z0, float x1, float z1, float x2, float z2,
                   float oL, float oR, float oB, float oT, float snap, float step,
                   float viewX, float viewZ)
{
    float xs[3] = { x0, x1, x2 };
    float zs[3] = { z0, z1, z2 };
    float ys[3];
    for (int i = 0; i < 3; i++) ys[i] = BandHeight(xs[i], zs[i], oL, oR, oB, oT, snap);
    for (int i = 0; i < 3; i++) {
        if (!FarPush(b, xs[i], ys[i], zs[i], viewX, viewZ, step, NULL)) return;
    }
}

static void FarQuad(FarBuf* b, float x0, float z0, float x1, float z1,
                    float oL, float oR, float oB, float oT, float snap, float step,
                    float viewX, float viewZ)
{
    float y00 = BandHeight(x0, z0, oL, oR, oB, oT, snap);
    float y10 = BandHeight(x1, z0, oL, oR, oB, oT, snap);
    float y11 = BandHeight(x1, z1, oL, oR, oB, oT, snap);
    float y01 = BandHeight(x0, z1, oL, oR, oB, oT, snap);
    float span = x1 - x0;
    if (z1 - z0 > span) span = z1 - z0;
    float minY = y00;
    float maxY = y00;
    if (y10 < minY) minY = y10;
    if (y11 < minY) minY = y11;
    if (y01 < minY) minY = y01;
    if (y10 > maxY) maxY = y10;
    if (y11 > maxY) maxY = y11;
    if (y01 > maxY) maxY = y01;
    /* Keep following the bank down to the same 4 m grid the chunks use,
     * so the shore does not jump when this cell becomes a detailed tile. */
    if (span > 4.5f && minY < TERRAIN_WATER + 2.0f && maxY > TERRAIN_WATER - 0.4f && b->count < 180000) {
        float xm = (x0 + x1) * 0.5f;
        float zm = (z0 + z1) * 0.5f;
        FarQuad(b, x0, z0, xm, zm, oL, oR, oB, oT, snap, step, viewX, viewZ);
        FarQuad(b, xm, z0, x1, zm, oL, oR, oB, oT, snap, step, viewX, viewZ);
        FarQuad(b, x0, zm, xm, z1, oL, oR, oB, oT, snap, step, viewX, viewZ);
        FarQuad(b, xm, zm, x1, z1, oL, oR, oB, oT, snap, step, viewX, viewZ);
        return;
    }
    FarTri(b, x0, z0, x1, z1, x1, z0, oL, oR, oB, oT, snap, step, viewX, viewZ);
    FarTri(b, x0, z0, x0, z1, x1, z1, oL, oR, oB, oT, snap, step, viewX, viewZ);
}

static int CellInside(float x0, float x1, float z0, float z1,
                      float iL, float iR, float iB, float iT)
{
    return x0 >= iL - 0.2f && x1 <= iR + 0.2f && z0 >= iB - 0.2f && z1 <= iT + 0.2f;
}

/* Border between an inner rectangle and a larger one, in quads of `step`.
 * The outer edge is snapped onto `snap` (the next band's step). */
static void FarFrame(FarBuf* b, float iL, float iR, float iB, float iT,
                     float oL, float oR, float oB, float oT, float step, float snap,
                     float viewX, float viewZ)
{
    if (oR - oL < step || oT - oB < step) return;
    for (float z = oB; z < oT - 0.1f; z += step) {
        float z1 = z + step;
        if (z1 > oT) z1 = oT;
        for (float x = oL; x < oR - 0.1f; x += step) {
            float x1 = x + step;
            if (x1 > oR) x1 = oR;
            if (CellInside(x, x1, z, z1, iL, iR, iB, iT)) continue;
            FarQuad(b, x, z, x1, z1, oL, oR, oB, oT, snap, step, viewX, viewZ);
        }
    }
}

int FarField_Build(float viewX, float viewZ,
                   float holeX0, float holeX1, float holeZ0, float holeZ1,
                   float outer, float seam, Mesh* out)
{
    memset(out, 0, sizeof(*out));
    if (seam < 4.0f) seam = 4.0f;
    if (seam > 64.0f) seam = 64.0f;
    /* First ring matches the streamed edge. Its outside is snapped onto the
     * next band's 128 m step so the two share a line. */
    const float reach[] = { 96.0f, 640.0f, 1664.0f, 3712.0f, 7808.0f, 12000.0f };
    const float step[] = { seam, 128.0f, 256.0f, 512.0f, 1024.0f, 2048.0f };
    const float snap[] = { 128.0f, 256.0f, 512.0f, 1024.0f, 2048.0f, 2048.0f };
    const int bands = 6;

    if (outer < 256.0f) outer = 256.0f;
    if (outer > 12000.0f) outer = 12000.0f;

    FarBuf buf;
    memset(&buf, 0, sizeof(buf));

    float iL = holeX0, iR = holeX1, iB = holeZ0, iT = holeZ1;
    for (int i = 0; i < bands; i++) {
        float oL = AlignDown(holeX0 - reach[i], snap[i]);
        float oR = AlignUp(holeX1 + reach[i], snap[i]);
        float oB = AlignDown(holeZ0 - reach[i], snap[i]);
        float oT = AlignUp(holeZ1 + reach[i], snap[i]);
        float limL = AlignDown(viewX - outer, snap[i]);
        float limR = AlignUp(viewX + outer, snap[i]);
        float limB = AlignDown(viewZ - outer, snap[i]);
        float limT = AlignUp(viewZ + outer, snap[i]);
        if (oL < limL) oL = limL;
        if (oR > limR) oR = limR;
        if (oB < limB) oB = limB;
        if (oT > limT) oT = limT;
        if (!(oL < iL && oR > iR && oB < iB && oT > iT)) {
            if (reach[i] >= outer) break;
            continue;
        }
        FarFrame(&buf, iL, iR, iB, iT, oL, oR, oB, oT, step[i], snap[i], viewX, viewZ);
        iL = oL;
        iR = oR;
        iB = oB;
        iT = oT;
        if (reach[i] >= outer) break;
    }

    if (buf.count < 3) {
        MemFree(buf.v);
        MemFree(buf.n);
        MemFree(buf.c);
        MemFree(buf.uv);
        return 0;
    }
    UploadFlat(out, buf.count, buf.v, buf.n, buf.c, buf.uv);
    return 1;
}
