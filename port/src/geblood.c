/**
 * GoldenEye's blood: the wash that runs down the screen when Bond dies, and
 * down the lens when he fires in the gun barrel (the decomp's
 * src/game/blood_animation.c and blood_decrypt.c).
 *
 * One animation serves both. It is 42 frames of an 80x96 intensity picture,
 * each decoded from where the last one ended, and the conversion of the
 * player's ROM writes the encoded stream as menu/introblood.bin (geconvert.c).
 * GoldenEye draws a frame of it as a 4-bit intensity texture over the whole of
 * the screen it is on, tinted its own dark red at seven tenths.
 *
 * **The death screen.** Perfect Dark kept the whole of GoldenEye's death -
 * `redbloodfinished`, the red wash that follows the blood, the fade and the
 * death camera are all still in player.c - and dropped only the animation,
 * setting the flag on the first frame so the red wash comes at once. With the
 * conversion's blood, a death on one of the remake's missions or arenas runs
 * the way GoldenEye runs it (`geBloodDeathRender()`, from playerRenderHud()).
 *
 * **The rate.** GoldenEye steps the wash once a frame while the player dies,
 * which at its thirty frames a second is about a second and a half, and this
 * port ticks that at 60Hz, so it steps every two ticks (`GEBLOOD_TICKS`). The
 * gun barrel steps it once every two frames and the console draws that screen
 * at thirty as well, so there it takes 2.8 seconds - geintro.c counts those
 * four ticks itself.
 *
 * A dying player's frame buffer is allocated the first time they die and kept
 * (four of them at worst, 7680 bytes each): it cannot be freed when the wash
 * ends, since the display list built that frame still points at it and the
 * renderer reads it after the game thread has moved on.
 *
 * **The renderer keeps a texture by the address it was uploaded from**, and a
 * step writes the next frame into the same buffer - so without dropping it
 * (`videoFreeCachedTexture()`) the first frame of the wash is drawn for the
 * whole of it. That is why the gun barrel's blood barely showed: it was there
 * from the first frame, and it was one nearly empty picture held for a second
 * and a half.
 */
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "geblood.h"
#include "modloader.h"
#include "system.h"
#include "video.h"
#include "lib/vi.h"

// One frame of the wash every other tick: GoldenEye's own second and a half
#define GEBLOOD_TICKS 2

// the encoded animation, loaded once from whichever mod dir the conversion
// wrote it into
static u8 *g_BloodStream;
static u32 g_BloodStreamLen;
static s32 g_BloodSearchedDirs = -1;

// a dying player's own place in it
static struct geblood g_BloodDeath[MAX_PLAYERS];

s32 geBloodAvailable(void)
{
	char path[FS_MAXPATH + 1];

	if (g_BloodStream) {
		return 1;
	}

	// nothing found, and nothing new to look in: a mod swap is what changes
	// the count, and the wash is asked for often enough to matter
	if (g_BloodSearchedDirs == fsGetNumModDirs()) {
		return 0;
	}

	g_BloodSearchedDirs = fsGetNumModDirs();

	for (s32 i = 0; i < g_BloodSearchedDirs; i++) {
		const char *dir = fsGetModDirAt(i);

		if (!dir) {
			continue;
		}

		snprintf(path, sizeof(path), "%s/menu/introblood.bin", dir);

		// asked for first, since a load of a file that is not there is an
		// error line of its own and every mod dir but one has no menu/
		if (fsFileSize(path) > 0) {
			g_BloodStream = fsFileLoad(path, &g_BloodStreamLen);

			if (g_BloodStream) {
				return 1;
			}
		}
	}

	return 0;
}

/* ------------------------------------------------------------------------ */
/* the animation (blood_decrypt.c) */

/**
 * One frame of the wash, decoded from where the last one ended: runs of lit and
 * unlit texels down a column, or a count of solid ones and how many columns
 * repeat it. Returns where the next frame starts, NULL at the end.
 */
static const u8 *geBloodDecode(const u8 *in, const u8 *end, u8 *out)
{
	u8 *o = out;
	u8 *const olimit = out + GEBLOOD_W * GEBLOOD_H;
	s32 rows = GEBLOOD_H;
	u8 first;

	if (!in || in >= end) {
		return NULL;
	}

	first = *in++;

	do {
		u8 value = 0xff;
		u8 run;

		if (in >= end) {
			return NULL;
		}

		run = *in++;

		if (run == 0xff) {
			u8 written = 0;

			for (run = (in < end) ? *in++ : 0xff; run != 0xff; value ^= 0xff, run = (in < end) ? *in++ : 0xff) {
				written += run;

				while (run-- > 0 && o < olimit) {
					*o++ = value;
				}
			}

			while (written++ < GEBLOOD_W && o < olimit) {
				*o++ = value;
			}

			rows--;
		} else {
			u8 lit = first + (run & 0x1f);
			u8 columns = (run >> 5) + 1;

			rows -= columns;

			do {
				u8 n = lit;

				while (n-- > 0 && o < olimit) {
					*o++ = 0xff;
				}

				n = GEBLOOD_W - lit;

				while (n-- > 0 && o < olimit) {
					*o++ = 0;
				}
			} while (--columns > 0);
		}
	} while (rows > 0 && o < olimit);

	return in < end ? in : NULL;
}

/** The decoded frame is column major; the texture wants it row major. */
static void geBloodTranspose(const u8 *src, u8 *dst)
{
	for (s32 y = 0; y < GEBLOOD_H; y++) {
		for (s32 x = 0; x < GEBLOOD_W; x++) {
			dst[x * GEBLOOD_H + y] = src[y * GEBLOOD_W + x];
		}
	}
}

