/* Balloon VR
 * Desktop fallback: mouse look, F / left mouse burns, G / right mouse vents.
 * Headset: squeeze either ceiling handle and pull it down. The left wrist
 * carries the instrument. */
#include "scene.h"
#include "terrain.h"
#include "world.h"
#include "xr_session.h"

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

static int g_webBurn = 0;
static int g_webVent = 0;
static int g_touching = 0;
static Vector2 g_touchLast = { 0 };

EMSCRIPTEN_KEEPALIVE
void balloon_set_burn(int on) { g_webBurn = on ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE
void balloon_set_vent(int on) { g_webVent = on ? 1 : 0; }

EM_JS(void, Balloon_ExposeControls, (), {
    Module._balloon_set_burn = Module._balloon_set_burn || Module["_balloon_set_burn"];
    var burn = Module["_balloon_set_burn"] || Module._balloon_set_burn;
    var vent = Module["_balloon_set_vent"] || Module._balloon_set_vent;
    window.__balloonControls = {
        burn: function (on) { if (burn) burn(on ? 1 : 0); },
        vent: function (on) { if (vent) vent(on ? 1 : 0); },
        ready: true
    };
    window.dispatchEvent(new Event("balloon-ready"));
});
#endif

static int g_yawCaptured = 0;
static float g_yaw = 0.0f;
static float g_pitch = 0.22f;

static Vector3 BasisForward(void)
{
    return (Vector3){
        sinf(g_yaw) * cosf(g_pitch),
        sinf(g_pitch),
        -cosf(g_yaw) * cosf(g_pitch)
    };
}

static Quaternion WristFacing(Vector3 wrist, Vector3 eye)
{
    Vector3 normal = Vector3Subtract(eye, wrist);
    if (Vector3Length(normal) < 1.0e-4f) return (Quaternion){ 0, 0, 0, 1 };
    normal = Vector3Normalize(normal);
    Vector3 side = Vector3CrossProduct((Vector3){ 0, 1, 0 }, normal);
    if (Vector3Length(side) < 1.0e-4f) side = (Vector3){ 1, 0, 0 };
    side = Vector3Normalize(side);
    Vector3 up = Vector3Normalize(Vector3CrossProduct(normal, side));

    Matrix m = { 0 };
    m.m0 = side.x;   m.m1 = side.y;   m.m2 = side.z;
    m.m4 = normal.x; m.m5 = normal.y; m.m6 = normal.z;
    m.m8 = -up.x;    m.m9 = -up.y;    m.m10 = -up.z;
    m.m15 = 1.0f;
    return QuaternionFromMatrix(m);
}

static Quaternion ReadingPose(Vector3 hand, Vector3 eye, Vector3 elbowDir)
{
    Vector3 yAxis = Vector3Subtract(eye, hand);
    if (Vector3Length(yAxis) < 1.0e-4f) return (Quaternion){ 0, 0, 0, 1 };
    yAxis = Vector3Normalize(yAxis);
    elbowDir.y = 0.0f;
    if (Vector3Length(elbowDir) < 1.0e-4f) elbowDir = (Vector3){ -1, 0, 0 };
    Vector3 zAxis = Vector3Normalize(elbowDir);
    zAxis = Vector3Subtract(zAxis, Vector3Scale(yAxis, Vector3DotProduct(zAxis, yAxis)));
    if (Vector3Length(zAxis) < 1.0e-4f) zAxis = (Vector3){ 0, 0, 1 };
    zAxis = Vector3Normalize(zAxis);
    Vector3 xAxis = Vector3Normalize(Vector3CrossProduct(yAxis, zAxis));

    Matrix m = { 0 };
    m.m0 = xAxis.x; m.m1 = xAxis.y; m.m2 = xAxis.z;
    m.m4 = yAxis.x; m.m5 = yAxis.y; m.m6 = yAxis.z;
    m.m8 = zAxis.x; m.m9 = zAxis.y; m.m10 = zAxis.z;
    m.m15 = 1.0f;
    return QuaternionFromMatrix(m);
}

static Quaternion HangingPose(Vector3 outDir)
{
    Vector3 zAxis = { 0.0f, 1.0f, 0.0f };
    outDir.y = 0.0f;
    if (Vector3Length(outDir) < 1.0e-4f) outDir = (Vector3){ 1.0f, 0.0f, 0.0f };
    Vector3 yAxis = Vector3Normalize(outDir);
    Vector3 xAxis = Vector3Normalize(Vector3CrossProduct(yAxis, zAxis));
    Matrix m = { 0 };
    m.m0 = xAxis.x; m.m1 = xAxis.y; m.m2 = xAxis.z;
    m.m4 = yAxis.x; m.m5 = yAxis.y; m.m6 = yAxis.z;
    m.m8 = zAxis.x; m.m9 = zAxis.y; m.m10 = zAxis.z;
    m.m15 = 1.0f;
    return QuaternionFromMatrix(m);
}

static void HandAt(HandInput* hand, Vector3 pos, Quaternion q, int grip)
{
    hand->active = 1;
    hand->grip = grip;
    hand->x = pos.x;
    hand->y = pos.y;
    hand->z = pos.z;
    hand->qx = q.x;
    hand->qy = q.y;
    hand->qz = q.z;
    hand->qw = q.w;
}

static void DesktopHands(HandInput hands[2], Vector3 eye, int burn, int vent)
{
    Vector3 forward = BasisForward();
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, (Vector3){ 0, 1, 0 }));
    memset(hands, 0, sizeof(HandInput) * 2);

    Vector3 leftPos;
    Vector3 rightPos;
    Quaternion leftQ;
    /* The watch shows only with the forearm horizontal and the back of the
     * hand toward the face. Look down, or hold C, to take that pose. */
    int reading = g_pitch < -0.40f || IsKeyDown(KEY_C);
    if (getenv("BALLOON_WATCH")) reading = 1;
    if (vent) {
        leftPos = (Vector3){ HANDLE_GREEN_X, HANDLE_REST_Y - HANDLE_MAX_PULL * 0.85f, HANDLE_Z };
        leftQ = WristFacing(leftPos, eye);
    } else if (reading) {
        leftPos = Vector3Add(eye, Vector3Add(Vector3Scale(right, -0.18f),
                              Vector3Add((Vector3){ 0, -0.08f, 0 }, Vector3Scale(forward, 0.42f))));
        Vector3 elbow = Vector3Add(Vector3Scale(forward, -0.85f), Vector3Scale(right, -0.45f));
        leftQ = ReadingPose(leftPos, eye, elbow);
    } else {
        leftPos = Vector3Add(eye, Vector3Add(Vector3Scale(right, -0.28f),
                              Vector3Add((Vector3){ 0, -0.55f, 0 }, Vector3Scale(forward, 0.10f))));
        /* Elbow above the wrist, so a lowered arm does not count as reading. */
        leftQ = HangingPose(right);
    }
    if (burn) {
        rightPos = (Vector3){ HANDLE_RED_X, HANDLE_REST_Y - HANDLE_MAX_PULL * 0.85f, HANDLE_Z };
    } else {
        rightPos = Vector3Add(eye, Vector3Add(Vector3Scale(right, 0.22f),
                               Vector3Add((Vector3){ 0, -0.22f, 0 }, Vector3Scale(forward, 0.40f))));
    }
    HandAt(&hands[0], leftPos, leftQ, vent);
    HandAt(&hands[1], rightPos, WristFacing(rightPos, eye), burn);
}

