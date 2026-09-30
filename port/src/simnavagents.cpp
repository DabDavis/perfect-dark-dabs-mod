/**
 * Simulants following the navmesh (PLANS/AI-REWORK.md M3): a path corridor
 * each, the path queries that fill them run a few a frame, and local
 * avoidance. The Recast side of port/src/simbrain.c, which knows the game;
 * nothing here does (port/include/simnav.h has the C interface).
 *
 * The pieces are DetourCrowd's - dtPathCorridor, dtLocalBoundary,
 * dtObstacleAvoidanceQuery - but not dtCrowd itself. dtCrowd moves its agents
 * itself and crosses an off-mesh link by sliding the agent along it on a
 * timer; a simulant is moved by the chr code, and crosses a link by walking
 * off a ledge, jumping, climbing or riding a lift, which takes as long as it
 * takes. So each agent's corridor is moved to wherever the chr really is
 * every tick, and the link is handed back to the caller to cross.
 *
 * Nothing here draws on a random number: two runs of one seed follow the
 * same paths.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <new>
#include <vector>
#include <algorithm>

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"
#include "DetourPathCorridor.h"
#include "DetourLocalBoundary.h"
#include "DetourObstacleAvoidance.h"

#include "simnav.h"
#include "simnavlinks.h"

struct simnavmesh {
	dtNavMesh *nav;
	simnavstats stats;
};

namespace {

const int MAXPATH = 512;        // polygons in a corridor; the largest arena's longest path is under 200
const int MAXQUERYNODES = 4096; // A* nodes per query
const int MAXCORNERS = 4;
const int MAXNEIGHBOURS = 6;

enum {
	REQ_NONE,
	REQ_PENDING,
};

#ifndef DT_VIRTUAL_QUERYFILTER
#error "simnavagents.cpp needs Detour built with DT_VIRTUAL_QUERYFILTER (CMakeLists.txt)"
#endif

/**
 * Ways from one polygon to the next that a simulant was stuck on: the mesh is
 * only as exact as its cells, and where it walks up a step the chr code does
 * not (a riser flagged as a wall), a path through it stops a simulant for
 * good. Blamed once, the way costs BLAMECOST more; twice, as good as closed
 * (a path still takes it if there is no other). Open addressing on the pair.
 */
const int MAXBLAMED = 1024;
const float BLAMECOST = 3000.0f;
const float CLOSEDCOST = 1000000.0f;

struct Blame {
	dtPolyRef from;
	dtPolyRef to;
	int count;
};

class BlameTable {
public:
	Blame slots[MAXBLAMED];
	int num;

	BlameTable() : num(0)
	{
		memset(slots, 0, sizeof(slots));
	}

	static unsigned int hash(dtPolyRef from, dtPolyRef to)
	{
		unsigned long long h = (unsigned long long)from * 0x9e3779b97f4a7c15ull ^ (unsigned long long)to * 0xc2b2ae3d27d4eb4full;
		return (unsigned int)(h >> 40) & (MAXBLAMED - 1);
	}

	const Blame *find(dtPolyRef from, dtPolyRef to) const
	{
		for (unsigned int i = hash(from, to), n = 0; n < MAXBLAMED; i = (i + 1) & (MAXBLAMED - 1), n++) {
			if (slots[i].from == 0) {
				return nullptr;
			}

			if (slots[i].from == from && slots[i].to == to) {
				return &slots[i];
			}
		}

		return nullptr;
	}

	int blame(dtPolyRef from, dtPolyRef to)
	{
		if (num >= MAXBLAMED / 2) {
			return 0;
		}

		for (unsigned int i = hash(from, to), n = 0; n < MAXBLAMED; i = (i + 1) & (MAXBLAMED - 1), n++) {
			if (slots[i].from == 0) {
				slots[i].from = from;
				slots[i].to = to;
				slots[i].count = 1;
				num++;
				return 1;
			}

			if (slots[i].from == from && slots[i].to == to) {
				return ++slots[i].count;
			}
		}

		return 0;
	}
};

class SimFilter : public dtQueryFilter {
public:
	const BlameTable *blamed = nullptr;

