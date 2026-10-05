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
 * Prediction (PLANS/NETPLAY.md "Prediction", netpredict.c). A client runs
 * its own player on its own commands at once; when the host's state for a
 * command it played differs from what this machine had after the same
 * command, the host's state is taken and the commands since are played
 * again through bmoveTick. g_NetReplaying is set for that replay: anything
 * other than the player's movement that the walk would do (a sound, a door,
 * a death, the gun, a push) is not done again. Each such place returns at
 * its top on it.
 */
extern s32 g_NetReplaying;
// bmoveTick, at its head: a tick's movement starts (the client's own player)
void netPredictMoveBegin(s32 allowc1x, s32 allowc1y, s32 allowc1buttons, s32 ignorec2);
// playerTick, around bmoveTick on a frame drawn between ticks (g_NetPass
// NETPASS_PRESENT_ONLY): the walk's state as the last tick left it, whatever
// the input handling of a zero-length frame did to it (each machine draws
// its own number of such frames, so nothing they do may stick)
void netPredictPresentBegin(void);
void netPredictPresentEnd(void);
// bwalkTryJump, at its head: the tick's jump (pressed after its walk), for a replay
void netPredictJumpTry(void);
// playerTick's camera (player.c): the eye eased off a correction, and the
// mouse not yet ticked turned in on a frame drawn between ticks
struct coord;
void netPredictView(struct coord *eye, struct coord *up, struct coord *look);

/**
 * Set around code that belongs to the player of the current pass (its gun,
 * its HUD, its own life) when that player is not this machine's: sndStart
 * then plays nothing, as nobody here is that player (spec-players.md §6).
 * World sounds started from the same pass still play: positional ones
 * lift the flag with netWorldSoundBegin/End.
 */
extern s32 g_NetRemotePass;
// The pass's player while it is another machine's (-1 otherwise): what its
// code makes there (its shot, the shot's sparks and flames) that machine
// made itself, so the events leave it out (netevents.c)
extern s32 g_NetPassPlayer;
void netRemotePassBegin(void);
static inline void netRemotePassEnd(void)
{
	g_NetRemotePass = 0;
	g_NetPassPlayer = -1;
}

#define NET_REMOTE_PASS_BEGIN() do { if (g_NetMode != NETMODE_NONE) netRemotePassBegin(); } while (0)
#define NET_REMOTE_PASS_END()   do { if (g_NetMode != NETMODE_NONE) netRemotePassEnd(); } while (0)

