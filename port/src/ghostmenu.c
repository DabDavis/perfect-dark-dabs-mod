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
#include "lib/vi.h"
#include "ghostnet.h"
#include "ghostrecovery.h"
#include "game/lang.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/setup.h"
#include "game/menugfx.h"
#include "game/game_1531a0.h"
#include "game/camdraw.h"
#include "video.h"
#include "system.h"

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

// How far in from the right edge of a dialog the character preview stands, in
// menu units. Far enough that a whole body is inside the window it is drawn in.
#define MODGHOST_MODELINSET 40

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
	s32 head = g_ModGhostHead > MODGHOST_BODY_DEFAULT ? g_ModGhostHead - 1 : modGhostBodyDefaultHead(body);

	switch (operation) {
	case MENUOP_SET:
		g_ModGhostBody = data->carousel.value + 1;

		// A body brings its own head unless one was picked on purpose, which
		// is what the arena page does and the reason a head can be left alone.
		if (g_ModGhostHead <= MODGHOST_BODY_DEFAULT) {
			g_ModGhostHead = modGhostBodyDefaultHead(data->carousel.value) + 1;
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
 * The live state of one of this dialog's items, by identity.
 *
 * A list keeps which row the cursor is on in its own menuitemdata, which the
 * menu owns rather than the handler: the handler is asked for a row's text and
 * told which row, never which row is focused. So a page that wants to react to
 * the cursor moving has to go and look, and this walks the dialog's rows the
 * way menu.c walks them to render them.
 *
 * NULL when the item has no data block yet, which is the frame the dialog
 * opens on.
 */
static union menuitemdata *menuGhostItemData(struct menudialog *dialog, struct menuitem *item)
{
	struct menu *menu = &g_Menus[g_MpPlayerNum];
	s32 colindex;
	s32 j;

	for (colindex = dialog->colstart; colindex < dialog->colstart + dialog->numcols; colindex++) {
		for (j = 0; j < menu->cols[colindex].numrows; j++) {
			s32 rowindex = menu->cols[colindex].rowstart + j;

			if (&dialog->definition->items[menu->rows[rowindex].itemindex] == item
					&& menu->rows[rowindex].blockindex != -1) {
				return (union menuitemdata *)&menu->blocks[menu->rows[rowindex].blockindex];
			}
		}
	}

	return NULL;
}

/**
 * Show the run under the cursor as the character who set it.
 *
 * A row of a list is a time and a name, and a field of ghosts is a field of
 * people wearing different things - the one thing a row cannot say. The header
 * already carries the character, so the page that lists runs can stand the
 * model up beside them and the choice becomes "which of these am I racing"
 * rather than a column of names.
 *
 * The model is the menu's own, the same one the Combat Simulator's character
 * page turns: pointing it at a different body and head is the whole of the
 * work, and the dialog renders it for having MENUDIALOGFLAG_0002 set. Params
 * are only written when they change, because assigning newparams is what makes
 * the model reload.
 *
 * A run that names nobody - anything recorded before the picker existed - gets
 * Joanna, which is who it was.
 */
// The character the preview was last placed for. Cleared when a page opens, so
// that coming back to one sets the model up again rather than trusting what the
// last page left behind.
static u32 g_GhostModelParams = 0;

/**
 * Stand the model up as one character and keep it turning.
 *
 * Split from the pages that call it because two different kinds of list want
 * it: My Ghosts and Choose Ghosts read a character out of a file on disk, and
 * Leaderboards reads one out of a row the server sent. Where it comes from is
 * their business; what it looks like is this.
 */
static void menuGhostShowCharacter(struct menudialog *dialog, s32 mpbody, s32 mphead)
{
	struct menumodel *model = &g_Menus[g_MpPlayerNum].menumodel;
	u32 params;

	params = MENUMODELPARAMS_SET_MP_HEADBODY(mphead, mpbody);

	// Placing the model is a one off. menuConfigureModel() starts a transition
	// to where it is being put, so calling it every frame restarts that
	// transition every frame and the model never arrives anywhere. The arena's
	// character page does this once when a body is chosen; here the equivalent
	// moment is the cursor landing on a different run.
	//
	// What is remembered is the character this was last placed for, and not
	// model->curparams: curparams is only written once the load has finished,
	// so testing it re-entered this every frame while the load was still going
	// and set loaddelay again, which restarted the load that was about to
	// complete. The model stayed unloaded for exactly as long as the page was
	// open.
	if (g_GhostModelParams != params) {
		g_GhostModelParams = params;

		menuConfigureModel(model, 0, 0, 0, 0, 0, 0, 1, MENUMODELFLAG_HASSCALE);

		// posx and posy are offsets from the centre of the view in menu units:
		//   screenpos[0] = posx * g_ScaleX + viewleft + viewwidth * 0.5
		// - which is an offset from the middle of the view, in the same menu
		// units the dialog is measured in, times g_ScaleX. So it is worked out
		// from the dialog rather than written down: a constant that stands the
		// model beside the rows at one resolution walks it off the side of the
		// screen at another, which is what a fixed 168 did.
		//
		// MODELINSET is from the right edge of the dialog to the middle of the
		// model, so the whole of it is inside the window - the window is what
		// clips it. MENUDIALOGFLAG_MODELOVERLAY would lift it out and draw it
		// over the top instead, but menuRenderModel() takes both its scissor
		// and its viewport from the same four numbers, so widening one widens
		// the other and the model changes size with it. Splitting those is
		// what that idea needs and it is not done.
		model->curposx = model->newposx =
			(f32)(dialog->x + dialog->width - MODGHOST_MODELINSET)
			- (viGetViewLeft() + viGetViewWidth() * 0.5f) / (f32)g_ScaleX;

		// posy is pixels rather than menu units - menu.c adds it to the middle
		// of the view without scaling it - so it stays a small nudge.
		model->curposy = model->newposy = -4.1f;
		// Full size at once, rather than the arena's zoom in.
		//
		// scale multiplies zoom, and zoom is the size that matters: menu.c
		// works out zoomy = zoom / (half the model's bounding box height), so
		// zoom is an on screen height in menu units whatever the character's
		// own proportions are, and scale 1 is the size that height describes.
		// Starting at 0.002 and creeping towards 1 a fraction a frame is the
		// arena page's zoom in, which is fine on a page you flick through and
		// wrong on one you sit on: the model kept growing until it was larger
		// than the window and the window is what clips it, which showed as an
		// arm crossing the rows now and then.
		model->curscale = model->newscale = 0.58f;
		model->curroty = model->newroty = -0.2f;
		model->rottimer60 = TICKS(60);
		// Parked at a phase and left there. menuGetLinearOscPauseFrac() holds
		// frac at 1 for timer values between TICKS(120) and TICKS(240), and
		// frac 1 is the far end of the swell and the only phase with no
		// vertical shove - so the model sits still, level with the rows, at a
		// size scale below then fixes.
		model->zoomtimer60 = TICKS(180);
		model->loaddelay = 8;
		model->removingpiece = false;
	}

	// And this is the every frame half, which is what the arena page does from
	// its own tick: name the character, keep the idle animation, and turn the
	// model slowly once it has settled.
	model->newparams = params;
	model->newanimnum = ANIM_01FC;
	model->partvisibility = NULL;

	model->zoomtimer60 += g_Vars.diffframe60;

	if (model->zoomtimer60 > TICKS(480)) {
		model->zoomtimer60 -= TICKS(480);
	}
	// zoom is deliberately not assigned. menuRenderModel() writes it itself
	// from the phase of zoomtimer60:
	//
	//     zoom      = 100 + (1 - frac) * 270
	//     zoompos.y = -(height / 7.6) * (1 - frac * frac)
	//
	// which is the arena page's slow swell from far to near and back, and the
	// vertical drift that goes with it. Anything set here is overwritten on
	// the next frame, so the way to change the size is scale, and the way to
	// stop the swell is to stop advancing the timer below.

	if (model->rottimer60 > 0) {
		model->rottimer60 -= g_Vars.diffframe60;
	} else {
		model->curroty += 0.01f * g_Vars.diffframe60f;
		model->newroty = model->curroty;
	}
}

/**
 * The character of the run under the cursor on a page listing local ghosts.
 */
static void menuGhostTickCatalogueModel(struct menudialog *dialog, struct menuitem *listitem)
{
	union menuitemdata *data = menuGhostItemData(dialog, listitem);
	struct modghostentry *entry;
	s32 mpbody = MPBODY_DARK_AF1;
	s32 mphead = MPHEAD_DARK_COMBAT;

	if (data == NULL) {
		return;
	}

	entry = modGhostGetCatalogueEntry(data->list.index);

	if (entry != NULL) {
		modGhostEntryCharacter(entry, &mpbody, &mphead);
	}

	menuGhostShowCharacter(dialog, mpbody, mphead);
}

/**
 * The same, for a row of the leaderboard.
 *
 * The board carries the character the same way a file does, so a time can be
 * shown as whoever set it before it has been downloaded - which is the point,
 * since deciding whether to download is what the page is for. A row from a
 * server too old to send one, or a run recorded before the picker existed,
 * leaves the two at zero and gets the default.
 */
static void menuGhostTickBoardModel(struct menudialog *dialog, struct menuitem *listitem)
{
	union menuitemdata *data = menuGhostItemData(dialog, listitem);
	struct ghostboardentry *entry;
	s32 mpbody = MPBODY_DARK_AF1;
	s32 mphead = MPHEAD_DARK_COMBAT;

	if (data == NULL) {
		return;
	}

	entry = ghostnetGetBoardEntry(data->list.index);

	if (entry != NULL && entry->mpbody > MODGHOST_BODY_DEFAULT
			&& (s32)entry->mpbody - 1 < (s32)mpGetNumBodies()) {
		mpbody = entry->mpbody - 1;
		mphead = (entry->mphead > MODGHOST_BODY_DEFAULT
				&& (s32)entry->mphead - 1 < mpGetNumHeads2())
			? entry->mphead - 1
			: modGhostBodyDefaultHead(mpbody);
	}

	menuGhostShowCharacter(dialog, mpbody, mphead);
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
	// Its own buffer rather than the row one, for the reason spelled out over
	// menutextGhostMineCount(): the label and the list rows on this page are
	// resolved by the same pass, and a label sharing storage with the rows
	// reads as whichever row was drawn last.
	static char text[96];

	snprintf(text, sizeof(text), "%d of %d chosen - A to toggle\n",
			modGhostGetNumChosen(), MODGHOST_MAXRACERS);

	return text;
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
		g_GhostModelParams = 0;
		menuGhostChooserRescan();
	}

	// items[3] is the list; see the table below.
	if (operation == MENUOP_TICK
			&& g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		menuGhostTickCatalogueModel(g_Menus[g_MpPlayerNum].curdialog, &dialogdef->items[3]);
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
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_0002,
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
		// screen is not something a downloaded ghost gets to do. The widths
		// are also what leaves the right of the dialog free for the character
		// model - see menuGhostTickModel().
		snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%c%c%.13s %-2s %d:%02d.%02d %.9s",
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
		g_GhostModelParams = 0;
		modGhostSetCatalogueFilter(-1, -1);
		modGhostScanCatalogue();
	}

	// items[1] is the list; see the table below.
	if (operation == MENUOP_TICK
			&& g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		menuGhostTickCatalogueModel(g_Menus[g_MpPlayerNum].curdialog, &dialogdef->items[1]);
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
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_0002,
	NULL,
};

/**
 * The rules are not listed here any more.
 *
 * They were a line at the top of this page, which is a page a player opens to
 * change how trials look rather than to find out what one is. They are in a
 * window of their own on the Ghost Trials page now - see
 * menuGhostRenderRules() - which is the door everything here is behind and the
 * last thing read before a run starts.
 */
struct menuitem g_GhostOptionsMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Time Trial",
		0,
		menuhandlerGhostMode,
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
	} else if (ghostnetGetAccountRecovery() == GHOSTNET_RECOVERY_MISSING) {
		// Signed in, and the server has said this account cannot be reset.
		// Only a reply to a correct PIN knows that, so this line is the only
		// warning its owner will ever get.
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"No Security Questions - you cannot reset a lost PIN.\n");
	} else if (ghostnetGetAccountRecovery() == GHOSTNET_RECOVERY_PARTIAL) {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Signed in as %s - set 3 Security Questions.\n", g_GhostNetUser);
	} else if (ghostnetIsSignedIn()) {
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Signed in as %s\n", g_GhostNetUser);
	} else if (ghostnetAccountIsValid() && !ghostnetRecoveryIsSet()) {
		// Create Account is refused without one, and a greyed out button with
		// no reason beside it is the thing this page has already been wrong
		// about once. Signing in is not gated: somebody who set their question
		// on another machine has nothing to pick here.
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Set 3 Security Questions, then Create Account.\n");
	} else if (ghostnetAccountIsValid()) {
		// Well formed, and that is all this end knows. Whether the name is
		// registered, and whether the PIN is its PIN, are questions only the
		// server can answer - so the page names the two buttons that ask it
		// rather than saying the account works.
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Create Account if %s is new, or Sign In.\n", g_GhostNetUser);
	} else if (ghostnetHasAccount()) {
		// Both are filled in and one of them is not something the server will
		// take. Saying which beats letting the player press a greyed out
		// button and wonder, or press a live one and be refused by a machine.
		snprintf(g_GhostAccountMsg, sizeof(g_GhostAccountMsg),
				"Name needs 3-15 of letters, digits, _ . - and PIN 4-8 digits.\n");
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
		// A new account picks its security question here or never: the only
		// other way to set one is to sign in with the PIN, which is exactly
		// what somebody who has lost it cannot do.
		return !ghostnetIsAvailable() || !ghostnetAccountIsValid()
			|| !ghostnetRecoveryIsSet()
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
		return !ghostnetIsAvailable() || !ghostnetAccountIsValid()
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
	// Its own buffer: this and the PIN row below are two rows of one page, and
	// a page is resolved in one pass. See menutextGhostMineCount().
	static char text[64];

	snprintf(text, sizeof(text), "Name: %s\n",
			g_GhostNetUser[0] ? g_GhostNetUser : "(not set)");

	return text;
}

