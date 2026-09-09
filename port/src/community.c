/**
 * Community Packs - texture packs installed from inside the game.
 * See community.h for what this is and why; this file is the how.
 *
 * The shape is update.c's, because the problem is update.c's: something on the
 * far side of the internet takes its time and the menu has to stay drawable
 * while it does. One worker, one job, a mutex around the result, and a page
 * that polls. What is different is the last step - a build replaces itself,
 * where a pack has to be unpacked and handed to the texture pack loader, and
 * that half belongs to the game thread. So the worker stops at "the files are
 * on disk" and communityTick() does the rest.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <SDL2/SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "types.h"
#include "fs.h"
#include "system.h"
#include "archive.h"
#include "ghostnet.h"
#include "sha256.h"
#include "texpack.h"
#include "menuimage.h"
#include "community.h"

// GitHub's own answer to "which release is current". Asked once per page
// opening rather than at startup: nobody who never opens the page should be
// making requests, and a release list read at startup would be stale by the
// time it was looked at anyway.
#define COMMUNITY_API "https://api.github.com/repos/%s/releases/latest"

// The pack's own page, said on the menu so that somebody who would rather
// install it by hand - or wants to see what they are about to download - has
// somewhere to go.
// Without the scheme, and without /releases on the end: it is read off a
// television and typed into a browser, and the window is not wide enough for
// the whole of it - the menu breaks it at the last slash.
#define COMMUNITY_PAGE "github.com/%s"

// A pack is hundreds of megabytes over whatever line the player has.
#define COMMUNITY_DOWNLOADTIMEOUT 3600

// A bound on what a release may claim before anything is downloaded. The
// biggest pack in circulation is under 200MB; this is room for one four times
// that and a refusal for anything that could only be a mistake.
#define COMMUNITY_MAXBYTES (800u * 1024u * 1024u)

// The download, under a name the pack lister skips - it starts with a dot -
// so a transfer that was interrupted is never mistaken for an installed pack.
#define COMMUNITY_TMPNAME ".community-download"

#define COMMUNITY_JOB_NONE  0
#define COMMUNITY_JOB_CHECK 1
#define COMMUNITY_JOB_GET   2

/**
 * One pack in the catalogue.
 *
 * Everything here is about the pack rather than about any release of it: the
 * version, the size, the file name and the URL all come from the release page
 * when the player asks, because a table in the binary is out of date the day
 * after it ships and a player on last month's build should still get this
 * month's pack.
 *
 * `match` is how the one file for this port is picked out of a release that
 * has several - the PD Plus release carries a Quest build and a spare set of
 * textures beside the pack itself - and `avoid` is how the ones that look like
 * it are ruled out. Both are substrings of the asset's name, matched without
 * case. If neither finds anything the largest archive in the release is taken,
 * so a rename is a wrong guess rather than a dead page.
 */
struct communitypack {
	const char *name;
	const char *author;
	const char *blurb;
	const char *repo;
	const char *match;
	const char *avoid;
	// What the installed folder is called, with the version after it. The
	// player sees this in the texture pack dropdown, so it is a name rather
	// than a file name: "PD Plus HD v0.09d", not
	// "PD.PLUS.HD.TEXTURE.PACK.v0.09d".
	const char *folder;
	// The pack's images are in N64 row order, as everything built for an
	// emulator or the VR fork is. A folder called ext_tex says so by itself
	// and this pack's no longer is - see the row order note in
	// CLAUDE-notes/texture-packs.md - so the marker file goes in as it is
	// installed. Getting it wrong is a pack that is entirely upside down.
	s32 bottomUp;
	const u8 *thumbPng;
	const u32 *thumbLen;
	struct menuimage thumb;
};

extern const u8 g_MenuImagePdPlusHd[];
extern const u32 g_MenuImagePdPlusHdLen;

static struct communitypack packs[] = {
	{
		"PD Plus HD Textures",
		"Parabolee of Retro Foundry",
		"The XBLA release's textures upscaled, several hundred\n"
		"drawn by hand, and the gaps filled from Howardphilips'\n"
		"and Enndee's packs. Fonts by Trov.\n",
		"retro-foundry/Perfect-Dark-Plus-HD-Textures",
		"TEXTURE.PACK",
		"QUEST",
		"PD Plus HD",
		1,
		g_MenuImagePdPlusHd,
		&g_MenuImagePdPlusHdLen,
		{ 0 },
	},
};

