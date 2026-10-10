#include <ultra64.h>
#include "constants.h"
#include "game/atan2f.h"
#include "game/padhalllv.h"
#include "bss.h"
#include "lib/lib_17ce0.h"
#include "lib/mtx.h"
#include "data.h"
#include "types.h"

u8 var8005ef20 = 254;

#ifdef PLATFORM_N64
u8 var8009a4e0[456][2];
#else
#include <stdlib.h>
#include <string.h>
#include "system.h"

/**
 * portal00018148()'s per-portal cache of the line's crossing, by portal
 * number. Stock's was 456 portals in bss, enough for Perfect Dark's stages,
 * and nothing bounded the index: a converted GoldenEye level or a mod's map
 * with more portals wrote past it into the mema heap's free-space table that
 * follows it, and vtxstoreAllocate() was handed address 0x450 for a hit's
 * blood (crash 20261010-083117), or the stage pools' descriptors further on.
 * It is grown to the stage's count when the portals are counted (bg.c).
 */
static u8 s_PortalIsectStock[456][2];
static u8 (*var8009a4e0)[2] = s_PortalIsectStock;
static s32 s_PortalIsectCap = ARRAYCOUNT(s_PortalIsectStock);

void portalReserveIsectCache(s32 numportals)
{
	if (numportals > s_PortalIsectCap) {
		u8 (*grown)[2] = malloc((size_t)numportals * sizeof(*grown));

		if (grown) {
			memcpy(grown, var8009a4e0, (size_t)s_PortalIsectCap * sizeof(*grown));
			// 0xff is no tick's mark (var8005ef20 runs 0 to 254): not cached
			memset(grown + s_PortalIsectCap, 0xff, (size_t)(numportals - s_PortalIsectCap) * sizeof(*grown));

			if (var8009a4e0 != s_PortalIsectStock) {
				free(var8009a4e0);
			}

			var8009a4e0 = grown;
			s_PortalIsectCap = numportals;
		}
	}
}
#endif

void portalGetAvgVertexPos(s32 portalnum, struct coord *avg)
{
	struct portalvertices *pvertices = (struct portalvertices *)((uintptr_t)g_BgPortals + g_BgPortals[portalnum].verticesoffset);
	f32 f0;
	s32 i;

	avg->x = pvertices->vertices[0].x;
	avg->y = pvertices->vertices[0].y;
	avg->z = pvertices->vertices[0].z;

	f0 = 1.0f / pvertices->count;

	for (i = 1; i < pvertices->count; i++) {
		avg->x += pvertices->vertices[i].x;
		avg->y += pvertices->vertices[i].y;
		avg->z += pvertices->vertices[i].z;
	}

	avg->x *= f0;
	avg->y *= f0;
	avg->z *= f0;
}

#if VERSION < VERSION_NTSC_1_0
bool portal00018e34nb(s32 portalnum)
{
	return g_BgPortals[portalnum].flags & PORTALFLAG_02;
}

void portal00018e54nb(s32 portalnum)
{
	g_BgPortals[portalnum].flags |= PORTALFLAG_02;
}

void portal00018e74nb(s32 portalnum)
{
	g_BgPortals[portalnum].flags &= (~PORTALFLAG_02 & 0xff);
}
#endif

/**
 * Add roomnum to the rooms list, provided it's not already there and there's
 * space available at the end of the list.
 *
 * The list is assumed to have 16 slots, with the last being reserved for the
 * -1 terminator.
 */
void portal00017dc4(RoomNum *rooms, RoomNum roomnum)
{
	s32 i;

	for (i = 0; i < 16 && rooms[i] != -1; i++) {
		if (rooms[i] == roomnum) {
			return;
		}
	}

	if (i < 15) {
		rooms[i] = roomnum;
		rooms[i + 1] = -1;
	}
}

/**
 * Determine whether a line from pos1 to pos2 intersects the portal,
 * and if so then in which direction.
 *
 * Return one of:
 * PORTALINTERSECTION_NONE
 * PORTALINTERSECTION_BEHINDTOFRONT
 * PORTALINTERSECTION_FRONTTOBEHIND
 *
 * The portal's normal vector is the front.
 */
