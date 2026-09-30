/**
 * The navmesh's off-mesh links (PLANS/AI-REWORK.md M2): everything a simulant
 * gets about by that is not walking. No game code here; the chr's numbers
 * come in simnavlinkparams from port/src/simnavstage.c, which says where each
 * is from.
 *
 * - **Ladders**: a GEOFLAG_LADDER face, found by the stage side, joins the
 *   floor at its foot to the floor at its head, both ways. A chr on a ladder
 *   climbs whichever way it walks; it comes down one by walking off its head.
 * - **Lifts**: a lift's car is a prop, so the mesh stops round its shaft;
 *   each side it is left by at a stop is joined to each at the next stop,
 *   and to the other sides at its own, both ways.
 * - **Drops**: from a sample every samplespacing along each open edge of the
 *   mesh, a walker steps out at the slowest simulant's speed and is followed
 *   tick by tick: its box against the walls, gravity as chr0f01f378()
 *   integrates it, until it lands. A landing within maxdrop, on a floor that
 *   does not kill and that the mesh covers, is a one-way link. None down a
 *   lift's shaft, which the car fills as often as not.
 * - **Jumps**: from the same samples, the jump at each Jump Height setting in
 *   turn, with the box a jumping simulant is given (chrGetBbox()). The lowest
 *   setting that lands where walking off does not - over a gap into the
 *   void or onto a killing floor, or higher than the drop's floor - is the
 *   link, flagged with that setting.
 *
 * A link the mesh can already walk at not much more than its length is
 * dropped, and links of a kind close together at both ends are one.
 */

#include <float.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include "simnavlinks.h"

namespace {

/*
 * The input triangles in an xz grid, for the floor under a point and for a
 * segment's hits
 */
class Geom {
public:
	explicit Geom(const simnavinput *input) : in(input)
	{
		float minx = FLT_MAX, minz = FLT_MAX, maxx = -FLT_MAX, maxz = -FLT_MAX;

		for (int i = 0; i < in->numverts; i++) {
			minx = std::min(minx, in->verts[i * 3 + 0]);
			maxx = std::max(maxx, in->verts[i * 3 + 0]);
			minz = std::min(minz, in->verts[i * 3 + 2]);
			maxz = std::max(maxz, in->verts[i * 3 + 2]);
		}

		ox = minx;
		oz = minz;
		cell = std::max(64.0f, std::max(maxx - minx, maxz - minz) / 512.0f);
		w = (int)((maxx - minx) / cell) + 1;
		h = (int)((maxz - minz) / cell) + 1;

		start.assign((size_t)w * h + 1, 0);

		for (int pass = 0; pass < 2; pass++) {
			std::vector<int> fill;

			if (pass == 1) {
				for (size_t c = 1; c < start.size(); c++) {
					start[c] += start[c - 1];
				}

				items.resize(start.back());
				fill.assign(start.begin(), start.end() - 1);
			}

			for (int t = 0; t < in->numtris; t++) {
				if (in->kinds[t] == 0) {
					continue;
				}

				float bmin[3], bmax[3];
				triBounds(t, bmin, bmax);

				int x0, z0, x1, z1;
				cellRange(bmin[0], bmin[2], bmax[0], bmax[2], x0, z0, x1, z1);

				for (int cz = z0; cz <= z1; cz++) {
					for (int cx = x0; cx <= x1; cx++) {
						const int c = cz * w + cx;

						if (pass == 0) {
							start[c + 1]++;
						} else {
							items[fill[c]++] = t;
						}
					}
				}
			}
		}

		stamp.assign(in->numtris, 0);
		cur = 0;
	}

