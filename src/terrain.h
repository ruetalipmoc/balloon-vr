/* Deterministic drained height field. Tiles of 256 m share their edges, so a
 * river that crosses one is the same river on the next. The same (x, z)
 * always returns the same ground. */
#ifndef BALLOON_TERRAIN_H
#define BALLOON_TERRAIN_H

#define TERRAIN_CHUNK_SIZE 64.0f
#define TERRAIN_WATER 6.2f

/* Landscape class at a point. The height field blends across borders;
 * this is the dominant class, used for colour and props. */
#define TERRAIN_ZONE_PLAINS   0
#define TERRAIN_ZONE_FARM     1
#define TERRAIN_ZONE_FOREST   2
#define TERRAIN_ZONE_HIGHLAND 3
#define TERRAIN_ZONE_CITY     4

/* Axis-aligned lot size. Fields and city blocks share the world axes
 * so the patchwork stays square when seen from the basket. */
#define TERRAIN_FIELD      128.0f
#define TERRAIN_CITY_BLOCK 64.0f

/* If set, Terrain_Height uses this instead of the drained field.
 * The newgen experiment installs one. NULL keeps the default. */
void Terrain_SetHeightFn(float (*fn)(float x, float z));

float Terrain_Height(float x, float z);
float Terrain_Moisture(float x, float z);
int Terrain_Zone(float x, float z);

/* Soft cover masks, each 0..1. The ground shader paints fields and streets
 * per pixel; these weights only say which cover is allowed here. */
void Terrain_Cover(float x, float z, float* city, float* farm, float* forest, float* highland);
/* Height and cover from one evaluation. Used by the clipmap, which samples
 * tens of thousands of points. */
void Terrain_HeightCover(float x, float z, float* height, float* city, float* farm, float* forest, float* highland);

/* Unit normal from central differences. ny is positive on a flat field. */
void Terrain_Normal(float x, float z, float* nx, float* ny, float* nz);

/* Highest ground sample under the basket footprint, so a slope does not
 * poke through the floor. */
float Terrain_GroundAt(float x, float z);

/* `--view` is the radius of the finest ring at TERRAIN_ALT_REF above the
 * ground. `--lod` is how many rings. Both stay put at or below that height;
 * higher up the radii grow so the ground over the rail keeps its angle.
 * The outer ring is capped so the far plane stays finite. */
#define TERRAIN_VIEW_DEFAULT 128.0f
#define TERRAIN_VIEW_MIN 64.0f
#define TERRAIN_VIEW_MAX 512.0f
#define TERRAIN_RINGS_DEFAULT 8
#define TERRAIN_RINGS_MIN 3
#define TERRAIN_RINGS_MAX 8
#define TERRAIN_CLIP_CELLS 32
#define TERRAIN_OUTER_MAX 160000.0f

/* `--view` and `--lod` are the values at this height above the ground. */
#define TERRAIN_ALT_REF 40.0f

int Terrain_LodForDistance(float distMeters);
int Terrain_GridForLod(int lod);

/* Grid actually used for a chunk. Nearby banks stay one step finer than
 * the distance level. Far banks use the normal level: a 4 m shore over
 * the whole view is what made the frame rate collapse. */
int Terrain_EffectiveGrid(int cx, int cz, int lod, float viewX, float viewZ);

/* Clamp and store the finest-ring radius used at TERRAIN_ALT_REF. The
 * radius actually used this frame may be larger once altitude is applied. */
float Terrain_SetViewDistance(float meters);
float Terrain_ViewDistance(void);

/* How many nested rings to draw, from 3 to TERRAIN_RINGS_MAX. */
int Terrain_SetRingCount(int count);
int Terrain_RingCount(void);

/* Half-extent and cell size of one ring, after altitude scaling. Level 0
 * is the finest. */
float Terrain_RingSpacing(int level);
float Terrain_RingRadius(int level);
float Terrain_OuterRadius(void);
float Terrain_FarPlane(void);

/* Chunk distance bands. The clipmap does not use these; `--lod` is the
 * ring count. */
void Terrain_SetLodThresholds(float d0, float d1, float d2, float d3);

/* Grow the ring radii for this height above the ground. */
void Terrain_ApplyAltitude(float agl);

/* Effective threshold for band 0..3 (after altitude). */
float Terrain_LodThreshold(int band);

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
