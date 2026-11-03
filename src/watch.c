#include "watch.h"

#include "terrain.h"
#include "wind.h"

#include "rlgl.h"
#include "raymath.h"

#include <math.h>
#include <stdio.h>

#define WATCH_SIZE 512

static void DrawArrow(int x, int y, float bearing, int len, float thick, Color color)
{
    float dx = sinf(bearing);
    float dy = -cosf(bearing); /* screen y grows downward; north is up */
    Vector2 a = { (float)x, (float)y };
    Vector2 tip = { a.x + dx * (float)len, a.y + dy * (float)len };
    DrawLineEx(a, tip, thick, color);
    float px = -dy;
    float py = dx;
    float head = thick * 3.2f;
    Vector2 b = { tip.x - dx * head + px * head * 0.55f, tip.y - dy * head + py * head * 0.55f };
    Vector2 c = { tip.x - dx * head - px * head * 0.55f, tip.y - dy * head - py * head * 0.55f };
    DrawTriangle(tip, c, b, color);
}

static void DrawDots(int page)
{
    int y = 70;
    for (int i = 0; i < 3; i++) {
        int on = page == i;
        DrawCircle(WATCH_SIZE / 2 + (i - 1) * 28, y, on ? 7 : 4,
                   on ? (Color){ 110, 245, 210, 255 } : (Color){ 70, 100, 110, 255 });
    }
}

void Watch_Init(Watch* w)
{
    w->screen = LoadRenderTexture(WATCH_SIZE, WATCH_SIZE);
    SetTextureFilter(w->screen.texture, TEXTURE_FILTER_BILINEAR);
    w->ready = 1;
    w->page = 0;
    w->resetRequest = 0;
    w->clickWas = 0;
    w->prevStickX = 0.0f;
}

void Watch_Shutdown(Watch* w)
{
    if (!w->ready) return;
    UnloadRenderTexture(w->screen);
    w->ready = 0;
}

void Watch_UpdatePage(Watch* w, float stickX, int stickClick, int keyDelta, int forcePage)
{
    if (!w->ready) return;
    if (forcePage >= 0 && forcePage <= 2) {
        w->page = forcePage;
    } else {
        int step = 0;
        if (stickX > 0.65f && w->prevStickX <= 0.65f) step = 1;
        else if (stickX < -0.65f && w->prevStickX >= -0.65f) step = -1;
        if (keyDelta < 0) step = -1;
        else if (keyDelta > 0) step = 1;
        if (step != 0) w->page = (w->page + step + 3) % 3;
    }
    w->prevStickX = stickX;
    if (w->page == 2 && stickClick && !w->clickWas) w->resetRequest = 1;
    w->clickWas = stickClick;
}

int Watch_TakeReset(Watch* w)
{
    int v = w->resetRequest;
    w->resetRequest = 0;
    return v;
}

static void DrawFlight(const Balloon* balloon, float agl, float hspd, float course)
{
    Color cyan = { 110, 245, 210, 255 };
    Color dim = { 150, 184, 190, 255 };
    Color paper = { 236, 248, 246, 255 };
    Color up = { 130, 255, 186, 255 };
    Color down = { 255, 140, 110, 255 };
    Color gold = { 255, 206, 90, 255 };
    Color vcol = balloon->vy > 0.15f ? up : (balloon->vy < -0.15f ? down : paper);

    DrawText("FLIGHT", 36, 28, 28, cyan);
    const char* courseName = hspd > 0.25f ? Wind_Compass(course) : "--";
    char head[32];
    if (hspd > 0.25f) snprintf(head, sizeof(head), "%s  %03.0f", courseName, course * (180.0f / 3.14159265f));
    else snprintf(head, sizeof(head), "HOVER");
    int hw = MeasureText(head, 32);
    DrawText(head, WATCH_SIZE - 36 - hw, 28, 32, paper);

    int cx = WATCH_SIZE / 2;
    int cy = 196;
    DrawCircleLines(cx, cy, 78, cyan);
    DrawCircleLines(cx, cy, 74, cyan);
    DrawCircle(cx, cy, 5, cyan);
    DrawText("N", cx - 9, cy - 70, 26, paper);
    DrawText("E", cx + 56, cy - 12, 22, dim);
    DrawText("S", cx - 8, cy + 50, 22, dim);
    DrawText("W", cx - 78, cy - 12, 22, dim);
    if (hspd > 0.25f) DrawArrow(cx, cy, course, 58, 6.0f, gold);

    char line[64];
    snprintf(line, sizeof(line), "%.0f m", agl);
    DrawText(line, 40, 292, 64, paper);
    DrawText("ABOVE GROUND", 44, 360, 22, cyan);

    snprintf(line, sizeof(line), "%.1f m/s", hspd);
    DrawText(line, 40, 404, 40, paper);
    DrawText("HORIZONTAL", 44, 452, 20, cyan);

    snprintf(line, sizeof(line), "%+.1f m/s", balloon->vy);
    int vw = MeasureText(line, 40);
    DrawText(line, WATCH_SIZE - 40 - vw, 404, 40, vcol);
    const char* vlabel = "VERTICAL";
    int lw = MeasureText(vlabel, 20);
    DrawText(vlabel, WATCH_SIZE - 44 - lw, 452, 20, cyan);
}