static void DrawHint(int vr)
{
#ifdef __EMSCRIPTEN__
    (void)vr;
    const char* line = "Glisser pour regarder    F bruleur    G soupape    P pause    S capture    regarder en bas : montre";
#else
    const char* line = vr
        ? "Red burns, green vents.  P pauses.  S saves a screenshot of this window."
        : "Click to look.  F burner  G vent  P pause  S screenshot  C: watch  Arrows: pages";
#endif
    DrawRectangle(0, GetScreenHeight() - 36, GetScreenWidth(), 36, (Color){ 8, 16, 24, 180 });
    DrawText(line, 16, GetScreenHeight() - 26, 18, (Color){ 232, 244, 240, 255 });
}

static int g_paused = 0;

static void DrawPause(void)
{
    if (!g_paused) return;
    const char* label = "PAUSE";
    int w = MeasureText(label, 28);
    DrawText(label, GetScreenWidth() / 2 - w / 2, 48, 28, (Color){ 255, 196, 96, 255 });
}

/* Grab the window after the frame is drawn, before it is swapped away. */
static void SaveScreenshot(void)
{
    static int serial = 0;
    char name[64];
    snprintf(name, sizeof(name), "/tmp/balloon-%04d.png", ++serial);
    Image img = LoadImageFromScreen();
    int ok = ExportImage(img, name);
    if (ok) printf("screenshot %s (%dx%d)\n", name, img.width, img.height);
    else fprintf(stderr, "screenshot failed: %s\n", name);
#ifdef __EMSCRIPTEN__
    if (ok) {
        EM_ASM({
            try {
                var name = UTF8ToString($0);
                var data = FS.readFile(name);
                var blob = new Blob([data], { type: "image/png" });
                var url = URL.createObjectURL(blob);
                var a = document.createElement("a");
                a.href = url;
                var leaf = name.substring(name.lastIndexOf("/") + 1);
                a.download = leaf;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                URL.revokeObjectURL(url);
            } catch (e) {
                console.warn("screenshot download failed", e);
            }
        }, name);
    }
#endif
    UnloadImage(img);
}

