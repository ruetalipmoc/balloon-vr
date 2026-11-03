#include "newgen.h"
#include "scene.h"

#include "ground_shader.h"
#include "sim_math.h"
#include "terrain.h"
#include "terrain_mesh.h"
#include "world.h"

#include "rlgl.h"
#include "raymath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Clipmap rings replace the old chunk slots. */
#define MAX_PARTICLES 192
#define MAX_PROPS 768

typedef struct ClipSlot {
    Mesh mesh;
    int ready;
    ClipJob job;
    Mesh pending;
    int pendingReady;
} ClipSlot;

typedef struct Particle {
    float x, y, z;
    float vx, vy, vz;
    float life, maxLife;
    int kind; /* 0 flame, 1 steam */
} Particle;

struct Scene {
    Balloon balloon;
    Handles handles;
    Watch watch;
    float groundY;
    float flame;
    float ventOpen;
    float simTime;
    HandInput lastHands[2];

    Mesh envelope;
    Mesh crown;
    Mesh sky;
    ClipSlot clip[CLIP_LEVELS];
    int clipSnap;
    ClipJob clipTarget[CLIP_LEVELS];
    Prop props[MAX_PROPS];
    int propCount;
    int propsReady;
    float propX, propZ;
    Mesh propMesh;
    int propMeshReady;
    int resetView;
    Material material;
    Material groundMat;
    int locEye;
    int locWater;
    int locCoverMap;
    Texture2D coverTex;
    unsigned char* coverPx;
    unsigned char* coverScratch;
    float coverOriginX, coverOriginZ, coverTexel;
    int coverShiftX, coverShiftY;
    int coverJob;
    float jobTexel, jobOriginX, jobOriginZ;
    int coverReady;
    int meshesReady;

    Particle particles[MAX_PARTICLES];
    AudioStream burner;
    int audioReady;
};

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

/* Four broad gores, two cloth colours. Thin multi-colour panels read as a
 * pinwheel from the basket, which is the only place the envelope is seen. */
static Color GoreColor(int slice)
{
    if (slice & 4) return (Color){ 176, 52, 56, 255 };
    return (Color){ 232, 220, 198, 255 };
}

/* Nomex skirt: the surface closest to the camera when looking up. One colour,
 * so the mouth is a circle instead of where every gore happens to end. */
static Color SkirtColor(void)
{
    return (Color){ 198, 156, 108, 255 };
}

static Mesh MakeFlatMesh(int vertCount, float* v, float* n, unsigned char* c, float* uv)
{
    Mesh mesh = { 0 };
    mesh.vertexCount = vertCount;
    mesh.triangleCount = vertCount / 3;
    mesh.vertices = v;
    mesh.normals = n;
    mesh.colors = c;
    mesh.texcoords = uv;
    UploadMesh(&mesh, 0);
    return mesh;
}

static void PushTri(float* v, float* n, unsigned char* c, float* uv, int* i,
                     Vector3 a, Vector3 b, Vector3 d, Color col)
{
    Vector3 e1 = Vector3Subtract(b, a);
    Vector3 e2 = Vector3Subtract(d, a);
    Vector3 normal = Vector3Normalize(Vector3CrossProduct(e1, e2));
    Vector3 p[3] = { a, b, d };
    for (int k = 0; k < 3; k++) {
        int idx = *i;
        v[idx * 3 + 0] = p[k].x;
        v[idx * 3 + 1] = p[k].y;
        v[idx * 3 + 2] = p[k].z;
        n[idx * 3 + 0] = normal.x;
        n[idx * 3 + 1] = normal.y;
        n[idx * 3 + 2] = normal.z;
        c[idx * 4 + 0] = col.r;
        c[idx * 4 + 1] = col.g;
        c[idx * 4 + 2] = col.b;
        c[idx * 4 + 3] = col.a;
        uv[idx * 2 + 0] = 0;
        uv[idx * 2 + 1] = 0;
        (*i)++;
    }
}

static void PushTriN(float* v, float* n, unsigned char* c, float* uv, int* i,
                      Vector3 a, Vector3 b, Vector3 d,
                      Vector3 na, Vector3 nb, Vector3 nd,
                      Color ca, Color cb, Color cd)
{
    Vector3 p[3] = { a, b, d };
    Vector3 pn[3] = { na, nb, nd };
    Color pc[3] = { ca, cb, cd };
    for (int k = 0; k < 3; k++) {
        int idx = *i;
        v[idx * 3 + 0] = p[k].x;
        v[idx * 3 + 1] = p[k].y;
        v[idx * 3 + 2] = p[k].z;
        n[idx * 3 + 0] = pn[k].x;
        n[idx * 3 + 1] = pn[k].y;
        n[idx * 3 + 2] = pn[k].z;
        c[idx * 4 + 0] = pc[k].r;
        c[idx * 4 + 1] = pc[k].g;
        c[idx * 4 + 2] = pc[k].b;
        c[idx * 4 + 3] = pc[k].a;
        uv[idx * 2 + 0] = 0;
        uv[idx * 2 + 1] = 0;
        (*i)++;
    }
}

static float EnvelopeRadius(float t)
{
    /* t = 0 at the crown, t = 1 at the mouth. Fat teardrop, pinched at both ends. */
    float bell = sinf(powf(fmaxf(t, 0.0f), 0.70f) * (float)M_PI);
    float r = bell * ENVELOPE_RADIUS * (0.62f + 0.38f * sinf(t * (float)M_PI));
    /* Wide scoop rather than a tight nozzle. The passenger looks straight up
     * this opening, so it should read as one round mouth. */
    if (t > 0.86f) {
        float u = (t - 0.86f) / 0.14f;
        u = u * u * (3.0f - 2.0f * u);
        r = Lerpf(r, ENVELOPE_RADIUS * 0.42f, u);
    }
    if (t < 0.06f) r = Lerpf(0.9f, r, t / 0.06f);
    return r;
}

static Vector3 EnvelopePoint(float t, float a)
{
    float r = EnvelopeRadius(t);
    float y = Lerpf(ENVELOPE_TOP_Y, ENVELOPE_MOUTH_Y, t);
    return (Vector3){ cosf(a) * r, y, sinf(a) * r };
}

/* Outward normal shared by every gore that meets this vertex, so the
 * cloth stays one surface instead of a stack of flat panels. */
static Vector3 EnvelopeNormal(float t, float a)
{
    const float e = 0.01f;
    float t0 = t - e;
    float t1 = t + e;
    if (t0 < 0.0f) t0 = 0.0f;
    if (t1 > 1.0f) t1 = 1.0f;
    float r0 = EnvelopeRadius(t0);
    float r1 = EnvelopeRadius(t1);
    float y0 = Lerpf(ENVELOPE_TOP_Y, ENVELOPE_MOUTH_Y, t0);
    float y1 = Lerpf(ENVELOPE_TOP_Y, ENVELOPE_MOUTH_Y, t1);
    float dr = r1 - r0;
    float dy = y1 - y0;
    float nr = -dy;
    float ny = dr;
    float len = sqrtf(nr * nr + ny * ny);
    if (len < 1.0e-5f) {
        nr = 1.0f;
        ny = 0.0f;
    } else {
        nr /= len;
        ny /= len;
    }
    return (Vector3){ nr * cosf(a), ny, nr * sinf(a) };
}

static Mesh MakeEnvelope(void)
{
    const int stacks = 12;
    const int slices = 16;
    int verts = stacks * slices * 6;
    float* v = (float*)MemAlloc((unsigned int)verts * 3 * sizeof(float));
    float* n = (float*)MemAlloc((unsigned int)verts * 3 * sizeof(float));
    unsigned char* c = (unsigned char*)MemAlloc((unsigned int)verts * 4);
    float* uv = (float*)MemAlloc((unsigned int)verts * 2 * sizeof(float));
    int i = 0;
    for (int s = 0; s < stacks; s++) {
        float t0 = (float)s / (float)stacks;
        float t1 = (float)(s + 1) / (float)stacks;
        for (int k = 0; k < slices; k++) {
            float a0 = (float)k / (float)slices * 2.0f * (float)M_PI;
            float a1 = (float)(k + 1) / (float)slices * 2.0f * (float)M_PI;
            Vector3 p00 = EnvelopePoint(t0, a0);
            Vector3 p10 = EnvelopePoint(t0, a1);
            Vector3 p11 = EnvelopePoint(t1, a1);
            Vector3 p01 = EnvelopePoint(t1, a0);
            Vector3 n00 = EnvelopeNormal(t0, a0);
            Vector3 n10 = EnvelopeNormal(t0, a1);
            Vector3 n11 = EnvelopeNormal(t1, a1);
            Vector3 n01 = EnvelopeNormal(t1, a0);
            /* Lower third is the plain skirt. Above it, four gores of four
             * slices each: cream, red, cream, red. */
            int skirt = (t0 >= 0.68f);
            Color col = skirt ? SkirtColor() : GoreColor(k);
            PushTriN(v, n, c, uv, &i, p00, p10, p11, n00, n10, n11, col, col, col);
            PushTriN(v, n, c, uv, &i, p00, p11, p01, n00, n11, n01, col, col, col);
        }
    }
    return MakeFlatMesh(i, v, n, c, uv);
}

static Mesh MakeCrown(void)
{
    const int slices = 12;
    float r = 1.05f;
    float y = 0.0f;
    int verts = slices * 3;
    float* v = (float*)MemAlloc((unsigned int)verts * 3 * sizeof(float));
    float* n = (float*)MemAlloc((unsigned int)verts * 3 * sizeof(float));
    unsigned char* c = (unsigned char*)MemAlloc((unsigned int)verts * 4);
    float* uv = (float*)MemAlloc((unsigned int)verts * 2 * sizeof(float));
    int i = 0;
    Color col = { 214, 218, 210, 255 };
    Vector3 center = { 0, y, 0 };
    for (int k = 0; k < slices; k++) {
        float a0 = (float)k / (float)slices * 2.0f * (float)M_PI;
        float a1 = (float)(k + 1) / (float)slices * 2.0f * (float)M_PI;
        Vector3 p0 = { cosf(a0) * r, y, sinf(a0) * r };
        Vector3 p1 = { cosf(a1) * r, y, sinf(a1) * r };
        PushTri(v, n, c, uv, &i, center, p1, p0, col);
    }
    return MakeFlatMesh(i, v, n, c, uv);
}