static char *menutextGhostPinRow(struct menuitem *item)
{
	// Shown as dots. It is a four digit PIN on a game leaderboard rather than
	// a secret worth much, but a page you might be streaming should not put it
	// on screen.
	static char text[64];
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

	snprintf(text, sizeof(text), "PIN: %s\n", len ? dots : "(not set)");

	return text;
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
 * The security questions: three times, one category out of ten and one answer
 * out of its list.
 *
 * Two dropdowns a pair and no keyboard. A typed answer is a second thing to
 * spell the same way a year later, on a game's on screen keyboard, by somebody
 * who has already forgotten one thing - and the list is also what lets the
 * server hold a hash of a known id rather than of whatever was typed.
 *
 * The indices live in ghostnet.c beside the name and the PIN, and none is
 * written to pd.ini. See the note there: an answer kept next to the PIN it
 * recovers is a decoration on the PIN, not a second thing to know.
 *
 * item->param says which of the three a row is. The same two handlers serve
 * the six rows of the question page and the six of the reset page.
 */
static MenuItemHandlerResult menuhandlerGhostQuestion(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const struct ghostrecoverycategory *cat;
	s32 i = item->param;

	if (i < 0 || i >= GHOSTNET_NUMQUESTIONS) {
		return 0;
	}

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		// The ten, and "(not set)" at the top. A dropdown has no way to say
		// "nothing chosen", and index zero being a real category would make
		// the first favourite in the first list the answer for everybody who
		// opened the page and left.
		data->dropdown.value = GHOSTRECOVERY_NUMCATEGORIES + 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value <= 0) {
			return (intptr_t)"(not set)";
		}

		cat = ghostRecoveryGetCategory(data->dropdown.value - 1);

		return (intptr_t)(cat ? cat->name : "");
	case MENUOP_SET:
		// An answer belongs to the category it was picked from, so changing
		// the category drops it rather than keeping a row number that now
		// names somebody else's favourite.
		if (data->dropdown.value - 1 != g_GhostNetQuestion[i]) {
			g_GhostNetAnswer[i] = -1;
		}

		g_GhostNetQuestion[i] = data->dropdown.value - 1;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_GhostNetQuestion[i] + 1;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostAnswer(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 i = item->param;

	if (i < 0 || i >= GHOSTNET_NUMQUESTIONS) {
		return 0;
	}

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_GhostNetQuestion[i] < 0;
	case MENUOP_GETOPTIONCOUNT:
		// Zero answers plus "(not set)" while no category is chosen, so the
		// row is a dropdown with nothing in it rather than a list of the
		// wrong category's favourites.
		data->dropdown.value = ghostRecoveryGetNumAnswers(g_GhostNetQuestion[i]) + 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value <= 0) {
			return (intptr_t)"(not set)";
		}

		return (intptr_t)ghostRecoveryGetAnswerName(g_GhostNetQuestion[i], data->dropdown.value - 1);
	case MENUOP_SET:
		g_GhostNetAnswer[i] = data->dropdown.value - 1;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_GhostNetAnswer[i] + 1;
		break;
	}

	return 0;
}

