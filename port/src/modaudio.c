/**
 * A PD mod's audio, entered and left between stages (2026-10-09).
 *
 * At boot sndInit() reads the five audio segments once: sfxctl and seqctl are
 * converted to the host layout into buffers of their own (preprocessALBankFile()),
 * the sequence table is byte-swapped in place, and the game keeps pointers
 * into all of it. A mod's audio is swapped in beside that, never over it:
 *
 * - **Sounds.** The game's sound table (g_ALSoundRomOffsets) holds each sound's
 *   ALSound as an address, and every loader in snd.c adds the boot sfxctl's and
 *   sfxtbl's starts to the offsets inside it. Those starts stay where they are -
 *   GoldenEye's sounds (gesfx.c) and a borrowed mod's (modborrow.c) are appended
 *   after the game's by offsets from them - so the mod's ALSounds are rebased
 *   onto them (sndRebaseSound()), exactly as an appended sound is, and its ids
 *   take the game's own slots. Those slots are reserved at the boot for the
 *   longest bank an installed mod has (modAudioReserveSounds(), before anything
 *   is appended), so the ids appended after them never move and a mod's
 *   sounds never run into them; a mod with fewer keeps the game's past its end.
 * - **Music.** The mod's seqctl is converted into a buffer of ours and made a
 *   bank on its seqtbl (alBnkfNew()), the music players are pointed at it
 *   (var80095204; seqPlay() rebinds a player whose bank differs), and its
 *   sequence table and addresses replace g_SeqTable/g_SeqRomAddrs, at its own
 *   length (up to SEQ_EXTRA_BASE; a shorter one keeps the game's past its end).
 *   Appended sequences (seqAppend()) are numbered from SEQ_EXTRA_BASE, not
 *   from the table's end, so they mean the same under every mod.
 * - A segment the mod does not ship is the game's: a ctl without a tbl names
 *   samples in the game's sfxtbl, a tbl without a ctl is read through a fresh
 *   conversion of the game's ctl out of the ROM.
 * - A sound appended as a second number for one of the game's
 *   (sndAppendSoundCopy(), gesfx.c) follows the bank, as it would have had the
 *   mod been booted with.
 *
 * Every cache holding a sample or a sound is flushed on each swap: the sound
 * cache (sndFlushCaches()), the players' bound banks, and the sample DMA cache
 * (admaFlush()), which is keyed by host address and would otherwise hand a
 * freed buffer's bytes to a sound whose new buffer landed at the same address.
 */

#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include "n_libaudio.h"
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "mod.h"
#include "modaudio.h"
#include "preprocess.h"
#include "romdata.h"
#include "system.h"
#include "lib/audiodma.h"
#include "lib/lib_317f0.h"
#include "lib/snd.h"
#include "game/music.h"

extern s32 g_NumSounds;
extern uintptr_t *g_ALSoundRomOffsets;
extern ALBank *var80095204;
extern struct seqtable *g_SeqTable;
extern uintptr_t *g_SeqRomAddrs;
extern bool g_SndDisabled;

void func00033cf0(u8 flags); // n_sndplayer.c: ends every sound whose flags hold these

/* ---- rebasing ---------------------------------------------------------- */

static s32 rebaseOnce(struct sndrebase *rb, uintptr_t off)
{
	for (s32 i = 0; i < rb->numseen; i++) {
		if (rb->seen[i] == off) {
			return 0;
		}
	}

	if (rb->numseen == rb->maxseen) {
		const s32 max = rb->maxseen ? rb->maxseen * 2 : 256;
		uintptr_t *grown = sysMemRealloc(rb->seen, max * sizeof(uintptr_t));

		if (!grown) {
			return 0;
		}

		rb->seen = grown;
		rb->maxseen = max;
	}

	rb->seen[rb->numseen++] = off;

	return 1;
}