static Mesh MakeSky(void)
{
    const int stacks = 28;
    const int slices = 64;
    float radius = 8000.0f;
    int verts = stacks * slices * 6;
    float* v = (float*)MemAlloc((unsigned int)verts * 3 * sizeof(float));
    float* n = (float*)MemAlloc((unsigned int)verts * 3 * sizeof(float));
    unsigned char* c = (unsigned char*)MemAlloc((unsigned int)verts * 4);
    float* uv = (float*)MemAlloc((unsigned int)verts * 2 * sizeof(float));
    int i = 0;
    Color zenith = { 86, 168, 220, 255 };
    Color high = { 140, 198, 230, 255 };
    Color horizon = { 232, 210, 180, 255 };
    for (int s = 0; s < stacks; s++) {
        float t0 = (float)s / (float)stacks;
        float t1 = (float)(s + 1) / (float)stacks;
        /* 0 at the zenith, past 1 is below the horizon so a gap reads as haze
         * rather than the blue clear colour. */
        float p0 = t0 * 1.28f;
        float p1 = t1 * 1.28f;
        float y0 = cosf(p0 * (float)M_PI * 0.5f) * radius;
        float y1 = cosf(p1 * (float)M_PI * 0.5f) * radius;
        float r0 = sinf(p0 * (float)M_PI * 0.5f) * radius;
        float r1 = sinf(p1 * (float)M_PI * 0.5f) * radius;
        Color c0 = t0 < 0.35f ? LerpColor(zenith, high, t0 / 0.35f)
                              : LerpColor(high, horizon, (t0 - 0.35f) / 0.65f);
        Color c1 = t1 < 0.35f ? LerpColor(zenith, high, t1 / 0.35f)
                              : LerpColor(high, horizon, (t1 - 0.35f) / 0.65f);
        for (int k = 0; k < slices; k++) {
            float a0 = (float)k / (float)slices * 2.0f * (float)M_PI;
            float a1 = (float)(k + 1) / (float)slices * 2.0f * (float)M_PI;
            Vector3 a = { cosf(a0) * r0, y0, sinf(a0) * r0 };
            Vector3 b = { cosf(a1) * r0, y0, sinf(a1) * r0 };
            Vector3 d = { cosf(a1) * r1, y1, sinf(a1) * r1 };
            Vector3 e = { cosf(a0) * r1, y1, sinf(a0) * r1 };
            Vector3 flat = { 0.0f, 1.0f, 0.0f };
            /* One normal everywhere, so the colour bands do not catch a
             * light and draw a circle. */
            PushTriN(v, n, c, uv, &i, a, e, d, flat, flat, flat, c0, c1, c1);
            PushTriN(v, n, c, uv, &i, a, d, b, flat, flat, flat, c0, c1, c0);
        }
    }
    return MakeFlatMesh(i, v, n, c, uv);
}

/* The burner used to be a two-second clip. It ended, faded, and started
 * again, so the roar cut every couple of seconds. This fills the device
 * from the audio thread, so a slow frame does not open a hole in it. */
static volatile float s_burnTarget = 0.0f;

static void BurnerFill(void* buffer, unsigned int frames)
{
    short* out = (short*)buffer;
    static unsigned int rng = 0x1234567u;
    static float phase = 0.0f;
    static float gain = 0.0f;
    float target = s_burnTarget;
    for (unsigned int i = 0; i < frames; i++) {
        gain += (target - gain) * 0.004f;
        if (gain < 0.0008f) {
            gain = 0.0f;
            out[i] = 0;
            continue;
        }
        rng = rng * 1664525u + 1013904223u;
        float n = (float)((int)(rng >> 16) % 2000 - 1000) / 1000.0f;
        phase += 48.0f / 22050.0f;
        if (phase >= 1.0f) phase -= 1.0f;
        float rumble = sinf(phase * 2.0f * (float)M_PI) * 0.35f
                     + sinf(phase * 4.0f * (float)M_PI) * 0.12f;
        float sample = (rumble + n * 0.28f) * gain * 0.55f;
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        out[i] = (short)(sample * 28000.0f);
    }
}

static void MakeBurnerSound(Scene* s)
{
    if (!IsAudioDeviceReady()) return;
    s->burner = LoadAudioStream(22050, 16, 1);
    if (s->burner.buffer == NULL) return;
    SetAudioStreamCallback(s->burner, BurnerFill);
    PlayAudioStream(s->burner);
    s->audioReady = 1;
}

/* World-space cover. One texel is the same colour on every ring.
 * The map is a torus: flying only repaints the strip that scrolled in,
 * a few rows a frame, so a rescale never stops the view. */
#define COVER_N 512
#define COVER_ROWS 4

static int CoverMod(int v)
{
    int m = v % COVER_N;
    return m < 0 ? m + COVER_N : m;
}