#define COMMUNITY_NUMPACKS ((s32)(sizeof(packs) / sizeof(packs[0])))

static SDL_mutex *lock;
static SDL_Thread *worker;
static SDL_atomic_t workerDone;
// What the worker is doing, for the tick to mirror into state. The worker
// cannot write state itself: the menu reads it every frame on the other
// thread, and the two would disagree about which of them is in charge of it.
static SDL_atomic_t workerStage;
static s32 job;
static s32 state = COMMUNITY_IDLE;
static s32 selected;
static char status[192];

// What the ask found. Written on the worker under the lock, read by the menu.
static char version[32];
static char assetName[192];
static char assetUrl[512];
static char assetSha[65];
static char installName[96];
static u32 assetSize;

// Which entry the answer above belongs to. Selecting another pack in the menu
// is a different question, and an answer to the previous one shown against it
// would offer to install one pack under another's name.
static s32 resolvedFor = -1;

// Where packs are installed to, asked for on the game thread before the worker
// starts: it is a lazily filled static in texpack.c and two threads arriving
// at it together would race.
static char packsDir[FS_MAXPATH + 1];

// The reply as it arrives. len is counted up by the transport on the worker
// and read by the menu, which is a race with nothing at stake - the answer is
// a number on a screen and a stale one is last frame's.
static struct ghostnetbuf download = { NULL, 0, NULL, 0 };

static volatile bool cancelFlag;

// Raised by the worker when the files are in place, lowered by communityTick()
// once the pack loader has been told about them. The handover is a flag rather
// than a call because everything the loader does - rescanning the folder,
// dropping the renderer's texture cache - belongs to the game thread.
static SDL_atomic_t pendingSelect;

PD_CONSTRUCTOR static void communityInit(void)
{
	s32 i;

	lock = SDL_CreateMutex();

	for (i = 0; i < COMMUNITY_NUMPACKS; i++) {
		packs[i].thumb.png = packs[i].thumbPng;
		packs[i].thumb.pnglen = packs[i].thumbLen ? *packs[i].thumbLen : 0;
		packs[i].thumb.name = packs[i].name;

		menuImageRegister(&packs[i].thumb);
	}
}

static void communitySetStatus(const char *fmt, ...)
{
	va_list args;

	SDL_LockMutex(lock);
	va_start(args, fmt);
	vsnprintf(status, sizeof(status), fmt, args);
	va_end(args);
	SDL_UnlockMutex(lock);
}

bool communityIsAvailable(void)
{
	return ghostnetIsAvailable();
}

s32 communityGetNumPacks(void) { return COMMUNITY_NUMPACKS; }
s32 communityGetSelected(void) { return selected; }

static struct communitypack *communityPack(s32 index)
{
	if (index < 0 || index >= COMMUNITY_NUMPACKS) {
		index = 0;
	}

	return &packs[index];
}

const char *communityGetName(s32 index) { return communityPack(index)->name; }
const char *communityGetAuthor(s32 index) { return communityPack(index)->author; }
const char *communityGetBlurb(s32 index) { return communityPack(index)->blurb; }
struct menuimage *communityGetThumb(s32 index) { return &communityPack(index)->thumb; }

const char *communityGetSource(s32 index)
{
	static char text[160];

	snprintf(text, sizeof(text), COMMUNITY_PAGE, communityPack(index)->repo);

	return text;
}

s32 communityGetState(void) { return state; }
const char *communityGetStatus(void) { return status; }
const char *communityGetVersion(void) { return version; }
u32 communityGetSize(void) { return assetSize; }

void communityGetProgress(u32 *done, u32 *total)
{
	*done = (u32)download.len;
	*total = assetSize;
}

static s32 communityBusy(void)
{
	return state == COMMUNITY_ASKING || state == COMMUNITY_DOWNLOAD || state == COMMUNITY_UNPACKING;
}

