#include <ultra64.h>
#include "constants.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/coop.h"
#include "game/inv.h"
#include "game/modalarm.h"
#include "lib/rng.h"
#include "game/game_0b0fd0.h"
#include <string.h>
#include "gexplus.h"
#include "game/chrai.h"
#include "game/bondgun.h"
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

u32 g_CoopAntiMask = 0;

/**
 * Online counter-op: the host's choices of a counter-op's guard draw from a
 * sequence of their own, never the game's: the clients take no guard
 * themselves (the host's blocks put their player in it), and a draw from the
 * game's at the stage's load - the first guards are taken there - left the
 * host's g_RngSeed off the clients' ("The stage loaded differently here: its
 * rng"). Offline the game's own, as stock.
 */
static u64 g_CoopAntiSeed = 0x6a09e667f3bcc909ull;

u32 coopAntiRandom(void)
{
	if (g_CoopAntiMask == 0) {
		return rngRandom();
	}

	return rngRotateSeed(&g_CoopAntiSeed);
}

// online counter-op: one counter-op or more beside the agents (netcoop.c);
// never offline, where the game's own counter-op is the one g_Vars.anti
static bool coopAntiMany(void)
{
	return g_Vars.antiplayernum >= 0 && g_CoopAntiMask != 0;
}

// the game's own answers for two players: not when one of them may be a
// counter-op online (a room of one agent and one counter-op has the
// counter-op as g_Vars.coop: netcoop.c)
static bool coopTwoRules(void)
{
	return PLAYERCOUNT() <= 2 && !coopAntiMany();
}

bool coopIsAnti(struct player *player)
{
	s32 i;

	if (g_Vars.antiplayernum < 0 || player == NULL) {
		return false;
	}

	if (g_CoopAntiMask == 0) {
		return player == g_Vars.anti;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_Vars.players[i] == player) {
			return coopIsAntiNum(i);
		}
	}

	return false;
}

bool coopIsAntiProp(struct prop *prop)
{
	if (g_Vars.antiplayernum < 0 || prop == NULL || prop->type != PROPTYPE_PLAYER) {
		return false;
	}

	if (g_CoopAntiMask == 0) {
		return g_Vars.anti && prop == g_Vars.anti->prop;
	}

	return coopIsAntiNum(playermgrGetPlayerNumByProp(prop));
}

bool coopIsAgentProp(struct prop *prop)
{
	if (prop == NULL || prop->type != PROPTYPE_PLAYER) {
		return false;
	}

	if (g_CoopAntiMask == 0) {
		return g_Vars.bond && prop == g_Vars.bond->prop;
	}

	return !coopIsAntiProp(prop);
}

bool coopSameSide(struct prop *a, struct prop *b)
{
	if (a == NULL || b == NULL || a->type != PROPTYPE_PLAYER || b->type != PROPTYPE_PLAYER) {
		return false;
	}

	if (g_Vars.antiplayernum < 0) {
		return true;
	}

	return coopIsAntiProp(a) == coopIsAntiProp(b);
}

bool coopIsTeammateProp(struct prop *prop)
{
	if (!coopAntiMany()) {
		return coopIsPlayerProp(prop);
	}

	return prop != NULL && prop->type == PROPTYPE_PLAYER && g_Vars.currentplayer
		&& coopSameSide(prop, g_Vars.currentplayer->prop);
}

static s16 g_CoopAntiBodies[MAX_PLAYERS];
static s16 g_CoopAntiHeads[MAX_PLAYERS];
static u8 g_CoopAntiSpawns[MAX_PLAYERS];
static f32 g_CoopAntiRadii[MAX_PLAYERS];
static u8 g_CoopAntiWaiting[MAX_PLAYERS];
static s32 g_CoopAntiRetry60[MAX_PLAYERS];

void coopAntiResetBodies(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_CoopAntiBodies[i] = -1;
		g_CoopAntiHeads[i] = -1;
		g_CoopAntiSpawns[i] = 0;
		g_CoopAntiRadii[i] = 0;
		g_CoopAntiWaiting[i] = 0;
		g_CoopAntiRetry60[i] = 0;
	}
}

void coopAntiSetWaiting(s32 playernum, bool waiting)
{
	if (playernum >= 0 && playernum < MAX_PLAYERS) {
		g_CoopAntiWaiting[playernum] = waiting;
		g_CoopAntiRetry60[playernum] = g_Vars.lvframe60 + TICKS(60);
	}
}

