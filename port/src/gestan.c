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
#include "geroom.h"
#include "romdata.h"
#include "fs.h"
#include "system.h"
#include "gestan.h"
#include "sha256.h"
#include "game/prop.h"

#ifndef PLATFORM_N64

#define GESTAN_CELL      512.0f
#define GESTAN_MAXFLOOD  512
#define GESTAN_MAXCLIMB  32      // floors one climb test weighs
#define GESTAN_NOWALL    (-2)
#define GESTAN_UNLINKED  (-1)
#define GESTAN_CLIMBWALL 0x4000
#define GESTAN_SPECIAL_CROUCH 1
#define GESTAN_SPECIAL_LADDER 3
#define GESTAN_RISE      60.0f   // how far over a body's foot its own floor may be: two of a stair's steps

struct stanpoint {
	// 32 bits: a GoldenEye ROM hack's level can be wider than 16 hold
	// (Goldfinger's Alpine Highway runs 233,000 units across, GST2)
	s32 x, y, z;
	s16 across;      // the tile across the edge to the next point, -1 a wall, -2 nothing
	u8 climbwall;    // linked, and a wall raised on it all the same: the link climbs more than a step
};

struct stantile {
	s16 room;
	u8 special;
	u8 npts;
	s32 first;       // its first point in `points`
	s32 xmin, xmax, zmin, zmax;
};

