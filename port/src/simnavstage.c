/**
 * A Perfect Dark stage's navmesh: its collision tiles taken apart into
 * Recast's input, the mesh and its off-mesh links cached on disk, drawn over
 * the level with a few simulants' paths, and built for every arena at once
 * from the command line. port/include/simnav.h has the overview;
 * port/src/simnav.cpp is the Recast side and port/src/simnavlinks.cpp finds
 * the links.
 *
 * **What is walkable.** A chr stands on a tile flagged GEOFLAG_FLOOR1 or
 * FLOOR2 (cdFindGroundInfoAtCyl() asks for exactly those) and is stopped by
 * one flagged GEOFLAG_WALL (every cylinder move - cdTestCylMove*,
 * cdExamCylMove* - collects GEOFLAG_WALL and nothing else). No slope limit
 * applies to a chr's floor: the steepness rules in bondwalk.c are the
 * player's. So the input is every floor tile, walkable, and every wall tile,
 * solid; a tile that is neither (a sight or shot blocker, a ladder face) is
 * not there for a walker and is left out. A floor with GEOFLAG_DIE is kept as
 * solid ground nobody may stand on, so the mesh stops at its edge rather than
 * running on over it.
 *
 * **Out of scope**: props (doors, glass, crates, lifts' cars - their geometry
 * moves, or stands on the setup rather than the tiles, and is DetourTileCache's
 * in M6; a pad on a crate is off the mesh). A GoldenEye level converted into
 * GE Plus has its own collision (gestan.c's 2-D tile graph, whose walls are
 * there for one body and not another) and is skipped with a log line.
 *
 * **Agent size**, from the chr movement code:
 *  - radius 20: chrInit() (chr.c) gives every chr 20; only a few special
 *    bodies (Dr Caroll 30, the ChicRob 42, body.c) change it, none a simulant.
 *  - height 185: chrTick() sets an aibot's height to 185 standing, and
 *    chrGetBbox() tops its box at manground + height. Over a floor flagged
 *    GEOFLAG_AIBOTDUCK it is 135, over AIBOTCROUCH 90 (chrTick() asks for
 *    the flags from 10 under the feet up, so the floor under a duct carries
 *    them): the mesh is built for 90 and a place with less room than 185 is
 *    kept only on such a floor, as its own area.
 *  - step over 20: chrGetBbox() starts a walker's box 20 over its manground,
 *    so a wall lower than that is walked over and a taller one stops it.
 *  - climb 60: the floor a walker's move ends on is looked for from at least
 *    69 over its manground (chr0f01f378()), and the ground is let lag the
 *    floor by 30 on the way up; so a step whose riser is not a wall is walked
 *    up to that height. Complex's upper walkway has a 39 riser flagged only
 *    as floor and Villa dozens of 30-39; at the first climb of 30 they cut
 *    the stage in two. A riser flagged as a wall stops a walker over 20, and
 *    Recast sees it only where it stands taller than the floor it shares a
 *    cell with - under 60 it is still climbed (two triangles on Felicity).
 *
 * **Cache.** cache/navmesh/pd/<stage>-<hash>.bin beside the executable (or in
 * the save directory where that cannot be written, fsChooseOutputDir()). The
 * hash is over the triangles, their areas and kinds, the ladders and lifts
 * found and every build and link parameter, so a mod or the XBLA release's
 * tiles (xblaStageLoadTiles()) that change a level make a new file rather
 * than reading an old one. SIMNAV_EXTRACT_VERSION is for the code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "config.h"
#include "system.h"
#include "fs.h"
#include "gbiex.h"
#include "game/file.h"
#include "game/gfxmemory.h"
#include "game/stagetable.h"
#include "game/tex.h"
#include "lib/mtx.h"
#include "game/chraction.h"
#include "game/pad.h"
#include "game/setuputils.h"
#include "game/modoptions.h"
#include "game/modspectate.h"
#include "modloader.h"
#include "simnav.h"

#define SIMNAV_EXTRACT_VERSION 3 // bump when the extraction, the links or the saved form change
#define SIMNAV_CACHE_MAGIC     0x4e44504e // 'NPDN'
#define SIMNAV_DRAW_LIFT       6.0f   // units the drawn mesh stands over the floor
#define SIMNAV_DRAW_REACH      4000.0f // tiles further than this from the camera are not drawn
#define SIMNAV_DRAW_ALPHA      0x78
#define SIMNAV_STEPOVER        20.0f  // chrGetBbox(): a walker's box starts this far over its feet

static s32 g_SimNavDebugOpt = 0;

PD_CONSTRUCTOR static void simnavConfigInit(void)
{
	configRegisterInt("Mod.SimNavDebug", &g_SimNavDebugOpt, 0, 1);
}

s32 simnavDebugEnabled(void)
{
	return g_SimNavDebugOpt || sysArgCheck("--simnav-debug");
}

static void simnavGetParams(struct simnavparams *p)
{
	memset(p, 0, sizeof(*p));
	p->cellsize = 5.0f;
	p->cellheight = 5.0f;
	p->agentheight = 185.0f;
	p->agentradius = 20.0f;
	p->agentclimb = 60.0f;
	p->agentstepover = SIMNAV_STEPOVER;
	p->agentduckheight = 135.0f;
	p->agentcrouchheight = 90.0f;
	p->agentslope = 60.0f;
	p->tilesize = 64;
	p->maxedgelen = 240.0f;
	p->maxsimplificationerror = 1.3f;
	p->minregionsize = 8;
	p->mergeregionsize = 20;
	p->detailsampledist = 6.0f;
	p->detailsamplemaxerror = 1.0f;
}

/*
 * Extraction
 */

#define SIMNAV_MAXLIFTS 32

struct simnavgeom {
	f32 *verts;
	s32 numverts, maxverts;
	s32 *tris;
	u8 *areas;
	u8 *kinds;
	s32 numtris, maxtris;
	s32 floors, walls, died, blocks, skipped, laddertiles;
	struct simnavladder *ladders;
	s32 numladders, maxladders;
	struct simnavlift lifts[SIMNAV_MAXLIFTS];
	s32 numlifts;
};

static void simnavGeomFree(struct simnavgeom *g)
{
	free(g->verts);
	free(g->tris);
	free(g->areas);
	free(g->kinds);
	free(g->ladders);
	memset(g, 0, sizeof(*g));
}

static s32 simnavGeomReserve(struct simnavgeom *g, s32 verts, s32 tris)
{
	if (g->numverts + verts > g->maxverts) {
		s32 max = g->maxverts ? g->maxverts * 2 : 4096;
		f32 *v;

		while (max < g->numverts + verts) {
			max *= 2;
		}

		v = realloc(g->verts, sizeof(f32) * 3 * max);

		if (!v) {
			return 0;
		}

		g->verts = v;
		g->maxverts = max;
	}

	if (g->numtris + tris > g->maxtris) {
		s32 max = g->maxtris ? g->maxtris * 2 : 4096;
		s32 *t;
		u8 *a;

		while (max < g->numtris + tris) {
			max *= 2;
		}

		t = realloc(g->tris, sizeof(s32) * 3 * max);

		if (!t) {
			return 0;
		}

		g->tris = t;
		a = realloc(g->areas, max);

		if (!a) {
			return 0;
		}

		g->areas = a;
		a = realloc(g->kinds, max);

		if (!a) {
			return 0;
		}

		g->kinds = a;
		g->maxtris = max;
	}

	return 1;
}

static s32 simnavGeomVert(struct simnavgeom *g, f32 x, f32 y, f32 z)
{
	f32 *v = &g->verts[g->numverts * 3];

	v[0] = x;
	v[1] = y;
	v[2] = z;

	return g->numverts++;
}

static void simnavGeomTri(struct simnavgeom *g, s32 a, s32 b, s32 c, u8 area, u8 kind)
{
	s32 *t = &g->tris[g->numtris * 3];

	t[0] = a;
	t[1] = b;
	t[2] = c;
	g->areas[g->numtris] = area;
	g->kinds[g->numtris] = kind;
	g->numtris++;
}

/** What a tile is to a chr: a floor stood on (and one that kills), a wall that stops it */
static u8 simnavTileKind(u16 flags)
{
	u8 kind = 0;

	if (flags & (GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2)) {
		kind |= SIMNAV_KIND_FLOOR;

		if (flags & GEOFLAG_DIE) {
			kind |= SIMNAV_KIND_DEATH;
		}
	}

	if (flags & GEOFLAG_WALL) {
		kind |= SIMNAV_KIND_WALL;
	}

	return kind;
}

