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
// biggest pack in circulation is about 420MB (v0.10 of the XBLA Plus pack);
// this is room for one twice that and a refusal for anything that could only
// be a mistake.
#define COMMUNITY_MAXBYTES (800u * 1024u * 1024u)

// The download, under a name the pack lister skips - it starts with a dot -
// so a transfer that was interrupted is never mistaken for an installed pack.
#define COMMUNITY_TMPNAME ".community-download"

#define COMMUNITY_JOB_NONE  0
#define COMMUNITY_JOB_CHECK 1
#define COMMUNITY_JOB_GET   2

#define COMMUNITY_PLUSHD_REPO "retro-foundry/Perfect-Dark-Plus-HD-Textures"

/**
 * One pack in the catalogue.
 *
 * Everything here is about the pack rather than about any release of it: the
 * version, the size, the file name and the URL all come from the release page
 * when the player asks, because a table in the binary is out of date the day
 * after it ships and a player on last month's build should still get this
 * month's pack.
 *
 * `match` is how the one file for this pack is picked out of a release that
 * has several - since v0.10 the Plus HD release is three packs side by side,
 * and v0.09 carried a Quest build beside its one - and `avoid` is how the ones
 * that look like it are ruled out. Both are substrings of the asset's name,
 * matched without case.
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
	// than a file name: "PD Ultimate Plus HD v0.10", not
	// "PD.Ultimate.Plus.HD.v0.10".
	const char *folder;
	// The pack's images are in N64 row order, as everything built for an
	// emulator or the VR fork is, and the marker file goes in as it is
	// installed - see the row order note in CLAUDE-notes/texture-packs.md.
	// Getting it wrong is a pack that is entirely upside down, both ways: the
	// Plus HD packs were in N64 order up to v0.09 and are stored the right way
	// up from v0.10, xbla folder and fonts included.
	s32 bottomUp;
	const u8 *thumbPng;
	const u32 *thumbLen;
	struct menuimage thumb;
};

/**
 * What the last ask found for one pack. Filled by the worker into `pending`
 * and copied over `releases` by communityTick() on the game thread, which is
 * the only thread the menu reads it from - so nothing here needs the lock.
 */
struct communityrelease {
	s32 found;
	u32 size;
	char version[32];
	char assetName[192];
	char assetUrl[512];
	char assetSha[65];
	char installName[96];
	char err[96]; // why this pack has nothing to offer, when the ask as a whole worked
};

extern const u8 g_MenuImageUltimatePlusHd[];
extern const u32 g_MenuImageUltimatePlusHdLen;
extern const u8 g_MenuImageXblaPlusHd[];
extern const u32 g_MenuImageXblaPlusHdLen;
extern const u8 g_MenuImageForeverPlusHd[];
extern const u32 g_MenuImageForeverPlusHdLen;

