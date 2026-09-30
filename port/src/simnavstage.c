/**
 * A Perfect Dark stage's navmesh: its collision tiles taken apart into
 * Recast's input, the mesh cached on disk, drawn over the level, and built for
 * every arena at once from the command line. port/include/simnav.h has the
 * overview; port/src/simnav.cpp is the Recast side.
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
 * **Out of scope for M1**: props (doors, glass, crates, lifts - their geometry
 * moves, and is DetourTileCache's in M6), ladders and drops as links (M2), and
 * the low-clearance hints GEOFLAG_AIBOTCROUCH/DUCK, which mark the geometry
 * over a crouch rather than the floor under it. A GoldenEye level converted
 * into GE Plus has its own collision (gestan.c's 2-D tile graph, whose walls
 * are there for one body and not another) and is skipped with a log line.
 *
 * **Agent size**, from the chr movement code:
 *  - radius 20: chrInit() (chr.c) gives every chr 20; only a few special
 *    bodies (Dr Caroll 30, the ChicRob 42, body.c) change it, none a simulant.
 *  - height 185: chrTick() sets an aibot's height to 185 standing (135 ducked,
 *    90 crouched, chr.c), and chrGetBbox() tops its box at manground + height.
 *  - climb 30: chrGetBbox() starts a walker's box 20 over its manground, so a
 *    wall lower than that is walked over, and the ground a walker follows is
 *    let lag it by up to 30 (chrTickFalling()'s "ground - 30", the bot step's
 *    "rise = ground - 30 - manground"); a floor within 30 is the same floor to
 *    it. 30 is 6 cells of 5.
 *
 * **Cache.** cache/navmesh/pd/<stage>-<hash>.bin beside the executable (or in
 * the save directory where that cannot be written, fsChooseOutputDir()). The
 * hash is over the triangles, their areas and the build parameters, so a mod
 * or the XBLA release's tiles (xblaStageLoadTiles()) that change a level make
 * a new file rather than reading an old one.
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
#include "modloader.h"
#include "simnav.h"

#define SIMNAV_EXTRACT_VERSION 1 // bump when the extraction or the saved form changes
#define SIMNAV_CACHE_MAGIC     0x4e44504e // 'NPDN'
#define SIMNAV_DRAW_LIFT       6.0f   // units the drawn mesh stands over the floor
#define SIMNAV_DRAW_REACH      4000.0f // tiles further than this from the camera are not drawn
#define SIMNAV_DRAW_ALPHA      0x78

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
	p->agentclimb = 30.0f;
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

struct simnavgeom {
	f32 *verts;
	s32 numverts, maxverts;
	s32 *tris;
	u8 *areas;
	s32 numtris, maxtris;
	s32 floors, walls, died, blocks, skipped;
};

static void simnavGeomFree(struct simnavgeom *g)
{
	free(g->verts);
	free(g->tris);
	free(g->areas);
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

static void simnavGeomTri(struct simnavgeom *g, s32 a, s32 b, s32 c, u8 area)
{
	s32 *t = &g->tris[g->numtris * 3];

	t[0] = a;
	t[1] = b;
	t[2] = c;
	g->areas[g->numtris] = area;
	g->numtris++;
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

		simnavGeomTri(g, base + i * 2, base + j * 2, base + j * 2 + 1, SIMNAV_AREA_NONE);
		simnavGeomTri(g, base + i * 2, base + j * 2 + 1, base + i * 2 + 1, SIMNAV_AREA_NONE);
	}

	for (i = 1; i < n - 1; i++) {
		simnavGeomTri(g, base + 1, base + i * 2 + 1, base + (i + 1) * 2 + 1, SIMNAV_AREA_NONE);
		simnavGeomTri(g, base, base + (i + 1) * 2, base + i * 2, SIMNAV_AREA_NONE);
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

				if (nv >= 3 && simnavTileWanted(g, geo->flags)) {
					const u8 area = simnavTileArea(geo->flags);
					s32 base, i;

					if (!simnavGeomReserve(g, nv, nv - 2)) {
						return 0;
					}

					base = g->numverts;

					for (i = 0; i < nv; i++) {
						simnavGeomVert(g, tile->vertices[i][0], tile->vertices[i][1], tile->vertices[i][2]);
					}

					// tiles are convex, as cdIs2dPointInIntTile() takes them
					for (i = 1; i < nv - 1; i++) {
						simnavGeomTri(g, base, base + i, base + i + 1, area);
					}
				}

				p += nv * 6 + 0xe;
			} else if (geo->type == GEOTYPE_TILE_F) {
				const struct geotilef *tile = (const struct geotilef *)geo;

				if (nv >= 3 && simnavTileWanted(g, geo->flags)) {
					const u8 area = simnavTileArea(geo->flags);
					s32 base, i;

					if (!simnavGeomReserve(g, nv, nv - 2)) {
						return 0;
					}

					base = g->numverts;

					for (i = 0; i < nv; i++) {
						simnavGeomVert(g, tile->vertices[i].x, tile->vertices[i].y, tile->vertices[i].z);
					}

					for (i = 1; i < nv - 1; i++) {
						simnavGeomTri(g, base, base + i, base + i + 1, area);
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

	return 1;
}

/** FNV-1a over the input and the parameters it is built with */
static u64 simnavHash(const struct simnavgeom *g, const struct simnavparams *params)
{
	u64 h = 0xcbf29ce484222325ull;
	const u32 version = SIMNAV_EXTRACT_VERSION;
	const struct { const void *p; size_t len; } parts[] = {
		{ &version, sizeof(version) },
		{ params, sizeof(*params) },
		{ g->verts, sizeof(f32) * 3 * g->numverts },
		{ g->tris, sizeof(s32) * 3 * g->numtris },
		{ g->areas, g->numtris },
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
static struct simnavmesh *simnavObtain(s32 stagenum, const u8 *tiledata, struct simnavgeom *g, s32 *fromcache, u64 *hashout)
{
	struct simnavparams params;
	struct simnavinput input;
	struct simnavstats stats;
	struct simnavmesh *mesh;
	char path[FS_MAXPATH + 1];
	char err[256];
	s32 havepath;
	u64 hash;

	*fromcache = 0;
	simnavGetParams(&params);

	if (!simnavExtract(tiledata, g)) {
		sysLogPrintf(LOG_WARNING, "simnav: stage 0x%02x: its tiles could not be read", stagenum);
		return NULL;
	}

	hash = simnavHash(g, &params);
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
	input.numtris = g->numtris;

	mesh = simnavMeshBuild(&input, &params, &stats, err, sizeof(err));

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

struct simnavdrawtile {
	f32 minx, minz, maxx, maxz;
	Gfx *dl;
};

static struct {
	s32 stagenum;
	struct simnavmesh *mesh;
	struct simnavdrawtile *tiles;
	s32 numtiles;
	Vtx *vtx;
	Col *col;
	Gfx *gfx;
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

static s16 simnavS16(f32 v)
{
	return v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (s16)floorf(v + 0.5f);
}

static void simnavFreeDraw(void)
{
	free(g_SimNav.tiles);
	free(g_SimNav.vtx);
	free(g_SimNav.col);
	free(g_SimNav.gfx);
	g_SimNav.tiles = NULL;
	g_SimNav.vtx = NULL;
	g_SimNav.col = NULL;
	g_SimNav.gfx = NULL;
	g_SimNav.numtiles = 0;
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
}

void simnavStageStop(void)
{
	simnavFreeDraw();
	simnavMeshFree(g_SimNav.mesh);
	g_SimNav.mesh = NULL;
	g_SimNav.stagenum = -1;
}

void simnavStageStart(s32 stagenum)
{
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

	g_SimNav.mesh = simnavObtain(stagenum, g_TileFileData.u8, &g, &fromcache, &hash);

	if (g_SimNav.mesh) {
		g_SimNav.stagenum = stagenum;
		simnavMeshGetStats(g_SimNav.mesh, &stats);
		simnavLogStats(stagenum, &stats, &g, fromcache, hash);
		simnavBuildDraw();
	}

	simnavGeomFree(&g);
}

Gfx *simnavRender(Gfx *gdl)
{
	struct player *player = g_Vars.currentplayer;
	Mtxf *mtx;
	s32 i;

	if (!g_SimNav.mesh || !g_SimNav.tiles || !player || g_SimNav.stagenum != g_Vars.stagenum) {
		return gdl;
	}

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

void simnavBuildAllFromCommandLine(void)
{
	s32 built = 0, failed = 0, skipped = 0;
	f32 totalms = 0;
	s32 i;

	if (!sysArgCheck("--simnav-build-all")) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "simnav: building every multiplayer arena's navmesh");
	printf("\n%-4s  %-22s  %6s  %6s  %7s  %5s  %6s  %9s  %s\n",
			"id", "arena", "polys", "verts", "dtris", "tiles", "failed", "build ms", "note");

	for (i = 0; i < MP_NUM_ARENAS_STATIC; i++) {
		const s32 stagenum = g_MpArenas[i].stagenum;
		const s32 index = stageGetIndex(stagenum);
		const char *name = simnavArenaName(i);
		char namebuf[32];
		struct simnavgeom g;
		struct simnavstats stats;
		struct simnavmesh *mesh;
		s32 fromcache, fileid;
		u32 size;
		u64 hash = 0;
		u8 *buf;

		if (stagenum == STAGE_MP_RANDOM) {
			continue;
		}

		if (!name) {
			snprintf(namebuf, sizeof(namebuf), "(solo stage 0x%02x)", stagenum);
			name = namebuf;
		}

		if (index < 0 || (fileid = g_Stages[index].tilefileid) == 0) {
			printf("0x%02x  %-22s  %6s  %6s  %7s  %5s  %6s  %9s  %s\n", stagenum, name, "-", "-", "-", "-", "-", "-", "no stage");
			skipped++;
			continue;
		}

		if (modloaderStageIsRemake(stagenum)) {
			printf("0x%02x  %-22s  %6s  %6s  %7s  %5s  %6s  %9s  %s\n", stagenum, name, "-", "-", "-", "-", "-", "-", "GoldenEye conversion, skipped");
			skipped++;
			continue;
		}

		// the file as tilesReset() loads it, into memory of our own
		size = fileGetInflatedSize(fileid, LOADTYPE_TILES);

		if (size == 0) {
			printf("0x%02x  %-22s  %6s  %6s  %7s  %5s  %6s  %9s  %s\n", stagenum, name, "-", "-", "-", "-", "-", "-", "no tiles file");
			failed++;
			continue;
		}

		size = ((size + 0x20) & ~0xfu) + 0x8000;
		buf = calloc(1, size);

		if (!buf) {
			failed++;
			continue;
		}

		g_LoadType = LOADTYPE_TILES;
		fileLoadToAddr(fileid, FILELOADMETHOD_EXTRAMEM, buf, size);

		mesh = simnavObtain(stagenum, buf, &g, &fromcache, &hash);

		if (mesh) {
			simnavMeshGetStats(mesh, &stats);
			simnavLogStats(stagenum, &stats, &g, fromcache, hash);
			printf("0x%02x  %-22s  %6d  %6d  %7d  %5d  %6d  %9.1f  %s\n", stagenum, name,
					stats.polys, stats.verts, stats.detailtris, stats.tiles, stats.failedtiles, stats.buildms,
					fromcache ? "cached" : "");
			totalms += stats.buildms;
			built++;
			simnavMeshFree(mesh);
		} else {
			printf("0x%02x  %-22s  %6s  %6s  %7s  %5s  %6s  %9s  %s\n", stagenum, name, "-", "-", "-", "-", "-", "-", "FAILED (see log)");
			failed++;
		}

		simnavGeomFree(&g);
		free(buf);
	}

	printf("\nsimnav: %d built, %d failed, %d skipped, %.1f ms building\n", built, failed, skipped, totalms);
	fflush(stdout);

	exit(failed ? 1 : 0);
}