static void CoverStore(unsigned char* px, int i, int j, float x, float z)
{
    float city, farm, forest, highland;
    Terrain_Cover(x, z, &city, &farm, &forest, &highland);
    int k = (j * COVER_N + i) * 4;
    px[k + 0] = (unsigned char)(Clampf(city, 0.0f, 1.0f) * 255.0f + 0.5f);
    px[k + 1] = (unsigned char)(Clampf(farm, 0.0f, 1.0f) * 255.0f + 0.5f);
    px[k + 2] = (unsigned char)(Clampf(forest, 0.0f, 1.0f) * 255.0f + 0.5f);
    px[k + 3] = (unsigned char)(Clampf(highland, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static float CoverAxis(float base, float texel, int shift, int i)
{
    float span = texel * (float)COVER_N;
    float z = base + ((float)i + 0.5f) * texel;
    float win = base + (float)shift * texel;
    float k = ceilf((win - z) / span);
    return z + k * span;
}

static void CoverUploadColumn(Scene* s, int col)
{
    unsigned char line[COVER_N * 4];
    for (int j = 0; j < COVER_N; j++) {
        memcpy(line + j * 4, s->coverPx + (j * COVER_N + col) * 4, 4);
    }
    Rectangle rec = { (float)col, 0.0f, 1.0f, (float)COVER_N };
    UpdateTextureRec(s->coverTex, rec, line);
}

static void CoverUploadRow(Scene* s, int row)
{
    Rectangle rec = { 0.0f, (float)row, (float)COVER_N, 1.0f };
    UpdateTextureRec(s->coverTex, rec, s->coverPx + row * COVER_N * 4);
}

static void CoverStepX(Scene* s, int dir)
{
    float texel = s->coverTexel;
    int col;
    float wx;
    if (dir > 0) {
        col = CoverMod(s->coverShiftX);
        wx = s->coverOriginX + ((float)s->coverShiftX + (float)COVER_N + 0.5f) * texel;
    } else {
        col = CoverMod(s->coverShiftX - 1);
        wx = s->coverOriginX + ((float)s->coverShiftX - 1.0f + 0.5f) * texel;
    }
    for (int j = 0; j < COVER_N; j++) {
        float wz = CoverAxis(s->coverOriginZ, texel, s->coverShiftY, j);
        CoverStore(s->coverPx, col, j, wx, wz);
    }
    CoverUploadColumn(s, col);
    s->coverShiftX += dir;
}

static void CoverStepZ(Scene* s, int dir)
{
    float texel = s->coverTexel;
    int row;
    float wz;
    if (dir > 0) {
        row = CoverMod(s->coverShiftY);
        wz = s->coverOriginZ + ((float)s->coverShiftY + (float)COVER_N + 0.5f) * texel;
    } else {
        row = CoverMod(s->coverShiftY - 1);
        wz = s->coverOriginZ + ((float)s->coverShiftY - 1.0f + 0.5f) * texel;
    }
    for (int i = 0; i < COVER_N; i++) {
        float wx = CoverAxis(s->coverOriginX, texel, s->coverShiftX, i);
        CoverStore(s->coverPx, i, row, wx, wz);
    }
    CoverUploadRow(s, row);
    s->coverShiftY += dir;
}

static void CoverFillAll(Scene* s, float ox, float oz, float texel)
{
    for (int j = 0; j < COVER_N; j++) {
        float wz = oz + ((float)j + 0.5f) * texel;
        for (int i = 0; i < COVER_N; i++) {
            CoverStore(s->coverPx, i, j, ox + ((float)i + 0.5f) * texel, wz);
        }
    }
    UpdateTexture(s->coverTex, s->coverPx);
    s->coverOriginX = ox;
    s->coverOriginZ = oz;
    s->coverTexel = texel;
    s->coverShiftX = 0;
    s->coverShiftY = 0;
    s->coverJob = 0;
}

static void CoverStartJob(Scene* s, float ox, float oz, float texel)
{
    if (s->coverJob && s->jobTexel == texel && s->jobOriginX == ox && s->jobOriginZ == oz) return;
    s->coverJob = 1;
    s->jobTexel = texel;
    s->jobOriginX = ox;
    s->jobOriginZ = oz;
}

static void CoverAdvanceJob(Scene* s)
{
    if (!s->coverJob) return;
    int row0 = s->coverJob - 1;
    int row1 = row0 + COVER_ROWS;
    if (row1 > COVER_N) row1 = COVER_N;
    for (int j = row0; j < row1; j++) {
        float wz = s->jobOriginZ + ((float)j + 0.5f) * s->jobTexel;
        for (int i = 0; i < COVER_N; i++) {
            CoverStore(s->coverScratch, i, j, s->jobOriginX + ((float)i + 0.5f) * s->jobTexel, wz);
        }
    }
    if (row1 < COVER_N) {
        s->coverJob = row1 + 1;
        return;
    }
    unsigned char* tmp = s->coverPx;
    s->coverPx = s->coverScratch;
    s->coverScratch = tmp;
    UpdateTexture(s->coverTex, s->coverPx);
    s->coverOriginX = s->jobOriginX;
    s->coverOriginZ = s->jobOriginZ;
    s->coverTexel = s->jobTexel;
    s->coverShiftX = 0;
    s->coverShiftY = 0;
    s->coverJob = 0;
}

static void CoverUpdate(Scene* s, float x, float z)
{
    if (!s->coverReady) return;
    if (s->coverJob) {
        CoverAdvanceJob(s);
        return;
    }
    float need = Terrain_OuterRadius() * 2.4f;
    float texel = 16.0f;
    while (texel * (float)COVER_N < need && texel < 4096.0f) texel *= 2.0f;
    float span = texel * (float)COVER_N;
    float ox = floorf(x / texel) * texel - span * 0.5f;
    float oz = floorf(z / texel) * texel - span * 0.5f;
    if (s->coverTexel == 0.0f) {
        CoverFillAll(s, ox, oz, texel);
        return;
    }
    if (s->coverTexel != texel) {
        CoverStartJob(s, ox, oz, texel);
        CoverAdvanceJob(s);
        return;
    }
    int wantX = (int)floorf((ox - s->coverOriginX) / texel + 0.5f);
    int wantY = (int)floorf((oz - s->coverOriginZ) / texel + 0.5f);
    if (wantX - s->coverShiftX >= COVER_N || s->coverShiftX - wantX >= COVER_N ||
        wantY - s->coverShiftY >= COVER_N || s->coverShiftY - wantY >= COVER_N) {
        CoverStartJob(s, ox, oz, texel);
        CoverAdvanceJob(s);
        return;
    }
    int guard = COVER_ROWS;
    while (guard > 0 && s->coverShiftX != wantX) {
        CoverStepX(s, s->coverShiftX < wantX ? 1 : -1);
        guard--;
    }
    guard = COVER_ROWS;
    while (guard > 0 && s->coverShiftY != wantY) {
        CoverStepZ(s, s->coverShiftY < wantY ? 1 : -1);
        guard--;
    }
}

static void CoverInit(Scene* s)
{
    Image img = GenImageColor(COVER_N, COVER_N, (Color){ 0, 0, 0, 0 });
    s->coverTex = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureFilter(s->coverTex, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(s->coverTex, TEXTURE_WRAP_REPEAT);
    s->coverPx = (unsigned char*)MemAlloc(COVER_N * COVER_N * 4);
    s->coverScratch = (unsigned char*)MemAlloc(COVER_N * COVER_N * 4);
    s->coverTexel = 0.0f;
    s->coverJob = 0;
    s->coverReady = (s->coverTex.id > 0 && s->coverPx && s->coverScratch);
    if (!s->coverReady) return;
    SetMaterialTexture(&s->groundMat, MATERIAL_MAP_DIFFUSE, s->coverTex);
    CoverUpdate(s, s->balloon.x, s->balloon.z);
}

Scene* Scene_Create(void)
{
    Scene* s = (Scene*)calloc(1, sizeof(Scene));
    if (!s) return NULL;

    float x = 2400.0f;
    float z = 1800.0f;
    const char* sx = getenv("BALLOON_X");
    const char* sz = getenv("BALLOON_Z");
    if (sx) x = strtof(sx, NULL);
    if (sz) z = strtof(sz, NULL);
    float ground = Terrain_GroundAt(x, z);
    float agl = 90.0f;
    const char* sagl = getenv("BALLOON_AGL");
    if (sagl) agl = strtof(sagl, NULL);
    if (agl < 2.0f) agl = 2.0f;
    Balloon_Reset(&s->balloon, x, z, ground + agl);
    s->groundY = ground;
    Handles_Init(&s->handles);

    s->envelope = MakeEnvelope();
    s->crown = MakeCrown();
    s->sky = MakeSky();
    s->material = LoadMaterialDefault();
    s->groundMat = LoadMaterialDefault();
#ifdef PLATFORM_WEB
    s->groundMat.shader = LoadShaderFromMemory(GROUND_VS_100, GROUND_FS_100);
#else
    s->groundMat.shader = LoadShaderFromMemory(GROUND_VS_330, GROUND_FS_330);
#endif
    if (s->groundMat.shader.id == 0) {
        fprintf(stderr, "ground shader failed, using vertex colours\n");
        s->groundMat.shader = s->material.shader;
    }
    s->locEye = GetShaderLocation(s->groundMat.shader, "uEye");
    s->locWater = GetShaderLocation(s->groundMat.shader, "uWater");
    s->locCoverMap = GetShaderLocation(s->groundMat.shader, "uCoverMap");
    if (s->locWater >= 0) {
        float water = TERRAIN_WATER;
        SetShaderValue(s->groundMat.shader, s->locWater, &water, SHADER_UNIFORM_FLOAT);
    }
    if (s->locCoverMap >= 0) CoverInit(s);
    s->meshesReady = 1;

    Watch_Init(&s->watch);

    InitAudioDevice();
    MakeBurnerSound(s);
    return s;
}

void Scene_Destroy(Scene* s)
{
    if (!s) return;
    for (int i = 0; i < CLIP_LEVELS; i++) {
        if (s->clip[i].ready) UnloadMesh(s->clip[i].mesh);
        if (s->clip[i].pendingReady) UnloadMesh(s->clip[i].pending);
    }
    if (s->propMeshReady) UnloadMesh(s->propMesh);
    if (s->meshesReady) {
        UnloadMesh(s->envelope);
        UnloadMesh(s->crown);
        UnloadMesh(s->sky);
        UnloadMaterial(s->groundMat);
        UnloadMaterial(s->material);
        MemFree(s->coverPx);
        MemFree(s->coverScratch);
    }
    Watch_Shutdown(&s->watch);
    if (s->audioReady) {
        StopAudioStream(s->burner);
        UnloadAudioStream(s->burner);
    }
    if (IsAudioDeviceReady()) CloseAudioDevice();
    free(s);
}

const Balloon* Scene_Balloon(const Scene* s) { return &s->balloon; }
float Scene_Ground(const Scene* s) { return s->groundY; }

int Scene_TakeViewReset(Scene* s)
{
    int v = s->resetView;
    s->resetView = 0;
    return v;
}

static void SpawnParticle(Scene* s, int kind, Vector3 p, Vector3 v, float life)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (s->particles[i].life > 0.0f) continue;
        s->particles[i].x = p.x;
        s->particles[i].y = p.y;
        s->particles[i].z = p.z;
        s->particles[i].vx = v.x;
        s->particles[i].vy = v.y;
        s->particles[i].vz = v.z;
        s->particles[i].life = life;
        s->particles[i].maxLife = life;
        s->particles[i].kind = kind;
        return;
    }
}

static void UpdateParticles(Scene* s, float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle* p = &s->particles[i];
        if (p->life <= 0.0f) continue;
        p->life -= dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->z += p->vz * dt;
        if (p->kind == 0) p->vy += 2.5f * dt;
        else p->vy += 0.8f * dt;
    }
}

void Scene_Update(Scene* s, const HandInput hands[2], float dt, int keyBurner, int keyVent)
{
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    s->lastHands[0] = hands[0];
    s->lastHands[1] = hands[1];

    Handles_Update(&s->handles, hands, dt);

    /* Keyboard / mouse fallback pulls the cable directly, same threshold rules. */
    if (keyBurner && s->handles.h[HANDLE_BURNER].heldBy < 0) {
        s->handles.h[HANDLE_BURNER].pull = HANDLE_MAX_PULL * 0.85f;
        s->handles.h[HANDLE_BURNER].pullVel = 0.0f;
        s->handles.h[HANDLE_BURNER].active = 1;
    }
    if (keyVent && s->handles.h[HANDLE_VENT].heldBy < 0) {
        s->handles.h[HANDLE_VENT].pull = HANDLE_MAX_PULL * 0.85f;
        s->handles.h[HANDLE_VENT].pullVel = 0.0f;
        s->handles.h[HANDLE_VENT].active = 1;
    }

    s->balloon.burner = s->handles.h[HANDLE_BURNER].active ? 1.0f : 0.0f;
    s->balloon.vent = s->handles.h[HANDLE_VENT].active ? 1.0f : 0.0f;

    s->groundY = Terrain_GroundAt(s->balloon.x, s->balloon.z);
    Balloon_Update(&s->balloon, dt, s->groundY);
    s->simTime += dt;

    float targetFlame = s->balloon.burner;
    float targetVent = s->balloon.vent;
    s->flame += (targetFlame - s->flame) * Minf(1.0f, dt * 8.0f);
    s->ventOpen += (targetVent - s->ventOpen) * Minf(1.0f, dt * 4.0f);

    /* dt == 0 is a pause: keep the particles that are already in the air. */
    if (dt > 0.0f && s->flame > 0.15f) {
        /* Jets sit on top of the burner coils, just under the envelope mouth. */
        float ang = (float)(rand() % 628) / 100.0f;
        float rad = 0.03f + (float)(rand() % 40) / 1000.0f;
        float side = (rand() & 1) ? -0.055f : 0.055f;
        Vector3 p = { side + cosf(ang) * rad, 2.68f, sinf(ang) * rad };
        Vector3 v = { (float)(rand() % 10 - 5) / 50.0f, 3.0f + s->flame * 2.4f, (float)(rand() % 10 - 5) / 50.0f };
        SpawnParticle(s, 0, p, v, 0.45f + s->flame * 0.25f);
    }
    if (s->ventOpen > 0.2f && dt > 0.0f) {
        Vector3 p = { (float)(rand() % 20 - 10) / 80.0f, ENVELOPE_TOP_Y + s->ventOpen * 0.4f, (float)(rand() % 20 - 10) / 80.0f };
        Vector3 v = { (float)(rand() % 10 - 5) / 30.0f, 2.5f + s->ventOpen * 3.0f, (float)(rand() % 10 - 5) / 30.0f };
        SpawnParticle(s, 1, p, v, 1.1f);
    }
    UpdateParticles(s, dt);

    if (dt <= 0.0f) s_burnTarget = 0.0f;
    else if (s->audioReady) s_burnTarget = (s->flame > 0.05f) ? Clampf(s->flame, 0.0f, 1.0f) * 0.8f : 0.0f;

    int force = -1;
    const char* page = getenv("BALLOON_WATCH_PAGE");
    if (page && page[0] >= '0' && page[0] <= '2' && page[1] == '\0') force = page[0] - '0';
    int delta = 0;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_V)) delta = 1;
    else if (IsKeyPressed(KEY_LEFT)) delta = -1;
    int keyConfirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    int trigger = hands[0].trigger || keyConfirm;
    int watchOn = Watch_IsVisible() || keyConfirm;
    Watch_UpdatePage(&s->watch, hands[0].stickX, trigger, watchOn, delta, force);
    if (Watch_TakeReset(&s->watch)) s->resetView = 1;
    Watch_Render(&s->watch, &s->balloon, s->groundY);
}