bool coopAntiIsWaiting(s32 playernum)
{
	return playernum >= 0 && playernum < MAX_PLAYERS && g_CoopAntiMask && g_CoopAntiWaiting[playernum];
}

bool coopAntiRetryDue(s32 playernum)
{
	if (playernum < 0 || playernum >= MAX_PLAYERS || !g_CoopAntiWaiting[playernum]
			|| g_Vars.lvframe60 < g_CoopAntiRetry60[playernum]) {
		return false;
	}

	g_CoopAntiRetry60[playernum] = g_Vars.lvframe60 + TICKS(60);

	return true;
}

void coopAntiNoteSpawn(s32 playernum, f32 radius)
{
	if (playernum >= 0 && playernum < MAX_PLAYERS) {
		g_CoopAntiSpawns[playernum]++;
		g_CoopAntiRadii[playernum] = radius;
		g_CoopAntiWaiting[playernum] = 0;
	}
}

s32 coopAntiSpawnCount(s32 playernum)
{
	return playernum >= 0 && playernum < MAX_PLAYERS ? g_CoopAntiSpawns[playernum] : 0;
}

f32 coopAntiRadius(s32 playernum)
{
	return playernum >= 0 && playernum < MAX_PLAYERS ? g_CoopAntiRadii[playernum] : 0;
}

void coopAntiSetBody(s32 playernum, s32 bodynum, s32 headnum)
{
	if (playernum >= 0 && playernum < MAX_PLAYERS) {
		g_CoopAntiBodies[playernum] = bodynum;
		g_CoopAntiHeads[playernum] = headnum;
	}
}

bool coopAntiGetBody(s32 playernum, s32 *bodynum, s32 *headnum)
{
	if (!coopAntiMany()) {
		// the game's own: one counter-op, one stolen body
		if (g_Vars.antiheadnum >= 0 && g_Vars.antibodynum >= 0) {
			*bodynum = g_Vars.antibodynum;
			*headnum = g_Vars.antiheadnum;
			return true;
		}

		return false;
	}

	if (playernum < 0 || playernum >= MAX_PLAYERS || g_CoopAntiBodies[playernum] < 0 || g_CoopAntiHeads[playernum] < 0) {
		return false;
	}

	*bodynum = g_CoopAntiBodies[playernum];
	*headnum = g_CoopAntiHeads[playernum];
	return true;
}

/**
 * A counter-op spawns into a guard off the agents' screens where it can
 * (playerReset()): the game marks a chr CHRHFLAG_ONBONDSSCREEN while bond's
 * view has it. Past one agent a chr is on "bond's" screen while any agent's
 * view has it (chr->coopagentseen, a bit per player).
 */
void coopNoteChrOnScreen(struct chrdata *chr, bool onscreen)
{
	u16 bit;

	if (g_Vars.antiplayernum < 0) {
		return;
	}

	if (!coopAntiMany()) {
		if (g_Vars.currentplayer == g_Vars.bond) {
			if (onscreen) {
				chr->hidden |= CHRHFLAG_ONBONDSSCREEN;
			} else {
				chr->hidden &= ~CHRHFLAG_ONBONDSSCREEN;
			}
		}

		return;
	}

	if (g_Vars.currentplayernum < 0 || g_Vars.currentplayernum >= MAX_PLAYERS || coopIsAntiNum(g_Vars.currentplayernum)) {
		return;
	}

	bit = 1 << g_Vars.currentplayernum;

	if (onscreen) {
		chr->coopagentseen |= bit;
	} else {
		chr->coopagentseen &= ~bit;
	}

	if (chr->coopagentseen) {
		chr->hidden |= CHRHFLAG_ONBONDSSCREEN;
	} else {
		chr->hidden &= ~CHRHFLAG_ONBONDSSCREEN;
	}
}

static bool coopChrArmed(struct chrdata *chr);

/**
 * A guard a counter-op may take (playerSpawn()): the game's own are the ones
 * its setup lists mark CHRHFLAG_BASICGUARD. Guards Alerted!'s guards
 * (modalarm.c) are bodies for the counter-ops too (the user, 2026-10-12:
 * "counter-ops should work with Guards Alerted! so players on the
 * counter-ops side can use the additional guard bodies"): the mod's own,
 * armed - every wave a counter-op thins, the mod fills again. A converted
 * mission's lists (GoldenEye's, Goldfinger 64's, TND64's) never mark one.
 */
