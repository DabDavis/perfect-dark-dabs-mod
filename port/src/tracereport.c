/**
 * Report a Problem: the F3 trace, with a note, sent to Dab.
 *
 * F3 has always written a state dump and a screenshot of the frame on screen,
 * which is everything needed to debug "the guard went invisible" except the
 * one thing a tester had to do by hand: get both files, and a sentence about
 * what was wrong, to somebody who can read them. This is that step. The key
 * still writes both files first; then a dialog comes up over the game (paused,
 * in a solo mission) with the keyboard already typing into a note, and Enter
 * or Send puts the dump, the note, a scaled copy of the picture, the [Mod]
 * settings and the log tail on the wire to the server's /report.
 *
 * A report also carries an optional name, which is the only thing that can
 * credit the person who sent it: nothing else in a report identifies anybody,
 * on purpose. It is typed once and kept in pd.ini as Mod.ReportName, so a
 * tester who fills it in is credited for every report after it as well. TAB,
 * the arrow keys or a click move the typing between the note and the name, and
 * the name is written to pd.ini the moment it is finished.
 *
 * Nothing is sent unless the player presses Send, the same as a crash report,
 * and Close leaves the files in traces/ where they always were.
 *
 * The dialog is opened from lvTick() rather than from the key's own tick,
 * because pushing a menu is game state and the key is read in the scheduler.
 * The picture is taken before the dialog exists, so the dialog is never in it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "constants.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "game/lv.h"
#include "bss.h"
#include "gexfront.h"
#include "geintro.h"
#include "gewatch.h"
#include "config.h"
#include "fs.h"
#include "input.h"
#include "system.h"
#include "crashreport.h"
#include "ghostnet.h"
#include "versioninfo.h"
#include "trace.h"

#define STATE_IDLE   0
#define STATE_BUSY   1
#define STATE_SENT   2
#define STATE_FAILED 3

// What a send may carry. The dump of a busy stage is tens of kilobytes; this
// is what the server keeps of one, so nothing past it would be read anyway.
#define TRACEREPORT_MAXTEXT (480 * 1024)
#define TRACEREPORT_MAXSHOT (4 * 1024 * 1024)
// The width a line of the note is folded at on the page.
#define TRACEREPORT_WRAP 44

extern s32 g_MenuKeyboardPlayer;
extern struct menudialogdef g_TraceReportMenuDialog;
extern struct menuitem g_TraceReportMenuItems[];

static s32 g_Enabled = 1;

static char g_TracePath[FS_MAXPATH + 1];
static char g_ShotPath[FS_MAXPATH + 1];
static char g_When[32];
static bool g_OfferPending;
// The stage F3 was pressed on. An offer carried into another stage (from the
// title's logos, which have no menus) opens over that stage's menus only.
static s32 g_OfferStage;
static bool g_Open;
// The menu (g_Menus[] index) the dialog is on while open
static s32 g_OpenMenu;
// Frames after the dialog closed in which the screen under it still keeps its
// hands off the pads: GE Plus's folder and watch read presses by the frame, and
// the press that closed the dialog is still "this frame's" when they tick next.
static s32 g_HoldFrames;
#define TRACEREPORT_HOLD_FRAMES 3

static char g_Note[TRACEREPORT_MAXNOTE + 1];
// Kept across reports and across runs, where the note is not: a name is who
// the player is, and asking for it once is the point of it. This is the name
// pd.ini holds; typing goes into g_NameEdit and only a finished name comes
// back here, so a settings save on the way out (or a crash) never writes a
// half-typed or backspaced-away name over a good one.
static char g_Name[TRACEREPORT_MAXNAME + 1];
static char g_NameEdit[TRACEREPORT_MAXNAME + 1];
// Whether g_NameEdit is being typed and is not yet back in g_Name
static bool g_NameOpen;
#define TRACEREPORT_FIELD_NOTE 0
#define TRACEREPORT_FIELD_NAME 1
static s32 g_Field;
// A press the dialog acted on while typing, which it waits to see let go of
// before it gives the pads back (TRACEREPORT_ONRELEASE_*). The pads are blank
// while the keyboard types; handed back while the press was still down, it
// reads as a new press to the menu the next frame and does something else.
static s32 g_ReleaseKey;
static s32 g_ReleaseAction;
#define TRACEREPORT_ONRELEASE_STOP  1
#define TRACEREPORT_ONRELEASE_CLOSE 2
// A controller's button was pressed in the dialog, so the player may have no
// keyboard at hand: the name row says it needs one.
static bool g_PadUsed;
static char g_Text[TRACEREPORT_MAXNOTE * 2 + 512];
static char g_Err[256];
static SDL_Thread *g_Thread;
static volatile s32 g_State;
// When a sent report's dialog closes itself, in SDL ticks; 0 while not yet sent.
static u32 g_CloseAt;
#define TRACEREPORT_SENT_LINGER_MS 5000

s32 traceReportEnabled(void)
{
	return g_Enabled && crashReportCanSend();
}

s32 traceReportGetOffer(void)
{
	return g_Enabled;
}

void traceReportSetOffer(s32 offer)
{
	g_Enabled = !!offer;
}

void traceReportOffer(const char *tracepath, const char *shotpath)
{
	const time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	// A send still on the wire owns the paths it was given.
	if (g_State == STATE_BUSY) {
		return;
	}

	snprintf(g_TracePath, sizeof(g_TracePath), "%s", tracepath ? tracepath : "");
	snprintf(g_ShotPath, sizeof(g_ShotPath), "%s", shotpath ? shotpath : "");

	if (tm) {
		strftime(g_When, sizeof(g_When), "%H:%M:%S", tm);
	} else {
		g_When[0] = '\0';
	}

	// A new frame is a new report: the last one's note was about something else.
	// g_Name is not cleared - it is the same person reporting.
	g_Note[0] = '\0';
	g_Field = TRACEREPORT_FIELD_NOTE;
	g_Err[0] = '\0';
	g_State = STATE_IDLE;
	g_OfferStage = g_Vars.stagenum;
	g_OfferPending = g_TracePath[0] != '\0';
}

/**
 * The name, finished: trimmed, and written to pd.ini at once if it changed,
 * rather than when the game next saves its settings on the way out, which a
 * crash or a killed process never reaches.
 *
 * An empty or all-space field never replaces a kept name: it is a slip of the
 * backspace key, and the name comes back. A different name replaces it.
 */
