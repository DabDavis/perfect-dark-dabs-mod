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
#include <math.h>
#include <ultra64.h>
#include <PR/libaudio.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "gesfx.h"
#include "gexfront.h"
#include "modloader.h"
#include "preprocess.h"
#include "system.h"
#include "lib/audiodma.h"
#include "lib/snd.h"
#include "game/propsnd.h"

#define GESFX_MAX 512
// the converted mods a bank is held for: GoldenEye's, and a ROM hack's or two
#define GESFX_MAX_BANKS 4

/**
 * A conversion's sound effects: GoldenEye's, or a GoldenEye ROM hack's own
 * (Goldfinger 64's wave table is a quarter again GoldenEye's, on GoldenEye's
 * numbering). A level plays its own mod's, the folder screens theirs
 * (sfxBank()), and a mod with none GoldenEye's.
 */
struct sfxbank {
	s32 moddir;
	u8 *ctl;
	u8 *tbl;
	ALInstrument *inst;
	// GoldenEye's id -> ours; 0 not looked at yet, -1 none
	s16 map[GESFX_MAX];
	// the sound a sound's key map chains to, 0 for none, read before anything
	// in the bank is touched since sounds share key maps
	s16 next[GESFX_MAX];
	// GoldenEye's id -> the config mapping's row + 1 (sfxPropNum())
	s16 proprow[GESFX_MAX];
	uintptr_t *rebased;
	s32 numrebased;
	s32 maxrebased;
};

static struct sfxbank g_SfxBanks[GESFX_MAX_BANKS];
static s32 g_SfxNumBanks;
// the dirs looked in and found without a bank, a bit a dir, and how many
// dirs were mounted then
static u32 g_SfxNoBank;
static s32 g_SfxNumDirs = -1;

// ours -> GoldenEye's id and its bank + 1, for the chain (geSfxChain())
static s16 g_SfxGeId[SND_MAX_SOUNDS];
static u8 g_SfxGeBank[SND_MAX_SOUNDS];

// mod dir `moddir`'s bank, read the first time it is asked for; NULL where it has none
static struct sfxbank *sfxBankAt(s32 moddir)
{
	char path[FS_MAXPATH + 1];
	const char *dir = moddir >= 0 ? fsGetModDirAt(moddir) : NULL;
	struct sfxbank *b;
	u32 len = 0;
	u32 ctllen = 0;
	u8 *raw;
	u8 *ctl;
	u8 *tbl;

	if (!dir) {
		return NULL;
	}

	for (s32 i = 0; i < g_SfxNumBanks; i++) {
		if (g_SfxBanks[i].moddir == moddir) {
			return &g_SfxBanks[i];
		}
	}

	// mods mounted since: look again
	if (g_SfxNumDirs != fsGetNumModDirs()) {
		g_SfxNumDirs = fsGetNumModDirs();
		g_SfxNoBank = 0;
	}

	if (moddir < 32 && (g_SfxNoBank & (1u << moddir))) {
		return NULL;
	}

	// asked for first: a load of a file that is not there is an error
	// line of its own, and every mod dir but one has no menu/
	snprintf(path, sizeof(path), "%s/menu/sfxctl", dir);

	if (g_SfxNumBanks >= GESFX_MAX_BANKS || fsFileSize(path) <= 0 || !(raw = fsFileLoad(path, &len))) {
		if (moddir < 32) {
			g_SfxNoBank |= 1u << moddir;
		}

		return NULL;
	}

	ctl = preprocessALBankFile(raw, len, &ctllen);
	sysMemFree(raw);

	snprintf(path, sizeof(path), "%s/menu/sfxtbl", dir);
	// padded: the sound DMA reads a whole item from where a sample starts
	tbl = fsFileSize(path) > 0 ? fsFileLoadPadded(path, &len, ADMA_ITEM_SIZE) : NULL;

	if (!ctl || !tbl) {
		sysMemFree(ctl);
		sysMemFree(tbl);

		if (moddir < 32) {
			g_SfxNoBank |= 1u << moddir;
		}

		return NULL;
	}

	b = &g_SfxBanks[g_SfxNumBanks++];
	memset(b, 0, sizeof(*b));
	b->moddir = moddir;
	b->ctl = ctl;
	b->tbl = tbl;

