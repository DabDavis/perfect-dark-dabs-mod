#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "bss.h"
#include "game/modghost.h"
#include "config.h"
#include "ghostnet.h"
#include "game/lang.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/setup.h"

/**
 * Ghost Trials - the ghost feature's own corner of the main menu.
 *
 * Everything here was reachable before through Dab's Mod Options and a mission
 * started the ordinary way, which is fine for a setting and wrong for a mode.
 * A time trial is a way of playing the game rather than a preference about it,
 * so it gets a door next to the one marked Solo Missions.
 *
 * Ghost Mission does not clone the mission select. It arms the trial and then
 * pushes the same dialog the stock Solo Missions item pushes, so the mission
 * list, the difficulty, the briefing, the accept screen and every unlock rule
 * behind them are the ones the game already has. A copy of that flow would be
 * a second place for those rules to be wrong.
 */

static char g_GhostRowText[96];

/**
 * Whose a ghost is, for the player to read.
 *
 * The account if the file carries one, because that is the name on the
 * leaderboard and the thing another player can be found by. The agent name is
 * the fallback for runs recorded before ghosts carried an account, and it is
 * only ever a label on a save file - two people can both be Joanna.
 */
static const char *menuGhostWhose(const struct modghostentry *entry)
{
	return entry->owner[0] ? entry->owner : entry->player;
}

/**
 * The mark on a run that cannot be raced or published.
 *
 * A question mark, because what is wrong with it is not that it is slow: it was
 * recorded before trials fixed the rules, so nothing can say whether it was set
 * with a jump. Marked rather than hidden - it is still a file the player has,
 * and a row they cannot see is a row they cannot delete.
 */
static char menuGhostMark(const struct modghostentry *entry)
{
	return (entry->flags & MODGHOSTHF_TRIALRULES) ? ' ' : '?';
}

/**
 * Start a mission as a trial: recording on whatever the global setting says.
 *
 * The arming is a flag rather than a write to g_ModGhostMode, because the mode
 * is saved to pd.ini. Coming in through this door for one mission should not
 * leave every later mission recording, and the stock Solo Missions item
 * disarms it on the way past for exactly that reason.
 */
static MenuItemHandlerResult menuhandlerGhostMission(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_MissionConfig.iscoop = false;
		g_MissionConfig.isanti = false;

		modGhostArmTrial();
		menuPushDialog(&g_SelectMissionMenuDialog);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostRacers(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = MODGHOST_MAXRACERS;
		break;
	case MENUOP_GETOPTIONTEXT:
		snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%d", (s32)data->dropdown.value + 1);
		return (intptr_t)g_GhostRowText;
	case MENUOP_SET:
		g_ModGhostMaxRacers = data->dropdown.value + 1;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModGhostMaxRacers - 1;
	}

	return 0;
}

/**
 * What a trial does: run alone against the clock, or against the field.
 *
 * There is no Off. A trial records - that is what the door is for - and a
 * player who wants to play the mission without any of this plays Solo
 * Missions. Racing does not turn recording off either, because the run worth
 * keeping is usually the one raced, and a mode that watched a good run go past
 * without writing it down would be a trap rather than a setting.
 *
 * The values are the MODGHOST_* ones, so the list is offset by Record Only
 * rather than starting at zero.
 */
static MenuItemHandlerResult menuhandlerGhostMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Record Only", "Record + Race" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModGhostMode = data->dropdown.value + MODGHOST_RECORD;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModGhostMode <= MODGHOST_RECORD
			? 0 : g_ModGhostMode - MODGHOST_RECORD;
	}

	return 0;
}

/**
 * Which ghosts make up the field.
 *
 * Fastest and My Best pick themselves from whatever is on disk and need no
 * upkeep. Chosen races exactly what was ticked in the chooser, which is the
 * one that lets a downloaded run be raced against your own on purpose rather
 * than because it happened to be quick.
 */
static MenuItemHandlerResult menuhandlerGhostPick(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Fastest Available", "My Best Only", "Chosen Ghosts" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModGhostPick = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModGhostPick;
	}

	return 0;
}

