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
#include "patchnotes.h"
#include "versioninfo.h"
#include "langpack.h"
#include "config.h"
#include "fs.h"
#include "system.h"
#include "gexfront.h"
#include <time.h>

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

static char g_UpdateText[320];
static char g_SwitchBackup[160];
static bool g_SwitchBackupOk;
// See menudialogUpdate().
static bool g_UpdateFocusAction = false;

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

	snprintf(g_UpdateText, sizeof(g_UpdateText), langTr("This build: %s %s (%s)\n%s\n"),
			VERSION_BRANCH, VERSION_HASH, VERSION_CHANNEL, langTr(msg[0] ? msg : "Not checked yet."));

	if (g_SwitchBackup[0]) {
		u32 len = strlen(g_UpdateText);

		snprintf(g_UpdateText + len, sizeof(g_UpdateText) - len,
				langTr(g_SwitchBackupOk ? "Saves backed up to %s\n" : "Could not back up to %s - nothing was changed.\n"),
				g_SwitchBackup);
	}

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
			snprintf(g_UpdateText, sizeof(g_UpdateText), langTr("Downloading... %u.%u of %u.%u MB\n"),
					done / 1048576, (done % 1048576) * 10 / 1048576,
					total / 1048576, (total % 1048576) * 10 / 1048576);

			return g_UpdateText;
		}

		return (char *)langTr("Working...\n");
	case UPDATE_FOUND:
		snprintf(g_UpdateText, sizeof(g_UpdateText), langTr("Download and Install %s\n"), updateGetVersion());
		return g_UpdateText;
	case UPDATE_STAGED:
		return (char *)langTr("Restart Now\n");
	default:
		break;
	}

	return (char *)langTr("Check Now\n");
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
			// The row is disabled while the check runs, which moves the
			// focus off it; put it back once there is an answer.
			g_UpdateFocusAction = true;
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
	return (char *)langTr(updateIsForced() ? "Re-Download Armed\n" : "Re-Download Update\n");
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
 * The patch notes: what the release found brings, or what this build has.
 *
 * One row whose text says which, the way the row above says what pressing it
 * does. The count is how many fixes the release has that this build does not,
 * which is the number worth seeing before deciding whether to install it.
 */
static char *menutextUpdateNotes(struct menuitem *item)
{
	static char text[48];
	s32 count = patchnotesCountForUpdate();

	if (count > 0) {
		snprintf(text, sizeof(text), langTr("What's in the Update (%d)\n"), count);
		return text;
	}

	return (char *)langTr("What's New in This Build\n");
}

