#include "newgen.h"

#include "terrain.h"

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 12.3 km on a side: about 150 km². Coarse sheet at 64 m.
 * Tiles are 512 m. Detail 3 (8 m) is the one under the basket, detail 2
 * (16 m) the eight around it, detail 1 (32 m) the next crown. Detail 0
 * is the sheet, and it is not rebuilt while flying. */
#define NG_MAP   12288.0f
#define NG_CHUNK 2048.0f
#define NG_SPACE 64.0f
#define NG_TILE  512.0f
#define NG_SIDE  6
#define NG_QUADS 32

static int g_on = 0;
static int g_ownShader = 0;
static int g_perm[512];
static int g_tx, g_tz;
static int g_haveCenter = 0;

static Shader g_shader;
static int g_locEye = -1;
static int g_locFog = -1;
static int g_fog = 1;
static Material g_mat;

typedef struct Sheet {
    Mesh mesh;
    int ready;
} Sheet;

static Sheet g_sheet[NG_SIDE * NG_SIDE];
static Mesh g_fine;
static int g_fineReady = 0;

static float NgFade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float Grad(int h, float x, float z)
{
    switch (h & 3) {
        case 0: return x + z;
        case 1: return -x + z;
        case 2: return x - z;
        default: return -x - z;
    }
}

static void BuildPerm(unsigned int seed)
{
    int p[256];
    unsigned int s = seed ? seed : 1u;
    for (int i = 0; i < 256; i++) p[i] = i;
    for (int i = 255; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)(s % (unsigned int)(i + 1));
        int tmp = p[i];
        p[i] = p[j];
        p[j] = tmp;
    }
    for (int i = 0; i < 256; i++) {
        g_perm[i] = p[i];
        g_perm[i + 256] = p[i];
    }
}

static float Perlin(float x, float z)
{
    float xf = floorf(x);
    float zf = floorf(z);
    int x0 = ((int)xf) & 255;
    int z0 = ((int)zf) & 255;
    float tx = x - xf;
    float tz = z - zf;
    float u = NgFade(tx);
    float v = NgFade(tz);
    int aa = g_perm[g_perm[x0] + z0];
    int ba = g_perm[g_perm[x0 + 1] + z0];
    int ab = g_perm[g_perm[x0] + z0 + 1];
    int bb = g_perm[g_perm[x0 + 1] + z0 + 1];
    float g1 = Grad(aa, tx, tz) + (Grad(ba, tx - 1.0f, tz) - Grad(aa, tx, tz)) * u;
    float g2 = Grad(ab, tx, tz - 1.0f) + (Grad(bb, tx - 1.0f, tz - 1.0f) - Grad(ab, tx, tz - 1.0f)) * u;
    return g1 + (g2 - g1) * v;
}

float NewGen_Height(float x, float z)
{
    /* Warp first, so the same hills do not line up with the axes.
     * The offset is another, slower noise. Two samples. */
    float wx = Perlin(x * 0.00012f + 40.0f, z * 0.00012f) * 1100.0f;
    float wz = Perlin(x * 0.00012f, z * 0.00012f + 40.0f) * 1100.0f;
    float sx = x + wx;
    float sz = z + wz;

    float h = 12.0f;
    h += Perlin(sx * 0.00022f, sz * 0.00022f) * 26.0f;
    h += Perlin(sx * 0.0009f, sz * 0.0009f) * 12.0f;
    h += Perlin(sx * 0.0034f, sz * 0.0034f) * 4.5f;
    h += Perlin(sx * 0.012f, sz * 0.012f) * 1.4f;

    /* A slow mask: plains where it is low, a range where it is high.
     * Ridges are 1 - |noise|, squared, and each finer octave is kept
     * only on the crest of the previous one (Musgrave). */
    float mask = Perlin(sx * 0.00022f + 120.0f, sz * 0.00022f) * 0.5f + 0.5f;
    mask = (mask - 0.38f) / 0.28f;
    if (mask < 0.0f) mask = 0.0f;
    if (mask > 1.0f) mask = 1.0f;
    mask = mask * mask * (3.0f - 2.0f * mask);

    float ridge = 0.0f;
    float weight = 1.0f;
    float amp = 42.0f;
    float freq = 0.0011f;
    for (int oct = 0; oct < 4; oct++) {
        float n = Perlin(sx * freq + 19.0f, sz * freq + 7.0f);
        if (n > 1.0f) n = 1.0f;
        if (n < -1.0f) n = -1.0f;
        float crest = 1.0f - fabsf(n);
        crest = crest * crest * weight;
        ridge += crest * amp;
        weight = crest * 1.4f;
        if (weight > 1.0f) weight = 1.0f;
        freq *= 2.05f;
        amp *= 0.45f;
    }
    h += ridge * mask;
    if (h < 0.0f) h = 0.0f;
    return h;
}