/**
 * Customize Character: who the player is in a trial, and so who their ghost is.
 *
 * The Combat Simulator character page, pointed at the trial's own storage. The
 * two carousels and the turning model beside them are that page's handlers -
 * mpCharacterBodyMenuHandler and mpCharacterHeadMenuHandler both take the body
 * and head to show as arguments, so what makes the arena page the arena page is
 * only which variables the wrappers read. These wrappers read the trial's.
 *
 * Reusing them rather than copying them is the difference between one character
 * picker with two entry points and two pickers that drift apart. The preview
 * model, the unlock check on bodies, the head that follows a body unless it was
 * chosen on purpose - all of that is behaviour the arena page already has and
 * this one would otherwise have to grow badly.
 *
 * The choice is written into every run recorded afterwards, so changing it does
 * not restyle the ghosts already on disk. They keep whoever set them.
 */
static char *menutextGhostCharacterName(struct menuitem *item)
{
	if (g_ModGhostBody <= MODGHOST_BODY_DEFAULT) {
		snprintf(g_GhostRowText, sizeof(g_GhostRowText), "Joanna\n");
	} else {
		snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%s\n",
				mpGetBodyName(g_ModGhostBody - 1));
	}

	return g_GhostRowText;
}

static MenuItemHandlerResult menuhandlerGhostCharacterBody(s32 operation, struct menuitem *item, union handlerdata *data)
{
	// The stored values are one above the index, so that zero can mean the
	// default. The arena handlers deal in the index itself.
	s32 body = g_ModGhostBody > MODGHOST_BODY_DEFAULT ? g_ModGhostBody - 1 : 0;
	s32 head = g_ModGhostHead > MODGHOST_BODY_DEFAULT ? g_ModGhostHead - 1 : mpGetMpheadnumByMpbodynum(body);

	switch (operation) {
	case MENUOP_SET:
		g_ModGhostBody = data->carousel.value + 1;

		// A body brings its own head unless one was picked on purpose, which
		// is what the arena page does and the reason a head can be left alone.
		if (g_ModGhostHead <= MODGHOST_BODY_DEFAULT) {
			g_ModGhostHead = mpGetMpheadnumByMpbodynum(data->carousel.value) + 1;
		}
		break;
	case MENUOP_CHECKPREFOCUSED:
		mpCharacterBodyMenuHandler(operation, item, data, body, head, true);
		return true;
	}

	return mpCharacterBodyMenuHandler(operation, item, data, body, head, true);
}

static MenuItemHandlerResult menuhandlerGhostCharacterHead(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 head = g_ModGhostHead > MODGHOST_BODY_DEFAULT ? g_ModGhostHead - 1 : 0;

	if (operation == MENUOP_SET) {
		g_ModGhostHead = data->carousel.value + 1;
	}

	return mpCharacterHeadMenuHandler(operation, item, data, head, true);
}

static MenuDialogHandlerResult menudialogGhostCharacter(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	// Keep the model turning while neither carousel has the focus, the way the
	// arena page does. Without it the preview freezes the moment the cursor
	// sits on the name at the top.
	if (operation == MENUOP_TICK
			&& g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef
			&& g_Menus[g_MpPlayerNum].curdialog->focuseditem != &dialogdef->items[1]
			&& g_Menus[g_MpPlayerNum].curdialog->focuseditem != &dialogdef->items[2]) {
		union handlerdata scratch;
		menuhandlerGhostCharacterBody(MENUOP_11, &dialogdef->items[2], &scratch);
	}

	return 0;
}

