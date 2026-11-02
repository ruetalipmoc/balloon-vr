#include "scene.h"

#include "sim_math.h"
#include "terrain.h"
#include "terrain_mesh.h"
#include "world.h"

#include "rlgl.h"
#include "raymath.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHUNKS 256
#define CHUNK_RADIUS 7
#define MAX_PARTICLES 192

typedef struct Slot {
    int occupied;
    int cx, cz, lod;
    ChunkMesh mesh;
} Slot;

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
    HandInput lastHands[2];

    Mesh envelope;
    Mesh crown;
    Mesh sky;
    Material material;
    int meshesReady;

    Slot slots[MAX_CHUNKS];

    Particle particles[MAX_PARTICLES];
    Sound burner;
    int audioReady;

    RenderTexture2D labelBurn;
    RenderTexture2D labelVent;
    int labelsReady;
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

static Color StripeColor(int slice)
{
    static const Color stripes[8] = {
        { 196, 48, 54, 255 },
        { 245, 232, 210, 255 },
        { 226, 170, 64, 255 },
        { 38, 140, 148, 255 },
        { 245, 232, 210, 255 },
        { 186, 42, 58, 255 },
        { 32, 58, 112, 255 },
        { 245, 232, 210, 255 }
    };
    return stripes[slice & 7];
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

static float EnvelopeRadius(float t)
{
    /* t = 0 at the crown, t = 1 at the mouth. Fat teardrop, pinched at both ends. */
    float bell = sinf(powf(fmaxf(t, 0.0f), 0.70f) * (float)M_PI);
    float r = bell * ENVELOPE_RADIUS * (0.62f + 0.38f * sinf(t * (float)M_PI));
    if (t > 0.84f) {
        float u = (t - 0.84f) / 0.16f;
        r = Lerpf(r, ENVELOPE_RADIUS * 0.26f, u);
    }
    if (t < 0.06f) r = Lerpf(0.9f, r, t / 0.06f);
    return r;
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
        float y0 = Lerpf(ENVELOPE_TOP_Y, ENVELOPE_MOUTH_Y, t0);
        float y1 = Lerpf(ENVELOPE_TOP_Y, ENVELOPE_MOUTH_Y, t1);
        float r0 = EnvelopeRadius(t0);
        float r1 = EnvelopeRadius(t1);
        for (int k = 0; k < slices; k++) {
            float a0 = (float)k / (float)slices * 2.0f * (float)M_PI;
            float a1 = (float)(k + 1) / (float)slices * 2.0f * (float)M_PI;
            Vector3 p00 = { cosf(a0) * r0, y0, sinf(a0) * r0 };
            Vector3 p10 = { cosf(a1) * r0, y0, sinf(a1) * r0 };
            Vector3 p11 = { cosf(a1) * r1, y1, sinf(a1) * r1 };
            Vector3 p01 = { cosf(a0) * r1, y1, sinf(a0) * r1 };
            Color col = StripeColor(k / 2);
            PushTri(v, n, c, uv, &i, p00, p10, p11, col);
            PushTri(v, n, c, uv, &i, p00, p11, p01, col);
        }
    }
    return MakeFlatMesh(i, v, n, c, uv);
}

static Mesh MakeCrown(void)
{
    const int slices = 12;
    float r = 1.35f;
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
    const int stacks = 7;
    const int slices = 14;
    float radius = 880.0f;
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
        /* 0 at the zenith, 1 a little below the horizon. */
        float p0 = t0 * 1.08f;
        float p1 = t1 * 1.08f;
        float y0 = cosf(p0 * (float)M_PI * 0.5f) * radius;
        float y1 = cosf(p1 * (float)M_PI * 0.5f) * radius;
        float r0 = sinf(p0 * (float)M_PI * 0.5f) * radius;
        float r1 = sinf(p1 * (float)M_PI * 0.5f) * radius;
        Color face = t0 < 0.35f ? LerpColor(zenith, high, t0 / 0.35f)
                                : LerpColor(high, horizon, (t0 - 0.35f) / 0.65f);
        for (int k = 0; k < slices; k++) {
            float a0 = (float)k / (float)slices * 2.0f * (float)M_PI;
            float a1 = (float)(k + 1) / (float)slices * 2.0f * (float)M_PI;
            Vector3 a = { cosf(a0) * r0, y0, sinf(a0) * r0 };
            Vector3 b = { cosf(a1) * r0, y0, sinf(a1) * r0 };
            Vector3 d = { cosf(a1) * r1, y1, sinf(a1) * r1 };
            Vector3 e = { cosf(a0) * r1, y1, sinf(a0) * r1 };
            /* Winding faces inward so the dome is visible from the basket. */
            PushTri(v, n, c, uv, &i, a, e, d, face);
            PushTri(v, n, c, uv, &i, a, d, b, face);
        }
    }
    return MakeFlatMesh(i, v, n, c, uv);
}