static int SameJob(const ClipJob* a, const ClipJob* b)
{
    return fabsf(a->cx - b->cx) < 0.05f && fabsf(a->cz - b->cz) < 0.05f
        && fabsf(a->radius - b->radius) < 0.05f
        && fabsf(a->holeL - b->holeL) < 0.05f && fabsf(a->holeR - b->holeR) < 0.05f
        && fabsf(a->holeB - b->holeB) < 0.05f && fabsf(a->holeT - b->holeT) < 0.05f
        && fabsf(a->boundL - b->boundL) < 0.05f && fabsf(a->boundR - b->boundR) < 0.05f
        && fabsf(a->boundB - b->boundB) < 0.05f && fabsf(a->boundT - b->boundT) < 0.05f
        && a->hasHole == b->hasHole && a->morph == b->morph;
}

static void RebuildPropMesh(Scene* s);

static void UpdateClip(Scene* s)
{
    float agl = s->balloon.altitude - s->groundY;
    Terrain_ApplyAltitude(agl);
    float x = s->balloon.x;
    float z = s->balloon.z;
    int rings = Terrain_RingCount();

    for (int level = rings; level < CLIP_LEVELS; level++) {
        ClipSlot* slot = &s->clip[level];
        if (slot->ready) {
            UnloadMesh(slot->mesh);
            slot->ready = 0;
        }
        if (slot->pendingReady) {
            UnloadMesh(slot->pending);
            slot->pendingReady = 0;
        }
    }

    int primed = 0;
    int dirty = 0;
    for (int level = 0; level < rings; level++) {
        ClipJob job;
        Clip_Describe(level, x, z, &job);
        if (s->clip[level].ready) primed = 1;
        if (!s->clip[level].ready || !SameJob(&s->clip[level].job, &job)) dirty = 1;
        if (!s->clipSnap) s->clipTarget[level] = job;
    }

    /* The first fill is shown at once. Later, rings are built one per
     * frame and swapped together: showing a new ring next to an old one
     * draws a bright seam that races out to the horizon. */
    if (!primed) {
        for (int level = 0; level < rings; level++) {
            ClipSlot* slot = &s->clip[level];
            Mesh mesh;
            if (!Clip_Build(&s->clipTarget[level], &mesh)) continue;
            slot->mesh = mesh;
            slot->job = s->clipTarget[level];
            slot->ready = 1;
        }
        s->clipSnap = 0;
    } else if (dirty || s->clipSnap) {
        if (!s->clipSnap) s->clipSnap = 1;
        int pending = 0;
        int built = 0;
        for (int level = 0; level < rings; level++) {
            ClipSlot* slot = &s->clip[level];
            if (slot->pendingReady) {
                pending++;
                continue;
            }
            if (built) continue;
            Mesh mesh;
            if (!Clip_Build(&s->clipTarget[level], &mesh)) continue;
            slot->pending = mesh;
            slot->pendingReady = 1;
            built = 1;
            pending++;
        }
        if (pending >= rings) {
            for (int level = 0; level < rings; level++) {
                ClipSlot* slot = &s->clip[level];
                if (slot->ready) UnloadMesh(slot->mesh);
                slot->mesh = slot->pending;
                slot->job = s->clipTarget[level];
                slot->ready = 1;
                slot->pendingReady = 0;
            }
            s->clipSnap = 0;
        }
    }

    float snap = 32.0f;
    float px = floorf(x / snap) * snap;
    float pz = floorf(z / snap) * snap;
    if (!s->propsReady || fabsf(px - s->propX) > 0.5f || fabsf(pz - s->propZ) > 0.5f) {
        s->propCount = Clip_GatherProps(x, z, s->props, MAX_PROPS);
        s->propX = px;
        s->propZ = pz;
        s->propsReady = 1;
        RebuildPropMesh(s);
    }
}

static Vector3 TubePerp(Vector3 d)
{
    Vector3 a = (fabsf(d.y) < 0.85f) ? (Vector3){ 0.0f, 1.0f, 0.0f } : (Vector3){ 1.0f, 0.0f, 0.0f };
    return Vector3Normalize(Vector3CrossProduct(d, a));
}

/* One tube through every point. Separate cylinders leave a disc, and a
 * lighting break, at each joint. */
static void DrawTube(const Vector3* pts, int count, float radius, int sides, Color color, int closed)
{
    if (count < 2 || sides < 3 || radius <= 0.0f) return;
    if (count > 40) count = 40;

    Vector3 tangent[40];
    Vector3 side[40];
    Vector3 up[40];
    for (int i = 0; i < count; i++) {
        Vector3 d;
        if (!closed && i == 0) d = Vector3Subtract(pts[1], pts[0]);
        else if (!closed && i == count - 1) d = Vector3Subtract(pts[i], pts[i - 1]);
        else {
            int ip = (i - 1 + count) % count;
            int in = (i + 1) % count;
            Vector3 a = Vector3Subtract(pts[i], pts[ip]);
            Vector3 b = Vector3Subtract(pts[in], pts[i]);
            if (Vector3Length(a) > 1.0e-5f) a = Vector3Normalize(a);
            if (Vector3Length(b) > 1.0e-5f) b = Vector3Normalize(b);
            d = Vector3Add(a, b);
        }
        tangent[i] = (Vector3Length(d) < 1.0e-5f) ? (Vector3){ 0.0f, 1.0f, 0.0f } : Vector3Normalize(d);
    }

    side[0] = TubePerp(tangent[0]);
    up[0] = Vector3Normalize(Vector3CrossProduct(tangent[0], side[0]));
    for (int i = 1; i < count; i++) {
        Vector3 s = Vector3Subtract(side[i - 1], Vector3Scale(tangent[i], Vector3DotProduct(side[i - 1], tangent[i])));
        if (Vector3Length(s) < 1.0e-5f) s = TubePerp(tangent[i]);
        else s = Vector3Normalize(s);
        side[i] = s;
        up[i] = Vector3Normalize(Vector3CrossProduct(tangent[i], side[i]));
    }

    rlBegin(RL_TRIANGLES);
    rlColor4ub(color.r, color.g, color.b, color.a);
    int segments = closed ? count : count - 1;
    for (int i = 0; i < segments; i++) {
        int j = (i + 1) % count;
        for (int s = 0; s < sides; s++) {
            int s1 = (s + 1) % sides;
            float a0 = (float)s / (float)sides * 2.0f * (float)M_PI;
            float a1 = (float)s1 / (float)sides * 2.0f * (float)M_PI;
            Vector3 r0 = Vector3Add(Vector3Scale(side[i], cosf(a0) * radius), Vector3Scale(up[i], sinf(a0) * radius));
            Vector3 r1 = Vector3Add(Vector3Scale(side[i], cosf(a1) * radius), Vector3Scale(up[i], sinf(a1) * radius));
            Vector3 q0 = Vector3Add(Vector3Scale(side[j], cosf(a0) * radius), Vector3Scale(up[j], sinf(a0) * radius));
            Vector3 q1 = Vector3Add(Vector3Scale(side[j], cosf(a1) * radius), Vector3Scale(up[j], sinf(a1) * radius));
            Vector3 p0 = Vector3Add(pts[i], r0);
            Vector3 p1 = Vector3Add(pts[i], r1);
            Vector3 p2 = Vector3Add(pts[j], q0);
            Vector3 p3 = Vector3Add(pts[j], q1);
            Vector3 n0 = Vector3Scale(r0, 1.0f / radius);
            Vector3 n1 = Vector3Scale(r1, 1.0f / radius);
            Vector3 n2 = Vector3Scale(q0, 1.0f / radius);
            Vector3 n3 = Vector3Scale(q1, 1.0f / radius);
            rlNormal3f(n0.x, n0.y, n0.z); rlVertex3f(p0.x, p0.y, p0.z);
            rlNormal3f(n1.x, n1.y, n1.z); rlVertex3f(p1.x, p1.y, p1.z);
            rlNormal3f(n3.x, n3.y, n3.z); rlVertex3f(p3.x, p3.y, p3.z);
            rlNormal3f(n2.x, n2.y, n2.z); rlVertex3f(p2.x, p2.y, p2.z);
            rlNormal3f(n0.x, n0.y, n0.z); rlVertex3f(p0.x, p0.y, p0.z);
            rlNormal3f(n3.x, n3.y, n3.z); rlVertex3f(p3.x, p3.y, p3.z);
        }
    }
    if (!closed) {
        int last = count - 1;
        for (int s = 0; s < sides; s++) {
            int s1 = (s + 1) % sides;
            float a0 = (float)s / (float)sides * 2.0f * (float)M_PI;
            float a1 = (float)s1 / (float)sides * 2.0f * (float)M_PI;
            Vector3 p0 = Vector3Add(pts[0], Vector3Add(Vector3Scale(side[0], cosf(a0) * radius), Vector3Scale(up[0], sinf(a0) * radius)));
            Vector3 p1 = Vector3Add(pts[0], Vector3Add(Vector3Scale(side[0], cosf(a1) * radius), Vector3Scale(up[0], sinf(a1) * radius)));
            rlNormal3f(-tangent[0].x, -tangent[0].y, -tangent[0].z);
            rlVertex3f(pts[0].x, pts[0].y, pts[0].z);
            rlVertex3f(p1.x, p1.y, p1.z);
            rlVertex3f(p0.x, p0.y, p0.z);
            Vector3 q0 = Vector3Add(pts[last], Vector3Add(Vector3Scale(side[last], cosf(a0) * radius), Vector3Scale(up[last], sinf(a0) * radius)));
            Vector3 q1 = Vector3Add(pts[last], Vector3Add(Vector3Scale(side[last], cosf(a1) * radius), Vector3Scale(up[last], sinf(a1) * radius)));
            rlNormal3f(tangent[last].x, tangent[last].y, tangent[last].z);
            rlVertex3f(pts[last].x, pts[last].y, pts[last].z);
            rlVertex3f(q0.x, q0.y, q0.z);
            rlVertex3f(q1.x, q1.y, q1.z);
        }
    }
    rlEnd();
}

