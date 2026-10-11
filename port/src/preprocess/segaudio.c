#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libaudio.h"

#include "preprocess/common.h"

struct n64_adpcm_waveinfo {
	u32 loop; // ptr to ALADPCMloop
	u32 book; // ptr to ALADPCMBook
};

struct n64_raw_waveinfo {
	u32 loop; // ptr to ALRawLoop
};

struct n64_wavetable {
	u32 base;
	s32 len;
	u8 type;
	u8 flags;
	union {
		struct n64_adpcm_waveinfo adpcmWave;
		struct n64_raw_waveinfo rawWave;
	} waveInfo;
};

struct n64_sound {
	u32 envelope; // ptr to ALEnvelope
	u32 keyMap; // ptr to ALKeyMap
	u32 wavetable; // ptr to struct n64_wavetable
	u8 samplePan;
	u8 sampleVolume;
	u8 flags;
};

struct n64_instrument {
	u8 volume;
	u8 pan;
	u8 priority;
	u8 flags;
	u8 tremType;
	u8 tremRate;
	u8 tremDepth;
	u8 tremDelay;
	u8 vibType;
	u8 vibRate;
	u8 vibDepth;
	u8 vibDelay;
	s16 bendRange;
	s16 soundCount;
	u32 soundArray[1]; // ptr to struct n64_sound
};

struct n64_bank {
	s16 instCount;
	u8 flags;
	u8 pad;
	s32 sampleRate;
	u32 percussion; // ptr to struct n64_instrument
	u32 instArray[1]; // ptr to struct n64_instrument
};

struct n64_bankfile {
	s16 revision;
	s16 bankCount;
	u32 bankArray[1]; // ptr to struct n64_bank
};

// only proceeds to convert the next item if it's not already converted
#define AL_NEXT_ITEM(field, func) { \
	struct ptrmarker *marker = ptrFind(srcpos); \
	if (marker == NULL) { \
		field = (void *)(uintptr_t)(dstpos); \
		ptrAdd(srcpos, (uintptr_t)(field)); \
		dstpos = func(dst, dstpos, src, srcpos); \
	} else { \
		field = (void *)marker->ptr_host; } \
}

static u32 convertAudioEnvelope(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	ALEnvelope *n64_envelope = (ALEnvelope *) &src[srcpos];
	ALEnvelope *host_envelope = (ALEnvelope *) &dst[dstpos];

	dstpos += sizeof(ALEnvelope);

	host_envelope->attackTime = PD_BE32(n64_envelope->attackTime);
	host_envelope->decayTime = PD_BE32(n64_envelope->decayTime);
	host_envelope->releaseTime = PD_BE32(n64_envelope->releaseTime);
	host_envelope->attackVolume = n64_envelope->attackVolume;
	host_envelope->decayVolume = n64_envelope->decayVolume;

	return dstpos;
}

static u32 convertAudioKeyMap(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	ALKeyMap *n64_keymap = (ALKeyMap *) &src[srcpos];
	ALKeyMap *host_keymap = (ALKeyMap *) &dst[dstpos];

	dstpos += sizeof(ALKeyMap);

	host_keymap->velocityMin = n64_keymap->velocityMin;
	host_keymap->velocityMax = n64_keymap->velocityMax;
	host_keymap->keyMin = n64_keymap->keyMin;
	host_keymap->keyMax = n64_keymap->keyMax;
	host_keymap->keyBase = n64_keymap->keyBase;
	host_keymap->detune = n64_keymap->detune;

	return dstpos;
}

static u32 convertAudioAdpcmLoop(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	ALADPCMloop *n64_loop = (ALADPCMloop *) &src[srcpos];
	ALADPCMloop *host_loop = (ALADPCMloop *) &dst[dstpos];

	dstpos += sizeof(ALADPCMloop);

	host_loop->start = PD_BE32(n64_loop->start);
	host_loop->end = PD_BE32(n64_loop->end);
	host_loop->count = PD_BE32(n64_loop->count);

	for (int i = 0; i < ARRAYCOUNT(host_loop->state); i++) {
		host_loop->state[i] = PD_BE16(n64_loop->state[i]);
	}

	return dstpos;
}