struct menuitem g_GhostCharacterMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_DARKERBG,
		(uintptr_t)&menutextGhostCharacterName,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_CAROUSEL,
		0,
		0,
		0,
		0x00000022,
		menuhandlerGhostCharacterHead,
	},
	{
		MENUITEMTYPE_CAROUSEL,
		0,
		0,
		0,
		0x0000001b,
		menuhandlerGhostCharacterBody,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_GhostCharacterMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Customize Character",
	g_GhostCharacterMenuItems,
	menudialogGhostCharacter,
	MENUDIALOGFLAG_0002 | MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static const s32 g_GhostAlphaValues[] = { 60, 110, 170, 230 };

static MenuItemHandlerResult menuhandlerGhostAlpha(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Faint", "Normal", "Strong", "Solid" };
	s32 i;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModGhostAlpha = g_GhostAlphaValues[data->dropdown.value];
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;

		for (i = 0; i < (s32)ARRAYCOUNT(g_GhostAlphaValues); i++) {
			if (g_ModGhostAlpha >= g_GhostAlphaValues[i]) {
				data->dropdown.value = i;
			}
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostSplits(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModGhostSplits;
	case MENUOP_SET:
		g_ModGhostSplits = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * The chooser: every ghost on disk, ticked or not.
 *
 * A list rather than a page of checkboxes because the number of them is not
 * known until the directory is read, and the menu item tables here are static.
 * MENUITEMTYPE_LIST is the game's own answer to that - it asks the handler how
 * many rows there are and what each one says, which is exactly the shape of a
 * directory listing.
 *
 * Selecting a row toggles it. Ten is the ceiling and an eleventh is refused
 * rather than pushing one out, so the row simply does not change and the
 * counter above it explains why.
 */
static MenuItemHandlerResult menuhandlerGhostChooser(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct modghostentry *entry;
	static const char *diffs[] = { "A", "SA", "PA" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = modGhostGetCatalogueCount();
		break;
	case MENUOP_GETOPTIONTEXT:
		entry = modGhostGetCatalogueEntry(data->list.value);

		if (entry == NULL) {
			return (intptr_t)"";
		}

		// Bounded rather than trusted: the stage name comes from the ROM but
		// the player name came out of a file that may have been written
		// anywhere, and a row wide enough to push the dialog off screen is a
		// thing a downloaded ghost should not be able to do.
		// No stage or difficulty on the row: the dropdowns above say what
		// they would say, and the space buys the hundredths that tell two
		// attempts at the same route apart.
		snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%c%c%d:%02d.%02d  %.16s",
				entry->chosen ? '*' : '-',
				menuGhostMark(entry),
				entry->time60 / 3600, (entry->time60 / 60) % 60,
				(entry->time60 % 60) * 100 / 60,
				menuGhostWhose(entry));

		return (intptr_t)g_GhostRowText;
	case MENUOP_SET:
		modGhostToggleChosen(data->list.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0xfffff;
		break;
	}

	return 0;
}

static char *menutextGhostChosenCount(struct menuitem *item)
{
	snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%d of %d chosen - A to toggle\n",
			modGhostGetNumChosen(), MODGHOST_MAXRACERS);

	return g_GhostRowText;
}

/**
 * Which mission the chooser is choosing a field for.
 *
 * A field is raced on one mission, so the page asks about one mission. Listing
 * every run on disk meant scrolling past four stages to find the three rows
 * that could possibly be raced, and ticking one from the wrong stage did
 * nothing at all - the scan that builds the field filters by stage anyway, so
 * the chooser was offering choices that could not have an effect.
 *
 * The pair is kept across openings rather than reset, because the mission
 * somebody is working on is the one they were working on a minute ago.
 */
static s32 g_GhostChooserStageIndex = 0;
static s32 g_GhostChooserDiff = 0;

static void menuGhostChooserRescan(void)
{
	modGhostSetCatalogueFilter(g_SoloStages[g_GhostChooserStageIndex].stagenum,
			g_GhostChooserDiff);
	modGhostScanCatalogue();
}

static MenuItemHandlerResult menuhandlerGhostChooserStage(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = NUM_SOLOSTAGES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)langGet(g_SoloStages[data->dropdown.value].name3);
	case MENUOP_SET:
		g_GhostChooserStageIndex = data->dropdown.value;
		menuGhostChooserRescan();
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_GhostChooserStageIndex;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostChooserDiff(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Agent", "Special Agent", "Perfect Agent" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_GhostChooserDiff = data->dropdown.value;
		menuGhostChooserRescan();
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_GhostChooserDiff;
	}

	return 0;
}

static MenuDialogHandlerResult menudialogGhostChooser(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	// Read the directory when the page opens rather than every frame: it is a
	// header read per file, and nothing changes it while the page is up
	// except a tick, which does not change which files are there.
	if (operation == MENUOP_OPEN) {
		menuGhostChooserRescan();
	}

	return 0;
}

struct menuitem g_GhostChooserMenuItems[] = {
	{
		// No MENUITEMFLAG_LITERAL_TEXT: that flag says param2 is a string, and
		// this one is a function that builds the string. menuResolveText()
		// calls anything above 0x5a00 that is not marked literal, which is the
		// only way a row can say something that changes.
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostChosenCount,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mission",
		0,
		menuhandlerGhostChooserStage,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Difficulty",
		0,
		menuhandlerGhostChooserDiff,
	},
	{
		// param2 is the list width in menu units. The default of 80 clips a
		// row that names a time and a player.
		MENUITEMTYPE_LIST,
		0,
		0,
		0x000000c8,
		0,
		menuhandlerGhostChooser,
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

struct menudialogdef g_GhostChooserMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Choose Ghosts",
	g_GhostChooserMenuItems,
	menudialogGhostChooser,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * My Ghosts: everything in the ghosts directory, whoever ran it.
 *
 * The chooser next door answers "which of these do I want to race" and is
 * therefore about ticks. This page answers "what have I got", which is a
 * different question now that every finished run is kept rather than only the
 * one that beat the last: a stage you have practised is a column of your own
 * attempts, and a run somebody sent you is in among them.
 *
 * So the name goes on every row. Stage, difficulty and time no longer identify
 * a run - two of yours can share all three across a retry, and a downloaded
 * one can land on top of a time of your own - and a list that cannot tell
 * whose a ghost is would show the same row twice with no way to read it. The
 * time carries hundredths for the same reason: whole seconds tie too often
 * between attempts at the same route.
 *
 * It is also where ghosts get thrown away. Keeping every run is what makes the
 * directory grow, so the page that shows the growth is the page that trims it.
 * A is a delete and deletes are not undoable, so the first press arms the row
 * and the second one does it: a confirmation that costs a keypress rather than
 * a dialog, in a list where the player is holding A to move through rows.
 */
static s32 g_GhostMineArmed = -1;

static bool menuIsMyGhost(const struct modghostentry *entry)
{
	if (entry->owner[0]) {
		return strcasecmp(entry->owner, ghostnetGetAccountName()) == 0;
	}

	return strncmp(entry->player, g_GameFile.name[0] ? g_GameFile.name : "player",
			MODGHOST_NAMELEN) == 0;
}

static MenuItemHandlerResult menuhandlerGhostMine(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct modghostentry *entry;
	static const char *diffs[] = { "A", "SA", "PA" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = modGhostGetCatalogueCount();
		break;
	case MENUOP_GETOPTIONTEXT:
		entry = modGhostGetCatalogueEntry(data->list.value);

		if (entry == NULL) {
			return (intptr_t)"";
		}

		// Bounded the same way the chooser bounds it: the stage name comes
		// from the ROM, the player name came out of a file that may have been
		// written anywhere, and a row wide enough to push the dialog off
		// screen is not something a downloaded ghost gets to do.
		snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%c%c%.16s %-2s %d:%02d.%02d %.12s",
				(s32)data->list.value == g_GhostMineArmed ? '!' : ' ',
				menuGhostMark(entry),
				modGhostStageName(entry->stagenum),
				entry->difficulty < 3 ? diffs[entry->difficulty] : "?",
				entry->time60 / 3600, (entry->time60 / 60) % 60,
				(entry->time60 % 60) * 100 / 60,
				menuGhostWhose(entry));

		return (intptr_t)g_GhostRowText;
	case MENUOP_SET:
		if ((s32)data->list.value == g_GhostMineArmed) {
			modGhostDeleteCatalogueEntry(g_GhostMineArmed);
			g_GhostMineArmed = -1;
		} else {
			g_GhostMineArmed = data->list.value;
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0xfffff;
		break;
	}

	return 0;
}

static char *menutextGhostMineCount(struct menuitem *item)
{
	// Its own buffer rather than the row one: the label and the rows are
	// resolved by the same pass over the page, and a count that shares storage
	// with the row text is a count that reads as a ghost.
	static char text[96];

	s32 count = modGhostGetCatalogueCount();
	s32 mine = 0;
	s32 unraceable = 0;
	s32 i;

	for (i = 0; i < count; i++) {
		struct modghostentry *entry = modGhostGetCatalogueEntry(i);

		if (entry == NULL) {
			continue;
		}

		if (menuIsMyGhost(entry)) {
			mine++;
		}

		if (menuGhostMark(entry) != ' ') {
			unraceable++;
		}
	}

	// Both lines are kept inside the width the dialog gets from the list under
	// them. The unarmed one said "from others" and "deletes one" until the
	// last two characters of it were drawn over the border.
	if (g_GhostMineArmed >= 0 && g_GhostMineArmed < count) {
		snprintf(text, sizeof(text), "A again deletes the marked run - B to leave it\n");
	} else {
		snprintf(text, sizeof(text), "%d here: %d yours, %d unraceable - A twice deletes\n",
				count, mine, unraceable);
	}

	return text;
}

static MenuDialogHandlerResult menudialogGhostMine(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	// Read on open, like the chooser: a header read per file, and the
	// directory only changes here, from a delete that rescans as it goes.
	//
	// Nothing is armed on the way in. Leaving the page and coming back is how
	// a player takes back a press they did not mean, and an arm that survived
	// that would turn the next A into a delete of whatever row had inherited
	// the index.
	if (operation == MENUOP_OPEN) {
		g_GhostMineArmed = -1;
		modGhostSetCatalogueFilter(-1, -1);
		modGhostScanCatalogue();
	}

	return 0;
}

struct menuitem g_GhostMineMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostMineCount,
		0,
		NULL,
	},
	{
		// Same width as the chooser: a row names a stage, a difficulty, a time
		// and a player, and the default of 80 clips it.
		MENUITEMTYPE_LIST,
		0,
		0,
		0x000000c8,
		0,
		menuhandlerGhostMine,
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

struct menudialogdef g_GhostMineMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"My Ghosts",
	g_GhostMineMenuItems,
	menudialogGhostMine,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_GhostOptionsMenuItems[] = {
	{
		// Said here rather than left to be discovered mid run. A player who
		// presses jump in a trial and does not leave the ground has found a
		// bug unless something told them otherwise.
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Trials record, with Jump and Combat Roll off.\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Time Trial",
		0,
		menuhandlerGhostMode,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Customize Character...\n",
		0,
		(void *)&g_GhostCharacterMenuDialog,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghosts Raced At Once",
		0,
		menuhandlerGhostRacers,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Field",
		0,
		menuhandlerGhostPick,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Choose Ghosts...\n",
		0,
		(void *)&g_GhostChooserMenuDialog,
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
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Visibility",
		0,
		menuhandlerGhostAlpha,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Split Times",
		0,
		menuhandlerGhostSplits,
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

struct menudialogdef g_GhostOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Options",
	g_GhostOptionsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};


/**
 * The leaderboard account.
 *
 * A username and a PIN, no password, and the server keeps usernames unique -
 * so signing in on a second machine is typing the same two things rather than
 * moving a file about. Both are kept in pd.ini; see the note where they are
 * registered for why a PIN is written there as typed.
 */
static char g_GhostAccountMsg[128];

static char *menutextGhostAccountStatus(struct menuitem *item)
{
	s32 state = ghostnetGetState();

	if (!ghostnetIsAvailable()) {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Network support is not built into this copy.\n");
	} else if (state == GHOSTNET_BUSY || state == GHOSTNET_OK || state == GHOSTNET_ERROR) {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg), "%s\n", ghostnetGetMessage());
	} else if (ghostnetHasAccount()) {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Signed in as %s\n", g_GhostNetUser);
	} else {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Pick a name and a PIN, then Create Account.\n");
	}

	return g_GhostAccountMsg;
}

/**
 * The name, edited on the game's own on screen keyboard.
 *
 * handlerdata.keyboard.string is a pointer to the menu's editing buffer, not
 * the buffer itself, so the length has to be named: sizeof() on it is the size
 * of a pointer and silently cuts the name to seven characters. The buffer it
 * points at is char[MPSETUP_MAXNAME + 1], which is shorter than a name the
 * server would accept, so this is also what keeps a long name out of the
 * fields that follow it in the menu data.
 */
static MenuItemHandlerResult menuhandlerGhostUser(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		snprintf(data->keyboard.string, MPSETUP_MAXNAME + 1, "%s", g_GhostNetUser);
		break;
	case MENUOP_SETTEXT:
		snprintf(g_GhostNetUser, sizeof(g_GhostNetUser), "%s", data->keyboard.string);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostPin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		snprintf(data->keyboard.string, MPSETUP_MAXNAME + 1, "%s", g_GhostNetPin);
		break;
	case MENUOP_SETTEXT:
		snprintf(g_GhostNetPin, sizeof(g_GhostNetPin), "%s", data->keyboard.string);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostCreate(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !ghostnetIsAvailable() || !ghostnetHasAccount()
			|| ghostnetGetState() == GHOSTNET_BUSY;
	case MENUOP_SET:
		ghostnetRegister();
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostSignIn(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !ghostnetIsAvailable() || !ghostnetHasAccount()
			|| ghostnetGetState() == GHOSTNET_BUSY;
	case MENUOP_SET:
		ghostnetLogin();
		break;
	}

	return 0;
}

static MenuDialogHandlerResult menudialogGhostAccount(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	// Whatever the last request said belongs to the last time the page was
	// open. Clearing it means the status line reads as the state of the
	// account rather than as the outcome of something the player has
	// forgotten doing.
	if (operation == MENUOP_OPEN) {
		ghostnetClearState();
	}

	return 0;
}

static char *menutextGhostName(struct menuitem *item)
{
	snprintf(g_GhostRowText, sizeof(g_GhostRowText), "Name: %s\n",
			g_GhostNetUser[0] ? g_GhostNetUser : "(not set)");

	return g_GhostRowText;
}

static char *menutextGhostPinRow(struct menuitem *item)
{
	// Shown as dots. It is a four digit PIN on a game leaderboard rather than
	// a secret worth much, but a page you might be streaming should not put it
	// on screen.
	char dots[GHOSTNET_MAXPIN + 1];
	u32 len = strlen(g_GhostNetPin);
	u32 i;

	if (len > GHOSTNET_MAXPIN) {
		len = GHOSTNET_MAXPIN;
	}

	for (i = 0; i < len; i++) {
		dots[i] = '*';
	}

	dots[len] = '\0';

	snprintf(g_GhostRowText, sizeof(g_GhostRowText), "PIN: %s\n",
			len ? dots : "(not set)");

	return g_GhostRowText;
}

/**
 * The name and the PIN each get a page of their own.
 *
 * Both on one page is what this started as, and the game's on screen keyboard
 * is tall enough that two of them pushed Create Account, Sign In and Back off
 * the bottom of the dialog with no way to reach them. One keyboard per page is
 * also how the file manager does renaming.
 */
struct menuitem g_GhostNameMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)"3-15 characters: letters, digits, _ . -\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_KEYBOARD,
		0,
		0,
		0,
		0,
		menuhandlerGhostUser,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_GhostNameMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Account Name",
	g_GhostNameMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_GhostPinMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)"4-8 digits\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_KEYBOARD,
		0,
		0,
		0,
		0,
		menuhandlerGhostPin,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_GhostPinMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Account PIN",
	g_GhostPinMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * Ghost Account: who you are on the boards, chosen the way an agent is.
 *
 * The game opens by asking which agent you are and remembers the ones on the
 * machine, and this is the same question one level up - an agent is a save
 * file, an account is a name on a leaderboard, and a couch with three people
 * on it needs both. Accounts are remembered with their PINs in pd.ini so that
 * coming back is choosing a row rather than typing a PIN again.
 *
 * Nothing here gates play. A player with no account records, races and keeps
 * ghosts exactly as before; what they cannot do is publish them, and the page
 * says so rather than standing in the way.
 */
extern struct menudialogdef g_GhostAccountMenuDialog;

static char g_GhostAccountsMsg[128];

static char *menutextGhostAccountsStatus(struct menuitem *item)
{
	s32 state = ghostnetGetState();

	if (!ghostnetIsAvailable()) {
		snprintf(g_GhostAccountsMsg, sizeof(g_GhostAccountsMsg),
				"Network support is not built into this copy.\n");
	} else if (state == GHOSTNET_BUSY || state == GHOSTNET_OK || state == GHOSTNET_ERROR) {
		snprintf(g_GhostAccountsMsg, sizeof(g_GhostAccountsMsg), "%s\n", ghostnetGetMessage());
	} else if (ghostnetHasAccount()) {
		snprintf(g_GhostAccountsMsg, sizeof(g_GhostAccountsMsg),
				"Racing as %s - A on a name switches.\n", ghostnetGetAccountName());
	} else {
		snprintf(g_GhostAccountsMsg, sizeof(g_GhostAccountsMsg),
				"No account - ghosts stay on this machine.\n");
	}

	return g_GhostAccountsMsg;
}

static MenuItemHandlerResult menuhandlerGhostAccountList(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = ghostnetGetNumAccounts();
		break;
	case MENUOP_GETOPTIONTEXT:
		// The active account is marked rather than moved to the top, because a
		// list that reorders itself under the cursor is a list you cannot
		// point at.
		snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%c %.15s",
				data->list.value == 0 ? '*' : ' ',
				ghostnetGetAccountAt(data->list.value));

		return (intptr_t)g_GhostRowText;
	case MENUOP_SET:
		ghostnetSelectAccount(data->list.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0xfffff;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostNewAccount(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !ghostnetIsAvailable() || ghostnetGetState() == GHOSTNET_BUSY;
	case MENUOP_SET:
		// The account in use is put aside first, so that making a second one
		// does not type over the first and sign the player out of something
		// they never left.
		ghostnetBeginNewAccount();
		menuPushDialog(&g_GhostAccountMenuDialog);
		break;
	}

	return 0;
}

static MenuDialogHandlerResult menudialogGhostAccounts(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		ghostnetClearState();
	}

	return 0;
}

// A list takes the height that is left over, so anything placed after one is
// pushed off the bottom of the dialog and simply does not appear - which is
// where New Account and Sign In first went. Everything that is not the list
// goes above it, and the list is followed by Back and nothing else, which is
// the shape the chooser pages in this file already use.
struct menuitem g_GhostAccountsMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostAccountsStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"New Account...\n",
		0,
		menuhandlerGhostNewAccount,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Name And PIN...\n",
		0,
		(void *)&g_GhostAccountMenuDialog,
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
		MENUITEMTYPE_LIST,
		0,
		0,
		0x00000078,
		0,
		menuhandlerGhostAccountList,
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

struct menudialogdef g_GhostAccountsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Account",
	g_GhostAccountsMenuItems,
	menudialogGhostAccounts,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_GhostAccountMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostAccountStatus,
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
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG,
		(uintptr_t)&menutextGhostName,
		0,
		(void *)&g_GhostNameMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG,
		(uintptr_t)&menutextGhostPinRow,
		0,
		(void *)&g_GhostPinMenuDialog,
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
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Create Account\n",
		0,
		menuhandlerGhostCreate,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Sign In\n",
		0,
		menuhandlerGhostSignIn,
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

struct menudialogdef g_GhostAccountMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Account",
	g_GhostAccountMenuItems,
	menudialogGhostAccount,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * Ghost Share: publish what you have set.
 *
 * One button rather than a file picker. The server keeps one run per player
 * per mission per difficulty and refuses anything slower than what it already
 * has, so sending everything is both cheap and idempotent - and "publish my
 * times" is the only thing anybody actually wants from this page.
 */
static MenuItemHandlerResult menuhandlerGhostUpload(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !ghostnetIsAvailable() || !ghostnetHasAccount()
			|| ghostnetGetState() == GHOSTNET_BUSY;
	case MENUOP_SET:
		ghostnetUploadMine();
		break;
	}

	return 0;
}

