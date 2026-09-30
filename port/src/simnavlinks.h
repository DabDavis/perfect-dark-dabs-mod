#ifndef _IN_SIMNAVLINKS_H
#define _IN_SIMNAVLINKS_H

/**
 * The off-mesh links of port/src/simnavlinks.cpp, between the two halves of
 * port/src/simnav.cpp's build. C++ only.
 */

#include <vector>

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include "simnav.h"

struct SimNavLink {
	float a[3];
	float b[3];
	float rad;
	unsigned char area;
	unsigned char bidir;
	unsigned short flags;
};

// The links of a mesh built without any, from the same input
void simnavGenerateLinks(const dtNavMesh *nav, const simnavinput *in, const simnavparams *params,
		const simnavlinkparams *lp, std::vector<SimNavLink> &out);

// The floor polygon near pos (simnavQueryFindFloor())
bool simnavFindFloorPoly(const dtNavMeshQuery *query, const dtQueryFilter *filter, const float *pos,
		float below, float above, float radius, dtPolyRef *ref, float *out);

#endif