bool coopAntiHostable(struct chrdata *chr)
{
	if (chr->hidden & CHRHFLAG_BASICGUARD) {
		return true;
	}

	if (modAlarmIsGuard(chr) && coopChrArmed(chr)) {
		return true;
	}

	if (g_CoopAntiMask && modloaderStageIsMission(g_Vars.stagenum)) {
		return coopConvertedGuard(chr);
	}

	return false;
}

s32 coopNearestAgentNum(struct coord *pos, f32 *sqdist)
{
	s32 best = -1;
	f32 bestsq = 0;
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		f32 dx, dy, dz, sq;

		if (!p || !p->prop || p->isdead || coopIsAntiNum(i) || netPlayerOutOfPlay(i)) {
			continue;
		}

		dx = p->prop->pos.x - pos->x;
		dy = p->prop->pos.y - pos->y;
		dz = p->prop->pos.z - pos->z;
		sq = dx * dx + dy * dy + dz * dz;

		if (best < 0 || sq < bestsq) {
			best = i;
			bestsq = sq;
		}
	}

	if (sqdist) {
		*sqdist = bestsq;
	}

	return best;
}

/**
 * A converted mission's lists (GoldenEye's, Goldfinger 64's, TND64's) ask
 * about Bond through CHR_P1P2, which coopRetarget() leaves on the host's
 * player. Where the question is what Bond did - the objects he collected,
 * the item in his hand, a gadget used on an object (IFBondCollectedObject,
 * IFBondHasItemEquipped, IFBondUsedGadgetOnObject) - only the host's player
 * could answer it: a co-op player who attached Bunker's data thief to the
 * mainframe never downloaded the data (F3 20261010-044027). With two players
 * or more on such a mission every player is Bond to those three.
 */
bool coopAnyPlayerIsBond(s32 chrref)
{
	return chrref == CHR_P1P2 && coopIsOn() && PLAYERCOUNT() >= 2 && modloaderStageIsMission(g_Vars.stagenum);
}

#define COOP_MAX_ACTIVATIONS 16

static struct {
	struct defaultobj *obj;
	s8 playernum;
} g_CoopActivations[COOP_MAX_ACTIVATIONS];

static struct {
	struct chrdata *ref;
	s8 playernum;
} g_CoopActors[COOP_MAX_ACTIVATIONS];

bool coopBgListAsksAnyPlayer(struct chrdata *chr)
{
	return chr && chr->prop == NULL && chr->target == -1 && coopIsOn() && PLAYERCOUNT() >= 2
		&& modloaderStageIsMission(g_Vars.stagenum);
}

void coopActorsReset(void)
{
	s32 i;

	for (i = 0; i < COOP_MAX_ACTIVATIONS; i++) {
		g_CoopActivations[i].obj = NULL;
		g_CoopActors[i].ref = NULL;
	}
}

void coopNoteActivation(struct defaultobj *obj, s32 playernum)
{
	s32 i;
	s32 slot = -1;

	if (PLAYERCOUNT() <= 2) {
		return;
	}

	for (i = 0; i < COOP_MAX_ACTIVATIONS; i++) {
		if (g_CoopActivations[i].obj == obj) {
			slot = i;
			break;
		}

		if (slot < 0 && g_CoopActivations[i].obj == NULL) {
			slot = i;
		}
	}

	if (slot < 0) {
		slot = 0;
	}

	g_CoopActivations[slot].obj = obj;
	g_CoopActivations[slot].playernum = playernum;
}

void coopTakeActivation(struct chrdata *ref, struct defaultobj *obj)
{
	s32 i;
	s32 playernum = -1;
	s32 slot = -1;

	if (PLAYERCOUNT() <= 2 || ref == NULL) {
		return;
	}

	for (i = 0; i < COOP_MAX_ACTIVATIONS; i++) {
		if (g_CoopActivations[i].obj == obj) {
			playernum = g_CoopActivations[i].playernum;
			g_CoopActivations[i].obj = NULL;
			break;
		}
	}

	if (playernum < 0) {
		return;
	}

	for (i = 0; i < COOP_MAX_ACTIVATIONS; i++) {
		if (g_CoopActors[i].ref == ref) {
			slot = i;
			break;
		}

		if (slot < 0 && g_CoopActors[i].ref == NULL) {
			slot = i;
		}
	}

	if (slot < 0) {
		slot = 0;
	}

	g_CoopActors[slot].ref = ref;
	g_CoopActors[slot].playernum = playernum;
}