static void traceReportKeepName(void)
{
	char name[TRACEREPORT_MAXNAME + 1];
	const char *start = g_NameEdit;
	u32 len;

	while (*start == ' ') {
		start++;
	}

	snprintf(name, sizeof(name), "%s", start);
	len = strlen(name);

	while (len > 0 && name[len - 1] == ' ') {
		name[--len] = '\0';
	}

	if (name[0] == '\0' || strcmp(name, g_Name) == 0) {
		snprintf(g_NameEdit, sizeof(g_NameEdit), "%s", g_Name);
		return;
	}

	snprintf(g_Name, sizeof(g_Name), "%s", name);
	snprintf(g_NameEdit, sizeof(g_NameEdit), "%s", g_Name);

	if (configSave(CONFIG_PATH)) {
		sysLogPrintf(LOG_NOTE, "trace: report name saved to " CONFIG_FNAME);
	} else {
		sysLogPrintf(LOG_WARNING, "trace: report name could not be saved to " CONFIG_FNAME);
	}
}

static void traceReportCloseName(void)
{
	if (g_NameOpen) {
		traceReportKeepName();
		g_NameOpen = false;
	}
}

/** Moves the typing to FIELD, finishing the name if it leaves it. */
static void traceReportSetField(s32 field)
{
	if (field == TRACEREPORT_FIELD_NAME) {
		if (!g_NameOpen) {
			snprintf(g_NameEdit, sizeof(g_NameEdit), "%s", g_Name);
			g_NameOpen = true;
		}
	} else {
		traceReportCloseName();
	}

	g_Field = field;
}

static void traceReportStartTyping(s32 field)
{
	traceReportSetField(field);
	g_ReleaseKey = 0;
	g_MenuKeyboardPlayer = g_MpPlayerNum;
	inputClearLastKey();
	inputClearLastTextChar();
	inputStartTextInput();
}

static void traceReportStopTyping(void)
{
	if (g_MenuKeyboardPlayer == g_MpPlayerNum) {
		g_MenuKeyboardPlayer = -1;
		inputStopTextInput();
	}
}

s32 traceReportIsOpen(void)
{
	return g_Open;
}

s32 traceReportHoldsInput(void)
{
	return g_Open || g_HoldFrames > 0;
}