s32 portalCalculateIntersection(s32 portalnum, struct coord *pos1, struct coord *pos2)
{
	s32 i;
	struct coord sp60;
	struct portalvertices *pvertices;
	struct coord *curr;
	struct coord *next;
	struct coord sp48;
	u8 lastside;
	f32 sp40[1];
	struct coord sp34;
	f32 value1;
	f32 value2;
	f32 tmp;
	u32 stack;

	lastside = 0;
	pvertices = (struct portalvertices *)((uintptr_t)g_BgPortals + g_BgPortals[portalnum].verticesoffset);
	value1 = pos1->f[0] * (g_PortalMetrics + portalnum)->normal.f[0] + pos1->f[1] * (g_PortalMetrics + portalnum)->normal.f[1] + pos1->f[2] * (g_PortalMetrics + portalnum)->normal.f[2];
	value2 = pos2->f[0] * (g_PortalMetrics + portalnum)->normal.f[0] + pos2->f[1] * (g_PortalMetrics + portalnum)->normal.f[1] + pos2->f[2] * (g_PortalMetrics + portalnum)->normal.f[2];

	if (value1 < (g_PortalMetrics + portalnum)->min) {
		if (value2 < (g_PortalMetrics + portalnum)->min) {
			return PORTALINTERSECTION_NONE;
		}
	} else if ((g_PortalMetrics + portalnum)->max < value1 && (g_PortalMetrics + portalnum)->max < value2) {
		return PORTALINTERSECTION_NONE;
	}

	sp60.f[0] = pos2->f[0] - pos1->f[0];
	sp60.f[1] = pos2->f[1] - pos1->f[1];
	sp60.f[2] = pos2->f[2] - pos1->f[2];

	var8007fcb4 = (value1 + value2) * 0.5f - (g_PortalMetrics + portalnum)->min;

	curr = &pvertices->vertices[0];
	next = &pvertices->vertices[1];

	for (i = 0; i < pvertices->count; i++) {
		if (i + 1 == pvertices->count) {
			next = &pvertices->vertices[0];
		}

		sp48.f[0] = next->f[0] - curr->f[0];
		sp48.f[1] = next->f[1] - curr->f[1];
		sp48.f[2] = next->f[2] - curr->f[2];

		sp34.f[0] = sp48.f[1] * sp60.f[2] - sp48.f[2] * sp60.f[1];
		sp34.f[1] = sp48.f[2] * sp60.f[0] - sp48.f[0] * sp60.f[2];
		sp34.f[2] = sp48.f[0] * sp60.f[1] - sp48.f[1] * sp60.f[0];

		tmp = sp34.f[0] * sp34.f[0] + sp34.f[1] * sp34.f[1] + sp34.f[2] * sp34.f[2];

		if (tmp == 0.0f) {
			return PORTALINTERSECTION_NONE;
		}

		sp40[0] = sp34.f[0] * curr->f[0] + sp34.f[1] * curr->f[1] + sp34.f[2] * curr->f[2];
		tmp = sp34.f[0] * pos1->f[0] + sp34.f[1] * pos1->f[1] + sp34.f[2] * pos1->f[2];

		if (tmp < sp40[0]) {
			if (lastside == 2) {
				return PORTALINTERSECTION_NONE;
			}

			lastside = 1;
		} else {
			if (lastside == 1) {
				return PORTALINTERSECTION_NONE;
			}

			lastside = 2;
		}

		curr++;
		next++;
	}

	return (value1 < (g_PortalMetrics + portalnum)->min)
		? PORTALINTERSECTION_BEHINDTOFRONT
		: PORTALINTERSECTION_FRONTTOBEHIND;
}