void communitySetSelected(s32 index)
{
	if (index < 0 || index >= COMMUNITY_NUMPACKS || index == selected || communityBusy()) {
		return;
	}

	selected = index;

	// Whatever was found belongs to the pack it was asked about.
	if (state != COMMUNITY_DONE || resolvedFor != index) {
		state = COMMUNITY_IDLE;
		status[0] = '\0';
	}
}

/**
 * Whether some version of this pack is already installed.
 *
 * By the folder prefix rather than the exact name, because the version is part
 * of the name and the point of this answer is "you already have this pack" -
 * a player with v0.08 installed being offered v0.09 is right, and being told
 * they have nothing is not.
 */
s32 communityIsInstalled(s32 index)
{
	const struct communitypack *pack = communityPack(index);
	const u32 len = (u32)strlen(pack->folder);
	s32 i;

	for (i = 0; i < texpackGetNumPacks(); i++) {
		if (!strncasecmp(texpackGetPackName(i), pack->folder, len)) {
			return 1;
		}
	}

	return 0;
}

/**
 * The bounds of the next object in a JSON array.
 *
 * ghostnetJsonField() reads one flat object and a release's assets are a list
 * of them, so something has to say where each one starts and stops. Braces at
 * depth one, with strings skipped - an asset's name is written by whoever
 * uploaded it and a brace inside one would otherwise end the object early.
 */
static s32 communityNextObject(const char **at, const char **outStart, const char **outEnd)
{
	const char *p = *at;
	const char *start = NULL;
	s32 depth = 0;
	s32 instring = 0;

	for (; *p; p++) {
		if (instring) {
			if (*p == '\\' && p[1]) {
				p++;
			} else if (*p == '"') {
				instring = 0;
			}

			continue;
		}

		switch (*p) {
		case '"':
			instring = 1;
			break;
		case '{':
			if (depth++ == 0) {
				start = p;
			}
			break;
		case '}':
			if (--depth == 0) {
				*outStart = start;
				*outEnd = p + 1;
				*at = p + 1;
				return 1;
			}
			break;
		case ']':
			if (depth == 0) {
				return 0;
			}
			break;
		}
	}

	return 0;
}

static s32 communityContains(const char *haystack, const char *needle)
{
	const u32 len = needle ? (u32)strlen(needle) : 0;
	const char *p;

	if (len == 0) {
		return 0;
	}

	for (p = haystack; *p; p++) {
		if (!strncasecmp(p, needle, len)) {
			return 1;
		}
	}

	return 0;
}

/**
 * The version, out of the file's own name.
 *
 * A release tag is whatever its author felt like typing - this one's is
 * "Beta_Release_V0.09" - where the file inside it is named after the pack and
 * ends in the version, which is the part a player recognises. So the name is
 * preferred and the tag is the fallback.
 */
static void communityVersionFromName(const char *name, const char *tag, char *out, u32 outsize)
{
	static const char *exts[] = { ".zip", ".7z", ".rar", ".pk3" };
	const char *p;

	for (p = name; *p; p++) {
		u32 len;
		u32 i;

		if ((*p != 'v' && *p != 'V') || p[1] < '0' || p[1] > '9') {
			continue;
		}

		len = (u32)strlen(p);

		if (len >= outsize) {
			len = outsize - 1;
		}

		memcpy(out, p, len);
		out[len] = '\0';

		// The extension is not part of the version, and is the only thing
		// after it: a release names its files for people rather than to a
		// scheme, so what is taken here is "from the v to the end, less .zip".
		for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
			const u32 extlen = (u32)strlen(exts[i]);

			if (len > extlen && !strcasecmp(out + len - extlen, exts[i])) {
				out[len - extlen] = '\0';
				break;
			}
		}

		if (out[1]) {
			return;
		}
	}

	snprintf(out, outsize, "%s", tag);
}

/**
 * Make a name that can be a folder out of one that was written for a person.
 *
 * Only what a pack's name plausibly contains: anything else becomes a dash
 * rather than being dropped, so two names cannot collapse into one.
 */
static void communitySanitise(char *s)
{
	for (; *s; s++) {
		const char c = *s;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
				|| c == ' ' || c == '.' || c == '-' || c == '_') {
			continue;
		}

		*s = '-';
	}
}

/**
 * Ask the release page which release is current, and which file in it is ours.
 */