/**
 * Called from lvTick() once a frame. Opens the dialog for a report F3 made,
 * once there is somewhere safe to open it.
 *
 * Over a menu that is already up it goes on top, and closing it goes back to
 * that menu. That includes the Perfect Menu under GE Plus's intro and folder
 * screens, which are drawn and ticked instead of the menus: menuTick() and
 * menuRender() hand the dialog the frame while it is open, over the folder.
 * GE Plus's watch is a pause with no menu at all; the dialog is pushed over it
 * as a root, the level already stopped, and the watch keeps the pause when it
 * closes (func0f0fa6ac()).
 *
 * In play it is pushed the way the game pushes its controller pak warnings
 * mid-mission: as a root dialog, pausing a one player game. A cutscene or a
 * pause menu (or the watch) on its way in or out waits, and so do the title
 * and its attract demo: F3 there opens over the menus the title leads to.
 */
void traceReportTick(void)
{
	const s32 prevplayernum = g_MpPlayerNum;

	// A root dialog pushed over the report (a match's end, a stage change)
	// throws it away without closing it. Without this the dialog would count
	// as open for good: F3 ignored and the keyboard still typing into it.
	if (g_Open && (g_Menus[g_OpenMenu].curdialog == NULL
				|| g_Menus[g_OpenMenu].curdialog->definition != &g_TraceReportMenuDialog)) {
		if (g_MenuKeyboardPlayer == g_OpenMenu) {
			g_MenuKeyboardPlayer = -1;
			inputStopTextInput();
		}

		traceReportCloseName();
		g_ReleaseKey = 0;
		g_Open = false;
		g_HoldFrames = TRACEREPORT_HOLD_FRAMES;
	}

	if (g_HoldFrames > 0 && !g_Open) {
		g_HoldFrames--;
	}

	if (!g_OfferPending || g_Open || !traceReportEnabled()) {
		return;
	}

	g_MpPlayerNum = 0;

	if (g_Menus[0].curdialog != NULL) {
		// A menu, the Perfect Menu under GE Plus's folder or intro included
		menuPushDialog(&g_TraceReportMenuDialog);
		g_OfferPending = false;
	} else if (gexFrontIsActive() || geIntroIsActive()) {
		// The folder or intro with nothing under it to go back to: it waits
	} else if (g_IsTitleDemo || g_Vars.stagenum != g_OfferStage) {
		// The title's attract demo, which any press ends, and a stage the
		// title went on to before it had a menu up: it waits for the menus
	} else if (STAGE_IS_LEVEL(g_Vars.stagenum)
			&& g_Vars.currentplayer && g_Vars.currentplayer->prop
			&& !g_Vars.in_cutscene
			&& g_Menus[0].openinhibit == 0
			&& geWatchIsOpen()) {
		// GE Plus's watch, once it is all the way up and the level stopped.
		// Its own root and no pause of the report's: the watch paused the
		// level and is still holding it when the report closes.
		if (geWatchIsSettled() && g_Vars.currentplayer->pausemode == PAUSEMODE_PAUSED) {
			g_Menus[0].playernum = 0;
			menuPushRootDialog(&g_TraceReportMenuDialog, MENUROOT_MAINMENU);
			g_OfferPending = false;
		}
	} else if (STAGE_IS_LEVEL(g_Vars.stagenum)
			&& g_Vars.currentplayer && g_Vars.currentplayer->prop
			&& !g_Vars.in_cutscene
			&& g_Menus[0].openinhibit == 0
			&& g_Vars.currentplayer->pausemode == PAUSEMODE_UNPAUSED) {
		// Opened the way Start opens this mode's own pause menu (bondmove.c).
		// A match is g_Vars.mplayerisrunning whatever PLAYERCOUNT() says: the
		// solo root pushed into a one-player Combat Simulator match closed to
		// a black screen that Start could not get out of, because the match
		// never unpauses a MENUROOT_MAINMENU pause.
		if (!g_Vars.mplayerisrunning) {
			g_Menus[0].playernum = 0;
			menuPushRootDialog(&g_TraceReportMenuDialog, MENUROOT_MAINMENU);
			lvSetPaused(true);
			g_Vars.currentplayer->pausemode = PAUSEMODE_PAUSED;
		} else {
			g_MpPlayerNum = g_Vars.currentplayerstats->mpindex;
			g_Menus[g_MpPlayerNum].playernum = g_Vars.currentplayernum;
			menuPushRootDialog(&g_TraceReportMenuDialog, MENUROOT_MPPAUSE);
		}

		g_OfferPending = false;
	}

	g_MpPlayerNum = prevplayernum;
}

/**
 * A whole file into a malloc()ed, NUL terminated buffer, cut at maxsize.
 * The paths here are real ones from fsFullPath(), not VFS names.
 */
