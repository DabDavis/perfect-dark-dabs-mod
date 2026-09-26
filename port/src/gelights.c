#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "modloader.h"
#include "xblastage.h"
#include "gelights.h"
#include "system.h"
#include "game/dlights.h"
#include "game/propsnd.h"
#include "game/shards.h"
#include "lib/rng.h"

#ifndef PLATFORM_N64

/**
 * GoldenEye's light fixtures on a converted level.
 *
 * The conversion (geconvert.c fixtureLights()) makes a Perfect Dark light of
 * each group of touching triangles drawn with one of GoldenEye's ten light
 * textures, as the rectangle round the group in its mean plane. A light is
 * then broken the way Perfect Dark breaks its own: a shot's line has to
 * cross that rectangle (lightsHandleHit()). That suits a flat panel in a
 * wall and nothing else. Caverns' hanging lamps are hexagonal prisms of
 * GoldenEye's HANGING_LAMP picture on every face, and the conversion only
 * collected the caps' G_TRI1 triangles, so each lamp is two lights of one
 * triangle each, 11 units across, at its top and bottom - and a shot at the
 * lamp's side stops on the glass before it reaches either one. They could
 * hardly ever be shot out, in either look (F3 20260926-094614, "can't shoot
 * out the lights xbla"), and when one was, nothing on the lamp changed:
 * Perfect Dark only takes the glare away and dims the room, where GoldenEye
 * darkens the whole fixture to a quarter (vertex colour >> 2) and sheds
 * glass off it (lightfixture.c lightFixtureBreak()).
 *
 * So on a remake stage a shot breaks a light when it lands in the fixture's
 * box - the light's rectangle grown by FIXTURE_MARGIN, which takes in the
 * lamp round the two caps (Caverns' lamps stand up to 12 units past them) -
 * and it breaks every light of the room within FIXTURE_SPREAD of that one
 * (GoldenEye darkens every fixture triangle within 100 units of one it has
 * darkened), so a lamp goes out whole. A broken
 * light darkens the room's vertices in its box: in the N64 look each vertex
 * has a colour of its own (the conversion gives every G_VTX load its own
 * G_COL), which goes to a quarter; an HD room (gebeanstage.c) has one
 * palette of at most 64 for the room, so the vertex takes the palette's
 * nearest to a quarter of its colour. Room data is read afresh whenever a
 * room loads, so geLightsRoomLoaded() darkens the fixtures already broken
 * again, as GoldenEye's redarken_lights_in_room() does.
 *
 * The conversion is left as it is: fixtureLights() sees only G_TRI1, where
 * GoldenEye draws most of a fixture with G_TRI4, but a whole lamp is a
 * closed prism whose normals sum to nothing, which that function drops, and
 * changing its lights changes every converted level's file.
 */

#define FIXTURE_MARGIN 16
#define FIXTURE_SPREAD 100
#define MAXROOMS 1024

// Whether the room as loaded is an HD one (a palette), set as it loads
static u8 roomIsHd[MAXROOMS];

static struct light *geLight(s32 roomnum, s32 i)
{
	return (struct light *)&g_BgLightsFileData[(g_Rooms[roomnum].lightindex + i) * 0x22];
}

/** The light's box in the room's space, grown by the margin. */
static void geLightBox(const struct light *light, s32 margin, s32 *lo, s32 *hi)
{
	for (s32 k = 0; k < 3; k++) {
		lo[k] = 0x7fffffff;
		hi[k] = -0x7fffffff;
	}

	for (s32 q = 0; q < 4; q++) {
		const s16 c[3] = { light->bbox[q].x, light->bbox[q].y, light->bbox[q].z };

		for (s32 k = 0; k < 3; k++) {
			if (c[k] < lo[k]) {
				lo[k] = c[k];
			}

			if (c[k] > hi[k]) {
				hi[k] = c[k];
			}
		}
	}

	for (s32 k = 0; k < 3; k++) {
		lo[k] -= margin;
		hi[k] += margin;
	}
}

static s32 geLightBoxHas(const s32 *lo, const s32 *hi, f32 x, f32 y, f32 z)
{
	return x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] && z <= hi[2];
}

