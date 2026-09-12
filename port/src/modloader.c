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
#include "utils.h"
#include "mod.h"
#include "modloader.h"

extern struct stageallocation g_StageAllocations8Mb[];

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

#define MODSTAGE_FIRST_SLOT 61   // spares start after the stock table
static s32 g_ModStageNextSlot = MODSTAGE_FIRST_SLOT;

/**
 * Which mod each registered stage came from, indexed by stage number and
 * holding the mod dir index plus one.
 */
static u8 g_ModStageDirs[STAGE_MAX_ID + 1];

/**
 * The mod directory a runtime-registered stage belongs to, or NULL for a stock
 * stage.
 *
 * Only the first mod dir joins the general file search, so an asset a later mod
 * replaces is unreachable by name. Anything loaded per stage can consult this
 * to reach the right mod's copy.
 */
const char *modloaderGetStageModDir(s32 stagenum)
{
	if (stagenum < 0 || stagenum >= (s32)ARRAYCOUNT(g_ModStageDirs) || !g_ModStageDirs[stagenum]) {
		return NULL;
	}

	return fsGetModDirAt(g_ModStageDirs[stagenum] - 1);
}

s32 modloaderGetStageModDirIndex(s32 stagenum)
{
	if (stagenum < 0 || stagenum >= (s32)ARRAYCOUNT(g_ModStageDirs) || !g_ModStageDirs[stagenum]) {
		return -1;
	}

	return g_ModStageDirs[stagenum] - 1;
}

/**
 * The memory allocation string a runtime-registered stage should use, or NULL
 * for a stock stage.
 *
 * g_StageAllocations8Mb has no entry for these, so the lookup walks off the end
 * of the table and uses its sentinel: -mgfx120 -mvtx98 -ma300. A Combat Sim
 * arena is given -mgfx200 -mvtx200 -ma400, so a mod arena runs with half the
 * vertex pool the map was built for.
 *
 * Neither graphics pool bounds checks - gfxAllocateVertices() and friends just
 * bump g_GfxMemPos - so the overflow lands in whatever MEMPOOL_STAGE handed out
 * next and corrupts it silently. That is how a mod arena came to crash in
 * setCurrentPlayerNum(): the overrun had rewritten g_HudMessages, and
 * hudmsgsTick() then read a playernum of 283365012 out of it.
 *
 * These stages are cloned from STAGE_MP_SKEDAR, so they get its allocation too.
 */
const char *modloaderGetStageAllocation(s32 stagenum)
{
	if (!modloaderGetStageModDir(stagenum)) {
		return NULL;
	}

	for (const struct stageallocation *p = g_StageAllocations8Mb; p->stagenum; ++p) {
		if (p->stagenum == STAGE_MP_SKEDAR) {
			return p->string;
		}
	}

	return NULL;
}

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
 * Ids above the stock table and below the title screen's are preferred, then
 * the gaps below the table, and last the range above STAGE_4MBMENU up to
 * STAGE_MAX_ID. The game used to treat "stagenum < STAGE_TITLE" as "this is a
 * real level" in sixteen places, so a stage numbered above it never loaded its
 * setup at all - leaving no props and no rooms, and a collision walk that did
 * not terminate; those sites now ask STAGE_IS_LEVEL(), which admits the high
 * range. A stage number is a byte in g_MpSetup, so STAGE_MAX_ID is the end.
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

	for (s32 id = STAGE_4MBMENU + 1; id <= STAGE_MAX_ID; ++id) {
		if (!modloaderIdIsReserved(id) && stageGetIndex(id) < 0) {
			return id;
		}
	}

	return 0;
}

/**
 * Size of a file inside one specific mod dir, or -1 if it is not there. The
 * search order is no use here: several mods ship the same filenames for
 * different maps.
 */
static s32 modloaderModFileSize(s32 modIndex, const char *fmt, const char *name)
{
	char path[FS_MAXPATH + 1];
	char rel[128];
	const char *dir = fsGetModDirAt(modIndex);

	if (!dir) {
		return -1;
	}

	snprintf(rel, sizeof(rel), fmt, name);
	snprintf(path, sizeof(path), "%s/files/%s", dir, rel);

	return fsFileSize(path);
}

static bool modloaderModHasFile(s32 modIndex, const char *fmt, const char *name)
{
	return modloaderModFileSize(modIndex, fmt, name) > 0;
}