/**
 * The six dropdown rows, written once.
 *
 * Both pages that ask the questions ask all three, and a row is the same row
 * on either: the pair's number in item->param, the handler above.
 */
#define GHOST_QUESTION_ROWS \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Question 1", 0, menuhandlerGhostQuestion }, \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Answer 1", 0, menuhandlerGhostAnswer }, \
	{ MENUITEMTYPE_DROPDOWN, 1, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Question 2", 0, menuhandlerGhostQuestion }, \
	{ MENUITEMTYPE_DROPDOWN, 1, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Answer 2", 0, menuhandlerGhostAnswer }, \
	{ MENUITEMTYPE_DROPDOWN, 2, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Question 3", 0, menuhandlerGhostQuestion }, \
	{ MENUITEMTYPE_DROPDOWN, 2, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Answer 3", 0, menuhandlerGhostAnswer }

/**
 * Send the question to an account that already exists.
 *
 * A new account carries it in with the registration and never comes here. This
 * is for the accounts made before there was a question to ask, and for the
 * player who wants a different one - both of which need the PIN, which is the
 * whole of the authority the server asks for. Somebody who cannot sign in
 * cannot change the answer that would let them back in.
 */
static MenuItemHandlerResult menuhandlerGhostSaveRecovery(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !ghostnetIsAvailable() || !ghostnetAccountIsValid()
			|| !ghostnetRecoveryIsSet()
			|| ghostnetGetState() == GHOSTNET_BUSY;
	case MENUOP_SET:
		ghostnetSetRecovery();
		break;
	}

	return 0;
}

static char g_GhostQuestionMsg[128];

static char *menutextGhostQuestionStatus(struct menuitem *item)
{
	s32 state = ghostnetGetState();

	if (state == GHOSTNET_BUSY || state == GHOSTNET_OK || state == GHOSTNET_ERROR) {
		snprintf(g_GhostQuestionMsg, sizeof(g_GhostQuestionMsg), "%s\n", ghostnetGetMessage());
	} else if (ghostnetGetAccountRecovery() == GHOSTNET_RECOVERY_MISSING
			&& !ghostnetRecoveryIsSet()) {
		// The state a player is pushed into this page in, having pressed
		// nothing. The first line has to say why they are looking at it.
		snprintf(g_GhostQuestionMsg, sizeof(g_GhostQuestionMsg),
				"This account cannot reset a lost PIN yet.\n");
	} else if (ghostnetGetAccountRecovery() == GHOSTNET_RECOVERY_PARTIAL
			&& !ghostnetRecoveryIsSet()) {
		// The other state they are pushed in with: an account made when one
		// question was all there was.
		snprintf(g_GhostQuestionMsg, sizeof(g_GhostQuestionMsg),
				"Your account has fewer than 3 questions - pick 3.\n");
	} else if (ghostnetRecoveryIsRepeated()) {
		snprintf(g_GhostQuestionMsg, sizeof(g_GhostQuestionMsg),
				"Each question must be different.\n");
	} else if (!ghostnetRecoveryIsSet()) {
		snprintf(g_GhostQuestionMsg, sizeof(g_GhostQuestionMsg),
				"Pick 3 pairs you will still know in a year.\n");
	} else if (ghostnetGetAccountRecovery() == GHOSTNET_RECOVERY_MISSING
			|| ghostnetGetAccountRecovery() == GHOSTNET_RECOVERY_PARTIAL) {
		// Picked here, but the account still has nothing, or less, on it.
		snprintf(g_GhostQuestionMsg, sizeof(g_GhostQuestionMsg),
				"Not on the account yet - Save To Account.\n");
	} else {
		snprintf(g_GhostQuestionMsg, sizeof(g_GhostQuestionMsg),
				"Save To Account to replace the existing ones.\n");
	}

	return g_GhostQuestionMsg;
}

static MenuDialogHandlerResult menudialogGhostQuestion(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		ghostnetClearState();
	}

	return 0;
}

struct menuitem g_GhostQuestionMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostQuestionStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)"Used only to reset your PIN if you forget it.\n",
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
	GHOST_QUESTION_ROWS,
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
		(uintptr_t)"Save To Account\n",
		0,
		menuhandlerGhostSaveRecovery,
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

struct menudialogdef g_GhostQuestionMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Security Questions",
	g_GhostQuestionMenuItems,
	menudialogGhostQuestion,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * Reset PIN: the one way in that does not need the PIN.
 *
 * The PIN row on this page is the PIN the account is to have, not the one it
 * has - it is the same box the rest of the pages type into, because a reset is
 * "here are my answers, and here is the PIN I want now" and a second PIN box
 * beside the first is a second thing to mistype. The server takes the answers
 * as the authority and writes the PIN that came with it.
 *
 * One pair is enough to press the button, not three: an account made when one
 * question was all there was is reset by its one, and the server reads as many
 * of the pairs sent as the account holds. Fewer than it holds is refused as a
 * wrong answer, which the line above the rows warns about.
 *
 * Five wrong answers at one account in a day is all the server will take, and
 * ten reset attempts an hour from one machine, each after a wait. Ten
 * categories times fifty answers is not a password; the limiter is what makes
 * it hold, and three different categories is what makes it more than a hint.
 */
static MenuItemHandlerResult menuhandlerGhostResetPin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !ghostnetIsAvailable() || !ghostnetAccountIsValid()
			|| ghostnetRecoveryCount() < 1
			|| ghostnetGetState() == GHOSTNET_BUSY;
	case MENUOP_SET:
		ghostnetResetPin();
		break;
	}

	return 0;
}

static char g_GhostResetMsg[128];

static char *menutextGhostResetStatus(struct menuitem *item)
{
	s32 state = ghostnetGetState();

	if (!ghostnetIsAvailable()) {
		snprintf(g_GhostResetMsg, sizeof(g_GhostResetMsg),
				"Network support is not built into this copy.\n");
	} else if (state == GHOSTNET_BUSY || state == GHOSTNET_OK || state == GHOSTNET_ERROR) {
		snprintf(g_GhostResetMsg, sizeof(g_GhostResetMsg), "%s\n", ghostnetGetMessage());
	} else {
		snprintf(g_GhostResetMsg, sizeof(g_GhostResetMsg),
				"Answer every question your account has.\n");
	}

	return g_GhostResetMsg;
}

static char *menutextGhostNewPinRow(struct menuitem *item)
{
	// The same box as the account page's PIN row, named for what it means
	// here. See menutextGhostPinRow() for why it is shown as dots.
	static char text[64];
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

	snprintf(text, sizeof(text), "New PIN: %s\n", len ? dots : "(not set)");

	return text;
}

static MenuDialogHandlerResult menudialogGhostReset(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		ghostnetClearState();
	}

	return 0;
}

