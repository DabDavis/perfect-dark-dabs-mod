/**
 * Reading Xbox 360 containers. See x360.h for what the three layers are.
 *
 * The LZX decoder is the part with room to go subtly wrong, so two things are
 * written down here rather than left to be rediscovered:
 *
 *   * A chunk is not a stream. The window, the repeated offsets and the
 *     Huffman trees all carry across a chunk boundary, and only the bit buffer
 *     restarts. Decoding each chunk from a fresh context gives plausible
 *     looking output for the first chunk and rubbish after it.
 *   * The window is 17 bits, which fixes the number of position slots at 34
 *     and the main tree at 528 symbols. Every other size fails to decode
 *     these streams at all, so "bad huffman code" means the framing was read
 *     wrong, not that the window needs trying at another size.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "dxt.h"
#include "x360.h"

/* -------------------------------------------------------------------------
 * LZX
 * ------------------------------------------------------------------------- */

#define LZX_MIN_MATCH      2
#define LZX_NUM_CHARS      256
#define LZX_PRETREE_SYMS   20
#define LZX_LENGTH_SYMS    249
#define LZX_ALIGNED_SYMS   8
#define LZX_MAX_POS_SLOTS  50
#define LZX_MAX_MAIN       (LZX_NUM_CHARS + LZX_MAX_POS_SLOTS * 8)
#define LZX_MAX_SYMS       LZX_MAX_MAIN

#define LZX_BLOCK_VERBATIM    1
#define LZX_BLOCK_ALIGNED     2
#define LZX_BLOCK_UNCOMPRESSED 3

// Chunks whose header does not say otherwise hold this much output.
#define LZX_DEFAULT_CHUNK 0x8000

struct lzxbits {
	const u8 *in;
	u32 inLen;
	u32 inPos;
	u64 buf;
	s32 count;   // bits held in buf, never above 48, so buf cannot overflow
};

struct lzxhuff {
	u16 count[17];
	u16 sym[LZX_MAX_SYMS];
};

struct lzxstate {
	u8 mainLens[LZX_MAX_MAIN];
	u8 lengthLens[LZX_LENGTH_SYMS];
	struct lzxhuff main;
	struct lzxhuff length;
	struct lzxhuff aligned;
	u32 R[3];
	u32 numMain;
	s32 blockType;
	u32 blockLeft;
	s32 started;
	s32 failed;
	u8 *out;
	u32 outLen;
	u32 outPos;
};

// Extra bits and base offset per position slot, built once. The tables are
// the LZX spec's: pairs of slots share an extra bit count, which climbs to 17.
static u8 lzxExtraBits[LZX_MAX_POS_SLOTS + 1];
static u32 lzxBaseOffset[LZX_MAX_POS_SLOTS + 1];
static s32 lzxTablesReady = 0;

static void lzxInitTables(void)
{
	u32 j = 0;

	if (lzxTablesReady) {
		return;
	}

	for (u32 i = 0; i <= LZX_MAX_POS_SLOTS; i += 2) {
		lzxExtraBits[i] = (u8)j;
		if (i + 1 <= LZX_MAX_POS_SLOTS) {
			lzxExtraBits[i + 1] = (u8)j;
		}
		if (i != 0 && j < 17) {
			j++;
		}
	}

	j = 0;
	for (u32 i = 0; i <= LZX_MAX_POS_SLOTS; i++) {
		lzxBaseOffset[i] = j;
		j += 1u << lzxExtraBits[i];
	}

	lzxTablesReady = 1;
}

static u32 lzxPositionSlots(u32 windowBits)
{
	switch (windowBits) {
	case 20: return 42;
	case 21: return 50;
	default: return windowBits * 2;
	}
}

static void lzxFill(struct lzxbits *b, s32 need)
{
	while (b->count < need) {
		const u32 lo = b->inPos < b->inLen ? b->in[b->inPos] : 0;
		const u32 hi = b->inPos + 1 < b->inLen ? b->in[b->inPos + 1] : 0;

		b->inPos += 2;
		b->buf = (b->buf << 16) | (lo | (hi << 8));
		b->count += 16;
	}
}