/**
 * Smallest bg a real map can be built from.
 *
 * Mods ship placeholder geometry under names they reach some other way - the
 * GoldenEye X suite has three 512 byte bg files, and remaps the stock stages
 * that use them through its modconfig instead. Registering one as an arena
 * gives a map with no rooms, which crashes as soon as it is entered. The
 * smallest real map in that suite is 4320 bytes.
 */
#define MODSTAGE_MIN_BG_SIZE 2048

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
static bool modloaderAddStage(s32 modIndex, const char *mapName, const char *modLabel,
		s32 bg, s32 tiles, s32 pads, s32 setup)
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

	if (bg <= 0 || pads <= 0 || setup <= 0) {
		return false;
	}

	struct stagetableentry *dst = &g_Stages[g_ModStageNextSlot];

	*dst = g_Stages[tmpl];
	dst->id = stageId;
	dst->bgfileid = bg;
	dst->padsfileid = pads;
	dst->mpsetupfileid = setup;

	// And the solo setup, which is the same file: a mod map ships one setup
	// and it is the arena's. Left as the template's, a stage loaded outside
	// the Combat Simulator - which is what a Randomizer run's landing is, and
	// what --boot-stage is - reads Skedar's setup over this map's pads and
	// bg. That is not just the wrong props: Skedar's intro starts the player
	// on its own spawn pad, pad 99, and a mod map with fewer pads than that
	// unpacks a room number out of whatever follows its pad table.
	dst->setupfileid = setup;
	if (tiles > 0) {
		dst->tilefileid = tiles;
	}

	// the map first, since that is what tells one arena from the next, and
	// the mod after it, cut to what is left of the arena row's 30 characters
	char label[32];
	const s32 room = 30 - (s32)strlen(mapName) - 3;
	if (room >= 4) {
		snprintf(label, sizeof(label), "%s (%.*s)", mapName, room, modLabel);
	} else {
		snprintf(label, sizeof(label), "%.30s", mapName);
	}

	if (!mpRegisterArena(dst->id, label)) {
		dst->id = 0; // hand the slot back
		return false;
	}

	++g_ModStageNextSlot;
	g_ModStageDirs[stageId] = modIndex + 1;

	sysLogPrintf(LOG_NOTE, "modloader: %s -> stage 0x%02x", label, stageId);

	return true;
}

static bool modloaderAddMap(s32 modIndex, const char *mapName, const char *modLabel)
{
	const s32 bg = modloaderRegister(modIndex, "bgdata/bg_%s.seg", mapName);
	const s32 pads = modloaderRegister(modIndex, "bgdata/bg_%s_padsZ", mapName);
	const s32 setup = modloaderRegister(modIndex, "Ump_setup%sZ", mapName);

	// tiles are optional; some maps reuse another stage's
	s32 tiles = 0;
	if (modloaderModHasFile(modIndex, "bgdata/bg_%s_tilesZ", mapName)) {
		tiles = modloaderRegister(modIndex, "bgdata/bg_%s_tilesZ", mapName);
	}

	return modloaderAddStage(modIndex, mapName, modLabel, bg, tiles, pads, setup);
}

/**
 * The file slot one of a map's files loads from: the mod's own copy where it
 * has one, and the port's stock slot where it does not.
 *
 * A console mod ships only the files it changed and its maps borrow the rest -
 * GoldenEye X's Bunker is its own bg_tra.seg with mp6's pads, and a map that
 * changed nothing but its pads still wants stock geometry. A pinned slot costs
 * a name out of the pool, so the stock slot is also the cheaper answer.
 */
static s32 modloaderFileSlot(s32 modIndex, const char *rel)
{
	if (!rel || !rel[0]) {
		return 0;
	}

	if (modloaderModHasFile(modIndex, "%s", rel)) {
		return modloaderRegister(modIndex, "%s", rel);
	}

	const s32 stock = romdataFileGetNumForName(rel);

	return stock > 0 ? stock : 0;
}

/**
 * One `map` line of a mod's `maps` block: the arena's own name, and the four
 * files its stage row loads.
 */
/**
 * Whether a map in a mod's block is one of the mod's own.
 *
 * The block is the mod's whole arena list, and a mod that only adds maps keeps
 * Perfect Dark's arenas in it - PD Kakariko's list still has Skedar, Ravine and
 * the Villa. Those load stock files the port already has an arena for, so
 * registering them would put every stock map in the list a second time. A map
 * the mod actually brought ships at least one of its four files.
 */
