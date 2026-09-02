// The build is -std=c11, which is strict enough that unistd.h hides readlink().
// As in record.c, this has to come before the first system header.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <PR/ultratypes.h>
#include "platform.h"

// Every system header this needs comes before the project ones, the way
// ghostnet.c orders its own. types.h does `#define bool s32`, and a system
// header that pulls in <stdbool.h> - mach-o/dyld.h does - puts bool back to
// _Bool underneath it. Included after update.h, that made every function
// declared there disagree with its definition below, on macOS only.
#ifdef PLATFORM_WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

#ifdef PLATFORM_OSX
#include <mach-o/dyld.h>
#endif

#include <SDL2/SDL.h>
#include "types.h"
#include "fs.h"
#include "system.h"
#include "ghostnet.h"
#include "update.h"
#include "versioninfo.h"

/**
 * See update.h for what this is. This file is the how.
 *
 * The shape is the ghost client's, because the problem is the same one: a
 * menu that has to stay responsive while something on the far side of the
 * internet takes its time. One worker thread, one job at a time, a mutex
 * around the result, and a menu that polls. The transport is the ghost
 * client's too - it is the port's only HTTP client and there is no reason for
 * a second.
 */

// Where releases are.
//
// Stable does not name a tag: "latest" is GitHub's own redirect to the newest
// release that is not a prerelease, so which release that is stays the
// server's decision rather than becoming a version comparison written here.
// Dev names its tag because the rolling prerelease is one tag that the release
// job keeps moving, and "latest" would never point at it.
#define UPDATE_REPO "https://github.com/DabDavis/perfect-dark-dabs-mod/releases"
#define UPDATE_URL_STABLE UPDATE_REPO "/latest/download"
#define UPDATE_URL_DEV    UPDATE_REPO "/download/dabs-mod-dev"

// The manifest, which is the only thing here that is not a file the CI already
// had a reason to build. It is written by the release job and read by this.
#define UPDATE_MANIFEST "update.txt"

// A whole copy of the game, so minutes rather than the twenty seconds a
// leaderboard gets. The manifest itself keeps the ordinary budget.
#define UPDATE_DOWNLOADTIMEOUT 600

// Bigger than any build has been and small enough that a redirect to something
// else entirely is refused rather than written to disk.
#define UPDATE_MAXBYTES (192 * 1024 * 1024)

#define UPDATE_JOB_NONE    0
#define UPDATE_JOB_CHECK   1
#define UPDATE_JOB_INSTALL 2

/**
 * Where to look instead, when somebody is testing this.
 *
 * Empty means the release URL for this build's channel, which is what every
 * copy in the wild uses. Set, it replaces the whole base URL - which is how
 * the updater gets exercised at all, because the alternative is cutting a real
 * release to find out whether the code that replaces the game works.
 *
 * It is a bigger knob than Mod.GhostServer: that one decides where a PIN goes,
 * this one decides where a program comes from. Anything but loopback gets said
 * out loud in the log for that reason.
 */
char g_UpdateUrl[256] = { 0 };

static SDL_mutex *g_Lock = NULL;
static SDL_Thread *g_Thread = NULL;
static s32 g_Job = UPDATE_JOB_NONE;
static s32 g_State = UPDATE_IDLE;
static char g_Message[160] = { 0 };
static bool g_Staged = false;

// Where the new build was put, remembered rather than worked out again later.
// updateSelfPath() asks the operating system which file this process is running
// out of, and after the swap the honest answer is the one that was moved aside:
// on Linux /proc/self/exe follows the inode through a rename. Recomputing it at
// relaunch would start the build that was just replaced.
static char g_StagedPath[FS_MAXPATH] = { 0 };

// What the last check found, and what an install afterwards acts on. Written
// on the worker under the lock and read by the menu, like everything else here.
static char g_Version[UPDATE_MAXVERSION + 1] = { 0 };
static char g_Commit[UPDATE_MAXCOMMIT + 1] = { 0 };
static char g_Asset[64] = { 0 };
static char g_Sha[65] = { 0 };
static u32 g_Size = 0;

