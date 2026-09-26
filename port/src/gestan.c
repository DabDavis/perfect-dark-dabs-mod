#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "modloader.h"
#include "romdata.h"
#include "fs.h"
#include "system.h"
#include "gestan.h"

#ifndef PLATFORM_N64

#define GESTAN_CELL      512.0f
#define GESTAN_MAXFLOOD  512
#define GESTAN_NOWALL    (-2)
#define GESTAN_UNLINKED  (-1)
#define GESTAN_CLIMBWALL 0x4000
#define GESTAN_SPECIAL_CROUCH 1
#define GESTAN_RISE      60.0f   // how far over a body's foot its own floor may be: two of a stair's steps

struct stanpoint {
	s16 x, y, z;
	s16 across;      // the tile across the edge to the next point, -1 a wall, -2 nothing
	u8 climbwall;    // linked, and a wall raised on it all the same: the link climbs more than a step
};

struct stantile {
	s16 room;
	u8 special;
	u8 npts;
	s32 first;       // its first point in `points`
	s16 xmin, xmax, zmin, zmax;
};

struct stanwall {
	const struct geo *geo;
	s32 tile;
};

static struct {
	// what the graph was built for: a stage's tiles as they are loaded now
	s32 stagenum;
	const u8 *tiledata;
	bool active;

	struct stantile *tiles;
	struct stanpoint *points;
	s32 numtiles;

	struct stanwall *walls;
	s32 numwalls;

	// tiles by where they are: a grid over the level of GESTAN_CELL squares
	f32 gridx, gridz;
	s32 gridw, gridh;
	s32 *cellstart;
	s32 *celltiles;

	// the tiles a body reaches from where it stands, marked with `gen`
	u32 *reached;
	u32 gen;
	f32 lastx, lastz, lastx2, lastz2, lastlimit, lastrise, lastreach;
	bool lastfound;
} g_Stan = { .stagenum = -1 };

s32 g_GeStanAsked;
s32 g_GeStanSkipped;
s32 g_GeStanNoTile;

static s32 stanBe16(const u8 *p)
{
	return (s16)((p[0] << 8) | p[1]);
}

static void stanFree(void)
{
	free(g_Stan.tiles);
	free(g_Stan.points);
	free(g_Stan.walls);
	free(g_Stan.cellstart);
	free(g_Stan.celltiles);
	free(g_Stan.reached);
	g_Stan.tiles = NULL;
	g_Stan.points = NULL;
	g_Stan.walls = NULL;
	g_Stan.cellstart = NULL;
	g_Stan.celltiles = NULL;
	g_Stan.reached = NULL;
	g_Stan.numtiles = 0;
	g_Stan.numwalls = 0;
	g_Stan.active = false;
	g_Stan.lastreach = -1.0f;
}

/** The conversion's tile graph for the stage: its tiles file's name with the graph's ending. */
static u8 *stanLoadFile(u32 *len)
{
	const char *dir = modloaderGetStageModDir(g_Vars.stagenum);
	const char *tilesname = romdataFileGetName(g_Stages[g_StageIndex].tilefileid);
	char name[128];
	char path[FS_MAXPATH + 1];
	char *ending;

	if (!dir || !tilesname) {
		return NULL;
	}

	snprintf(name, sizeof(name), "%s", tilesname);
	ending = strstr(name, "_tilesZ");

	if (!ending || strlen(name) + 1 >= sizeof(name)) {
		return NULL;
	}

	strcpy(ending, "_stan");
	snprintf(path, sizeof(path), "%s/files/%s", dir, name);

	return fsFileLoad(path, len);
}

static s32 stanCellOf(f32 v, f32 origin, s32 count)
{
	s32 c = (s32)floorf((v - origin) / GESTAN_CELL);

	return c < 0 ? 0 : (c >= count ? count - 1 : c);
}

static void stanBuildGrid(void)
{
	s32 xmin = 32767, xmax = -32768, zmin = 32767, zmax = -32768;
	s32 *fill;
	s32 total = 0;

	for (s32 i = 0; i < g_Stan.numtiles; i++) {
		const struct stantile *t = &g_Stan.tiles[i];

		if (t->xmin < xmin) xmin = t->xmin;
		if (t->xmax > xmax) xmax = t->xmax;
		if (t->zmin < zmin) zmin = t->zmin;
		if (t->zmax > zmax) zmax = t->zmax;
	}

	g_Stan.gridx = (f32)xmin;
	g_Stan.gridz = (f32)zmin;
	g_Stan.gridw = (s32)((xmax - xmin) / GESTAN_CELL) + 1;
	g_Stan.gridh = (s32)((zmax - zmin) / GESTAN_CELL) + 1;
	g_Stan.cellstart = calloc((size_t)g_Stan.gridw * g_Stan.gridh + 1, sizeof(s32));

	if (!g_Stan.cellstart) {
		return;
	}

	for (s32 pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			// counts to starts, and a cursor a cell to fill from
			s32 run = 0;

			for (s32 c = 0; c < g_Stan.gridw * g_Stan.gridh; c++) {
				const s32 n = g_Stan.cellstart[c];

				g_Stan.cellstart[c] = run;
				run += n;
			}

			g_Stan.cellstart[g_Stan.gridw * g_Stan.gridh] = run;
			total = run;
			g_Stan.celltiles = malloc(sizeof(s32) * (size_t)(total ? total : 1));
			fill = calloc((size_t)g_Stan.gridw * g_Stan.gridh, sizeof(s32));

			if (!g_Stan.celltiles || !fill) {
				free(fill);
				free(g_Stan.cellstart);
				g_Stan.cellstart = NULL;
				return;
			}
		}

		for (s32 i = 0; i < g_Stan.numtiles; i++) {
			const struct stantile *t = &g_Stan.tiles[i];
			const s32 cx0 = stanCellOf(t->xmin, g_Stan.gridx, g_Stan.gridw);
			const s32 cx1 = stanCellOf(t->xmax, g_Stan.gridx, g_Stan.gridw);
			const s32 cz0 = stanCellOf(t->zmin, g_Stan.gridz, g_Stan.gridh);
			const s32 cz1 = stanCellOf(t->zmax, g_Stan.gridz, g_Stan.gridh);

			for (s32 cz = cz0; cz <= cz1; cz++) {
				for (s32 cx = cx0; cx <= cx1; cx++) {
					const s32 c = cz * g_Stan.gridw + cx;

					if (pass == 0) {
						g_Stan.cellstart[c]++;
					} else {
						g_Stan.celltiles[g_Stan.cellstart[c] + fill[c]++] = i;
					}
				}
			}
		}

		if (pass == 1) {
			free(fill);
		}
	}
}