// Around a positional sound (psCreate, a shot's hit sounds in bondgun.c),
// which the local listener hears wherever it was started from: the remote
// pass's silence is lifted and put back after
static inline s32 netWorldSoundBegin(void)
{
	const s32 pass = g_NetRemotePass;

	if (g_NetMode != NETMODE_NONE && !g_NetReplaying) g_NetRemotePass = 0;
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

/**
 * Entities (PLANS/netplay/spec-entities.md §1, §6): a prop slot's
 * generation, bumped whenever the slot is allocated or freed, so an entity
 * id (pool index, generation) never names two props. A side table, never a
 * field of struct prop.
 */
struct prop;
extern u16 *g_NetPropGen;
void netPropGenAlloc(s32 maxprops);   // H1 varsreset.c, after g_Vars.props
void netPropGenBump(struct prop *prop); // H2 propAllocate, H3 propFree
// H5 lv.c lvRender, after propsTickPlayer: what this player's pass put on its screen
void netHostNoteVisible(s32 playernum);

/**
 * Puppets on the client (PLANS/netplay/spec-entities.md §2-§4). In a
 * match's stage a client's world is the host's: no AI, no damage, no drops,
 * no pickups; every chr but its own player, every object, door and
 * projectile is posed from the snapshots (netpuppets.c). g_NetClientWorld
 * is set from the stage's start (netStageStart) to its end (H12).
 */
extern s32 g_NetClientWorld;
#define NET_CLIENT (g_NetMode == NETMODE_CLIENT && g_NetClientWorld)

// Whether the client poses this chr or player prop from snapshots (C1, C2,
// C14): every PROPTYPE_CHR, and another machine's player once its first
// record has come
s32 netIsPuppet(struct prop *prop);
// The pose step (proptick.c, before chraTickBg): the interpolated snapshot
// into every puppet; puppets are made and taken away only here
void netClientPosePuppets(void);
// C5: objTick on the client
u32 netPuppetObjTick(struct prop *prop);
// C12: currentPlayerInteract on the client: whether nothing was in reach
// (the host does the opening)
s32 netClientInteract(s32 eyespy);
// pdmain.c and lvRender's player loops: another machine's player is not
// simulated here once it is a puppet. netClientPuppetPlayerTick returns 1
// to skip lvTickPlayer (keeping its body built); netClientRenderPass 1 to
// skip the view's pass, and otherwise says whether this pass is the last
s32 netClientPuppetPlayerTick(s32 playernum);
s32 netClientRenderPass(s32 order, s32 count, s32 *islast);
// after playermgrShuffle: the local player first, so the passes keyed to
// index 0 (the swirl, propsTickPlayer's frame start, bgTick) run in its
// pass when the others are skipped
void netClientOrderPlayers(void);
// a client in a match's stage, from its load (setup.c's scenario props)
s32 netClientInMatch(void);
// The host refuses to start a net match it cannot play yet (menutick.c):
// a client never starts one, and scenarios are not online yet
s32 netRefuseMatchStart(void);
s32 netMatchStartRefused(void);

/**
 * Events (PLANS/netplay/spec-entities.md §5; the wire is EVENTS in
 * netproto.h, netevents.c does the work). The host's hooks record, behind
 * g_NetMode == NETMODE_SERVER; the ones that return 1 tell the caller to
 * return (a hudmsg or pickup sound sent to the machine whose player it is;
 * a client's own death count refused, the host's arriving as E6).
 */
struct chrdata;
struct coord;
struct defaultobj;
void netEvChrFireslot(struct chrdata *chr, s32 handnum, s32 withsound, s32 withbeam, struct coord *from, struct coord *to); // E1
void netEvPlayerShot(s32 handnum);                                                                                       // E2
void netEvExplosion(struct prop *source, struct coord *pos, s16 *rooms, s32 type, s32 playernum,
		s32 makescorch, struct coord *arg6, s16 room, struct coord *arg8);                                              // E3
void netEvSparks(s32 room, struct prop *prop, struct coord *pos, struct coord *arg3, struct coord *arg4, s32 type);     // E2/E4
void netEvChrDamage(struct chrdata *chr, struct prop *aprop, s32 hitpart, s32 damageshield, s32 explosion);            // E4
void netEvChoke(struct chrdata *chr, s32 choketype);                                                                    // E4
void netEvObjDeform(struct defaultobj *obj, s32 level);                                                                 // E5
void netEvGlassDestroy(struct defaultobj *obj);                                                                         // E5
s32 netEvRecordDeath(s32 aplayernum, s32 vplayernum);                                                                   // E6
s32 netEvHudmsg(char *text, s32 type, s32 conf00, s32 conf01, s32 conf02, u32 textcolour, u32 glowcolour,
		u32 alignh, s32 conf16, u32 alignv, s32 conf18, s32 arg14, u32 flags);                                           // E7
s32 netEvPickupSound(s32 sound);                                                                                        // E8
void netEvOriginMade(s32 on);   // E8: what follows the pass's player's machine makes itself (its CamSpy pickup)
void netEvNbomb(struct coord *pos, struct prop *owner);                                                                 // E9
void netEvGas(struct coord *pos);                                                                                       // E9

// The session's hooks (H1-H14, HA-HD)
#include "net/netsession.h"

// The lobby's rooms (Online Game)
#include "net/netlobby.h"

#endif
