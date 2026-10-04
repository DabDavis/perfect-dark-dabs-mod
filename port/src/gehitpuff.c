/**
 * GoldenEye's bullet puffs on a converted GoldenEye level.
 *
 * GoldenEye draws no sparks where a shot hits. It draws glass2.c's "bullet
 * sparks": one camera-facing square of smoke, turned to a random angle, that
 * plays an animation of pale frames and is gone in a fifth of a second. A shot
 * into a body makes one (chr.c's chrCreateHitPuffs(), sized by the part hit
 * from g_HitReactionTable) pulled 42 units back towards the shooter so it is
 * not inside the body, and half the time a second, bigger one of the other
 * animation 42 units beyond the hit, as if out of the back. A shot into a
 * wall (chrprop.c), an object (propobj.c) or a guard's shot that misses
 * (chraction.c) makes one of size 26. Nothing limits them by distance: the
 * console draws every one on screen, so a hit across a level still flashes -
 * which is what Perfect Dark's sparks, a few pixels a few metres out, do not
 * (F3 20260930-235506).
 *
 * Perfect Dark's chrEmitSparks() is chrCreateHitPuffs() rewritten (the 42 and
 * the coin are the same), so on a converted level it hands the hit here
 * instead and draws no sparks, as GoldenEye draws none.
 *
 * The frames are GoldenEye ROM images (oddtextures.c's s_explosion_smokeimages
 * and s_scattered_explosions) the conversion writes to the level's textures/
 * like geimpact.c's holes; a conversion from before they were written has
 * none, and its levels keep Perfect Dark's sparks. In the release's look each
 * frame is the release's own texture/sfx/hit<n> or backhit<n> - the decomp's
 * comments name the frames after them - bound at the loaded texture's address
 * the way geimpact.c binds its holes.
 */
#include <ultra64.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "modloader.h"
#include "system.h"
#include "video.h"
#include "game/camera.h"
#include "game/gfxmemory.h"
#include "game/tex.h"
#include "game/texdecompress.h"
#include "lib/mtx.h"
#include "lib/rng.h"
#include "gebean.h"
#include "geconvert.h"
#include "gehitpuff.h"
#include "xblamesh.h"
#include "xblatex.h"

#ifndef PLATFORM_N64

// glass2.c's BULLET_SPARKS_MAX: a puff with no free slot is not made
#define GEPUFF_MAX 20

// bullet_sparks_init()'s types: 1 the impact smoke, 3 the back puff
#define GEPUFF_SMOKE 0
#define GEPUFF_BALLS 1

#define GEPUFF_NUMFRAMES 11

struct gepuffanim {
	u8 first;          // into g_GePuffFrames
	u8 lifetime;       // in GoldenEye's 60Hz ticks
	f32 speed;         // frames a tick
	u8 r, g, b;        // g_BulletSparkColors[type]
};

static const struct gepuffanim g_GePuffAnims[] = {
	/* type 1 */ { 0, 11, 0.5f, 0xff, 0xff, 0xc8 },
	/* type 3 */ { 6,  9, 0.5f, 0xff, 0xff, 0xff },
};

// GoldenEye's image numbers (assets/images.def order), all 64x64 IA8
static const struct {
	u16 image;
	const char *bean;
} g_GePuffFrames[GEPUFF_NUMFRAMES] = {
	{ 2176, "hit5" },      // IMAGE_SMOKE1..6
	{ 2177, "hit10" },
	{ 2178, "hit15" },
	{ 2179, "hit20" },
	{ 2180, "hit25" },
	{ 2181, "hit30" },
	{ 2182, "backhit6" },  // IMAGE_SMOKEBALLS1..5
	{ 2183, "backhit12" },
	{ 2184, "backhit18" },
	{ 2185, "backhit24" },
	{ 2186, "backhit30" },
};

