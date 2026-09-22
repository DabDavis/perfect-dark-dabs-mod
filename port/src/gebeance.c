/**
 * The GoldenEye XBLA Community Edition, applied by the game to the player's
 * own copy of the release (Mod.GeXblaCommunityEdition).
 *
 * The Community Edition is a set of fixes the community made to the leaked
 * build: new skydomes, Frigate's water, props, doors, icons and text, and a
 * patched executable for its multiplayer. It is distributed as patches against
 * the release - CommunityEditionUpdaterV6.zip, HDiffPatch files - and never as
 * the files themselves, and the game keeps to that: nothing of it ships here.
 * The player puts the updater's zip in added-content/ beside the release, and
 * this applies its files.diff to their copy.
 *
 * What comes out is an overlay, not a second release: the files the patch
 * writes, changes or renames, in the cache, read before the release's own
 * (gebeanCeFilePath()). The rest it leaves as they are under their own names,
 * and those are not copied. The executable's patches are no use to a game that does not run it,
 * and are not applied.
 *
 * **It takes a restart.** The release's characters, guns and pictures are
 * loaded once and kept, so the choice is read at startup and holds for the
 * session, as a mod does; the menu's Restart Now finishes a change. The patch
 * itself is applied then, once, with a notice on the window: well under a
 * second from an unpacked copy, longer from an archive, whose files the patch
 * reads from have to be unpacked first.
 *
 * The patch checks its own checksums of every file it reads, which is the
 * updater's own rule: it applies to an unmodified Fyodorovna release and to
 * nothing else. A copy it does not fit is said so in the log, and the release
 * is drawn as it is.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "platform.h"
#include "config.h"
#include "system.h"
#include "fs.h"
#include "archive.h"
#include "gexplusrom.h"
#include "video.h"
#include "gebean.h"
#include "external/hdiffpatch/hdpglue.h"

#ifndef PLATFORM_N64

// Where the updater keeps its file patch
#define GEBEANCE_DIFF_ENTRY "CEUpdate/files.diff"
// Written when the overlay is complete, holding the updater's size, so a
// different updater is applied afresh; the name moves on when what the
// overlay holds does (2: renamed files are copied)
#define GEBEANCE_DONE_FILE ".applied2"

static s32 ceWanted;           // Mod.GeXblaCommunityEdition, as the menu leaves it
static s32 ceActive;           // what this session is drawing
static s32 ceFailed;           // chosen at startup and the patch did not fit this copy
static s32 ceScanned;
static char ceZip[FS_MAXPATH + 1];
static char ceRoot[FS_MAXPATH + 1]; // the overlay's files/, when active

PD_CONSTRUCTOR static void gebeanCeConfigInit(void)
{
	configRegisterInt("Mod.GeXblaCommunityEdition", &ceWanted, 0, 1);
}

struct cescan {
	const char *dir;
	s32 depth;
};

static void gebeanCeScanDir(const char *dir, s32 depth);

static void gebeanCeScanEntry(const char *name, void *arg)
{
	struct cescan *scan = arg;
	char path[FS_MAXPATH + 1];

	if (ceZip[0] || name[0] == '.') {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", scan->dir, name);

	if (archiveIsSupported(path)) {
		// looked into, never taken on its name
		if (archiveFindEntry(path, GEBEANCE_DIFF_ENTRY)) {
			snprintf(ceZip, sizeof(ceZip), "%s", path);
		}
	} else if (scan->depth > 0) {
		gebeanCeScanDir(path, scan->depth - 1);
	}
}

static void gebeanCeScanDir(const char *dir, s32 depth)
{
	struct cescan scan = { dir, depth };

	fsScanDir(dir, gebeanCeScanEntry, &scan);
}

/** The updater's zip in added-content/, or a folder in it; found once. */
static s32 gebeanCeFindZip(void)
{
	static const char *const dirs[] = { FS_ADDED_CONTENT_SEARCH };

	if (!ceScanned) {
		ceScanned = 1;

		for (s32 d = 0; d < ARRAYCOUNT(dirs) && !ceZip[0]; d++) {
			char dir[FS_MAXPATH + 1];

			snprintf(dir, sizeof(dir), "%s", fsFullPath(dirs[d]));

			gebeanCeScanDir(dir, 1);
		}

		if (ceZip[0]) {
			sysLogPrintf(LOG_NOTE, "gebeance: the Community Edition updater is %s", ceZip);
		}
	}

	return ceZip[0] != 0;
}