/**
 * Which tile each of the conversion's walls came from.
 *
 * The tiles file holds a room's geometry as the conversion wrote it: for each
 * of the room's tiles in the graph's own order, the floor made from it and then
 * a wall for each of its edges marked -1 or as a link that climbs. Walking the
 * two together names every wall's tile; a floor that is not the tile's own
 * shape means the two files are not one conversion's, and the graph is not
 * used.
 */
static bool stanMatchWalls(void)
{
	s32 numwalls = 0;
	s32 at = 0;

	for (s32 i = 0; i < g_Stan.numtiles; i++) {
		for (s32 k = 0; k < g_Stan.tiles[i].npts; k++) {
			const struct stanpoint *p = &g_Stan.points[g_Stan.tiles[i].first + k];

			numwalls += p->across == GESTAN_UNLINKED || p->climbwall;
		}
	}

	g_Stan.walls = malloc(sizeof(*g_Stan.walls) * (size_t)(numwalls ? numwalls : 1));

	if (!g_Stan.walls) {
		return false;
	}

	for (s32 room = 0; room < g_TileNumRooms; room++) {
		const struct geo *geo = (const struct geo *)(g_TileFileData.u8 + g_TileRooms[room]);
		const struct geo *end = (const struct geo *)(g_TileFileData.u8 + g_TileRooms[room + 1]);

		for (s32 i = 0; i < g_Stan.numtiles; i++) {
			const struct stantile *t = &g_Stan.tiles[i];
			const struct geotilei *floor = (const struct geotilei *)geo;

			if (t->room != room) {
				continue;
			}

			if (geo >= end || geo->type != GEOTYPE_TILE_I || floor->header.numvertices != t->npts
					|| floor->vertices[0][0] != g_Stan.points[t->first].x
					|| floor->vertices[0][2] != g_Stan.points[t->first].z) {
				sysLogPrintf(LOG_WARNING, "gestan: room %d's geometry is not tile %d's: the graph is not this conversion's", room, i);
				return false;
			}

			geo = (const struct geo *)((uintptr_t)geo + floor->header.numvertices * 6 + 0xe);

			for (s32 k = 0; k < t->npts; k++) {
				const struct geotilei *wall = (const struct geotilei *)geo;

				if (g_Stan.points[t->first + k].across != GESTAN_UNLINKED && !g_Stan.points[t->first + k].climbwall) {
					continue;
				}

				if (geo >= end || geo->type != GEOTYPE_TILE_I || at >= numwalls
						|| wall->vertices[0][0] != g_Stan.points[t->first + k].x
						|| wall->vertices[0][2] != g_Stan.points[t->first + k].z) {
					sysLogPrintf(LOG_WARNING, "gestan: room %d has no wall for tile %d's edge %d: the graph is not this conversion's", room, i, k);
					return false;
				}

				g_Stan.walls[at].geo = geo;
				g_Stan.walls[at].tile = i;
				at++;
				geo = (const struct geo *)((uintptr_t)geo + wall->header.numvertices * 6 + 0xe);
			}
		}
	}

	g_Stan.numwalls = at;

	// by address, which is the order they were met in unless the rooms' lists
	// are not in the order of their numbers
	for (s32 i = 1; i < at; i++) {
		if (g_Stan.walls[i].geo < g_Stan.walls[i - 1].geo) {
			sysLogPrintf(LOG_WARNING, "gestan: the rooms' geometry is not in the order of the rooms");
			return false;
		}
	}

	return true;
}

