#ifndef _IN_SIMNAV_H
#define _IN_SIMNAV_H

/**
 * The simulants' navmesh (PLANS/AI-REWORK.md, milestone M1): Recast/Detour
 * (port/src/external/recastnavigation) built from a stage's collision tiles.
 *
 * Two halves. port/src/simnav.cpp wraps Recast and Detour behind the plain C
 * interface below and knows nothing of the game; port/src/simnavstage.c takes
 * a Perfect Dark stage's tiles apart into its input, keeps the one mesh of the
 * stage being played, caches it on disk, draws it, and runs the batch build.
 *
 * All of it is off unless Mod.SimNavDebug=1 or --simnav-debug is given, or
 * --simnav-build-all is run: with it off nothing is built or allocated and no
 * frame is drawn differently. Nothing here moves a simulant yet.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Area ids a triangle of the input is given (Recast keeps the highest of two
// spans it merges, so the order matters only where surfaces meet within a
// climb of each other). 0 is not walkable: walls, and floors that kill.
#define SIMNAV_AREA_NONE   0
#define SIMNAV_AREA_GROUND 1
#define SIMNAV_AREA_SLOPE  2 // GEOFLAG_SLOPE: the player slides here
#define SIMNAV_AREA_STEP   3 // GEOFLAG_STEP: ascended whatever its steepness
#define SIMNAV_AREA_WATER  4 // GEOFLAG_UNDERWATER
#define SIMNAV_AREA_LADDER 5 // GEOFLAG_LADDER / LADDER_PLAYERONLY on a floor
#define SIMNAV_NUMAREAS    6

struct simnavparams {
	float cellsize;       // xz size of a voxel, world units
	float cellheight;     // y size of a voxel
	float agentheight;    // clearance a walker needs
	float agentradius;    // how far a walker keeps from a wall
	float agentclimb;     // the highest step walked up without a jump
	float agentslope;     // degrees (only for Recast's own marking; we mark by flags)
	int tilesize;         // cells along a tile's side
	float maxedgelen;     // world units
	float maxsimplificationerror; // cells
	int minregionsize;    // cells, one side of a square
	int mergeregionsize;  // cells, one side of a square
	float detailsampledist;     // cells
	float detailsamplemaxerror; // cells of height
};

struct simnavinput {
	const float *verts; // x, y, z each
	int numverts;
	const int *tris;    // three vertex indices each
	const unsigned char *areas; // one SIMNAV_AREA_* per triangle
	int numtris;
};

struct simnavstats {
	int tiles;      // tiles with any polygons
	int polys;
	int verts;
	int detailtris;
	int emptytiles; // tiles whose cells held nothing walkable
	int failedtiles;
	float buildms;
};

struct simnavmesh;

// Builds a tiled navmesh. NULL on failure, with the reason in err.
struct simnavmesh *simnavMeshBuild(const struct simnavinput *input, const struct simnavparams *params,
		struct simnavstats *stats, char *err, size_t errlen);

void simnavMeshFree(struct simnavmesh *mesh);

void simnavMeshGetStats(const struct simnavmesh *mesh, struct simnavstats *stats);

// Serialises the mesh (after a caller's header) into a malloc'd buffer the
// caller frees with free().
int simnavMeshSave(const struct simnavmesh *mesh, uint8_t **outbuf, size_t *outlen);

// The mesh back from simnavMeshSave()'s bytes. NULL if they do not hold one.
struct simnavmesh *simnavMeshLoad(const uint8_t *buf, size_t len);

// Every detail triangle of the mesh, for drawing. Each is reported with the
// index of the tile it is in, its polygon's area and a running polygon number.
typedef void (*simnavtrifn)(void *arg, int tile, int poly, unsigned char area,
		const float *a, const float *b, const float *c);
int simnavMeshForEachTri(const struct simnavmesh *mesh, simnavtrifn fn, void *arg);

// The most tiles the mesh can hold; tile indices above are below this
int simnavMeshNumTiles(const struct simnavmesh *mesh);

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
#include <ultra64.h>
#include "types.h"

// lvReset(), once the stage's tiles are loaded: builds or loads the stage's
// mesh when the debug view is on. Nothing when it is off.
void simnavStageStart(s32 stagenum);

// lvStop(): the stage's mesh and its drawing are freed
void simnavStageStop(void);

// bgRenderScene(), after the wall hits, under the orthogonal projection: the
// debug view of the mesh
Gfx *simnavRender(Gfx *gdl);

// mainInit(): --simnav-build-all builds every multiplayer arena's mesh, logs
// each and exits. Returns at once without it.
void simnavBuildAllFromCommandLine(void);

s32 simnavDebugEnabled(void);
#endif

#endif
