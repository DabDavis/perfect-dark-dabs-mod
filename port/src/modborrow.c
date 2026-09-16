/**
 * Borrowing an installed mod's assets beside the mod that is loaded.
 *
 * The Stage Loader showed the shape of it: a mod mounted with fsAddMapsDir()
 * never overlays the file search, and anything reached through a file slot
 * pinned to it is that mod's. A map is files and nothing else. A gun is not:
 * its definition is .data in the mod's ROM (moddata.c reads it out of
 * segs/data), its model names textures by the mod's numbers, its fire and
 * reload scripts play the mod's animations and sounds by the mod's numbers -
 * and GoldenEye X rewrote 86 of the animations in place and replaced the whole
 * sound bank, so every one of those numbers means something else to the ROM.
 *
 * So a borrowed definition is read with its numbers moved:
 *
 * - files the mod ships are pinned to its mount (the file's own name, the
 *   mod's copy), and one it does not ship is the stock file it kept;
 * - a model in such a file loads its textures from the mod whatever stage is
 *   running (modSetTextureSourceMod(), keyed in the texture pool by mod);
 * - an animation the mod changed is appended after the game's own
 *   (animAppendExternal()), one it left alone keeps its number;
 * - a sound is appended out of the mod's own bank (sndAppendSound()), its
 *   ALSound rebased so the game's loaders, which add the stock bank's start to
 *   every offset, land in the mod's; a sound played through a config takes the
 *   mod's config row with it (sndAppendRussMapping(), sndAppendAudioConfig()).
 *
 * The first thing borrowed is GoldenEye's guns (WEAPON_GE_FIRST) out of
 * GoldenEye X: "ge-x is a good reference honestly, they implemented the hand
 * grips, everything correctly, even the reload animations". The mod is found
 * by its guns rather than its name - its weapon table holds GoldenEye's gun
 * models in the slots GoldenEye X put them - and Mod.BorrowGoldenEyeGuns
 * names one outright, or "none".
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <PR/ultratypes.h>
#include <PR/libaudio.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "system.h"
#include "config.h"
#include "fs.h"
#include "romdata.h"
#include "mod.h"
#include "modborrow.h"
#include "modloader.h"
#include "preprocess.h"
#include "lib/anim.h"
#include "lib/snd.h"
#include "lib/rng.h"
#include "geguns.h"
#include "game/mplayer/mplayer.h"
#include "game/lang.h"
#include <zlib.h>

#define BORROW_NAME_LEN 128

// Where GoldenEye X keeps each GoldenEye gun: its weapon slot and the model
// file that slot draws, which is also how the mod is recognised. Both knives
// are its one knife, whose secondary function throws.
static const struct {
	u8 slot;
	const char *model;
} geSlots[NUM_GE_WEAPONS] = {
	[WEAPON_GE_PP7             - WEAPON_GE_FIRST] = {  3, "GwppkZ" },
	[WEAPON_GE_PP7SILENCED     - WEAPON_GE_FIRST] = {  4, "GwppkZ" },
	[WEAPON_GE_DD44            - WEAPON_GE_FIRST] = {  5, "Gtt33Z" },
	[WEAPON_GE_KLOBB           - WEAPON_GE_FIRST] = {  6, "GskorpionZ" },
	[WEAPON_GE_KF7SOVIET       - WEAPON_GE_FIRST] = {  7, "Gak47Z" },
	[WEAPON_GE_ZMG             - WEAPON_GE_FIRST] = {  8, "GuziZ" },
	[WEAPON_GE_D5K             - WEAPON_GE_FIRST] = {  9, "Gmp5kZ" },
	[WEAPON_GE_D5KSILENCED     - WEAPON_GE_FIRST] = { 10, "Gcmp150Z" },
	[WEAPON_GE_PHANTOM         - WEAPON_GE_FIRST] = { 11, "GcycloneZ" },
	[WEAPON_GE_AR33            - WEAPON_GE_FIRST] = { 12, "Gm16Z" },
	[WEAPON_GE_RCP90           - WEAPON_GE_FIRST] = { 13, "Gfnp90Z" },
	[WEAPON_GE_SHOTGUN         - WEAPON_GE_FIRST] = { 14, "GshotgunZ" },
	[WEAPON_GE_AUTOSHOTGUN     - WEAPON_GE_FIRST] = { 15, "Grcp120Z" },
	[WEAPON_GE_SNIPERRIFLE     - WEAPON_GE_FIRST] = { 16, "GsniperrifleZ" },
	[WEAPON_GE_COUGARMAGNUM    - WEAPON_GE_FIRST] = { 17, "GmaianpistolZ" },
	[WEAPON_GE_GOLDENGUN       - WEAPON_GE_FIRST] = { 18, "Gleegun1Z" },
	[WEAPON_GE_MOONRAKER       - WEAPON_GE_FIRST] = { 21, "GdysuperdragonZ" },
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = { 23, "GdydevastatorZ" },
	[WEAPON_GE_ROCKETLAUNCHER  - WEAPON_GE_FIRST] = { 24, "GdyrocketZ" },
	[WEAPON_GE_HUNTINGKNIFE    - WEAPON_GE_FIRST] = {  2, "GknifeZ" },
	[WEAPON_GE_THROWINGKNIFE   - WEAPON_GE_FIRST] = {  2, "GknifeZ" },
	[WEAPON_GE_GRENADE         - WEAPON_GE_FIRST] = { 26, "GgrenadeZ" },
	[WEAPON_GE_TIMEDMINE       - WEAPON_GE_FIRST] = { 27, "GtimedmineZ" },
	[WEAPON_GE_PROXIMITYMINE   - WEAPON_GE_FIRST] = { 28, "GproximitymineZ" },
	[WEAPON_GE_REMOTEMINE      - WEAPON_GE_FIRST] = { 29, "GremotemineZ" },
};

// Recognised when this many of the slots hold the model GoldenEye X put there
#define GE_MIN_MATCHES 20

// The sound tables at the stock data segment's addresses, which GoldenEye X
// keeps (pd.ntsc-final.datasym); the importer's spec does not carry them
#define N64_AUDIORUSSMAPPINGS 0x8005dde4
#define N64_AUDIOCONFIGS      0x8005e4d8
#define N64_AUDIOCONFIG_SIZE  0x20

static char borrowSetting[BORROW_NAME_LEN] = "auto";

static struct {
	s32 found;                   // looked for since boot
	char name[BORROW_NAME_LEN];
	char dir[FS_MAXPATH + 1];
	s32 moddir;                  // mount index, -1 before mounting
	s32 mountedhere;             // mounted for its guns, not for its maps
	struct moddataspec spec;

	// its animations segment
	u8 *anims;
	u32 animslen;
	u32 animtable;               // offset of the table's count word
	u32 numanims;
	s32 *animmap;                // mod number -> ours; 0 not looked at yet

	// its sound bank, converted to the host layout as the game's own is
	u8 *ctl;
	u32 ctllen;
	u8 *tbl;
	ALInstrument *inst;
	s32 soundmap[SND_MAX_SOUNDS];     // mod id -> ours; 0 not looked at yet, -1 none
	s32 configmap[SND_RUSS_CAPACITY]; // mod config -> ours + 1
	s32 audioconfigmap[256];          // mod audio config row -> ours + 1
	uintptr_t *rebased;
	s32 numrebased;
	s32 maxrebased;

	s32 animsappended;
	s32 soundsappended;

	struct moddataborrow *reader; // while the guns are read
} src = { .moddir = -1 };

s32 modBorrowIsGunsOnlyMount(s32 moddir)
{
	return src.mountedhere && moddir == src.moddir;
}

const char *modBorrowGoldenEyeName(void)
{
	return src.found > 0 ? src.name : NULL;
}

/* ---- finding the mod ---------------------------------------------------- */