static void stanBuild(void)
{
	u32 len = 0;
	u8 *d;
	s32 numtiles;
	s32 numpoints = 0;
	u32 o;

	stanFree();
	g_Stan.stagenum = g_Vars.stagenum;
	g_Stan.tiledata = g_TileFileData.u8;

	if (!g_TileFileData.u8 || !modloaderStageIsRemake(g_Vars.stagenum)) {
		return;
	}

	d = stanLoadFile(&len);

	if (!d || len < 8 || memcmp(d, "GST1", 4)) {
		sysLogPrintf(LOG_NOTE, "gestan: stage 0x%02x has no tile graph; its walls are all there for everybody", g_Vars.stagenum);
		sysMemFree(d);
		return;
	}

	numtiles = (s32)(((u32)d[4] << 24) | ((u32)d[5] << 16) | ((u32)d[6] << 8) | d[7]);

	for (o = 8; numtiles > 0 && o + 4 <= len; ) {
		const s32 n = d[o + 3];

		numpoints += n;
		o += 4 + 8 * (u32)n;
	}

	if (numtiles <= 0 || o > len) {
		sysLogPrintf(LOG_WARNING, "gestan: stage 0x%02x's tile graph is cut short", g_Vars.stagenum);
		sysMemFree(d);
		return;
	}

	g_Stan.tiles = calloc((size_t)numtiles, sizeof(*g_Stan.tiles));
	g_Stan.points = calloc((size_t)(numpoints ? numpoints : 1), sizeof(*g_Stan.points));
	g_Stan.reached = calloc((size_t)numtiles, sizeof(*g_Stan.reached));

	if (!g_Stan.tiles || !g_Stan.points || !g_Stan.reached) {
		sysMemFree(d);
		stanFree();
		return;
	}

	numpoints = 0;
	o = 8;

	for (s32 i = 0; i < numtiles && o + 4 <= len; i++) {
		struct stantile *t = &g_Stan.tiles[i];

		t->room = (s16)stanBe16(d + o);
		t->special = d[o + 2];
		t->npts = d[o + 3];
		t->first = numpoints;
		t->xmin = t->zmin = 32767;
		t->xmax = t->zmax = -32768;
		o += 4;

		for (s32 k = 0; k < t->npts; k++, o += 8) {
			struct stanpoint *p = &g_Stan.points[numpoints++];

			p->x = (s16)stanBe16(d + o);
			p->y = (s16)stanBe16(d + o + 2);
			p->z = (s16)stanBe16(d + o + 4);
			p->across = (s16)stanBe16(d + o + 6);
			p->climbwall = false;

			// a link that climbs (GESTAN_CLIMBWALL): the conversion raised a
			// wall on the low side of it, and the link is a link still
			if (p->across >= 0 && (p->across & GESTAN_CLIMBWALL)) {
				p->across &= ~GESTAN_CLIMBWALL;
				p->climbwall = true;
			}

			if (p->across >= numtiles) {
				p->across = GESTAN_NOWALL;
			}

			if (p->x < t->xmin) t->xmin = p->x;
			if (p->x > t->xmax) t->xmax = p->x;
			if (p->z < t->zmin) t->zmin = p->z;
			if (p->z > t->zmax) t->zmax = p->z;
		}
	}

	sysMemFree(d);
	g_Stan.numtiles = numtiles;

	if (!stanMatchWalls()) {
		stanFree();
		return;
	}

	stanBuildGrid();

	if (!g_Stan.cellstart) {
		stanFree();
		return;
	}

	g_Stan.active = true;
	sysLogPrintf(LOG_NOTE, "gestan: stage 0x%02x: %d tiles, %d walls, each wall its tile's alone", g_Vars.stagenum, numtiles, g_Stan.numwalls);
}

/** Whether x/z is inside the tile in plan, its edges included. */
static bool stanHolds(const struct stantile *t, f32 x, f32 z)
{
	const struct stanpoint *p = &g_Stan.points[t->first];
	bool inside = false;

	if (x < t->xmin || x > t->xmax || z < t->zmin || z > t->zmax) {
		return false;
	}

	for (s32 i = 0, j = t->npts - 1; i < t->npts; j = i++) {
		if (((p[i].z > z) != (p[j].z > z))
				&& (x < (f32)(p[j].x - p[i].x) * (z - p[i].z) / (f32)(p[j].z - p[i].z) + p[i].x)) {
			inside = !inside;
		}
	}

	return inside;
}

/** The tile's own surface at x/z, from the fan triangle that holds it. */
static f32 stanSurface(const struct stantile *t, f32 x, f32 z)
{
	const struct stanpoint *p = &g_Stan.points[t->first];
	f32 sum = 0.0f;

	for (s32 k = 1; k + 1 < t->npts; k++) {
		const struct stanpoint *a = &p[0], *b = &p[k], *c = &p[k + 1];
		const f32 det = (f32)(b->z - c->z) * (a->x - c->x) + (f32)(c->x - b->x) * (a->z - c->z);
		f32 w0, w1, w2;

		if (det == 0.0f) {
			continue;
		}

		w0 = ((f32)(b->z - c->z) * (x - c->x) + (f32)(c->x - b->x) * (z - c->z)) / det;
		w1 = ((f32)(c->z - a->z) * (x - c->x) + (f32)(a->x - c->x) * (z - c->z)) / det;
		w2 = 1.0f - w0 - w1;

		if (w0 >= -0.001f && w1 >= -0.001f && w2 >= -0.001f) {
			return w0 * a->y + w1 * b->y + w2 * c->y;
		}
	}

	for (s32 k = 0; k < t->npts; k++) {
		sum += p[k].y;
	}

	return sum / (t->npts ? t->npts : 1);
}

/**
 * The tile a body stands on: the highest under it whose surface is at or under
 * `limit` - or one no more than `rise` over the limit, where that is nearer the
 * limit than any under it.
 *
 * The rise is for a body whose foot is known and lags the floor. A player's
 * ground follows a staircase on a spring, and running up Dam's outside flight
 * with the stick held to one side it fell fifty under the tread they were on:
 * no tread was at or under the limit then, the tile taken was the ground a
 * storey below the flight, none of the flight's tiles was linked to that within
 * reach, and every wall of the stair was left out - through the rail on one
 * side and into the tower's wall on the other.
 */