// chr.c's g_HitReactionTable by HITPART_* (which are GoldenEye's HIT_*):
// impact puff size, back puff size (0 for none)
static const struct {
	s16 hitpart;
	f32 size;
	f32 backsize;
} g_GePuffParts[] = {
	{ HITPART_LFOOT,    17, 34 },
	{ HITPART_LSHIN,    17, 39 },
	{ HITPART_LTHIGH,   21, 43 },
	{ HITPART_RFOOT,    17, 34 },
	{ HITPART_RSHIN,    17, 39 },
	{ HITPART_RTHIGH,   21, 43 },
	{ HITPART_PELVIS,   21, 52 },
	{ HITPART_HEAD,     21, 43 },
	{ HITPART_LHAND,    17, 34 },
	{ HITPART_LFOREARM, 17, 43 },
	{ HITPART_LBICEP,   21, 52 },
	{ HITPART_RHAND,    17, 34 },
	{ HITPART_RFOREARM, 17, 43 },
	{ HITPART_RBICEP,   21, 52 },
	{ HITPART_TORSO,    26, 60 },
	{ HITPART_GUN,      26, 0 },
	{ HITPART_HAT,      21, 0 },
};

struct gepuff {
	s32 age240;        // 0 and lifetime 0: free
	s32 life240;
	u8 anim;
	struct coord pos;
	f32 c, s;          // the corner's two components (unk1c, unk20)
	struct prop *prop; // the body it was shot out of, if any
};

static struct gepuff g_GePuffs[GEPUFF_MAX];
static struct textureconfig g_GePuffConfigs[GEPUFF_NUMFRAMES];
static s32 g_GePuffOn;

static const void *g_GePuffBound[GEPUFF_NUMFRAMES];
static s32 g_GePuffBoundHd[GEPUFF_NUMFRAMES];

void geHitPuffStageStart(s32 stagenum)
{
	const char *dir;
	char path[FS_MAXPATH + 1];

	for (s32 i = 0; i < GEPUFF_NUMFRAMES; i++) {
		if (g_GePuffBound[i]) {
			xblaTexForgetPicture(g_GePuffBound[i]);
			g_GePuffBound[i] = NULL;
		}

		g_GePuffBoundHd[i] = 0;
	}

	memset(g_GePuffs, 0, sizeof(g_GePuffs));
	g_GePuffOn = false;

	for (s32 i = 0; i < GEPUFF_NUMFRAMES; i++) {
		struct textureconfig *tc = &g_GePuffConfigs[i];

		memset(tc, 0, sizeof(*tc));
		tc->texturenum = geconvertTexRemap(g_GePuffFrames[i].image);
		tc->width = 0x40;
		tc->height = 0x40;
		tc->level = 0;
		tc->format = G_IM_FMT_IA;
		tc->depth = G_IM_SIZ_8b;
		tc->s = G_TX_WRAP;
		tc->t = G_TX_WRAP;
	}

	if (!modloaderStageIsRemake(stagenum) || (dir = modloaderGetStageModDir(stagenum)) == NULL) {
		return;
	}

	for (s32 i = 0; i < GEPUFF_NUMFRAMES; i++) {
		snprintf(path, sizeof(path), "%s/textures/%04x.bin", dir, geconvertTexRemap(g_GePuffFrames[i].image));

		if (fsFileSize(path) <= 0) {
			sysLogPrintf(LOG_NOTE, "gehitpuff: stage 0x%02x: the conversion has no image %d, Perfect Dark's sparks",
					stagenum, g_GePuffFrames[i].image);
			return;
		}
	}

	g_GePuffOn = true;
}

s32 geHitPuffActive(void)
{
	return g_GePuffOn;
}

static f32 geHitPuffRandom(void)
{
	return (f32)rngRandom() * (1.0f / 4294967296.0f);
}

// glass2.c's bullet_spark_create() + bullet_sparks_init()
static void geHitPuffCreate(struct coord *pos, s32 anim, f32 size, struct prop *prop)
{
	for (s32 i = 0; i < GEPUFF_MAX; i++) {
		struct gepuff *p = &g_GePuffs[i];

		if (p->life240 == 0) {
			f32 angle = geHitPuffRandom() * (f32)(M_PI * 2);

			p->age240 = 0;
			p->life240 = g_GePuffAnims[anim].lifetime * 4;
			p->anim = anim;
			p->pos = *pos;
			p->prop = prop;

			size *= 1.0f + geHitPuffRandom() * 0.25f;
			size *= 1.41421356f;

			p->c = cosf(angle) * size;
			p->s = sinf(angle) * size;
			return;
		}
	}
}

