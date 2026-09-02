#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "bss.h"
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
	switch (updateGetState()) {
	case UPDATE_BUSY:
		return "Working...\n";
	case UPDATE_FOUND:
		snprintf(g_UpdateText, sizeof(g_UpdateText), "Download and Install %s\n", updateGetVersion());
		return g_UpdateText;
	case UPDATE_STAGED:
		return "Installed - restart the game\n";
	default:
		break;
	}

	return "Check Now\n";
}

static MenuItemHandlerResult menuhandlerUpdateAction(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		// Nothing to press while the worker has the job, and nothing left to
		// press once the new build is in place - the only thing that finishes
		// that is quitting.
		s32 state = updateGetState();

		return !updateIsAvailable() || state == UPDATE_BUSY || state == UPDATE_STAGED;
	}

	if (operation == MENUOP_SET) {
		if (updateGetState() == UPDATE_FOUND) {
			updateInstall();
		} else {
			updateCheck();
		}
	}

	return 0;
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

struct menudialogdef g_UpdateMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Check for Updates",
	g_UpdateMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