	float getCost(const float *pa, const float *pb,
			const dtPolyRef prevRef, const dtMeshTile *prevTile, const dtPoly *prevPoly,
			const dtPolyRef curRef, const dtMeshTile *curTile, const dtPoly *curPoly,
			const dtPolyRef nextRef, const dtMeshTile *nextTile, const dtPoly *nextPoly) const override
	{
		float cost = dtQueryFilter::getCost(pa, pb, prevRef, prevTile, prevPoly, curRef, curTile, curPoly,
				nextRef, nextTile, nextPoly);

		if (blamed && blamed->num && curRef && nextRef) {
			const Blame *b = blamed->find(curRef, nextRef);

			if (b) {
				cost += b->count >= 2 ? CLOSEDCOST : BLAMECOST;
			}
		}

		return cost;
	}
};

struct Agent {
	dtPathCorridor corridor;
	dtLocalBoundary boundary;
	SimFilter filter;
	int status;           // SIMNAV_AGENT_*
	int request;          // REQ_*
	unsigned int reqseq;  // order of the request, oldest first
	float reqstart[3];
	float reqend[3];
	unsigned short reqinclude;
	int complete;
	int failreason;       // 1 no floor under the start, 2 none under the end, 3 no way between
	float goal[3];        // the end asked for (the corridor's target is on the mesh)
	int boundaryage;
	dtPolyRef lastlink;   // the link it crossed last
};

const int MAXFAILEDLINKS = 64;

struct FailedLink {
	dtPolyRef ref;
	int count;
};

}

struct simnavagents {
	const dtNavMesh *nav;
	dtNavMeshQuery *q;
	dtObstacleAvoidanceQuery *avoid;
	dtObstacleAvoidanceParams avoidparams;
	float areacost[SIMNAV_NUMAREAS];
	int numagents;
	Agent *agents;
	unsigned int seq;
	dtPolyRef path[MAXPATH];
	int queriesrun;
	FailedLink failed[MAXFAILEDLINKS];
	BlameTable blamed;
};

extern "C" struct simnavagents *simnavAgentsCreate(const struct simnavmesh *mesh, int numagents)
{
	simnavagents *a = new (std::nothrow) simnavagents();

	if (!a) {
		return nullptr;
	}

	a->nav = mesh->nav;
	a->numagents = numagents;
	a->q = dtAllocNavMeshQuery();
	a->avoid = dtAllocObstacleAvoidanceQuery();
	a->agents = new (std::nothrow) Agent[numagents];

	for (int i = 0; i < SIMNAV_NUMAREAS; i++) {
		a->areacost[i] = 1.0f;
	}

	if (!a->q || !a->avoid || !a->agents
			|| dtStatusFailed(a->q->init(mesh->nav, MAXQUERYNODES))
			|| !a->avoid->init(MAXNEIGHBOURS, 8)) {
		simnavAgentsFree(a);
		return nullptr;
	}

	for (int i = 0; i < numagents; i++) {
		Agent &ag = a->agents[i];

		if (!ag.corridor.init(MAXPATH)) {
			simnavAgentsFree(a);
			return nullptr;
		}

		ag.filter.blamed = &a->blamed;
		ag.status = SIMNAV_AGENT_NONE;
		ag.request = REQ_NONE;
		ag.reqseq = 0;
		ag.complete = 0;
		ag.boundaryage = 0;
		ag.lastlink = 0;
	}

	// RecastDemo's "medium" quality, one ring deep less: two rings of five,
	// refined once. The horizon is a second (velocities are units a second)
	memset(&a->avoidparams, 0, sizeof(a->avoidparams));
	a->avoidparams.velBias = 0.4f;
	a->avoidparams.weightDesVel = 2.0f;
	a->avoidparams.weightCurVel = 0.75f;
	a->avoidparams.weightSide = 0.75f;
	a->avoidparams.weightToi = 2.5f;
	a->avoidparams.horizTime = 1.0f;
	a->avoidparams.gridSize = 33;
	a->avoidparams.adaptiveDivs = 5;
	a->avoidparams.adaptiveRings = 2;
	a->avoidparams.adaptiveDepth = 2;

	return a;
}

extern "C" void simnavAgentsFree(struct simnavagents *a)
{
	if (a) {
		delete[] a->agents;
		dtFreeObstacleAvoidanceQuery(a->avoid);
		dtFreeNavMeshQuery(a->q);
		delete a;
	}
}