static bool modloaderMapIsOwn(s32 modIndex, const char *bg, const char *tiles, const char *pads, const char *mpsetup)
{
	const char *const files[4] = { bg, tiles, pads, mpsetup };

	for (s32 i = 0; i < 4; ++i) {
		if (files[i] && files[i][0] && modloaderModHasFile(modIndex, "%s", files[i])) {
			return true;
		}
	}

	return false;
}

static bool modloaderAddConfigMap(s32 modIndex, const char *modLabel, const char *name,
		const char *bg, const char *tiles, const char *pads, const char *mpsetup)
{
	const s32 size = modloaderModFileSize(modIndex, "%s", bg);

	// a placeholder bg is a map with no rooms, which crashes as soon as it is
	// entered; one the mod does not ship at all is stock's, and fine
	if (size >= 0 && size < MODSTAGE_MIN_BG_SIZE) {
		sysLogPrintf(LOG_NOTE, "modloader: %s has only placeholder geometry; skipped", name);
		return false;
	}

	const s32 bgid = modloaderFileSlot(modIndex, bg);
	const s32 padsid = modloaderFileSlot(modIndex, pads);
	const s32 setupid = modloaderFileSlot(modIndex, mpsetup);
	const s32 tilesid = modloaderFileSlot(modIndex, tiles);

	if (!bgid || !padsid || !setupid) {
		sysLogPrintf(LOG_WARNING, "modloader: %s: no file for %s; skipped", name,
				!bgid ? bg : (!padsid ? pads : mpsetup));
		return false;
	}

	return modloaderAddStage(modIndex, name, modLabel, bgid, tilesid, padsid, setupid);
}

/**
 * Register a mod's maps from the `maps` block its modconfig.txt carries.
 *
 * The block is the mod's own arena list: one line a map, with the name the mod
 * calls it and the files its stage table gives it (mods.md, "The Stage
 * Loader"). Scanning for bg_NAME.seg instead gets three things wrong on any
 * console mod, because the file names are Perfect Dark's and say nothing about
 * the map: a map that borrows another's geometry is not found at all, a bg the
 * mod ships but no stage uses is paired with another map's pads, and a setup
 * that is not an arena is registered as one. The names are wrong too - GE-X's
 * `crad` is Aztec, and its Cradle is `mp18`.
 *
 * The mod is not loaded and its config is not parsed (that would apply its
 * weapons); only this block is read, straight out of its directory. Returns
 * false when there is no block, and the caller falls back to the scan.
 */
static bool modloaderAddFromConfig(s32 modIndex, const char *dir, struct modloaderScan *scan)
{
	char path[FS_MAXPATH + 1];
	char token[UTIL_MAX_TOKEN + 1];
	u32 len = 0;
	char *data;
	char *p;
	bool found = false;

	snprintf(path, sizeof(path), "%s/" MOD_CONFIG_FNAME, dir);

	if (fsFileSize(path) <= 0) {
		return false;
	}

	data = fsFileLoad(path, &len);
	if (!data) {
		return false;
	}

	p = strParseToken(data, token, NULL);

	// the block, wherever it is in the file: every other block is somebody
	// else's and is not parsed here
	while (p && token[0]) {
		if (!strcmp(token, "maps")) {
			p = strParseToken(p, token, NULL);
			if (token[0] == '{' && !token[1]) {
				found = true;
				break;
			}
		} else {
			p = strParseToken(p, token, NULL);
		}
	}

	if (found) {
		p = strParseToken(p, token, NULL);

		while (p && token[0] && strcmp(token, "}") != 0) {
			char name[UTIL_MAX_TOKEN + 1] = { 0 };
			char files[4][UTIL_MAX_TOKEN + 1] = { { 0 } };

			if (strcmp(token, "map") != 0) {
				sysLogPrintf(LOG_WARNING, "modloader: %s: unexpected %s in the maps block", dir, token);
				break;
			}

			p = strParseToken(p, token, NULL);
			snprintf(name, sizeof(name), "%s", strUnquote(token));

			// bg, tiles, pads and mpsetup, each after its key
			p = strParseToken(p, token, NULL);
			while (p && token[0] && strcmp(token, "map") != 0 && strcmp(token, "}") != 0) {
				static const char *const keys[4] = { "bg", "tiles", "pads", "mpsetup" };
				s32 which = -1;
				for (s32 i = 0; i < 4; ++i) {
					if (!strcmp(token, keys[i])) {
						which = i;
						break;
					}
				}
				p = strParseToken(p, token, NULL);
				if (which >= 0) {
					snprintf(files[which], sizeof(files[which]), "%s", strUnquote(token));
				}
				p = strParseToken(p, token, NULL);
			}

			if (!name[0] || !files[0][0] || !files[2][0] || !files[3][0]) {
				sysLogPrintf(LOG_WARNING, "modloader: %s: a map line with no name or files", dir);
			} else if (modloaderMapIsOwn(modIndex, files[0], files[1], files[2], files[3])) {
				++scan->found;
				if (modloaderAddConfigMap(modIndex, scan->label, name, files[0], files[1], files[2], files[3])) {
					++scan->registered;
				}
			}
		}
	}

	sysMemFree(data);

	return found;
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

	if (modloaderModFileSize(scan->modIndex, "bgdata/bg_%s.seg", mapName) < MODSTAGE_MIN_BG_SIZE) {
		sysLogPrintf(LOG_NOTE, "modloader: %s has only placeholder geometry; skipped", mapName);
		return;
	}

	++scan->found;

	if (modloaderAddMap(scan->modIndex, mapName, scan->label)) {
		++scan->registered;
	}
}

