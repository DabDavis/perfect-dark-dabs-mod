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
#include <vector>
#include <algorithm>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourAlloc.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"

#include "simnav.h"
#include "simnavlinks.h"

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

// A tile's polygons, kept between the build without links and the one with
struct KeptTile {
	int tx = 0, ty = 0;
	rcPolyMesh *pmesh = nullptr;
	rcPolyMeshDetail *dmesh = nullptr;
};

TileResult buildTile(SimNavContext &ctx, const rcConfig &base, const simnavparams *sp, const simnavinput *in,
		const float *tribounds, int tx, int ty, const float *meshbmin, const float *meshbmax,
		int *scratchtris, unsigned char *scratchareas, KeptTile *out)
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

	// every area but none can be walked
	for (int i = 0; i < b.pmesh->npolys; i++) {
		b.pmesh->flags[i] = b.pmesh->areas[i] != RC_NULL_AREA ? SIMNAV_FLAG_WALK : 0;
	}

	out->tx = tx;
	out->ty = ty;
	out->pmesh = b.pmesh;
	out->dmesh = b.dmesh;
	b.pmesh = nullptr;
	b.dmesh = nullptr;

	return TILE_OK;
}

/** A kept tile's Detour data, with the links that start in it */
bool makeTileData(SimNavContext &ctx, const rcConfig &cfg, const KeptTile &t, const std::vector<SimNavLink> &links,
		unsigned char **outdata, int *outsize)
{
	std::vector<float> converts, conrad;
	std::vector<unsigned short> conflags;
	std::vector<unsigned char> conareas, condir;
	std::vector<unsigned int> conids;

	for (size_t i = 0; i < links.size(); i++) {
		const SimNavLink &l = links[i];

		converts.insert(converts.end(), l.a, l.a + 3);
		converts.insert(converts.end(), l.b, l.b + 3);
		conrad.push_back(l.rad);
		conflags.push_back(l.flags);
		conareas.push_back(l.area);
		condir.push_back(l.bidir ? DT_OFFMESH_CON_BIDIR : 0);
		conids.push_back((unsigned int)i);
	}

	dtNavMeshCreateParams p;
	memset(&p, 0, sizeof(p));
	p.verts = t.pmesh->verts;
	p.vertCount = t.pmesh->nverts;
	p.polys = t.pmesh->polys;
	p.polyAreas = t.pmesh->areas;
	p.polyFlags = t.pmesh->flags;
	p.polyCount = t.pmesh->npolys;
	p.nvp = t.pmesh->nvp;
	p.detailMeshes = t.dmesh->meshes;
	p.detailVerts = t.dmesh->verts;
	p.detailVertsCount = t.dmesh->nverts;
	p.detailTris = t.dmesh->tris;
	p.detailTriCount = t.dmesh->ntris;
	p.offMeshConVerts = converts.empty() ? nullptr : converts.data();
	p.offMeshConRad = conrad.empty() ? nullptr : conrad.data();
	p.offMeshConFlags = conflags.empty() ? nullptr : conflags.data();
	p.offMeshConAreas = conareas.empty() ? nullptr : conareas.data();
	p.offMeshConDir = condir.empty() ? nullptr : condir.data();
	p.offMeshConUserID = conids.empty() ? nullptr : conids.data();
	p.offMeshConCount = (int)links.size();
	p.walkableHeight = cfg.walkableHeight * cfg.ch;
	p.walkableRadius = cfg.walkableRadius * cfg.cs;
	p.walkableClimb = cfg.walkableClimb * cfg.ch;
	p.tileX = t.tx;
	p.tileY = t.ty;
	p.tileLayer = 0;
	rcVcopy(p.bmin, t.pmesh->bmin);
	rcVcopy(p.bmax, t.pmesh->bmax);
	p.cs = cfg.cs;
	p.ch = cfg.ch;
	p.buildBvTree = true;

	if (!dtCreateNavMeshData(&p, outdata, outsize)) {
		ctx.log(RC_LOG_ERROR, "tile %d,%d: dtCreateNavMeshData failed", t.tx, t.ty);
		return false;
	}

	return true;
}

