/* Futuristic wrist instrument. The face stays hidden until the back of the
 * left hand is turned toward the eyes. Flight numbers are one page; the
 * winds aloft are the next, opened by rolling the wrist (or V on desktop). */
#ifndef BALLOON_WATCH_H
#define BALLOON_WATCH_H

#include "balloon.h"
#include "raylib.h"

typedef struct Watch {
    RenderTexture2D screen;
    int ready;
    int page; /* 0 flight, 1 wind */
} Watch;

void Watch_Init(Watch* w);
void Watch_Shutdown(Watch* w);

/* `forcePage` is 0 or 1 to pin a page, or -1 to follow the wrist roll. */
void Watch_UpdatePage(Watch* w, Quaternion q, int forcePage);

void Watch_Render(Watch* w, const Balloon* balloon, float groundY);

/* Draws nothing unless the face points at `eye`. `q` is the left grip pose:
 * +Y comes out of the back of the hand, -Z points toward the fingers. */
void Watch_Draw(const Watch* w, Vector3 handPos, Quaternion q, Vector3 eye);

#endif