struct stanwall {
	const struct geo *geo;
	s32 tile;
	s32 point;       // the point its edge starts at, in `points`
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

static u32 stanBe32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
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
	// the graph beside the tiles the stage loaded (tilesReset()): the
	// Community Edition's in the HD look where it mends one
	const s32 tilefileid = geRoomCeData()
		? modloaderGetStageCeFile(g_Vars.stagenum, g_Stages[g_StageIndex].tilefileid) : g_Stages[g_StageIndex].tilefileid;
	const char *tilesname = romdataFileGetName(tilefileid);
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

/**
 * The SHA-256 of the stage's tile graph file as it is on disk, for netplay's
 * stage hash (the graph is read lazily, outside fileLoad(), so the hash
 * window never sees it: spec-stage.md §5 HD). 0 if the stage has none.
 */
s32 geStanFileHash(u8 *out)
{
	struct sha256ctx ctx;
	u32 len = 0;
	u8 *d = stanLoadFile(&len);

	if (!d) {
		return 0;
	}

	sha256Begin(&ctx);
	sha256Add(&ctx, d, len);
	sha256End(&ctx, out);
	sysMemFree(d);

	return 1;
}

static s32 stanCellOf(f32 v, f32 origin, s32 count)
{
	s32 c = (s32)floorf((v - origin) / GESTAN_CELL);

	return c < 0 ? 0 : (c >= count ? count - 1 : c);
}

static void stanBuildGrid(void)
{
	s32 xmin = 0x7fffffff, xmax = -0x7fffffff, zmin = 0x7fffffff, zmax = -0x7fffffff;
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
/**
 * A tile of the converted file: 16-bit vertices (GEOTYPE_TILE_I), or floats
 * (GEOTYPE_TILE_F) where the level is too wide for 16 (a ROM hack's, GST2).
 * Whether it has `npts` vertices (-1: any) and its first is the point's x
 * and z, and the one after it.
 */
static bool stanGeoStartsAt(const struct geo *geo, s32 npts, const struct stanpoint *p)
{
	if (npts >= 0 && geo->numvertices != npts) {
		return false;
	}

	if (geo->type == GEOTYPE_TILE_I) {
		const struct geotilei *tile = (const struct geotilei *)geo;

		return tile->vertices[0][0] == p->x && tile->vertices[0][2] == p->z;
	}

	if (geo->type == GEOTYPE_TILE_F) {
		const struct geotilef *tile = (const struct geotilef *)geo;

		return (s32)floorf(tile->vertices[0].x + 0.5f) == p->x && (s32)floorf(tile->vertices[0].z + 0.5f) == p->z;
	}

	return false;
}

static const struct geo *stanGeoNext(const struct geo *geo)
{
	if (geo->type == GEOTYPE_TILE_F) {
		return (const struct geo *)((uintptr_t)geo + geo->numvertices * 12 + 0x10);
	}

	return (const struct geo *)((uintptr_t)geo + geo->numvertices * 6 + 0xe);
}

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

			if (t->room != room) {
				continue;
			}

			if (geo >= end || !stanGeoStartsAt(geo, t->npts, &g_Stan.points[t->first])) {
				sysLogPrintf(LOG_WARNING, "gestan: room %d's geometry is not tile %d's: the graph is not this conversion's", room, i);
				return false;
			}

			geo = stanGeoNext(geo);

			for (s32 k = 0; k < t->npts; k++) {
				if (g_Stan.points[t->first + k].across != GESTAN_UNLINKED && !g_Stan.points[t->first + k].climbwall) {
					continue;
				}

				if (geo >= end || at >= numwalls || !stanGeoStartsAt(geo, -1, &g_Stan.points[t->first + k])) {
					sysLogPrintf(LOG_WARNING, "gestan: room %d has no wall for tile %d's edge %d: the graph is not this conversion's", room, i, k);
					return false;
				}

				g_Stan.walls[at].geo = geo;
				g_Stan.walls[at].tile = i;
				g_Stan.walls[at].point = t->first + k;
				at++;
				geo = stanGeoNext(geo);
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
	bool wide;
	u32 ptlen;

	stanFree();
	g_Stan.stagenum = g_Vars.stagenum;
	g_Stan.tiledata = g_TileFileData.u8;

	if (!g_TileFileData.u8 || !modloaderStageIsRemake(g_Vars.stagenum)) {
		return;
	}

	d = stanLoadFile(&len);

	if (!d || len < 8 || (memcmp(d, "GST1", 4) && memcmp(d, "GST2", 4))) {
		sysLogPrintf(LOG_NOTE, "gestan: stage 0x%02x has no tile graph; its walls are all there for everybody", g_Vars.stagenum);
		sysMemFree(d);
		return;
	}

	numtiles = (s32)(((u32)d[4] << 24) | ((u32)d[5] << 16) | ((u32)d[6] << 8) | d[7]);
	// GST2's points are 32 bits a coordinate (a level too wide for 16) and a
	// 16-bit link, padded to 16 bytes; GST1's 16 bits a coordinate
	wide = memcmp(d, "GST2", 4) == 0;
	ptlen = wide ? 16 : 8;

	for (o = 8; numtiles > 0 && o + 4 <= len; ) {
		const s32 n = d[o + 3];

		numpoints += n;
		o += 4 + ptlen * (u32)n;
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
		t->xmin = t->zmin = 0x7fffffff;
		t->xmax = t->zmax = -0x7fffffff;
		o += 4;

		for (s32 k = 0; k < t->npts; k++, o += ptlen) {
			struct stanpoint *p = &g_Stan.points[numpoints++];

			if (wide) {
				p->x = (s32)stanBe32(d + o);
				p->y = (s32)stanBe32(d + o + 4);
				p->z = (s32)stanBe32(d + o + 8);
				p->across = (s16)stanBe16(d + o + 12);
			} else {
				p->x = (s16)stanBe16(d + o);
				p->y = (s16)stanBe16(d + o + 2);
				p->z = (s16)stanBe16(d + o + 4);
				p->across = (s16)stanBe16(d + o + 6);
			}
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
 * Whether the tile is part of a ladder: a ladder tile (special 3, the floor
 * made from it carries Perfect Dark's ladder flag), or a tile on edge beside
 * one - the panel at a ladder's side, which the conversion treats as the
 * ladder too (stanClimb() in geconvert.c).
 */
static bool stanTileLadder(s32 i)
{
	const struct stantile *t = &g_Stan.tiles[i];
	const struct stanpoint *p = &g_Stan.points[t->first];

	if (t->special == GESTAN_SPECIAL_LADDER) {
		return true;
	}

	if (!stanTileUpright(i)) {
		return false;
	}

	for (s32 a = 0; a < t->npts; a++) {
		if (p[a].across >= 0 && g_Stan.tiles[p[a].across].special == GESTAN_SPECIAL_LADDER) {
			return true;
		}
	}

	return false;
}

static bool stanListHas(const s32 *list, s32 n, s32 tile)
{
	for (s32 k = 0; k < n; k++) {
		if (list[k] == tile) {
			return true;
		}
	}

	return false;
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
 *
 * `ladders` false stops the flood at a ladder (stanTileLadder()): the climb
 * asks that way, a ladder being climbed as Perfect Dark's own are. `noclimb`
 * stops it at a link the conversion raised a wall on (stanClimb()), which
 * leaves the tiles a body walks to on Perfect Dark's own feet.
 */
static s32 stanFloodList(s32 start, f32 x, f32 z, f32 x2, f32 z2, f32 reach, bool ladders, bool noclimb, s32 *queue)
{
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

			if (!ladders && stanTileLadder(n)) {
				continue;
			}

			if (noclimb && p[k].climbwall) {
				continue;
			}

			g_Stan.reached[n] = g_Stan.gen;
			queue[tail++] = n;
		}
	}

	return tail;
}

static void stanFlood(s32 start, f32 x, f32 z, f32 x2, f32 z2, f32 reach, bool ladders)
{
	s32 queue[GESTAN_MAXFLOOD];

	stanFloodList(start, x, z, x2, z2, reach, ladders, false, queue);
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

static s32 stanMoverTile(f32 x, f32 z, f32 limit, f32 rise);

// The player whose own move the walls are being asked about (geStanSetMover()),
// or -1: a cylinder test for anyone else finds its tile by height alone
static s32 g_StanMover = -1;

/**
 * Which of the conversion's walls `geo` is, with the tiles a body at `pos`
 * reaches worked out (stanFlood()), or -1 for a wall that is not one of them.
 */
static s32 stanWallFind(struct geo *geo, struct coord *pos, struct coord *to, f32 limit, f32 rise, f32 reach)
{
	s32 lo, hi;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active || g_Stan.numwalls == 0
			|| (const struct geo *)geo < g_Stan.walls[0].geo
			|| (const struct geo *)geo > g_Stan.walls[g_Stan.numwalls - 1].geo) {
		return -1;
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
		return -1;
	}

	// where the body stands, worked out once for all the walls of one test
	if (!to) {
		to = pos;
	}

	if (pos->x != g_Stan.lastx || pos->z != g_Stan.lastz || to->x != g_Stan.lastx2 || to->z != g_Stan.lastz2
			|| limit != g_Stan.lastlimit || rise != g_Stan.lastrise || reach != g_Stan.lastreach) {
		s32 tile = g_StanMover >= 0 ? stanMoverTile(pos->x, pos->z, limit, rise) : -1;

		if (tile < 0) {
			tile = stanTileUnder(pos->x, pos->z, limit, rise);
		}

		g_Stan.lastx = pos->x;
		g_Stan.lastz = pos->z;
		g_Stan.lastx2 = to->x;
		g_Stan.lastz2 = to->z;
		g_Stan.lastlimit = limit;
		g_Stan.lastrise = rise;
		g_Stan.lastreach = reach;
		g_Stan.lastfound = tile >= 0;

		if (tile >= 0) {
			stanFlood(tile, pos->x, pos->z, to->x, to->z, reach, true);
		} else {
			g_GeStanNoTile++;
		}
	}

	return lo;
}

bool geStanWallSkipped(struct geo *geo, struct coord *pos, struct coord *to, f32 limit, f32 rise, f32 reach)
{
	const s32 w = stanWallFind(geo, pos, to, limit, rise, reach);

	if (w < 0) {
		return false;
	}

	g_GeStanAsked++;


	// a body over no tile at all - thrown, falling, out of the level - is
	// asked of every wall, as it always was
	if (!g_Stan.lastfound) {
		return false;
	}

	if (g_Stan.reached[g_Stan.walls[w].tile] == g_Stan.gen) {
		return false;
	}

	g_GeStanSkipped++;

	return true;
}

bool geStanWallOverhead(struct geo *geo, struct coord *pos, f32 limit, f32 rise, f32 reach)
{
	const s32 w = stanWallFind(geo, pos, NULL, limit, rise, reach);
	s32 tile;

	if (w < 0 || !g_Stan.lastfound) {
		return false;
	}

	tile = g_Stan.walls[w].tile;

	return g_Stan.reached[tile] == g_Stan.gen
		&& g_Stan.points[g_Stan.walls[w].point].across == GESTAN_UNLINKED
		&& !stanTileUpright(tile) && !stanTileLadder(tile);
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

	// a ladder's head: the ladder is the way up there, not a lift
	if (stanTileLadder(n)) {
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
 * Whether a body's circle at x/z is clear of every unlinked edge of the tiles
 * it reaches from `tile` - stanTestVolume(), which bondviewTryMoveToStan()
 * asks of every move before it puts Bond on the tile he is moving to. It is
 * what keeps him out of a window he is wider than: Dam's mini-bunker has
 * windows 51 across in plan, each linked through tiles on edge to the sill 154
 * over the floor inside, and a body 60 across touches the jambs on either side
 * of one before its middle is over the sill.
 */
static bool stanCircleClear(s32 tile, f32 x, f32 z, f32 radius)
{
	s32 queue[GESTAN_MAXFLOOD];
	const s32 n = stanFloodList(tile, x, z, x, z, radius, true, false, queue);

	for (s32 q = 0; q < n; q++) {
		const struct stantile *t = &g_Stan.tiles[queue[q]];
		const struct stanpoint *p = &g_Stan.points[t->first];

		for (s32 k = 0; k < t->npts; k++) {
			if (p[k].across < 0 && stanEdgeDistSq(&p[k], &p[(k + 1) % t->npts], x, z) < radius * radius) {
				return false;
			}
		}
	}

	return true;
}

/**
 * The floor GoldenEye would lift the player onto as he walks from `pos` to
 * `to`: the highest tile with an area in plan that his circle at `to` touches
 * and that is linked to the one under his foot through edges within his reach
 * - across tiles on edge, the way GoldenEye joins a floor to one well over it -
 * and that is reached only across a link the conversion raised a wall on.
 * GESTAN_NOCLIMBFLOOR where there is none.
 */
f32 geStanClimbFloor(struct coord *pos, struct coord *to, f32 ground, f32 radius)
{
	s32 walked[GESTAN_MAXFLOOD];
	s32 nwalked;
	f32 best = GESTAN_NOCLIMBFLOOR;
	f32 movelen;
	s32 tile;
	s32 cx0, cx1, cz0, cz1;
	f32 cand[GESTAN_MAXCLIMB][3];
	s32 ncand = 0;

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

	// not up a ladder: a ladder is climbed as Perfect Dark climbs its own
	// (cdFindLadder()), and the deck at its head is linked to the floor at
	// its foot through it. Flooded across, the deck was a floor the circle
	// touched as the player walked into the ladder, and he was lifted the
	// ladder's whole height in one frame whenever that came before the
	// ladder took hold of him (F3 report 20260926-113743, Dam's tower
	// ladders, 321 high)
	//
	// And only a floor behind one of the conversion's climb walls: anything
	// else the player walks onto on Perfect Dark's own feet, which ease him
	// up the step as GoldenEye's do (bwalkUpdateVertical()). A stair was a
	// climb as well - its risers are tiles on edge - and the circle a move
	// goes to reaches two treads on: the player was put on the second, 47 up,
	// fell back onto the first and was put up again, every other step of
	// every flight (F3 reports 20260926-204407 Surface, 213221 Dam)
	nwalked = stanFloodList(tile, pos->x, pos->z, to->x, to->z, radius, false, true, walked);
	stanFlood(tile, pos->x, pos->z, to->x, to->z, radius, false);

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
				f32 atx = to->x, atz = to->z;

				if (g_Stan.reached[i] != g_Stan.gen) {
					continue;
				}

				// a tile on edge is the climb, not a floor
				if (stanTileUpright(i)) {
					continue;
				}

				if (stanListHas(walked, nwalked, i)) {
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
					f32 ex0 = to->x, ez0 = to->z;

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

							if (dto < nearto) {
								nearto = dto;
								ex0 = e0->x + ex * f;
								ez0 = e0->z + ez * f;
							}

							if (y > ey) ey = y;
						}
					}

					if (sqrtf(nearfrom) - sqrtf(nearto) >= 0.5f * movelen && movelen > 0.0f) {
						y = ey;
						atx = ex0;
						atz = ez0;
					}
				}

				// (only a floor over the step Perfect Dark's feet take)
				if (y > ground + 30.0f && ncand < GESTAN_MAXCLIMB) {
					cand[ncand][0] = y;
					cand[ncand][1] = atx;
					cand[ncand][2] = atz;
					ncand++;
				}
			}
		}
	}

	// and only a floor he fits onto. GoldenEye lifts Bond once his middle is
	// over the tile, which it has to walk him to first, and it makes no move
	// whose circle touches an unlinked edge (stanTestVolume() in
	// bondviewTryMoveToStan()). So the circle must be clear where he is going
	// and, for a floor touched from outside, with its middle on the edge he
	// climbs across. Dam's mini-bunker has windows 51 across in plan, linked
	// through tiles on edge to the sill 154 over the floor inside: a body 60
	// across meets the jambs before its middle is at the window, and
	// GoldenEye stops Bond there. Lifted as his circle reached the sill, the
	// player was put on it with his eye over the roof, the jambs stopped him,
	// he fell back and was put up again (F3 report 20260929-015652). Asked
	// once the loop is done: the test floods the graph afresh
	if (ncand > 0 && stanCircleClear(tile, to->x, to->z, radius)) {
		for (s32 c = 0; c < ncand; c++) {
			if (cand[c][0] > best && stanCircleClear(tile, cand[c][1], cand[c][2], radius)) {
				best = cand[c][0];
			}
		}
	}

	// the marks are this flood's now, not the one a wall test remembers
	g_Stan.lastreach = -1.0f;

	return best;
}

/**
 * Whether a body's circle at `pos` still touches a floor at height `y` (a tile
 * with an area in plan within 3 of it under the middle, or at the nearest
 * point of an edge within the radius). bwalkUpdateVertical() holds a player lifted by
 * geStanClimbFloor() on that floor while it does: he is lifted as his circle
 * meets the floor across, with his middle still over the ground he climbed
 * from, and Perfect Dark's ground is the floor under the middle - he fell
 * back down the climb wall while walking on to it, stopped against it, and
 * was lifted again (Streets' windows, F3 report 20260930-235737).
 */
bool geStanTouchesFloor(struct coord *pos, f32 radius, f32 y)
{
	s32 cx0, cx1, cz0, cz1;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return false;
	}

	cx0 = stanCellOf(pos->x - radius, g_Stan.gridx, g_Stan.gridw);
	cx1 = stanCellOf(pos->x + radius, g_Stan.gridx, g_Stan.gridw);
	cz0 = stanCellOf(pos->z - radius, g_Stan.gridz, g_Stan.gridh);
	cz1 = stanCellOf(pos->z + radius, g_Stan.gridz, g_Stan.gridh);

	for (s32 cz = cz0; cz <= cz1; cz++) {
		for (s32 cx = cx0; cx <= cx1; cx++) {
			const s32 c = cz * g_Stan.gridw + cx;

			for (s32 k = g_Stan.cellstart[c]; k < g_Stan.cellstart[c + 1]; k++) {
				const s32 i = g_Stan.celltiles[k];
				const struct stantile *t = &g_Stan.tiles[i];
				const struct stanpoint *p = &g_Stan.points[t->first];
				s32 ymin = 0x7fff, ymax = -0x8000;
				bool touches;

				if (stanTileUpright(i)) {
					continue;
				}

				for (s32 a = 0; a < t->npts; a++) {
					if (p[a].y < ymin) ymin = p[a].y;
					if (p[a].y > ymax) ymax = p[a].y;
				}

				if (y < ymin - 3.0f || y > ymax + 3.0f) {
					continue;
				}

				// and at that height where it is touched: a tile that slopes
				// spans every height between its ends, and Egyptian's tunnels
				// are each one ramp from a lip 90 over the floor down 734.
				// Asked by the tile's span, the lip's height held the player
				// up the whole way down, walking out over the tunnel's roof
				// (F3 report 20261002-211321)
				if (stanHolds(t, pos->x, pos->z)) {
					const f32 h = stanSurface(t, pos->x, pos->z);

					touches = h >= y - 3.0f && h <= y + 3.0f;
				} else {
					touches = false;
				}

				for (s32 a = 0; a < t->npts && !touches; a++) {
					const struct stanpoint *e0 = &p[a], *e1 = &p[(a + 1) % t->npts];

					if (stanEdgeDistSq(e0, e1, pos->x, pos->z) <= radius * radius) {
						const f32 ex = (f32)(e1->x - e0->x), ez = (f32)(e1->z - e0->z);
						const f32 len = ex * ex + ez * ez;
						f32 f = len > 0.0f ? ((pos->x - e0->x) * ex + (pos->z - e0->z) * ez) / len : 0.0f;
						f32 h;

						f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
						h = e0->y + (e1->y - e0->y) * f;
						touches = h >= y - 3.0f && h <= y + 3.0f;
					}
				}

				if (touches) {
					return true;
				}
			}
		}
	}

	return false;
}

/**
 * Whether GoldenEye would hold a body down here: a tile's special value 1 is
 * g_StanTileSpecialFlags[]'s STANTILEFLAG_FORCECROUCH - Facility's vents, the
 * crawl spaces of seven levels more - and bondview's move sets autocrouchpos
 * to the squat when the tile Bond is on, or one linked to it across an edge
 * his circle touches (stanTileDistanceRelated()), carries it. It is asked from
 * the edge and not from inside, so he is down before his head is under the
 * duct.
 *
 * `hold` (may be NULL) is whether GoldenEye would hold the move as well: its
 * walk out from Bond's tile (stanCheckLinkedSpecialTile()) takes a link into a
 * force-crouch tile for a wall, and bondview's move goes on through it only
 * once `ducking_height_offset` is the full squat. So Bond stops at the mouth
 * of a vent until he is down, and never walks under its roof with his head up.
 * It is true where a force-crouch tile other than the one under `from` - the
 * body's own, where it stands now - was reached.
 */
bool geStanForcesCrouch(struct coord *pos, f32 limit, f32 rise, f32 reach, struct coord *from, bool *hold)
{
	s32 tile;
	s32 own = -1;
	bool result = false;

	if (hold) {
		*hold = false;
	}

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return false;
	}