// LZX reads 16 bit little-endian words and takes bits out of them MSB first.
static u32 lzxRead(struct lzxbits *b, s32 n)
{
	u32 v;

	if (n <= 0) {
		return 0;
	}

	lzxFill(b, n);
	v = (u32)((b->buf >> (b->count - n)) & ((1ull << n) - 1));
	b->count -= n;

	return v;
}

static void lzxAlign(struct lzxbits *b)
{
	// Back the read position up over whatever whole words are still buffered,
	// so a raw read afterwards carries on from the right byte.
	if (b->count & 15) {
		lzxRead(b, b->count & 15);
	}

	b->inPos -= (u32)(b->count >> 3);
	b->count = 0;
	b->buf = 0;
}

static void lzxBuildHuff(struct lzxhuff *h, const u8 *lens, u32 n)
{
	u32 index = 0;

	memset(h->count, 0, sizeof(h->count));

	for (u32 i = 0; i < n; i++) {
		if (lens[i]) {
			h->count[lens[i]]++;
		}
	}

	for (u32 l = 1; l <= 16; l++) {
		for (u32 i = 0; i < n; i++) {
			if (lens[i] == l) {
				h->sym[index++] = (u16)i;
			}
		}
	}
}

// Canonical decode, a bit at a time. Returns -1 on a code no length covers.
static s32 lzxDecodeSym(struct lzxhuff *h, struct lzxbits *b)
{
	u32 code = 0;
	u32 first = 0;
	u32 index = 0;

	for (u32 l = 1; l <= 16; l++) {
		code |= lzxRead(b, 1);

		if (code - first < h->count[l]) {
			return h->sym[index + code - first];
		}

		index += h->count[l];
		first = (first + h->count[l]) << 1;
		code <<= 1;
	}

	return -1;
}

/**
 * Reads a run of code lengths, which are deltas against the ones already
 * there - which is why lens[] persists in the state across blocks.
 */
static s32 lzxReadLengths(struct lzxstate *s, struct lzxbits *b, u8 *lens,
		u32 first, u32 last)
{
	u8 preLens[LZX_PRETREE_SYMS];
	struct lzxhuff pre;
	u32 x = first;

	for (u32 i = 0; i < LZX_PRETREE_SYMS; i++) {
		preLens[i] = (u8)lzxRead(b, 4);
	}

	lzxBuildHuff(&pre, preLens, LZX_PRETREE_SYMS);

	while (x < last) {
		const s32 z = lzxDecodeSym(&pre, b);
		s32 v;
		u32 run;

		if (z < 0) {
			return 0;
		}

		switch (z) {
		case 17:
			run = lzxRead(b, 4) + 4;
			while (run-- && x < last) {
				lens[x++] = 0;
			}
			break;
		case 18:
			run = lzxRead(b, 5) + 20;
			while (run-- && x < last) {
				lens[x++] = 0;
			}
			break;
		case 19:
			run = lzxRead(b, 1) + 4;
			v = lzxDecodeSym(&pre, b);
			if (v < 0) {
				return 0;
			}
			v = (s32)lens[x] - v;
			if (v < 0) {
				v += 17;
			}
			while (run-- && x < last) {
				lens[x++] = (u8)v;
			}
			break;
		default:
			v = (s32)lens[x] - z;
			if (v < 0) {
				v += 17;
			}
			lens[x++] = (u8)v;
			break;
		}
	}

	return 1;
}