struct menuitem g_GhostResetMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostResetStatus,
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
		(uintptr_t)&menutextGhostNewPinRow,
		0,
		(void *)&g_GhostPinMenuDialog,
	},
	GHOST_QUESTION_ROWS,
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
		(uintptr_t)"Reset PIN\n",
		0,
		menuhandlerGhostResetPin,
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

struct menudialogdef g_GhostResetMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Reset PIN",
	g_GhostResetMenuItems,
	menudialogGhostReset,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * The row on the account page, which says how much is chosen and not what.
 *
 * It used to name the category, and a category on a page that might be on
 * stream is a third of the secret handed out with the PIN's dots beside it.
 * The names are only ever shown on the pages behind the red warning.
 */
static char *menutextGhostQuestionRow(struct menuitem *item)
{
	static char text[64];
	s32 count = ghostnetRecoveryCount();

	if (ghostnetRecoveryIsSet()) {
		snprintf(text, sizeof(text), "Security Questions: (set)\n");
	} else if (count > 0) {
		snprintf(text, sizeof(text), "Security Questions: (%d of %d)\n", count, GHOSTNET_NUMQUESTIONS);
	} else {
		snprintf(text, sizeof(text), "Security Questions: (not set)\n");
	}

	return text;
}

/**
 * Streamer beware: the red window in front of anything that shows a secret.
 *
 * The account page itself shows the PIN as dots and the questions as a count,
 * so it is safe to have on screen. The PIN keyboard shows the digits as they
 * are typed, the questions page shows three categories and three answers by
 * name, and Reset PIN shows both - and a player streaming to a hundred people
 * has no reason to expect a game menu to do that. So each of those pages is
 * reached through this one, every time: a warning shown once a run is one a
 * streamer who started their stream after dismissing it never saw.
 *
 * MENUDIALOGTYPE_DANGER is the game's own red, the one the abort and delete
 * confirmations use. The Show It row closes this dialog before its handler
 * runs (menuitemSelectableTick pops first, then calls), which is what lets
 * the handler push the page this stood in front of and have it land on top
 * of the account page rather than on top of the warning.
 */
static struct menudialogdef *g_GhostSensitiveNext = NULL;

extern struct menudialogdef g_GhostSensitiveMenuDialog;

static void menuGhostOpenSensitive(struct menudialogdef *next)
{
	g_GhostSensitiveNext = next;
	menuPushDialog(&g_GhostSensitiveMenuDialog);
}

static MenuItemHandlerResult menuhandlerGhostSensitiveShow(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET && g_GhostSensitiveNext) {
		menuPushDialog(g_GhostSensitiveNext);
	}

	return 0;
}

struct menuitem g_GhostSensitiveMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)"The next screen shows sensitive info -\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)"your PIN or security answers, readable.\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)"Streaming or recording? Hide the game first.\n",
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
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Show It\n",
		0,
		menuhandlerGhostSensitiveShow,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_GhostSensitiveMenuDialog = {
	MENUDIALOGTYPE_DANGER,
	(uintptr_t)"Streamer Beware!",
	g_GhostSensitiveMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * The three doors on the account page that open through the warning.
 *
 * A row that opens a dialog has no handler - the dialog sits where the
 * handler would - so these are rows with a handler that pushes the warning,
 * with the page it stands in front of remembered for the Show It row.
 */
static MenuItemHandlerResult menuhandlerGhostPinDoor(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuGhostOpenSensitive(&g_GhostPinMenuDialog);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostQuestionDoor(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuGhostOpenSensitive(&g_GhostQuestionMenuDialog);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGhostResetDoor(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuGhostOpenSensitive(&g_GhostResetMenuDialog);
	}

	return 0;
}

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
		0,
		(uintptr_t)&menutextGhostPinRow,
		0,
		menuhandlerGhostPinDoor,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)&menutextGhostQuestionRow,
		0,
		menuhandlerGhostQuestionDoor,
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
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Forgot My PIN...\n",
		0,
		menuhandlerGhostResetDoor,
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

		g_GhostModelParams = 0;
	}

	// items[4] is the list of times; see the table below.
	if (operation == MENUOP_TICK
			&& g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		menuGhostTickBoardModel(g_Menus[g_MpPlayerNum].curdialog, &dialogdef->items[4]);
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
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_0002,
	NULL,
};

/**
 * The nameplate in the top left corner of Ghost Trials.
 *
 * Every page under this door acts on behalf of one account and records as one
 * character, and neither of those is visible from the rows: "Upload My Ghosts"
 * uploads as somebody, "Ghost Mission" records as somebody wearing something.
 * A player with three accounts on one machine has no way to check which of
 * them is loaded short of opening the account page and coming back.
 *
 * So it is drawn rather than listed - a window in the corner with the face
 * that will be recorded and the name it will be filed under. A row of text
 * could say the same thing and did, but a face is the thing that is wrong at a
 * glance when the wrong account is loaded.
 *
 * It is not a dialog. A dialog would be pushed onto the menu stack and become
 * the current one, and the current dialog is the one input goes to, so the
 * page behind it would stop responding - the same reason the character preview
 * beside a list is drawn rather than pushed. What is here instead is the parts
 * a dialog is made of: the same background, borders and title bar the menu
 * draws for every other window, at a rectangle of our own choosing.
 */

// The plaque, in menu units. Inset from the top right corner of the screen -
// see the alignment note in ghostmenuRenderPlaque() for why the corner of the
// screen and the corner of the view are not the same place. The right rather
// than the left because the frame counter is drawn in the top left, and a
// nameplate underneath it is a nameplate with a number over its title bar.
#define MODGHOST_PLAQUEMARGIN 2
#define MODGHOST_PLAQUEY      6
// The window is measured to the name rather than fixed, because a nameplate
// sized for the longest name a rule allows is mostly empty for every real one:
// accounts are called dab and dnx. The floor is a plaque that still reads as
// one when the dialog underneath leaves no corner to sit in; the ceiling stops
// a fifteen character name from becoming a banner across the top of the
// screen.
#define MODGHOST_PLAQUEMINW  54
#define MODGHOST_PLAQUEMAXW  108
// The body below the title bar: tall enough for a head with a name beside it.
#define MODGHOST_PLAQUEBODYH 26
// The square the head is drawn in, at the left of the body. It is the model's
// whole viewport, so the head is centred in it without any offset being asked
// for - see the scale note below for why an offset would be the hard way.
#define MODGHOST_PLAQUEHEADW 24

/**
 * Whether the plaque is on screen at all.
 *
 * Every page under Ghost Trials, and the mission select and briefing that
 * Ghost Mission opens, because all of them are the same trial being set up and
 * the account and character are what the plaque is there to keep in front of
 * the player throughout.
 */
static bool menuGhostPlaqueVisible(void)
{
	struct menu *menu = &g_Menus[g_MpPlayerNum];

	return menu->curdialog != NULL && menuIsDialogOpen(&g_GhostTrialsMenuDialog);
}

/**
 * The plaque's own model, and the memory it loads into.
 *
 * There is one menumodel per player and the pages that show a character have
 * it: Customize Character turns one, and My Ghosts, Choose Ghosts and
 * Leaderboards stand one beside their rows. The plaque used to stand down on
 * those pages for exactly that reason, which left the account and the
 * character invisible on the three pages where you are picking between other
 * people's runs - the place a player is most likely to be wondering whose
 * name is about to go on theirs.
 *
 * So it has one of its own. A head is a small model and this is a head-sized
 * buffer, taken once from the host and never given back.
 *
 * Not from the game's own pools, which are the obvious place and both wrong.
 * MEMPOOL_STAGE is emptied on every stage load, so the pointer would outlive
 * the pool it came from and the plaque would have to notice; MEMPOOL_PERMANENT
 * is closed off the first time the stage pool is reset, which is before any
 * menu the plaque appears in, so it has nothing left to give - asking it
 * returns NULL and the head silently never loaded. osVirtualToPhysical() is
 * the identity here and the port already loads whole ROM segments into
 * sysMemAlloc() memory, so plain host memory is what the renderer wants.
 *
 * The buffer must never be left NULL when the model is drawn. menuRenderModel()
 * answers a null allocstart by helping itself to the gun memory, which is
 * where the page's own model lives - the preview beside the rows would go out
 * to make room for the head in the corner. If the allocation fails the plaque
 * draws without a face instead.
 */