	// the player's own tile as their walk has it (stanMoverTile()), as the
	// walls are found: Cartel's second silo (Goldfinger 64) has the same
	// sliver of floor along the top of its wall as the first, linked to
	// nothing, across the doorway's force-crouch tiles. Found by height it was
	// the floor under a player stopped in the doorway, its flood reached no
	// crouch tile, and he stood up; each step on was then held until he was
	// down again, and at a high refresh rate the squat never got there - stuck
	// in the doorway (F3 20261003-214052)
	tile = g_StanMover >= 0 ? stanMoverTile(pos->x, pos->z, limit, rise) : -1;

	if (tile < 0) {
		tile = stanTileUnder(pos->x, pos->z, limit, rise);
	}

	if (tile < 0) {
		return false;
	}

	if (hold && from) {
		own = g_StanMover >= 0 ? stanMoverTile(from->x, from->z, limit, rise) : -1;

		if (own < 0) {
			own = stanTileUnder(from->x, from->z, limit, rise);
		}
	}

	stanFlood(tile, pos->x, pos->z, pos->x, pos->z, reach, true);

	for (s32 i = 0; i < g_Stan.numtiles; i++) {
		if (g_Stan.reached[i] == g_Stan.gen && g_Stan.tiles[i].special == GESTAN_SPECIAL_CROUCH) {
			result = true;
			break;
		}
	}

