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
    int grid = Terrain_EffectiveGrid(cx, cz, lod, viewX, viewZ);
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

/* Cell count lives in TERRAIN_CLIP_CELLS. Spacing and radius come from
 * Terrain_RingSpacing, so a climb widens every ring without adding vertices. */

/* 0 at the inner edge of this ring, 1 well before the outer edge.
 * The outer band is already the coarser surface, so the square where two
 * rings meet is not a place where the shape or the colour changes. */
static float MorphAlpha(float x, float z, float viewX, float viewZ, float outer, int level)
{
    float dx = fabsf(x - viewX);
    float dz = fabsf(z - viewZ);
    float cheb = dx > dz ? dx : dz;
    float u;
    if (level <= 0) {
        u = (cheb / outer - 0.15f) / 0.55f;
    } else {
        float inner = outer * 0.5f;
        float t = (cheb - inner) / (outer - inner);
        u = t / 0.5f;
    }
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    return u * u * (3.0f - 2.0f * u);
}

void Clip_Describe(int level, float viewX, float viewZ, ClipJob* job)
{
    int rings = Terrain_RingCount();
    float s = Terrain_RingSpacing(level);
    float half = Terrain_RingRadius(level);
    float cx = floorf(viewX / s) * s;
    float cz = floorf(viewZ / s) * s;
    memset(job, 0, sizeof(*job));
    job->level = level;
    job->spacing = s;
    job->cx = cx;
    job->cz = cz;
    job->radius = half;
    job->viewX = viewX;
    job->viewZ = viewZ;
    if (level > 0) {
        float s0 = Terrain_RingSpacing(level - 1);
        float half0 = Terrain_RingRadius(level - 1);
        float cx0 = floorf(viewX / s0) * s0;
        float cz0 = floorf(viewZ / s0) * s0;
        job->holeL = floorf((cx0 - half0) / s + 1.0e-4f) * s;
        job->holeR = ceilf((cx0 + half0) / s - 1.0e-4f) * s;
        job->holeB = floorf((cz0 - half0) / s + 1.0e-4f) * s;
        job->holeT = ceilf((cz0 + half0) / s - 1.0e-4f) * s;
        job->hasHole = 1;
        job->seamL = job->holeL;
        job->seamR = job->holeR;
        job->seamB = job->holeB;
        job->seamT = job->holeT;
        /* The coarser quads run a full cell under the finer ring. */
        float tuck = s;
        job->holeL += tuck;
        job->holeR -= tuck;
        job->holeB += tuck;
        job->holeT -= tuck;
    }
    if (level + 1 < rings) {
        float S = Terrain_RingSpacing(level + 1);
        job->boundL = floorf((cx - half) / S + 1.0e-4f) * S;
        job->boundR = ceilf((cx + half) / S - 1.0e-4f) * S;
        job->boundB = floorf((cz - half) / S + 1.0e-4f) * S;
        job->boundT = ceilf((cz + half) / S - 1.0e-4f) * S;
        job->morph = 1;
    } else {
        job->boundL = cx - half;
        job->boundR = cx + half;
        job->boundB = cz - half;
        job->boundT = cz + half;
        job->morph = 0;
    }
}

static int QuadInside(float x, float z, float s, float L, float R, float B, float T)
{
    return x >= L - 0.05f && x + s <= R + 0.05f && z >= B - 0.05f && z + s <= T + 0.05f;
}

static void GridNormal(const float* h, int stride, int i, int j, int n, int m, float step,
                       float* nx, float* ny, float* nz)
{
    float hl = h[j * stride + (i > 0 ? i - 1 : i)];
    float hr = h[j * stride + (i < n ? i + 1 : i)];
    float hd = h[(j > 0 ? j - 1 : j) * stride + i];
    float hu = h[(j < m ? j + 1 : j) * stride + i];
    float sx = (i > 0 && i < n) ? 2.0f * step : step;
    float sz = (j > 0 && j < m) ? 2.0f * step : step;
    float dhx = (hl - hr) / sx;
    float dhz = (hd - hu) / sz;
    float len = sqrtf(dhx * dhx + 1.0f + dhz * dhz);
    *nx = dhx / len;
    *ny = 1.0f / len;
    *nz = dhz / len;
}

