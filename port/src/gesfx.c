/**
 * GoldenEye's own sound effects, out of the player's ROM.
 *
 * GoldenEye's sfx.ctl is an ordinary ALBankFile of one bank and one instrument
 * with 261 sounds, and sndPlaySfx() indexes that instrument's soundArray with
 * the SFX_ID itself - through a struct of its own (snd.h's ALInstrumentAlt_s)
 * that puts soundArray at 12 where the instrument has it at 16. So SFX_ID n is
 * the bank's sound n - 1, as Perfect Dark's ids are, and id 0 is nothing. The
 * bank says so itself: its endlessly looped waves are entries 192, 203, 215
 * and 235, one under GAS_LEAK, METAL_SLIDE_LOOP, HEAVY_SINGLE_LOOP and
 * WATCH_STATIC. Read from 0, as this was until the doors were given their
 * sounds, every sound played was the enum's next one: the mode select's door
 * was CONSOLE_ON, and the hum after it was that sound's own second half. The
 * conversion copies the bank and its wave table out of the ROM as menu/sfxctl
 * and menu/sfxtbl.
 *
 * A sound is appended after the game's own the way a borrowed mod's is
 * (modborrow.c): snd.c's loaders add the stock bank's start to every offset
 * they read, so everything the ALSound points at is rebased onto that start,
 * once each, since sounds share envelopes, key maps and waves.
 *
 * **The chain.** Both games' players read a key map's velocityMin and the top
 * of its keyMin as the *next* sound to play with this one, by number - ten
 * bits of GoldenEye's numbering, which here would name a sound of Perfect
 * Dark's, and an appended id does not fit in ten bits. So the link is read out
 * of the bank as it is loaded and taken off the key map, and geSfxPlay() starts
 * every link itself, all at once: a link is not played after the one before
 * it but after its *own* velocityMax thirtieths of a second, which the player
 * underneath still does for a sound started alone. A difficulty's turned page
 * (77, which goes on to 78) and the watch's static (236, to 10) are the two
 * that need it here.
 */
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/libaudio.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "gesfx.h"
#include "modloader.h"
#include "preprocess.h"
#include "system.h"
#include "lib/snd.h"
#include "game/propsnd.h"

#define GESFX_MAX 512

static u8 *g_SfxCtl;
static u8 *g_SfxTbl;
static ALInstrument *g_SfxInst;
static s32 g_SfxSearchedDirs = -1;

// GoldenEye's id -> ours; 0 not looked at yet, -1 none
static s16 g_SfxMap[GESFX_MAX];

// the sound a sound's key map chains to, 0 for none, read before anything in
// the bank is touched since sounds share key maps
static s16 g_SfxNext[GESFX_MAX];

static uintptr_t *g_SfxRebased;
static s32 g_SfxNumRebased;
static s32 g_SfxMaxRebased;

static s32 sfxLoad(void)
{
	char path[FS_MAXPATH + 1];

	if (g_SfxInst) {
		return 1;
	}

	// nothing found, and nothing new to look in
	if (g_SfxSearchedDirs == fsGetNumModDirs()) {
		return 0;
	}

	g_SfxSearchedDirs = fsGetNumModDirs();

	for (s32 i = 0; i < g_SfxSearchedDirs; i++) {
		const char *dir = fsGetModDirAt(i);
		u32 len = 0;
		u32 ctllen = 0;
		u8 *raw;

		if (!dir) {
			continue;
		}

		// asked for first: a load of a file that is not there is an error
		// line of its own, and every mod dir but one has no menu/
		snprintf(path, sizeof(path), "%s/menu/sfxctl", dir);

		if (fsFileSize(path) <= 0 || !(raw = fsFileLoad(path, &len))) {
			continue;
		}

		g_SfxCtl = preprocessALBankFile(raw, len, &ctllen);
		sysMemFree(raw);

		snprintf(path, sizeof(path), "%s/menu/sfxtbl", dir);
		g_SfxTbl = fsFileSize(path) > 0 ? fsFileLoad(path, &len) : NULL;

		if (g_SfxCtl && g_SfxTbl) {
			ALBankFile *file = (ALBankFile *)g_SfxCtl;
			ALBank *bank = (ALBank *)(g_SfxCtl + (uintptr_t)file->bankArray[0]);

			g_SfxInst = (ALInstrument *)(g_SfxCtl + (uintptr_t)bank->instArray[0]);

			for (s32 id = 1; id <= g_SfxInst->soundCount && id < GESFX_MAX; id++) {
				const ALSound *sound = (ALSound *)(g_SfxCtl + (uintptr_t)g_SfxInst->soundArray[id - 1]);
				const ALKeyMap *keymap = sound->keyMap ? (ALKeyMap *)(g_SfxCtl + (uintptr_t)sound->keyMap) : NULL;

				g_SfxNext[id] = keymap ? keymap->velocityMin + (keymap->keyMin & 0xc0) * 4 : 0;
			}

			return 1;
		}

		sysMemFree(g_SfxCtl);
		sysMemFree(g_SfxTbl);
		g_SfxCtl = NULL;
		g_SfxTbl = NULL;
	}

	return 0;
}