#ifdef PLATFORM_64BIT
#define MODGHOST_PLAQUEMODELMEM 0x38400
#else
#define MODGHOST_PLAQUEMODELMEM 0x25800
#endif

static struct menumodel g_GhostPlaqueModel = { 0 };

// Two entries because vi0000af00() writes into the one the frame's back buffer
// index names, the same shape as the array the dialogs use.
static Vp g_GhostPlaqueViewport[2];

// The character the plaque's head was last placed for. Its own rather than the
// one the previews beside the lists use, now that the two are different models
// and both can be on screen at once.
static u32 g_GhostPlaqueParams = 0;

static struct menumodel *menuGhostPlaqueModelAlloc(void)
{
	if (g_GhostPlaqueModel.allocstart == NULL) {
		// Rounded up to a 64 byte boundary, which is the alignment the model
		// loader assumes of the buffer it is handed and the one mempAlloc()
		// would have given it.
		u8 *mem = sysMemAlloc(MODGHOST_PLAQUEMODELMEM + 64);

		if (mem == NULL) {
			return NULL;
		}

		menuResetModel(&g_GhostPlaqueModel, MODGHOST_PLAQUEMODELMEM, false);

		g_GhostPlaqueModel.allocstart = (u8 *)ALIGN64((uintptr_t)mem);

		// menuResetModel() leaves a load pending for the head and body pair,
		// which is not what this model ever shows. The placement below names
		// the head it wants on the first frame it draws.
		g_GhostPlaqueModel.newparams = 0;
	}

	return &g_GhostPlaqueModel;
}

/**
 * Stand the trial character's head up in the plaque, facing the player.
 *
 * The head on its own rather than a whole body, because at the size of a
 * corner window a whole character is a blue smudge. This is the model the
 * arena's head carousel shows and it is loaded the same way: a head is its own
 * file, so the params are a file number rather than the head-and-body pair the
 * previews beside the lists use. The two encodings cannot collide - a
 * head-and-body params word always has 0xffff in its low half - which is what
 * lets one "what is the model showing" variable serve both.
 *
 * Facing forward and still. The carousel turns its head a little off centre
 * and the previews rotate, both of which say "there is more of this to see".
 * There is not: this is a label.
 */
static void menuGhostPlaqueModel(struct menumodel *model, s32 mphead, s32 x, s32 y, s32 size)
{
	s32 headnum;
	u32 params;

	// The sunglasses, the closed eyes and the hudpiece are parts of a head
	// model that the head carousel hides and a face on a nameplate should not
	// be wearing either.
	static struct modelpartvisibility visibility[] = {
		{ MODELPART_HEAD_SUNGLASSES, false },
		{ MODELPART_HEAD_EYESCLOSED, false },
		{ MODELPART_HEAD_HUDPIECE,   false },
		{ 255, false },
	};

	if (mphead < mpGetNumHeads2()) {
		headnum = mpGetHeadId(mphead);
		params = MENUMODELPARAMS_SET_FILENUM(g_HeadsAndBodies[headnum].filenum);
	} else {
		headnum = mpGetBeauHeadId(func0f14a9f8(mphead - mpGetNumHeads2()));
		params = MENUMODELPARAMS_SET_FILENUM(g_HeadsAndBodies[headnum].filenum);
	}

	if (g_GhostPlaqueParams != params) {
		g_GhostPlaqueParams = params;

		model->isperfecthead = mphead >= mpGetNumHeads2();
		model->perfectheadnum = model->isperfecthead ? mphead - mpGetNumHeads2() : 0;

		menuConfigureModel(model, 0, 0, 0, 0, 0, 0, 1, MENUMODELFLAG_HASSCALE);

		// Centred in its own viewport, so no offset within the plaque is asked
		// for. The model's position is worked out against the whole view and
		// then drawn through a viewport the size of the scissor, which shrinks
		// every distance by the ratio between the two - about a seventh here.
		// Placing a head a few units left of centre would mean asking for a
		// few hundred, and the number would change with the size of the box.
		// Giving the head a box of its own costs one rectangle and no
		// arithmetic at all.
		model->curposx = model->newposx = 0.0f;
		model->curposy = model->newposy = 0.0f;

		// Same shrink, in reverse: zoom 30 is the size the head carousel asks
		// for against a dialog most of the screen tall, and this viewport is a
		// fraction of that, so scale puts back roughly what the ratio takes
		// away. It is tuned by eye rather than derived, because fovy is the
		// player's setting and the ratio is not the only term.
		model->curscale = model->newscale = 1.0f;
		model->zoom = 30.0f;

		// Square on to the player and left there. rottimer60 is what the
		// previews count down before they start turning; never touching it
		// again is what keeps this one still.
		model->currotx = model->newrotx = 0.0f;
		model->curroty = model->newroty = 0.0f;
		model->currotz = model->newrotz = 0.0f;

		model->loaddelay = 8;
		model->removingpiece = false;
	}

	model->newparams = params;
	model->newanimnum = 0;
	model->partvisibility = visibility;

	g_MenuScissorX1 = x;
	g_MenuScissorY1 = y;
	g_MenuScissorX2 = x + size;
	g_MenuScissorY2 = y + size;
}

/**
 * The window a corner plaque is drawn in.
 *
 * Title bar, its two shimmers, the body and its borders, then the title: the
 * same calls dialogRender() makes in the same order, so these weather a change
 * to how a window looks along with every other window in the game.
 *
 * The alignment and the scissor are the caller's, because the caller is the
 * one holding on to an edge of the screen. What the caller draws inside is its
 * own too - this only puts the box there.
 */
static Gfx *menuGhostWindow(Gfx *gdl, s32 x1, s32 y1, s32 x2, s32 y2, char *title)
{
	// Standing in for a dialog where the menu's own drawing wants one. Only
	// its type and transition are read, and both say the same thing every
	// frame: an ordinary blue window, not mid-change. Keeping it separate from
	// the dialog on screen is deliberate - a red confirmation over the top
	// should not turn these red with it. Ghost Trials is named as the
	// definition because that is the page they belong to wherever the cursor
	// has got to, and nothing reads it except code that would want that.
	static struct menudialog window = { 0 };

	const struct menucolourpalette *colours = &g_MenuColours[MENUDIALOGTYPE_DEFAULT];
	s32 bodytop = y1 + LINEHEIGHT;
	s32 x;
	s32 y;

	window.definition = &g_GhostTrialsMenuDialog;
	window.type = MENUDIALOGTYPE_DEFAULT;
	window.type2 = MENUDIALOGTYPE_DEFAULT;
	window.transitionfrac = -1.0f;
	window.colourweight = 0;
	window.x = x1;
	window.y = y1;
	window.width = x2 - x1;
	window.height = y2 - y1;

	gdl = menugfxRenderGradient(gdl, x1 - 2, y1, x2 + 2, bodytop,
			colours->dialog_border1, colours->dialog_titlebg, colours->dialog_border2);
	gdl = menugfxDrawShimmer(gdl, x1 - 2, y1, x2 + 2, y1 + 1,
			(colours->dialog_border1 & 0xff) >> 1, 1, 40, 0);
	gdl = menugfxDrawShimmer(gdl, x1 - 2, y1 + 10, x2 + 2, bodytop,
			(colours->dialog_border1 & 0xff) >> 1, 0, 40, 1);
	gdl = menugfxRenderDialogBackground(gdl, x1 + 1, bodytop, x2 - 1, y2, &window,
			colours->dialog_bodybg, colours->unused14, -1.0f);

	gdl = text0f153628(gdl);

	x = x1 + 3;
	y = y1 + 3;
	gdl = textRenderProjected(gdl, &x, &y, title, g_CharsHandelGothicSm,
			g_FontHandelGothicSm, colours->dialog_titlefg & 0xff, window.width, viGetHeight(), 0, 0);

	x = x1 + 2;
	y = y1 + 2;
	gdl = textRenderProjected(gdl, &x, &y, title, g_CharsHandelGothicSm,
			g_FontHandelGothicSm, colours->dialog_titlefg, window.width, viGetHeight(), 0, 0);

	gdl = text0f153780(gdl);

	return gdl;
}