void countTiles(const dtNavMesh *nav, simnavstats *stats)
{
	const float buildms = stats->buildms;
	const float linkms = stats->linkms;
	const int emptytiles = stats->emptytiles;
	const int failedtiles = stats->failedtiles;

	memset(stats, 0, sizeof(*stats));
	stats->buildms = buildms;
	stats->linkms = linkms;
	stats->emptytiles = emptytiles;
	stats->failedtiles = failedtiles;

	for (int i = 0; i < nav->getMaxTiles(); i++) {
		const dtMeshTile *tile = nav->getTile(i);

		if (!tile || !tile->header || !tile->dataSize) {
			continue;
		}

		stats->tiles++;
		stats->polys += tile->header->polyCount - tile->header->offMeshConCount;
		stats->verts += tile->header->vertCount - tile->header->offMeshConCount * 2;
		stats->detailtris += tile->header->detailTriCount;

		for (int j = 0; j < tile->header->offMeshConCount; j++) {
			const dtOffMeshConnection *con = &tile->offMeshCons[j];
			const dtPoly *poly = &tile->polys[con->poly];

			switch (poly->getArea()) {
			case SIMNAV_AREA_LADDER: stats->ladderlinks++; break;
			case SIMNAV_AREA_LIFT: stats->liftlinks++; break;
			case SIMNAV_AREA_DROP: stats->droplinks++; break;
			case SIMNAV_AREA_JUMP:
				stats->jumplinks++;

				for (int h = 0; h < SIMNAV_MAXJUMPHEIGHTS; h++) {
					if (poly->flags & SIMNAV_FLAG_JUMP(h + 1)) {
						stats->jumpsbyheight[h]++;
					}
				}
				break;
			}
		}
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
		const struct simnavlinkparams *lp, struct simnavstats *outstats, char *err, size_t errlen)
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

	std::vector<KeptTile> kept;

	for (int ty = 0; ty < th; ty++) {
		for (int tx = 0; tx < tw; tx++) {
			KeptTile t;
			TileResult r = buildTile(ctx, cfg, sp, in, tribounds, tx, ty, bmin, bmax, scratchtris, scratchareas, &t);

			if (r == TILE_EMPTY) {
				stats.emptytiles++;
			} else if (r == TILE_FAILED) {
				stats.failedtiles++;
			} else {
				kept.push_back(t);
			}
		}
	}

	free(tribounds);
	free(scratchtris);
	free(scratchareas);

	// The mesh without links, then the links found on it, then the mesh
	// again with each link in the tile its start is in
	std::vector<SimNavLink> links;
	std::vector<std::vector<SimNavLink>> tilelinks(kept.size());

	if (lp) {
		dtNavMesh *bare = dtAllocNavMesh();

		if (bare && dtStatusSucceed(bare->init(&np))) {
			for (const KeptTile &t : kept) {
				unsigned char *data = nullptr;
				int size = 0;

				if (makeTileData(ctx, cfg, t, std::vector<SimNavLink>(), &data, &size)
						&& dtStatusFailed(bare->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr))) {
					dtFree(data);
				}
			}

			const auto linkstart = std::chrono::steady_clock::now();
			simnavGenerateLinks(bare, in, sp, lp, links);
			stats.linkms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - linkstart).count();
		}

		dtFreeNavMesh(bare);

		for (const SimNavLink &l : links) {
			const int tx = (int)floorf((l.a[0] - np.orig[0]) / np.tileWidth);
			const int ty = (int)floorf((l.a[2] - np.orig[2]) / np.tileHeight);

			for (size_t i = 0; i < kept.size(); i++) {
				if (kept[i].tx == tx && kept[i].ty == ty) {
					tilelinks[i].push_back(l);
					break;
				}
			}
		}
	}

	for (size_t i = 0; i < kept.size(); i++) {
		unsigned char *data = nullptr;
		int size = 0;

		if (!makeTileData(ctx, cfg, kept[i], tilelinks[i], &data, &size)) {
			stats.failedtiles++;
		} else if (dtStatusFailed(mesh->nav->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr))) {
			dtFree(data);
			stats.failedtiles++;
		}

		rcFreePolyMesh(kept[i].pmesh);
		rcFreePolyMeshDetail(kept[i].dmesh);
	}

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
#define SIMNAV_SAVE_VERSION 2

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
	float linkms;
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
	h.linkms = mesh->stats.linkms;
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
	mesh->stats.linkms = h.linkms;
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

