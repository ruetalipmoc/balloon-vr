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
    DrawCircle(WATCH_SIZE / 2 - 14, y, page == 0 ? 7 : 4, page == 0 ? (Color){ 110, 245, 210, 255 } : (Color){ 70, 100, 110, 255 });
    DrawCircle(WATCH_SIZE / 2 + 14, y, page == 1 ? 7 : 4, page == 1 ? (Color){ 110, 245, 210, 255 } : (Color){ 70, 100, 110, 255 });
}

void Watch_Init(Watch* w)
{
    w->screen = LoadRenderTexture(WATCH_SIZE, WATCH_SIZE);
    SetTextureFilter(w->screen.texture, TEXTURE_FILTER_BILINEAR);
    w->ready = 1;
    w->page = 0;
}

void Watch_Shutdown(Watch* w)
{
    if (!w->ready) return;
    UnloadRenderTexture(w->screen);
    w->ready = 0;
}

void Watch_UpdatePage(Watch* w, Quaternion q, int forcePage)
{
    if (!w->ready) return;
    if (forcePage == 0 || forcePage == 1) {
        w->page = forcePage;
        return;
    }
    /* 12 o'clock is -Z of the grip. Upright keeps the flight page; rolling
     * the wrist so the face is sideways opens the wind page. */
    Vector3 fingers = Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, q);
    if (w->page == 0 && fingers.y < 0.20f) w->page = 1;
    else if (w->page == 1 && fingers.y > 0.72f) w->page = 0;
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
    else DrawWind(balloon);
    DrawDots(w->page);
    EndTextureMode();
}

static int FaceTowardEye(Vector3 handPos, Quaternion q, Vector3 eye)
{
    Vector3 back = Vector3RotateByQuaternion((Vector3){ 0, 1, 0 }, q);
    Vector3 toEye = Vector3Subtract(eye, handPos);
    float dist = Vector3Length(toEye);
    if (dist < 0.05f || dist > 0.85f) return 0;
    toEye = Vector3Scale(toEye, 1.0f / dist);
    return Vector3DotProduct(back, toEye) > 0.78f;
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