/**
 * The tile under x/z whose surface is nearest `limit` (at or under it, or at
 * most `rise` over). Where two tiles hold the point equally - a point on the
 * edge between two rooms' floors - the one in room `prefer` wins, if either is.
 */
static s32 stanTileUnderPrefer(f32 x, f32 z, f32 limit, f32 rise, s32 prefer)
{
	const s32 cx = stanCellOf(x, g_Stan.gridx, g_Stan.gridw);
	const s32 cz = stanCellOf(z, g_Stan.gridz, g_Stan.gridh);
	const s32 c = cz * g_Stan.gridw + cx;
	s32 best = -1;
	f32 bestoff = 1e30f;

	for (s32 k = g_Stan.cellstart[c]; k < g_Stan.cellstart[c + 1]; k++) {
		const struct stantile *t = &g_Stan.tiles[g_Stan.celltiles[k]];

		if (stanHolds(t, x, z)) {
			const f32 y = stanSurface(t, x, z);
			const f32 off = y <= limit ? limit - y : y - limit;

			if ((y <= limit || y - limit <= rise)
					&& (off < bestoff - 1.0f
						|| (off < bestoff + 1.0f && (best < 0 || g_Stan.tiles[best].room != prefer) && t->room == prefer)
						|| (off < bestoff && (best < 0 || g_Stan.tiles[best].room != prefer || t->room == prefer)))) {
				bestoff = off;
				best = g_Stan.celltiles[k];
			}
		}
	}

	return best;
}

static s32 stanTileUnder(f32 x, f32 z, f32 limit, f32 rise)
{
	return stanTileUnderPrefer(x, z, limit, rise, -1);
}

/** How near x/z comes to the edge from a to b, in plan, squared. */
static f32 stanEdgeDistSq(const struct stanpoint *a, const struct stanpoint *b, f32 x, f32 z)
{
	const f32 ex = (f32)(b->x - a->x), ez = (f32)(b->z - a->z);
	const f32 len = ex * ex + ez * ez;
	f32 f = len > 0.0f ? ((x - a->x) * ex + (z - a->z) * ez) / len : 0.0f;
	f32 dx, dz;

	f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
	dx = x - (a->x + ex * f);
	dz = z - (a->z + ez * f);

	return dx * dx + dz * dz;
}

/** How near the move from x/z to x2/z2 comes to the edge from a to b, in plan, squared. */
static f32 stanEdgeSegDistSq(const struct stanpoint *a, const struct stanpoint *b, f32 x, f32 z, f32 x2, f32 z2)
{
	const f32 mx = x2 - x, mz = z2 - z;
	const f32 ex = (f32)(b->x - a->x), ez = (f32)(b->z - a->z);
	const f32 mlen = mx * mx + mz * mz;
	f32 best, d, f, dx, dz;

	if (mlen == 0.0f) {
		return stanEdgeDistSq(a, b, x, z);
	}

	// they cross: the two ends of each lie either side of the other
	if (((ex * (z - a->z) - ez * (x - a->x)) > 0.0f) != ((ex * (z2 - a->z) - ez * (x2 - a->x)) > 0.0f)
			&& ((mx * (a->z - z) - mz * (a->x - x)) > 0.0f) != ((mx * (b->z - z) - mz * (b->x - x)) > 0.0f)) {
		return 0.0f;
	}

	// or the nearest approach is at one of the four ends
	best = stanEdgeDistSq(a, b, x, z);
	d = stanEdgeDistSq(a, b, x2, z2);
	best = d < best ? d : best;

	f = ((a->x - x) * mx + (a->z - z) * mz) / mlen;
	f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
	dx = a->x - (x + mx * f); dz = a->z - (z + mz * f);
	d = dx * dx + dz * dz;
	best = d < best ? d : best;

	f = ((b->x - x) * mx + (b->z - z) * mz) / mlen;
	f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
	dx = b->x - (x + mx * f); dz = b->z - (z + mz * f);
	d = dx * dx + dz * dz;

	return d < best ? d : best;
}

/**
 * The tiles a body reaches: its own, and every tile linked to it - through any
 * number of links - across an edge that comes within `reach` of where it
 * stands. That is stanTestVolume()'s walk (sub_GAME_7F0B1DDC: a link is
 * followed where the body's circle touches the edge itself) and the walk a
 * move makes, and it is the whole of what GoldenEye ever consults.
 *
 * A move is asked about as a line (`x2`/`z2`, the same point for a body that
 * stands), and the edge is measured from the line: GoldenEye's line test
 * (walkTilesBetweenPoints()) goes through the tiles the line crosses and no
 * others. Measured from the line's start by the line's whole length it was
 * every tile in a circle that wide, and at the end of Facility that took in
 * the upright tile of the wall beside the bottling room's door, whose open
 * edge runs across the doorway in plan: the line a guard opens a door by
 * stopped at it a pace short of the door, and Ourumov's squad ran on the spot.
 *
 * It is the edge and not the neighbour's box: the ground under Dam's outside
 * stair is one triangle whose box holds the whole flight, and its wall runs
 * across under the treads. By its box it was reached from every tread, and a
 * body a little off the middle of the flight stopped at a wall a storey under
 * its feet.
 */