static s32 g_ModStagesRegistered;
static s32 g_ModStagesFound;
static s32 g_ModStageMods;

/**
 * How the last scan went, for the Stage Loader page: maps registered, maps
 * found, and mods that had any.
 */
void modloaderGetStats(s32 *registered, s32 *found, s32 *mods)
{
	*registered = g_ModStagesRegistered;
	*found = g_ModStagesFound;
	*mods = g_ModStageMods;
}

/**
 * Register every map of every directory mounted for its maps. Run at boot
 * and again on a live swap, after modTablesRestore() has put the stock stage
 * and arena tables back and romdataResetFiles() has dropped the pinned
 * slots, so it starts from nothing each time.
 */
void modloaderInit(void)
{
	g_ModStageNextSlot = MODSTAGE_FIRST_SLOT;
	memset(g_ModStageDirs, 0, sizeof(g_ModStageDirs));
	g_ModStagesRegistered = g_ModStagesFound = g_ModStageMods = 0;

	if (fsGetNumModDirs() <= 0) {
		return;
	}

	modloaderFixStageBg(STAGE_WAR, "stat", FILE_BG_STAT_SEG, FILE_BG_STAT_TILES);

	// The overlay mod, when there is one, keeps priority in the search order
	// and its maps already have arenas. Every directory mounted for its maps
	// gets its files pinned and its own stage entries, so mods that share
	// filenames never collide, and no stock stage is touched.
	for (s32 i = fsGetNumOverlayModDirs(); i < fsGetNumModDirs(); ++i) {
		const char *dir = fsGetModDirAt(i);
		char path[FS_MAXPATH + 1];
		struct modloaderScan scan = { i, 0, 0, NULL };
		const char *base = strrchr(dir, '/');

		base = base ? base + 1 : dir;
		if (!strncmp(base, "mod_", 4)) {
			base += 4;
		}
		scan.label = base;

		// what the mod says it has, and only if it says nothing, what its
		// file names look like
		if (!modloaderAddFromConfig(i, dir, &scan)) {
			snprintf(path, sizeof(path), "%s/files/bgdata", dir);

			if (fsFileSize(path) < 0) {
				continue;   // a mod with no maps of its own
			}

			if (fsScanDir(path, modloaderScanEntry, &scan) < 0) {
				sysLogPrintf(LOG_WARNING, "modloader: could not scan %s", path);
				continue;
			}
		}

		sysLogPrintf(LOG_NOTE, "modloader: %s registered %d of %d maps", base, scan.registered, scan.found);

		if (scan.registered < scan.found) {
			sysLogPrintf(LOG_WARNING, "modloader: out of usable stage numbers; %d maps skipped",
				scan.found - scan.registered);
		}

		g_ModStagesRegistered += scan.registered;
		g_ModStagesFound += scan.found;
		if (scan.found) {
			g_ModStageMods++;
		}
	}
}