	// held at the edge of a force-crouch tile until the squat is done, as
	// GoldenEye's link into one is a wall until then - from a tile that is not
	// one. A player already on one is in: a second crouch tile in reach (the
	// doorway is two) is no edge to wait at
	if (result && hold && (own < 0 || g_Stan.tiles[own].special != GESTAN_SPECIAL_CROUCH)) {
		*hold = true;
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
static s32 stanWalkLineReached(s32 tile, f32 x0, f32 z0, f32 x1, f32 z1, bool noclimb, bool *reached)
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
			if (reached) {
				*reached = crossings == 0 || next == tile;
			}

			return tile;
		}

		tile = next;
	}

	if (reached) {
		*reached = false;
	}

	return tile;
}

static s32 stanWalkLine(s32 tile, f32 x0, f32 z0, f32 x1, f32 z1, bool noclimb)
{
	return stanWalkLineReached(tile, x0, z0, x1, z1, noclimb, NULL);
}

/**
 * A tile with an area in plan that is a wall all the same: a sliver under half
 * a body across rising more than three times its width, and higher than a
 * body's own reach (GESTAN_RISE). Facility's vent ends in two, a lip leaning 10
 * across and 257 up from the toilet seat to the duct. A stair's riser is the
 * same shape a step high - Control's open metal flights rise 32 over ~10 - and
 * without the height test every other riser of those flights was a wall the
 * player stopped at (F3 20260928-210641).
 */
static bool stanTileSheer(s32 i)
{
	const struct stantile *t = &g_Stan.tiles[i];
	const struct stanpoint *p = &g_Stan.points[t->first];
	f32 area = 0.0f, longest = 0.0f, width;
	s32 ymin = 0x7fff, ymax = -0x8000;

	for (s32 a = 0, b = t->npts - 1; a < t->npts; b = a++) {
		const f32 len = sqrtf((f32)(p[a].x - p[b].x) * (p[a].x - p[b].x) + (f32)(p[a].z - p[b].z) * (p[a].z - p[b].z));

		area += (f32)p[b].x * p[a].z - (f32)p[a].x * p[b].z;

		if (len > longest) longest = len;
		if (p[a].y < ymin) ymin = p[a].y;
		if (p[a].y > ymax) ymax = p[a].y;
	}

	if (longest <= 0.0f) {
		return false;
	}

	width = fabsf(area) / longest;

	return width < 16.0f && ymax - ymin > 3.0f * width && ymax - ymin > GESTAN_RISE;
}

static s32 g_StanPlayerTile[MAX_PLAYERS] = { -1, -1, -1, -1 };
// and the tile the move it walked started on, under the player as they stand
static s32 g_StanPlayerFromTile[MAX_PLAYERS] = { -1, -1, -1, -1 };
static s32 g_StanPlayerTileStage = -1;

void geStanForgetPlayerTile(s32 playernum)
{
	if (playernum >= 0 && playernum < MAX_PLAYERS) {
		g_StanPlayerTile[playernum] = -1;
		g_StanPlayerFromTile[playernum] = -1;
	}
}

/**
 * The tile a player's own move is tested on, as GoldenEye has it: the one its
 * walk from Bond's tile through the links reaches (bondviewTryMoveToStan():
 * stanTestLineUnobstructed() from current_tile_ptr, then stanTestVolume() from
 * where that ended), not the one whose height is nearest the foot. Cartel's
 * first silo (Goldfinger 64) has a sliver of floor along the top of its wall,
 * thirty over the ground and linked to nothing, lying across the doorway in
 * plan; found by height it was the floor under the player in the doorway,
 * every wall of its own stood round him, and the mission could not be finished
 * (F3 20261003-051322). geStanFloorAhead() leaves the walk's tile here for the
 * move it is about to make. -1 where the player has none that holds x/z, at a
 * height a foot could stand on, and the caller finds one by height.
 */
static s32 stanMoverTileFrom(s32 tile, f32 x, f32 z, f32 limit, f32 rise)
{
	const struct coord *from = &g_Vars.players[g_StanMover]->prop->pos;
	f32 y;

	if (tile < 0 || tile >= g_Stan.numtiles || stanTileUpright(tile)) {
		return -1;
	}

	if (!stanHolds(&g_Stan.tiles[tile], x, z)) {
		// a test between where the player stands and where they go: walked
		// on from their tile, as GoldenEye's line test goes
		if (!stanHolds(&g_Stan.tiles[tile], from->x, from->z)) {
			return -1;
		}

		tile = stanWalkLine(tile, from->x, from->z, x, z, false);

		if (!stanHolds(&g_Stan.tiles[tile], x, z) || stanTileUpright(tile)) {
			return -1;
		}
	}

	// a floor the body's foot is on: not one over its reach, nor a storey
	// under it (a player who walked off a ledge where no wall was raised)
	y = stanSurface(&g_Stan.tiles[tile], x, z);

	if (y > limit + rise || y < limit - 160.0f) {
		return -1;
	}

	return tile;
}

