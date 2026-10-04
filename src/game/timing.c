#include <ultra64.h>
#include "constants.h"
#include "game/timing.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "system.h"
#include "net/net.h"
extern s32 g_FixedStep;
#endif

void frametimeInit(void)
{
	g_Vars.thisframestartt = osGetCount();
	g_Vars.prevframestartt = g_Vars.thisframestartt;
}

void frametimeApply(s32 diffframe60, s32 diffframe240, s32 frametime)
{
	g_Vars.prevframestartt = g_Vars.thisframestartt;
	g_Vars.thisframestartt = frametime;

	g_Vars.diffframe60 = diffframe60;
	g_Vars.diffframe60f = diffframe60;
	g_Vars.diffframe60freal = PALUPF(g_Vars.diffframe60f);

	g_Vars.prevframestart240 = g_Vars.thisframestart240;
	g_Vars.thisframestart240 += diffframe240;
	g_Vars.diffframe240 = diffframe240;
	g_Vars.diffframe240f = diffframe240;
	g_Vars.diffframe240freal = PALUPF(g_Vars.diffframe240f);
}

void frametimeCalculate(void)
{
	u32 count;
	u32 diffframet;
	u32 diffframe60;
	u32 diffframe240;

	do {
		count = osGetCount();
		diffframet = count - g_Vars.thisframestartt;
		g_Vars.diffframet = diffframet;

		diffframe60 = (g_Vars.lostframetime60t + diffframet + CYCLES_PER_FRAME / 2) / CYCLES_PER_FRAME;
		diffframe240 = (g_Vars.lostframetime240t + diffframet + CYCLES_PER_FRAME / 2 / 4) / (CYCLES_PER_FRAME / 4);

#ifndef PLATFORM_N64
		if (g_TickExtraSleep) {
			sysSleep(EXTRA_SLEEP_TIME);
		}
#endif
	} while (g_Vars.mininc60 && diffframe60 < g_Vars.mininc60);

#ifndef PLATFORM_N64
	if (g_FixedStep) {
		// --fixed-step: every frame is exactly one tick whatever the clock
		// says, so a headless run with --rng-seed replays the same match
		diffframe60 = 1;
		diffframe240 = 4;
		g_Vars.lostframetime60t = 0;
		g_Vars.lostframetime240t = 0;
		diffframet = CYCLES_PER_FRAME;
	}
#endif

	g_Vars.lostframetime60t = g_Vars.lostframetime60t + diffframet - diffframe60 * CYCLES_PER_FRAME;
	g_Vars.lostframetime240t = g_Vars.lostframetime240t + diffframet - diffframe240 * (CYCLES_PER_FRAME / 4);

#ifdef PLATFORM_N64
	g_Vars.mininc60 = 1;
#else
	g_Vars.mininc60 = g_TickRateDiv;
#endif

	frametimeApply(diffframe60, diffframe240, count);
}

#ifndef PLATFORM_N64
/**
 * Netplay's tick clock. The accumulator counts in microseconds times 60, so
 * one tick is exactly 1000000 of it and nothing is lost to rounding: a tick
 * is 1000000/60 us, which is not a whole number. A client trims its clock by
 * rateppm parts per million to keep a command or two queued on the host;
 * the host passes 0. Integer all the way, so it never drifts.
 */
#define NETTICK_UNIT 1000000

static u64 s_NetTickLast;
static u64 s_NetTickAcc;
static u64 s_NetTickRem; // the ppm scaling's remainder, carried so a trim loses nothing

void frametimeNetReset(void)
{
	s_NetTickLast = sysGetMicroseconds();
	s_NetTickAcc = 0;
	s_NetTickRem = 0;
}

s32 frametimeNetTicksDue(s32 maxticks, s32 rateppm)
{
	const u64 now = sysGetMicroseconds();
	u64 elapsed = now - s_NetTickLast;
	s64 rate = 1000000 + (s64)rateppm;
	s32 n;

	s_NetTickLast = now;

	// --net-clock-test: a fixed number of ticks per presented frame, unpaced,
	// so its state hashes can be laid beside a stock --fixed-step run's
	if (g_NetClockTest > 0) {
		return g_NetClockTest;
	}

	if (rate < 1) {
		rate = 1;
	}

	// A stall of more than a second is clamped below anyway; keeping the
	// product small keeps it in range whatever the clock did
	if (elapsed > 1000000) {
		elapsed = 1000000;
	}

	{
		const u64 scaled = elapsed * 60 * (u64)rate + s_NetTickRem;

		s_NetTickAcc += scaled / 1000000;
		s_NetTickRem = scaled % 1000000;
	}
	n = (s32)(s_NetTickAcc / NETTICK_UNIT);
	s_NetTickAcc -= (u64)n * NETTICK_UNIT;

	if (n > maxticks) {
		// logged on the first drop, then at most once a second with a count
		static u64 lastlog = 0;
		static s32 drops = 0;

		drops++;

		if (lastlog == 0 || now - lastlog >= 1000000) {
			sysLogPrintf(LOG_NOTE, "net: %d ticks behind, dropped to %d (%d drops)", n, maxticks, drops);
			lastlog = now;
			drops = 0;
		}

		n = maxticks;
	}

	return n;
}

s32 frametimeNetUsToNextTick(void)
{
	const u64 left = NETTICK_UNIT - s_NetTickAcc;

	return (s32)((left + 59) / 60);
}

f32 frametimeNetAlpha(void)
{
	return (f32)s_NetTickAcc / (f32)NETTICK_UNIT;
}

/**
 * A pass of mainNetFrame: a sim tick is a stock frame of diff (1, 4), a
 * present-only pass one of (0, 0). mininc60 0 so nothing spins; the next
 * stock frametimeCalculate() puts it back.
 */
void frametimeNetApplyPass(s32 sim)
{
	frametimeApply(sim ? 1 : 0, sim ? 4 : 0, osGetCount());
	g_Vars.diffframet = sim ? CYCLES_PER_FRAME : 0;
	g_Vars.mininc60 = 0;
}
#endif

void func0f16cf8c(s32 arg0)
{
	// empty
}

void func0f16cf94(void)
{
	// empty
}