static s32 sfxRebaseOnce(uintptr_t off)
{
	for (s32 i = 0; i < g_SfxNumRebased; i++) {
		if (g_SfxRebased[i] == off) {
			return 0;
		}
	}

	if (g_SfxNumRebased == g_SfxMaxRebased) {
		const s32 max = g_SfxMaxRebased ? g_SfxMaxRebased * 2 : 64;
		uintptr_t *grown = sysMemRealloc(g_SfxRebased, max * sizeof(uintptr_t));

		if (!grown) {
			return 0;
		}

		g_SfxRebased = grown;
		g_SfxMaxRebased = max;
	}

	g_SfxRebased[g_SfxNumRebased++] = off;

	return 1;
}

#define GESFX_CTL_DELTA() ((uintptr_t)g_SfxCtl - sndGetCtlStart())

s32 geSfxGet(s32 id)
{
	ALSound *sound;
	uintptr_t off;
	s32 ours;

	if (id <= 0 || id >= GESFX_MAX) {
		return 0;
	}

	if (g_SfxMap[id]) {
		return g_SfxMap[id] > 0 ? g_SfxMap[id] : 0;
	}

	if (!sfxLoad()) {
		// not remembered: the conversion may not have been mounted yet
		return 0;
	}

	if (id > g_SfxInst->soundCount) {
		g_SfxMap[id] = -1;
		return 0;
	}

	off = (uintptr_t)g_SfxInst->soundArray[id - 1];
	sound = (ALSound *)(g_SfxCtl + off);

	if (sfxRebaseOnce(off)) {
		if (sound->envelope) {
			sound->envelope = (ALEnvelope *)((uintptr_t)sound->envelope + GESFX_CTL_DELTA());
		}

		if (sound->keyMap) {
			const uintptr_t koff = (uintptr_t)sound->keyMap;
			ALKeyMap *keymap = (ALKeyMap *)(g_SfxCtl + koff);

			sound->keyMap = (ALKeyMap *)(koff + GESFX_CTL_DELTA());

			if (sfxRebaseOnce(koff)) {
				// the link, which geSfxPlay() follows from g_SfxNext[]
				keymap->velocityMin = 0;
				keymap->keyMin &= ~0xc0;
			}
		}

		if (sound->wavetable) {
			const uintptr_t woff = (uintptr_t)sound->wavetable;
			ALWaveTable *wave = (ALWaveTable *)(g_SfxCtl + woff);

			sound->wavetable = (ALWaveTable *)(woff + GESFX_CTL_DELTA());

			if (sfxRebaseOnce(woff)) {
				wave->base = (u8 *)((uintptr_t)wave->base + (uintptr_t)g_SfxTbl - sndGetTblStart());

				if (wave->type == AL_ADPCM_WAVE) {
					if (wave->waveInfo.adpcmWave.book) {
						wave->waveInfo.adpcmWave.book = (ALADPCMBook *)((uintptr_t)wave->waveInfo.adpcmWave.book + GESFX_CTL_DELTA());
					}

					if (wave->waveInfo.adpcmWave.loop) {
						wave->waveInfo.adpcmWave.loop = (ALADPCMloop *)((uintptr_t)wave->waveInfo.adpcmWave.loop + GESFX_CTL_DELTA());
					}
				} else if (wave->waveInfo.rawWave.loop) {
					wave->waveInfo.rawWave.loop = (ALRawLoop *)((uintptr_t)wave->waveInfo.rawWave.loop + GESFX_CTL_DELTA());
				}
			}
		}
	}

	ours = sndAppendSound(off + GESFX_CTL_DELTA());

	if (ours <= 0) {
		// no bank loaded (--no-sound) or no ids left
		g_SfxMap[id] = -1;
		return 0;
	}

	return g_SfxMap[id] = ours;
}

