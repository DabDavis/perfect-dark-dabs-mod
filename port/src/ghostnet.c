#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include <SDL2/SDL.h>
#include "platform.h"
#include "types.h"
#include "game/modghost.h"
#include "bss.h"
#include "fs.h"
#include "system.h"
#include "ghostnet.h"

#ifdef PD_HAVE_CURL
#include <curl/curl.h>
#endif

/**
 * See ghostnet.h for what this is and why it is asynchronous.
 *
 * The threading here is deliberately the smallest thing that works: one
 * worker, one job, one result, one mutex. A menu asks one question at a time
 * and waits for the answer before it can ask another, so a queue would be
 * machinery with no second user. SDL's threads are used because SDL2 is
 * already linked and its mutexes work the same on all three platforms.
 */

/**
 * The account in use, and the ones this machine remembers.
 *
 * Slot zero is the active account and is what every request sends, which is
 * why it keeps the plain Mod.GhostUser and Mod.GhostPin names in pd.ini: a
 * config file written by an older build still signs the same person in. The
 * rest are accounts that were signed into before and can be switched back to
 * without typing a PIN again, the way the game remembers agents rather than
 * making you name one every time you sit down.
 *
 * Switching swaps rather than copies, so nothing is lost by choosing: the
 * account you were using goes into the slot the one you chose came out of.
 */
char g_GhostNetUser[GHOSTNET_MAXUSER + 2] = { 0 };
char g_GhostNetPin[GHOSTNET_MAXPIN + 2] = { 0 };
char g_GhostNetSavedUser[GHOSTNET_MAXACCOUNTS - 1][GHOSTNET_MAXUSER + 2] = { 0 };
char g_GhostNetSavedPin[GHOSTNET_MAXACCOUNTS - 1][GHOSTNET_MAXPIN + 2] = { 0 };
char g_GhostNetUrl[256] = "https://texturepacks.art/pdghosts";

static s32 g_State = GHOSTNET_IDLE;
static char g_Message[128] = { 0 };

static struct ghostboardentry g_Board[GHOSTNET_MAXBOARD];
static s32 g_BoardCount = 0;
static s32 g_BoardStage = -1;
static s32 g_BoardDiff = -1;

#ifdef PD_HAVE_CURL

#define JOB_NONE     0
#define JOB_REGISTER 1
#define JOB_LOGIN    2
#define JOB_UPLOAD   3
#define JOB_BOARD    4
#define JOB_DOWNLOAD 5

static SDL_mutex *g_Lock = NULL;
static SDL_Thread *g_Thread = NULL;
static s32 g_Job = JOB_NONE;
static s32 g_JobStage = 0;
static s32 g_JobDiff = 0;
static s32 g_JobId = 0;
static char g_JobFile[FS_MAXPATH + 1];

// What a request may return before it is treated as a broken or hostile
// server. A leaderboard of a hundred rows is a few kilobytes; a ghost is a
// megabyte or two.
#define GHOSTNET_MAXREPLY (8 * 1024 * 1024)
#define GHOSTNET_TIMEOUT  20L

struct ghostnetbuf {
	char *data;
	size_t len;
};

static size_t ghostnetWrite(void *ptr, size_t size, size_t nmemb, void *arg)
{
	struct ghostnetbuf *buf = arg;
	size_t add = size * nmemb;
	char *grown;

	if (buf->len + add > GHOSTNET_MAXREPLY) {
		return 0;
	}

	grown = realloc(buf->data, buf->len + add + 1);

	if (grown == NULL) {
		return 0;
	}

	buf->data = grown;
	memcpy(buf->data + buf->len, ptr, add);
	buf->len += add;
	buf->data[buf->len] = '\0';

	return add;
}

static void ghostnetSetResult(s32 state, const char *msg)
{
	SDL_LockMutex(g_Lock);
	g_State = state;
	snprintf(g_Message, sizeof(g_Message), "%s", msg ? msg : "");
	SDL_UnlockMutex(g_Lock);
}

/**
 * Pull one value out of a flat JSON object.
 *
 * The replies this speaks to are small, flat and written by the server on the
 * other end of this file, so a scanner is enough and a parser would be a
 * dependency. It is still written to survive nonsense: nothing is copied
 * without a length, and a key that is not there simply is not found.
 */