/**
 * SHA-256, because the thing being downloaded is then run.
 *
 * TLS says the bytes came from GitHub and were not changed on the way, which
 * is most of it. What it does not say is that all of them arrived: a
 * connection that dies two thirds of the way through a sixteen megabyte
 * download leaves a file that is a perfectly good prefix of a program, and
 * putting that where the game lives is how an install ends up unable to start
 * with nothing to explain why. The size would catch that one; the hash catches
 * it and everything else, and the release job knows the answer already.
 *
 * The plain FIPS 180-4 implementation, which is short enough that reaching for
 * a dependency to avoid writing it would cost more than it saved.
 */
struct sha256 {
	u32 h[8];
	u64 len;
	u8 block[64];
	u32 fill;
};

static const u32 g_Sha256K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define SHA256_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256Block(struct sha256 *ctx, const u8 *p)
{
	u32 w[64];
	u32 a, b, c, d, e, f, g, h;
	u32 t1, t2;
	s32 i;

	for (i = 0; i < 16; i++) {
		w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
	}

	for (i = 16; i < 64; i++) {
		t1 = SHA256_ROR(w[i - 15], 7) ^ SHA256_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		t2 = SHA256_ROR(w[i - 2], 17) ^ SHA256_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + t1 + w[i - 7] + t2;
	}

	a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
	e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

	for (i = 0; i < 64; i++) {
		t1 = h + (SHA256_ROR(e, 6) ^ SHA256_ROR(e, 11) ^ SHA256_ROR(e, 25)) + ((e & f) ^ (~e & g)) + g_Sha256K[i] + w[i];
		t2 = (SHA256_ROR(a, 2) ^ SHA256_ROR(a, 13) ^ SHA256_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
	ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256Init(struct sha256 *ctx)
{
	ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85; ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
	ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c; ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
	ctx->len = 0;
	ctx->fill = 0;
}

static void sha256Update(struct sha256 *ctx, const void *ptr, u32 len)
{
	const u8 *p = ptr;

	ctx->len += len;

	while (len > 0) {
		u32 take = 64 - ctx->fill;

		if (take > len) {
			take = len;
		}

		memcpy(ctx->block + ctx->fill, p, take);
		ctx->fill += take;
		p += take;
		len -= take;

		if (ctx->fill == 64) {
			sha256Block(ctx, ctx->block);
			ctx->fill = 0;
		}
	}
}

static void sha256Final(struct sha256 *ctx, char *out)
{
	static const char hex[] = "0123456789abcdef";
	u64 bits = ctx->len * 8;
	u8 tail[8];
	u8 pad = 0x80;
	u8 zero = 0;
	s32 i;

	sha256Update(ctx, &pad, 1);

	while (ctx->fill != 56) {
		sha256Update(ctx, &zero, 1);
	}

	for (i = 0; i < 8; i++) {
		tail[i] = (u8)(bits >> (56 - i * 8));
	}

	// Not through sha256Update(), which would count these eight bytes into the
	// length that is being written.
	memcpy(ctx->block + ctx->fill, tail, 8);
	sha256Block(ctx, ctx->block);

	for (i = 0; i < 32; i++) {
		u8 byte = (u8)(ctx->h[i / 4] >> (24 - (i % 4) * 8));
		out[i * 2] = hex[byte >> 4];
		out[i * 2 + 1] = hex[byte & 15];
	}

	out[64] = '\0';
}

/**
 * The hash of a file already on disk, which is the one that matters: the bytes
 * checked are the bytes that will be run, rather than the bytes that were
 * meant to have been written.
 */
static bool updateHashFile(const char *path, char *out)
{
	struct sha256 ctx;
	u8 chunk[16384];
	size_t got;
	FILE *f = fopen(path, "rb");

	if (f == NULL) {
		return false;
	}

	sha256Init(&ctx);

	while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		sha256Update(&ctx, chunk, (u32)got);
	}

	if (ferror(f)) {
		fclose(f);
		return false;
	}

	fclose(f);
	sha256Final(&ctx, out);

	return true;
}

static void updateSetResult(s32 state, const char *msg)
{
	SDL_LockMutex(g_Lock);
	g_State = state;
	snprintf(g_Message, sizeof(g_Message), "%s", msg ? msg : "");
	SDL_UnlockMutex(g_Lock);
}

/**
 * The file this process is running out of, asked of the operating system.
 *
 * Every platform will say, and saying is the only answer that is always right.
 * Building the name instead - the directory the game started from plus the one
 * CMake wrote - was wrong twice over. On Windows CMake appends .exe to
 * OUTPUT_NAME itself, so VERSION_BINNAME is the name without it and the file
 * that was looked for did not exist; and on any platform a player who renamed
 * their copy would have had the running one left alone and a stranger written
 * beside it.
 *
 * The reconstructed name is still the fallback, for a platform with no answer
 * to this question. It is a guess, and the failure it leads to is the one that
 * was reported: nothing to move aside.
 */
static bool updateSelfPath(char *out, u32 outsize)
{
#if defined(PLATFORM_WIN32)
	wchar_t wide[FS_MAXPATH];
	DWORD len = GetModuleFileNameW(NULL, wide, ARRAYCOUNT(wide));

	if (len > 0 && len < ARRAYCOUNT(wide)) {
		if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)outsize, NULL, NULL) > 0) {
			return true;
		}
	}