static char *traceReportLoad(const char *path, u32 maxsize, u32 *outsize)
{
	FILE *f = fopen(path, "rb");
	char *data;
	u32 size;

	*outsize = 0;

	if (!f) {
		return NULL;
	}

	data = malloc(maxsize + 1);

	if (!data) {
		fclose(f);
		return NULL;
	}

	size = fread(data, 1, maxsize, f);
	fclose(f);
	data[size] = '\0';
	*outsize = size;

	return data;
}

static char *traceReportBase64(const u8 *data, u32 size)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *out = malloc((size + 2) / 3 * 4 + 1);
	char *o = out;
	u32 i;

	if (!out) {
		return NULL;
	}

	for (i = 0; i + 2 < size; i += 3) {
		const u32 v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		*o++ = digits[(v >> 18) & 63];
		*o++ = digits[(v >> 12) & 63];
		*o++ = digits[(v >> 6) & 63];
		*o++ = digits[v & 63];
	}

	if (i < size) {
		const u32 v = (data[i] << 16) | (i + 1 < size ? data[i + 1] << 8 : 0);
		*o++ = digits[(v >> 18) & 63];
		*o++ = digits[(v >> 12) & 63];
		*o++ = i + 1 < size ? digits[(v >> 6) & 63] : '=';
		*o++ = '=';
	}

	*o = '\0';

	return out;
}

/**
 * The file's own name. The full path is not sent: it carries the player's
 * account name on every desktop.
 */
static const char *traceReportBaseName(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *back = strrchr(path, '\\');

	if (back && (!slash || back > slash)) {
		slash = back;
	}

	return slash ? slash + 1 : path;
}

#ifdef PD_GHOST_NET

static bool traceReportSend(char *err, u32 errsize)
{
	struct ghostnetreq req;
	struct ghostnetbuf buf;
	char url[512];
	char header[TRACEREPORT_MAXNOTE + TRACEREPORT_MAXNAME + 512];
	char notebuf[TRACEREPORT_MAXNOTE * 6 + 8];
	char namebuf[TRACEREPORT_MAXNAME * 6 + 8];
	char *trace;
	char *shot = NULL;
	char *shot64 = NULL;
	char *text;
	char *escaped;
	char *body;
	u32 tracesize;
	u32 shotsize = 0;
	u32 textsize;
	u32 bodysize;
	s32 status = 0;
	u32 i;
	bool ok;

	trace = traceReportLoad(g_TracePath, TRACEREPORT_MAXTEXT, &tracesize);

	if (!trace || tracesize == 0) {
		snprintf(err, errsize, "the trace could not be read");
		free(trace);
		return false;
	}

	if (g_ShotPath[0]) {
		shot = traceReportLoad(g_ShotPath, TRACEREPORT_MAXSHOT, &shotsize);

		// A picture cut at the cap is not a picture. Better none than half.
		if (shot && shotsize > 0 && shotsize < TRACEREPORT_MAXSHOT) {
			shot64 = traceReportBase64((u8 *)shot, shotsize);
		}

		free(shot);
	}

	snprintf(header, sizeof(header),
			"Dab's Mod problem report\n"
			"version: %s\n"
			"channel: %s\n"
			"trace: %s\n"
			"name: %s\n"
			"note: %s\n\n"
			"--- trace ---\n",
			sysGetVersionString(), VERSION_CHANNEL, traceReportBaseName(g_TracePath),
			g_Name[0] ? g_Name : "-", g_Note[0] ? g_Note : "-");

	textsize = strlen(header) + tracesize;
	text = malloc(textsize + 1);
	escaped = malloc(textsize * 6 + 8);

	if (!text || !escaped) {
		snprintf(err, errsize, "not enough memory to send the report");
		free(text);
		free(escaped);
		free(trace);
		free(shot64);
		return false;
	}

	snprintf(text, textsize + 1, "%s%s", header, trace);
	free(trace);

	// JSON wants UTF-8 and the dump is meant to be ASCII; a stray high byte
	// from a model name would make the whole body unreadable to the server.
	for (i = 0; text[i]; i++) {
		if ((u8)text[i] >= 0x80) {
			text[i] = '?';
		}
	}

	ghostnetJsonEscape(text, escaped, textsize * 6 + 8);
	ghostnetJsonEscape(g_Note, notebuf, sizeof(notebuf));
	ghostnetJsonEscape(g_Name, namebuf, sizeof(namebuf));
	free(text);

	bodysize = strlen(escaped) + strlen(notebuf) + strlen(namebuf)
			+ (shot64 ? strlen(shot64) : 0) + 512;
	body = malloc(bodysize);

	if (!body) {
		snprintf(err, errsize, "not enough memory to send the report");
		free(escaped);
		free(shot64);
		return false;
	}

	snprintf(body, bodysize,
			"{\"version\":\"%s\",\"platform\":\"%s\",\"channel\":\"%s\",\"name\":\"%s\",\"note\":\"%s\",\"screenshot\":\"%s\",\"report\":\"%s\"}",
			VERSION_HASH, VERSION_TARGET, VERSION_CHANNEL, namebuf, notebuf, shot64 ? shot64 : "", escaped);

	free(escaped);
	free(shot64);

	snprintf(url, sizeof(url), "%s/report", g_GhostNetUrl);

	memset(&req, 0, sizeof(req));
	memset(&buf, 0, sizeof(buf));

	req.url = url;
	req.body = body;
	req.bodylen = strlen(body);
	req.type = "application/json";
	// No account, for the same reason a crash report has none.
	req.auth = false;
	req.timeout = 60;

	err[0] = '\0';
	ok = ghostnetSend(&req, &buf, &status, err, errsize);

	if (ok && status != 200) {
		char msg[128];

		if (buf.data && ghostnetJsonField(buf.data, NULL, "error", msg, sizeof(msg))) {
			snprintf(err, errsize, "%s", msg);
		} else {
			snprintf(err, errsize, "the server answered %d", status);
		}

		ok = false;
	}

	free(buf.data);
	free(body);

	if (ok) {
		sysLogPrintf(LOG_NOTE, "trace: report sent (%s)", g_TracePath);
	} else {
		sysLogPrintf(LOG_WARNING, "trace: report not sent: %s", err);
	}

	return ok;
}