/**
 * A GEOFLAG_LADDER face (only that flag: a simulant's cdFindLadder() in
 * chr0f01f378() asks for no other), as a ladder of its own until
 * simnavMergeLadders() joins the faces of one.
 */
static void simnavAddLadderTile(struct simnavgeom *g, const f32 *xyz, s32 nv)
{
	f32 n[3] = { 0, 0, 0 };
	f32 len, nx, nz, tx, tz, d, smin = 1e30f, smax = -1e30f, ymin = 1e30f, ymax = -1e30f;
	s32 i;

	// Newell's normal
	for (i = 0; i < nv; i++) {
		const f32 *a = &xyz[i * 3];
		const f32 *b = &xyz[((i + 1) % nv) * 3];

		n[0] += (a[1] - b[1]) * (a[2] + b[2]);
		n[1] += (a[2] - b[2]) * (a[0] + b[0]);
		n[2] += (a[0] - b[0]) * (a[1] + b[1]);
	}

	len = sqrtf(n[0] * n[0] + n[2] * n[2]);

	// a face that stands up, not a floor with the flag
	if (len < 1e-3f || fabsf(n[1]) > len) {
		return;
	}

	nx = n[0] / len;
	nz = n[2] / len;
	tx = -nz;
	tz = nx;
	d = 0;

	for (i = 0; i < nv; i++) {
		const f32 s = xyz[i * 3 + 0] * tx + xyz[i * 3 + 2] * tz;

		d += xyz[i * 3 + 0] * nx + xyz[i * 3 + 2] * nz;
		smin = s < smin ? s : smin;
		smax = s > smax ? s : smax;
		ymin = xyz[i * 3 + 1] < ymin ? xyz[i * 3 + 1] : ymin;
		ymax = xyz[i * 3 + 1] > ymax ? xyz[i * 3 + 1] : ymax;
	}

	d /= nv;
	g->laddertiles++;

	if (g->numladders >= g->maxladders) {
		s32 max = g->maxladders ? g->maxladders * 2 : 32;
		struct simnavladder *nl = realloc(g->ladders, sizeof(*nl) * max);

		if (!nl) {
			return;
		}

		g->ladders = nl;
		g->maxladders = max;
	}

	g->ladders[g->numladders].nx = nx;
	g->ladders[g->numladders].nz = nz;
	g->ladders[g->numladders].x = nx * d + tx * (smin + smax) * 0.5f;
	g->ladders[g->numladders].z = nz * d + tz * (smin + smax) * 0.5f;
	g->ladders[g->numladders].halfwidth = (smax - smin) * 0.5f;
	g->ladders[g->numladders].ymin = ymin;
	g->ladders[g->numladders].ymax = ymax;
	g->numladders++;
}

/**
 * o into l, if it continues it: the same plane, overlapping or touching
 * across and up
 */
static s32 simnavLadderMerge(struct simnavladder *l, const struct simnavladder *o)
{
	const f32 dot = l->nx * o->nx + l->nz * o->nz;
	const f32 ltx = -l->nz, ltz = l->nx;
	const f32 lc = l->x * ltx + l->z * ltz;
	const f32 oc = o->x * ltx + o->z * ltz; // o's centre along l's tangent
	const f32 ld = l->x * l->nx + l->z * l->nz;
	const f32 od = o->x * l->nx + o->z * l->nz;
	f32 smin, smax;

	if (fabsf(dot) < 0.98f || fabsf(ld - od) > 8.0f) {
		return 0;
	}

	if (oc - o->halfwidth > lc + l->halfwidth + 10.0f || oc + o->halfwidth < lc - l->halfwidth - 10.0f
			|| o->ymin > l->ymax + 20.0f || o->ymax < l->ymin - 20.0f) {
		return 0;
	}

	smin = oc - o->halfwidth < lc - l->halfwidth ? oc - o->halfwidth : lc - l->halfwidth;
	smax = oc + o->halfwidth > lc + l->halfwidth ? oc + o->halfwidth : lc + l->halfwidth;
	l->x = l->nx * ld + ltx * (smin + smax) * 0.5f;
	l->z = l->nz * ld + ltz * (smin + smax) * 0.5f;
	l->halfwidth = (smax - smin) * 0.5f;
	l->ymin = o->ymin < l->ymin ? o->ymin : l->ymin;
	l->ymax = o->ymax > l->ymax ? o->ymax : l->ymax;

	return 1;
}

/** The faces of a stage merged into its ladders, until none continues another */
static void simnavMergeLadders(struct simnavgeom *g)
{
	s32 merged = 1;
	s32 i, j;

	while (merged) {
		merged = 0;

		for (i = 0; i < g->numladders; i++) {
			for (j = i + 1; j < g->numladders; j++) {
				if (simnavLadderMerge(&g->ladders[i], &g->ladders[j])) {
					g->ladders[j] = g->ladders[--g->numladders];
					merged = 1;
					j--;
				}
			}
		}
	}
}

static u8 simnavTileArea(u16 flags)
{
	if (flags & GEOFLAG_DIE) {
		return SIMNAV_AREA_NONE;
	}

	if (flags & (GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2)) {
		if (flags & (GEOFLAG_LADDER | GEOFLAG_LADDER_PLAYERONLY)) {
			return SIMNAV_AREA_LADDER;
		}

		// chrTick() asks for these flags within 10 under a simulant's feet
		// and a standing height over them: it is the floor that has them
		if (flags & GEOFLAG_AIBOTCROUCH) {
			return SIMNAV_AREA_CROUCH;
		}

		if (flags & GEOFLAG_AIBOTDUCK) {
			return SIMNAV_AREA_DUCK;
		}

		if (flags & GEOFLAG_UNDERWATER) {
			return SIMNAV_AREA_WATER;
		}

		if (flags & GEOFLAG_STEP) {
			return SIMNAV_AREA_STEP;
		}

		if (flags & GEOFLAG_SLOPE) {
			return SIMNAV_AREA_SLOPE;
		}

		return SIMNAV_AREA_GROUND;
	}

	return SIMNAV_AREA_NONE;
}

/** Whether a tile is anything to a walker: a floor to stand on or a wall to stop at. */
static s32 simnavTileWanted(struct simnavgeom *g, u16 flags)
{
	if (flags & (GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2)) {
		if (flags & GEOFLAG_DIE) {
			g->died++;
		} else {
			g->floors++;
		}

		return 1;
	}

	if (flags & GEOFLAG_WALL) {
		g->walls++;
		return 1;
	}

	g->skipped++;
	return 0;
}

/** A block (an object's footprint, rare in a tiles file) as a solid prism. */
static s32 simnavGeomPrism(struct simnavgeom *g, const f32 (*xz)[2], s32 n, f32 ymin, f32 ymax)
{
	s32 base, i;

	if (n < 3 || !simnavGeomReserve(g, n * 2, n * 2 + (n - 2) * 2)) {
		return n < 3;
	}

	base = g->numverts;

	for (i = 0; i < n; i++) {
		simnavGeomVert(g, xz[i][0], ymin, xz[i][1]);
		simnavGeomVert(g, xz[i][0], ymax, xz[i][1]);
	}

	for (i = 0; i < n; i++) {
		s32 j = (i + 1) % n;

		simnavGeomTri(g, base + i * 2, base + j * 2, base + j * 2 + 1, SIMNAV_AREA_NONE, SIMNAV_KIND_WALL);
		simnavGeomTri(g, base + i * 2, base + j * 2 + 1, base + i * 2 + 1, SIMNAV_AREA_NONE, SIMNAV_KIND_WALL);
	}

	for (i = 1; i < n - 1; i++) {
		simnavGeomTri(g, base + 1, base + i * 2 + 1, base + (i + 1) * 2 + 1, SIMNAV_AREA_NONE, SIMNAV_KIND_WALL);
		simnavGeomTri(g, base, base + (i + 1) * 2, base + i * 2, SIMNAV_AREA_NONE, SIMNAV_KIND_WALL);
	}

	g->blocks++;

	return 1;
}

/**
 * Every room's geometry in a loaded tiles file (a word of room count, then
 * each room's offset, then the geo lists) - g_TileFileData's layout, whether
 * or not stageParseTiles() has been over it: only the vertices, flags and
 * counts are read, which it does not touch.
 */
