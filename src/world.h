/* Shared gondola dimensions. The tracked play space is this basket:
 * small on purpose, because the passenger never leaves the nacelle. */
#ifndef BALLOON_WORLD_H
#define BALLOON_WORLD_H

/* Interior of the wicker, in metres. Wide enough to turn around,
 * and the frame above clears a 2.10 m passenger. */
#define BASKET_HALF_X 0.82f
#define BASKET_HALF_Z 0.68f
#define BASKET_RIM_Y 1.12f

/* Beam centre. The 5 cm timber leaves about 2.50 m under it. */
#define FRAME_Y 2.55f

/* Handles hang from the middle of the frame, so either one can be reached
 * from anywhere in the basket, facing any way. Red is left, green is right. */
#define HANDLE_RED_X (-0.18f)
#define HANDLE_GREEN_X 0.18f
#define HANDLE_Z (-0.16f)
#define HANDLE_REST_Y 2.02f
#define HANDLE_ANCHOR_Y 2.46f
#define HANDLE_MAX_PULL 0.46f

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