#else

static bool traceReportSend(char *err, u32 errsize)
{
	snprintf(err, errsize, "this build has no network support");
	return false;
}

#endif

static int traceReportWorker(void *arg)
{
	g_State = traceReportSend(g_Err, sizeof(g_Err)) ? STATE_SENT : STATE_FAILED;
	return 0;
}

static void traceReportStartSend(void)
{
	if (g_State == STATE_BUSY || g_State == STATE_SENT || !g_TracePath[0]) {
		return;
	}

	// A name still being typed goes with the report, and is kept. Typing it
	// carries on: the worker reads g_Name, which only changes at a finish.
	if (g_NameOpen) {
		traceReportKeepName();
	}

	if (g_Thread) {
		SDL_WaitThread(g_Thread, NULL);
		g_Thread = NULL;
	}

	// Typing stays on through the send. Stopping it here handed the controller
	// back while ENTER was still held, the menu read that as a press of its
	// own and the dialog closed before it could say whether the send worked.

	g_Err[0] = '\0';
	g_State = STATE_BUSY;
	g_Thread = SDL_CreateThread(traceReportWorker, "pdtracereport", NULL);

	if (g_Thread == NULL) {
		snprintf(g_Err, sizeof(g_Err), "could not start the send");
		g_State = STATE_FAILED;
	}
}

/**
 * The note, folded at word breaks for a dialog that does not fold text itself.
 */
static void traceReportFoldNote(char *out, u32 outsize, bool cursor)
{
	const char *src = g_Note;
	u32 len = 0;
	u32 col = 0;

	out[0] = '\0';

	while (*src && len + 3 < outsize) {
		const char *space = strchr(src, ' ');
		u32 word = space ? (u32)(space - src) : strlen(src);

		if (col > 0 && col + word > TRACEREPORT_WRAP) {
			out[len++] = '\n';
			col = 0;
		}

		// A word longer than a line is broken where the line ends.
		while (word > 0 && len + 3 < outsize) {
			if (col >= TRACEREPORT_WRAP) {
				out[len++] = '\n';
				col = 0;
			}

			out[len++] = *src++;
			col++;
			word--;
		}

		if (*src == ' ' && len + 3 < outsize) {
			out[len++] = ' ';
			col++;
			src++;
		}
	}

	if (cursor && len + 2 < outsize) {
		out[len++] = '_';
	}

	out[len] = '\0';
}