static s32 simnavExtract(const u8 *tiledata, struct simnavgeom *g)
{
	const u32 numrooms = *(const u32 *)tiledata;
	const u32 *rooms = (const u32 *)tiledata + 1;
	u32 r;

	memset(g, 0, sizeof(*g));

	if (numrooms == 0 || numrooms > 0x10000) {
		return 0;
	}

	for (r = 0; r < numrooms; r++) {
		const u8 *p = tiledata + rooms[r];
		const u8 *end = tiledata + rooms[r + 1];

		while (p < end) {
			const struct geo *geo = (const struct geo *)p;
			const s32 nv = geo->numvertices;

			if (geo->type == GEOTYPE_TILE_I) {
				const struct geotilei *tile = (const struct geotilei *)geo;

				if (nv >= 3 && nv <= 64 && (geo->flags & GEOFLAG_LADDER)) {
					f32 xyz[64 * 3];
					s32 i;

					for (i = 0; i < nv; i++) {
						xyz[i * 3 + 0] = tile->vertices[i][0];
						xyz[i * 3 + 1] = tile->vertices[i][1];
						xyz[i * 3 + 2] = tile->vertices[i][2];
					}

					simnavAddLadderTile(g, xyz, nv);
				}

				if (nv >= 3 && simnavTileWanted(g, geo->flags)) {
					const u8 kind = simnavTileKind(geo->flags);
					s32 base, i;
					u8 area;

					if (!simnavGeomReserve(g, nv, nv - 2)) {
						return 0;
					}

					base = g->numverts;

					for (i = 0; i < nv; i++) {
						simnavGeomVert(g, tile->vertices[i][0], tile->vertices[i][1], tile->vertices[i][2]);
					}

					area = simnavTileArea(geo->flags);

					// tiles are convex, as cdIs2dPointInIntTile() takes them
					for (i = 1; i < nv - 1; i++) {
						simnavGeomTri(g, base, base + i, base + i + 1, area, kind);
					}
				}

				p += nv * 6 + 0xe;
			} else if (geo->type == GEOTYPE_TILE_F) {
				const struct geotilef *tile = (const struct geotilef *)geo;

				if (nv >= 3 && nv <= 64 && (geo->flags & GEOFLAG_LADDER)) {
					simnavAddLadderTile(g, &tile->vertices[0].x, nv);
				}

				if (nv >= 3 && simnavTileWanted(g, geo->flags)) {
					const u8 kind = simnavTileKind(geo->flags);
					s32 base, i;
					u8 area;

					if (!simnavGeomReserve(g, nv, nv - 2)) {
						return 0;
					}

					base = g->numverts;

					for (i = 0; i < nv; i++) {
						simnavGeomVert(g, tile->vertices[i].x, tile->vertices[i].y, tile->vertices[i].z);
					}

					area = simnavTileArea(geo->flags);

					for (i = 1; i < nv - 1; i++) {
						simnavGeomTri(g, base, base + i, base + i + 1, area, kind);
					}
				}

				p += sizeof(struct geotilef) + sizeof(struct coord) * (nv - 64);
			} else if (geo->type == GEOTYPE_BLOCK) {
				const struct geoblock *block = (const struct geoblock *)geo;

				if (!simnavGeomPrism(g, (const f32 (*)[2])block->vertices, nv > 8 ? 8 : nv, block->ymin, block->ymax)) {
					return 0;
				}

				p += sizeof(struct geoblock);
			} else if (geo->type == GEOTYPE_CYL) {
				const struct geocyl *cyl = (const struct geocyl *)geo;
				f32 xz[8][2];
				s32 i;

				for (i = 0; i < 8; i++) {
					xz[i][0] = cyl->x + cyl->radius * cosf(i * 0.78539816f);
					xz[i][1] = cyl->z + cyl->radius * sinf(i * 0.78539816f);
				}

				if (!simnavGeomPrism(g, (const f32 (*)[2])xz, 8, cyl->ymin, cyl->ymax)) {
					return 0;
				}

				p += sizeof(struct geocyl);
			} else {
				sysLogPrintf(LOG_WARNING, "simnav: room %u: geo type %d is not one there is; the rest of the room is left out",
						r, geo->type);
				break;
			}
		}
	}

	simnavMergeLadders(g);

	return 1;
}

/**
 * How a simulant moves when it is not walking, from the game's own numbers:
 *
 * - gravity 0.27777779 a tick a tick: func0f0965e4() (game_096360.c), which
 *   chr0f01f378() integrates a falling or jumping chr's fallspeed.y with,
 *   trapezoidally, once a 60 Hz tick.
 * - jump impulse JUMP_IMPULSE (5.75) times modGetJumpImpulse()'s root of the
 *   Jump Height setting, 1 to 5: botTryJump() sets fallspeed.y to it. The
 *   apex is v * v / (2 * gravity), 59.5 units at 1 to 298 at 5.
 * - how far a jumper's box reaches down, JUMP_APEX (60) times the setting:
 *   modGetJumpApex(), which chrGetBbox() takes from the manground, never
 *   below the ground, then adds the 20 every walker's box starts over it. A
 *   jump therefore never lifts a simulant's box more than 20 over the floor
 *   it is over: it clears gaps, not ledges higher than a step.
 * - run speed 5 units a tick: botCalculateMaxSpeed() for a MeatSim (5 times
 *   a body-height factor of about 1), the slowest difficulty. At the
 *   difficulty's speed and full stick bot0f1921f8()'s eased rate settles at
 *   that many units per 60 Hz tick. A faster simulant jumps further, which
 *   clears the same gap; the speed sliders are for M3 to weigh.
 * - no fall damage: nothing in chr0f01f378() hurts a landing chr; it dies
 *   only landing on a GEOFLAG_DIE floor or below -30000. The limit is the
 *   route follower's: chrHasFloorBelow() holds a simulant at any edge with no
 *   floor within BOTDROP_MAX (1000, chr.c) below, so a drop is at most that.
 * - 20 and 30: a walker's box starts 20 over its feet (chrGetBbox()), and its
 *   ground may lag the floor by 30 (chrTickFalling(), the bot step).
 */
static void simnavGetLinkParams(struct simnavlinkparams *lp)
{
	static const f32 roots[SIMNAV_MAXJUMPHEIGHTS] = { 1.0f, 1.4142135f, 1.7320508f, 2.0f, 2.2360680f };
	s32 i;

	memset(lp, 0, sizeof(*lp));
	lp->gravity = 0.27777779f;
	lp->runspeed = 5.0f;
	lp->numjumpheights = JUMPHEIGHT_MAX < SIMNAV_MAXJUMPHEIGHTS ? JUMPHEIGHT_MAX : SIMNAV_MAXJUMPHEIGHTS;

	for (i = 0; i < lp->numjumpheights; i++) {
		lp->jumpimpulse[i] = JUMP_IMPULSE * roots[i];
		lp->jumpapex[i] = JUMP_APEX * (i + 1);
	}

	lp->maxdrop = 1000.0f;
	lp->boxfloor = 20.0f;
	lp->groundlag = 30.0f;
	lp->samplespacing = 40.0f;
	lp->clusterdist = 120.0f;
}

/*
 * The stage's pads and setup: its spawn and weapon pads, for the batch's
 * reachability, and its lifts' stops, for their links
 */

#define SIMNAV_MAXPADPOINTS 512

struct simnavstagepads {
	f32 points[SIMNAV_MAXPADPOINTS][3];
	s32 numpoints;
	s32 numspawn;
	s32 numweapon;
	struct simnavlift lifts[SIMNAV_MAXLIFTS];
	s32 numlifts;
};

/** A file loaded into memory of our own, as the game's loader loads it */
static u8 *simnavLoadFile(s32 fileid, u32 loadtype)
{
	struct fileinfo saved;
	u32 size;
	u8 *buf;

	if (fileid <= 0 || fileid >= NUM_FILE_SLOTS) {
		return NULL;
	}

	size = fileGetInflatedSize(fileid, loadtype);

	if (size == 0) {
		return NULL;
	}

	size = ((size + 0x20) & ~0xfu) + 0x8000;
	buf = calloc(1, size);

	if (!buf) {
		return NULL;
	}

	// what the game knows of the file stays as it was
	saved = g_FileInfo[fileid];
	g_LoadType = loadtype;
	fileLoadToAddr(fileid, FILELOADMETHOD_EXTRAMEM, buf, size);
	g_FileInfo[fileid] = saved;

	return buf;
}

// The intro stream's command lengths: modRandomIntroCmdLen() has why
static s32 simnavIntroCmdLen(s32 type)
{
	switch (type) {
	case INTROCMD_SPAWN:        return 12;
	case INTROCMD_WEAPON:       return 16;
	case INTROCMD_AMMO:         return 16;
	case INTROCMD_3:            return 32;
	case INTROCMD_4:            return 8;
	case INTROCMD_OUTFIT:       return 8;
	case INTROCMD_6:            return 40;
	case INTROCMD_WATCHTIME:    return 12;
	case INTROCMD_CREDITOFFSET: return 8;
	case INTROCMD_CASE:         return 12;
	case INTROCMD_CASERESPAWN:  return 12;
	case INTROCMD_HILL:         return 8;
	}

	return 0;
}