static void DrawRope(Vector3 a, Vector3 b, float sag, float radius, Color color)
{
    const int n = 7;
    Vector3 pts[8];
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        pts[i] = (Vector3){
            Lerpf(a.x, b.x, t),
            Lerpf(a.y, b.y, t) - sinf(t * (float)M_PI) * sag,
            Lerpf(a.z, b.z, t)
        };
    }
    DrawTube(pts, n + 1, radius, 5, color, 0);
}

/* Axis-aligned spar. The two endpoints must differ on one axis only. */
static void DrawBar(Vector3 a, Vector3 b, float thick, Color color)
{
    Vector3 mid = {
        (a.x + b.x) * 0.5f,
        (a.y + b.y) * 0.5f,
        (a.z + b.z) * 0.5f
    };
    float sx = fabsf(b.x - a.x);
    float sy = fabsf(b.y - a.y);
    float sz = fabsf(b.z - a.z);
    if (sx < thick) sx = thick;
    if (sy < thick) sy = thick;
    if (sz < thick) sz = thick;
    DrawCube(mid, sx, sy, sz, color);
}

/* Open lattice just above the wicker. Short on purpose: eyes at ~1.62 m
 * look over the handrail, the lattice only frames the view. */
static void DrawGuardRail(void)
{
    Color iron = { 68, 74, 82, 255 };
    Color ironDark = { 44, 48, 54, 255 };
    Color leather = { 122, 70, 46, 255 };
    Color brass = { 198, 156, 74, 255 };

    const float yPlant = BASKET_RIM_Y - 0.02f;
    const float yFoot = BASKET_RIM_Y + 0.07f;
    const float yHand = BASKET_RIM_Y + 0.34f;
    const float yMid = (yFoot + yHand) * 0.5f;
    const float ix = BASKET_HALF_X - 0.10f;
    const float iz = BASKET_HALF_Z - 0.08f;

    const Vector3 corner[4] = {
        { -ix, 0, -iz },
        {  ix, 0, -iz },
        {  ix, 0,  iz },
        { -ix, 0,  iz }
    };

    for (int s = 0; s < 4; s++) {
        Vector3 a = corner[s];
        Vector3 b = corner[(s + 1) & 3];
        /* Long sides (along X) get one more bay than the short sides. */
        int bays = (s & 1) ? 3 : 4;
        Vector3 dir = { b.x - a.x, 0.0f, b.z - a.z };
        float len = sqrtf(dir.x * dir.x + dir.z * dir.z);
        if (len < 1.0e-4f) continue;
        dir.x /= len;
        dir.z /= len;
        /* Rails stop on the post face. Corner posts are drawn by the
         * previous side, so each joint is a single cube. */
        const float inset = 0.016f;
        Vector3 ra = { a.x + dir.x * inset, 0, a.z + dir.z * inset };
        Vector3 rb = { b.x - dir.x * inset, 0, b.z - dir.z * inset };

        for (int i = 1; i <= bays; i++) {
            float t = (float)i / (float)bays;
            float x = Lerpf(a.x, b.x, t);
            float z = Lerpf(a.z, b.z, t);
            int end = (i == bays);
            float thick = end ? 0.032f : 0.018f;
            DrawCube((Vector3){ x, (yPlant + yHand) * 0.5f, z }, thick, yHand - yPlant, thick,
                     end ? ironDark : iron);
            if (end) DrawSphere((Vector3){ x, yHand + 0.014f, z }, 0.016f, brass);
        }

        DrawBar((Vector3){ ra.x, yHand, ra.z }, (Vector3){ rb.x, yHand, rb.z }, 0.028f, leather);
        DrawBar((Vector3){ ra.x, yMid, ra.z }, (Vector3){ rb.x, yMid, rb.z }, 0.014f, iron);
        DrawBar((Vector3){ ra.x, yFoot, ra.z }, (Vector3){ rb.x, yFoot, rb.z }, 0.014f, ironDark);

        for (int i = 0; i < bays; i++) {
            float t0 = (float)i / (float)bays;
            float t1 = (float)(i + 1) / (float)bays;
            Vector3 p0 = { Lerpf(a.x, b.x, t0), 0, Lerpf(a.z, b.z, t0) };
            Vector3 p1 = { Lerpf(a.x, b.x, t1), 0, Lerpf(a.z, b.z, t1) };
            int flip = (i + s) & 1;
            DrawCylinderEx((Vector3){ p0.x, flip ? yFoot : yMid, p0.z },
                           (Vector3){ p1.x, flip ? yMid : yFoot, p1.z },
                           0.007f, 0.007f, 4, iron);
            DrawCylinderEx((Vector3){ p0.x, flip ? yMid : yHand, p0.z },
                           (Vector3){ p1.x, flip ? yHand : yMid, p1.z },
                           0.007f, 0.007f, 4, iron);
        }
    }
}

/* Where the red cable ends: a blast-valve lever on the front of the burner.
 * Pulling the ceiling handle drags this lever down. */
static Vector3 BurnerValveTip(float pull01)
{
    Vector3 pivot = { -0.05f, 2.28f, -0.20f };
    float ang = Lerpf(0.55f, 2.15f, Clampf(pull01, 0.0f, 1.0f));
    return (Vector3){
        pivot.x - 0.04f,
        pivot.y + cosf(ang) * 0.16f,
        pivot.z - sinf(ang) * 0.16f
    };
}

static void DrawCoil(float cx, float cz, float y0, float y1, float radius, int turns, Color color)
{
    const int segPerTurn = 8;
    int n = turns * segPerTurn;
    Vector3 pts[40];
    if (n + 1 > 40) n = 39;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        float a = t * (float)turns * 2.0f * (float)M_PI;
        pts[i] = (Vector3){
            cx + cosf(a) * radius,
            Lerpf(y0, y1, t),
            cz + sinf(a) * radius
        };
    }
    DrawTube(pts, n + 1, 0.016f, 6, color, 0);
}

/* One tongue of the burner flame. `tip` already includes flicker-free aim;
 * this only wobbles it so neighbouring jets do not fuse into a prism. */
static void DrawFlameJet(Vector3 base, Vector3 tip, float width, float time, int seed)
{
    float phase = time * (7.2f + (float)(seed % 5) * 0.4f) + (float)seed * 1.7f;
    float flick = 0.78f + 0.22f * sinf(phase);
    Vector3 aimed = {
        Lerpf(base.x, tip.x, flick) + 0.025f * sinf(phase * 1.6f),
        Lerpf(base.y, tip.y, flick),
        Lerpf(base.z, tip.z, flick) + 0.025f * cosf(phase * 1.3f)
    };
    Vector3 mid = {
        Lerpf(base.x, aimed.x, 0.40f),
        Lerpf(base.y, aimed.y, 0.38f),
        Lerpf(base.z, aimed.z, 0.40f)
    };
    float w = width * (0.88f + 0.12f * sinf(phase * 2.1f));
    Color root = { 180, 40, 16, 200 };
    Color outer = { 255, 78, 18, 115 };
    Color body = { 255, 156, 32, 175 };
    Color core = { 255, 236, 190, 210 };
    DrawCylinderEx(base, mid, w * 0.20f, w * 0.55f, 5, root);
    DrawCylinderEx(base, mid, w * 0.10f, w * 0.95f, 5, outer);
    DrawCylinderEx(mid, aimed, w * 0.70f, w * 0.04f, 4, body);
    DrawCylinderEx(base, (Vector3){
                       Lerpf(base.x, aimed.x, 0.62f),
                       Lerpf(base.y, aimed.y, 0.62f),
                       Lerpf(base.z, aimed.z, 0.62f)
                   }, w * 0.04f, w * 0.16f, 4, core);
}

/* Double burner hung in the open centre of the load frame. Coils, skirt and
 * blast valve stay put; the flame is only drawn while the red handle is down. */