uintptr_t sndRebaseSound(struct sndrebase *rb, uintptr_t off)
{
	const uintptr_t delta = (uintptr_t)rb->ctl - sndGetCtlStart();
	const uintptr_t tdelta = rb->tbl ? (uintptr_t)rb->tbl - sndGetTblStart() : 0;
	ALSound *sound = (ALSound *)(rb->ctl + off);

	if (rebaseOnce(rb, off)) {
		if (sound->envelope) {
			sound->envelope = (ALEnvelope *)((uintptr_t)sound->envelope + delta);
		}

		if (sound->keyMap) {
			sound->keyMap = (ALKeyMap *)((uintptr_t)sound->keyMap + delta);
		}

		if (sound->wavetable) {
			const uintptr_t woff = (uintptr_t)sound->wavetable;
			ALWaveTable *wave = (ALWaveTable *)(rb->ctl + woff);

			sound->wavetable = (ALWaveTable *)(woff + delta);

			if (rebaseOnce(rb, woff)) {
				wave->base = (u8 *)((uintptr_t)wave->base + tdelta);

				if (wave->type == AL_ADPCM_WAVE) {
					if (wave->waveInfo.adpcmWave.book) {
						wave->waveInfo.adpcmWave.book = (ALADPCMBook *)((uintptr_t)wave->waveInfo.adpcmWave.book + delta);
					}

					if (wave->waveInfo.adpcmWave.loop) {
						wave->waveInfo.adpcmWave.loop = (ALADPCMloop *)((uintptr_t)wave->waveInfo.adpcmWave.loop + delta);
					}
				} else if (wave->type == AL_RAW16_WAVE && wave->waveInfo.rawWave.loop) {
					// read by sndLoadWavetable() as a ctl offset, as the ADPCM loop is
					wave->waveInfo.rawWave.loop = (ALRawLoop *)((uintptr_t)wave->waveInfo.rawWave.loop + delta);
				}
			}
		}
	}

	return off + delta;
}

void sndRebaseFree(struct sndrebase *rb)
{
	sysMemFree(rb->seen);
	rb->seen = NULL;
	rb->numseen = 0;
	rb->maxseen = 0;
}

/* ---- state ------------------------------------------------------------- */

// the banks the game booted with, taken at the first swap
static struct {
	s32 taken;
	s32 numsounds;       // the boot bank's sounds: ids 1..numsounds, slots 0..numsounds-1
	uintptr_t *offsets;  // their g_ALSoundRomOffsets
	ALBank *bank;
	struct seqtable *seqtable;
	uintptr_t *seqaddrs;
} boot;

// the mod entered; every buffer here is ours
static struct {
	s32 active;
	char dir[FS_MAXPATH + 1];
	u8 *ctl;             // sfxctl converted, its ALSounds rebased (NULL: the game's)
	u8 *tbl;             // sfxtbl (NULL: the game's)
	u8 *seqctl;          // seqctl converted and made a bank (NULL: the game's bank)
	u8 *seqtbl;
	u8 *seqs;            // the sequences segment (NULL: the game's)
	struct seqtable *seqtable;
	uintptr_t *seqaddrs;
} cur;

static s32 g_QuiesceCalls;
static s32 g_ReservedSounds; // modAudioReserveSounds(), 0 before it

// room left after the reserve for what is appended (GoldenEye's, a borrowed mod's)
#define MODAUDIO_APPEND_ROOM 256

static ALInstrument *bankFirstInst(u8 *ctl)
{
	ALBankFile *file = (ALBankFile *)ctl;
	ALBank *bank = (ALBank *)(ctl + (uintptr_t)file->bankArray[0]);

	return (ALInstrument *)(ctl + (uintptr_t)bank->instArray[0]);
}

static void bootTake(void)
{
	if (boot.taken) {
		return;
	}

	// the slots reserved at the boot, not g_NumSounds: sounds may be appended already
	boot.numsounds = g_ReservedSounds ? g_ReservedSounds : bankFirstInst((u8 *)sndGetCtlStart())->soundCount;

	if (boot.numsounds > g_NumSounds - 1) {
		boot.numsounds = g_NumSounds - 1;
	}

	boot.offsets = sysMemAlloc(boot.numsounds * sizeof(uintptr_t));
	memcpy(boot.offsets, g_ALSoundRomOffsets, boot.numsounds * sizeof(uintptr_t));
	boot.bank = var80095204;
	boot.seqtable = g_SeqTable;
	boot.seqaddrs = g_SeqRomAddrs;
	boot.taken = 1;
}

/** A converted-or-raw bank file's first instrument's sound count, read big-endian from the raw file. */
static s32 rawBankSoundCount(const u8 *d, u32 len)
{
	u32 b, i;

	if (len < 8) {
		return 0;
	}

	b = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];

	if (b + 16 > len) {
		return 0;
	}

	i = (d[b + 12] << 24) | (d[b + 13] << 16) | (d[b + 14] << 8) | d[b + 15];

	if (i + 16 > len) {
		return 0;
	}

	return (s16)((d[i + 14] << 8) | d[i + 15]);
}

