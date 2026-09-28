/**
 * The first person hands, painted from the player's own character.
 *
 * Perfect Dark draws the player's hands from a hand file of their own - a
 * forearm and a hand on a skeleton of its own, with a joint for every finger,
 * which each gun's animation poses round its grip. A body has no fingers: its
 * hand is one piece, modelled closed. So the hand file keeps the shape and the
 * grip, and what it takes from the character is its paint: the sleeve is
 * repainted in the colour the character's own forearm is painted, and the hand
 * in the colour its own hand is, glove or skin (F3 20260926-204323 and the
 * user's follow-up: Mayday's bare dark arm, the Moonraker Elite's yellow, the
 * N64 look's own colours for a character).
 *
 * The colours are read off the mesh the character is drawn with in the look
 * the game is in (xblamesh.c's xblaMeshAnalyse(): the texel under each
 * triangle's middle times its vertex colour, by the bone it hangs off) - a
 * GoldenEye character is a Bean mesh in both looks, Bean's HD one or the N64
 * original it shipped beside it. Which of the hand's textures is the sleeve is
 * read off its own mesh the same way in the XBLA look (the ones on the
 * forearm's bone), and off its own lists in the N64 look (the textures whose
 * triangles are loaded under the forearm's matrix).
 *
 * The repaint happens in the renderer as the texture goes up (gfx_pc.cpp's
 * gfx_upload_texture()): each texel keeps its brightness against the
 * picture's mean and takes the character's colour, so a sleeve keeps its
 * folds and a hand its knuckles. A texture where both kinds of triangle meet
 * (a hand file painted as one picture) is left as it is.
 *
 * GoldenEye's characters only, by default: Perfect Dark's own have hands made
 * for them, and repainting those is no better - Cassandra's hand file is one
 * picture over hand and cuff alike, and came out gold. Mod.HandsMatchBody=2
 * paints everyone's, which is the way for a mod's characters; 0 turns it off, leaving gebean.c's table of Perfect
 * Dark's hands by outfit, which is what is drawn when no colours can be read
 * (a character from the ROM alone, with no release to take a mesh from).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "game/player.h"
#include "lib/model.h"
#include "system.h"
#include "video.h"
#include "xblamesh.h"
#include "gebean.h"
#include "handtint.h"

s32 g_HandTintMode = 1;

#define HANDTINT_MAX 24
#define XBLAMESH_MAXMTX_HT 64 // xblamesh.c's XBLAMESH_MAXMTX, the rows xblaMeshAnalysedTextures() hands back

struct handtint {
	const void *addr;
	u8 rgb[3];
};

static SDL_mutex *tintLock;
static struct handtint tints[HANDTINT_MAX];
static volatile s32 numTints;

// What the tints were made for, and how many more times to try when the
// character's mesh was not built yet
static s32 lastBody = -1;
static s32 lastLook = -1;
static const void *lastHandDef;
static const void *lastHandTex;  // where its textures were loaded, which moves with every gun load
static s32 lastHandFile;         // and which hand file it is: a new one can load at the same address,
                                 // its textures in the same places (Russian Soldier's hands, then
                                 // the Moonraker Elite's, kept the first's tints and drew unpainted)
static s32 lastMode = -1;
static s32 retries;
static s32 retryWait;

s32 handtintWantsMesh(s32 fileid, s32 ischr)
{
	if (g_HandTintMode <= 0) {
		return 0;
	}

	// every skinned mesh is read for its textures by bone - a hand's is how
	// its sleeve is told from its skin - and only a character asked for is
	// decoded for its colours
	return ischr && g_HandTintMode >= 2 ? 2 : 1;
}

s32 handtintLookup(const void *addr, u8 *rgb)
{
	s32 found = 0;

	if (numTints == 0 || !addr || !tintLock) {
		return 0;
	}

	SDL_LockMutex(tintLock);

	for (s32 i = 0; i < numTints; i++) {
		if (tints[i].addr == addr) {
			rgb[0] = tints[i].rgb[0];
			rgb[1] = tints[i].rgb[1];
			rgb[2] = tints[i].rgb[2];
			found = 1;
			break;
		}
	}

	SDL_UnlockMutex(tintLock);

	return found;
}

void handtintApply(u8 *rgba, u32 width, u32 height, const u8 *rgb)
{
	const size_t n = (size_t)width * height;
	f64 sum = 0.0;
	size_t count = 0;
	f32 mean;

	for (size_t i = 0; i < n; i++) {
		const u8 *p = rgba + i * 4;

		if (p[3] >= 32) {
			sum += 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
			count++;
		}
	}

	if (count == 0) {
		return;
	}

	mean = (f32)(sum / count);

	if (mean < 1.0f) {
		mean = 1.0f;
	}

	for (size_t i = 0; i < n; i++) {
		u8 *p = rgba + i * 4;
		const f32 lum = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / mean;

		for (s32 j = 0; j < 3; j++) {
			f32 v = rgb[j] * lum;

			p[j] = v > 255.0f ? 255 : (u8)v;
		}
	}
}

/**
 * The nearest position node above node.
 */
