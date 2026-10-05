#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "game/timing.h"
#include "lib/joy.h"
#include "system.h"
#include "input.h"
#include "net/net.h"
#include "net/nettransport.h"
#include "netint.h"

/**
 * Netplay's core state and the calls the game's hooks make (net.h). This
 * file is the game's side: it sees types.h's bool, so it never includes
 * ENet; the transport is reached through nettransport.h's plain C face.
 *
 * Phase 2 (PLANS/netplay/spec-tick.md): the fixed tick, one pad sample per
 * tick, the mouse summed over a frame and spent by one tick, the host's slow
 * motion, and --dedicated. The session itself (--host/--connect, rules,
 * stage, barrier, match end) is netsession.c. Commands and snapshots come
 * later; their calls are here as the stubs the main loop already makes.
 */

s32 g_NetMode = NETMODE_NONE;
s32 g_NetPass = NETPASS_NONE;
s32 g_NetTicksThisFrame = 0;
s32 g_NetDedicated = 0;
u32 g_NetTick = 0;
s32 g_NetLocalSlot = 0;
s32 g_NetInStageLoop = 0;
volatile sig_atomic_t g_NetQuit = 0;
s32 g_NetHostRenderAtTickRate = 0;
s32 g_NetClockTest = 0;

extern s32 g_StageNum;

// The host's lvupdate240 for the tick: what a client plays its ticks at
static s32 g_NetHostSocketLvupdate240 = 4;

// Raw mouse counts summed since the last tick spent them
static s32 s_NetMouseDX = 0;
static s32 s_NetMouseDY = 0;

static s32 s_NetSignalsInstalled = 0;

/**
 * The first SIGINT/SIGTERM/SIGHUP asks for a clean stop at the top of the
 * next loop pass (netCheckQuit); a second one, sent because that never came,
 * takes the default action.
 */
static void netHandleSignal(int sig)
{
	if (g_NetQuit) {
		signal(sig, SIG_DFL);
		raise(sig);
		return;
	}

	g_NetQuit = 1;
}

/**
 * Once per main loop pass on a dedicated server, stock loop and stage loop
 * alike. The handlers go in on the first call, so until mainLoop runs (the
 * ROM conversion, mod mounting) a signal still stops the process outright.
 */
void netCheckQuit(void)
{
	if (!g_NetDedicated) {
		return;
	}

	if (!s_NetSignalsInstalled) {
		s_NetSignalsInstalled = 1;
		signal(SIGINT, netHandleSignal);
		signal(SIGTERM, netHandleSignal);
#ifdef SIGHUP
		signal(SIGHUP, netHandleSignal);
#endif
	}

	if (g_NetQuit) {
		sysLogPrintf(LOG_NOTE, "net: asked to quit");
		exit(0);
	}
}

void netInitArgs(void)
{
	g_NetDedicated = sysArgCheck("--dedicated");

	// --net-clock-test N: offline, N tick passes per presented frame through
	// netplay's main loop, for laying its --state-hash lines beside a stock
	// --fixed-step run's (tools/ci/netclocktest.sh)
	g_NetClockTest = sysArgGetInt("--net-clock-test", 0);

	if (g_NetClockTest < 0) {
		g_NetClockTest = 0;
	} else if (g_NetClockTest > 8) {
		g_NetClockTest = 8;
	}

	if (g_NetDedicated) {
		g_NetMode = NETMODE_SERVER;
		g_NetLocalSlot = -1;
		g_NetHostRenderAtTickRate = 1;

		sysLogPrintf(LOG_NOTE, "net: dedicated server: no window, sound or local input");
	} else if (g_NetClockTest) {
		g_NetMode = NETMODE_SERVER;
		g_NetLocalSlot = 0;
		g_NetHostRenderAtTickRate = 1;

		sysLogPrintf(LOG_NOTE, "net: clock test, %d ticks per presented frame", g_NetClockTest);
	}

	// --net-ticket-selftest: the lobby ticket check against pdlobbyd's vector
	if (sysArgCheck("--net-ticket-selftest")) {
		const s32 fail = netTicketSelfTest();

		sysLogPrintf(fail ? LOG_ERROR : LOG_NOTE, "net: ticket self test %s (%d)", fail ? "FAILED" : "passed", fail);
		fflush(stdout);
		exit(fail ? 1 : 0);
	}

	// --host [port] / --connect addr[:port] (the socket opens in netSessionInit)
	netSessionArgs();
	netPlayersArgs();
}

/**
 * Whether mainLoop runs this stage through mainNetFrame. It must be false
 * on the title, the menus and the lobby, which keep the stock loop and its
 * input. Sets g_NetInStageLoop, which moves the pad read into the ticks.
 */