static bool ghostnetJsonField(const char *json, const char *key, char *out, u32 outsize)
{
	char pattern[64];
	const char *at;
	u32 i = 0;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	at = strstr(json, pattern);

	if (at == NULL) {
		return false;
	}

	at += strlen(pattern);

	while (*at == ' ' || *at == ':') {
		at++;
	}

	if (*at == '"') {
		at++;

		while (*at && *at != '"' && i + 1 < outsize) {
			out[i++] = *at++;
		}
	} else {
		while (*at && *at != ',' && *at != '}' && *at != ' ' && i + 1 < outsize) {
			out[i++] = *at++;
		}
	}

	out[i] = '\0';

	return true;
}

static CURL *ghostnetNewHandle(struct ghostnetbuf *buf, const char *url)
{
	CURL *curl = curl_easy_init();

	if (curl == NULL) {
		return NULL;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ghostnetWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, GHOSTNET_TIMEOUT);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "pd-dabs-mod-ghost/1");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	return curl;
}

/**
 * Post a username and PIN as JSON, for register and login.
 *
 * The PIN goes over TLS, which is the whole reason the endpoint is https. It
 * is a four digit number and the server treats the rate limiter rather than
 * the PIN as the actual control, but sending it in clear would still be
 * handing it to anyone on the same network.
 */
static bool ghostnetPostCredentials(const char *endpoint, char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0 };
	char url[320];
	char body[192];
	CURL *curl;
	CURLcode res;
	long code = 0;
	bool ok = false;

	snprintf(url, sizeof(url), "%s/%s", g_GhostNetUrl, endpoint);
	snprintf(body, sizeof(body), "{\"username\":\"%s\",\"pin\":\"%s\"}",
			g_GhostNetUser, g_GhostNetPin);

	curl = ghostnetNewHandle(&buf, url);

	if (curl == NULL) {
		snprintf(msg, msgsize, "could not start request");
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

	if (res != CURLE_OK) {
		snprintf(msg, msgsize, "%s", curl_easy_strerror(res));
	} else if (buf.data && strstr(buf.data, "\"ok\": true")) {
		ok = true;
	} else {
		char err[96];

		if (buf.data && ghostnetJsonField(buf.data, "error", err, sizeof(err))) {
			snprintf(msg, msgsize, "%s", err);
		} else {
			snprintf(msg, msgsize, "server said %ld", code);
		}
	}

	curl_easy_cleanup(curl);
	free(buf.data);

	return ok;
}

static bool ghostnetUploadFile(const char *rel, char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0 };
	struct curl_slist *headers = NULL;
	char url[320];
	char header[128];
	CURL *curl;
	CURLcode res;
	void *data;
	u32 size = 0;
	bool ok = false;

	data = fsFileLoad(rel, &size);

	if (data == NULL || size == 0) {
		snprintf(msg, msgsize, "could not read %s", rel);
		return false;
	}

	snprintf(url, sizeof(url), "%s/upload", g_GhostNetUrl);
	curl = ghostnetNewHandle(&buf, url);

	if (curl == NULL) {
		free(data);
		snprintf(msg, msgsize, "could not start request");
		return false;
	}

	snprintf(header, sizeof(header), "X-Ghost-User: %s", g_GhostNetUser);
	headers = curl_slist_append(headers, header);
	snprintf(header, sizeof(header), "X-Ghost-Pin: %s", g_GhostNetPin);
	headers = curl_slist_append(headers, header);
	headers = curl_slist_append(headers, "Content-Type: application/octet-stream");

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)size);

	res = curl_easy_perform(curl);

	if (res != CURLE_OK) {
		snprintf(msg, msgsize, "%s", curl_easy_strerror(res));
	} else if (buf.data && strstr(buf.data, "\"ok\": true")) {
		ok = true;
	} else {
		char err[96];

		if (buf.data && ghostnetJsonField(buf.data, "error", err, sizeof(err))) {
			snprintf(msg, msgsize, "%s", err);
		} else {
			snprintf(msg, msgsize, "upload refused");
		}
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(buf.data);
	free(data);

	return ok;
}

/**
 * Upload the runs this agent set.
 *
 * A run recorded since ghosts carried an owner is yours to send if that owner
 * is your account. The agent in the header says who ran it, which is a
 * different question - two people can both play as Joanna, and an agent name
 * was never a claim on anything.
 *
 * Runs from before the owner field, and runs recorded while signed out, name
 * nobody and are sent by nobody. The server refuses them too: what the client
 * sends is a proposal, and the file that arrives is what decides.
 *
 * One button that means "publish my times".
 */