static bool communityResolve(const struct communitypack *pack, char *err, u32 errsize)
{
	struct ghostnetbuf buf = { NULL, 0, NULL, 0 };
	struct ghostnetreq req;
	char url[320];
	char tag[64] = { 0 };
	char bestName[192] = { 0 };
	char bestUrl[512] = { 0 };
	char bestSha[80] = { 0 };
	u32 bestSize = 0;
	s32 bestMatched = 0;
	const char *at;
	s32 status_ = 0;

	snprintf(url, sizeof(url), COMMUNITY_API, pack->repo);

	memset(&req, 0, sizeof(req));
	req.url = url;
	req.redirect = true;
	req.cancel = &cancelFlag;

	if (!ghostnetSend(&req, &buf, &status_, err, errsize)) {
		free(buf.data);
		return false;
	}

	if (status_ != 200 || buf.data == NULL || buf.len == 0) {
		snprintf(err, errsize, status_ == 404
				? "that pack has no releases yet"
				: "the release page answered %d", status_);
		free(buf.data);
		return false;
	}

	ghostnetJsonField(buf.data, NULL, "tag_name", tag, sizeof(tag));

	at = strstr(buf.data, "\"assets\"");

	if (at == NULL) {
		snprintf(err, errsize, "the release has no files");
		free(buf.data);
		return false;
	}

	for (;;) {
		const char *start;
		const char *end;
		char name[192];
		char num[32];
		char digest[80];
		char link[512];
		u32 size;
		s32 matched;

		if (!communityNextObject(&at, &start, &end)) {
			break;
		}

		if (!ghostnetJsonField(start, end, "name", name, sizeof(name))
				|| !ghostnetJsonField(start, end, "size", num, sizeof(num))
				|| !ghostnetJsonField(start, end, "browser_download_url", link, sizeof(link))) {
			continue;
		}

		size = (u32)strtoul(num, NULL, 10);

		// Only an archive this can open, only over https, and only a size that
		// could be a texture pack. Every one of these is a thing to refuse
		// before three hundred megabytes are written to somebody's disk.
		if (!archiveIsSupported(name) || size == 0 || size > COMMUNITY_MAXBYTES
				|| strncmp(link, "https://", 8) != 0) {
			continue;
		}

		if (pack->avoid && communityContains(name, pack->avoid)) {
			continue;
		}

		matched = pack->match && communityContains(name, pack->match);

		// A named match beats an unnamed one however big it is; among equals
		// the largest wins, which for a release carrying a pack and a patch
		// for it is the pack.
		if (matched < bestMatched || (matched == bestMatched && size <= bestSize)) {
			continue;
		}

		bestMatched = matched;
		bestSize = size;
		snprintf(bestName, sizeof(bestName), "%s", name);
		snprintf(bestUrl, sizeof(bestUrl), "%s", link);

		if (ghostnetJsonField(start, end, "digest", digest, sizeof(digest))
				&& !strncasecmp(digest, "sha256:", 7) && strlen(digest + 7) == 64) {
			snprintf(bestSha, sizeof(bestSha), "%s", digest + 7);
		} else {
			bestSha[0] = '\0';
		}
	}

	free(buf.data);

	if (bestName[0] == '\0') {
		snprintf(err, errsize, "no file in the latest release is one this can install");
		return false;
	}

	if (!bestMatched) {
		// Worth saying: the catalogue's guess at which file is ours no longer
		// finds anything, so what is about to be offered is the biggest
		// archive in the release rather than a file anybody named.
		sysLogPrintf(LOG_NOTE, "community: nothing in %s matches \"%s\", taking %s",
				pack->repo, pack->match ? pack->match : "", bestName);
	}

	SDL_LockMutex(lock);
	snprintf(assetName, sizeof(assetName), "%s", bestName);
	snprintf(assetUrl, sizeof(assetUrl), "%s", bestUrl);
	snprintf(assetSha, sizeof(assetSha), "%s", bestSha);
	assetSize = bestSize;
	communityVersionFromName(bestName, tag, version, sizeof(version));
	snprintf(installName, sizeof(installName), "%s %s", pack->folder, version);
	communitySanitise(installName);
	SDL_UnlockMutex(lock);

	sysLogPrintf(LOG_NOTE, "community: %s %s is %s, %u bytes%s", pack->name, version,
			bestName, bestSize, bestSha[0] ? "" : " (no hash published)");

	return true;
}