static void geLightCentre(const struct light *light, f32 *c)
{
	c[0] = c[1] = c[2] = 0;

	for (s32 q = 0; q < 4; q++) {
		c[0] += light->bbox[q].x * 0.25f;
		c[1] += light->bbox[q].y * 0.25f;
		c[2] += light->bbox[q].z * 0.25f;
	}
}

/** Whether the vertex at v is in the box of a broken light of the room other than skip. */
static s32 geLightsVertexInBroken(s32 roomnum, const Vtx *v, s32 skip)
{
	for (s32 i = 0; i < g_Rooms[roomnum].numlights; i++) {
		const struct light *light = geLight(roomnum, i);
		s32 lo[3], hi[3];

		if (i == skip || light->healthy) {
			continue;
		}

		geLightBox(light, FIXTURE_MARGIN, lo, hi);

		if (geLightBoxHas(lo, hi, v->x, v->y, v->z)) {
			return 1;
		}
	}

	return 0;
}

/** The palette entry nearest a quarter of the colour at index. */
static s32 geLightsDarkEntry(const Col *pal, s32 num, s32 index)
{
	const s32 r = pal[index].r >> 2, g = pal[index].g >> 2, b = pal[index].b >> 2;
	s32 best = index;
	s32 bestdist = 0x7fffffff;

	for (s32 i = 0; i < num; i++) {
		const s32 dr = pal[i].r - r, dg = pal[i].g - g, db = pal[i].b - b;
		const s32 dist = dr * dr + dg * dg + db * db + (pal[i].a != pal[index].a ? 1 << 20 : 0);

		if (dist < bestdist) {
			bestdist = dist;
			best = i;
		}
	}

	return best;
}

/**
 * Darken the room's vertices in the box of light lightnum, or with lightnum
 * -1 in the box of every broken light; a vertex already in another broken
 * light's box has been darkened by it.
 */
static s32 geLightsDarken(s32 roomnum, s32 lightnum)
{
	struct roomgfxdata *gfx;
	s32 palette;
	s32 num = 0;

	if (roomnum <= 0 || roomnum >= g_Vars.roomcount || !g_Rooms[roomnum].loaded240
			|| !(gfx = g_Rooms[roomnum].gfxdata) || !gfx->vertices || !gfx->colours) {
		return 0;
	}

	// An HD room's colour byte is its index in the room's one palette; a
	// converted N64 room gives every vertex its own colour, at its own index
	palette = roomnum < MAXROOMS && roomIsHd[roomnum];

	if (!palette && gfx->numcolours < gfx->numvertices) {
		return 0;
	}

	for (s32 v = 0; v < gfx->numvertices; v++) {
		Vtx *vtx = &gfx->vertices[v];
		s32 in = 0;

		if (lightnum >= 0) {
			const struct light *light = geLight(roomnum, lightnum);
			s32 lo[3], hi[3];

			geLightBox(light, FIXTURE_MARGIN, lo, hi);
			in = geLightBoxHas(lo, hi, vtx->x, vtx->y, vtx->z) && !geLightsVertexInBroken(roomnum, vtx, lightnum);
		} else {
			in = geLightsVertexInBroken(roomnum, vtx, -1);
		}

		if (!in) {
			continue;
		}

		if (palette) {
			const s32 index = vtx->colour >> 2;

			if (index < gfx->numcolours) {
				vtx->colour = (u8)(geLightsDarkEntry(gfx->colours, gfx->numcolours < 64 ? gfx->numcolours : 64, index) << 2);
			}
		} else {
			gfx->colours[v].r >>= 2;
			gfx->colours[v].g >>= 2;
			gfx->colours[v].b >>= 2;
		}

		num++;
	}

	return num;
}

/** GoldenEye's glass off the fixture: a shard every 10 units or so over its box. */
static void geLightsShards(s32 roomnum, const struct light *light)
{
	s32 lo[3], hi[3];
	s32 n = 0;

	geLightBox(light, FIXTURE_MARGIN / 2, lo, hi);

	for (s32 x = lo[0]; x <= hi[0]; x += 10) {
		for (s32 y = lo[1]; y <= hi[1]; y += 10) {
			for (s32 z = lo[2]; z <= hi[2] && n < 24; z += 10) {
				struct coord pos;

				pos.x = x + g_BgRooms[roomnum].pos.x;
				pos.y = y + g_BgRooms[roomnum].pos.y;
				pos.z = z + g_BgRooms[roomnum].pos.z;

				shardCreate(roomnum, &pos, RANDOMFRAC() * M_BADTAU, 3.0f + RANDOMFRAC() * 3.0f, SHARDTYPE_GLASS);
				n++;
			}
		}
	}
}

