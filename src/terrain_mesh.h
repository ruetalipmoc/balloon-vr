/* Chunk meshes with an aggressive distance LOD. Near chunks are faceted
 * hills; far chunks collapse to one or two quads tinted toward the sky.
 * Borders are stitched onto the coarser neighbour so LOD changes do not crack. */
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

#endif