	/** The highest floor at (x, z) no higher than ymax, or -FLT_MAX */
	float floorAt(float x, float z, float ymax, unsigned char *kind) const
	{
		int cx = (int)((x - ox) / cell);
		int cz = (int)((z - oz) / cell);
		float best = -FLT_MAX;

		if (cx < 0 || cz < 0 || cx >= w || cz >= h) {
			return best;
		}

		const int c = cz * w + cx;

		for (int i = start[c]; i < start[c + 1]; i++) {
			const int t = items[i];

			if (!(in->kinds[t] & SIMNAV_KIND_FLOOR)) {
				continue;
			}

			const float *a = &in->verts[in->tris[t * 3 + 0] * 3];
			const float *b = &in->verts[in->tris[t * 3 + 1] * 3];
			const float *d = &in->verts[in->tris[t * 3 + 2] * 3];
			const float v0x = b[0] - a[0], v0z = b[2] - a[2];
			const float v1x = d[0] - a[0], v1z = d[2] - a[2];
			const float den = v0x * v1z - v1x * v0z;

			if (fabsf(den) < 1e-3f) {
				continue; // standing on its edge
			}

			const float px = x - a[0], pz = z - a[2];
			const float u = (px * v1z - v1x * pz) / den;
			const float v = (v0x * pz - px * v0z) / den;

			if (u < -1e-4f || v < -1e-4f || u + v > 1.0001f) {
				continue;
			}

			const float y = a[1] + u * (b[1] - a[1]) + v * (d[1] - a[1]);

			if (y <= ymax && y > best) {
				best = y;

				if (kind) {
					*kind = in->kinds[t];
				}
			}
		}

		return best;
	}

	/** The highest floor under a disc, as a chr's cylinder finds its ground */
	float floorUnder(float x, float z, float r, float ymax, unsigned char *kind) const
	{
		static const float offs[5][2] = { { 0, 0 }, { 0.7f, 0 }, { -0.7f, 0 }, { 0, 0.7f }, { 0, -0.7f } };
		float best = -FLT_MAX;

		for (int i = 0; i < 5; i++) {
			unsigned char k = 0;
			const float y = floorAt(x + offs[i][0] * r, z + offs[i][1] * r, ymax, &k);

			if (y > best) {
				best = y;

				if (kind) {
					*kind = k;
				}
			}
		}

		return best;
	}

	/** Whether a segment passes through a triangle of one of the kinds */
	bool segHits(const float *p, const float *q, unsigned char kindmask) const
	{
		int x0, z0, x1, z1;
		cellRange(std::min(p[0], q[0]), std::min(p[2], q[2]), std::max(p[0], q[0]), std::max(p[2], q[2]), x0, z0, x1, z1);

		if (++cur == 0) {
			std::fill(stamp.begin(), stamp.end(), 0);
			cur = 1;
		}

		const float dir[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };

		for (int cz = z0; cz <= z1; cz++) {
			for (int cx = x0; cx <= x1; cx++) {
				const int c = cz * w + cx;

				for (int i = start[c]; i < start[c + 1]; i++) {
					const int t = items[i];

					if (stamp[t] == cur) {
						continue;
					}

					stamp[t] = cur;

					if (!(in->kinds[t] & kindmask)) {
						continue;
					}

					if (segTri(p, dir, t)) {
						return true;
					}
				}
			}
		}

		return false;
	}

private:
	const simnavinput *in;
	float ox, oz, cell;
	int w, h;
	std::vector<int> start;
	std::vector<int> items;
	mutable std::vector<unsigned> stamp;
	mutable unsigned cur;

	void triBounds(int t, float *bmin, float *bmax) const
	{
		for (int k = 0; k < 3; k++) {
			bmin[k] = FLT_MAX;
			bmax[k] = -FLT_MAX;
		}

		for (int v = 0; v < 3; v++) {
			const float *p = &in->verts[in->tris[t * 3 + v] * 3];

			for (int k = 0; k < 3; k++) {
				bmin[k] = std::min(bmin[k], p[k]);
				bmax[k] = std::max(bmax[k], p[k]);
			}
		}
	}

	void cellRange(float minx, float minz, float maxx, float maxz, int &x0, int &z0, int &x1, int &z1) const
	{
		x0 = std::max(0, std::min(w - 1, (int)((minx - ox) / cell)));
		x1 = std::max(0, std::min(w - 1, (int)((maxx - ox) / cell)));
		z0 = std::max(0, std::min(h - 1, (int)((minz - oz) / cell)));
		z1 = std::max(0, std::min(h - 1, (int)((maxz - oz) / cell)));
	}

