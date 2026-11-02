/* Futuristic wrist instrument. The face is a render texture stuck to the
 * back of the left hand: course, speeds, altitude, and the winds above and
 * below the envelope. */
#ifndef BALLOON_WATCH_H
#define BALLOON_WATCH_H

#include "balloon.h"
#include "raylib.h"

typedef struct Watch {
    RenderTexture2D screen;
    int ready;
} Watch;

void Watch_Init(Watch* w);
void Watch_Shutdown(Watch* w);
void Watch_Render(Watch* w, const Balloon* balloon, float groundY);

/* Draw the casing in basket space. `q` is the left grip pose (see main.c):
 * +Y comes out of the back of the hand, -Z points forward. */
void Watch_Draw(const Watch* w, Vector3 handPos, Quaternion q);

#endif