extern "C" void simnavAgentsSetAreaCost(struct simnavagents *a, int area, float cost)
{
	if (area >= 0 && area < SIMNAV_NUMAREAS) {
		a->areacost[area] = cost;
	}
}

static Agent *agentAt(simnavagents *a, int idx)
{
	return a && idx >= 0 && idx < a->numagents ? &a->agents[idx] : nullptr;
}

extern "C" void simnavAgentClear(struct simnavagents *a, int idx)
{
	Agent *ag = agentAt(a, idx);

	if (ag) {
		ag->status = SIMNAV_AGENT_NONE;
		ag->request = REQ_NONE;
		ag->corridor.reset(0, ag->corridor.getPos());
		ag->boundary.reset();
	}
}

extern "C" void simnavAgentRequest(struct simnavagents *a, int idx, const float *start, const float *end,
		unsigned short include)
{
	Agent *ag = agentAt(a, idx);

	if (!ag) {
		return;
	}

	// the latest request wins, but keeps its place in the queue
	if (ag->request != REQ_PENDING) {
		ag->reqseq = ++a->seq;
	}

	ag->request = REQ_PENDING;
	dtVcopy(ag->reqstart, start);
	dtVcopy(ag->reqend, end);
	ag->reqinclude = include;

	if (ag->status != SIMNAV_AGENT_FOLLOWING) {
		ag->status = SIMNAV_AGENT_PENDING;
	}
}

extern "C" int simnavAgentIsPending(struct simnavagents *a, int idx)
{
	Agent *ag = agentAt(a, idx);

	return ag && ag->request == REQ_PENDING;
}

extern "C" int simnavAgentStatus(struct simnavagents *a, int idx, int *complete, float *goal)
{
	Agent *ag = agentAt(a, idx);

	if (!ag) {
		return SIMNAV_AGENT_NONE;
	}

	if (complete) {
		*complete = ag->complete;
	}

	if (goal) {
		dtVcopy(goal, ag->goal);
	}

	return ag->status;
}

/** One queued request, run whole */
static void runQuery(simnavagents *a, Agent *ag)
{
	dtPolyRef sref, eref;
	float s[3], e[3];
	int npath = 0;

	ag->request = REQ_NONE;
	ag->filter.setIncludeFlags(ag->reqinclude);
	ag->filter.setExcludeFlags(0);

	for (int i = 0; i < SIMNAV_NUMAREAS; i++) {
		ag->filter.setAreaCost(i, a->areacost[i]);
	}

	dtVcopy(ag->goal, ag->reqend);
	ag->complete = 0;
	ag->boundary.reset();

	// the start is a chr's feet (or a crate's top: props are not in the
	// mesh, and its floor is looked for below); the end a prop's or a player's position,
	// which may stand up to a pad's height over its floor
	ag->failreason = 0;

	if (!simnavFindFloorPoly(a->q, &ag->filter, ag->reqstart, 200.0f, 60.0f, 60.0f, &sref, s)) {
		ag->failreason = 1;
	} else if (!simnavFindFloorPoly(a->q, &ag->filter, ag->reqend, 260.0f, 60.0f, 80.0f, &eref, e)) {
		ag->failreason = 2;
	} else if (dtStatusFailed(a->q->findPath(sref, eref, s, e, &ag->filter, a->path, &npath, MAXPATH)) || npath == 0) {
		ag->failreason = 3;
	}

	if (ag->failreason) {
		ag->status = SIMNAV_AGENT_FAILED;
		return;
	}

	ag->complete = a->path[npath - 1] == eref;

	if (!ag->complete) {
		float near[3];
		bool over;

		if (dtStatusSucceed(a->q->closestPointOnPoly(a->path[npath - 1], e, near, &over))) {
			dtVcopy(e, near);
		}

		// as good as there: within a stride of the end, on its floor
		if (dtVdist2DSqr(e, ag->reqend) < 100.0f * 100.0f && ag->reqend[1] - e[1] > -60.0f
				&& ag->reqend[1] - e[1] < 260.0f) {
			ag->complete = 1;
		}
	}

	ag->corridor.reset(sref, s);
	ag->corridor.setCorridor(e, a->path, npath);
	ag->status = SIMNAV_AGENT_FOLLOWING;
}

