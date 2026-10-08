#include <ultra64.h>
#include "constants.h"
#include "game/bondgun.h"
#include "game/hudmsg.h"
#include "game/inv.h"
#include "game/modoptions.h"
#include "game/modrespawn.h"
#include "game/music.h"
#include "game/player.h"
#include "net/net.h"
#include "bss.h"
#include "data.h"
#include "types.h"

/**
 * Mission Respawn.
 *
 * Stock, a solo death is the death animation, a fade to black and Mission
 * Failed; only the Combat Simulator and co-operative have a next life. With
 * this on, a mission has one too: the same animation and fade, then the
 * player stands up where they fell with full health, the inventory they had
 * and the guns they were holding, the mission's clocks and objectives
 * untouched. It rides the co-operative restart's machinery - dostartnewlife
 * and playerStartNewLife() - with three differences that live here: where
 * the new life starts (the spot of the death rather than the mission's
 * spawn pad), what it keeps (everything; a mission's kit is what its
 * objectives need, and the co-operative branch of playerStartNewLife()
 * already keeps the inventory), and what goes in the hands (what was held
 * at the death, rather than another roll of Start Armed's Random).
 *
 * Lives count from the mission's start: with five, the fifth death is
 * Mission Failed as before. Unlimited is the first setting because the
 * point of the option is to keep playing.
 */

static s32 g_ModRespawnDeaths = 0; // this mission's, all players
static bool g_ModRespawning[MAX_PLAYERS];
static s32 g_ModRespawnWeapons[MAX_PLAYERS][2];

/**
 * Stage start.
 */
void modRespawnReset(void)
{
	s32 i;

	g_ModRespawnDeaths = 0;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ModRespawning[i] = false;
		g_ModRespawnWeapons[i][HAND_LEFT] = WEAPON_NONE;
		g_ModRespawnWeapons[i][HAND_RIGHT] = WEAPON_NONE;
	}

#ifndef PLATFORM_N64
	// online co-op plays by these rules too, its lives the team's (netcoop.c)
	netCoopStageReset();
#endif
}

/**
 * Whether the death being played out ends in a new life rather than the
 * stage. A mission only: co-operative and counter-operative have their own
 * rules for a dead player, and a match its own respawn.
 */
bool modRespawnCanRespawn(void)
{
	s32 lives;

	if (g_Vars.mplayerisrunning || !modIsMissionRespawnOn()) {
		return false;
	}

	lives = modGetMissionLives();

	return lives == MODLIVES_UNLIMITED || g_ModRespawnDeaths + 1 < lives;
}

/**
 * At the death, before the guns are thrown off the screen: what the hands
 * held, so the new life can hold it again.
 */
void modRespawnRecordDeath(void)
{
	s32 playernum = g_Vars.currentplayernum;

	g_ModRespawnWeapons[playernum][HAND_RIGHT] = bgunGetWeaponNum(HAND_RIGHT);
	g_ModRespawnWeapons[playernum][HAND_LEFT] = bgunGetWeaponNum(HAND_LEFT);
}

/**
 * The fade has finished: ask for the new life. Once per death - this is
 * asked every tick until playerStartNewLife() clears dostartnewlife.
 */
void modRespawnBegin(void)
{
	if (g_Vars.currentplayer->dostartnewlife) {
		return;
	}

	g_ModRespawnDeaths++;
	g_ModRespawning[g_Vars.currentplayernum] = true;
	g_Vars.currentplayer->dostartnewlife = true;
}

/**
 * netplay, a client: the host's new life for this machine's player is
 * Mission Respawn's (netcoop.c), and the local copy of it is set up the same
 * way - where the player fell, with the kit and guns it had, the fade back in.
 * The host counted the death.
 */
void modRespawnMark(void)
{
	g_ModRespawning[g_Vars.currentplayernum] = true;
}

/**
 * True from the ask until the new life is set up, which is how
 * playerStartNewLife() and playerSpawnWeapons() know to keep the position,
 * the inventory and the guns.
 */
bool modRespawnIsRespawning(void)
{
	return g_ModRespawning[g_Vars.currentplayernum];
}

/**
 * The gun that hand held at the death, or the mission's default if it is no
 * longer in the inventory.
 */
s32 modRespawnGetWeapon(s32 handnum)
{
	s32 weaponnum = g_ModRespawnWeapons[g_Vars.currentplayernum][handnum];

	if (weaponnum > WEAPON_NONE && !invHasSingleWeaponIncAllGuns(weaponnum)) {
		weaponnum = handnum == HAND_RIGHT ? g_DefaultWeapons[HAND_RIGHT] : WEAPON_NONE;
	}

	return weaponnum;
}

/**
 * The new life is set up: bring the picture and the music back, and say
 * how many lives are left. The fade is the death's own, black over a
 * second; this is the same second the other way, as the intro's fade-in
 * is. musicEndDeath() is what a match's respawn calls for the same track.
 */
void modRespawnEnd(void)
{
	static char text[32];
	s32 lives = modGetMissionLives();

	g_ModRespawning[g_Vars.currentplayernum] = false;

	// netplay: the music is this machine's, never another's player's to end
	if (netIsLocalSlot(g_Vars.currentplayernum)) {
		musicEndDeath();
	}

	playerSetFadeColour(0, 0, 0, 1);
	playerSetFadeFrac(60, 0);

	// netplay: a client's line comes from the host, which counts the team's
	// lives (a hudmsg for another machine's player goes to it)
	if (lives != MODLIVES_UNLIMITED && !NET_CLIENT) {
		s32 left = lives - g_ModRespawnDeaths;

		// Four seconds where the default type's second and a bit would be
		// gone before the fade-in had let the player read it
		extern struct hudmsgtype g_HudmsgTypes[];

		sprintf(text, left == 1 ? "Last life\n" : "%d lives left\n", left);
		hudmsgCreateWithDuration(text, HUDMSGTYPE_DEFAULT, &g_HudmsgTypes[HUDMSGTYPE_DEFAULT], TICKS(240));
	}
}