s32 modAudioReserveSounds(s32 bootcount)
{
	s32 reserve = bootcount;

	for (s32 m = 0; m < modListGetCount(); m++) {
		const char *dir = modListGetPath(m);
		char path[FS_MAXPATH + 1];
		u32 len = 0;
		u8 *raw;
		s32 n;

		if (!dir) {
			continue;
		}

		snprintf(path, sizeof(path), "%s/segs/sfxctl", dir);

		if (fsFileSize(path) <= 0 || !(raw = fsFileLoad(path, &len))) {
			continue;
		}

		n = rawBankSoundCount(raw, len);
		sysMemFree(raw);

		if (n > reserve) {
			sysLogPrintf(LOG_NOTE, "modaudio: %s has %d sounds, the boot bank %d: ids kept for them", dir, n, bootcount);
			reserve = n;
		}
	}

	if (reserve > SND_MAX_SOUNDS - 1 - MODAUDIO_APPEND_ROOM) {
		reserve = SND_MAX_SOUNDS - 1 - MODAUDIO_APPEND_ROOM;
	}

	if (reserve < bootcount) {
		reserve = bootcount;
	}

	g_ReservedSounds = reserve;

	return reserve;
}

/** A segment the mod ships, loaded; NULL when it does not. */
static u8 *modSegLoad(const char *moddir, const char *seg, u32 *len, u32 pad)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/segs/%s", moddir, seg);

	if (fsFileSize(path) <= 0) {
		return NULL;
	}

	return fsFileLoadPadded(path, len, pad);
}

/** A copy of the stock ROM's bytes of a segment (its declared size). */
static u8 *romSegCopy(const char *seg, u32 *len)
{
	for (s32 i = 0; i < romdataGetNumSegments(); i++) {
		u32 ofs = 0, size = 0;
		const char *name = romdataGetSegmentInfo(i, &ofs, &size);

		if (name && !strcmp(name, seg) && ofs && size && g_RomFile) {
			u8 *copy = sysMemAlloc(size);

			if (copy) {
				memcpy(copy, g_RomFile + ofs, size);
				*len = size;
			}

			return copy;
		}
	}

	return NULL;
}

/** A bank file converted to the host layout: the mod's, or the ROM's when only the mod's tbl came. */
static u8 *bankConvert(u8 *raw, u32 rawlen, const char *seg)
{
	u32 outlen = 0;
	u8 *ctl;

	if (!raw) {
		raw = romSegCopy(seg, &rawlen);
	}

	if (!raw) {
		return NULL;
	}

	ctl = preprocessALBankFile(raw, rawlen, &outlen);
	sysMemFree(raw);

	return ctl;
}

/* ---- flush ------------------------------------------------------------ */

// Buffers of a mod left while something still played: a voice may hold a
// pointer into its samples or its music bank until the stop reaches it, so
// they are freed once everything is quiet (modAudioQuiesce(), the next swap).
#define MODAUDIO_MAX_GRAVE 64
static void *g_Grave[MODAUDIO_MAX_GRAVE];
static s32 g_NumGrave;

static s32 audioIsQuiet(void)
{
	s16 numfree = 0, numalloced = 0;

	sndpCountStates(&numfree, &numalloced);

	if (numalloced) {
		return 0;
	}

	for (s32 i = 0; i < ARRAYCOUNT(g_SeqInstances); i++) {
		if (g_SeqInstances[i].seqp && n_alCSPGetState(g_SeqInstances[i].seqp) != AL_STOPPED) {
			return 0;
		}
	}

	return 1;
}

static void graveFree(void)
{
	for (s32 i = 0; i < g_NumGrave; i++) {
		sysMemFree(g_Grave[i]);
	}

	g_NumGrave = 0;
}

/** Frees buf now when nothing plays, else once nothing does. */
static void bufRelease(void *buf, s32 quiet)
{
	if (!buf) {
		return;
	}

	if (quiet) {
		sysMemFree(buf);
		return;
	}

	if (g_NumGrave == MODAUDIO_MAX_GRAVE) {
		// never quiet across 64 buffers: the oldest are long stopped
		sysMemFree(g_Grave[0]);
		memmove(g_Grave, g_Grave + 1, (MODAUDIO_MAX_GRAVE - 1) * sizeof(void *));
		g_NumGrave--;
	}

	g_Grave[g_NumGrave++] = buf;
}

