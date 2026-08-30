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

void modloaderInit(void)
{
	if (fsGetNumModDirs() <= 0) {
		return;
	}

	modloaderFixStageBg(STAGE_WAR, "stat", FILE_BG_STAT_SEG, FILE_BG_STAT_TILES);
}