bool geLightsHandleHit(struct coord *gunpos, struct coord *hitpos, s32 roomnum)
{
	struct coord from, at, dir;
	s32 hit = -1;
	f32 len;

	if (roomnum <= 0 || roomnum >= g_Vars.roomcount || g_Rooms[roomnum].numlights == 0) {
		return false;
	}

	from.x = gunpos->x - g_BgRooms[roomnum].pos.x;
	from.y = gunpos->y - g_BgRooms[roomnum].pos.y;
	from.z = gunpos->z - g_BgRooms[roomnum].pos.z;

	at.x = hitpos->x - g_BgRooms[roomnum].pos.x;
	at.y = hitpos->y - g_BgRooms[roomnum].pos.y;
	at.z = hitpos->z - g_BgRooms[roomnum].pos.z;

	dir.x = at.x - from.x;
	dir.y = at.y - from.y;
	dir.z = at.z - from.z;

	len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

	if (len <= 0) {
		return false;
	}

	// a hair into the surface, as lightsHandleHit() goes
	at.x += dir.x * 2.0f / len;
	at.y += dir.y * 2.0f / len;
	at.z += dir.z * 2.0f / len;

	for (s32 i = 0; i < g_Rooms[roomnum].numlights && hit < 0; i++) {
		struct light *light = geLight(roomnum, i);
		s32 lo[3], hi[3];

		if (!light->healthy || !light->vulnerable) {
			continue;
		}

		geLightBox(light, FIXTURE_MARGIN, lo, hi);

		if (geLightBoxHas(lo, hi, at.x, at.y, at.z)) {
			hit = i;
		}
	}

	if (hit < 0) {
		return false;
	}

	{
		struct light *first = geLight(roomnum, hit);
		struct coord soundpos;
		f32 c[3];

		geLightCentre(first, c);

		soundpos.x = c[0] + g_BgRooms[roomnum].pos.x;
		soundpos.y = c[1] + g_BgRooms[roomnum].pos.y;
		soundpos.z = c[2] + g_BgRooms[roomnum].pos.z;

		// the whole fixture: GoldenEye darkens every one of its triangles
		// within 100 units of one it has darkened
		for (s32 i = 0; i < g_Rooms[roomnum].numlights; i++) {
			struct light *light = geLight(roomnum, i);
			f32 d[3];

			if (!light->healthy || !light->vulnerable) {
				continue;
			}

			geLightCentre(light, d);

			if (i == hit || fabsf(d[0] - c[0]) + fabsf(d[1] - c[1]) + fabsf(d[2] - c[2]) < FIXTURE_SPREAD) {
				geLightsShards(roomnum, light);
				roomSetLightBroken(roomnum, i);
			}
		}

		psCreate(0, 0, SFX_HIT_GLASS, -1, -1, PSFLAG_0400, 0, PSTYPE_NONE, &soundpos, -1.0f, 0, roomnum, -1.0f, -1.0f, -1.0f);
	}

	return true;
}

void geLightsBroken(s32 roomnum, s32 lightnum, bool washealthy)
{
	if (washealthy && modloaderStageIsRemake(g_Vars.stagenum)) {
		const s32 num = geLightsDarken(roomnum, lightnum);

		sysLogPrintf(LOG_NOTE, "gelights: room %d light %d shot out, %d vertices darkened", roomnum, lightnum, num);
	}
}

void geLightsRoomLoaded(s32 roomnum)
{
	if (roomnum > 0 && roomnum < MAXROOMS) {
		// still inside bgLoadRoom(), before xblaStageRoomDone()
		roomIsHd[roomnum] = xblaStageIsRelease() != 0;
	}

	if (modloaderStageIsRemake(g_Vars.stagenum) && g_Rooms[roomnum].numlights > 0) {
		const s32 num = geLightsDarken(roomnum, -1);

		if (num > 0) {
			sysLogPrintf(LOG_NOTE, "gelights: room %d loaded, %d vertices of its broken fixtures darkened", roomnum, num);
		}
	}
}

#endif