static void stanFlood(s32 start, f32 x, f32 z, f32 x2, f32 z2, f32 reach)
{
	s32 queue[GESTAN_MAXFLOOD];
	s32 head = 0, tail = 0;

	g_Stan.gen++;

	if (g_Stan.gen == 0) {
		memset(g_Stan.reached, 0, sizeof(*g_Stan.reached) * (size_t)g_Stan.numtiles);
		g_Stan.gen = 1;
	}

	g_Stan.reached[start] = g_Stan.gen;
	queue[tail++] = start;

	while (head < tail) {
		const struct stantile *t = &g_Stan.tiles[queue[head++]];
		const struct stanpoint *p = &g_Stan.points[t->first];

		for (s32 k = 0; k < t->npts; k++) {
			const s32 n = p[k].across;

			if (n < 0 || g_Stan.reached[n] == g_Stan.gen || tail >= GESTAN_MAXFLOOD) {
				continue;
			}

			if (stanEdgeSegDistSq(&p[k], &p[(k + 1) % t->npts], x, z, x2, z2) > reach * reach) {
				continue;
			}

			g_Stan.reached[n] = g_Stan.gen;
			queue[tail++] = n;
		}
	}
}

f32 geStanRise(bool checkvertical)
{
	// only where the limit is the body's own foot: under a middle or an eye
	// the limit is already most of a body over the floor, and a flight over a
	// head would be within any rise of it
	return checkvertical ? GESTAN_RISE : 0.0f;
}

f32 geStanLimit(struct coord *pos, bool checkvertical, f32 ymin)
{
	// A cylinder's foot is a step's height over the floor it stands on, and a
	// flight passing over a head is a hundred and more over it. Where the test
	// has no cylinder the position is the body's middle or its eye, both of
	// them more than sixty over their own floor and less than sixty under the
	// one above.
	return checkvertical ? pos->y + ymin + 10.0f : pos->y - 60.0f;
}

bool geStanWallSkipped(struct geo *geo, struct coord *pos, struct coord *to, f32 limit, f32 rise, f32 reach)
{
	s32 lo, hi;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active || g_Stan.numwalls == 0
			|| (const struct geo *)geo < g_Stan.walls[0].geo
			|| (const struct geo *)geo > g_Stan.walls[g_Stan.numwalls - 1].geo) {
		return false;
	}

	// one of the conversion's walls?
	lo = 0;
	hi = g_Stan.numwalls - 1;

	while (lo < hi) {
		const s32 mid = (lo + hi) / 2;

		if (g_Stan.walls[mid].geo < (const struct geo *)geo) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}

	if (g_Stan.walls[lo].geo != (const struct geo *)geo) {
		return false;
	}

	g_GeStanAsked++;

	// where the body stands, worked out once for all the walls of one test
	if (!to) {
		to = pos;
	}

	if (pos->x != g_Stan.lastx || pos->z != g_Stan.lastz || to->x != g_Stan.lastx2 || to->z != g_Stan.lastz2
			|| limit != g_Stan.lastlimit || rise != g_Stan.lastrise || reach != g_Stan.lastreach) {
		const s32 tile = stanTileUnder(pos->x, pos->z, limit, rise);

		g_Stan.lastx = pos->x;
		g_Stan.lastz = pos->z;
		g_Stan.lastx2 = to->x;
		g_Stan.lastz2 = to->z;
		g_Stan.lastlimit = limit;
		g_Stan.lastrise = rise;
		g_Stan.lastreach = reach;
		g_Stan.lastfound = tile >= 0;

		if (tile >= 0) {
			stanFlood(tile, pos->x, pos->z, to->x, to->z, reach);
		} else {
			g_GeStanNoTile++;
		}
	}

	// a body over no tile at all - thrown, falling, out of the level - is
	// asked of every wall, as it always was
	if (!g_Stan.lastfound) {
		return false;
	}

	if (g_Stan.reached[g_Stan.walls[lo].tile] == g_Stan.gen) {
		return false;
	}

	g_GeStanSkipped++;

	return true;
}

/** Whether the tile's area in plan is next to nothing: a tile on edge, a wall or a riser. */
static bool stanTileUpright(s32 i)
{
	const struct stantile *t = &g_Stan.tiles[i];
	const struct stanpoint *p = &g_Stan.points[t->first];
	f32 area = 0.0f;

	for (s32 a = 0, b = t->npts - 1; a < t->npts; b = a++) {
		area += (f32)p[b].x * p[a].z - (f32)p[a].x * p[b].z;
	}

	return area > -1.0f && area < 1.0f;
}

/**
 * Whether a floor's edge, from e0 to the point after it, is one a body climbs
 * across rather than walks over. Only a link is crossed at all (GoldenEye's
 * line walk stops at an unlinked edge), and a link into a floor lying flush
 * with it - the next triangle of the same deck or ramp, the matching edge at
 * the same heights - is level ground: the step it makes is the floor's own
 * slope, which Perfect Dark's feet follow. A link into a tile on edge is the
 * climb itself (Facility's conveyor rim), and so is one whose far side is at
 * another height.
 */
static bool stanEdgeClimbs(const struct stanpoint *e0, const struct stanpoint *e1)
{
	const s32 n = e0->across;
	const struct stantile *t;
	const struct stanpoint *p;

	if (n < 0) {
		return false;
	}

	if (stanTileUpright(n)) {
		return true;
	}

	t = &g_Stan.tiles[n];
	p = &g_Stan.points[t->first];

	for (s32 a = 0; a < t->npts; a++) {
		const struct stanpoint *q0 = &p[a], *q1 = &p[(a + 1) % t->npts];

		if (q0->x == e1->x && q0->z == e1->z && q1->x == e0->x && q1->z == e0->z) {
			return q0->y - e1->y > 2 || e1->y - q0->y > 2 || q1->y - e0->y > 2 || e0->y - q1->y > 2;
		}

		if (q0->x == e0->x && q0->z == e0->z && q1->x == e1->x && q1->z == e1->z) {
			return q0->y - e0->y > 2 || e0->y - q0->y > 2 || q1->y - e1->y > 2 || e1->y - q1->y > 2;
		}
	}

	// joined along part of an edge only: whatever it is, it is no seam
	return true;
}

