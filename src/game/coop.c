#include <ultra64.h>
#include "constants.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/coop.h"
#include "game/playermgr.h"
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
		if (g_Vars.players[i] && !g_Vars.players[i]->isdead) {
			return false;
		}
	}

	return true;
}

bool coopAllDeadDone(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *p = g_Vars.players[i];

		if (p && !(p->isdead && p->redbloodfinished && p->deathanimfinished)) {
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