struct ghostnetuploadscan {
	s32 sent;
	s32 skipped;
	s32 failed;
	char msg[128];
};

/**
 * Whether this account may publish this run.
 *
 * Case insensitively, because the server holds usernames that way and Dab and
 * dab are one account there: a comparison here that disagreed would offer to
 * upload a file the server then refused.
 */
static bool ghostnetOwns(const struct modghostentry *entry)
{
	// A run that cannot say whose it is is nobody's to publish. There was a
	// fallback here comparing the agent instead, and it was a hole rather than
	// a kindness: an agent is a save file and does not change when the ghost
	// account does, so signing in as somebody new and pressing Upload
	// published every unowned run on the machine under the new name. That is
	// the theft the owner field exists to stop, arrived at by being helpful
	// about old files. They still race and still list locally; they just do
	// not go on a board.
	if (entry->owner[0] == '\0') {
		return false;
	}

	return strcasecmp(entry->owner, g_GhostNetUser) == 0;
}

static bool ghostnetIsBestOfMine(const struct modghostentry *entry)
{
	s32 count = modGhostGetCatalogueCount();
	s32 i;

	for (i = 0; i < count; i++) {
		const struct modghostentry *other = modGhostGetCatalogueEntry(i);

		// Only runs this account may publish get a say in which of them is
		// the best. Without that, a downloaded ghost sitting in the directory
		// with a quicker time would be found first and would answer no for a
		// run of your own - the stolen file cannot be uploaded and would
		// silently stop yours from being uploaded either.
		if (other == NULL
				|| other->stagenum != entry->stagenum
				|| other->difficulty != entry->difficulty
				|| !ghostnetOwns(other)) {
			continue;
		}

		return strcmp(other->filename, entry->filename) == 0;
	}

	return true;
}

static void ghostnetUploadScan(const char *name, void *arg)
{
	struct ghostnetuploadscan *scan = arg;
	struct modghostentry *entry;
	char rel[FS_MAXPATH + 1];
	s32 count = modGhostGetCatalogueCount();
	s32 i;

	for (i = 0; i < count; i++) {
		entry = modGhostGetCatalogueEntry(i);

		if (entry == NULL || strcmp(entry->filename, name) != 0) {
			continue;
		}

		if (!ghostnetOwns(entry)) {
			scan->skipped++;
			return;
		}

		// Only your quickest run of a stage and difficulty is sent, because
		// only your quickest is a place on the board: the server keeps one row
		// per player per level and answers "you already have a faster time" to
		// the rest. Deciding that here rather than letting the server decide
		// it is the difference between one upload and an evening of them.
		//
		// The catalogue is sorted by stage, then difficulty, then time, so the
		// first entry that matches this one's level is the run to send and
		// anything else matching is slower.
		if (!ghostnetIsBestOfMine(entry)) {
			scan->skipped++;
			return;
		}

		snprintf(rel, sizeof(rel), MODGHOST_DIR "/%s", name);

		if (ghostnetUploadFile(rel, scan->msg, sizeof(scan->msg))) {
			scan->sent++;
		} else {
			scan->failed++;
		}

		return;
	}
}

static bool ghostnetFetchBoardNow(char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0 };
	char url[320];
	CURL *curl;
	CURLcode res;
	const char *at;
	s32 count = 0;

	snprintf(url, sizeof(url), "%s/leaderboard?stage=%d&diff=%d&limit=%d",
			g_GhostNetUrl, g_JobStage, g_JobDiff, GHOSTNET_MAXBOARD);

	curl = ghostnetNewHandle(&buf, url);

	if (curl == NULL) {
		snprintf(msg, msgsize, "could not start request");
		return false;
	}

	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		snprintf(msg, msgsize, "%s", curl_easy_strerror(res));
		free(buf.data);
		return false;
	}

	if (buf.data == NULL) {
		snprintf(msg, msgsize, "no reply");
		return false;
	}

	// One object per row, each with the same three keys. Walking from one
	// "id" to the next is enough structure for a reply this shape, and every
	// copy out of it is bounded.
	at = buf.data;

	while (count < GHOSTNET_MAXBOARD) {
		char num[24];
		const char *next = strstr(at, "{\"id\"");

		if (next == NULL) {
			break;
		}

		at = next + 1;

		if (!ghostnetJsonField(at, "id", num, sizeof(num))) {
			break;
		}

		g_Board[count].id = atoi(num);

		if (ghostnetJsonField(at, "time60", num, sizeof(num))) {
			g_Board[count].time60 = (u32)atoi(num);
		} else {
			g_Board[count].time60 = 0;
		}

		if (!ghostnetJsonField(at, "user", g_Board[count].user, sizeof(g_Board[count].user))) {
			g_Board[count].user[0] = '\0';
		}

		g_Board[count].have = false;
		count++;
	}

	free(buf.data);

	SDL_LockMutex(g_Lock);
	g_BoardCount = count;
	g_BoardStage = g_JobStage;
	g_BoardDiff = g_JobDiff;
	SDL_UnlockMutex(g_Lock);

	snprintf(msg, msgsize, "%d %s on the board", count, count == 1 ? "time" : "times");

	return true;
}