static u16 borrowBE16(const u8 *p)
{
	return (u16)((p[0] << 8) | p[1]);
}

static u32 borrowBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/** How many of GoldenEye X's gun slots in dir's weapon table hold its model. */
static s32 borrowScoreGoldenEye(const char *dir, struct moddataspec *spec)
{
	struct moddataborrow *b;
	s32 score = 0;

	modDataSpecInit(spec);

	if (!modConfigReadDataSegment(dir, spec) || !spec->weapons || spec->numweapons <= 29) {
		return 0;
	}

	b = modDataBorrowOpen(spec, dir, -1, NULL, NULL, NULL);

	if (!b) {
		return 0;
	}

	for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
		const u32 def = modDataBorrowRd32(b, spec->weapons + geSlots[i].slot * 4);
		u8 hi[2];
		const char *name;

		if (!def || !modDataBorrowRead(b, def, hi, 2)) {
			continue;
		}

		name = modDataBorrowFileName(b, borrowBE16(hi));

		if (name && !strcmp(name, geSlots[i].model)) {
			score++;
		}
	}

	modDataBorrowClose(b);

	return score;
}

static void borrowFind(void)
{
	s32 best = 0;

	if (src.found) {
		return;
	}

	src.found = -1;

	if (!strcasecmp(borrowSetting, "none") || !borrowSetting[0]) {
		return;
	}

	for (s32 i = 0; i < modListGetCount(); i++) {
		const char *name = modListGetName(i);
		const char *dir = modListGetPath(i);
		struct moddataspec spec;
		s32 score;

		if (strcasecmp(borrowSetting, "auto") && strcasecmp(borrowSetting, name)) {
			continue;
		}

		score = borrowScoreGoldenEye(dir, &spec);

		if (score >= GE_MIN_MATCHES && score > best) {
			best = score;
			src.found = 1;
			src.spec = spec;
			snprintf(src.name, sizeof(src.name), "%s", name);
			snprintf(src.dir, sizeof(src.dir), "%s", dir);
		}
	}

	if (src.found > 0) {
		sysLogPrintf(LOG_NOTE, "modborrow: GoldenEye's guns come from `%s` (%d of %d slots)", src.name, best, NUM_GE_WEAPONS);
	} else {
		sysLogPrintf(LOG_NOTE, "modborrow: no installed mod has GoldenEye's guns");
	}
}

/** Whether dir is the mod loaded over the game, whose numbers are live already. */
static s32 borrowIsLoaded(void)
{
	const char *loaded = fsGetModDir();

	return loaded && !strcmp(loaded, src.dir);
}