	/** Moller-Trumbore, for 0 <= t <= 1 along p + dir */
	bool segTri(const float *p, const float *dir, int t) const
	{
		const float *a = &in->verts[in->tris[t * 3 + 0] * 3];
		const float *b = &in->verts[in->tris[t * 3 + 1] * 3];
		const float *c = &in->verts[in->tris[t * 3 + 2] * 3];
		float e1[3], e2[3], pv[3], tv[3], qv[3];

		dtVsub(e1, b, a);
		dtVsub(e2, c, a);
		dtVcross(pv, dir, e2);

		const float det = dtVdot(e1, pv);

		if (fabsf(det) < 1e-6f) {
			return false;
		}

		const float inv = 1.0f / det;
		dtVsub(tv, p, a);

		const float u = dtVdot(tv, pv) * inv;

		if (u < 0.0f || u > 1.0f) {
			return false;
		}

		dtVcross(qv, tv, e1);

		const float v = dtVdot(dir, qv) * inv;

		if (v < 0.0f || u + v > 1.0f) {
			return false;
		}

		const float s = dtVdot(e2, qv) * inv;

		return s >= 0.0f && s <= 1.0f;
	}
};

enum SimStatus {
	SIM_NOLEDGE, // stopped by a wall or never left the floor: no edge to go over here
	SIM_BAD,     // went over, into the void, a killing floor or a ceiling
	SIM_LANDED,
};

struct Mover {
	const Geom &geom;
	const simnavparams *p;
	const simnavlinkparams *lp;

	/** Whether the box, moved from (x, z) to (nx, nz), meets a wall */
	bool sweepBlocked(float x, float z, float nx, float nz, const float *dir, float boxmin, float boxmax) const
	{
		const float r = p->agentradius;
		const float side[2] = { -dir[1] * r * 0.9f, dir[0] * r * 0.9f };
		const float heights[3] = { boxmin + 1.0f, (boxmin + boxmax) * 0.5f, boxmax - 1.0f };

		for (int i = 0; i < 3; i++) {
			const float y = heights[i];
			const float a[3] = { x, y, z };
			const float b[3] = { nx + dir[0] * r, y, nz + dir[1] * r };
			const float la[3] = { x + side[0], y, z + side[1] };
			const float lb[3] = { nx + side[0], y, nz + side[1] };
			const float ra[3] = { x - side[0], y, z - side[1] };
			const float rb[3] = { nx - side[0], y, nz - side[1] };

			if (geom.segHits(a, b, SIMNAV_KIND_WALL)
					|| geom.segHits(la, lb, SIMNAV_KIND_WALL)
					|| geom.segHits(ra, rb, SIMNAV_KIND_WALL)) {
				return true;
			}
		}

		return false;
	}