static s32 lzxChunk(struct lzxstate *s, const u8 *src, u32 srcLen, u32 outBytes)
{
	struct lzxbits b = { src, srcLen, 0, 0, 0 };
	const u32 target = s->outPos + outBytes;

	if (target > s->outLen) {
		return 0;
	}

	if (!s->started) {
		// One header for the whole stream: an Intel (E8) translation size,
		// which these packages never set.
		if (lzxRead(&b, 1)) {
			lzxRead(&b, 16);
			lzxRead(&b, 16);
		}
		s->started = 1;
	}

	while (s->outPos < target) {
		if (s->blockLeft == 0) {
			s->blockType = (s32)lzxRead(&b, 3);
			s->blockLeft = (lzxRead(&b, 16) << 8) | lzxRead(&b, 8);

			switch (s->blockType) {
			case LZX_BLOCK_ALIGNED: {
				u8 alignedLens[LZX_ALIGNED_SYMS];
				for (u32 i = 0; i < LZX_ALIGNED_SYMS; i++) {
					alignedLens[i] = (u8)lzxRead(&b, 3);
				}
				lzxBuildHuff(&s->aligned, alignedLens, LZX_ALIGNED_SYMS);
			}
			/* fallthrough */
			case LZX_BLOCK_VERBATIM:
				if (!lzxReadLengths(s, &b, s->mainLens, 0, LZX_NUM_CHARS) ||
						!lzxReadLengths(s, &b, s->mainLens, LZX_NUM_CHARS, s->numMain) ||
						!lzxReadLengths(s, &b, s->lengthLens, 0, LZX_LENGTH_SYMS)) {
					return 0;
				}
				lzxBuildHuff(&s->main, s->mainLens, s->numMain);
				lzxBuildHuff(&s->length, s->lengthLens, LZX_LENGTH_SYMS);
				break;
			case LZX_BLOCK_UNCOMPRESSED:
				lzxAlign(&b);
				if (b.inPos + 12 > b.inLen) {
					return 0;
				}
				for (s32 i = 0; i < 3; i++) {
					const u8 *p = b.in + b.inPos + i * 4;
					s->R[i] = p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
				}
				b.inPos += 12;
				break;
			default:
				return 0;
			}
		}

		u32 todo = target - s->outPos;
		if (todo > s->blockLeft) {
			todo = s->blockLeft;
		}

		if (s->blockType == LZX_BLOCK_UNCOMPRESSED) {
			if (b.inPos + todo > b.inLen) {
				return 0;
			}
			memcpy(s->out + s->outPos, b.in + b.inPos, todo);
			b.inPos += todo;
			s->outPos += todo;
			s->blockLeft -= todo;
			if (s->blockLeft == 0 && (b.inPos & 1)) {
				b.inPos++;
			}
			continue;
		}

		u32 done = 0;

		while (done < todo) {
			const s32 sym = lzxDecodeSym(&s->main, &b);
			u32 matchLen;
			u32 slot;
			u32 offset;

			if (sym < 0) {
				return 0;
			}

			if (sym < LZX_NUM_CHARS) {
				s->out[s->outPos++] = (u8)sym;
				done++;
				continue;
			}

			matchLen = (u32)(sym - LZX_NUM_CHARS) & 7;

			if (matchLen == 7) {
				const s32 extra = lzxDecodeSym(&s->length, &b);
				if (extra < 0) {
					return 0;
				}
				matchLen = (u32)extra + 7;
			}

			matchLen += LZX_MIN_MATCH;
			slot = (u32)(sym - LZX_NUM_CHARS) >> 3;

			if (slot == 0) {
				offset = s->R[0];
			} else if (slot == 1) {
				offset = s->R[1];
				s->R[1] = s->R[0];
				s->R[0] = offset;
			} else if (slot == 2) {
				offset = s->R[2];
				s->R[2] = s->R[0];
				s->R[0] = offset;
			} else {
				const s32 extra = lzxExtraBits[slot];
				u32 verbatim = 0;
				u32 aligned = 0;

				if (s->blockType == LZX_BLOCK_ALIGNED && extra >= 3) {
					s32 a;
					verbatim = lzxRead(&b, extra - 3) << 3;
					a = lzxDecodeSym(&s->aligned, &b);
					if (a < 0) {
						return 0;
					}
					aligned = (u32)a;
				} else {
					verbatim = lzxRead(&b, extra);
				}

				offset = lzxBaseOffset[slot] - 2 + verbatim + aligned;
				s->R[2] = s->R[1];
				s->R[1] = s->R[0];
				s->R[0] = offset;
			}

			if (offset > s->outPos || s->outPos + matchLen > target) {
				return 0;
			}

			// Overlapping by design: a match may reach into what it is about
			// to write, so this copies forward a byte at a time.
			u8 *dst = s->out + s->outPos;
			const u8 *from = dst - offset;
			for (u32 i = 0; i < matchLen; i++) {
				dst[i] = from[i];
			}

			s->outPos += matchLen;
			done += matchLen;
		}

		s->blockLeft -= done;
	}

	return 1;
}