static struct modelnode *handtintParentPosition(struct modelnode *node)
{
	for (node = node ? node->parent : NULL; node; node = node->parent) {
		if ((node->type & 0xff) == MODELNODETYPE_POSITION) {
			return node;
		}
	}

	return NULL;
}

static s32 handtintMtx(struct modelnode *node)
{
	return node && (node->type & 0xff) == MODELNODETYPE_POSITION ? node->rodata->position.mtxindex0 : -1;
}

/**
 * The colour a character's forearms (which = 0) or hands (1) are painted,
 * from the mesh it is drawn with: both sides, weighted by how much of each
 * there is.
 */
static s32 handtintBodyColour(struct xblameshbuilt *m, struct modeldef *def, s32 which, f32 *out)
{
	static const s32 parts[] = { MODELPART_CHR_RIGHTHAND, MODELPART_CHR_LEFTHAND };
	f32 sum[3] = { 0, 0, 0 };
	f32 total = 0.0f;

	for (s32 i = 0; i < 2; i++) {
		struct modelnode *hand = modelGetPart(def, parts[i]);
		struct modelnode *node = which == 0 ? handtintParentPosition(hand) : hand;
		const s32 mtx = handtintMtx(node);
		f32 rgb[3];
		f32 weight;

		if (mtx >= 0 && xblaMeshAnalysedBoneColour(m, mtx, rgb, &weight)) {
			for (s32 j = 0; j < 3; j++) {
				sum[j] += rgb[j] * weight;
			}

			total += weight;
		}
	}

	if (total <= 0.0f) {
		return 0;
	}

	for (s32 j = 0; j < 3; j++) {
		out[j] = sum[j] / total;
	}

	return 1;
}

/**
 * Whether a hand file's matrix is a forearm's: a joint hanging straight off
 * the root that has joints of its own under it (the hand skeleton's 1 and 17,
 * each with a wrist and five fingers below).
 */
static s32 handtintIsForearm(struct modeldef *handdef, s32 mtx)
{
	struct modelnode *node = handdef->rootnode;

	while (node) {
		if ((node->type & 0xff) == MODELNODETYPE_POSITION && node->rodata->position.mtxindex0 == mtx) {
			struct modelnode *up = handtintParentPosition(node);
			struct modelnode *c;
			s32 haschild = 0;

			for (c = node->child; c; c = c->next) {
				if ((c->type & 0xff) == MODELNODETYPE_POSITION) {
					haschild = 1;
				}
			}

			return up && handtintMtx(up) == 0 && haschild;
		}

		if (node->child) {
			node = node->child;
			continue;
		}

		while (node) {
			if (node->next) {
				node = node->next;
				break;
			}

			node = node->parent;
		}
	}

	return 0;
}