extern "C" int simnavAgentsUpdate(struct simnavagents *a, int maxqueries)
{
	int run = 0;

	if (!a) {
		return 0;
	}

	while (run < maxqueries) {
		Agent *oldest = nullptr;

		for (int i = 0; i < a->numagents; i++) {
			Agent *ag = &a->agents[i];

			if (ag->request == REQ_PENDING && (!oldest || ag->reqseq < oldest->reqseq)) {
				oldest = ag;
			}
		}

		if (!oldest) {
			break;
		}

		runQuery(a, oldest);
		run++;
	}

	a->queriesrun += run;

	return run;
}

extern "C" int simnavAgentFailReason(struct simnavagents *a, int idx)
{
	Agent *ag = agentAt(a, idx);

	return ag ? ag->failreason : 0;
}

extern "C" int simnavAgentBlockAhead(struct simnavagents *a, int idx, int walled)
{
	Agent *ag = agentAt(a, idx);
	const dtPolyRef *path;
	const dtMeshTile *tile;
	const dtPoly *poly;

	if (!ag || ag->status != SIMNAV_AGENT_FOLLOWING || ag->corridor.getPathCount() < 2) {
		return 0;
	}

	path = ag->corridor.getPath();

	// a ground polygon to a ground polygon: a link that fails has its own count
	for (int i = 0; i < 2; i++) {
		if (dtStatusFailed(a->nav->getTileAndPolyByRef(path[i], &tile, &poly))
				|| poly->getType() != DT_POLYTYPE_GROUND) {
			return 0;
		}
	}

	int count = a->blamed.blame(path[0], path[1]);

	// stopped by a wall, not a crowd: closed at once
	if (walled && count == 1) {
		count = a->blamed.blame(path[0], path[1]);
	}

	return count;
}

extern "C" int simnavAgentsQueriesRun(struct simnavagents *a)
{
	return a ? a->queriesrun : 0;
}

extern "C" int simnavAgentRunNow(struct simnavagents *a, int idx)
{
	Agent *ag = agentAt(a, idx);

	if (!ag || ag->request != REQ_PENDING) {
		return ag ? ag->status : SIMNAV_AGENT_NONE;
	}

	runQuery(a, ag);
	a->queriesrun++;

	return ag->status;
}

extern "C" int simnavAgentMoveGoal(struct simnavagents *a, int idx, const float *end)
{
	Agent *ag = agentAt(a, idx);
	dtPolyRef ref;
	float onmesh[3];

	if (!ag || ag->status != SIMNAV_AGENT_FOLLOWING || !ag->complete || ag->request == REQ_PENDING) {
		return 0;
	}

	if (!simnavFindFloorPoly(a->q, &ag->filter, end, 260.0f, 60.0f, 80.0f, &ref, onmesh)) {
		return 0;
	}

	if (!ag->corridor.moveTargetPosition(onmesh, a->q, &ag->filter)) {
		return 0;
	}

	// moved only as far as the surface let it: not there, so ask again
	const float *t = ag->corridor.getTarget();

	if (dtVdist2DSqr(t, onmesh) > 10.0f * 10.0f || fabsf(t[1] - onmesh[1]) > 40.0f
			|| ag->corridor.getLastPoly() != ref) {
		return 0;
	}

	dtVcopy(ag->goal, end);

	return 1;
}

extern "C" int simnavAgentFollow(struct simnavagents *a, int idx, const float *feet, int corners,
		struct simnavsteer *out)
{
	Agent *ag = agentAt(a, idx);
	float cverts[MAXCORNERS * 3];
	unsigned char cflags[MAXCORNERS];
	dtPolyRef cpolys[MAXCORNERS];

	if (!ag || ag->status != SIMNAV_AGENT_FOLLOWING || ag->corridor.getPathCount() == 0) {
		return 0;
	}

	ag->corridor.movePosition(feet, a->q, &ag->filter);

	const float *cpos = ag->corridor.getPos();
	dtVcopy(out->pos, cpos);

	// Off the corridor: fallen off a ledge, pushed through a gap, carried
	// off by a lift. The caller asks for a new path.
	if (dtVdist2DSqr(cpos, feet) > 60.0f * 60.0f || fabsf(cpos[1] - feet[1]) > 200.0f) {
		return 0;
	}