void modBorrowMount(void)
{
	borrowFind();

	src.moddir = -1;
	src.mountedhere = 0;

	if (src.found <= 0 || borrowIsLoaded()) {
		return;
	}

	for (s32 i = 0; i < fsGetNumModDirs(); i++) {
		const char *dir = fsGetModDirAt(i);

		if (dir && !strcmp(dir, src.dir)) {
			src.moddir = i;
			return;
		}
	}

	src.moddir = fsAddMapsDir(src.dir);

	if (src.moddir >= 0) {
		src.mountedhere = 1;
		sysLogPrintf(LOG_NOTE, "modborrow: `%s` mounted for its guns", src.name);
	}
}

/* ---- animations --------------------------------------------------------- */

static void borrowLoadAnims(void)
{
	char path[FS_MAXPATH + 1];

	if (src.anims) {
		return;
	}

	snprintf(path, sizeof(path), "%s/segs/animations", src.dir);

	if (fsFileSize(path) <= 0 || !(src.anims = fsFileLoad(path, &src.animslen)) || src.animslen < 0x38a0) {
		return;
	}

	// The table is at the segment's end, as the ROM's is (preprocessAnimations()),
	// and big-endian here since this copy is never preprocessed
	src.animtable = src.animslen - 0x38a0;
	src.numanims = borrowBE32(src.anims + src.animtable);

	if (src.animtable + 4 + src.numanims * 12 > src.animslen || src.numanims > 0x7fff) {
		src.numanims = 0;
		return;
	}

	src.animmap = sysMemZeroAlloc(src.numanims * sizeof(s32));
}

static s32 borrowRemapAnim(void *ctx, s32 num)
{
	struct animtableentry e;
	const u8 *row;
	u32 len;
	u8 *copy;
	s32 ours;

	borrowLoadAnims();

	if (!src.animmap || num < 0 || (u32)num >= src.numanims) {
		return num;
	}

	if (src.animmap[num]) {
		return src.animmap[num];
	}

	row = src.anims + src.animtable + 4 + num * 12;
	e.numframes = borrowBE16(row);
	e.bytesperframe = borrowBE16(row + 2);
	e.data = borrowBE32(row + 4);
	e.headerlen = borrowBE16(row + 8);
	e.framelen = row[10];
	e.flags = row[11];

	len = e.headerlen + (u32)e.numframes * e.bytesperframe;

	if (e.data > src.animtable || len > src.animtable - e.data) {
		return src.animmap[num] = num;
	}

	if (animIsSame(num, &e, src.anims + e.data)) {
		return src.animmap[num] = num;
	}

	copy = sysMemAlloc(len + 64);

	if (!copy) {
		return num;
	}

	memcpy(copy, src.anims + e.data, len);
	memset(copy + len, 0, 64);

	ours = animAppendExternal(&e, copy);

	if (ours < 0) {
		sysLogPrintf(LOG_WARNING, "modborrow: no room for `%s`'s animation %d", src.name, num);
		sysMemFree(copy);
		return src.animmap[num] = num;
	}

	src.animsappended++;

	return src.animmap[num] = ours;
}

/* ---- sounds ------------------------------------------------------------- */

static void borrowLoadSounds(void)
{
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *raw;

	if (src.ctl) {
		return;
	}

	snprintf(path, sizeof(path), "%s/segs/sfxctl", src.dir);

	if (fsFileSize(path) <= 0 || !(raw = fsFileLoad(path, &len))) {
		return;
	}

	src.ctl = preprocessALBankFile(raw, len, &src.ctllen);
	sysMemFree(raw);

	snprintf(path, sizeof(path), "%s/segs/sfxtbl", src.dir);
	src.tbl = fsFileSize(path) > 0 ? fsFileLoad(path, &len) : NULL;

	if (!src.ctl || !src.tbl) {
		return;
	}

	{
		ALBankFile *file = (ALBankFile *)src.ctl;
		ALBank *bank = (ALBank *)(src.ctl + (uintptr_t)file->bankArray[0]);

		src.inst = (ALInstrument *)(src.ctl + (uintptr_t)bank->instArray[0]);
	}
}

/**
 * Makes an offset in the mod's converted bank an offset from the game's own
 * bank start, which is how every loader in snd.c reads one. Each object is
 * rebased once: sounds share envelopes and key maps.
 */
static s32 borrowRebaseOnce(uintptr_t off)
{
	for (s32 i = 0; i < src.numrebased; i++) {
		if (src.rebased[i] == off) {
			return 0;
		}
	}

	if (src.numrebased == src.maxrebased) {
		const s32 max = src.maxrebased ? src.maxrebased * 2 : 256;
		uintptr_t *grown = sysMemRealloc(src.rebased, max * sizeof(uintptr_t));

		if (!grown) {
			return 0;
		}

		src.rebased = grown;
		src.maxrebased = max;
	}

	src.rebased[src.numrebased++] = off;

	return 1;
}

#define BORROW_CTL_DELTA() ((uintptr_t)src.ctl - sndGetCtlStart())