struct handtintcount {
	const void *addr;
	u32 fore;
	u32 other;
};

#define HT_C0(pos, width) ((u32)(cmd->words.w0 >> (pos)) & ((1U << (width)) - 1))
#define HT_C1(pos, width) ((u32)(cmd->words.w1 >> (pos)) & ((1U << (width)) - 1))

/**
 * The hand file's lists, as the renderer would read them (gfx_pc.cpp's
 * gfx_run_dl()), for no more than which texture each triangle is drawn with
 * and whether its corners hang off a forearm: the matrix a vertex is loaded
 * under is the joint it moves with.
 */
static void handtintWalkList(struct modeldef *handdef, Gfx *cmd, uintptr_t seg5, s8 *slotmtx,
		s32 *curmtx, const void **curtex, const void **lastimg, struct handtintcount *counts, s32 *numcounts, s32 depth)
{
	for (s32 guard = 0; cmd && guard < 20000; guard++, cmd++) {
		const u32 op = (u32)(cmd->words.w0 >> 24);
		const uintptr_t w1 = cmd->words.w1;
		const uintptr_t addr = (w1 & 1) && ((w1 >> 24) & 0xf) == 5 ? seg5 + (w1 & 0xfffffe) : w1;

		switch (op) {
		case G_ENDDL & 0xff:
			return;
		case G_DL:
			if (depth < 4) {
				handtintWalkList(handdef, (Gfx *)addr, seg5, slotmtx, curmtx, curtex, lastimg, counts, numcounts, depth + 1);
			}

			if (HT_C0(16, 1)) {
				return;
			}
			break;
		case G_MTX:
			if ((w1 & 1) && ((w1 >> 24) & 0xf) == SPSEGMENT_MODEL_MTX) {
				*curmtx = (s32)((w1 & 0xfffffe) / sizeof(Mtx));
			}
			break;
		case G_VTX: {
			const s32 count = HT_C0(0, 16) / sizeof(Vtx);
			const s32 first = HT_C0(16, 4);

			for (s32 i = 0; i < count && first + i < 16; i++) {
				slotmtx[first + i] = (s8)*curmtx;
			}
			break;
		}
		case G_SETTIMG:
			*lastimg = (const void *)w1;
			break;
		case G_LOADBLOCK:
		case G_LOADTILE:
			*curtex = *lastimg;
			break;
		case G_TRI1 & 0xff:
		case G_TRI4 & 0xff: {
			s32 v[12];
			s32 n = 0;

			if (op == (G_TRI1 & 0xff)) {
				v[0] = HT_C1(16, 8) / 10;
				v[1] = HT_C1(8, 8) / 10;
				v[2] = HT_C1(0, 8) / 10;
				n = 3;
			} else {
				for (s32 t = 0; t < 4; t++) {
					const s32 x = HT_C1(t * 8, 4);
					const s32 y = HT_C1(t * 8 + 4, 4);
					const s32 z = HT_C0(t * 4, 4);

					if (x || y || z) {
						v[n++] = x;
						v[n++] = y;
						v[n++] = z;
					}
				}
			}

			if (*curtex && n) {
				s32 i;

				for (i = 0; i < *numcounts && counts[i].addr != *curtex; i++);

				if (i == *numcounts && i < HANDTINT_MAX) {
					counts[i].addr = *curtex;
					counts[i].fore = 0;
					counts[i].other = 0;
					(*numcounts)++;
				}

				if (i < *numcounts) {
					for (s32 k = 0; k < n; k++) {
						if (v[k] < 16 && slotmtx[v[k]] >= 0 && handtintIsForearm(handdef, slotmtx[v[k]])) {
							counts[i].fore++;
						} else {
							counts[i].other++;
						}
					}
				}
			}
			break;
		}
		}
	}
}

