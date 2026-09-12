/**
 * Crash reports. See port/include/crashreport.h for what this is for.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "fs.h"
#include "config.h"
#include "ghostnet.h"
#include "crashreport.h"
#include "versioninfo.h"

/**
 * The last lines the game logged, whether or not there is a log file.
 *
 * pd.log only exists when the game was started with --log, which no player
 * does, so reading the tail of a file would attach nothing to the report that
 * matters most. The lines themselves are printed either way, so they are kept
 * here as they go past: a fixed ring, no allocation, and the cost is one
 * bounded copy per log line.
 *
 * Written from whichever thread logged, without a lock. A line is copied into
 * a slot of its own and terminated inside it, so the worst a race can do is
 * put two lines in the ring out of order or lose one - which is a diagnostic
 * being slightly wrong, against taking a mutex on a path the renderer uses.
 */
#define RING_LINELEN 192

static char g_Ring[CRASHREPORT_LOGLINES][RING_LINELEN];
static u32 g_RingNext;
static u32 g_RingCount;

static char g_PendingPath[FS_MAXPATH];
static char g_PendingWhen[32];
static s32 g_Count;

void crashReportLogLine(const char *line)
{
	const u32 slot = g_RingNext % CRASHREPORT_LOGLINES;

	snprintf(g_Ring[slot], RING_LINELEN, "%s", line);

	g_RingNext = (g_RingNext + 1) % CRASHREPORT_LOGLINES;

	if (g_RingCount < CRASHREPORT_LOGLINES) {
		++g_RingCount;
	}
}

static void crashReportWriteRing(FILE *f)
{
	const u32 first = (g_RingNext + CRASHREPORT_LOGLINES - g_RingCount) % CRASHREPORT_LOGLINES;

	for (u32 i = 0; i < g_RingCount; ++i) {
		fprintf(f, "%s\n", g_Ring[(first + i) % CRASHREPORT_LOGLINES]);
	}
}

/**
 * The name of the mod the game had mounted, or "-".
 *
 * The directory name rather than the mod's own title: a title is the mod
 * author's to change between releases and the directory is what the player
 * unzipped, which is also what an IMPORT.txt sits next to.
 */
static const char *crashReportModName(void)
{
	const char *dir = fsGetModDir();
	const char *slash;

	if (dir == NULL || dir[0] == '\0') {
		return "-";
	}

	slash = strrchr(dir, '/');

#ifdef PLATFORM_WIN32
	{
		const char *back = strrchr(dir, '\\');

		if (back && (slash == NULL || back > slash)) {
			slash = back;
		}
	}
#endif

	return slash ? slash + 1 : dir;
}

const char *crashReportSave(const char *text)
{
	char path[FS_MAXPATH];
	char stamp[32];
	char settings[4096];
	const time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	FILE *f;

	if (tm == NULL) {
		return NULL;
	}

	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
	strftime(g_PendingWhen, sizeof(g_PendingWhen), "%Y-%m-%d %H:%M", tm);

	fsCreateDir(CRASHREPORT_DIR);

	snprintf(path, sizeof(path), CRASHREPORT_DIR "/crash-%s.txt", stamp);

	f = fsFileOpenWrite(path);

	if (f == NULL) {
		return NULL;
	}

	// The header is what a reader needs before the stack means anything: which
	// build, which machine, and what was loaded into it.
	fprintf(f, "Dab's Mod crash report\n");
	fprintf(f, "version: %s\n", sysGetVersionString());
	fprintf(f, "channel: %s\n", VERSION_CHANNEL);
	fprintf(f, "when: %s\n", g_PendingWhen);
	fprintf(f, "mod: %s\n", crashReportModName());
	fprintf(f, "note: \n");

	fprintf(f, "\n--- crash ---\n%s\n", text ? text : "(none)");

	configDumpSection("Mod", settings, sizeof(settings));
	fprintf(f, "\n--- pd.ini [Mod] ---\n%s", settings);

	fprintf(f, "\n--- log (last %d lines) ---\n", CRASHREPORT_LOGLINES);
	crashReportWriteRing(f);

	fsFileFree(f);

	snprintf(g_PendingPath, sizeof(g_PendingPath), "%s", path);
	++g_Count;

	return g_PendingPath;
}

const char *crashReportPending(void)
{
	return g_PendingPath[0] ? g_PendingPath : NULL;
}

const char *crashReportPendingWhen(void)
{
	return g_PendingWhen;
}

s32 crashReportCount(void)
{
	return g_Count;
}

struct scanstate {
	char newest[FS_MAXPATH];
	s32 count;
};

static void crashReportScanEntry(const char *name, void *arg)
{
	struct scanstate *state = (struct scanstate *)arg;
	const u32 len = strlen(name);

	if (len < 7 || strncmp(name, "crash-", 6) != 0 || strcmp(name + len - 4, ".txt") != 0) {
		return;
	}

	++state->count;

	// The names are the timestamps they were written at, so the newest sorts
	// last and no file has to be opened to find it.
	if (strcmp(name, state->newest) > 0) {
		snprintf(state->newest, sizeof(state->newest), "%s", name);
	}
}