	/**
	 * A simulant leaving start in dir at the run speed: walking (v0 0) or
	 * jumping with v0 and the jump's apex. Where it lands, in land.
	 */
	SimStatus run(const float *start, const float *dir, float v0, float apex, float *land, unsigned char *landkind) const
	{
		const float lag = lp->groundlag;
		const float r = p->agentradius;
		const float speed = lp->runspeed;
		const bool jumping = v0 > 0.0f;
		const int maxgroundticks = (int)ceilf((2.0f * r + 20.0f) / speed) + 1;
		const float y0 = geom.floorAt(start[0], start[2], start[1] + lag, nullptr);
		float x = start[0], z = start[2], y = y0, vy = v0;
		bool air = jumping, left = false, hblocked = false;
		int groundticks = 0;

		if (y0 == -FLT_MAX || fabsf(y0 - start[1]) > lag) {
			return SIM_NOLEDGE;
		}

		for (int tick = 0; tick < 900; tick++) {
			if (!hblocked) {
				const float nx = x + dir[0] * speed;
				const float nz = z + dir[1] * speed;
				float boxmin = y + lp->boxfloor;

				// chrGetBbox(): a jumper's box reaches down to its ground,
				// but never more than the jump's apex
				if (jumping) {
					const float ground = geom.floorUnder(x, z, r, y + lag, nullptr);
					boxmin = std::max(y - apex, ground) + lp->boxfloor;
				}

				if (sweepBlocked(x, z, nx, nz, dir, boxmin, y + p->agentheight)) {
					if (!left) {
						return SIM_NOLEDGE;
					}

					hblocked = true; // it goes on down the wall
				} else {
					x = nx;
					z = nz;
				}
			}

			unsigned char kind = 0;
			const float ground = geom.floorUnder(x, z, r, y + lag, &kind);

			if (!air) {
				if (ground != -FLT_MAX && ground >= y - lag) {
					// still walking: down a step or a slope
					y = ground;

					if (++groundticks > maxgroundticks) {
						return SIM_NOLEDGE;
					}

					continue;
				}

				air = true;
			}

			if (ground < y0 - lag) {
				left = true;
			}

			// gravity as func0f0965e4() integrates it
			const float vn = vy - lp->gravity;
			const float ny = y + (vy + vn) * 0.5f;

			if (ny > y) {
				const float a[3] = { x, y + p->agentheight, z };
				const float b[3] = { x, ny + p->agentheight, z };

				if (geom.segHits(a, b, SIMNAV_KIND_FLOOR | SIMNAV_KIND_WALL)) {
					return SIM_BAD; // a head in the ceiling
				}
			}

			vy = vn;
			y = ny;

			if (ground != -FLT_MAX && y <= ground) {
				if (!left) {
					return SIM_NOLEDGE; // back where it started
				}

				land[0] = x;
				land[1] = ground;
				land[2] = z;
				*landkind = kind;

				return (kind & SIMNAV_KIND_DEATH) ? SIM_BAD : SIM_LANDED;
			}

			if (y < y0 - lp->maxdrop) {
				return SIM_BAD; // the void, or deeper than a simulant goes
			}
		}

		return SIM_BAD;
	}
};

float dist3(const float *a, const float *b)
{
	return dtVdist(a, b);
}

/** Whether walking there is much longer than the link would be */
bool worthLinking(const dtNavMeshQuery *q, const dtQueryFilter *f, dtPolyRef sref, const float *a, dtPolyRef eref, const float *b)
{
	dtPolyRef path[256];
	float straight[64 * 3];
	int npath = 0, nstraight = 0;

	if (sref == eref) {
		return false;
	}

	if (dtStatusFailed(q->findPath(sref, eref, a, b, f, path, &npath, 256)) || npath == 0 || path[npath - 1] != eref) {
		return true;
	}

	if (dtStatusFailed(q->findStraightPath(a, b, path, npath, straight, nullptr, nullptr, &nstraight, 64)) || nstraight == 0) {
		return true;
	}

	float len = 0.0f;

	for (int i = 1; i < nstraight; i++) {
		len += dtVdist(&straight[(i - 1) * 3], &straight[i * 3]);
	}

	const float dx = b[0] - a[0], dz = b[2] - a[2];
	const float direct = sqrtf(dx * dx + dz * dz) + fabsf(b[1] - a[1]);

	return len > direct * 1.5f + 150.0f;
}

void addClustered(std::vector<SimNavLink> &kept, const SimNavLink &l, float d)
{
	for (const SimNavLink &k : kept) {
		if (k.area == l.area && dist3(k.a, l.a) < d && dist3(k.b, l.b) < d) {
			return;
		}
	}

	kept.push_back(l);
}

void ladderLinks(const dtNavMeshQuery *q, const dtQueryFilter *f, const simnavinput *in, const simnavparams *p,
		std::vector<SimNavLink> &out)
{
	for (int i = 0; i < in->numladders; i++) {
		const simnavladder *l = &in->ladders[i];
		bool done = false;

		for (int s = 1; s >= -1 && !done; s -= 2) {
			const float foot[3] = { l->x + s * l->nx * 30.0f, l->ymin, l->z + s * l->nz * 30.0f };
			float bottom[3];
			dtPolyRef bref;

			if (!simnavFindFloorPoly(q, f, foot, 100.0f, 60.0f, 25.0f, &bref, bottom)) {
				continue;
			}

			// over the head first, then on the climber's side
			for (int t = -s; t != 3 * s && !done; t += 2 * s) {
				const float head[3] = { l->x + t * l->nx * 30.0f, l->ymax, l->z + t * l->nz * 30.0f };
				float top[3];
				dtPolyRef tref;

				if (!simnavFindFloorPoly(q, f, head, 60.0f, 60.0f, 30.0f, &tref, top)) {
					continue;
				}

				if (top[1] < bottom[1] + p->agentclimb || tref == bref) {
					continue;
				}

				SimNavLink link;
				dtVcopy(link.a, bottom);
				dtVcopy(link.b, top);
				link.rad = 20.0f;
				link.area = SIMNAV_AREA_LADDER;
				link.bidir = 1;
				link.flags = SIMNAV_FLAG_LADDER;
				out.push_back(link);
				done = true;
			}
		}
	}
}

/**
 * A lift's links. Its car is a prop, not in the mesh, so at each stop the
 * mesh ends round its shaft: every side the car can be left by at a stop is
 * an exit (up to four, found round the stop pad), and each exit of a stop is
 * linked to each of the next stop's, and to the other exits of its own stop
 * where the car is the only way across.
 */
void liftLinks(const dtNavMeshQuery *q, const dtQueryFilter *f, const simnavinput *in, const simnavparams *p,
		std::vector<SimNavLink> &out)
{
	const int maxexits = 4;

	for (int i = 0; i < in->numlifts; i++) {
		const simnavlift *lift = &in->lifts[i];
		struct Stop {
			float level;
			int numexits;
			float exits[4][3];
		} stops[4];
		int n = 0;

		for (int s = 0; s < lift->numstops && s < 4; s++) {
			Stop &stop = stops[n];
			dtPolyRef ref;

			if (!simnavFindFloorPoly(q, f, lift->stops[s], 250.0f, 80.0f, 250.0f, &ref, stop.exits[0])) {
				continue;
			}

			stop.level = stop.exits[0][1];
			stop.numexits = 1;

			for (int dir = 0; dir < 8 && stop.numexits < maxexits; dir++) {
				const float angle = dir * 0.78539816f;

				for (float dist = 100.0f; dist <= 300.0f; dist += 50.0f) {
					const float at[3] = {
						lift->stops[s][0] + cosf(angle) * dist,
						stop.level,
						lift->stops[s][2] + sinf(angle) * dist,
					};
					float exit[3];
					bool seen = false;

					if (!simnavFindFloorPoly(q, f, at, 40.0f, 40.0f, 30.0f, &ref, exit)) {
						continue;
					}

					// another side only if it is not walked round to from one had
					for (int e = 0; e < stop.numexits && !seen; e++) {
						dtPolyRef eref;
						float epos[3];

						seen = dtVdist(exit, stop.exits[e]) < 150.0f
							|| !simnavFindFloorPoly(q, f, stop.exits[e], 20.0f, 20.0f, 20.0f, &eref, epos)
							|| !worthLinking(q, f, eref, epos, ref, exit);
					}

					if (!seen) {
						dtVcopy(stop.exits[stop.numexits++], exit);
					}

					break;
				}
			}

			n++;
		}

		for (int s = 1; s < n; s++) {
			for (int k = s; k > 0 && stops[k].level < stops[k - 1].level; k--) {
				std::swap(stops[k], stops[k - 1]);
			}
		}

		SimNavLink link;
		link.rad = 30.0f;
		link.area = SIMNAV_AREA_LIFT;
		link.bidir = 1;
		link.flags = SIMNAV_FLAG_LIFT;

		for (int s = 0; s < n; s++) {
			// across the car at this stop
			for (int a = 0; a < stops[s].numexits; a++) {
				for (int b = a + 1; b < stops[s].numexits; b++) {
					dtPolyRef ra, rb;
					float pa[3], pb[3];

					if (simnavFindFloorPoly(q, f, stops[s].exits[a], 20.0f, 20.0f, 20.0f, &ra, pa)
							&& simnavFindFloorPoly(q, f, stops[s].exits[b], 20.0f, 20.0f, 20.0f, &rb, pb)
							&& worthLinking(q, f, ra, pa, rb, pb)) {
						dtVcopy(link.a, stops[s].exits[a]);
						dtVcopy(link.b, stops[s].exits[b]);
						out.push_back(link);
					}
				}
			}

			// up to the next
			if (s + 1 < n && stops[s + 1].level - stops[s].level > p->agentclimb) {
				for (int a = 0; a < stops[s].numexits; a++) {
					for (int b = 0; b < stops[s + 1].numexits; b++) {
						dtVcopy(link.a, stops[s].exits[a]);
						dtVcopy(link.b, stops[s + 1].exits[b]);
						out.push_back(link);
					}
				}
			}
		}
	}
}

struct EdgeSample {
	dtPolyRef ref;
	float pos[3];
	float dir[2];
};

/** Points along every open edge of the mesh, facing out */
void edgeSamples(const dtNavMesh *nav, const dtNavMeshQuery *q, float spacing, std::vector<EdgeSample> &out)
{
	for (int i = 0; i < nav->getMaxTiles(); i++) {
		const dtMeshTile *tile = nav->getTile(i);

		if (!tile || !tile->header || !tile->dataSize) {
			continue;
		}

		const dtPolyRef base = nav->getPolyRefBase(tile);

		for (int j = 0; j < tile->header->polyCount; j++) {
			const dtPoly *poly = &tile->polys[j];

			if (poly->getType() != DT_POLYTYPE_GROUND) {
				continue;
			}

			float centre[3] = { 0, 0, 0 };

			for (int k = 0; k < poly->vertCount; k++) {
				dtVadd(centre, centre, &tile->verts[poly->verts[k] * 3]);
			}

			dtVscale(centre, centre, 1.0f / poly->vertCount);

			for (int k = 0; k < poly->vertCount; k++) {
				if (poly->neis[k] != 0) {
					continue;
				}

				const float *va = &tile->verts[poly->verts[k] * 3];
				const float *vb = &tile->verts[poly->verts[(k + 1) % poly->vertCount] * 3];
				const float ex = vb[0] - va[0], ez = vb[2] - va[2];
				const float len = sqrtf(ex * ex + ez * ez);

				if (len < 10.0f) {
					continue;
				}

				const int n = std::max(1, (int)(len / spacing));
				float nx = ez / len, nz = -ex / len;

				for (int s = 0; s < n; s++) {
					const float t = (s + 0.5f) / n;
					EdgeSample e;

					e.pos[0] = va[0] + ex * t;
					e.pos[1] = va[1] + (vb[1] - va[1]) * t;
					e.pos[2] = va[2] + ez * t;

					if (nx * (e.pos[0] - centre[0]) + nz * (e.pos[2] - centre[2]) < 0) {
						nx = -nx;
						nz = -nz;
					}

					e.dir[0] = nx;
					e.dir[1] = nz;
					e.ref = base | (dtPolyRef)j;

					// a hair inside, on the detail surface
					const float inside[3] = { e.pos[0] - nx * 1.0f, e.pos[1], e.pos[2] - nz * 1.0f };
					float h;

					if (dtStatusSucceed(q->getPolyHeight(e.ref, inside, &h))) {
						e.pos[1] = h;
					}

					e.pos[0] = inside[0];
					e.pos[2] = inside[2];
					out.push_back(e);
				}
			}
		}
	}
}

} // namespace