static char *menutextTraceReportStatus(struct menuitem *item)
{
	switch (g_State) {
	case STATE_BUSY:
		snprintf(g_Text, sizeof(g_Text), "Sending...\n");
		return g_Text;
	case STATE_SENT: {
		// Where it can be seen once sent: the server's public board, named
		// without its scheme, which is noise on a line that is only read.
		const char *where = strstr(g_GhostNetUrl, "://");

		where = where ? where + 3 : g_GhostNetUrl;
		snprintf(g_Text, sizeof(g_Text), "Sent. Thank you!\nSee all reports at\n%s/board\n", where);
		return g_Text;
	}
	case STATE_FAILED:
		snprintf(g_Text, sizeof(g_Text), "Not sent: %s\nThe files are still in traces/.\n", g_Err);
		return g_Text;
	default:
		break;
	}

	// Said before it is offered: what goes, and that nothing goes on its own.
	snprintf(g_Text, sizeof(g_Text),
			"Captured the frame from %s: a screenshot\n"
			"and a dump of the game's state, your [Mod]\n"
			"settings and the last lines of the log.\n"
			"Nothing is sent unless you press Send.\n",
			g_When);

	return g_Text;
}

/** Whether the keyboard is in this dialog at all, on either field. */
static bool traceReportTyping(void)
{
	return g_MenuKeyboardPlayer == g_MpPlayerNum && g_State != STATE_BUSY && g_State != STATE_SENT;
}

static char *menutextTraceReportNote(struct menuitem *item)
{
	const bool typing = traceReportTyping() && g_Field == TRACEREPORT_FIELD_NOTE;
	char folded[TRACEREPORT_MAXNOTE * 2 + 64];

	traceReportFoldNote(folded, sizeof(folded), typing);

	if (typing) {
		snprintf(g_Text, sizeof(g_Text), "What went wrong? (ENTER sends, ESC stops typing)\n%s\n", folded);
	} else if (g_Note[0]) {
		snprintf(g_Text, sizeof(g_Text), "Note:\n%s\n", folded);
	} else {
		snprintf(g_Text, sizeof(g_Text), "No note.\n");
	}

	return g_Text;
}

/**
 * The name, and what it is for. Said in the dialog rather than only here,
 * because a field labelled "Name" on a bug report reads like something the
 * game needs rather than an offer. While the note is typed it also says how to
 * get to the name: the dialog opens typing, and a tester who did not know ESC
 * gave up the menu under it had no way there.
 */
static char *menutextTraceReportName(struct menuitem *item)
{
	const bool typing = traceReportTyping();

	if (typing && g_Field == TRACEREPORT_FIELD_NAME) {
		snprintf(g_Text, sizeof(g_Text),
				"Your name, kept for next time (ENTER: done)\n%s_\n", g_NameEdit);
	} else if (typing && g_Name[0]) {
		snprintf(g_Text, sizeof(g_Text), "Credit: %s (TAB to change)\n", g_Name);
	} else if (typing) {
		snprintf(g_Text, sizeof(g_Text), "Your name, for the credits: TAB or click Name\n");
	} else if (g_Name[0]) {
		snprintf(g_Text, sizeof(g_Text), "Credit: %s\n", g_Name);
	} else if (g_PadUsed) {
		snprintf(g_Text, sizeof(g_Text), "Name: optional, typed on a keyboard\n");
	} else {
		snprintf(g_Text, sizeof(g_Text), "Name: optional, for CREDITS.md\n");
	}

	return g_Text;
}

static MenuItemHandlerResult menuhandlerTraceReportType(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return g_State == STATE_BUSY || g_State == STATE_SENT;
	}

	if (operation == MENUOP_SET) {
		traceReportStartTyping(TRACEREPORT_FIELD_NOTE);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTraceReportName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return g_State == STATE_BUSY || g_State == STATE_SENT;
	}

	if (operation == MENUOP_SET) {
		traceReportStartTyping(TRACEREPORT_FIELD_NAME);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTraceReportSend(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !g_TracePath[0] || g_State == STATE_BUSY || g_State == STATE_SENT;
	}

	if (operation == MENUOP_SET) {
		traceReportStartSend();
	}

	return 0;
}

/**
 * Puts the menu's cursor on the row of the field being typed, so that the
 * highlight and the typing agree when the keyboard moves between them. The
 * keyboard has the menu from here, as the menu's own arrow keys take it: the
 * pointer would otherwise pull the cursor back to the row it rests on.
 */
static void traceReportFocusField(struct menudialog *dialog)
{
	s32 i;

	for (i = 0; g_TraceReportMenuItems[i].type != MENUITEMTYPE_END; i++) {
		if (g_TraceReportMenuItems[i].handler == (g_Field == TRACEREPORT_FIELD_NAME
					? menuhandlerTraceReportName : menuhandlerTraceReportType)) {
			dialog->focuseditem = &g_TraceReportMenuItems[i];
			g_MenuUsingMouse = false;
			break;
		}
	}
}