// the size of the bank file being converted: a book is read as its 128
// entries at most, and no further than the file goes
static u32 g_ConvSrcSize;

static u32 convertAudioAdpcmBook(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	ALADPCMBook *n64_book = (ALADPCMBook *) &src[srcpos];
	ALADPCMBook *host_book = (ALADPCMBook *) &dst[dstpos];
	u32 count = ARRAYCOUNT(host_book->book);

	if (g_ConvSrcSize && (g_ConvSrcSize - srcpos - 8) / 2 < count) {
		count = (g_ConvSrcSize - srcpos - 8) / 2;
	}

	dstpos += sizeof(ALADPCMBook);

	host_book->order = PD_BE32(n64_book->order);
	host_book->npredictors = PD_BE32(n64_book->npredictors);

	for (u32 i = 0; i < count; i++) {
		host_book->book[i] = PD_BE16(n64_book->book[i]);
	}

	return dstpos;
}

static u32 convertAudioRawLoop(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	ALRawLoop *n64_loop = (ALRawLoop *) &src[srcpos];
	ALRawLoop *host_loop = (ALRawLoop *) &dst[dstpos];

	dstpos += sizeof(ALRawLoop);

	host_loop->start = PD_BE32(n64_loop->start);
	host_loop->end = PD_BE32(n64_loop->end);
	host_loop->count = PD_BE32(n64_loop->count);

	return dstpos;
}

static u32 convertAudioWaveTable(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	struct n64_wavetable *n64_wavetable = (struct n64_wavetable *) &src[srcpos];
	ALWaveTable *host_wavetable = (ALWaveTable *) &dst[dstpos];

	dstpos += sizeof(ALWaveTable);

	host_wavetable->base = (void *)(uintptr_t)PD_BE32(n64_wavetable->base);
	host_wavetable->len = PD_BE32(n64_wavetable->len);
	host_wavetable->type = n64_wavetable->type;
	host_wavetable->flags = n64_wavetable->flags;

	if (host_wavetable->type == AL_ADPCM_WAVE) {
		if (n64_wavetable->waveInfo.adpcmWave.loop) {
			srcpos = PD_BE32(n64_wavetable->waveInfo.adpcmWave.loop);
			AL_NEXT_ITEM(host_wavetable->waveInfo.adpcmWave.loop, convertAudioAdpcmLoop);
		} else {
			host_wavetable->waveInfo.adpcmWave.loop = NULL;
		}

		if (n64_wavetable->waveInfo.adpcmWave.book) {
			srcpos = PD_BE32(n64_wavetable->waveInfo.adpcmWave.book);
			AL_NEXT_ITEM(host_wavetable->waveInfo.adpcmWave.book, convertAudioAdpcmBook);
		} else {
			host_wavetable->waveInfo.adpcmWave.book = NULL;
		}
	} else if (host_wavetable->type == AL_RAW16_WAVE) {
		if (n64_wavetable->waveInfo.rawWave.loop) {
			srcpos = PD_BE32(n64_wavetable->waveInfo.rawWave.loop);
			AL_NEXT_ITEM(host_wavetable->waveInfo.rawWave.loop, convertAudioRawLoop);
		} else {
			host_wavetable->waveInfo.rawWave.loop = NULL;
		}
	}

	return dstpos;
}