static void handtintWalkHands(struct modeldef *handdef, struct handtintcount *counts, s32 *numcounts)
{
	struct modelnode *node = handdef->rootnode;

	while (node) {
		if ((node->type & 0xff) == MODELNODETYPE_GUNDL && node->rodata->gundl.opagdl) {
			const uintptr_t seg5 = (uintptr_t)node->rodata->gundl.baseaddr;
			uintptr_t gdl = (uintptr_t)node->rodata->gundl.opagdl;
			s8 slotmtx[16];
			s32 curmtx = 0;
			const void *curtex = NULL;
			const void *lastimg = NULL;

			memset(slotmtx, -1, sizeof(slotmtx));

			if ((gdl & 1) && ((gdl >> 24) & 0xf) == 5) {
				gdl = seg5 + (gdl & 0xfffffe);
			}

			handtintWalkList(handdef, (Gfx *)gdl, seg5, slotmtx, &curmtx, &curtex, &lastimg, counts, numcounts, 0);
		}

		if (node->child) {
			node = node->child;
			continue;
		}

		while (node) {
			if (node->next) {
				node = node->next;
				break;
			}

			node = node->parent;
		}
	}
}

static void handtintSet(const struct handtint *next, s32 num)
{
	struct handtint old[HANDTINT_MAX];
	s32 numold;

	if (!tintLock) {
		tintLock = SDL_CreateMutex();

		if (!tintLock) {
			return;
		}
	}

	SDL_LockMutex(tintLock);
	numold = numTints;
	memcpy(old, tints, sizeof(old));
	memcpy(tints, next, num * sizeof(*next));
	numTints = num;
	SDL_UnlockMutex(tintLock);

	// the renderer keeps what it uploaded by address: both the textures that
	// were painted and the ones that are now are let go, so each comes back
	// up through the repaint (or without it)
	for (s32 i = 0; i < numold; i++) {
		videoEvictCachedTexture(old[i].addr);
	}

	for (s32 i = 0; i < num; i++) {
		videoEvictCachedTexture(next[i].addr);
	}
}

static void handtintToByte(const f32 *rgb, u8 *out)
{
	for (s32 j = 0; j < 3; j++) {
		const f32 v = rgb[j];

		out[j] = v <= 0.0f ? 0 : v >= 255.0f ? 255 : (u8)(v + 0.5f);
	}
}

/**
 * The tints for these hands on this character, or none. 1 when done (made,
 * or nothing to make), 0 when the character's mesh is not there yet.
 */