	{
		const dtPolyRef *path = ag->corridor.getPath();
		const int n = ag->corridor.getPathCount();
		unsigned char area = 0;

		out->area = dtStatusSucceed(a->nav->getPolyArea(path[0], &area)) ? area : 0;
		out->aheadarea = out->area;

		for (int i = 1; i < n && i < 3; i++) {
			if (dtStatusSucceed(a->nav->getPolyArea(path[i], &area))
					&& (area == SIMNAV_AREA_DUCK || area == SIMNAV_AREA_CROUCH) && area > out->aheadarea) {
				out->aheadarea = area;
			}
		}
	}

	if (!corners) {
		return 1;
	}

	int nc = ag->corridor.findCorners(cverts, cflags, cpolys, MAXCORNERS, a->q, &ag->filter);

	// When the next corner is some way off, a raycast along the mesh may
	// find a straighter corridor to the one after (dtCrowd does this too)
	if (nc > 1 && (cflags[0] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) == 0 && dtVdist2DSqr(cverts, cpos) > 40.0f * 40.0f) {
		ag->corridor.optimizePathVisibility(&cverts[3], 600.0f, a->q, &ag->filter);
		nc = ag->corridor.findCorners(cverts, cflags, cpolys, MAXCORNERS, a->q, &ag->filter);
	}

	out->numcorners = nc;
	out->linkarea = SIMNAV_AREA_NONE;
	out->final = 0;

	if (nc <= 0) {
		// on the target's polygon: the target is the corner
		dtVcopy(out->corner, ag->corridor.getTarget());
		dtVcopy(out->after, out->corner);
		out->final = 1;
		out->numcorners = 0;
		return 1;
	}

	dtVcopy(out->corner, cverts);
	dtVcopy(out->after, nc > 1 ? &cverts[3] : cverts);

	if (cflags[0] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) {
		unsigned char area = 0;
		out->linkarea = dtStatusSucceed(a->nav->getPolyArea(cpolys[0], &area)) ? area : SIMNAV_AREA_DROP;
	} else if (cflags[0] & DT_STRAIGHTPATH_END) {
		out->final = 1;
	}

	return 1;
}

extern "C" int simnavAgentTakeLink(struct simnavagents *a, int idx, float *start, float *end, unsigned char *area)
{
	Agent *ag = agentAt(a, idx);
	float cverts[MAXCORNERS * 3];
	unsigned char cflags[MAXCORNERS];
	dtPolyRef cpolys[MAXCORNERS];
	dtPolyRef refs[2];

	if (!ag || ag->status != SIMNAV_AGENT_FOLLOWING) {
		return 0;
	}

	int nc = ag->corridor.findCorners(cverts, cflags, cpolys, MAXCORNERS, a->q, &ag->filter);

	if (nc <= 0 || (cflags[0] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) == 0) {
		return 0;
	}

	if (!ag->corridor.moveOverOffmeshConnection(cpolys[0], refs, start, end, a->q)) {
		return 0;
	}

	unsigned char ar = 0;
	*area = dtStatusSucceed(a->nav->getPolyArea(cpolys[0], &ar)) ? ar : SIMNAV_AREA_DROP;
	ag->boundary.reset();
	ag->lastlink = cpolys[0];

	return 1;
}

extern "C" int simnavAgentLinkFailed(struct simnavagents *a, int idx)
{
	Agent *ag = agentAt(a, idx);
	int slot = -1;

	if (!ag || !ag->lastlink) {
		return 0;
	}

	for (int i = 0; i < MAXFAILEDLINKS; i++) {
		if (a->failed[i].ref == ag->lastlink) {
			slot = i;
			break;
		}

		if (slot < 0 && a->failed[i].ref == 0) {
			slot = i;
		}
	}

	if (slot < 0) {
		return 0;
	}

	a->failed[slot].ref = ag->lastlink;

	if (++a->failed[slot].count < 2) {
		return 0;
	}

	// twice now: the link does not work for a simulant, whatever the mesh
	// says, and no query takes it again this match
	const_cast<dtNavMesh *>(a->nav)->setPolyFlags(ag->lastlink, 0);
	ag->lastlink = 0;

	return 1;
}

