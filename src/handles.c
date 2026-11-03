#include "handles.h"

#include "sim_math.h"
#include "world.h"

#include <math.h>
#include <string.h>

void Handles_Init(Handles* hs)
{
    memset(hs, 0, sizeof(*hs));

    hs->h[HANDLE_BURNER].restX = HANDLE_RED_X;
    hs->h[HANDLE_BURNER].restY = HANDLE_REST_Y;
    hs->h[HANDLE_BURNER].restZ = HANDLE_Z;
    hs->h[HANDLE_BURNER].anchorX = HANDLE_RED_X;
    hs->h[HANDLE_BURNER].anchorY = HANDLE_ANCHOR_Y;
    hs->h[HANDLE_BURNER].anchorZ = HANDLE_Z;

    hs->h[HANDLE_VENT].restX = HANDLE_GREEN_X;
    hs->h[HANDLE_VENT].restY = HANDLE_REST_Y;
    hs->h[HANDLE_VENT].restZ = HANDLE_Z;
    hs->h[HANDLE_VENT].anchorX = HANDLE_GREEN_X;
    hs->h[HANDLE_VENT].anchorY = HANDLE_ANCHOR_Y;
    hs->h[HANDLE_VENT].anchorZ = HANDLE_Z;

    hs->h[HANDLE_BURNER].heldBy = -1;
    hs->h[HANDLE_VENT].heldBy = -1;
}

void Handle_GripPosition(const HandleState* h, float* x, float* y, float* z)
{
    *x = h->restX;
    *y = h->restY - h->pull;
    *z = h->restZ;
}

static void SpringBack(HandleState* h, float dt)
{
    /* Critically-ish damped so a released handle returns without bouncing
     * through the activation threshold over and over. */
    const float stiffness = 70.0f;
    const float damping = 14.0f;
    float acc = -stiffness * h->pull - damping * h->pullVel;
    h->pullVel += acc * dt;
    h->pull += h->pullVel * dt;
    if (h->pull < 0.0f) {
        h->pull = 0.0f;
        h->pullVel = 0.0f;
    }
    if (h->pull > HANDLE_MAX_PULL) {
        h->pull = HANDLE_MAX_PULL;
        h->pullVel = 0.0f;
    }
}

static void RefreshActive(HandleState* h)
{
    if (!h->active && h->pull >= HANDLE_ACTIVATE_ON) h->active = 1;
    else if (h->active && h->pull <= HANDLE_ACTIVATE_OFF) h->active = 0;
}

void Handles_Update(Handles* hs, const HandInput hands[2], float dt)
{
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;

    /* A grip that is already down on the first tracked pose does not count.
     * After it has been released once, holding it and moving onto a cable
     * grabs as before. */
    static int seen[2] = { 0, 0 };
    static int needRelease[2] = { 0, 0 };

    for (int handIndex = 0; handIndex < 2; handIndex++) {
        if (!hands[handIndex].active) continue;
        if (!seen[handIndex]) {
            seen[handIndex] = 1;
            needRelease[handIndex] = hands[handIndex].grip ? 1 : 0;
        } else if (!hands[handIndex].grip) {
            needRelease[handIndex] = 0;
        }
    }

    for (int i = 0; i < HANDLE_COUNT; i++) {
        HandleState* h = &hs->h[i];

        if (h->heldBy >= 0) {
            const HandInput* hand = &hands[h->heldBy];
            if (!hand->active || !hand->grip) {
                h->heldBy = -1;
            } else {
                float dx = hand->x - h->restX;
                float dz = hand->z - h->restZ;
                if (dx * dx + dz * dz > HANDLE_BREAK_RADIUS * HANDLE_BREAK_RADIUS) {
                    h->heldBy = -1;
                } else {
                    h->pull = Clampf(h->restY - hand->y, 0.0f, HANDLE_MAX_PULL);
                    h->pullVel = 0.0f;
                }
            }
        }

        if (h->heldBy < 0) {
            for (int handIndex = 0; handIndex < 2; handIndex++) {
                const HandInput* hand = &hands[handIndex];
                if (!hand->active || !hand->grip || needRelease[handIndex]) continue;
                /* Already holding the other handle. */
                int busy = 0;
                for (int k = 0; k < HANDLE_COUNT; k++) {
                    if (hs->h[k].heldBy == handIndex) busy = 1;
                }
                if (busy) continue;

                float dx = hand->x - h->restX;
                float dz = hand->z - h->restZ;
                float horiz = sqrtf(dx * dx + dz * dz);
                float top = h->restY + 0.06f;
                float bot = h->restY - HANDLE_MAX_PULL - 0.05f;
                /* The whole cable is grabbable, not only the rest pose, so a
                 * hand can catch the grip on the way down. */
                if (horiz <= HANDLE_GRAB_RADIUS && hand->y <= top && hand->y >= bot) {
                    h->heldBy = handIndex;
                    h->pull = Clampf(h->restY - hand->y, 0.0f, HANDLE_MAX_PULL);
                    h->pullVel = 0.0f;
                    break;
                }
            }
        }

        if (h->heldBy < 0) SpringBack(h, dt);
        RefreshActive(h);
    }
}