int NewGen_Active(void) { return g_on; }

float NewGen_FarPlane(void) { return 28000.0f; }

static const char* VS_330 =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec3 vertexNormal;\n"
    "in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "out vec3 vWorld;\n"
    "void main() {\n"
    "    vWorld = vertexPosition;\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char* FS_330 =
    "#version 330\n"
    "in vec3 vWorld;\n"
    "out vec4 finalColor;\n"
    "uniform vec2 uEye;\n"
    "uniform float uFog;\n"
    "uniform vec4 colDiffuse;\n"
    "void main() {\n"
    "    float h = vWorld.y;\n"
    "    vec3 sand = vec3(0.78, 0.72, 0.46);\n"
    "    vec3 grass = vec3(0.34, 0.60, 0.30);\n"
    "    vec3 deep = vec3(0.22, 0.46, 0.24);\n"
    "    vec3 rock = vec3(0.52, 0.50, 0.46);\n"
    "    vec3 snow = vec3(0.93, 0.94, 0.96);\n"
    "    vec3 water = vec3(0.20, 0.42, 0.58);\n"
    "    vec3 c = mix(sand, grass, smoothstep(7.0, 12.0, h));\n"
    "    c = mix(c, deep, smoothstep(14.0, 26.0, h));\n"
    "    c = mix(c, rock, smoothstep(32.0, 46.0, h));\n"
    "    c = mix(c, snow, smoothstep(52.0, 66.0, h));\n"
    "    c = mix(water, c, smoothstep(4.2, 6.6, h));\n"
    "    float dist = length(vWorld.xz - uEye);\n"
    "    float edge = min(min(vWorld.x + 6144.0, 6144.0 - vWorld.x),\n"
    "                     min(vWorld.z + 6144.0, 6144.0 - vWorld.z));\n"
    "    float fog = smoothstep(2400.0, 5600.0, dist);\n"
    "    fog = max(fog, 1.0 - smoothstep(160.0, 1500.0, edge));\n"
    "    c = mix(c, vec3(0.910, 0.824, 0.706), clamp(fog, 0.0, 1.0) * uFog);\n"
    "    finalColor = vec4(c, 1.0) * colDiffuse;\n"
    "}\n";

static const char* VS_100 =
    "#version 100\n"
    "precision highp float;\n"
    "attribute vec3 vertexPosition;\n"
    "attribute vec2 vertexTexCoord;\n"
    "attribute vec3 vertexNormal;\n"
    "attribute vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "varying vec3 vWorld;\n"
    "void main() {\n"
    "    vWorld = vertexPosition;\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char* FS_100 =
    "#version 100\n"
    "precision highp float;\n"
    "varying vec3 vWorld;\n"
    "uniform vec2 uEye;\n"
    "uniform float uFog;\n"
    "uniform vec4 colDiffuse;\n"
    "void main() {\n"
    "    float h = vWorld.y;\n"
    "    vec3 sand = vec3(0.78, 0.72, 0.46);\n"
    "    vec3 grass = vec3(0.34, 0.60, 0.30);\n"
    "    vec3 deep = vec3(0.22, 0.46, 0.24);\n"
    "    vec3 rock = vec3(0.52, 0.50, 0.46);\n"
    "    vec3 snow = vec3(0.93, 0.94, 0.96);\n"
    "    vec3 water = vec3(0.20, 0.42, 0.58);\n"
    "    vec3 c = mix(sand, grass, smoothstep(7.0, 12.0, h));\n"
    "    c = mix(c, deep, smoothstep(14.0, 26.0, h));\n"
    "    c = mix(c, rock, smoothstep(32.0, 46.0, h));\n"
    "    c = mix(c, snow, smoothstep(52.0, 66.0, h));\n"
    "    c = mix(water, c, smoothstep(4.2, 6.6, h));\n"
    "    float dist = length(vWorld.xz - uEye);\n"
    "    float edge = min(min(vWorld.x + 6144.0, 6144.0 - vWorld.x),\n"
    "                     min(vWorld.z + 6144.0, 6144.0 - vWorld.z));\n"
    "    float fog = smoothstep(2400.0, 5600.0, dist);\n"
    "    fog = max(fog, 1.0 - smoothstep(160.0, 1500.0, edge));\n"
    "    c = mix(c, vec3(0.910, 0.824, 0.706), clamp(fog, 0.0, 1.0) * uFog);\n"
    "    gl_FragColor = vec4(c, 1.0) * colDiffuse;\n"
    "}\n";