// In the order the menu's pages swipe, the recommended one first, because the
// first is the page Community Packs opens on. communitymenu.c has a page for
// each and names them in the same order.
static struct communitypack packs[] = {
	{
		"PD Ultimate Plus HD",
		"Parabolee of Retro Foundry",
		"The recommended pack, for the XBLA models: the\n"
		"release's textures upscaled and redrawn closer to\n"
		"the original art, hundreds by hand. Fonts by Trov.\n",
		COMMUNITY_PLUSHD_REPO,
		"ULTIMATE",
		"QUEST",
		"PD Ultimate Plus HD",
		0,
		g_MenuImageUltimatePlusHd,
		&g_MenuImageUltimatePlusHdLen,
		{ 0 },
	},
	{
		"XBLA Plus HD",
		"Parabolee of Retro Foundry",
		"For the XBLA models: the release's own textures,\n"
		"upscaled and enhanced and kept faithful to the Xbox\n"
		"360 look. Fonts by Trov.\n",
		COMMUNITY_PLUSHD_REPO,
		"XBLA.PLUS",
		"QUEST",
		"XBLA Plus HD",
		0,
		g_MenuImageXblaPlusHd,
		&g_MenuImageXblaPlusHdLen,
		{ 0 },
	},
	{
		"PD Forever Plus HD",
		"Howard Phillips",
		"Faithful to the N64 art, for play without the XBLA\n"
		"models. Converted and completed by Rafccq, Enigmata,\n"
		"Atari-Dude and Parabolee. Fonts by Trov.\n",
		COMMUNITY_PLUSHD_REPO,
		"FOREVER",
		"QUEST",
		"PD Forever Plus HD",
		0,
		g_MenuImageForeverPlusHd,
		&g_MenuImageForeverPlusHdLen,
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

// Which pack a download, an install or its failure belongs to; -1 while the
// job is an ask, which is every pack's.
static s32 jobPack = -1;

static char status[192];
// Which page the status line is for: -1 is every page (an ask, and its
// failure), otherwise the pack jobPack named when the job started.
static s32 statusPack = -1;

static struct communityrelease releases[COMMUNITY_NUMPACKS];
static struct communityrelease pending[COMMUNITY_NUMPACKS];

// Where packs are installed to, asked for on the game thread before the worker
// starts: it is a lazily filled static in texpack.c and two threads arriving
// at it together would race.
static char packsDir[FS_MAXPATH + 1];

// The reply as it arrives. len is counted up by the transport on the worker
// and read by the menu, which is a race with nothing at stake - the answer is
// a number on a screen and a stale one is last frame's.
static struct ghostnetbuf download = { NULL, 0, NULL, 0 };

static volatile bool cancelFlag;

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

static s32 communityClampIndex(s32 index)
{
	return index < 0 || index >= COMMUNITY_NUMPACKS ? 0 : index;
}

static struct communitypack *communityPack(s32 index)
{
	return &packs[communityClampIndex(index)];
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

static s32 communityBusy(void)
{
	return state == COMMUNITY_ASKING || state == COMMUNITY_DOWNLOAD || state == COMMUNITY_UNPACKING;
}

s32 communityGetState(s32 index)
{
	index = communityClampIndex(index);

	if (state == COMMUNITY_ASKING) {
		return COMMUNITY_ASKING;
	}

	// One download at a time, and it is only the page of the pack it is for
	// that shows its progress: every page is drawn during a swipe.
	if (state == COMMUNITY_DOWNLOAD || state == COMMUNITY_UNPACKING) {
		return jobPack == index ? state : COMMUNITY_ELSEWHERE;
	}

	if (state == COMMUNITY_DONE && jobPack == index) {
		return COMMUNITY_DONE;
	}

	if (state == COMMUNITY_ERROR && (statusPack < 0 || statusPack == index)) {
		return COMMUNITY_ERROR;
	}

	if (releases[index].found) {
		return COMMUNITY_FOUND;
	}

	if (releases[index].err[0]) {
		return COMMUNITY_ERROR;
	}

	return COMMUNITY_IDLE;
}

const char *communityGetStatus(s32 index)
{
	static char text[192];
	const s32 packstate = communityGetState(index);

	index = communityClampIndex(index);

	if (packstate == COMMUNITY_ELSEWHERE) {
		snprintf(text, sizeof(text), "Installing %s - one pack at a time", communityGetName(jobPack));
		return text;
	}

	if (status[0] && (statusPack < 0 || statusPack == index)) {
		return status;
	}

	if (packstate == COMMUNITY_FOUND) {
		snprintf(text, sizeof(text), "%s is the latest release", releases[index].version);
		return text;
	}

	if (packstate == COMMUNITY_ERROR && releases[index].err[0]) {
		return releases[index].err;
	}

	return "";
}

const char *communityGetVersion(s32 index) { return releases[communityClampIndex(index)].version; }
u32 communityGetSize(s32 index) { return releases[communityClampIndex(index)].size; }

s32 communityGetActivePack(void)
{
	return state == COMMUNITY_DOWNLOAD || state == COMMUNITY_UNPACKING ? jobPack : -1;
}

void communityGetProgress(u32 *done, u32 *total)
{
	*done = (u32)download.len;
	*total = jobPack >= 0 ? releases[jobPack].size : 0;
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
 * "Beta_Release_V0.10" - where the file inside it is named after the pack and
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

static s32 communityRepoPackCount(const char *repo)
{
	s32 count = 0;
	s32 i;

	for (i = 0; i < COMMUNITY_NUMPACKS; i++) {
		count += !strcmp(packs[i].repo, repo);
	}

	return count;
}

/**
 * Which file in a release is this pack's.
 *
 * If nothing is named for it and the pack is the only one the catalogue takes
 * from this release, the largest archive is taken, so a rename is a wrong guess
 * rather than a dead page. Where a release holds several of the catalogue's
 * packs that guess would be another pack's file installed under this one's
 * name, so there it is refused instead.
 */
static bool communityPickAsset(const struct communitypack *pack, const char *json, const char *tag,
		struct communityrelease *out, char *err, u32 errsize)
{
	char bestName[192] = { 0 };
	char bestUrl[512] = { 0 };
	char bestSha[80] = { 0 };
	u32 bestSize = 0;
	s32 bestMatched = 0;
	const char *at = strstr(json, "\"assets\"");

	if (at == NULL) {
		snprintf(err, errsize, "the release has no files");
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

	if (bestName[0] == '\0') {
		snprintf(err, errsize, "no file in the latest release is one this can install");
		return false;
	}

	if (!bestMatched) {
		if (communityRepoPackCount(pack->repo) > 1) {
			sysLogPrintf(LOG_WARNING, "community: nothing in %s matches \"%s\"", pack->repo,
					pack->match ? pack->match : "");
			snprintf(err, errsize, "the latest release does not have this pack in it");
			return false;
		}

		// Worth saying: the catalogue's guess at which file is ours no longer
		// finds anything, so what is about to be offered is the biggest
		// archive in the release rather than a file anybody named.
		sysLogPrintf(LOG_NOTE, "community: nothing in %s matches \"%s\", taking %s",
				pack->repo, pack->match ? pack->match : "", bestName);
	}

	out->found = 1;
	out->size = bestSize;
	snprintf(out->assetName, sizeof(out->assetName), "%s", bestName);
	snprintf(out->assetUrl, sizeof(out->assetUrl), "%s", bestUrl);
	snprintf(out->assetSha, sizeof(out->assetSha), "%s", bestSha);
	communityVersionFromName(bestName, tag, out->version, sizeof(out->version));
	snprintf(out->installName, sizeof(out->installName), "%s %s", pack->folder, out->version);
	communitySanitise(out->installName);

	sysLogPrintf(LOG_NOTE, "community: %s %s is %s, %u bytes%s", pack->name, out->version,
			bestName, bestSize, bestSha[0] ? "" : " (no hash published)");

	return true;
}

/**
 * Ask every release page in the catalogue which release is current, and which
 * file in it is each pack's - once per page rather than once per pack, since
 * one release can hold several packs and GitHub counts every question.
 */
static bool communityResolve(char *err, u32 errsize)
{
	s32 asked[COMMUNITY_NUMPACKS] = { 0 };
	s32 numfound = 0;
	s32 i;
	s32 j;

	err[0] = '\0';
	memset(pending, 0, sizeof(pending));

	for (i = 0; i < COMMUNITY_NUMPACKS; i++) {
		struct ghostnetbuf buf = { NULL, 0, NULL, 0 };
		struct ghostnetreq req;
		char url[320];
		char tag[64] = { 0 };
		char msg[96] = { 0 };
		s32 status_ = 0;
		bool ok;

		if (asked[i]) {
			continue;
		}

		snprintf(url, sizeof(url), COMMUNITY_API, packs[i].repo);

		memset(&req, 0, sizeof(req));
		req.url = url;
		req.redirect = true;
		req.cancel = &cancelFlag;

		ok = ghostnetSend(&req, &buf, &status_, msg, sizeof(msg));

		if (ok && (status_ != 200 || buf.data == NULL || buf.len == 0)) {
			snprintf(msg, sizeof(msg), status_ == 404
					? "that pack has no releases yet"
					: "the release page answered %d", status_);
			ok = false;
		}

		if (ok) {
			ghostnetJsonField(buf.data, NULL, "tag_name", tag, sizeof(tag));
		}

		for (j = i; j < COMMUNITY_NUMPACKS; j++) {
			if (strcmp(packs[j].repo, packs[i].repo) != 0) {
				continue;
			}

			asked[j] = 1;

			if (!ok) {
				snprintf(pending[j].err, sizeof(pending[j].err), "%s", msg);
			} else if (communityPickAsset(&packs[j], buf.data, tag, &pending[j],
						pending[j].err, sizeof(pending[j].err))) {
				numfound++;
			}

			if (!pending[j].found && err[0] == '\0') {
				snprintf(err, errsize, "%s", pending[j].err);
			}
		}

		free(buf.data);

		if (cancelFlag) {
			return false;
		}
	}

	return numfound > 0;
}

/**
 * Download it, check it is what was promised, and unpack it into the pack
 * folder.
 *
 * The order is update.c's and for the same reason: everything that can fail
 * happens to a file under a name nothing else reads, and the pack folder only
 * gains a pack once there is one to gain.
 */
static bool communityFetch(const struct communitypack *pack, const struct communityrelease *rel,
		char *msg, u32 msgsize)
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

	ext = strrchr(rel->assetName, '.');
	snprintf(tmp, sizeof(tmp), "%s/%s%s", packsDir, COMMUNITY_TMPNAME, ext ? ext : ".zip");
	snprintf(dest, sizeof(dest), "%s/%s", packsDir, rel->installName);

	f = fopen(tmp, "wb");

	if (f == NULL) {
		snprintf(msg, msgsize, "cannot write to the texture-packs folder");
		return false;
	}

	memset(&req, 0, sizeof(req));
	req.url = rel->assetUrl;
	req.redirect = true;
	req.timeout = COMMUNITY_DOWNLOADTIMEOUT;
	req.cancel = &cancelFlag;

	download.data = NULL;
	download.len = 0;
	download.sink = f;
	// The release said how big it is, so anything past that is not the file
	// and is refused as it arrives rather than written to disk first.
	download.maxlen = rel->size;

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

	if (download.len != rel->size) {
		snprintf(msg, msgsize, "the download stopped early (%u of %u MB)",
				(u32)(download.len / 1048576), rel->size / 1048576);
		remove(tmp);
		return false;
	}

	if (rel->assetSha[0]) {
		if (!sha256File(tmp, sha)) {
			snprintf(msg, msgsize, "could not read back what was downloaded");
			remove(tmp);
			return false;
		}

		if (strcasecmp(sha, rel->assetSha) != 0) {
			snprintf(msg, msgsize, "the download is not the file the release describes");
			remove(tmp);
			return false;
		}
	}

	SDL_AtomicSet(&workerStage, COMMUNITY_UNPACKING);
	communitySetStatus("Unpacking %s - this takes a minute", rel->assetName);

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
	char msg[192];
	bool ok;

	msg[0] = '\0';

	if (job == COMMUNITY_JOB_CHECK) {
		ok = communityResolve(msg, sizeof(msg));
	} else {
		ok = communityFetch(communityPack(jobPack), &releases[jobPack], msg, sizeof(msg));
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

static void communityStart(s32 which, s32 pack)
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
	jobPack = pack;
	statusPack = pack;
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
	// Every opening of the page, not only the first: the answer is a release
	// that somebody else moves, so this is also how "Installed" goes back to
	// being a version number when a newer release turns up.
	communityStart(COMMUNITY_JOB_CHECK, -1);
}

void communityInstall(s32 index)
{
	if (index < 0 || index >= COMMUNITY_NUMPACKS || !releases[index].found) {
		return;
	}

	communityStart(COMMUNITY_JOB_GET, index);
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

	if (job == COMMUNITY_JOB_CHECK) {
		// An ask that failed leaves nothing on offer: what was found before is
		// an answer to a question that has since been asked again.
		if (SDL_AtomicGet(&workerDone) < 0) {
			memset(releases, 0, sizeof(releases));
		} else {
			memcpy(releases, pending, sizeof(releases));
			status[0] = '\0';
		}
	}

	if (SDL_AtomicGet(&workerDone) < 0) {
		state = cancelFlag ? COMMUNITY_IDLE : COMMUNITY_ERROR;

		if (cancelFlag) {
			communitySetStatus("Stopped");
		}

		return;
	}

	if (job == COMMUNITY_JOB_CHECK) {
		state = COMMUNITY_IDLE;
		return;
	}

	// The files are on disk. What is left is the pack loader's, and the pack
	// loader is this thread's.
	state = COMMUNITY_DONE;

	if (!texpackSelectPackByName(releases[jobPack].installName)) {
		communitySetStatus("Installed, but it is not in the pack list - see the log");
		sysLogPrintf(LOG_ERROR, "community: %s installed but did not list", releases[jobPack].installName);
		return;
	}

	if (!texpackLoadEnabled()) {
		texpackSetLoadEnabled(1);
	}

	communitySetStatus("Installed - %s is selected", releases[jobPack].installName);
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