static s32 handtintMake(s32 bodynum, struct modeldef *handdef)
{
	struct handtint next[HANDTINT_MAX];
	struct modeldef *bodydef;
	struct xblameshbuilt *bodymesh;
	struct xblameshbuilt *handmesh;
	f32 sleeve[3];
	f32 skin[3];
	u8 sleeveb[3];
	u8 skinb[3];
	s32 num = 0;

	if (g_HandTintMode <= 0 || !handdef || bodynum < 0 || bodynum >= NUM_HEADSANDBODIES
			|| (g_HandTintMode == 1 && !gebeanIsGoldenEyeBody(bodynum))) {
		handtintSet(next, 0);
		return 1;
	}

	bodydef = g_HeadsAndBodies[bodynum].modeldef;

	if (!bodydef) {
		handtintSet(next, 0);
		return 0;
	}

	bodymesh = xblaMeshAnalysedForModel(bodydef);

	if (!bodymesh) {
		handtintSet(next, 0);
		return 0;
	}

	if (!handtintBodyColour(bodymesh, bodydef, 0, sleeve) || !handtintBodyColour(bodymesh, bodydef, 1, skin)) {
		handtintSet(next, 0);
		return 1;
	}

	handtintToByte(sleeve, sleeveb);
	handtintToByte(skin, skinb);

	// The hands in the XBLA look: 4J's mesh for the hand file, whose sleeve
	// is whatever texture hangs off the forearms
	handmesh = xblaMeshAnalysedForModel(handdef);

	if (handmesh) {
		const void **tex;
		const u16 *bones;
		const s32 numtex = xblaMeshAnalysedTextures(handmesh, &tex, &bones);

		for (s32 i = 0; i < numtex && num < HANDTINT_MAX; i++) {
			u32 fore = 0;
			u32 other = 0;

			for (s32 b = 0; b < XBLAMESH_MAXMTX_HT; b++) {
				const u32 c = bones[i * XBLAMESH_MAXMTX_HT + b];

				if (!c) {
					continue;
				}

				if (handtintIsForearm(handdef, b)) {
					fore += c;
				} else {
					other += c;
				}
			}

			// only a texture that is plainly one or the other
			if (fore * 4 >= (fore + other) * 3) {
				next[num].addr = tex[i];
				memcpy(next[num].rgb, sleeveb, 3);
				num++;
			} else if (other * 4 >= (fore + other) * 3) {
				next[num].addr = tex[i];
				memcpy(next[num].rgb, skinb, 3);
				num++;
			}
		}
	} else {
		// The N64 look: the hand file's own lists, read for which texture
		// each triangle is drawn with and which joints its corners are on
		struct handtintcount counts[HANDTINT_MAX];
		s32 numcounts = 0;

		handtintWalkHands(handdef, counts, &numcounts);

		for (s32 i = 0; i < numcounts && num < HANDTINT_MAX; i++) {
			const u32 fore = counts[i].fore;
			const u32 other = counts[i].other;

			if (fore * 4 >= (fore + other) * 3) {
				next[num].addr = counts[i].addr;
				memcpy(next[num].rgb, sleeveb, 3);
				num++;
			} else if (other * 4 >= (fore + other) * 3) {
				next[num].addr = counts[i].addr;
				memcpy(next[num].rgb, skinb, 3);
				num++;
			}
		}
	}

	sysLogPrintf(LOG_NOTE, "handtint: body %d's sleeve %02x%02x%02x, hands %02x%02x%02x, on %d of the hands' textures (%s look)",
			bodynum, sleeveb[0], sleeveb[1], sleeveb[2], skinb[0], skinb[1], skinb[2], num,
			handmesh ? "XBLA mesh" : "N64");

	handtintSet(next, num);

	return 1;
}

void handtintTick(void)
{
	struct player *player = g_Vars.currentplayer;
	struct modeldef *handdef;
	const void *handtex;
	s32 bodynum = -1;
	s32 headnum = -1;
	s32 look;

	if (!player || PLAYERCOUNT() != 1) {
		if (numTints) {
			handtintSet(NULL, 0);
		}

		return;
	}

	handdef = player->gunctrl.handmodeldef;
	handtex = handdef && handdef->numtexconfigs > 0 ? handdef->texconfigs[0].textureptr : NULL;
	playerChooseBodyAndHead(&bodynum, &headnum, NULL);
	// the body the player's model is, on GoldenEye's own rig for a GoldenEye pair (player.c)
	gebeanOwnRigPair(&bodynum, &headnum);
	look = xblaMeshGetEnabled();

	if (bodynum == lastBody && look == lastLook && (const void *)handdef == lastHandDef
			&& handtex == lastHandTex && player->gunctrl.handfilenum == lastHandFile
			&& g_HandTintMode == lastMode) {
		// the character's mesh may be built a moment after its hands load
		if (retries > 0 && --retryWait <= 0) {
			retries--;
			retryWait = 30;

			if (handtintMake(bodynum, handdef)) {
				retries = 0;
			}
		}

		return;
	}

	lastBody = bodynum;
	lastLook = look;
	lastHandDef = handdef;
	lastHandTex = handtex;
	lastHandFile = player->gunctrl.handfilenum;
	lastMode = g_HandTintMode;
	retries = 0;

	if (!handtintMake(bodynum, handdef)) {
		retries = 20;
		retryWait = 30;
	}
}