static void UpdateDesktopLook(void)
{
#ifdef __EMSCRIPTEN__
    /* No pointer lock: the preview frame often refuses it. Drag, or a finger,
     * orbits the view. The left button is look, not the burner. */
    if (GetTouchPointCount() > 0) {
        Vector2 pos = GetTouchPosition(0);
        if (!g_touching) {
            g_touchLast = pos;
            g_touching = 1;
        } else {
            g_yaw += (pos.x - g_touchLast.x) * 0.0045f;
            g_pitch -= (pos.y - g_touchLast.y) * 0.0045f;
            g_touchLast = pos;
        }
    } else {
        g_touching = 0;
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 delta = GetMouseDelta();
            g_yaw += delta.x * 0.0032f;
            g_pitch -= delta.y * 0.0032f;
        }
    }
    if (g_pitch > 1.15f) g_pitch = 1.15f;
    if (g_pitch < -1.20f) g_pitch = -1.20f;
    return;
#endif
    if (!g_yawCaptured) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            g_yawCaptured = 1;
            DisableCursor();
        }
        return;
    }
    Vector2 delta = GetMouseDelta();
    g_yaw += delta.x * 0.0032f;
    g_pitch -= delta.y * 0.0032f;
    if (g_pitch > 1.15f) g_pitch = 1.15f;
    if (g_pitch < -1.20f) g_pitch = -1.20f;
}

static int ParseFloat(const char* s, float* out)
{
    if (!s || !*s) return 0;
    char* end = NULL;
    float v = strtof(s, &end);
    if (end == s || *end != '\0') return 0;
    *out = v;
    return 1;
}

static int ParseInt(const char* s, int* out)
{
    char* end = NULL;
    long v;
    if (!s || !out) return 0;
    v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0')) return 0;
    *out = (int)v;
    return 1;
}

static void PrintUsage(void)
{
    printf("balloon-vr [--desktop] [--view METRES] [--lod RINGS]\n");
    printf("  Headset: pull the red ceiling handle to burn, the green one to vent.\n");
    printf("  Desktop: mouse look, F/left mouse burns, G/right mouse vents.\n");
    printf("  --view   Radius of the finest ring at %.0f m above the ground\n", TERRAIN_ALT_REF);
    printf("           (default %.0f, range %.0f..%.0f). It grows as you climb\n",
           TERRAIN_VIEW_DEFAULT, TERRAIN_VIEW_MIN, TERRAIN_VIEW_MAX);
    printf("           so the ground over the rail keeps the same angle.\n");
    printf("  --lod    How many rings (default %d, range %d..%d). Each ring is\n",
           TERRAIN_RINGS_DEFAULT, TERRAIN_RINGS_MIN, TERRAIN_RINGS_MAX);
    printf("           twice as coarse as the one inside it.\n");
}

int main(int argc, char** argv)
{
    int forceDesktop = 0;
    float view = TERRAIN_VIEW_DEFAULT;
    int rings = TERRAIN_RINGS_DEFAULT;
#ifdef __EMSCRIPTEN__
    forceDesktop = 1;
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--desktop") == 0) forceDesktop = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PrintUsage();
            return 0;
        } else if (strcmp(argv[i], "--view") == 0) {
            if (i + 1 >= argc || !ParseFloat(argv[++i], &view) || !(view > 0.0f)) {
                fprintf(stderr, "bad --view (want metres, e.g. 160)\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--lod") == 0) {
            if (i + 1 >= argc || !ParseInt(argv[++i], &rings) || rings < 1) {
                fprintf(stderr, "bad --lod (want a ring count, e.g. 6)\n");
                return 1;
            }
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            PrintUsage();
            return 1;
        }
    }
    view = Terrain_SetViewDistance(view);
    rings = Terrain_SetRingCount(rings);
    printf("fine ring %.0f m   rings %d\n", view, rings);

    const char* pitchEnv = getenv("BALLOON_PITCH");
    if (pitchEnv) g_pitch = strtof(pitchEnv, NULL);

#ifdef __EMSCRIPTEN__
    int winW = EM_ASM_INT({ return Math.max(640, window.innerWidth || 1280); });
    int winH = EM_ASM_INT({ return Math.max(480, window.innerHeight || 800); });
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(winW, winH, "Montgolfiere");
    /* WindowShouldClose already yields ~16 ms. Don't sleep a second time. */
    SetTargetFPS(0);
    SetExitKey(KEY_NULL);
#else
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(1440, 900, "Balloon VR");
    SetTargetFPS(72);