static bool ghostnetDownloadNow(char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0 };
	char url[320];
	CURL *curl;
	CURLcode res;
	FILE *f;
	long code = 0;

	snprintf(url, sizeof(url), "%s/download?id=%d", g_GhostNetUrl, g_JobId);
	curl = ghostnetNewHandle(&buf, url);

	if (curl == NULL) {
		snprintf(msg, msgsize, "could not start request");
		return false;
	}

	res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		snprintf(msg, msgsize, "%s", curl_easy_strerror(res));
		free(buf.data);
		return false;
	}

	// A ghost, not a JSON error: the server sends one or the other and the
	// magic is what tells them apart without trusting the status code.
	if (buf.len < 128 || memcmp(buf.data, MODGHOST_MAGIC, 8) != 0) {
		snprintf(msg, msgsize, "server did not send a ghost (%ld)", code);
		free(buf.data);
		return false;
	}

	if (fsFileSize(MODGHOST_DIR) < 0 && fsCreateDir(MODGHOST_DIR) != 0) {
		snprintf(msg, msgsize, "could not create the ghosts folder");
		free(buf.data);
		return false;
	}

	f = fsFileOpenWrite(g_JobFile);

	if (f == NULL) {
		snprintf(msg, msgsize, "could not write %s", g_JobFile);
		free(buf.data);
		return false;
	}

	if (fwrite(buf.data, 1, buf.len, f) != buf.len) {
		snprintf(msg, msgsize, "only part of the ghost was written");
		fclose(f);
		free(buf.data);
		return false;
	}

	fclose(f);
	free(buf.data);

	snprintf(msg, msgsize, "downloaded, it is in Choose Ghosts now");

	return true;
}

static int ghostnetWorker(void *arg)
{
	char msg[128] = { 0 };
	bool ok = false;

	switch (g_Job) {
	case JOB_REGISTER:
		ok = ghostnetPostCredentials("register", msg, sizeof(msg));

		if (ok) {
			snprintf(msg, sizeof(msg), "account created, you are signed in");
		}
		break;
	case JOB_LOGIN:
		ok = ghostnetPostCredentials("login", msg, sizeof(msg));

		if (ok) {
			snprintf(msg, sizeof(msg), "signed in as %s", g_GhostNetUser);
		}
		break;
	case JOB_UPLOAD: {
		struct ghostnetuploadscan scan;

		memset(&scan, 0, sizeof(scan));
		modGhostScanCatalogue();
		fsScanDir(MODGHOST_DIR, ghostnetUploadScan, &scan);

		ok = scan.failed == 0;

		if (scan.sent == 0 && scan.failed == 0) {
			// Naming the agent matters here: an empty result almost always
			// means the runs on disk were set by a different one, and a
			// message that does not say whose runs it was looking for gives
			// the player nothing to go on.
			snprintf(msg, sizeof(msg), "no runs by %s here - %d are not yours",
					g_GhostNetUser, scan.skipped);
			ok = true;
		} else if (scan.failed) {
			snprintf(msg, sizeof(msg), "sent %d, %d failed: %s",
					scan.sent, scan.failed, scan.msg);
		} else {
			snprintf(msg, sizeof(msg), "uploaded %d of your ghosts", scan.sent);
		}
		break;
	}
	case JOB_BOARD:
		ok = ghostnetFetchBoardNow(msg, sizeof(msg));
		break;
	case JOB_DOWNLOAD:
		ok = ghostnetDownloadNow(msg, sizeof(msg));
		break;
	}

	ghostnetSetResult(ok ? GHOSTNET_OK : GHOSTNET_ERROR, msg);

	return 0;
}

