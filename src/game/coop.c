#include <ultra64.h>
#include "constants.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/coop.h"
#include "game/playermgr.h"
#include "game/modspectate.h"
#include "modloader.h"
#include "net/net.h"
#include "bss.h"
#include "data.h"
#include "types.h"

bool coopPlayerAlive(s32 playernum)
{
	return playernum >= 0 && playernum < MAX_PLAYERS
		&& g_Vars.players[playernum] && !g_Vars.players[playernum]->isdead;
}

bool coopAllDead(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_Vars.players[i] && !g_Vars.players[i]->isdead && !netPlayerOutOfPlay(i)) {
			return false;
		}
	}

	return true;
}

/**
 * Every player dead and done falling. Online, an open seat's player is left
 * out (netPlayerOutOfPlay): it was killed to vacate the seat, and the host
 * builds no view for it, so its fall never finishes - a room played alone
 * (every member slot a seat) never ended when its one player died, and sat
 * on a black screen.
 */
bool coopAllDeadDone(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];

		if (p && !(p->isdead && p->redbloodfinished && p->deathanimfinished) && !netPlayerOutOfPlay(i)) {
			return false;
		}
	}

	return true;
}

bool coopAnyAborted(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_Vars.players[i] && g_Vars.players[i]->aborted) {
			return true;
		}
	}

	return false;
}

static f32 coopSqDist(struct prop *a, struct prop *b)
{
	const f32 dx = a->pos.x - b->pos.x;
	const f32 dy = a->pos.y - b->pos.y;
	const f32 dz = a->pos.z - b->pos.z;

	return dx * dx + dy * dy + dz * dz;
}

s32 coopOtherPlayerNum(s32 playernum)
{
	const s32 count = PLAYERCOUNT();
	struct prop *me;
	s32 best = -1;
	s32 any = -1;
	f32 bestsq = 0;
	s32 i;

	if (count <= 2) {
		// the game's own answer: bond's other is coop, coop's is bond
		return playernum == g_Vars.bondplayernum ? g_Vars.coopplayernum : g_Vars.bondplayernum;
	}

	me = playernum >= 0 && playernum < MAX_PLAYERS && g_Vars.players[playernum] ? g_Vars.players[playernum]->prop : NULL;

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];

		if (i == playernum || !p || !p->prop) {
			continue;
		}

		if (any < 0) {
			any = i;
		}

		if (p->isdead) {
			continue;
		}

		if (best < 0 || (me && coopSqDist(me, p->prop) < bestsq)) {
			best = i;
			bestsq = me ? coopSqDist(me, p->prop) : 0;
		}
	}

	return best >= 0 ? best : any >= 0 ? any : playernum;
}

s32 coopRespawnBuddy(s32 playernum)
{
	const s32 count = PLAYERCOUNT();
	s32 best = -1;
	f32 besthealth = 0;
	s32 i;

	if (count <= 2) {
		return playernum == g_Vars.bondplayernum ? g_Vars.coopplayernum : g_Vars.bondplayernum;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		f32 health;

		if (i == playernum || !p || !p->prop || p->isdead || !p->prop->chr) {
			continue;
		}

		health = p->bondhealth + chrGetShield(p->prop->chr) * 0.125f;

		if (best < 0 || health > besthealth) {
			best = i;
			besthealth = health;
		}
	}

	return best;
}

struct prop *coopAlternatePlayerProp(void)
{
	const s32 count = PLAYERCOUNT();
	s32 index;

	if (count <= 2) {
		return (g_Vars.lvframenum & 1) ? g_Vars.bond->prop : g_Vars.coop->prop;
	}

	// the same order at two: frame odd -> player 0, even -> player 1
	index = (count - 1) - (g_Vars.lvframenum % count);

	if (index < 0 || index >= MAX_PLAYERS || !g_Vars.players[index] || !g_Vars.players[index]->prop) {
		return g_Vars.bond->prop;
	}