s32 geSfxPlay(s32 id, s32 volume)
{
	s32 started = 0;

	// the sound and whatever it chains to, which is never long: GoldenEye's
	// own loop has no bound and a bank that looped would hang it
	for (s32 links = 0; id > 0 && id < GESFX_MAX && links < 8; links++) {
		const s32 ours = geSfxGet(id);

		if (ours <= 0) {
			break;
		}

		if (sndStart(var80095200, ours, NULL, volume, -1, -1, -1, -1)) {
			started = 1;
		}

		id = g_SfxNext[id];
	}

	return started;
}

/* ------------------------------------------------------------------------ */
/* A converted level's doors                                                 */
/* ------------------------------------------------------------------------ */

// GoldenEye hears an object's sound at full within 200 of it, down a root
// curve to 5000 and out by 6000 (chrobjSndCreatePostEventDefault()), which is
// the curve Perfect Dark's audio configs still describe; the share of full is
// GESFX_VOLUME's
static s32 g_SfxPropConfig = -1;

// GoldenEye's id -> the config mapping's row + 1
static s16 g_SfxPropRow[GESFX_MAX];

/** GoldenEye's sound as a number psCreate() takes, heard as GoldenEye hears an object; 0 for none. */
static s32 sfxPropNum(s32 id)
{
	s32 ours;
	s32 row;

	if (id <= 0 || id >= GESFX_MAX) {
		return 0;
	}

	if (g_SfxPropRow[id]) {
		return 0x8000 | (g_SfxPropRow[id] - 1);
	}

	ours = geSfxGet(id);

	if (ours <= 0) {
		return 0;
	}

	if (g_SfxPropConfig < 0) {
		const struct audioconfig config = { 200, 5000, 6000, -1, GESFX_VOLUME * 100 / AL_VOL_FULL, -1, 0, 0 };

		g_SfxPropConfig = sndAppendAudioConfig(&config);
	}

	row = g_SfxPropConfig >= 0 ? sndAppendRussMapping(ours, g_SfxPropConfig) : -1;

	if (row < 0) {
		// no rows left: the sound itself, on Perfect Dark's own falloff
		return ours;
	}

	g_SfxPropRow[id] = row + 1;

	return 0x8000 | row;
}

/** The sound and whatever it chains to (geSfxPlay()), from a prop. */
static void sfxPlayAtProp(s32 id, struct prop *prop, s32 type, u16 flags)
{
	for (s32 links = 0; id > 0 && id < GESFX_MAX && links < 8; links++) {
		const s32 num = sfxPropNum(id);

		if (!num) {
			break;
		}

		psCreate(NULL, prop, num, -1, -1, flags, 0, type, 0, -1, 0, -1, -1, -1, -1);

		id = g_SfxNext[id];
	}
}

#define SFX_TRAIN_SLIDE        7
#define SFX_WOOD_CLOSE         187
#define SFX_WOOD_OPEN          188
#define SFX_WOOD_SLIDE         191
#define SFX_TRAIN_CATCH        192
#define SFX_SHUTTER_OPEN       194
#define SFX_SHUTTER_CLOSE      195
#define SFX_METAL_OPEN         196
#define SFX_METAL_CLOSE        197
#define SFX_METAL_CLOSE2       199
#define SFX_METAL_OPEN3        200
#define SFX_METAL_CLOSE3       201
#define SFX_METAL_SLIDE_OPEN   202
#define SFX_METAL_SLIDE_CLOSE  203
#define SFX_METAL_SLIDE_LOOP   204
#define SFX_SMART_CATCH        210
#define SFX_SMART_SLIDE        211
#define SFX_HEAVY_SLIDE_OPEN   214
#define SFX_HEAVY_SLIDE_CLOSE  215
#define SFX_HEAVY_SLIDE_LOOP   216
#define SFX_HYDRAL_CLOSE       218
#define SFX_HYDRAL_OPEN        219
#define SFX_STONE_OPEN         225
#define SFX_STONE_CLOSE        226