/**
 * Download it, check it is what was promised, and unpack it into the pack
 * folder.
 *
 * The order is update.c's and for the same reason: everything that can fail
 * happens to a file under a name nothing else reads, and the pack folder only
 * gains a pack once there is one to gain.
 */
static bool communityFetch(const struct communitypack *pack, char *msg, u32 msgsize)
{
	struct ghostnetreq req;
	char tmp[FS_MAXPATH + 1];
	char dest[FS_MAXPATH + 1];
	char sha[65];
	const char *ext;
	s32 status_ = 0;
	s32 count;
	FILE *f;

	if (packsDir[0] == '\0') {
		snprintf(msg, msgsize, "nowhere to install to that can be written");
		return false;
	}

	ext = strrchr(assetName, '.');
	snprintf(tmp, sizeof(tmp), "%s/%s%s", packsDir, COMMUNITY_TMPNAME, ext ? ext : ".zip");
	snprintf(dest, sizeof(dest), "%s/%s", packsDir, installName);

	f = fopen(tmp, "wb");

	if (f == NULL) {
		snprintf(msg, msgsize, "cannot write to the texture-packs folder");
		return false;
	}

	memset(&req, 0, sizeof(req));
	req.url = assetUrl;
	req.redirect = true;
	req.timeout = COMMUNITY_DOWNLOADTIMEOUT;
	req.cancel = &cancelFlag;

	download.data = NULL;
	download.len = 0;
	download.sink = f;
	// The release said how big it is, so anything past that is not the file
	// and is refused as it arrives rather than written to disk first.
	download.maxlen = assetSize;

	if (!ghostnetSend(&req, &download, &status_, msg, msgsize)) {
		fclose(f);
		remove(tmp);
		return false;
	}

	fclose(f);
	download.sink = NULL;

	if (cancelFlag) {
		snprintf(msg, msgsize, "stopped");
		remove(tmp);
		return false;
	}

	if (status_ != 200) {
		snprintf(msg, msgsize, "the download answered %d", status_);
		remove(tmp);
		return false;
	}

	if (download.len != assetSize) {
		snprintf(msg, msgsize, "the download stopped early (%u of %u MB)",
				(u32)(download.len / 1048576), assetSize / 1048576);
		remove(tmp);
		return false;
	}

	if (assetSha[0]) {
		if (!sha256File(tmp, sha)) {
			snprintf(msg, msgsize, "could not read back what was downloaded");
			remove(tmp);
			return false;
		}

		if (strcasecmp(sha, assetSha) != 0) {
			snprintf(msg, msgsize, "the download is not the file the release describes");
			remove(tmp);
			return false;
		}
	}

	SDL_AtomicSet(&workerStage, COMMUNITY_UNPACKING);
	communitySetStatus("Unpacking %s - this takes a minute", assetName);

	count = archiveExtract(tmp, dest);

	remove(tmp);

	if (count <= 0) {
		snprintf(msg, msgsize, "nothing came out of the archive");
		return false;
	}

	// The row order marker, for a pack whose images are stored the way an
	// emulator wants them. Nothing else says so once the pack is on disk and
	// getting it wrong turns every texture in the game upside down, so it goes
	// in here rather than being left to the player to know about.
	if (pack->bottomUp) {
		char marker[FS_MAXPATH + 1];

		snprintf(marker, sizeof(marker), "%s/bottomup.txt", dest);

		f = fopen(marker, "wb");

		if (f) {
			fprintf(f, "%s is stored in N64 row order, like every pack built for an\n"
					"emulator or the VR fork. This file is what tells the loader not to\n"
					"turn it over. Written by Community Packs; deleting it puts every\n"
					"texture in the game upside down.\n", pack->name);
			fclose(f);
		}
	}

	sysLogPrintf(LOG_NOTE, "community: installed %d files into %s", count, dest);

	return true;
}