static MenuDialogHandlerResult menudialogGhostShare(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		ghostnetClearState();
	}

	return 0;
}

struct menuitem g_GhostShareMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostAccountStatus,
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
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Upload My Ghosts\n",
		0,
		menuhandlerGhostUpload,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Account...\n",
		0,
		(void *)&g_GhostAccountsMenuDialog,
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

struct menudialogdef g_GhostShareMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Share",
	g_GhostShareMenuItems,
	menudialogGhostShare,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * Leaderboards: the top hundred for one mission and difficulty.
 *
 * Which mission is a dropdown over the solo stage table rather than a guess
 * from context, because this page is reached from the main menu where there is
 * no mission in progress to guess from.
 *
 * Choosing in either dropdown fetches that board. These open a list and commit
 * once rather than stepping through values in place, so choosing is a single
 * decision and costs a single request - the fear that scrolling a dropdown
 * would fire twenty of them was about a control this menu does not use.
 *
 * The rows are cleared first. A fetch is not instant and the old board under
 * the new mission's name is a leaderboard that is lying for a moment.
 *
 * Load Times stays as the way to ask again when a fetch failed or the board has
 * moved on, which is the only thing left that needs asking for.
 */
static s32 g_GhostBoardStageIndex = 0;
static s32 g_GhostBoardDiff = 0;

