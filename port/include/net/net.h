#ifndef _IN_NET_NET_H
#define _IN_NET_NET_H

#include <signal.h>
#include <PR/ultratypes.h>

/**
 * Netplay's face to the game: the mode, the pass the main loop is on, and the
 * one-line calls the hooks make (PLANS/NETPLAY.md, PLANS/netplay/spec-*.md).
 *
 * Game code includes this, so it must never pull in <stdbool.h> or ENet
 * (bool-two-sizes): nothing here is a bool, everything is s32/u32. Every hook
 * in the game is a call into port/src/net/ behind g_NetMode != NETMODE_NONE
 * or g_NetPass != NETPASS_NONE, so with netplay off (the default) the game
 * runs exactly as it did.
 */

#define NETMODE_NONE   0 // offline: every hook is skipped
#define NETMODE_SERVER 1 // the authority: a listen host, --dedicated, or --net-clock-test
#define NETMODE_CLIENT 2

/**
 * What the current mainTick() pass is (mainNetFrame in pdmain.c). A tick
 * pass is exactly a stock frame of diff (1, 4); a present-only pass is a
 * paused frame of diff (0, 0) that consumes no input.
 */
#define NETPASS_NONE         0 // the stock loop
#define NETPASS_PRESENT_ONLY 1 // no tick due: draw, consume nothing
#define NETPASS_TICK         2 // a sim tick whose display list is dropped
#define NETPASS_TICK_PRESENT 3 // the frame's last tick, which is drawn

extern s32 g_NetMode;
extern s32 g_NetPass;
extern s32 g_NetTicksThisFrame;   // tick passes in this presented frame (audio, frame history)
extern s32 g_NetDedicated;        // --dedicated: no window, no audio, no local input
extern u32 g_NetTick;             // sim passes since the stage started (never lvframenum)
extern s32 g_NetLocalSlot;        // this machine's player: 0 on a listen host, k on a client, -1 dedicated
extern s32 g_NetInStageLoop;      // mainLoop is running mainNetFrame (the pad read moved into the ticks)
extern volatile sig_atomic_t g_NetQuit; // a signal asked a dedicated server to stop
extern s32 g_NetHostRenderAtTickRate; // Net.HostRenderAtTickRate: the host never runs present-only passes
extern s32 g_NetClockTest;        // --net-clock-test N: offline, N tick passes per presented frame

static inline s32 netIsLocalSlot(s32 playernum)
{
	return g_NetMode == NETMODE_NONE || playernum == g_NetLocalSlot;
}

s32 netSlotHasMouse(s32 playernum);

// Whether this player's mouse drives it (bondmove.c, player.c, bondeyespy.c)
#define NET_MOUSE_SLOT(pn) (g_NetMode == NETMODE_NONE ? (pn) == 0 : netSlotHasMouse(pn))

// How many views share the screen: one per machine in a net game, whatever
// the player count (layout branches only; loops and counts keep PLAYERCOUNT())
#define VIEWCOUNT() (g_NetMode != NETMODE_NONE ? 1 : PLAYERCOUNT())

// Boot: reads --dedicated and --net-clock-test (main.c, before videoInit)
void netInitArgs(void);

// Main loop (pdmain.c)
s32 netSessionInStage(void);
s32 netStageReady(void);
s32 netClockPpm(void);
void netPump(void);
void netFlush(void);
void netWait(s32 us);
void netStageStart(void);
void netCheckQuit(void);

// Tick passes (pdmain.c mainTick)
void netTickReadPad(void);
void netTickBegin(void);
void netTickEnd(void);
void netPosePuppets(f32 alpha);

// lvTick: the host decides slow motion and pause
s32 netClientLvupdate240(void);
void netHostSetLvupdate240(s32 lvupdate240);

// lvRender: a view nobody here looks at is simulated and its display list dropped
s32 netDiscardPass(s32 playernum);

// Mouse: summed over a presented frame's inputUpdate(), spent by one tick
void netInputAccumulateMouse(void);
void netInputMouseRaw(s32 *dx, s32 *dy);

// gfxReset: the player-count row the display list pools are sized by
s32 netGfxSizeIndex(s32 index);

#endif