u32 x360LzxDecompress(const u8 *src, u32 srcLen, u8 *dst, u32 dstLen)
{
	struct lzxstate *s;
	u32 p = 0;
	u32 result = 0;

	lzxInitTables();

	s = calloc(1, sizeof(*s));
	if (!s) {
		return 0;
	}

	s->numMain = LZX_NUM_CHARS + lzxPositionSlots(X360_LZX_WINDOW_BITS) * 8;
	s->R[0] = s->R[1] = s->R[2] = 1;
	s->out = dst;
	s->outLen = dstLen;

	while (p < srcLen) {
		u32 outBytes;
		u32 chunkLen;
		u32 head;

		if (src[p] == 0xff) {
			if (p + 5 > srcLen) {
				break;
			}
			outBytes = ((u32)src[p + 1] << 8) | src[p + 2];
			chunkLen = ((u32)src[p + 3] << 8) | src[p + 4];
			head = 5;
		} else {
			if (p + 2 > srcLen) {
				break;
			}
			chunkLen = ((u32)src[p] << 8) | src[p + 1];
			outBytes = LZX_DEFAULT_CHUNK;
			head = 2;
		}

		if (chunkLen == 0 || p + head + chunkLen > srcLen) {
			break;
		}

		if (!lzxChunk(s, src + p + head, chunkLen, outBytes)) {
			s->failed = 1;
			break;
		}

		p += head + chunkLen;
	}

	if (!s->failed) {
		result = s->outPos;
	}

	free(s);

	return result;
}

/* -------------------------------------------------------------------------
 * STFS
 * ------------------------------------------------------------------------- */

#define STFS_DATA_START      0xc000
#define STFS_BLOCK           0x1000
#define STFS_BLOCKS_PER_HASH 0xaa
#define STFS_ENTRY_SIZE      0x40
#define STFS_MAX_FILES       0x8000

static u32 stfsBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 stfsLE24(const u8 *p)
{
	return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
}

static u32 stfsBackingBlock(struct x360stfs *stfs, u32 block)
{
	u32 adjust = 0;

	if (block >= 0xaa) {
		adjust += ((block / 0xaa) + 1) << stfs->tableShift;
	}

	if (block >= 0x70e4) {
		adjust += ((block / 0x70e4) + 1) << stfs->tableShift;
	}

	return block + adjust;
}

static u32 stfsBlockOffset(struct x360stfs *stfs, u32 block)
{
	return STFS_DATA_START + stfsBackingBlock(stfs, block) * STFS_BLOCK;
}

static s32 stfsReadBlock(struct x360stfs *stfs, u32 block, u8 *dst)
{
	FILE *fp = stfs->fp;

	if (fseek(fp, (long)stfsBlockOffset(stfs, block), SEEK_SET) != 0) {
		return 0;
	}

	return fread(dst, 1, STFS_BLOCK, fp) == STFS_BLOCK;
}

