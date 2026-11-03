/* Deterministic height field. The same (x, z) always returns the same ground,
 * so chunks can be dropped and rebuilt as the balloon drifts. */
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

float Terrain_Height(float x, float z);
float Terrain_Moisture(float x, float z);
int Terrain_Zone(float x, float z);

/* Soft cover masks, each 0..1. The ground shader paints fields and streets
 * per pixel; these weights only say which cover is allowed here. */
void Terrain_Cover(float x, float z, float* city, float* farm, float* forest, float* highland);

/* Unit normal from central differences. ny is positive on a flat field. */
void Terrain_Normal(float x, float z, float* nx, float* ny, float* nz);

/* Highest ground sample under the basket footprint, so a slope does not
 * poke through the floor. */
float Terrain_GroundAt(float x, float z);

/* Distance LOD. Grids are nested by halves (16, 8, 4, 2, 1) so a coarse
 * edge is a subset of the fine one. Border samples are snapped onto the
 * coarser edge, and the height everywhere else is the real field, so two
 * tiles that share an edge occupy the same line.
 * Thresholds and the draw distance can be changed from the command line. */
#define TERRAIN_VIEW_DEFAULT (11.0f * TERRAIN_CHUNK_SIZE)
#define TERRAIN_VIEW_MAX (24.0f * TERRAIN_CHUNK_SIZE)

/* `--view` and `--lod` are the distances at this height above the ground.
 * Higher up they stretch so a band stays at the same angle below the horizon. */
#define TERRAIN_ALT_REF 40.0f

int Terrain_LodForDistance(float distMeters);
int Terrain_GridForLod(int lod);

/* Clamp and store the draw distance used at TERRAIN_ALT_REF. The value
 * actually used this frame may be larger once altitude is applied. */
float Terrain_SetViewDistance(float meters);
float Terrain_ViewDistance(void);

/* Four ascending distances, in metres, at TERRAIN_ALT_REF. */
void Terrain_SetLodThresholds(float d0, float d1, float d2, float d3);

/* Stretch the stored distances for this height above the ground. */
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