void crashReportScan(void)
{
	struct scanstate state;

	memset(&state, 0, sizeof(state));

	fsScanDir(CRASHREPORT_DIR, crashReportScanEntry, &state);

	g_Count = state.count;

	if (state.count == 0) {
		return;
	}

	snprintf(g_PendingPath, sizeof(g_PendingPath), CRASHREPORT_DIR "/%s", state.newest);

	// crash-YYYYmmdd-HHMMSS.txt, read back into something to put on the page.
	if (strlen(state.newest) >= 21) {
		snprintf(g_PendingWhen, sizeof(g_PendingWhen), "%.4s-%.2s-%.2s %.2s:%.2s",
				state.newest + 6, state.newest + 10, state.newest + 12,
				state.newest + 15, state.newest + 17);
	}

	sysLogPrintf(LOG_NOTE, "crash: %d report%s waiting to be sent (%s)",
			g_Count, g_Count == 1 ? "" : "s", g_PendingPath);
}

void crashReportDiscard(void)
{
	if (g_PendingPath[0]) {
		fsRemoveFile(g_PendingPath);
		g_PendingPath[0] = '\0';
		g_PendingWhen[0] = '\0';

		if (g_Count > 0) {
			--g_Count;
		}
	}
}

bool crashReportCanSend(void)
{
#ifdef PD_GHOST_NET
	return true;
#else
	return false;
#endif
}

#ifdef PD_GHOST_NET

bool crashReportSend(const char *path, const char *note, char *err, u32 errsize)
{
	struct ghostnetreq req;
	struct ghostnetbuf buf;
	char url[512];
	char *body;
	char *escaped;
	u32 bodysize;
	u32 size = 0;
	char *text;
	s32 status = 0;
	bool ok;

	if (path == NULL || path[0] == '\0') {
		snprintf(err, errsize, "there is no report to send");
		return false;
	}

	text = fsFileLoad(path, &size);

	if (text == NULL || size == 0) {
		snprintf(err, errsize, "the report could not be read");
		sysMemFree(text);
		return false;
	}

	if (size > CRASHREPORT_MAXTEXT) {
		size = CRASHREPORT_MAXTEXT;
		text[size] = '\0';
	}

	// A newline becomes two characters and a control character six, so the
	// escaped copy is given room for the worst of it rather than the usual.
	bodysize = size * 6 + CRASHREPORT_MAXNOTE * 6 + 512;
	escaped = sysMemAlloc(size * 6 + 8);
	body = sysMemAlloc(bodysize);

	if (escaped == NULL || body == NULL) {
		snprintf(err, errsize, "not enough memory to send the report");
		sysMemFree(escaped);
		sysMemFree(body);
		sysMemFree(text);
		return false;
	}

	ghostnetJsonEscape(text, escaped, size * 6 + 8);

	{
		char notebuf[CRASHREPORT_MAXNOTE * 6 + 8];
		char trimmed[CRASHREPORT_MAXNOTE + 1];

		snprintf(trimmed, sizeof(trimmed), "%s", note ? note : "");
		ghostnetJsonEscape(trimmed, notebuf, sizeof(notebuf));

		snprintf(body, bodysize,
				"{\"version\":\"%s\",\"platform\":\"%s\",\"channel\":\"%s\",\"note\":\"%s\",\"report\":\"%s\"}",
				VERSION_HASH, VERSION_TARGET, VERSION_CHANNEL, notebuf, escaped);
	}

	snprintf(url, sizeof(url), "%s/crash", g_GhostNetUrl);

	memset(&req, 0, sizeof(req));
	memset(&buf, 0, sizeof(buf));

	req.url = url;
	req.body = body;
	req.bodylen = strlen(body);
	req.type = "application/json";
	// No account headers. A crash report is not a thing an account owns, and
	// the page that sends one is reachable by somebody who has never signed in.
	req.auth = false;
	req.timeout = 20;

	err[0] = '\0';
	ok = ghostnetSend(&req, &buf, &status, err, errsize);

	if (ok && status != 200) {
		char msg[128];

		// The server says why in the same shape everything else here answers
		// in, and what it says is better than the status number.
		if (buf.data && ghostnetJsonField(buf.data, NULL, "error", msg, sizeof(msg))) {
			snprintf(err, errsize, "%s", msg);
		} else {
			snprintf(err, errsize, "the server answered %d", status);
		}

		ok = false;
	}

	free(buf.data);
	sysMemFree(body);
	sysMemFree(escaped);
	sysMemFree(text);

	if (ok) {
		sysLogPrintf(LOG_NOTE, "crash: report sent (%s)", path);

		if (g_PendingPath[0] && strcmp(path, g_PendingPath) == 0) {
			crashReportDiscard();
		} else {
			fsRemoveFile(path);
		}
	} else {
		sysLogPrintf(LOG_WARNING, "crash: report not sent: %s", err);
	}

	return ok;
}

#else

bool crashReportSend(const char *path, const char *note, char *err, u32 errsize)
{
	snprintf(err, errsize, "this build has no network support");
	return false;
}

#endif