/**
 * Every sound and sequence told to end now - the swap online, at the host's
 * stage load, may come before modAudioQuiesce() has drained them. The ends
 * reach the voices on the next audio frame; a click is the price.
 */
static void hardStop(void)
{
	musicStop();
	func00033cf0(0); // every state: (flags & 0) == 0

	for (s32 i = 0; i < ARRAYCOUNT(g_SeqInstances); i++) {
		N_ALCSPlayer *seqp = g_SeqInstances[i].seqp;

		if (seqp && seqp->state != AL_STOPPED) {
			// what a sequence's end does (n_csplayer.c): the stop frees its voices
			N_ALEvent evt;

			evt.type = AL_SEQP_STOP_EVT;
			seqp->state = AL_STOPPING;
			n_alEvtqPostEvent(&seqp->evtq, &evt, 0, 0);
		}
	}

	g_QuiesceCalls = 0;
}

/** Stops whatever still plays and flushes every cache; returns whether it was quiet already. */
static s32 flushAll(void)
{
	const s32 quiet = audioIsQuiet();

	if (quiet) {
		graveFree();
	} else {
		sysLogPrintf(LOG_NOTE, "modaudio: swapping with sound still playing: stopped hard");
		hardStop();
	}

	sndFlushCaches();
	admaFlush();

	return quiet;
}

/* ---- the swap ---------------------------------------------------------- */

/** Points appended copies of the game's sounds (sndAppendSoundCopy()) from one table to the other. */
static void copiesFollow(const uintptr_t *from, const uintptr_t *to, s32 num)
{
	for (s32 j = boot.numsounds; j < g_NumSounds - 1; j++) {
		for (s32 k = 0; k < num; k++) {
			if (g_ALSoundRomOffsets[j] == from[k]) {
				g_ALSoundRomOffsets[j] = to[k];
				break;
			}
		}
	}
}

static void enterSounds(const char *moddir)
{
	u32 ctllen = 0, tbllen = 0;
	u8 *raw = modSegLoad(moddir, "sfxctl", &ctllen, 0);
	u8 *tbl = modSegLoad(moddir, "sfxtbl", &tbllen, ADMA_ITEM_SIZE);
	struct sndrebase rb = {0};
	uintptr_t *mod;
	ALInstrument *inst;
	s32 num;

	if (!raw && !tbl) {
		return;
	}

	cur.ctl = bankConvert(raw, ctllen, "sfxctl");
	cur.tbl = tbl;

	if (!cur.ctl) {
		sysLogPrintf(LOG_WARNING, "modaudio: %s: no sound bank to read", moddir);
		sysMemFree(cur.tbl);
		cur.tbl = NULL;
		return;
	}

	inst = bankFirstInst(cur.ctl);
	num = inst->soundCount;

	if (num != boot.numsounds) {
		sysLogPrintf(LOG_WARNING, "modaudio: %s has %d sounds, the game %d: %s", moddir, num, boot.numsounds,
				num > boot.numsounds ? "the rest are left out" : "the game's own past its end");
	}

	if (num > boot.numsounds) {
		num = boot.numsounds;
	}

	rb.ctl = cur.ctl;
	rb.tbl = cur.tbl;

	mod = sysMemAlloc(boot.numsounds * sizeof(uintptr_t));
	memcpy(mod, boot.offsets, boot.numsounds * sizeof(uintptr_t));

	for (s32 i = 0; i < num; i++) {
		mod[i] = sndGetCtlStart() + sndRebaseSound(&rb, (uintptr_t)inst->soundArray[i]);
	}

	sndRebaseFree(&rb);

	copiesFollow(boot.offsets, mod, boot.numsounds);
	memcpy(g_ALSoundRomOffsets, mod, boot.numsounds * sizeof(uintptr_t));
	sysMemFree(mod);

	sysLogPrintf(LOG_NOTE, "modaudio: %d sounds from %s%s%s", num, moddir,
			raw ? "" : " (the game's sfxctl)", tbl ? "" : " (the game's sfxtbl)");
}