static MenuItemHandlerResult menuhandlerGhostBoardStage(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = NUM_SOLOSTAGES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)langGet(g_SoloStages[data->dropdown.value].name3);
	case MENUOP_SET:
		g_GhostBoardStageIndex = data->dropdown.value;
		ghostnetClearBoard();
		ghostnetFetchBoard(g_SoloStages[g_GhostBoardStageIndex].stagenum, g_GhostBoardDiff);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_GhostBoardStageIndex;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostBoardDiff(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Agent", "Special Agent", "Perfect Agent" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_GhostBoardDiff = data->dropdown.value;
		ghostnetClearBoard();
		ghostnetFetchBoard(g_SoloStages[g_GhostBoardStageIndex].stagenum, g_GhostBoardDiff);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_GhostBoardDiff;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostBoard(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct ghostboardentry *entry;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = ghostnetGetBoardCount();
		break;
	case MENUOP_GETOPTIONTEXT:
		entry = ghostnetGetBoardEntry(data->list.value);

		if (entry == NULL) {
			return (intptr_t)"";
		}

		// A row that cannot show it was set under trial rules is marked rather
		// than hidden. The server refuses to store one, so this should never
		// appear - and if it ever does, the player should be able to see why a
		// time beside theirs is not a time beside theirs.
		snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%2d.%c%d:%02d.%02d  %.14s",
				(s32)data->list.value + 1,
				entry->trialrules ? ' ' : '?',
				entry->time60 / 3600, (entry->time60 / 60) % 60,
				(entry->time60 % 60) * 100 / 60,
				entry->user);

		return (intptr_t)g_GhostRowText;
	case MENUOP_SET:
		// Selecting a time downloads the run that set it.
		ghostnetDownload(data->list.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0xfffff;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostBoardLoad(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !ghostnetIsAvailable() || ghostnetGetState() == GHOSTNET_BUSY;
	case MENUOP_SET:
		ghostnetFetchBoard(g_SoloStages[g_GhostBoardStageIndex].stagenum, g_GhostBoardDiff);
		break;
	}

	return 0;
}

static char *menutextGhostBoardStatus(struct menuitem *item)
{
	s32 state = ghostnetGetState();

	if (!ghostnetIsAvailable()) {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Network support is not built into this copy.\n");
	} else if (state == GHOSTNET_IDLE && ghostnetGetBoardCount() < 1) {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"No times on this board yet - Load Times asks again.\n");
	} else if (state == GHOSTNET_IDLE) {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"A on a time downloads that ghost.\n");
	} else {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg), "%s\n", ghostnetGetMessage());
	}

	return g_GhostAccountMsg;
}

