#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "config.h"
#include "fs.h"
#include "archive.h"
#include "rompatch.h"
#include "modimport.h"
#include <sys/stat.h>
#include "utils.h"
#include "romdata.h"
#include "modloader.h"
#include "video.h"
#include "mod.h"
#include "game/file.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/modunlocks.h"
#include "game/modrules.h"
#include "modloader.h"
#include "lib/main.h"
#include "data.h"
#include "game/stagetable.h"
#include "game/stagemusic.h"
#include "game/mplayer/setup.h"
#include "game/mplayer/mplayer.h"
#include "game/bondgun.h"
#include "game/game_0b0fd0.h"

#define MOD_TEXTURES_DIR "textures"
#define MOD_ANIMATIONS_DIR "animations"
#define MOD_SEQUENCES_DIR "sequences"

// Whether each of those directories exists, looked up once. File scope rather
// than function scope because switching mods has to make them stale: the mod
// coming in may have a textures/ where the one going out had none.
static s32 modTexturesDirExists = -1;
static s32 modAnimationsDirExists = -1;
static s32 modSequencesDirExists = -1;

extern struct stagemusic *g_StageTracks;
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

#define PARSE_ADDR(sec, name, v, ret) \
	p = modConfigParseAddrValue(p, token, &v); \
	if (!p || !v) { \
		sysLogPrintf(LOG_ERROR, "mod: %s: invalid " name " value: %s", sec, token); \
		return ret; \
	}

#define PARSE_FLOAT(sec, name, v, min, max, ret) \
	p = modConfigParseFloatValue(p, token, &v); \
	if (!p || v < (min) || v > (max)) { \
		sysLogPrintf(LOG_ERROR, "mod: %s: invalid " name " value: %s", sec, token); \
		return ret; \
	}

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