static void enterMusic(const char *moddir)
{
	u32 ctllen = 0, tbllen = 0, seqlen = 0;
	u8 *raw = modSegLoad(moddir, "seqctl", &ctllen, 0);
	u8 *tbl = modSegLoad(moddir, "seqtbl", &tbllen, ADMA_ITEM_SIZE);
	u8 *seqs = modSegLoad(moddir, "sequences", &seqlen, 0);

	if (raw || tbl) {
		cur.seqctl = bankConvert(raw, ctllen, "seqctl");
		cur.seqtbl = tbl;

		if (cur.seqctl) {
			ALBankFile *file = (ALBankFile *)cur.seqctl;

			alBnkfNew(file, tbl ? tbl : (u8 *)sndGetSeqTblStart());
			var80095204 = file->bankArray[0];
			sysLogPrintf(LOG_NOTE, "modaudio: music bank from %s%s%s", moddir,
					raw ? "" : " (the game's seqctl)", tbl ? "" : " (the game's seqtbl)");
		} else {
			sysMemFree(cur.seqtbl);
			cur.seqtbl = NULL;
		}
	}

	if (seqs && seqlen >= 4) {
		struct seqtable *modtable = (struct seqtable *)seqs;
		s32 num, count;

		preprocessSequences(seqs, seqlen, &seqlen);
		num = modtable->count;

		if (num * sizeof(struct seqtableentry) + 4 > seqlen) {
			sysLogPrintf(LOG_WARNING, "modaudio: %s: a sequence table of %d past its segment", moddir, num);
			sysMemFree(seqs);
			return;
		}

		if (num > SEQ_EXTRA_BASE) {
			sysLogPrintf(LOG_WARNING, "modaudio: %s has %d sequences, %d are kept", moddir, num, SEQ_EXTRA_BASE);
			num = SEQ_EXTRA_BASE;
		}

		// a shorter table keeps the game's own past its end
		count = num > boot.seqtable->count ? num : boot.seqtable->count;

		cur.seqs = seqs;
		cur.seqtable = sysMemZeroAlloc(count * sizeof(struct seqtableentry) + 4);
		cur.seqaddrs = sysMemZeroAlloc(count * sizeof(uintptr_t));
		cur.seqtable->count = count;

		for (s32 i = 0; i < count; i++) {
			if (i < num) {
				cur.seqtable->entries[i] = modtable->entries[i];
				cur.seqaddrs[i] = (uintptr_t)seqs + modtable->entries[i].romaddr;
			} else {
				cur.seqtable->entries[i] = boot.seqtable->entries[i];
				cur.seqaddrs[i] = boot.seqaddrs[i];
			}
		}

		g_SeqTable = cur.seqtable;
		g_SeqRomAddrs = cur.seqaddrs;
		sysLogPrintf(LOG_NOTE, "modaudio: %d sequences from %s", num, moddir);
	} else if (seqs) {
		sysMemFree(seqs);
	}
}

/**
 * A mod directory as --moddir finds one (fsAddModDir()): a bare relative path
 * is looked for beside the working directory, then beside the executable,
 * not under the base directory where fsFullPath() would put it.
 */
static void modDirResolve(const char *moddir, char *out)
{
	const char *tries[] = { ".", "$E" };

	snprintf(out, FS_MAXPATH + 1, "%s", moddir);

	if (moddir[0] == '$' || fsPathIsAbsolute(moddir) || fsPathIsCwdRelative(moddir)) {
		return;
	}

	for (s32 i = 0; i < ARRAYCOUNT(tries); i++) {
		char tmp[FS_MAXPATH + 1];

		snprintf(tmp, sizeof(tmp), "%s/%s", tries[i], moddir);

		if (fsFileSize(tmp) >= 0) {
			snprintf(out, FS_MAXPATH + 1, "%s", fsFullPath(tmp));
			return;
		}
	}
}

void modAudioEnter(const char *moddir)
{
	if (g_SndDisabled || !g_ALSoundRomOffsets || !g_SeqTable || !moddir || !moddir[0]) {
		return;
	}

	if (cur.active) {
		modAudioLeave();
	}

	bootTake();
	flushAll();

	cur.active = 1;
	modDirResolve(moddir, cur.dir);

	enterSounds(cur.dir);
	enterMusic(cur.dir);
}