static void simnavAddPadPoint(struct simnavstagepads *sp, s32 padnum)
{
	struct pad pad;

	if (sp->numpoints >= SIMNAV_MAXPADPOINTS || padnum < 0 || padnum >= g_PadsFile->numpads) {
		return;
	}

	padUnpack(padnum, PADFIELD_POS, &pad);
	sp->points[sp->numpoints][0] = pad.pos.x;
	sp->points[sp->numpoints][1] = pad.pos.y;
	sp->points[sp->numpoints][2] = pad.pos.z;
	sp->numpoints++;
}

/**
 * The stage's pads file and setup file (the multiplayer one where it has
 * one), each loaded into memory of our own and read without being set up: a
 * level's own copies are not loaded yet when its mesh is built (lvReset()
 * reaches setupLoadFiles() after the tiles), and the batch has no level. The
 * pad globals padUnpack() reads are pointed at ours for the while and put
 * back.
 */
static void simnavReadSetup(u8 *setupbuf, struct simnavstagepads *sp)
{
	struct stagesetup *setup = (struct stagesetup *)setupbuf;

	if (setup->intro) {
		const s32 *cmd = (const s32 *)(setupbuf + (uintptr_t)setup->intro);

		while (cmd[0] != INTROCMD_END) {
			const s32 len = simnavIntroCmdLen(cmd[0]);

			if (len == 0) {
				break;
			}

			if (cmd[0] == INTROCMD_SPAWN && cmd[2] == 0) {
				simnavAddPadPoint(sp, cmd[1]);
				sp->numspawn++;
			}

			cmd = (const s32 *)((const u8 *)cmd + len);
		}
	}

	if (setup->props) {
		u32 *cmd = (u32 *)(setupbuf + (uintptr_t)setup->props);
		s32 guard = 0;

		while (((struct defaultobj *)cmd)->type != OBJTYPE_END && guard++ < 0x10000) {
			struct defaultobj *obj = (struct defaultobj *)cmd;
			u32 len;

			switch (obj->type) {
			case OBJTYPE_WEAPON:
			case OBJTYPE_AMMOCRATE:
			case OBJTYPE_MULTIAMMOCRATE:
				simnavAddPadPoint(sp, obj->pad);
				sp->numweapon++;
				break;
			case OBJTYPE_LIFT:
				if (sp->numlifts < SIMNAV_MAXLIFTS) {
					struct liftobj *lift = (struct liftobj *)obj;
					struct simnavlift *l = &sp->lifts[sp->numlifts];
					s32 i;

					for (i = 0; i < 4; i++) {
						struct pad pad;

						if (lift->pads[i] < 0 || lift->pads[i] >= g_PadsFile->numpads) {
							continue;
						}

						padUnpack(lift->pads[i], PADFIELD_POS, &pad);
						l->stops[l->numstops][0] = pad.pos.x;
						l->stops[l->numstops][1] = pad.pos.y;
						l->stops[l->numstops][2] = pad.pos.z;
						l->numstops++;
					}

					if (l->numstops >= 2) {
						sp->numlifts++;
					} else {
						memset(l, 0, sizeof(*l));
					}
				}
				break;
			}

			len = setupGetCmdLength(cmd);

			if (len == 0) {
				break;
			}

			cmd += len;
		}
	}

}

static void simnavLoadStagePads(s32 stagenum, struct simnavstagepads *sp)
{
	const s32 index = stageGetIndex(stagenum);
	struct padsfileheader *savedfile = g_PadsFile;
	u16 *savedoffsets = g_PadOffsets;
	s8 *saveddata = g_StageSetup.padfiledata;
	u8 *pads;
	s32 i;

	memset(sp, 0, sizeof(*sp));

	if (index < 0 || (pads = simnavLoadFile(g_Stages[index].padsfileid, LOADTYPE_PADS)) == NULL) {
		return;
	}

	g_StageSetup.padfiledata = (s8 *)pads;
	g_PadsFile = (struct padsfileheader *)pads;
#ifdef PLATFORM_64BIT
	g_PadOffsets = (u16 *)(pads + 0x20);
#else
	g_PadOffsets = (u16 *)(pads + 0x14);
#endif

	// The multiplayer setup, or the solo one where that has no pads to read
	// (a solo stage the Stage Loader offers as an arena)
	for (i = 0; i < 2 && sp->numpoints == 0; i++) {
		const s32 fileid = i == 0 ? g_Stages[index].mpsetupfileid : g_Stages[index].setupfileid;
		u8 *setupbuf;

		if (fileid == 0 || (i == 1 && fileid == g_Stages[index].mpsetupfileid)) {
			continue;
		}

		if ((setupbuf = simnavLoadFile(fileid, LOADTYPE_SETUP)) != NULL) {
			memset(sp, 0, sizeof(*sp));
			simnavReadSetup(setupbuf, sp);
			free(setupbuf);
		}
	}

	g_PadsFile = savedfile;
	g_PadOffsets = savedoffsets;
	g_StageSetup.padfiledata = saveddata;

	free(pads);
}

/** FNV-1a over the input and the parameters it is built with */
static u64 simnavHash(const struct simnavgeom *g, const struct simnavparams *params, const struct simnavlinkparams *lp)
{
	u64 h = 0xcbf29ce484222325ull;
	const u32 version = SIMNAV_EXTRACT_VERSION;
	const struct { const void *p; size_t len; } parts[] = {
		{ &version, sizeof(version) },
		{ params, sizeof(*params) },
		{ lp, sizeof(*lp) },
		{ g->verts, sizeof(f32) * 3 * g->numverts },
		{ g->tris, sizeof(s32) * 3 * g->numtris },
		{ g->areas, g->numtris },
		{ g->kinds, g->numtris },
		{ g->ladders, sizeof(*g->ladders) * g->numladders },
		{ g->lifts, sizeof(*g->lifts) * g->numlifts },
	};
	size_t i, j;

	for (i = 0; i < ARRAYCOUNT(parts); i++) {
		const u8 *b = parts[i].p;

		for (j = 0; j < parts[i].len; j++) {
			h ^= b[j];
			h *= 0x100000001b3ull;
		}
	}

	return h;
}

/*
 * Cache
 */

struct simnavcachehdr {
	u32 magic;
	u32 version;
	u64 hash;
	s32 stagenum;
	s32 pad;
};

static s32 simnavCachePath(s32 stagenum, u64 hash, char *dst, u32 dstlen)
{
	char rel[FS_MAXPATH + 1];
	char sub[FS_MAXPATH + 1];

	if (fsChooseOutputDir("cache", rel, sizeof(rel)) != 0) {
		return 0;
	}

	snprintf(sub, sizeof(sub), "%s/navmesh", rel);

	if (fsFileSize(sub) < 0) {
		fsCreateDir(sub);
	}

	snprintf(sub, sizeof(sub), "%s/navmesh/pd", rel);

	if (fsFileSize(sub) < 0) {
		fsCreateDir(sub);
	}

	snprintf(dst, dstlen, "%s/%02x-%016llx.bin", sub, stagenum, (unsigned long long)hash);

	return 1;
}

static struct simnavmesh *simnavCacheRead(const char *path, s32 stagenum, u64 hash)
{
	struct simnavcachehdr hdr;
	struct simnavmesh *mesh = NULL;
	u32 len = 0;
	u8 *buf;

	if (fsFileSize(path) < (s32)sizeof(hdr)) {
		return NULL;
	}

	buf = fsFileLoad(path, &len);

	if (!buf) {
		return NULL;
	}

	if (len >= sizeof(hdr)) {
		memcpy(&hdr, buf, sizeof(hdr));

		if (hdr.magic == SIMNAV_CACHE_MAGIC && hdr.version == SIMNAV_EXTRACT_VERSION
				&& hdr.hash == hash && hdr.stagenum == stagenum) {
			mesh = simnavMeshLoad(buf + sizeof(hdr), len - sizeof(hdr));
		}
	}

	sysMemFree(buf);

	return mesh;
}