static void DrawBurner(const Scene* s)
{
    Color steel = { 176, 182, 190, 255 };
    Color steelDark = { 52, 56, 62, 255 };
    Color skirtCol = { 72, 78, 86, 255 };
    Color copper = { 176, 102, 54, 255 };
    Color hose = { 26, 28, 32, 255 };
    Color brass = { 198, 156, 74, 255 };
    Color valveRed = { 196, 46, 48, 255 };

    const float half = 0.17f;
    const float yb = 2.36f;
    const float yt = 2.74f;
    const float ys = 2.46f;
    const float hx = BASKET_HALF_X;
    const float hz = BASKET_HALF_Z;

    if (s->flame > 0.05f) {
        skirtCol.r = (unsigned char)Lerpf((float)skirtCol.r, 168.0f, s->flame);
        skirtCol.g = (unsigned char)Lerpf((float)skirtCol.g, 86.0f, s->flame);
        skirtCol.b = (unsigned char)Lerpf((float)skirtCol.b, 48.0f, s->flame);
    }

    Vector3 mount[4] = {
        { -hx + 0.10f, FRAME_Y + 0.03f, -hz + 0.06f },
        {  hx - 0.10f, FRAME_Y + 0.03f, -hz + 0.06f },
        {  hx - 0.10f, FRAME_Y + 0.03f,  hz - 0.06f },
        { -hx + 0.10f, FRAME_Y + 0.03f,  hz - 0.06f }
    };
    Vector3 foot[4] = {
        { -half, yb, -half },
        {  half, yb, -half },
        {  half, yb,  half },
        { -half, yb,  half }
    };
    for (int i = 0; i < 4; i++) DrawCylinderEx(mount[i], foot[i], 0.016f, 0.012f, 5, steel);

    for (int i = 0; i < 4; i++) {
        DrawCube((Vector3){ foot[i].x, (yb + yt) * 0.5f, foot[i].z }, 0.026f, yt - yb, 0.026f, steel);
    }

    const Vector3 rim[4] = {
        { -half, 0, -half }, { half, 0, -half }, { half, 0, half }, { -half, 0, half }
    };
    const float lip = half + 0.03f;
    const Vector3 lipC[4] = {
        { -lip, 0, -lip }, { lip, 0, -lip }, { lip, 0, lip }, { -lip, 0, lip }
    };
    for (int i = 0; i < 4; i++) {
        int n = (i + 1) & 3;
        DrawBar((Vector3){ rim[i].x, yb, rim[i].z }, (Vector3){ rim[n].x, yb, rim[n].z }, 0.02f, steelDark);
        DrawBar((Vector3){ lipC[i].x, yt, lipC[i].z }, (Vector3){ lipC[n].x, yt, lipC[n].z }, 0.022f, steel);
    }

    /* Lower skirt is closed. The upper half stays open so the coils read. */
    float skh = ys - yb;
    float sky = (yb + ys) * 0.5f;
    float span = half * 2.0f - 0.04f;
    DrawCube((Vector3){ 0, sky, -half + 0.008f }, span, skh, 0.012f, skirtCol);
    DrawCube((Vector3){ 0, sky,  half - 0.008f }, span, skh, 0.012f, skirtCol);
    DrawCube((Vector3){ -half + 0.008f, sky, 0 }, 0.012f, skh, span, skirtCol);
    DrawCube((Vector3){  half - 0.008f, sky, 0 }, 0.012f, skh, span, skirtCol);

    DrawCylinder((Vector3){ 0.0f, 2.18f, 0.0f }, 0.05f, 0.07f, 0.16f, 8, steelDark);
    DrawCylinder((Vector3){ 0.0f, 2.32f, 0.0f }, 0.09f, 0.05f, 0.04f, 8, steel);
    DrawCylinderEx((Vector3){ -0.16f, 2.34f, 0 }, (Vector3){ 0.16f, 2.34f, 0 }, 0.016f, 0.016f, 6, steelDark);

    float pull = s->handles.h[HANDLE_BURNER].pull / HANDLE_MAX_PULL;
    Vector3 pivot = { -0.05f, 2.28f, -0.20f };
    Vector3 tip = BurnerValveTip(pull);
    DrawSphere(pivot, 0.022f, brass);
    DrawCylinderEx(pivot, tip, 0.013f, 0.008f, 5, valveRed);
    DrawSphere(tip, 0.016f, valveRed);

    DrawRope((Vector3){ -0.16f, 2.34f, 0.0f }, (Vector3){ -hx + 0.06f, FRAME_Y, -0.06f }, 0.12f, 0.010f, hose);
    DrawRope((Vector3){  0.16f, 2.34f, 0.0f }, (Vector3){  hx - 0.06f, FRAME_Y, -0.06f }, 0.12f, 0.010f, hose);

    Color coil = copper;
    if (s->flame > 0.15f) {
        coil.r = (unsigned char)Lerpf((float)copper.r, 230.0f, s->flame);
        coil.g = (unsigned char)Lerpf((float)copper.g, 150.0f, s->flame);
        coil.b = (unsigned char)Lerpf((float)copper.b, 70.0f, s->flame);
    }
    DrawCoil(-0.055f, 0.0f, 2.44f, 2.64f, 0.046f, 3, coil);
    DrawCoil( 0.055f, 0.0f, 2.44f, 2.64f, 0.046f, 3, coil);

    Vector3 jets[8];
    for (int side = 0; side < 2; side++) {
        float cx = side ? 0.055f : -0.055f;
        for (int k = 0; k < 4; k++) {
            float a = ((float)k + 0.5f) / 4.0f * 2.0f * (float)M_PI;
            Vector3 base = { cx + cosf(a) * 0.028f, 2.64f, sinf(a) * 0.028f };
            DrawCylinder(base, 0.014f, 0.018f, 0.04f, 5, steelDark);
            jets[side * 4 + k] = (Vector3){ base.x, base.y + 0.04f, base.z };
        }
    }

    /* Pilot lights stay lit so the burner reads even with the handle up. */
    Color pilot = { 150, 206, 255, 230 };
    DrawCylinder(jets[0], 0.0f, 0.010f, 0.08f, 4, pilot);
    DrawCylinder(jets[4], 0.0f, 0.010f, 0.08f, 4, pilot);

    if (s->flame > 0.04f) {
        BeginBlendMode(BLEND_ALPHA);
        rlDisableDepthMask();
        rlDisableBackfaceCulling();
        float time = s->simTime;
        /* Two columns, one over each coil, fanned apart so they stay readable. */
        for (int i = 0; i < 8; i++) {
            float side = (i < 4) ? -1.0f : 1.0f;
            float vary = 0.72f + 0.28f * (float)((i * 3) % 5) / 4.0f;
            float h = (0.55f + s->flame * 1.05f) * vary;
            float spread = 0.05f + s->flame * 0.10f;
            float ang = ((float)(i % 4) + 0.5f) / 4.0f * 2.0f * (float)M_PI;
            Vector3 tip = {
                jets[i].x + cosf(ang) * spread + side * 0.04f * s->flame,
                jets[i].y + h,
                jets[i].z + sinf(ang) * spread
            };
            DrawFlameJet(jets[i], tip, 0.055f + s->flame * 0.03f, time, i);
        }
        rlEnableBackfaceCulling();
        rlEnableDepthMask();
        EndBlendMode();
    }
}

static void DrawBasket(const Scene* s)
{
    Color wicker = { 176, 124, 64, 255 };
    Color wickerDark = { 132, 86, 42, 255 };
    Color wood = { 92, 64, 40, 255 };
    Color rope = { 92, 74, 52, 255 };

    DrawCube((Vector3){ 0, 0.05f, 0 }, BASKET_HALF_X * 2.0f, 0.10f, BASKET_HALF_Z * 2.0f, (Color){ 120, 84, 48, 255 });

    const float hx = BASKET_HALF_X;
    const float hz = BASKET_HALF_Z;
    for (int row = 0; row < 7; row++) {
        float y = 0.16f + (float)row * 0.13f;
        Color c = (row & 1) ? wickerDark : wicker;
        float t = 0.035f;
        DrawCube((Vector3){ 0, y, -hz }, hx * 2.0f, 0.10f, t, c);
        DrawCube((Vector3){ 0, y, hz }, hx * 2.0f, 0.10f, t, c);
        DrawCube((Vector3){ -hx, y, 0 }, t, 0.10f, hz * 2.0f, c);
        DrawCube((Vector3){ hx, y, 0 }, t, 0.10f, hz * 2.0f, c);
    }
    /* Vertical stakes so the bands read as a weave, not stacked planks. */
    Color stake = { 108, 70, 36, 255 };
    float stakeH = BASKET_RIM_Y - 0.16f;
    float stakeY = 0.08f + stakeH * 0.5f;
    for (float x = -hx + 0.18f; x <= hx - 0.12f; x += 0.20f) {
        DrawCube((Vector3){ x, stakeY, -hz - 0.02f }, 0.026f, stakeH, 0.018f, stake);
        DrawCube((Vector3){ x, stakeY,  hz + 0.02f }, 0.026f, stakeH, 0.018f, stake);
    }
    for (float z = -hz + 0.18f; z <= hz - 0.12f; z += 0.22f) {
        DrawCube((Vector3){ -hx - 0.02f, stakeY, z }, 0.018f, stakeH, 0.026f, stake);
        DrawCube((Vector3){  hx + 0.02f, stakeY, z }, 0.018f, stakeH, 0.026f, stake);
    }
    /* Corner blocks so the four walls meet instead of leaving a notch. */
    DrawCube((Vector3){ -hx, stakeY, -hz }, 0.05f, stakeH, 0.05f, stake);
    DrawCube((Vector3){  hx, stakeY, -hz }, 0.05f, stakeH, 0.05f, stake);
    DrawCube((Vector3){  hx, stakeY,  hz }, 0.05f, stakeH, 0.05f, stake);
    DrawCube((Vector3){ -hx, stakeY,  hz }, 0.05f, stakeH, 0.05f, stake);
    DrawCube((Vector3){ 0, BASKET_RIM_Y, 0 }, hx * 2.0f + 0.06f, 0.06f, hz * 2.0f + 0.06f, (Color){ 196, 148, 78, 255 });
    DrawGuardRail();

    Vector3 posts[4] = {
        { -hx, 0, -hz }, { hx, 0, -hz }, { hx, 0, hz }, { -hx, 0, hz }
    };
    float postBase = 0.10f;
    float postTop = FRAME_Y - 0.025f;
    float postH = postTop - postBase;
    for (int i = 0; i < 4; i++) {
        DrawCube((Vector3){ posts[i].x, postBase + postH * 0.5f, posts[i].z }, 0.06f, postH, 0.06f, wood);
    }
    DrawCube((Vector3){ 0, FRAME_Y, -hz }, hx * 2.0f, 0.05f, 0.05f, wood);
    DrawCube((Vector3){ 0, FRAME_Y, hz }, hx * 2.0f, 0.05f, 0.05f, wood);
    DrawCube((Vector3){ -hx, FRAME_Y, 0 }, 0.05f, 0.05f, hz * 2.0f, wood);
    DrawCube((Vector3){ hx, FRAME_Y, 0 }, 0.05f, 0.05f, hz * 2.0f, wood);

    DrawBurner(s);

    /* Four flying wires, one per corner. They stay on the post's azimuth so
     * they drop beside the basket instead of crossing the view upward. */
    for (int i = 0; i < 4; i++) {
        float a = atan2f(posts[i].z, posts[i].x);
        float attachT = 1.0f;
        float y = Lerpf(ENVELOPE_TOP_Y, ENVELOPE_MOUTH_Y, attachT);
        float r = EnvelopeRadius(attachT);
        Vector3 top = { cosf(a) * r, y, sinf(a) * r };
        Vector3 bot = posts[i];
        bot.y = FRAME_Y;
        DrawRope(top, bot, 0.06f, 0.014f, rope);
    }
}

