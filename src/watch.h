/* Futuristic wrist instrument. The face stays hidden until the left forearm
 * is about horizontal and the back of the hand is turned toward the eyes.
 * That test uses only the wrist orientation, so it works anywhere in the basket.
 * Pages are flight, wind, then a menu. The left thumbstick changes page;
 * the trigger confirms the menu. */
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

/* `confirm` is the trigger (or a desktop key) on the menu page. */
void Watch_UpdatePage(Watch* w, float stickX, int confirm, int keyDelta, int forcePage);

/* 1 once, when the menu's reset button was pressed. */
int Watch_TakeReset(Watch* w);

void Watch_Render(Watch* w, const Balloon* balloon, float groundY);

/* Draws nothing unless the forearm is horizontal and the back of the hand
 * points at `eye`. `q` is the left grip pose: +Y comes out of the back of
 * the hand, -Z points toward the fingers, +Z runs back along the forearm. */
void Watch_Draw(const Watch* w, Vector3 handPos, Quaternion q, Vector3 eye);

#endif