/**
 * How far the right hand alignment moves what is drawn under it, in menu units.
 *
 * G_ASPECT_RIGHT_EXT is an offset applied to vertices as they are projected:
 * fast3d adds aspect_ofs, which is the window's aspect over the view's less
 * one, and that is half the screen's worth of it. Everything drawn moves by
 * that and nothing has to know - except the model, whose viewport
 * menuRenderModel() builds out of g_MenuScissor without ever seeing a vertex.
 * So the model's rectangle is moved by hand, by the amount everything else
 * moves by itself, or the head is drawn beside the viewport it is clipped to
 * and nothing appears at all.
 *
 * SCREEN_WIDTH_LO / 2 rather than the view's own half width: the offset is
 * given in the projection's units, and that is the pair menuRenderModel()
 * already works in when it turns a rectangle into a viewport.
 */
static s32 menuGhostPlaqueAlignOffset(void)
{
	f32 window = videoGetAspect();
	f32 native = (f32)videoGetNativeWidth() / (f32)videoGetNativeHeight();
	f32 ofs = window / native - 1.0f;

	if (ofs <= 0.0f) {
		return 0;
	}

	// The cap G_ASPECT_WIDE_EXT applies, so an ultrawide display puts the
	// plaque beside the menu rather than a foot away from it.
	if (window > 16.0f / 9.0f) {
		ofs *= (16.0f / 9.0f) / window;
	}

	return (s32)(ofs * (SCREEN_WIDTH_LO / 2));
}

/**
 * Hold what follows against one edge of the screen instead of the middle.
 *
 * menuRender() draws everything after the background with G_ASPECT_CENTER_EXT,
 * which holds the menu at the view's own aspect in the middle of the window,
 * so on a widescreen display there is a pillar of screen either side that no
 * dialog reaches. G_ASPECT_CENTER_EXT is G_ASPECT_LEFT_EXT |
 * G_ASPECT_RIGHT_EXT, and dropping one half of it is the port's own way of
 * saying "hold this against that edge" - what the HUD's Align setting does
 * with g_HudAlignModeL and g_HudAlignModeR. What is drawn moves out into the
 * pillar at the size and shape it already had, because the alignment is an
 * offset rather than a stretch.
 *
 * WIDE stops the offset growing past 16:9, so an ultrawide display puts these
 * beside the menu rather than a foot away from it. On a 4:3 window there is no
 * pillar, the offset is zero, and this is the corner of the view.
 *
 * Called before the scissor rather than after: fast3d turns a scissor
 * rectangle into pixels at the moment the command is sent, using the alignment
 * in force then, so a scissor set first stays behind in the middle of the
 * screen and cuts the window in half.
 */
static Gfx *menuGhostAlign(Gfx *gdl, s32 side)
{
	gSPClearExtraGeometryModeEXT(gdl++, side == G_ASPECT_RIGHT_EXT ? G_ASPECT_LEFT_EXT : G_ASPECT_RIGHT_EXT);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_WIDE_EXT);

	return gdl;
}

/**
 * Put back what these borrowed: the menu's own alignment, and a scissor that
 * is the view rather than a square in the corner of it.
 */
static Gfx *menuGhostAlignRestore(Gfx *gdl, s32 viewleft, s32 viewtop, s32 viewright)
{
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_WIDE_EXT);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	g_MenuScissorX1 = viewleft;
	g_MenuScissorY1 = viewtop;
	g_MenuScissorX2 = viewright;
	g_MenuScissorY2 = viewtop + viGetViewHeight();

	return menuApplyScissor(gdl);
}

/**
 * What a trial does to the game, said on the way in.
 *
 * This was a line of small text at the top of Ghost Options, which is where it
 * went when it was one sentence and the wrong place for it from the start: a
 * player opens Ghost Options to change how the ghosts look, and the rules of
 * the mode are not a setting. Somebody who pressed jump in a trial and did not
 * leave the ground had found a bug unless something had told them otherwise,
 * and nothing on the way in did.
 *
 * So it is a window on the Ghost Trials page instead, next to the door rather
 * than behind it. Down the left because that is where the room is: the Ghost
 * Trials dialog is as tall as the view has room for, so there is no bottom of
 * the screen to put it along, and holding it against the left edge puts the
 * whole of it in the pillar the menu does not reach on a widescreen display.
 *
 * Short lines rather than sentences, and written out rather than wrapped. The
 * room is what it is - the gap between the left of the screen and the left of
 * the dialog is about ninety menu units, which is fifteen characters of the
 * font the menus are written in - and the alternatives were both worse. The
 * font one size down is capitals only and barely narrower, and a window wide
 * enough for sentences would have to sit on top of the dialog, which is the
 * thing the plaque was moved out of the way to stop doing.
 *
 * A list is what a rules box wants to be anyway: something to be taken in at a
 * glance on the way past, not read.
 */
static char *g_GhostRulesLines[] = {
	"Recording on.\n",
	"No Jump, Roll\n",
	"or Melee Combo\n",
	"No Flinch.\n",
	"Cheats are off\n",
	"automatically.\n",
};

// How far down the window each line sits, and how far its text is inset.
#define MODGHOST_RULESLINE   9
#define MODGHOST_RULESINSET  3
// How far into the dialog's own left margin the window may go when there is no
// screen beside the menu to go into instead.
#define MODGHOST_RULESBITE   32

static Gfx *menuGhostRenderRules(Gfx *gdl)
{
	const struct menucolourpalette *colours = &g_MenuColours[MENUDIALOGTYPE_DEFAULT];
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;
	s32 viewleft = viGetViewLeft() / g_ScaleX;
	s32 viewtop = viGetViewTop();
	s32 viewright = (viGetViewLeft() + viGetViewWidth()) / g_ScaleX;
	s32 width = 0;
	s32 maxwidth;
	s32 textwidth;
	s32 textheight;
	s32 x1;
	s32 x2;
	s32 y1;
	s32 y2;
	s32 x;
	s32 y;
	s32 i;

	// The page itself, not everything under it. On the pages inside Ghost
	// Trials the rules have already been read and the room down the left is
	// wanted by whatever those pages are showing.
	if (dialog == NULL || dialog->definition != &g_GhostTrialsMenuDialog) {
		return gdl;
	}

	for (i = 0; i < (s32)ARRAYCOUNT(g_GhostRulesLines); i++) {
		textMeasure(&textheight, &textwidth, g_GhostRulesLines[i],
				g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);

		if (textwidth > width) {
			width = textwidth;
		}
	}

	width += MODGHOST_RULESINSET * 2;

	// How far right it may reach. The alignment moves this window left and
	// leaves the dialog where it was, so on a widescreen display the gap
	// between the two is worth that many units of extra width - the difference
	// between three lines of this and six.
	//
	// On a 4:3 window there is no such gap and the honest width would be ten
	// characters, so a little of the dialog is allowed. Its rows are centred
	// and its own left margin is wider than this, so what is covered is the
	// window's background rather than anything written on it. The natural
	// width is what it is: this is a ceiling, and on any display wide enough
	// the lines fit inside the gap and never reach it.
	x1 = viewleft + MODGHOST_PLAQUEMARGIN;
	maxwidth = dialog->x + MODGHOST_RULESBITE - 2 + menuGhostPlaqueAlignOffset() - x1;

	if (width > maxwidth) {
		width = maxwidth;
	}

	x2 = x1 + width;
	y2 = viewtop + viGetViewHeight() - MODGHOST_PLAQUEY;
	y1 = y2 - LINEHEIGHT - ARRAYCOUNT(g_GhostRulesLines) * MODGHOST_RULESLINE - 3;

	gdl = menuGhostAlign(gdl, G_ASPECT_LEFT_EXT);

	g_MenuScissorX1 = viewleft;
	g_MenuScissorY1 = viewtop;
	g_MenuScissorX2 = viewright;
	g_MenuScissorY2 = viewtop + viGetViewHeight();
	gdl = menuApplyScissor(gdl);

	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	gdl = menuGhostWindow(gdl, x1, y1, x2, y2, "Trial Rules\n");

	gdl = text0f153628(gdl);

	for (i = 0; i < (s32)ARRAYCOUNT(g_GhostRulesLines); i++) {
		x = x1 + MODGHOST_RULESINSET;
		y = y1 + LINEHEIGHT + 1 + i * MODGHOST_RULESLINE;

		gdl = textRenderProjected(gdl, &x, &y, g_GhostRulesLines[i],
				g_CharsHandelGothicSm, g_FontHandelGothicSm, colours->item_unfocused,
				x2 - x, viGetHeight(), 0, 0);
	}

	gdl = text0f153780(gdl);

	return menuGhostAlignRestore(gdl, viewleft, viewtop, viewright);
}

