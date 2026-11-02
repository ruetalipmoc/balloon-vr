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

/* Distance LOD. Grids are nested (16, 8, 2, 1) so a coarse edge is a
 * subset of the fine one and the two can be stitched. */
int Terrain_LodForDistance(float distMeters);
int Terrain_GridForLod(int lod);

/* One grid sample. When it sits on the border with a coarser neighbour,
 * `snapped` is set and y lies on that neighbour's edge instead of the
 * raw height field, so the two meshes share a silhouette. */
typedef struct TerrainSample {
    float x, z, y;
    int snapped;
    float ax, az, bx, bz;
    float t;
} TerrainSample;

TerrainSample Terrain_Sample(int jx, int jz, int grid, float x0, float z0, float size,
                             int cx, int cz, float viewX, float viewZ);

#endif