void geHitPuffChr(struct prop *chrprop, s32 hitpart, struct coord *hitpos, struct coord *dir)
{
	f32 size = 0;
	f32 backsize = 0;
	struct coord pos;
	struct coord d;
	f32 len;

	// the shot's way in; GoldenEye's is the view ray, from the camera
	if (dir) {
		d = *dir;
	} else {
		d.x = hitpos->x - g_Vars.currentplayer->cam_pos.x;
		d.y = hitpos->y - g_Vars.currentplayer->cam_pos.y;
		d.z = hitpos->z - g_Vars.currentplayer->cam_pos.z;
	}

	len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);

	if (len <= 0.0f) {
		return;
	}

	d.x /= len;
	d.y /= len;
	d.z /= len;
	dir = &d;

	// Perfect Dark's chrs' own shots name no part (HITPART_GENERAL): a
	// GoldenEye shot at a body with no part hit the chest
	if (hitpart == HITPART_GENERAL || hitpart == HITPART_GENERALHALF) {
		hitpart = HITPART_TORSO;
	}

	for (s32 i = 0; i < ARRAYCOUNT(g_GePuffParts); i++) {
		if (g_GePuffParts[i].hitpart == hitpart) {
			size = g_GePuffParts[i].size;
			backsize = g_GePuffParts[i].backsize;
			break;
		}
	}

	if (backsize > 0 && (rngRandom() & 4) == 0) {
		pos.x = hitpos->x + dir->x * 42.0f;
		pos.y = hitpos->y + dir->y * 42.0f;
		pos.z = hitpos->z + dir->z * 42.0f;

		geHitPuffCreate(&pos, GEPUFF_BALLS, backsize, chrprop);
	}

	if (size > 0) {
		pos.x = hitpos->x - dir->x * 42.0f;
		pos.y = hitpos->y - dir->y * 42.0f;
		pos.z = hitpos->z - dir->z * 42.0f;

		geHitPuffCreate(&pos, GEPUFF_SMOKE, size, chrprop);
	}
}

void geHitPuffBg(struct coord *hitpos, struct coord *dir, f32 back)
{
	struct coord pos = *hitpos;

	if (dir) {
		pos.x -= dir->x * back;
		pos.y -= dir->y * back;
		pos.z -= dir->z * back;
	}

	geHitPuffCreate(&pos, GEPUFF_SMOKE, 26.0f, NULL);
}

void geHitPuffTick(void)
{
	if (!g_GePuffOn) {
		return;
	}

	for (s32 i = 0; i < GEPUFF_MAX; i++) {
		struct gepuff *p = &g_GePuffs[i];

		if (p->life240 > 0) {
			p->age240 += g_Vars.lvupdate240;

			if (p->age240 >= p->life240) {
				p->life240 = 0;
			}
		}
	}
}

static s32 geHitPuffReleaseLook(void)
{
	return gebeanGetEnabled() && xblaMeshGetEnabled();
}

static struct textureconfig *geHitPuffConfig(s32 i)
{
	struct textureconfig *tc = &g_GePuffConfigs[i];
	const s32 hd = geHitPuffReleaseLook();

	if ((u32)tc->texturenum < NUM_TEXTURES) {
		texLoadFromConfigs(tc, 1, NULL, 0);
	}

	if (tc->unk0b != 1) {
		return tc;
	}

	// the release's frame in its look, the ROM's otherwise; F6 flips it
	if (hd != g_GePuffBoundHd[i]) {
		g_GePuffBoundHd[i] = hd;

		if (g_GePuffBound[i]) {
			xblaTexForgetPicture(g_GePuffBound[i]);
			g_GePuffBound[i] = NULL;
		}

		if (hd) {
			char source[64];
			s32 w = 0, h = 0;
			u8 *rgba;

			snprintf(source, sizeof(source), "texture/sfx/%s", g_GePuffFrames[i].bean);
			rgba = gebeanDecodePictureFile(source, &w, &h);

			if (rgba && xblaTexBindPictureAt(tc->textureptr, rgba, w, h)) {
				g_GePuffBound[i] = tc->textureptr;
			}
		}

		// the renderer's copy alone: the registry still names the frame the
		// conversion's (gemonitor.c's geMonitorBindPicture(), geimpact.c)
		videoEvictCachedTexture(tc->textureptr);
	}

	return tc;
}