static s32 borrowAppendSound(s32 id)
{
	ALSound *sound;
	uintptr_t off;
	s32 ours;

	borrowLoadSounds();

	if (!src.inst || id <= 0 || id > src.inst->soundCount || id >= SND_MAX_SOUNDS) {
		return 0;
	}

	if (src.soundmap[id]) {
		return src.soundmap[id] > 0 ? src.soundmap[id] : 0;
	}

	off = (uintptr_t)src.inst->soundArray[id - 1];
	sound = (ALSound *)(src.ctl + off);

	if (borrowRebaseOnce(off)) {
		if (sound->envelope) {
			sound->envelope = (ALEnvelope *)((uintptr_t)sound->envelope + BORROW_CTL_DELTA());
		}

		if (sound->keyMap) {
			sound->keyMap = (ALKeyMap *)((uintptr_t)sound->keyMap + BORROW_CTL_DELTA());
		}

		if (sound->wavetable) {
			const uintptr_t woff = (uintptr_t)sound->wavetable;
			ALWaveTable *wave = (ALWaveTable *)(src.ctl + woff);

			sound->wavetable = (ALWaveTable *)(woff + BORROW_CTL_DELTA());

			if (borrowRebaseOnce(woff)) {
				wave->base = (u8 *)((uintptr_t)wave->base + (uintptr_t)src.tbl - sndGetTblStart());

				if (wave->type == AL_ADPCM_WAVE) {
					if (wave->waveInfo.adpcmWave.book) {
						wave->waveInfo.adpcmWave.book = (ALADPCMBook *)((uintptr_t)wave->waveInfo.adpcmWave.book + BORROW_CTL_DELTA());
					}

					if (wave->waveInfo.adpcmWave.loop) {
						wave->waveInfo.adpcmWave.loop = (ALADPCMloop *)((uintptr_t)wave->waveInfo.adpcmWave.loop + BORROW_CTL_DELTA());
					}
				}
			}
		}
	}

	ours = sndAppendSound(off + BORROW_CTL_DELTA());

	if (ours <= 0) {
		// no bank loaded (--no-sound) or no ids left: the gun is silent there
		sysLogPrintf(LOG_WARNING, "modborrow: no room for `%s`'s sound %d", src.name, id);
		src.soundmap[id] = -1;
		return 0;
	}

	src.soundsappended++;

	return src.soundmap[id] = ours;
}

/** The mod's audio config row, as one of ours: the same row if it matches. */
static s32 borrowAudioConfig(s32 index)
{
	struct audioconfig c;
	u8 raw[N64_AUDIOCONFIG_SIZE];
	s32 ours;

	if (index < 0 || index >= ARRAYCOUNT(src.audioconfigmap)) {
		return index;
	}

	if (src.audioconfigmap[index]) {
		return src.audioconfigmap[index] - 1;
	}

	if (!src.reader || !modDataBorrowRead(src.reader, N64_AUDIOCONFIGS + index * N64_AUDIOCONFIG_SIZE, raw, sizeof(raw))) {
		return index;
	}

	for (s32 i = 0; i < 4; i++) {
		const u32 v = borrowBE32(raw + i * 4);
		memcpy((f32 *)&c + i, &v, 4);
	}

	c.volpercentage = (s32)borrowBE32(raw + 16);
	c.pan = (s32)borrowBE32(raw + 20);
	c.volchangespeed = (s32)borrowBE32(raw + 24);
	c.flags = borrowBE32(raw + 28);

	if (index < SND_NUM_ROM_CONFIGS && !memcmp(&c, &g_AudioConfigs[index], sizeof(c))) {
		ours = index;
	} else {
		ours = sndAppendAudioConfig(&c);

		if (ours < 0) {
			ours = index;
		}
	}

	src.audioconfigmap[index] = ours + 1;

	return ours;
}

static s32 borrowRemapSound(void *ctx, s32 num)
{
	union soundnumhack in;
	union soundnumhack out;

	in.packed = (s16)num;

	if (!num) {
		return 0;
	}

	if (in.hasconfig) {
		struct moddataborrow *b = src.reader;
		const s32 config = in.confignum;
		u8 raw[4];
		union soundnumhack mapped;
		s32 row;

		if (config >= SND_RUSS_CAPACITY) {
			return 0;
		}

		if (src.configmap[config]) {
			return 0x8000 | (src.configmap[config] - 1);
		}

		if (!b || !modDataBorrowRead(b, N64_AUDIORUSSMAPPINGS + config * 4, raw, 4)) {
			return 0;
		}

		mapped.packed = (s16)borrowBE16(raw);
		row = borrowAudioConfig(borrowBE16(raw + 2));

		if (mapped.id) {
			const s32 id = borrowAppendSound(mapped.id);

			if (!id) {
				return 0;
			}

			mapped.id = id;
		}

		row = sndAppendRussMapping(mapped.packed, (u16)row);

		if (row < 0) {
			return 0;
		}

		src.configmap[config] = row + 1;

		return 0x8000 | row;
	}

	out = in;
	out.id = borrowAppendSound(in.id);

	return out.id ? (u16)out.packed : 0;
}

/* ---- the music ---------------------------------------------------------- */

static s32 borrowLangString(struct moddataborrow *b, u16 textid, char *out, u32 outlen);

#define N64_MPTRACK_SIZE 6

extern struct mptrack g_MpTracks[];