#elif defined(PLATFORM_OSX)
	uint32_t size = outsize;

	if (_NSGetExecutablePath(out, &size) == 0) {
		return true;
	}
#else
	ssize_t len = readlink("/proc/self/exe", out, outsize - 1);

	if (len > 0) {
		out[len] = '\0';
		return true;
	}
#endif

	{
		char dir[FS_MAXPATH];

		dir[0] = '\0';
		sysGetExecutablePath(dir, sizeof(dir));
		snprintf(out, outsize, "%s/" VERSION_BINNAME, dir);
	}

	return false;
}

/**
 * That path, with a suffix on the end for the copies that stand beside it
 * while the swap happens.
 */
static void updatePath(const char *suffix, char *out, u32 outsize)
{
	char self[FS_MAXPATH];

	updateSelfPath(self, sizeof(self));

	snprintf(out, outsize, "%s%s", self, suffix ? suffix : "");
}

/**
 * Pull one "build" line out of the manifest.
 *
 * The manifest is lines of words rather than JSON because it is written by a
 * shell script in the release job and read by this, and neither end gains
 * anything from braces. One line per target:
 *
 *     build x86_64-linux pd.x86_64-linux <sha256> <size>
 *
 * The target is VERSION_TARGET, which is the same string CMake put in the
 * binary and the release job puts in the manifest, so a build only ever
 * matches the file that was built for it.
 */
static bool updateParseManifest(const char *text, char *err, u32 errsize)
{
	const char *at = text;
	char version[UPDATE_MAXVERSION + 1] = { 0 };
	char commit[UPDATE_MAXCOMMIT + 1] = { 0 };
	char asset[64] = { 0 };
	char sha[65] = { 0 };
	unsigned long size = 0;
	bool found = false;

	while (*at) {
		const char *eol = strchr(at, '\n');
		char line[256];
		u32 len = eol ? (u32)(eol - at) : (u32)strlen(at);
		char target[64];

		if (len >= sizeof(line)) {
			len = sizeof(line) - 1;
		}

		memcpy(line, at, len);
		line[len] = '\0';

		if (sscanf(line, "version %32s", version) == 1) {
			// nothing else to do
		} else if (sscanf(line, "commit %16s", commit) == 1) {
			// nor here
		} else if (!found && sscanf(line, "build %63s %63s %64s %lu", target, asset, sha, &size) == 4) {
			// Only the line for this build is kept, and the rest of the
			// manifest is still read: version and commit are as likely to be
			// written after the builds as before, and stopping at the first
			// match would leave a release describing itself as nameless.
			found = strcmp(target, VERSION_TARGET) == 0;
		}

		if (!eol) {
			break;
		}

		at = eol + 1;
	}

	if (version[0] == '\0' || commit[0] == '\0') {
		snprintf(err, errsize, "the release did not say which version it is");
		return false;
	}

	if (!found) {
		snprintf(err, errsize, "the latest release has no build for " VERSION_TARGET);
		return false;
	}

	if (strlen(sha) != 64 || size == 0 || size > UPDATE_MAXBYTES) {
		snprintf(err, errsize, "the release described a file this cannot be");
		return false;
	}

	SDL_LockMutex(g_Lock);
	snprintf(g_Version, sizeof(g_Version), "%s", version);
	snprintf(g_Commit, sizeof(g_Commit), "%s", commit);
	snprintf(g_Asset, sizeof(g_Asset), "%s", asset);
	snprintf(g_Sha, sizeof(g_Sha), "%s", sha);
	g_Size = (u32)size;
	SDL_UnlockMutex(g_Lock);

	return true;
}

