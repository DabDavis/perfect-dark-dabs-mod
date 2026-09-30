#ifndef _IN_SIMNAV_H
#define _IN_SIMNAV_H

/**
 * The simulants' navmesh (PLANS/AI-REWORK.md, milestones M1-M2):
 * Recast/Detour (port/src/external/recastnavigation) built from a stage's
 * collision tiles, with off-mesh links for ladders, lifts, drops and jumps,
 * and path queries.
 *
 * Two halves. port/src/simnav.cpp wraps Recast and Detour behind the plain C
 * interface below and knows nothing of the game (port/src/simnavlinks.cpp
 * finds the links, from the chr numbers it is handed); port/src/simnavstage.c
 * takes a Perfect Dark stage's tiles, pads and setup apart into its input,
 * keeps the one mesh of the stage being played, caches it on disk, draws it
 * and a few simulants' paths, and runs the batch build.
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
#define SIMNAV_AREA_LADDER 5 // GEOFLAG_LADDER / LADDER_PLAYERONLY on a floor; a ladder link
// Off-mesh links (M2) have areas of their own
#define SIMNAV_AREA_LIFT   6 // a lift's stops, one to the next
#define SIMNAV_AREA_DROP   7 // walked off a ledge onto a floor below, one way
#define SIMNAV_AREA_JUMP   8 // jumped across a gap, one way
// Floors a simulant goes over lowered (GEOFLAG_AIBOTDUCK, AIBOTCROUCH):
// chrTick() gives it 135 or 90 of height over them
#define SIMNAV_AREA_DUCK   9
#define SIMNAV_AREA_CROUCH 10
#define SIMNAV_NUMAREAS    11

// Polygon and link flags, for a query's include mask. Every walkable polygon
// is WALK; a link has the flag of its kind, and a jump link the flag of the
// lowest Jump Height setting (1-5, modGetJumpHeight()) that makes it.
#define SIMNAV_FLAG_WALK   0x0001
#define SIMNAV_FLAG_LADDER 0x0002
#define SIMNAV_FLAG_LIFT   0x0004
#define SIMNAV_FLAG_DROP   0x0008
#define SIMNAV_FLAG_JUMP(height) (0x0010 << ((height) - 1)) // 0x10 to 0x100
#define SIMNAV_MAXJUMPHEIGHTS 5
#define SIMNAV_FLAGS_NOJUMP (SIMNAV_FLAG_WALK | SIMNAV_FLAG_LADDER | SIMNAV_FLAG_LIFT | SIMNAV_FLAG_DROP)

// What a triangle of the input is to a chr, as bits: stood on, stopped by, killed by
#define SIMNAV_KIND_FLOOR 0x01
#define SIMNAV_KIND_WALL  0x02
#define SIMNAV_KIND_DEATH 0x04

// A ladder: GEOFLAG_LADDER faces merged, standing on the plane through
// (x, z) with the horizontal unit normal (nx, nz), from ymin to ymax
struct simnavladder {
	float x, z;
	float nx, nz;
	float halfwidth;
	float ymin, ymax;
};

// A lift's stops (its pads), bottom to top or in any order
struct simnavlift {
	int numstops;
	float stops[4][3];
};

// How a chr moves when it is not walking, for the drop and jump links. The
// numbers are the game's; simnavstage.c says where each comes from.
struct simnavlinkparams {
	float gravity;      // per tick per tick
	float runspeed;     // units a tick, the slowest simulant's
	float jumpimpulse[SIMNAV_MAXJUMPHEIGHTS]; // per Jump Height setting
	float jumpapex[SIMNAV_MAXJUMPHEIGHTS];    // how far a jumper's box reaches down
	int numjumpheights;
	float maxdrop;      // deepest drop a simulant takes
	float boxfloor;     // a walker's box starts this far over its feet
	float groundlag;    // a floor within this of the feet is the same floor
	float samplespacing; // along a mesh edge, between tries
	float clusterdist;  // links of a kind this close at both ends are one
};

struct simnavparams {
	float cellsize;       // xz size of a voxel, world units
	float cellheight;     // y size of a voxel
	float agentheight;    // clearance a walker needs
	float agentradius;    // how far a walker keeps from a wall
	float agentclimb;     // the highest step walked up without a jump
	float agentstepover;  // the highest wall walked over
	float agentduckheight;   // a walker's height where the floor says duck
	float agentcrouchheight; // and where it says crouch
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
	const unsigned char *kinds; // SIMNAV_KIND_* bits per triangle
	int numtris;
	const struct simnavladder *ladders;
	int numladders;
	const struct simnavlift *lifts;
	int numlifts;
};

struct simnavstats {
	int tiles;      // tiles with any polygons
	int polys;
	int verts;
	int detailtris;
	int emptytiles; // tiles whose cells held nothing walkable
	int failedtiles;
	float buildms;
	// off-mesh links, by kind
	int ladderlinks;
	int liftlinks;
	int droplinks;
	int jumplinks;
	int jumpsbyheight[SIMNAV_MAXJUMPHEIGHTS];
	float linkms;
};

struct simnavmesh;

// Builds a tiled navmesh and its off-mesh links (linkparams NULL: none).
// NULL on failure, with the reason in err.
struct simnavmesh *simnavMeshBuild(const struct simnavinput *input, const struct simnavparams *params,
		const struct simnavlinkparams *linkparams, struct simnavstats *stats, char *err, size_t errlen);

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

// Every off-mesh link, for drawing: its area, flags and ends
typedef void (*simnavlinkfn)(void *arg, unsigned char area, unsigned short flags, int bidir,
		const float *a, const float *b);
int simnavMeshForEachLink(const struct simnavmesh *mesh, simnavlinkfn fn, void *arg);

// Of n points (pads), how many stand on the mesh (a polygon within
// `below` under them and `radius` across), and of the n * (n - 1) / 2 pairs
// how many can each reach the other through polygons and links whose flags
// are in include. groups, if not NULL, gets each point's group: the lowest
// numbered point it reaches and is reached from (itself, if none), or -1 for
// a point off the mesh.
void simnavMeshReachability(const struct simnavmesh *mesh, const float *points, int n,
		float below, float radius, unsigned short include, int *onmesh, int *mutualpairs, int *groups);

// Path queries on a mesh (Detour's dtNavMeshQuery). One per user; not shared
// between threads.
struct simnavquery;
struct simnavquery *simnavQueryCreate(const struct simnavmesh *mesh);
void simnavQueryFree(struct simnavquery *query);

// The point on the mesh under pos: the polygon whose surface is within
// `below` under it or `above` over it and `radius` across, nearest first.
int simnavQueryFindFloor(struct simnavquery *query, const float *pos, float below, float above,
		float radius, float *out);

// A path from start to end through polygons and links whose flags are in
// include, as its corners (Detour's straight path). linkareas[i] is the area
// of the link leaving point i, or SIMNAV_AREA_NONE when it is walked. The
// number of points, 0 for none; *complete is whether it reaches end.
int simnavQueryPath(struct simnavquery *query, const float *start, const float *end, unsigned short include,
		float *points, unsigned char *linkareas, int maxpoints, int *complete);

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