/** The link at the head of the corridor, if the next corner is one: its polygon and ends */
static dtPolyRef headLink(simnavagents *a, Agent *ag, float *start, float *end)
{
	float cverts[MAXCORNERS * 3];
	unsigned char cflags[MAXCORNERS];
	dtPolyRef cpolys[MAXCORNERS];
	const dtPolyRef *path = ag->corridor.getPath();
	const int npath = ag->corridor.getPathCount();

	int nc = ag->corridor.findCorners(cverts, cflags, cpolys, MAXCORNERS, a->q, &ag->filter);

	if (nc <= 0 || (cflags[0] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) == 0) {
		return 0;
	}

	for (int i = 1; i < npath; i++) {
		if (path[i] == cpolys[0]) {
			if (dtStatusSucceed(a->nav->getOffMeshConnectionPolyEndPoints(path[i - 1], path[i], start, end))) {
				return path[i];
			}

			break;
		}
	}

	return 0;
}

extern "C" int simnavAgentPeekLink(struct simnavagents *a, int idx, float *start, float *end)
{
	Agent *ag = agentAt(a, idx);

	return ag && ag->status == SIMNAV_AGENT_FOLLOWING && headLink(a, ag, start, end) != 0;
}

extern "C" int simnavAgentDisableLink(struct simnavagents *a, int idx)
{
	Agent *ag = agentAt(a, idx);
	float start[3], end[3];
	dtPolyRef ref;

	if (!ag || ag->status != SIMNAV_AGENT_FOLLOWING || (ref = headLink(a, ag, start, end)) == 0) {
		return 0;
	}

	// the mesh is the stage's for this match, and a link found wrong is
	// wrong for everybody: no query takes it again
	return dtStatusSucceed(const_cast<dtNavMesh *>(a->nav)->setPolyFlags(ref, 0));
}

extern "C" int simnavAgentAvoid(struct simnavagents *a, int idx, const float *feet, float radius, float maxspeed,
		const float *vel, const float *dvel, const float *neighbours, int numneighbours, float *nvel)
{
	Agent *ag = agentAt(a, idx);

	dtVcopy(nvel, dvel);

	if (!ag || ag->status != SIMNAV_AGENT_FOLLOWING || numneighbours <= 0 || maxspeed <= 0.0f) {
		return 0;
	}

	// the walls round it, from the mesh; refreshed as the agent moves
	if (ag->boundaryage-- <= 0 || dtVdist2DSqr(feet, ag->boundary.getCenter()) > (radius * 0.5f) * (radius * 0.5f)) {
		ag->boundary.update(ag->corridor.getFirstPoly(), feet, radius * 8.0f, a->q, &ag->filter);
		ag->boundaryage = 10;
	}

	a->avoid->reset();

	for (int i = 0; i < numneighbours && i < MAXNEIGHBOURS; i++) {
		const float *nb = &neighbours[i * 7];
		a->avoid->addCircle(nb, nb[6], &nb[3], &nb[3]);
	}

	for (int i = 0; i < ag->boundary.getSegmentCount(); i++) {
		const float *s = ag->boundary.getSegment(i);

		if (dtTriArea2D(feet, s, s + 3) < 0.0f) {
			continue;
		}

		a->avoid->addSegment(s, s + 3);
	}

	a->avoid->sampleVelocityAdaptive(feet, radius, maxspeed, vel, dvel, nvel, &a->avoidparams);

	return 1;
}

extern "C" int simnavAgentCorners(struct simnavagents *a, int idx, float *points, int maxpoints)
{
	Agent *ag = agentAt(a, idx);
	std::vector<unsigned char> flags(maxpoints > 0 ? maxpoints : 1);
	std::vector<dtPolyRef> refs(maxpoints > 0 ? maxpoints : 1);
	int n = 0;

	if (!ag || ag->status != SIMNAV_AGENT_FOLLOWING || maxpoints < 2 || ag->corridor.getPathCount() == 0) {
		return 0;
	}

	if (dtStatusFailed(a->q->findStraightPath(ag->corridor.getPos(), ag->corridor.getTarget(), ag->corridor.getPath(),
				ag->corridor.getPathCount(), points, flags.data(), refs.data(), &n, maxpoints))) {
		return 0;
	}

	return n;
}