static bool ghostnetStart(s32 job)
{
	if (g_State == GHOSTNET_BUSY) {
		return false;
	}

	if (g_Thread) {
		SDL_WaitThread(g_Thread, NULL);
		g_Thread = NULL;
	}

	g_Job = job;
	ghostnetSetResult(GHOSTNET_BUSY, "talking to the server...");

	g_Thread = SDL_CreateThread(ghostnetWorker, "pd-ghostnet", NULL);

	if (g_Thread == NULL) {
		ghostnetSetResult(GHOSTNET_ERROR, "could not start the network thread");
		return false;
	}

	return true;
}

bool ghostnetIsAvailable(void)
{
	return true;
}

const char *ghostnetGetAccountName(void)
{
	return g_GhostNetUser;
}

static char *ghostnetSlotUser(s32 index)
{
	return index == 0 ? g_GhostNetUser : g_GhostNetSavedUser[index - 1];
}

static char *ghostnetSlotPin(s32 index)
{
	return index == 0 ? g_GhostNetPin : g_GhostNetSavedPin[index - 1];
}

/**
 * How many slots the chooser lists: the ones with a name in them, plus the
 * first empty one if there is room, which is the row that means "another".
 */
s32 ghostnetGetNumAccounts(void)
{
	s32 i;

	for (i = 0; i < GHOSTNET_MAXACCOUNTS; i++) {
		if (ghostnetSlotUser(i)[0] == '\0') {
			return i;
		}
	}

	return GHOSTNET_MAXACCOUNTS;
}

const char *ghostnetGetAccountAt(s32 index)
{
	if (index < 0 || index >= GHOSTNET_MAXACCOUNTS) {
		return "";
	}

	return ghostnetSlotUser(index);
}

/**
 * Make a remembered account the active one and prove it against the server.
 *
 * The swap is the whole of the local work; the login that follows is what says
 * whether the PIN still opens that account. Choosing an account offline leaves
 * it selected with an error on screen, which is the right outcome: recording
 * carries on and stamps runs with the account the player chose, and the
 * uploading those runs will need is the thing that was never going to work
 * without a network anyway.
 */
void ghostnetSelectAccount(s32 index)
{
	char user[GHOSTNET_MAXUSER + 2];
	char pin[GHOSTNET_MAXPIN + 2];

	if (index <= 0 || index >= GHOSTNET_MAXACCOUNTS || ghostnetSlotUser(index)[0] == '\0') {
		return;
	}

	snprintf(user, sizeof(user), "%s", g_GhostNetUser);
	snprintf(pin, sizeof(pin), "%s", g_GhostNetPin);

	snprintf(g_GhostNetUser, sizeof(g_GhostNetUser), "%s", ghostnetSlotUser(index));
	snprintf(g_GhostNetPin, sizeof(g_GhostNetPin), "%s", ghostnetSlotPin(index));

	snprintf(ghostnetSlotUser(index), GHOSTNET_MAXUSER + 2, "%s", user);
	snprintf(ghostnetSlotPin(index), GHOSTNET_MAXPIN + 2, "%s", pin);

	if (ghostnetIsAvailable()) {
		ghostnetLogin();
	}
}

/**
 * Put the active account aside and clear the fields for a new one.
 *
 * Without this, making a second account would type over the first and the
 * player would find they had signed out of something they never left. If every
 * slot is full the oldest remembered one goes, which is the only slot that can
 * go without losing what is in front of the player.
 */
void ghostnetBeginNewAccount(void)
{
	s32 i;

	if (g_GhostNetUser[0]) {
		for (i = GHOSTNET_MAXACCOUNTS - 1; i > 1; i--) {
			snprintf(ghostnetSlotUser(i), GHOSTNET_MAXUSER + 2, "%s", ghostnetSlotUser(i - 1));
			snprintf(ghostnetSlotPin(i), GHOSTNET_MAXPIN + 2, "%s", ghostnetSlotPin(i - 1));
		}

		snprintf(ghostnetSlotUser(1), GHOSTNET_MAXUSER + 2, "%s", g_GhostNetUser);
		snprintf(ghostnetSlotPin(1), GHOSTNET_MAXPIN + 2, "%s", g_GhostNetPin);
	}

	g_GhostNetUser[0] = '\0';
	g_GhostNetPin[0] = '\0';
}

