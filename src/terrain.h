/* Deterministic height field. The same (x, z) always returns the same ground,
 * so chunks can be dropped and rebuilt as the balloon drifts. */
#ifndef BALLOON_TERRAIN_H
#define BALLOON_TERRAIN_H

#define TERRAIN_CHUNK_SIZE 64.0f
#define TERRAIN_WATER 6.2f

float Terrain_Height(float x, float z);
float Terrain_Moisture(float x, float z);

/* Unit normal from central differences. ny is positive on a flat field. */
void Terrain_Normal(float x, float z, float* nx, float* ny, float* nz);

/* Highest ground sample under the basket footprint, so a slope does not
 * poke through the floor. */
float Terrain_GroundAt(float x, float z);

#endif