// a ROM address: past what an s32 holds, so not modConfigParseIntValue
static inline char *modConfigParseAddrValue(char *p, char *token, u32 *out)
{
	p = strParseToken(p, token, NULL);
	if (!token[0]) {
		return NULL;
	}
	char *endp = token;
	const unsigned long num = strtoul(token, &endp, 0);
	if (endp == token || *endp != '\0') {
		return NULL;
	}
	*out = (u32)num;
	return p;
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

/* ---- a stage's settings: the music entry, the weather entry ---------------
 *
 * Shared by the `stage` block and the setters' callers. Return 1 applied,
 * 0 unknown key, -1 value out of range, -2 no such stage or entry.
 */

s32 modStageLookup(s32 stagenum, struct stagetableentry **stab, struct stageallocation **salloc)
{
	const s32 sidx = (stagenum > 0x01 && stagenum <= 0x50) ? stageGetIndex(stagenum) : -1;
	if (sidx < 0) {
		return -2;
	}
	if (stab) {
		*stab = &g_Stages[sidx];
	}
	if (salloc) {
		*salloc = NULL;
		for (struct stageallocation *a = g_StageAllocations8Mb; a->stagenum; ++a) {
			if (a->stagenum == stagenum) {
				*salloc = a;
				break;
			}
		}
	}
	return 1;
}

s32 modStageSetKey(s32 stagenum, const char *key, s32 value)
{
	struct stagetableentry *stab;
	if (modStageLookup(stagenum, &stab, NULL) < 0) {
		return -2;
	}
	if (!strcmp(key, "alarm")) {
		if (value < 1 || value > 0xffff) {
			return -1;
		}
		stab->alarm = value;
		return 1;
	}
	if (!strcmp(key, "extragunmem")) {
		if (value < 0 || value > 0xffff) {
			return -1;
		}
		stab->extragunmem = value;
		return 1;
	}
	return 0;
}

// a stage's file slot, by the file's name or its number
s32 modStageSetFile(s32 stagenum, const char *key, const char *nameOrNum)
{
	struct stagetableentry *stab;
	s32 num;
	char *endp;
	if (modStageLookup(stagenum, &stab, NULL) < 0) {
		return -2;
	}
	num = strtol(nameOrNum, &endp, 0);
	if (endp == nameOrNum || *endp || num <= 0 || !romdataFileGetName(num)) {
		num = romdataFileGetNumForName(nameOrNum);
		if (num < 0) {
			return -1;
		}
	}
	if (!strcmp(key, "bgfile")) {
		stab->bgfileid = num;
	} else if (!strcmp(key, "tilesfile")) {
		stab->tilefileid = num;
	} else if (!strcmp(key, "padsfile")) {
		stab->padsfileid = num;
	} else if (!strcmp(key, "setupfile")) {
		stab->setupfileid = num;
	} else if (!strcmp(key, "mpsetupfile")) {
		stab->mpsetupfileid = num;
	} else {
		return 0;
	}
	return 1;
}

s32 modStageSetAllocation(s32 stagenum, const char *str)
{
	struct stageallocation *salloc;
	if (modStageLookup(stagenum, NULL, &salloc) < 0 || !salloc) {
		return -2;
	}
	// FIXME: this leaks
	char *dup = strDuplicate(str);
	if (!dup) {
		return -1;
	}
	salloc->string = dup;
	return 1;
}

s32 modStageMusicSetKey(s32 stagenum, const char *key, s32 value)
{
	struct stagemusic *smus = NULL;
	for (struct stagemusic *m = g_StageTracks; m->stagenum; ++m) {
		if (m->stagenum == stagenum) {
			smus = m;
			break;
		}
	}
	if (!smus) {
		return -2;
	}
	if (value < 0 || value > 128) {
		return -1;
	}
	if (!strcmp(key, "primarytrack")) {
		smus->primarytrack = value;
	} else if (!strcmp(key, "ambienttrack")) {
		smus->ambienttrack = value;
	} else if (!strcmp(key, "xtrack")) {
		smus->xtrack = value;
	} else {
		return 0;
	}
	return 1;
}

/**
 * The weather entry a stage's block edits, made if the stage has none; its
 * flags are cleared, as every block re-specifies them. NULL when the table
 * is full.
 */
struct weathercfg *modStageWeatherBegin(s32 stagenum)
{
	s32 wi;
	for (wi = 0; wi < ARRAYCOUNT(g_WeatherConfig) && g_WeatherConfig[wi].stagenum; ++wi) {
		if (g_WeatherConfig[wi].stagenum == stagenum) {
			break;
		}
	}
	if (wi >= WEATHERCFG_MAX_STAGES) {
		return NULL;
	}
	struct weathercfg *wcfg = &g_WeatherConfig[wi];
	if (!wcfg->stagenum) {
		*wcfg = g_DefaultWeatherConfig;
		wcfg->stagenum = stagenum;
	} else {
		wcfg->flags = 0;
	}
	return wcfg;
}

s32 modStageWeatherSetKey(struct weathercfg *wcfg, const char *key, f32 value)
{
	if (!strcmp(key, "windspeed")) {
		if (value < -1024.f || value > 1024.f) {
			return -1;
		}
		wcfg->windspeed = value;
		return 1;
	}
	if (!strcmp(key, "ymin") || !strcmp(key, "ymax") || !strcmp(key, "zmax")) {
		if (value < -65536.f || value > 65536.f) {
			return -1;
		}
		*(key[0] == 'z' ? &wcfg->zmax : key[2] == 'i' ? &wcfg->ymin : &wcfg->ymax) = value;
		return 1;
	}
	if (!strcmp(key, "cutscene_only")) {
		if (value != 0.f) {
			wcfg->flags |= WEATHERFLAG_CUTSCENE_ONLY;
		}
		return 1;
	}
	return 0;
}

s32 modStageWeatherSetConstantWind(struct weathercfg *wcfg, f32 anglerad, f32 speedx, f32 speedz)
{
	if (anglerad < -M_TAU || anglerad > M_TAU || speedx < -1024.f || speedx > 1024.f || speedz < -1024.f || speedz > 1024.f) {
		return -1;
	}
	wcfg->windanglerad = anglerad;
	wcfg->windspeedx = speedx;
	wcfg->windspeedz = speedz;
	wcfg->flags |= WEATHERFLAG_FORCE_WINDDIR;
	return 1;
}

/**
 * The room list: the rooms that have weather (include) or the rooms that do
 * not. `clear` empties the list first, else the rooms are appended. A room
 * out of range is -1 and the list is left as it was up to it; rooms past the
 * table's end are dropped.
 */
s32 modStageWeatherSetRooms(struct weathercfg *wcfg, s32 include, s32 clear, const s32 *rooms, s32 count)
{
	s32 idx;
	if (clear) {
		memset(wcfg->skiprooms, 0, sizeof(wcfg->skiprooms));
	}
	for (idx = 0; idx < WEATHERCFG_MAX_SKIPROOMS && wcfg->skiprooms[idx]; ++idx);
	for (s32 i = 0; i < count; ++i) {
		if (rooms[i] <= 0 || rooms[i] > 32767) {
			return -1;
		}
		if (idx < WEATHERCFG_MAX_SKIPROOMS) {
			wcfg->skiprooms[idx++] = rooms[i];
		}
	}
	if (wcfg->skiprooms[0] && include) {
		wcfg->flags |= WEATHERFLAG_INCLUDE;
	}
	return 1;
}

static char *modConfigParseStageMusic(char *p, char *token, s32 stagenum)
{
	if (modStageMusicSetKey(stagenum, "primarytrack", -2) == -2) {
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
		char key[UTIL_MAX_TOKEN + 1];
		strcpy(key, token);
		PARSE_STAGE_INT("music:", "track", tmp, 0, 128);
		if (modStageMusicSetKey(stagenum, key, tmp) != 1) {
			sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: music: invalid key: %s", stagenum, key);
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

static char *modConfigParseStageWeatherRooms(char *p, char *token, s32 stagenum, struct weathercfg *wcfg, s32 include)
{
	s32 rooms[WEATHERCFG_MAX_SKIPROOMS];
	s32 count = 0;
	s32 clear = 0;

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	// check if user wants to clear the whole list
	p = strParseToken(p, token, NULL);
	if (!strcmp(token, "clear")) {
		clear = 1;
		p = strParseToken(p, token, NULL);
	}

	while (p && token[0] && strcmp(token, "}") != 0) {
		if (token[0] == ',' && !token[1]) {
			p = strParseToken(p, token, NULL);
			continue;
		}

		const s32 tmp = strtol(token, NULL, 0);
		if (tmp <= 0 || tmp > 32767) {
			sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: weather: rooms: invalid room %s", stagenum, token);
			return NULL;
		}

		if (count < WEATHERCFG_MAX_SKIPROOMS) {
			rooms[count++] = tmp;
		}

		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: weather: unterminated rooms block", stagenum);
		return NULL;
	}

	modStageWeatherSetRooms(wcfg, include, clear, rooms, count);

	return p;
}

static char *modConfigSkipBlock(char *p, char *token);

static char *modConfigParseStageWeather(char *p, char *token, s32 stagenum)
{
	struct weathercfg *wcfg = modStageWeatherBegin(stagenum);

	if (!wcfg) {
		// not a reason to lose the rest of the file: skip this block
		sysLogPrintf(LOG_WARNING, "modconfig: stage 0x%02x: no more space for weather config, skipping block", stagenum);
		p = strParseToken(p, token, NULL);
		if (token[0] != '{' || token[1] != '\0') {
			return NULL;
		}
		return modConfigSkipBlock(p, token);
	}

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	// parse keyvalues until } is reached
	f32 tmpf = 0.f;
	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		if (!strcmp(token, "include_rooms") || !strcmp(token, "exclude_rooms")) {
			// include_rooms | exclude_rooms { ROOM_NUMBERS... }
			p = modConfigParseStageWeatherRooms(p, token, stagenum, wcfg, token[0] == 'i');
			if (!p) {
				return NULL;
			}
		} else if (!strcmp(token, "cutscene_only")) {
			modStageWeatherSetKey(wcfg, "cutscene_only", 1.f);
		} else if (!strcmp(token, "constant_wind")) {
			f32 a, x, z;
			PARSE_STAGE_FLOAT("weather:", "constant_wind (0)", a, -M_TAU, M_TAU);
			PARSE_STAGE_FLOAT("weather:", "constant_wind (1)", x, -1024.f, 1024.f);
			PARSE_STAGE_FLOAT("weather:", "constant_wind (2)", z, -1024.f, 1024.f);
			modStageWeatherSetConstantWind(wcfg, a, x, z);
		} else {
			char key[UTIL_MAX_TOKEN + 1];
			strcpy(key, token);
			PARSE_STAGE_FLOAT("weather:", "value", tmpf, -65536.f, 65536.f);
			if (modStageWeatherSetKey(wcfg, key, tmpf) != 1) {
				sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: weather: invalid key or value: %s %s", stagenum, key, token);
				return NULL;
			}
		}
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: unterminated weather block", stagenum);
		return NULL;
	}

	return p;
}


/**
 * Consume tokens up to the } closing a block whose { has already been eaten,
 * counting nested blocks on the way. Returns NULL if the file ends first.
 */
static char *modConfigSkipBlock(char *p, char *token)
{
	s32 depth = 1;

	while (p) {
		p = strParseToken(p, token, NULL);
		if (!token[0]) {
			break;
		}

		// a quoted filename keeps its quotes, so a brace inside one is not
		// token[0] and cannot be miscounted here
		if (token[0] == '{' && !token[1]) {
			++depth;
		} else if (token[0] == '}' && !token[1]) {
			if (--depth == 0) {
				return p;
			}
		}
	}

	return NULL;
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

	if (modStageLookup(stagenum, NULL, NULL) < 0) {
		// A stage this build does not have is not a syntax error: the config is
		// written against the mod's own stage table. Skipping the block keeps
		// the rest of the file, which aborting here threw away. GE-X opens with
		// a stage number we do not carry, and the three valid map remaps behind
		// it were lost with it, leaving stage 0x49 to load a bg whose tiles did
		// not match and take the fatal in preprocessBgSection1().
		sysLogPrintf(LOG_WARNING, "modconfig: stage 0x%02x: unknown stage number, skipping block", stagenum);
		return modConfigSkipBlock(p, token);
	}

	// parse keyvalues until } is reached
	s32 tmp = 0;
	char *tmps = NULL;
	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		if (!strcmp(token, "bgfile") || !strcmp(token, "tilesfile") || !strcmp(token, "padsfile")
				|| !strcmp(token, "setupfile") || !strcmp(token, "mpsetupfile")) {
			// KEY FILE_NAME_OR_NUM
			char key[UTIL_MAX_TOKEN + 1];
			strcpy(key, token);
			p = strParseToken(p, token, NULL);
			if (!p || !token[0] || modStageSetFile(stagenum, key, strUnquote(token)) != 1) {
				sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: invalid %s value: %s", stagenum, key, token);
				return NULL;
			}
		} else if (!strcmp(token, "alarm") || !strcmp(token, "extragunmem")) {
			char key[UTIL_MAX_TOKEN + 1];
			strcpy(key, token);
			PARSE_STAGE_INT("", "value", tmp, 0, 0xFFFF);
			if (modStageSetKey(stagenum, key, tmp) != 1) {
				sysLogPrintf(LOG_ERROR, "modconfig: stage 0x%02x: invalid %s value: %s", stagenum, key, token);
				return NULL;
			}
		} else if (!strcmp(token, "allocation")) {
			// allocation "ALLOCSTRING"
			PARSE_STAGE_STRING("", "allocation", tmps);
			modStageSetAllocation(stagenum, tmps);
		} else if (!strcmp(token, "music")) {
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

// the port's own weapon flags by the name a modconfig uses for them; `word`
// says which flags word holds it (flags2 ran out at BOTLIMITLESS)
static const struct {
	const char *name;
	u32 flag;
	u32 word;
} weaponFlagNames[] = {
	{ "unequippedreload", WEAPONFLAG2_UNEQUIPPEDRELOAD, 2 },
	{ "pumpaction",       WEAPONFLAG2_PUMPACTION, 2 },
	{ "chargeable",       WEAPONFLAG2_CHARGEABLE, 2 },
	{ "missioncritical",  WEAPONFLAG2_MISSIONCRITICAL, 2 },
	{ "noeject",          WEAPONFLAG2_NOEJECT, 2 },
	{ "landsonhit",       WEAPONFLAG2_LANDSONHIT, 2 },
	{ "nocarteject",      WEAPONFLAG2_NOCARTEJECT, 2 },
	{ "heavysmoke",       WEAPONFLAG2_HEAVYSMOKE, 2 },
	{ "detonatorhand",    WEAPONFLAG2_DETONATORHAND, 2 },
	{ "noreloadsound",    WEAPONFLAG2_NORELOADSOUND, 2 },
	{ "pickupsingle",     WEAPONFLAG2_PICKUPSINGLE, 2 },
	{ "explodeswhenshot", WEAPONFLAG2_EXPLODESWHENSHOT, 2 },
	{ "nopickupwhilearmed", WEAPONFLAG2_NOPICKUPWHILEARMED, 2 },
	{ "nopickupinflight", WEAPONFLAG2_NOPICKUPINFLIGHT, 2 },
	{ "nowallhit",        WEAPONFLAG2_NOWALLHIT, 2 },
	{ "isproximitymine",  WEAPONFLAG2_ISPROXIMITYMINE, 2 },
	{ "stickstowall",     WEAPONFLAG2_STICKSTOWALL, 2 },
	{ "hardwhenlanded",   WEAPONFLAG2_HARDWHENLANDED, 2 },
	{ "poisons",          WEAPONFLAG2_POISONS, 2 },
	{ "minigun",          WEAPONFLAG2_MINIGUN, 2 },
	{ "bladehit",         WEAPONFLAG2_BLADEHIT, 2 },
	{ "laserhit",         WEAPONFLAG2_LASERHIT, 2 },
	{ "bluntmelee",       WEAPONFLAG2_BLUNTMELEE, 2 },
	{ "pistolcasing",     WEAPONFLAG2_PISTOLCASING, 2 },
	{ "shotgundamage",    WEAPONFLAG2_SHOTGUNDAMAGE, 2 },
	{ "piercesshield",    WEAPONFLAG2_PIERCESSHIELD, 2 },
	{ "laserbeam",        WEAPONFLAG2_LASERBEAM, 2 },
	{ "crossbeam",        WEAPONFLAG2_CROSSBEAM, 2 },
	{ "fainttracer",      WEAPONFLAG2_FAINTTRACER, 2 },
	{ "laserflight",      WEAPONFLAG2_LASERFLIGHT, 2 },
	{ "pellets",          WEAPONFLAG2_PELLETS, 2 },
	{ "botlimitless",     WEAPONFLAG2_BOTLIMITLESS, 2 },
	{ "cloakammo",        WEAPONFLAG3_CLOAKAMMO, 3 },
	{ "xrayshot",         WEAPONFLAG3_XRAYSHOT, 3 },
	{ "nosparks",         WEAPONFLAG3_NOSPARKS, 3 },
	{ "fusetimer",        WEAPONFLAG3_FUSETIMER, 3 },
	{ "timedfuse",        WEAPONFLAG3_TIMEDFUSE, 3 },
	{ "remotedetonated",  WEAPONFLAG3_REMOTEDETONATED, 3 },
	{ "ejectspin",        WEAPONFLAG3_EJECTSPIN, 3 },
	{ "ejectsdart",       WEAPONFLAG3_EJECTSDART, 3 },
	{ "heldmuzzle",       WEAPONFLAG3_HELDMUZZLE, 3 },
	{ "shotgunmodel",     WEAPONFLAG3_SHOTGUNMODEL, 3 },
	{ "shellparts",       WEAPONFLAG3_SHELLPARTS, 3 },
	{ "sniperscope",      WEAPONFLAG3_SNIPERSCOPE, 3 },
	{ "loadslide",        WEAPONFLAG3_LOADSLIDE, 3 },
	{ "revolver",         WEAPONFLAG3_REVOLVER, 3 },
	{ "heldrocket",       WEAPONFLAG3_HELDROCKET, 3 },
	{ "lasersight",       WEAPONFLAG3_LASERSIGHT, 3 },
	{ "thrownblade",      WEAPONFLAG3_THROWNBLADE, 3 },
	{ "grenadearc",       WEAPONFLAG3_GRENADEARC, 3 },
	{ "pinball",          WEAPONFLAG3_PINBALL, 3 },
	{ "deploys",          WEAPONFLAG3_DEPLOYS, 3 },
	{ "botignores",       WEAPONFLAG3_BOTIGNORES, 3 },
	{ "boosthud",         WEAPONFLAG3_BOOSTHUD, 3 },
	{ "knifereload",      WEAPONFLAG3_KNIFERELOAD, 3 },
	{ "keepsfunction",    WEAPONFLAG3_KEEPSFUNCTION, 3 },
	{ "chrshotbeam",      WEAPONFLAG3_CHRSHOTBEAM, 3 },
	{ "sdgrenade",        WEAPONFLAG3_SDGRENADE, 3 },
	{ "piercesbulletproof", WEAPONFLAG3_PIERCESBULLETPROOF, 3 },
	{ "freeshots",        WEAPONFLAG3_FREESHOTS, 3 },
};

// the port's function flags by the name a modconfig uses for them
static const struct {
	const char *name;
	u32 flag;
} weaponFuncFlagNames[] = {
	{ "proximitymine", FUNCFLAG_PROXIMITYMINE },
	{ "leavessmoke",   FUNCFLAG_LEAVESSMOKE },
	{ "laserstream",   FUNCFLAG_LASERSTREAM },
};

/* ---- the settings a modconfig block or a script can make -----------------
 *
 * Every parser below goes through these, so
 * a key's name and its range are written once. Return 1 when applied, 0 for
 * a key or flag name that does not exist, -1 for a value out of range, -2 for
 * a weapon or function that does not exist.
 */

s32 modWeaponFlagLookup(const char *name, u32 *flag, u32 *word)
{
	for (u32 i = 0; i < ARRAYCOUNT(weaponFlagNames); ++i) {
		if (!strcmp(name, weaponFlagNames[i].name)) {
			*flag = weaponFlagNames[i].flag;
			*word = weaponFlagNames[i].word;
			return 1;
		}
	}
	return 0;
}

const char *modWeaponFlagName(s32 index)
{
	return (index >= 0 && index < (s32)ARRAYCOUNT(weaponFlagNames)) ? weaponFlagNames[index].name : NULL;
}

s32 modWeaponFuncFlagLookup(const char *name, u32 *flag)
{
	for (u32 i = 0; i < ARRAYCOUNT(weaponFuncFlagNames); ++i) {
		if (!strcmp(name, weaponFuncFlagNames[i].name)) {
			*flag = weaponFuncFlagNames[i].flag;
			return 1;
		}
	}
	return 0;
}

const char *modWeaponFuncFlagName(s32 index)
{
	return (index >= 0 && index < (s32)ARRAYCOUNT(weaponFuncFlagNames)) ? weaponFuncFlagNames[index].name : NULL;
}

void modWeaponFlagClearAll(u32 flag, u32 word)
{
	for (s32 i = 0; i <= WEAPON_SUICIDEPILL; ++i) {
		struct weapon *weapon = bgunGetWeaponDefinition(i);
		if (weapon) {
			*(word == 3 ? &weapon->flags3 : &weapon->flags2) &= ~flag;
		}
	}
}

s32 modWeaponFlagSet(s32 weaponnum, u32 flag, u32 word, s32 on)
{
	struct weapon *weapon = (weaponnum >= 0 && weaponnum <= WEAPON_SUICIDEPILL) ? bgunGetWeaponDefinition(weaponnum) : NULL;
	if (!weapon) {
		return -2;
	}
	u32 *w = word == 3 ? &weapon->flags3 : &weapon->flags2;
	if (on) {
		*w |= flag;
	} else {
		*w &= ~flag;
	}
	return 1;
}

s32 modWeaponFlagGet(s32 weaponnum, u32 flag, u32 word)
{
	struct weapon *weapon = (weaponnum >= 0 && weaponnum <= WEAPON_SUICIDEPILL) ? bgunGetWeaponDefinition(weaponnum) : NULL;
	if (!weapon) {
		return -2;
	}
	return ((word == 3 ? weapon->flags3 : weapon->flags2) & flag) != 0;
}

void modWeaponFuncFlagClearAll(u32 flag)
{
	for (s32 i = 0; i <= WEAPON_SUICIDEPILL; ++i) {
		for (s32 f = 0; f < 2; ++f) {
			struct weaponfunc *func = weaponGetFunctionById(i, f);
			if (func) {
				func->flags &= ~flag;
			}
		}
	}
}

s32 modWeaponFuncFlagSet(s32 weaponnum, s32 funcnum, u32 flag, s32 on)
{
	if (weaponnum < 0 || weaponnum > WEAPON_SUICIDEPILL || funcnum < 0 || funcnum > 1) {
		return -2;
	}
	struct weaponfunc *func = weaponGetFunctionById(weaponnum, funcnum);
	if (!func) {
		return -2;
	}
	if (on) {
		func->flags |= flag;
	} else {
		func->flags &= ~flag;
	}
	return 1;
}

s32 modWeaponFuncFlagGet(s32 weaponnum, s32 funcnum, u32 flag)
{
	if (weaponnum < 0 || weaponnum > WEAPON_SUICIDEPILL || funcnum < 0 || funcnum > 1) {
		return -2;
	}
	struct weaponfunc *func = weaponGetFunctionById(weaponnum, funcnum);
	if (!func) {
		return -2;
	}
	return (func->flags & flag) != 0;
}

/**
 * A `weapon` block's key: a flag name with 0 or 1, or one of the two fields.
 */
s32 modWeaponSetKey(s32 weaponnum, const char *key, s32 value)
{
	struct weapon *weapon = (weaponnum >= 0 && weaponnum <= WEAPON_SUICIDEPILL) ? bgunGetWeaponDefinition(weaponnum) : NULL;
	u32 flag, word;

	if (!weapon) {
		return -2;
	}

	if (modWeaponFlagLookup(key, &flag, &word)) {
		if (value < 0 || value > 1) {
			return -1;
		}
		return modWeaponFlagSet(weaponnum, flag, word, value != 0);
	}

	if (!strcmp(key, "unequippedreloadindex")) {
		if (value < -1 || value > 127) {
			return -1;
		}
		weapon->unequippedreloadindex = value;
		return 1;
	}

	if (!strcmp(key, "pickupsound")) {
		if (value < 0 || value > 0xffff) {
			return -1;
		}
		weapon->pickupsound = value;
		return 1;
	}

	return 0;
}

/**
 * A `weaponfunc` block's key: a function flag name with 0 or 1.
 */
s32 modWeaponFuncSetKey(s32 weaponnum, s32 funcnum, const char *key, s32 value)
{
	u32 flag;

	if (!modWeaponFuncFlagLookup(key, &flag)) {
		return 0;
	}
	if (value < 0 || value > 1) {
		return -1;
	}
	return modWeaponFuncFlagSet(weaponnum, funcnum, flag, value != 0);
}

u32 g_ModUnlocks = 0;

static const struct { const char *name; u32 bit; } unlockKeys[] = {
	{ "cheats", MODUNLOCK_CHEATS }, { "difficulties", MODUNLOCK_DIFFICULTIES },
	{ "mpoptions", MODUNLOCK_MPOPTIONS }, { "firingrange", MODUNLOCK_FIRINGRANGE },
	{ "specialstages", MODUNLOCK_SPECIALSTAGES }, { "completion", MODUNLOCK_COMPLETION },
	{ "allguns", MODUNLOCK_ALLGUNS },
};

s32 modUnlockSetKey(const char *key, s32 value)
{
	for (u32 i = 0; i < ARRAYCOUNT(unlockKeys); ++i) {
		if (!strcmp(key, unlockKeys[i].name)) {
			if (value < 0 || value > 1) {
				return -1;
			}
			if (value) {
				g_ModUnlocks |= unlockKeys[i].bit;
			} else {
				g_ModUnlocks &= ~unlockKeys[i].bit;
			}
			return 1;
		}
	}
	return 0;
}

s32 modDamageSetKey(const char *key, f32 value)
{
	if (!strcmp(key, "playerheadshotscale")) {
		if (value < 0.f || value > 1000.f) {
			return -1;
		}
		g_ModPlayerHeadshotScale = value;
		return 1;
	}
	if (!strcmp(key, "shieldbreakhits")) {
		if (value != 0.f && value != 1.f) {
			return -1;
		}
		g_ModShieldBreakHits = value != 0.f;
		return 1;
	}
	if (!strcmp(key, "poisonmatch") || !strcmp(key, "poisonmission")) {
		if (value < 0.f || value > 65535.f) {
			return -1;
		}
		*(key[7] == 'a' ? &g_ModPoisonMatch : &g_ModPoisonMission) = (s32)value;
		return 1;
	}
	return 0;
}

s32 modPickupQtySet(s32 mode, s32 ammotype, s32 qty)
{
	if (mode < 0 || mode > 1 || ammotype < 0 || ammotype > AMMOTYPE_ECM_MINE) {
		return -2;
	}
	if (qty < 0 || qty > 32767) {
		return -1;
	}
	g_ModPickupQty[mode][ammotype] = qty;
	return 1;
}

s32 modAmmoTypeWeaponSet(s32 ammotype, s32 weaponnum)
{
	if (ammotype < 0 || ammotype > AMMOTYPE_ECM_MINE) {
		return -2;
	}
	if (weaponnum < 0 || weaponnum > WEAPON_SUICIDEPILL) {
		return -1;
	}
	g_AmmoTypeWeapons[ammotype] = weaponnum;
	return 1;
}

/* ---- the tail's rules and colours (game/modrules.h) ---------------------- */

s32 modMovementSetKey(const char *key, f32 value)
{
	if (!strcmp(key, "fastspeed")) {
		if (value < 0.1f || value > 8.f) {
			return -1;
		}
		g_ModFastMoveScale = value;
		return 1;
	}
	if (!strcmp(key, "fastcheat")) {
		if (value < -1.f || value > 63.f) {
			return -1;
		}
		g_ModFastMoveCheat = (s32)value;
		return 1;
	}
	return 0;
}

s32 modCheatsSetKey(const char *key, s32 value)
{
	if (!strcmp(key, "slowmotion")) {
		if (value < -1 || value > 63) {
			return -1;
		}
		g_ModSlowMotionCheat = value;
		return 1;
	}
	return 0;
}

s32 modKohSetColour(const char *key, const f32 *rgb)
{
	f32 *dst;
	if (!strcmp(key, "hillcolour")) {
		dst = g_ModKohHillColour;
	} else if (!strcmp(key, "freecolour")) {
		dst = g_ModKohFreeColour;
	} else {
		return 0;
	}
	for (s32 c = 0; c < 3; c++) {
		if (rgb[c] < 0.f || rgb[c] > 1.f) {
			return -1;
		}
	}
	for (s32 c = 0; c < 3; c++) {
		dst[c] = rgb[c];
	}
	return 1;
}

// the port's colour constants by the name a modconfig uses for them
static const struct { const char *name; s32 index; } modColourNames[] = {
	{ "kohhud",      MODCOLOUR_KOHHUD },
	{ "timer",       MODCOLOUR_TIMER },
	{ "scannerin0",  MODCOLOUR_SCANNERIN0 },
	{ "scannerin1",  MODCOLOUR_SCANNERIN1 },
	{ "scannerout0", MODCOLOUR_SCANNEROUT0 },
	{ "scannerout1", MODCOLOUR_SCANNEROUT1 },
	{ "jointext",    MODCOLOUR_JOINTEXT },
	{ "joinblend",   MODCOLOUR_JOINBLEND },
	{ "interlace0",  MODCOLOUR_INTERLACE0 },
	{ "interlace1",  MODCOLOUR_INTERLACE1 },
};

s32 modColourLookup(const char *name)
{
	for (u32 i = 0; i < ARRAYCOUNT(modColourNames); ++i) {
		if (!strcmp(name, modColourNames[i].name)) {
			return modColourNames[i].index;
		}
	}
	return -1;
}

const char *modColourName(s32 index)
{
	return (index >= 0 && index < (s32)ARRAYCOUNT(modColourNames)) ? modColourNames[index].name : NULL;
}

s32 modColourSet(const char *name, u32 rgba)
{
	const s32 index = modColourLookup(name);
	if (index < 0) {
		return 0;
	}
	g_ModColours[index] = rgba;
	return 1;
}

s32 modTvScreenSetSameAs(s32 num, s32 src)
{
	if (num < 0 || num >= (s32)ARRAYCOUNT(g_TvCmdlists)) {
		return -2;
	}
	if (src < 0 || src >= (s32)ARRAYCOUNT(g_TvCmdlists)) {
		return -1;
	}
	g_TvCmdlists[num] = g_TvCmdlists[src];
	return 1;
}

/**
 * weaponfuncflags FLAG { [clear] WEAPON FUNC ... }
 *
 * One function flag across the whole table: `clear` takes it off every
 * function of every weapon first, then it goes onto the (weapon, function)
 * pairs listed. What `weaponflags` is for a weapon's flags, for a function's:
 * the importer writes it when a test of weapon and function together - the
 * laser's stream, which GE-X moves to its Moonraker's primary - is read out of
 * a mod's code. A function definition can be shared between weapons, and a
 * flag set through one is seen through the other.
 */
static char *modConfigParseWeaponFuncFlags(char *p, char *token)
{
	u32 flag = 0;

	p = strParseToken(p, token, NULL);

	if (!modWeaponFuncFlagLookup(token, &flag)) {
		sysLogPrintf(LOG_ERROR, "modconfig: weaponfuncflags: unknown flag %s", token);
		return NULL;
	}

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	if (!strcmp(token, "clear")) {
		modWeaponFuncFlagClearAll(flag);
		p = strParseToken(p, token, NULL);
	}

	while (p && token[0] && strcmp(token, "}") != 0) {
		s32 weaponnum, funcnum;
		char *endp;

		weaponnum = strtol(token, &endp, 0);
		if (endp == token || *endp || weaponnum < 0 || weaponnum > WEAPON_SUICIDEPILL) {
			sysLogPrintf(LOG_ERROR, "modconfig: weaponfuncflags: invalid weapon number %s", token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
		funcnum = strtol(token, &endp, 0);
		if (endp == token || *endp || funcnum < 0 || funcnum > 1) {
			sysLogPrintf(LOG_ERROR, "modconfig: weaponfuncflags: invalid function number %s", token);
			return NULL;
		}

		if (modWeaponFuncFlagSet(weaponnum, funcnum, flag, true) < 0) {
			sysLogPrintf(LOG_WARNING, "modconfig: weaponfuncflags: weapon %d has no function %d to flag", weaponnum, funcnum);
		}

		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated weaponfuncflags block");
		return NULL;
	}

	return p;
}

/**
 * unlocks { cheats 1 difficulties 1 mpoptions 1 firingrange 1 specialstages 1 completion 1 allguns 1 }
 *
 * What the mod's code unlocks outright: each key a family of tests its
 * code forced to true (game/modunlocks.h). Written by the importer.
 */
static char *modConfigParseUnlocks(char *p, char *token)
{
	s32 tmp = 0;

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		char key[UTIL_MAX_TOKEN + 1];
		strcpy(key, token);
		PARSE_INT("unlocks", "flag", tmp, 0, 1, NULL);
		if (modUnlockSetKey(key, tmp) != 1) {
			sysLogPrintf(LOG_ERROR, "modconfig: unlocks: invalid key: %s", key);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated unlocks block");
		return NULL;
	}

	return p;
}

/**
 * damage { playerheadshotscale N shieldbreakhits 0|1 }
 *
 * The rules chr_damage decides that a mod's code changes: how much more a
 * player's headshot does in a mission (stock 25; GE-X 1), and whether a hit
 * that breaks a shield still lands on the health behind it (stock no; GE-X
 * and three more yes). Written by the importer from the mod's code.
 */
static char *modConfigParseDamage(char *p, char *token)
{
	f32 tmpf = 0;

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		char key[UTIL_MAX_TOKEN + 1];
		strcpy(key, token);
		PARSE_FLOAT("damage", "value", tmpf, -1e9f, 1e9f, NULL);
		if (modDamageSetKey(key, tmpf) != 1) {
			sysLogPrintf(LOG_ERROR, "modconfig: damage: invalid key or value: %s %s", key, token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated damage block");
		return NULL;
	}

	return p;
}

/**
 * pickupqty { mp|solo AMMOTYPE QTY ... }
 *
 * How much ammo a dropped weapon of each type gives when picked up, in a
 * match (mp) or a mission (solo), where a mod's code changed it: GE-X halves
 * most of them. Written by the importer from weapon_get_pickup_ammo_qty's
 * two tables; 0 puts stock's back.
 */
static char *modConfigParsePickupQty(char *p, char *token)
{
	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		s32 mode, ammotype, qty;
		char *endp;
		if (!strcmp(token, "mp")) {
			mode = 0;
		} else if (!strcmp(token, "solo")) {
			mode = 1;
		} else {
			sysLogPrintf(LOG_ERROR, "modconfig: pickupqty: expected mp or solo, got %s", token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
		ammotype = strtol(token, &endp, 0);
		if (endp == token || *endp || ammotype < 0 || ammotype > AMMOTYPE_ECM_MINE) {
			sysLogPrintf(LOG_ERROR, "modconfig: pickupqty: invalid ammo type %s", token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
		qty = strtol(token, &endp, 0);
		if (endp == token || *endp || qty < 0 || qty > 32767) {
			sysLogPrintf(LOG_ERROR, "modconfig: pickupqty: invalid quantity %s", token);
			return NULL;
		}
		modPickupQtySet(mode, ammotype, qty);
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated pickupqty block");
		return NULL;
	}

	return p;
}

/**
 * ammotypeweapon { AMMOTYPE WEAPON ... }
 *
 * Which weapon picking up ammo of a type puts in the inventory - grenade ammo
 * gives the grenade - where a mod's code names another number: GE-X's
 * grenades are 26. Written by the importer from ammo_handle_pickup's chain.
 */
static char *modConfigParseAmmoTypeWeapon(char *p, char *token)
{
	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		s32 ammotype, weaponnum;
		char *endp;
		ammotype = strtol(token, &endp, 0);
		if (endp == token || *endp || ammotype < 0 || ammotype > AMMOTYPE_ECM_MINE) {
			sysLogPrintf(LOG_ERROR, "modconfig: ammotypeweapon: invalid ammo type %s", token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
		weaponnum = strtol(token, &endp, 0);
		if (endp == token || *endp || weaponnum < 0 || weaponnum > WEAPON_SUICIDEPILL) {
			sysLogPrintf(LOG_ERROR, "modconfig: ammotypeweapon: invalid weapon number %s", token);
			return NULL;
		}
		modAmmoTypeWeaponSet(ammotype, weaponnum);
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated ammotypeweapon block");
		return NULL;
	}

	return p;
}

/**
 * weaponflags FLAG { [clear] NUMBER... }
 *
 * One flag across the whole weapon table: `clear` takes it off every weapon
 * first, then it goes onto the numbers listed. A `weapon` block says what one
 * weapon is; this says which weapons a behaviour belongs to, which is the
 * shape of a list the game's code compares against, and is what the importer
 * writes when it has read such a list out of a mod's code. A flag sits on the
 * definition, so a number that shares its definition with another gets it too.
 */
static char *modConfigParseWeaponFlags(char *p, char *token)
{
	u32 flag = 0;
	u32 word = 2;
	s32 tmp = 0;

	p = strParseToken(p, token, NULL);

	if (!modWeaponFlagLookup(token, &flag, &word)) {
		sysLogPrintf(LOG_ERROR, "modconfig: weaponflags: unknown flag %s", token);
		return NULL;
	}

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	if (!strcmp(token, "clear")) {
		modWeaponFlagClearAll(flag, word);
		p = strParseToken(p, token, NULL);
	}

	while (p && token[0] && strcmp(token, "}") != 0) {
		char *endp = token;

		tmp = strtol(token, &endp, 0);
		if (endp == token || *endp != '\0' || tmp < 0 || tmp > WEAPON_SUICIDEPILL) {
			sysLogPrintf(LOG_ERROR, "modconfig: weaponflags: invalid weapon number %s", token);
			return NULL;
		}

		if (modWeaponFlagSet(tmp, flag, word, true) < 0) {
			sysLogPrintf(LOG_WARNING, "modconfig: weaponflags: no weapon %d to flag", tmp);
		}

		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated weaponflags block");
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

	if (!bgunGetWeaponDefinition(weaponnum)) {
		sysLogPrintf(LOG_ERROR, "modconfig: weapon 0x%02x: no such weapon", weaponnum);
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);

	while (p && token[0] && strcmp(token, "}") != 0) {
		char key[UTIL_MAX_TOKEN + 1];
		s32 tmp = 0;

		strcpy(key, token);
		PARSE_INT("weapon", "value", tmp, -0x7fffffff, 0x7fffffff, NULL);

		switch (modWeaponSetKey(weaponnum, key, tmp)) {
		case 1:
			break;
		case 0:
			sysLogPrintf(LOG_ERROR, "modconfig: weapon 0x%02x: invalid key: %s", weaponnum, key);
			return NULL;
		default:
			sysLogPrintf(LOG_ERROR, "modconfig: weapon 0x%02x: invalid %s value: %s", weaponnum, key, token);
			return NULL;
		}

		p = strParseToken(p, token, NULL);
	}

	return p;
}

/**
 * tvscreen NUMBER { sameas NUMBER }
 *
 * Which command list a screen program draws with. Twelve of the stock programs
 * already share a list with another; this lets a mod say the same thing.
 */
static char *modConfigParseTvScreen(char *p, char *token)
{
	s32 num = 0;
	s32 src = 0;

	p = modConfigParseIntValue(p, token, &num);
	if (!p || num < 0 || num >= (s32)ARRAYCOUNT(g_TvCmdlists)) {
		sysLogPrintf(LOG_ERROR, "modconfig: tvscreen: invalid program number: %s", token);
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);

	while (p && token[0] && strcmp(token, "}") != 0) {
		if (!strcmp(token, "sameas")) {
			PARSE_INT("tvscreen", "sameas", src, 0, (s32)ARRAYCOUNT(g_TvCmdlists) - 1, NULL);
			modTvScreenSetSameAs(num, src);
		} else {
			sysLogPrintf(LOG_ERROR, "modconfig: tvscreen %d: invalid key: %s", num, token);
			return NULL;
		}

		p = strParseToken(p, token, NULL);
	}

	return p;
}

/**
 * weaponfunc WEAPON FUNCTION { KEYVALUES... }
 *
 * Behaviour that belongs to one function of a weapon rather than to the weapon.
 * Ten function definitions are shared between weapons, so setting one here can
 * reach further than the weapon named - check invitems.c before assuming it
 * does not.
 */
static char *modConfigParseWeaponFunc(char *p, char *token)
{
	s32 weaponnum = 0;
	s32 funcnum = 0;

	p = modConfigParseIntValue(p, token, &weaponnum);
	if (!p || weaponnum < 0 || weaponnum > WEAPON_SUICIDEPILL) {
		sysLogPrintf(LOG_ERROR, "modconfig: weaponfunc: invalid weapon number: %s", token);
		return NULL;
	}

	p = modConfigParseIntValue(p, token, &funcnum);
	if (!p || funcnum < 0 || funcnum > 1) {
		sysLogPrintf(LOG_ERROR, "modconfig: weaponfunc 0x%02x: invalid function number: %s", weaponnum, token);
		return NULL;
	}

	if (!weaponGetFunctionById(weaponnum, funcnum)) {
		sysLogPrintf(LOG_ERROR, "modconfig: weaponfunc 0x%02x %d: no such function", weaponnum, funcnum);
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);

	while (p && token[0] && strcmp(token, "}") != 0) {
		char key[UTIL_MAX_TOKEN + 1];
		s32 tmp = 0;

		strcpy(key, token);
		PARSE_INT("weaponfunc", "flag", tmp, 0, 1, NULL);

		if (modWeaponFuncSetKey(weaponnum, funcnum, key, tmp) != 1) {
			sysLogPrintf(LOG_ERROR, "modconfig: weaponfunc 0x%02x %d: invalid key: %s", weaponnum, funcnum, key);
			return NULL;
		}

		p = strParseToken(p, token, NULL);
	}

	return p;
}

/**
 * datasegment { file "segs/data" names "segs/data.names" base ADDR weapons ADDR COUNT ... }
 *
 * A mod's ROM data segment and where its tables sit in it; the weapon
 * definitions and model tables are rebuilt from it. tools/importmod writes
 * this block. It goes before any weapon block, since those edit what this
 * puts in place.
 */
/* ---- the data segment spec, key by key ------------------------------------
 *
 * Shared by the `datasegment` block and its callers: the
 * tables by name, and the constant lists. Return 1 applied, 0 unknown key,
 * -1 value out of range, -3 list full (the entry is dropped).
 */

#define DSTABLE(key, addr, count) { key, offsetof(struct moddataspec, addr), offsetof(struct moddataspec, count) }
static const struct { const char *key; u16 addrofs; u16 countofs; } dataSegTables[] = {
	DSTABLE("weapons", weapons, numweapons),
	DSTABLE("modelstates", modelstates, nummodelstates),
	DSTABLE("mpweapons", mpweapons, nummpweapons),
	DSTABLE("mpweaponsets", mpweaponsets, nummpweaponsets),
	DSTABLE("mparenas", mparenas, nummparenas),
	DSTABLE("headsandbodies", headsandbodies, numheadsandbodies),
	DSTABLE("mpheads", mpheads, nummpheads),
	DSTABLE("mpbodies", mpbodies, nummpbodies),
	DSTABLE("botheads", botheads, numbotheads),
	DSTABLE("mpbeauheads", mpbeauheads, nummpbeauheads),
	DSTABLE("mpmaleheads", mpmaleheads, nummpmaleheads),
	DSTABLE("mpfemaleheads", mpfemaleheads, nummpfemaleheads),
	DSTABLE("maleguardheads", maleguardheads, nummaleguardheads),
	DSTABLE("maleguardteamheads", maleguardteamheads, nummaleguardteamheads),
	DSTABLE("femaleguardheads", femaleguardheads, numfemaleguardheads),
	DSTABLE("femaleguardteamheads", femaleguardteamheads, numfemaleguardteamheads),
	DSTABLE("stages", stages, numstages),
	DSTABLE("solostages", solostages, numsolostages),
	DSTABLE("fogenvs", fogenvs, numfogenvs),
	DSTABLE("nofogenvs", nofogenvs, numnofogenvs),
	DSTABLE("commandlengths", commandlengths, numcommandlengths),
	DSTABLE("stagetracks", stagetracks, numstagetracks),
	DSTABLE("mptracks", mptracks, nummptracks),
	DSTABLE("ammotypes", ammotypes, numammotypes),
	DSTABLE("explosiontypes", explosiontypes, numexplosiontypes),
	DSTABLE("autoswitchprimary", autoswitchprimary, numautoswitchprimary),
	DSTABLE("autoswitchsecondary", autoswitchsecondary, numautoswitchsecondary),
	DSTABLE("botweaponprefs", botweaponprefs, numbotweaponprefs),
	DSTABLE("hudmsgtypes", hudmsgtypes, numhudmsgtypes),
	DSTABLE("globalailists", globalailists, numglobalailists),
};
#undef DSTABLE

// the stock -> mod constant lists: where the pairs go, how many fit, and
// whether a kind word comes first (the buddies')
#define DSPAIRS(key, arr, count, width) { key, offsetof(struct moddataspec, arr), offsetof(struct moddataspec, count), \
	sizeof(((struct moddataspec *)0)->arr) / sizeof(((struct moddataspec *)0)->arr[0]), width }
static const struct { const char *key; u16 arrofs; u16 countofs; u16 max; u16 width; } dataSegPairs[] = {
	DSPAIRS("playerconst", playerconsts, numplayerconsts, 2),
	DSPAIRS("buddyconst", buddyconsts, numbuddyconsts, 3),
	DSPAIRS("texconst", texconsts, numtexconsts, 2),
	DSPAIRS("roomnum", roomnums, numroomnums, 2),
	DSPAIRS("roomstage", roomstages, numroomstages, 2),
	DSPAIRS("bgstage", bgstages, numbgstages, 2),
};
#undef DSPAIRS

static const char *const buddyConstKinds[] = { NULL, "body", "head", "model", "weapon" };

void modDataSpecInit(struct moddataspec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->playerbody = -1;
	spec->playerhead = -1;
}

s32 modDataSpecIsTableKey(const char *key)
{
	for (u32 i = 0; i < ARRAYCOUNT(dataSegTables); ++i) {
		if (!strcmp(key, dataSegTables[i].key)) {
			return 1;
		}
	}
	return 0;
}

s32 modDataSpecIsPairKey(const char *key)
{
	for (u32 i = 0; i < ARRAYCOUNT(dataSegPairs); ++i) {
		if (!strcmp(key, dataSegPairs[i].key)) {
			return dataSegPairs[i].width;
		}
	}
	return 0;
}

s32 modDataSpecSetTable(struct moddataspec *spec, const char *key, u32 addr, s32 count)
{
	for (u32 i = 0; i < ARRAYCOUNT(dataSegTables); ++i) {
		if (!strcmp(key, dataSegTables[i].key)) {
			if (!addr || count < 0 || count > 4096) {
				return -1;
			}
			*(u32 *)((u8 *)spec + dataSegTables[i].addrofs) = addr;
			*(s32 *)((u8 *)spec + dataSegTables[i].countofs) = count;
			return 1;
		}
	}
	return 0;
}

// kind is the buddyconst kind word, NULL for the others
s32 modDataSpecAddPair(struct moddataspec *spec, const char *key, const char *kind, s32 stock, s32 mod)
{
	for (u32 i = 0; i < ARRAYCOUNT(dataSegPairs); ++i) {
		if (strcmp(key, dataSegPairs[i].key)) {
			continue;
		}
		s32 k = 0;
		if (dataSegPairs[i].width == 3) {
			for (s32 j = 1; j < 5; ++j) {
				if (kind && !strcmp(kind, buddyConstKinds[j])) {
					k = j;
				}
			}
			if (!k) {
				return -1;
			}
		}
		if (stock < 0 || stock > 0xffff || mod < 0 || mod > 0xffff) {
			return -1;
		}
		s32 *count = (s32 *)((u8 *)spec + dataSegPairs[i].countofs);
		if (*count >= dataSegPairs[i].max) {
			return -3;
		}
		u16 *row = (u16 *)((u8 *)spec + dataSegPairs[i].arrofs) + *count * dataSegPairs[i].width;
		if (dataSegPairs[i].width == 3) {
			*row++ = (u16)k;
		}
		row[0] = (u16)stock;
		row[1] = (u16)mod;
		(*count)++;
		return 1;
	}
	return 0;
}

s32 modDataSpecSetValue(struct moddataspec *spec, const char *key, s32 value)
{
	if (!strcmp(key, "base")) {
		if (!value) {
			return -1;
		}
		spec->base = (u32)value;
		return 1;
	}
	if (!strcmp(key, "playerbody") || !strcmp(key, "playerhead")) {
		if (value < 0 || value > 255) {
			return -1;
		}
		*(key[6] == 'b' ? &spec->playerbody : &spec->playerhead) = value;
		return 1;
	}
	return 0;
}

s32 modDataSpecSetString(struct moddataspec *spec, const char *key, const char *value)
{
	if (!strcmp(key, "file") || !strcmp(key, "names")) {
		if (!value || !value[0]) {
			return -1;
		}
		strncpy(key[0] == 'f' ? spec->file : spec->names, value, sizeof(spec->file) - 1);
		return 1;
	}
	return 0;
}

// Rebuild the tables from the segment. -1 without a file and a base.
s32 modDataSpecApply(const struct moddataspec *spec)
{
	if (!spec->file[0] || !spec->base) {
		return -1;
	}
	modDataImport(spec);
	return 1;
}

/**
 * datasegment { file "segs/data" names "segs/data.names" base ADDR weapons ADDR COUNT ... }
 *
 * A mod's ROM data segment and where its tables sit in it; the weapon
 * definitions and model tables are rebuilt from it. tools/importmod writes
 * this block. It goes before any weapon block, since those edit what this
 * puts in place.
 */
static char *modConfigParseDataSegment(char *p, char *token)
{
	struct moddataspec spec;

	modDataSpecInit(&spec);

	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);

	while (p && token[0] && strcmp(token, "}") != 0) {
		char key[UTIL_MAX_TOKEN + 1];
		strcpy(key, token);

		if (!strcmp(key, "file") || !strcmp(key, "names")) {
			p = strParseToken(p, token, NULL);
			if (!p || !token[0] || modDataSpecSetString(&spec, key, strUnquote(token)) != 1) {
				sysLogPrintf(LOG_ERROR, "modconfig: datasegment: missing file name");
				return NULL;
			}
		} else if (!strcmp(key, "base")) {
			u32 addr = 0;
			PARSE_ADDR("datasegment", "base", addr, NULL);
			spec.base = addr;
		} else if (modDataSpecIsTableKey(key)) {
			u32 addr = 0;
			s32 count = 0;
			PARSE_ADDR("datasegment", "table", addr, NULL);
			PARSE_INT("datasegment", "table count", count, 0, 4096, NULL);
			modDataSpecSetTable(&spec, key, addr, count);
		} else if (modDataSpecIsPairKey(key)) {
			// one line per constant the mod's code changed: [kind] stock mod
			char kind[UTIL_MAX_TOKEN + 1] = "";
			s32 k = 0, v = 0;
			if (modDataSpecIsPairKey(key) == 3) {
				p = strParseToken(p, token, NULL);
				strcpy(kind, token);
			}
			PARSE_INT("datasegment", "stock constant", k, 0, 0xffff, NULL);
			PARSE_INT("datasegment", "mod constant", v, 0, 0xffff, NULL);
			const s32 r = modDataSpecAddPair(&spec, key, kind[0] ? kind : NULL, k, v);
			if (r == -1) {
				sysLogPrintf(LOG_ERROR, "mod: datasegment: invalid %s kind: %s", key, kind);
				return NULL;
			}
		} else if (!strcmp(key, "playerbody") || !strcmp(key, "playerhead")) {
			s32 v = 0;
			PARSE_INT("datasegment", "player body or head", v, 0, 255, NULL);
			modDataSpecSetValue(&spec, key, v);
		} else {
			sysLogPrintf(LOG_ERROR, "modconfig: datasegment: invalid key: %s", key);
			return NULL;
		}

		p = strParseToken(p, token, NULL);
	}

	if (modDataSpecApply(&spec) < 0) {
		sysLogPrintf(LOG_ERROR, "modconfig: datasegment: needs a file and a base address");
		return NULL;
	}

	return p;
}

/**
 * movement { fastspeed 1.375 fastcheat 6 }, cheats { slowmotion -1 },
 * colours { NAME 0xRRGGBBAA ... }: one key and one value a line
 * (game/modrules.h). Written by the importer from the mod's code.
 */
static char *modConfigParseRules(char *p, char *token, const char *block)
{
	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		char key[UTIL_MAX_TOKEN + 1];
		s32 r;
		strcpy(key, token);
		if (!strcmp(block, "movement")) {
			f32 v = 0;
			PARSE_FLOAT("movement", "value", v, -1e9f, 1e9f, NULL);
			r = modMovementSetKey(key, v);
		} else if (!strcmp(block, "cheats")) {
			s32 v = 0;
			PARSE_INT("cheats", "value", v, -1, 63, NULL);
			r = modCheatsSetKey(key, v);
		} else {
			u32 v = 0;
			PARSE_ADDR("colours", "colour", v, NULL);
			r = modColourSet(key, v);
		}
		if (r != 1) {
			sysLogPrintf(LOG_ERROR, "modconfig: %s: invalid key or value: %s %s", block, key, token);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated %s block", block);
		return NULL;
	}

	return p;
}

/**
 * koh { hillcolour R G B freecolour R G B }: King of the Hill's colours, as
 * fractions (game/modrules.h). Written by the importer from the mod's code.
 */
static char *modConfigParseKoh(char *p, char *token)
{
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		char key[UTIL_MAX_TOKEN + 1];
		f32 rgb[3];
		strcpy(key, token);
		for (s32 c = 0; c < 3; c++) {
			PARSE_FLOAT("koh", "colour", rgb[c], 0.f, 1.f, NULL);
		}
		if (modKohSetColour(key, rgb) != 1) {
			sysLogPrintf(LOG_ERROR, "modconfig: koh: invalid key: %s", key);
			return NULL;
		}
		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated koh block");
		return NULL;
	}

	return p;
}

/**
 * shieldcolour hit|player { ramp TOP R G B SR SG SB ... constant R G B }
 *
 * The colour of a shield flash by how much shield is left, at one of the two
 * places the game asks (chr.h). Each `ramp` row answers below TOP with
 * base - (int)((TOP - shield) * slope) per channel, the arithmetic of the
 * game's own ramp; `constant` is the flat tail for everything above, and is
 * the last row. A block of only `constant` is one colour whatever the shield,
 * which is what the console mods that rewrote this function do. Written by
 * the importer from running the mod's shieldhit_health_to_rgb.
 */
s32 modShieldColourSiteLookup(const char *name)
{
	if (!strcmp(name, "hit")) {
		return SHIELDCOLOUR_HIT;
	}
	if (!strcmp(name, "player")) {
		return SHIELDCOLOUR_PLAYER;
	}
	return -1;
}

/**
 * Check rows the way the block does - every ramp row a top above 0, the
 * last row and only it constant - and set them. -1 when they are not.
 */
s32 modShieldColourApply(s32 site, const struct shieldcolour *rows, s32 numrows)
{
	if (site < 0 || site >= SHIELDCOLOUR_NUMSITES || numrows < 1 || numrows > SHIELDCOLOUR_MAXROWS) {
		return -1;
	}
	for (s32 i = 0; i < numrows; ++i) {
		const bool last = i == numrows - 1;
		if (last ? rows[i].top != 0.f : rows[i].top <= 0.f || rows[i].top > 64.f) {
			return -1;
		}
		for (s32 c = 0; c < 3; ++c) {
			if (rows[i].slope[c] < -4096.f || rows[i].slope[c] > 4096.f) {
				return -1;
			}
		}
	}
	shieldColourSet(site, rows, numrows);
	return 1;
}

static char *modConfigParseShieldColour(char *p, char *token)
{
	struct shieldcolour rows[SHIELDCOLOUR_MAXROWS];
	s32 numrows = 0;
	s32 site;
	s32 tmp = 0;
	f32 tmpf = 0;
	bool tail = false;

	p = strParseToken(p, token, NULL);
	site = modShieldColourSiteLookup(token);
	if (site < 0) {
		sysLogPrintf(LOG_ERROR, "modconfig: shieldcolour: unknown site %s (hit or player)", token);
		return NULL;
	}

	// eat opening bracket
	p = strParseToken(p, token, NULL);
	if (token[0] != '{' || token[1] != '\0') {
		return NULL;
	}

	p = strParseToken(p, token, NULL);
	while (p && token[0] && strcmp(token, "}") != 0) {
		struct shieldcolour *row = &rows[numrows < SHIELDCOLOUR_MAXROWS ? numrows : SHIELDCOLOUR_MAXROWS - 1];
		bool isramp = !strcmp(token, "ramp");

		if (!isramp && strcmp(token, "constant") != 0) {
			sysLogPrintf(LOG_ERROR, "modconfig: shieldcolour: invalid key: %s", token);
			return NULL;
		}

		if (tail) {
			sysLogPrintf(LOG_ERROR, "modconfig: shieldcolour: constant must be the last row");
			return NULL;
		}

		memset(row, 0, sizeof(*row));

		if (isramp) {
			PARSE_FLOAT("shieldcolour", "ramp top", tmpf, 0.f, 64.f, NULL);
			row->top = tmpf;
			if (row->top <= 0.f) {
				sysLogPrintf(LOG_ERROR, "modconfig: shieldcolour: a ramp row needs a top above 0");
				return NULL;
			}
		}

		for (s32 c = 0; c < 3; c++) {
			PARSE_INT("shieldcolour", "colour", tmp, -32768, 32767, NULL);
			row->base[c] = tmp;
		}

		if (isramp) {
			for (s32 c = 0; c < 3; c++) {
				PARSE_FLOAT("shieldcolour", "slope", tmpf, -4096.f, 4096.f, NULL);
				row->slope[c] = tmpf;
			}
		} else {
			tail = true;
		}

		if (numrows < SHIELDCOLOUR_MAXROWS) {
			numrows++;
		} else {
			sysLogPrintf(LOG_WARNING, "modconfig: shieldcolour: more than %d rows, the rest are dropped", SHIELDCOLOUR_MAXROWS);
		}

		p = strParseToken(p, token, NULL);
	}

	if (token[0] != '}') {
		sysLogPrintf(LOG_ERROR, "modconfig: unterminated shieldcolour block");
		return NULL;
	}

	if (!tail) {
		sysLogPrintf(LOG_ERROR, "modconfig: shieldcolour: needs a constant row for the shield above its ramps");
		return NULL;
	}

	if (modShieldColourApply(site, rows, numrows) < 0) {
		sysLogPrintf(LOG_ERROR, "modconfig: shieldcolour: rows out of range");
		return NULL;
	}

	return p;
}

/**
 * Parse modconfig text and apply it. The buffer is scratch: the tokeniser
 * writes into it. `what` names the source in the log.
 */
s32 modConfigParse(char *data, u32 dataLen, const char *what)
{
	s32 success = true;
	char token[UTIL_MAX_TOKEN + 1] = { 0 };
	char *end = data + dataLen;
	char *p = strParseToken(data, token, NULL);
	while (p && token[0]) {
		if (!strcmp(token, "weaponfunc")) {
			// weaponfunc WEAPON FUNCTION { KEYVALUES... }
			char *prev = p;
			p = modConfigParseWeaponFunc(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed weaponfunc block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "tvscreen")) {
			// tvscreen NUMBER { sameas NUMBER }
			char *prev = p;
			p = modConfigParseTvScreen(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed tvscreen block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "weapon")) {
			// weapon NUMBER { KEYVALUES... }
			char *prev = p;
			p = modConfigParseWeapon(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed weapon block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "datasegment")) {
			// datasegment { file "..." base ADDR weapons ADDR COUNT ... }
			char *prev = p;
			p = modConfigParseDataSegment(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed datasegment block at offset %d", prev - data);
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
		} else if (!strcmp(token, "unlocks")) {
			// unlocks { KEYVALUES... }
			char *prev = p;
			p = modConfigParseUnlocks(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed unlocks block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "weaponfuncflags")) {
			// weaponfuncflags FLAG { clear WEAPON FUNC ... }
			char *prev = p;
			p = modConfigParseWeaponFuncFlags(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed weaponfuncflags block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "pickupqty")) {
			// pickupqty { mp|solo AMMOTYPE QTY ... }
			char *prev = p;
			p = modConfigParsePickupQty(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed pickupqty block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "ammotypeweapon")) {
			// ammotypeweapon { AMMOTYPE WEAPON ... }
			char *prev = p;
			p = modConfigParseAmmoTypeWeapon(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed ammotypeweapon block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "damage")) {
			// damage { KEYVALUES... }
			char *prev = p;
			p = modConfigParseDamage(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed damage block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "weaponflags")) {
			// weaponflags FLAG { clear NUMBER... }
			char *prev = p;
			p = modConfigParseWeaponFlags(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed weaponflags block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "shieldcolour")) {
			// shieldcolour SITE { ramp ... constant ... }
			char *prev = p;
			p = modConfigParseShieldColour(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed shieldcolour block at offset %d", prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "movement") || !strcmp(token, "cheats") || !strcmp(token, "colours")) {
			// movement { fastspeed N fastcheat N }, cheats { slowmotion N }, colours { NAME 0x... }
			char block[UTIL_MAX_TOKEN + 1];
			char *prev = p;
			strcpy(block, token);
			p = modConfigParseRules(p, token, block);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed %s block at offset %d", block, prev - data);
				success = false;
				break;
			}
		} else if (!strcmp(token, "koh")) {
			// koh { hillcolour R G B freecolour R G B }
			char *prev = p;
			p = modConfigParseKoh(p, token);
			if (!p) {
				sysLogPrintf(LOG_ERROR, "modconfig: malformed koh block at offset %d", prev - data);
				success = false;
				break;
			}
		} else {
			// garbage
			sysLogPrintf(LOG_ERROR, "modconfig: %s: unexpected %s at offset %d", what, token[0] ? token : "end of file", p - data);
			success = false;
			break;
		}
		p = strParseToken(p, token, NULL);
	}

	(void)end;
	return success;
}

s32 modConfigLoad(const char *fname)
{
	// A mod need not ship one: files/, segs/ and textures/ each make a mod dir
	// on their own, and an imported console mod has none of this. Asking
	// fsFileLoad() for it anyway logs the miss as an error.
	if (fsFileSize(fname) < 0) {
		return false;
	}

	u32 dataLen = 0;
	char *data = fsFileLoad(fname, &dataLen);
	if (!data) {
		return false;
	}

	s32 success = modConfigParse(data, dataLen, fname);

	sysMemFree(data);
	return success;
}

/**
 * Whether texture ids currently resolve against the running stage's own mod.
 *
 * A texture id means different things to different mods, so it has to be
 * resolved against the mod that supplied the file referencing it. A mod stage's
 * own art wants that mod's textures; a stock prop model that happens to share
 * an id does not, and pointing it at the mod's copy is how ammo crates ended up
 * wearing GoldenEye art on a GoldenEye X map. Model files turn this off while
 * their display lists are scanned.
 */
static s32 g_ModTextureStageOff = 0;

s32 modSetTextureFromStage(s32 on)
{
	const s32 prev = !g_ModTextureStageOff;

	g_ModTextureStageOff = !on;

	return prev;
}

s32 modTextureLoad(u16 num, void *dst, u32 dstSize)
{
	char path[FS_MAXPATH + 1];
	const char *stageDir = g_ModTextureStageOff
		? NULL
		: modloaderGetStageModDir(mainGetStageNum());

	if (stageDir) {
		// one line a stage, not one a texture: GE-X's maps load hundreds
		static const char *loggedDir = NULL;
		static s32 loggedStage = -1;

		snprintf(path, sizeof(path), "%s/" MOD_TEXTURES_DIR "/%04x.bin", stageDir, num);

		const s32 ret = fsFileLoadTo(path, dst, dstSize);
		if (ret > 0) {
			if (loggedDir != stageDir || loggedStage != mainGetStageNum()) {
				loggedDir = stageDir;
				loggedStage = mainGetStageNum();
				sysLogPrintf(LOG_NOTE, "mod: stage 0x%02x draws with textures from %s", loggedStage, stageDir);
			}
			return ret;
		}
	}

	if (modTexturesDirExists < 0) {
		modTexturesDirExists = (fsFileSize(MOD_TEXTURES_DIR) >= 0);
	}

	if (!modTexturesDirExists) {
		return -1;
	}

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
#define MOD_IMPORT_REPORT "IMPORT.txt"
#define MOD_MAX_MODS 128
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

// The Stage Loader's choice (Mod.MapMods): "*" for every installed mod's
// maps, "" for none, else the mods' names separated by ';'. The maps of the
// mods named are mounted for their maps alone, beside whatever mod is
// loaded, and the mod loader gives each map its own stage and arena.
#define MOD_MAPMODS_LEN 2048
static char mapModsSetting[MOD_MAPMODS_LEN];
static s32 numMapDirsMounted;
static s32 modMapsMount(void);

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

/**
 * Archives and console patches dropped where mod directories go.
 *
 * A mod passed around is a zip of its directory, or - for a console mod - a
 * zip holding the ROM patch and a readme, or a whole collection of those. The
 * one step that goes wrong is asking the player to unpack and convert by hand,
 * so the list does it: an archive found in mods/ (or a mod*.zip beside the
 * executable) is unpacked into a directory of its name; a patch found there,
 * or in a folder that was unpacked and is not itself a mod, is imported by
 * modImportPatch() into mods/<patch name>/, which is then an ordinary mod
 * directory. Nested archives are unpacked too, a few levels down, so a
 * collection of mods drops in as one file.
 *
 * Everything is done once: an archive is skipped while its directory exists,
 * a patch while its directory has an IMPORT.txt. Deleting the directory has it
 * done again.
 *
 * Names are collected before anything is written, since writing into a
 * directory while it is being read is undefined on some filesystems.
 */
#define MOD_ENTRY_LEN 256
#define MOD_UNPACK_DEPTH 4

struct modnamelist {
	char (*names)[MOD_ENTRY_LEN];
	s32 count;
	s32 cap;
};

static void modNameListAdd(const char *name, void *arg)
{
	struct modnamelist *list = (struct modnamelist *)arg;

	if (strlen(name) >= MOD_ENTRY_LEN) {
		return;
	}

	if (list->count == list->cap) {
		list->cap = list->cap ? list->cap * 2 : 32;
		list->names = realloc(list->names, list->cap * MOD_ENTRY_LEN);
	}

	snprintf(list->names[list->count++], MOD_ENTRY_LEN, "%s", name);
}

static s32 modNameListCollect(const char *dir, struct modnamelist *list)
{
	list->count = 0;
	return fsScanDir(dir, modNameListAdd, list);
}

static bool modPathIsDir(const char *path)
{
	struct stat st;
	return stat(fsFullPath(path), &st) == 0 && S_ISDIR(st.st_mode);
}

// The name without its extension, cut to what a mod name may be
static void modStemName(const char *name, char *dst, u32 dstlen)
{
	const char *dot = strrchr(name, '.');
	const u32 n = dot && dot != name ? (u32)(dot - name) : (u32)strlen(name);

	snprintf(dst, dstlen, "%.*s", (int)(n < dstlen - 1 ? n : dstlen - 1), name);
}

/**
 * How many entries dest holds, and the name of the last one seen, for telling
 * an archive that wrapped its directory in one folder from a flat one.
 */
struct modsoleentry {
	s32 count;
	char name[MOD_ENTRY_LEN];
};

static void modListSoleEntry(const char *name, void *arg)
{
	struct modsoleentry *sole = (struct modsoleentry *)arg;

	++sole->count;
	snprintf(sole->name, sizeof(sole->name), "%s", name);
}

/**
 * Unpacks dir/name into dir/<stem>, unless that exists. Returns true when the
 * directory is there afterwards, unpacked now or earlier.
 */
static bool modListUnpackOne(const char *dir, const char *name)
{
	char archive[FS_MAXPATH + 1];
	char dest[FS_MAXPATH + 1];
	char destName[MOD_NAME_LEN];
	s32 count;

	modStemName(name, destName, sizeof(destName));

	if (!destName[0]) {
		return false;
	}

	snprintf(dest, sizeof(dest), "%s/%s", dir, destName);

	if (fsFileSize(dest) >= 0) {
		// already unpacked (or a directory of that name was there first)
		return true;
	}

	snprintf(archive, sizeof(archive), "%s/%s", dir, name);
	strncpy(archive, fsFullPath(archive), FS_MAXPATH);
	archive[FS_MAXPATH] = '\0';

	sysLogPrintf(LOG_NOTE, "mod: unpacking %s into %s, this happens once", name, destName);

	{
		char destFull[FS_MAXPATH + 1];

		strncpy(destFull, fsFullPath(dest), FS_MAXPATH);
		destFull[FS_MAXPATH] = '\0';
		count = archiveExtract(archive, destFull);
	}

	if (count <= 0) {
		sysLogPrintf(LOG_ERROR, "mod: nothing came out of %s", name);
		fsRemoveDir(dest); // only goes if it is empty, so a partial unpack stays for a look
		return false;
	}

	// An archive made of the directory rather than its contents lands one
	// folder deep. Hoist that folder up so the mod is where the list looks -
	// and a collection's wrapper folder too, so its own archives are one
	// level nearer the top.
	if (!modListLooksLikeMod(dest)) {
		struct modsoleentry sole = { 0, "" };
		char inner[FS_MAXPATH + 1];

		fsScanDir(dest, modListSoleEntry, &sole);
		snprintf(inner, sizeof(inner), "%s/%s", dest, sole.name);

		if (sole.count == 1 && modPathIsDir(inner)) {
			char tmp[FS_MAXPATH + 1];

			snprintf(tmp, sizeof(tmp), "%s/%s.unpacking", dir, destName);

			if (fsRename(inner, tmp) == 0 && fsRemoveDir(dest) == 0 && fsRename(tmp, dest) == 0) {
				sysLogPrintf(LOG_NOTE, "mod: %s kept its mod in a folder called %s; hoisted", name, sole.name);
			} else {
				sysLogPrintf(LOG_WARNING, "mod: could not hoist %s out of %s; the mod is listed as is", sole.name, destName);
			}
		}
	}

	sysLogPrintf(LOG_NOTE, "mod: unpacked %d files from %s", count, name);

	return true;
}

/**
 * Copies one file, for the texture cache below. Read whole: they are tens of
 * megabytes at most and this happens once.
 */
static bool modCopyFile(const char *srcFull, const char *dstFull)
{
	FILE *in = fopen(srcFull, "rb");
	FILE *out;
	u8 buf[65536];
	size_t n;

	if (!in) {
		return false;
	}

	out = fopen(dstFull, "wb");

	if (!out) {
		fclose(in);
		return false;
	}

	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		fwrite(buf, 1, n, out);
	}

	fclose(in);
	fclose(out);
	return true;
}

/**
 * A console mod's download may carry the emulator texture pack made for it
 * as the plugin's cache file (GE-X: `1964_HIRES_Files/GoldenEye
 * X_HIRESTEXTURES.htc`, and a `.dat` in Glide64's older layout beside it).
 * texpack.c reads those from a mod's textures/, so any found beside the
 * patch, two folders down, is copied there. Returns how many.
 */
static s32 modListAdoptTextureCaches(const char *dir, const char *dest, s32 depth)
{
	struct modnamelist list = { NULL, 0, 0 };
	s32 adopted = 0;

	if (modNameListCollect(dir, &list) < 0) {
		free(list.names);
		return 0;
	}

	for (s32 i = 0; i < list.count; ++i) {
		const char *name = list.names[i];
		const char *dot = strrchr(name, '.');
		char path[FS_MAXPATH + 1];

		snprintf(path, sizeof(path), "%s/%s", dir, name);

		const u32 namelen = strlen(name);
		const bool isDatCache = namelen > 19 && !strcasecmp(name + namelen - 19, "_HIRESTEXTURES.dat");

		if (dot && (!strcasecmp(dot, ".htc") || isDatCache)) {
			char texdir[FS_MAXPATH + 1];
			char target[FS_MAXPATH + 1];
			char srcFull[FS_MAXPATH + 1];

			snprintf(texdir, sizeof(texdir), "%s/textures", dest);
			snprintf(target, sizeof(target), "%s/%s", texdir, name);

			if (fsFileSize(target) >= 0) {
				continue;
			}

			fsCreateDir(texdir);
			strncpy(srcFull, fsFullPath(path), FS_MAXPATH);
			srcFull[FS_MAXPATH] = '\0';

			if (modCopyFile(srcFull, fsFullPath(target))) {
				sysLogPrintf(LOG_NOTE, "mod: %s is an emulator texture cache; copied into the mod's textures/", name);
				++adopted;
			} else {
				sysLogPrintf(LOG_WARNING, "mod: could not copy %s into %s", name, texdir);
			}
		} else if (depth < 2 && modPathIsDir(path) && strcasecmp(name, "textures") && strcasecmp(name, "files")
				&& strcasecmp(name, "segs") && strcasecmp(name, "files.incompatible") && strcasecmp(name, "segs.unlocated")) {
			adopted += modListAdoptTextureCaches(path, dest, depth + 1);
		}
	}

	free(list.names);
	return adopted;
}

// Whether dir holds an IMPORT.txt written by an importer older than this one
static bool modImportIsStale(const char *dir)
{
	char marker[FS_MAXPATH + 1];
	char first[128] = "";
	FILE *f;

	snprintf(marker, sizeof(marker), "%s/" MOD_IMPORT_REPORT, dir);
	f = fsFileOpenRead(marker);

	if (!f) {
		return false;
	}

	if (!fgets(first, sizeof(first), f)) {
		first[0] = '\0';
	}

	fclose(f);

	return strncmp(first, MODIMPORT_VERSION_LINE, strlen(MODIMPORT_VERSION_LINE)) != 0;
}

// Deletes a directory and everything in it, for redoing an import
static void modRemoveTree(const char *dir)
{
	struct modnamelist list = { NULL, 0, 0 };

	if (modNameListCollect(dir, &list) < 0) {
		free(list.names);
		return;
	}

	for (s32 i = 0; i < list.count; ++i) {
		char path[FS_MAXPATH + 1];

		snprintf(path, sizeof(path), "%s/%s", dir, list.names[i]);

		if (modPathIsDir(path)) {
			modRemoveTree(path);
		} else {
			remove(fsFullPath(path));
		}
	}

	free(list.names);
	fsRemoveDir(dir);
}

/**
 * Imports the console patch at patchPath into container/<stem>, the stem
 * prefixed with "mod_" beside the executable so the loose scan sees it. Done
 * once: an IMPORT.txt in the directory, or a mod already there, means so.
 */
static s32 modListImportPatch(const char *container, bool loose, const char *patchPath, const char *name, const char *basePath)
{
	char stem[MOD_NAME_LEN];
	char destName[MOD_NAME_LEN];
	char dest[FS_MAXPATH + 1];
	char marker[FS_MAXPATH + 1];
	char patchFull[FS_MAXPATH + 1];
	char destFull[FS_MAXPATH + 1];

	char baseFull[FS_MAXPATH + 1];
	s32 result;
	bool redo = false;

	modStemName(name, stem, sizeof(stem));

	if (!stem[0]) {
		return -1;
	}

	if (loose && strncasecmp(stem, "mod", 3)) {
		snprintf(destName, sizeof(destName), "mod_%.*s", MOD_NAME_LEN - 5, stem);
	} else {
		snprintf(destName, sizeof(destName), "%s", stem);
	}

	snprintf(dest, sizeof(dest), "%s/%s", container, destName);
	snprintf(marker, sizeof(marker), "%s/" MOD_IMPORT_REPORT, dest);

	if (fsFileSize(marker) >= 0 && !basePath) {
		// imported before, or tried and failed - either way, not again,
		// unless an older importer did it and this one would do it better
		char first[128] = "";
		FILE *f = fsFileOpenRead(marker);

		if (f) {
			if (!fgets(first, sizeof(first), f)) {
				first[0] = '\0';
			}
			fclose(f);
		}

		if (!strncmp(first, MODIMPORT_VERSION_LINE, strlen(MODIMPORT_VERSION_LINE))) {
			return 0;
		}

		sysLogPrintf(LOG_NOTE, "mod: %s was imported by an earlier version; doing it again", destName);
		redo = true;
	}

	if (!redo && fsFileSize(dest) >= 0 && modListLooksLikeMod(dest)) {
		return 0; // a mod of that name is already there
	}

	if (redo) {
		// what the earlier import wrote, so nothing of it outlives this one
		static const char *const outputs[] = { "files", "segs", "textures", "files.incompatible", "segs.unlocated" };

		for (u32 i = 0; i < ARRAYCOUNT(outputs); ++i) {
			char sub[FS_MAXPATH + 1];
			snprintf(sub, sizeof(sub), "%s/%s", dest, outputs[i]);
			if (modPathIsDir(sub)) {
				modRemoveTree(sub);
			}
		}
	}

	fsCreateDir(dest);

	strncpy(patchFull, fsFullPath(patchPath), FS_MAXPATH);
	patchFull[FS_MAXPATH] = '\0';
	strncpy(destFull, fsFullPath(dest), FS_MAXPATH);
	destFull[FS_MAXPATH] = '\0';

	if (basePath) {
		strncpy(baseFull, fsFullPath(basePath), FS_MAXPATH);
		baseFull[FS_MAXPATH] = '\0';
	} else {
		sysLogPrintf(LOG_NOTE, "mod: importing the console patch %s into %s, this happens once", name, destName);
	}

	result = modImportPatch(patchFull, destFull, basePath ? baseFull : NULL);

	if (result >= 1) {
		// the emulator texture pack that came with it, if one did
		char patchDir[FS_MAXPATH + 1];
		const char *slash;

		snprintf(patchDir, sizeof(patchDir), "%s", patchPath);
		slash = strrchr(patchDir, '/');

		if (slash) {
			patchDir[slash - patchDir] = '\0';

			if (modListAdoptTextureCaches(patchDir, dest, 0) > 0) {
				sysLogPrintf(LOG_NOTE, "mod: %s's textures load with the mod once Use Texture Packs is on (Extended Options > Texture Packs; Mod.LoadTextures in pd.ini)", destName);
			}
		}
	}

	if (result == MODIMPORT_NEEDS_BASE) {
		// the caller tries the patches beside it as a base; quiet until then
	} else if (result < 0) {
		sysLogPrintf(LOG_WARNING, "mod: %s could not be imported; %s/" MOD_IMPORT_REPORT " says why", name, destName);
	} else if (!modListLooksLikeMod(dest)) {
		sysLogPrintf(LOG_WARNING, "mod: %s carried nothing the port can use; %s/" MOD_IMPORT_REPORT " says what it changed", name, destName);
	}

	return result;
}

/**
 * Archives in dir become directories, patches in it become mod directories
 * in container, and directories that are not mods are looked into for more
 * of both, to MOD_UNPACK_DEPTH. Beside the executable only names starting
 * with "mod" are touched, that directory being everyone's.
 */
static void modListPrepareDir(const char *container, const char *dir, s32 depth, bool loose)
{
	struct modnamelist list = { NULL, 0, 0 };
	const bool filter = loose && depth == 0;

	if (modNameListCollect(dir, &list) < 0) {
		free(list.names);
		return;
	}

	for (s32 i = 0; i < list.count; ++i) {
		if (archiveIsSupported(list.names[i]) && !(filter && strncasecmp(list.names[i], "mod", 3))) {
			modListUnpackOne(dir, list.names[i]);
		}
	}

	if (modNameListCollect(dir, &list) < 0) {
		free(list.names);
		return;
	}

	// the patches in this directory and how each went, for the second pass
	s32 *results = calloc(list.count ? list.count : 1, sizeof(s32));

	for (s32 i = 0; i < list.count; ++i) {
		const char *name = list.names[i];
		char path[FS_MAXPATH + 1];

		results[i] = -1;

		if (filter && (strncasecmp(name, "mod", 3) || !strcasecmp(name, MOD_MODS_DIR))) {
			// beside the executable only mod* names are touched, and mods/
			// itself is the container scanned in its own right - looking
			// into it from here imported every patch a second time
			continue;
		}

		snprintf(path, sizeof(path), "%s/%s", dir, name);

		if (rompatchIsPatchName(name)) {
			results[i] = modListImportPatch(container, loose, path, name, NULL);
		} else if (depth < MOD_UNPACK_DEPTH && modPathIsDir(path)
				&& (!modListLooksLikeMod(path) || modImportIsStale(path))) {
			// a finished mod is left alone - unless an earlier importer made
			// it, when its patch is still inside and wants doing again
			modListPrepareDir(container, path, depth + 1, loose);
		}
	}

	// A patch that does not apply to the stock ROM may have been made against
	// another mod's - GE Gun Name Display's "PD Names" is a patch on top of
	// its first patch. The ones beside it that did apply are tried as a base.
	for (s32 i = 0; i < list.count; ++i) {
		char path[FS_MAXPATH + 1];

		if (results[i] != MODIMPORT_NEEDS_BASE) {
			continue;
		}

		snprintf(path, sizeof(path), "%s/%s", dir, list.names[i]);

		for (s32 j = 0; j < list.count && results[i] == MODIMPORT_NEEDS_BASE; ++j) {
			char basePath[FS_MAXPATH + 1];

			if (j == i || results[j] < 0 || !rompatchIsPatchName(list.names[j])) {
				continue;
			}

			snprintf(basePath, sizeof(basePath), "%s/%s", dir, list.names[j]);
			sysLogPrintf(LOG_NOTE, "mod: %s does not apply to the stock ROM; trying it on top of %s", list.names[i], list.names[j]);
			results[i] = modListImportPatch(container, loose, path, list.names[i], basePath);
		}

		if (results[i] == MODIMPORT_NEEDS_BASE) {
			sysLogPrintf(LOG_WARNING, "mod: %s does not apply to the stock ROM or on top of any patch beside it; its IMPORT.txt says so", list.names[i]);
		}
	}

	free(results);
	free(list.names);
}

void modListRefresh(void)
{
	static const char *const containers[] = { "$E/" MOD_MODS_DIR, "$H/" MOD_MODS_DIR, "./" MOD_MODS_DIR };
	static const char *const loose[] = { "$E", "." };

	numModsListed = 0;

	for (s32 i = 0; i < ARRAYCOUNT(containers); ++i) {
		modListPrepareDir(containers[i], containers[i], 0, false);
		fsScanDir(containers[i], modListScanEntry, (void *)containers[i]);
	}

	for (s32 i = 0; i < ARRAYCOUNT(loose); ++i) {
		modListPrepareDir(loose[i], loose[i], 0, true);
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
static struct solostage soloStagesSnapshot[NUM_SOLOSTAGES];
static struct weathercfg weatherSnapshot[ARRAYCOUNT(g_WeatherConfig)];
static struct weapon *weaponsSnapshot[WEAPON_SUICIDEPILL + 1];
static struct modelstate modelStatesSnapshot[NUM_MODELS];
static struct mpweapon mpWeaponsSnapshot[NUM_MPWEAPONS];
static struct mpweaponset mpWeaponSetsSnapshot[ARRAYCOUNT(g_MpWeaponSets)];
static struct mparena mpArenasSnapshot[ARRAYCOUNT(g_MpArenas)];
extern struct mptrack g_MpTracks[];
static struct mptrack mpTracksSnapshot[MP_MAX_TRACKS];
static s32 numMpTracksSnapshot;
static struct headorbody headsAndBodiesSnapshot[ARRAYCOUNT(g_HeadsAndBodies)];
static struct mphead mpHeadsSnapshot[ARRAYCOUNT(g_MpHeads)];
static struct mpbody mpBodiesSnapshot[ARRAYCOUNT(g_MpBodies)];
static u32 botHeadsSnapshot[ARRAYCOUNT(g_BotHeads)];
static struct mphead mpBeauHeadsSnapshot[ARRAYCOUNT(g_MpBeauHeads)];
static u32 mpMaleHeadsSnapshot[ARRAYCOUNT(g_MpMaleHeads)];
static u32 mpFemaleHeadsSnapshot[ARRAYCOUNT(g_MpFemaleHeads)];
static struct mplistcounts mpListCountsSnapshot;
static s32 numMpArenasSnapshot;
static bool mpArenasImportedSnapshot;
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
	memcpy(soloStagesSnapshot, g_SoloStages, sizeof(soloStagesSnapshot));
	memcpy(weatherSnapshot, g_WeatherConfig, sizeof(weatherSnapshot));
	memcpy(weaponsSnapshot, g_Weapons, sizeof(weaponsSnapshot));
	memcpy(modelStatesSnapshot, g_ModelStates, sizeof(modelStatesSnapshot));
	memcpy(mpWeaponsSnapshot, g_MpWeapons, sizeof(mpWeaponsSnapshot));
	memcpy(mpWeaponSetsSnapshot, g_MpWeaponSets, sizeof(mpWeaponSetsSnapshot));
	memcpy(mpArenasSnapshot, g_MpArenas, sizeof(mpArenasSnapshot));
	memcpy(mpTracksSnapshot, g_MpTracks, sizeof(mpTracksSnapshot));
	numMpTracksSnapshot = mpGetNumTracks();
	memcpy(headsAndBodiesSnapshot, g_HeadsAndBodies, sizeof(headsAndBodiesSnapshot));
	memcpy(mpHeadsSnapshot, g_MpHeads, sizeof(mpHeadsSnapshot));
	memcpy(mpBodiesSnapshot, g_MpBodies, sizeof(mpBodiesSnapshot));
	memcpy(botHeadsSnapshot, g_BotHeads, sizeof(botHeadsSnapshot));
	memcpy(mpBeauHeadsSnapshot, g_MpBeauHeads, sizeof(mpBeauHeadsSnapshot));
	memcpy(mpMaleHeadsSnapshot, g_MpMaleHeads, sizeof(mpMaleHeadsSnapshot));
	memcpy(mpFemaleHeadsSnapshot, g_MpFemaleHeads, sizeof(mpFemaleHeadsSnapshot));
	mpListCountsSnapshot = g_MpListCounts;
	numMpArenasSnapshot = g_MpNumArenas;
	mpArenasImportedSnapshot = g_MpArenasImported;

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
	memcpy(g_SoloStages, soloStagesSnapshot, sizeof(soloStagesSnapshot));
	memcpy(g_WeatherConfig, weatherSnapshot, sizeof(weatherSnapshot));
	memcpy(g_Weapons, weaponsSnapshot, sizeof(weaponsSnapshot));
	memcpy(g_ModelStates, modelStatesSnapshot, sizeof(modelStatesSnapshot));
	memcpy(g_MpWeapons, mpWeaponsSnapshot, sizeof(mpWeaponsSnapshot));
	memcpy(g_MpWeaponSets, mpWeaponSetsSnapshot, sizeof(mpWeaponSetsSnapshot));
	memcpy(g_MpArenas, mpArenasSnapshot, sizeof(mpArenasSnapshot));
	memcpy(g_MpTracks, mpTracksSnapshot, sizeof(mpTracksSnapshot));
	mpSetNumTracks(numMpTracksSnapshot);
	memcpy(g_HeadsAndBodies, headsAndBodiesSnapshot, sizeof(headsAndBodiesSnapshot));
	memcpy(g_MpHeads, mpHeadsSnapshot, sizeof(mpHeadsSnapshot));
	memcpy(g_MpBodies, mpBodiesSnapshot, sizeof(mpBodiesSnapshot));
	memcpy(g_BotHeads, botHeadsSnapshot, sizeof(botHeadsSnapshot));
	memcpy(g_MpBeauHeads, mpBeauHeadsSnapshot, sizeof(mpBeauHeadsSnapshot));
	memcpy(g_MpMaleHeads, mpMaleHeadsSnapshot, sizeof(mpMaleHeadsSnapshot));
	memcpy(g_MpFemaleHeads, mpFemaleHeadsSnapshot, sizeof(mpFemaleHeadsSnapshot));
	g_MpListCounts = mpListCountsSnapshot;
	g_MpNumArenas = numMpArenasSnapshot;
	g_MpArenasImported = mpArenasImportedSnapshot;
	// the unlocks, the damage rules, and the shield colours: the game's own are compiled in
	g_ModUnlocks = 0;
	g_ModPlayerHeadshotScale = 25;
	g_ModShieldBreakHits = false;
	modRulesReset();
	shieldColourSet(SHIELDCOLOUR_HIT, NULL, 0);
	shieldColourSet(SHIELDCOLOUR_PLAYER, NULL, 0);
	// back to the port's own table before the copy, so an imported one is dropped
	stageSetTracks(NULL);
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
	modMapsMount();

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

	if (modDirsFromArgs) {
		// --moddir was given. An explicit command line is the one the player is
		// looking at, so it wins, and the menu says which is which.
		if (selectedModName[0]) {
			sysLogPrintf(LOG_NOTE, "mod: `%s` is selected but mod dirs came from the command line", selectedModName);
		}
		return;
	}

	if (selectedModName[0]) {
		const s32 index = modListGetSelected();

		if (index < 0) {
			sysLogPrintf(LOG_WARNING, "mod: selected mod `%s` is not installed", selectedModName);
		} else if (fsAddModDir(modList[index].path) >= 0) {
			sysLogPrintf(LOG_NOTE, "mod: mounted `%s`", modList[index].name);
		}
	}

	// after the overlay, so it stays first in the search order
	modMapsMount();
}

/* ---- the Stage Loader: every installed mod's maps, beside the mod loaded --- */

s32 modMapsAllEnabled(void)
{
	return mapModsSetting[0] == '*' && mapModsSetting[1] == '\0';
}

s32 modMapsIsEnabled(const char *name)
{
	const char *p = mapModsSetting;
	const size_t len = strlen(name);

	if (modMapsAllEnabled()) {
		return 1;
	}

	while (*p) {
		const char *end = strchr(p, ';');
		const size_t n = end ? (size_t)(end - p) : strlen(p);
		if (n == len && !strncmp(p, name, len)) {
			return 1;
		}
		if (!end) {
			break;
		}
		p = end + 1;
	}

	return 0;
}

void modMapsSetAll(s32 on)
{
	if (on) {
		strcpy(mapModsSetting, "*");
	} else {
		mapModsSetting[0] = '\0';
	}
}

/**
 * Turn one mod's maps on or off. Leaving "every mod" writes the list of
 * every installed mod but this one, so the others stay as they were.
 */
void modMapsSetEnabled(const char *name, s32 on)
{
	char list[MOD_MAPMODS_LEN] = "";
	u32 len = 0;

	if (modMapsAllEnabled() && on) {
		return;
	}

	for (s32 i = 0; i < numModsListed; ++i) {
		const char *n = modList[i].name;
		const s32 keep = !strcmp(n, name) ? on : modMapsIsEnabled(n);
		if (keep && len + strlen(n) + 2 < sizeof(list)) {
			len += snprintf(list + len, sizeof(list) - len, "%s%s", len ? ";" : "", n);
		}
	}

	strncpy(mapModsSetting, list, sizeof(mapModsSetting) - 1);
	mapModsSetting[sizeof(mapModsSetting) - 1] = '\0';
}

/**
 * Mount the maps of every enabled mod, all but the one loaded, for their maps
 * alone. After the overlay mount at boot, and again after a swap has emptied
 * the mount list. Returns how many were mounted.
 */
static s32 modMapsMount(void)
{
	const char *loaded = fsGetModDir();

	numMapDirsMounted = 0;

	if (modDirsFromArgs) {
		return 0;
	}

	for (s32 i = 0; i < numModsListed; ++i) {
		if (!modMapsIsEnabled(modList[i].name)) {
			continue;
		}
		if (loaded && !strcmp(loaded, modList[i].path)) {
			continue;
		}
		if (fsAddMapsDir(modList[i].path) >= 0) {
			numMapDirsMounted++;
		}
	}

	if (numMapDirsMounted) {
		sysLogPrintf(LOG_NOTE, "mod: %d mod%s mounted for their maps", numMapDirsMounted, numMapDirsMounted == 1 ? "" : "s");
	}

	return numMapDirsMounted;
}

s32 modMapsNumMounted(void)
{
	return numMapDirsMounted;
}

/**
 * Whether what is mounted for maps matches the setting. A change the menu
 * could not apply where it stood (a mod with segments loaded) waits for a
 * restart, and the page says so.
 */
s32 modMapsPending(void)
{
	s32 want = 0;
	const char *loaded = fsGetModDir();

	if (modDirsFromArgs) {
		return 0;
	}

	for (s32 i = 0; i < numModsListed; ++i) {
		if (modMapsIsEnabled(modList[i].name) && !(loaded && !strcmp(loaded, modList[i].path))) {
			want++;
		}
	}

	return want != numMapDirsMounted;
}

/**
 * Apply the setting now if the loaded mod allows a live swap: the same path
 * as choosing a mod, which drops the file slots, restores the tables, mounts
 * again and lets the mod loader register the maps. Returns false when a
 * restart is what it takes.
 */
s32 modMapsApply(void)
{
	return modListSwap(modListGetSelected());
}

PD_CONSTRUCTOR static void modListConfigInit(void)
{
	configRegisterString("Mod.ModDir", selectedModName, sizeof(selectedModName));
	configRegisterString("Mod.MapMods", mapModsSetting, sizeof(mapModsSetting));
}