static float HSample(const float* h, int stride, int n, int m, int i, int j)
{
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > n) i = n;
    if (j > m) j = m;
    return h[j * stride + i];
}

static Color CSample(const Color* c, int stride, int n, int m, int i, int j)
{
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > n) i = n;
    if (j > m) j = m;
    return c[j * stride + i];
}

/* Height, cover and normal of the coarser triangle under this vertex.
 * The fine vertex flattens onto that plane, so the two rings are the same
 * surface where they meet. */
static void CoarseSurface(const float* h, const Color* col, int stride, int n, int m,
                          int ii, int jj, float s, float* hy, Color* c, float* nx, float* ny, float* nz)
{
    int ci = ii & ~1;
    int cj = jj & ~1;
    float tx = (float)(ii - ci) * 0.5f;
    float tz = (float)(jj - cj) * 0.5f;
    float h00 = HSample(h, stride, n, m, ci, cj);
    float h10 = HSample(h, stride, n, m, ci + 2, cj);
    float h01 = HSample(h, stride, n, m, ci, cj + 2);
    float h11 = HSample(h, stride, n, m, ci + 2, cj + 2);
    Color c00 = CSample(col, stride, n, m, ci, cj);
    Color c10 = CSample(col, stride, n, m, ci + 2, cj);
    Color c01 = CSample(col, stride, n, m, ci, cj + 2);
    Color c11 = CSample(col, stride, n, m, ci + 2, cj + 2);

    float y0, y1, y2;
    Color k0, k1, k2;
    float x0, z0, x1, z1, x2, z2;
    if (tx >= tz) {
        y0 = h00; y1 = h11; y2 = h10;
        k0 = c00; k1 = c11; k2 = c10;
        x0 = 0.0f; z0 = 0.0f; x1 = 1.0f; z1 = 1.0f; x2 = 1.0f; z2 = 0.0f;
    } else {
        y0 = h00; y1 = h01; y2 = h11;
        k0 = c00; k1 = c01; k2 = c11;
        x0 = 0.0f; z0 = 0.0f; x1 = 0.0f; z1 = 1.0f; x2 = 1.0f; z2 = 1.0f;
    }
    float den = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2);
    float w0 = 0.0f, w1 = 0.0f, w2 = 1.0f;
    if (fabsf(den) > 1.0e-6f) {
        w0 = ((z1 - z2) * (tx - x2) + (x2 - x1) * (tz - z2)) / den;
        w1 = ((z2 - z0) * (tx - x2) + (x0 - x2) * (tz - z2)) / den;
        w2 = 1.0f - w0 - w1;
    }
    *hy = y0 * w0 + y1 * w1 + y2 * w2;
    c->r = (unsigned char)(w0 * k0.r + w1 * k1.r + w2 * k2.r + 0.5f);
    c->g = (unsigned char)(w0 * k0.g + w1 * k1.g + w2 * k2.g + 0.5f);
    c->b = (unsigned char)(w0 * k0.b + w1 * k1.b + w2 * k2.b + 0.5f);
    c->a = (unsigned char)(w0 * k0.a + w1 * k1.a + w2 * k2.a + 0.5f);

    float ex1 = (x1 - x0) * 2.0f * s;
    float ez1 = (z1 - z0) * 2.0f * s;
    float ey1 = y1 - y0;
    float ex2 = (x2 - x0) * 2.0f * s;
    float ez2 = (z2 - z0) * 2.0f * s;
    float ey2 = y2 - y0;
    float nxw = ey1 * ez2 - ey2 * ez1;
    float nyw = ez1 * ex2 - ez2 * ex1;
    float nzw = ex1 * ey2 - ex2 * ey1;
    float len = sqrtf(nxw * nxw + nyw * nyw + nzw * nzw);
    if (len < 1.0e-5f) {
        *nx = 0.0f; *ny = 1.0f; *nz = 0.0f;
    } else if (nyw < 0.0f) {
        *nx = -nxw / len; *ny = -nyw / len; *nz = -nzw / len;
    } else {
        *nx = nxw / len; *ny = nyw / len; *nz = nzw / len;
    }
}