extern "C" int simnavMeshForEachLink(const struct simnavmesh *mesh, simnavlinkfn fn, void *arg)
{
	const dtNavMesh *nav = mesh->nav;
	int count = 0;

	for (int i = 0; i < nav->getMaxTiles(); i++) {
		const dtMeshTile *tile = nav->getTile(i);

		if (!tile || !tile->header || !tile->dataSize) {
			continue;
		}

		for (int j = 0; j < tile->header->offMeshConCount; j++) {
			const dtOffMeshConnection *con = &tile->offMeshCons[j];
			const dtPoly *poly = &tile->polys[con->poly];

			fn(arg, poly->getArea(), poly->flags, (con->flags & DT_OFFMESH_CON_BIDIR) != 0, &con->pos[0], &con->pos[3]);
			count++;
		}
	}

	return count;
}

/*
 * Queries
 */

struct simnavquery {
	const dtNavMesh *nav;
	dtNavMeshQuery *q;
	dtPolyRef path[512];
};

extern "C" struct simnavquery *simnavQueryCreate(const struct simnavmesh *mesh)
{
	simnavquery *query = new (std::nothrow) simnavquery();

	if (!query) {
		return nullptr;
	}

	query->nav = mesh->nav;
	query->q = dtAllocNavMeshQuery();

	if (!query->q || dtStatusFailed(query->q->init(mesh->nav, 4096))) {
		simnavQueryFree(query);
		return nullptr;
	}

	return query;
}

extern "C" void simnavQueryFree(struct simnavquery *query)
{
	if (query) {
		dtFreeNavMeshQuery(query->q);
		delete query;
	}
}

extern "C" int simnavQueryFindFloor(struct simnavquery *query, const float *pos, float below, float above,
		float radius, float *out)
{
	dtQueryFilter filter;
	dtPolyRef ref;

	filter.setIncludeFlags(0xffff);

	return simnavFindFloorPoly(query->q, &filter, pos, below, above, radius, &ref, out) ? 1 : 0;
}

extern "C" int simnavQueryPath(struct simnavquery *query, const float *start, const float *end, unsigned short include,
		float *points, unsigned char *linkareas, int maxpoints, int *complete)
{
	dtQueryFilter filter;
	dtPolyRef sref, eref;
	float s[3], e[3];
	int npath = 0;
	int n = 0;

	*complete = 0;
	filter.setIncludeFlags(include);

	// a chr's position stands over its floor, a player's at eye height
	if (maxpoints <= 0
			|| !simnavFindFloorPoly(query->q, &filter, start, 250.0f, 60.0f, 60.0f, &sref, s)
			|| !simnavFindFloorPoly(query->q, &filter, end, 250.0f, 60.0f, 60.0f, &eref, e)) {
		return 0;
	}

	if (dtStatusFailed(query->q->findPath(sref, eref, s, e, &filter, query->path, &npath, 512)) || npath == 0) {
		return 0;
	}

	*complete = query->path[npath - 1] == eref;

	if (!*complete) {
		// as near as it gets
		float near[3];
		bool over;

		if (dtStatusSucceed(query->q->closestPointOnPoly(query->path[npath - 1], e, near, &over))) {
			dtVcopy(e, near);
		}
	}

	std::vector<unsigned char> flags(maxpoints);
	std::vector<dtPolyRef> refs(maxpoints);

	if (dtStatusFailed(query->q->findStraightPath(s, e, query->path, npath, points, flags.data(), refs.data(), &n, maxpoints))) {
		return 0;
	}

	for (int i = 0; i < n; i++) {
		linkareas[i] = SIMNAV_AREA_NONE;

		if ((flags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) && refs[i]) {
			unsigned char area = 0;

			if (dtStatusSucceed(query->nav->getPolyArea(refs[i], &area))) {
				linkareas[i] = area;
			}
		}
	}

	return n;
}

