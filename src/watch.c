#include "watch.h"

#include "terrain.h"
#include "wind.h"

#include "rlgl.h"
#include "raymath.h"

#include <math.h>
#include <stdio.h>

#define WATCH_SIZE 512

static void DrawArrow(int x, int y, float bearing, int len, Color color)
{
    float dx = sinf(bearing);
    float dy = -cosf(bearing); /* screen y grows downward; north is up */
    int x2 = x + (int)(dx * (float)len);
    int y2 = y + (int)(dy * (float)len);
    DrawLineEx((Vector2){ (float)x, (float)y }, (Vector2){ (float)x2, (float)y2 }, 3.0f, color);
    float px = -dy;
    float py = dx;
    Vector2 tip = { (float)x2, (float)y2 };
    Vector2 a = { tip.x - dx * 10.0f + px * 6.0f, tip.y - dy * 10.0f + py * 6.0f };
    Vector2 b = { tip.x - dx * 10.0f - px * 6.0f, tip.y - dy * 10.0f - py * 6.0f };
    DrawTriangle(tip, b, a, color);
}

void Watch_Init(Watch* w)
{
    w->screen = LoadRenderTexture(WATCH_SIZE, WATCH_SIZE);
    SetTextureFilter(w->screen.texture, TEXTURE_FILTER_BILINEAR);
    w->ready = 1;
}

void Watch_Shutdown(Watch* w)
{
    if (!w->ready) return;
    UnloadRenderTexture(w->screen);
    w->ready = 0;
}

void Watch_Render(Watch* w, const Balloon* balloon, float groundY)
{
    if (!w->ready) return;

    float agl = balloon->altitude - groundY;
    if (agl < 0.0f) agl = 0.0f;
    float hspd = sqrtf(balloon->vx * balloon->vx + balloon->vz * balloon->vz);
    float course = atan2f(balloon->vx, balloon->vz);
    const char* courseName = hspd > 0.25f ? Wind_Compass(course) : "--";

    Color bg = { 6, 16, 26, 255 };
    Color panel = { 12, 32, 46, 255 };
    Color cyan = { 110, 245, 210, 255 };
    Color dim = { 150, 184, 190, 255 };
    Color paper = { 232, 246, 244, 255 };
    Color up = { 130, 255, 186, 255 };
    Color down = { 255, 130, 96, 255 };
    Color vcol = balloon->vy > 0.15f ? up : (balloon->vy < -0.15f ? down : paper);

    BeginTextureMode(w->screen);
    ClearBackground(bg);
    DrawRectangle(18, 18, WATCH_SIZE - 36, WATCH_SIZE - 36, panel);
    DrawRectangleLines(18, 18, WATCH_SIZE - 36, WATCH_SIZE - 36, cyan);

    DrawText("GONDOLA", 36, 32, 28, cyan);
    char head[32];
    if (hspd > 0.25f) snprintf(head, sizeof(head), "%s  %03.0f", courseName, course * (180.0f / 3.14159265f));
    else snprintf(head, sizeof(head), "HOVER");
    int hw = MeasureText(head, 28);
    DrawText(head, WATCH_SIZE - 36 - hw, 32, 28, paper);

    int cx = WATCH_SIZE / 2;
    int cy = 168;
    DrawCircleLines(cx, cy, 78, cyan);
    DrawCircleLines(cx, cy, 4, cyan);
    DrawText("N", cx - 8, cy - 74, 22, paper);
    DrawText("E", cx + 60, cy - 8, 18, dim);
    DrawText("S", cx - 6, cy + 52, 18, dim);
    DrawText("W", cx - 74, cy - 8, 18, dim);
    if (hspd > 0.25f) DrawArrow(cx, cy, course, 58, (Color){ 255, 196, 80, 255 });

    char line[64];
    snprintf(line, sizeof(line), "AGL  %5.0f m", agl);
    DrawText(line, 48, 262, 34, paper);
    snprintf(line, sizeof(line), "ALT  %5.0f m", balloon->altitude);
    DrawText(line, 48, 302, 26, dim);
    snprintf(line, sizeof(line), "HSPD %4.1f m/s", hspd);
    DrawText(line, 48, 338, 30, paper);
    snprintf(line, sizeof(line), "VSPD %+4.1f", balloon->vy);
    DrawText(line, 48, 376, 30, vcol);

    DrawText("WIND", 48, 408, 20, cyan);
    const float bands[4] = { -50.0f, 0.0f, 50.0f, 100.0f };
    for (int i = 0; i < 4; i++) {
        float alt = balloon->altitude + bands[i];
        int y = 432 + i * 16;
        if (alt < 0.0f) {
            DrawText("----", 48, y, 16, dim);
            continue;
        }
        WindSample s;
        Wind_AtAltitude(alt, &s);
        const char* tag = (bands[i] == 0.0f) ? "NOW" : NULL;
        char label[16];
        if (tag) snprintf(label, sizeof(label), "NOW");
        else snprintf(label, sizeof(label), "%+3.0f", bands[i]);
        Color row = (bands[i] == 0.0f) ? paper : dim;
        DrawText(label, 48, y, 16, row);
        DrawText(Wind_Compass(s.bearing), 130, y, 16, row);
        snprintf(line, sizeof(line), "%4.1f", s.speed);
        DrawText(line, 190, y, 16, row);
        DrawArrow(300, y + 6, s.bearing, 18, row);
    }
    EndTextureMode();
}