void ghostnetInit(void)
{
	g_Lock = SDL_CreateMutex();
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

void ghostnetShutdown(void)
{
	if (g_Thread) {
		SDL_WaitThread(g_Thread, NULL);
		g_Thread = NULL;
	}

	if (g_Lock) {
		SDL_DestroyMutex(g_Lock);
		g_Lock = NULL;
	}

	curl_global_cleanup();
}

void ghostnetRegister(void)
{
	ghostnetStart(JOB_REGISTER);
}

void ghostnetLogin(void)
{
	ghostnetStart(JOB_LOGIN);
}

void ghostnetUploadMine(void)
{
	ghostnetStart(JOB_UPLOAD);
}

void ghostnetFetchBoard(s32 stagenum, s32 difficulty)
{
	g_JobStage = stagenum;
	g_JobDiff = difficulty;
	ghostnetStart(JOB_BOARD);
}

void ghostnetDownload(s32 index)
{
	struct ghostboardentry *entry = ghostnetGetBoardEntry(index);

	if (entry == NULL) {
		return;
	}

	g_JobId = entry->id;

	// Named the way a locally recorded ghost is named, time and all, so the
	// chooser lists it beside everything else. The time matters here because a
	// board holds a hundred rows without caring how many of them one player
	// owns: two of somebody's runs downloaded from the same board are two
	// files, and without the time the second would land on the first and the
	// list would show one row where the player asked for two.
	snprintf(g_JobFile, sizeof(g_JobFile), MODGHOST_DIR "/pd-s%02d-d%d-%s-%06u" MODGHOST_EXT,
			g_BoardStage, g_BoardDiff, entry->user, entry->time60);

	ghostnetStart(JOB_DOWNLOAD);
}

s32 ghostnetGetState(void)
{
	s32 state;

	SDL_LockMutex(g_Lock);
	state = g_State;
	SDL_UnlockMutex(g_Lock);

	return state;
}

const char *ghostnetGetMessage(void)
{
	return g_Message;
}

#else // PD_HAVE_CURL

bool ghostnetIsAvailable(void) { return false; }
const char *ghostnetGetAccountName(void) { return ""; }
void ghostnetInit(void) {}
void ghostnetShutdown(void) {}
void ghostnetRegister(void) {}
void ghostnetLogin(void) {}
void ghostnetUploadMine(void) {}
void ghostnetFetchBoard(s32 stagenum, s32 difficulty) {}
void ghostnetDownload(s32 index) {}
void ghostnetClearBoard(void) {}
s32 ghostnetGetNumAccounts(void) { return g_GhostNetUser[0] ? 1 : 0; }
const char *ghostnetGetAccountAt(s32 index) { return index == 0 ? g_GhostNetUser : ""; }
void ghostnetSelectAccount(s32 index) {}
void ghostnetBeginNewAccount(void) { g_GhostNetUser[0] = '\0'; g_GhostNetPin[0] = '\0'; }
s32 ghostnetGetState(void) { return GHOSTNET_IDLE; }

const char *ghostnetGetMessage(void)
{
	return "this build has no network support";
}

#endif

bool ghostnetHasAccount(void)
{
	return g_GhostNetUser[0] != '\0' && g_GhostNetPin[0] != '\0';
}

void ghostnetClearState(void)
{
	g_State = GHOSTNET_IDLE;
	g_Message[0] = '\0';
}

/**
 * Forget the board that is held.
 *
 * Called when the mission or difficulty selection changes, because the rows on
 * screen belong to whatever was last fetched and showing Villa's times under
 * the word Chicago is worse than showing none. The alternative - refetching on
 * every change - turns scrolling a dropdown into twenty requests.
 */
void ghostnetClearBoard(void)
{
	g_BoardCount = 0;
	g_BoardStage = -1;
	g_BoardDiff = -1;
}

s32 ghostnetGetBoardCount(void)
{
	return g_BoardCount;
}

s32 ghostnetGetBoardStage(void)
{
	return g_BoardStage;
}

s32 ghostnetGetBoardDifficulty(void)
{
	return g_BoardDiff;
}

struct ghostboardentry *ghostnetGetBoardEntry(s32 index)
{
	if (index < 0 || index >= g_BoardCount) {
		return NULL;
	}

	return &g_Board[index];
}
