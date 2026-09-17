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

static s32 g_Enabled = 1;

static char g_TracePath[FS_MAXPATH + 1];
static char g_ShotPath[FS_MAXPATH + 1];
static char g_When[32];
static bool g_OfferPending;
static bool g_Open;

static char g_Note[TRACEREPORT_MAXNOTE + 1];
static char g_Text[TRACEREPORT_MAXNOTE * 2 + 512];
static char g_Err[256];
static SDL_Thread *g_Thread;
static volatile s32 g_State;
// When a sent report's dialog closes itself, in SDL ticks; 0 while not yet sent.
static u32 g_CloseAt;
#define TRACEREPORT_SENT_LINGER_MS 2000

s32 traceReportEnabled(void)
{
	return g_Enabled && crashReportCanSend();
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
	g_Note[0] = '\0';
	g_Err[0] = '\0';
	g_State = STATE_IDLE;
	g_OfferPending = g_TracePath[0] != '\0';
}

static void traceReportStartTyping(void)
{
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

/**
 * Called from lvTick() once a frame. Opens the dialog for a report F3 made,
 * once there is somewhere safe to open it.
 *
 * Over a menu that is already up it goes on top. In play it is pushed the way
 * the game pushes its controller pak warnings mid-mission: as a root dialog,
 * pausing a one player game. A cutscene or a pause menu on its way in waits.
 */
void traceReportTick(void)
{
	const s32 prevplayernum = g_MpPlayerNum;

	if (!g_OfferPending || g_Open || !traceReportEnabled()) {
		return;
	}

	g_MpPlayerNum = 0;

	if (g_Menus[0].curdialog != NULL) {
		menuPushDialog(&g_TraceReportMenuDialog);
		g_OfferPending = false;
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
	char header[TRACEREPORT_MAXNOTE + 512];
	char notebuf[TRACEREPORT_MAXNOTE * 6 + 8];
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
			"note: %s\n\n"
			"--- trace ---\n",
			sysGetVersionString(), VERSION_CHANNEL, traceReportBaseName(g_TracePath), g_Note[0] ? g_Note : "-");

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
	free(text);

	bodysize = strlen(escaped) + strlen(notebuf) + (shot64 ? strlen(shot64) : 0) + 512;
	body = malloc(bodysize);

	if (!body) {
		snprintf(err, errsize, "not enough memory to send the report");
		free(escaped);
		free(shot64);
		return false;
	}

	snprintf(body, bodysize,
			"{\"version\":\"%s\",\"platform\":\"%s\",\"channel\":\"%s\",\"note\":\"%s\",\"screenshot\":\"%s\",\"report\":\"%s\"}",
			VERSION_HASH, VERSION_TARGET, VERSION_CHANNEL, notebuf, shot64 ? shot64 : "", escaped);

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
	case STATE_SENT:
		snprintf(g_Text, sizeof(g_Text), "Sent. Thank you!\n");
		return g_Text;
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

static char *menutextTraceReportNote(struct menuitem *item)
{
	const bool typing = g_MenuKeyboardPlayer == g_MpPlayerNum && g_State != STATE_BUSY && g_State != STATE_SENT;
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

static MenuItemHandlerResult menuhandlerTraceReportType(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return g_State == STATE_BUSY || g_State == STATE_SENT;
	}

	if (operation == MENUOP_SET) {
		traceReportStartTyping();
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

static MenuDialogHandlerResult menudialogTraceReport(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_OPEN:
		g_Open = true;
		g_CloseAt = 0;

		if (g_State != STATE_SENT) {
			traceReportStartTyping();
		}
		break;
	case MENUOP_CLOSE:
		traceReportStopTyping();
		g_Open = false;
		break;
	case MENUOP_TICK:
		if (g_Menus[g_MpPlayerNum].curdialog == NULL
				|| g_Menus[g_MpPlayerNum].curdialog->definition != dialogdef) {
			break;
		}

		if (g_State == STATE_BUSY || g_State == STATE_SENT) {
			inputClearLastTextChar();
			inputClearLastKey();

			// Long enough to read "Sent", then back to the game.
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

		if (g_MenuKeyboardPlayer != g_MpPlayerNum) {
			break;
		}

		{
			u32 len = strlen(g_Note);
			const char chr = inputGetLastTextChar();
			const s32 key = inputGetLastKey();
			const bool ctrl = (inputGetKeyModState() & KM_CTRL) != 0;

			inputClearLastTextChar();
			inputClearLastKey();

			if (key == VK_RETURN) {
				traceReportStartSend();
			} else if (key == VK_BACKSPACE) {
				if (len > 0) {
					g_Note[len - 1] = '\0';
				}
			} else if (ctrl && key == VK_A + ('v' - 'a')) {
				const char *clip = inputGetClipboard();

				if (clip) {
					snprintf(g_Note + len, sizeof(g_Note) - len, "%s", clip);
					inputClearClipboard();
				}
			} else if (!ctrl && chr >= 0x20 && chr < 0x7f && len < TRACEREPORT_MAXNOTE) {
				g_Note[len] = chr;
				g_Note[len + 1] = '\0';
			}
		}
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
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

PD_CONSTRUCTOR static void traceReportConfigInit(void)
{
	configRegisterInt("Mod.TraceReport", &g_Enabled, 0, 1);
}