static MenuItemHandlerResult menuhandlerUpdateNotes(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		patchnotesOpenForUpdate();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerUpdateNotesPopup(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return patchnotesPopupIsEnabled();
	case MENUOP_SET:
		patchnotesPopupSetEnabled(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerUpdateNoticeEnabled(s32 operation, struct menuitem *item, union handlerdata *data);

/**
 * The two channels, side by side: what the last check found on each.
 *
 * "patch N" is the newest entry in that build's patch notes, which is what puts
 * a dev build and a stable release in order - both are numbered from the same
 * file - and the date is that entry's.
 */
static const char *updatemenuChannelName(const char *channel)
{
	return strcmp(channel, "stable") == 0 ? langTr("Stable") : langTr("Dev");
}

static void updatemenuDescribe(char *out, u32 outsize, const char *version, s32 notes, const char *date)
{
	if (notes > 0) {
		snprintf(out, outsize, langTr("%s, patch %d%s%s"), version, notes, date[0] ? " - " : "", date);
	} else {
		snprintf(out, outsize, "%s", version);
	}
}

static char *menutextUpdateChannels(struct menuitem *item)
{
	static char text[256];
	char mine[96];
	char theirs[96];
	struct updaterel other;
	s32 state = updateGetState();
	bool checked = state == UPDATE_FOUND || state == UPDATE_CURRENT || state == UPDATE_STAGED;
	s32 num;
	char date[16];

	if (checked) {
		updateGetNotesNewest(&num, date, sizeof(date));
		updatemenuDescribe(mine, sizeof(mine), updateGetVersion(), num, date);
	} else {
		snprintf(mine, sizeof(mine), "%s", langTr("not checked"));
	}

	if (updateGetOther(&other)) {
		updatemenuDescribe(theirs, sizeof(theirs), other.version, other.notesnum, other.notesdate);
	} else {
		snprintf(theirs, sizeof(theirs), "%s", langTr(updateOtherWasAsked() && state != UPDATE_BUSY ? "not found" : "not checked"));
	}

	snprintf(text, sizeof(text), langTr("%s (yours): %s\n%s: %s\n"),
			updatemenuChannelName(VERSION_CHANNEL), mine,
			updatemenuChannelName(updateOtherChannel()), theirs);

	return text;
}

extern struct menudialogdef g_UpdateSwitchMenuDialog;

// The first patch notes entry whose builds have the switch row. Set to the
// entry this feature ships in; see menutextUpdateSwitchInfo().
#define UPDATE_SWITCH_FIRST_NOTES 20

static char *menutextUpdateSwitch(struct menuitem *item)
{
	static char text[80];
	struct updaterel other;

	if (updateGetOther(&other)) {
		snprintf(text, sizeof(text), langTr(strcmp(updateOtherChannel(), "stable") == 0
				? "Switch to Stable (%s)\n" : "Switch to Dev (%s)\n"), other.version);
		return text;
	}

	return (char *)langTr(strcmp(updateOtherChannel(), "stable") == 0 ? "Switch to Stable\n" : "Switch to Dev\n");
}

static MenuItemHandlerResult menuhandlerUpdateSwitch(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		s32 state = updateGetState();

		return !updateGetOther(NULL) || state == UPDATE_BUSY || state == UPDATE_STAGED;
	}

	if (operation == MENUOP_SET) {
		menuPushDialog(&g_UpdateSwitchMenuDialog);
	}

	return 0;
}

/**
 * The confirm page for a switch. Says which build replaces which, whether it is
 * older - a downgrade, by the patch notes numbers - and what that costs.
 */
static char *menutextUpdateSwitchInfo(struct menuitem *item)
{
	static char text[700];
	struct updaterel other;
	s32 mine = patchnotesBuildNumber();
	u32 len;

	if (!updateGetOther(&other)) {
		return "\n";
	}

	len = snprintf(text, sizeof(text), langTr("Replace this build (%s %s, patch %d)\nwith %s from the %s channel.\n"),
			updatemenuChannelName(VERSION_CHANNEL), VERSION_HASH, mine, other.version,
			updatemenuChannelName(updateOtherChannel()));

	if (len < sizeof(text)) {
		len += snprintf(text + len, sizeof(text) - len, "%s", strcmp(updateOtherChannel(), "stable") == 0
				? langTr("\nStable is the safe fallback, but it may still\nhave bugs that Dev has already fixed.\n")
				: langTr("\nDev is newer and gets fixes first. Stable is\nalways here to switch back to.\n"));
	}

	if (len < sizeof(text)) {
		if (other.notesnum > 0 && other.notesnum < mine) {
			len += snprintf(text + len, sizeof(text) - len,
					langTr("\nThis is a DOWNGRADE: %s is older\n(patch %d). Settings added in the newer build\nmay reset, and saves it wrote may not load.\n"),
					other.version, other.notesnum);
		} else if (other.notesnum <= 0) {
			len += snprintf(text + len, sizeof(text) - len, "%s",
					langTr("\nIts age is unknown: it may be older than yours.\nSettings added in a newer build may reset.\n"));
		}
	}

	// Builds before this page existed have no switch row, so from one of them
	// the only way back is the file kept as .prev. UPDATE_SWITCH_FIRST_NOTES is
	// the patch notes entry the switch shipped in.
	if (len < sizeof(text) && other.notesnum > 0 && other.notesnum < UPDATE_SWITCH_FIRST_NOTES) {
		len += snprintf(text + len, sizeof(text) - len, "%s",
				langTr("\nThat build has no switch back: to return, put\nthe .prev file back in place of the game.\n"));
	}

	if (len < sizeof(text)) {
		snprintf(text + len, sizeof(text) - len, "%s",
				langTr("\npd.ini and your saves are copied to backups/\nfirst, and this build is kept beside the new\none with .prev on the end of its name.\n"));
	}

	return text;
}

static MenuItemHandlerResult menuhandlerUpdateSwitchConfirm(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_SwitchBackupOk = updateBackupSaves(g_SwitchBackup, sizeof(g_SwitchBackup));

		if (!g_SwitchBackupOk) {
			// Nothing is replaced when the copy could not be made: the whole
			// point of it is having something to go back to.
			sysLogPrintf(LOG_ERROR, "update: switch cancelled, backup to %s failed", g_SwitchBackup);
		} else {
			updateSwitchChannel();
			g_UpdateFocusAction = true;
		}

		menuPopDialog();
	}

	return 0;
}

struct menuitem g_UpdateSwitchMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextUpdateSwitchInfo,
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
		// Cancel first, so a press that was meant for the page behind does
		// nothing.
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Cancel\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Back Up, Download and Install\n",
		0,
		menuhandlerUpdateSwitchConfirm,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_UpdateSwitchMenuDialog = {
	MENUDIALOGTYPE_DANGER,
	(uintptr_t)"Switch Channel",
	g_UpdateSwitchMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

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
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextUpdateChannels,
		0,
		NULL,
	},
	{
		// What the two channels are for, in the owner's words. Kept to lines
		// that fit the page at 640x480.
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Dev gets fixes first and likely fixes bugs that\nStable still has. Stable is the safe fallback:\nif Dev won't start or misbehaves, use Stable.\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)&menutextUpdateSwitch,
		0,
		menuhandlerUpdateSwitch,
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
		(uintptr_t)&menutextUpdateNotes,
		0,
		menuhandlerUpdateNotes,
	},
	{
		// Mod.PatchNotesPopup. Here rather than on an options page because
		// this is where a player looking at patch notes already is.
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Show What's New After Updating",
		0,
		menuhandlerUpdateNotesPopup,
	},
	{
		// Mod.UpdateNotice, beside the other "tell me about updates" switch.
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Show Update Notices",
		0,
		menuhandlerUpdateNoticeEnabled,
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

/**
 * Set by the update notice's Update Now, which opens this page and asks again
 * straight away. The rows are disabled while that runs, so the menu moves the
 * focus down past them; this puts it back on the action row once the answer is
 * in, which is the row the player came here to press.
 */
static MenuDialogHandlerResult menudialogUpdate(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;

	if (operation == MENUOP_TICK && g_UpdateFocusAction && dialog && dialog->definition == dialogdef
			&& updateGetState() != UPDATE_BUSY) {
		g_UpdateFocusAction = false;
		dialog->focuseditem = &g_UpdateMenuItems[2];
	}

	return 0;
}

struct menudialogdef g_UpdateMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Check for Updates",
	g_UpdateMenuItems,
	menudialogUpdate,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * The update notice: "a newer build is out" on the Perfect Menu, unasked.
 *
 * Check for Updates only answers a player who goes and asks, and most never
 * do. This asks once at startup, on the worker the page already uses, and when
 * the answer is a newer build on this build's channel it puts a small dialog
 * over the Perfect Menu: go to the page, not now, not this version, or never.
 *
 * On by default, like the patch notes popup and for the same reason: it is
 * about the mod rather than the game, and changes nothing about how either
 * plays. Everything about it stays out of the way:
 *
 * - Only this build's own channel, ever (the owner's rule): a stable player
 *   hears about new stable releases and a dev player about new dev builds.
 *   The other channel is only the switch row on the Check for Updates page,
 *   and the startup check never even asks for it. After a switch the new
 *   build follows its own channel, and a skipped version is kept per channel.
 * - The request is on the updater's worker thread, started after the window is
 *   up, so boot never waits on it; a failure is silent (updateCheckInBackground())
 *   and quitting does not wait for it either.
 * - A successful answer is kept in pd.ini with the time and the build that
 *   asked, and a start within UPDATENOTICE_INTERVAL of it reuses the answer
 *   instead of asking GitHub again. A different build (the player updated, or
 *   built their own) always asks afresh.
 * - The dialog only opens from the Perfect Menu's own tick while it is the
 *   dialog on top - never in a mission, never over the patch notes popup,
 *   which gets there first - and only once the player has left the controls
 *   alone for a moment, so a press meant for the menu is never taken by a
 *   dialog that appeared under it.
 * - Not on the first start of a fresh install, not under automation (a boot
 *   into a stage, fixed-step runs, the offscreen video driver) or with
 *   --no-update-notice.
 *
 * PD_UPDATE_NOTICE_FAKE=<version> in the environment pretends the check found
 * that version, without asking anybody and without touching the cache. It is
 * the way to see the dialog without cutting a release, and it overrides the
 * automation guard so it can be tested headless. PD_UPDATE_NOTICE_FORCE=1
 * overrides the guard for the real check (with Mod.UpdateServer pointed at a
 * test server), and PD_NO_UPDATE_NOTICE=1 is the guard by hand.
 */

#define UPDATENOTICE_INTERVAL (6 * 60 * 60) // seconds between real checks
#define UPDATENOTICE_IDLE     45            // 60ths of quiet before it may open

// Mod.UpdateNotice: whether to check and tell at all.
static s32 g_NoticeEnabled = 1;
// Mod.UpdateNoticeSkipDev / Mod.UpdateNoticeSkipStable: the version "Skip This
// Version" was pressed on, one per channel, so a skip made on one channel
// says nothing about the other after a switch. g_NoticeSkip is this build's.
static char g_NoticeSkipDev[UPDATE_MAXVERSION + 1] = "";
static char g_NoticeSkipStable[UPDATE_MAXVERSION + 1] = "";
#define g_NoticeSkip (strcmp(VERSION_CHANNEL, "stable") == 0 ? g_NoticeSkipStable : g_NoticeSkipDev)
// Mod.UpdateLastCheck/Build/Version/Commit: the last answer, see above.
static s32 g_NoticeLastCheck = 0;
// "<hash>-<channel>": a stable tag cut on the same commit as a dev build
// follows another channel and must not reuse its answer.
static char g_NoticeLastBuild[UPDATE_MAXCOMMIT + 16] = "";
static char g_NoticeLastVersion[UPDATE_MAXVERSION + 1] = "";
static char g_NoticeLastCommit[UPDATE_MAXCOMMIT + 1] = "";

#define NOTICE_OFF      0 // not this run
#define NOTICE_ASKING   1 // the background check is out
#define NOTICE_READY    2 // a newer build is known: waiting for the menu
#define NOTICE_DONE     3 // shown, or nothing to show

static s32 g_NoticeState = NOTICE_OFF;
static bool g_NoticeFreshInstall = false;
static bool g_NoticeFake = false;
static char g_NoticeVersion[UPDATE_MAXVERSION + 1] = "";
static s32 g_NoticeIdle = 0;
static char g_NoticeText[192];

static bool updatenoticeWanted(void)
{
	const char *driver = getenv("SDL_VIDEODRIVER");

	if (!g_NoticeEnabled || !updateIsAvailable()) {
		return false;
	}

	if (g_NoticeFake || getenv("PD_UPDATE_NOTICE_FORCE")) {
		return true;
	}

	if (g_NoticeFreshInstall || getenv("PD_NO_UPDATE_NOTICE")
			|| sysArgCheck("--no-update-notice") || sysArgCheck("--boot-stage")
			|| sysArgCheck("--fixed-step") || sysArgCheck("--exit-frame")
			|| (driver && strcmp(driver, "offscreen") == 0)) {
		return false;
	}

	return true;
}

/**
 * The same comparison updateIsNewer() makes: a release is "newer" when it is a
 * different commit on this build's channel, compared to the shorter hash.
 */
static bool updatenoticeDiffers(const char *commit)
{
	u32 mine = (u32)strlen(VERSION_HASH);
	u32 theirs = (u32)strlen(commit);

	return commit[0] && strncmp(commit, VERSION_HASH, mine < theirs ? mine : theirs) != 0;
}

static void updatenoticeFound(const char *version, const char *commit)
{
	if (!updatenoticeDiffers(commit) || version[0] == '\0') {
		g_NoticeState = NOTICE_DONE;
		return;
	}

	if (strcmp(version, g_NoticeSkip) == 0) {
		sysLogPrintf(LOG_NOTE, "updatenotice: %s is out, skipped by the player", version);
		g_NoticeState = NOTICE_DONE;
		return;
	}

	snprintf(g_NoticeVersion, sizeof(g_NoticeVersion), "%s", version);
	g_NoticeState = NOTICE_READY;
	sysLogPrintf(LOG_NOTE, "updatenotice: %s is out (this is %s)", version, VERSION_HASH);
}

/**
 * Straight after configInit(), with patchnotesInit(): whether pd.ini was there
 * when the game started, before anything has had a chance to write one.
 */
void updatenoticeInit(void)
{
	const char *fake = getenv("PD_UPDATE_NOTICE_FAKE");

	g_NoticeFreshInstall = fsFileSize(CONFIG_PATH) <= 0;

	if (fake && fake[0]) {
		g_NoticeFake = true;
		snprintf(g_NoticeVersion, sizeof(g_NoticeVersion), "%s", fake);
	}
}

/**
 * After updateInit(): answer from the cache, or start the background check.
 */
void updatenoticeStart(void)
{
	s32 now = (s32)time(NULL);

	if (!updatenoticeWanted()) {
		return;
	}

	if (g_NoticeFake) {
		char version[UPDATE_MAXVERSION + 1];

		snprintf(version, sizeof(version), "%s", g_NoticeVersion);
		updatenoticeFound(version, "fffffff");
		return;
	}

	if (g_NoticeLastCheck > 0 && now >= g_NoticeLastCheck
			&& now - g_NoticeLastCheck < UPDATENOTICE_INTERVAL
			&& strcmp(g_NoticeLastBuild, VERSION_HASH "-" VERSION_CHANNEL) == 0) {
		sysLogPrintf(LOG_NOTE, "updatenotice: using the answer from %d min ago", (now - g_NoticeLastCheck) / 60);
		updatenoticeFound(g_NoticeLastVersion, g_NoticeLastCommit);
		return;
	}

	g_NoticeState = NOTICE_ASKING;
	updateCheckInBackground();

	if (updateGetState() != UPDATE_BUSY) {
		g_NoticeState = NOTICE_DONE;
	}
}

/**
 * The background check's answer, once it has one. Written to pd.ini straight
 * away, so a crash or a kill before exit does not ask again next start.
 */
static void updatenoticePoll(void)
{
	s32 state;

	if (g_NoticeState != NOTICE_ASKING) {
		return;
	}

	state = updateGetState();

	if (state == UPDATE_BUSY) {
		return;
	}

	if (state != UPDATE_FOUND && state != UPDATE_CURRENT) {
		// No answer: no network, a timeout, a release without a manifest.
		// Not cached, so the next start asks again.
		g_NoticeState = NOTICE_DONE;
		return;
	}

	g_NoticeLastCheck = (s32)time(NULL);
	snprintf(g_NoticeLastBuild, sizeof(g_NoticeLastBuild), "%s", VERSION_HASH "-" VERSION_CHANNEL);

	if (state == UPDATE_FOUND) {
		snprintf(g_NoticeLastVersion, sizeof(g_NoticeLastVersion), "%s", updateGetVersion());
		snprintf(g_NoticeLastCommit, sizeof(g_NoticeLastCommit), "%s", updateGetCommit());
	} else {
		g_NoticeLastVersion[0] = '\0';
		g_NoticeLastCommit[0] = '\0';
	}

	configSave(CONFIG_PATH);

	if (state == UPDATE_FOUND && !updateIsForced()) {
		updatenoticeFound(g_NoticeLastVersion, g_NoticeLastCommit);
	} else {
		g_NoticeState = NOTICE_DONE;
	}
}

static bool updatenoticeInputIdle(struct menuinputs *inputs)
{
	if (inputs == NULL) {
		return true;
	}

	return inputs->leftright == 0 && inputs->updown == 0
		&& inputs->select == 0 && inputs->back == 0 && inputs->start == 0
		&& inputs->shoulder == 0
		&& inputs->xaxis > -20 && inputs->xaxis < 20
		&& inputs->yaxis > -20 && inputs->yaxis < 20
		&& inputs->mouseheld == 0 && inputs->mousemoved == 0 && inputs->mousescroll == 0;
}

extern struct menudialogdef g_UpdateNoticeMenuDialog;

/**
 * From the Perfect Menu's tick, every frame it is open, whether it is the dialog
 * on top or not (`ontop`). After patchnotesMainMenuTick(), which may have just
 * put its popup over the menu; this waits until that is closed again.
 */
void updatenoticeMainMenuTick(struct menudialogdef *menudef, struct menuinputs *inputs, bool ontop)
{
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;

	updatenoticePoll();

	if (!ontop || dialog == NULL || dialog->definition != menudef
			|| dialog->state != MENUDIALOGSTATE_POPULATED
			|| g_Vars.stagenum != STAGE_CITRAINING || gexFrontIsActive()) {
		g_NoticeIdle = 0;
		return;
	}

	if (!updatenoticeInputIdle(inputs)) {
		g_NoticeIdle = 0;
		return;
	}

	g_NoticeIdle += g_Vars.diffframe60;

	if (g_NoticeState != NOTICE_READY || g_NoticeIdle < UPDATENOTICE_IDLE) {
		return;
	}

	if (!g_NoticeEnabled) {
		// Switched off on the Check for Updates page while the answer was on
		// its way.
		g_NoticeState = NOTICE_DONE;
		return;
	}

	g_NoticeState = NOTICE_DONE;
	menuPushDialog(&g_UpdateNoticeMenuDialog);
}

static char *menutextUpdateNotice(struct menuitem *item)
{
	snprintf(g_NoticeText, sizeof(g_NoticeText), langTr("%s is out.\nThis build is %s (%s).\n"),
			g_NoticeVersion, VERSION_HASH, VERSION_CHANNEL);

	return g_NoticeText;
}

static MenuItemHandlerResult menuhandlerUpdateNoticeNow(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		s32 state = updateGetState();

		menuPopDialog();
		menuPushDialog(&g_UpdateMenuDialog);

		// Asked again now, whatever the startup check or the cache said: the
		// page's rows are then the real answer, and the startup check never
		// asks about the other channel, which the page also shows.
		if (state != UPDATE_BUSY && state != UPDATE_STAGED) {
			updateCheck();
		}

		g_UpdateFocusAction = true;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerUpdateNoticeSkip(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		snprintf(g_NoticeSkip, sizeof(g_NoticeSkipDev), "%s", g_NoticeVersion);
		configSave(CONFIG_PATH);
		sysLogPrintf(LOG_NOTE, "updatenotice: skipping %s", g_NoticeSkip);
		menuPopDialog();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerUpdateNoticeNever(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_NoticeEnabled = 0;
		configSave(CONFIG_PATH);
		sysLogPrintf(LOG_NOTE, "updatenotice: turned off");
		menuPopDialog();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerUpdateNoticeEnabled(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_NoticeEnabled != 0;
	case MENUOP_SET:
		g_NoticeEnabled = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

struct menuitem g_UpdateNoticeMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextUpdateNotice,
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
		(uintptr_t)"Update Now\n",
		0,
		menuhandlerUpdateNoticeNow,
	},
	{
		// Back does the same.
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Later\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Skip This Version\n",
		0,
		menuhandlerUpdateNoticeSkip,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Don't Show Update Notices\n",
		0,
		menuhandlerUpdateNoticeNever,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_UpdateNoticeMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Update Available",
	g_UpdateNoticeMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

PD_CONSTRUCTOR static void updatenoticeConfigInit(void)
{
	configRegisterInt("Mod.UpdateNotice", &g_NoticeEnabled, 0, 1);
	configRegisterString("Mod.UpdateNoticeSkipDev", g_NoticeSkipDev, sizeof(g_NoticeSkipDev));
	configRegisterString("Mod.UpdateNoticeSkipStable", g_NoticeSkipStable, sizeof(g_NoticeSkipStable));
	configRegisterInt("Mod.UpdateLastCheck", &g_NoticeLastCheck, 0, 0x7fffffff);
	configRegisterString("Mod.UpdateLastBuild", g_NoticeLastBuild, sizeof(g_NoticeLastBuild));
	configRegisterString("Mod.UpdateLastVersion", g_NoticeLastVersion, sizeof(g_NoticeLastVersion));
	configRegisterString("Mod.UpdateLastCommit", g_NoticeLastCommit, sizeof(g_NoticeLastCommit));
}