static struct {
	s32 loaded;
	ALBank *bank;
	u8 *tbl;
	u8 *sequences;
	u32 seqlen;
	s32 seqmap[256];                     // mod sequence -> ours + 1
	char names[MP_MAX_TRACKS][32];
	s32 base;                            // where its tracks start in g_MpTracks
} music = { .base = -1 };

static s32 borrowLoadMusic(void)
{
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *raw;
	u32 ctllen = 0;
	u8 *ctl;

	if (music.loaded) {
		return music.loaded > 0;
	}

	music.loaded = -1;

	snprintf(path, sizeof(path), "%s/segs/seqctl", src.dir);

	if (fsFileSize(path) <= 0 || !(raw = fsFileLoad(path, &len))) {
		return 0;
	}

	// The bank as sndInit() makes the game's: converted to the host layout,
	// then its offsets made pointers into itself and into the sample table
	ctl = preprocessALBankFile(raw, len, &ctllen);
	sysMemFree(raw);

	snprintf(path, sizeof(path), "%s/segs/seqtbl", src.dir);
	music.tbl = fsFileSize(path) > 0 ? fsFileLoad(path, &len) : NULL;

	snprintf(path, sizeof(path), "%s/segs/sequences", src.dir);
	music.sequences = fsFileSize(path) > 0 ? fsFileLoad(path, &music.seqlen) : NULL;

	if (!ctl || !music.tbl || !music.sequences || music.seqlen < 6) {
		return 0;
	}

	alBnkfNew((ALBankFile *)ctl, music.tbl);
	music.bank = ((ALBankFile *)ctl)->bankArray[0];
	music.loaded = music.bank ? 1 : -1;

	return music.loaded > 0;
}

/** Our number for the mod's sequence n, appended once; -1 when it cannot be. */
static s32 borrowSequence(s32 n)
{
	const u8 *seq = music.sequences;
	const u32 count = borrowBE16(seq);
	const u8 *e;
	u32 addr;
	s32 ours;

	if (n < 0 || n >= (s32)ARRAYCOUNT(music.seqmap) || (u32)n >= count || 4 + (u32)n * 8 + 8 > music.seqlen) {
		return -1;
	}

	if (music.seqmap[n]) {
		return music.seqmap[n] - 1;
	}

	// {u16 count, then u32 offset, u16 inflated, u16 zipped} a sequence (preprocessSequences())
	e = seq + 4 + n * 8;
	addr = borrowBE32(e);

	if (addr >= music.seqlen || borrowBE16(e + 6) > music.seqlen - addr) {
		return -1;
	}

	ours = seqAppend(seq + addr, borrowBE16(e + 4), borrowBE16(e + 6), music.bank);
	music.seqmap[n] = ours + 1;

	return ours;
}

/**
 * GoldenEye X's Combat Simulator tracks, after the list's own: its GoldenEye
 * sequences on its GoldenEye instruments, under its own names and unlocked,
 * the way its list has them. After sndInit(), and again when a live swap has
 * put the lists back.
 */
static void borrowMusic(struct moddataborrow *b)
{
	s32 added = 0;

	music.base = -1;

	if (!src.spec.mptracks || src.spec.nummptracks <= 0 || !borrowLoadMusic()) {
		return;
	}

	music.base = mpGetNumTracks();

	for (s32 i = 0; i < src.spec.nummptracks && mpGetNumTracks() < MP_MAX_TRACKS; i++) {
		u8 raw[N64_MPTRACK_SIZE];
		const s32 at = mpGetNumTracks();
		s32 ours;
		u16 w;

		if (!modDataBorrowRead(b, src.spec.mptracks + i * N64_MPTRACK_SIZE, raw, sizeof(raw))) {
			break;
		}

		w = borrowBE16(raw);
		ours = borrowSequence(w >> 9);

		if (ours < 0) {
			continue;
		}

		if (!borrowLangString(b, borrowBE16(raw + 2), music.names[at], sizeof(music.names[at]))) {
			snprintf(music.names[at], sizeof(music.names[at]), "GoldenEye X %d", i + 1);
		}

		g_MpTracks[at].musicnum = (u16)ours;
		g_MpTracks[at].duration = w & 0x1ff;
		g_MpTracks[at].name = (s16)langAddPortText(music.names[at]);
		g_MpTracks[at].unlockstage = -1;
		mpSetNumTracks(at + 1);
		added++;
	}

	if (added) {
		sysLogPrintf(LOG_NOTE, "modborrow: %d music tracks from `%s` in the Combat Simulator's list", added, src.name);
	}
}

/**
 * The music a match on one of the borrowed mod's own arenas plays, when the
 * player left the choice to the game (Random, not multiple tunes): the mod's
 * track named for the map - GoldenEye X names its tunes for GoldenEye's
 * levels, and its arenas are those levels - or one of its tracks at random for
 * a map GoldenEye had no level for (Temple, Complex...). A match on Random
 * plays one tune throughout (music switching is only on with multiple tunes),
 * but it asks for it more than once as it starts, so an ask inside the pick's
 * own length gets the same pick. -1 for a stage that is not the mod's, or for a
 * player's own choice, which stageGetPrimaryTrack() answers from the list.
 */