static s32 stanMoverTile(f32 x, f32 z, f32 limit, f32 rise)
{
	const s32 m = g_StanMover;
	s32 tile;

	if (m < 0 || m >= MAX_PLAYERS || g_StanPlayerTileStage != g_Stan.stagenum || !g_Vars.players[m]) {
		return -1;
	}

	// where the move about to be made ends, then where it began (a test of
	// the player standing where they are: getting up, a step up)
	tile = stanMoverTileFrom(g_StanPlayerTile[m], x, z, limit, rise);

	if (tile < 0) {
		tile = stanMoverTileFrom(g_StanPlayerFromTile[m], x, z, limit, rise);
	}

	return tile;
}

void geStanSetMover(s32 playernum)
{
	if (g_StanMover != playernum) {
		g_StanMover = playernum;
		g_Stan.lastreach = -1.0f;
	}
}

/**
 * The tile x/z is on, walked to through the links from inside `tile` (or else
 * `other`), at a height a foot at `ground` stands on - or -1.
 */
static s32 stanWalkOnTo(s32 tile, s32 other, f32 x, f32 z, f32 ground)
{
	for (s32 n = 0; n < 2; n++, tile = other) {
		const struct stantile *t;
		const struct stanpoint *p;
		f32 cx = 0.0f, cz = 0.0f, y;
		s32 end;

		if (tile < 0 || tile >= g_Stan.numtiles || stanTileUpright(tile)) {
			continue;
		}

		t = &g_Stan.tiles[tile];
		p = &g_Stan.points[t->first];

		if (t->npts == 0) {
			continue;
		}

		// a tile is convex: its middle is inside it
		for (s32 k = 0; k < t->npts; k++) {
			cx += p[k].x;
			cz += p[k].z;
		}

		cx /= t->npts;
		cz /= t->npts;

		end = stanHolds(t, x, z) ? tile : stanWalkLine(tile, cx, cz, x, z, false);

		if (!stanHolds(&g_Stan.tiles[end], x, z) || stanTileUpright(end)) {
			continue;
		}

		y = stanSurface(&g_Stan.tiles[end], x, z);

		if (y > ground + 10.0f + GESTAN_RISE || y < ground - 160.0f) {
			continue;
		}

		return end;
	}

	return -1;
}

bool geStanFloorAhead(s32 playernum, struct coord *pos, struct coord *to, f32 ground, f32 *surface, bool *sheer)
{
	s32 tile, end;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active || playernum < 0 || playernum >= MAX_PLAYERS) {
		return false;
	}

	if (g_StanPlayerTileStage != g_Stan.stagenum) {
		for (s32 i = 0; i < MAX_PLAYERS; i++) {
			g_StanPlayerTile[i] = -1;
			g_StanPlayerFromTile[i] = -1;
		}

		g_StanPlayerTileStage = g_Stan.stagenum;
	}

	// the tile the player was left on, while it still holds them in plan,
	// whatever Perfect Dark's feet make of its height: GoldenEye keeps Bond's
	// (current_tile_ptr) and walks on from it. Found afresh by height, a
	// player held on the edge of the toilet seat by the ground cylinder with
	// his centre over the vent's lip was on the floor a storey under it, and
	// every wall round the seat was left out of his collision
	tile = g_StanPlayerTile[playernum];

	if (tile < 0 || tile >= g_Stan.numtiles || stanTileUpright(tile)
			|| !stanHolds(&g_Stan.tiles[tile], pos->x, pos->z)) {
		// The player left the tile by a move this walk did not see (a slide
		// along a wall, the easing to a stop): walked on from it through the
		// links, as GoldenEye's current_tile_ptr is, before the floor is
		// looked for by height. In a Cartel silo's doorway (Goldfinger 64, door
		// on pad 1440) the two doorway tiles are crossed by an unlinked sliver at the
		// doorway floor's own height; stopping just over the line between the
		// two, a player was put on the sliver, all its walls stood round him
		// and he could not move again (F3 20261004-084819).
		const s32 walked = stanWalkOnTo(tile, g_StanPlayerFromTile[playernum], pos->x, pos->z, ground);

		tile = walked >= 0 ? walked : stanTileUnder(pos->x, pos->z, ground + 10.0f, GESTAN_RISE);
	}

	g_StanPlayerTile[playernum] = tile;
	g_StanPlayerFromTile[playernum] = tile;

	if (tile < 0) {
		return false;
	}

	end = stanWalkLine(tile, pos->x, pos->z, to->x, to->z, false);

	if (!stanHolds(&g_Stan.tiles[end], to->x, to->z) || stanTileUpright(end)) {
		return false;
	}

	g_StanPlayerTile[playernum] = end;
	*surface = stanSurface(&g_Stan.tiles[end], to->x, to->z);
	*sheer = stanTileSheer(end) && !stanTileLadder(end);

	return true;
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

/**
 * The rooms either side of a door, GoldenEye's way (prop.c's
 * sub_GAME_7F00324C()): from the pad's own tile along the floor to the
 * middle of the door's box (or the pad itself, if the walk cannot get there),
 * then fifty either way along the door's normal, each walk ending on a tile
 * whose room is that side's. `pt1`/`pt2` get the two ends, at the middle's
 * height. `room2` is -1 where both walks end in the same room. False where
 * there is no tile graph or no tile under the pad.
 */
bool geStanDoorSideRooms(struct coord *padpos, struct coord *centre, struct coord *normal,
		s32 *room1, s32 *room2, struct coord *pt1, struct coord *pt2)
{
	struct coord mid = *centre;
	f32 nx = normal->x, ny = normal->y, nz = normal->z;
	f32 len = sqrtf(nx * nx + ny * ny + nz * nz);
	s32 tile, start, end;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active || len <= 0.0f) {
		return false;
	}

	nx /= len;
	nz /= len;

	tile = stanTileUnderPrefer(padpos->x, padpos->z, padpos->y + 5.0f, GESTAN_RISE, -1);

	// A door pad standing in the wall's thickness has no tile of its own
	// storey under it, and the tile found is the floor of the deck below.
	// The conversion filed the pad in that deck's room too (roomsFind() takes
	// the highest tile under a point), so every walk from it stayed down
	// there: six of Frigate's upper-deck doors (pads 247-266) were rooms of
	// the lower deck alone, where no guard's sight, shot or body on their own
	// deck ever met them - guards saw, shot and walked through them shut, and
	// they closed no portal (F3 20260930-004609, 20260929-214745). GoldenEye
	// starts from the tile its pad names, on the door's own floor; take the
	// floor either side of the leaf at the door's height instead.
	if (tile >= 0) {
		s32 side[2] = { -1, -1 };
		f32 sidey[2];
		struct coord at[2];

		for (s32 s = 0; s < 2; s++) {
			const f32 sign = s == 0 ? 1.0f : -1.0f;

			for (f32 d = 10.0f; d <= 60.0f && side[s] < 0; d += 10.0f) {
				const f32 x = centre->x + nx * d * sign, z = centre->z + nz * d * sign;
				const s32 t = stanTileUnderPrefer(x, z, padpos->y + 5.0f, GESTAN_RISE, -1);

				if (t >= 0) {
					side[s] = t;
					sidey[s] = stanSurface(&g_Stan.tiles[t], x, z);
					at[s].x = x;
					at[s].y = centre->y;
					at[s].z = z;
				}
			}
		}

		if (side[0] >= 0 && side[1] >= 0
				&& tile >= 0
				&& sidey[0] - stanSurface(&g_Stan.tiles[tile], padpos->x, padpos->z) > GESTAN_RISE
				&& sidey[1] - stanSurface(&g_Stan.tiles[tile], padpos->x, padpos->z) > GESTAN_RISE) {
			*pt1 = at[0];
			*pt2 = at[1];
			*room1 = g_Stan.tiles[side[0]].room;
			*room2 = g_Stan.tiles[side[1]].room;

			if (*room2 == *room1) {
				*room2 = -1;
			}

			return true;
		}
	}

	if (tile < 0) {
		return false;
	}

	start = stanWalkLine(tile, padpos->x, padpos->z, mid.x, mid.z, false);

	if (!stanHolds(&g_Stan.tiles[start], mid.x, mid.z)) {
		start = tile;
		mid = *padpos;
	}

	pt1->x = mid.x + nx * 50.0f;
	pt1->y = mid.y;
	pt1->z = mid.z + nz * 50.0f;

	pt2->x = mid.x - nx * 50.0f;
	pt2->y = mid.y;
	pt2->z = mid.z - nz * 50.0f;

	end = stanWalkLine(start, mid.x, mid.z, pt1->x, pt1->z, false);
	*room1 = g_Stan.tiles[end].room;

	end = stanWalkLine(start, mid.x, mid.z, pt2->x, pt2->z, false);
	*room2 = g_Stan.tiles[end].room;

	if (*room2 == *room1) {
		*room2 = -1;
	}

	return true;
}

