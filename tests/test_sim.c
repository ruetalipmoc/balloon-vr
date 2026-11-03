/* Physics and terrain checks. No window, no OpenXR. */
#include "../src/balloon.h"
#include "../src/handles.h"
#include "../src/terrain.h"
#include "../src/wind.h"
#include "../src/world.h"
#include "../src/sim_math.h"

#include <math.h>
#include <stdio.h>

static int g_fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_fails++; \
    } \
} while (0)

static int Finite(float v) { return v == v && v < 1.0e20f && v > -1.0e20f; }

static void TestWind(void)
{
    WindSample low, high;
    Wind_AtAltitude(0.0f, &low);
    Wind_AtAltitude(400.0f, &high);
    CHECK(low.speed > 0.5f && low.speed < 30.0f);
    CHECK(high.speed > 0.5f && high.speed < 30.0f);
    CHECK(Finite(low.x) && Finite(low.z) && Finite(high.bearing));
    float delta = fabsf(low.x - high.x) + fabsf(low.z - high.z);
    CHECK(delta > 0.2f);
    CHECK(Wind_Compass(0.0f)[0] == 'N');
    CHECK(Wind_Compass(1.5707963f)[0] == 'E');
}

static void TestSeams(void)
{
    float size = TERRAIN_CHUNK_SIZE;
    /* Same LOD on both sides of x = 64: the border is the raw height field. */
    TerrainSample same = Terrain_Sample(16, 3, 16, 0.0f, 0.0f, size, 0, 0, 32.0f, 32.0f);
    CHECK(!same.snapped);
    CHECK(fabsf(same.y - Terrain_Height(64.0f, 12.0f)) < 1.0e-3f);

    /* View sits so chunk (0,0) is LOD 0 and chunk (1,0) is LOD 1. */
    float viewX = -20.0f;
    float viewZ = 32.0f;
    CHECK(Terrain_GridForLod(Terrain_LodForDistance(hypotf(32.0f - viewX, 0.0f))) == 16);
    CHECK(Terrain_GridForLod(Terrain_LodForDistance(hypotf(96.0f - viewX, 0.0f))) == 8);
    CHECK(Terrain_GridForLod(0) == 16);
    CHECK(Terrain_GridForLod(1) == 8);
    CHECK(Terrain_GridForLod(2) == 4);
    CHECK(Terrain_GridForLod(3) == 2);
    CHECK(Terrain_GridForLod(4) == 1);
    CHECK(Terrain_LodForDistance(150.0f) == 1);
    CHECK(Terrain_LodForDistance(260.0f) == 2);
    CHECK(Terrain_LodForDistance(400.0f) == 3);
    CHECK(Terrain_LodForDistance(640.0f) == 4);

    TerrainSample fine = Terrain_Sample(16, 1, 16, 0.0f, 0.0f, size, 0, 0, viewX, viewZ);
    float expect = Lerpf(Terrain_Height(64.0f, 0.0f), Terrain_Height(64.0f, 8.0f), 0.5f);
    CHECK(fine.snapped);
    CHECK(fabsf(fine.y - expect) < 1.0e-3f);

    /* The coarser chunk keeps its own samples; shared vertices agree. */
    TerrainSample coarse = Terrain_Sample(0, 1, 8, size, 0.0f, size, 1, 0, viewX, viewZ);
    CHECK(!coarse.snapped);
    CHECK(fabsf(coarse.y - Terrain_Height(64.0f, 8.0f)) < 1.0e-3f);
    TerrainSample shared = Terrain_Sample(16, 2, 16, 0.0f, 0.0f, size, 0, 0, viewX, viewZ);
    CHECK(!shared.snapped);
    CHECK(fabsf(shared.y - coarse.y) < 1.0e-3f);
}