/**
 * Draw the plaque: the window, the head and the name.
 *
 * Called after the dialogs rather than among them, because it belongs to no
 * dialog. That also means the scissor is wherever the last dialog left it, so
 * every part of this sets its own and the model's viewport is put back before
 * anything else is drawn through it.
 */
static Gfx *menuGhostRenderPlaque(Gfx *gdl)
{
	const struct menucolourpalette *colours = &g_MenuColours[MENUDIALOGTYPE_DEFAULT];
	// The dialog the plaque last drew over, per player, and NULL for a player
	// it is not drawing for.
	//
	// Per player because menuRender() walks every player's menu each frame and
	// three of the four have no dialog open at all; one shared latch was
	// cleared by those three every frame, so the plaque re-placed the head on
	// every one of its own frames and the load never got to the end of its
	// delay - an empty box, forever.
	static struct menudialogdef *lastdialog[MAX_PLAYERS] = { NULL };
	struct menumodel *model;
	s32 viewleft = viGetViewLeft() / g_ScaleX;
	s32 viewtop = viGetViewTop();
	s32 viewright = (viGetViewLeft() + viGetViewWidth()) / g_ScaleX;
	s32 width;
	s32 textwidth;
	s32 textheight;
	s32 x1;
	s32 x2;
	s32 y1;
	s32 y2;
	s32 bodytop;
	s32 headx;
	s32 heady;
	s32 mpbody;
	s32 mphead;
	s32 x;
	s32 y;

	if (!menuGhostPlaqueVisible()) {
		lastdialog[g_MpPlayerNum] = NULL;
		return gdl;
	}

	// The account, or the fact that there is not one. A run recorded without
	// an account still goes in the ghosts directory and still carries the
	// character, so the plaque has something true to say either way.
	snprintf(g_GhostRowText, sizeof(g_GhostRowText), "%s\n",
			ghostnetHasAccount() ? ghostnetGetAccountName() : "No Account");

	textMeasure(&textheight, &textwidth, g_GhostRowText, g_CharsHandelGothicSm,
			g_FontHandelGothicSm, 0);

	width = MODGHOST_PLAQUEHEADW + textwidth + 9;

	if (width < MODGHOST_PLAQUEMINW) {
		width = MODGHOST_PLAQUEMINW;
	} else if (width > MODGHOST_PLAQUEMAXW) {
		width = MODGHOST_PLAQUEMAXW;
	}

	// The right edge of the view, which with the alignment set below is the
	// right edge of the screen. Everything the menu draws is in view units and
	// stays inside them - text0f15568c() drops any glyph past viGetWidth()
	// whatever the scissor says - so the way out of the middle of the screen
	// is not a bigger coordinate, it is drawing the same coordinates somewhere
	// else. Two units of margin rather than none, because the last character
	// of a name has to land inside the view rather than on the line.
	x2 = viewright - MODGHOST_PLAQUEMARGIN;
	x1 = x2 - width;
	y1 = viewtop + MODGHOST_PLAQUEY;
	y2 = y1 + LINEHEIGHT + MODGHOST_PLAQUEBODYH;
	bodytop = y1 + LINEHEIGHT;
	headx = x1 + 2;
	heady = bodytop + 1;

	// Only on the way back from not being drawn at all, not on every page.
	// The plaque's model is its own and menuPushDialog() does not unload it,
	// so moving between the pages of Ghost Trials leaves the head where it is
	// rather than blinking it out for the length of a load. What it does catch
	// is coming back to the menu later, when the model in the buffer is from
	// before whatever the game did in between.
	if (lastdialog[g_MpPlayerNum] == NULL) {
		g_GhostPlaqueParams = 0;
	}

	lastdialog[g_MpPlayerNum] = g_Menus[g_MpPlayerNum].curdialog->definition;

	// Against the right of the screen, out of the middle where the dialog is.
	gdl = menuGhostAlign(gdl, G_ASPECT_RIGHT_EXT);

	g_MenuScissorX1 = viewleft;
	g_MenuScissorY1 = viewtop;
	g_MenuScissorX2 = viewright;
	g_MenuScissorY2 = viewtop + viGetViewHeight();
	gdl = menuApplyScissor(gdl);

	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	gdl = menuGhostWindow(gdl, x1, y1, x2, y2, "Player\n");

	gdl = text0f153628(gdl);

	x = headx + MODGHOST_PLAQUEHEADW + 3;
	y = bodytop + 9;
	gdl = textRenderProjected(gdl, &x, &y, g_GhostRowText, g_CharsHandelGothicSm,
			g_FontHandelGothicSm, colours->item_unfocused,
			x2 - x, viGetHeight(), 0, 0);

	gdl = text0f153780(gdl);

	model = menuGhostPlaqueModelAlloc();

	if (model == NULL) {
		// No buffer, so no face. The window and the name are the half of this
		// that costs nothing, and drawing them is better than the corner
		// disappearing because a pool was full.
		return menuGhostAlignRestore(gdl, viewleft, viewtop, viewright);
	}

	// The character, from the same two settings a recorded run is stamped
	// with, so a face here that is not the face in the ghost file would be a
	// bug rather than a difference of opinion.
	mpbody = MPBODY_DARK_AF1;
	mphead = MPHEAD_DARK_COMBAT;

	if (g_ModGhostBody > MODGHOST_BODY_DEFAULT && g_ModGhostBody - 1 < (s32)mpGetNumBodies()) {
		mpbody = g_ModGhostBody - 1;
		mphead = modGhostBodyDefaultHead(mpbody);
	}

	if (g_ModGhostHead > MODGHOST_BODY_DEFAULT && g_ModGhostHead - 1 < mpGetNumHeads2()) {
		mphead = g_ModGhostHead - 1;
	}

	// The scissor stays the plaque's - the whole view, shifted with everything
	// else - and the head's square is only ever the model's viewport. That is
	// deliberate: a scissor is turned into pixels with the alignment offset
	// added and a viewport is not, so one rectangle cannot serve as both out
	// here. The head is drawn well inside its square anyway; the square is
	// where it stands, not what clips it.
	//
	// Taking the frame's z-buffer here is what lets that be true.
	// menuRenderModel() prepares it on the first model of the frame and
	// applies g_MenuScissor while it is at it, which would put the shifted
	// square back in the scissor's place. Doing its work first leaves it
	// nothing to do.
	if (g_MenuData.usezbuf) {
		gdl = viPrepareZbuf(gdl);
		gdl = vi0000b1d0(gdl);
		g_MenuData.usezbuf = false;
		gdl = menuApplyScissor(gdl);
	}

	menuGhostPlaqueModel(model, mphead, headx + menuGhostPlaqueAlignOffset(), heady,
			MODGHOST_PLAQUEHEADW);

	// A viewport of its own for the same reason it has a model of its own: the
	// page's model was rendered a moment ago and the display list still points
	// at the Vp it was given, so sharing one would draw the character beside
	// the rows through the little square in the corner and it would disappear.
	gSPSetGeometryMode(gdl++, G_ZBUFFER);
	g_MenuModelViewport = g_GhostPlaqueViewport;
	gdl = menuRenderModel(gdl, model, MENUMODELTYPE_DEFAULT);
	g_MenuModelViewport = NULL;
	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	// menuRenderModel() leaves the viewport it drew through behind it. Every
	// other caller puts the player's back and so does this one, because the
	// next thing to draw is whatever the menu draws after the dialogs.
	viSetViewPosition(g_Vars.currentplayer->viewleft, g_Vars.currentplayer->viewtop);
	viSetFovAspectAndSize(g_Vars.currentplayer->fovy, g_Vars.currentplayer->aspect,
			g_Vars.currentplayer->viewwidth, g_Vars.currentplayer->viewheight);

	return menuGhostAlignRestore(gdl, viewleft, viewtop, viewright);
}