static int LodAt(int tx, int tz)
{
    int dx = tx - g_tx;
    int dz = tz - g_tz;
    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;
    int d = dx > dz ? dx : dz;
    if (d >= 3) return 0;
    return 3 - d;
}

static float SpacingOf(int lod)
{
    return NG_SPACE / (float)(1 << lod);
}

static int Covered(float x, float z)
{
    int tx = (int)floorf(x / NG_TILE);
    int tz = (int)floorf(z / NG_TILE);
    return LodAt(tx, tz) > 0;
}

static void ClearMesh(Mesh* mesh, int* ready)
{
    if (*ready) {
        UnloadMesh(*mesh);
        *ready = 0;
    }
    memset(mesh, 0, sizeof(*mesh));
}

static void Upload(Mesh* mesh, int* ready, int verts, float* v, float* n, unsigned char* c, float* uv, unsigned short* idx, int idxCount)
{
    ClearMesh(mesh, ready);
    if (verts < 3 || idxCount < 3) {
        MemFree(v);
        MemFree(n);
        MemFree(c);
        MemFree(uv);
        MemFree(idx);
        return;
    }
    memset(mesh, 0, sizeof(*mesh));
    mesh->vertexCount = verts;
    mesh->triangleCount = idxCount / 3;
    mesh->vertices = v;
    mesh->normals = n;
    mesh->colors = c;
    mesh->texcoords = uv;
    mesh->indices = idx;
    UploadMesh(mesh, 0);
    *ready = 1;
}

static void FillGrid(float x0, float z0, int n, float s, float* y, int lodEdge)
{
    int stride = n + 1;
    for (int j = 0; j <= n; j++) {
        for (int i = 0; i <= n; i++) {
            float x = x0 + (float)i * s;
            float z = z0 + (float)j * s;
            float h = NewGen_Height(x, z);
            if (lodEdge) {
                int ex = (i == 0 || i == n);
                int ez = (j == 0 || j == n);
                float cs = s;
                if (i == 0) cs = fmaxf(cs, SpacingOf(LodAt((int)floorf((x0 - 1.0f) / NG_TILE), (int)floorf(z0 / NG_TILE))));
                if (i == n) cs = fmaxf(cs, SpacingOf(LodAt((int)floorf((x0 + (float)n * s) / NG_TILE), (int)floorf(z0 / NG_TILE))));
                if (j == 0) cs = fmaxf(cs, SpacingOf(LodAt((int)floorf(x0 / NG_TILE), (int)floorf((z0 - 1.0f) / NG_TILE))));
                if (j == n) cs = fmaxf(cs, SpacingOf(LodAt((int)floorf(x0 / NG_TILE), (int)floorf((z0 + (float)n * s) / NG_TILE))));
                if (ex && ez) {
                    /* Corner sits on every coarser grid. */
                } else if (ex && cs > s + 0.1f) {
                    float a = floorf(z / cs) * cs;
                    float t = (z - a) / cs;
                    h = NewGen_Height(x, a) * (1.0f - t) + NewGen_Height(x, a + cs) * t;
                } else if (ez && cs > s + 0.1f) {
                    float a = floorf(x / cs) * cs;
                    float t = (x - a) / cs;
                    h = NewGen_Height(a, z) * (1.0f - t) + NewGen_Height(a + cs, z) * t;
                }
            }
            y[j * stride + i] = h;
        }
    }
}

