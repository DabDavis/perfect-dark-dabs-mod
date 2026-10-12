#ifndef IN_GAME_COOP_H
#define IN_GAME_COOP_H
#include <ultra64.h>
#include "bss.h"
#include "data.h"
#include "types.h"

/**
 * Co-operative with more than two players (online co-op: up to twelve on a
 * mission, PLANS/netplay/spec-coop.md). The game's own co-op is bond
 * (g_Vars.bond, player g_Vars.bondplayernum) and one coop player
 * (g_Vars.coop, g_Vars.coopplayernum); every place that asked "the other
 * player" or "bond or coop" asks these instead. At two players each gives
 * exactly the answer the old code did, so offline co-op is unchanged.
 */

/**
 * Counter-operative with more than one (online counter-op, netcoop.c): the
 * game's own is one player, g_Vars.anti (g_Vars.antiplayernum), against bond.
 * Online any number of seats are counter-ops (g_CoopAntiMask, a bit per
 * player number) and the rest are agents: bond, and past one agent the coop
 * players, so online co-op's rules (Mission Respawn, the dead watching, the
 * guards turned among the players) are the agents'. The mask is empty
 * offline, and every question below then gives the game's own answer.
 */
extern u32 g_CoopAntiMask;

// player playernum is a counter-operative (online the mask alone: a seat's
// side is its player's, and a join in progress may change it)
static inline bool coopIsAntiNum(s32 playernum)
{
	if (g_Vars.antiplayernum < 0 || playernum < 0) {
		return false;
	}

	if (g_CoopAntiMask) {
		return playernum < 32 && ((g_CoopAntiMask >> playernum) & 1);
	}

	return playernum == g_Vars.antiplayernum;
}

bool coopIsAnti(struct player *player);  // player is a counter-operative (offline: player == g_Vars.anti)
bool coopIsAntiProp(struct prop *prop);  // prop is a counter-operative's
bool coopIsAgentProp(struct prop *prop); // prop is a player's on the agents' side (bond's in the game's own)
u32 coopAntiRandom(void);                // a counter-op's guard choice's draw (online its own sequence)

// co-op is on: g_Vars.coopplayernum >= 0
static inline bool coopIsOn(void)
{
	return g_Vars.coopplayernum >= 0;
}

// a co-op player other than bond (the second player offline, slots 1-11
// online; never a counter-operative)
static inline bool coopIsCoopPlayer(struct player *player)
{
	return g_Vars.coopplayernum >= 0 && player != NULL && player != g_Vars.bond
		&& (g_CoopAntiMask == 0 || !coopIsAnti(player));
}

// prop is some player's prop in a co-op game (bond's or any coop player's,
// never a counter-operative's)
static inline bool coopIsPlayerProp(struct prop *prop)
{
	return g_Vars.coopplayernum >= 0 && prop != NULL && prop->type == PROPTYPE_PLAYER
		&& (g_CoopAntiMask == 0 || !coopIsAntiProp(prop));
}

// prop is a player's on the current player's side: in co-op any player's but
// a counter-operative's, and a counter-operative's to another
bool coopIsTeammateProp(struct prop *prop);
// the players a and b are on one side (both agents or both counter-ops)
bool coopSameSide(struct prop *a, struct prop *b);
// a counter-operative's body and head while it is in a guard's (-1 none):
// offline g_Vars.antibodynum/antiheadnum, online one a player (netcoop.c)
bool coopAntiGetBody(s32 playernum, s32 *bodynum, s32 *headnum);
void coopAntiSetBody(s32 playernum, s32 bodynum, s32 headnum);
void coopAntiResetBodies(void);
// a counter-op took a guard (its spawn count and the guard's radius, which
// the clients take with the body: netcoop.c)
void coopAntiNoteSpawn(s32 playernum, f32 radius);
s32 coopAntiSpawnCount(s32 playernum);
// online: a counter-op whose spawn found no guard (none left, or none yet)
// is looked for one again each second on the host (playerAntiRetryTick())
void coopAntiSetWaiting(s32 playernum, bool waiting);
bool coopAntiRetryDue(s32 playernum);
// ... and is out of the game until it has one: hidden, no collisions, no
// harm to anyone, nothing picked up (the host's word: clients see it hidden)
bool coopAntiIsWaiting(s32 playernum);
f32 coopAntiRadius(s32 playernum);
// chr is on screen (or not) for the current player: an agent's view is what
// a counter-operative's spawn keeps away from (CHRHFLAG_ONBONDSSCREEN)
void coopNoteChrOnScreen(struct chrdata *chr, bool onscreen);
// a guard a counter-op may take: the game's CHRHFLAG_BASICGUARD, or a
// converted mission's plain armed guard (coopConvertedGuard())
bool coopAntiHostable(struct chrdata *chr);
bool coopConvertedGuard(struct chrdata *chr);
// the agent nearest pos alive (-1 none): where a counter-op spawns from
s32 coopNearestAgentNum(struct coord *pos, f32 *sqdist);