static void TestTerrain(void)
{
    float a = Terrain_Height(2400.0f, 1800.0f);
    float b = Terrain_Height(2400.0f, 1800.0f);
    CHECK(a == b);
    CHECK(a >= 0.0f);
    CHECK(Terrain_GroundAt(10.0f, -30.0f) >= Terrain_Height(10.0f, -30.0f) - 0.01f);
    float nx, ny, nz;
    Terrain_Normal(100.0f, 200.0f, &nx, &ny, &nz);
    CHECK(ny > 0.0f);
    CHECK(fabsf(nx * nx + ny * ny + nz * nz - 1.0f) < 0.02f);
    float moisture = Terrain_Moisture(0.0f, 0.0f);
    CHECK(moisture >= 0.0f && moisture <= 1.0f);

    int count[5] = { 0, 0, 0, 0, 0 };
    int wet = 0;
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    for (int iz = -30; iz <= 30; iz++) {
        for (int ix = -30; ix <= 30; ix++) {
            float x = (float)ix * 96.0f;
            float z = (float)iz * 96.0f;
            float h = Terrain_Height(x, z);
            if (h < lo) lo = h;
            if (h > hi) hi = h;
            if (h < TERRAIN_WATER) wet++;
            int zone = Terrain_Zone(x, z);
            CHECK(zone >= TERRAIN_ZONE_PLAINS && zone <= TERRAIN_ZONE_CITY);
            count[zone]++;
        }
    }
    int kinds = 0;
    for (int i = 0; i < 5; i++) if (count[i] > 8) kinds++;
    if (kinds < 4 || wet < 8 || hi - lo < 22.0f) {
        fprintf(stderr, "zones plains %d farm %d forest %d highland %d city %d  wet %d  relief %.1f\n",
                count[0], count[1], count[2], count[3], count[4], wet, hi - lo);
    }
    CHECK(kinds >= 4);
    CHECK(wet > 8);
    CHECK(hi - lo > 22.0f);
}

static void TestBalloon(void)
{
    Balloon balloon;
    Balloon_Reset(&balloon, 0.0f, 0.0f, 200.0f);
    float start = balloon.altitude;
    balloon.burner = 1.0f;
    for (int i = 0; i < 400; i++) Balloon_Update(&balloon, 0.02f, -1000.0f);
    CHECK(balloon.altitude > start + 8.0f);
    CHECK(Finite(balloon.temperature));
    CHECK(Finite(balloon.vy));

    Balloon_Reset(&balloon, 0.0f, 0.0f, 200.0f);
    start = balloon.altitude;
    balloon.vent = 1.0f;
    for (int i = 0; i < 400; i++) Balloon_Update(&balloon, 0.02f, -1000.0f);
    CHECK(balloon.altitude < start - 8.0f);

    Balloon_Reset(&balloon, 12.0f, -4.0f, 80.0f);
    for (int i = 0; i < 1500; i++) Balloon_Update(&balloon, 0.02f, -50.0f);
    CHECK(Finite(balloon.altitude));
    CHECK(Finite(balloon.vx) && Finite(balloon.vz));
    CHECK(balloon.altitude >= -50.0f);
}

static void TestHandles(void)
{
    Handles handles;
    Handles_Init(&handles);
    HandInput hands[2];
    for (int i = 0; i < 2; i++) {
        hands[i].active = 0;
        hands[i].grip = 0;
        hands[i].x = hands[i].y = hands[i].z = 0;
        hands[i].qx = hands[i].qy = hands[i].qz = 0;
        hands[i].qw = 1;
    }
    Handles_Update(&handles, hands, 0.016f);
    CHECK(!handles.h[HANDLE_BURNER].active);

    hands[1].active = 1;
    hands[1].grip = 1;
    hands[1].x = HANDLE_RED_X;
    hands[1].y = HANDLE_REST_Y - 0.30f;
    hands[1].z = HANDLE_Z;
    Handles_Update(&handles, hands, 0.016f);
    CHECK(handles.h[HANDLE_BURNER].heldBy == 1);
    CHECK(handles.h[HANDLE_BURNER].active);
    CHECK(handles.h[HANDLE_BURNER].pull > 0.2f);
    CHECK(!handles.h[HANDLE_VENT].active);

    hands[1].grip = 0;
    for (int i = 0; i < 200; i++) Handles_Update(&handles, hands, 0.016f);
    CHECK(handles.h[HANDLE_BURNER].heldBy == -1);
    CHECK(handles.h[HANDLE_BURNER].pull < 0.02f);
    CHECK(!handles.h[HANDLE_BURNER].active);
}

