#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "bss.h"
#include "game/menugfx.h"
#include "lib/vi.h"
#include <stdlib.h>
#include "update.h"
#include "versioninfo.h"

/**
 * Check for Updates, on the Perfect Menu next to the doors it might change.
 *
 * One page with one thing on it, because there is only ever one thing to do:
 * ask, and then either be told this is the latest build or be offered the one
 * that is not. The row's text is the state, so there is no separate button
 * that is greyed out most of the time and nothing to explain about which of
 * two rows to press.
 *
 * The work is all in update.c and all on a worker thread. This polls
 * updateGetState() the way the ghost menus poll ghostnetGetState(), for the
 * same reason: a page that blocked while GitHub thought about it would look
 * like the game had stopped.
 */

static char g_UpdateText[192];

/**
 * What is running, and what the last question was answered with.
 *
 * The build is named on the page whatever state it is in, because "you are up
 * to date" is only worth anything next to a build number that can be compared
 * with what somebody else is running.
 */
static char *menutextUpdateStatus(struct menuitem *item)
{
	const char *msg = updateGetMessage();

	snprintf(g_UpdateText, sizeof(g_UpdateText), "This build: %s %s (%s)\n%s\n",
			VERSION_BRANCH, VERSION_HASH, VERSION_CHANNEL, msg[0] ? msg : "Not checked yet.");

	return g_UpdateText;
}

/**
 * The one row, whose text says what pressing it does now.
 */
static char *menutextUpdateAction(struct menuitem *item)
{
	u32 done;
	u32 total;

	switch (updateGetState()) {
	case UPDATE_BUSY:
		updateGetProgress(&done, &total);

		// Sixteen megabytes is long enough that a row which only says it is
		// working looks the same as one that has stopped. Megabytes rather
		// than a percentage because the number a player wants when it is slow
		// is how much is left, and because the total is worth seeing before
		// deciding to wait for it.
		if (total > 0 && done > 0) {
			snprintf(g_UpdateText, sizeof(g_UpdateText), "Downloading... %u.%u of %u.%u MB\n",
					done / 1048576, (done % 1048576) * 10 / 1048576,
					total / 1048576, (total % 1048576) * 10 / 1048576);

			return g_UpdateText;
		}

		return "Working...\n";
	case UPDATE_FOUND:
		snprintf(g_UpdateText, sizeof(g_UpdateText), "Download and Install %s\n", updateGetVersion());
		return g_UpdateText;
	case UPDATE_STAGED:
		return "Restart Now\n";
	default:
		break;
	}

	return "Check Now\n";
}

static MenuItemHandlerResult menuhandlerUpdateAction(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		// Only while the worker has the job. Once the new build is in place
		// the row is the way out to it rather than something spent.
		return !updateIsAvailable() || updateGetState() == UPDATE_BUSY;
	}

	if (operation == MENUOP_SET) {
		switch (updateGetState()) {
		case UPDATE_FOUND:
			updateInstall();
			break;
		case UPDATE_STAGED:
			// The new build is already where this one was, and cleanup() hands
			// over to it on the way out - so restarting is quitting, and this
			// is the same exit() the Exit Game item calls. Going through the
			// ordinary shutdown is the point: the config, the binds and an
			// unfinished recording all get written before anything starts
			// again.
			exit(0);
			break;
		default:
			updateCheck();
			break;
		}
	}

	return 0;
}

/**
 * Ask for the release again even though this build already is it.
 *
 * A testing door, and it is in the menu rather than behind a command line flag
 * because what it is for is trying the download on the machine that has the
 * problem. Pressing it arms the next check rather than starting anything, so
 * what runs afterwards is the ordinary path with nothing skipped.
 */
static char *menutextUpdateAgain(struct menuitem *item)
{
	return updateIsForced() ? "Re-Download Armed\n" : "Re-Download Update\n";
}