static MenuDialogHandlerResult menudialogGhostBoard(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	// Opening the board fetches it. That is one request because a player asked
	// to look at a leaderboard, which is not the thing "no continuous
	// connections" was about - nothing here polls, nothing is held open, and
	// every other page in Ghost Trials works with the network unplugged.
	//
	// It briefly did not fetch here, on the theory that every connection
	// should be asked for out loud. What that produced was a leaderboard that
	// was empty until you found the button, which is a worse answer to a
	// player who has just uploaded a time and wants to see it.
	if (operation == MENUOP_OPEN) {
		ghostnetClearState();

		if (ghostnetIsAvailable()) {
			ghostnetFetchBoard(g_SoloStages[g_GhostBoardStageIndex].stagenum, g_GhostBoardDiff);
		}
	}

	return 0;
}

struct menuitem g_GhostBoardMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mission",
		0,
		menuhandlerGhostBoardStage,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Difficulty",
		0,
		menuhandlerGhostBoardDiff,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Load Times\n",
		0,
		menuhandlerGhostBoardLoad,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostBoardStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LIST,
		0,
		0,
		0x000000c8,
		0,
		menuhandlerGhostBoard,
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

struct menudialogdef g_GhostBoardMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Leaderboards",
	g_GhostBoardMenuItems,
	menudialogGhostBoard,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * The line at the top of Ghost Trials, saying who the runs will belong to.
 *
 * Worth the row because everything below it is stamped with that name: a run
 * recorded now carries the account that recorded it, and a player who thought
 * they were signed in as somebody else finds out at upload time otherwise.
 */
