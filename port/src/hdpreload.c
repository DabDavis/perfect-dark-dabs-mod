/**
 * A GoldenEye stage's HD (Bean) meshes, built while the stage loads.
 *
 * In the HD look a GoldenEye model's mesh used to be built on the game thread
 * the first frame the model was drawn - 20-100 ms for a first-person gun, 4-15
 * ms for a character - which froze the game each time a new guard, prop or
 * gun came into view or into the hand (F3 20261003-123556). On a GE Plus or
 * converted stage in the HD look, lvReset() now switches the build on for the
 * whole of the load (hdPreloadBegin()): every model the setup loads - the
 * characters and heads, the props, the guns the guards carry and the ones
 * lying about - is built as it registers (xblaMeshRegisterModel()). What the
 * setup does not load is the first-person models, which bondgun.c loads one at
 * a time as a gun is drawn: hdPreloadEnd() builds those of every gun the stage
 * has handed out by then - in the players' hands, in the guards' and on the
 * floor - and loads the third-person model of each gun the players start with.
 *
 * The meshes go into the cache the draw reads (xblamesh.c's beanBuilt, by
 * file, kept across stages), so nothing about the draw changes but when the
 * work is done. The N64 look and Perfect Dark's own stages are untouched.
 */

#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "game/inv.h"
#include "game/playermgr.h"
#include "game/setuputils.h"
#include "game/game_0b0fd0.h"
#include "system.h"
#include "gebean.h"
#include "modloader.h"
#include "xblamesh.h"
#include "hdpreload.h"

#define WANT_FIRST 1
#define WANT_THIRD 2

// The stage's first ticks, which run its intro AI: guards are handed their
// guns and hats there, after lvReset(). They are the load's own frames - the
// first frame drawn already carries the HD level's setup - so what they load
// is built in them too, rather than on its first frame in play.
#define HDPRELOAD_TICKS 10

static u64 preloadStart;
static s32 stageOn;
static s32 numGuns;
// the first-person files tried this stage, built or not (most of a stage's
// weapons are not GoldenEye's and have no HD mesh to build)
static u8 tried[NUM_FILE_SLOTS];

void hdPreloadBegin(s32 stagenum)
{
	const s32 on = xblaMeshGetEnabled() && gebeanGetEnabled()
			&& modloaderStageIsRemake(stagenum)
			&& modloaderDirIndexIsConversion(modloaderGetStageModDirIndex(stagenum));

	xblaMeshPreloadSet(on);
	preloadStart = sysGetMicroseconds();
	stageOn = xblaMeshPreloading();
	numGuns = 0;
	memset(tried, 0, sizeof(tried));
}

static void hdPreloadWant(u8 *want, s32 weaponnum, u8 what)
{
	if (weaponnum > WEAPON_NONE && weaponnum < NUM_WEAPONS) {
		want[weaponnum] |= what;
	}
}

// A prop and everything it carries: a gun on the floor, or in a hand
static void hdPreloadWalkProp(u8 *want, struct prop *prop, s32 depth)
{
	s32 n = 0;

	if (prop->type == PROPTYPE_WEAPON && prop->weapon && prop->weapon->base.type == OBJTYPE_WEAPON) {
		hdPreloadWant(want, prop->weapon->weaponnum, WANT_FIRST);
	}

	for (struct prop *child = prop->child; child && depth < 4 && n < 64; child = child->next, n++) {
		hdPreloadWalkProp(want, child, depth + 1);
	}
}

static void hdPreloadWalkList(u8 *want, struct prop *list)
{
	s32 n = 0;

	for (struct prop *prop = list; prop && n < g_Vars.maxprops; prop = prop->next, n++) {
		hdPreloadWalkProp(want, prop, 0);
	}
}

static void hdPreloadGuns(void)
{
	u8 want[NUM_WEAPONS];
	const s32 prevplayer = g_Vars.currentplayernum;

	memset(want, 0, sizeof(want));

	// What the players start with: both of a gun's models
	for (s32 i = 0; i < PLAYERCOUNT(); i++) {
		setCurrentPlayerNum(i);

		for (s32 j = 0; j < invGetCount(); j++) {
			const struct invitem *item = invGetItemByIndex(j);

			if (!item) {
				continue;
			}

			if (item->type == INVITEMTYPE_WEAP) {
				hdPreloadWant(want, item->type_weap.weapon1, WANT_FIRST | WANT_THIRD);
			} else if (item->type == INVITEMTYPE_DUAL) {
				hdPreloadWant(want, item->type_dual.weapon1, WANT_FIRST | WANT_THIRD);
				hdPreloadWant(want, item->type_dual.weapon2, WANT_FIRST | WANT_THIRD);
			}
		}

		hdPreloadWant(want, g_Vars.currentplayer->gunctrl.weaponnum, WANT_FIRST | WANT_THIRD);
	}

	// What can be picked up: the guns lying about and the ones the guards
	// hold, which they drop. Their third-person models came with the setup.
	hdPreloadWalkList(want, g_Vars.activeprops);
	hdPreloadWalkList(want, g_Vars.pausedprops);

	for (s32 i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->chrnum < 0 || !chr->prop) {
			continue;
		}

		for (s32 h = 0; h < 2; h++) {
			struct prop *held = chr->weapons_held[h];

			if (held && held->type == PROPTYPE_WEAPON && held->weapon) {
				hdPreloadWant(want, held->weapon->weaponnum, WANT_FIRST);
			}
		}
	}

	for (s32 w = 0; w < NUM_WEAPONS; w++) {
		// the first-person model as bondgun.c would pick it for player 1
		if (want[w] & WANT_FIRST) {
			u16 fileid;

			setCurrentPlayerNum(0);
			fileid = weaponGetFileNum(w);

			if (fileid && fileid < NUM_FILE_SLOTS && !tried[fileid]) {
				tried[fileid] = 1;
				numGuns += xblaMeshPreloadModelFile(fileid);
			}
		}

		if (want[w] & WANT_THIRD) {
			const s32 modelnum = playermgrGetModelOfWeapon(w);

			if (modelnum >= 0 && modelnum < NUM_MODELS && g_ModelStates[modelnum].fileid) {
				setupLoadModeldef(modelnum);
			}
		}
	}

	setCurrentPlayerNum(prevplayer);
}

void hdPreloadEnd(void)
{
	if (stageOn) {
		hdPreloadGuns();
	}
}

void hdPreloadTick(void)
{
	s32 meshes;
	u64 us;

	if (!stageOn) {
		return;
	}

	if (g_Vars.lvframenum <= HDPRELOAD_TICKS) {
		hdPreloadGuns();
		return;
	}

	stageOn = 0;
	xblaMeshPreloadStats(&meshes, &us);
	xblaMeshPreloadSet(0);

	sysLogPrintf(LOG_NOTE, "hdpreload: %d HD meshes built while the stage loaded (%d of them first-person guns), "
			"%.1f ms; the load and first ticks took %.1f ms", meshes, numGuns, us / 1000.0,
			(sysGetMicroseconds() - preloadStart) / 1000.0);
}