static MenuItemHandlerResult menuhandlerUpdateAgain(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		s32 state = updateGetState();

		return !updateIsAvailable() || state == UPDATE_BUSY || state == UPDATE_STAGED;
	}

	if (operation == MENUOP_SET) {
		updateForceRedownload();
		updateCheck();
	}

	return 0;
}

/**
 * A bar for the download, drawn rather than written.
 *
 * Sixteen megabytes on a slow line is long enough that a number changing once
 * a second is not obviously progress, and the menu's own MENUITEMTYPE_METER is
 * no help - it is the pak repair meter, a fixed nine units wide with no handler
 * behind it.
 *
 * It goes in the blank row at the bottom of the page, which is there for it and
 * is otherwise an empty line nobody notices. Drawn after the dialogs, from the
 * same place in menuRenderDialogs() the Ghost Trials windows are drawn from,
 * because a menu item cannot draw and this is not one.
 */
#define UPDATE_BARINSET  8
#define UPDATE_BARHEIGHT 5

Gfx *updatemenuRenderProgress(Gfx *gdl)
{
	const struct menucolourpalette *colours = &g_MenuColours[MENUDIALOGTYPE_DEFAULT];
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;
	s32 viewleft = viGetViewLeft() / g_ScaleX;
	s32 viewtop = viGetViewTop();
	u32 done;
	u32 total;
	s32 x1;
	s32 x2;
	s32 y1;
	s32 y2;
	s32 fill;

	if (dialog == NULL || dialog->definition != &g_UpdateMenuDialog) {
		return gdl;
	}

	if (updateGetState() != UPDATE_BUSY) {
		return gdl;
	}

	updateGetProgress(&done, &total);

	if (total == 0) {
		// Checking rather than downloading: there is no length to draw yet,
		// and a bar that sits empty says the wrong thing about a request that
		// takes a moment.
		return gdl;
	}

	if (done > total) {
		done = total;
	}

	x1 = dialog->x + UPDATE_BARINSET;
	x2 = dialog->x + dialog->width - UPDATE_BARINSET;
	y2 = dialog->y + dialog->height - 4;
	y1 = y2 - UPDATE_BARHEIGHT;

	// Worked out in the wide type before it is cut down, because sixteen
	// million times a width overflows a u32 at about four thousand.
	fill = (s32)((u64)(x2 - x1) * done / total);

	g_MenuScissorX1 = viewleft;
	g_MenuScissorY1 = viewtop;
	g_MenuScissorX2 = (viGetViewLeft() + viGetViewWidth()) / g_ScaleX;
	g_MenuScissorY2 = viewtop + viGetViewHeight();
	gdl = menuApplyScissor(gdl);

	// The track, then what has arrived over the top of it. The colours are the
	// menu's own - a focused row for the part that is done - and the track is
	// the same colour at a quarter of the alpha, so the two read as one bar
	// filling rather than as two bars side by side.
	gdl = menugfxDrawFilledRect(gdl, x1, y1, x2, y2,
			(colours->item_unfocused & 0xffffff00) | 0x40,
			(colours->item_unfocused & 0xffffff00) | 0x40);

	if (fill > 0) {
		gdl = menugfxDrawFilledRect(gdl, x1, y1, x1 + fill, y2,
				colours->item_focused_inner, colours->item_focused_outer);
	}

	return gdl;
}

struct menuitem g_UpdateMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextUpdateStatus,
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
		0,
		(uintptr_t)&menutextUpdateAction,
		0,
		menuhandlerUpdateAction,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)&menutextUpdateAgain,
		0,
		menuhandlerUpdateAgain,
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
	{
		// The row the progress bar is drawn in. Empty the rest of the time,
		// which costs one line of an otherwise short page and is the only way
		// to reserve the space: the bar is not a menu item and the menu will
		// not leave room for something it does not know about.
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)" \n",
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_UpdateMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Check for Updates",
	g_UpdateMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