/**
 * A click while the keyboard types. The mouse's button is one of the pads'
 * bindings and the pads are blank while it types, so the menu never saw it:
 * "Name (Optional)" lit up under the pointer, the click did nothing, and the
 * name typed next went on the end of the note - which ENTER then sent. It is
 * the row under the pointer that is acted on, as the menu's own click does.
 */
static void traceReportClick(struct menudialog *dialog, struct menuinputs *inputs)
{
	struct menuitem *item = dialog->focuseditem;

	if (!g_MenuUsingMouse || inputs == NULL || item == NULL
			|| inputs->mousex < dialog->x || inputs->mousex > dialog->x + dialog->width
			|| inputs->mousey < dialog->y || inputs->mousey > dialog->y + dialog->height) {
		return;
	}

	if (item->handler == menuhandlerTraceReportType) {
		traceReportSetField(TRACEREPORT_FIELD_NOTE);
	} else if (item->handler == menuhandlerTraceReportName) {
		traceReportSetField(TRACEREPORT_FIELD_NAME);
	} else if (item->handler == menuhandlerTraceReportSend) {
		traceReportStartSend();
	} else if (item->flags & MENUITEMFLAG_SELECTABLE_CLOSESDIALOG) {
		// Back, once the button is up: closing stops the typing, and the pads
		// coming back with it still down would click the menu underneath
		g_ReleaseKey = VK_MOUSE_LEFT;
		g_ReleaseAction = TRACEREPORT_ONRELEASE_CLOSE;
	}
}

/**
 * The keyboard, a click or a pad's button, while this dialog types.
 */
static void traceReportTypingTick(struct menudialog *dialog, struct menuinputs *inputs)
{
	const bool name = g_Field == TRACEREPORT_FIELD_NAME;
	char *buf = name ? g_NameEdit : g_Note;
	const u32 max = name ? TRACEREPORT_MAXNAME : TRACEREPORT_MAXNOTE;
	u32 len = strlen(buf);
	const s32 key = inputGetLastKey();
	const bool ctrl = (inputGetKeyModState() & KM_CTRL) != 0;
	char chr;

	inputClearLastKey();

	// Every character typed since the last frame, in order
	while ((chr = inputGetLastTextChar()) != 0) {
		inputClearLastTextChar();

		if (!ctrl && chr >= 0x20 && chr < 0x7f && len < max) {
			buf[len++] = chr;
			buf[len] = '\0';
		}
	}

	if (key == VK_RETURN) {
		// ENTER sends from the note, because that is the field the dialog
		// opens on and sending is what the player came to do. From the name
		// it finishes the name and goes back to the note: a report sent by the
		// keystroke that filled a form in would carry no note, and stopping
		// the typing gave the pads back with ENTER still down, which the menu
		// took as a press on "Name (Optional)" and started the name again.
		if (name) {
			traceReportSetField(TRACEREPORT_FIELD_NOTE);
			traceReportFocusField(dialog);
		} else {
			traceReportStartSend();
		}
	} else if (key == VK_KEYBOARD_BEGIN + SDL_SCANCODE_TAB) {
		traceReportSetField(name ? TRACEREPORT_FIELD_NOTE : TRACEREPORT_FIELD_NAME);
		traceReportFocusField(dialog);
	} else if (key == VK_KEYBOARD_BEGIN + SDL_SCANCODE_UP) {
		traceReportSetField(TRACEREPORT_FIELD_NOTE);
		traceReportFocusField(dialog);
	} else if (key == VK_KEYBOARD_BEGIN + SDL_SCANCODE_DOWN) {
		traceReportSetField(TRACEREPORT_FIELD_NAME);
		traceReportFocusField(dialog);
	} else if (key == VK_BACKSPACE) {
		if (len > 0) {
			buf[len - 1] = '\0';
		}
	} else if (ctrl && key == VK_A + ('v' - 'a')) {
		const char *clip = inputGetClipboard();

		if (clip) {
			snprintf(buf + len, max + 1 - len, "%s", clip);
			inputClearClipboard();

			// A pasted name is one line: the server writes it into a
			// header line, and typing cannot produce a newline here.
			if (name) {
				u32 i;

				for (i = 0; buf[i]; i++) {
					if ((u8)buf[i] < 0x20) {
						buf[i] = ' ';
					}
				}
			}
		}
	} else if (key == VK_MOUSE_LEFT) {
		traceReportClick(dialog, inputs);
	} else if (key >= VK_JOY_BEGIN && key < VK_TOTAL_COUNT) {
		// A pad's button, which does nothing while the keyboard types: it
		// stops the typing, so that a player on a controller is never stuck
		// in a dialog only ESC could get out of
		g_PadUsed = true;
		g_ReleaseKey = key;
		g_ReleaseAction = TRACEREPORT_ONRELEASE_STOP;
	}
}