/**
 * The release this build follows, as a URL to fetch files out of.
 */
static const char *updateBaseUrl(void)
{
	if (g_UpdateUrl[0]) {
		return g_UpdateUrl;
	}

	return strcmp(VERSION_CHANNEL, "stable") == 0 ? UPDATE_URL_STABLE : UPDATE_URL_DEV;
}

static bool updateFetchManifest(char *err, u32 errsize)
{
	struct ghostnetbuf buf = { NULL, 0, NULL };
	struct ghostnetreq req;
	char url[512];
	s32 status = 0;
	bool ok;

	snprintf(url, sizeof(url), "%s/%s", updateBaseUrl(), UPDATE_MANIFEST);

	memset(&req, 0, sizeof(req));
	req.url = url;
	req.redirect = true;

	if (!ghostnetSend(&req, &buf, &status, err, errsize)) {
		free(buf.data);
		return false;
	}

	if (status != 200 || buf.len == 0) {
		// 404 is the ordinary answer from a repository that has tags but no
		// release carrying a manifest yet, which is a thing to say plainly
		// rather than an error to blame the network for.
		snprintf(err, errsize, status == 404
				? "there is no release to update to yet"
				: "the update server answered %d", status);
		free(buf.data);
		return false;
	}

	ok = updateParseManifest(buf.data, err, errsize);
	free(buf.data);

	return ok;
}

/**
 * Whether the release the manifest describes is a different build from this
 * one.
 *
 * The commit rather than the version, because the version is a tag and this
 * build may not have been cut from a tag at all - anything built from a branch
 * checkout has a hash and no version. Two builds of the same commit are the
 * same program whatever they were called at the time.
 */
static bool updateIsNewer(void)
{
	u32 mine = (u32)strlen(VERSION_HASH);
	u32 theirs;
	bool differs;

	SDL_LockMutex(g_Lock);
	theirs = (u32)strlen(g_Commit);
	// Whichever is shorter decides how much is compared, because the two ends
	// do not agree on how long a short hash is. `git rev-parse --short` picks
	// its own length out of how many objects the repository has, so the CI
	// runner's fresh clone says 2969ab9 where a working copy that has been
	// fetched into for weeks says 2969ab9a2. They are the same commit, and a
	// straight strcmp told anyone building locally that they were a release
	// behind themselves.
	//
	// A prefix match cannot say one is newer than the other and is not asked
	// to: what it answers is whether the release is a different build, and the
	// only release ever offered is the latest one on this build's channel.
	differs = strncmp(g_Commit, VERSION_HASH, mine < theirs ? mine : theirs) != 0;
	SDL_UnlockMutex(g_Lock);

	return differs;
}

/**
 * Fetch the new build, check it, and put it where this one is.
 *
 * The order matters and is the whole of the care here. Everything that can
 * fail happens to a file with another name: the download, the size check, the
 * hash. Only once the file on disk is known to be the file the release
 * describes does anything move, and the move is two renames rather than a
 * write over the top - because the file being replaced is the program doing
 * the replacing, which on Windows cannot be written to at all and on Linux
 * cannot be written to safely.
 *
 * If the second rename fails the first is undone, because a machine with
 * neither an old game nor a new one is the one outcome worth going out of the
 * way to avoid.
 */