bool simnavFindFloorPoly(const dtNavMeshQuery *q, const dtQueryFilter *f, const float *pos,
		float below, float above, float radius, dtPolyRef *ref, float *out)
{
	const float centre[3] = { pos[0], pos[1] + (above - below) * 0.5f, pos[2] };
	const float ext[3] = { radius, (above + below) * 0.5f, radius };
	dtPolyRef polys[64];
	int n = 0;
	float bestscore = FLT_MAX;

	if (dtStatusFailed(q->queryPolygons(centre, ext, f, polys, &n, 64))) {
		return false;
	}

	for (int i = 0; i < n; i++) {
		float c[3];
		bool over;

		if (dtStatusFailed(q->closestPointOnPoly(polys[i], pos, c, &over))) {
			continue;
		}

		const float dx = c[0] - pos[0], dz = c[2] - pos[2], dy = c[1] - pos[1];
		const float dxz = sqrtf(dx * dx + dz * dz);

		if (dxz > radius || dy < -below || dy > above) {
			continue;
		}

		const float score = dxz + fabsf(dy) * 0.25f;

		if (score < bestscore) {
			bestscore = score;
			*ref = polys[i];
			dtVcopy(out, c);
		}
	}

	return bestscore < FLT_MAX;
}

/**
 * Whether a drop or a jump goes by way of a lift's shaft (within 120 of a stop
 * across). The mesh stops round the car, so the shaft is a hole the walker
 * falls down whenever the car is not there - and a simulant that took the
 * link with the car at the top stood on the car instead (Grid, M3).
 */