// The block after this one, from its level 0 hash table entry. Only needed for
// a file whose blocks are not consecutive, which in a read-only package is
// rare but not impossible.
static u32 stfsNextBlock(struct x360stfs *stfs, u32 block)
{
	const u32 table = stfsBlockOffset(stfs, block - (block % STFS_BLOCKS_PER_HASH))
			- STFS_BLOCK;
	u8 entry[0x18];
	FILE *fp = stfs->fp;

	if (fseek(fp, (long)(table + (block % STFS_BLOCKS_PER_HASH) * 0x18), SEEK_SET) != 0 ||
			fread(entry, 1, sizeof(entry), fp) != sizeof(entry)) {
		return 0xffffff;
	}

	// The entry is 20 bytes of hash then a status byte, and the next block
	// number as 24 bits big-endian.
	return ((u32)entry[0x15] << 16) | ((u32)entry[0x16] << 8) | entry[0x17];
}

s32 x360StfsOpen(struct x360stfs *stfs, const char *path)
{
	u8 head[STFS_BLOCK];
	u32 headerSize;
	FILE *fp;

	memset(stfs, 0, sizeof(*stfs));

	fp = fopen(path, "rb");
	if (!fp) {
		return 0;
	}

	if (fread(head, 1, sizeof(head), fp) != sizeof(head)) {
		fclose(fp);
		return 0;
	}

	if (memcmp(head, "LIVE", 4) && memcmp(head, "CON ", 4) && memcmp(head, "PIRS", 4)) {
		fclose(fp);
		return 0;
	}

	stfs->fp = fp;

	// A read-only package keeps one hash table before each run of 0xaa
	// blocks; a read-write one keeps two and every table offset doubles. The
	// rounded header size is what says which: 0xb000 means one.
	headerSize = stfsBE32(head + 0x340);
	stfs->tableShift = (((headerSize + 0xfff) & 0xf000) >> 12) == 0xb ? 0 : 1;

	// Volume descriptor at 0x379: length, version, block separation, then the
	// file table's block count and block number. Reading these one byte out
	// is the classic mistake here - it turns a one block table of names into
	// 257 blocks of mostly garbage.
	stfs->fileTableBlocks = head[0x37c] | ((u32)head[0x37d] << 8);
	stfs->fileTableBlock = stfsLE24(head + 0x37e);
	stfs->totalBlocks = stfsBE32(head + 0x395);

	if (stfs->fileTableBlocks == 0 || stfs->fileTableBlocks > 0x400) {
		x360StfsClose(stfs);
		return 0;
	}

	const u32 tableBytes = stfs->fileTableBlocks * STFS_BLOCK;
	u8 *table = malloc(tableBytes);

	if (!table) {
		x360StfsClose(stfs);
		return 0;
	}

	for (u32 i = 0; i < stfs->fileTableBlocks; i++) {
		if (!stfsReadBlock(stfs, stfs->fileTableBlock + i, table + i * STFS_BLOCK)) {
			free(table);
			x360StfsClose(stfs);
			return 0;
		}
	}

	const u32 slots = tableBytes / STFS_ENTRY_SIZE;
	stfs->files = calloc(slots, sizeof(*stfs->files));

	if (!stfs->files) {
		free(table);
		x360StfsClose(stfs);
		return 0;
	}

	for (u32 i = 0; i < slots && stfs->numFiles < STFS_MAX_FILES; i++) {
		const u8 *e = table + i * STFS_ENTRY_SIZE;
		const u32 flags = e[0x28];
		const u32 nameLen = flags & 0x3f;
		struct x360stfsfile *f;

		if (nameLen == 0) {
			continue;
		}

		f = &stfs->files[stfs->numFiles++];
		memcpy(f->name, e, nameLen);
		f->name[nameLen] = '\0';
		f->isDir = (flags & 0x80) ? 1 : 0;
		f->consecutive = (flags & 0x40) ? 1 : 0;
		f->numBlocks = stfsLE24(e + 0x29);
		f->startBlock = stfsLE24(e + 0x2f);
		f->parent = (u16)((e[0x32] << 8) | e[0x33]);
		f->size = stfsBE32(e + 0x34);
	}

	free(table);

	return 1;
}