static bool updateDownload(char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0, NULL };
	struct ghostnetreq req;
	char url[512];
	char newpath[FS_MAXPATH];
	char oldpath[FS_MAXPATH];
	char curpath[FS_MAXPATH];
	char sha[65];
	char asset[64];
	char want[65];
	u32 size;
	s32 status = 0;
	FILE *f;

	SDL_LockMutex(g_Lock);
	snprintf(asset, sizeof(asset), "%s", g_Asset);
	snprintf(want, sizeof(want), "%s", g_Sha);
	size = g_Size;
	SDL_UnlockMutex(g_Lock);

	if (asset[0] == '\0') {
		snprintf(msg, msgsize, "check for an update first");
		return false;
	}

	updatePath(".new", newpath, sizeof(newpath));
	updatePath(".old", oldpath, sizeof(oldpath));
	updatePath(NULL, curpath, sizeof(curpath));

	f = fopen(newpath, "wb");

	if (f == NULL) {
		snprintf(msg, msgsize, "cannot write %s (%s)", newpath, strerror(errno));
		return false;
	}

	snprintf(url, sizeof(url), "%s/%s", updateBaseUrl(), asset);

	memset(&req, 0, sizeof(req));
	req.url = url;
	req.redirect = true;
	req.timeout = UPDATE_DOWNLOADTIMEOUT;
	buf.sink = f;

	if (!ghostnetSend(&req, &buf, &status, msg, msgsize)) {
		fclose(f);
		remove(newpath);
		return false;
	}

	fclose(f);

	if (status != 200) {
		snprintf(msg, msgsize, "the download answered %d", status);
		remove(newpath);
		return false;
	}

	if (buf.len != size) {
		snprintf(msg, msgsize, "the download stopped early (%u of %u bytes)", (u32)buf.len, size);
		remove(newpath);
		return false;
	}

	if (!updateHashFile(newpath, sha)) {
		snprintf(msg, msgsize, "could not read back what was downloaded");
		remove(newpath);
		return false;
	}

	if (strcmp(sha, want) != 0) {
		snprintf(msg, msgsize, "the download is not the file the release describes");
		remove(newpath);
		return false;
	}

#ifndef PLATFORM_WIN32
	// The package the release job builds carries the executable bit; a file
	// this wrote itself does not, and a copy of the game nothing can start is
	// the same as no copy at all.
	if (chmod(newpath, 0755) != 0) {
		snprintf(msg, msgsize, "could not make the new build executable");
		remove(newpath);
		return false;
	}
#endif

	remove(oldpath);

	if (rename(curpath, oldpath) != 0) {
		// The path is in the message because the only way this fails is that
		// the path is not the file it was meant to be, and a player cannot act
		// on being told that something did not work.
		snprintf(msg, msgsize, "could not move %s aside (%s)", curpath, strerror(errno));
		remove(newpath);
		return false;
	}

	if (rename(newpath, curpath) != 0) {
		// Put back what was moved. This is the only path here that can leave
		// the install worse than it found it, so it is the only one that
		// tidies up after itself rather than reporting and stopping.
		rename(oldpath, curpath);
		snprintf(msg, msgsize, "could not put the new build in place");
		remove(newpath);
		return false;
	}

	SDL_LockMutex(g_Lock);
	g_Staged = true;
	snprintf(g_StagedPath, sizeof(g_StagedPath), "%s", curpath);
	SDL_UnlockMutex(g_Lock);

	return true;
}

static int updateWorker(void *arg)
{
	char msg[160] = { 0 };
	s32 job;

	SDL_LockMutex(g_Lock);
	job = g_Job;
	SDL_UnlockMutex(g_Lock);

	if (job == UPDATE_JOB_CHECK) {
		if (!updateFetchManifest(msg, sizeof(msg))) {
			updateSetResult(UPDATE_ERROR, msg);
		} else if (updateIsNewer()) {
			SDL_LockMutex(g_Lock);
			snprintf(msg, sizeof(msg), "%s is out. You have %s.", g_Version, VERSION_HASH);
			SDL_UnlockMutex(g_Lock);
			updateSetResult(UPDATE_FOUND, msg);
		} else {
			updateSetResult(UPDATE_CURRENT, strcmp(VERSION_CHANNEL, "stable") == 0
					? "This is the latest release."
					: "This is the latest dev build.");
		}
	} else if (job == UPDATE_JOB_INSTALL) {
		if (updateDownload(msg, sizeof(msg))) {
			updateSetResult(UPDATE_STAGED, "Installed. It starts when you quit and open the game again.");
		} else {
			updateSetResult(UPDATE_ERROR, msg);
		}
	}

	SDL_LockMutex(g_Lock);
	g_Job = UPDATE_JOB_NONE;
	SDL_UnlockMutex(g_Lock);

	return 0;
}

static void updateStart(s32 job)
{
	if (!updateIsAvailable() || updateGetState() == UPDATE_BUSY) {
		return;
	}

	if (g_Thread) {
		SDL_WaitThread(g_Thread, NULL);
		g_Thread = NULL;
	}

	SDL_LockMutex(g_Lock);
	g_Job = job;
	g_State = UPDATE_BUSY;
	snprintf(g_Message, sizeof(g_Message), "%s",
			job == UPDATE_JOB_INSTALL ? "Downloading..." : "Asking GitHub...");
	SDL_UnlockMutex(g_Lock);

	g_Thread = SDL_CreateThread(updateWorker, "pdupdate", NULL);

	if (g_Thread == NULL) {
		updateSetResult(UPDATE_ERROR, "could not start the update");
		SDL_LockMutex(g_Lock);
		g_Job = UPDATE_JOB_NONE;
		SDL_UnlockMutex(g_Lock);
	}
}

