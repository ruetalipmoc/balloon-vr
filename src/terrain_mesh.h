/* Ground is a clipmap: nested rings centred on the viewer, each twice as
 * coarse as the one inside. Shared edges are morphed onto the coarser
 * ring so the surface has no crack and no pop. Colour stays in the shader. */
#ifndef BALLOON_TERRAIN_MESH_H
#define BALLOON_TERRAIN_MESH_H

#include "raylib.h"

#define CLIP_LEVELS 8
#define CHUNK_MAX_PROPS 24

typedef struct Prop {
    float x, y, z;
    float yaw;
    float scale;
    int kind; /* 0 broadleaf, 1 pine, 2 rock, 3 house, 4 barn, 5 block, 6 tower */
} Prop;

/* One ring. `bound*` is the edge that must meet the next coarser ring. */
typedef struct ClipJob {
    int level;
    float spacing;
    float cx, cz;
    float radius;
    float holeL, holeR, holeB, holeT;
    int hasHole;
    float boundL, boundR, boundB, boundT;
    int morph;
    float viewX, viewZ;
} ClipJob;

void Clip_Describe(int level, float viewX, float viewZ, ClipJob* job);
int Clip_Build(const ClipJob* job, Mesh* out);
int Clip_GatherProps(float viewX, float viewZ, Prop* dst, int cap);

/* Previous chunk mesh. Still built by terrain_mesh.c; the scene uses the clipmap. */
typedef struct ChunkMesh {
    Mesh ground;
    int ready;
    Prop props[CHUNK_MAX_PROPS];
    int propCount;
} ChunkMesh;

int ChunkMesh_Build(int cx, int cz, int lod, float viewX, float viewZ, ChunkMesh* out);
void ChunkMesh_Free(ChunkMesh* chunk);
int FarField_Build(float viewX, float viewZ,
                   float holeX0, float holeX1, float holeZ0, float holeZ1,
                   float outer, float seam, Mesh* out);

#endif
