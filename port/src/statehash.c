/**
 * A hash of the simulation's state, for telling whether two runs played the
 * same game (tools/ci/replaytest.sh, PLANS/OPTIMIZATIONS.md #2).
 *
 * Vertex counts off `--gfxstats` were the check before this: they catch a
 * divergence only once it reaches the screen, and a draw count carries a HUD
 * element that differs between identical runs. This reads the state itself,
 * at the top of the level tick before anything of that frame has run: the
 * RNG, every prop on the active and paused lists (type, position, first
 * room) and every chr in a slot (number, action, damage, position, where its
 * AI list has got to). Floats are hashed by their bits, so -0.0 and a last
 * ulp count.
 *
 * It only reads. A hash that changed the game would prove nothing.
 */

#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "statehash.h"

s32 g_StateHashEvery = 0;

extern u64 g_RngSeed;
extern struct chrdata *g_ChrSlots;
extern s32 g_NumChrSlots;

#define FNV_OFFSET 0xcbf29ce484222325ull
#define FNV_PRIME 0x100000001b3ull

static inline u64 hashBytes(u64 h, const void *p, size_t n)
{
	const u8 *b = p;

	while (n--) {
		h ^= *b++;
		h *= FNV_PRIME;
	}

	return h;
}

static inline u64 hashU32(u64 h, u32 v)
{
	return hashBytes(h, &v, sizeof(v));
}

static inline u64 hashF32(u64 h, f32 f)
{
	u32 v;
	memcpy(&v, &f, sizeof(v));
	return hashU32(h, v);
}

static u64 hashCoord(u64 h, const struct coord *c)
{
	h = hashF32(h, c->x);
	h = hashF32(h, c->y);
	return hashF32(h, c->z);
}

u64 stateHashCompute(void)
{
	u64 h = FNV_OFFSET;
	struct prop *prop;
	s32 count = 0;
	s32 i;

	h = hashU32(h, g_Vars.lvframenum);
	h = hashBytes(h, &g_RngSeed, sizeof(g_RngSeed));

	// activepropstail's next is pausedprops, so one walk covers both lists
	for (prop = g_Vars.activeprops; prop && count < 100000; prop = prop->next, count++) {
		h = hashU32(h, prop->type);
		h = hashCoord(h, &prop->pos);
		h = hashU32(h, (u32)(s32)prop->rooms[0]);
	}

	h = hashU32(h, count);

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0) {
			continue;
		}

		h = hashU32(h, (u32)chr->chrnum);
		h = hashU32(h, (u32)(s32)chr->actiontype);
		h = hashF32(h, chr->damage);
		h = hashF32(h, chr->maxdamage);
		h = hashU32(h, chr->aioffset);

		if (chr->prop) {
			h = hashCoord(h, &chr->prop->pos);
		}
	}

	return h;
}

void stateHashTick(void)
{
	if (g_StateHashEvery > 0 && g_Vars.lvframenum % g_StateHashEvery == 0) {
		sysLogPrintf(LOG_NOTE, "statehash: frame %d %016llx", g_Vars.lvframenum,
			(unsigned long long)stateHashCompute());
	}
}