/**
 * The floor GoldenEye would lift the player onto as he walks from `pos` to
 * `to`: the highest tile with an area in plan that his circle at `to` touches
 * and that is linked to the one under his foot through edges within his reach
 * - across tiles on edge, the way GoldenEye joins a floor to one well over it.
 * GESTAN_NOCLIMBFLOOR where there is none.
 */
f32 geStanClimbFloor(struct coord *pos, struct coord *to, f32 ground, f32 radius)
{
	f32 best = GESTAN_NOCLIMBFLOOR;
	f32 movelen;
	s32 tile;
	s32 cx0, cx1, cz0, cz1;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return best;
	}

	tile = stanTileUnder(pos->x, pos->z, ground + 40.0f, GESTAN_RISE);

	if (tile < 0) {
		return best;
	}

	stanFlood(tile, pos->x, pos->z, to->x, to->z, radius);

	movelen = sqrtf((to->x - pos->x) * (to->x - pos->x) + (to->z - pos->z) * (to->z - pos->z));
	cx0 = stanCellOf(to->x - radius, g_Stan.gridx, g_Stan.gridw);
	cx1 = stanCellOf(to->x + radius, g_Stan.gridx, g_Stan.gridw);
	cz0 = stanCellOf(to->z - radius, g_Stan.gridz, g_Stan.gridh);
	cz1 = stanCellOf(to->z + radius, g_Stan.gridz, g_Stan.gridh);

	for (s32 cz = cz0; cz <= cz1; cz++) {
		for (s32 cx = cx0; cx <= cx1; cx++) {
			const s32 c = cz * g_Stan.gridw + cx;

			for (s32 k = g_Stan.cellstart[c]; k < g_Stan.cellstart[c + 1]; k++) {
				const s32 i = g_Stan.celltiles[k];
				const struct stantile *t = &g_Stan.tiles[i];
				const struct stanpoint *p = &g_Stan.points[t->first];
				f32 y = GESTAN_NOCLIMBFLOOR;

				if (g_Stan.reached[i] != g_Stan.gen) {
					continue;
				}

				// a tile on edge is the climb, not a floor
				if (stanTileUpright(i)) {
					continue;
				}

				if (stanHolds(t, to->x, to->z)) {
					y = stanSurface(t, to->x, to->z);
				} else if (!stanHolds(t, pos->x, pos->z)) {
					// touched from outside - and only by a move into it, more
					// than 30 degrees off its edge: GoldenEye lifts Bond once
					// his middle is over the tile, and a body brushing along
					// a ledge's edge never is
					f32 nearto = 1e30f, nearfrom = 1e30f;
					f32 ey = GESTAN_NOCLIMBFLOOR;

					for (s32 a = 0; a < t->npts; a++) {
						const struct stanpoint *e0 = &p[a], *e1 = &p[(a + 1) % t->npts];
						const f32 dto = stanEdgeDistSq(e0, e1, to->x, to->z);
						const f32 dfrom = stanEdgeDistSq(e0, e1, pos->x, pos->z);

						nearfrom = dfrom < nearfrom ? dfrom : nearfrom;

						if (dto <= radius * radius && stanEdgeClimbs(e0, e1)) {
							// the floor's height where the circle meets
							// the edge, not the edge's higher end: Cradle's
							// walkways are ramps split into triangles, and
							// touching the next triangle's diagonal gave
							// the top of the ramp, 70 over the player -
							// lifted there, he fell back (F3 report
							// 20260926-082928)
							const f32 ex = (f32)(e1->x - e0->x), ez = (f32)(e1->z - e0->z);
							const f32 len = ex * ex + ez * ez;
							f32 f = len > 0.0f ? ((to->x - e0->x) * ex + (to->z - e0->z) * ez) / len : 0.0f;
							f32 y;

							f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
							y = e0->y + (e1->y - e0->y) * f;
							nearto = dto < nearto ? dto : nearto;

							if (y > ey) ey = y;
						}
					}

					if (sqrtf(nearfrom) - sqrtf(nearto) >= 0.5f * movelen && movelen > 0.0f) {
						y = ey;
					}
				}

				if (y > best) {
					best = y;
				}
			}
		}
	}

	// the marks are this flood's now, not the one a wall test remembers
	g_Stan.lastreach = -1.0f;

	return best;
}

/**
 * Whether GoldenEye would hold a body down here: a tile's special value 1 is
 * g_StanTileSpecialFlags[]'s STANTILEFLAG_FORCECROUCH - Facility's vents, the
 * crawl spaces of seven levels more - and bondview's move sets autocrouchpos
 * to the squat when the tile Bond is on, or one linked to it across an edge
 * his circle touches (stanTileDistanceRelated()), carries it. It is asked from
 * the edge and not from inside, so he is down before his head is under the
 * duct.
 */
