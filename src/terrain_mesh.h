/* Chunk meshes with distance LOD. Shared edges use the same heights, and a
 * short skirt under each border hides the hairline a T-junction can leave.
 * The farthest ring drops its colour into a blue haze. */
#ifndef BALLOON_TERRAIN_MESH_H
#define BALLOON_TERRAIN_MESH_H

#include "raylib.h"

#define CHUNK_MAX_PROPS 24

typedef struct Prop {
    float x, y, z;
    float yaw;
    float scale;
    int kind; /* 0 broadleaf, 1 pine, 2 rock, 3 house, 4 barn, 5 block, 6 tower */
} Prop;

typedef struct ChunkMesh {
    Mesh ground;
    Mesh water;
    int hasWater;
    int ready;
    Prop props[CHUNK_MAX_PROPS];
    int propCount;
} ChunkMesh;

int ChunkMesh_Build(int cx, int cz, int lod, float viewX, float viewZ, ChunkMesh* out);
void ChunkMesh_Free(ChunkMesh* chunk);

/* Coarse ground outside the streamed square. Heights are the real field, so
 * flying there meets the same hills. `outer` is the half-extent in metres. */
int FarField_Build(float viewX, float viewZ,
                   float holeX0, float holeX1, float holeZ0, float holeZ1,
                   float outer, float seam, Mesh* out);

#endif