s32 gebeanCeAvailable(void)
{
	return gebeanGetEnabled() && gebeanCeFindZip();
}

s32 gebeanCeGetWanted(void)
{
	return ceWanted;
}

void gebeanCeSetWanted(s32 on)
{
	ceWanted = on ? 1 : 0;
}

s32 gebeanCeIsActive(void)
{
	return ceActive;
}

/** Whether a start would change what is drawn - never for a copy the patch did not fit. */
s32 gebeanCeRestartNeeded(void)
{
	return ceWanted != ceActive && !(ceWanted && ceFailed);
}

/**
 * The overlay's copy of a release file, when this session draws the
 * Community Edition and the patch wrote one. source is the path under files/,
 * name the file in it.
 */
s32 gebeanCeFilePath(char *dst, u32 dstLen, const char *source, const char *name)
{
	if (!ceActive) {
		return 0;
	}

	snprintf(dst, dstLen, "%s/%s/%s", ceRoot, source, name);

	return fsFileSize(dst) >= 0;
}

/**
 * The Community Edition split Surface into the two it is in GoldenEye, sf1
 * and sf2, where the release has the one file for both. key is GoldenEye's own
 * level key, name the release's file.
 */
const char *gebeanCeLevelName(const char *key, const char *name)
{
	if (!ceActive || !name || strcmp(name, "surface") != 0 || !key) {
		return name;
	}

	return strcmp(key, "sevxb") == 0 ? "sf2" : "sf1";
}

/* ---- applying it ------------------------------------------------------- */

struct cerefs {
	const char *root;
	char **missing;
	s32 nummissing;
	s32 maxmissing;
};

static void gebeanCeNoteRef(const char *name, void *arg)
{
	struct cerefs *refs = arg;
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/%s", refs->root, name);

	if (fsFileSize(path) >= 0) {
		return;
	}

	if (refs->nummissing == refs->maxmissing) {
		char **grown = realloc(refs->missing, sizeof(char *) * (refs->maxmissing + 64));

		if (!grown) {
			return;
		}

		refs->missing = grown;
		refs->maxmissing += 64;
	}

	refs->missing[refs->nummissing] = malloc(strlen(name) + 1);

	if (refs->missing[refs->nummissing]) {
		strcpy(refs->missing[refs->nummissing++], name);
	}
}

/** Whether an archive entry is one of the patch's reference files the copy is missing. */
static s32 gebeanCeWantRef(const char *name, void *arg)
{
	const struct cerefs *refs = arg;
	char norm[FS_MAXPATH + 1];
	size_t i;

	for (i = 0; name[i] && i + 1 < sizeof(norm); i++) {
		norm[i] = name[i] == '\\' ? '/' : name[i];
	}

	norm[i] = '\0';

	for (s32 k = 0; k < refs->nummissing; k++) {
		const size_t n = strlen(refs->missing[k]);
		const size_t len = strlen(norm);

		// the entry ends in files/<ref>
		if (len > n + 6 && strcmp(norm + len - n, refs->missing[k]) == 0
				&& strncmp(norm + len - n - 6, "files/", 6) == 0) {
			return 1;
		}
	}

	return 0;
}

static s32 gebeanCeWantDiff(const char *name, void *arg)
{
	const size_t len = strlen(name);
	const size_t n = strlen(GEBEANCE_DIFF_ENTRY);
	char tail[64];

	if (len < n) {
		return 0;
	}

	for (size_t i = 0; i < n; i++) {
		tail[i] = name[len - n + i] == '\\' ? '/' : name[len - n + i];
	}

	tail[n] = '\0';

	return strcmp(tail, GEBEANCE_DIFF_ENTRY) == 0;
}