s32 netSessionInStage(void)
{
	s32 instage = 0;

	if (g_NetDedicated || g_NetClockTest) {
		instage = STAGE_IS_LEVEL(g_StageNum);
	}

	// a net match's stage, host and client alike
	if (netSessionMatchActive()) {
		instage = 1;
	}

	g_NetInStageLoop = instage;

	return instage;
}

/**
 * After lvReset, on every stage: the tick count, the clock and the mouse sum
 * start over. Only here: a readiness dip mid-stage resets the clock alone
 * (frametimeNetReset), never g_NetTick, which commands and snapshots key on.
 */
void netStageStart(void)
{
	g_NetTick = 0;
	g_NetInStageLoop = 0;
	s_NetMouseDX = 0;
	s_NetMouseDY = 0;
	frametimeNetReset();
}

s32 netStageReady(void)
{
	return 1;
}

// The client's clock trim in parts per million (0 on the host)
s32 netClockPpm(void)
{
	return netPlayersClockPpm();
}

static void netHandleEvent(const struct netevent *ev)
{
	switch (ev->type) {
	case NETEVENT_CONNECT:
		sysLogPrintf(LOG_NOTE, "net: peer %d connected", ev->peer);
		break;
	case NETEVENT_DISCONNECT:
		sysLogPrintf(LOG_NOTE, "net: peer %d gone%s", ev->peer, ev->timedout ? " (timed out)" : "");
		break;
	default:
		break;
	}

	netSessionEvent(ev);
}

// Everything queued, without waiting; once per loop
void netPump(void)
{
	struct netevent ev;

	if (!g_NetHostSocket) {
		return;
	}

	while (netHostService(g_NetHostSocket, &ev, 0) > 0) {
		netHandleEvent(&ev);
	}

	netSessionTick();
}

void netFlush(void)
{
	if (g_NetHostSocket) {
		netHostFlush(g_NetHostSocket);
	}
}

/**
 * Sleeps until the next tick is due, waking for a packet. Rounded up to a
 * whole millisecond: under 1 ms truncates to 0, which would spin.
 */
void netWait(s32 us)
{
	struct netevent ev;

	if (us < 1) {
		us = 1;
	}

	if (g_NetHostSocket) {
		if (netHostService(g_NetHostSocket, &ev, (u32)((us + 999) / 1000)) > 0) {
			netHandleEvent(&ev);
			netPump();
		}
	} else {
		sysSleep((s64)us * 10);
	}
}

/**
 * One pad sample per tick (spec-tick.md §3d): held timers then advance once
 * a tick, and a press is seen by one tick only. A sample the stock loop read
 * before this stage loop took over stands for the first tick.
 */
void netTickReadPad(void)
{
	// only the local read is skipped for a pending sample: whatever this
	// function later writes into the tick's sample (remote players' pads,
	// spec-tick.md §3d) must run on every tick pass, the first included
	if (!joyHasPendingSample()) {
		joyStartReadData(&g_PiMesgQueue);
		joyReadData();
	}

	// the host: each remote player's command for the tick into its pad
	netPlayersTickReadPad();
}

void netTickBegin(void)
{
	netSessionTickBegin();
	netPlayersTickBegin();
}

void netTickEnd(void)
{
	netPlayersTickEnd();

	// the barrier's ticks are not the match's: everyone starts from 0 at GO
	if (!netSessionBarrierHeld()) {
		g_NetTick++;
	}

	// the tick has spent the frame's mouse; any further tick sees none
	s_NetMouseDX = 0;
	s_NetMouseDY = 0;
}

void netPosePuppets(f32 alpha)
{
}

s32 netClientLvupdate240(void)
{
	return g_NetHostSocketLvupdate240;
}

void netHostSetLvupdate240(s32 lvupdate240)
{
	g_NetHostSocketLvupdate240 = lvupdate240;
}

s32 netDiscardPass(s32 playernum)
{
	return !netIsLocalSlot(playernum);
}

void netInputAccumulateMouse(void)
{
	s32 dx;
	s32 dy;

	if (!g_NetInStageLoop) {
		return;
	}

	inputMouseGetRawDelta(&dx, &dy);
	s_NetMouseDX += dx;
	s_NetMouseDY += dy;
}

/**
 * The mouse a getter reads in the stage loop: none on a present-only pass,
 * otherwise everything since the last tick, which netTickEnd clears (a pass
 * can read it several times: bondmove, the slayer, the eyespy, the menu)
 */
void netInputMouseRaw(s32 *dx, s32 *dy)
{
	if (!g_NetInStageLoop) {
		return;
	}

	if (g_NetPass == NETPASS_PRESENT_ONLY) {
		*dx = 0;
		*dy = 0;
	} else {
		*dx = s_NetMouseDX;
		*dy = s_NetMouseDY;
	}
}

s32 netGfxSizeIndex(s32 index)
{
	if (index < 0) {
		return 0;
	}

	if (index > 3) {
		return 3;
	}

	return index;
}