static void DrawWind(const Balloon* balloon)
{
    Color cyan = { 110, 245, 210, 255 };
    Color dim = { 170, 198, 204, 255 };
    Color paper = { 236, 248, 246, 255 };
    Color gold = { 255, 206, 90, 255 };

    DrawText("WIND", 36, 26, 36, cyan);
    DrawText("m/s", WATCH_SIZE - 120, 36, 22, dim);

    const float bands[5] = { 100.0f, 50.0f, 0.0f, -50.0f, -100.0f };
    for (int i = 0; i < 5; i++) {
        int y = 88 + i * 72;
        float alt = balloon->altitude + bands[i];
        Color row = (bands[i] == 0.0f) ? paper : dim;
        if (bands[i] == 0.0f) DrawRectangle(28, y - 8, WATCH_SIZE - 56, 64, (Color){ 18, 48, 64, 255 });
        char label[16];
        if (bands[i] == 0.0f) snprintf(label, sizeof(label), "NOW");
        else snprintf(label, sizeof(label), "%+.0f", bands[i]);
        DrawText(label, 40, y + 8, 36, bands[i] == 0.0f ? cyan : row);
        if (alt < 2.0f) alt = 2.0f;
        WindSample s;
        Wind_AtAltitude(alt, &s);
        DrawText(Wind_Compass(s.bearing), 168, y + 8, 36, row);
        char spd[16];
        snprintf(spd, sizeof(spd), "%.1f", s.speed);
        DrawText(spd, 300, y + 6, 40, row);
        DrawArrow(430, y + 28, s.bearing, 52, 6.0f, bands[i] == 0.0f ? gold : row);
    }
}

static void DrawMenu(void)
{
    Color cyan = { 110, 245, 210, 255 };
    Color dim = { 170, 198, 204, 255 };
    Color paper = { 236, 248, 246, 255 };

    DrawText("MENU", 36, 26, 36, cyan);

    int bx = 48;
    int by = 168;
    int bw = WATCH_SIZE - 96;
    int bh = 168;
    DrawRectangle(bx, by, bw, bh, (Color){ 14, 58, 72, 255 });
    DrawRectangleLines(bx, by, bw, bh, cyan);
    DrawRectangleLines(bx + 3, by + 3, bw - 6, bh - 6, (Color){ 40, 110, 124, 255 });
    const char* label = "RESET VIEW";
    int lw = MeasureText(label, 42);
    DrawText(label, (WATCH_SIZE - lw) / 2, by + 46, 42, paper);
    const char* hint = "STICK CLICK";
    int hw = MeasureText(hint, 22);
    DrawText(hint, (WATCH_SIZE - hw) / 2, by + 108, 22, dim);

    const char* pages = "STICK LEFT / RIGHT";
    int pw = MeasureText(pages, 22);
    DrawText(pages, (WATCH_SIZE - pw) / 2, 400, 22, dim);
}