void Watch_Draw(const Watch* w, Vector3 handPos, Quaternion q)
{
    if (!w->ready) return;

    /* Grip convention shared with the desktop wrist: +Y out of the back of
     * the hand (the screen normal), -Z toward the fingers (12 o'clock). */
    Vector3 right = Vector3RotateByQuaternion((Vector3){ 1, 0, 0 }, q);
    Vector3 back = Vector3RotateByQuaternion((Vector3){ 0, 1, 0 }, q);
    Vector3 fingers = Vector3RotateByQuaternion((Vector3){ 0, 0, -1 }, q);

    /* Clear of the mitten sphere (radius ~4 cm) so the face is readable. */
    Vector3 pos = Vector3Add(handPos, Vector3Add(Vector3Scale(back, 0.075f), Vector3Scale(fingers, -0.02f)));

    Matrix m = { 0 };
    m.m0 = right.x;   m.m1 = right.y;   m.m2 = right.z;
    m.m4 = fingers.x; m.m5 = fingers.y; m.m6 = fingers.z;
    m.m8 = back.x;    m.m9 = back.y;    m.m10 = back.z;
    m.m12 = pos.x;    m.m13 = pos.y;    m.m14 = pos.z;
    m.m15 = 1.0f;

    rlPushMatrix();
    float16 f = MatrixToFloatV(m);
    rlMultMatrixf(f.v);

    /* Local +Z is out of the screen. A wide slab, so the readouts stay large. */
    DrawCube((Vector3){ 0, 0, -0.007f }, 0.118f, 0.108f, 0.014f, (Color){ 28, 34, 40, 255 });
    DrawCube((Vector3){ 0, 0.068f, -0.004f }, 0.036f, 0.030f, 0.008f, (Color){ 18, 22, 28, 255 });
    DrawCube((Vector3){ 0, -0.068f, -0.004f }, 0.036f, 0.030f, 0.008f, (Color){ 18, 22, 28, 255 });
    DrawCube((Vector3){ 0.050f, 0.040f, 0.001f }, 0.010f, 0.010f, 0.004f, (Color){ 80, 255, 210, 255 });

    /* v = 0 is the top of this render texture, matching how the face was drawn. */
    rlSetTexture(w->screen.texture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    rlNormal3f(0, 0, 1);
    float s = 0.048f;
    rlTexCoord2f(0, 0); rlVertex3f(-s, -s, 0.002f);
    rlTexCoord2f(1, 0); rlVertex3f( s, -s, 0.002f);
    rlTexCoord2f(1, 1); rlVertex3f( s,  s, 0.002f);
    rlTexCoord2f(0, 1); rlVertex3f(-s,  s, 0.002f);
    rlEnd();
    rlSetTexture(0);
    rlPopMatrix();
}