#define GESFX_NUM_DOOR_TYPES 18

/**
 * GoldenEye's propobj.c, doorPlayOpenSound0/1() and doorPlayCloseSound0/1(),
 * by DOOR_OPEN_SOUND. A sound is started one of two ways there. `once` is
 * given its volume where the door is and let go: nothing stops it, and it
 * rings out over whatever the door does next. `held` goes in one of the door's
 * two sound states: its volume follows the player, and it is what the door's
 * next sound stops - the slide under a moving door, which is a looped wave
 * for the metal and the heavy doors.
 */
static const struct {
	u8 once[2];
	u8 held;
} g_SfxDoors[4][GESFX_NUM_DOOR_TYPES] = {
	[GESFX_DOOR_OPENING] = {
		[1]  = { { SFX_SMART_CATCH }, SFX_SMART_SLIDE },
		[2]  = { { SFX_SMART_CATCH }, SFX_TRAIN_SLIDE },
		[3]  = { { SFX_METAL_SLIDE_OPEN }, SFX_METAL_SLIDE_LOOP },
		[4]  = { { SFX_HEAVY_SLIDE_OPEN }, SFX_HEAVY_SLIDE_LOOP },
		[5]  = { { SFX_WOOD_OPEN } },
		[6]  = { { SFX_TRAIN_SLIDE } },
		[7]  = { { SFX_TRAIN_CATCH }, SFX_WOOD_SLIDE },
		[8]  = { { SFX_WOOD_OPEN }, SFX_TRAIN_SLIDE },
		[9]  = { { 0 }, SFX_SHUTTER_OPEN },
		[10] = { { SFX_METAL_OPEN } },
		[11] = { { SFX_TRAIN_SLIDE } },
		[12] = { { SFX_METAL_OPEN3 } },
		[13] = { { SFX_TRAIN_SLIDE }, SFX_TRAIN_SLIDE },
		[14] = { { 0 }, SFX_HYDRAL_CLOSE },
		[15] = { { 0 }, SFX_STONE_OPEN },
		[16] = { { SFX_HEAVY_SLIDE_OPEN } },
		[17] = { { SFX_TRAIN_SLIDE, SFX_METAL_SLIDE_OPEN }, SFX_METAL_SLIDE_LOOP },
	},
	// the opening's, less the five that swing: a door that swings shut is
	// not heard until it closes
	[GESFX_DOOR_CLOSING] = {
		[1]  = { { SFX_SMART_CATCH }, SFX_SMART_SLIDE },
		[2]  = { { SFX_SMART_CATCH }, SFX_TRAIN_SLIDE },
		[3]  = { { SFX_METAL_SLIDE_OPEN }, SFX_METAL_SLIDE_LOOP },
		[4]  = { { SFX_HEAVY_SLIDE_OPEN }, SFX_HEAVY_SLIDE_LOOP },
		[7]  = { { SFX_TRAIN_CATCH }, SFX_WOOD_SLIDE },
		[8]  = { { SFX_WOOD_OPEN }, SFX_TRAIN_SLIDE },
		[9]  = { { 0 }, SFX_SHUTTER_OPEN },
		[13] = { { SFX_TRAIN_SLIDE }, SFX_TRAIN_SLIDE },
		[14] = { { 0 }, SFX_HYDRAL_CLOSE },
		[15] = { { 0 }, SFX_STONE_OPEN },
		[16] = { { SFX_HEAVY_SLIDE_OPEN } },
		[17] = { { SFX_TRAIN_SLIDE, SFX_METAL_SLIDE_OPEN }, SFX_METAL_SLIDE_LOOP },
	},
	[GESFX_DOOR_OPENED] = {
		[1]  = { { SFX_SMART_CATCH } },
		[2]  = { { SFX_SMART_CATCH } },
		[3]  = { { SFX_METAL_SLIDE_CLOSE } },
		[4]  = { { SFX_HEAVY_SLIDE_CLOSE } },
		[7]  = { { SFX_SMART_CATCH } },
		[8]  = { { SFX_WOOD_CLOSE } },
		[9]  = { { SFX_SHUTTER_CLOSE } },
		[13] = { { SFX_TRAIN_SLIDE } },
		[14] = { { SFX_HYDRAL_OPEN } },
		[15] = { { SFX_STONE_CLOSE } },
		[16] = { { SFX_HEAVY_SLIDE_CLOSE } },
		[17] = { { SFX_METAL_SLIDE_CLOSE } },
	},
	[GESFX_DOOR_CLOSED] = {
		[1]  = { { SFX_SMART_CATCH } },
		[2]  = { { SFX_SMART_CATCH } },
		[3]  = { { SFX_METAL_SLIDE_CLOSE } },
		[4]  = { { SFX_HEAVY_SLIDE_CLOSE } },
		[5]  = { { SFX_WOOD_CLOSE } },
		[6]  = { { SFX_TRAIN_SLIDE } },
		[7]  = { { SFX_SMART_CATCH } },
		[8]  = { { SFX_WOOD_CLOSE } },
		[9]  = { { SFX_SHUTTER_CLOSE } },
		[10] = { { SFX_METAL_CLOSE } },
		[11] = { { SFX_METAL_CLOSE2 } },
		[12] = { { SFX_METAL_CLOSE3 } },
		[13] = { { SFX_TRAIN_SLIDE } },
		[14] = { { SFX_HYDRAL_OPEN } },
		[15] = { { SFX_STONE_CLOSE } },
		[16] = { { SFX_HEAVY_SLIDE_CLOSE } },
		[17] = { { SFX_METAL_SLIDE_CLOSE } },
	},
};