s32 coopActorFor(struct chrdata *ref)
{
	s32 i;

	if (PLAYERCOUNT() <= 2 || ref == NULL) {
		return -1;
	}

	for (i = 0; i < COOP_MAX_ACTIVATIONS; i++) {
		if (g_CoopActors[i].ref == ref) {
			s32 pn = g_CoopActors[i].playernum;

			if (pn >= 0 && pn < PLAYERCOUNT() && g_Vars.players[pn] && g_Vars.players[pn]->prop) {
				return pn;
			}

			return -1;
		}
	}

	return -1;
}

void coopGiveWeaponToOthers(struct defaultobj *obj, s32 givenplayernum)
{
	struct weaponobj *weapon;
	s32 prevplayernum = g_Vars.currentplayernum;
	s32 ammotype;
	s32 qty = 0;
	s32 i;

	if (PLAYERCOUNT() <= 2 || obj == NULL || obj->type != OBJTYPE_WEAPON || !coopIsOn()) {
		return;
	}

	weapon = (struct weaponobj *)obj;

	setCurrentPlayerNum(givenplayernum);
	ammotype = bgunGetAmmoTypeForWeapon(weapon->weaponnum, FUNC_PRIMARY);

	if (ammotype > 0) {
		qty = bgunGetReservedAmmoCount(ammotype);
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct player *p = g_Vars.players[i];

		if (i == givenplayernum || !p || !p->prop || !coopIsCoopPlayer(p)) {
			continue;
		}

		setCurrentPlayerNum(i);
		invGiveSingleWeapon(weapon->weaponnum);

		if (ammotype > 0 && bgunGetReservedAmmoCount(ammotype) < qty) {
			bgunSetAmmoQuantity(ammotype, qty);
		}
	}

	setCurrentPlayerNum(prevplayernum);
}

bool coopAllDead(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		// online counter-op: the agents' side is the mission's (a
		// counter-op's next guard is always to come)
		if (coopAntiMany() && coopIsAntiNum(i)) {
			continue;
		}

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

		if (coopAntiMany() && coopIsAntiNum(i)) {
			continue;
		}

		if (p && !(p->isdead && p->redbloodfinished && p->deathanimfinished) && !netPlayerOutOfPlay(i)) {
			return false;
		}
	}

	return true;
}

/**
 * A counter-op mission's outcome, the game's own questions about bond and
 * anti asked of each side: offline the one bond and the one anti, online
 * every agent and every counter-op.
 */
bool coopAgentsAborted(void)
{
	s32 i;

	if (!coopAntiMany()) {
		return g_Vars.bond && g_Vars.bond->aborted;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_Vars.players[i] && g_Vars.players[i]->aborted && !coopIsAntiNum(i)) {
			return true;
		}
	}

	return false;
}

bool coopAntisAborted(void)
{
	s32 i;

	if (!coopAntiMany()) {
		return g_Vars.anti && g_Vars.anti->aborted;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_Vars.players[i] && g_Vars.players[i]->aborted && coopIsAntiNum(i)) {
			return true;
		}
	}

	return false;
}

