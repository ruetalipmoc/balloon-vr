#include "terrain_mesh.h"

#include "terrain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int Terrain_LodForDistance(float distMeters)
{
    if (distMeters < 90.0f) return 0;    /* 16x16 quads */
    if (distMeters < 180.0f) return 1;   /* 6x6 */
    if (distMeters < 320.0f) return 2;   /* 2x2 */
    return 3;                            /* a single quad */
}

static int GridForLod(int lod)
{
    switch (lod) {
        case 0: return 16;
        case 1: return 6;
        case 2: return 2;
        default: return 1;
    }
}

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

static Color GroundColor(float x, float z, float h, float slopeY, float fog)
{
    Color grassA = { 78, 168, 72, 255 };
    Color grassB = { 52, 128, 64, 255 };
    Color forest = { 36, 96, 52, 255 };
    Color sand = { 214, 196, 138, 255 };
    Color rock = { 142, 138, 128, 255 };
    Color snow = { 236, 242, 246, 255 };
    Color sky = { 186, 214, 228, 255 };

    float moisture = Terrain_Moisture(x, z);
    float n = Terrain_Height(x * 0.37f + 20.0f, z * 0.37f - 10.0f);
    float patch = n - floorf(n);
    Color grass = LerpColor(grassA, grassB, patch);

    Color c = grass;
    if (moisture > 0.58f && h < 42.0f && slopeY > 0.82f) c = LerpColor(grass, forest, 0.65f);
    if (h < 9.0f && moisture < 0.42f) c = LerpColor(sand, grass, h / 9.0f);
    if (slopeY < 0.78f) {
        float rockT = (0.78f - slopeY) / 0.28f;
        if (rockT > 1.0f) rockT = 1.0f;
        c = LerpColor(c, rock, rockT);
    }
    if (h > 46.0f) {
        float snowT = (h - 46.0f) / 16.0f;
        if (snowT > 1.0f) snowT = 1.0f;
        c = LerpColor(c, snow, snowT);
    }
    if (h < TERRAIN_WATER) c = (Color){ 46, 110, 150, 255 };
    return LerpColor(c, sky, fog);
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

static void AddProps(int cx, int cz, float x0, float z0, float size, ChunkMesh* out)
{
    out->propCount = 0;
    const int cells = 4;
    float cell = size / (float)cells;
    for (int jz = 0; jz < cells; jz++) {
        for (int jx = 0; jx < cells; jx++) {
            if (out->propCount >= CHUNK_MAX_PROPS) return;
            int wx = cx * 16 + jx;
            int wz = cz * 16 + jz;
            unsigned int h = CellHash(wx, wz);
            float px = x0 + ((float)jx + 0.2f + (float)(h & 255) / 255.0f * 0.6f) * cell;
            float pz = z0 + ((float)jz + 0.2f + (float)((h >> 8) & 255) / 255.0f * 0.6f) * cell;
            float py = Terrain_Height(px, pz);
            if (py < TERRAIN_WATER + 0.4f) continue;
            float nx, ny, nz;
            Terrain_Normal(px, pz, &nx, &ny, &nz);
            if (ny < 0.86f) {
                if ((h % 5u) != 0u) continue;
                Prop* p = &out->props[out->propCount++];
                p->kind = 2;
                p->x = px;
                p->y = py;
                p->z = pz;
                p->yaw = (float)(h % 360u);
                p->scale = 0.6f + (float)(h % 100u) / 100.0f * 1.4f;
                continue;
            }
            float moisture = Terrain_Moisture(px, pz);
            if ((h % 23u) == 0u && moisture < 0.7f && py < 28.0f && py > TERRAIN_WATER + 1.0f) {
                Prop* p = &out->props[out->propCount++];
                p->kind = 3;
                p->x = px;
                p->y = py;
                p->z = pz;
                p->yaw = (float)((h >> 4) % 180u);
                p->scale = 1.0f;
                continue;
            }
            if (moisture < 0.48f) continue;
            if ((h % 3u) == 0u) continue;
            Prop* p = &out->props[out->propCount++];
            p->kind = (moisture > 0.72f) ? 1 : 0;
            p->x = px;
            p->y = py;
            p->z = pz;
            p->yaw = (float)(h % 360u);
            p->scale = 0.75f + (float)((h >> 12) % 100u) / 100.0f * 0.9f;
        }
    }
}

int ChunkMesh_Build(int cx, int cz, int lod, float viewX, float viewZ, ChunkMesh* out)
{
    memset(out, 0, sizeof(*out));
    int grid = GridForLod(lod);
    float size = TERRAIN_CHUNK_SIZE;
    float x0 = (float)cx * size;
    float z0 = (float)cz * size;
    float step = size / (float)grid;

    int quads = grid * grid;
    int groundVerts = quads * 6;
    float* gv = (float*)MemAlloc((unsigned int)groundVerts * 3 * sizeof(float));
    float* gn = (float*)MemAlloc((unsigned int)groundVerts * 3 * sizeof(float));
    unsigned char* gc = (unsigned char*)MemAlloc((unsigned int)groundVerts * 4);
    float* gu = (float*)MemAlloc((unsigned int)groundVerts * 2 * sizeof(float));

    int waterQuads = 0;
    /* Count water cells first so the water mesh can be exact. */
    for (int jz = 0; jz < grid; jz++) {
        for (int jx = 0; jx < grid; jx++) {
            float xa = x0 + (float)jx * step;
            float za = z0 + (float)jz * step;
            float h00 = Terrain_Height(xa, za);
            float h10 = Terrain_Height(xa + step, za);
            float h11 = Terrain_Height(xa + step, za + step);
            float h01 = Terrain_Height(xa, za + step);
            if (h00 < TERRAIN_WATER || h10 < TERRAIN_WATER || h11 < TERRAIN_WATER || h01 < TERRAIN_WATER)
                waterQuads++;
        }
    }

    float cxm = x0 + size * 0.5f;
    float czm = z0 + size * 0.5f;
    float dist = hypotf(cxm - viewX, czm - viewZ);
    float fog = (dist - 70.0f) / 380.0f;
    if (fog < 0.0f) fog = 0.0f;
    if (fog > 0.82f) fog = 0.82f;

    int gi = 0;
    for (int jz = 0; jz < grid; jz++) {
        for (int jx = 0; jx < grid; jx++) {
            float xa = x0 + (float)jx * step;
            float za = z0 + (float)jz * step;
            float xb = xa + step;
            float zb = za + step;
            float h00 = Terrain_Height(xa, za);
            float h10 = Terrain_Height(xb, za);
            float h11 = Terrain_Height(xb, zb);
            float h01 = Terrain_Height(xa, zb);
            float mx = (xa + xb) * 0.5f;
            float mz = (za + zb) * 0.5f;
            float mh = (h00 + h10 + h11 + h01) * 0.25f;
            float nx, ny, nz;
            Terrain_Normal(mx, mz, &nx, &ny, &nz);
            Color col = GroundColor(mx, mz, mh, ny, fog);

            float px[4] = { xa, xb, xb, xa };
            float pz[4] = { za, za, zb, zb };
            float py[4] = { h00, h10, h11, h01 };
            /* Winding faces up (right-handed Y-up). The other order is culled
             * from the basket, which sits above the mesh. */
            int tris[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
            for (int t = 0; t < 2; t++) {
                for (int k = 0; k < 3; k++) {
                    int id = tris[t][k];
                    PushVert(gv, gn, gc, gu, gi, px[id], py[id], pz[id], nx, ny, nz, col);
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
        Color water = { 64, 150, 196, 168 };
        water = LerpColor(water, (Color){ 186, 214, 228, 168 }, fog * 0.7f);
        water.a = 170;
        int wi = 0;
        for (int jz = 0; jz < grid; jz++) {
            for (int jx = 0; jx < grid; jx++) {
                float xa = x0 + (float)jx * step;
                float za = z0 + (float)jz * step;
                float xb = xa + step;
                float zb = za + step;
                float h00 = Terrain_Height(xa, za);
                float h10 = Terrain_Height(xb, za);
                float h11 = Terrain_Height(xb, zb);
                float h01 = Terrain_Height(xa, zb);
                if (!(h00 < TERRAIN_WATER || h10 < TERRAIN_WATER || h11 < TERRAIN_WATER || h01 < TERRAIN_WATER))
                    continue;
                float y = TERRAIN_WATER;
                float px[4] = { xa, xb, xb, xa };
                float pz[4] = { za, za, zb, zb };
                /* Winding faces up (right-handed Y-up). The other order is culled
             * from the basket, which sits above the mesh. */
            int tris[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
                for (int t = 0; t < 2; t++) {
                    for (int k = 0; k < 3; k++) {
                        int id = tris[t][k];
                        PushVert(wv, wn, wc, wu, wi, px[id], y, pz[id], 0, 1, 0, water);
                        wi++;
                    }
                }
            }
        }
        UploadFlat(&out->water, wvCount, wv, wn, wc, wu);
        out->hasWater = 1;
    }

    if (lod == 0) AddProps(cx, cz, x0, z0, size, out);
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
