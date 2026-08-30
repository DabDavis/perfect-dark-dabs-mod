/**
 * Mod loader.
 *
 * The port can mount several mod directories, but mods ship content the stock
 * tables have no way to reach: stage entries whose file ids point elsewhere,
 * and maps with no arena to select them from. This corrects what it can at
 * startup, using only what a mod actually has on disk.
 */

#include <ultra64.h>
#include <string.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "files.h"
#include "game/stagetable.h"
#include "game/mplayer/setup.h"
#include "fs.h"
#include "romdata.h"
#include "system.h"
#include "modloader.h"

/**
 * True if any mounted mod supplies this file. fsFullPath() searches the mod
 * dirs before the base dir, and the base dir holds the ROM rather than loose
 * files, so a hit means a mod provides it.
 */
static bool modloaderHasFile(const char *relPath)
{
	return fsFileSize(relPath) > 0;
}

/**
 * Point a stage's geometry at its own background when a mod supplies one.
 *
 * Some stages share another stage's geometry and bring only their own pads.
 * WAR! is the case in point: it draws Skedar Ruins geometry (FILE_BG_SHO_SEG)
 * with WAR! pads. That is correct for the original game, but a mod shipping
 * bg_stat.seg means it intends the real thing, and nothing would otherwise
 * reference that file - producing a level with mismatched geometry and spawns.
 */
static void modloaderFixStageBg(s32 stagenum, const char *bgName, u16 segFileId, u16 tilesFileId)
{
	char path[256];
	const s32 index = stageGetIndex(stagenum);

	if (index < 0) {
		return;
	}

	snprintf(path, sizeof(path), "files/bgdata/bg_%s.seg", bgName);

	if (!modloaderHasFile(path)) {
		return;
	}

	if (g_Stages[index].bgfileid != segFileId) {
		sysLogPrintf(LOG_NOTE, "modloader: stage 0x%02x now uses its own bg_%s", stagenum, bgName);
		g_Stages[index].bgfileid = segFileId;

		snprintf(path, sizeof(path), "files/bgdata/bg_%s_tilesZ", bgName);
		if (modloaderHasFile(path)) {
			g_Stages[index].tilefileid = tilesFileId;
		}
	}
}

/* ---- discovery ---------------------------------------------------------- */

struct modloaderScan {
	s32 modIndex;
	s32 registered;
	s32 found;
	const char *label;
};

static s32 g_ModStageNextSlot = 61; // spares start after the stock table

/**
 * Stage numbers that are in use without appearing in the stage table. Handing
 * one to a map makes selecting it load the title screen, the credits or a
 * menu, so they have to be skipped explicitly.
 */
static bool modloaderIdIsReserved(s32 id)
{
	return id == STAGE_MP_RANDOM
		|| id == STAGE_TITLE
		|| id == STAGE_BOOTPAKMENU
		|| id == STAGE_CREDITS
		|| id == STAGE_4MBMENU;
}

/**
 * Lowest stage number that is free and safe to use, or 0 if there are none.
 *
 * Ids above the stock table are preferred; the gaps below it are used only
 * once those run out.
 *
 * The hard limit is STAGE_TITLE, not the 7 bits the save format allows. The
 * game treats "stagenum < STAGE_TITLE" as "this is a real level" in sixteen
 * places, so a stage numbered at or above it never loads its setup at all -
 * leaving no props and no rooms, and a collision walk that does not terminate.
 */
static s32 modloaderNextStageId(void)
{
	for (s32 id = MODSTAGE_FIRST_ID; id < STAGE_TITLE; ++id) {
		if (!modloaderIdIsReserved(id) && stageGetIndex(id) < 0) {
			return id;
		}
	}

	for (s32 id = 2; id < MODSTAGE_FIRST_ID; ++id) {
		if (!modloaderIdIsReserved(id) && stageGetIndex(id) < 0) {
			return id;
		}
	}

	return 0;
}

/**
 * Does this file exist inside one specific mod dir? The search order is no use
 * here: several mods ship the same filenames for different maps.
 */
static bool modloaderModHasFile(s32 modIndex, const char *fmt, const char *name)
{
	char path[FS_MAXPATH + 1];
	char rel[128];
	const char *dir = fsGetModDirAt(modIndex);

	if (!dir) {
		return false;
	}

	snprintf(rel, sizeof(rel), fmt, name);
	snprintf(path, sizeof(path), "%s/files/%s", dir, rel);

	return fsFileSize(path) > 0;
}

static s32 modloaderRegister(s32 modIndex, const char *fmt, const char *name)
{
	char rel[128];

	snprintf(rel, sizeof(rel), fmt, name);

	return romdataRegisterModFile(rel, modIndex);
}