	{
		ALBankFile *file = (ALBankFile *)b->ctl;
		ALBank *bank = (ALBank *)(b->ctl + (uintptr_t)file->bankArray[0]);

		b->inst = (ALInstrument *)(b->ctl + (uintptr_t)bank->instArray[0]);
	}

	for (s32 id = 1; id <= b->inst->soundCount && id < GESFX_MAX; id++) {
		const ALSound *sound = (ALSound *)(b->ctl + (uintptr_t)b->inst->soundArray[id - 1]);
		const ALKeyMap *keymap = sound->keyMap ? (ALKeyMap *)(b->ctl + (uintptr_t)sound->keyMap) : NULL;

		b->next[id] = keymap ? keymap->velocityMin + (keymap->keyMin & 0xc0) * 4 : 0;
	}

	sysLogPrintf(LOG_NOTE, "gesfx: %d sounds from %s", b->inst->soundCount, dir);

	return b;
}

/** GoldenEye's own: the conversion's dir (GEXPLUSROM_DIR), else the first mounted dir that has one. */
static struct sfxbank *sfxBankGoldenEye(void)
{
	struct sfxbank *b = sfxBankAt(modloaderGexPlusDirIndex());

	for (s32 i = 0; !b && i < fsGetNumModDirs(); i++) {
		b = sfxBankAt(i);
	}

	return b;
}

/**
 * The bank playing now: a remake level's own mod's in a level, the folder's
 * (GoldenEye's, or a ROM hack's) in its menus, GoldenEye's where that has
 * none.
 */
static struct sfxbank *sfxBank(void)
{
	s32 moddir;
	struct sfxbank *b = NULL;

	if (modloaderStageIsRemake(g_Vars.stagenum)) {
		moddir = modloaderGetStageModDirIndex(g_Vars.stagenum);
	} else {
		moddir = gexFrontTextModDir();
	}

	if (moddir >= 0) {
		b = sfxBankAt(moddir);
	}

	return b ? b : sfxBankGoldenEye();
}

static s32 sfxRebaseOnce(struct sfxbank *b, uintptr_t off)
{
	for (s32 i = 0; i < b->numrebased; i++) {
		if (b->rebased[i] == off) {
			return 0;
		}
	}

	if (b->numrebased == b->maxrebased) {
		const s32 max = b->maxrebased ? b->maxrebased * 2 : 64;
		uintptr_t *grown = sysMemRealloc(b->rebased, max * sizeof(uintptr_t));

		if (!grown) {
			return 0;
		}

		b->rebased = grown;
		b->maxrebased = max;
	}

	b->rebased[b->numrebased++] = off;

	return 1;
}

#define GESFX_CTL_DELTA(b) ((uintptr_t)(b)->ctl - sndGetCtlStart())

