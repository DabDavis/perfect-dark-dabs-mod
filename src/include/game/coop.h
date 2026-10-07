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

// co-op is on: g_Vars.coopplayernum >= 0
static inline bool coopIsOn(void)
{
	return g_Vars.coopplayernum >= 0;
}

// a co-op player other than bond (the second player offline, slots 1-11 online)
static inline bool coopIsCoopPlayer(struct player *player)
{
	return g_Vars.coopplayernum >= 0 && player != NULL && player != g_Vars.bond;
}

// prop is some player's prop in a co-op game (bond's or any coop player's)
static inline bool coopIsPlayerProp(struct prop *prop)
{
	return g_Vars.coopplayernum >= 0 && prop != NULL && prop->type == PROPTYPE_PLAYER;
}

// the owner index a setup file's mine carries in co-op: "no player's", 2 on
// the N64 (its third slot), past every slot online where slot 2 is a player
extern s32 g_NetMode;
#define COOP_SETUP_MINE_OWNER (g_NetMode != 0 ? MAX_PLAYERS : 2)

bool coopAllDead(void);      // every player is dead (isdead)
bool coopAllDeadDone(void);  // ... and every death has finished (redblood and the death anim)
bool coopAnyAborted(void);
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

#endif
