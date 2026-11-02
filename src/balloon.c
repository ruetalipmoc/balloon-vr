#include "balloon.h"

#include "sim_math.h"
#include "wind.h"

#include <math.h>

/* Envelope and all-up mass (wicker, burner, one passenger). Chosen so the
 * neutral temperature sits a little above a cool morning. */
#define BALLOON_VOLUME 2800.0f
#define BALLOON_MASS 260.0f

/* How hard excess temperature accelerates the balloon, and quadratic drag
 * that caps climb / descent around a brisk real-world rate. */
#define LIFT_PER_DEGREE 0.16f
#define VERTICAL_DRAG 0.11f

/* Temperature rates, degrees per second. */
#define BURNER_HEAT 16.0f
#define VENT_COOL 22.0f
#define LEAK_RATE 0.055f
#define MAX_EXCESS 36.0f

float Balloon_NeutralTemp(float altitude)
{
    float alt = altitude;
    if (alt < 0.0f) alt = 0.0f;
    /* ISA-ish ambient lapse, clamped so the formula stays sane very high up. */
    float tamb = 15.0f - alt * 0.0065f;
    if (tamb < -50.0f) tamb = -50.0f;
    float rho = 1.225f * expf(-alt / 8500.0f);
    /* (rho - rho_inside) * V = mass  =>  rho_inside/rho = 1 - mass/(rho*V). */
    float ratio = 1.0f - BALLOON_MASS / (rho * BALLOON_VOLUME);
    if (ratio < 0.55f) ratio = 0.55f;
    float tKin = (tamb + 273.15f) / ratio;
    return tKin - 273.15f;
}

void Balloon_Reset(Balloon* b, float x, float z, float altitude)
{
    b->x = x;
    b->z = z;
    b->altitude = altitude;
    b->vx = 0.0f;
    b->vz = 0.0f;
    b->vy = 0.0f;
    b->burner = 0.0f;
    b->vent = 0.0f;
    b->onGround = 0;
    b->temperature = Balloon_NeutralTemp(altitude);
}

void Balloon_Update(Balloon* b, float dt, float groundY)
{
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;

    float tamb = 15.0f - b->altitude * 0.0065f;
    if (tamb < -50.0f) tamb = -50.0f;
    float tHover = Balloon_NeutralTemp(b->altitude);

    float heat = b->burner * BURNER_HEAT - b->vent * VENT_COOL;
    heat -= (b->temperature - tamb) * LEAK_RATE;
    b->temperature += heat * dt;

    float tMin = tamb;
    float tMax = tHover + MAX_EXCESS;
    if (tMax < tMin + 5.0f) tMax = tMin + 5.0f;
    b->temperature = Clampf(b->temperature, tMin, tMax);

    float excess = b->temperature - tHover;
    float liftAccel = excess * LIFT_PER_DEGREE;
    float drag = VERTICAL_DRAG * b->vy * fabsf(b->vy);
    b->vy += (liftAccel - drag) * dt;

    WindSample wind;
    Wind_AtAltitude(b->altitude, &wind);
    /* The envelope is a huge sail: horizontal velocity relaxes toward the
     * wind over a few seconds rather than matching it instantly. */
    float follow = 0.28f;
    b->vx += (wind.x - b->vx) * follow * dt;
    b->vz += (wind.z - b->vz) * follow * dt;

    b->altitude += b->vy * dt;
    b->x += b->vx * dt;
    b->z += b->vz * dt;

    if (b->altitude <= groundY) {
        b->altitude = groundY;
        if (b->vy < 0.0f) b->vy = 0.0f;
        /* Wicker on the ground does not ice-skate with the wind. */
        float stick = Minf(1.0f, 3.0f * dt);
        b->vx *= (1.0f - stick);
        b->vz *= (1.0f - stick);
        b->onGround = 1;
    } else {
        b->onGround = 0;
    }
}