s32 geSfxDoor(s32 moment, s32 soundtype, struct prop *prop)
{
	if (!modloaderStageIsRemake(g_Vars.stagenum) || !sfxLoad()) {
		return 0;
	}

	if (moment < 0 || moment > GESFX_DOOR_CLOSED || soundtype <= 0 || soundtype >= GESFX_NUM_DOOR_TYPES) {
		return 1;
	}

	for (s32 i = 0; i < 2; i++) {
		// PSFLAG_0400 is GoldenEye's: the volume taken once, where the door is
		sfxPlayAtProp(g_SfxDoors[moment][soundtype].once[i], prop, PSTYPE_GENERAL, PSFLAG_0400);
	}

	// GoldenEye holds a door's own sound at nothing while the controls are
	// locked (sub_GAME_7F053A3C())
	if (!g_Vars.in_cutscene) {
		sfxPlayAtProp(g_SfxDoors[moment][soundtype].held, prop, PSTYPE_DOOR, 0);
	}

	return 1;
}

/* ------------------------------------------------------------------------ */
/* What a converted level hands the player                                   */
/* ------------------------------------------------------------------------ */

s32 geSfxPickup(s32 pdsound, struct prop *prop)
{
	s32 id;

	switch (pdsound) {
	case SFX_PICKUP_SHIELD:  id = 81;  break; // ARMOUR_COLLECT_SFX
	case SFX_PICKUP_KEYCARD: id = 229; break; // KEYCARD_SFX
	case SFX_PICKUP_GUN:     id = 232; break;
	case SFX_PICKUP_KNIFE:   id = 233; break;
	case SFX_PICKUP_AMMO:    id = 234; break;
	case SFX_PICKUP_MINE:    id = 235; break;
	case SFX_PICKUP_LASER:   id = 242; break;
	default:
		return 0;
	}

	if (!modloaderStageIsRemake(g_Vars.stagenum) || geSfxGet(id) <= 0) {
		return 0;
	}

	if (prop) {
		sfxPlayAtProp(id, prop, PSTYPE_NONE, PSFLAG_0400);
	} else {
		geSfxPlay(id, GESFX_VOLUME);
	}

	return 1;
}