void Watch_Render(Watch* w, const Balloon* balloon, float groundY)
{
    if (!w->ready) return;

    float agl = balloon->altitude - groundY;
    if (agl < 0.0f) agl = 0.0f;
    float hspd = sqrtf(balloon->vx * balloon->vx + balloon->vz * balloon->vz);
    float course = atan2f(balloon->vx, balloon->vz);

    Color bg = { 6, 16, 26, 255 };
    Color panel = { 12, 32, 46, 255 };
    Color cyan = { 110, 245, 210, 255 };

    BeginTextureMode(w->screen);
    ClearBackground(bg);
    DrawRectangle(16, 16, WATCH_SIZE - 32, WATCH_SIZE - 32, panel);
    DrawRectangleLines(16, 16, WATCH_SIZE - 32, WATCH_SIZE - 32, cyan);
    if (w->page == 0) DrawFlight(balloon, agl, hspd, course);
    else if (w->page == 1) DrawWind(balloon);
    else DrawMenu();
    DrawDots(w->page);
    EndTextureMode();
}

static int g_watchVisible = 0;

static int FaceTowardEye(Vector3 handPos, Quaternion q, Vector3 eye)
{
    Vector3 back = Vector3RotateByQuaternion((Vector3){ 0, 1, 0 }, q);
    Vector3 forearm = Vector3RotateByQuaternion((Vector3){ 0, 0, 1 }, q);
    Vector3 toEye = Vector3Subtract(eye, handPos);
    float dist = Vector3Length(toEye);
    if (dist < 0.08f || dist > 0.95f) {
        g_watchVisible = 0;
        return 0;
    }
    toEye = Vector3Scale(toEye, 1.0f / dist);
    float facing = Vector3DotProduct(back, toEye);
    float tilt = fabsf(forearm.y);
    /* Forearm near horizontal, back of the hand toward the face.
     * A little hysteresis so it does not flicker on the threshold. */
    if (g_watchVisible) g_watchVisible = tilt < 0.52f && facing > 0.48f;
    else g_watchVisible = tilt < 0.36f && facing > 0.68f;
    return g_watchVisible;
}

void Watch_Draw(const Watch* w, Vector3 handPos, Quaternion q, Vector3 eye)
{
    if (!w->ready) return;
    if (!FaceTowardEye(handPos, q, eye)) return;

    Vector3 right = Vector3RotateByQuaternion((Vector3){ 1, 0, 0 }, q);
    Vector3 back = Vector3RotateByQuaternion((Vector3){ 0, 1, 0 }, q);
    Vector3 fingers = Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, q);

    Vector3 pos = Vector3Add(handPos, Vector3Add(Vector3Scale(back, 0.09f), Vector3Scale(fingers, -0.01f)));

    Matrix m = { 0 };
    m.m0 = right.x;   m.m1 = right.y;   m.m2 = right.z;
    m.m4 = fingers.x; m.m5 = fingers.y; m.m6 = fingers.z;
    m.m8 = back.x;    m.m9 = back.y;    m.m10 = back.z;
    m.m12 = pos.x;    m.m13 = pos.y;    m.m14 = pos.z;
    m.m15 = 1.0f;

    rlPushMatrix();
    float16 f = MatrixToFloatV(m);
    rlMultMatrixf(f.v);

    DrawCube((Vector3){ 0, 0, -0.008f }, 0.188f, 0.176f, 0.016f, (Color){ 28, 34, 40, 255 });
    DrawCube((Vector3){ 0, 0.108f, -0.004f }, 0.046f, 0.028f, 0.010f, (Color){ 18, 22, 28, 255 });
    DrawCube((Vector3){ 0, -0.108f, -0.004f }, 0.046f, 0.028f, 0.010f, (Color){ 18, 22, 28, 255 });

    rlSetTexture(w->screen.texture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    rlNormal3f(0, 0, 1);
    float s = 0.078f;
    rlTexCoord2f(0, 0); rlVertex3f(-s, -s, 0.002f);
    rlTexCoord2f(1, 0); rlVertex3f( s, -s, 0.002f);
    rlTexCoord2f(1, 1); rlVertex3f( s,  s, 0.002f);
    rlTexCoord2f(0, 1); rlVertex3f(-s,  s, 0.002f);
    rlEnd();
    rlSetTexture(0);
    rlPopMatrix();
}