static MenuDialogHandlerResult menudialogTraceReport(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	struct menudialog *dialog;
	struct menuinputs *inputs;

	switch (operation) {
	case MENUOP_OPEN:
		g_Open = true;
		g_OpenMenu = g_MpPlayerNum;
		g_CloseAt = 0;
		g_ReleaseKey = 0;
		g_PadUsed = false;

		if (g_State != STATE_SENT) {
			traceReportStartTyping(TRACEREPORT_FIELD_NOTE);
		}
		break;
	case MENUOP_CLOSE:
		traceReportStopTyping();
		traceReportCloseName();
		g_ReleaseKey = 0;
		g_Open = false;
		g_HoldFrames = TRACEREPORT_HOLD_FRAMES;
		break;
	case MENUOP_TICK:
		dialog = g_Menus[g_MpPlayerNum].curdialog;
		inputs = data ? data->dialog2.inputs : NULL;

		if (dialog == NULL || dialog->definition != dialogdef) {
			break;
		}

		if (g_State == STATE_BUSY || g_State == STATE_SENT) {
			while (inputGetLastTextChar()) {
				inputClearLastTextChar();
			}

			inputClearLastKey();

			// Long enough to read "Sent" and where the reports are, then
			// back to the game.
			if (g_State == STATE_SENT) {
				if (g_CloseAt == 0) {
					g_CloseAt = SDL_GetTicks() + TRACEREPORT_SENT_LINGER_MS;
				} else if (SDL_GetTicks() >= g_CloseAt) {
					g_CloseAt = 0;
					menuPopDialog();
				}
			}
			break;
		}

		// Typing that stopped with the name open (ESC, which menu.c takes)
		// finishes the name
		if (g_NameOpen && !(g_MenuKeyboardPlayer == g_MpPlayerNum && g_Field == TRACEREPORT_FIELD_NAME)) {
			traceReportCloseName();
		}

		if (g_ReleaseKey) {
			while (inputGetLastTextChar()) {
				inputClearLastTextChar();
			}

			inputClearLastKey();

			if (!inputKeyPressed(g_ReleaseKey)) {
				const s32 action = g_ReleaseAction;

				g_ReleaseKey = 0;
				traceReportStopTyping();
				traceReportCloseName();

				if (action == TRACEREPORT_ONRELEASE_CLOSE) {
					menuPopDialog();
				}
			}
			break;
		}

		if (g_MenuKeyboardPlayer != g_MpPlayerNum) {
			// Not typing, and TAB still goes to the other field rather than
			// being the menu's Start, which closed the whole pause menu with
			// the report in it (or put Ready over it in a match's setup)
			if (inputKeyPressedThisFrame(VK_KEYBOARD_BEGIN + SDL_SCANCODE_TAB)) {
				traceReportStartTyping(g_Field == TRACEREPORT_FIELD_NAME ? TRACEREPORT_FIELD_NOTE : TRACEREPORT_FIELD_NAME);
				traceReportFocusField(dialog);

				if (inputs) {
					inputs->select = 0;
					inputs->start = 0;
					inputs->back = 0;
				}
			}
			break;
		}

		traceReportTypingTick(dialog, inputs);
		break;
	}

	return 0;
}

struct menuitem g_TraceReportMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextTraceReportStatus,
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
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextTraceReportNote,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextTraceReportName,
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
		(uintptr_t)"Type a Note\n",
		0,
		menuhandlerTraceReportType,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Name (Optional)\n",
		0,
		menuhandlerTraceReportName,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Send to Dab\n",
		0,
		menuhandlerTraceReportSend,
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

struct menudialogdef g_TraceReportMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Report a Problem",
	g_TraceReportMenuItems,
	menudialogTraceReport,
	// Start (TAB on a keyboard) picks the row, as A does, rather than closing
	// the pause menu the report is in and the report with it
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

PD_CONSTRUCTOR static void traceReportConfigInit(void)
{
	configRegisterInt("Mod.TraceReport", &g_Enabled, 0, 1);
	configRegisterString("Mod.ReportName", g_Name, sizeof(g_Name));
}