static char g_GhostTrialsMsg[96];

static char *menutextGhostTrialsStatus(struct menuitem *item)
{
	if (ghostnetHasAccount()) {
		snprintf(g_GhostTrialsMsg, sizeof(g_GhostTrialsMsg),
				"Racing as %s\n", ghostnetGetAccountName());
	} else {
		snprintf(g_GhostTrialsMsg, sizeof(g_GhostTrialsMsg),
				"No account - runs stay on this machine\n");
	}

	return g_GhostTrialsMsg;
}

/**
 * Offer the account chooser the first time somebody opens Ghost Trials.
 *
 * Once per run of the game, and only when there is no account at all, because
 * the point is to ask a new player the question rather than to keep asking it.
 * Backing out of it leaves everything working: recording, racing and the
 * ghosts directory need no account and never did.
 */
static MenuDialogHandlerResult menudialogGhostTrials(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	static bool asked = false;

	if (operation == MENUOP_OPEN && !asked) {
		asked = true;

		if (ghostnetIsAvailable() && !ghostnetHasAccount()) {
			menuPushDialog(&g_GhostAccountsMenuDialog);
		}
	}

	return 0;
}

struct menuitem g_GhostTrialsMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostTrialsStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Mission\n",
		0,
		menuhandlerGhostMission,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Options\n",
		0,
		(void *)&g_GhostOptionsMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"My Ghosts\n",
		0,
		(void *)&g_GhostMineMenuDialog,
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
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Account\n",
		0,
		(void *)&g_GhostAccountsMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Share\n",
		0,
		(void *)&g_GhostShareMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Leaderboards\n",
		0,
		(void *)&g_GhostBoardMenuDialog,
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

struct menudialogdef g_GhostTrialsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Trials",
	g_GhostTrialsMenuItems,
	menudialogGhostTrials,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