static u32 convertAudioSound(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	struct n64_sound *n64_sound = (struct n64_sound *) &src[srcpos];
	ALSound *host_sound = (ALSound *) &dst[dstpos];

	dstpos += sizeof(ALSound);

	if (n64_sound->envelope) {
		srcpos = PD_BE32(n64_sound->envelope);
		AL_NEXT_ITEM(host_sound->envelope, convertAudioEnvelope);
	} else {
		host_sound->envelope = NULL;
	}

	if (n64_sound->keyMap) {
		srcpos = PD_BE32(n64_sound->keyMap);
		AL_NEXT_ITEM(host_sound->keyMap, convertAudioKeyMap);
	} else {
		host_sound->keyMap = NULL;
	}

	if (n64_sound->wavetable) {
		srcpos = PD_BE32(n64_sound->wavetable);
		AL_NEXT_ITEM(host_sound->wavetable, convertAudioWaveTable);
	} else {
		host_sound->wavetable = NULL;
	}

	host_sound->samplePan = n64_sound->samplePan;
	host_sound->sampleVolume = n64_sound->sampleVolume;
	host_sound->flags = n64_sound->flags;

	return dstpos;
}

static u32 convertAudioInstrument(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	struct n64_instrument *n64_instrument = (struct n64_instrument *) &src[srcpos];
	ALInstrument *host_instrument = (ALInstrument *) &dst[dstpos];
	const s16 soundCount = PD_BE16(n64_instrument->soundCount);

	host_instrument->volume = n64_instrument->volume;
	host_instrument->pan = n64_instrument->pan;
	host_instrument->priority = n64_instrument->priority;
	host_instrument->flags = n64_instrument->flags;
	host_instrument->tremType = n64_instrument->tremType;
	host_instrument->tremRate = n64_instrument->tremRate;
	host_instrument->tremDepth = n64_instrument->tremDepth;
	host_instrument->tremDelay = n64_instrument->tremDelay;
	host_instrument->vibType = n64_instrument->vibType;
	host_instrument->vibRate = n64_instrument->vibRate;
	host_instrument->vibDepth = n64_instrument->vibDepth;
	host_instrument->vibDelay = n64_instrument->vibDelay;
	host_instrument->bendRange = PD_BE16(n64_instrument->bendRange);
	host_instrument->soundCount = (soundCount);

	dstpos = dstpos + sizeof(ALInstrument) + sizeof(uintptr_t) * (soundCount - 1);

	for (int i = 0; i < soundCount; i++) {
		srcpos = PD_BE32(n64_instrument->soundArray[i]);
		AL_NEXT_ITEM(host_instrument->soundArray[i], convertAudioSound);
	}

	return dstpos;
}

static u32 convertAudioBank(u8 *dst, u32 dstpos, u8 *src, u32 srcpos)
{
	struct n64_bank *n64_bank = (struct n64_bank *) &src[srcpos];
	ALBank *host_bank = (ALBank *) &dst[dstpos];
	const s16 instCount = PD_BE16(n64_bank->instCount);

	host_bank->instCount = (instCount);
	host_bank->flags = n64_bank->flags;
	host_bank->pad = n64_bank->pad;
	host_bank->sampleRate = PD_BE32(n64_bank->sampleRate);

	dstpos = dstpos + sizeof(ALBank) + sizeof(uintptr_t) * (instCount - 1);

	if (n64_bank->percussion) {
		srcpos = PD_BE32(n64_bank->percussion);
		AL_NEXT_ITEM(host_bank->percussion, convertAudioInstrument);
	} else {
		host_bank->percussion = 0;
	}

	for (int i = 0; i < instCount; i++) {
		srcpos = PD_BE32(n64_bank->instArray[i]);
		AL_NEXT_ITEM(host_bank->instArray[i], convertAudioInstrument);
	}

	return dstpos;
}

static u32 convertAudioBankFile(u8 *dst, u8 *src)
{
	struct n64_bankfile *n64_bankfile = (struct n64_bankfile *)src;
	ALBankFile *host_bankfile = (ALBankFile *)dst;
	const s16 bankCount = PD_BE16(n64_bankfile->bankCount);

	host_bankfile->revision = PD_BE16(n64_bankfile->revision);
	host_bankfile->bankCount = (bankCount);

	u32 dstpos = sizeof(ALBankFile) + sizeof(uintptr_t) * (bankCount - 1);

	for (int i = 0; i < bankCount; i++) {
		host_bankfile->bankArray[i] = (void *)(uintptr_t)(dstpos);
		u32 srcpos = PD_BE32(n64_bankfile->bankArray[i]);
		dstpos = convertAudioBank(dst, dstpos, src, srcpos);
	}

	return dstpos;
}

