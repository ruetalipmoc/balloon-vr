/* Basket-space scene. The gondola stays at the origin (the tracked room).
 * The world is drawn translated by the balloon's position, so walking the
 * play space only moves you inside the basket. */
#ifndef BALLOON_SCENE_H
#define BALLOON_SCENE_H

#include "balloon.h"
#include "handles.h"
#include "watch.h"

#include "raylib.h"

typedef struct Scene Scene;

Scene* Scene_Create(void);
void Scene_Destroy(Scene* s);

void Scene_Update(Scene* s, const HandInput hands[2], float dt, int keyBurner, int keyVent);

/* `eye` is the viewer in basket space, used to keep the sky wrapped around them. */
void Scene_Draw(Scene* s, Vector3 eye);

const Balloon* Scene_Balloon(const Scene* s);
float Scene_Ground(const Scene* s);

#endif
