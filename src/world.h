/* Shared gondola dimensions. The tracked play space is this basket:
 * small on purpose, because the passenger never leaves the nacelle. */
#ifndef BALLOON_WORLD_H
#define BALLOON_WORLD_H

/* Interior half-extents of the wicker basket, in metres. */
#define BASKET_HALF_X 0.60f
#define BASKET_HALF_Z 0.50f
#define BASKET_RIM_Y 1.12f

/* Overhead frame the handles hang from. Reachable from a standing pose. */
#define FRAME_Y 2.16f

/* Handles hang just in front of the eyes, high enough to grab with a raised hand. */
#define HANDLE_RED_X (-0.22f)
#define HANDLE_GREEN_X 0.22f
#define HANDLE_Z (-0.34f)
#define HANDLE_REST_Y 1.82f
#define HANDLE_ANCHOR_Y 2.12f
#define HANDLE_MAX_PULL 0.48f

/* Pull past ON to light the burner / open the vent. Drop below OFF to stop.
 * The gap is hysteresis so a trembling hand does not flicker the flame. */
#define HANDLE_ACTIVATE_ON 0.12f
#define HANDLE_ACTIVATE_OFF 0.07f

/* Horizontal grab radius around the cable, and the distance that breaks a hold. */
#define HANDLE_GRAB_RADIUS 0.18f
#define HANDLE_BREAK_RADIUS 0.40f

/* Envelope, metres above the basket floor (the floor is y = 0 in basket space). */
#define ENVELOPE_TOP_Y 17.6f
#define ENVELOPE_MOUTH_Y 3.35f
#define ENVELOPE_RADIUS 7.4f

#endif