static void BuildChunk(int cx, int cz)
{
    Sheet* sh = &g_sheet[cz * NG_SIDE + cx];
    float x0 = -NG_MAP * 0.5f + (float)cx * NG_CHUNK;
    float z0 = -NG_MAP * 0.5f + (float)cz * NG_CHUNK;
    const int n = NG_QUADS;
    const int stride = n + 1;
    const int verts = stride * stride;
    float* y = (float*)MemAlloc((unsigned int)verts * sizeof(float));
    float* v = (float*)MemAlloc((unsigned int)verts * 3 * sizeof(float));
    float* nr = (float*)MemAlloc((unsigned int)verts * 3 * sizeof(float));
    unsigned char* c = (unsigned char*)MemAlloc((unsigned int)verts * 4);
    float* uv = (float*)MemAlloc((unsigned int)verts * 2 * sizeof(float));
    unsigned short* idx = (unsigned short*)MemAlloc((unsigned int)n * n * 6 * sizeof(unsigned short));
    if (!y || !v || !nr || !c || !uv || !idx) {
        MemFree(y); MemFree(v); MemFree(nr); MemFree(c); MemFree(uv); MemFree(idx);
        return;
    }
    FillGrid(x0, z0, n, NG_SPACE, y, 0);
    for (int j = 0; j <= n; j++) {
        for (int i = 0; i <= n; i++) {
            int id = j * stride + i;
            float x = x0 + (float)i * NG_SPACE;
            float z = z0 + (float)j * NG_SPACE;
            v[id * 3] = x;
            v[id * 3 + 1] = y[id];
            v[id * 3 + 2] = z;
            nr[id * 3] = 0.0f;
            nr[id * 3 + 1] = 1.0f;
            nr[id * 3 + 2] = 0.0f;
            c[id * 4] = 255;
            c[id * 4 + 1] = 255;
            c[id * 4 + 2] = 255;
            c[id * 4 + 3] = 255;
            uv[id * 2] = 0.0f;
            uv[id * 2 + 1] = 0.0f;
        }
    }
    int ni = 0;
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            float x = x0 + (float)i * NG_SPACE;
            float z = z0 + (float)j * NG_SPACE;
            if (Covered(x, z)) continue;
            unsigned short i00 = (unsigned short)(j * stride + i);
            unsigned short i10 = (unsigned short)(i00 + 1);
            unsigned short i01 = (unsigned short)(i00 + stride);
            unsigned short i11 = (unsigned short)(i01 + 1);
            idx[ni++] = i00; idx[ni++] = i11; idx[ni++] = i10;
            idx[ni++] = i00; idx[ni++] = i01; idx[ni++] = i11;
        }
    }
    MemFree(y);
    Upload(&sh->mesh, &sh->ready, verts, v, nr, c, uv, idx, ni);
}

static void PushFine(float* v, int* count, int cap, float x, float y, float z)
{
    if (*count >= cap) return;
    int i = *count;
    v[i * 3] = x;
    v[i * 3 + 1] = y;
    v[i * 3 + 2] = z;
    (*count)++;
}

static void BuildFine(void)
{
    const int cap = 100000;
    float* pos = (float*)MemAlloc((unsigned int)cap * 3 * sizeof(float));
    float* ybuf = (float*)MemAlloc(65 * 65 * sizeof(float));
    if (!pos || !ybuf) {
        MemFree(pos);
        MemFree(ybuf);
        return;
    }
    int count = 0;
    for (int dz = -2; dz <= 2; dz++) {
        for (int dx = -2; dx <= 2; dx++) {
            int adx = dx < 0 ? -dx : dx;
            int adz = dz < 0 ? -dz : dz;
            int d = adx > adz ? adx : adz;
            if (d >= 3) continue;
            int lod = 3 - d;
            float s = SpacingOf(lod);
            int n = (int)(NG_TILE / s + 0.5f);
            if (n > 64) n = 64;
            float x0 = (float)(g_tx + dx) * NG_TILE;
            float z0 = (float)(g_tz + dz) * NG_TILE;
            FillGrid(x0, z0, n, s, ybuf, 1);
            int stride = n + 1;
            for (int j = 0; j < n; j++) {
                for (int i = 0; i < n; i++) {
                    float x = x0 + (float)i * s;
                    float z = z0 + (float)j * s;
                    float y00 = ybuf[j * stride + i];
                    float y10 = ybuf[j * stride + i + 1];
                    float y11 = ybuf[(j + 1) * stride + i + 1];
                    float y01 = ybuf[(j + 1) * stride + i];
                    if (count + 6 > cap) break;
                    PushFine(pos, &count, cap, x, y00, z);
                    PushFine(pos, &count, cap, x + s, y11, z + s);
                    PushFine(pos, &count, cap, x + s, y10, z);
                    PushFine(pos, &count, cap, x, y00, z);
                    PushFine(pos, &count, cap, x, y01, z + s);
                    PushFine(pos, &count, cap, x + s, y11, z + s);
                }
            }
        }
    }
    MemFree(ybuf);
    if (count < 3) {
        MemFree(pos);
        ClearMesh(&g_fine, &g_fineReady);
        return;
    }
    float* nr = (float*)MemAlloc((unsigned int)count * 3 * sizeof(float));
    unsigned char* c = (unsigned char*)MemAlloc((unsigned int)count * 4);
    float* uv = (float*)MemAlloc((unsigned int)count * 2 * sizeof(float));
    if (!nr || !c || !uv) {
        MemFree(pos); MemFree(nr); MemFree(c); MemFree(uv);
        return;
    }
    for (int i = 0; i < count; i++) {
        nr[i * 3] = 0.0f;
        nr[i * 3 + 1] = 1.0f;
        nr[i * 3 + 2] = 0.0f;
        c[i * 4] = 255;
        c[i * 4 + 1] = 255;
        c[i * 4 + 2] = 255;
        c[i * 4 + 3] = 255;
        uv[i * 2] = 0.0f;
        uv[i * 2 + 1] = 0.0f;
    }
    ClearMesh(&g_fine, &g_fineReady);
    memset(&g_fine, 0, sizeof(g_fine));
    g_fine.vertexCount = count;
    g_fine.triangleCount = count / 3;
    g_fine.vertices = pos;
    g_fine.normals = nr;
    g_fine.colors = c;
    g_fine.texcoords = uv;
    UploadMesh(&g_fine, 0);
    g_fineReady = 1;
}