static void DrawEnvelopeHoop(float t, float tubeR, Color color)
{
    const int n = 24;
    Vector3 pts[24];
    float y = Lerpf(ENVELOPE_TOP_Y, ENVELOPE_MOUTH_Y, t);
    float r = EnvelopeRadius(t);
    for (int i = 0; i < n; i++) {
        float a = (float)i / (float)n * 2.0f * (float)M_PI;
        pts[i] = (Vector3){ cosf(a) * r, y, sinf(a) * r };
    }
    DrawTube(pts, n, tubeR, 5, color, 1);
}

static void DrawHandle(const HandleState* h, Color color, int ring)
{
    float x, y, z;
    Handle_GripPosition(h, &x, &y, &z);
    Vector3 grip = { x, y, z };
    Vector3 anchor = { h->anchorX, h->anchorY, h->anchorZ };
    DrawRope(anchor, grip, 0.0f, 0.01f, (Color){ 70, 74, 80, 255 });
    DrawSphere(anchor, 0.03f, (Color){ 90, 94, 100, 255 });

    if (!ring) {
        /* Red burner handle: a T-bar. */
        DrawCylinderEx((Vector3){ x, y + 0.06f, z }, grip, 0.012f, 0.012f, 6, color);
        DrawCylinderEx((Vector3){ x - 0.11f, y, z }, (Vector3){ x + 0.11f, y, z }, 0.022f, 0.022f, 8, color);
    } else {
        /* Green vent handle: a ring, so the two stay distinct without colour. */
        const int n = 12;
        Vector3 ring[12];
        for (int i = 0; i < n; i++) {
            float a = (float)i / (float)n * 2.0f * (float)M_PI;
            ring[i] = (Vector3){ x + cosf(a) * 0.09f, y + sinf(a) * 0.09f, z };
        }
        DrawTube(ring, n, 0.016f, 5, color, 1);
    }
}

static void DrawHands(const Scene* s)
{
    for (int i = 0; i < 2; i++) {
        const HandInput* hand = &s->lastHands[i];
        if (!hand->active) continue;
        Color skin = { 232, 186, 150, 255 };
        Color cuff = { 40, 48, 58, 255 };
        Vector3 p = { hand->x, hand->y, hand->z };
        DrawSphere(p, 0.038f, skin);
        Vector3 forward = Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, (Quaternion){ hand->qx, hand->qy, hand->qz, hand->qw });
        DrawSphere(Vector3Add(p, Vector3Scale(forward, 0.045f)), 0.026f, skin);
        DrawCube(Vector3Add(p, (Vector3){ 0, -0.03f, 0 }), 0.05f, 0.03f, 0.05f, cuff);
    }
}

static Color Melt(Color c, float fade)
{
    Color ground = { 96, 142, 82, 255 };
    if (fade < 0.0f) fade = 0.0f;
    if (fade > 1.0f) fade = 1.0f;
    Color o;
    o.r = (unsigned char)((float)ground.r + ((float)c.r - (float)ground.r) * fade);
    o.g = (unsigned char)((float)ground.g + ((float)c.g - (float)ground.g) * fade);
    o.b = (unsigned char)((float)ground.b + ((float)c.b - (float)ground.b) * fade);
    o.a = 255;
    return o;
}

typedef struct PropBuf {
    float* v;
    float* n;
    unsigned char* c;
    float* uv;
    int count;
    int cap;
} PropBuf;

static void PropNeed(PropBuf* b, int extra)
{
    if (b->count + extra <= b->cap) return;
    int cap = b->cap > 0 ? b->cap : 2048;
    while (cap < b->count + extra) cap *= 2;
    b->v = (float*)MemRealloc(b->v, (unsigned int)cap * 3 * sizeof(float));
    b->n = (float*)MemRealloc(b->n, (unsigned int)cap * 3 * sizeof(float));
    b->c = (unsigned char*)MemRealloc(b->c, (unsigned int)cap * 4);
    b->uv = (float*)MemRealloc(b->uv, (unsigned int)cap * 2 * sizeof(float));
    b->cap = cap;
}

static void PropVert(PropBuf* b, float x, float y, float z, float nx, float ny, float nz, Color col)
{
    PropNeed(b, 1);
    int i = b->count++;
    b->v[i * 3] = x;
    b->v[i * 3 + 1] = y;
    b->v[i * 3 + 2] = z;
    b->n[i * 3] = nx;
    b->n[i * 3 + 1] = ny;
    b->n[i * 3 + 2] = nz;
    b->c[i * 4] = col.r;
    b->c[i * 4 + 1] = col.g;
    b->c[i * 4 + 2] = col.b;
    b->c[i * 4 + 3] = col.a;
    b->uv[i * 2] = 0.0f;
    b->uv[i * 2 + 1] = 0.0f;
}

static void PropTri(PropBuf* b,
                    float x0, float y0, float z0,
                    float x1, float y1, float z1,
                    float x2, float y2, float z2,
                    Color col)
{
    float e1x = x1 - x0, e1y = y1 - y0, e1z = z1 - z0;
    float e2x = x2 - x0, e2y = y2 - y0, e2z = z2 - z0;
    float nx = e1y * e2z - e1z * e2y;
    float ny = e1z * e2x - e1x * e2z;
    float nz = e1x * e2y - e1y * e2x;
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len < 1.0e-6f) len = 1.0f;
    nx /= len; ny /= len; nz /= len;
    PropVert(b, x0, y0, z0, nx, ny, nz, col);
    PropVert(b, x1, y1, z1, nx, ny, nz, col);
    PropVert(b, x2, y2, z2, nx, ny, nz, col);
}

static void PropCube(PropBuf* b, float x, float y, float z, float w, float h, float d, Color col)
{
    float hx = w * 0.5f, hy = h * 0.5f, hz = d * 0.5f;
    float x0 = x - hx, x1 = x + hx, y0 = y - hy, y1 = y + hy, z0 = z - hz, z1 = z + hz;
    PropTri(b, x0, y0, z1, x1, y0, z1, x0, y1, z1, col);
    PropTri(b, x1, y1, z1, x0, y1, z1, x1, y0, z1, col);
    PropTri(b, x1, y0, z0, x0, y0, z0, x1, y1, z0, col);
    PropTri(b, x0, y1, z0, x1, y1, z0, x0, y0, z0, col);
    PropTri(b, x0, y1, z0, x0, y1, z1, x1, y1, z1, col);
    PropTri(b, x1, y1, z1, x1, y1, z0, x0, y1, z0, col);
    PropTri(b, x0, y0, z1, x0, y0, z0, x1, y0, z0, col);
    PropTri(b, x1, y0, z0, x1, y0, z1, x0, y0, z1, col);
    PropTri(b, x1, y0, z0, x1, y1, z0, x1, y1, z1, col);
    PropTri(b, x1, y1, z1, x1, y0, z1, x1, y0, z0, col);
    PropTri(b, x0, y0, z1, x0, y1, z1, x0, y1, z0, col);
    PropTri(b, x0, y1, z0, x0, y0, z0, x0, y0, z1, col);
}

static void PropCyl(PropBuf* b, float x, float y, float z, float r0, float r1, float h, int sides, Color col)
{
    if (sides < 3) sides = 3;
    float step = 2.0f * (float)M_PI / (float)sides;
    for (int i = 0; i < sides; i++) {
        float a0 = (float)i * step;
        float a1 = (float)(i + 1) * step;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        float xb0 = x + s0 * r0, zb0 = z + c0 * r0;
        float xb1 = x + s1 * r0, zb1 = z + c1 * r0;
        float xt1 = x + s1 * r1, zt1 = z + c1 * r1;
        float xt0 = x + s0 * r1, zt0 = z + c0 * r1;
        float y1 = y + h;
        PropTri(b, xb0, y, zb0, xb1, y, zb1, xt1, y1, zt1, col);
        PropTri(b, xt1, y1, zt1, xt0, y1, zt0, xb0, y, zb0, col);
        if (r1 > 0.0f) PropTri(b, x, y1, z, xt0, y1, zt0, xt1, y1, zt1, col);
        if (r0 > 0.0f) PropTri(b, x, y, z, xb1, y, zb1, xb0, y, zb0, col);
    }
}