bool throughLiftShaft(const simnavinput *in, const float *a, const float *b)
{
	for (int i = 0; i < in->numlifts; i++) {
		for (int s = 0; s < in->lifts[i].numstops && s < 4; s++) {
			float t;
			const float *c = in->lifts[i].stops[s];

			if (dtDistancePtSegSqr2D(c, a, b, t) < 120.0f * 120.0f) {
				return true;
			}
		}
	}

	return false;
}

void simnavGenerateLinks(const dtNavMesh *nav, const simnavinput *in, const simnavparams *p,
		const simnavlinkparams *lp, std::vector<SimNavLink> &out)
{
	dtNavMeshQuery *q = dtAllocNavMeshQuery();
	dtQueryFilter filter;

	if (!q || dtStatusFailed(q->init(nav, 4096))) {
		dtFreeNavMeshQuery(q);
		return;
	}

	filter.setIncludeFlags(SIMNAV_FLAG_WALK);
	filter.setExcludeFlags(0);

	ladderLinks(q, &filter, in, p, out);
	liftLinks(q, &filter, in, p, out);

	Geom geom(in);
	Mover mover = { geom, p, lp };
	std::vector<EdgeSample> samples;
	std::vector<SimNavLink> cands;

	edgeSamples(nav, q, lp->samplespacing, samples);

	for (const EdgeSample &e : samples) {
		float dropland[3];
		unsigned char kind;
		const SimStatus drop = mover.run(e.pos, e.dir, 0.0f, 0.0f, dropland, &kind);
		bool dropok = false;
		SimNavLink link;

		if (drop == SIM_NOLEDGE) {
			continue;
		}

		if (drop == SIM_LANDED && dropland[1] < e.pos[1] - lp->groundlag) {
			dtPolyRef lref;

			if (simnavFindFloorPoly(q, &filter, dropland, 40.0f, 40.0f, 40.0f, &lref, link.b) && lref != e.ref) {
				dtVcopy(link.a, e.pos);
				link.rad = 10.0f;
				link.area = SIMNAV_AREA_DROP;
				link.bidir = 0;
				link.flags = SIMNAV_FLAG_DROP;
				cands.push_back(link);
				dropok = true;
				dtVcopy(dropland, link.b);
			}
		}

		for (int k = 0; k < lp->numjumpheights && k < SIMNAV_MAXJUMPHEIGHTS; k++) {
			float land[3];
			dtPolyRef lref;

			if (mover.run(e.pos, e.dir, lp->jumpimpulse[k], lp->jumpapex[k], land, &kind) != SIM_LANDED) {
				continue;
			}

			if (!simnavFindFloorPoly(q, &filter, land, 40.0f, 40.0f, 40.0f, &lref, link.b) || lref == e.ref) {
				continue;
			}

			// where a walk off the edge already goes, or lower
			if (dropok && link.b[1] < dropland[1] + p->agentclimb) {
				continue;
			}

			dtVcopy(link.a, e.pos);
			link.rad = 10.0f;
			link.area = SIMNAV_AREA_JUMP;
			link.bidir = 0;
			link.flags = SIMNAV_FLAG_JUMP(k + 1);
			cands.push_back(link);
			break;
		}
	}

	// one of each cluster, then only where walking is the long way round
	std::vector<SimNavLink> clustered;

	for (const SimNavLink &l : cands) {
		if (!throughLiftShaft(in, l.a, l.b)) {
			addClustered(clustered, l, lp->clusterdist);
		}
	}

	for (const SimNavLink &l : clustered) {
		dtPolyRef sref, eref;
		float a[3], b[3];

		if (!simnavFindFloorPoly(q, &filter, l.a, 20.0f, 20.0f, 20.0f, &sref, a)
				|| !simnavFindFloorPoly(q, &filter, l.b, 20.0f, 20.0f, 20.0f, &eref, b)) {
			continue;
		}

		if (worthLinking(q, &filter, sref, a, eref, b)) {
			out.push_back(l);
		}
	}

	dtFreeNavMeshQuery(q);
}
