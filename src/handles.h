/* Two overhead handles on cables. While a handle is pulled down past a
 * threshold the matching system stays on: red fires the burner, green opens
 * the crown vent. Either hand can hold either handle. */
#ifndef BALLOON_HANDLES_H
#define BALLOON_HANDLES_H

#define HANDLE_BURNER 0
#define HANDLE_VENT 1
#define HANDLE_COUNT 2

typedef struct HandInput {
    int active;          /* pose is valid this frame */
    int grip;            /* squeeze is held */
    float stickX;        /* -1..1, thumbstick or trackpad */
    float stickY;
    int stickClick;      /* thumbstick / trackpad press */
    int trigger;         /* index trigger, used to confirm the watch menu */
    int burnButton;      /* right-hand Y / B, held to burn */
    int ventButton;      /* right-hand X / A, held to vent */
    float x, y, z;       /* metres, basket space */
    float qx, qy, qz, qw; /* orientation, identity if unused */
} HandInput;

typedef struct HandleState {
    float restX, restY, restZ;
    float anchorX, anchorY, anchorZ;
    float pull;          /* metres downward from rest */
    float pullVel;
    int heldBy;          /* -1, or 0 / 1 for left / right hand */
    int active;          /* past the pull threshold */
} HandleState;

typedef struct Handles {
    HandleState h[HANDLE_COUNT];
} Handles;

void Handles_Init(Handles* hs);
void Handles_Update(Handles* hs, const HandInput hands[2], float dt);

/* World-in-basket position of the grip after the current pull. */
void Handle_GripPosition(const HandleState* h, float* x, float* y, float* z);

#endif
