/* Hot-air balloon vertical channel and wind drift.
 *
 * A real envelope is so large that a few degrees of excess temperature
 * already produce several m/s of climb, and the burner is a blunt instrument.
 * We solve the neutral temperature from buoyancy, then drive a tuned excess
 * model around that point so holding a handle yields a few metres per second
 * rather than a rocket. */
#ifndef BALLOON_BALLOON_H
#define BALLOON_BALLOON_H

typedef struct Balloon {
    float x, z;          /* world metres, east / north */
    float altitude;      /* basket floor, world up */
    float vx, vz, vy;    /* m/s */
    float temperature;   /* celsius, air inside the envelope */
    float burner;        /* 0..1, commanded by the red handle */
    float vent;          /* 0..1, commanded by the green handle */
    int onGround;
} Balloon;

void Balloon_Reset(Balloon* b, float x, float z, float altitude);

/* groundY is the terrain under the basket. The basket cannot sink through it. */
void Balloon_Update(Balloon* b, float dt, float groundY);

/* Temperature at which buoyancy matches weight at this altitude. */
float Balloon_NeutralTemp(float altitude);

#endif