s32 modBorrowStageTrack(s32 stagenum)
{
	static s32 laststage = -1;
	static s32 lasttrack = -1;
	static s32 lastpicked = 0;
	const char *map;
	const s32 first = music.base;
	const s32 end = mpGetNumTracks();
	s32 pick = -1;
	size_t bestlen = 0;

	if (first < 0 || end <= first || src.moddir < 0 || modloaderGetStageModDirIndex(stagenum) != src.moddir
			|| mpGetUsingMultipleTunes() || mpGetCurrentTrackSlotNum() >= 0
			|| !(map = modloaderGetStageMapName(stagenum))) {
		return -1;
	}

	// Asked again while the pick has not run its length - a match's start asks
	// more than once - it is the same pick. g_MusicAge60 cannot say this: the
	// switch zeroes it before it asks.
	if (stagenum == laststage && lasttrack >= 0 && lasttrack < end && g_Vars.lvframe60 >= lastpicked
			&& g_Vars.lvframe60 - lastpicked < g_MpTracks[lasttrack].duration * TICKS(60)) {
		g_MusicLife60 = g_MpTracks[lasttrack].duration * TICKS(60);
		return g_MpTracks[lasttrack].musicnum;
	}

	if (stagenum != laststage || g_Vars.lvframe60 < lastpicked) {
		// the longest track name the map's starts with: "Facility BZ" plays
		// Facility, and never "Facility X", the level's alarm version
		for (s32 i = first; i < end; i++) {
			const size_t len = strlen(music.names[i]);

			if (len > bestlen && strncasecmp(map, music.names[i], len) == 0
					&& (map[len] == '\0' || map[len] == ' ')) {
				pick = i;
				bestlen = len;
			}
		}
	}

	if (pick < 0) {
		s32 tries = 0;

		do {
			pick = first + (s32)(rngRandom() % (u32)(end - first));
		} while (pick == lasttrack && end - first > 1 && ++tries < 8);
	}

	laststage = stagenum;
	lasttrack = pick;
	lastpicked = g_Vars.lvframe60;
	g_MusicLife60 = g_MpTracks[pick].duration * TICKS(60);

	sysLogPrintf(LOG_NOTE, "modborrow: %s plays %s", map, music.names[pick]);

	return g_MpTracks[pick].musicnum;
}

/* ---- the guns ----------------------------------------------------------- */

void modBorrowCommit(void)
{
	struct moddataborrow *b;
	s32 borrowed = 0;

	for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
		gegunsBorrow(i, NULL, 0, 0);
	}

	if (src.found <= 0 || src.moddir < 0) {
		return;
	}

	b = modDataBorrowOpen(&src.spec, src.dir, src.moddir, borrowRemapAnim, borrowRemapSound, NULL);

	if (!b) {
		return;
	}

	// the sound remap reads the mod's own tables through it
	src.reader = b;

	for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
		const u32 def = modDataBorrowRd32(b, src.spec.weapons + geSlots[i].slot * 4);
		struct weapon *w;
		u8 hi[2];
		const char *name;
		u16 pickupfile = 0;
		u16 pickupscale = 0;

		if (!def || !modDataBorrowRead(b, def, hi, 2)) {
			continue;
		}

		name = modDataBorrowFileName(b, borrowBE16(hi));

		if (!name || strcmp(name, geSlots[i].model)) {
			continue;
		}

		w = modDataBorrowWeapon(b, &src.spec, geSlots[i].slot);

		if (!w || !w->hi_model) {
			continue;
		}

		// The pickup is the model the mod's Combat Simulator list gives the slot
		for (s32 k = 0; k < src.spec.nummpweapons; k++) {
			u8 row[10];

			if (modDataBorrowRead(b, src.spec.mpweapons + k * 10, row, sizeof(row)) && row[0] == geSlots[i].slot) {
				modDataBorrowModelState(b, &src.spec, (s16)borrowBE16(row + 6), &pickupfile, &pickupscale);
				break;
			}
		}

		gegunsBorrow(i, w, pickupfile, pickupscale);
		borrowed++;
	}

	borrowMusic(b);

	// What was converted is kept: the definitions point into it
	src.reader = NULL;

	sysLogPrintf(LOG_NOTE, "modborrow: %d of GoldenEye's guns from `%s`, with %d animations and %d sounds of its own",
			borrowed, src.name, src.animsappended, src.soundsappended);
}

/* ---- the characters ----------------------------------------------------- */

// The data segment's g_LangFiles (pd.ntsc-final.datasym), a u16 file id a bank
#define N64_LANGFILES       0x80084124
#define N64_MPBODY_SIZE     8
#define N64_MPHEAD_SIZE     4
#define N64_HEADORBODY_SIZE 0x14
#define BORROW_MAXROWS      256
#define BORROW_NAMELEN      32

static s32 charBase = -1;
static s32 charRows;
static char charNames[BORROW_MAXROWS][BORROW_NAMELEN];

const char *modBorrowBodyName(s32 bodynum)
{
	const s32 i = bodynum - charBase;

	return charBase >= 0 && i >= 0 && i < charRows && charNames[i][0] ? charNames[i] : NULL;
}

/** Whether the mod ships this file of its own, rather than keeping the stock one. */
static s32 borrowShips(const char *name)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/files/%s", src.dir, name);

	return fsFileSize(path) > 0;
}

