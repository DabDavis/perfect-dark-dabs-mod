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

// Whether this player's mouse drives it (bondmove.c, player.c, bondeyespy.c):
// the local player's here, a remote player's from its commands on the host
#define NET_MOUSE_SLOT(pn) (g_NetMode == NETMODE_NONE ? (pn) == 0 : netSlotHasMouse(pn))

// Whether this player's menus take this machine's mouse, ESC and shoulder
// keys (menu.c): player 0 offline, the local player in a net game
#define NET_LOCAL_UI(pn) (g_NetMode == NETMODE_NONE ? (pn) == 0 : netIsLocalSlot(pn))

/**
 * Remote players (PLANS/netplay/spec-players.md). A pad index is a player's
 * mpindex (its contpad1); a slot or playernum is g_Vars.players' index.
 */

// Hook V (libultra.c osContGetReadData): the four pads for a sample during a
// net match: the local player's from this machine, every other neutral (a
// remote player's are written per tick by netTickReadPad). `pads` is
// OSContPad[4]. 0 outside a net match: the stock read then runs.
s32 netContGetReadData(void *pads);
// Hook V2 (libultra.c osContGetQuery): every human's pad is connected in a match
s32 netVpadConnected(s32 idx);
// Rumble only for this machine's own player (libultra.c __osMotorAccess)
s32 netIsLocalPad(s32 idx);
// menu.c's shoulder keys: 0 for this machine's player, -1 for any other
s32 netLocalUiIndex(s32 playernum);

// A player's mouse for the tick: the live one for the local player, the
// command's for a remote one on the host (bondmove.c, player.c,
// bondeyespy.c, activemenutick.c)
void netMouseDelta(s32 playernum, f32 *dx, f32 *dy);
void netMouseAbsDelta(s32 playernum, f32 *dx, f32 *dy);
s32 netMouseLocked(s32 playernum);

// player0f0bd358: the aspect of the screen this player is seen on
f32 netSlotAspect(s32 playernum);
// a player's own Aim Lock / akimbo triggers (modoptions.c): a remote
// player's from its SLOTCFG, else `mine`, this machine's
s32 netSlotAimLock(s32 playernum, s32 codaiming, s32 mine);
s32 netSlotAkimboTriggers(s32 playernum, s32 mine);

// The player whose ears and eyes this machine has (propsnd.c), or fallback
struct player;
struct player *netLocalPlayer(struct player *fallback);

/**
 * Set around code that belongs to the player of the current pass (its gun,
 * its HUD, its own life) when that player is not this machine's: sndStart
 * then plays nothing, as nobody here is that player (spec-players.md §6).
 * World sounds started from the same pass still play: positional ones
 * lift the flag with netWorldSoundBegin/End.
 */
extern s32 g_NetRemotePass;
void netRemotePassBegin(void);
static inline void netRemotePassEnd(void)
{
	g_NetRemotePass = 0;
}

#define NET_REMOTE_PASS_BEGIN() do { if (g_NetMode != NETMODE_NONE) netRemotePassBegin(); } while (0)
#define NET_REMOTE_PASS_END()   do { if (g_NetMode != NETMODE_NONE) netRemotePassEnd(); } while (0)

// Around a positional sound (psCreate, a shot's hit sounds in bondgun.c),
// which the local listener hears wherever it was started from: the remote
// pass's silence is lifted and put back after
static inline s32 netWorldSoundBegin(void)
{
	const s32 pass = g_NetRemotePass;

	if (g_NetMode != NETMODE_NONE) g_NetRemotePass = 0;
	return pass;
}

static inline void netWorldSoundEnd(s32 pass)
{
	if (g_NetMode != NETMODE_NONE) g_NetRemotePass = pass;
}

// The players' own settings for the match (data.h PLAYER_EXTCFG()):
// g_NetExtCfg is on from the match's start to its end (H1/H3 .. H12)
extern s32 g_NetExtCfgOn;

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

// The session's hooks (H1-H14, HA-HD)
#include "net/netsession.h"

// The lobby's rooms (Online Game)
#include "net/netlobby.h"

#endif