static s32 sfxGetIn(struct sfxbank *b, s32 id)
{
	ALSound *sound;
	uintptr_t off;
	s32 ours;

	if (!b || id <= 0 || id >= GESFX_MAX) {
		return 0;
	}

	if (b->map[id]) {
		return b->map[id] > 0 ? b->map[id] : 0;
	}

	if (id > b->inst->soundCount) {
		b->map[id] = -1;
		return 0;
	}

	off = (uintptr_t)b->inst->soundArray[id - 1];
	sound = (ALSound *)(b->ctl + off);

	if (sfxRebaseOnce(b, off)) {
		if (sound->envelope) {
			sound->envelope = (ALEnvelope *)((uintptr_t)sound->envelope + GESFX_CTL_DELTA(b));
		}

		if (sound->keyMap) {
			const uintptr_t koff = (uintptr_t)sound->keyMap;
			ALKeyMap *keymap = (ALKeyMap *)(b->ctl + koff);

			sound->keyMap = (ALKeyMap *)(koff + GESFX_CTL_DELTA(b));

			if (sfxRebaseOnce(b, koff)) {
				// the link, which geSfxPlay() follows from the bank's next[]
				keymap->velocityMin = 0;
				keymap->keyMin &= ~0xc0;
			}
		}

		if (sound->wavetable) {
			const uintptr_t woff = (uintptr_t)sound->wavetable;
			ALWaveTable *wave = (ALWaveTable *)(b->ctl + woff);

			sound->wavetable = (ALWaveTable *)(woff + GESFX_CTL_DELTA(b));

			if (sfxRebaseOnce(b, woff)) {
				wave->base = (u8 *)((uintptr_t)wave->base + (uintptr_t)b->tbl - sndGetTblStart());

				if (wave->type == AL_ADPCM_WAVE) {
					if (wave->waveInfo.adpcmWave.book) {
						wave->waveInfo.adpcmWave.book = (ALADPCMBook *)((uintptr_t)wave->waveInfo.adpcmWave.book + GESFX_CTL_DELTA(b));
					}

					if (wave->waveInfo.adpcmWave.loop) {
						wave->waveInfo.adpcmWave.loop = (ALADPCMloop *)((uintptr_t)wave->waveInfo.adpcmWave.loop + GESFX_CTL_DELTA(b));
					}
				} else if (wave->waveInfo.rawWave.loop) {
					wave->waveInfo.rawWave.loop = (ALRawLoop *)((uintptr_t)wave->waveInfo.rawWave.loop + GESFX_CTL_DELTA(b));
				}
			}
		}
	}

	ours = sndAppendSound(off + GESFX_CTL_DELTA(b));

	if (ours <= 0) {
		// no bank loaded (--no-sound) or no ids left
		b->map[id] = -1;
		return 0;
	}

	if (ours < SND_MAX_SOUNDS) {
		g_SfxGeId[ours] = id;
		g_SfxGeBank[ours] = (u8)(b - g_SfxBanks + 1);
	}

	return b->map[id] = ours;
}

s32 geSfxGet(s32 id)
{
	if (id <= 0 || id >= GESFX_MAX) {
		return 0;
	}

	// not remembered when there is no bank: the conversion may not have been
	// mounted yet
	return sfxGetIn(sfxBank(), id);
}

s32 geSfxIsBankSound(s32 ours)
{
	return ours > 0 && ours < SND_MAX_SOUNDS && g_SfxGeBank[ours] != 0;
}

s32 geSfxChain(s32 ours)
{
	const s32 id = ours > 0 && ours < SND_MAX_SOUNDS ? g_SfxGeId[ours] : 0;
	struct sfxbank *b = id > 0 ? &g_SfxBanks[g_SfxGeBank[ours] - 1] : NULL;

	return b && b->next[id] != id ? sfxGetIn(b, b->next[id]) : 0;
}

s32 geSfxCurveVolume(f32 dist, s32 full)
{
	s32 vol;

	// sub_GAME_7F0537B8(dist, 5000, 6000), out of SHRT_MAX
	if (dist <= 200.0f) {
		vol = 0x7fff;
	} else if (dist >= 6000.0f) {
		vol = 0;
	} else if (dist >= 5000.0f) {
		vol = (6000.0f - dist) * 10000.0f / 1000.0f;
	} else {
		vol = 0x7fff - (s32)(sqrtf(dist - 200.0f) * 22767.0f / sqrtf(4800.0f));
	}

	return (s32)((s64)vol * full / 0x7fff);
}

s32 geChrRocketLaunchSilent(s32 weaponnum)
{
	return weaponnum == WEAPON_GE_ROCKETLAUNCHER && geSfxStage() && geSfxGet(GESFX_ROCKET_LAUNCH) > 0;
}

s32 geSfxPlay(s32 id, s32 volume)
{
	const s32 ours = geSfxGet(id);

	// whatever it chains to follows it in the player (geSfxChain())
	return ours > 0 && sndStart(var80095200, ours, NULL, volume, -1, -1, -1, -1) != NULL;
}

/* ------------------------------------------------------------------------ */
/* A converted level's doors                                                 */
/* ------------------------------------------------------------------------ */

// GoldenEye hears an object's sound at full within 200 of it, down a root
// curve to 5000 and out by 6000 (chrobjSndCreatePostEventDefault()), which is
// the curve Perfect Dark's audio configs still describe; the share of full is
// GESFX_VOLUME's
static s32 g_SfxPropConfig = -1;

