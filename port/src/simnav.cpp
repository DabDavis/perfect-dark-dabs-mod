/**
 * Recast/Detour behind a C interface (port/include/simnav.h). No game code
 * here: port/src/simnavstage.c hands in triangles and gets a mesh back.
 *
 * The build is Recast's tiled one (RecastDemo's Sample_TileMesh, without its
 * chunky mesh): a single tile would hold at most 65535 vertices, which the
 * larger solo stages come near, and DetourTileCache - doors and glass, later -
 * works a tile at a time too. Poly refs are 64 bits (DT_POLYREF64, set for
 * every file of the library in CMakeLists.txt) so neither the tile count nor
 * the polygons in one tile is capped by the other.
 *
 * Every allocation is Recast's and Detour's own (malloc), never the game's
 * pools: the mesh outlives nothing and belongs to no stage allocation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <new>
#include <chrono>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourAlloc.h"

#include "simnav.h"

#ifndef DT_POLYREF64
#error "simnav.cpp and Detour must be built with DT_POLYREF64 (CMakeLists.txt)"
#endif

struct simnavmesh {
	dtNavMesh *nav;
	simnavstats stats;
};

namespace {

class SimNavContext : public rcContext {
public:
	char *err;
	size_t errlen;

	SimNavContext(char *e, size_t len) : rcContext(true), err(e), errlen(len)
	{
		if (err && errlen) {
			err[0] = '\0';
		}
	}

protected:
	void doLog(const rcLogCategory category, const char *msg, const int len) override
	{
		// the first error is the one that says why
		if (category == RC_LOG_ERROR && err && errlen && !err[0]) {
			snprintf(err, errlen, "%.*s", len, msg);
		}
	}
};

struct TileBuild {
	rcHeightfield *hf = nullptr;
	rcCompactHeightfield *chf = nullptr;
	rcContourSet *cset = nullptr;
	rcPolyMesh *pmesh = nullptr;
	rcPolyMeshDetail *dmesh = nullptr;

	~TileBuild()
	{
		rcFreeHeightField(hf);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
	}
};

enum TileResult { TILE_OK, TILE_EMPTY, TILE_FAILED };

TileResult buildTile(SimNavContext &ctx, const rcConfig &base, const simnavparams *sp, const simnavinput *in,
		const float *tribounds, int tx, int ty, const float *meshbmin, const float *meshbmax,
		int *scratchtris, unsigned char *scratchareas, unsigned char **outdata, int *outsize)
{
	const int stepover = (int)floorf(sp->agentstepover / base.ch);
	const int standing = (int)ceilf(sp->agentheight / base.ch);
	const int ducked = (int)ceilf(sp->agentduckheight / base.ch);
	rcConfig cfg = base;
	const float tcs = cfg.tileSize * cfg.cs;
	TileBuild b;
	int n = 0;

	cfg.bmin[0] = meshbmin[0] + tx * tcs;
	cfg.bmin[1] = meshbmin[1];
	cfg.bmin[2] = meshbmin[2] + ty * tcs;
	cfg.bmax[0] = meshbmin[0] + (tx + 1) * tcs;
	cfg.bmax[1] = meshbmax[1];
	cfg.bmax[2] = meshbmin[2] + (ty + 1) * tcs;
	cfg.bmin[0] -= cfg.borderSize * cfg.cs;
	cfg.bmin[2] -= cfg.borderSize * cfg.cs;
	cfg.bmax[0] += cfg.borderSize * cfg.cs;
	cfg.bmax[2] += cfg.borderSize * cfg.cs;

	// the triangles reaching into the tile and its border
	for (int i = 0; i < in->numtris; i++) {
		const float *tb = &tribounds[i * 4];

		if (tb[0] > cfg.bmax[0] || tb[2] < cfg.bmin[0] || tb[1] > cfg.bmax[2] || tb[3] < cfg.bmin[2]) {
			continue;
		}

		scratchtris[n * 3 + 0] = in->tris[i * 3 + 0];
		scratchtris[n * 3 + 1] = in->tris[i * 3 + 1];
		scratchtris[n * 3 + 2] = in->tris[i * 3 + 2];
		scratchareas[n] = in->areas[i];
		n++;
	}

	if (n == 0) {
		return TILE_EMPTY;
	}

	b.hf = rcAllocHeightfield();

	if (!b.hf || !rcCreateHeightfield(&ctx, *b.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
		return TILE_FAILED;
	}

	// A wall and the floor under it are one span in a cell they share, and
	// the floor's area wins only where the wall tops out within a step over
	// (chrGetBbox()'s box starts that far over the feet); a taller wall
	// stands on the floor as solid, however high a step the floors either
	// side of it are. The walkable climb is for floors only.
	if (!rcRasterizeTriangles(&ctx, in->verts, in->numverts, scratchtris, scratchareas, n, *b.hf, stepover)) {
		return TILE_FAILED;
	}

	rcFilterLowHangingWalkableObstacles(&ctx, stepover, *b.hf);
	rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *b.hf);
	rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *b.hf);

	b.chf = rcAllocCompactHeightfield();

	if (!b.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *b.hf, *b.chf)) {
		return TILE_FAILED;
	}

	rcFreeHeightField(b.hf);
	b.hf = nullptr;

	if (b.chf->spanCount == 0) {
		return TILE_EMPTY;
	}

	// The mesh is built for a crouching simulant; where there is less room
	// than a standing one needs, it goes only over the floors that have it
	// duck or crouch, as far as it gets down
	for (int i = 0; i < b.chf->spanCount; i++) {
		const int room = b.chf->spans[i].h;

		if (b.chf->areas[i] == RC_NULL_AREA || room >= standing) {
			continue;
		}

		if (b.chf->areas[i] == SIMNAV_AREA_CROUCH || (b.chf->areas[i] == SIMNAV_AREA_DUCK && room >= ducked)) {
			continue;
		}

		b.chf->areas[i] = RC_NULL_AREA;
	}

	if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *b.chf)
			|| !rcBuildDistanceField(&ctx, *b.chf)
			|| !rcBuildRegions(&ctx, *b.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) {
		return TILE_FAILED;
	}

	b.cset = rcAllocContourSet();

	if (!b.cset || !rcBuildContours(&ctx, *b.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *b.cset)) {
		return TILE_FAILED;
	}

	if (b.cset->nconts == 0) {
		return TILE_EMPTY;
	}

	b.pmesh = rcAllocPolyMesh();

	if (!b.pmesh || !rcBuildPolyMesh(&ctx, *b.cset, cfg.maxVertsPerPoly, *b.pmesh)) {
		return TILE_FAILED;
	}

	b.dmesh = rcAllocPolyMeshDetail();

	if (!b.dmesh || !rcBuildPolyMeshDetail(&ctx, *b.pmesh, *b.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *b.dmesh)) {
		return TILE_FAILED;
	}

	if (b.pmesh->npolys == 0) {
		return TILE_EMPTY;
	}

	if (b.pmesh->nverts >= 0xffff) {
		ctx.log(RC_LOG_ERROR, "tile %d,%d: %d vertices is more than a tile holds", tx, ty, b.pmesh->nverts);
		return TILE_FAILED;
	}

	// every area but none can be walked; M2 gives the others their costs
	for (int i = 0; i < b.pmesh->npolys; i++) {
		b.pmesh->flags[i] = b.pmesh->areas[i] != RC_NULL_AREA ? 1 : 0;
	}

	dtNavMeshCreateParams p;
	memset(&p, 0, sizeof(p));
	p.verts = b.pmesh->verts;
	p.vertCount = b.pmesh->nverts;
	p.polys = b.pmesh->polys;
	p.polyAreas = b.pmesh->areas;
	p.polyFlags = b.pmesh->flags;
	p.polyCount = b.pmesh->npolys;
	p.nvp = b.pmesh->nvp;
	p.detailMeshes = b.dmesh->meshes;
	p.detailVerts = b.dmesh->verts;
	p.detailVertsCount = b.dmesh->nverts;
	p.detailTris = b.dmesh->tris;
	p.detailTriCount = b.dmesh->ntris;
	p.walkableHeight = cfg.walkableHeight * cfg.ch;
	p.walkableRadius = cfg.walkableRadius * cfg.cs;
	p.walkableClimb = cfg.walkableClimb * cfg.ch;
	p.tileX = tx;
	p.tileY = ty;
	p.tileLayer = 0;
	rcVcopy(p.bmin, b.pmesh->bmin);
	rcVcopy(p.bmax, b.pmesh->bmax);
	p.cs = cfg.cs;
	p.ch = cfg.ch;
	p.buildBvTree = true;

	if (!dtCreateNavMeshData(&p, outdata, outsize)) {
		ctx.log(RC_LOG_ERROR, "tile %d,%d: dtCreateNavMeshData failed", tx, ty);
		return TILE_FAILED;
	}

	return TILE_OK;
}

void countTiles(const dtNavMesh *nav, simnavstats *stats)
{
	const float buildms = stats->buildms;
	const int emptytiles = stats->emptytiles;
	const int failedtiles = stats->failedtiles;

	memset(stats, 0, sizeof(*stats));
	stats->buildms = buildms;
	stats->emptytiles = emptytiles;
	stats->failedtiles = failedtiles;

	for (int i = 0; i < nav->getMaxTiles(); i++) {
		const dtMeshTile *tile = nav->getTile(i);

		if (!tile || !tile->header || !tile->dataSize) {
			continue;
		}

		stats->tiles++;
		stats->polys += tile->header->polyCount;
		stats->verts += tile->header->vertCount;
		stats->detailtris += tile->header->detailTriCount;
	}
}

int nextPow2(int v)
{
	int p = 1;

	while (p < v) {
		p <<= 1;
	}

	return p;
}

} // namespace

extern "C" struct simnavmesh *simnavMeshBuild(const struct simnavinput *in, const struct simnavparams *sp,
		struct simnavstats *outstats, char *err, size_t errlen)
{
	const auto start = std::chrono::steady_clock::now();
	SimNavContext ctx(err, errlen);
	float bmin[3], bmax[3];
	rcConfig cfg;
	simnavstats stats;
	int gw, gh;

	memset(&stats, 0, sizeof(stats));

	if (!in || in->numverts <= 0 || in->numtris <= 0) {
		if (err && errlen) {
			snprintf(err, errlen, "no geometry");
		}
		return nullptr;
	}

	rcCalcBounds(in->verts, in->numverts, bmin, bmax);

	memset(&cfg, 0, sizeof(cfg));
	cfg.cs = sp->cellsize;
	cfg.ch = sp->cellheight;
	cfg.walkableSlopeAngle = sp->agentslope;
	cfg.walkableHeight = (int)ceilf(sp->agentcrouchheight / cfg.ch);
	cfg.walkableClimb = (int)floorf(sp->agentclimb / cfg.ch);
	cfg.walkableRadius = (int)ceilf(sp->agentradius / cfg.cs);
	cfg.maxEdgeLen = (int)(sp->maxedgelen / cfg.cs);
	cfg.maxSimplificationError = sp->maxsimplificationerror;
	cfg.minRegionArea = sp->minregionsize * sp->minregionsize;
	cfg.mergeRegionArea = sp->mergeregionsize * sp->mergeregionsize;
	cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
	cfg.tileSize = sp->tilesize;
	cfg.borderSize = cfg.walkableRadius + 3;
	cfg.width = cfg.tileSize + cfg.borderSize * 2;
	cfg.height = cfg.tileSize + cfg.borderSize * 2;
	cfg.detailSampleDist = sp->detailsampledist < 0.9f ? 0 : cfg.cs * sp->detailsampledist;
	cfg.detailSampleMaxError = cfg.ch * sp->detailsamplemaxerror;
	rcVcopy(cfg.bmin, bmin);
	rcVcopy(cfg.bmax, bmax);

	rcCalcGridSize(bmin, bmax, cfg.cs, &gw, &gh);

	const int tw = (gw + cfg.tileSize - 1) / cfg.tileSize;
	const int th = (gh + cfg.tileSize - 1) / cfg.tileSize;

	dtNavMeshParams np;
	memset(&np, 0, sizeof(np));
	rcVcopy(np.orig, bmin);
	np.tileWidth = cfg.tileSize * cfg.cs;
	np.tileHeight = cfg.tileSize * cfg.cs;
	np.maxTiles = nextPow2(tw * th);
	np.maxPolys = 1 << DT_POLY_BITS;

	simnavmesh *mesh = new (std::nothrow) simnavmesh();

	if (!mesh) {
		return nullptr;
	}

	mesh->nav = dtAllocNavMesh();

	if (!mesh->nav || dtStatusFailed(mesh->nav->init(&np))) {
		if (err && errlen) {
			snprintf(err, errlen, "dtNavMesh::init failed (%dx%d tiles)", tw, th);
		}
		simnavMeshFree(mesh);
		return nullptr;
	}

	// each triangle's xz bounds, so a tile takes only its own
	float *tribounds = (float *)malloc(sizeof(float) * 4 * in->numtris);
	int *scratchtris = (int *)malloc(sizeof(int) * 3 * in->numtris);
	unsigned char *scratchareas = (unsigned char *)malloc(in->numtris);

	if (!tribounds || !scratchtris || !scratchareas) {
		free(tribounds);
		free(scratchtris);
		free(scratchareas);
		simnavMeshFree(mesh);
		return nullptr;
	}

	for (int i = 0; i < in->numtris; i++) {
		const float *a = &in->verts[in->tris[i * 3 + 0] * 3];
		const float *b = &in->verts[in->tris[i * 3 + 1] * 3];
		const float *c = &in->verts[in->tris[i * 3 + 2] * 3];
		float *tb = &tribounds[i * 4];

		tb[0] = rcMin(a[0], rcMin(b[0], c[0]));
		tb[1] = rcMin(a[2], rcMin(b[2], c[2]));
		tb[2] = rcMax(a[0], rcMax(b[0], c[0]));
		tb[3] = rcMax(a[2], rcMax(b[2], c[2]));
	}

	for (int ty = 0; ty < th; ty++) {
		for (int tx = 0; tx < tw; tx++) {
			unsigned char *data = nullptr;
			int size = 0;
			TileResult r = buildTile(ctx, cfg, sp, in, tribounds, tx, ty, bmin, bmax,
					scratchtris, scratchareas, &data, &size);

			if (r == TILE_EMPTY) {
				stats.emptytiles++;
			} else if (r == TILE_FAILED) {
				stats.failedtiles++;
			} else if (dtStatusFailed(mesh->nav->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr))) {
				dtFree(data);
				stats.failedtiles++;
			}
		}
	}

	free(tribounds);
	free(scratchtris);
	free(scratchareas);

	stats.buildms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	countTiles(mesh->nav, &stats);
	mesh->stats = stats;

	if (stats.polys == 0) {
		if (err && errlen && !err[0]) {
			snprintf(err, errlen, "no walkable polygons (%d empty tiles, %d failed)", stats.emptytiles, stats.failedtiles);
		}
		simnavMeshFree(mesh);
		return nullptr;
	}

	if (outstats) {
		*outstats = stats;
	}

	return mesh;
}

extern "C" void simnavMeshFree(struct simnavmesh *mesh)
{
	if (mesh) {
		dtFreeNavMesh(mesh->nav);
		delete mesh;
	}
}

extern "C" void simnavMeshGetStats(const struct simnavmesh *mesh, struct simnavstats *stats)
{
	if (mesh) {
		*stats = mesh->stats;
	} else {
		memset(stats, 0, sizeof(*stats));
	}
}

extern "C" int simnavMeshNumTiles(const struct simnavmesh *mesh)
{
	return mesh ? mesh->nav->getMaxTiles() : 0;
}

// The saved form, after the caller's own header: RecastDemo's "MSET" layout
// (the params, then each tile's size and bytes), host byte order. A cache,
// not an interchange format.
#define SIMNAV_SAVE_MAGIC   0x534e4156 // 'SNAV'
#define SIMNAV_SAVE_VERSION 1

namespace {

struct SaveHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t numtiles;
	uint32_t pad;
	dtNavMeshParams params;
	float buildms;
	int32_t emptytiles;
	int32_t failedtiles;
	int32_t pad2;
};

struct SaveTile {
	uint64_t ref;
	int32_t size;
	int32_t pad;
};

} // namespace

extern "C" int simnavMeshSave(const struct simnavmesh *mesh, uint8_t **outbuf, size_t *outlen)
{
	const dtNavMesh *nav = mesh->nav;
	size_t len = sizeof(SaveHeader);
	uint32_t numtiles = 0;

	for (int i = 0; i < nav->getMaxTiles(); i++) {
		const dtMeshTile *tile = nav->getTile(i);

		if (tile && tile->header && tile->dataSize) {
			len += sizeof(SaveTile) + tile->dataSize;
			numtiles++;
		}
	}

	uint8_t *buf = (uint8_t *)malloc(len);

	if (!buf) {
		return 0;
	}

	SaveHeader h;
	memset(&h, 0, sizeof(h));
	h.magic = SIMNAV_SAVE_MAGIC;
	h.version = SIMNAV_SAVE_VERSION;
	h.numtiles = numtiles;
	memcpy(&h.params, nav->getParams(), sizeof(dtNavMeshParams));
	h.buildms = mesh->stats.buildms;
	h.emptytiles = mesh->stats.emptytiles;
	h.failedtiles = mesh->stats.failedtiles;
	memcpy(buf, &h, sizeof(h));

	size_t off = sizeof(h);

	for (int i = 0; i < nav->getMaxTiles(); i++) {
		const dtMeshTile *tile = nav->getTile(i);

		if (!tile || !tile->header || !tile->dataSize) {
			continue;
		}

		SaveTile t;
		memset(&t, 0, sizeof(t));
		t.ref = nav->getTileRef(tile);
		t.size = tile->dataSize;
		memcpy(buf + off, &t, sizeof(t));
		off += sizeof(t);
		memcpy(buf + off, tile->data, tile->dataSize);
		off += tile->dataSize;
	}

	*outbuf = buf;
	*outlen = len;

	return 1;
}

extern "C" struct simnavmesh *simnavMeshLoad(const uint8_t *buf, size_t len)
{
	SaveHeader h;

	if (len < sizeof(h)) {
		return nullptr;
	}

	memcpy(&h, buf, sizeof(h));

	if (h.magic != SIMNAV_SAVE_MAGIC || h.version != SIMNAV_SAVE_VERSION) {
		return nullptr;
	}

	simnavmesh *mesh = new (std::nothrow) simnavmesh();

	if (!mesh) {
		return nullptr;
	}

	mesh->nav = dtAllocNavMesh();

	if (!mesh->nav || dtStatusFailed(mesh->nav->init(&h.params))) {
		simnavMeshFree(mesh);
		return nullptr;
	}

	size_t off = sizeof(h);

	for (uint32_t i = 0; i < h.numtiles; i++) {
		SaveTile t;

		if (off + sizeof(t) > len) {
			simnavMeshFree(mesh);
			return nullptr;
		}

		memcpy(&t, buf + off, sizeof(t));
		off += sizeof(t);

		if (t.size <= 0 || off + (size_t)t.size > len) {
			simnavMeshFree(mesh);
			return nullptr;
		}

		unsigned char *data = (unsigned char *)dtAlloc(t.size, DT_ALLOC_PERM);

		if (!data) {
			simnavMeshFree(mesh);
			return nullptr;
		}

		memcpy(data, buf + off, t.size);
		off += t.size;

		if (dtStatusFailed(mesh->nav->addTile(data, t.size, DT_TILE_FREE_DATA, (dtTileRef)t.ref, nullptr))) {
			dtFree(data);
			simnavMeshFree(mesh);
			return nullptr;
		}
	}

	mesh->stats.buildms = h.buildms;
	mesh->stats.emptytiles = h.emptytiles;
	mesh->stats.failedtiles = h.failedtiles;
	countTiles(mesh->nav, &mesh->stats);

	return mesh;
}

extern "C" int simnavMeshForEachTri(const struct simnavmesh *mesh, simnavtrifn fn, void *arg)
{
	const dtNavMesh *nav = mesh->nav;
	int count = 0;
	int polynum = 0;

	for (int i = 0; i < nav->getMaxTiles(); i++) {
		const dtMeshTile *tile = nav->getTile(i);

		if (!tile || !tile->header || !tile->dataSize) {
			continue;
		}

		for (int j = 0; j < tile->header->polyCount; j++) {
			const dtPoly *poly = &tile->polys[j];

			if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
				continue;
			}

			const dtPolyDetail *pd = &tile->detailMeshes[j];

			for (int k = 0; k < pd->triCount; k++) {
				const unsigned char *t = &tile->detailTris[(pd->triBase + k) * 4];
				const float *v[3];

				for (int m = 0; m < 3; m++) {
					if (t[m] < poly->vertCount) {
						v[m] = &tile->verts[poly->verts[t[m]] * 3];
					} else {
						v[m] = &tile->detailVerts[(pd->vertBase + (t[m] - poly->vertCount)) * 3];
					}
				}

				fn(arg, i, polynum, poly->getArea(), v[0], v[1], v[2]);
				count++;
			}

			polynum++;
		}
	}

	return count;
}