bool geStanWalk(struct coord *from, struct coord *to, s32 *room, f32 *ground)
{
	return geStanWalkFromRoom(from, -1, to, room, ground);
}

/**
 * stan.c's walkTilesBetweenPoints_NoCallback() (sub_GAME_7F0B0914()) as it
 * is: from `*tile` along the line, true when the end is reached; `*tile` is the
 * last tile walked onto either way (stanWalkLine() is the same walk without
 * the answer).
 */
static bool stanWalkTo(s32 *tile, f32 x0, f32 z0, f32 x1, f32 z1)
{
	s32 cur = *tile, prev = *tile, prevprev = *tile, next = -1;
	const f32 negdz = -(z1 - z0);
	const f32 dx = x1 - x0;

	for (s32 iter = 0;; iter++) {
		const struct stantile *t = &g_Stan.tiles[cur];
		const struct stanpoint *p = &g_Stan.points[t->first];
		s32 crossings = 0;

		for (s32 k = 0; k < t->npts; k++) {
			const struct stanpoint *a = &p[k], *b = &p[(k + 1) % t->npts];
			const bool linked = a->across >= 0;

			if (negdz * (b->x - a->x) + dx * (b->z - a->z) <= 0.0f
					&& stanCrosses(x0, z0, x1, z1, a->x, a->z, b->x, b->z, linked)) {
				crossings++;

				if (!linked || (a->across != prev && a->across != prevprev)) {
					next = linked ? a->across : -1;
				}
			}
		}

		prevprev = prev;
		prev = cur;

		if (cur == next || crossings == 0) {
			return true;
		}

		if (iter >= 0x1f5 || next < 0) {
			return false;
		}

		cur = next;
		*tile = next;
	}
}

/**
 * The tile and the place an object's rooms are counted from, as GoldenEye
 * keeps them in its prop (prop->stan and prop->pos; prop.c's
 * domakedefaultobj() and propobj.c's sub_GAME_7F04088C()). From the pad's own
 * tile - the one under the pad in its own room (the conversion files a pad in
 * the room of the tile GoldenEye names for it) - a bound pad's object first
 * walks to its box's middle (`centre`, NULL for none) or stays at the pad,
 * then from there to where the object stands: that is prop->pos where the
 * walk gets there, and where it does not, prop->pos stays where it started.
 * False with no tile at all.
 */
bool geStanObjectTile(struct coord *padpos, s32 padroom, struct coord *centre, struct coord *objpos,
		s32 *tile, struct coord *seed)
{
	struct coord start = *padpos;
	s32 t, t0;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return false;
	}

	t0 = stanTileUnderPrefer(padpos->x, padpos->z, padpos->y + 5.0f, GESTAN_RISE, padroom);

	if (t0 < 0) {
		t0 = stanTileUnderPrefer(objpos->x, objpos->z, objpos->y + 5.0f, GESTAN_RISE, padroom);

		if (t0 < 0) {
			return false;
		}

		*tile = t0;
		*seed = *objpos;
		return true;
	}

	*tile = t0;

	if (centre) {
		t = t0;

		if (stanWalkTo(&t, padpos->x, padpos->z, centre->x, centre->z)) {
			*tile = t;
			start = *centre;
		}
	}

	t = *tile;

	if (stanWalkTo(&t, start.x, start.z, objpos->x, objpos->z)) {
		*tile = t;
		*seed = *objpos;
	} else {
		*seed = start;
	}

	return true;
}

/** A tile's room, or -1. */
s32 geStanTileRoom(s32 tile)
{
	return g_Stan.active && tile >= 0 && tile < g_Stan.numtiles ? g_Stan.tiles[tile].room : -1;
}

/** stan.c's getShortest2dDispToInfTileEdge(): the signed distance from the edge's line. */
static f32 stanEdgeDisp(const struct stantile *t, s32 k, f32 x, f32 z)
{
	const struct stanpoint *p = &g_Stan.points[t->first];
	const struct stanpoint *a = &p[k], *b = &p[(k + 1) % t->npts];
	const f32 ex = (f32)(b->x - a->x), ez = (f32)(b->z - a->z);
	const f32 len = sqrtf(ex * ex + ez * ez);

	if (len == 0.0f) {
		const f32 vx = x - b->x, vz = z - b->z;
		return sqrtf(vx * vx + vz * vz);
	}

	return (ez * (x - a->x) + -ex * (z - a->z)) / len;
}

/** stan.c's stanPointProjectsOntoTileEdge(). */
static bool stanProjectsOntoEdge(const struct stantile *t, s32 k, f32 x, f32 z)
{
	const struct stanpoint *p = &g_Stan.points[t->first];
	const struct stanpoint *a = &p[k], *b = &p[(k + 1) % t->npts];
	const f32 ex = (f32)(b->x - a->x), ez = (f32)(b->z - a->z);
	const f32 len2 = ex * ex + ez * ez;
	const f32 dot = (x - a->x) * ex + (z - a->z) * ez;

	return (len2 < dot && dot < 0.0f) || (0.0f < dot && dot < len2);
}

/**
 * The rooms of the tiles round a place (stan.c's sub_GAME_7F0B21B0() through
 * sub_GAME_7F0B1DDC()): from `tile`, every tile across a linked edge that
 * comes within `radius` of x/z, breadth first, at most 41 tiles; each new
 * tile's room appended to `rooms` while there is room for it. Returns the
 * count. GoldenEye's own units are its level's scaled by a constant, which
 * every comparison here is free of.
 */