void modAudioLeave(void)
{
	if (!cur.active) {
		return;
	}

	const s32 quiet = flushAll();

	if (cur.ctl) {
		uintptr_t *mod = sysMemAlloc(boot.numsounds * sizeof(uintptr_t));

		memcpy(mod, g_ALSoundRomOffsets, boot.numsounds * sizeof(uintptr_t));
		copiesFollow(mod, boot.offsets, boot.numsounds);
		sysMemFree(mod);
	}

	memcpy(g_ALSoundRomOffsets, boot.offsets, boot.numsounds * sizeof(uintptr_t));
	var80095204 = boot.bank;
	g_SeqTable = boot.seqtable;
	g_SeqRomAddrs = boot.seqaddrs;

	bufRelease(cur.ctl, quiet);
	bufRelease(cur.tbl, quiet);
	bufRelease(cur.seqctl, quiet);
	bufRelease(cur.seqtbl, quiet);
	bufRelease(cur.seqs, quiet);
	bufRelease(cur.seqtable, quiet);
	bufRelease(cur.seqaddrs, quiet);
	memset(&cur, 0, sizeof(cur));

	sysLogPrintf(LOG_NOTE, "modaudio: back to the game's own audio");
}

const char *modAudioCurrent(void)
{
	return cur.active ? cur.dir : NULL;
}

s32 modAudioQuiesce(void)
{
	if (g_SndDisabled || !g_ALSoundRomOffsets) {
		return 1;
	}

	if (g_QuiesceCalls == 0) {
		musicStop();
	}

	// the heads of the chains, as a stage's end does (lv.c); a sound that will
	// not stop for that is ended outright after a second
	func00033dd8();

	if (g_QuiesceCalls >= 60) {
		func00033cf0(SNDSTATEFLAG_01);
	}

	if (audioIsQuiet()) {
		g_QuiesceCalls = 0;
		graveFree();
		return 1;
	}

	g_QuiesceCalls++;

	return 0;
}

/* ---- the dump ---------------------------------------------------------- */

static u64 fnv(u64 h, const void *data, u32 len)
{
	const u8 *p = data;

	for (u32 i = 0; i < len; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}

	return h;
}

#define FNV_INIT 0xcbf29ce484222325ull
#define FNV_VAL(h, v) do { u64 _v = (u64)(v); h = fnv(h, &_v, sizeof(_v)); } while (0)

/** Hashes an ALSound and what it points at; ctl/tbl are added to its offsets (0 for a bank's pointers). */
static u64 hashSound(u64 h, const ALSound *s, uintptr_t ctl, uintptr_t tbl)
{
	FNV_VAL(h, s->samplePan);
	FNV_VAL(h, s->sampleVolume);

	if (s->envelope) {
		h = fnv(h, (u8 *)ctl + (uintptr_t)s->envelope, sizeof(ALEnvelope));
	}

	if (s->keyMap) {
		h = fnv(h, (u8 *)ctl + (uintptr_t)s->keyMap, sizeof(ALKeyMap));
	}

	if (s->wavetable) {
		const ALWaveTable *w = (ALWaveTable *)((u8 *)ctl + (uintptr_t)s->wavetable);

		// an ADPCM wave's len is cut to whole 9-byte frames the first time a
		// voice plays it (n_load.c), so it is hashed as that
		const s32 len = w->type == AL_ADPCM_WAVE ? w->len / 9 * 9 : w->len;

		FNV_VAL(h, w->type);
		FNV_VAL(h, len);

		if (len > 0 && len < 0x1000000) {
			h = fnv(h, (u8 *)tbl + (uintptr_t)w->base, len);
		}

		if (w->type == AL_ADPCM_WAVE) {
			const ALADPCMBook *book = w->waveInfo.adpcmWave.book
				? (ALADPCMBook *)((u8 *)ctl + (uintptr_t)w->waveInfo.adpcmWave.book) : NULL;

			if (book) {
				FNV_VAL(h, book->order);
				FNV_VAL(h, book->npredictors);

				if (book->order > 0 && book->npredictors > 0 && book->order * book->npredictors <= 64) {
					h = fnv(h, book->book, book->order * book->npredictors * 8 * sizeof(s16));
				}
			}

			if (w->waveInfo.adpcmWave.loop) {
				// not its state[], which the synth writes back as a voice plays
				const ALADPCMloop *loop = (ALADPCMloop *)((u8 *)ctl + (uintptr_t)w->waveInfo.adpcmWave.loop);

				FNV_VAL(h, loop->start);
				FNV_VAL(h, loop->end);
				FNV_VAL(h, loop->count);
			}
		} else if (w->waveInfo.rawWave.loop) {
			h = fnv(h, (u8 *)ctl + (uintptr_t)w->waveInfo.rawWave.loop, sizeof(ALRawLoop));
		}
	}

	return h;
}