bool geStanForcesCrouch(struct coord *pos, f32 limit, f32 rise, f32 reach)
{
	s32 tile;
	bool result = false;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return false;
	}

	tile = stanTileUnder(pos->x, pos->z, limit, rise);

	if (tile < 0) {
		return false;
	}

	stanFlood(tile, pos->x, pos->z, pos->x, pos->z, reach);

	for (s32 i = 0; i < g_Stan.numtiles; i++) {
		if (g_Stan.reached[i] == g_Stan.gen && g_Stan.tiles[i].special == GESTAN_SPECIAL_CROUCH) {
			result = true;
			break;
		}
	}

	// the marks are this flood's now, not the one a wall test remembers
	g_Stan.lastreach = -1.0f;

	return result;
}

/** stan.c's getRotationalDirectionBetween(): which way b lies from a, in plan. */
static s32 stanTurn(f32 ax, f32 az, f32 bx, f32 bz)
{
	if (az * bx < ax * bz) {
		return 1;
	}

	if (ax * bz < az * bx) {
		return -1;
	}

	if (ax * bx < 0 || az * bz < 0) {
		return -1;
	}

	return ax * ax + az * az < bx * bx + bz * bz ? 1 : 0;
}

/** stan.c's sub_GAME_7F0B07BC(): whether the line from 0 to 1 crosses the edge from a to b. */
static bool stanCrosses(f32 x0, f32 z0, f32 x1, f32 z1, f32 ax, f32 az, f32 bx, f32 bz, s32 linked)
{
	const s32 v1 = stanTurn(x1 - x0, z1 - z0, ax - x0, az - z0) * stanTurn(x1 - x0, z1 - z0, bx - x0, bz - z0);
	const s32 v2 = stanTurn(bx - ax, bz - az, x0 - ax, z0 - az) * stanTurn(bx - ax, bz - az, x1 - ax, z1 - az);

	return v1 < linked && v2 < linked;
}

/**
 * sub_GAME_7F0B0914(): from `tile`, through every linked edge the line leaves a
 * tile by, to the tile that holds its end or to the last one before an edge
 * with nothing across it - a camera out over a drop stays on the brink's tile.
 * With `noclimb`, a link the conversion raised a climb wall on stops it too.
 */
static s32 stanWalkLine(s32 tile, f32 x0, f32 z0, f32 x1, f32 z1, bool noclimb)
{
	s32 prev, prevprev, next = -1;
	const f32 negdz = -(z1 - z0);
	const f32 dx = x1 - x0;

	prev = prevprev = tile;

	for (s32 i = 0; i < 0x1f5; i++) {
		const struct stantile *t = &g_Stan.tiles[tile];
		const struct stanpoint *p = &g_Stan.points[t->first];
		s32 crossings = 0;

		for (s32 k = 0; k < t->npts; k++) {
			const struct stanpoint *a = &p[k], *b = &p[(k + 1) % t->npts];
			const s32 across = noclimb && a->climbwall ? GESTAN_UNLINKED : a->across;

			if (negdz * (b->x - a->x) + dx * (b->z - a->z) <= 0.0f
					&& stanCrosses(x0, z0, x1, z1, a->x, a->z, b->x, b->z, across >= 0)) {
				crossings++;

				if (across < 0 || (across != prev && across != prevprev)) {
					next = across >= 0 ? across : -1;
				}
			}
		}

		prevprev = prev;
		prev = tile;

		if (crossings == 0 || next == tile || next < 0) {
			break;
		}

		tile = next;
	}

	return tile;
}

bool geStanWalkFromRoom(struct coord *from, s32 fromroom, struct coord *to, s32 *room, f32 *ground)
{
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return false;
	}

	// GoldenEye starts from the pad's own tile, which the conversion does not
	// carry: the one under the pad, a pad standing on its floor or a little over
	tile = stanTileUnderPrefer(from->x, from->z, from->y + 5.0f, GESTAN_RISE, fromroom);

	if (tile < 0) {
		return false;
	}

	tile = stanWalkLine(tile, from->x, from->z, to->x, to->z, false);

	*room = g_Stan.tiles[tile].room;
	*ground = stanSurface(&g_Stan.tiles[tile], to->x, to->z);

	return true;
}

bool geStanWalk(struct coord *from, struct coord *to, s32 *room, f32 *ground)
{
	return geStanWalkFromRoom(from, -1, to, room, ground);
}

/**
 * Whether a guard may run straight from `from` (standing on the floor at
 * `ground`) to `to`, past the waypoints between: GoldenEye's test before it
 * skips one (chraction.c's sub_GAME_7F030128()) walks the tile graph from the
 * guard's own tile along the line, and passes only where the walk ends on the
 * tile of the pad or position being run to.
 *
 * Perfect Dark's test (func0f03654c()) is a cylinder swept in plan against the
 * walls, and a converted level has no wall where a floor ends over a drop: from
 * the floor of Facility's room 49 the pad on the landing three metres over it
 * was "in sight", and from the landing the player standing under it was. The
 * guard ran to the spot under or over what it was running to and stayed there,
 * since it never came within the 150 of it in height that counts as arriving.
 *
 * A walk that ends on a tile holding `to` at the same height is the same floor
 * (a point on the seam between two tiles). A link the conversion raised a
 * climb wall on ends the walk: GoldenEye lifts a guard up it and Perfect Dark
 * does not. True where there is no graph, or either end is over no tile.
 */