// glass2.c's bullet_spark_render(): a square on the camera's axes, its
// corners at (c, s) turned, in its frame of the animation, depth tested and
// never written
Gfx *geHitPuffRender(Gfx *gdl)
{
	Mtxf *cam;
	bool setup = false;

	if (!g_GePuffOn) {
		return gdl;
	}

	cam = camGetProjectionMtxF();

	for (s32 i = 0; i < GEPUFF_MAX; i++) {
		struct gepuff *p = &g_GePuffs[i];
		const struct gepuffanim *anim;
		struct textureconfig *tc;
		Vtx *vertices;
		Col *colours;
		Mtxf mtxf;
		Mtxf *mtx;
		f32 s0[3], s1[3], s2[3], s3[3];
		s32 frame;

		if (p->life240 <= 0) {
			continue;
		}

		// a body's own puff is never in its own eyes: GoldenEye's guards
		// make none on Bond
		if (p->prop && p->prop == g_Vars.currentplayer->prop) {
			continue;
		}

		anim = &g_GePuffAnims[p->anim];
		frame = (s32)((f32)p->age240 * 0.25f * anim->speed);

		if (frame > (p->anim == GEPUFF_SMOKE ? 5 : 4)) {
			frame = p->anim == GEPUFF_SMOKE ? 5 : 4;
		}

		tc = geHitPuffConfig(anim->first + frame);

		if (tc->unk0b != 1) {
			continue;
		}

		if (!setup) {
			setup = true;
			gSPClearGeometryMode(gdl++, G_CULL_BOTH | G_FOG | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
			gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);
		}

		texSelect(&gdl, tc, 4, 1, 2, true, NULL);
		gDPSetColorDither(gdl++, G_CD_DISABLE);
		gDPSetAlphaCompare(gdl++, G_AC_NONE);

		colours = gfxAllocateColours(1);
		colours[0].r = anim->r;
		colours[0].g = anim->g;
		colours[0].b = anim->b;
		colours[0].a = 0xff;

		// the corners in eighths of a unit about the puff, the matrix
		// carries it out to the puff and the screen
		for (s32 k = 0; k < 3; k++) {
			s0[k] = cam->m[0][k] * p->c * 8.0f;
			s1[k] = cam->m[0][k] * p->s * 8.0f;
			s2[k] = cam->m[1][k] * p->c * 8.0f;
			s3[k] = cam->m[1][k] * p->s * 8.0f;
		}

		vertices = gfxAllocateVertices(4);

		for (s32 k = 0; k < 4; k++) {
			vertices[k].flags = 0;
			vertices[k].colour = 0;
		}

		vertices[0].x = -s0[0] - s3[0];
		vertices[0].y = -s0[1] - s3[1];
		vertices[0].z = -s0[2] - s3[2];
		vertices[0].s = tc->width << 5;
		vertices[0].t = 0;

		vertices[1].x = s1[0] - s2[0];
		vertices[1].y = s1[1] - s2[1];
		vertices[1].z = s1[2] - s2[2];
		vertices[1].s = 0;
		vertices[1].t = 0;

		vertices[2].x = s0[0] + s3[0];
		vertices[2].y = s0[1] + s3[1];
		vertices[2].z = s0[2] + s3[2];
		vertices[2].s = 0;
		vertices[2].t = tc->height << 5;

		vertices[3].x = -s1[0] + s2[0];
		vertices[3].y = -s1[1] + s2[1];
		vertices[3].z = -s1[2] + s2[2];
		vertices[3].s = tc->width << 5;
		vertices[3].t = tc->height << 5;

		mtx4LoadIdentity(&mtxf);
		mtxf.m[0][0] = 0.125f;
		mtxf.m[1][1] = 0.125f;
		mtxf.m[2][2] = 0.125f;
		mtx4SetTranslation(&p->pos, &mtxf);
		mtx00015be0(camGetWorldToScreenMtxf(), &mtxf);

		mtx = gfxAllocateMatrix();
		mtxF2L(&mtxf, mtx);

		gSPMatrix(gdl++, osVirtualToPhysical(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
		gSPColor(gdl++, osVirtualToPhysical(colours), 1);
		gSPVertex(gdl++, osVirtualToPhysical(vertices), 4, 0);
		gSPTri2(gdl++, 0, 1, 2, 0, 2, 3);
	}

	if (setup) {
		gDPSetColorDither(gdl++, G_CD_BAYER);
	}

	return gdl;
}

#endif