	return g_Vars.players[index]->prop;
}

// a player a guard can be turned to: living, and noticeable (not spectating)
static bool coopPlayerTargetable(s32 playernum)
{
	return coopPlayerAlive(playernum) && g_Vars.players[playernum]->prop
		&& modSpectatePropNoticeable(g_Vars.players[playernum]->prop);
}

// guards are turned among the players by coop.c's rules: past two players,
// and at two on a converted mission, whose lists never switch players
static bool coopTurnsGuards(void)
{
	const s32 count = PLAYERCOUNT();

	return count > 2 || (count == 2 && modloaderStageIsMission(g_Vars.stagenum));
}

s32 coopNearestPlayerNum(struct chrdata *chr, s32 skip)
{
	s32 best = -1;
	f32 bestsq = 0;
	s32 i;

	if (!chr || !chr->prop) {
		return -1;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		f32 sq;

		if (i == skip || !coopPlayerTargetable(i)) {
			continue;
		}

		sq = coopSqDist(chr->prop, g_Vars.players[i]->prop);

		if (best < 0 || sq < bestsq) {
			best = i;
			bestsq = sq;
		}
	}

	return best;
}

// the next living player after chr's turn other than nearest, which becomes
// its turn (nearest if no other lives)
static s32 coopNextInTurn(struct chrdata *chr, s32 nearest)
{
	s32 i;

	for (i = 1; i <= MAX_PLAYERS; i++) {
		const s32 next = (chr->coopturn + i) % MAX_PLAYERS;

		if (next != nearest && coopPlayerTargetable(next)) {
			chr->coopturn = next;
			return next;
		}
	}

	return nearest;
}

/**
 * The guard lists' chr_toggle_p1p2 turns a guard to "the other player" and
 * often straight back: switch, test the new one, switch again. At two players
 * that is the game's swap. Past two the switch has a home, the living player
 * nearest the guard: from anyone else it goes home, and from home to the next
 * living player after the last one it went to (chr->coopturn). So a guard
 * keeps to whoever is closest, every other player is looked at in turn, and
 * two switches in a row come home again as they do at two.
 */
s32 coopToggleP1P2(struct chrdata *chr)
{
	s32 nearest;

	if (PLAYERCOUNT() <= 2) {
		const s32 other = coopOtherPlayerNum(chr->p1p2);

		return coopPlayerAlive(other) ? other : chr->p1p2;
	}

	nearest = coopNearestPlayerNum(chr, -1);

	if (nearest < 0) {
		return chr->p1p2;
	}

	if (chr->p1p2 != nearest) {
		return nearest;
	}

	return coopNextInTurn(chr, nearest);
}

bool coopNoiseReaches(struct chrdata *chr, struct prop *noiseprop)
{
	struct prop *target = chrGetTargetProp(chr);

	return target == noiseprop || (coopIsPlayerProp(target) && coopTurnsGuards());
}

/**
 * The game's guards hear only their target's noise. Past two players a guard
 * whose target is another player turns to the noise - the player making it
 * becomes its p1p2 and, if it had one, its target - when that player is
 * nearer than its target or its target has gone a second unseen. A guard
 * fighting a nearer player it can see keeps to that one.
 */
bool coopHearPlayerNoise(struct chrdata *chr, struct prop *noiseprop, s32 playernum)
{
	struct prop *target = chrGetTargetProp(chr);

	if (target == noiseprop) {
		return true;
	}

	if (!coopIsPlayerProp(target) || !coopTurnsGuards() || !coopPlayerTargetable(playernum)
			|| g_Vars.players[playernum]->prop != noiseprop || !chr->prop) {
		return false;
	}

	if (chr->lastseetarget60 >= g_Vars.lvframe60 - TICKS(60)
			&& coopSqDist(chr->prop, noiseprop) >= coopSqDist(chr->prop, target)) {
		return false;
	}

	if (modloaderStageIsMission(g_Vars.stagenum)) {
		// a converted mission's guard: its target, p1p2 staying the host's
		// for the Bond-only commands (coopRetarget())
		chr->target = noiseprop - g_Vars.props;
	} else {
		chr->p1p2 = playernum;

		if (chr->target != -1) {
			chr->target = noiseprop - g_Vars.props;
		}
	}

	return true;
}