static void Rebuild(void)
{
    for (int cz = 0; cz < NG_SIDE; cz++)
        for (int cx = 0; cx < NG_SIDE; cx++)
            BuildChunk(cx, cz);
    BuildFine();
}

void NewGen_Begin(unsigned int seed)
{
    g_on = 1;
    BuildPerm(seed);
    Terrain_SetHeightFn(NewGen_Height);
    g_haveCenter = 0;
#ifdef PLATFORM_WEB
    g_shader = LoadShaderFromMemory(VS_100, FS_100);
    (void)VS_330;
    (void)FS_330;
#else
    g_shader = LoadShaderFromMemory(VS_330, FS_330);
    (void)VS_100;
    (void)FS_100;
#endif
    g_mat = LoadMaterialDefault();
    g_ownShader = (g_shader.id != 0);
    if (g_ownShader) g_mat.shader = g_shader;
    g_locEye = GetShaderLocation(g_shader, "uEye");
    g_locFog = GetShaderLocation(g_shader, "uFog");
    NewGen_SetFog(1);
    printf("newgen seed %u   sheet %.1f km at %.0f m   tiles %.0f m\n",
           seed, NG_MAP / 1000.0f, NG_SPACE, NG_TILE);
}

void NewGen_SetFog(int on)
{
    g_fog = on ? 1 : 0;
    if (g_locFog >= 0) {
        float v = g_fog ? 1.0f : 0.0f;
        SetShaderValue(g_shader, g_locFog, &v, SHADER_UNIFORM_FLOAT);
    }
}

void NewGen_Update(float x, float z)
{
    if (!g_on) return;
    int tx = (int)floorf(x / NG_TILE);
    int tz = (int)floorf(z / NG_TILE);
    if (g_haveCenter && tx == g_tx && tz == g_tz) return;
    g_tx = tx;
    g_tz = tz;
    g_haveCenter = 1;
    Rebuild();
}

void NewGen_Draw(float eyeX, float eyeZ)
{
    if (!g_on) return;
    if (g_locEye >= 0) {
        float eye[2] = { eyeX, eyeZ };
        SetShaderValue(g_shader, g_locEye, eye, SHADER_UNIFORM_VEC2);
    }
    for (int i = 0; i < NG_SIDE * NG_SIDE; i++) {
        if (!g_sheet[i].ready) continue;
        DrawMesh(g_sheet[i].mesh, g_mat, MatrixIdentity());
    }
    if (g_fineReady) DrawMesh(g_fine, g_mat, MatrixIdentity());
}

void NewGen_Shutdown(void)
{
    if (!g_on) return;
    for (int i = 0; i < NG_SIDE * NG_SIDE; i++) ClearMesh(&g_sheet[i].mesh, &g_sheet[i].ready);
    ClearMesh(&g_fine, &g_fineReady);
    if (g_ownShader) UnloadMaterial(g_mat);
    g_shader.id = 0;
    g_ownShader = 0;
    g_on = 0;
    Terrain_SetHeightFn(0);
}
