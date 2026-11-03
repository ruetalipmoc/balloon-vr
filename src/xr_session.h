/* OpenXR session on top of the raylib window.
 * Room-scale (STAGE) when the runtime has a floor; LOCAL otherwise.
 * The tracked origin is recentered so the basket floor sits under the player. */
#ifndef BALLOON_XR_SESSION_H
#define BALLOON_XR_SESSION_H

#include "handles.h"

typedef struct XrFrame {
    int shouldRender;
    int eyeCount;
    float dt;
    HandInput hands[2];
    float eyeX[2];
    float eyeY[2];
    float eyeZ[2];
    float mirrorPx, mirrorPy, mirrorPz;
    float mirrorTx, mirrorTy, mirrorTz;
    float mirrorUx, mirrorUy, mirrorUz;
    float mirrorFovy;
} XrFrame;

/* Call after InitWindow. Returns 0 when no runtime / headset is available;
 * the desktop loop still runs. */
int XR_Init(void);
void XR_Shutdown(void);
int XR_IsSessionRunning(void);
int XR_WantsQuit(void);

/* Drop the next head pose onto a standing position in the basket:
 * centred, eyes 1.62 m above the floor. */
void XR_Recenter(void);

/* 1: xrBeginFrame was called and XR_EndFrame is required.
 * 0: session not running; render the desktop view instead. */
int XR_BeginFrame(XrFrame* frame);
void XR_EndFrame(void);
void XR_BeginEye(int eye);
void XR_EndEye(int eye);

#endif