/**
 * A converted mission's lists (GoldenEye's, Goldfinger 64's, Tomorrow Never
 * Dies 64's) never switch players: GoldenEye had no co-op. Their "Bond" is
 * the chr's target where they see, hear, aim at, run to or measure the
 * distance to him, and CHR_P1P2 where they give, equip, take control or ask
 * whether he is dead. With two co-op players or more the host chooses the
 * target again every ten ticks of the chr's own, toward the living player
 * nearest it who is not spectating (coopPlayerTargetable()), and leaves p1p2 -
 * the host's player - to the Bond-only commands:
 * - a target that is dead, spectating or a second unseen gives way to the
 *   nearest;
 * - a seen one gives way to a nearer player in the chr's line of sight at
 *   under 70% of its distance, so two players about as near do not trade it;
 * - a chr on the nearest that has gone two seconds without seeing it, and is
 *   not attacking, looks at the next living player in its turn
 *   (chr->coopturn) at one choice in four, until the choice after, which
 *   comes back to the nearest unless it saw that one: a player in plain view
 *   further off is still noticed.
 * Called as the chr's list is about to run, so only where the AI runs.
 */
void coopRetarget(struct chrdata *chr)
{
	struct prop *target;
	struct prop *nearprop;
	s32 cur;
	s32 nearest;
	s32 pick;

	if (!coopIsOn() || netIsClient() || PLAYERCOUNT() < 2 || !modloaderStageIsMission(g_Vars.stagenum)
			|| (g_Vars.tickmode != TICKMODE_NORMAL && g_Vars.tickmode != TICKMODE_GE_FADEIN) || g_InCutscene
			|| !chr->prop || chr->prop->type != PROPTYPE_CHR || chr->aibot
			|| chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD) {
		return;
	}

	if (chr->coopretarget60 <= g_Vars.lvframe60 && chr->coopretarget60 > g_Vars.lvframe60 - TICKS(10)) {
		return;
	}

	chr->coopretarget60 = g_Vars.lvframe60;
	target = chrGetTargetProp(chr);

	if (!coopIsPlayerProp(target)) {
		return;
	}

	cur = playermgrGetPlayerNumByProp(target);
	nearest = coopNearestPlayerNum(chr, -1);

	if (nearest < 0) {
		return;
	}

	nearprop = g_Vars.players[nearest]->prop;
	pick = cur;

	if (cur == nearest) {
		// one choice in four, so the nearest is the one looked for most
		if (((u32)(g_Vars.lvframe60 / TICKS(10)) + (u32)chr->chrnum) % 4 == 0
				&& chr->lastseetarget60 < g_Vars.lvframe60 - TICKS(120)
				&& chr->actiontype != ACT_ATTACK && chr->actiontype != ACT_ATTACKWALK
				&& chr->actiontype != ACT_ATTACKROLL && chr->actiontype != ACT_ATTACKAMOUNT) {
			pick = coopNextInTurn(chr, nearest);
		}
	} else if (!coopPlayerTargetable(cur) || chr->lastseetarget60 < g_Vars.lvframe60 - TICKS(60)) {
		pick = nearest;
	} else if (coopSqDist(chr->prop, nearprop) < 0.49f * coopSqDist(chr->prop, target)
			&& chrHasLosToProp(chr, nearprop)) {
		pick = nearest;
	}

	if (pick != cur) {
		chr->target = g_Vars.players[pick]->prop - g_Vars.props;
	}
}