static void RebuildPropMesh(Scene* s)
{
    if (s->propMeshReady) {
        UnloadMesh(s->propMesh);
        s->propMeshReady = 0;
    }
    if (s->propCount <= 0) return;
    PropBuf buf;
    memset(&buf, 0, sizeof(buf));
    for (int i = 0; i < s->propCount; i++) {
        const Prop* p = &s->props[i];
        float fade = p->fade;
        float sc = p->scale * (0.55f + 0.45f * fade);
        float x = p->x, y = p->y, z = p->z;
        if (p->kind == 0) {
            PropCyl(&buf, x, y, z, 0.10f * sc, 0.08f * sc, 0.7f * sc, 5, Melt((Color){ 92, 64, 40, 255 }, fade));
            PropCyl(&buf, x, y + 0.55f * sc, z, 0.55f * sc, 0.0f, 0.7f * sc, 5, Melt((Color){ 48, 130, 62, 255 }, fade));
        } else if (p->kind == 1) {
            PropCyl(&buf, x, y, z, 0.09f * sc, 0.07f * sc, 0.55f * sc, 5, Melt((Color){ 84, 60, 40, 255 }, fade));
            PropCyl(&buf, x, y + 0.45f * sc, z, 0.55f * sc, 0.02f * sc, 1.7f * sc, 5, Melt((Color){ 30, 90, 58, 255 }, fade));
        } else if (p->kind == 2) {
            PropCube(&buf, x, y + 0.25f * sc, z, 0.7f * sc, 0.45f * sc, 0.55f * sc, Melt((Color){ 128, 126, 120, 255 }, fade));
        } else if (p->kind == 3) {
            PropCube(&buf, x, y + 0.7f * sc, z, 1.6f * sc, 1.3f * sc, 1.4f * sc, Melt((Color){ 196, 168, 140, 255 }, fade));
            PropCyl(&buf, x, y + 1.35f * sc, z, 1.15f * sc, 0.05f * sc, 0.7f * sc, 4, Melt((Color){ 150, 64, 52, 255 }, fade));
        } else if (p->kind == 4) {
            PropCube(&buf, x, y + 1.15f * sc, z, 3.6f * sc, 2.3f * sc, 2.3f * sc, Melt((Color){ 176, 84, 62, 255 }, fade));
            PropCube(&buf, x, y + 2.45f * sc, z, 3.9f * sc, 0.38f * sc, 2.55f * sc, Melt((Color){ 148, 42, 40, 255 }, fade));
            PropCyl(&buf, x + 2.35f * sc, y, z + 0.2f, 0.55f * sc, 0.55f * sc, 2.8f * sc, 6, Melt((Color){ 198, 198, 190, 255 }, fade));
            PropCyl(&buf, x + 2.35f * sc, y + 2.7f * sc, z + 0.2f, 0.7f * sc, 0.05f * sc, 0.35f * sc, 6, Melt((Color){ 150, 150, 146, 255 }, fade));
        } else {
            float tall = (p->kind == 6) ? 1.45f : 1.0f;
            float height = (14.0f + sc * 7.0f) * tall;
            float w = (p->kind == 6) ? 12.0f + sc * 2.0f : 22.0f + sc * 4.0f;
            float d = w * 0.72f;
            Color wall = (p->kind == 6) ? (Color){ 118, 128, 140, 255 } : (Color){ 188, 176, 160, 255 };
            if (((int)floorf(x + z) & 1) != 0) wall = (Color){ 164, 172, 180, 255 };
            PropCube(&buf, x, y + height * 0.5f, z, w, height, d, Melt(wall, fade));
            PropCube(&buf, x, y + height + 0.12f, z, w + 0.35f, 0.28f, d + 0.35f, Melt((Color){ 92, 96, 102, 255 }, fade));
            Color glass = Melt((Color){ 64, 92, 112, 255 }, fade);
            float band = height * 0.42f;
            PropCube(&buf, x, y + height * 0.55f, z + d * 0.5f + 0.04f, w * 0.72f, band, 0.08f, glass);
            PropCube(&buf, x + w * 0.5f + 0.04f, y + height * 0.55f, z, 0.08f, band, d * 0.72f, glass);
        }
    }
    if (buf.count < 3) {
        MemFree(buf.v);
        MemFree(buf.n);
        MemFree(buf.c);
        MemFree(buf.uv);
        return;
    }
    Mesh mesh;
    memset(&mesh, 0, sizeof(mesh));
    mesh.vertexCount = buf.count;
    mesh.triangleCount = buf.count / 3;
    mesh.vertices = buf.v;
    mesh.normals = buf.n;
    mesh.colors = buf.c;
    mesh.texcoords = buf.uv;
    UploadMesh(&mesh, 0);
    s->propMesh = mesh;
    s->propMeshReady = 1;
}

static void DrawClouds(const Balloon* b)
{
    Color cloud = { 244, 246, 248, 230 };
    int gx0 = (int)floorf((b->x - 520.0f) / 180.0f);
    int gz0 = (int)floorf((b->z - 520.0f) / 180.0f);
    int gx1 = (int)floorf((b->x + 520.0f) / 180.0f);
    int gz1 = (int)floorf((b->z + 520.0f) / 180.0f);
    for (int gz = gz0; gz <= gz1; gz++) {
        for (int gx = gx0; gx <= gx1; gx++) {
            unsigned int h = (unsigned int)gx * 73856093u ^ (unsigned int)gz * 19349663u;
            if ((h % 3u) == 0u) continue;
            float x = ((float)gx + 0.5f) * 180.0f;
            float z = ((float)gz + 0.5f) * 180.0f;
            float y = 220.0f + (float)(h % 180u);
            int blobs = 3 + (int)(h % 3u);
            for (int i = 0; i < blobs; i++) {
                float ox = (float)((h >> (i * 3)) & 7u) - 3.5f;
                float oz = (float)((h >> (i * 3 + 1)) & 7u) - 3.5f;
                float oy = (float)(i % 2) * 2.0f;
                DrawCube((Vector3){ x + ox * 6.0f, y + oy, z + oz * 5.0f }, 16.0f, 6.0f, 12.0f, cloud);
            }
        }
    }
}

static void DrawParticles(const Scene* s)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle* p = &s->particles[i];
        if (p->life <= 0.0f) continue;
        float u = p->life / p->maxLife;
        Vector3 pos = { p->x, p->y, p->z };
        if (p->kind == 0) {
            unsigned char g = (unsigned char)(80 + 160 * u);
            DrawCube(pos, 0.08f * u + 0.04f, 0.12f * u + 0.04f, 0.08f * u + 0.04f, (Color){ 255, g, 30, (unsigned char)(220 * u) });
        } else {
            DrawCube(pos, 0.16f * u + 0.06f, 0.16f * u + 0.06f, 0.16f * u + 0.06f, (Color){ 245, 248, 250, (unsigned char)(140 * u) });
        }
    }
}

/* Backdrop for every lake and river. Submerged vertices are lifted to the
 * waterline in the ground shader, so the lake is the same surface as the
 * bank and the two cannot flicker against each other. */
void Scene_Draw(Scene* s, Vector3 eye)
{
    if (!NewGen_Active()) UpdateClip(s);

    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    Matrix skyXform = MatrixTranslate(eye.x, eye.y, eye.z);
    DrawMesh(s->sky, s->material, skyXform);
    rlEnableBackfaceCulling();
    rlEnableDepthMask();

    Vector3 sunDir = Vector3Normalize((Vector3){ 0.45f, 0.62f, -0.28f });
    DrawSphere(Vector3Add(eye, Vector3Scale(sunDir, 700.0f)), 22.0f, (Color){ 255, 214, 120, 255 });

    /* World scrolls under a stationary basket. Ground colour is per pixel,
     * so the mesh only has to carry the shape and the cover weights. */
    rlPushMatrix();
    rlTranslatef(-s->balloon.x, -s->balloon.altitude, -s->balloon.z);
    if (NewGen_Active()) {
        NewGen_Update(s->balloon.x, s->balloon.z);
        NewGen_Draw(s->balloon.x, s->balloon.z);
    } else {
    if (s->locEye >= 0) {
        float eye[2] = { s->balloon.x, s->balloon.z };
        SetShaderValue(s->groundMat.shader, s->locEye, eye, SHADER_UNIFORM_VEC2);
    }
    CoverUpdate(s, s->balloon.x, s->balloon.z);
    if (s->locCoverMap >= 0 && s->coverReady) {
        float map[4] = {
            s->coverOriginX, s->coverOriginZ, s->coverTexel * (float)COVER_N, 0.0f
        };
        SetShaderValue(s->groundMat.shader, s->locCoverMap, map, SHADER_UNIFORM_VEC4);
    }
    for (int i = CLIP_LEVELS - 1; i >= 0; i--) {
        if (!s->clip[i].ready) continue;
        DrawMesh(s->clip[i].mesh, s->groundMat, MatrixIdentity());
    }
    if (s->propMeshReady) DrawMesh(s->propMesh, s->material, MatrixIdentity());
    }
    /* Clouds are scenery. They must not hide the gondola when the balloon
     * climbs through their layer, or the posts draw on top of a missing floor. */
    BeginBlendMode(BLEND_ALPHA);
    rlDisableDepthMask();
    DrawClouds(&s->balloon);
    rlEnableDepthMask();
    EndBlendMode();
    rlPopMatrix();

    /* Fabric is a single shell. From the basket the camera looks at the
     * inside, so both sides have to draw. */
    rlDisableBackfaceCulling();
    DrawMesh(s->envelope, s->material, MatrixIdentity());
    /* Mouth rim, and the tape where the plain skirt meets the gores. */
    DrawEnvelopeHoop(1.0f, 0.055f, (Color){ 96, 64, 40, 255 });
    DrawEnvelopeHoop(0.68f, 0.03f, (Color){ 120, 78, 48, 255 });
    Matrix crown = MatrixTranslate(0.0f, ENVELOPE_TOP_Y - 0.05f + s->ventOpen * 1.5f, 0.0f);
    DrawMesh(s->crown, s->material, crown);
    rlEnableBackfaceCulling();

    DrawBasket(s);

    Color red = { 214, 42, 52, 255 };
    Color green = { 36, 168, 78, 255 };
    DrawHandle(&s->handles.h[HANDLE_BURNER], red, 0);
    DrawHandle(&s->handles.h[HANDLE_VENT], green, 1);

    /* Signal cables: red runs to the burner, green runs up to the crown.
     * Pulling shortens the sag, which is the visual "I am holding this open". */
    float rx, ry, rz, gx, gy, gz;
    Handle_GripPosition(&s->handles.h[HANDLE_BURNER], &rx, &ry, &rz);
    Handle_GripPosition(&s->handles.h[HANDLE_VENT], &gx, &gy, &gz);
    float redSag = 0.18f * (1.0f - s->handles.h[HANDLE_BURNER].pull / HANDLE_MAX_PULL);
    float greenSag = 0.8f * (1.0f - s->handles.h[HANDLE_VENT].pull / HANDLE_MAX_PULL);
    DrawRope((Vector3){ rx, ry, rz }, BurnerValveTip(s->handles.h[HANDLE_BURNER].pull / HANDLE_MAX_PULL),
             redSag, 0.008f, red);
    DrawRope((Vector3){ gx, gy, gz }, (Vector3){ 0.15f, ENVELOPE_TOP_Y, 0.0f }, greenSag, 0.008f, green);

    BeginBlendMode(BLEND_ALPHA);
    DrawParticles(s);
    EndBlendMode();

    DrawHands(s);

    if (s->lastHands[0].active) {
        Quaternion q = { s->lastHands[0].qx, s->lastHands[0].qy, s->lastHands[0].qz, s->lastHands[0].qw };
        Vector3 p = { s->lastHands[0].x, s->lastHands[0].y, s->lastHands[0].z };
        Watch_Draw(&s->watch, p, q, eye);
    } else {
        Watch_Hide();
    }
}