static int communityWorker(void *arg)
{
	const struct communitypack *pack = communityPack(selected);
	char msg[192];
	bool ok;

	msg[0] = '\0';

	if (job == COMMUNITY_JOB_CHECK) {
		ok = communityResolve(pack, msg, sizeof(msg));
	} else {
		ok = communityFetch(pack, msg, sizeof(msg));
	}

	if (!ok) {
		SDL_LockMutex(lock);
		snprintf(status, sizeof(status), "%s", msg[0] ? msg : "it did not work - see the log");
		SDL_UnlockMutex(lock);
		SDL_AtomicSet(&workerDone, -1);
	} else {
		SDL_AtomicSet(&workerDone, 1);
	}

	return 0;
}

static void communityStart(s32 which)
{
	if (communityBusy() || worker != NULL || !communityIsAvailable()) {
		return;
	}

	// On this thread, because it is a lazily filled static over in texpack.c.
	{
		const char *dir = texpackGetPacksDirPath();

		snprintf(packsDir, sizeof(packsDir), "%s", dir ? dir : "");
	}

	job = which;
	cancelFlag = false;
	SDL_AtomicSet(&workerStage, which == COMMUNITY_JOB_CHECK ? COMMUNITY_ASKING : COMMUNITY_DOWNLOAD);
	download.len = 0;
	SDL_AtomicSet(&workerDone, 0);

	state = which == COMMUNITY_JOB_CHECK ? COMMUNITY_ASKING : COMMUNITY_DOWNLOAD;
	communitySetStatus(which == COMMUNITY_JOB_CHECK
			? "Asking what the latest release is..."
			: "Downloading...");

	worker = SDL_CreateThread(communityWorker, "pdcommunity", NULL);

	if (worker == NULL) {
		state = COMMUNITY_ERROR;
		communitySetStatus("could not start the download");
	}
}

void communityCheck(void)
{
	if (communityBusy()) {
		return;
	}

	// Whatever was found before is a different question's answer, and an
	// install that finished is over: the page asks again every time it opens,
	// so this is also how "Installed" goes back to being a version number
	// when a newer release turns up.
	resolvedFor = -1;
	version[0] = '\0';
	assetSize = 0;
	communityStart(COMMUNITY_JOB_CHECK);
}

void communityInstall(void)
{
	if (state != COMMUNITY_FOUND || resolvedFor != selected) {
		return;
	}

	communityStart(COMMUNITY_JOB_GET);
}

void communityCancel(void)
{
	if (communityBusy()) {
		cancelFlag = true;
		communitySetStatus("Stopping...");
	}
}

void communityTick(void)
{
	if (!communityBusy()) {
		return;
	}

	if (state == COMMUNITY_DOWNLOAD) {
		// The worker says when it moves on to unpacking; the state the menu
		// draws from is only ever written here.
		const s32 stage = SDL_AtomicGet(&workerStage);

		if (stage == COMMUNITY_UNPACKING) {
			state = COMMUNITY_UNPACKING;
		}
	}

	if (SDL_AtomicGet(&workerDone) == 0) {
		return;
	}

	SDL_WaitThread(worker, NULL);
	worker = NULL;

	if (SDL_AtomicGet(&workerDone) < 0) {
		state = cancelFlag ? COMMUNITY_IDLE : COMMUNITY_ERROR;

		if (cancelFlag) {
			communitySetStatus("Stopped");
		}

		return;
	}

	if (job == COMMUNITY_JOB_CHECK) {
		resolvedFor = selected;
		state = COMMUNITY_FOUND;
		communitySetStatus("%s is the latest release", version);
		return;
	}

	// The files are on disk. What is left is the pack loader's, and the pack
	// loader is this thread's.
	state = COMMUNITY_DONE;

	if (!texpackSelectPackByName(installName)) {
		communitySetStatus("Installed, but it is not in the pack list - see the log");
		sysLogPrintf(LOG_ERROR, "community: %s installed but did not list", installName);
		return;
	}

	if (!texpackLoadEnabled()) {
		texpackSetLoadEnabled(1);
	}

	communitySetStatus("Installed - %s is selected", installName);
}

void communityShutdown(void)
{
	if (worker == NULL) {
		return;
	}

	// A transfer in flight gives up at its next read rather than the game
	// waiting on it, windowless, for as long as the download budget allows.
	cancelFlag = true;
	SDL_WaitThread(worker, NULL);
	worker = NULL;
}