s32 geStanLocusRooms(s32 tile, f32 x, f32 z, f32 radius, s32 *rooms, s32 max)
{
	s32 stack[48];
	s32 cat = 1;
	s32 visited = 0;
	s32 count = 0;

	if (!g_Stan.active || tile < 0 || tile >= g_Stan.numtiles) {
		return 0;
	}

	stack[0] = tile;

	do {
		const s32 cur = stack[visited++];
		const struct stantile *t = &g_Stan.tiles[cur];
		const struct stanpoint *p = &g_Stan.points[t->first];
		s32 i;

		// stanLocusAddTileRoomIfNew()
		for (i = 0; i < count && rooms[i] != t->room; i++);

		if (i == count && count < max) {
			rooms[count++] = t->room;
		}

		for (s32 k = 0; k < t->npts; k++) {
			const s32 k2 = (k + 1) % t->npts;
			const f32 edge = stanEdgeDisp(t, k, x, z);
			const f32 da = sqrtf((x - p[k].x) * (x - p[k].x) + (z - p[k].z) * (z - p[k].z));
			const f32 db = sqrtf((x - p[k2].x) * (x - p[k2].x) + (z - p[k2].z) * (z - p[k2].z));

			if (edge < radius && (da < radius || db < radius || stanProjectsOntoEdge(t, k, x, z))
					&& p[k].across >= 0) {
				s32 j;

				for (j = cat - 1; j >= 0 && stack[j] != p[k].across; j--);

				if (j < 0 && cat < (s32)ARRAYCOUNT(stack)) {
					stack[cat++] = p[k].across;
				}
			}
		}

		if (cat >= 41) {
			break;
		}
	} while (visited < cat);

	return count;
}

s32 geStanRoomUnder(struct coord *pos, f32 ground, s32 prefer)
{
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return -1;
	}

	// the floor found is the highest in the body's circle, which a tile at
	// the circle's edge can give: the one under its middle is at it or under
	tile = stanTileUnderPrefer(pos->x, pos->z, ground + 1.0f, 1.0f, prefer);

	if (tile < 0 || ground - stanSurface(&g_Stan.tiles[tile], pos->x, pos->z) > GESTAN_RISE) {
		return -1;
	}

	return g_Stan.tiles[tile].room;
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

s32 geStanLinks(struct coord *from, f32 fromground, struct coord *to, f32 toground, bool climbs)
{
	s32 fromtile;
	s32 totile;
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return -1;
	}

	fromtile = stanTileUnder(from->x, from->z, fromground + 10.0f, 0.0f);
	totile = stanTileUnder(to->x, to->z, toground + 10.0f, 0.0f);

	if (fromtile < 0 || totile < 0) {
		return -1;
	}

	tile = stanWalkLine(fromtile, from->x, from->z, to->x, to->z, !climbs);

	if (tile == totile) {
		return 1;
	}

	return stanHolds(&g_Stan.tiles[tile], to->x, to->z)
		&& fabsf(stanSurface(&g_Stan.tiles[tile], to->x, to->z) - stanSurface(&g_Stan.tiles[totile], to->x, to->z)) < 1.0f;
}

f32 geStanFloorAt(struct coord *pos)
{
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return -1e30f;
	}

	tile = stanTileUnder(pos->x, pos->z, pos->y + 5.0f, 0.0f);

	return tile >= 0 ? stanSurface(&g_Stan.tiles[tile], pos->x, pos->z) : -1e30f;
}

/**
 * An autogun's pad is where GoldenEye stands the object (autogunGeEye()), and
 * its tile is the pad's; the gun sees Bond where stanTestLineUnobstructed()
 * from it to him gets through and ends on his own tile (`collisionTile ==
 * playerProp2->stan`). Perfect Dark's line of sight from the pad is a line in
 * three dimensions through the level's geometry, and Control's gun hung in
 * the corner of the room over the blast door's corridor saw and shot Bond
 * through the wall of it (F3 20260927-234309), where GoldenEye's walk meets
 * the wall's edge and stops on the gun's side of it. The pad is on the floor
 * under a ceiling gun or up at it; either way the tile is the one of the
 * pad's room at or under it.
 */
s32 geStanAutogunSees(struct coord *from, s32 fromroom, struct coord *to, f32 toground)
{
	s32 fromtile;
	s32 totile;
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return -1;
	}

	fromtile = stanTileUnderPrefer(from->x, from->z, from->y + 5.0f, GESTAN_RISE, fromroom);
	totile = stanTileUnder(to->x, to->z, toground + 10.0f, GESTAN_RISE);

	if (fromtile < 0 || totile < 0) {
		return -1;
	}

	tile = stanWalkLine(fromtile, from->x, from->z, to->x, to->z, false);

	if (tile == totile) {
		return 1;
	}

	// the target on the seam between two tiles at the same height
	return stanHolds(&g_Stan.tiles[tile], to->x, to->z)
		&& fabsf(stanSurface(&g_Stan.tiles[tile], to->x, to->z) - stanSurface(&g_Stan.tiles[totile], to->x, to->z)) < 1.0f;
}

/**
 * Whether a player standing at `from` (feet at `ground`) may pick up the
 * collectable at `to`: GoldenEye's objTestForPickup() (propobj.c) walks
 * stanTestLineUnobstructed() in plan from Bond's tile to the object and takes
 * it only where the walk ends on the object's own tile (`stan == prop->stan`).
 * No wall, grate or floor in three dimensions comes into it, which is how the
 * watch magnet lifts Bunker 2's throwing knives out of the cell's drain: they
 * lie 240 under the drain's tile, and Perfect Dark's line of sight from the
 * player's eye to them went through its floor (F3 20260929-215647,
 * 20260930-030654).
 *
 * The object's tile is the one under it; one under every floor (the knives in
 * the drain) is on the tile over it, whichever the walk ends on there. 1 may,
 * 0 may not, -1 where the level has no graph or the player is over no tile.
 */
s32 geStanPickupReaches(struct coord *from, f32 ground, struct coord *to)
{
	s32 fromtile;
	s32 totile;
	s32 tile;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return -1;
	}

	fromtile = stanTileUnder(from->x, from->z, ground + 10.0f, GESTAN_RISE);

	if (fromtile < 0) {
		return -1;
	}

	tile = stanWalkLine(fromtile, from->x, from->z, to->x, to->z, false);

	if (!stanHolds(&g_Stan.tiles[tile], to->x, to->z)) {
		return 0;
	}

	totile = stanTileUnder(to->x, to->z, to->y + 10.0f, GESTAN_RISE);

	if (totile < 0 || totile == tile) {
		return 1;
	}

	// on the seam between two tiles at the same height
	return fabsf(stanSurface(&g_Stan.tiles[tile], to->x, to->z) - stanSurface(&g_Stan.tiles[totile], to->x, to->z)) < 1.0f;
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
 * GoldenEye's walkTilesBetweenPoints_NoCallback() from the tile under `from`
 * to x1/z1, as its objTestForInteract() asks it for a prop with
 * PROPFLAG2_INTERACTCHECKLOS: 1 when the walk ends without an edge left to
 * cross, 0 when an edge with nothing across stops it, -1 with no graph or no
 * tile under `from`. Unlike geStanLineReach() the end need not be held by a
 * tile: a point on a hole's edge counts as reached, which is where Aztec's
 * mainframe keyboard stands, on the edge of the hole under its desk.
 */
