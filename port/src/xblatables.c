/**
 * The game's tables as the XBLA release has them, while the release is on.
 * See xblatables.h for what changes and xblatablesdata.h for the values.
 *
 * Checked every frame rather than when the switch moves: the switch has more
 * than one way to move (F6, each checkbox on the page, the settings preset),
 * and mod.c copies its snapshot of g_HeadsAndBodies back over the table on a
 * mod load. A field is only moved from the value the other mode expects, so
 * whatever a mod set is left alone, and a snapshot copied back is simply put
 * right again the next frame. It is thirty-odd compares.
 */

#include <stdint.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "lib/vi.h"
#include "game/env.h"
#include "xblaswitch.h"
#include "xblatables.h"

#ifndef PLATFORM_N64

extern struct smoketype g_SmokeTypes[];
extern struct fogenvironment g_FogEnvironments[];
extern struct fogenvironment *g_EnvOrigFogEnvironment;

struct xblatabfield {
	void *field;
	u8 size;
	u32 n64;    // the field's raw bits, as many as it has
	u32 xbla;
};

struct xblatabtype {
	u8 index;   // into g_HeadsAndBodies
	u8 n64;
	u8 xbla;
};

#define XBLATAB(f, n64, xbla) { &(f), sizeof(f), (n64), (xbla) }

#include "xblatablesdata.h"

// No stock fog table row lies past this; only used to tell a stock row's
// field from anything else.
#define XBLATAB_FOGROWS 32

static s32 applied;

static u32 xblaTabRead(const struct xblatabfield *f)
{
	if (f->size == 1) {
		u8 v;
		memcpy(&v, f->field, 1);
		return v;
	}

	if (f->size == 2) {
		u16 v;
		memcpy(&v, f->field, 2);
		return v;
	}

	u32 v;
	memcpy(&v, f->field, 4);
	return v;
}

static void xblaTabWrite(const struct xblatabfield *f, u32 value)
{
	if (f->size == 1) {
		const u8 v = value;
		memcpy(f->field, &v, 1);
	} else if (f->size == 2) {
		const u16 v = value;
		memcpy(f->field, &v, 2);
	} else {
		memcpy(f->field, &value, 4);
	}
}

static s32 xblaTabIsStockFog(const void *p)
{
	const uintptr_t lo = (uintptr_t)&g_FogEnvironments[0];
	const uintptr_t hi = lo + XBLATAB_FOGROWS * sizeof(struct fogenvironment);

	return (uintptr_t)p >= lo && (uintptr_t)p < hi;
}

/**
 * The stage being played takes a changed fog row at its next load; this is for
 * the one it is already in. Only the draw range and the fog's own range are
 * set, the way envApplyFogEnvironment() sets them, so nothing the port layers
 * over the environment afterwards is reset.
 */
static void xblaTabReapplyFog(void)
{
	struct fogenvironment *env = g_EnvOrigFogEnvironment;

	if (!env || !xblaTabIsStockFog(env) || env->stage != g_Vars.stagenum) {
		return;
	}

	viSetZRange(env->near, env->far);
	envGetCurrent()->fogmin = env->fogmin;
	envGetCurrent()->fogmax = env->fogmax;
	envTick();
}

void xblaTablesTick(void)
{
	const s32 want = xblaSwitchGetEnabled() ? 1 : 0;
	s32 fields = 0;
	s32 types = 0;
	s32 fog = 0;

	for (u32 i = 0; i < ARRAYCOUNT(xblaTabFields); i++) {
		const struct xblatabfield *f = &xblaTabFields[i];

		if (xblaTabRead(f) == (want ? f->n64 : f->xbla)) {
			xblaTabWrite(f, want ? f->xbla : f->n64);
			fields++;
			fog |= xblaTabIsStockFog(f->field);
		}
	}

	for (u32 i = 0; i < ARRAYCOUNT(xblaTabTypes); i++) {
		const struct xblatabtype *t = &xblaTabTypes[i];

		if (g_HeadsAndBodies[t->index].type == (want ? t->n64 : t->xbla)) {
			g_HeadsAndBodies[t->index].type = want ? t->xbla : t->n64;
			types++;
		}
	}

	if (fog) {
		xblaTabReapplyFog();
	}

	if (fields || types) {
		sysLogPrintf(LOG_NOTE, "xblatables: %s: %d of %d fields, %d of %d head/body types%s",
				want ? "the release's tables" : "the N64's tables",
				fields, (s32)ARRAYCOUNT(xblaTabFields), types, (s32)ARRAYCOUNT(xblaTabTypes),
				want == applied ? " (put back after something reset them)" : "");
	}

	applied = want;
}

s32 xblaTablesGetApplied(void)
{
	return applied;
}

#else

void xblaTablesTick(void) { }
s32 xblaTablesGetApplied(void) { return 0; }

#endif