/** The two four-texel averages GoldenEye softens the wash with. */
static void geBloodBlur(u8 *p)
{
	for (s32 i = 1; i < GEBLOOD_W - 1; i++) {
		for (s32 j = 1; j < GEBLOOD_H - 1; j++) {
			const s32 at = i * GEBLOOD_H + j;
			p[at] = (p[at + 1] + p[at] + p[at + GEBLOOD_H + 1] + p[at + GEBLOOD_H] + 2) >> 2;
		}
	}

	for (s32 i = 1; i < GEBLOOD_W - 1; i++) {
		for (s32 j = 1; j < GEBLOOD_H - 1; j++) {
			const s32 at = i * GEBLOOD_H + j;
			p[at] = (p[at - 1] + p[at] + p[at - GEBLOOD_H - 1] + p[at - GEBLOOD_H] + 2) >> 2;
		}
	}
}

/**
 * The next frame of the wash into b->frame, ready to draw as a 4-bit intensity
 * texture (two texels a byte). `restart` begins it again.
 * (die_blood_image_routine(), whose 0 and 1 are the same two.)
 */
s32 geBloodStep(struct geblood *b, s32 restart)
{
	u8 *frame;

	if (!geBloodAvailable()) {
		return 1;
	}

	if (!b->frame) {
		b->frame = sysMemZeroAlloc(GEBLOOD_W * GEBLOOD_H);

		if (!b->frame) {
			return 1;
		}
	}

	if (restart) {
		b->next = g_BloodStream;
	}

	if (!b->next) {
		return 1;
	}

	frame = sysMemZeroAlloc(GEBLOOD_W * GEBLOOD_H);

	if (!frame) {
		return 1;
	}

	b->next = geBloodDecode(b->next, g_BloodStream + g_BloodStreamLen, frame);
	geBloodTranspose(frame, b->frame);
	geBloodBlur(b->frame);
	sysMemFree(frame);

	// two texels a byte, the high nibble first
	for (s32 i = 0; i < GEBLOOD_W * GEBLOOD_H / 2; i++) {
		b->frame[i] = (b->frame[i * 2] & 0xf0) | (b->frame[i * 2 + 1] >> 4);
	}

	// the frame the renderer holds for this buffer is the one before it
	videoFreeCachedTexture(b->frame);

	return b->next == NULL;
}

void geBloodDrop(struct geblood *b)
{
	if (b->frame) {
		videoFreeCachedTexture(b->frame);
		sysMemFree(b->frame);
	}

	b->frame = NULL;
	b->next = NULL;
	b->wait = 0;
}

/* ------------------------------------------------------------------------ */
/* the death screen */

/**
 * gameplayBloodOverlayDL(): the frame of the wash over the player's viewport,
 * a 4-bit intensity texture tinted GoldenEye's own dark red.
 */
static Gfx *geBloodDrawViewport(Gfx *gdl, const struct geblood *b)
{
	const s32 x0 = viGetViewLeft();
	const s32 y0 = viGetViewTop();
	const s32 w = viGetViewWidth();
	const s32 h = viGetViewHeight();

	if (!b->frame || w <= 0 || h <= 0) {
		return gdl;
	}

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetColorDither(gdl++, G_CD_MAGICSQ);
	gDPSetRenderMode(gdl++, G_RM_CLD_SURF, G_RM_CLD_SURF2);
	gDPSetCombineMode(gdl++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
	gDPSetPrimColor(gdl++, 0, 0, 0x96, 0x00, 0x00, 0xb4);
	gSPTexture(gdl++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON);
	gDPLoadTextureBlock_4b(gdl++, b->frame, G_IM_FMT_I, GEBLOOD_H, GEBLOOD_W, 0,
			G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
			G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
	gSPTextureRectangle(gdl++, x0 * 4, y0 * 4, (x0 + w) * 4 - 1, (y0 + h) * 4 - 1,
			G_TX_RENDERTILE, 0, 0, (GEBLOOD_H << 10) / w, (GEBLOOD_W << 10) / h);
	gDPPipeSync(gdl++);
	gDPSetColorDither(gdl++, G_CD_BAYER);
	gDPSetTexturePersp(gdl++, G_TP_PERSP);

	return gdl;
}

/**
 * Whether the player dying is on one of the remake's stages: a converted
 * GoldenEye mission, or one of its arenas while GE Plus is the mode. Perfect
 * Dark's own death is unchanged anywhere else.
 */
s32 geBloodDeathActive(void)
{
	if (!modloaderStageIsMission(g_Vars.stagenum)
			&& !(g_GexPlusMode && modloaderStageIsRemake(g_Vars.stagenum))) {
		return 0;
	}

	return geBloodAvailable();
}

void geBloodDeathStart(void)
{
	struct geblood *b = &g_BloodDeath[g_Vars.currentplayernum];

	geBloodStep(b, 1);
	b->wait = GEBLOOD_TICKS;
}

Gfx *geBloodDeathRender(Gfx *gdl)
{
	struct geblood *b = &g_BloodDeath[g_Vars.currentplayernum];

	// paused, the wash holds where it is: GoldenEye steps it on its own clock
	b->wait -= g_Vars.lvupdate60;

	if (b->wait <= 0) {
		b->wait = GEBLOOD_TICKS;

		if (geBloodStep(b, 0)) {
			g_Vars.currentplayer->redbloodfinished = true;
		}
	}

	return geBloodDrawViewport(gdl, b);
}