#endif
    rlSetClipPlanes(0.05, Terrain_FarPlane());
    srand(0xB411u);

    Scene* scene = Scene_Create();
    if (!scene) {
        CloseWindow();
        return 1;
    }

    int xr = 0;
    if (!forceDesktop) xr = XR_Init();

#ifdef __EMSCRIPTEN__
    Balloon_ExposeControls();
#endif

    while (!WindowShouldClose() && !XR_WantsQuit()) {
        int session = xr && XR_IsSessionRunning();
#ifdef __EMSCRIPTEN__
        (void)session;
#endif
        int burn = IsKeyDown(KEY_F);
        int vent = IsKeyDown(KEY_G);
#ifdef __EMSCRIPTEN__
        burn = burn || g_webBurn;
        vent = vent || g_webVent || IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
#else
        burn = burn || (!session && g_yawCaptured && IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        vent = vent || (!session && g_yawCaptured && IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
#endif
        if (getenv("BALLOON_BURN")) burn = 1;
        if (getenv("BALLOON_VENT")) vent = 1;
        if (IsKeyPressed(KEY_R) || Scene_TakeViewReset(scene)) {
            XR_Recenter();
            g_yaw = 0.0f;
            g_pitch = 0.22f;
        }
        if (IsKeyPressed(KEY_P)) g_paused = !g_paused;
        int shotRequest = IsKeyPressed(KEY_S);

        if (xr) {
            XrFrame frame;
            if (XR_BeginFrame(&frame)) {
                Scene_Update(scene, frame.hands, g_paused ? 0.0f : frame.dt, burn, vent);
                if (frame.shouldRender) {
                    for (int eye = 0; eye < frame.eyeCount; eye++) {
                        XR_BeginEye(eye);
                        Scene_Draw(scene, (Vector3){ frame.eyeX[eye], frame.eyeY[eye], frame.eyeZ[eye] });
                        XR_EndEye(eye);
                    }
                }
                XR_EndFrame();

                Camera3D mirror = { 0 };
                mirror.position = (Vector3){ frame.mirrorPx, frame.mirrorPy, frame.mirrorPz };
                mirror.target = (Vector3){ frame.mirrorTx, frame.mirrorTy, frame.mirrorTz };
                mirror.up = (Vector3){ frame.mirrorUx, frame.mirrorUy, frame.mirrorUz };
                mirror.fovy = frame.mirrorFovy > 1.0f ? frame.mirrorFovy : 70.0f;
                mirror.projection = CAMERA_PERSPECTIVE;
                BeginDrawing();
                ClearBackground((Color){ 232, 210, 180, 255 });
                rlSetClipPlanes(0.05, Terrain_FarPlane());
                BeginMode3D(mirror);
                Scene_Draw(scene, mirror.position);
                EndMode3D();
                DrawHint(1);
                DrawPause();
                if (shotRequest) SaveScreenshot();
                EndDrawing();
                continue;
            }
        }

        UpdateDesktopLook();
        Vector3 eye = { 0.0f, 1.62f, 0.06f };
        Vector3 forward = BasisForward();
        HandInput hands[2];
        DesktopHands(hands, eye, burn, vent);
        float dt = g_paused ? 0.0f : GetFrameTime();
        Scene_Update(scene, hands, dt, burn, vent);

        Camera3D camera = { 0 };
        camera.position = eye;
        camera.target = Vector3Add(eye, forward);
        camera.up = (Vector3){ 0, 1, 0 };
        camera.fovy = 70.0f;
        camera.projection = CAMERA_PERSPECTIVE;

        BeginDrawing();
        ClearBackground((Color){ 232, 210, 180, 255 });
        rlSetClipPlanes(0.05, Terrain_FarPlane());
        BeginMode3D(camera);
        Scene_Draw(scene, eye);
        EndMode3D();
        DrawHint(0);
        DrawPause();
        const Balloon* balloon = Scene_Balloon(scene);
        char hud[160];
        snprintf(hud, sizeof(hud), "AGL %.0f m    V %+.1f m/s    fine %.0f m    rings %d%s",
                 balloon->altitude - Scene_Ground(scene), balloon->vy,
                 Terrain_ViewDistance(), Terrain_RingCount(),
                 g_paused ? "    PAUSE" : "");
        DrawText(hud, 16, 16, 20, (Color){ 240, 248, 246, 255 });
        if (shotRequest) SaveScreenshot();
        const char* shot = getenv("BALLOON_SCREENSHOT");
        static int shotFrame = 0;
        if (shot) {
            shotFrame++;
            if (shotFrame == 12) TakeScreenshot(shot);
            if (shotFrame > 14) break;
        }
        EndDrawing();
    }

    if (xr) XR_Shutdown();
    Scene_Destroy(scene);
    CloseWindow();
    return 0;
}
