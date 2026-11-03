/* Alternate ground, behind --newgen.
 * One seeded stack of Perlin octaves. A coarse sheet is built at startup
 * (the far view). Around the basket, nested tiles sample that same function
 * on a finer grid: one tile under the basket, eight around it, then a crown,
 * and the precomputed sheet everywhere else. No trees, no buildings.
 * Detail 0 is the sheet. Detail 3 is the tile under the basket. */
#ifndef BALLOON_NEWGEN_H
#define BALLOON_NEWGEN_H

void NewGen_Begin(unsigned int seed);
int NewGen_Active(void);
float NewGen_Height(float x, float z);
float NewGen_FarPlane(void);
void NewGen_Update(float x, float z);
void NewGen_ScrollClouds(float dt, float altitude);
void NewGen_Draw(float eyeX, float eyeZ);
void NewGen_SetFog(int on);
void NewGen_Shutdown(void);

#endif