/**
 * A text id's string out of the mod's own language file (bank in the top bits,
 * index in the low nine; the file is a table of u32 offsets into itself), or
 * out of the stock file of that name when the mod kept it. The same reading
 * the importer does for arena names (modimport.c, langString()).
 */
static s32 borrowLangString(struct moddataborrow *b, u16 textid, char *out, u32 outlen)
{
	const u32 bank = textid >> 9;
	const u32 index = textid & 0x1ff;
	char path[FS_MAXPATH + 1];
	u8 raw[2];
	const char *name;
	u8 *file = NULL;
	u8 *data = NULL;
	u32 len = 0;
	s32 ok = 0;

	out[0] = '\0';

	if (!textid || bank > 68 || !modDataBorrowRead(b, N64_LANGFILES + bank * 2, raw, 2)) {
		return 0;
	}

	name = modDataBorrowFileName(b, borrowBE16(raw));

	if (!name) {
		return 0;
	}

	snprintf(path, sizeof(path), "%s/files/%s", src.dir, name);

	if (fsFileSize(path) > 0) {
		file = fsFileLoad(path, &len);
	} else {
		const s32 stock = romdataFileGetNumForName(name);
		const u8 *rom = stock > 0 ? romdataFileGetData(stock) : NULL;

		len = stock > 0 ? (u32)romdataFileGetSize(stock) : 0;

		if (rom && len) {
			file = sysMemAlloc(len);

			if (file) {
				memcpy(file, rom, len);
			}
		}
	}

	if (!file || len < 5) {
		sysMemFree(file);
		return 0;
	}

	// 0x1173, a 24-bit length, raw deflate
	if (file[0] == 0x11 && file[1] == 0x73) {
		const u32 declared = ((u32)file[2] << 16) | ((u32)file[3] << 8) | file[4];
		z_stream zs;

		memset(&zs, 0, sizeof(zs));
		data = declared ? sysMemAlloc(declared) : NULL;

		if (data && inflateInit2(&zs, -MAX_WBITS) == Z_OK) {
			zs.next_in = file + 5;
			zs.avail_in = len - 5;
			zs.next_out = data;
			zs.avail_out = declared;
			inflate(&zs, Z_FINISH);
			len = declared - zs.avail_out;
			inflateEnd(&zs);
		} else {
			sysMemFree(data);
			data = NULL;
		}

		sysMemFree(file);
	} else {
		data = file;
	}

	if (data && (index + 1) * 4 <= len && index * 4 < borrowBE32(data)) {
		const u32 at = borrowBE32(data + index * 4);
		u32 n = 0;

		for (u32 i = at; at && i < len && data[i] && n + 1 < outlen; i++) {
			if (data[i] >= ' ' && data[i] <= '~') {
				out[n++] = (char)data[i];
			}
		}

		while (n && out[n - 1] == ' ') {
			n--;
		}

		out[n] = '\0';
		ok = n > 0;
	}

	sysMemFree(data);

	return ok;
}

/** One of the mod's g_HeadsAndBodies rows as the port's, its files pinned. */
static s32 borrowHeadOrBody(struct moddataborrow *b, s32 index, struct headorbody *out)
{
	u8 raw[N64_HEADORBODY_SIZE];
	u32 v;
	f32 scale;
	f32 animscale;
	u16 bits;
	const char *name;

	if (index < 0 || index >= src.spec.numheadsandbodies
			|| !modDataBorrowRead(b, src.spec.headsandbodies + index * N64_HEADORBODY_SIZE, raw, sizeof(raw))) {
		return 0;
	}

	bits = borrowBE16(raw);
	v = borrowBE32(raw + 4);
	memcpy(&scale, &v, 4);
	v = borrowBE32(raw + 8);
	memcpy(&animscale, &v, 4);
	name = modDataBorrowFileName(b, borrowBE16(raw + 2));

	// only what the mod made itself: a stock file is a character the game has
	if (!name || !borrowShips(name) || !(scale > 0.01f && scale < 100.0f) || !(animscale > 0.01f && animscale < 100.0f)) {
		return 0;
	}

	memset(out, 0, sizeof(*out));
	out->ismale = bits >> 15;
	out->unk00_01 = (bits >> 14) & 1;
	out->canvaryheight = (bits >> 13) & 1;
	out->type = (bits >> 10) & 7;
	out->height = (bits >> 2) & 0xff;
	out->filenum = modDataBorrowFileId(b, borrowBE16(raw + 2));
	out->scale = scale;
	out->animscale = animscale;
	out->handfilenum = borrowBE16(raw + 16) ? modDataBorrowFileId(b, borrowBE16(raw + 16)) : 0;

	return out->filenum != 0;
}

/**
 * GoldenEye X's Combat Simulator characters, beside the game's: each body its
 * list names that is a model of its own (the list repeats some, and names a
 * few of the game's own), once, under its own name, and the heads those
 * bodies wear, then the rest of its heads while the list has room. A row a
 * stage has loaded keeps its model, since a chr may be wearing it.
 */
