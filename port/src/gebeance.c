/**
 * What is left of the GoldenEye XBLA Community Edition's updater path.
 *
 * Until 2026-10-10 the game applied the player's CommunityEditionUpdaterV6.zip
 * from added-content/ to their copy of the release (Mod.GeXblaCommunityEdition,
 * HDiffPatch) and drew the overlay it wrote. The owner then made the Community
 * Edition part of the game for everyone, silently on in the HD look and built
 * from our own source rather than from the zip: its data fixes are the
 * converter's g_RomPatches and the HD builder's own tables, its fog distances
 * are gebeanstage.c's, Doctor Doak's face is the release's own head re-pointed
 * (gebeanchrtable.h). See CLAUDE-notes/ge-bean.md, "The Community Edition
 * built in".
 *
 * All that remains is clearing up after the old path once a start: the
 * overlay folder in the release's cache, and the HD levels built from it
 * (cache/xbla/goldeneye/levels/<key>_<file>_ce.bin), which nothing reads any
 * more. A pd.ini that still has the old key loads as any unknown key does.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "platform.h"
#include "system.h"
#include "fs.h"
#include "gebean.h"

#ifndef PLATFORM_N64

#define GEBEANCE_MAXDEPTH 12

struct cepurge {
	const char *dir;
	s32 depth;
};

static void gebeanCePurgeEntry(const char *name, void *arg);

/** A folder and everything in it (fsScanDir() leaves out dot names: the old markers are named). */
static void gebeanCePurgeTree(const char *path, s32 depth)
{
	static const char *const markers[] = { ".applied", ".applied2", ".applied3" };
	struct cepurge scan = { path, depth };
	char marker[FS_MAXPATH + 1];
	s32 last = -1;

	if (depth > GEBEANCE_MAXDEPTH) {
		return;
	}

	for (s32 i = 0; i < ARRAYCOUNT(markers); i++) {
		snprintf(marker, sizeof(marker), "%s/%s", path, markers[i]);
		remove(marker);
	}

	// removing while reading need not visit everything: round again until a
	// pass removes nothing more
	for (s32 i = 0; i < 8; i++) {
		const s32 left = fsScanDir(path, gebeanCePurgeEntry, &scan);

		if (left <= 0 || left == last) {
			break;
		}

		last = left;
	}

	fsRemoveDir(path);
}

static void gebeanCePurgeEntry(const char *name, void *arg)
{
	const struct cepurge *scan = arg;
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/%s", scan->dir, name);

	if (remove(path) != 0) {
		gebeanCePurgeTree(path, scan->depth + 1);
	}
}

static void gebeanCePurgeLevel(const char *name, void *arg)
{
	const char *dir = arg;
	const size_t len = strlen(name);
	char path[FS_MAXPATH + 1];

	// <key>_<file>_ce.bin, and a .tmp of one a build left half written
	if ((len > 7 && strcmp(name + len - 7, "_ce.bin") == 0)
			|| (len > 11 && strcmp(name + len - 11, "_ce.bin.tmp") == 0)) {
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		remove(path);
	}
}

void gebeanCePurge(void)
{
	char root[FS_MAXPATH + 1], archive[FS_MAXPATH + 1], cache[FS_MAXPATH + 1], path[FS_MAXPATH + 1];

	if (!gebeanTreeInfo(root, sizeof(root), archive, sizeof(archive), cache, sizeof(cache)) || !cache[0]) {
		return;
	}

	snprintf(path, sizeof(path), "%s/" GEBEAN_CE_DIR, cache);

	if (fsFileSize(path) >= 0) {
		gebeanCePurgeTree(path, 0);
		sysLogPrintf(LOG_NOTE, "gebeance: removed the old Community Edition overlay %s", path);
	}

	snprintf(path, sizeof(path), "%s/levels", cache);

	if (fsFileSize(path) >= 0) {
		fsScanDir(path, gebeanCePurgeLevel, path);
	}
}

#endif