/**
 * A walk of an ALBankFile as the converter below takes it, reading nothing
 * outside its size bytes: every item's offset and the bytes the converter
 * reads there, every count, each item once (by offset, as ptrFind() dedups).
 * Adds up the bytes the converted file takes. A mod's own sfx.ctl with an
 * offset past its end took the game down at startup in convertAudioWaveTable()
 * (crash 20261010-210721, mod DamOverture).
 */
struct bankcheck {
	const u8 *src;
	u32 size;
	u8 *seen;
	u32 need;
	s32 bad;
};

static s32 bankRead(struct bankcheck *c, u32 pos, u32 len)
{
	if (c->bad || pos >= c->size || len > c->size - pos) {
		c->bad = 1;
		return 0;
	}

	return 1;
}

static u32 bankU32(struct bankcheck *c, u32 pos)
{
	return bankRead(c, pos, 4) ? PD_BE32(*(u32 *)&c->src[pos]) : 0;
}

static s32 bankS16(struct bankcheck *c, u32 pos)
{
	return bankRead(c, pos, 2) ? (s16)PD_BE16(*(u16 *)&c->src[pos]) : 0;
}

// whether the item at pos (len bytes) is new: then it is counted as hostlen
static s32 bankItem(struct bankcheck *c, u32 pos, u32 len, u32 hostlen)
{
	if (!bankRead(c, pos, len)) {
		return 0;
	}

	if (c->seen[pos >> 3] & (1 << (pos & 7))) {
		return 0;
	}

	c->seen[pos >> 3] |= 1 << (pos & 7);
	c->need += hostlen;

	return 1;
}

static void bankCheckWave(struct bankcheck *c, u32 pos)
{
	if (!bankItem(c, pos, sizeof(struct n64_wavetable), sizeof(ALWaveTable))) {
		return;
	}

	const u8 type = c->src[pos + 8];
	const u32 a = bankU32(c, pos + 12);
	const u32 b = bankU32(c, pos + 16);

	if (type == AL_ADPCM_WAVE) {
		if (a) {
			bankItem(c, a, sizeof(ALADPCMloop), sizeof(ALADPCMloop));
		}

		if (b) {
			bankItem(c, b, 8, sizeof(ALADPCMBook));
		}
	} else if (type == AL_RAW16_WAVE && a) {
		bankItem(c, a, sizeof(ALRawLoop), sizeof(ALRawLoop));
	}
}

static void bankCheckInstrument(struct bankcheck *c, u32 pos)
{
	const s32 count = bankS16(c, pos + 14);

	if (count < 0 || !bankRead(c, pos, 16 + 4 * (u32)count)
			|| !bankItem(c, pos, 16 + 4 * (u32)count, sizeof(ALInstrument) + sizeof(uintptr_t) * count)) {
		return;
	}

	for (s32 i = 0; i < count && !c->bad; i++) {
		const u32 snd = bankU32(c, pos + 16 + 4 * i);

		if (!bankItem(c, snd, sizeof(struct n64_sound), sizeof(ALSound))) {
			continue;
		}

		const u32 env = bankU32(c, snd);
		const u32 keymap = bankU32(c, snd + 4);
		const u32 wave = bankU32(c, snd + 8);

		if (env) {
			bankItem(c, env, sizeof(ALEnvelope), sizeof(ALEnvelope));
		}

		if (keymap) {
			bankItem(c, keymap, sizeof(ALKeyMap), sizeof(ALKeyMap));
		}

		if (wave) {
			bankCheckWave(c, wave);
		}
	}
}

