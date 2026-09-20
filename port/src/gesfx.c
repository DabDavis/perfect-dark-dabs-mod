/**
 * GoldenEye's own sound effects, out of the player's ROM.
 *
 * GoldenEye's sfx.ctl is an ordinary ALBankFile of one bank and one instrument
 * with 261 sounds, and sndPlaySfx() indexes that instrument's soundArray with
 * the SFX_ID itself - from 0, where Perfect Dark's ids count from 1. The
 * conversion copies the bank and its wave table out of the ROM as menu/sfxctl
 * and menu/sfxtbl.
 *
 * A sound is appended after the game's own the way a borrowed mod's is
 * (modborrow.c): snd.c's loaders add the stock bank's start to every offset
 * they read, so everything the ALSound points at is rebased onto that start,
 * once each, since sounds share envelopes, key maps and waves.
 *
 * **The chain.** Both games' players read a key map's velocityMin and the top
 * of its keyMin as the *next* sound to play after this one, by number - ten
 * bits of GoldenEye's numbering, which here would name a sound of Perfect
 * Dark's. An appended id does not fit in ten bits, so a chain cannot be
 * rewritten and is cut instead; the sounds that have one say so in the log.
 * The gun barrel's shot has none.
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
#include "preprocess.h"
#include "system.h"
#include "lib/snd.h"

#define GESFX_MAX 512

static u8 *g_SfxCtl;
static u8 *g_SfxTbl;
static ALInstrument *g_SfxInst;
static s32 g_SfxSearchedDirs = -1;

// GoldenEye's id -> ours; 0 not looked at yet, -1 none
static s16 g_SfxMap[GESFX_MAX];

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

	if (id >= g_SfxInst->soundCount) {
		g_SfxMap[id] = -1;
		return 0;
	}

	off = (uintptr_t)g_SfxInst->soundArray[id];
	sound = (ALSound *)(g_SfxCtl + off);

	if (sfxRebaseOnce(off)) {
		if (sound->envelope) {
			sound->envelope = (ALEnvelope *)((uintptr_t)sound->envelope + GESFX_CTL_DELTA());
		}

		if (sound->keyMap) {
			const uintptr_t koff = (uintptr_t)sound->keyMap;
			ALKeyMap *keymap = (ALKeyMap *)(g_SfxCtl + koff);

			sound->keyMap = (ALKeyMap *)(koff + GESFX_CTL_DELTA());

			if (sfxRebaseOnce(koff) && (keymap->velocityMin || (keymap->keyMin & 0xc0))) {
				sysLogPrintf(LOG_NOTE, "gesfx: sound %d chains to %d, which is cut", id,
						keymap->velocityMin + (keymap->keyMin & 0xc0) * 4);
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
	const s32 ours = geSfxGet(id);

	if (ours <= 0) {
		return 0;
	}

	return sndStart(var80095200, ours, NULL, volume, -1, -1, -1, -1) != NULL;
}