static RenderTexture2D MakeLabel(const char* text, Color ink, Color plate)
{
    RenderTexture2D rt = LoadRenderTexture(256, 64);
    BeginTextureMode(rt);
    ClearBackground(plate);
    int w = MeasureText(text, 40);
    DrawText(text, (256 - w) / 2, 12, 40, ink);
    EndTextureMode();
    SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
    return rt;
}

static void MakeBurnerSound(Scene* s)
{
    if (!IsAudioDeviceReady()) return;
    const int rate = 22050;
    const int frames = rate * 2;
    short* pcm = (short*)MemAlloc((unsigned int)frames * sizeof(short));
    if (!pcm) return;
    unsigned int rng = 0x1234567u;
    for (int i = 0; i < frames; i++) {
        rng = rng * 1664525u + 1013904223u;
        float n = ((int)(rng >> 16) % 2000 - 1000) / 1000.0f;
        float t = (float)i / (float)rate;
        float rumble = sinf(2.0f * (float)M_PI * 48.0f * t) * 0.35f
                     + sinf(2.0f * (float)M_PI * 96.0f * t) * 0.12f;
        float env = 1.0f;
        float edge = 0.04f;
        if (t < edge) env = t / edge;
        if (t > 2.0f - edge) env = (2.0f - t) / edge;
        float sample = (rumble + n * 0.28f) * env * 0.55f;
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        pcm[i] = (short)(sample * 28000.0f);
    }
    Wave wave = { 0 };
    wave.frameCount = (unsigned int)frames;
    wave.sampleRate = (unsigned int)rate;
    wave.sampleSize = 16;
    wave.channels = 1;
    wave.data = pcm;
    s->burner = LoadSoundFromWave(wave);
    UnloadWave(wave);
    s->audioReady = (s->burner.frameCount > 0);
}

Scene* Scene_Create(void)
{
    Scene* s = (Scene*)calloc(1, sizeof(Scene));
    if (!s) return NULL;

    float x = 2400.0f;
    float z = 1800.0f;
    float ground = Terrain_GroundAt(x, z);
    Balloon_Reset(&s->balloon, x, z, ground + 90.0f);
    s->groundY = ground;
    Handles_Init(&s->handles);

    s->envelope = MakeEnvelope();
    s->crown = MakeCrown();
    s->sky = MakeSky();
    s->material = LoadMaterialDefault();
    s->meshesReady = 1;

    Watch_Init(&s->watch);
    s->labelBurn = MakeLabel("BURN", (Color){ 255, 236, 230, 255 }, (Color){ 150, 32, 40, 255 });
    s->labelVent = MakeLabel("VENT", (Color){ 230, 255, 236, 255 }, (Color){ 24, 110, 58, 255 });
    s->labelsReady = 1;

    InitAudioDevice();
    MakeBurnerSound(s);
    return s;
}

void Scene_Destroy(Scene* s)
{
    if (!s) return;
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (s->slots[i].occupied) ChunkMesh_Free(&s->slots[i].mesh);
    }
    if (s->meshesReady) {
        UnloadMesh(s->envelope);
        UnloadMesh(s->crown);
        UnloadMesh(s->sky);
        UnloadMaterial(s->material);
    }
    Watch_Shutdown(&s->watch);
    if (s->labelsReady) {
        UnloadRenderTexture(s->labelBurn);
        UnloadRenderTexture(s->labelVent);
    }
    if (s->audioReady) UnloadSound(s->burner);
    if (IsAudioDeviceReady()) CloseAudioDevice();
    free(s);
}

