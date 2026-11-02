/* Web / desktop-only stand-in. The headset path stays in xr_session.c. */
#include "xr_session.h"

#include <string.h>

int XR_Init(void) { return 0; }
void XR_Shutdown(void) {}
int XR_IsSessionRunning(void) { return 0; }
int XR_WantsQuit(void) { return 0; }
void XR_Recenter(void) {}

int XR_BeginFrame(XrFrame* frame)
{
    if (frame) memset(frame, 0, sizeof(*frame));
    return 0;
}

void XR_EndFrame(void) {}
void XR_BeginEye(int eye) { (void)eye; }
void XR_EndEye(int eye) { (void)eye; }