void x360StfsClose(struct x360stfs *stfs)
{
	if (stfs->fp) {
		fclose(stfs->fp);
	}

	free(stfs->files);
	memset(stfs, 0, sizeof(*stfs));
}

const char *x360StfsPath(struct x360stfs *stfs, u32 index, char *buf, u32 bufLen)
{
	const char *parts[32];
	u32 numParts = 0;
	u32 at = index;
	u32 pos = 0;

	if (index >= stfs->numFiles) {
		buf[0] = '\0';
		return buf;
	}

	// Walk up to the root, bounded: a malformed table could name a cycle.
	while (numParts < 32) {
		parts[numParts++] = stfs->files[at].name;

		const u16 parent = stfs->files[at].parent;
		if (parent == 0xffff || parent >= stfs->numFiles) {
			break;
		}

		at = parent;
	}

	for (s32 i = (s32)numParts - 1; i >= 0; i--) {
		const u32 len = (u32)strlen(parts[i]);

		if (pos + len + 2 > bufLen) {
			break;
		}

		memcpy(buf + pos, parts[i], len);
		pos += len;

		if (i > 0) {
			buf[pos++] = '/';
		}
	}

	buf[pos] = '\0';

	return buf;
}

s32 x360StfsFind(struct x360stfs *stfs, const char *path)
{
	char buf[512];

	for (u32 i = 0; i < stfs->numFiles; i++) {
		if (stfs->files[i].isDir) {
			continue;
		}

		if (!strcmp(x360StfsPath(stfs, i, buf, sizeof(buf)), path)) {
			return (s32)i;
		}
	}

	return -1;
}

u8 *x360StfsRead(struct x360stfs *stfs, u32 index, u32 *outLen)
{
	struct x360stfsfile *f;
	u8 block[STFS_BLOCK];
	u8 *out;
	u32 left;
	u32 at;
	u32 pos = 0;

	if (index >= stfs->numFiles) {
		return NULL;
	}

	f = &stfs->files[index];
	out = malloc(f->size ? f->size : 1);

	if (!out) {
		return NULL;
	}

	left = f->size;
	at = f->startBlock;

	for (u32 i = 0; i < f->numBlocks && left; i++) {
		const u32 take = left < STFS_BLOCK ? left : STFS_BLOCK;

		if (!stfsReadBlock(stfs, at, block)) {
			free(out);
			return NULL;
		}

		memcpy(out + pos, block, take);
		pos += take;
		left -= take;

		at = f->consecutive ? at + 1 : stfsNextBlock(stfs, at);

		if (at == 0xffffff || at >= stfs->totalBlocks) {
			break;
		}
	}

	if (outLen) {
		*outLen = pos;
	}

	return out;
}

s32 x360StfsStreamOpen(struct x360stfs *stfs, u32 index, struct x360stfsstream *stream)
{
	struct x360stfsfile *f;
	u32 at;

	memset(stream, 0, sizeof(*stream));

	if (index >= stfs->numFiles) {
		return 0;
	}

	f = &stfs->files[index];

	if (!f->numBlocks) {
		return 0;
	}

	stream->blocks = malloc(f->numBlocks * sizeof(u32));

	if (!stream->blocks) {
		return 0;
	}

	stream->stfs = stfs;
	stream->size = f->size;
	at = f->startBlock;

	for (u32 i = 0; i < f->numBlocks; i++) {
		stream->blocks[stream->numBlocks++] = at;
		at = f->consecutive ? at + 1 : stfsNextBlock(stfs, at);

		if (at == 0xffffff || at >= stfs->totalBlocks) {
			break;
		}
	}

	return 1;
}

void x360StfsStreamClose(struct x360stfsstream *stream)
{
	free(stream->blocks);
	memset(stream, 0, sizeof(*stream));
}

