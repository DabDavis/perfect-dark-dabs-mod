#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "config.h"
#include "fs.h"
#include "utils.h"
#include "romdata.h"
#include "modloader.h"
#include "video.h"
#include "mod.h"
#include "game/file.h"
#include "data.h"
#include "game/stagetable.h"
#include "game/bondgun.h"

#define MOD_TEXTURES_DIR "textures"
#define MOD_ANIMATIONS_DIR "animations"
#define MOD_SEQUENCES_DIR "sequences"

// Whether each of those directories exists, looked up once. File scope rather
// than function scope because switching mods has to make them stale: the mod
// coming in may have a textures/ where the one going out had none.
static s32 modTexturesDirExists = -1;
static s32 modAnimationsDirExists = -1;
static s32 modSequencesDirExists = -1;

extern struct stagemusic g_StageTracks[];
extern struct stageallocation g_StageAllocations8Mb[];

#define PARSE_STAGE_FLOAT(sec, name, v, min, max) \
	p = modConfigParseFloatValue(p, token, &v); \
	if (!p || v < (min) || v > (max)) { \
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: " sec " invalid " name " value: %s", stagenum, token); \
		return NULL; \
	}

#define PARSE_STAGE_INT(sec, name, v, min, max) \
	p = modConfigParseIntValue(p, token, &v); \
	if (!p || v < (min) || v > (max)) { \
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: " sec " invalid " name " value: %s", stagenum, token); \
		return NULL; \
	}

#define PARSE_STAGE_FILENAME(sec, name, v) \
	p = modConfigParseFileValue(p, token, &v); \
	if (!p) { \
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: " sec " invalid " name " value: %s", stagenum, token); \
		return NULL; \
	}

#define PARSE_STAGE_STRING(sec, name, v) \
	p = strParseToken(p, token, NULL); \
	if (!p) { \
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: " sec " invalid " name " value: %s", stagenum, token); \
		return NULL; \
	} \
	v = strUnquote(token);

#define PARSE_INT(sec, name, v, min, max, ret) \
	p = modConfigParseIntValue(p, token, &v); \
	if (!p || v < (min) || v > (max)) { \
		sysLogPrintf(LOG_ERROR, "mod: %s: invalid " name " value: %s", sec, token); \
		return ret; \
	}

static inline char *modConfigParseFileValue(char *p, char *token, s32 *filenum)
{
	p = strParseToken(p, token, NULL);
	if (!token[0]) {
		return NULL; // empty 
	}
	// check if it is a number already
	s32 num = strtol(token, NULL, 0);
	if (num > 0 && romdataFileGetName(num)) {
		*filenum = num;
		return p;
	}
	// it's a filename
	num = romdataFileGetNumForName(strUnquote(token));
	if (num >= 0) {
		*filenum = num;
		return p;
	}
	// the filename was invalid
	return NULL;
}

static inline char *modConfigParseIntValue(char *p, char *token, s32 *out)
{
	p = strParseToken(p, token, NULL);
	if (!token[0]) {
		return NULL; // empty 
	}
	char *endp = token;
	const s32 num = strtol(token, &endp, 0);
	if (num == 0 && (endp == token || *endp != '\0')) {
		return NULL;
	}
	*out = num;
	return p;
}

static inline char *modConfigParseFloatValue(char *p, char *token, f32 *out)
{
	p = strParseToken(p, token, NULL);
	if (!token[0]) {
		return NULL; // empty 
	}
	char *endp = token;
	const f32 num = strtof(token, &endp);
	if (num == 0.f && (endp == token || *endp != '\0')) {
		return NULL;
	}
	*out = num;
	return p;
}

static char *modConfigParseStageMusic(char *p, char *token, s32 stagenum)
{
	struct stagemusic *smus = NULL;
	for (struct stagemusic *p = g_StageTracks; p->stagenum; ++p) {
		if (p->stagenum == stagenum) {
			smus = p;
			break;
		}
	}

	if (!smus) {
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: music can't be changed for this stage", stagenum);
		return NULL;
	}

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	// parse keyvalues until } is reached
	s32 tmp = 0;
	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		if (!strcmp(token, "primarytrack")) {
			PARSE_STAGE_INT("music:", "primarytrack", tmp, 0, 128);
			smus->primarytrack = tmp;
		} else if (!strcmp(token, "ambienttrack")) {
			PARSE_STAGE_INT("music:", "ambienttrack", tmp, 0, 128);
			smus->ambienttrack = tmp;
		} else if (!strcmp(token, "xtrack")) {
			PARSE_STAGE_INT("music:", "xtrack", tmp, 0, 128);
			smus->xtrack = tmp;
		} else {
			sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: music: invalid key: %s", stagenum, token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: unterminated music block", stagenum);
		return NULL;
	}

	return p;
}

static char *modConfigParseStageWeatherRooms(char *p, char *token, s32 stagenum, struct weathercfg *wcfg)
{
	// determine where we can start adding rooms
	s32 idx;
	for (idx = 0; idx < WEATHERCFG_MAX_SKIPROOMS && wcfg->skiprooms[idx]; ++idx);

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	// check if user wants to clear the whole list
	p = strParseToken(p, token, NULL);
	if (!strcmp(token, "clear")) {
		memset(wcfg->skiprooms, 0, sizeof(wcfg->skiprooms));
		idx = 0;
		p = strParseToken(p, token, NULL);
	}

	s32 tmp = 0;
	while (p && token[0] && strcmp(token, "}") != 0) {
		if (token[0] == ',' && !token[1]) {
			p = strParseToken(p, token, NULL);
			continue;
		}

		tmp = strtol(token, NULL, 0);
		if (tmp <= 0 || tmp > 32767) {
			sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: weather: rooms: invalid room %s", stagenum, token);
			return NULL;
		}

		if (idx < WEATHERCFG_MAX_SKIPROOMS) {
			wcfg->skiprooms[idx++] = tmp;
		}

		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: weather: unterminated rooms block", stagenum);
		return NULL;
	}

	return p;
}

static char *modConfigParseStageWeather(char *p, char *token, s32 stagenum)
{
	s32 wi;
	struct weathercfg *wcfg = NULL;
	for (wi = 0; wi < ARRAYCOUNT(g_WeatherConfig) && g_WeatherConfig[wi].stagenum; ++wi) {
		if (g_WeatherConfig[wi].stagenum == stagenum) {
			break;
		}
	}

	if (wi >= WEATHERCFG_MAX_STAGES) {
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: no more space for weather config", stagenum);
		return NULL;
	}

	wcfg = &g_WeatherConfig[wi];

	if (!wcfg->stagenum) {
		// new weather config; initialize with defaults
		*wcfg = g_DefaultWeatherConfig;
		wcfg->stagenum = stagenum;
	} else {
		// flags have to be re-specified
		wcfg->flags = 0;
	}

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	// parse keyvalues until } is reached
	s32 tmpi = 0;
	f32 tmpf = 0.f;
	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		if (!strcmp(token, "include_rooms") || !strcmp(token, "exclude_rooms")) {
			// include_rooms | exclude_rooms { ROOM_NUMBERS... }
			const s32 include = (token[0] == 'i');
			p = modConfigParseStageWeatherRooms(p, token, stagenum, wcfg);
			if (!p) {
				return NULL;
			}
			if (wcfg->skiprooms[0] && include) {
				wcfg->flags |= WEATHERFLAG_INCLUDE;
			}
		} else if (!strcmp(token, "cutscene_only")) {
			wcfg->flags |= WEATHERFLAG_CUTSCENE_ONLY;
		} else if (!strcmp(token, "constant_wind")) {
			PARSE_STAGE_FLOAT("weather:", "constant_wind (0)", wcfg->windanglerad, -M_TAU, M_TAU);
			PARSE_STAGE_FLOAT("weather:", "constant_wind (1)", wcfg->windspeedx, -1024.f, 1024.f);
			PARSE_STAGE_FLOAT("weather:", "constant_wind (2)", wcfg->windspeedz, -1024.f, 1024.f);
			wcfg->flags |= WEATHERFLAG_FORCE_WINDDIR;
		} else if (!strcmp(token, "windspeed")) {
			PARSE_STAGE_FLOAT("weather:", "windspeed", tmpf, -1024.f, 1024.f);
			wcfg->windspeed = tmpf;
		} else if (!strcmp(token, "ymin")) {
			PARSE_STAGE_FLOAT("weather:", "ymin", tmpf, -65536.f, 65536.f);
			wcfg->ymin = tmpf;
		} else if (!strcmp(token, "ymax")) {
			PARSE_STAGE_FLOAT("weather:", "ymax", tmpf, -65536.f, 65536.f);
			wcfg->ymax = tmpf;
		} else if (!strcmp(token, "zmax")) {
			PARSE_STAGE_FLOAT("weather:", "zmax", tmpf, -65536.f, 65536.f);
			wcfg->zmax = tmpf;
		} else {
			sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: weather: invalid key: %s", stagenum, token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: unterminated weather block", stagenum);
		return NULL;
	}

	return p;
}

static char *modConfigParseStage(char *p, char *token)
{
	// stage number
	p = strParseToken(p, token, NULL);
	const s32 stagenum = strtol(token, NULL, 0);
	if (stagenum <= 0x01 || stagenum > 0x50) {
		return NULL;
	}

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	// find the stage table pointers this corresponds to
	struct stagetableentry *stab = NULL;
	struct stageallocation *salloc = NULL;
	const s32 sidx = stageGetIndex(stagenum);
	if (sidx >= 0) {
		stab = &g_Stages[sidx];
	} else {
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: unknown stage number", stagenum);
		return NULL;
	}
	for (struct stageallocation *p = g_StageAllocations8Mb; p->stagenum; ++p) {
		if (p->stagenum == stagenum) {
			salloc = p;
			break;
		}
	}

	// parse keyvalues until } is reached
	s32 tmp = 0;
	char *tmps = NULL;
	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		if (!strcmp(token, "bgfile")) {
			// bg FILE_NAME_OR_NUM
			PARSE_STAGE_FILENAME("", "bgfile", tmp);
			stab->bgfileid = tmp;
		} else if (!strcmp(token, "tilesfile")) {
			// tilesfile FILE_NAME_OR_NUM
			PARSE_STAGE_FILENAME("", "tilesfile", tmp);
			stab->tilefileid = tmp;
		} else if (!strcmp(token, "padsfile")) {
			// padsfile FILE_NAME_OR_NUM
			PARSE_STAGE_FILENAME("", "padsfile", tmp);
			stab->padsfileid = tmp;
		} else if (!strcmp(token, "setupfile")) {
			// setupfile FILE_NAME_OR_NUM
			PARSE_STAGE_FILENAME("", "setupfile", tmp);
			stab->setupfileid = tmp;
		} else if (!strcmp(token, "mpsetupfile")) {
			// mpsetupfile FILE_NAME_OR_NUM
			PARSE_STAGE_FILENAME("", "mpsetupfile", tmp);
			stab->mpsetupfileid = tmp;
		} else if (!strcmp(token, "alarm")) {
			PARSE_STAGE_INT("", "alarm", tmp, 1, 0xFFFF);
			stab->alarm = tmp;
		} else if (!strcmp(token, "extragunmem")) {
			PARSE_STAGE_INT("", "extragunmem", tmp, 0, 0xFFFF);
			stab->extragunmem = tmp;
		}  else if (!strcmp(token, "allocation")) {
			// allocation "ALLOCSTRING"
			PARSE_STAGE_STRING("", "allocation", tmps);
			// FIXME: this leaks
			tmps = strDuplicate(tmps);
			if (tmps) {
				salloc->string = tmps;
			}
		}	else if (!strcmp(token, "music")) {
			// music { KEYVALUES... }
			p = modConfigParseStageMusic(p, token, stagenum);
			if (!p) {
				return NULL;
			}
		} else if (!strcmp(token, "weather")) {
			// weather { KEYVALUES... }
			p = modConfigParseStageWeather(p, token, stagenum);
			if (!p) {
				return NULL;
			}
		} else {
			sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: invalid key: %s", stagenum, token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated stage 0x%02x block", stagenum);
		return NULL;
	}

	return p;
}

/**
 * weapon NUMBER { KEYVALUES... }
 *
 * The behaviours the game used to decide by comparing the weapon number. A mod
 * that brings its own guns renumbers them, and no amount of asset importing
 * tells the code that its number 15 is a pump-action - this does.
 */
static char *modConfigParseWeapon(char *p, char *token)
{
	s32 weaponnum = 0;

	p = modConfigParseIntValue(p, token, &weaponnum);
	if (!p || weaponnum < 0 || weaponnum > WEAPON_SUICIDEPILL) {
		sysLogPrintf(LOG_ERROR, "modconfig: weapon: invalid weapon number: %s", token);
		return NULL;
	}

	struct weapon *weapon = bgunGetWeaponDefinition(weaponnum);

	if (!weapon) {
		sysLogPrintf(LOG_ERROR, "modconfig: weapon 0x%02x: no such weapon", weaponnum);
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);

	while (p && token[0] && strcmp(token, "}") != 0) {
		static const struct {
			const char *name;
			u32 flag;
		} flags[] = {
			{ "unequippedreload", WEAPONFLAG2_UNEQUIPPEDRELOAD },
			{ "pumpaction",       WEAPONFLAG2_PUMPACTION },
			{ "chargeable",       WEAPONFLAG2_CHARGEABLE },
		};

		s32 handled = false;
		s32 tmp = 0;

		for (u32 i = 0; i < ARRAYCOUNT(flags); ++i) {
			if (strcmp(token, flags[i].name)) {
				continue;
			}

			PARSE_INT("weapon", "flag", tmp, 0, 1, NULL);

			if (tmp) {
				weapon->flags2 |= flags[i].flag;
			} else {
				weapon->flags2 &= ~flags[i].flag;
			}

			handled = true;
			break;
		}

		if (!handled) {
			if (!strcmp(token, "unequippedreloadindex")) {
				PARSE_INT("weapon", "unequippedreloadindex", tmp, -1, 127, NULL);
				weapon->unequippedreloadindex = tmp;
			} else {
				sysLogPrintf(LOG_ERROR, "modconfig: weapon 0x%02x: invalid key: %s", weaponnum, token);
				return NULL;
			}
		}

		p = strParseToken(p, token, NULL);
	}

	return p;
}

s32 modConfigLoad(const char *fname)
{
	u32 dataLen = 0;
	char *data = fsFileLoad(fname, &dataLen);
	if (!data) {
		return false;
	}

	s32 success = true;
	char token[UTIL_MAX_TOKEN + 1] = { 0 };
	char *end = data + dataLen;
	char *p = strParseToken(data, token, NULL);
	while (p && token[0]) {
		if (!strcmp(token, "weapon")) {
			// weapon NUMBER { KEYVALUES... }
			char *prev = p;
			p = modConfigParseWeapon(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed weapon block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "stage")) {
			// stage NUMBER { KEYVALUES... }
			char *prev = p;
			p = modConfigParseStage(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed stage block at offset %d", prev - data);
				success = false;
				break;
			}
		} else {
			// garbage
			sysLogPrintf(LOG_ERROR, "modconfig: unexpected %s at offset %d", token[0] ? token : "end of file", p - data);
			success = false;
			break;
		}
		p = strParseToken(p, token, NULL);
	}

	sysMemFree(data);
	return success;
}

s32 modTextureLoad(u16 num, void *dst, u32 dstSize)
{
	if (modTexturesDirExists < 0) {
		modTexturesDirExists = (fsFileSize(MOD_TEXTURES_DIR) >= 0);
	}

	if (!modTexturesDirExists) {
		return -1;
	}

	char path[FS_MAXPATH + 1];
	snprintf(path, sizeof(path), MOD_TEXTURES_DIR "/%04x.bin", num);

	const s32 ret = fsFileLoadTo(path, dst, dstSize);
	if (ret > 0) {
		sysLogPrintf(LOG_NOTE, "mod: loaded external texture %04x", num);
	}

	return ret;
}

void *modSequenceLoad(u16 num, u32 *outSize)
{
	if (modSequencesDirExists < 0) {
		modSequencesDirExists = (fsFileSize(MOD_SEQUENCES_DIR) >= 0);
	}

	if (!modSequencesDirExists) {
		return NULL;
	}

	char path[FS_MAXPATH + 1];
	snprintf(path, sizeof(path), MOD_SEQUENCES_DIR "/%04x.bin", num);
	if (fsFileSize(path) > 0) {
		void *ret = fsFileLoad(path, outSize);
		if (ret) {
			sysLogPrintf(LOG_NOTE, "mod: loaded external sequence %04x", num);
			return ret;
		}
	}

	return NULL;
}

void *modAnimationLoadData(u16 num)
{
	char path[FS_MAXPATH + 1];
	// load the animation data
	snprintf(path, sizeof(path), MOD_ANIMATIONS_DIR "/%04x.bin", num);
	void *data = fsFileLoad(path, NULL);
	if (!data) {
		sysFatalError("External animation %04x has no data file.\nEnsure that it is placed at %s or delete the descriptor.", num, path);
	}
	return data;
}

s32 modAnimationLoadDescriptor(u16 num, struct animtableentry *anim)
{
	if (modAnimationsDirExists < 0) {
		modAnimationsDirExists = (fsFileSize(MOD_ANIMATIONS_DIR) >= 0);
	}

	if (!modAnimationsDirExists) {
		return false;
	}

	char path[FS_MAXPATH + 1];

	// load the descriptor, if any
	snprintf(path, sizeof(path), MOD_ANIMATIONS_DIR "/%04x.txt", num);
	if (fsFileSize(path) <= 0) {
		return false;
	}

	char *desc = fsFileLoad(path, NULL);
	if (!desc) {
		return false;
	}

	// parse the descriptor
	char token[UTIL_MAX_TOKEN + 1] = { 0 };
	char *p = strParseToken(desc, token, NULL);
	s32 tmp = 0;
	while (p && token[0]) {
		if (!strcmp(token, "numframes")) {
			PARSE_INT(path, "numframes", tmp, 0, 0xFFFF, false);
			anim->numframes = tmp;
		} else if (!strcmp(token, "bytesperframe")) {
			PARSE_INT(path, "bytesperframe", tmp, 0, 0xFFFF, false);
			anim->bytesperframe = tmp;
		} else if (!strcmp(token, "headerlen")) {
			PARSE_INT(path, "headerlen", tmp, 0, 0xFFFF, false);
			anim->headerlen = tmp;
		} else if (!strcmp(token, "framelen")) {
			PARSE_INT(path, "framelen", tmp, 0, 0xFF, false);
			anim->framelen = tmp;
		} else if (!strcmp(token, "flags")) {
			PARSE_INT(path, "flags", tmp, 0, 0xFF, false);
			anim->flags = tmp;
		} else {
			sysLogPrintf(LOG_ERROR, "mod: %s: invalid key: %s", path, token);
			return false;
		}
		p = strParseToken(p, token, NULL);
	}

	sysMemFree(desc);

	sysLogPrintf(LOG_NOTE, "mod: loaded external animation %04x", num);

	return true;
}

/* ---- the mod list ------------------------------------------------------- */

/**
 * Mods the player can pick between, and the one they picked.
 *
 * A mod directory replaces asset files and ROM segments, both of which are read
 * once at startup and then pointed at from everywhere - so unlike a texture
 * pack, which is only ever consulted through one function, a mod cannot be
 * swapped while the game is running. The choice is written to the config and
 * mounted on the next start.
 *
 * Only one is mounted this way. Several can still be passed on the command
 * line, which is what the All in One launcher does, but the general file search
 * only ever reaches the first of them (see numOverlayModDirs in fs.c) and a
 * menu that let you stack them would be offering something that does not work.
 */

#define MOD_MODS_DIR "mods"
#define MOD_MAX_MODS 32
#define MOD_NAME_LEN 64

struct modlistentry {
	char name[MOD_NAME_LEN];
	char path[FS_MAXPATH + 1];
};

static struct modlistentry modList[MOD_MAX_MODS];
static s32 numModsListed;

// The name in the config, which is not necessarily one of the above: a mod can
// be deleted between one start and the next.
static char selectedModName[MOD_NAME_LEN];

// Whether the mounted mod dirs came from --moddir. The menu leaves those alone.
static bool modDirsFromArgs;

/**
 * Does this directory hold a mod? A folder with none of these in it is somebody
 * else's - a screenshot folder, a texture pack - and listing it would only
 * offer a choice that does nothing.
 */
static bool modListLooksLikeMod(const char *path)
{
	static const char *const marks[] = { "files", "segs", "textures", MOD_CONFIG_FNAME };
	char tmp[FS_MAXPATH + 1];

	for (s32 i = 0; i < ARRAYCOUNT(marks); ++i) {
		snprintf(tmp, sizeof(tmp), "%s/%s", path, marks[i]);
		if (fsFileSize(tmp) >= 0) {
			return true;
		}
	}

	return false;
}

static void modListAdd(const char *dir, const char *name)
{
	char path[FS_MAXPATH + 1];

	if (numModsListed >= MOD_MAX_MODS) {
		return;
	}

	// the same directory can be reached two ways - the working directory is
	// usually the executable's - so a name already listed is the same mod
	for (s32 i = 0; i < numModsListed; ++i) {
		if (!strcasecmp(modList[i].name, name)) {
			return;
		}
	}

	snprintf(path, sizeof(path), "%s/%s", dir, name);

	if (!modListLooksLikeMod(path)) {
		return;
	}

	snprintf(modList[numModsListed].name, MOD_NAME_LEN, "%s", name);
	snprintf(modList[numModsListed].path, sizeof(modList[0].path), "%s", fsFullPath(path));
	++numModsListed;
}

static void modListScanEntry(const char *name, void *arg)
{
	modListAdd((const char *)arg, name);
}

/**
 * Loose directories next to the executable, filtered by name. Mods have shipped
 * as `mod_something` beside the game since before there was a list to put them
 * in, and asking everyone to move theirs into mods/ to see it here would be a
 * poor trade for the one line this costs.
 */
static void modListScanLooseEntry(const char *name, void *arg)
{
	if (!strncasecmp(name, "mod", 3)) {
		modListAdd((const char *)arg, name);
	}
}

void modListRefresh(void)
{
	static const char *const containers[] = { "$E/" MOD_MODS_DIR, "$H/" MOD_MODS_DIR, "./" MOD_MODS_DIR };
	static const char *const loose[] = { "$E", "." };

	numModsListed = 0;

	for (s32 i = 0; i < ARRAYCOUNT(containers); ++i) {
		fsScanDir(containers[i], modListScanEntry, (void *)containers[i]);
	}

	for (s32 i = 0; i < ARRAYCOUNT(loose); ++i) {
		fsScanDir(loose[i], modListScanLooseEntry, (void *)loose[i]);
	}
}

s32 modListGetCount(void)
{
	return numModsListed;
}

const char *modListGetName(s32 index)
{
	if (index < 0 || index >= numModsListed) {
		return "";
	}

	return modList[index].name;
}

/**
 * Index of the chosen mod in the current list, or -1 for none. Matched by name
 * rather than remembered as an index, the list being re-read whenever the
 * dropdown opens.
 */
s32 modListGetSelected(void)
{
	if (selectedModName[0]) {
		for (s32 i = 0; i < numModsListed; ++i) {
			if (!strcasecmp(modList[i].name, selectedModName)) {
				return i;
			}
		}
	}

	return -1;
}

void modListSetSelected(s32 index)
{
	const char *name = (index >= 0 && index < numModsListed) ? modList[index].name : "";

	snprintf(selectedModName, sizeof(selectedModName), "%s", name);
}

const char *modListGetSelectedName(void)
{
	return selectedModName;
}

/**
 * Whether --moddir put the mounted mods there. The menu neither swaps those nor
 * pretends a stored choice would replace them on the next start.
 */
s32 modListIsFromArgs(void)
{
	return modDirsFromArgs;
}

/**
 * Name of the mod that is actually loaded, or NULL. This is what the game is
 * running with, which is not the selection until it has been restarted.
 */
const char *modListGetLoadedName(void)
{
	const char *dir = fsGetModDir();

	if (!dir) {
		return NULL;
	}

	const char *slash = strrchr(dir, '/');

#ifdef PLATFORM_WIN32
	const char *back = strrchr(dir, '\\');
	if (back > slash) {
		slash = back;
	}
#endif

	return slash ? slash + 1 : dir;
}

/**
 * Mount the mod from the config. Called once, after the config is read and
 * before romdata goes looking for files.
 */
/* ---- switching mods without restarting ---------------------------------- */

/**
 * The stock stage tables, kept so a mod's edits can be taken back.
 *
 * modloaderInit() and modConfigLoad() both write into tables the game owns, in
 * place and with no record of what was there - which is fine for something read
 * once at startup and no use at all for switching mods. The copy is taken
 * before either of them has run.
 *
 * g_StageTracks and g_StageAllocations8Mb are declared without a size, so their
 * length is found by walking to the terminator, the way every reader of them
 * does.
 */
static struct stagetableentry stagesSnapshot[ARRAYCOUNT(g_Stages)];
static struct weathercfg weatherSnapshot[ARRAYCOUNT(g_WeatherConfig)];
static struct stagemusic *tracksSnapshot;
static s32 numTracksSnapshot;
static struct stageallocation *allocsSnapshot;
static s32 numAllocsSnapshot;
static bool tablesSnapshotted;

static void modTablesSnapshot(void)
{
	if (tablesSnapshotted) {
		return;
	}

	memcpy(stagesSnapshot, g_Stages, sizeof(stagesSnapshot));
	memcpy(weatherSnapshot, g_WeatherConfig, sizeof(weatherSnapshot));

	while (g_StageTracks[numTracksSnapshot].stagenum) {
		++numTracksSnapshot;
	}

	while (g_StageAllocations8Mb[numAllocsSnapshot].stagenum) {
		++numAllocsSnapshot;
	}

	tracksSnapshot = sysMemAlloc(sizeof(struct stagemusic) * numTracksSnapshot);
	allocsSnapshot = sysMemAlloc(sizeof(struct stageallocation) * numAllocsSnapshot);

	if (!tracksSnapshot || !allocsSnapshot) {
		sysLogPrintf(LOG_ERROR, "mod: could not copy the stage tables; mods will need a restart");
		return;
	}

	memcpy(tracksSnapshot, g_StageTracks, sizeof(struct stagemusic) * numTracksSnapshot);
	memcpy(allocsSnapshot, g_StageAllocations8Mb, sizeof(struct stageallocation) * numAllocsSnapshot);

	tablesSnapshotted = true;
}

static bool modTablesRestore(void)
{
	if (!tablesSnapshotted) {
		return false;
	}

	memcpy(g_Stages, stagesSnapshot, sizeof(stagesSnapshot));
	memcpy(g_WeatherConfig, weatherSnapshot, sizeof(weatherSnapshot));
	memcpy(g_StageTracks, tracksSnapshot, sizeof(struct stagemusic) * numTracksSnapshot);
	memcpy(g_StageAllocations8Mb, allocsSnapshot, sizeof(struct stageallocation) * numAllocsSnapshot);

	return true;
}

/**
 * Does this mod replace ROM segments?
 *
 * Segments - the audio banks, the animation table, the texture list, the fonts
 * - are read once at boot and end up in memory that is never given back
 * (MEMPOOL_PERMANENT, and the stage pool is placed immediately after it), with
 * the game holding pointers into them from everywhere. There is no taking that
 * back at runtime, so a mod with a segs/ directory can only be swapped in by
 * starting again. Its files alone would leave the game half converted, which is
 * worse than the restart.
 */
static bool modDirHasSegs(const char *path)
{
	char tmp[FS_MAXPATH + 1];

	if (!path || !path[0]) {
		return false;
	}

	snprintf(tmp, sizeof(tmp), "%s/segs", path);

	return fsFileSize(tmp) >= 0;
}

/**
 * Can this mod be switched to where we stand? Both sides matter: the segments
 * of the mod already loaded are just as stuck as the ones coming in.
 */
s32 modListSwapIsLive(s32 index)
{
	if (!tablesSnapshotted || modDirsFromArgs) {
		return false;
	}

	const char *loaded = fsGetModDir();

	if (modDirHasSegs(loaded)) {
		return false;
	}

	if (index >= 0 && index < numModsListed && modDirHasSegs(modList[index].path)) {
		return false;
	}

	return true;
}

/**
 * Switch mods now.
 *
 * Everything a mod reaches through the file layer is dropped and looked up
 * again: the port's file slots, the sizes the game remembers for them, the
 * stage tables, and the caches saying which of a mod's optional directories
 * exist. What is already loaded into the stage pool is not touched and does not
 * need to be - the next stage load wipes that pool and reads everything again,
 * so the level you start after this is the new mod's, while the menu backdrop
 * behind you stays as it was.
 */
s32 modListSwap(s32 index)
{
	if (!modListSwapIsLive(index)) {
		return false;
	}

	const char *path = (index >= 0 && index < numModsListed) ? modList[index].path : NULL;

	fsReplaceModDir(path);

	romdataResetFiles();
	filesInit();          // the game's own record of how big each file was

	modTexturesDirExists = -1;
	modAnimationsDirExists = -1;
	modSequencesDirExists = -1;

	modTablesRestore();
	modloaderInit();

	if (fsGetModDir()) {
		modConfigLoad(MOD_CONFIG_FNAME);
	}

	videoResetTextureCache();

	modListSetSelected(index);

	sysLogPrintf(LOG_NOTE, "mod: switched to %s", path ? modListGetName(index) : "no mod");

	return true;
}

void modListApplySelection(void)
{
	// Taken before modloaderInit() and modConfigLoad() get to write into them.
	modTablesSnapshot();

	modDirsFromArgs = fsGetNumModDirs() > 0;

	modListRefresh();

	// "Why is my mod not in the list" is the question this invites, and the
	// answer is usually that what was dropped in is not a mod directory - no
	// files/, no segs/ - which is visible here and nowhere else.
	{
		char names[256];
		u32 len = 0;

		for (s32 i = 0; i < numModsListed && len < sizeof(names) - 1; ++i) {
			const s32 n = snprintf(names + len, sizeof(names) - len, "%s%s",
					len ? ", " : "", modList[i].name);

			if (n <= 0) {
				break;
			}

			len += (u32)n;
		}

		sysLogPrintf(LOG_NOTE, "mod: %d installed%s%s", numModsListed,
				numModsListed ? ": " : "", len ? names : "");
	}

	if (!selectedModName[0]) {
		return;
	}

	if (modDirsFromArgs) {
		// --moddir was given. An explicit command line is the one the player is
		// looking at, so it wins, and the menu says which is which.
		sysLogPrintf(LOG_NOTE, "mod: `%s` is selected but mod dirs came from the command line", selectedModName);
		return;
	}

	const s32 index = modListGetSelected();

	if (index < 0) {
		sysLogPrintf(LOG_WARNING, "mod: selected mod `%s` is not installed", selectedModName);
		return;
	}

	if (fsAddModDir(modList[index].path) >= 0) {
		sysLogPrintf(LOG_NOTE, "mod: mounted `%s`", modList[index].name);
	}
}

PD_CONSTRUCTOR static void modListConfigInit(void)
{
	configRegisterString("Mod.ModDir", selectedModName, sizeof(selectedModName));
}