/** Finds the one file an extraction wrote, wherever under dir its archive path put it. */
struct cefind {
	const char *dir;
	const char *leaf;
	char found[FS_MAXPATH + 1];
	s32 depth;
};

static void gebeanCeFindEntry(const char *name, void *arg);

static void gebeanCeFindIn(struct cefind *find, const char *dir, s32 depth)
{
	struct cefind sub = *find;

	sub.dir = dir;
	sub.depth = depth;
	sub.found[0] = '\0';
	fsScanDir(dir, gebeanCeFindEntry, &sub);

	if (sub.found[0]) {
		snprintf(find->found, sizeof(find->found), "%s", sub.found);
	}
}

static void gebeanCeFindEntry(const char *name, void *arg)
{
	struct cefind *find = arg;
	char path[FS_MAXPATH + 1];

	if (find->found[0] || name[0] == '.') {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", find->dir, name);

	if (strcmp(name, find->leaf) == 0 && fsFileSize(path) >= 0) {
		snprintf(find->found, sizeof(find->found), "%s", path);
	} else if (find->depth > 0) {
		gebeanCeFindIn(find, path, find->depth - 1);
	}
}

struct ceapply {
	char root[FS_MAXPATH + 1];    // the release's files/
	char archive[FS_MAXPATH + 1]; // and the archive it came out of, if it did
	char cache[FS_MAXPATH + 1];   // the release's cache folder
	char dir[FS_MAXPATH + 1];     // cache/ce
	char marker[64];
	s32 result;
	SDL_atomic_t done;
	const char *stage;
};

static int gebeanCeWorker(void *arg)
{
	struct ceapply *a = arg;
	struct cerefs refs;
	struct cefind find;
	char unpack[FS_MAXPATH + 1];
	char oldDir[FS_MAXPATH + 2];
	char outDir[FS_MAXPATH + 2];
	char path[FS_MAXPATH + 1];
	FILE *fp;

	a->result = -100;

	// the patch out of the updater
	a->stage = "READING THE UPDATER";
	snprintf(unpack, sizeof(unpack), "%s/updater", a->dir);
	fsCreateDir(unpack);

	if (archiveExtractMatching(ceZip, unpack, gebeanCeWantDiff, NULL) <= 0) {
		sysLogPrintf(LOG_ERROR, "gebeance: no %s came out of %s", GEBEANCE_DIFF_ENTRY, ceZip);
		SDL_AtomicSet(&a->done, 1);
		return 0;
	}

	memset(&find, 0, sizeof(find));
	find.leaf = "files.diff";
	gebeanCeFindIn(&find, unpack, 4);

	if (!find.found[0]) {
		SDL_AtomicSet(&a->done, 1);
		return 0;
	}

	// The files the patch reads from. An unpacked copy has them all; the
	// game's own unpack of an archive took only what it draws, so the rest
	// come out of the archive now.
	memset(&refs, 0, sizeof(refs));
	refs.root = a->root;

	if (!hdpListOldRefs(find.found, gebeanCeNoteRef, &refs)) {
		sysLogPrintf(LOG_ERROR, "gebeance: %s is not a patch this reads", find.found);
		SDL_AtomicSet(&a->done, 1);
		return 0;
	}

	if (refs.nummissing > 0) {
		a->stage = "UNPACKING WHAT IT PATCHES";

		if (a->archive[0]) {
			archiveExtractMatching(a->archive, a->cache, gebeanCeWantRef, &refs);
		}

		for (s32 k = 0; k < refs.nummissing; k++) {
			snprintf(path, sizeof(path), "%s/%s", a->root, refs.missing[k]);

			if (fsFileSize(path) < 0) {
				sysLogPrintf(LOG_ERROR, "gebeance: the release has no files/%s, which the patch needs",
						refs.missing[k]);
				a->result = -7;
			}
		}
	}

	for (s32 k = 0; k < refs.nummissing; k++) {
		free(refs.missing[k]);
	}

	free(refs.missing);

	if (a->result == -7) {
		SDL_AtomicSet(&a->done, 1);
		return 0;
	}

	a->stage = "APPLYING IT";
	snprintf(oldDir, sizeof(oldDir), "%s/", a->root);
	snprintf(outDir, sizeof(outDir), "%s/files/", a->dir);
	a->result = hdpApplyOverlay(oldDir, find.found, outDir);

	if (a->result > 0) {
		snprintf(path, sizeof(path), "%s/" GEBEANCE_DONE_FILE, a->dir);
		fp = fopen(path, "wb");

		if (fp) {
			fputs(a->marker, fp);
			fclose(fp);
		}
	}

	// the patch is not kept: it is the player's zip that is, and the folders
	// its archive path made are emptied up to updater/ itself (rmdir takes an
	// empty folder only)
	fsRemoveFile(find.found);

	for (char *slash = strrchr(find.found, '/'); slash && slash > find.found + strlen(a->dir);
			slash = strrchr(find.found, '/')) {
		*slash = '\0';
		fsRemoveDir(find.found);
	}

	SDL_AtomicSet(&a->done, 1);

	return 0;
}

/**
 * Startup, after the release is unpacked: makes the overlay when the player
 * has asked for the Community Edition and it is not made yet, and decides
 * whether this session draws it.
 */
void gebeanCePrepareAtStartup(void)
{
	static struct ceapply a;
	char path[FS_MAXPATH + 1];
	char have[64] = "";
	SDL_Thread *thread;
	FILE *fp;

	ceActive = 0;

	if (!ceWanted || !gebeanCeAvailable()) {
		if (ceWanted) {
			sysLogPrintf(LOG_WARNING, "gebeance: the Community Edition is chosen, but %s",
					gebeanGetEnabled() ? "its updater is not in added-content/" : "the GoldenEye XBLA release is not there");
		}

		return;
	}

	memset(&a, 0, sizeof(a));

	if (!gebeanTreeInfo(a.root, sizeof(a.root), a.archive, sizeof(a.archive), a.cache, sizeof(a.cache))) {
		return;
	}

	snprintf(a.dir, sizeof(a.dir), "%s/" GEBEAN_CE_DIR, a.cache);
	snprintf(a.marker, sizeof(a.marker), "%d", fsFileSize(ceZip));
	snprintf(path, sizeof(path), "%s/" GEBEANCE_DONE_FILE, a.dir);

	fp = fopen(path, "rb");

	if (fp) {
		if (!fgets(have, sizeof(have), fp)) {
			have[0] = '\0';
		}

		fclose(fp);
	}

	if (strcmp(have, a.marker) != 0) {
		fsCreateDir(a.dir);
		SDL_AtomicSet(&a.done, 0);
		a.stage = "STARTING";
		thread = SDL_CreateThread(gebeanCeWorker, "gebeance", &a);

		if (!thread) {
			ceFailed = 1;
			return;
		}

		videoUpdateNativeResolution(320, 240);

		while (!SDL_AtomicGet(&a.done)) {
			s32 done = 0, total = 0;

			archiveGetProgress(&done, &total);
			gexPlusRomNotice("GOLDENEYE XBLA COMMUNITY EDITION", a.stage, done, total);
			SDL_Delay(16);
		}

		SDL_WaitThread(thread, NULL);

		if (a.result <= 0) {
			ceFailed = 1;
			sysLogPrintf(LOG_ERROR, "gebeance: the Community Edition's patch did not apply (step %d) - "
					"it fits an unmodified Fyodorovna release only; the release is drawn as it is", a.result);
			return;
		}

		sysLogPrintf(LOG_NOTE, "gebeance: the Community Edition's patch wrote %d files into %s", a.result, a.dir);
	}

	snprintf(ceRoot, sizeof(ceRoot), "%s/files", a.dir);
	ceActive = 1;
	sysLogPrintf(LOG_NOTE, "gebeance: drawing the GoldenEye XBLA Community Edition");
}

#endif