static void TestLodConfig(void)
{
    Terrain_SetLodThresholds(50.0f, 80.0f, 120.0f, 200.0f);
    CHECK(Terrain_LodForDistance(40.0f) == 0);
    CHECK(Terrain_LodForDistance(60.0f) == 1);
    CHECK(Terrain_LodForDistance(100.0f) == 2);
    CHECK(Terrain_LodForDistance(150.0f) == 3);
    CHECK(Terrain_LodForDistance(250.0f) == 4);

    float small = Terrain_SetViewDistance(10.0f);
    CHECK(fabsf(small - TERRAIN_CHUNK_SIZE) < 0.1f);
    float huge = Terrain_SetViewDistance(10000.0f);
    CHECK(fabsf(huge - TERRAIN_VIEW_MAX) < 0.1f);
    float back = Terrain_SetViewDistance(TERRAIN_VIEW_DEFAULT);
    CHECK(fabsf(back - TERRAIN_VIEW_DEFAULT) < 0.1f);
    CHECK(fabsf(Terrain_ViewDistance() - TERRAIN_VIEW_DEFAULT) < 0.1f);

    /* Restore the bands the seam test relies on. */
    Terrain_SetLodThresholds(100.0f, 200.0f, 340.0f, 520.0f);
    CHECK(Terrain_LodForDistance(150.0f) == 1);
    CHECK(Terrain_LodForDistance(640.0f) == 4);
}

static void TestAltitude(void)
{
    Terrain_SetViewDistance(TERRAIN_VIEW_DEFAULT);
    Terrain_SetLodThresholds(100.0f, 200.0f, 340.0f, 520.0f);

    Terrain_ApplyAltitude(10.0f);
    CHECK(fabsf(Terrain_ViewDistance() - TERRAIN_VIEW_DEFAULT) < 0.1f);
    CHECK(fabsf(Terrain_LodThreshold(0) - 100.0f) < 0.1f);
    CHECK(Terrain_LodForDistance(90.0f) == 0);
    CHECK(Terrain_LodForDistance(150.0f) == 1);

    Terrain_ApplyAltitude(TERRAIN_ALT_REF);
    CHECK(fabsf(Terrain_LodThreshold(3) - 520.0f) < 0.1f);

    Terrain_ApplyAltitude(80.0f);
    CHECK(Terrain_ViewDistance() > TERRAIN_VIEW_DEFAULT + 32.0f);
    CHECK(Terrain_LodThreshold(0) > 100.0f);
    CHECK(Terrain_LodThreshold(1) > Terrain_LodThreshold(0));
    CHECK(Terrain_LodThreshold(3) > Terrain_LodThreshold(2));
    CHECK(Terrain_LodForDistance(Terrain_LodThreshold(0) - 1.0f) == 0);
    CHECK(Terrain_LodForDistance(Terrain_LodThreshold(0) + 1.0f) == 1);

    Terrain_ApplyAltitude(360.0f);
    CHECK(Terrain_LodThreshold(0) > 400.0f);
    CHECK(Terrain_LodThreshold(0) <= 512.0f + 0.1f);
    /* The look over the rail has to stay on the 8 m mesh, or streets and
     * hedges turn into diamonds. */
    CHECK(Terrain_LodThreshold(1) > 900.0f);

    Terrain_ApplyAltitude(800.0f);
    CHECK(fabsf(Terrain_ViewDistance() - TERRAIN_VIEW_MAX) < 0.1f);
    CHECK(Terrain_LodThreshold(0) <= 512.0f + 0.1f);
    CHECK(Terrain_LodThreshold(1) > Terrain_LodThreshold(0));
    CHECK(Terrain_LodThreshold(3) >= Terrain_LodThreshold(2));

    /* Back to the reference, so later checks see the typed distances. */
    Terrain_ApplyAltitude(0.0f);
    CHECK(fabsf(Terrain_LodThreshold(0) - 100.0f) < 0.1f);
    CHECK(fabsf(Terrain_ViewDistance() - TERRAIN_VIEW_DEFAULT) < 0.1f);
}

int main(void)
{
    TestWind();
    TestTerrain();
    TestSeams();
    TestBalloon();
    TestHandles();
    TestLodConfig();
    TestAltitude();
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    printf("sim checks passed\n");
    return 0;
}