extern "C" void simnavMeshReachability(const struct simnavmesh *mesh, const float *points, int n,
		float below, float radius, unsigned short include, int *onmesh, int *mutualpairs, int *groups)
{
	const dtNavMesh *nav = mesh->nav;
	simnavquery *query = simnavQueryCreate(mesh);
	dtQueryFilter filter;

	*onmesh = 0;
	*mutualpairs = 0;

	if (!query || n <= 0) {
		simnavQueryFree(query);
		return;
	}

	filter.setIncludeFlags(include);

	// a dense index for every polygon, links included
	std::vector<int> tilebase(nav->getMaxTiles() + 1, 0);

	for (int i = 0; i < nav->getMaxTiles(); i++) {
		const dtMeshTile *tile = nav->getTile(i);
		tilebase[i + 1] = tilebase[i] + (tile && tile->header ? tile->header->polyCount : 0);
	}

	const int numpolys = tilebase.back();
	auto indexOf = [&](dtPolyRef ref) -> int {
		const dtMeshTile *tile;
		const dtPoly *poly;
		nav->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
		return tilebase[nav->decodePolyIdTile(ref)] + (int)(poly - tile->polys);
	};

	// the pads a floor search may take: the walkable polygons whatever the
	// include mask
	dtQueryFilter floorfilter;
	floorfilter.setIncludeFlags(SIMNAV_FLAG_WALK);

	std::vector<dtPolyRef> start(n, 0);
	std::vector<std::vector<unsigned char>> reach(n);

	for (int i = 0; i < n; i++) {
		float out[3];

		if (simnavFindFloorPoly(query->q, &floorfilter, &points[i * 3], below, 60.0f, radius, &start[i], out)) {
			(*onmesh)++;
		} else {
			start[i] = 0;
		}
	}

	// everything each point reaches, by flood
	for (int i = 0; i < n; i++) {
		if (!start[i]) {
			continue;
		}

		std::vector<unsigned char> &seen = reach[i];
		std::vector<dtPolyRef> stack;

		seen.assign(numpolys, 0);
		stack.push_back(start[i]);
		seen[indexOf(start[i])] = 1;

		while (!stack.empty()) {
			const dtPolyRef ref = stack.back();
			const dtMeshTile *tile;
			const dtPoly *poly;

			stack.pop_back();
			nav->getTileAndPolyByRefUnsafe(ref, &tile, &poly);

			for (unsigned int k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next) {
				const dtPolyRef nref = tile->links[k].ref;
				const dtMeshTile *ntile;
				const dtPoly *npoly;

				if (!nref) {
					continue;
				}

				nav->getTileAndPolyByRefUnsafe(nref, &ntile, &npoly);

				if (!(npoly->flags & include)) {
					continue;
				}

				const int idx = indexOf(nref);

				if (!seen[idx]) {
					seen[idx] = 1;
					stack.push_back(nref);
				}
			}
		}
	}

	if (groups) {
		for (int i = 0; i < n; i++) {
			groups[i] = start[i] ? i : -1;
		}
	}

	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			if (start[i] && start[j] && reach[i][indexOf(start[j])] && reach[j][indexOf(start[i])]) {
				(*mutualpairs)++;

				if (groups && groups[j] == j) {
					groups[j] = groups[i];
				}
			}
		}
	}

	simnavQueryFree(query);
}