s32 x360StfsStreamRead(struct x360stfsstream *stream, u32 offset, u32 len, u8 *dst)
{
	u8 block[STFS_BLOCK];
	u32 done = 0;

	if (offset > stream->size || len > stream->size - offset) {
		return 0;
	}

	while (done < len) {
		const u32 pos = offset + done;
		const u32 index = pos / STFS_BLOCK;
		const u32 within = pos % STFS_BLOCK;
		u32 take = STFS_BLOCK - within;

		if (index >= stream->numBlocks) {
			return 0;
		}

		if (take > len - done) {
			take = len - done;
		}

		if (!stfsReadBlock(stream->stfs, stream->blocks[index], block)) {
			return 0;
		}

		memcpy(dst + done, block + within, take);
		done += take;
	}

	return 1;
}

/* -------------------------------------------------------------------------
 * Texture surfaces
 * ------------------------------------------------------------------------- */

void x360FetchRead(struct x360fetch *fetch, const u32 *dwords)
{
	const u32 d0 = dwords[0];
	const u32 d1 = dwords[1];
	const u32 d2 = dwords[2];

	fetch->tiled = (d0 >> 31) & 1;
	// Pitch counts 32 texel groups. A block format aligns to 32 blocks, so
	// its stored row comes out a multiple of 128 texels rather than 32.
	fetch->pitch = ((d0 >> 22) & 0x1ff) * 32;
	fetch->format = (u8)(d1 & 0x3f);
	fetch->endian = (u8)((d1 >> 6) & 3);
	fetch->width = (d2 & 0x1fff) + 1;
	fetch->height = ((d2 >> 13) & 0x1fff) + 1;
}

s32 x360FetchSupported(const struct x360fetch *fetch)
{
	switch (fetch->format) {
	case X360_FMT_8888:
	case X360_FMT_DXT1:
	case X360_FMT_DXT23:
	case X360_FMT_DXT45:
		return 1;
	default:
		return 0;
	}
}

static u32 x360BytesPerElement(u8 format)
{
	switch (format) {
	case X360_FMT_8888: return 4;
	case X360_FMT_DXT1: return 8;
	default:            return 16;
	}
}

static s32 x360DxtKind(u8 format)
{
	switch (format) {
	case X360_FMT_DXT1:  return DXT_KIND_1;
	case X360_FMT_DXT23: return DXT_KIND_3;
	default:             return DXT_KIND_5;
	}
}

static void x360EndianSwap(u8 *data, u32 len, u8 mode)
{
	if (mode == 1) {
		for (u32 i = 0; i + 1 < len; i += 2) {
			const u8 t = data[i];
			data[i] = data[i + 1];
			data[i + 1] = t;
		}
	} else if (mode == 2) {
		for (u32 i = 0; i + 3 < len; i += 4) {
			u8 t = data[i];
			data[i] = data[i + 3];
			data[i + 3] = t;
			t = data[i + 1];
			data[i + 1] = data[i + 2];
			data[i + 2] = t;
		}
	}
}

/**
 * XGAddress2DTiledOffset: where element (x, y) of a tiled surface sits.
 *
 * Returns an element index, so it is the same function for texels and for
 * 4x4 blocks - only log2bpp changes.
 */
