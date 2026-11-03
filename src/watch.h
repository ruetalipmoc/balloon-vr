/* Futuristic wrist instrument. The face stays hidden until the left forearm
 * is about horizontal and the back of the hand is turned toward the eyes.
 * Pages are flight, wind, then a menu. The left thumbstick changes page;
 * a stick click on the menu recenters the view. */
#ifndef BALLOON_WATCH_H
#define BALLOON_WATCH_H

#include "balloon.h"
#include "raylib.h"

typedef struct Watch {
    RenderTexture2D screen;
    int ready;
    int page; /* 0 flight, 1 wind, 2 menu */
    int resetRequest;
    int clickWas;
    float prevStickX;
} Watch;

void Watch_Init(Watch* w);
void Watch_Shutdown(Watch* w);

/* `forcePage` is 0..2 to pin a page, or -1 to follow the left stick.
 * `keyDelta` is -1, 0 or +1 from a desktop key press this frame. */
void Watch_UpdatePage(Watch* w, float stickX, int stickClick, int keyDelta, int forcePage);

/* 1 once, when the menu's reset button was pressed. */
int Watch_TakeReset(Watch* w);

void Watch_Render(Watch* w, const Balloon* balloon, float groundY);

/* Draws nothing unless the forearm is horizontal and the back of the hand
 * points at `eye`. `q` is the left grip pose: +Y comes out of the back of
 * the hand, -Z points toward the fingers, +Z runs back along the forearm. */
void Watch_Draw(const Watch* w, Vector3 handPos, Quaternion q, Vector3 eye);

#endif