void updateCheck(void)
{
	updateStart(UPDATE_JOB_CHECK);
}

void updateInstall(void)
{
	if (updateGetState() == UPDATE_FOUND) {
		updateStart(UPDATE_JOB_INSTALL);
	}
}

bool updateIsAvailable(void)
{
#ifdef PD_GHOST_NET
	return true;
#else
	return false;
#endif
}

s32 updateGetState(void)
{
	s32 state;

	SDL_LockMutex(g_Lock);
	state = g_State;
	SDL_UnlockMutex(g_Lock);

	return state;
}

const char *updateGetMessage(void)
{
	if (!updateIsAvailable()) {
		return "this build has no network support";
	}

	return g_Message;
}

const char *updateGetVersion(void)
{
	return g_Version;
}

bool updateIsStaged(void)
{
	bool staged;

	SDL_LockMutex(g_Lock);
	staged = g_Staged;
	SDL_UnlockMutex(g_Lock);

	return staged;
}

/**
 * Remove the build that was moved aside, if there is one.
 *
 * At startup rather than when it was moved, because on Windows the file being
 * moved aside is the running program and nothing can delete it until it is not
 * running any more. Failure is ignored on purpose: a leftover file next to the
 * game is untidy and nothing else, and there is no point telling a player
 * about it on the way into a menu they did not ask for.
 */
void updateCleanUp(void)
{
	char oldpath[FS_MAXPATH];

	updatePath(".old", oldpath, sizeof(oldpath));
	remove(oldpath);

	// A .new left behind is a download that was interrupted between being
	// written and being checked. It is dead weight and never a build anything
	// would start.
	updatePath(".new", oldpath, sizeof(oldpath));
	remove(oldpath);
}

/**
 * Hand over to the build that was downloaded.
 *
 * Called after the game has shut down, so what starts is not sharing a window,
 * an audio device or a save file with what it replaces. The arguments are this
 * process's own, so a player who started the game with --savedir or a mod list
 * gets the same game back.
 *
 * POSIX replaces this process and never returns. Windows has no such call, so
 * the new copy is started alongside and this one falls off the end of main()
 * immediately afterwards.
 */
void updateRelaunchIfStaged(void)
{
	char path[FS_MAXPATH];

	if (!updateIsStaged()) {
		return;
	}

	SDL_LockMutex(g_Lock);
	snprintf(path, sizeof(path), "%s", g_StagedPath);
	SDL_UnlockMutex(g_Lock);

	sysLogPrintf(LOG_NOTE, "update: starting %s", path);

	// exec does not flush what stdout is holding, and on the path that works
	// there is no later chance to. The one line worth having in the log when
	// somebody asks why the game came back different is this one.
	fflush(NULL);

#ifdef PLATFORM_WIN32
	_spawnv(_P_NOWAIT, path, sysGetArgv());
#else
	execv(path, (char *const *)sysGetArgv());
	// Only reached if the new build could not be started at all, which leaves
	// the player looking at a game that quit. Saying so in the log is all
	// there is left to do from here.
	sysLogPrintf(LOG_ERROR, "update: could not start %s", path);
#endif
}

void updateInit(void)
{
	if (g_Lock == NULL) {
		g_Lock = SDL_CreateMutex();
	}

	if (g_UpdateUrl[0] && strncmp(g_UpdateUrl, "http://127.0.0.1", 16) != 0
			&& strncmp(g_UpdateUrl, "http://localhost", 16) != 0) {
		sysLogPrintf(LOG_WARNING,
				"update: Mod.UpdateServer is %s - the game will replace itself with whatever that serves",
				g_UpdateUrl);
	}

	updateCleanUp();
}

void updateShutdown(void)
{
	if (g_Thread) {
		SDL_WaitThread(g_Thread, NULL);
		g_Thread = NULL;
	}

	if (g_Lock) {
		SDL_DestroyMutex(g_Lock);
		g_Lock = NULL;
	}
}