// the owner index a setup file's mine carries in co-op: "no player's", 2 on
// the N64 (its third slot), past every slot online where slot 2 is a player
extern s32 g_NetMode;
#define COOP_SETUP_MINE_OWNER (g_NetMode != 0 ? MAX_PLAYERS : 2)

bool coopAllDead(void);      // every player is dead (isdead); with counter-ops, every agent
bool coopAllDeadDone(void);  // ... and every death has finished (redblood and the death anim)
bool coopAnyAborted(void);
// a counter-op mission's sides (offline: bond's and anti's own answers)
bool coopAgentsAborted(void);
bool coopAntisAborted(void);
bool coopAgentsDead(void);
bool coopPlayerAlive(s32 playernum);
// "the other player" for playernum: at two players the other one; past that
// the nearest living other player (any other if none lives)
s32 coopOtherPlayerNum(s32 playernum);
// who a respawn steals half its health from: the other player at two; the
// living other player with the most health past that (-1 none alive)
s32 coopRespawnBuddy(s32 playernum);
// the player prop a camera or autogun looks for this frame, cycling the
// players over the frames (bond on odd frames and coop on even at two)
struct prop *coopAlternatePlayerProp(void);
// the living player nearest chr other than skip, not spectating (-1 none)
s32 coopNearestPlayerNum(struct chrdata *chr, s32 skip);
// the next living player after playernum not spectating, in slot order
// (playernum itself when no other lives)
s32 coopNextLivingAfter(s32 playernum);
// the player chr_toggle_p1p2 turns chr to: the other one at two; past that
// the nearest living player, or from the nearest the next in chr's turn
s32 coopToggleP1P2(struct chrdata *chr);
// a player's noise at noiseprop is for chr's ears: its target's (the game's
// rule), or - past two players, or two on a converted mission - any player's
// when chr's target is a player
bool coopNoiseReaches(struct chrdata *chr, struct prop *noiseprop);
// chr heard player playernum's noise (in its range, coopNoiseReaches): its
// target's, or past two the guard turned to it; false if it kept its target
bool coopHearPlayerNoise(struct chrdata *chr, struct prop *noiseprop, s32 playernum);
// a converted mission's chr with two co-op players or more: its target player
// chosen again toward the nearest every ten ticks (host only; p1p2 left alone)
void coopRetarget(struct chrdata *chr);
// a converted mission's Bond-only question (CHR_P1P2: what Bond carries,
// holds or used a gadget on) asked with two co-op players or more: any
// player's answer counts, as GoldenEye had one Bond and every player is him
bool coopAnyPlayerIsBond(s32 chrref);
// Past two players CHR_COOP is one player (g_Vars.coop), and Perfect Dark's
// lists answer "the co-op player activated it" for any of them
// (OBJHFLAG_ACTIVATED_BY_COOP). Which one did: noted when the flag is set,
// taken by the list that consumes it, and that list's CHR_COOP is that player
// until it consumes another (Defection's PC: the downloader's uplink)
void coopNoteActivation(struct defaultobj *obj, s32 playernum);
void coopTakeActivation(struct chrdata *ref, struct defaultobj *obj);
s32 coopActorFor(struct chrdata *ref);
void coopActorsReset(void);
// a converted mission's background list asking where Bond is (its target,
// none: CHR_P1P2, the host's player) with two players or more: any living
// player answers (IFBondInRoomWithPad, IFBondDistanceToPad*)
bool coopBgListAsksAnyPlayer(struct chrdata *chr);
// give_object_to_chr(weapon, CHR_COOP) past two players: the same gun, with
// the ammo the co-op player got, for every other co-op player
void coopGiveWeaponToOthers(struct defaultobj *obj, s32 givenplayernum);

#endif