/**
 * Give one of a mod's maps a stage entry of its own and an arena to reach it.
 *
 * The entry is cloned from a stock multiplayer stage so the lighting, alarm and
 * other tuning are sane, then pointed at this mod's files.
 */
static bool modloaderAddMap(s32 modIndex, const char *mapName, const char *modLabel)
{
	if (g_ModStageNextSlot >= (s32)ARRAYCOUNT(g_Stages)) {
		return false;
	}

	const s32 stageId = modloaderNextStageId();
	if (!stageId) {
		return false;
	}

	const s32 tmpl = stageGetIndex(STAGE_MP_SKEDAR);
	if (tmpl < 0) {
		return false;
	}

	const s32 bg = modloaderRegister(modIndex, "bgdata/bg_%s.seg", mapName);
	const s32 pads = modloaderRegister(modIndex, "bgdata/bg_%s_padsZ", mapName);
	const s32 setup = modloaderRegister(modIndex, "Ump_setup%sZ", mapName);

	if (!bg || !pads || !setup) {
		return false;
	}

	// tiles are optional; some maps reuse another stage's
	s32 tiles = 0;
	if (modloaderModHasFile(modIndex, "bgdata/bg_%s_tilesZ", mapName)) {
		tiles = modloaderRegister(modIndex, "bgdata/bg_%s_tilesZ", mapName);
	}

	struct stagetableentry *dst = &g_Stages[g_ModStageNextSlot];

	*dst = g_Stages[tmpl];
	dst->id = stageId;
	dst->bgfileid = bg;
	dst->padsfileid = pads;
	dst->mpsetupfileid = setup;
	if (tiles) {
		dst->tilefileid = tiles;
	}

	char label[32];
	snprintf(label, sizeof(label), "%s %s", modLabel, mapName);

	if (!mpRegisterArena(dst->id, label)) {
		dst->id = 0; // hand the slot back
		return false;
	}

	++g_ModStageNextSlot;

	return true;
}

static void modloaderScanEntry(const char *name, void *arg)
{
	struct modloaderScan *scan = arg;
	char mapName[64];
	const size_t len = strlen(name);

	// only interested in bg_NAME.seg
	if (strncmp(name, "bg_", 3) != 0 || len < 8 || strcmp(name + len - 4, ".seg") != 0) {
		return;
	}

	const size_t mapLen = len - 3 - 4;
	if (mapLen == 0 || mapLen >= sizeof(mapName)) {
		return;
	}

	memcpy(mapName, name + 3, mapLen);
	mapName[mapLen] = '\0';

	// a map is only playable if the mod also gives it a multiplayer setup
	if (!modloaderModHasFile(scan->modIndex, "Ump_setup%sZ", mapName)) {
		return;
	}

	++scan->found;

	if (modloaderAddMap(scan->modIndex, mapName, scan->label)) {
		++scan->registered;
	}
}

void modloaderInit(void)
{
	if (fsGetNumModDirs() <= 0) {
		return;
	}

	modloaderFixStageBg(STAGE_WAR, "stat", FILE_BG_STAT_SEG, FILE_BG_STAT_TILES);

	// Registering stages at runtime is incomplete and opt-in for now. The
	// mechanism works - files pin to their own mod, stages and arenas register
	// - but a stage created this way must satisfy every per-stage table in the
	// engine, and they are still being found one failure at a time. Known so
	// far: ids must stay below STAGE_TITLE, langGetLangBankIndexFromStagenum()
	// must know the stage, and g_StageAllocations8Mb has no entry for these so
	// they fall back to a default allocation that does not suit every map.
	if (!sysArgCheck("--modstages")) {
		return;
	}

	// Mod dir 0 keeps priority in the search order and its maps already have
	// arenas. Everything mounted after it gets its files pinned and its own
	// stage entries, so mods that share filenames no longer collide.
	for (s32 i = 1; i < fsGetNumModDirs(); ++i) {
		const char *dir = fsGetModDirAt(i);
		char path[FS_MAXPATH + 1];
		struct modloaderScan scan = { i, 0, 0, NULL };
		const char *base = strrchr(dir, '/');

		base = base ? base + 1 : dir;
		if (!strncmp(base, "mod_", 4)) {
			base += 4;
		}
		scan.label = base;

		snprintf(path, sizeof(path), "%s/files/bgdata", dir);

		if (fsScanDir(path, modloaderScanEntry, &scan) < 0) {
			sysLogPrintf(LOG_WARNING, "modloader: could not scan %s", path);
			continue;
		}

		sysLogPrintf(LOG_NOTE, "modloader: %s registered %d of %d maps", base, scan.registered, scan.found);

		if (scan.registered < scan.found) {
			sysLogPrintf(LOG_WARNING, "modloader: out of usable stage numbers; %d maps skipped",
				scan.found - scan.registered);
		}
	}
}
