#include <ultra64.h>
#include "constants.h"
#include "game/game_006900.h"
#include "game/lang.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "game/modghost.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/modrun.h"
#include "bss.h"
#include "lang.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"
#include <stdio.h>
#include <string.h>

/**
 * The Randomizer - the mode's own corner of the main menu.
 *
 * Everything here was reachable before through Dab's Mod Options, which is
 * fine for a setting and wrong for a mode, and the Randomizer had grown into
 * two ways of playing rather than a preference about how a mission behaves:
 * a mission dealt again from its own pieces, and a run that keeps dealing
 * rooms across every map in the game until the player dies. So it gets a door
 * next to the one marked Solo Missions, and Ghost Trials is the pattern for
 * what is behind it.
 *
 * Random Mission does not clone the mission select, for the reason
 * ghostmenu.c gives: it arms the roll and pushes the same dialog the stock
 * Solo Missions item pushes, so the mission list, the difficulty, the
 * briefing and every unlock rule behind them are the ones the game already
 * has.
 *
 * Start Run pushes no dialog at all. A run picks its own map, so there is
 * nothing to choose on the way in and a mission select would be a list of
 * things the mode is going to ignore.
 */

static char g_RandomRowText[96];

/**
 * Start a run: one room at a time across every map, until the first death.
 */
static MenuItemHandlerResult menuhandlerRunStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
#ifndef PLATFORM_N64
		// A run is not a trial and not an ordinary mission; both of the other
		// doors' arming comes off on the way through this one.
		modGhostDisarmTrial();
		modRandomDisarmMission();
#endif
		modRunStart();
	}

	return 0;
}

/**
 * A mission dealt again from its own pieces: the roll armed for one mission,
 * and then the game's own mission select.
 */
static MenuItemHandlerResult menuhandlerRandomMission(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_MissionConfig.iscoop = false;
		g_MissionConfig.isanti = false;

#ifndef PLATFORM_N64
		modGhostDisarmTrial();
#endif
		modRandomArmMission();
		menuPushDialog(&g_SelectMissionMenuDialog);
	}

	return 0;
}

/**
 * The run's score, on a row of its own.
 *
 * What the run in progress has done while it is being made, and the best kept
 * otherwise. A death is a fade and a failure dialog, so a number shown there
 * is a number nobody reads: this is where a player goes looking for it
 * afterwards.
 */
static char *menutextRunBest(struct menuitem *item)
{
	if (modRunIsOn()) {
		snprintf(g_RandomRowText, sizeof(g_RandomRowText),
				"This run: %d objectives, %d rooms (best %d)\n",
				modRunGetScore(), modRunGetRooms(), modRunGetBestScore());
	} else if (modRunGetBestScore() > 0 || modRunGetBestRooms() > 0) {
		snprintf(g_RandomRowText, sizeof(g_RandomRowText),
				"Best run: %d objectives, %d rooms\n",
				modRunGetBestScore(), modRunGetBestRooms());
	} else {
		snprintf(g_RandomRowText, sizeof(g_RandomRowText), "No run yet\n");
	}

	return g_RandomRowText;
}

/**
 * Which maps a run may land in.
 *
 * Everything is the default and the point of the mode. The narrower two are
 * for a build where a particular map has spoiled a run: a mod's map that
 * faults takes an hour of play with it, and this is the only thing a player
 * can do about that without editing pd.ini.
 */
static MenuItemHandlerResult menuhandlerRunPool(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Solo Missions", "Missions + Arenas", "Everything" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.runpool = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetRunPool();
	}

	return 0;
}

/**
 * The difficulty every room of a run is played on. One for the whole run:
 * the rooms are not missions and there is no briefing to choose it at.
 */
static MenuItemHandlerResult menuhandlerRunDifficulty(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Agent", "Special Agent", "Perfect Agent" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.rundifficulty = DIFF_A + data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetRunDifficulty() - DIFF_A;
	}

	return 0;
}