const Balloon* Scene_Balloon(const Scene* s) { return &s->balloon; }
float Scene_Ground(const Scene* s) { return s->groundY; }

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

    float targetFlame = s->balloon.burner;
    float targetVent = s->balloon.vent;
    s->flame += (targetFlame - s->flame) * Minf(1.0f, dt * 8.0f);
    s->ventOpen += (targetVent - s->ventOpen) * Minf(1.0f, dt * 4.0f);

    if (s->flame > 0.15f) {
        /* Jets sit on top of the burner coils, just under the envelope mouth. */
        float ang = (float)(rand() % 628) / 100.0f;
        float rad = 0.03f + (float)(rand() % 40) / 1000.0f;
        float side = (rand() & 1) ? -0.055f : 0.055f;
        Vector3 p = { side + cosf(ang) * rad, 2.68f, sinf(ang) * rad };
        Vector3 v = { (float)(rand() % 10 - 5) / 50.0f, 3.0f + s->flame * 2.4f, (float)(rand() % 10 - 5) / 50.0f };
        SpawnParticle(s, 0, p, v, 0.45f + s->flame * 0.25f);
    }
    if (s->ventOpen > 0.2f) {
        Vector3 p = { (float)(rand() % 20 - 10) / 80.0f, ENVELOPE_TOP_Y + s->ventOpen * 0.4f, (float)(rand() % 20 - 10) / 80.0f };
        Vector3 v = { (float)(rand() % 10 - 5) / 30.0f, 2.5f + s->ventOpen * 3.0f, (float)(rand() % 10 - 5) / 30.0f };
        SpawnParticle(s, 1, p, v, 1.1f);
    }
    UpdateParticles(s, dt);

    if (s->audioReady) {
        if (s->flame > 0.05f) {
            SetSoundVolume(s->burner, Clampf(s->flame, 0.0f, 1.0f) * 0.8f);
            if (!IsSoundPlaying(s->burner)) PlaySound(s->burner);
        } else if (IsSoundPlaying(s->burner)) {
            StopSound(s->burner);
        }
    }

    Watch_Render(&s->watch, &s->balloon, s->groundY);
}

static Slot* FindSlot(Scene* s, int cx, int cz)
{
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (s->slots[i].occupied && s->slots[i].cx == cx && s->slots[i].cz == cz) return &s->slots[i];
    }
    return NULL;
}

static Slot* AllocSlot(Scene* s)
{
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (!s->slots[i].occupied) return &s->slots[i];
    }
    return NULL;
}

static void UpdateChunks(Scene* s)
{
    int ccx = (int)floorf(s->balloon.x / TERRAIN_CHUNK_SIZE);
    int ccz = (int)floorf(s->balloon.z / TERRAIN_CHUNK_SIZE);

    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (!s->slots[i].occupied) continue;
        int dx = s->slots[i].cx - ccx;
        int dz = s->slots[i].cz - ccz;
        if (dx < 0) dx = -dx;
        if (dz < 0) dz = -dz;
        if (dx > CHUNK_RADIUS || dz > CHUNK_RADIUS) {
            ChunkMesh_Free(&s->slots[i].mesh);
            s->slots[i].occupied = 0;
        }
    }

    for (int dz = -CHUNK_RADIUS; dz <= CHUNK_RADIUS; dz++) {
        for (int dx = -CHUNK_RADIUS; dx <= CHUNK_RADIUS; dx++) {
            int cx = ccx + dx;
            int cz = ccz + dz;
            float mx = ((float)cx + 0.5f) * TERRAIN_CHUNK_SIZE;
            float mz = ((float)cz + 0.5f) * TERRAIN_CHUNK_SIZE;
            float dist = hypotf(mx - s->balloon.x, mz - s->balloon.z);
            int lod = Terrain_LodForDistance(dist);
            Slot* slot = FindSlot(s, cx, cz);
            if (slot && slot->lod == lod) continue;
            if (!slot) {
                slot = AllocSlot(s);
                if (!slot) continue;
            } else {
                ChunkMesh_Free(&slot->mesh);
            }
            ChunkMesh_Build(cx, cz, lod, s->balloon.x, s->balloon.z, &slot->mesh);
            slot->occupied = 1;
            slot->cx = cx;
            slot->cz = cz;
            slot->lod = lod;
        }
    }
}

static void DrawRope(Vector3 a, Vector3 b, float sag, float radius, Color color)
{
    Vector3 prev = a;
    const int n = 7;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n;
        Vector3 p = {
            Lerpf(a.x, b.x, t),
            Lerpf(a.y, b.y, t) - sinf(t * (float)M_PI) * sag,
            Lerpf(a.z, b.z, t)
        };
        DrawCylinderEx(prev, p, radius, radius, 5, color);
        prev = p;
    }
}