static void Skirt(float* v, float* n, unsigned char* col, float* uv, int* gi,
                  float x0, float y0, float z0, float x1, float y1, float z1, Color c0, Color c1)
{
    const float drop = 12.0f;
    int i = *gi;
    PushVert(v, n, col, uv, i++, x0, y0, z0, 0.0f, 1.0f, 0.0f, c0);
    PushVert(v, n, col, uv, i++, x0, y0 - drop, z0, 0.0f, 1.0f, 0.0f, c0);
    PushVert(v, n, col, uv, i++, x1, y1 - drop, z1, 0.0f, 1.0f, 0.0f, c1);
    PushVert(v, n, col, uv, i++, x0, y0, z0, 0.0f, 1.0f, 0.0f, c0);
    PushVert(v, n, col, uv, i++, x1, y1 - drop, z1, 0.0f, 1.0f, 0.0f, c1);
    PushVert(v, n, col, uv, i++, x1, y1, z1, 0.0f, 1.0f, 0.0f, c1);
    *gi = i;
}

int Clip_Build(const ClipJob* job, Mesh* out)
{
    float s = job->spacing;
    float x0, z0, x1, z1;
    if (job->morph) {
        x0 = job->boundL;
        z0 = job->boundB;
        x1 = job->boundR;
        z1 = job->boundT;
    } else {
        x0 = job->cx - job->radius;
        z0 = job->cz - job->radius;
        x1 = job->cx + job->radius;
        z1 = job->cz + job->radius;
    }
    int n = (int)floorf((x1 - x0) / s + 0.5f);
    int m = (int)floorf((z1 - z0) / s + 0.5f);
    if (n < 2) n = 2;
    if (m < 2) m = 2;
    int stride = n + 1;
    int count = stride * (m + 1);
    float* h = (float*)MemAlloc((unsigned int)count * sizeof(float));
    Color* col = (Color*)MemAlloc((unsigned int)count * sizeof(Color));
    if (!h || !col) {
        MemFree(h);
        MemFree(col);
        return 0;
    }
    for (int j = 0; j <= m; j++) {
        float z = z0 + (float)j * s;
        for (int i = 0; i <= n; i++) {
            float x = x0 + (float)i * s;
            float y, city, farm, forest, highland;
            Terrain_HeightCover(x, z, &y, &city, &farm, &forest, &highland);
            h[j * stride + i] = y;
            col[j * stride + i] = (Color){
                (unsigned char)(Clampf(city, 0.0f, 1.0f) * 255.0f + 0.5f),
                (unsigned char)(Clampf(farm, 0.0f, 1.0f) * 255.0f + 0.5f),
                (unsigned char)(Clampf(forest, 0.0f, 1.0f) * 255.0f + 0.5f),
                (unsigned char)(Clampf(highland, 0.0f, 1.0f) * 255.0f + 0.5f)
            };
        }
    }

    int maxVerts = n * m * 6 + (n + m) * 48;
    float* gv = (float*)MemAlloc((unsigned int)maxVerts * 3 * sizeof(float));
    float* gn = (float*)MemAlloc((unsigned int)maxVerts * 3 * sizeof(float));
    unsigned char* gc = (unsigned char*)MemAlloc((unsigned int)maxVerts * 4);
    float* gu = (float*)MemAlloc((unsigned int)maxVerts * 2 * sizeof(float));
    if (!gv || !gn || !gc || !gu) {
        MemFree(h);
        MemFree(col);
        MemFree(gv);
        MemFree(gn);
        MemFree(gc);
        MemFree(gu);
        return 0;
    }

    int gi = 0;
    int tris[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
    for (int j = 0; j < m; j++) {
        float z = z0 + (float)j * s;
        for (int i = 0; i < n; i++) {
            float x = x0 + (float)i * s;
            if (job->hasHole && QuadInside(x, z, s, job->holeL, job->holeR, job->holeB, job->holeT))
                continue;
            int id[4][2] = { { i, j }, { i + 1, j }, { i + 1, j + 1 }, { i, j + 1 } };
            float px[4], py[4], pz[4], pnx[4], pny[4], pnz[4];
            Color pc[4];
            for (int k = 0; k < 4; k++) {
                int ii = id[k][0];
                int jj = id[k][1];
                float wx = x0 + (float)ii * s;
                float wz = z0 + (float)jj * s;
                float wy = h[jj * stride + ii];
                float nx, ny, nz;
                GridNormal(h, stride, ii, jj, n, m, s, &nx, &ny, &nz);
                Color c = col[jj * stride + ii];
                if (job->morph) {
                    float a = MorphAlpha(wx, wz, job->viewX, job->viewZ, job->radius, job->level);
                    /* The shared edge is exactly the coarser surface, not
                     * almost. A partial blend there opens a crack. */
                    float edge = s * 1.5f;
                    if (wx - x0 < edge || x1 - wx < edge || wz - z0 < edge || z1 - wz < edge)
                        a = 1.0f;
                    if (a > 0.0f) {
                        float hy;
                        float cnx, cny, cnz;
                        Color cc;
                        CoarseSurface(h, col, stride, n, m, ii, jj, s, &hy, &cc, &cnx, &cny, &cnz);
                        wy = wy + (hy - wy) * a;
                        nx = nx + (cnx - nx) * a;
                        ny = ny + (cny - ny) * a;
                        nz = nz + (cnz - nz) * a;
                        c = LerpColor(c, cc, a);
                        float len = sqrtf(nx * nx + ny * ny + nz * nz);
                        if (len > 1.0e-5f) {
                            nx /= len;
                            ny /= len;
                            nz /= len;
                        }
                    }
                }
                px[k] = wx;
                py[k] = wy;
                pz[k] = wz;
                pnx[k] = nx;
                pny[k] = ny;
                pnz[k] = nz;
                pc[k] = c;
            }
            for (int t = 0; t < 2; t++) {
                for (int k = 0; k < 3; k++) {
                    int q = tris[t][k];
                    PushVert(gv, gn, gc, gu, gi, px[q], py[q], pz[q], pnx[q], pny[q], pnz[q], pc[q]);
                    gi++;
                }
            }
            /* A short skirt under the outer edge. A one-pixel crack then
             * shows ground, not the beige sky, and it cannot flicker. */
            if (i == 0) Skirt(gv, gn, gc, gu, &gi, px[0], py[0], pz[0], px[3], py[3], pz[3], pc[0], pc[3]);
            if (i + 1 == n) Skirt(gv, gn, gc, gu, &gi, px[1], py[1], pz[1], px[2], py[2], pz[2], pc[1], pc[2]);
            if (j == 0) Skirt(gv, gn, gc, gu, &gi, px[0], py[0], pz[0], px[1], py[1], pz[1], pc[0], pc[1]);
            if (j + 1 == m) Skirt(gv, gn, gc, gu, &gi, px[3], py[3], pz[3], px[2], py[2], pz[2], pc[3], pc[2]);
        }
    }
    MemFree(h);
    MemFree(col);
    if (gi < 3) {
        MemFree(gv);
        MemFree(gn);
        MemFree(gc);
        MemFree(gu);
        return 0;
    }
    UploadFlat(out, gi, gv, gn, gc, gu);
    return 1;
}

int Clip_GatherProps(float viewX, float viewZ, Prop* dst, int cap)
{
    int n = 0;
    const float tile = 64.0f;
    int ccx = (int)floorf(viewX / tile);
    int ccz = (int)floorf(viewZ / tile);
    for (int dz = -6; dz <= 6; dz++) {
        for (int dx = -6; dx <= 6; dx++) {
            float x0 = (float)(ccx + dx) * tile;
            float z0 = (float)(ccz + dz) * tile;
            float dist = hypotf(x0 + tile * 0.5f - viewX, z0 + tile * 0.5f - viewZ);
            if (dist > 340.0f) continue;
            ChunkMesh tmp;
            memset(&tmp, 0, sizeof(tmp));
            AddProps(0, 0, x0, z0, tile, dist < 160.0f ? 0 : 2, viewX, viewZ, &tmp);
            for (int p = 0; p < tmp.propCount && n < cap; p++) dst[n++] = tmp.props[p];
        }
    }
    return n;
}