bool geStanReaches(struct coord *from, f32 ground, struct coord *to)
{
	s32 fromtile;
	s32 totile;
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return true;
	}

	// the guard's floor, where its foot is; and the floor under the pad or
	// body it is running to, which stand anything up to two metres over theirs
	fromtile = stanTileUnder(from->x, from->z, ground + 10.0f, GESTAN_RISE);
	totile = stanTileUnder(to->x, to->z, to->y + 5.0f, 0.0f);

	if (fromtile < 0 || totile < 0) {
		return true;
	}

	tile = stanWalkLine(fromtile, from->x, from->z, to->x, to->z, true);

	if (tile == totile) {
		return true;
	}

	return stanHolds(&g_Stan.tiles[tile], to->x, to->z)
		&& fabsf(stanSurface(&g_Stan.tiles[tile], to->x, to->z) - stanSurface(&g_Stan.tiles[totile], to->x, to->z)) < 1.0f;
}

/**
 * Whether a vehicle can drive `n` lines laid end to end in plan, GoldenEye's
 * walkTilesBetweenPoints_NoCallback() chained through its truck's outline:
 * each line starts on the tile the last one ended on, and a line that meets
 * an edge with nothing across it ends short of its end - a wall. `y` is the
 * height the first tile is looked for under. True when there is no tile graph.
 */
bool geStanLinesClear(const f32 (*pts)[2], s32 n, f32 y)
{
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return true;
	}

	tile = stanTileUnder(pts[0][0], pts[0][1], y, GESTAN_RISE);

	if (tile < 0) {
		return true;
	}

	for (s32 i = 0; i + 1 < n; i++) {
		tile = stanWalkLine(tile, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], false);

		if (!stanHolds(&g_Stan.tiles[tile], pts[i + 1][0], pts[i + 1][1])) {
			return false;
		}
	}

	return true;
}

/**
 * GoldenEye's death camera's line (stanTestLineUnobstructed() and, where it
 * stops, chrlvStanPointPointIntersection()): the walk from the tile under
 * `from` (standing at or under from->y) along the line in plan to x1/z1.
 * 1 when it ends on a tile holding the end, with that tile's room and its
 * surface there; 0 when an edge stops it, with where the line leaves the last
 * tile it reached in `hitx`/`hitz`; -1 where the level has no graph or `from`
 * is over no tile.
 */
s32 geStanLineReach(struct coord *from, f32 x1, f32 z1, f32 *hitx, f32 *hitz, s32 *room, f32 *ground)
{
	const struct stantile *t;
	const struct stanpoint *p;
	f32 best = 0.0f;
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return -1;
	}

	tile = stanTileUnder(from->x, from->z, from->y, GESTAN_RISE);

	if (tile < 0) {
		return -1;
	}

	tile = stanWalkLine(tile, from->x, from->z, x1, z1, false);
	t = &g_Stan.tiles[tile];

	if (stanHolds(t, x1, z1)) {
		*room = t->room;
		*ground = stanSurface(t, x1, z1);
		*hitx = x1;
		*hitz = z1;
		return 1;
	}

	// the furthest along the line that the last tile's edges cross it
	p = &g_Stan.points[t->first];

	for (s32 k = 0; k < t->npts; k++) {
		const struct stanpoint *a = &p[k], *b = &p[(k + 1) % t->npts];
		const f32 dx = x1 - from->x, dz = z1 - from->z;
		const f32 ex = (f32)(b->x - a->x), ez = (f32)(b->z - a->z);
		const f32 den = dx * ez - dz * ex;
		f32 u, v;

		if (den > -1e-6f && den < 1e-6f) {
			continue;
		}

		u = ((a->x - from->x) * ez - (a->z - from->z) * ex) / den;
		v = ((a->x - from->x) * dz - (a->z - from->z) * dx) / den;

		if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f && u > best) {
			best = u;
		}
	}

	*hitx = from->x + (x1 - from->x) * best;
	*hitz = from->z + (z1 - from->z) * best;
	*room = t->room;
	*ground = stanSurface(t, *hitx, *hitz);

	return 0;
}

/**
 * Whether the floor between `a` and `b` stays under the line joining them: the
 * tile graph walked from the tile under `a` in steps of GESTAN_SIGHTSTEP, each
 * step's surface held against the line's height there. A converted level's
 * rolling ground - Jungle's mounds - is floor to the tile graph and nothing at
 * all to Perfect Dark's own line of sight tests. Where the walk meets an edge
 * with nothing across it the floor beyond is not the graph's to say, and the
 * line is taken as clear from there. True with no graph.
 */
#define GESTAN_SIGHTSTEP 16.0f

bool geStanSightClear(struct coord *a, struct coord *b)
{
	const f32 dx = b->x - a->x, dy = b->y - a->y, dz = b->z - a->z;
	const f32 len = sqrtf(dx * dx + dz * dz);
	f32 px = a->x, pz = a->z;
	s32 steps;
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return true;
	}

	tile = stanTileUnder(a->x, a->z, a->y, 0.0f);

	if (tile < 0) {
		return true;
	}

	steps = (s32)(len / GESTAN_SIGHTSTEP);

	for (s32 i = 1; i < steps; i++) {
		const f32 t = (f32)i / (f32)steps;
		const f32 x = a->x + dx * t, z = a->z + dz * t;

		tile = stanWalkLine(tile, px, pz, x, z, false);

		if (!stanHolds(&g_Stan.tiles[tile], x, z)) {
			return true;
		}

		if (stanSurface(&g_Stan.tiles[tile], x, z) > a->y + dy * t) {
			return false;
		}

		px = x;
		pz = z;
	}

	return true;
}

#endif