static u32 x360TiledOffset(u32 x, u32 y, u32 width, u32 log2bpp)
{
	const u32 macro = ((x >> 5) + (y >> 5) * ((width + 31) >> 5)) << (log2bpp + 7);
	const u32 micro = ((x & 7) + ((y & 0xe) << 2)) << log2bpp;
	const u32 off = macro + ((micro & ~0xfu) << 1) + (micro & 0xf) + ((y & 1) << 4);

	return ((((off & ~0x1ffu) << 3) + ((y & 16) << 7) + ((off & 0x1c0) << 2) +
			(((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3f)) >> log2bpp);
}

static u32 x360Log2(u32 v)
{
	u32 n = 0;

	while ((1u << n) < v) {
		n++;
	}

	return n;
}

/**
 * Where a surface's own level starts when it shares a tile with its mips.
 *
 * A tile is 32 elements square, and a picture small enough to leave half of
 * one free is stored with its whole mip chain packed in beside it - the
 * console's "packed mip tail". Level 0 does not sit at the tile's origin
 * there: it is 16 elements in, down the tile when the picture is wider than
 * it is tall and across it otherwise, with each smaller level halving that
 * offset in front of it. A picture with both sides past 16 has a tile to
 * itself and starts where it always did.
 *
 * Read from the origin regardless, such a texture comes back as the mip
 * levels stacked in front of it with the picture itself missing - which is
 * what emptied the explosion's colour ramp and every other record under 16
 * on a side (see xbla.md).
 */
static void x360PackedMipOffset(u32 width, u32 height, u32 *outX, u32 *outY)
{
	const u32 logw = x360Log2(width);
	const u32 logh = x360Log2(height);

	*outX = 0;
	*outY = 0;

	if (logw > 4 && logh > 4) {
		return;
	}

	if (logw > logh) {
		*outY = 16;
	} else {
		*outX = 16;
	}
}

s32 x360DecodeTexture(u8 *data, u32 dataLen, const struct x360fetch *fetch, u8 *rgba)
{
	const u32 w = fetch->width;
	const u32 h = fetch->height;
	const u32 bpe = x360BytesPerElement(fetch->format);
	const u32 log2bpp = x360Log2(bpe);
	const u32 stride = w * 4;
	u32 ew;
	u32 eh;
	u32 ox = 0;
	u32 oy = 0;

	if (!x360FetchSupported(fetch)) {
		return 0;
	}

	if (fetch->tiled) {
		x360PackedMipOffset(w, h, &ox, &oy);
	}

	x360EndianSwap(data, dataLen, fetch->endian);

	if (fetch->format == X360_FMT_8888) {
		ew = fetch->pitch > w ? fetch->pitch : w;
		eh = fetch->tiled ? ((h + 31) & ~31u) : h;

		// A tiled surface is a whole number of tiles across whatever its
		// pitch says, which is the span the offset above reaches into and so
		// the span the length check below has to cover.
		if (fetch->tiled) {
			ew = (ew + 31) & ~31u;
		}

		if ((u64)ew * eh * bpe > dataLen) {
			return 0;
		}

		for (u32 y = 0; y < h; y++) {
			for (u32 x = 0; x < w; x++) {
				const u32 e = fetch->tiled
						? x360TiledOffset(x + ox, y + oy, ew, log2bpp)
						: y * ew + x;
				const u8 *src = data + (size_t)e * bpe;
				u8 *dst = rgba + (size_t)y * stride + x * 4;

				// The surface is BGRA on the console.
				dst[0] = src[2];
				dst[1] = src[1];
				dst[2] = src[0];
				dst[3] = src[3];
			}
		}

		return 1;
	}

	const u32 bw = (w + 3) / 4;
	const u32 bh = (h + 3) / 4;
	const s32 kind = x360DxtKind(fetch->format);

	ew = (fetch->pitch > w ? fetch->pitch : w) / 4;
	eh = fetch->tiled ? ((bh + 31) & ~31u) : bh;

	if (fetch->tiled) {
		ew = (ew + 31) & ~31u;
	}

	if ((u64)ew * eh * bpe > dataLen) {
		return 0;
	}

	for (u32 by = 0; by < bh; by++) {
		for (u32 bx = 0; bx < bw; bx++) {
			const u32 e = fetch->tiled
					? x360TiledOffset(bx + ox, by + oy, ew, log2bpp)
					: by * ew + bx;

			dxtBlock(data + (size_t)e * bpe, kind,
					rgba + (size_t)by * 4 * stride + bx * 16, stride,
					(s32)(w - bx * 4), (s32)(h - by * 4));
		}
	}

	return 1;
}