void modAudioDump(const char *path)
{
	FILE *f = fsFileOpenWrite(path);
	u64 total;

	if (!f) {
		sysLogPrintf(LOG_WARNING, "modaudio: cannot write %s", path);
		return;
	}

	if (!g_ALSoundRomOffsets || !g_SeqTable) {
		fprintf(f, "no sound\n");
		fclose(f);
		return;
	}

	fprintf(f, "mod %s\nsounds %d\n", cur.active ? cur.dir : "-", g_NumSounds);
	total = FNV_INIT;

	for (s32 id = 1; id < g_NumSounds; id++) {
		const ALSound *s = (ALSound *)g_ALSoundRomOffsets[id - 1];
		const u64 h = hashSound(FNV_INIT, s, sndGetCtlStart(), sndGetTblStart());

		fprintf(f, "snd %d %016llx\n", id, (unsigned long long)h);
		FNV_VAL(total, h);
	}

	fprintf(f, "sounds total %016llx\n", (unsigned long long)total);

	if (var80095204) {
		ALBank *bank = var80095204;

		total = FNV_INIT;
		fprintf(f, "bank instruments %d\n", bank->instCount);

		for (s32 i = 0; i < bank->instCount; i++) {
			ALInstrument *inst = bank->instArray[i];
			u64 h = FNV_INIT;

			if (inst) {
				FNV_VAL(h, inst->volume);
				FNV_VAL(h, inst->pan);
				FNV_VAL(h, inst->priority);
				FNV_VAL(h, inst->tremType);
				FNV_VAL(h, inst->tremRate);
				FNV_VAL(h, inst->tremDepth);
				FNV_VAL(h, inst->tremDelay);
				FNV_VAL(h, inst->vibType);
				FNV_VAL(h, inst->vibRate);
				FNV_VAL(h, inst->vibDepth);
				FNV_VAL(h, inst->vibDelay);
				FNV_VAL(h, inst->bendRange);
				FNV_VAL(h, inst->soundCount);

				for (s32 j = 0; j < inst->soundCount; j++) {
					if (inst->soundArray[j]) {
						h = hashSound(h, inst->soundArray[j], 0, 0);
					}
				}
			}

			fprintf(f, "inst %d %016llx\n", i, (unsigned long long)h);
			FNV_VAL(total, h);
		}

		fprintf(f, "bank total %016llx\n", (unsigned long long)total);
	}

	total = FNV_INIT;
	fprintf(f, "sequences %d\n", g_SeqTable->count);

	for (s32 i = 0; i < g_SeqTable->count; i++) {
		const struct seqtableentry *e = &g_SeqTable->entries[i];
		u64 h = FNV_INIT;

		FNV_VAL(h, e->binlen);
		FNV_VAL(h, e->ziplen);

		if (g_SeqRomAddrs[i] >= 0x10000) {
			h = fnv(h, (u8 *)g_SeqRomAddrs[i], e->ziplen);
		}

		fprintf(f, "seq %d %016llx\n", i, (unsigned long long)h);
		FNV_VAL(total, h);
	}

	fprintf(f, "sequences total %016llx\n", (unsigned long long)total);
	fclose(f);

	sysLogPrintf(LOG_NOTE, "modaudio: dumped to %s", path);
}

/* ---- test switches ----------------------------------------------------- */

/**
 * --mod-audio-enter "A|B|-": enter A, then B, then leave ("-"), after the boot
 * has built its banks and before the first stage; --mod-dump-audio FILE then
 * dumps what is left. A test aid: compare with a --moddir boot's dump.
 */
void modAudioTestSwitches(void)
{
	const char *list = sysArgGetString("--mod-audio-enter");
	const char *dump = sysArgGetString("--mod-dump-audio");

	if (list) {
		char buf[4096];
		char *p = buf;

		snprintf(buf, sizeof(buf), "%s", list);

		while (p) {
			char *next = strchr(p, '|');

			if (next) {
				*next++ = '\0';
			}

			// nothing plays before the first stage; without frames a sound
			// could not finish stopping, so this is asked once
			modAudioQuiesce();

			if (!strcmp(p, "-")) {
				modAudioLeave();
			} else if (*p) {
				modAudioEnter(p);
			}

			p = next;
		}
	}

	if (dump) {
		modAudioDump(dump);
	}
}