/** GoldenEye's sound as a number psCreate() takes, heard as GoldenEye hears an object; 0 for none. */
static s32 sfxPropNumIn(struct sfxbank *b, s32 id)
{
	s32 ours;
	s32 row;

	if (!b || id <= 0 || id >= GESFX_MAX) {
		return 0;
	}

	if (b->proprow[id]) {
		return 0x8000 | (b->proprow[id] - 1);
	}

	ours = sfxGetIn(b, id);

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

	b->proprow[id] = row + 1;

	return 0x8000 | row;
}

static s32 sfxPropNum(s32 id)
{
	return sfxPropNumIn(sfxBank(), id);
}

s32 geSfxStage(void)
{
	return modloaderStageIsRemake(g_Vars.stagenum) && sfxBank();
}

s32 geSfxNum(s32 id)
{
	return geSfxStage() ? sfxPropNum(id) : 0;
}

/** The sound, from a prop or from a place. */
void geSfxPlayAt(s32 id, struct prop *prop, struct coord *pos, RoomNum *rooms, s32 type, u16 flags)
{
	const s32 num = sfxPropNum(id);

	if (num) {
		psCreate(NULL, prop, num, -1, -1, flags, 0, type, pos, -1, rooms, -1, -1, -1, -1);
	}
}

static void sfxPlayAtProp(s32 id, struct prop *prop, s32 type, u16 flags)
{
	geSfxPlayAt(id, prop, NULL, NULL, type, flags);
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
	if (!modloaderStageIsRemake(g_Vars.stagenum) || !sfxBank()) {
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

/* ------------------------------------------------------------------------ */
/* Every other sound of a converted level                                    */
/* ------------------------------------------------------------------------ */

/**
 * Perfect Dark's sounds 1 to 261 are GoldenEye's, number for number: its bank
 * grew from GoldenEye's, and everywhere its code descends from GoldenEye's it
 * still asks for GoldenEye's number - a body's thud, a yelp, a ricochet, a
 * surface hit, glass, an explosion, a casing, a reload, a switch, the alarm;
 * even its door table is GoldenEye's. What changed is the sample in the slot
 * (all but four of the 261 were re-recorded or replaced). So on a converted
 * level a sound of those numbers is GoldenEye's own out of the ROM, which
 * takes care of every site at once and of a gun's shot too.
 *
 * Left alone are the slots Perfect Dark's *own* code plays for a meaning of
 * its own: 9 and 55 are its "no sound" (55 is GoldenEye's evil laugh), 2 the
 * Horizon Scanner, 7 the sight's lock, 16 a bottle, 43 a menu, 100 a shield,
 * 101 the laser's stream, 245 the hoverbike. And 62, its HUD message beep, is
 * GoldenEye's tank: GoldenEye prints a message in silence, so it is silent.
 */
static s32 sfxRemappable(s32 id)
{
	switch (id) {
	case 2: case 7: case 9: case 16: case 43: case 55: case 100: case 101: case 245:
		return 0;
	}

	return id > 0 && id <= 261;
}

s32 geSfxRemaps(s32 id)
{
	return sfxRemappable(id) && geSfxStage();
}

s32 geSfxRemap(s32 id)
{
	s32 ours;

	if (!geSfxRemaps(id)) {
		return id;
	}

	if (id == 62) {
		return 0;
	}

	ours = geSfxGet(id);

	return ours > 0 ? ours : id;
}

s32 geSfxOr(s32 id, s32 pdsound)
{
	const s32 num = geSfxNum(id);

	return num ? num : pdsound;
}

s32 geSfxOurs(s32 id, s32 pdsound)
{
	const s32 ours = geSfxStage() ? geSfxGet(id) : 0;

	return ours > 0 ? ours : pdsound;
}

s32 geSfxNumRange(s32 id, f32 dist2, f32 dist3)
{
	static struct { s16 id; s16 row; f32 dist2; f32 dist3; struct sfxbank *bank; } made[16];
	static s32 nummade;
	struct sfxbank *b = geSfxStage() ? sfxBank() : NULL;
	const s32 ours = b ? sfxGetIn(b, id) : 0;
	struct audioconfig config = { 200, dist2, dist3, -1, GESFX_VOLUME * 100 / AL_VOL_FULL, -1, 0, 0 };
	s32 confignum;
	s32 row;

	if (ours <= 0) {
		return 0;
	}

	for (s32 i = 0; i < nummade; i++) {
		if (made[i].id == id && made[i].bank == b && made[i].dist2 == dist2 && made[i].dist3 == dist3) {
			return 0x8000 | made[i].row;
		}
	}

	if (nummade >= 16) {
		return sfxPropNum(id);
	}

	confignum = sndAppendAudioConfig(&config);
	row = confignum >= 0 ? sndAppendRussMapping(ours, confignum) : -1;

	if (row < 0) {
		return sfxPropNum(id);
	}

	made[nummade].id = id;
	made[nummade].row = row;
	made[nummade].dist2 = dist2;
	made[nummade].dist3 = dist3;
	made[nummade].bank = b;
	nummade++;

	return 0x8000 | row;
}

/* ------------------------------------------------------------------------ */
/* GoldenEye's guns on a stage of Perfect Dark's                             */
/* ------------------------------------------------------------------------ */

/**
 * On a converted level a GoldenEye gun's sounds are GoldenEye's by the remap
 * above: its shot is GoldenEye's own number (gegunsShootSound()), and the
 * clicks of a reload that its host's animation asks for are numbers under 262,
 * which are GoldenEye's too. Everywhere else they were the host's - the PP7
 * fired with the Falcon 2's shot, the shotguns with the Shotgun's (F3
 * 20260924-035424, "in perfect dark mode, almost all goldeneye weapons use the
 * wrong sound effects") - and the remap cannot be turned on for a whole stage
 * of Perfect Dark's, whose own guns and walls play the same numbers. So the
 * gun asks for GoldenEye's sample itself: the bank is loaded the first time
 * one of its guns does (about 800 KB, from the heap, kept after), and nothing
 * of Perfect Dark's own changes.
 */
s32 geSfxGuns(void)
{
	return sfxBank() != NULL;
}

// the numbers geSfxGunShot() gave in place of a slot the remap leaves alone,
// which are heard as the remapped ones are (geSfxHeardAsRemapped())
static u8 g_SfxShotAlias[SND_MAX_SOUNDS];

s32 geSfxHeardAsRemapped(s32 num)
{
	return num > 0 && num < SND_MAX_SOUNDS && g_SfxShotAlias[num] && geSfxStage();
}

s32 geSfxGunShot(s32 id)
{
	if (modloaderStageIsRemake(g_Vars.stagenum)) {
		if (!geSfxStage()) {
			return 0;
		}

		// a slot the remap leaves Perfect Dark's: GoldenEye's guns fire with
		// none of them, but a ROM hack's may - Goldfinger 64's Luger and MP40
		// with 100 and 101 (GoldenEye's taser, Perfect Dark's shield and
		// laser stream: "luger sounds like electricity"), its M1 Carbine with
		// 55 (Perfect Dark's "no sound"). Its own sample, by the number the
		// remap cannot reach (F3 20261003-062234-63c5b872 and five more)
		if (!sfxRemappable(id)) {
			const s32 ours = geSfxGet(id);

			if (ours > 0 && ours < SND_MAX_SOUNDS) {
				g_SfxShotAlias[ours] = 1;
			}

			return ours > 0 ? ours : 0;
		}

		return id;
	}

	// a stage of Perfect Dark's: GoldenEye's own gun's sample
	return sfxPropNumIn(sfxBankGoldenEye(), id);
}

/**
 * And the other way round: on a converted level (a GoldenEye Arenas map, a GE
 * Plus mission) one of Perfect Dark's own guns - the Randomizer's, a
 * simulant's, one the player starts armed with - asks for the numbers under
 * 262 as it always has, and the remap above gave it GoldenEye's sample of that
 * number: the Falcon 2's shot (102) was GoldenEye's gas, and the MagSec's,
 * DY357's, CMP150's, Laptop Gun's and Dragon's shots (121 116 110 113 117) and
 * the magazine clicks of a reload (83, the Mauler's 200) were GoldenEye's too
 * (F3 20260928-030000 and eight more, Odeyseis, "wrong sounds only on
 * GoldenEye maps"). So its sound goes to the player under a
 * second number for the same sample of Perfect Dark's (sndAppendSoundCopy()),
 * one past the remap's reach. WEAPON_UNARMED is GoldenEye's slappers there
 * (geslappers.c) and keeps GoldenEye's.
 */

// Perfect Dark's sound id -> its copy past the remap; 0 not made yet, -1 none
static s16 g_SfxPdCopy[262];

static s32 sfxPdCopy(s32 id)
{
	if (!g_SfxPdCopy[id]) {
		const s32 copy = sndAppendSoundCopy(id);

		g_SfxPdCopy[id] = copy > 0 ? copy : -1;
	}

	return g_SfxPdCopy[id];
}

// a sound number with a config -> the row appended for the replacing sample
// under the same config, + 1; -1 for none. One table for GoldenEye's samples
// on a stage of Perfect Dark's, one for Perfect Dark's copies on a converted
// level.
static s16 g_SfxGunRow[SND_RUSS_CAPACITY];
static s16 g_SfxPdGunRow[SND_RUSS_CAPACITY];

// pdonly: only the case of Perfect Dark's gun on a converted level
static s32 sfxGunSound(s32 weaponnum, s32 soundnum, s32 pdonly)
{
	union soundnumhack num;
	union soundnumhack raw;
	s16 *rows;
	s32 ours;
	s32 row;

	if (!soundnum || soundnum == -1) {
		return soundnum;
	}

	if (modloaderStageIsRemake(g_Vars.stagenum)) {
		if (weaponnum <= WEAPON_UNARMED || WEAPON_IS_GE(weaponnum)) {
			return soundnum;
		}
	} else if (pdonly || !WEAPON_IS_GE(weaponnum)) {
		return soundnum;
	}

	num.packed = soundnum;
	raw.packed = num.hasconfig ? g_AudioRussMappings[num.confignum].soundnum : num.packed;
	raw.hasconfig = false;

	if (sndIsMp3(raw.packed) || !sfxRemappable(raw.id) || !sfxBank()) {
		return soundnum;
	}

	if (modloaderStageIsRemake(g_Vars.stagenum)) {
		ours = sfxPdCopy(raw.id);
		rows = g_SfxPdGunRow;
	} else {
		if (raw.id == 62) {
			return 0;
		}

		// a stage of Perfect Dark's: GoldenEye's own gun's sample
		ours = sfxGetIn(sfxBankGoldenEye(), raw.id);
		rows = g_SfxGunRow;
	}

	if (ours <= 0 || !num.hasconfig) {
		return ours > 0 ? ours : soundnum;
	}

	// kept under the config, which is what a guard's shot is heard by
	if (num.confignum >= SND_RUSS_CAPACITY || rows[num.confignum] < 0) {
		return ours;
	}

	if (rows[num.confignum] == 0) {
		row = sndAppendRussMapping(ours, g_AudioRussMappings[num.confignum].audioconfig_index);
		rows[num.confignum] = row >= 0 ? row + 1 : -1;
	}

	return rows[num.confignum] > 0 ? 0x8000 | (rows[num.confignum] - 1) : ours;
}

s32 geSfxGunSound(s32 weaponnum, s32 soundnum)
{
	return sfxGunSound(weaponnum, soundnum, 0);
}

/**
 * What a Perfect Dark gun's bullet sets off where it lands - a surface's
 * impact, a ricochet, a body, a shield - and its casing landing are the gun's
 * too (the user, after F3 20260928-030000): on a converted level they are
 * Perfect Dark's own samples for one of its guns, as its shot is. A
 * GoldenEye gun's stay GoldenEye's on a converted level and Perfect Dark's on
 * a stage of Perfect Dark's, as they were: only the one case is taken.
 *
 * bondgun.c's hit functions pick their sounds deep in two dozen branches, so
 * they name the gun for their length (geSfxGunHitBegin() / End()) and
 * sndStart() asks geSfxGunHit() for every sound it starts meanwhile.
 */
s32 geSfxGunHitSound(s32 weaponnum, s32 soundnum)
{
	return sfxGunSound(weaponnum, soundnum, 1);
}

static s32 g_SfxHitWeapon = -1;

void geSfxGunHitBegin(s32 weaponnum)
{
	g_SfxHitWeapon = weaponnum;
}

void geSfxGunHitEnd(void)
{
	g_SfxHitWeapon = -1;
}

s32 geSfxGunHit(s32 soundnum)
{
	return g_SfxHitWeapon >= 0 ? geSfxGunHitSound(g_SfxHitWeapon, soundnum) : soundnum;
}