/** The bytes a valid bank file converts to, or 0 when src is not one. */
u32 preprocessALBankFileCheck(const u8 *src, u32 size)
{
	struct bankcheck c = { src, size, NULL, 0, 0 };
	const s32 nbanks = bankS16(&c, 2);

	if (c.bad || nbanks <= 0 || !bankRead(&c, 0, 4 + 4 * (u32)nbanks)) {
		return 0;
	}

	c.seen = sysMemZeroAlloc(size / 8 + 1);
	c.need = sizeof(ALBankFile) + sizeof(uintptr_t) * nbanks;

	for (s32 i = 0; i < nbanks && !c.bad; i++) {
		const u32 bank = bankU32(&c, 4 + 4 * i);
		const s32 ninst = bankS16(&c, bank);

		if (c.bad || ninst < 0) {
			c.bad = 1;
			break;
		}

		// a bank is converted once for each slot that names it (no ptrFind)
		if (!bankRead(&c, bank, 12 + 4 * (u32)ninst)) {
			break;
		}

		c.need += sizeof(ALBank) + sizeof(uintptr_t) * ninst;

		const u32 perc = bankU32(&c, bank + 8);

		if (perc) {
			bankCheckInstrument(&c, perc);
		}

		for (s32 k = 0; k < ninst && !c.bad; k++) {
			bankCheckInstrument(&c, bankU32(&c, bank + 12 + 4 * k));
		}
	}

	sysMemFree(c.seen);

	return c.bad ? 0 : c.need;
}

u8 *preprocessALBankFile(u8 *src, u32 size, u32 *outSize)
{
	// a file that does not read as a bank is not converted: the caller has
	// nothing, and keeps or falls back to what it had
	const u32 need = preprocessALBankFileCheck(src, size);

	if (need == 0) {
		sysLogPrintf(LOG_WARNING, "preprocess: a %u-byte ALBankFile does not read as one; not converted", size);
		*outSize = 0;
		return NULL;
	}

	ptrReset();
	g_ConvSrcSize = size;

	// This should overshoot any possible bank size: * 2 also works for vanilla
	// banks, and a GoldenEye ROM hack's sound bank (Goldfinger 64's) took more
	// than * 3 and was written past the end of it - the check below comes
	// after the writes. The room left over is given back below.
	const u32 dstlen = (need > size * 8 ? need : size * 8) + 0x1000;
	u8 *dst = sysMemZeroAlloc(dstlen);

	u32 reallen = convertAudioBankFile(dst, src);
	g_ConvSrcSize = 0;
	if (reallen > dstlen || ALIGN16(reallen) > dstlen) {
		sysFatalError("overflow when trying to preprocess an ALBankFile, size %u dstlen %u reallen %u", size, dstlen, reallen);
	}

	reallen = ALIGN16(reallen);

	if (reallen < dstlen) {
		dst = sysMemRealloc(dst, reallen);
	}

	if (reallen > size * 3) {
		sysLogPrintf(LOG_NOTE, "preprocess: a %u-byte ALBankFile took %u bytes", size, reallen);
	}

	*outSize = reallen;

	return dst;
}


u8 *preprocessALCMidiHdr(u8 *data, u32 size, u32 *outSize)
{
	ALCMidiHdr *hdr = (ALCMidiHdr *)data;
	PD_SWAP_VAL(hdr->division);
	for (s32 i = 0; i < ARRAYCOUNT(hdr->trackOffset); ++i) {
		PD_SWAP_VAL(hdr->trackOffset[i]);
	}
	return NULL;
}

u8 *preprocessSequences(u8* data, u32 size, u32 *outSize)
{
	struct seqtable *seq = (struct seqtable *)data;
	PD_SWAP_VAL(seq->count);

	for (s16 i = 0; i < seq->count; ++i) {
		PD_SWAP_VAL(seq->entries[i].binlen);
		PD_SWAP_VAL(seq->entries[i].ziplen);
		PD_SWAP_VAL(seq->entries[i].romaddr);
	}

	return NULL;
}
