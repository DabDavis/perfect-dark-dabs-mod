/**
 * A Perfect Dark mod entered from the Perfect Menu, without a restart.
 *
 * The owner, 2026-10-09: "with GE and GF/TND there is no restart, they populate
 * the main menu. I would like the PD Mod flow the same, no restart needed, a
 * separate main menu entry, and can be hosted/uploaded to clients from host."
 * So the Perfect Menu's "Perfect Dark Mods" row lists the installed mods
 * (mainmenu.c), choosing one enters it here, and the Perfect Menu comes back
 * over the Institute with the mod's own missions, arenas, guns and text, and a
 * "Back to Perfect Dark" row that puts stock back.
 *
 * A console mod is more than files. Its ROM segments - the animation table,
 * the texture list, the fonts, the sound and music banks - were read once at
 * boot until now (romdata.c), and the game holds pointers into what was built
 * from them, so loading one took a restart (mods.md, "Files swap live;
 * segments cannot"). They are swapped instead (modSegsEnter(), modAudioEnter())
 * at the one moment nothing points into them: between one stage's teardown
 * and the next one's load (mainLoop -> modModeStageBoundary()), after the
 * sound has been stopped and has drained (modAudioQuiesce(), pumped a frame at
 * a time from lvTick). The files and the tables a mod's modconfig and data
 * segment rewrite go through the same swap Load Mods used (modSwapPath()).
 *
 * The sequence, from a menu row:
 *   modModeRequestEnter/Leave()  queue it, stop the music and the sounds
 *   modModeTick()                each frame until modAudioQuiesce() says quiet,
 *                                then reload the Institute (gexFrontGoBack(),
 *                                which brings the Perfect Menu back)
 *   modModeStageBoundary()       the old mod's segments and banks back to stock,
 *                                the files and tables swapped, the new mod's
 *                                segments and banks in
 *
 * Nothing here runs unless a row asked for it: a run that never opens the list
 * plays exactly as before.
 */

#include <string.h>
#include <stdio.h>
#include <PR/ultratypes.h>
#include "types.h"
#include "data.h"
#include "fs.h"
#include "mod.h"
#include "modmode.h"
#include "modaudio.h"
#include "modloader.h"
#include "gexfront.h"
#include "system.h"
#include "net/net.h"
#include "net/netlobby.h"
#include "game/mplayer/setup.h"
#include "game/menu.h"
#include "constants.h"
#include "bss.h"

#define MODMODE_NAME_LEN 64

enum {
	MODMODE_IDLE,
	MODMODE_QUIETING,   // waiting for the sound to stop
	MODMODE_RELOADING,  // the Institute is reloading; the swap is at the boundary
};

static s32 s_State = MODMODE_IDLE;
static s32 s_QuietFrames;
static s32 s_WantsMenu;

// what is entered now ("" for stock)
static char s_Path[FS_MAXPATH + 1];
static char s_Name[MODMODE_NAME_LEN];

// what the queued swap goes to ("" path: back to stock)
static char s_NextPath[FS_MAXPATH + 1];
static char s_NextName[MODMODE_NAME_LEN];

// The sound gets this long to drain before the swap goes ahead regardless: a
// looping sound something forgot to stop must not hold the player on a menu.
#define MODMODE_QUIET_MAX_FRAMES 180

s32 modModeIsActive(void)
{
	return s_Path[0] != '\0';
}

const char *modModeName(void)
{
	return s_Name;
}

const char *modModePath(void)
{
	return s_Path;
}

s32 modModeWantsMenu(void)
{
	return s_WantsMenu;
}

void modModeMenuShown(void)
{
	s_WantsMenu = false;
}

s32 modModeIsBusy(void)
{
	return s_State != MODMODE_IDLE;
}

s32 modModeCanChange(void)
{
	return !modListIsFromArgs() && g_NetMode == NETMODE_NONE && !g_NetLobbyRoom;
}

static s32 modModeQueue(const char *path, const char *name)
{
	if (s_State != MODMODE_IDLE || modListIsFromArgs()) {
		return false;
	}

	snprintf(s_NextPath, sizeof(s_NextPath), "%s", path ? path : "");
	snprintf(s_NextName, sizeof(s_NextName), "%s", name ? name : "");

	if (!strcmp(s_NextPath, s_Path)) {
		return false;
	}

	// GoldenEye's mode and a ROM hack's are left the way their own rows leave
	// them when another row is chosen (mainmenu.c)
	g_GexPlusVariant = NULL;

	if (g_GexPlusMode) {
		mpSetGexPlusMode(false);
	}

	s_QuietFrames = 0;
	s_State = MODMODE_QUIETING;

	sysLogPrintf(LOG_NOTE, "modmode: %s %s%s%s", s_NextPath[0] ? "entering" : "leaving",
			s_NextPath[0] ? s_NextName : s_Name, s_NextPath[0] ? " from " : "", s_NextPath);

	return true;
}

s32 modModeRequestEnter(const char *path, const char *name)
{
	if (!path || !path[0]) {
		return false;
	}

	return modModeQueue(path, name);
}

s32 modModeRequestLeave(void)
{
	return modModeQueue(NULL, NULL);
}