s32 geStanWalkReaches(struct coord *from, f32 x1, f32 z1)
{
	bool reached;
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

	stanWalkLineReached(tile, from->x, from->z, x1, z1, false, &reached);

	return reached ? 1 : 0;
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

/**
 * GoldenEye's own test of a guard's pad as the setup is loaded: expand_09_
 * characters() (chraction.c) makes a guard only where getposstan(&pad->pos,
 * pad->stan, 20, ...) holds, and getposstan() (loadobjectmodel.c) neither moves
 * the guard nor looks for a clear spot - it asks stanTestVolume() whether a
 * circle of 20 at the pad itself is legal, and the guard stands at the pad or
 * is not made at all. stanTestVolume() (stan.c) is two tests, both in plan:
 *
 * - the tile walk (sub_GAME_7F0B21B0): out from the pad's tile through every
 *   linked edge the circle touches; an unlinked edge it touches is a wall and
 *   refuses the guard, and so does a walk of 41 tiles or more;
 * - the props in the rooms of the tiles walked, of the types objects, doors,
 *   players, chrs and path blockers: refused where the circle comes within 20
 *   of an *edge* of a prop's collision outline. Only the edges: a pad inside a
 *   big object's outline with every edge further than 20 is legal, and nothing
 *   is asked about height at all (its y range is off: arg5 0, arg6 1).
 *
 * Perfect Dark's bodyAllocateChr() asks cdTestVolume() instead - the
 * converted level's raised walls in the pad's room within 200 up or down,
 * and any prop whose block the circle overlaps or is inside - which refused
 * guards GoldenEye makes (Facility 51 and Caverns 33 at a raised wall,
 * Frigate 26 and Caverns 10 standing on an object) and made one it refuses
 * (Caverns 41).
 *
 * 1 legal, 0 refused, -1 where the level has no graph or the pad is over no
 * tile (the caller keeps Perfect Dark's own test).
 */
#define GESTAN_SPAWN_MAXTILES 41
#define GESTAN_SPAWN_MAXROOMS 20
// The graph's points are GoldenEye's tile units rounded to whole units of the
// converted level (geconvert.c's writeStan()), up to half a unit out on each
// axis: an edge GoldenEye measures 20.4 from Aztec's pad 92 is 19.99 here, and
// its guard 9 was refused. The tile walk is taken that much short of the
// radius. GoldenEye's nearest wall to a pad it refuses is 17.6 (Streets 170).
#define GESTAN_SPAWN_ROUNDING 0.75f

static bool stanSpawnEdgeNear(f32 ax, f32 az, f32 bx, f32 bz, f32 x, f32 z, f32 radius)
{
	const f32 ex = bx - ax, ez = bz - az;
	const f32 len = ex * ex + ez * ez;
	f32 f = len > 0.0f ? ((x - ax) * ex + (z - az) * ez) / len : 0.0f;
	f32 dx, dz;

	f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
	dx = x - (ax + ex * f);
	dz = z - (az + ez * f);

	return dx * dx + dz * dz < radius * radius;
}

/** Whether the circle at x/z touches an edge of the prop's collision outline (chraiGetCollisionBounds()). */
static bool stanSpawnPropNear(struct prop *prop, f32 x, f32 z, f32 radius)
{
	u8 *start;
	u8 *end;
	struct geo *geo;

	if (!propUpdateGeometry(prop, &start, &end)) {
		return false;
	}

	geo = (struct geo *)start;

	while (geo < (struct geo *)end) {
		if (geo->type == GEOTYPE_BLOCK) {
			struct geoblock *block = (struct geoblock *)geo;
			const s32 n = block->header.numvertices;

			for (s32 i = 0; i < n; i++) {
				const s32 j = (i + 1) % n;

				if (stanSpawnEdgeNear(block->vertices[i][0], block->vertices[i][1],
							block->vertices[j][0], block->vertices[j][1], x, z, radius)) {
					return true;
				}
			}

			geo = (struct geo *)((uintptr_t)geo + 0x4c);
		} else if (geo->type == GEOTYPE_CYL) {
			// GoldenEye's chr is a diamond chrwidth from its middle to each
			// corner (chrUpdateCollisionBounds()), its player the same
			struct geocyl *cyl = (struct geocyl *)geo;
			const f32 c[4][2] = {
				{ cyl->x + cyl->radius, cyl->z }, { cyl->x, cyl->z + cyl->radius },
				{ cyl->x - cyl->radius, cyl->z }, { cyl->x, cyl->z - cyl->radius },
			};

			for (s32 i = 0; i < 4; i++) {
				if (stanSpawnEdgeNear(c[i][0], c[i][1], c[(i + 1) % 4][0], c[(i + 1) % 4][1], x, z, radius)) {
					return true;
				}
			}

			geo = (struct geo *)((uintptr_t)geo + 0x18);
		} else {
			// a lift's tiles: GoldenEye has no such outline
			break;
		}
	}

	return false;
}

s32 geStanSpawnLegal(struct coord *pos, s32 padroom, f32 radius)
{
	s32 queue[GESTAN_MAXFLOOD];
	RoomNum rooms[GESTAN_SPAWN_MAXROOMS + 1];
	s16 propnums[MAX_ROOMPROPS];
	const f32 reach = radius - GESTAN_SPAWN_ROUNDING;
	s32 numrooms = 0;
	s32 tile;
	s32 n;

	if (g_Stan.stagenum != g_Vars.stagenum || g_Stan.tiledata != g_TileFileData.u8) {
		stanBuild();
	}

	if (!g_Stan.active) {
		return -1;
	}

	// the pad's own tile (pad->stan): the one of the pad's room at or under it,
	// as an autogun's pad finds its tile
	tile = stanTileUnderPrefer(pos->x, pos->z, pos->y + 5.0f, GESTAN_RISE, padroom);

	if (tile < 0) {
		return -1;
	}

	n = stanFloodList(tile, pos->x, pos->z, pos->x, pos->z, reach, true, false, queue);

	if (n >= GESTAN_SPAWN_MAXTILES) {
		sysLogPrintf(LOG_NOTE, "gestan: pad (%.0f %.0f %.0f) refused: its circle walks %d tiles", pos->x, pos->y, pos->z, n);
		return 0;
	}

	for (s32 q = 0; q < n; q++) {
		const struct stantile *t = &g_Stan.tiles[queue[q]];
		const struct stanpoint *p = &g_Stan.points[t->first];
		s32 r;

		for (s32 k = 0; k < t->npts; k++) {
			if (p[k].across < 0 && stanEdgeDistSq(&p[k], &p[(k + 1) % t->npts], pos->x, pos->z) < reach * reach) {
				sysLogPrintf(LOG_NOTE, "gestan: pad (%.0f %.0f %.0f) refused: tile %d's edge %d is a wall",
						pos->x, pos->y, pos->z, queue[q], k);
				return 0;
			}
		}

		for (r = 0; r < numrooms; r++) {
			if (rooms[r] == t->room) {
				break;
			}
		}

		if (r == numrooms && numrooms < GESTAN_SPAWN_MAXROOMS) {
			rooms[numrooms++] = t->room;
		}
	}

	rooms[numrooms] = -1;
	roomGetProps(rooms, propnums, MAX_ROOMPROPS);

	for (s16 *pn = propnums; *pn >= 0; pn++) {
		struct prop *prop = &g_Vars.props[*pn];

		if (propIsOfCdType(prop, CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PLAYERS | CDTYPE_CHRS | CDTYPE_PATHBLOCKER)
				&& stanSpawnPropNear(prop, pos->x, pos->z, radius)) {
			sysLogPrintf(LOG_NOTE, "gestan: pad (%.0f %.0f %.0f) refused: prop type %d at (%.0f %.0f %.0f)",
					pos->x, pos->y, pos->z, prop->type, prop->pos.x, prop->pos.y, prop->pos.z);
			return 0;
		}
	}

	return 1;
}

#endif