static void DrawLabel(Texture2D tex, Vector3 pos, Vector3 eye)
{
    Vector3 n = Vector3Subtract(eye, pos);
    float len = Vector3Length(n);
    if (len < 1.0e-4f) n = (Vector3){ 0, 0, 1 };
    else n = Vector3Scale(n, 1.0f / len);
    Vector3 right = Vector3CrossProduct((Vector3){ 0, 1, 0 }, n);
    if (Vector3Length(right) < 1.0e-4f) right = (Vector3){ 1, 0, 0 };
    right = Vector3Normalize(right);
    Vector3 up = Vector3Normalize(Vector3CrossProduct(n, right));

    Matrix m = { 0 };
    m.m0 = right.x; m.m1 = right.y; m.m2 = right.z;
    m.m4 = up.x;    m.m5 = up.y;    m.m6 = up.z;
    m.m8 = n.x;     m.m9 = n.y;     m.m10 = n.z;
    m.m12 = pos.x;  m.m13 = pos.y;  m.m14 = pos.z;
    m.m15 = 1.0f;

    rlPushMatrix();
    float16 f = MatrixToFloatV(m);
    rlMultMatrixf(f.v);
    rlSetTexture(tex.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    rlNormal3f(0, 0, 1);
    rlTexCoord2f(0, 0); rlVertex3f(-0.12f, -0.030f, 0);
    rlTexCoord2f(1, 0); rlVertex3f( 0.12f, -0.030f, 0);
    rlTexCoord2f(1, 1); rlVertex3f( 0.12f,  0.030f, 0);
    rlTexCoord2f(0, 1); rlVertex3f(-0.12f,  0.030f, 0);
    rlEnd();
    rlSetTexture(0);
    rlPopMatrix();
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
    const int segPerTurn = 7;
    int n = turns * segPerTurn;
    Vector3 prev = { cx + radius, y0, cz };
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n;
        float a = t * (float)turns * 2.0f * (float)M_PI;
        Vector3 p = {
            cx + cosf(a) * radius,
            Lerpf(y0, y1, t),
            cz + sinf(a) * radius
        };
        DrawCylinderEx(prev, p, 0.016f, 0.016f, 5, color);
        prev = p;
    }
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
        float time = (float)GetTime();
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
    DrawCube((Vector3){ 0, BASKET_RIM_Y, 0 }, hx * 2.0f + 0.06f, 0.06f, hz * 2.0f + 0.06f, (Color){ 196, 148, 78, 255 });
    DrawGuardRail();

    Vector3 posts[4] = {
        { -hx, 0, -hz }, { hx, 0, -hz }, { hx, 0, hz }, { -hx, 0, hz }
    };
    for (int i = 0; i < 4; i++) {
        DrawCube((Vector3){ posts[i].x, FRAME_Y * 0.5f, posts[i].z }, 0.06f, FRAME_Y, 0.06f, wood);
    }
    DrawCube((Vector3){ 0, FRAME_Y, -hz }, hx * 2.0f, 0.05f, 0.05f, wood);
    DrawCube((Vector3){ 0, FRAME_Y, hz }, hx * 2.0f, 0.05f, 0.05f, wood);
    DrawCube((Vector3){ -hx, FRAME_Y, 0 }, 0.05f, 0.05f, hz * 2.0f, wood);
    DrawCube((Vector3){ hx, FRAME_Y, 0 }, 0.05f, 0.05f, hz * 2.0f, wood);

    DrawBurner(s);

    /* Suspension lines from the skirt of the envelope down to the posts. */
    float skirtY = Lerpf(ENVELOPE_TOP_Y, ENVELOPE_MOUTH_Y, 0.78f);
    float skirtR = EnvelopeRadius(0.78f);
    for (int i = 0; i < 8; i++) {
        float a = (float)i / 8.0f * 2.0f * (float)M_PI;
        Vector3 top = { cosf(a) * skirtR, skirtY, sinf(a) * skirtR };
        Vector3 bot = posts[i & 3];
        bot.y = FRAME_Y;
        DrawRope(top, bot, 0.15f, 0.012f, rope);
    }
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
        const int n = 10;
        Vector3 prev = { x + 0.09f, y, z };
        for (int i = 1; i <= n; i++) {
            float a = (float)i / (float)n * 2.0f * (float)M_PI;
            Vector3 p = { x + cosf(a) * 0.09f, y + sinf(a) * 0.09f, z };
            DrawCylinderEx(prev, p, 0.016f, 0.016f, 5, color);
            prev = p;
        }
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

static void DrawProp(const Prop* p)
{
    Vector3 pos = { p->x, p->y, p->z };
    if (p->kind == 0) {
        DrawCylinder(pos, 0.08f * p->scale, 0.10f * p->scale, 0.7f * p->scale, 5, (Color){ 92, 64, 40, 255 });
        DrawSphere((Vector3){ pos.x, pos.y + 1.05f * p->scale, pos.z }, 0.55f * p->scale, (Color){ 48, 130, 62, 255 });
    } else if (p->kind == 1) {
        DrawCylinder(pos, 0.07f * p->scale, 0.09f * p->scale, 0.5f * p->scale, 5, (Color){ 84, 60, 40, 255 });
        DrawCylinder((Vector3){ pos.x, pos.y + 0.45f * p->scale, pos.z }, 0.02f, 0.55f * p->scale, 1.6f * p->scale, 6, (Color){ 30, 90, 58, 255 });
    } else if (p->kind == 2) {
        DrawCube((Vector3){ pos.x, pos.y + 0.25f * p->scale, pos.z }, 0.7f * p->scale, 0.45f * p->scale, 0.55f * p->scale, (Color){ 128, 126, 120, 255 });
    } else {
        DrawCube((Vector3){ pos.x, pos.y + 0.7f, pos.z }, 1.6f, 1.3f, 1.4f, (Color){ 196, 168, 140, 255 });
        DrawCylinder((Vector3){ pos.x, pos.y + 1.35f, pos.z }, 0.05f, 1.15f, 0.7f, 4, (Color){ 150, 64, 52, 255 });
    }
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

void Scene_Draw(Scene* s, Vector3 eye)
{
    UpdateChunks(s);

    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    Matrix skyXform = MatrixTranslate(eye.x, eye.y, eye.z);
    DrawMesh(s->sky, s->material, skyXform);
    rlEnableBackfaceCulling();
    rlEnableDepthMask();

    Vector3 sunDir = Vector3Normalize((Vector3){ 0.45f, 0.62f, -0.28f });
    DrawSphere(Vector3Add(eye, Vector3Scale(sunDir, 700.0f)), 22.0f, (Color){ 255, 214, 120, 255 });

    /* World scrolls under a stationary basket. */
    rlPushMatrix();
    rlTranslatef(-s->balloon.x, -s->balloon.altitude, -s->balloon.z);
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (!s->slots[i].occupied || !s->slots[i].mesh.ready) continue;
        DrawMesh(s->slots[i].mesh.ground, s->material, MatrixIdentity());
    }
    BeginBlendMode(BLEND_ALPHA);
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (!s->slots[i].occupied || !s->slots[i].mesh.hasWater) continue;
        DrawMesh(s->slots[i].mesh.water, s->material, MatrixIdentity());
    }
    EndBlendMode();
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (!s->slots[i].occupied) continue;
        for (int p = 0; p < s->slots[i].mesh.propCount; p++) DrawProp(&s->slots[i].mesh.props[p]);
    }
    DrawClouds(&s->balloon);
    rlPopMatrix();

    /* Fabric is a single shell. From the basket the camera looks at the
     * inside, so both sides have to draw. */
    rlDisableBackfaceCulling();
    DrawMesh(s->envelope, s->material, MatrixIdentity());
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

    if (s->labelsReady) {
        DrawLabel(s->labelBurn.texture, (Vector3){ HANDLE_RED_X, 1.66f, -0.02f }, eye);
        DrawLabel(s->labelVent.texture, (Vector3){ HANDLE_GREEN_X, 1.66f, -0.02f }, eye);
    }

    BeginBlendMode(BLEND_ALPHA);
    DrawParticles(s);
    EndBlendMode();

    DrawHands(s);

    if (s->lastHands[0].active) {
        Quaternion q = { s->lastHands[0].qx, s->lastHands[0].qy, s->lastHands[0].qz, s->lastHands[0].qw };
        Vector3 p = { s->lastHands[0].x, s->lastHands[0].y, s->lastHands[0].z };
        Watch_Draw(&s->watch, p, q);
    }
}