/**
 * Offer the account chooser the first time somebody opens Ghost Trials.
 *
 * Once per run of the game, and only when there is no account at all, because
 * the point is to ask a new player the question rather than to keep asking it.
 * Backing out of it leaves everything working: recording, racing and the
 * ghosts directory need no account and never did.
 *
 * An account that is already in pd.ini is checked here instead, on the same
 * one-per-run terms. It is the only thing the game does that asks the server
 * whether the two saved values still open an account, and it is worth one
 * request: the answer is what the account page reports for the rest of the
 * session, so a player who really is signed in is told so without having to
 * press anything, and a player whose name was never registered - the case this
 * exists for - finds out here rather than from a run they have already set and
 * cannot publish.
 *
 * The sign-in also comes back saying whether the account has a security
 * question, and an account made before there was one to ask for cannot reset a
 * PIN at all. Its owner has no way to find that out except by losing the PIN,
 * so the page they need is put in front of them once, when the answer arrives
 * - which is a frame or so after the page opened, not during it, and is why
 * this waits on the tick rather than doing it all in MENUOP_OPEN.
 *
 * Once each, and never again in the same run whether or not they set one:
 * backing out is an answer, and a page that reappears every time this one is
 * opened is a page people learn to dismiss without reading.
 */
/**
 * Whether this visit to Ghost Trials is an online one.
 *
 * Chosen on the page in front of this one every time it is entered from the
 * main menu, and remembered by nothing. Offline, the page does not sign in,
 * does not ask for an account and does not nag about the questions, and the
 * three rows that talk to the server are greyed out - a trial is recorded
 * and raced exactly as before, and the runs stay on this machine until a
 * visit that says Online shares them.
 */
static bool g_GhostOnline = false;

static MenuDialogHandlerResult menudialogGhostTrials(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	static bool asked = false;
	static bool nagged = false;

	if (operation == MENUOP_OPEN && g_GhostOnline && !asked) {
		asked = true;

		if (ghostnetIsAvailable()) {
			if (!ghostnetHasAccount()) {
				menuPushDialog(&g_GhostAccountsMenuDialog);
			} else if (ghostnetAccountIsValid() && !ghostnetIsSignedIn()) {
				ghostnetLogin();
			}
		}
	}

	// Only while this page is the one on top. A tick reaches every dialog on
	// the stack, and pushing one from underneath the player's own would open
	// the security questions over whatever they had gone on to open. Through
	// the red warning, because the page it opens shows the answers by name.
	if (operation == MENUOP_TICK && g_GhostOnline && !nagged
			&& g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef
			&& (ghostnetGetAccountRecovery() == GHOSTNET_RECOVERY_MISSING
				|| ghostnetGetAccountRecovery() == GHOSTNET_RECOVERY_PARTIAL)) {
		nagged = true;
		menuGhostOpenSensitive(&g_GhostQuestionMenuDialog);
	}

	return 0;
}

/**
 * The three rows that talk to the server, greyed out on an offline visit.
 *
 * A row that opens a dialog cannot also be disabled - the dialog sits where
 * the handler would - so each is a row with a handler that pushes its page.
 */
static MenuItemHandlerResult menuhandlerGhostOnlineRow(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct menudialogdef *dialogs[] = {
		&g_GhostAccountsMenuDialog,
		&g_GhostShareMenuDialog,
		&g_GhostBoardMenuDialog,
	};

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_GhostOnline;
	case MENUOP_SET:
		if (item->param < (s32)ARRAYCOUNT(dialogs)) {
			menuPushDialog(dialogs[item->param]);
		}
		break;
	}

	return 0;
}

/**
 * The rows are the main menu's, not the options menu's.
 *
 * MENUITEMFLAG_BIGFONT and no trailing newline is how every row of the Perfect
 * Menu is written, and this page sits next to Solo Missions rather than inside
 * Options - a door to a way of playing rather than a list of settings. Reading
 * like the settings pages was the wrong signal about what it is.
 *
 * Customize Character lives here for the same reason. It was in Ghost Options
 * because it began as a dropdown among settings, but choosing who runs is part
 * of setting up a trial rather than a preference about how trials behave.
 *
 * There is no account line above the rows any more. It said "Racing as dab"
 * and the plaque in the corner says the same thing with the face beside it, so
 * the row was the same sentence twice on one screen.
 */
struct menuitem g_GhostTrialsMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Mission",
		0,
		menuhandlerGhostMission,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Customize Character",
		0,
		(void *)&g_GhostCharacterMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Options",
		0,
		(void *)&g_GhostOptionsMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"My Ghosts",
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
		MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Account",
		0,
		menuhandlerGhostOnlineRow,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		1,
		MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Share",
		0,
		menuhandlerGhostOnlineRow,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		2,
		MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Leaderboards",
		0,
		menuhandlerGhostOnlineRow,
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

struct menudialogdef g_GhostTrialsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Trials",
	g_GhostTrialsMenuItems,
	menudialogGhostTrials,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * Offline or Online: the page the main menu's Ghost Trials row opens.
 *
 * Ghost Trials used to sign in the moment it opened, and open the account
 * page over itself for a player with no account - which is the right thing
 * for somebody here to race the boards and the wrong thing for somebody who
 * wants to run against their own ghost and has no wish to make an account,
 * be nagged about one, or have a game menu talk to the internet at all. So
 * the question is asked first, every time, and the answer decides what the
 * page behind it does (see g_GhostOnline).
 *
 * Both rows close this page before their handler pushes the next, so Back
 * from Ghost Trials lands on the main menu the way it always did, and coming
 * in again asks again.
 */
static MenuItemHandlerResult menuhandlerGhostOnlineMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		// Online needs a transport, and a copy without one has nothing to
		// offer the row but the sentence the account page would say.
		return item->param == 1 && !ghostnetIsAvailable();
	case MENUOP_SET:
		g_GhostOnline = item->param == 1;
		menuPushDialog(&g_GhostTrialsMenuDialog);
		break;
	}

	return 0;
}

static char *menutextGhostModeStatus(struct menuitem *item)
{
	if (!ghostnetIsAvailable()) {
		return "Network support is not built into this copy.\n";
	}

	return "Online signs in to share runs and race the boards.\n";
}

struct menuitem g_GhostModeMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)"Offline keeps your runs on this machine.\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextGhostModeStatus,
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
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Offline",
		0,
		menuhandlerGhostOnlineMode,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		1,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Online",
		0,
		menuhandlerGhostOnlineMode,
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

struct menudialogdef g_GhostModeMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Ghost Trials",
	g_GhostModeMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * Everything Ghost Trials draws beside the menu rather than inside it.
 *
 * Two windows, both held against an edge of the screen: who the runs will
 * belong to in the top right, and what a trial does to the game down the left.
 * Neither is a dialog, for the reason on menuGhostRenderPlaque().
 */
Gfx *ghostmenuRenderOverlay(Gfx *gdl)
{
	gdl = menuGhostRenderPlaque(gdl);
	gdl = menuGhostRenderRules(gdl);

	return gdl;
}