bool coopAgentsDead(void)
{
	if (!coopAntiMany()) {
		return g_Vars.bond && g_Vars.bond->isdead;
	}

	return coopAllDead();
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
	struct prop *me;
	s32 best = -1;
	s32 any = -1;
	f32 bestsq = 0;
	s32 i;

	if (coopTwoRules()) {
		// the game's own answer: bond's other is coop, coop's is bond
		return playernum == g_Vars.bondplayernum ? g_Vars.coopplayernum : g_Vars.bondplayernum;
	}

	me = playernum >= 0 && playernum < MAX_PLAYERS && g_Vars.players[playernum] ? g_Vars.players[playernum]->prop : NULL;

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];

		if (i == playernum || !p || !p->prop || (coopAntiMany() && coopIsAntiNum(i))) {
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
	s32 best = -1;
	f32 besthealth = 0;
	s32 i;

	if (coopTwoRules()) {
		return playernum == g_Vars.bondplayernum ? g_Vars.coopplayernum : g_Vars.bondplayernum;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];
		f32 health;

		if (i == playernum || !p || !p->prop || p->isdead || !p->prop->chr || (coopAntiMany() && coopIsAntiNum(i))) {
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

	if (coopTwoRules()) {
		return (g_Vars.lvframenum & 1) ? g_Vars.bond->prop : g_Vars.coop->prop;
	}

	if (coopAntiMany()) {
		// online counter-op: the agents in turn (a camera or autogun is
		// the counter-ops' own side)
		s32 agents[MAX_PLAYERS];
		s32 n = 0;
		s32 i;

		for (i = 0; i < count && i < MAX_PLAYERS; i++) {
			if (g_Vars.players[i] && g_Vars.players[i]->prop && !coopIsAntiNum(i)) {
				agents[n++] = i;
			}
		}

		if (n == 0) {
			return g_Vars.bond->prop;
		}

		return g_Vars.players[agents[(n - 1) - (g_Vars.lvframenum % n)]]->prop;
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
		&& modSpectatePropNoticeable(g_Vars.players[playernum]->prop)
		// a counter-op is the guards' own side
		&& !(coopAntiMany() && coopIsAntiNum(playernum));
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

s32 coopNextLivingAfter(s32 playernum)
{
	const s32 from = playernum >= 0 && playernum < MAX_PLAYERS ? playernum : MAX_PLAYERS - 1;
	s32 i;

	for (i = 1; i <= MAX_PLAYERS; i++) {
		const s32 next = (from + i) % MAX_PLAYERS;

		if (coopPlayerTargetable(next)) {
			return next;
		}
	}

	return playernum;
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

	if (coopTwoRules()) {
		const s32 other = coopOtherPlayerNum(chr->p1p2);

		return coopPlayerAlive(other) ? other : chr->p1p2;
	}

	if (!chr->prop) {
		// a stage's background list (g_BgChrs: no prop, so nothing is
		// nearest it) asks each living player in turn, as it asks both at
		// two: A51 Escape's hangar check (if_chr_in_room on CHR_P1P2) had
		// stayed on player 0 for ever, and on a dead open seat once player
		// 0's death handed the list to it (F3 20261009-160132)
		return coopNextLivingAfter(chr->p1p2);
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

/**
 * A converted mission's chrs a counter-op may not take because a list names
 * them (a bit per chrnum below 0xf0). Taking a chr frees it, and a list
 * asking whether a chr is dead (0x0033, 0x0034) passes on a chr that is not
 * there: a named one would fire its death or exit scripts. Read once a
 * stage from every list the stage has.
 */
static u8 g_CoopNamedChrs[0x100 / 8];
static struct ailist *g_CoopNamedLists = NULL;
static s32 g_CoopNamedStage = -1;

static void coopNameChr(u8 chrnum)
{
	if (chrnum < 0xf0) {
		g_CoopNamedChrs[chrnum >> 3] |= 1 << (chrnum & 7);
	}
}

static void coopScanNamedChrs(void)
{
	struct ailist *lists = g_StageSetup.ailists;
	s32 i;

	if (g_CoopNamedStage == g_Vars.stagenum && g_CoopNamedLists == lists) {
		return;
	}

	g_CoopNamedStage = g_Vars.stagenum;
	g_CoopNamedLists = lists;
	memset(g_CoopNamedChrs, 0, sizeof(g_CoopNamedChrs));

	for (i = 0; lists && lists[i].list; i++) {
		u8 *cmd = lists[i].list;
		s32 steps = 0;

		while (((cmd[0] << 8) | cmd[1]) != AICMD_END && steps++ < 100000) {
			const s32 type = (cmd[0] << 8) | cmd[1];
			const u32 len = chraiGetCommandLength(cmd, 0);

			switch (type) {
			case 0x0005: case 0x0026: case 0x002f: case 0x0030: case 0x0031:
			case 0x0033: case 0x0034: case 0x0045: case 0x0054: case 0x0055:
			case 0x005b: case 0x0068: case 0x0069: case 0x0081: case 0x0082:
			case 0x0083: case 0x009e: case 0x009f: case 0x00a0: case 0x00a7:
			case 0x00a8: case 0x00a9: case 0x00b1: case 0x00b3: case 0x00ca:
			case 0x00e2: case 0x01e4: case 0x01e6: case 0x01e7:
				coopNameChr(cmd[2]);
				break;
			case 0x001a:
				coopNameChr(cmd[2]);
				coopNameChr(cmd[3]);
				break;
			case 0x006a:
				coopNameChr(cmd[3]);
				break;
			case 0x0056: case 0x0057: case 0x00c7:
				coopNameChr(cmd[4]);
				break;
			}

			if (len == 0) {
				break;
			}

			cmd += len;
		}
	}
}

// chr's own list makes it something a plain guard is not (a civilian, one
// that cannot die, one kept hidden): what a counter-op's first guard, taken
// before any list has run, would otherwise miss
static bool coopListMakesSpecial(u8 *list)
{
	const u32 special = CHRCFLAG_KILLCOUNTABLE | CHRCFLAG_INVINCIBLE | CHRCFLAG_HIDDEN | CHRCFLAG_UNPLAYABLE;
	u8 *cmd = list;
	s32 steps = 0;

	while (cmd && ((cmd[0] << 8) | cmd[1]) != AICMD_END && steps++ < 4000) {
		const u32 len = chraiGetCommandLength(cmd, 0);

		// set_self_chrflag
		if (((cmd[0] << 8) | cmd[1]) == 0x00a4
				&& (((u32)cmd[2] << 24 | (u32)cmd[3] << 16 | (u32)cmd[4] << 8 | cmd[5]) & special)) {
			return true;
		}

		if (len == 0) {
			break;
		}

		cmd += len;
	}

	return false;
}

static bool coopWeaponShoots(s32 weaponnum)
{
	struct weapon *weapon = weaponFindById(weaponnum);
	struct weaponfunc *func = weapon ? weapon->functions[FUNC_PRIMARY] : NULL;

	return func != NULL && (func->type & 0xff) == INVENTORYFUNCTYPE_SHOOT;
}

// chr holds something that shoots, in either hand
static bool coopChrArmed(struct chrdata *chr)
{
	s32 h;

	for (h = 0; h < 2; h++) {
		struct prop *held = chr->weapons_held[h];

		if (held && held->obj && held->obj->type == OBJTYPE_WEAPON && held->weapon
				&& coopWeaponShoots(held->weapon->weaponnum)) {
			return true;
		}
	}

	return false;
}

/**
 * A converted mission's chr a counter-op may take. Nothing in a conversion
 * marks a plain guard (no list sets CHRHFLAG_BASICGUARD, and a record keeps
 * only GoldenEye's sunglasses and invincible flags), so the guard is told by
 * what it is not:
 * - one of the players' side, a non-combatant, psychosised, or a simulant;
 * - a civilian (GoldenEye's civilian bit is CHRCFLAG_KILLCOUNTABLE),
 *   invincible, hidden or unplayable, now or by its own list's first word;
 * - unarmed, or holding nothing that shoots;
 * - a named character: on GoldenEye's own conversion by its character number
 *   (the guards are 0-4, 17-21 and 37-40: Boris, Ourumov, Trevelyan,
 *   Natalya and the rest are not), on a hack by a body that carries its own
 *   head, as GoldenEye's named people's do;
 * - a chr any of the stage's lists names (coopScanNamedChrs()).
 */
bool coopConvertedGuard(struct chrdata *chr)
{
	const u32 never = CHRCFLAG_KILLCOUNTABLE | CHRCFLAG_INVINCIBLE | CHRCFLAG_HIDDEN | CHRCFLAG_UNPLAYABLE;

	if (chr->chrnum < 0 || chr->aibot || (chr->hidden & CHRHFLAG_PSYCHOSISED)
			|| (chr->team & (TEAM_ALLY | TEAM_NONCOMBAT)) || (chr->chrflags & never)) {
		return false;
	}

	if (!coopChrArmed(chr) || chr->bodynum < 0) {
		return false;
	}

	if (modloaderStageIsGexPlus(g_Vars.stagenum)) {
		const s32 gechr = gexPlusRomChrForRow(chr->bodynum);

		if (gechr >= 0) {
			if (!((gechr >= 0 && gechr <= 4) || (gechr >= 17 && gechr <= 21) || (gechr >= 37 && gechr <= 40))) {
				return false;
			}
		} else if (g_HeadsAndBodies[chr->bodynum].unk00_01) {
			return false;
		}
	} else if (g_HeadsAndBodies[chr->bodynum].unk00_01) {
		return false;
	}

	coopScanNamedChrs();

	if (chr->chrnum < 0xf0 && (g_CoopNamedChrs[chr->chrnum >> 3] & (1 << (chr->chrnum & 7)))) {
		return false;
	}

	if (chr->ailist && coopListMakesSpecial(chr->ailist)) {
		return false;
	}

	return true;
}