void modModeTick(void)
{
	if (s_State != MODMODE_QUIETING) {
		return;
	}

	// modAudioQuiesce() stops the music and the sounds on its first call and
	// says when the last of them has gone
	if (modAudioQuiesce() || ++s_QuietFrames >= MODMODE_QUIET_MAX_FRAMES) {
		if (s_QuietFrames >= MODMODE_QUIET_MAX_FRAMES) {
			sysLogPrintf(LOG_WARNING, "modmode: the sound did not stop in %d frames; swapping anyway", s_QuietFrames);
		}

		s_State = MODMODE_RELOADING;

		// the Institute again, with the Perfect Menu over it (menutick.c), the
		// way GoldenEye's folder goes back
		gexFrontGoBack();
	}
}

void modModeStageBoundary(void)
{
	if (s_State != MODMODE_RELOADING) {
		return;
	}

	const s32 entering = s_NextPath[0] != '\0';

	// the old mod's segments and banks back to stock first: a swap from one
	// mod to another is a leave and an enter
	if (s_Path[0]) {
		modSegsLeave();
		modAudioLeave();
	}

	// files, file slots, stage tables, modconfig, the data segment
	modSwapPath(entering ? s_NextPath : NULL);

	if (entering) {
		modSegsEnter(s_NextPath);
		modAudioEnter(s_NextPath);
	}

	memcpy(s_Path, s_NextPath, sizeof(s_Path));
	memcpy(s_Name, s_NextName, sizeof(s_Name));
	s_NextPath[0] = '\0';
	s_NextName[0] = '\0';
	s_State = MODMODE_IDLE;
	s_WantsMenu = true;

	sysLogPrintf(LOG_NOTE, "modmode: %s%s", entering ? "entered " : "back to Perfect Dark", entering ? s_Name : "");
}

/* ---- the list: the Perfect Menu's "Perfect Dark Mods" ------------------- */

static char s_RowText[MODMODE_NAME_LEN + 16];
static char s_ListNote[160];

// the loadable mods: every installed one but the conversions mounted for
// their maps alone (GoldenEye Arenas and the ROM hacks: their own rows)
static s32 modModeListCount(void)
{
	return modListGetLoadableCount();
}

static s32 modModeListIndex(s32 row)
{
	return modListLoadableToIndex(row);
}

static s32 modModeRowIsActive(s32 index)
{
	const char *path = modListGetPath(index);

	return path && s_Path[0] && !strcmp(path, s_Path);
}

static MenuItemHandlerResult menuhandlerModModeList(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 index;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = modModeListCount();
		break;
	case MENUOP_GETOPTIONTEXT:
		index = modModeListIndex((s32)data->list.value);
		snprintf(s_RowText, sizeof(s_RowText), "%s%s\n", modModeRowIsActive(index) ? "> " : "",
				index >= 0 ? modListGetName(index) : "");
		return (intptr_t)s_RowText;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0xfffff;

		for (s32 i = 0; i < modModeListCount(); i++) {
			if (modModeRowIsActive(modModeListIndex(i))) {
				data->list.value = i;
			}
		}
		break;
	case MENUOP_GETOPTGROUPCOUNT:
		data->list.value = 0;
		break;
	case MENUOP_GETOPTGROUPTEXT:
		return (intptr_t)"";
	case MENUOP_GETGROUPSTARTINDEX:
		data->list.groupstartindex = 0;
		break;
	case MENUOP_LISTITEMFOCUS:
		index = modModeListIndex((s32)data->list.value);
		s_ListNote[0] = '\0';

		if (index >= 0) {
			snprintf(s_ListNote, sizeof(s_ListNote), "%s\n", modModeRowIsActive(index)
					? "Playing now. Back to Perfect Dark leaves it."
					: modListHasSegs(index)
						? "Replaces the game's own data: no restart needed."
						: "Files and settings over Perfect Dark's own.");
		}
		break;
	case MENUOP_SET:
		index = modModeListIndex((s32)data->list.value);

		if (index >= 0 && !modModeRowIsActive(index) && modModeCanChange()
				&& modModeRequestEnter(modListGetPath(index), modListGetName(index))) {
			// back to the Perfect Menu, which the Institute's reload brings
			// back with the mod in it
			menuPopDialog();
		}
		break;
	}

	return 0;
}

static char *menutextModModeNote(struct menuitem *item)
{
	if (modModeIsBusy()) {
		return "Loading...\n";
	}

	if (s_ListNote[0]) {
		return s_ListNote;
	}

	return modModeListCount() ? "Choose a mod to play it.\n" : "No mods installed. Mods go in the mods folder.\n";
}

static MenuDialogHandlerResult menudialogModMode(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		// a folder dropped in while the game runs turns up
		modListRefresh();
		s_ListNote[0] = '\0';
	}

	return false;
}

static struct menuitem g_ModModeMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextModModeNote,
		0,
		NULL,
	},
	{
		// param2 is the list width in menu units: a mod folder's name is long
		MENUITEMTYPE_LIST,
		0,
		MENUITEMFLAG_LIST_LEAVEATENDS,
		0x000000d0,
		0,
		menuhandlerModModeList,
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

struct menudialogdef g_ModModeMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Perfect Dark Mods",
	g_ModModeMenuItems,
	menudialogModMode,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