/**
 * The seed: the run, or a fresh one every time.
 *
 * There is nowhere in this menu to type a number, and a seed is not a thing
 * anyone invents anyway - it is a thing they keep. So the choice is between
 * dealing something new every time and holding on to what was just dealt,
 * shown so it can be written down. A seed typed by hand goes in pd.ini as
 * Mod.RandomizerSeed, and one somebody else hands over arrives the same way.
 */
static MenuItemHandlerResult menuhandlerRandomSeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[32];

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 2;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value == 0) {
			return (intptr_t)"New Each Time";
		}

		if (g_ModOptions.randomseed) {
			snprintf(text, sizeof(text), "%u v%d", (u32)g_ModOptions.randomseed, g_ModOptions.randomversion);
		} else if (modRunIsOn() && modRunGetSeed()) {
			snprintf(text, sizeof(text), "Keep %u", modRunGetSeed());
		} else if (modRandomGetSeed()) {
			snprintf(text, sizeof(text), "Keep %u v%d", modRandomGetSeed(), modRandomGetVersion());
		} else {
			snprintf(text, sizeof(text), "Keep This Run");
		}

		return (intptr_t)text;
	case MENUOP_SET:
		if (data->dropdown.value == 0) {
			g_ModOptions.randomseed = 0;
		} else if (g_ModOptions.randomseed == 0) {
			// The seed the last thing dealt came from - a run's if one is
			// under way, the last mission's otherwise - so that liking what
			// was dealt and keeping it is one press rather than a number to
			// copy out of a log.
			const u32 seed = modRunIsOn() && modRunGetSeed() ? modRunGetSeed() : modRandomGetSeed();

			g_ModOptions.randomseed = (s32)(seed & S32_MAX);
			g_ModOptions.randomversion = modRandomGetVersion();
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.randomseed ? 1 : 0;
	}

	return 0;
}

/**
 * Endless Mode, which belongs to Random Mission rather than to a run: one
 * objective at a time across the whole map, dealt again the moment it is
 * finished, scored in rooms covered. A run has its own idea of both and turns
 * this off while it is playing.
 */
static MenuItemHandlerResult menuhandlerRandomEndless(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.randomendless;
	case MENUOP_SET:
		g_ModOptions.randomendless = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * The best an Endless Mode mission has managed, kept beside its checkbox for
 * the same reason the run's score is kept beside Start Run.
 */
static char g_RandomEndlessRowText[96];

static char *menutextEndlessBest(struct menuitem *item)
{
	if (modRandomIsEndless() && modRandomGetRooms() > 0) {
		snprintf(g_RandomEndlessRowText, sizeof(g_RandomEndlessRowText),
				"This mission: %d rooms, %d objectives (best %d)\n",
				modRandomGetRooms(), modRandomGetCleared(), g_ModOptions.endlessbest);
	} else if (g_ModOptions.endlessbest > 0) {
		snprintf(g_RandomEndlessRowText, sizeof(g_RandomEndlessRowText),
				"Best endless mission: %d rooms\n", g_ModOptions.endlessbest);
	} else {
		snprintf(g_RandomEndlessRowText, sizeof(g_RandomEndlessRowText), "No endless mission yet\n");
	}

	return g_RandomEndlessRowText;
}

struct menuitem g_RandomOptionsMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Run Maps",
		0,
		menuhandlerRunPool,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Run Difficulty",
		0,
		menuhandlerRunDifficulty,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Seed",
		0,
		menuhandlerRandomSeed,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Endless Random Mission",
		0,
		menuhandlerRandomEndless,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextEndlessBest,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_RandomOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Randomizer Options",
	g_RandomOptionsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * The rows are the main menu's, not the options menu's - MENUITEMFLAG_BIGFONT
 * and no trailing newline, the way every row of the Perfect Menu is written.
 * This page sits next to Solo Missions rather than inside Options, and reading
 * like a settings page was the wrong signal about what it is.
 */
struct menuitem g_RandomizerMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Start Run",
		0,
		menuhandlerRunStart,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextRunBest,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Random Mission",
		0,
		menuhandlerRandomMission,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Randomizer Options",
		0,
		(void *)&g_RandomOptionsMenuDialog,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_RandomizerMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Randomizer",
	g_RandomizerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