s32 modBorrowCharacters(s32 base, s32 maxrows, s32 maxindex)
{
	struct moddataborrow *b;
	s16 bodyrow[512];
	s16 headrow[512];
	s32 rows = 0;
	s32 numbodies = g_MpListCounts.bodies;
	s32 numheads = g_MpListCounts.heads;
	s32 addedbodies = 0;
	s32 addedheads = 0;

	charBase = -1;
	charRows = 0;

	if (src.found <= 0 || src.moddir < 0 || !src.spec.mpbodies || !src.spec.mpheads || !src.spec.headsandbodies) {
		return 0;
	}

	b = modDataBorrowOpen(&src.spec, src.dir, src.moddir, NULL, NULL, NULL);

	if (!b) {
		return 0;
	}

	for (s32 i = 0; i < (s32)ARRAYCOUNT(bodyrow); i++) {
		bodyrow[i] = headrow[i] = -1;
	}

	if (maxrows > BORROW_MAXROWS) {
		maxrows = BORROW_MAXROWS;
	}

	charBase = base;

	// A row for a mod index, made once
	#define TAKE(index, map) ({ \
		s32 taken_ = -1; \
		if ((index) >= 0 && (index) < (s32)ARRAYCOUNT(map)) { \
			if ((map)[(index)] >= 0) { \
				taken_ = (map)[(index)]; \
			} else if (rows < maxrows) { \
				struct headorbody hb_; \
				if (borrowHeadOrBody(b, (index), &hb_)) { \
					struct headorbody *e_ = &g_HeadsAndBodies[base + rows]; \
					struct modeldef *keep_ = e_->filenum == hb_.filenum ? e_->modeldef : NULL; \
					*e_ = hb_; \
					e_->modeldef = keep_; \
					charNames[rows][0] = '\0'; \
					(map)[(index)] = (s16)(base + rows); \
					taken_ = base + rows; \
					rows++; \
				} \
			} \
		} \
		taken_; })

	// The bodies, and the heads they wear
	for (s32 i = 0; i < src.spec.nummpbodies && numbodies + addedbodies <= maxindex; i++) {
		u8 raw[N64_MPBODY_SIZE];
		s16 modbody;
		s16 modhead;
		s32 body;
		s32 head;

		if (!modDataBorrowRead(b, src.spec.mpbodies + i * N64_MPBODY_SIZE, raw, sizeof(raw))) {
			break;
		}

		modbody = (s16)borrowBE16(raw);
		modhead = (s16)borrowBE16(raw + 4);

		if (modbody >= 0 && modbody < (s32)ARRAYCOUNT(bodyrow) && bodyrow[modbody] >= 0) {
			continue; // listed twice
		}

		body = TAKE(modbody, bodyrow);

		if (body < 0) {
			continue;
		}

		head = modhead == 1000 || modhead < 0 ? modhead : TAKE(modhead, headrow);

		if (!borrowLangString(b, borrowBE16(raw + 2), charNames[body - base], BORROW_NAMELEN)) {
			snprintf(charNames[body - base], BORROW_NAMELEN, "%s", romdataFileGetName(g_HeadsAndBodies[body].filenum));
		}

		g_MpBodies[numbodies + addedbodies].bodynum = body;
		g_MpBodies[numbodies + addedbodies].name = 0;
		g_MpBodies[numbodies + addedbodies].headnum = head >= 0 || head == 1000 ? head : -1;
		g_MpBodies[numbodies + addedbodies].requirefeature = 0;
		addedbodies++;
	}

	// The heads its list offers, those first that a body wears
	for (s32 pass = 0; pass < 2; pass++) {
		for (s32 i = 0; i < src.spec.nummpheads && numheads + addedheads <= maxindex; i++) {
			u8 raw[N64_MPHEAD_SIZE];
			s16 modhead;
			s32 head;
			s32 listed = 0;

			if (!modDataBorrowRead(b, src.spec.mpheads + i * N64_MPHEAD_SIZE, raw, sizeof(raw))) {
				break;
			}

			modhead = (s16)borrowBE16(raw);

			if (modhead < 0 || modhead >= (s32)ARRAYCOUNT(headrow) || (pass == 0 && headrow[modhead] < 0)) {
				continue;
			}

			head = TAKE(modhead, headrow);

			if (head < 0) {
				continue;
			}

			for (s32 k = numheads; k < numheads + addedheads; k++) {
				if (g_MpHeads[k].headnum == head) {
					listed = 1;
					break;
				}
			}

			if (!listed) {
				g_MpHeads[numheads + addedheads].headnum = head;
				g_MpHeads[numheads + addedheads].requirefeature = 0;
				addedheads++;
			}
		}
	}

	#undef TAKE

	modDataBorrowClose(b);

	g_MpListCounts.bodies = numbodies + addedbodies;
	g_MpListCounts.heads = numheads + addedheads;
	charRows = rows;

	if (rows) {
		sysLogPrintf(LOG_NOTE, "modborrow: %d characters and %d heads from `%s` in the Combat Simulator's lists (%d rows)",
				addedbodies, addedheads, src.name, rows);
	} else {
		charBase = -1;
	}

	return rows;
}

PD_CONSTRUCTOR static void modBorrowConfigInit(void)
{
	configRegisterString("Mod.BorrowGoldenEyeGuns", borrowSetting, sizeof(borrowSetting));
}