void portal00018148(struct coord *pos1, struct coord *pos2, RoomNum *rooms1, RoomNum *rooms2, RoomNum *rooms3, s32 arg5)
{
	s32 i;
	s32 j;
	s32 roomnum;
	s32 numportals;
	s16 *portalnums;
	RoomNum rooms9c[16];
	RoomNum rooms7c[16];
	RoomNum rooms5c[16];

	for (i = 0; i < 8; i++) {
		rooms9c[i] = rooms1[i];
		rooms5c[i] = rooms1[i];

		if (rooms1[i] == -1) {
			break;
		}
	}

	var8005ef20++;

	if (var8005ef20 == 255) {
		for (i = 0; i < g_BgNumPortalCameraCacheItems; i++) {
#ifndef PLATFORM_N64
			if (i >= s_PortalIsectCap) {
				break;
			}
#endif
			var8009a4e0[i][0] = 0xff;
		}

		var8005ef20 = 0;
	}

	do {
		rooms7c[0] = -1;

		for (j = 0; (roomnum = rooms9c[j]) != -1 && j < 16; j++) {
#ifndef PLATFORM_N64
			// A room number that is not the stage's reads a portal list from
			// past g_Rooms, and its portal numbers index the cache below
			if (roomnum < 0 || roomnum >= g_Vars.roomcount) {
				static s32 logged;

				if (logged++ < 4) {
					sysLogPrintf(LOG_WARNING, "portal: room %d in a line's rooms is not one of the stage's %d; skipped",
							roomnum, g_Vars.roomcount);
				}

				continue;
			}
#endif
			numportals = g_Rooms[roomnum].numportals;
			portalnums = &g_RoomPortals[g_Rooms[roomnum].roomportallistoffset];

			for (i = 0; i < numportals; i++) {
				s32 portalnum = *portalnums;
#ifdef PLATFORM_N64
				u8 *s1 = var8009a4e0[portalnum];
#else
				u8 spare[2] = { 0xff, 0 };
				u8 *s1 = spare;

				if (portalnum >= 0 && portalnum < s_PortalIsectCap) {
					s1 = var8009a4e0[portalnum];
				} else {
					static s32 logged;

					if (logged++ < 4) {
						sysLogPrintf(LOG_WARNING, "portal: room %d lists portal %d, past the line cache's %d",
								roomnum, portalnum, s_PortalIsectCap);
					}
				}
#endif

				if (s1[0] != var8005ef20) {
					s1[0] = var8005ef20;
					s1[1] = portalCalculateIntersection(portalnum, pos1, pos2);
				}

				if (s1[1] != PORTALINTERSECTION_NONE) {
					if (s1[1] == PORTALINTERSECTION_BEHINDTOFRONT) {
						if (roomnum == g_BgPortals[portalnum].roomnum1) {
							portal00017dc4(rooms7c, g_BgPortals[portalnum].roomnum2);
							portal00017dc4(rooms5c, g_BgPortals[portalnum].roomnum2);
							s1[1] = PORTALINTERSECTION_NONE;
						}
					}

					if (s1[1] == PORTALINTERSECTION_FRONTTOBEHIND) {
						if (roomnum == g_BgPortals[portalnum].roomnum2) {
							portal00017dc4(rooms7c, g_BgPortals[portalnum].roomnum1);
							portal00017dc4(rooms5c, g_BgPortals[portalnum].roomnum1);
							s1[1] = PORTALINTERSECTION_NONE;
						}
					}
				}

				portalnums++;
			}
		}

		if (rooms7c[0] == -1) {
			break;
		}

		for (i = 0; i < 16; i++) {
			rooms9c[i] = rooms7c[i];

			if (rooms7c[i] == -1) {
				break;
			}
		}
	} while (rooms7c[0] != -1);

	for (i = 0; i < 7 && rooms9c[i] != -1; i++) {
		rooms2[i] = rooms9c[i];
	}

	rooms2[i] = -1;

	if (rooms3 != NULL) {
		for (i = 0; i < arg5; i++) {
			rooms3[i] = rooms5c[i];

			if (rooms5c[i] == -1) {
				break;
			}
		}

		rooms3[i] = -1;
	}
}
