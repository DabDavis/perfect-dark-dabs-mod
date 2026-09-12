/**
 * Send Crash Report, on the Perfect Menu beside Check for Updates.
 *
 * The dialog a crash puts up can send the report where it stands, and this is
 * the other half of that: the report a player pressed Close on, or one from a
 * crash that took the window with it and never showed a dialog at all. It is
 * also the only place a note can be typed, because a message box cannot take
 * text and the game's own keyboard is here.
 *
 * The send is on a worker, the way Check for Updates and the ghost menus do
 * theirs: the page is ticked from the render loop, and a request across the
 * internet is long enough that doing it inline would look like the game had
 * stopped.
 */

#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "bss.h"
#include "system.h"
#include "crashreport.h"

#define STATE_IDLE 0
#define STATE_BUSY 1
#define STATE_SENT 2
#define STATE_FAILED 3

static char g_Note[CRASHREPORT_MAXNOTE + 1];
static char g_Text[512];
static char g_Err[256];
static SDL_Thread *g_Thread;
static s32 g_State;

static int crashReportMenuWorker(void *arg)
{
	const char *path = crashReportPending();

	if (path == NULL) {
		snprintf(g_Err, sizeof(g_Err), "there is no report to send");
		g_State = STATE_FAILED;
		return 0;
	}

	g_State = crashReportSend(path, g_Note, g_Err, sizeof(g_Err))
		? STATE_SENT : STATE_FAILED;

	return 0;
}

/**
 * Which crash this is about, and what became of the last send.
 */
static char *menutextCrashReportStatus(struct menuitem *item)
{
	switch (g_State) {
	case STATE_BUSY:
		snprintf(g_Text, sizeof(g_Text), "Sending...\n");
		return g_Text;
	case STATE_SENT:
		snprintf(g_Text, sizeof(g_Text),
				"Sent. Thank you - that is the whole of what Dab gets to see of a crash.\n");
		return g_Text;
	case STATE_FAILED:
		snprintf(g_Text, sizeof(g_Text),
				"Not sent: %s\nThe report is still saved and this can be tried again.\n", g_Err);
		return g_Text;
	default:
		break;
	}

	if (crashReportPending() == NULL) {
		snprintf(g_Text, sizeof(g_Text),
				"Nothing to send - this game has not crashed.\n");
		return g_Text;
	}

	// What is in it, before it is offered: a player deciding whether to send
	// their log should be told that is what it is. Wrapped by hand and kept
	// short, because the dialog is only as wide as its widest line and the
	// menu does not fold one that runs past the edge.
	snprintf(g_Text, sizeof(g_Text),
			"A crash from %s is waiting.\n"
			"It holds the error and its stack, the\n"
			"build, your [Mod] settings and the last\n"
			"few hundred lines of the log - nothing\n"
			"else, and nothing unless you press Send.\n",
			crashReportPendingWhen());

	return g_Text;
}

/**
 * What the note says now, on the row that opens the keyboard.
 */
static char *menutextCrashReportNoteRow(struct menuitem *item)
{
	if (g_Note[0]) {
		snprintf(g_Text, sizeof(g_Text), "Note: %s\n", g_Note);
	} else {
		snprintf(g_Text, sizeof(g_Text), "Add a Note (optional)\n");
	}

	return g_Text;
}

static MenuItemHandlerResult menuhandlerCrashReportNote(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		snprintf(data->keyboard.string, MPSETUP_MAXNAME + 1, "%s", g_Note);
		break;
	case MENUOP_SETTEXT:
		snprintf(g_Note, sizeof(g_Note), "%s", data->keyboard.string);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrashReportSend(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return crashReportPending() == NULL || !crashReportCanSend()
			|| g_State == STATE_BUSY || g_State == STATE_SENT;
	}

	if (operation == MENUOP_SET) {
		if (g_Thread) {
			SDL_WaitThread(g_Thread, NULL);
			g_Thread = NULL;
		}

		g_Err[0] = '\0';
		g_State = STATE_BUSY;
		g_Thread = SDL_CreateThread(crashReportMenuWorker, "pdcrashreport", NULL);

		if (g_Thread == NULL) {
			snprintf(g_Err, sizeof(g_Err), "could not start the send");
			g_State = STATE_FAILED;
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrashReportDelete(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return crashReportPending() == NULL || g_State == STATE_BUSY;
	}

	if (operation == MENUOP_SET) {
		crashReportDiscard();
		g_State = STATE_IDLE;
		g_Note[0] = '\0';
	}

	return 0;
}

/**
 * The row on the Perfect Menu, which says whether there is anything to send.
 *
 * Always there rather than appearing after a crash: a page nobody can find
 * until the day they need it is a page nobody knows to look for, and the row
 * costs a line of a menu that already has Check for Updates on it.
 */
char *menutextCrashReportRow(struct menuitem *item)
{
	return crashReportPending() ? "Send Crash Report" : "Crash Reports";
}

/**
 * The note has a page of its own, because the game's on screen keyboard is
 * tall: on one page with the rest it pushed Send and Delete off the bottom and
 * drew them over each other. One keyboard per page is how the ghost account
 * name and the file manager's rename do it, for the same reason.
 */
struct menuitem g_CrashReportNoteMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)"A line about what you were doing.\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_KEYBOARD,
		0,
		0,
		0,
		0,
		menuhandlerCrashReportNote,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_CrashReportNoteMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Crash Report Note",
	g_CrashReportNoteMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_CrashReportMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextCrashReportStatus,
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
		(uintptr_t)&menutextCrashReportNoteRow,
		0,
		(void *)&g_CrashReportNoteMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Send to Dab\n",
		0,
		menuhandlerCrashReportSend,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Delete Report\n",
		0,
		menuhandlerCrashReportDelete,
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

struct menudialogdef g_CrashReportMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Crash Reports",
	g_CrashReportMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