static void simnavCacheWrite(const char *path, s32 stagenum, u64 hash, const struct simnavmesh *mesh)
{
	struct simnavcachehdr hdr;
	char tmp[FS_MAXPATH + 16];
	uint8_t *buf = NULL;
	size_t len = 0;
	FILE *f;
	s32 ok;

	if (!simnavMeshSave(mesh, &buf, &len)) {
		return;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = SIMNAV_CACHE_MAGIC;
	hdr.version = SIMNAV_EXTRACT_VERSION;
	hdr.hash = hash;
	hdr.stagenum = stagenum;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fsFileOpenWrite(tmp);

	if (!f) {
		free(buf);
		sysLogPrintf(LOG_WARNING, "simnav: could not write %s", tmp);
		return;
	}

	ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 && fwrite(buf, len, 1, f) == 1;
	fclose(f);
	free(buf);

	if (!ok || fsReplaceFile(tmp, path) != 0) {
		fsRemoveFile(tmp);
		sysLogPrintf(LOG_WARNING, "simnav: could not write %s", path);
	}
}

/**
 * The stage's mesh from its tiles: from the cache when its hash is there, or
 * built and cached. NULL, logged, when it cannot be had.
 */
static struct simnavmesh *simnavObtain(s32 stagenum, const u8 *tiledata, const struct simnavstagepads *pads,
		struct simnavgeom *g, s32 *fromcache, u64 *hashout)
{
	struct simnavparams params;
	struct simnavlinkparams linkparams;
	struct simnavinput input;
	struct simnavstats stats;
	struct simnavmesh *mesh;
	char path[FS_MAXPATH + 1];
	char err[256];
	s32 havepath;
	u64 hash;

	*fromcache = 0;
	simnavGetParams(&params);
	simnavGetLinkParams(&linkparams);

	if (!simnavExtract(tiledata, g)) {
		sysLogPrintf(LOG_WARNING, "simnav: stage 0x%02x: its tiles could not be read", stagenum);
		return NULL;
	}

	memcpy(g->lifts, pads->lifts, sizeof(g->lifts));
	g->numlifts = pads->numlifts;

	hash = simnavHash(g, &params, &linkparams);
	*hashout = hash;
	havepath = simnavCachePath(stagenum, hash, path, sizeof(path));

	if (havepath && (mesh = simnavCacheRead(path, stagenum, hash)) != NULL) {
		*fromcache = 1;
		return mesh;
	}

	input.verts = g->verts;
	input.numverts = g->numverts;
	input.tris = g->tris;
	input.areas = g->areas;
	input.kinds = g->kinds;
	input.numtris = g->numtris;
	input.ladders = g->ladders;
	input.numladders = g->numladders;
	input.lifts = g->lifts;
	input.numlifts = g->numlifts;

	mesh = simnavMeshBuild(&input, &params, &linkparams, &stats, err, sizeof(err));

	if (!mesh) {
		sysLogPrintf(LOG_WARNING, "simnav: stage 0x%02x: no navmesh: %s", stagenum, err[0] ? err : "unknown");
		return NULL;
	}

	if (havepath) {
		simnavCacheWrite(path, stagenum, hash, mesh);
	}

	return mesh;
}

/*
 * The stage being played, and its debug view
 */

#define SIMNAV_DEBUG_PATHS 4  // simulants whose path to the player is drawn, one worked out a frame
#define SIMNAV_PATH_POINTS 48
#define SIMNAV_LINK_QUADS  16 // most a link's line takes (a jump's arc: eight pieces, crossed)

struct simnavdrawtile {
	f32 minx, minz, maxx, maxz;
	Gfx *dl;
};

struct simnavdebugpath {
	struct chrdata *chr;
	s32 numpoints;
	s32 complete;
	f32 points[SIMNAV_PATH_POINTS][3];
	u8 areas[SIMNAV_PATH_POINTS];
};

// Lines drawn as two long quads crossed along them, so they show from any side
struct simnavribbons {
	Vtx *vtx;
	Col *col;
	s32 num; // quads
	s32 max;
};

static struct {
	s32 stagenum;
	struct simnavmesh *mesh;
	struct simnavquery *query;
	struct simnavdrawtile *tiles;
	s32 numtiles;
	Vtx *vtx;
	Col *col;
	Gfx *gfx;
	struct simnavribbons links;
	Gfx *linkgfx;
	struct simnavdebugpath paths[SIMNAV_DEBUG_PATHS];
	s32 nextpath;
	s32 pathframe;
} g_SimNav = { -1 };

struct simnavtri {
	s32 tile;
	s32 poly;
	u8 area;
	f32 v[3][3];
};

struct simnavtrilist {
	struct simnavtri *tris;
	s32 num, max;
};

static void simnavCollectTri(void *arg, int tile, int poly, unsigned char area, const float *a, const float *b, const float *c)
{
	struct simnavtrilist *list = arg;
	struct simnavtri *t;

	if (list->num >= list->max) {
		s32 max = list->max ? list->max * 2 : 4096;
		struct simnavtri *n = realloc(list->tris, sizeof(*n) * max);

		if (!n) {
			return;
		}

		list->tris = n;
		list->max = max;
	}

	t = &list->tris[list->num++];
	t->tile = tile;
	t->poly = poly;
	t->area = area;
	memcpy(t->v[0], a, sizeof(t->v[0]));
	memcpy(t->v[1], b, sizeof(t->v[1]));
	memcpy(t->v[2], c, sizeof(t->v[2]));
}

static void simnavPolyColour(u8 area, s32 poly, Col *col)
{
	static const u8 colours[SIMNAV_NUMAREAS][3] = {
		{ 0x80, 0x80, 0x80 }, // none (not drawn: Detour keeps no unwalkable polys)
		{ 0x20, 0xa0, 0xff }, // ground
		{ 0xff, 0xc0, 0x20 }, // slope
		{ 0x40, 0xff, 0x60 }, // step
		{ 0x20, 0x40, 0xff }, // water
		{ 0xff, 0x40, 0xff }, // ladder floor
		{ 0x80, 0x80, 0x80 }, // (links only)
		{ 0x80, 0x80, 0x80 },
		{ 0x80, 0x80, 0x80 },
		{ 0xa0, 0x60, 0xff }, // duck
		{ 0xff, 0x50, 0x50 }, // crouch
	};
	const u8 *c = colours[area < SIMNAV_NUMAREAS ? area : 0];
	// neighbouring polygons a shade apart, so their edges show
	u32 h = (u32)poly * 2654435761u;
	s32 shade = 160 + (s32)((h >> 24) % 96);

	col->r = c[0] * shade / 255;
	col->g = c[1] * shade / 255;
	col->b = c[2] * shade / 255;
	col->a = SIMNAV_DRAW_ALPHA;
}

/** A link's colour by its kind; a path walked is yellow */
static void simnavLinkColour(u8 area, u8 *rgb)
{
	switch (area) {
	case SIMNAV_AREA_LADDER: rgb[0] = 0xff; rgb[1] = 0x40; rgb[2] = 0xff; break;
	case SIMNAV_AREA_LIFT:   rgb[0] = 0x40; rgb[1] = 0xff; rgb[2] = 0x60; break;
	case SIMNAV_AREA_DROP:   rgb[0] = 0xff; rgb[1] = 0x70; rgb[2] = 0x10; break;
	case SIMNAV_AREA_JUMP:   rgb[0] = 0x10; rgb[1] = 0xff; rgb[2] = 0xff; break;
	default:                 rgb[0] = 0xff; rgb[1] = 0xff; rgb[2] = 0x30; break;
	}
}

static s16 simnavS16(f32 v)
{
	return v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (s16)floorf(v + 0.5f);
}

/** One line of a ribbon list, from a (in colour ca) to b (in cb) */
static void simnavRibbon(struct simnavribbons *r, const f32 *a, const f32 *b, const u8 *ca, const u8 *cb, f32 halfwidth)
{
	f32 d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	f32 len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	f32 across = sqrtf(d[0] * d[0] + d[2] * d[2]);
	f32 w[2][3];
	s32 q, k;

	if (len < 1.0f) {
		return;
	}

	// across the line and level, or along x where the line stands up
	if (across > len * 0.01f) {
		w[0][0] = -d[2] / across;
		w[0][1] = 0;
		w[0][2] = d[0] / across;
	} else {
		w[0][0] = 1;
		w[0][1] = 0;
		w[0][2] = 0;
	}

	// and across both
	w[1][0] = d[1] * w[0][2] - d[2] * w[0][1];
	w[1][1] = d[2] * w[0][0] - d[0] * w[0][2];
	w[1][2] = d[0] * w[0][1] - d[1] * w[0][0];

	for (k = 0; k < 3; k++) {
		w[1][k] /= len;
	}

	for (q = 0; q < 2; q++) {
		const f32 *corner[4] = { a, b, b, a };
		const f32 side[4] = { 1, 1, -1, -1 };
		Vtx *v;
		Col *c;

		if (r->num >= r->max) {
			return;
		}

		v = &r->vtx[r->num * 4];
		c = &r->col[r->num * 4];

		for (k = 0; k < 4; k++) {
			const u8 *rgb = k == 1 || k == 2 ? cb : ca;

			v[k].x = simnavS16(corner[k][0] + w[q][0] * halfwidth * side[k]);
			v[k].y = simnavS16(corner[k][1] + w[q][1] * halfwidth * side[k]);
			v[k].z = simnavS16(corner[k][2] + w[q][2] * halfwidth * side[k]);
			v[k].flags = 0;
			v[k].colour = ((r->num % 3) * 4 + k) * 4;
			v[k].s = 0;
			v[k].t = 0;

			c[k].r = rgb[0];
			c[k].g = rgb[1];
			c[k].b = rgb[2];
			c[k].a = 0xe0;
		}

		r->num++;
	}
}

/** The ribbons' display list: three quads to a load of twelve vertices */
static Gfx *simnavRibbonGfx(Gfx *gdl, const struct simnavribbons *r)
{
	s32 i;

	for (i = 0; i < r->num; i += 3) {
		const s32 n = r->num - i < 3 ? r->num - i : 3;

		gSPColor(gdl++, &r->col[i * 4], n * 4);
		gSPVertex(gdl++, &r->vtx[i * 4], n * 4, 0);

		if (n == 1) {
			gSPTri2(gdl++, 0, 1, 2, 0, 2, 3);
		} else {
			gSPTri4(gdl++, 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7);

			if (n == 3) {
				gSPTri2(gdl++, 8, 9, 10, 8, 10, 11);
			}
		}
	}

	return gdl;
}

/** A link's line: up a ladder then over, out then down a drop, a jump's arc, straight up a lift */
static void simnavLinkRibbons(struct simnavribbons *r, u8 area, const f32 *a, const f32 *b)
{
	f32 pts[9][3];
	u8 dim[3], full[3];
	s32 n = 0, i, k;

	simnavLinkColour(area, full);

	for (k = 0; k < 3; k++) {
		dim[k] = full[k] / 3;
	}

	for (k = 0; k < 3; k++) {
		pts[0][k] = a[k];
	}

	if (area == SIMNAV_AREA_JUMP) {
		const f32 h = 60.0f;

		for (i = 1; i <= 8; i++) {
			const f32 t = i / 8.0f;

			for (k = 0; k < 3; k++) {
				pts[i][k] = a[k] + (b[k] - a[k]) * t;
			}

			pts[i][1] += 4.0f * h * t * (1.0f - t);
		}

		n = 9;
	} else if (area == SIMNAV_AREA_LADDER || area == SIMNAV_AREA_DROP) {
		// the ladder's climb at its foot, the drop's fall at its landing
		const f32 *vert = area == SIMNAV_AREA_LADDER ? a : b;
		const f32 *level = area == SIMNAV_AREA_LADDER ? b : a;

		pts[1][0] = vert[0];
		pts[1][1] = level[1];
		pts[1][2] = vert[2];
		pts[2][0] = b[0];
		pts[2][1] = b[1];
		pts[2][2] = b[2];
		n = 3;
	} else {
		pts[1][0] = b[0];
		pts[1][1] = b[1];
		pts[1][2] = b[2];
		n = 2;
	}

	for (i = 0; i < n; i++) {
		pts[i][1] += SIMNAV_DRAW_LIFT + 6.0f;
	}

	// dim at the start, bright at the end, so a one-way link shows its way
	for (i = 0; i + 1 < n; i++) {
		u8 c0[3], c1[3];

		for (k = 0; k < 3; k++) {
			c0[k] = dim[k] + (full[k] - dim[k]) * i / (n - 1);
			c1[k] = dim[k] + (full[k] - dim[k]) * (i + 1) / (n - 1);
		}

		simnavRibbon(r, pts[i], pts[i + 1], c0, c1, 3.0f);
	}
}

struct simnavlinkcount {
	s32 num;
	struct simnavribbons *r;
};

static void simnavCountLink(void *arg, unsigned char area, unsigned short flags, int bidir, const float *a, const float *b)
{
	((struct simnavlinkcount *)arg)->num++;
}

static void simnavDrawLink(void *arg, unsigned char area, unsigned short flags, int bidir, const float *a, const float *b)
{
	simnavLinkRibbons(((struct simnavlinkcount *)arg)->r, area, a, b);
}

static void simnavFreeDraw(void)
{
	free(g_SimNav.tiles);
	free(g_SimNav.vtx);
	free(g_SimNav.col);
	free(g_SimNav.gfx);
	free(g_SimNav.links.vtx);
	free(g_SimNav.links.col);
	free(g_SimNav.linkgfx);
	g_SimNav.tiles = NULL;
	g_SimNav.vtx = NULL;
	g_SimNav.col = NULL;
	g_SimNav.gfx = NULL;
	g_SimNav.numtiles = 0;
	memset(&g_SimNav.links, 0, sizeof(g_SimNav.links));
	g_SimNav.linkgfx = NULL;
}

/** Every off-mesh link as a line, in one display list built once */
static void simnavBuildLinkDraw(void)
{
	struct simnavlinkcount count = { 0 };
	Gfx *gdl;

	simnavMeshForEachLink(g_SimNav.mesh, simnavCountLink, &count);

	if (count.num == 0) {
		return;
	}

	g_SimNav.links.max = count.num * SIMNAV_LINK_QUADS;
	g_SimNav.links.vtx = calloc((size_t)g_SimNav.links.max * 4, sizeof(Vtx));
	g_SimNav.links.col = calloc((size_t)g_SimNav.links.max * 4, sizeof(Col));
	g_SimNav.linkgfx = calloc((size_t)g_SimNav.links.max + 1, sizeof(Gfx) * 2);

	if (!g_SimNav.links.vtx || !g_SimNav.links.col || !g_SimNav.linkgfx) {
		return;
	}

	count.r = &g_SimNav.links;
	simnavMeshForEachLink(g_SimNav.mesh, simnavDrawLink, &count);

	gdl = simnavRibbonGfx(g_SimNav.linkgfx, &g_SimNav.links);
	gSPEndDisplayList(gdl++);
}

/**
 * One display list per tile, built once: five triangles to a load of fifteen
 * vertices, each vertex its own (the mesh is a debug view, and a triangle's
 * own corners keep its polygon's colour).
 */
static void simnavBuildDraw(void)
{
	struct simnavtrilist list = { 0 };
	s32 numtiles = simnavMeshNumTiles(g_SimNav.mesh);
	s32 numbatches;
	s32 i, t;
	Vtx *vtx;
	Col *col;
	Gfx *gdl;

	simnavBuildLinkDraw();
	simnavMeshForEachTri(g_SimNav.mesh, simnavCollectTri, &list);

	if (list.num == 0 || numtiles <= 0) {
		free(list.tris);
		return;
	}

	numbatches = list.num / 5 + numtiles;

	g_SimNav.tiles = calloc(numtiles, sizeof(*g_SimNav.tiles));
	g_SimNav.vtx = calloc((size_t)numbatches * 15, sizeof(Vtx));
	g_SimNav.col = calloc((size_t)numbatches * 15, sizeof(Col));
	g_SimNav.gfx = calloc((size_t)numbatches * 4 + numtiles, sizeof(Gfx));

	if (!g_SimNav.tiles || !g_SimNav.vtx || !g_SimNav.col || !g_SimNav.gfx) {
		simnavFreeDraw();
		free(list.tris);
		return;
	}

	g_SimNav.numtiles = numtiles;
	vtx = g_SimNav.vtx;
	col = g_SimNav.col;
	gdl = g_SimNav.gfx;

	// the triangles come tile by tile
	for (i = 0; i < list.num; ) {
		struct simnavdrawtile *tile;

		t = list.tris[i].tile;

		if (t < 0 || t >= numtiles) {
			i++;
			continue;
		}

		tile = &g_SimNav.tiles[t];
		tile->dl = gdl;
		tile->minx = tile->minz = 1e30f;
		tile->maxx = tile->maxz = -1e30f;

		while (i < list.num && list.tris[i].tile == t) {
			s32 n = 0;
			Vtx *batchvtx = vtx;
			Col *batchcol = col;

			while (n < 5 && i < list.num && list.tris[i].tile == t) {
				const struct simnavtri *tri = &list.tris[i];
				s32 k;

				for (k = 0; k < 3; k++) {
					const s32 index = n * 3 + k;

					vtx->x = simnavS16(tri->v[k][0]);
					vtx->y = simnavS16(tri->v[k][1] + SIMNAV_DRAW_LIFT);
					vtx->z = simnavS16(tri->v[k][2]);
					vtx->flags = 0;
					vtx->colour = index * 4;
					vtx->s = 0;
					vtx->t = 0;
					vtx++;

					simnavPolyColour(tri->area, tri->poly, col);
					col++;

					if (tri->v[k][0] < tile->minx) tile->minx = tri->v[k][0];
					if (tri->v[k][0] > tile->maxx) tile->maxx = tri->v[k][0];
					if (tri->v[k][2] < tile->minz) tile->minz = tri->v[k][2];
					if (tri->v[k][2] > tile->maxz) tile->maxz = tri->v[k][2];
				}

				n++;
				i++;
			}

			gSPColor(gdl++, batchcol, n * 3);
			gSPVertex(gdl++, batchvtx, n * 3, 0);

			if (n >= 4) {
				gSPTri4(gdl++, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
			} else {
				gSPTri4(gdl++, 0, 1, 2,
						n > 1 ? 3 : 0, n > 1 ? 4 : 0, n > 1 ? 5 : 0,
						n > 2 ? 6 : 0, n > 2 ? 7 : 0, n > 2 ? 8 : 0,
						0, 0, 0);
			}

			if (n == 5) {
				gSPTri1(gdl++, 12, 13, 14);
			}
		}

		gSPEndDisplayList(gdl++);
	}

	free(list.tris);
}

static void simnavLogStats(s32 stagenum, const struct simnavstats *s, const struct simnavgeom *g, s32 fromcache, u64 hash)
{
	sysLogPrintf(LOG_NOTE, "simnav: stage 0x%02x: %d polys, %d verts, %d detail tris in %d tiles (%d empty, %d failed); "
			"%d floor, %d wall, %d death, %d block tiles, %d others left out; %s %.1f ms; hash %016llx",
			stagenum, s->polys, s->verts, s->detailtris, s->tiles, s->emptytiles, s->failedtiles,
			g->floors, g->walls, g->died, g->blocks, g->skipped,
			fromcache ? "from the cache, built in" : "built in", s->buildms, (unsigned long long)hash);
	sysLogPrintf(LOG_NOTE, "simnav: stage 0x%02x: links: %d ladder (%d faces, %d ladders), %d lift (%d lifts), %d drop, "
			"%d jump (by Jump Height 1-5: %d %d %d %d %d) in %.1f ms",
			stagenum, s->ladderlinks, g->laddertiles, g->numladders, s->liftlinks, g->numlifts, s->droplinks,
			s->jumplinks, s->jumpsbyheight[0], s->jumpsbyheight[1], s->jumpsbyheight[2], s->jumpsbyheight[3],
			s->jumpsbyheight[4], s->linkms);
}

/** What a simulant may use of the mesh with the options as they are */
static u16 simnavIncludeFlags(void)
{
	u16 flags = SIMNAV_FLAGS_NOJUMP;
	s32 h;

	if (modCanChrJump()) {
		for (h = 1; h <= modGetJumpHeight() && h <= SIMNAV_MAXJUMPHEIGHTS; h++) {
			flags |= SIMNAV_FLAG_JUMP(h);
		}
	}

	return flags;
}

void simnavStageStop(void)
{
	simnavFreeDraw();
	simnavQueryFree(g_SimNav.query);
	simnavMeshFree(g_SimNav.mesh);
	g_SimNav.query = NULL;
	g_SimNav.mesh = NULL;
	g_SimNav.stagenum = -1;
	memset(g_SimNav.paths, 0, sizeof(g_SimNav.paths));
	g_SimNav.nextpath = 0;
	g_SimNav.pathframe = -1;
}

void simnavStageStart(s32 stagenum)
{
	struct simnavstagepads *pads;
	struct simnavgeom g;
	struct simnavstats stats;
	s32 fromcache;
	u64 hash = 0;

	if (g_SimNav.mesh) {
		simnavStageStop();
	}

	if (!simnavDebugEnabled()) {
		return;
	}

	if (!g_TileFileData.u8) {
		return;
	}

	if (modloaderStageIsRemake(stagenum)) {
		sysLogPrintf(LOG_NOTE, "simnav: stage 0x%02x is a converted GoldenEye level; its collision is gestan.c's, not built (M1)", stagenum);
		return;
	}

	pads = calloc(1, sizeof(*pads));

	if (!pads) {
		return;
	}

	simnavLoadStagePads(stagenum, pads);
	g_SimNav.mesh = simnavObtain(stagenum, g_TileFileData.u8, pads, &g, &fromcache, &hash);

	if (g_SimNav.mesh) {
		g_SimNav.stagenum = stagenum;
		g_SimNav.query = simnavQueryCreate(g_SimNav.mesh);
		g_SimNav.pathframe = -1;
		simnavMeshGetStats(g_SimNav.mesh, &stats);
		simnavLogStats(stagenum, &stats, &g, fromcache, hash);
		simnavBuildDraw();
	}

	simnavGeomFree(&g);
	free(pads);
}

/**
 * Once a frame, the path of one of the first SIMNAV_DEBUG_PATHS simulants to
 * the player - or, spectating, to the floor under the camera - in turn.
 */
static void simnavUpdatePaths(struct player *player)
{
	struct simnavdebugpath *path;
	struct chrdata *chr = NULL;
	f32 start[3], end[3];
	s32 i, n = 0;

	if (!g_SimNav.query || g_SimNav.pathframe == g_Vars.lvframe60) {
		return;
	}

	g_SimNav.pathframe = g_Vars.lvframe60;
	path = &g_SimNav.paths[g_SimNav.nextpath];

	for (i = 0; i < g_MpNumChrs && i < MAX_MPCHRS; i++) {
		struct chrdata *c = g_MpAllChrPtrs[i];

		if (c && c->aibot && c->prop && !chrIsDead(c)) {
			if (n == g_SimNav.nextpath) {
				chr = c;
				break;
			}

			n++;
		}
	}

	g_SimNav.nextpath = (g_SimNav.nextpath + 1) % SIMNAV_DEBUG_PATHS;
	path->chr = chr;
	path->numpoints = 0;

	if (!chr) {
		return;
	}

	if (modSpectateIsOn() || !player->prop) {
		const f32 cam[3] = { player->cam_pos.x, player->cam_pos.y, player->cam_pos.z };

		if (!simnavQueryFindFloor(g_SimNav.query, cam, 5000.0f, 0.0f, 150.0f, end)) {
			return;
		}

		end[1] += 60.0f;
	} else {
		end[0] = player->prop->pos.x;
		end[1] = player->prop->pos.y;
		end[2] = player->prop->pos.z;
	}

	start[0] = chr->prop->pos.x;
	start[1] = chr->prop->pos.y;
	start[2] = chr->prop->pos.z;

	path->numpoints = simnavQueryPath(g_SimNav.query, start, end, simnavIncludeFlags(),
			&path->points[0][0], path->areas, SIMNAV_PATH_POINTS, &path->complete);
}

/** The paths, into this frame's memory */
static Gfx *simnavRenderPaths(Gfx *gdl)
{
	struct simnavribbons r;
	s32 numsegs = 0;
	s32 i, j;

	for (i = 0; i < SIMNAV_DEBUG_PATHS; i++) {
		if (g_SimNav.paths[i].numpoints > 1) {
			numsegs += g_SimNav.paths[i].numpoints - 1;
		}
	}

	if (numsegs == 0) {
		return gdl;
	}

	r.max = numsegs * 2;
	r.num = 0;
	r.vtx = gfxAllocateVertices(r.max * 4);
	r.col = gfxAllocateColours(r.max * 4);

	for (i = 0; i < SIMNAV_DEBUG_PATHS; i++) {
		const struct simnavdebugpath *path = &g_SimNav.paths[i];

		for (j = 0; j + 1 < path->numpoints; j++) {
			f32 a[3], b[3];
			u8 rgb[3];

			memcpy(a, path->points[j], sizeof(a));
			memcpy(b, path->points[j + 1], sizeof(b));
			a[1] += SIMNAV_DRAW_LIFT + 14.0f;
			b[1] += SIMNAV_DRAW_LIFT + 14.0f;

			// a link crossed is its own colour, the walking yellow - or
			// red, where the path stops short
			simnavLinkColour(path->areas[j], rgb);

			if (!path->complete && path->areas[j] == SIMNAV_AREA_NONE) {
				rgb[0] = 0xff;
				rgb[1] = 0x30;
				rgb[2] = 0x30;
			}

			simnavRibbon(&r, a, b, rgb, rgb, 5.0f);
		}
	}

	return simnavRibbonGfx(gdl, &r);
}

Gfx *simnavRender(Gfx *gdl)
{
	struct player *player = g_Vars.currentplayer;
	Mtxf *mtx;
	s32 i;

	if (!g_SimNav.mesh || !g_SimNav.tiles || !player || g_SimNav.stagenum != g_Vars.stagenum) {
		return gdl;
	}

	simnavUpdatePaths(player);

	// the mesh's vertices are the world's, so the rooms' translation with no room
	mtx = gfxAllocateMatrix();
	mtx4LoadIdentity(mtx);
	mtx->m[3][0] = -player->globaldrawworldoffset.x;
	mtx->m[3][1] = -player->globaldrawworldoffset.y;
	mtx->m[3][2] = -player->globaldrawworldoffset.z;
	mtxApplyGfxScale(mtx);

	gDPPipeSync(gdl++);
	gSPMatrix(gdl++, osVirtualToPhysical(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH | G_FOG | G_LIGHTING | G_TEXTURE_GEN);
	gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER);
	texSelect(&gdl, NULL, 2, 0, 2, 1, NULL);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gDPSetRenderMode(gdl++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);

	for (i = 0; i < g_SimNav.numtiles; i++) {
		const struct simnavdrawtile *tile = &g_SimNav.tiles[i];
		f32 dx, dz;

		if (!tile->dl) {
			continue;
		}

		dx = player->cam_pos.x < tile->minx ? tile->minx - player->cam_pos.x
			: player->cam_pos.x > tile->maxx ? player->cam_pos.x - tile->maxx : 0;
		dz = player->cam_pos.z < tile->minz ? tile->minz - player->cam_pos.z
			: player->cam_pos.z > tile->maxz ? player->cam_pos.z - tile->maxz : 0;

		if (dx * dx + dz * dz > SIMNAV_DRAW_REACH * SIMNAV_DRAW_REACH) {
			continue;
		}

		gSPDisplayList(gdl++, tile->dl);
	}

	// the links and paths, over the mesh (which writes no depth)
	if (g_SimNav.linkgfx) {
		gSPDisplayList(gdl++, g_SimNav.linkgfx);
	}

	gdl = simnavRenderPaths(gdl);

	gDPPipeSync(gdl++);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH);

	return gdl;
}

/*
 * --simnav-build-all
 */

static const char *simnavArenaName(s32 index)
{
	static const char *names[] = {
		"Skedar", "Pipes", "Ravine", "G5 Building", "Sewers", "Warehouse", "Grid", "Ruins",
		"Area 52", "Base", "Fortress", "Villa", "Car Park", "Temple", "Complex", "Felicity",
	};

	return index < (s32)ARRAYCOUNT(names) ? names[index] : NULL;
}

static void simnavPrintRowEmpty(s32 stagenum, const char *name, const char *note)
{
	printf("0x%02x  %-18s  %5s %5s %4s %7s | %4s %4s %4s %4s %6s | %4s %6s %6s %6s  %s\n", stagenum, name,
			"-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", note);
}



/**
 * The pads the reachability counted against the mesh: each off it, and each
 * outside the largest group of pads that all reach one another
 */
static void simnavLogStrays(s32 stagenum, const struct simnavstagepads *pads, const s32 *groups)
{
	s32 sizes[SIMNAV_MAXPADPOINTS] = { 0 };
	s32 biggest = -1;
	s32 i;

	for (i = 0; i < pads->numpoints; i++) {
		if (groups[i] >= 0 && ++sizes[groups[i]] > (biggest >= 0 ? sizes[biggest] : 0)) {
			biggest = groups[i];
		}
	}

	for (i = 0; i < pads->numpoints; i++) {
		if (groups[i] == biggest) {
			continue;
		}

		sysLogPrintf(LOG_NOTE, "simnav: stage 0x%02x: %s pad %d at (%.0f, %.0f, %.0f) %s",
				stagenum, i < pads->numspawn ? "spawn" : "weapon", i,
				pads->points[i][0], pads->points[i][1], pads->points[i][2],
				groups[i] < 0 ? "is off the mesh" : "is cut off from the most pads");
	}
}

void simnavBuildAllFromCommandLine(void)
{
	s32 built = 0, failed = 0, skipped = 0;
	f32 totalms = 0, totallinkms = 0;
	struct simnavstagepads *pads;
	s32 groups[SIMNAV_MAXPADPOINTS];
	s32 i;

	if (!sysArgCheck("--simnav-build-all")) {
		return;
	}

	pads = calloc(1, sizeof(*pads));

	if (!pads) {
		exit(1);
	}

	sysLogPrintf(LOG_NOTE, "simnav: building every multiplayer arena's navmesh");
	printf("\n%-4s  %-18s  %5s %5s %4s %7s | %4s %4s %4s %4s %6s | %4s %6s %6s %6s  %s\n",
			"id", "arena", "polys", "tiles", "fail", "buildms", "lad", "lift", "drop", "jump", "linkms",
			"pads", "onmesh", "reach", "+jump", "note");

	for (i = 0; i < MP_NUM_ARENAS_STATIC; i++) {
		const s32 stagenum = g_MpArenas[i].stagenum;
		const s32 index = stageGetIndex(stagenum);
		const char *name = simnavArenaName(i);
		char namebuf[32];
		struct simnavgeom g;
		struct simnavstats stats;
		struct simnavmesh *mesh;
		s32 fromcache, fileid;
		u64 hash = 0;
		u8 *buf;

		if (stagenum == STAGE_MP_RANDOM) {
			continue;
		}

		if (!name) {
			snprintf(namebuf, sizeof(namebuf), "(solo 0x%02x)", stagenum);
			name = namebuf;
		}

		if (index < 0 || (fileid = g_Stages[index].tilefileid) == 0) {
			simnavPrintRowEmpty(stagenum, name, "no stage");
			skipped++;
			continue;
		}

		if (modloaderStageIsRemake(stagenum)) {
			simnavPrintRowEmpty(stagenum, name, "GoldenEye conversion, skipped");
			skipped++;
			continue;
		}

		// the file as tilesReset() loads it, into memory of our own
		buf = simnavLoadFile(fileid, LOADTYPE_TILES);

		if (!buf) {
			simnavPrintRowEmpty(stagenum, name, "no tiles file");
			failed++;
			continue;
		}

		simnavLoadStagePads(stagenum, pads);
		mesh = simnavObtain(stagenum, buf, pads, &g, &fromcache, &hash);

		if (mesh) {
			s32 onmesh = 0, pairs = 0, onmeshj = 0, pairsj = 0;
			const s32 n = pads->numpoints;
			const f32 allpairs = n > 1 ? n * (n - 1) / 2.0f : 1.0f;

			// pads stand up to about a chr's height over their floor
			simnavMeshReachability(mesh, &pads->points[0][0], n, 250.0f, 60.0f, SIMNAV_FLAGS_NOJUMP,
					&onmesh, &pairs, groups);
			simnavMeshReachability(mesh, &pads->points[0][0], n, 250.0f, 60.0f,
					SIMNAV_FLAGS_NOJUMP | SIMNAV_FLAG_JUMP(1), &onmeshj, &pairsj, NULL);
			simnavLogStrays(stagenum, pads, groups);

			simnavMeshGetStats(mesh, &stats);
			simnavLogStats(stagenum, &stats, &g, fromcache, hash);
			sysLogPrintf(LOG_NOTE, "simnav: stage 0x%02x: %d pads (%d spawn, %d weapon), %d on the mesh; "
					"%d of %d pairs reach each other, %d with Jump Height 1",
					stagenum, n, pads->numspawn, pads->numweapon, onmesh, pairs, (s32)allpairs, pairsj);
			printf("0x%02x  %-18s  %5d %5d %4d %7.1f | %4d %4d %4d %4d %6.1f | %4d %5.0f%% %5.0f%% %5.0f%%  %s\n",
					stagenum, name, stats.polys, stats.tiles, stats.failedtiles, stats.buildms,
					stats.ladderlinks, stats.liftlinks, stats.droplinks, stats.jumplinks, stats.linkms,
					n, n ? 100.0f * onmesh / n : 0.0f, 100.0f * pairs / allpairs, 100.0f * pairsj / allpairs,
					fromcache ? "cached" : "");
			totalms += stats.buildms;
			totallinkms += stats.linkms;
			built++;
			simnavMeshFree(mesh);
		} else {
			simnavPrintRowEmpty(stagenum, name, "FAILED (see log)");
			failed++;
		}

		simnavGeomFree(&g);
		free(buf);
	}

	printf("\nsimnav: %d built, %d failed, %d skipped, %.1f ms building (%.1f of it links)\n",
			built, failed, skipped, totalms, totallinkms);
	fflush(stdout);
	free(pads);

	exit(failed ? 1 : 0);
}
