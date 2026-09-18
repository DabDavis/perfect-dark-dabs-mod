/**
 * GoldenEye's levels as the GoldenEye remake's arenas, converted from the
 * player's own GoldenEye ROM (US): GoldenEye's data cannot be shipped, so GE-X
 * Plus is built on the player's machine from the cartridge they have.
 *
 * The output is a maps-only mod directory the Stage Loader registers
 * (CLAUDE-notes/mods.md "The Stage Loader"): per level files/bgdata/bg_gxNAME.seg,
 * _tilesZ, _padsZ and files/Ump_setupgxNAMEZ, the remake's prop models as
 * files/PgxNNNZ, GoldenEye's textures in textures/, GE-X Plus's menu fonts
 * and strings in menu/, and a modconfig.txt with the `maps` and `models` blocks.
 *
 * This is tools/geconvert/geconvert.py step for step, and that script's
 * comments are the long form of everything here. The two write the same bytes:
 * the offsets the HD tables are generated from (gebeanstagetable.h,
 * geproptable.h) depend on it, so the arithmetic is done in the order numpy
 * does it (a 1-D mean is numpy's pairwise sum, a mean down a column is not,
 * rounding is half to even) and never contracted into fused multiply-adds.
 * Keep the two in step.
 *
 * Built alone with -DGECONVERT_MAIN it is a command line converter
 * (`geconvert ROM OUTDIR`), which is how it is compared against the script.
 */

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")
#endif
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <zlib.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "geconvert.h"
#include "geaitable.h"

#define SEG_BG 0x0f000000u
#define SEG_MODEL 0x05000000u
#define MODEL_REMAKE_FIRST 0x200
#define MOTORBIKE 287

#define US_ROM_SIZE 0xc00000
#define DATA_ROM 0x21990
#define DATA_VRAM 0x80020d90u
#define FILES_AT 0x252c4
#define IMAGES_AT 0x28570
#define IMAGES_ROM 0x8f7df0
#define NUM_IMAGES 2698
#define FOG_AT 0x24080
#define FOG_ROW 92
#define PROPS_AT 0x19498
#define NUM_PROPS 340

// GE-X Plus's menus (gexfront.c): the menu folder (PROP_WALLETBOND), its
// pictures, and the two fonts and the music, raw in the ROM
#define MENU_FOLDER_MODEL 278

// the crosshair cursor (IMAGE_CROSSHAIR1), the film strip's holes (IMAGE_DOT),
// a stage picture for every level (IMAGE_MP_ARCHIVES..TRAIN, TEMPLE..CAVES, RANDOM)
// and the character portraits' tiles (IMAGE_BROSNAN_UL..DALTON_LR,
// BORIS_UL..ODDJOB_LR, RANDOM_UL..LR, MISHKIN)
static const struct { uint32_t first, count; } g_MenuImages[] = {
	{ 2236, 1 }, { 2631, 1 }, { 2578, 20 }, { 2686, 4 }, { 2695, 1 },
	{ 2602, 16 }, { 2632, 40 }, { 2682, 4 }, { 2691, 4 },
};

static const struct { const char *name; size_t at, size; } g_MenuRaw[] = {
	{ "fontbankgothic.bin", 0x2e63f0, 0x24b0 },
	{ "fontzurichbold.bin", 0x2e88a0, 0x3540 },
	// its music: the instrument bank, and the sequence table ({u16 count, pad,
	// then u32 offset, u16 inflated, u16 zipped} a sequence) with the sequences
	{ "instrumentsctl", 0x3b4450, 0x43a0 },
	{ "instrumentstbl", 0x3b87f0, 0x60fa0 },
	{ "sequences", 0x419790, 0x1eed0 },
};

/**
 * The missions' text, in GoldenEye's mission order: each mission's briefing file
 * (front.h's struct BriefStruct - four paragraph text ids then ten objectives of
 * {text id, the difficulty it starts at}) and the level's own text bank, which
 * every id in that file indexes (a text id is bank * 0x400 + slot).
 */
static const struct { const char *brief, *lang; } g_MenuText[] = {
	{ "UbriefdamZ", "LdamE" },        { "UbriefarkZ", "LarkE" },
	{ "UbriefrunZ", "LrunE" },        { "UbriefsevxZ", "LsevxE" },
	{ "UbriefsevbunkerZ", "LsevE" },  { "UbriefsiloZ", "LsiloE" },
	{ "UbriefdestZ", "LdestE" },      { "UbriefsevxbZ", "LsevxbE" },
	{ "UbriefsevbZ", "LsevbE" },      { "UbriefstatueZ", "LstatE" },
	{ "UbriefarchZ", "LarchE" },      { "UbriefpeteZ", "LpeteE" },
	{ "UbriefdepoZ", "LdepoE" },      { "UbrieftraZ", "LtraE" },
	{ "UbriefjunZ", "LjunE" },        { "UbriefcontrolZ", "LarecE" },
	{ "UbriefcaveZ", "LcaveE" },      { "UbriefcradZ", "LcradE" },
	{ "UbriefaztZ", "LaztE" },        { "UbriefcrypZ", "LcrypE" },
};

const char *geconvertMissionLangFile(int mission)
{
	const int n = (int)(sizeof(g_MenuText) / sizeof(g_MenuText[0]));

	return mission >= 0 && mission < n ? g_MenuText[mission].lang : NULL;
}

#define MAX_TEXTURE_SIZE 4096
#define WALL_BELOW 50.0
#define WALL_ABOVE 400.0

/* ------------------------------------------------------------------------ */
/* failure */

static jmp_buf g_Fail;
static char g_FailMsg[256];

static void fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_FailMsg, sizeof(g_FailMsg), fmt, ap);
	va_end(ap);
	longjmp(g_Fail, 1);
}

/* ------------------------------------------------------------------------ */
/* allocations: everything a level makes is let go together, even on failure */

struct alloc {
	size_t slot;
};

static void **g_Allocs;
static size_t g_NumAllocs;
static size_t g_MaxAllocs;

static void *gcRealloc(void *ptr, size_t size)
{
	struct alloc *a = ptr ? (struct alloc *)ptr - 1 : NULL;
	struct alloc *b;

	if (!a) {
		if (g_NumAllocs == g_MaxAllocs) {
			size_t max = g_MaxAllocs ? g_MaxAllocs * 2 : 1024;
			void **allocs = realloc(g_Allocs, max * sizeof(*allocs));
			if (!allocs) {
				fail("out of memory");
			}
			g_Allocs = allocs;
			g_MaxAllocs = max;
		}
	}

	b = realloc(a, sizeof(*b) + (size ? size : 1));

	if (!b) {
		fail("out of memory");
	}

	if (!a) {
		b->slot = g_NumAllocs++;
	}

	g_Allocs[b->slot] = b;
	return b + 1;
}

static void *gcAlloc(size_t size)
{
	void *p = gcRealloc(NULL, size);
	memset(p, 0, size);
	return p;
}

static void gcFreeAll(void)
{
	for (size_t i = 0; i < g_NumAllocs; ++i) {
		free(g_Allocs[i]);
	}

	free(g_Allocs);
	g_Allocs = NULL;
	g_NumAllocs = 0;
	g_MaxAllocs = 0;
}

/* growable arrays of any element */
#define VEC(T) struct { T *v; size_t n, cap; }
#define VECPUSH(vec, x) do { \
	if ((vec).n == (vec).cap) { \
		(vec).cap = (vec).cap ? (vec).cap * 2 : 16; \
		(vec).v = gcRealloc((vec).v, (vec).cap * sizeof(*(vec).v)); \
	} \
	(vec).v[(vec).n++] = (x); \
} while (0)

/* ------------------------------------------------------------------------ */
/* big-endian byte buffers */

typedef VEC(uint8_t) buf;

static uint32_t be32(const uint8_t *b, size_t o) { return (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3]; }
static uint16_t be16(const uint8_t *b, size_t o) { return (uint16_t)(b[o] << 8 | b[o + 1]); }
static int16_t bes16(const uint8_t *b, size_t o) { return (int16_t)be16(b, o); }
static int32_t bes32(const uint8_t *b, size_t o) { return (int32_t)be32(b, o); }
static double bef32(const uint8_t *b, size_t o) { uint32_t u = be32(b, o); float f; memcpy(&f, &u, 4); return f; }

static void set32(uint8_t *b, size_t o, uint32_t v) { b[o] = v >> 24; b[o + 1] = v >> 16; b[o + 2] = v >> 8; b[o + 3] = v; }
static void set16(uint8_t *b, size_t o, uint32_t v) { b[o] = v >> 8; b[o + 1] = v; }
static void setf32(uint8_t *b, size_t o, double v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); set32(b, o, u); }

static void bufGrow(buf *b, size_t n)
{
	if (b->n + n > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
		while (cap < b->n + n) {
			cap *= 2;
		}
		b->v = gcRealloc(b->v, cap);
		b->cap = cap;
	}
}

static void bufPut(buf *b, const uint8_t *data, size_t n)
{
	bufGrow(b, n);
	if (n) {
		memcpy(b->v + b->n, data, n);
	}
	b->n += n;
}

static void bufZeros(buf *b, size_t n)
{
	bufGrow(b, n);
	memset(b->v + b->n, 0, n);
	b->n += n;
}

static void bufU8(buf *b, uint32_t v) { uint8_t x = v; bufPut(b, &x, 1); }
static void bufU16(buf *b, uint32_t v) { bufZeros(b, 2); set16(b->v, b->n - 2, v); }
static void bufU32(buf *b, uint32_t v) { bufZeros(b, 4); set32(b->v, b->n - 4, v); }
static void bufF32(buf *b, double v) { bufZeros(b, 4); setf32(b->v, b->n - 4, v); }
static void bufPad(buf *b, size_t align) { while (b->n % align) { bufU8(b, 0); } }

/* ------------------------------------------------------------------------ */
/* numbers as Python and numpy have them */

// int(round(v)) and np.round: half to even
static double rnd(double v)
{
	return nearbyint(v);
}

static int32_t s16(double v)
{
	double r = rnd(v);
	if (r < -32768 || r > 32767) {
		fail("a coordinate does not fit in 16 bits (%.1f)", v);
	}
	return (int32_t)r;
}

// numpy's pairwise sum, which a 1-D np.mean is
static double pairwise(const double *a, size_t n)
{
	if (n < 8) {
		double res = 0.;
		for (size_t i = 0; i < n; ++i) {
			res += a[i];
		}
		return res;
	} else if (n <= 128) {
		double r[8], res;
		size_t i;
		for (i = 0; i < 8; ++i) {
			r[i] = a[i];
		}
		for (i = 8; i < n - (n % 8); i += 8) {
			for (size_t j = 0; j < 8; ++j) {
				r[j] += a[i + j];
			}
		}
		res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
		for (; i < n; ++i) {
			res += a[i];
		}
		return res;
	} else {
		size_t n2 = n / 2;
		n2 -= n2 % 8;
		return pairwise(a, n2) + pairwise(a + n2, n - n2);
	}
}

static double mean1d(const double *a, size_t n)
{
	return pairwise(a, n) / (double)n;
}

// np.linalg.norm of a short vector: BLAS ddot, in order
static double norm3(const double *v)
{
	return sqrt(0. + v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static double dot3(const double *a, const double *b)
{
	return 0. + a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const double *a, const double *b, double *out)
{
	double r[3];
	r[0] = a[1] * b[2] - a[2] * b[1];
	r[1] = a[2] * b[0] - a[0] * b[2];
	r[2] = a[0] * b[1] - a[1] * b[0];
	memcpy(out, r, sizeof(r));
}

// Python's // on ints
static int64_t floordiv(int64_t a, int64_t b)
{
	int64_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) {
		--q;
	}
	return q;
}

/* ------------------------------------------------------------------------ */
/* compression */

static buf inflateRaw(const uint8_t *src, size_t len)
{
	buf out = {0};
	z_stream zs;
	int ret;

	memset(&zs, 0, sizeof(zs));

	if (inflateInit2(&zs, -15) != Z_OK) {
		fail("inflateInit2");
	}

	zs.next_in = (Bytef *)src;
	zs.avail_in = (uInt)len;

	do {
		bufGrow(&out, 65536);
		zs.next_out = out.v + out.n;
		zs.avail_out = (uInt)(out.cap - out.n);
		ret = inflate(&zs, Z_NO_FLUSH);
		out.n = out.cap - zs.avail_out;
		if (ret != Z_OK && ret != Z_STREAM_END) {
			inflateEnd(&zs);
			fail("a compressed block does not inflate (%d)", ret);
		}
		if (ret == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) {
			break; // the input ran out before the end marker, as zlib.decompressobj allows
		}
	} while (ret != Z_STREAM_END);

	inflateEnd(&zs);
	return out;
}

static buf inflate1172(const uint8_t *src, size_t len)
{
	if (len < 2 || src[0] != 0x11 || src[1] != 0x72) {
		fail("expected a 1172 block");
	}
	return inflateRaw(src + 2, len - 2);
}

static buf rzip1173(const uint8_t *data, size_t n)
{
	buf out = {0};
	z_stream zs;
	uLong bound;

	memset(&zs, 0, sizeof(zs));

	if (deflateInit2(&zs, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		fail("deflateInit2");
	}

	bound = deflateBound(&zs, (uLong)n);
	bufZeros(&out, 5 + bound);
	out.v[0] = 0x11;
	out.v[1] = 0x73;
	out.v[2] = (n >> 16) & 0xff;
	out.v[3] = (n >> 8) & 0xff;
	out.v[4] = n & 0xff;
	zs.next_in = (Bytef *)data;
	zs.avail_in = (uInt)n;
	zs.next_out = out.v + 5;
	zs.avail_out = (uInt)bound;

	if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
		deflateEnd(&zs);
		fail("deflate");
	}

	out.n = 5 + zs.total_out;
	deflateEnd(&zs);
	return out;
}

/* ------------------------------------------------------------------------ */
/* the ROM */

struct romfile {
	char stem[64];
	uint32_t addr;
	uint32_t size;
};

static uint8_t *g_Rom;
static size_t g_RomLen;
static uint8_t *g_Data;
static size_t g_DataLen;
static struct romfile *g_Files;
static size_t g_NumFiles;

struct prop {
	const char *file;
	double scale;
	int32_t numswitches, nummatrices, numtextures;
	double radius;
};

static struct prop g_Props[NUM_PROPS];

static const char *dataString(uint32_t ptr)
{
	size_t o = ptr - DATA_VRAM;
	if (ptr < DATA_VRAM || o >= g_DataLen || !memchr(g_Data + o, 0, g_DataLen - o)) {
		fail("a name outside the data segment");
	}
	return (const char *)g_Data + o;
}

// the ROM in .z64 order, whatever order it was dumped in
static int romToZ64(uint8_t *rom, size_t len)
{
	if (len < 4 || len % 4) {
		return 0;
	}

	if (rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40) {
		return 1;
	}

	if (rom[0] == 0x37 && rom[1] == 0x80 && rom[2] == 0x40 && rom[3] == 0x12) {
		for (size_t i = 0; i < len; i += 2) {
			uint8_t t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t;
		}
		return 1;
	}

	if (rom[0] == 0x40 && rom[1] == 0x12 && rom[2] == 0x37 && rom[3] == 0x80) {
		for (size_t i = 0; i < len; i += 4) {
			uint8_t t = rom[i]; rom[i] = rom[i + 3]; rom[i + 3] = t;
			t = rom[i + 1]; rom[i + 1] = rom[i + 2]; rom[i + 2] = t;
		}
		return 1;
	}

	return 0;
}

static int headerIsUs(const uint8_t *h)
{
	// the header's name and game code, then its CRCs, which cover the code
	static const uint8_t crc[8] = { 0xdc, 0xbc, 0x50, 0xd1, 0x09, 0xfd, 0x1a, 0xa3 };

	return !memcmp(h + 0x20, "GOLDENEYE", 9)
		&& !memcmp(h + 0x3b, "NGEE", 4)
		&& !memcmp(h + 0x10, crc, sizeof(crc));
}

int geconvertHeaderIsGoldenEyeUs(const uint8_t *head, size_t len)
{
	uint8_t h[0x40];

	if (len < sizeof(h)) {
		return 0;
	}

	memcpy(h, head, sizeof(h));
	return romToZ64(h, sizeof(h)) && headerIsUs(h);
}

int geconvertIsGoldenEyeUs(uint8_t *rom, size_t len)
{
	return len == US_ROM_SIZE && romToZ64(rom, len) && headerIsUs(rom);
}

static const struct romfile *romFind(const char *stem)
{
	for (size_t i = 0; i < g_NumFiles; ++i) {
		if (!strcmp(g_Files[i].stem, stem)) {
			return &g_Files[i];
		}
	}
	fail("%s is not in the ROM", stem);
	return NULL;
}

// a file by name: stored bytes for a bg file, inflated for the rest
static buf romFile(const char *stem)
{
	const struct romfile *f = romFind(stem);

	if (f->addr + (size_t)f->size > g_RomLen) {
		fail("%s runs off the ROM", stem);
	}

	if (!strncmp(stem, "bg_", 3)) {
		buf out = {0};
		bufPut(&out, g_Rom + f->addr, f->size);
		return out;
	}

	return inflate1172(g_Rom + f->addr, f->size);
}

static int romOpen(void)
{
	buf data = inflate1172(g_Rom + DATA_ROM, g_RomLen - DATA_ROM);
	size_t rows = 0;

	g_Data = data.v;
	g_DataLen = data.n;

	while (FILES_AT + 12 * (rows + 1) <= g_DataLen && be32(g_Data, FILES_AT + 12 * rows) == rows) {
		++rows;
	}

	if (rows < 3) {
		fail("no file table");
	}

	// a file's size is the distance to the next row's address; the last row ends the table
	g_NumFiles = rows - 2;
	g_Files = gcAlloc(g_NumFiles * sizeof(*g_Files));

	for (size_t k = 1; k + 1 < rows; ++k) {
		const char *name = dataString(be32(g_Data, FILES_AT + 12 * k + 4));
		const char *slash = strrchr(name, '/');
		struct romfile *f = &g_Files[k - 1];
		char *dot;

		snprintf(f->stem, sizeof(f->stem), "%s", slash ? slash + 1 : name);
		dot = strrchr(f->stem, '.');
		if (dot) {
			*dot = '\0';
		}
		f->addr = be32(g_Data, FILES_AT + 12 * k + 8);
		f->size = be32(g_Data, FILES_AT + 12 * (k + 1) + 8) - f->addr;
	}

	for (size_t k = 0; k < NUM_PROPS; ++k) {
		const size_t o = PROPS_AT + 12 * k;
		const size_t h = be32(g_Data, o) - DATA_VRAM;
		struct prop *p = &g_Props[k];

		if (h + 0x18 > g_DataLen) {
			fail("prop %d's header is outside the data segment", (int)k);
		}

		p->file = dataString(be32(g_Data, o + 4));
		p->scale = bef32(g_Data, o + 8);
		p->numswitches = bes16(g_Data, h + 12);
		p->nummatrices = bes16(g_Data, h + 14);
		p->radius = bef32(g_Data, h + 16);
		p->numtextures = bes16(g_Data, h + 22);
	}

	return 1;
}

static const uint8_t *romImage(int32_t num, size_t *len)
{
	size_t at = IMAGES_ROM;

	if (num < 0 || num >= NUM_IMAGES) {
		return NULL;
	}

	for (int32_t k = 0; k < num; ++k) {
		at += be32(g_Data, IMAGES_AT + 8 * k) & 0xffffff;
	}

	*len = be32(g_Data, IMAGES_AT + 8 * num) & 0xffffff;
	return at + *len <= g_RomLen ? g_Rom + at : NULL;
}

// a level's one-player fog row (the 30 values after the id, as bgfog.c lists them)
static int romFogRow(uint32_t levelid, double *r)
{
	int found = 0;

	for (size_t o = FOG_AT; o + FOG_ROW <= g_DataLen; o += FOG_ROW) {
		const uint32_t lid = be32(g_Data, o);
		const uint8_t *d = g_Data + o + 4;

		if ((lid == 0 && o > FOG_AT) || lid >= 0x10000) {
			break;
		}

		if (lid != levelid) {
			continue;
		}

		for (int i = 0; i < 6; ++i) r[i] = bef32(d, 4 * i);
		for (int i = 0; i < 3; ++i) r[6 + i] = be32(d, 24 + 4 * i);
		for (int i = 0; i < 4; ++i) r[9 + i] = d[36 + i];
		r[13] = bef32(d, 40);
		r[14] = be16(d, 44);
		r[15] = be16(d, 46);
		for (int i = 0; i < 3; ++i) r[16 + i] = bef32(d, 48 + 4 * i);
		r[19] = d[60];
		r[20] = r[21] = r[22] = 0;
		r[23] = bef32(d, 64);
		r[24] = be16(d, 68);
		r[25] = be16(d, 70);
		for (int i = 0; i < 4; ++i) r[26 + i] = bef32(d, 72 + 4 * i);
		found = 1;
	}

	return found;
}

/* ------------------------------------------------------------------------ */
/* texture numbers: GoldenEye's images that collide with the numbers Perfect
 * Dark's texture config tables load, moved past GoldenEye's last (texremap.py) */

static const uint16_t g_TexRemap[][2] = {
	{1, 2698}, {2, 2699}, {3, 2700}, {4, 2701}, {5, 2702}, {6, 2703}, {7, 2704}, {8, 2705}, {9, 2706}, {10, 2707},
	{11, 2708}, {12, 2709}, {13, 2710}, {14, 2711}, {15, 2712}, {16, 2713}, {17, 2715}, {18, 2716}, {19, 2717},
	{20, 2718}, {21, 2719}, {22, 2720}, {23, 2721}, {24, 2722}, {25, 2723}, {26, 2724}, {27, 2725}, {28, 2726},
	{29, 2727}, {30, 2728}, {31, 2729}, {32, 2730}, {33, 2731}, {34, 2732}, {35, 2733}, {36, 2734}, {37, 2735},
	{38, 2736}, {39, 2737}, {40, 2738}, {41, 2739}, {42, 2740}, {43, 2741}, {44, 2742}, {45, 2743}, {46, 2744},
	{47, 2745}, {48, 2746}, {49, 2747}, {50, 2748}, {51, 2749}, {52, 2750}, {53, 2751}, {54, 2752}, {55, 2753},
	{56, 2754}, {57, 2755}, {58, 2756}, {59, 2757}, {60, 2758}, {64, 2759}, {122, 2760}, {128, 2761}, {274, 2762},
	{485, 2763}, {901, 2764}, {1546, 2765}, {1559, 2766}, {1560, 2767}, {1561, 2768}, {1562, 2769}, {1563, 2770},
	{1564, 2771}, {1565, 2772}, {1566, 2773}, {1567, 2774}, {1568, 2775}, {1569, 2776}, {1570, 2777}, {1571, 2778},
	{1572, 2779}, {1573, 2780}, {1574, 2781}, {1595, 2782}, {1596, 2783}, {2126, 2784}, {2132, 2785}, {2133, 2786},
	{2134, 2787}, {2136, 2788}, {2137, 2789}, {2138, 2790}, {2288, 2791}, {2292, 2792}, {2714, 2793}, {2895, 2794},
	{2896, 2795}, {2897, 2796}, {2898, 2797}, {2899, 2798}, {2969, 2799}, {3111, 2800}, {3112, 2801}, {3121, 2802},
	{3122, 2803}, {3123, 2804}, {3124, 2805}, {3125, 2806}, {3126, 2807}, {3127, 2808}, {3128, 2809}, {3129, 2810},
	{3130, 2811}, {3131, 2812}, {3132, 2813}, {3133, 2814}, {3134, 2815}, {3135, 2816}, {3136, 2817}, {3137, 2818},
	{3138, 2819}, {3139, 2820}, {3141, 2821}, {3142, 2822}, {3143, 2823}, {3144, 2824}, {3145, 2825}, {3146, 2826},
	{3147, 2827}, {3148, 2828}, {3149, 2829}, {3150, 2830}, {3151, 2831}, {3152, 2832}, {3153, 2833}, {3154, 2834},
	{3155, 2835}, {3156, 2836}, {3157, 2837}, {3158, 2838}, {3159, 2839}, {3160, 2840}, {3161, 2841}, {3162, 2842},
	{3163, 2843}, {3164, 2844}, {3165, 2845}, {3166, 2846}, {3167, 2847}, {3168, 2848}, {3169, 2849}, {3170, 2850},
	{3171, 2851}, {3172, 2852}, {3173, 2853}, {3174, 2854}, {3175, 2855}, {3176, 2856}, {3177, 2857}, {3178, 2858},
	{3179, 2859}, {3180, 2860}, {3181, 2861}, {3182, 2862}, {3183, 2863}, {3184, 2864}, {3185, 2865}, {3186, 2866},
	{3187, 2867}, {3188, 2868}, {3189, 2869}, {3190, 2870}, {3191, 2871}, {3192, 2872}, {3193, 2873}, {3194, 2874},
	{3195, 2875}, {3196, 2876}, {3197, 2877}, {3198, 2878}, {3199, 2879}, {3200, 2880}, {3201, 2881}, {3202, 2882},
	{3203, 2883}, {3204, 2884}, {3205, 2885}, {3206, 2886}, {3207, 2887}, {3208, 2888}, {3209, 2889}, {3210, 2890},
	{3211, 2891}, {3212, 2892}, {3213, 2893}, {3214, 2894}, {3215, 2900}, {3216, 2901}, {3217, 2902}, {3218, 2903},
	{3219, 2904}, {3220, 2905}, {3221, 2906}, {3222, 2907}, {3223, 2908}, {3224, 2909}, {3225, 2910}, {3226, 2911},
	{3227, 2912}, {3228, 2913}, {3229, 2914}, {3230, 2915}, {3231, 2916}, {3232, 2917}, {3233, 2918}, {3234, 2919},
	{3235, 2920}, {3247, 2921}, {3442, 2922}, {3444, 2923}, {3488, 2924}, {3489, 2925}, {3490, 2926}, {3491, 2927},
	{3492, 2928}, {3493, 2929},
};

static uint32_t texRemap(uint32_t image)
{
	for (size_t i = 0; i < sizeof(g_TexRemap) / sizeof(g_TexRemap[0]); ++i) {
		if (g_TexRemap[i][0] == image) {
			return g_TexRemap[i][1];
		}
	}
	return image;
}

// sets of small numbers (texture and model numbers)
#define SETBITS 65536
typedef uint8_t numset[SETBITS / 8];
static void setAdd(uint8_t *s, uint32_t n) { if (n < SETBITS) s[n >> 3] |= 1 << (n & 7); }
static int setHas(const uint8_t *s, uint32_t n) { return n < SETBITS && (s[n >> 3] >> (n & 7)) & 1; }

/* ------------------------------------------------------------------------ */
/* the levels (gefiles.py LEVELS, geconvert.py LEVELIDS, NAMES, BIKES) */

struct level {
	const char *key;
	const char *bg;
	const char *stan;
	const char *solo;
	const char *mp;
	double levelscale;
	uint32_t levelid;
	const char *name;
	int bikes;
};

static const struct level g_Levels[] = {
	{ "dam",   "bg_dam",  "Tbg_dam",  "UsetupdamZ",       NULL,               0.23363999,  33, "Dam",         0 },
	{ "run",   "bg_run",  "Tbg_run",  "UsetuprunZ",       NULL,               0.089571431, 35, "Runway",      2 },
	{ "stat",  "bg_stat", "Tbg_stat", "UsetupstatueZ",    "Ump_setupstatueZ", 0.107202865, 22, "Statue Park", 0 },
	{ "tra",   "bg_tra",  "Tbg_tra",  "UsetuptraZ",       NULL,               0.15019713,  25, "Train",       0 },
	{ "pete",  "bg_pete", "Tbg_pete", "UsetuppeteZ",      NULL,               0.34187999,  29, "Streets",     0 },
	{ "jun",   "bg_jun",  "Tbg_jun",  "UsetupjunZ",       NULL,               0.094662853, 37, "Jungle",      0 },
	{ "oat",   "bg_oat",  "Tbg_oat",  NULL,               "Ump_setupoatZ",    0.14142857,  50, "Caves",       0 },
	{ "dish",  "bg_dish", "Tbg_dish", NULL,               "Ump_setupdishZ",   0.47142857,  38, "Temple",      0 },
	{ "ref",   "bg_ref",  "Tbg_ref",  NULL,               "Ump_setuprefZ",    0.94285715,  31, "Complex",     0 },
	{ "lib",   "bg_ame",  "Tbg_ame",  NULL,               "Ump_setupameZ",    0.65999997,  48, "Library",     0 },
	{ "base",  "bg_ame",  "Tbg_ame",  NULL,               "Ump_setupimpZ",    0.65999997,  45, "Basement",    0 },
	{ "stack", "bg_ame",  "Tbg_ame",  NULL,               "Ump_setupashZ",    0.65999997,  46, "Stack",       0 },
	{ "ark",   "bg_ark",  "Tbg_ark",  NULL,               "Ump_setuparkZ",    1.20648,     34, "Facility",    0 },
	{ "sevb",  "bg_sevb", "Tbg_sevb", NULL,               "Ump_setupsevbZ",   0.53931433,  27, "Bunker",      0 },
	{ "arch",  "bg_arch", "Tbg_arch", NULL,               "Ump_setuparchZ",   0.50678575,  24, "Archives",    0 },
	{ "cave",  "bg_cave", "Tbg_cave", NULL,               "Ump_setupcaveZ",   0.26824287,  39, "Caverns",     0 },
	{ "cryp",  "bg_cryp", "Tbg_cryp", NULL,               "Ump_setupcrypZ",   0.25608,     32, "Egyptian",    0 },
	{ "crad",  "bg_crad", "Tbg_crad", NULL,               "Ump_setupcradZ",   0.23571429,  41, "Cradle",      0 },
	{ "sevx",  "bg_sevx", "Tbg_sevx", "UsetupsevxZ",      NULL,               0.45445713,  36, "Surface",     0 },
	{ "sevxb", "bg_sevx", "Tbg_sevx", "UsetupsevxbZ",     NULL,               0.45445713,  43, "Surface 2",   0 },
	{ "silo",  "bg_silo", "Tbg_silo", "UsetupsiloZ",      NULL,               0.47256002,  20, "Silo",        0 },
	{ "dest",  "bg_dest", "Tbg_dest", "UsetupdestZ",      NULL,               0.44757429,  26, "Frigate",     0 },
	{ "depo",  "bg_depo", "Tbg_depo", "UsetupdepoZ",      NULL,               0.21847887,  30, "Depot",       0 },
	{ "arec",  "bg_arec", "Tbg_arec", "UsetupcontrolZ",   NULL,               0.49886572,  23, "Control",     0 },
	{ "sev",   "bg_sev",  "Tbg_sev",  "UsetupsevbunkerZ", NULL,               0.53931433,  9,  "Bunker 1",    0 },
	{ "azt",   "bg_azt",  "Tbg_azt",  "UsetupaztZ",       NULL,               0.35300568,  28, "Aztec",       0 },
};

#define NUM_LEVELS (sizeof(g_Levels) / sizeof(g_Levels[0]))

/* ------------------------------------------------------------------------ */
/* bg files (gefiles.py Bg) */

struct bgroom {
	double pos[3];
	buf vtx, pri, sec; // n == 0 and v == NULL when the room has none
	int hasvtx, haspri, hassec;
};

struct portal {
	int npts;
	double (*pts)[3];
	int room1, room2;
};

struct bg {
	int numrooms;
	struct bgroom *rooms;
	VEC(struct portal) portals;
};

static uint32_t segoff(uint32_t a)
{
	return a & 0xffffff;
}

static int cmpU32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static void bgRead(const buf *file, struct bg *bg)
{
	const uint8_t *d = file->v;
	const size_t len = file->n;
	uint32_t roomsat = segoff(be32(d, 4));
	uint32_t portalsat = segoff(be32(d, 8));
	VEC(uint32_t) entries = {0};
	uint32_t *offs;
	size_t noffs = 0, numentries;

	memset(bg, 0, sizeof(*bg));

	for (size_t i = 0;; ++i) {
		size_t o = roomsat + 24 * i;
		if (o + 24 > len) {
			fail("the room table runs off the bg file");
		}
		VECPUSH(entries, (uint32_t)o);
		if (i > 0 && be32(d, o + 4) == 0) {
			break;
		}
	}

	numentries = entries.n;
	offs = gcAlloc(3 * numentries * sizeof(*offs));

	for (size_t i = 0; i < numentries; ++i) {
		for (int k = 0; k < 3; ++k) {
			uint32_t x = be32(d, entries.v[i] + 4 * k);
			if (x) {
				offs[noffs++] = segoff(x);
			}
		}
	}

	qsort(offs, noffs, sizeof(*offs), cmpU32);

	bg->numrooms = (int)numentries - 3;
	if (bg->numrooms < 0) {
		fail("a bg file with no rooms");
	}
	bg->rooms = gcAlloc((bg->numrooms + 1) * sizeof(*bg->rooms));

	for (int r = 0; r < bg->numrooms; ++r) {
		const size_t o = entries.v[r + 1];
		struct bgroom *room = &bg->rooms[r];
		buf *blobs[3] = { &room->vtx, &room->pri, &room->sec };
		int *has[3] = { &room->hasvtx, &room->haspri, &room->hassec };

		for (int k = 0; k < 3; ++k) {
			const uint32_t addr = be32(d, o + 4 * k);
			uint32_t a, end = (uint32_t)len;

			if (!addr) {
				continue;
			}

			a = segoff(addr);
			for (size_t j = 0; j < noffs; ++j) {
				if (offs[j] > a) {
					end = offs[j];
					break;
				}
			}
			if (a >= len || end > len || end < a) {
				fail("a room's data is outside the bg file");
			}
			*blobs[k] = inflate1172(d + a, end - a);
			*has[k] = blobs[k]->n > 0;
		}

		for (int k = 0; k < 3; ++k) {
			room->pos[k] = bef32(d, o + 12 + 4 * k);
		}
	}

	for (size_t o = portalsat; o + 8 <= len && be32(d, o); o += 8) {
		struct portal p;
		const uint32_t at = segoff(be32(d, o));

		if (at + 4 > len) {
			fail("a portal outside the bg file");
		}

		p.npts = d[at];
		p.pts = gcAlloc((p.npts + 1) * sizeof(*p.pts));
		for (int k = 0; k < p.npts; ++k) {
			for (int c = 0; c < 3; ++c) {
				p.pts[k][c] = bef32(d, at + 4 + 12 * k + 4 * c);
			}
		}
		p.room1 = d[o + 4];
		p.room2 = d[o + 5];
		VECPUSH(bg->portals, p);
	}
}

/* ------------------------------------------------------------------------ */
/* stan (read_stan) */

struct tile {
	int room;
	int special;
	int npts;
	int16_t pts[15][3];
	uint16_t link[15];
};

typedef VEC(struct tile) tiles;

static tiles stanRead(const buf *file)
{
	tiles out = {0};
	const uint8_t *d = file->v;
	size_t o = be32(d, 4);

	while (o + 8 <= file->n) {
		struct tile t;
		static const uint8_t zero[8];

		if (!memcmp(d + o, zero, 8)) {
			break;
		}

		t.room = d[o + 3];
		t.special = be16(d, o + 4) >> 12;
		t.npts = be16(d, o + 6) >> 12;

		if (o + 8 + 8 * t.npts > file->n) {
			fail("a tile runs off the stan file");
		}

		for (int k = 0; k < t.npts; ++k) {
			for (int c = 0; c < 3; ++c) {
				t.pts[k][c] = bes16(d, o + 8 + 8 * k + 2 * c);
			}
			t.link[k] = be16(d, o + 8 + 8 * k + 6);
		}

		VECPUSH(out, t);
		o += 8 + 8 * t.npts;
	}

	return out;
}

/* ------------------------------------------------------------------------ */
/* rooms (convert_list, scaled_room, fixture_lights, write_room, write_bg) */

// GoldenEye's light textures (lightfixture.c check_if_imageID_is_light())
static int isLightImage(uint32_t image)
{
	static const uint32_t lights[] = { 201, 203, 205, 252, 254, 255, 256, 428, 982, 1383 };
	for (size_t i = 0; i < sizeof(lights) / sizeof(lights[0]); ++i) {
		if (lights[i] == image) {
			return 1;
		}
	}
	return 0;
}

struct outvtx {
	int16_t x, y, z;
	uint8_t c;
	int16_t s, t;
};

typedef VEC(struct outvtx) outvtxs;
typedef VEC(uint32_t) u32s;
typedef VEC(int32_t) s32s;
struct tri { int32_t p[3][3]; };
typedef VEC(struct tri) tris;

static void convertList(const buf *dl, const buf *vtx, outvtxs *outv, u32s *outc, size_t basevtx,
		uint8_t *textures, tris *lighttris, u32s *words)
{
	int32_t slots[32];
	int64_t curtex = -1;

	for (int i = 0; i < 32; ++i) {
		slots[i] = -1;
	}

	for (size_t o = 0; o + 8 <= dl->n; o += 8) {
		uint32_t w0 = be32(dl->v, o), w1 = be32(dl->v, o + 4);
		const uint32_t op = w0 >> 24;

		if (op == 0x04) {
			const uint32_t n = ((w0 >> 20) & 0xf) + 1;
			const uint32_t v0 = (w0 >> 16) & 0xf;
			const uint32_t src = (w1 & 0xffffff) / 16;
			const uint32_t start = (uint32_t)(outv->n - basevtx);

			for (uint32_t i = 0; i < n; ++i) {
				const size_t at = 16 * (size_t)(src + i);
				struct outvtx v;

				if (at + 16 > vtx->n) {
					fail("a list loads a vertex the room does not have");
				}

				v.x = bes16(vtx->v, at);
				v.y = bes16(vtx->v, at + 2);
				v.z = bes16(vtx->v, at + 4);
				v.c = (uint8_t)(i << 2);
				v.s = bes16(vtx->v, at + 8);
				v.t = bes16(vtx->v, at + 10);
				slots[v0 + i] = (int32_t)outv->n;
				VECPUSH(*outv, v);
				VECPUSH(*outc, be32(vtx->v, at + 12));
			}

			VECPUSH(*words, (0x07u << 24) | ((((n - 1) << 2) & 0xff) << 16) | (n * 4));
			VECPUSH(*words, 0x0d000000u | (start * 4));
			VECPUSH(*words, (0x04u << 24) | ((n - 1) << 20) | (v0 << 16) | (n * 12));
			VECPUSH(*words, 0x0e000000u | (start * 12));
		} else if (op == 0xbf) {
			const uint32_t a = ((w1 >> 16) & 0xff) / 10, b = ((w1 >> 8) & 0xff) / 10, c = (w1 & 0xff) / 10;
			const uint32_t k[3] = { a, b, c };

			VECPUSH(*words, (0xb1u << 24) | c);
			VECPUSH(*words, (b << 4) | a);

			if (lighttris && curtex >= 0 && isLightImage((uint32_t)curtex)
					&& a < 32 && b < 32 && c < 32 && slots[a] >= 0 && slots[b] >= 0 && slots[c] >= 0) {
				struct tri t;
				for (int j = 0; j < 3; ++j) {
					const struct outvtx *v = &outv->v[slots[k[j]]];
					t.p[j][0] = v->x;
					t.p[j][1] = v->y;
					t.p[j][2] = v->z;
				}
				VECPUSH(*lighttris, t);
			}
		} else if (op == 0xb8) {
			VECPUSH(*words, w0);
			VECPUSH(*words, w1);
			break;
		} else {
			if (op == 0xc0) {
				curtex = w1 & 0xfff;
				setAdd(textures, w1 & 0xfff);
				w1 = (w1 & ~0xfffu) | texRemap(w1 & 0xfff);
				if ((w0 & 7) == 1) {
					setAdd(textures, (w1 >> 12) & 0xfff);
					w1 = (w1 & ~0xfff000u) | (texRemap((w1 >> 12) & 0xfff) << 12);
				}
			}
			VECPUSH(*words, w0);
			VECPUSH(*words, w1);
		}
	}
}

static void loadedVertices(const buf *dl, uint8_t *seen, size_t nseen, int *any)
{
	for (size_t o = 0; o + 8 <= dl->n; o += 8) {
		const uint32_t w0 = be32(dl->v, o), w1 = be32(dl->v, o + 4);

		if (w0 >> 24 == 0x04) {
			const uint32_t src = (w1 & 0xffffff) / 16;
			const uint32_t n = ((w0 >> 20) & 0xf) + 1;
			for (uint32_t k = src; k < src + n; ++k) {
				*any = 1;
				if (k < nseen) {
					seen[k] = 1;
				}
			}
		} else if (w0 >> 24 == 0xb8) {
			break;
		}
	}
}

// the room's vertices moved into its own frame, and that frame's middle. A
// room that draws nothing (Streets files thirty-five of them) has no vertices
// to sit among, so it takes the middle of its own tiles: GoldenEye's own room
// position for those is the level origin, which is nowhere near them.
static buf scaledRoom(const struct bgroom *room, double inv, const double *offset, double *centre,
		const int32_t *tilebox)
{
	const size_t n = room->hasvtx ? room->vtx.n / 16 : 0;
	buf newvtx = {0};

	if (room->hasvtx) {
		bufPut(&newvtx, room->vtx.v, room->vtx.n);
	}

	if (n) {
		double *world = gcAlloc(n * 3 * sizeof(double));
		uint8_t *seen = gcAlloc(n);
		double mn[3], mx[3];
		int any = 0, first = 1;

		for (size_t k = 0; k < n; ++k) {
			for (int c = 0; c < 3; ++c) {
				world[3 * k + c] = ((double)bes16(room->vtx.v, 16 * k + 2 * c) + room->pos[c]) * inv - offset[c];
			}
		}

		if (room->haspri) {
			loadedVertices(&room->pri, seen, n, &any);
		}
		if (room->hassec) {
			loadedVertices(&room->sec, seen, n, &any);
		}

		for (size_t k = 0; k < n; ++k) {
			if (any && !seen[k]) {
				continue;
			}
			for (int c = 0; c < 3; ++c) {
				const double v = world[3 * k + c];
				if (first || v < mn[c]) mn[c] = v;
				if (first || v > mx[c]) mx[c] = v;
			}
			first = 0;
		}

		if (first) {
			fail("a room loads no vertex it has");
		}

		for (int c = 0; c < 3; ++c) {
			centre[c] = rnd((mn[c] + mx[c]) / 2);
		}

		for (size_t k = 0; k < n; ++k) {
			for (int c = 0; c < 3; ++c) {
				const double rel = world[3 * k + c] - centre[c];
				if (fabs(rel) > 32767) {
					fail("a room spans more than 16 bits");
				}
				set16(newvtx.v, 16 * k + 2 * c, (uint32_t)s16(rel));
			}
		}
	} else if (tilebox) {
		for (int c = 0; c < 3; ++c) {
			centre[c] = rnd((tilebox[c] + tilebox[3 + c]) / 2.0);
		}
	} else {
		for (int c = 0; c < 3; ++c) {
			centre[c] = rnd(room->pos[c] * inv - offset[c]);
		}
	}

	return newvtx;
}

struct light {
	int32_t corners[4][3];
	int32_t dir[3];
};

typedef VEC(struct light) lights;

static lights fixtureLights(const tris *t)
{
	lights out = {0};
	const size_t n = t->n;
	size_t *parent, *owner;
	double *pts;

	if (!n) {
		return out;
	}

	parent = gcAlloc(n * sizeof(*parent));
	owner = gcAlloc(3 * n * sizeof(*owner));

	for (size_t i = 0; i < n; ++i) {
		parent[i] = i;
	}

#define FIND(x) ({ size_t _i = (x); while (parent[_i] != _i) { parent[_i] = parent[parent[_i]]; _i = parent[_i]; } _i; })

	// owner: the first triangle to have each corner
	for (size_t i = 0; i < n; ++i) {
		for (int k = 0; k < 3; ++k) {
			size_t j = i;
			for (size_t q = 0; q < i * 3 + k; ++q) {
				if (!memcmp(t->v[q / 3].p[q % 3], t->v[i].p[k], sizeof(t->v[i].p[k]))) {
					j = owner[q];
					break;
				}
			}
			owner[3 * i + k] = j;
			{
				size_t ri = FIND(i), rj = FIND(j);
				parent[ri] = rj;
			}
		}
	}

	pts = gcAlloc(3 * n * 3 * sizeof(double));

	// the groups, in the order their first member comes
	for (size_t g = 0; g < n; ++g) {
		size_t root = FIND(g), m = 0;
		double normal[3] = { 0, 0, 0 }, nrm[3], middle[3] = { 0, 0, 0 }, glare[3], u[3], v[3], up[3];
		double *pu, *pv, *pn, len, d, umin, umax, vmin, vmax;
		int firstofgroup = 1;
		size_t count = 0;
		struct light l;

		for (size_t h = 0; h < g; ++h) {
			if (FIND(h) == root) {
				firstofgroup = 0;
				break;
			}
		}
		if (!firstofgroup) {
			continue;
		}

		for (size_t i = g; i < n; ++i) {
			double a[3], b[3], c[3];
			if (FIND(i) != root) {
				continue;
			}
			for (int k = 0; k < 3; ++k) {
				a[k] = (double)t->v[i].p[1][k] - (double)t->v[i].p[0][k];
				b[k] = (double)t->v[i].p[2][k] - (double)t->v[i].p[0][k];
			}
			cross3(a, b, c);
			for (int k = 0; k < 3; ++k) {
				normal[k] = normal[k] + c[k];
			}
			for (int j = 0; j < 3; ++j) {
				for (int k = 0; k < 3; ++k) {
					pts[3 * m + k] = t->v[i].p[j][k];
				}
				++m;
			}
			++count;
		}
		(void)count;

		len = norm3(normal);
		if (len < 1e-6) {
			continue;
		}

		for (int k = 0; k < 3; ++k) {
			nrm[k] = normal[k] / len;
		}

		// a mean down a column is summed in order
		for (size_t i = 0; i < m; ++i) {
			for (int k = 0; k < 3; ++k) {
				middle[k] += pts[3 * i + k];
			}
		}
		for (int k = 0; k < 3; ++k) {
			middle[k] = middle[k] / (double)m;
		}

		{
			double neg[3] = { -middle[0], -middle[1], -middle[2] };
			if (dot3(nrm, neg) < 0) {
				for (int k = 0; k < 3; ++k) {
					nrm[k] = -nrm[k];
				}
			}
		}

		if (fabs(nrm[1]) < 0.5) {
			memcpy(glare, nrm, sizeof(glare));
		} else {
			glare[0] = 0.0; glare[1] = -1.0; glare[2] = 0.0;
		}

		if (fabs(nrm[1]) < 0.9) {
			up[0] = 0; up[1] = 1; up[2] = 0;
		} else {
			up[0] = 1; up[1] = 0; up[2] = 0;
		}

		cross3(nrm, up, u);
		len = norm3(u);
		for (int k = 0; k < 3; ++k) {
			u[k] = u[k] / len;
		}
		cross3(nrm, u, v);

		pu = gcAlloc(m * sizeof(double));
		pv = gcAlloc(m * sizeof(double));
		pn = gcAlloc(m * sizeof(double));

		for (size_t i = 0; i < m; ++i) {
			pu[i] = dot3(&pts[3 * i], u);
			pv[i] = dot3(&pts[3 * i], v);
			pn[i] = dot3(&pts[3 * i], nrm);
		}

		d = mean1d(pn, m);
		umin = umax = pu[0];
		vmin = vmax = pv[0];
		for (size_t i = 1; i < m; ++i) {
			if (pu[i] < umin) umin = pu[i];
			if (pu[i] > umax) umax = pu[i];
			if (pv[i] < vmin) vmin = pv[i];
			if (pv[i] > vmax) vmax = pv[i];
		}

		{
			const double ab[4][2] = { { umin, vmin }, { umax, vmin }, { umax, vmax }, { umin, vmax } };
			for (int q = 0; q < 4; ++q) {
				for (int k = 0; k < 3; ++k) {
					l.corners[q][k] = s16(d * nrm[k] + ab[q][0] * u[k] + ab[q][1] * v[k]);
				}
			}
		}

		for (int k = 0; k < 3; ++k) {
			l.dir[k] = (int32_t)rnd(glare[k] * 127);
		}

		VECPUSH(out, l);
	}

#undef FIND

	return out;
}

struct roomout {
	buf data;
	double centre[3];
	int32_t bbox[6];
	lights lights;
};

static struct roomout writeRoom(const struct bgroom *room, double inv, const double *offset, uint32_t baseptr,
		uint8_t *textures, int32_t lightsindex, const int32_t *tilebox)
{
	struct roomout r;
	buf vtx = scaledRoom(room, inv, offset, r.centre, tilebox);
	outvtxs outv = {0};
	u32s outc = {0};
	tris lighttris = {0};
	u32s leafwords[2] = { {0}, {0} };
	size_t leafbase[2] = { 0, 0 };
	int hasleaf[2] = { room->haspri, room->hassec };
	const buf *dls[2] = { &room->pri, &room->sec };
	int nleaves = 0, vpad;
	size_t vtxat, colat, gdlat, at;
	uint32_t ptrs[2] = { 0, 0 };
	buf body = {0}, gdls = {0};

	for (int i = 0; i < 2; ++i) {
		if (!hasleaf[i]) {
			continue;
		}
		leafbase[i] = outv.n;
		convertList(dls[i], &vtx, &outv, &outc, leafbase[i], textures, &lighttris, &leafwords[i]);
		++nleaves;
	}

	r.lights = fixtureLights(&lighttris);
	if (r.lights.n > 255) {
		r.lights.n = 255;
	}

	vtxat = (0x18 + 20 * nleaves + 7) & ~7u;
	vpad = outv.n & 1;
	if (vpad) {
		struct outvtx last = outv.v[outv.n - 1];
		last.c = 0;
		last.s = 0;
		last.t = 0;
		VECPUSH(outv, last);
		VECPUSH(outc, 0);
	}

	colat = vtxat + 12 * outv.n;
	gdlat = (colat + 4 * outc.n + 7) & ~7u;
	bufZeros(&body, gdlat);
	set32(body.v, 0, baseptr + (uint32_t)vtxat);
	set32(body.v, 4, baseptr + (uint32_t)colat);

	at = 0x18;
	for (int i = 0; i < 2; ++i) {
		if (!hasleaf[i]) {
			continue;
		}
		ptrs[i] = baseptr + (uint32_t)at;
		body.v[at] = 0;
		set32(body.v, at + 4, 0);
		set32(body.v, at + 8, baseptr + (uint32_t)(gdlat + gdls.n));
		set32(body.v, at + 12, baseptr + (uint32_t)(vtxat + 12 * leafbase[i]));
		set32(body.v, at + 16, baseptr + (uint32_t)(colat + 4 * leafbase[i]));
		for (size_t w = 0; w < leafwords[i].n; ++w) {
			bufU32(&gdls, leafwords[i].v[w]);
		}
		at += 20;
	}

	set32(body.v, 8, ptrs[0]);
	set32(body.v, 12, ptrs[1]);
	set16(body.v, 0x10, (uint32_t)(r.lights.n ? lightsindex : -1));
	set16(body.v, 0x12, (uint32_t)r.lights.n);
	set16(body.v, 0x14, (uint32_t)(outv.n < 32767 ? outv.n : 32767));
	set16(body.v, 0x16, (uint32_t)(outc.n < 32767 ? outc.n : 32767));

	for (size_t k = 0; k < outv.n; ++k) {
		const struct outvtx *v = &outv.v[k];
		const size_t o = vtxat + 12 * k;
		set16(body.v, o, (uint16_t)v->x);
		set16(body.v, o + 2, (uint16_t)v->y);
		set16(body.v, o + 4, (uint16_t)v->z);
		body.v[o + 6] = 0;
		body.v[o + 7] = v->c;
		set16(body.v, o + 8, (uint16_t)v->s);
		set16(body.v, o + 10, (uint16_t)v->t);
	}

	for (size_t k = 0; k < outc.n; ++k) {
		set32(body.v, colat + 4 * k, outc.v[k]);
	}

	r.data = body;
	bufPut(&r.data, gdls.v, gdls.n);

	{
		size_t used = outv.n - ((outv.n && outc.v[outc.n - 1] == 0 && vpad) ? 1 : 0);
		if (outv.n && used) {
			int32_t mn[3], mx[3];
			for (int c = 0; c < 3; ++c) {
				mn[c] = mx[c] = (&outv.v[0].x)[c];
			}
			for (size_t k = 1; k < used; ++k) {
				const int32_t p[3] = { outv.v[k].x, outv.v[k].y, outv.v[k].z };
				for (int c = 0; c < 3; ++c) {
					if (p[c] < mn[c]) mn[c] = p[c];
					if (p[c] > mx[c]) mx[c] = p[c];
				}
			}
			for (int c = 0; c < 3; ++c) {
				r.bbox[c] = mn[c];
				r.bbox[3 + c] = mx[c];
			}
		} else {
			memset(r.bbox, 0, sizeof(r.bbox));
		}

		// The room holds the tiles GoldenEye files under it, which need not be
		// inside what the room draws - and a room that draws nothing has a
		// bbox of a point. bgFindRoomsByPos() answers from these boxes, and it
		// is what bwalkUpdateVertical() asks when the rooms a walker carries
		// hold no floor under them (bondwalk.c, "fell through right here on
		// runway"), so a room whose box misses its own floor cannot be found.
		if (tilebox) {
			for (int c = 0; c < 3; ++c) {
				const int32_t lo = (int32_t)floor(tilebox[c] - r.centre[c]);
				const int32_t hi = (int32_t)ceil(tilebox[3 + c] - r.centre[c]);

				if (lo < r.bbox[c]) {
					r.bbox[c] = lo < -32768 ? -32768 : lo;
				}
				if (hi > r.bbox[3 + c]) {
					r.bbox[3 + c] = hi > 32767 ? 32767 : hi;
				}
			}
		}
	}

	return r;
}

static buf writeBg(const struct bg *bg, double ls, const double *offset, uint8_t *leveltex, int *numlights,
		const int32_t (*tilebounds)[7])
{
	const double inv = 1.0 / ls;
	const int n = bg->numrooms;
	const size_t tableat = 0x18;
	size_t lightsat, cmdsat, portalsat, groupsat, inf;
	VEC(struct { int room; struct light l; }) alllights = {0};
	buf lightsblob = {0}, portals = {0}, groups = {0}, primary = {0}, primz, out = {0}, rooms = {0};
	VEC(int32_t) lens = {0};
	uint8_t *lightcounts = gcAlloc(n + 1);
	int32_t (*bboxes)[6] = gcAlloc((n + 1) * sizeof(*bboxes));
	uint32_t ptr;
	int32_t sumlights = 0;

	// the rooms are converted first, since their lights go in the primary data
	for (int r = 1; r <= n; ++r) {
		struct roomout ro = writeRoom(&bg->rooms[r - 1], inv, offset, 0, leveltex, (int32_t)alllights.n,
				tilebounds[r][6] ? tilebounds[r] : NULL);
		for (size_t k = 0; k < ro.lights.n; ++k) {
			__typeof__(*alllights.v) e = { r, ro.lights.v[k] };
			VECPUSH(alllights, e);
		}
	}

	lightsat = tableat + 20 * (n + 3);

	for (size_t k = 0; k < alllights.n; ++k) {
		bufU16(&lightsblob, alllights.v[k].room);
		bufU16(&lightsblob, 0xffff);
		bufU8(&lightsblob, 0);
		bufU8(&lightsblob, 0);
		bufU8(&lightsblob, 0);
		for (int c = 0; c < 3; ++c) {
			bufU8(&lightsblob, (uint8_t)alllights.v[k].l.dir[c]);
		}
		for (int q = 0; q < 4; ++q) {
			for (int c = 0; c < 3; ++c) {
				bufU16(&lightsblob, (uint16_t)alllights.v[k].l.corners[q][c]);
			}
		}
	}

	cmdsat = lightsat + lightsblob.n + ((4 - lightsblob.n % 4) % 4);
	portalsat = cmdsat + 8;

	for (size_t i = 0; i < bg->portals.n; ++i) {
		const struct portal *p = &bg->portals.v[i];
		bufU16(&portals, (uint32_t)(i + 1));
		bufU16(&portals, (uint16_t)p->room1);
		bufU16(&portals, (uint16_t)p->room2);
		bufU8(&portals, 0);
		bufU8(&portals, 0);
		bufU8(&groups, (uint8_t)p->npts);
		bufZeros(&groups, 3);
		for (int k = 0; k < p->npts; ++k) {
			for (int c = 0; c < 3; ++c) {
				bufF32(&groups, p->pts[k][c] * inv - offset[c]);
			}
		}
	}
	bufZeros(&portals, 8);
	bufZeros(&groups, 4);

	groupsat = portalsat + portals.n;
	bufZeros(&primary, groupsat + groups.n);
	bufPad(&primary, 4);
	set32(primary.v, 0, 0);
	set32(primary.v, 4, SEG_BG + (uint32_t)tableat);
	set32(primary.v, 8, SEG_BG + (uint32_t)portalsat);
	set32(primary.v, 12, SEG_BG + (uint32_t)cmdsat);
	set32(primary.v, 16, lightsblob.n ? SEG_BG + (uint32_t)lightsat : 0);
	set32(primary.v, 20, 0);
	if (lightsblob.n) {
		memcpy(primary.v + lightsat, lightsblob.v, lightsblob.n);
	}
	primary.v[cmdsat] = 0;
	primary.v[cmdsat + 1] = 1;
	memcpy(primary.v + portalsat, portals.v, portals.n);
	memcpy(primary.v + groupsat, groups.v, groups.n);
	inf = primary.n;

	// rooms, at pointers after the inflated primary
	ptr = SEG_BG + (uint32_t)inf;

	for (int r = 1; r <= n; ++r) {
		struct roomout ro = writeRoom(&bg->rooms[r - 1], inv, offset, ptr, leveltex, sumlights,
				tilebounds[r][6] ? tilebounds[r] : NULL);
		buf z = rzip1173(ro.data.v, ro.data.n);
		const size_t e = tableat + 20 * r;

		lightcounts[r - 1] = (uint8_t)ro.lights.n;
		sumlights += (int32_t)ro.lights.n;
		set32(primary.v, e, ptr);
		for (int c = 0; c < 3; ++c) {
			setf32(primary.v, e + 4 + 4 * c, ro.centre[c]);
		}
		primary.v[e + 16] = 128; // GE-X's brightness range; 0 draws every room black
		primary.v[e + 17] = 255;
		bufPut(&rooms, z.v, z.n);
		memcpy(bboxes[r - 1], ro.bbox, sizeof(ro.bbox));
		VECPUSH(lens, (int32_t)ro.data.n);
		ptr += (uint32_t)z.n;
	}

	set32(primary.v, tableat + 20 * (n + 1), ptr);

	primz = rzip1173(primary.v, primary.n);
	bufU32(&out, (uint32_t)inf);
	bufU32(&out, 0);
	bufU32(&out, (uint32_t)primz.n);
	bufPut(&out, primz.v, primz.n);
	bufPut(&out, rooms.v, rooms.n);
	bufPad(&out, 2);
	set32(out.v, 4, (uint32_t)(out.n - 0xc));

	// section 2: texture numbers
	{
		buf t = {0}, tz;
		for (uint32_t x = 0; x < SETBITS; ++x) {
			if (setHas(leveltex, x)) {
				bufU16(&t, texRemap(x));
			}
		}
		tz = rzip1173(t.v, t.n);
		// section 3 is read straight after section 2's stored bytes (g_BgSection3)
		bufU16(&out, 0x8000 | (uint32_t)t.n);
		bufU16(&out, (uint32_t)tz.n);
		bufPut(&out, tz.v, tz.n);
	}

	// section 3: bounding boxes, gfxdatalen, light counts
	{
		buf s3 = {0}, s3z;
		for (int r = 0; r < n; ++r) {
			for (int c = 0; c < 6; ++c) {
				bufU16(&s3, (uint16_t)s16(bboxes[r][c]));
			}
		}
		for (int r = 0; r < n; ++r) {
			const int32_t v = lens.v[r] / 16 + 1;
			bufU16(&s3, (uint32_t)(v < 0xffff ? v : 0xffff));
		}
		bufPut(&s3, lightcounts, n);
		s3z = rzip1173(s3.v, s3.n);
		bufU16(&out, 0x8000 | (uint32_t)s3.n);
		bufU16(&out, (uint32_t)s3z.n);
		bufPut(&out, s3z.v, s3z.n);
	}

	bufPad(&out, 16);
	*numlights = (int)alllights.n;
	return out;
}

/* ------------------------------------------------------------------------ */
/* tiles (write_tiles) */

// Each room's own tiles in world units, with the head and foot room of the
// walls writeTiles() raises round every unlinked tile edge: the box a player
// standing on this room's floor is inside. [6] says the room has tiles.
static int32_t (*roomTileBounds(const tiles *stan, int numrooms, double ls, const double *offset))[7]
{
	const double inv = 1.0 / ls;
	int32_t (*out)[7] = gcAlloc((numrooms + 2) * sizeof(*out));

	memset(out, 0, (numrooms + 2) * sizeof(*out));

	for (size_t i = 0; i < stan->n; ++i) {
		const struct tile *t = &stan->v[i];

		if (t->room < 1 || t->room > numrooms) {
			continue;
		}

		for (int k = 0; k < t->npts; ++k) {
			int32_t p[3];

			for (int c = 0; c < 3; ++c) {
				p[c] = s16(t->pts[k][c] * inv - offset[c]);
			}

			for (int c = 0; c < 3; ++c) {
				if (!out[t->room][6]) {
					out[t->room][c] = p[c];
					out[t->room][3 + c] = p[c];
				} else {
					if (p[c] < out[t->room][c]) out[t->room][c] = p[c];
					if (p[c] > out[t->room][3 + c]) out[t->room][3 + c] = p[c];
				}
			}

			out[t->room][6] = 1;
		}
	}

	for (int r = 0; r < numrooms + 2; ++r) {
		if (out[r][6]) {
			out[r][1] -= (int32_t)WALL_BELOW;
			out[r][4] += (int32_t)WALL_ABOVE;
		}
	}

	return out;
}

static buf writeTiles(const tiles *stan, int numrooms, double ls, const double *offset, int *numwalls)
{
	const double inv = 1.0 / ls;
	buf *bodies = gcAlloc((numrooms + 1) * sizeof(*bodies));
	buf out = {0};
	int walls = 0;
	uint32_t pos;

	for (size_t i = 0; i < stan->n; ++i) {
		const struct tile *t = &stan->v[i];
		double pts[15][3];
		const int n = t->npts;
		uint32_t flags = 0x0001 | 0x0002 | 0x0008 | 0x0010;

		if (t->room > numrooms) {
			fail("a tile in room %d, past the level's %d", t->room, numrooms);
		}

		for (int k = 0; k < n; ++k) {
			for (int c = 0; c < 3; ++c) {
				pts[k][c] = t->pts[k][c] * inv - offset[c];
			}
		}

		if (t->special == 3) {
			flags |= 0x0040;
		}

#define EMIT(FLAGS, P, N) do { \
	buf *b = &bodies[t->room]; \
	int32_t ints[15][3]; \
	int mn[3], mx[3]; \
	for (int k = 0; k < (N); ++k) for (int c = 0; c < 3; ++c) ints[k][c] = s16((P)[k][c]); \
	for (int c = 0; c < 3; ++c) { \
		mn[c] = mx[c] = 0; \
		for (int k = 1; k < (N); ++k) { \
			if (ints[k][c] < ints[mn[c]][c]) mn[c] = k; \
			if (ints[k][c] > ints[mx[c]][c]) mx[c] = k; \
		} \
	} \
	bufU8(b, 0); bufU8(b, (N)); bufU16(b, (FLAGS)); bufU16(b, 0); \
	for (int c = 0; c < 3; ++c) bufU8(b, mn[c]); \
	for (int c = 0; c < 3; ++c) bufU8(b, mx[c]); \
	bufU16(b, 0x0fff); \
	for (int k = 0; k < (N); ++k) for (int c = 0; c < 3; ++c) bufU16(b, (uint16_t)ints[k][c]); \
} while (0)

		EMIT(flags, pts, n);

		for (int k = 0; k < n; ++k) {
			double quad[4][3];
			const double *a = pts[k], *b = pts[(k + 1) % n];

			if (t->link[k] >> 4) {
				continue;
			}

			quad[0][0] = a[0]; quad[0][1] = a[1] - WALL_BELOW; quad[0][2] = a[2];
			quad[1][0] = b[0]; quad[1][1] = b[1] - WALL_BELOW; quad[1][2] = b[2];
			quad[2][0] = b[0]; quad[2][1] = b[1] + WALL_ABOVE; quad[2][2] = b[2];
			quad[3][0] = a[0]; quad[3][1] = a[1] + WALL_ABOVE; quad[3][2] = a[2];
			EMIT(0x0004, quad, 4);
			++walls;
		}
#undef EMIT
	}

	bufU32(&out, (uint32_t)(numrooms + 1));
	pos = 4 + 4 * (numrooms + 1) + 4;
	for (int r = 0; r <= numrooms; ++r) {
		bufU32(&out, pos);
		pos += (uint32_t)bodies[r].n;
	}
	bufU32(&out, pos);
	for (int r = 0; r <= numrooms; ++r) {
		bufPut(&out, bodies[r].v, bodies[r].n);
	}
	bufPad(&out, 16);
	*numwalls = walls;
	return rzip1173(out.v, out.n);
}

/* ------------------------------------------------------------------------ */
/* setups (read_setup) */

struct pad {
	double pos[3], up[3], look[3];
};

struct waypoint {
	int32_t pad, group;
	s32s neighbours;
};

struct waygroup {
	s32s neighbours, waypoints;
};

struct weaponpad {
	int32_t pad, loc;
};

struct setup {
	VEC(struct pad) pads;
	VEC(struct waypoint) waypoints;
	VEC(struct waygroup) groups;
	s32s spawns;
	VEC(struct weaponpad) weapons;
	s32s ammo;
};

static s32s readS32List(const buf *f, size_t at)
{
	s32s out = {0};
	for (;; at += 4) {
		int32_t v;
		if (at + 4 > f->n) {
			fail("a list runs off the setup");
		}
		v = bes32(f->v, at);
		if (v == -1) {
			return out;
		}
		VECPUSH(out, v);
	}
}

static void setupRead(const buf *f, struct setup *s)
{
	const uint8_t *d = f->v;
	uint32_t h[10];

	memset(s, 0, sizeof(*s));

	if (f->n < 40) {
		fail("a setup too short for its header");
	}

	for (int i = 0; i < 10; ++i) {
		h[i] = be32(d, 4 * i);
	}

	for (size_t o = h[6];; o += 0x2c) {
		struct pad p;
		if (o + 40 > f->n) {
			fail("the pads run off the setup");
		}
		if (be32(d, o + 36) == 0) {
			break;
		}
		for (int c = 0; c < 3; ++c) {
			p.pos[c] = bef32(d, o + 4 * c);
			p.up[c] = bef32(d, o + 12 + 4 * c);
			p.look[c] = bef32(d, o + 24 + 4 * c);
		}
		VECPUSH(s->pads, p);
	}

	if (h[0]) {
		for (size_t o = h[0];; o += 16) {
			struct waypoint w;
			if (o + 16 > f->n) {
				fail("the waypoints run off the setup");
			}
			w.pad = bes32(d, o);
			if (w.pad < 0) {
				break;
			}
			w.neighbours = readS32List(f, be32(d, o + 4));
			w.group = bes32(d, o + 8);
			VECPUSH(s->waypoints, w);
		}
	}

	if (h[1]) {
		for (size_t o = h[1];; o += 12) {
			struct waygroup g;
			if (o + 12 > f->n) {
				fail("the waypoint groups run off the setup");
			}
			if (be32(d, o) == 0) {
				break;
			}
			g.neighbours = readS32List(f, be32(d, o));
			g.waypoints = readS32List(f, be32(d, o + 4));
			VECPUSH(s->groups, g);
		}
	}

	if (h[2]) {
		static const int lengths[9] = { 3, 4, 4, 8, 2, 2, 13, 3, 2 };
		for (size_t o = h[2]; o + 4 <= f->n;) {
			const uint32_t t = be32(d, o) & 0xff;
			if (t == 9) {
				break;
			}
			if (t == 0) {
				if (o + 8 > f->n) {
					fail("an intro command runs off the setup");
				}
				VECPUSH(s->spawns, bes32(d, o + 4));
			}
			if (t > 8) {
				break;
			}
			o += 4 * lengths[t];
		}
	}

	if (h[3]) {
		for (size_t o = h[3]; o + 4 <= f->n;) {
			const uint32_t typ = be32(d, o) & 0xff;
			int32_t padnum;
			if (typ == 48) {
				break;
			}
			if (typ != 8 && typ != 20 && typ != 21) {
				// A multiplayer setup's other props (doors, boxes) are not
				// carried across; one this cannot size ends the walk
				break;
			}
			if (o + 0x84 > f->n) {
				fail("a prop runs off the setup");
			}
			padnum = be32(d, o + 4) & 0xffff;
			if (typ == 8) {
				const uint32_t wnum = d[o + 0x80];
				if (wnum >= 0xf0) {
					__typeof__(*s->weapons.v) w = { padnum, (int32_t)wnum - 0xf0 };
					VECPUSH(s->weapons, w);
				}
			} else if (typ == 20) {
				VECPUSH(s->ammo, padnum);
			}
			o += 4 * (typ == 20 ? 0x2d : 0x22);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* objects (geobjects.py) */

static const uint8_t g_GeSizes[48] = {
	[1] = 64, [2] = 2, [3] = 32, [4] = 33, [5] = 32, [6] = 0x3b, [7] = 0x21, [8] = 0x22, [9] = 7, [10] = 0x40,
	[11] = 0x95, [12] = 32, [13] = 0x36, [14] = 3, [17] = 32, [18] = 3, [19] = 4, [20] = 0x2d, [21] = 0x22,
	[22] = 4, [23] = 4, [24] = 1, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 4, [31] = 1, [32] = 4,
	[33] = 5, [34] = 1, [35] = 4, [36] = 32, [37] = 10, [38] = 4, [39] = 0x2c, [40] = 0x2d, [42] = 32, [43] = 32,
	[44] = 5, [45] = 0x38, [46] = 7, [47] = 37,
};

// GoldenEye type -> Perfect Dark type and its size in words; 0 is not carried
static const uint8_t g_Carry[48][2] = {
	[1] = { 0x01, 55 }, [3] = { 0x03, 23 }, [5] = { 0x05, 23 }, [10] = { 0x0a, 53 }, [11] = { 0x0b, 140 },
	[12] = { 0x0c, 23 }, [39] = { 0x03, 23 }, [40] = { 0x03, 23 }, [42] = { 0x2a, 24 }, [43] = { 0x2b, 23 },
	[45] = { 0x03, 23 }, [47] = { 0x2f, 26 },
};

struct record {
	uint32_t type;
	const uint8_t *b;
	size_t len;
};

typedef VEC(struct record) records;

static records setupRecords(const buf *f)
{
	records out = {0};
	size_t o = be32(f->v, 12);

	while (o + 4 <= f->n) {
		struct record r;
		r.type = f->v[o + 3];
		if (r.type == 48) {
			break;
		}
		if (r.type >= 48 || !g_GeSizes[r.type]) {
			fail("an object of type %u GoldenEye has no size for", r.type);
		}
		r.b = f->v + o;
		r.len = 4 * (size_t)g_GeSizes[r.type];
		if (o + r.len > f->n) {
			fail("an object runs off the setup");
		}
		VECPUSH(out, r);
		o += r.len;
	}

	return out;
}

struct padrec {
	uint32_t flags;
	double world[3];
	buf rec;
};

typedef VEC(struct padrec) padrecs;

static padrecs boundPads(const buf *f, double ls, const double *offset)
{
	padrecs out = {0};
	const uint32_t at = be32(f->v, 28);

	for (size_t o = at; at && o + 0x44 <= f->n; o += 0x44) {
		struct padrec p = {0};

		if (be32(f->v, o + 36) == 0) {
			break;
		}

		p.flags = 0x200;
		for (int c = 0; c < 3; ++c) {
			p.world[c] = bef32(f->v, o + 4 * c) / ls - offset[c];
		}
		bufU32(&p.rec, 0x200u << 14);
		for (int c = 0; c < 3; ++c) {
			bufF32(&p.rec, p.world[c]);
		}
		bufPut(&p.rec, f->v + o + 12, 24);
		for (int c = 0; c < 6; ++c) {
			// float(round()) goes through an int, so never -0.0
			bufF32(&p.rec, rnd(bef32(f->v, o + 0x2c + 4 * c) / ls) + 0.0);
		}
		VECPUSH(out, p);
	}

	return out;
}

static buf objBase(const struct record *r, uint32_t pdtype, uint32_t words, uint32_t padnum, uint32_t flags)
{
	buf out = {0};
	const int16_t model = bes16(r->b, 4);

	bufZeros(&out, 4 * (size_t)words);
	memcpy(out.v, r->b, 3);
	out.v[3] = (uint8_t)pdtype;
	set16(out.v, 4, (uint16_t)(MODEL_REMAKE_FIRST + model));
	set16(out.v, 6, padnum);
	set32(out.v, 8, flags);
	set32(out.v, 12, be32(r->b, 12));
	set16(out.v, 0x4c, 0);
	set16(out.v, 0x4e, 1000);
	set32(out.v, 0x58, 0x0fff0000);
	return out;
}

static uint32_t doorFlags(uint32_t f)
{
	const uint32_t top = f >> 24;
	uint32_t nw = top & ~0xc8u;
	if (top & 0x80) {
		nw |= 0x40;
	}
	if (top & 0x08) {
		nw |= 0x80;
	}
	return (nw << 24) | (f & 0xffffff);
}

static buf hoverbike(uint32_t padnum)
{
	buf rec = {0};
	bufZeros(&rec, 4 * 56);
	set16(rec.v, 0, 256);
	rec.v[2] = 0;
	rec.v[3] = 0x33;
	set16(rec.v, 4, MODEL_REMAKE_FIRST + MOTORBIKE);
	set16(rec.v, 6, padnum);
	set32(rec.v, 8, 0x05120101);
	set32(rec.v, 12, 0x00304300);
	set32(rec.v, 16, 0x02000000);
	set16(rec.v, 0x4c, 0);
	set16(rec.v, 0x4e, 1000);
	set32(rec.v, 0x58, 0x0fff0000);
	rec.v[0x5c] = 1;
	rec.v[0x5d] = 1;
	return rec;
}

static void objects(const buf *f, size_t numpads, int32_t firstindex, int32_t bodyarmour, const s32s *bikepads,
		buf *out, uint8_t *models)
{
	records recs = setupRecords(f);
	int32_t *newindex = gcAlloc((recs.n + 1) * sizeof(*newindex));
	int32_t kept = 0;

	for (size_t i = 0; i < recs.n; ++i) {
		const uint32_t t = recs.v[i].type;
		newindex[i] = -1;
		if (g_Carry[t][1] || t == 21) {
			newindex[i] = firstindex + kept++;
		}
	}

	for (size_t i = 0; i < recs.n; ++i) {
		const struct record *r = &recs.v[i];
		const uint32_t t = r->type;
		int32_t model, padnum;
		uint32_t flags;
		buf rec;

		if (newindex[i] < 0) {
			continue;
		}

		model = bes16(r->b, 4);
		padnum = be16(r->b, 6);
		padnum = t == 1 ? padnum + (int32_t)numpads : (padnum >= 10000 ? padnum + (int32_t)numpads - 10000 : padnum);
		if (padnum < 0 || padnum > 0xffff) {
			fail("an object's pad number does not fit");
		}
		flags = be32(r->b, 8);

		if (t == 21) {
			rec = objBase(r, 0x15, 26, padnum, flags);
			set16(rec.v, 4, (uint16_t)bodyarmour);
			memcpy(rec.v + 0x5c, r->b + 0x80, 4);
			bufPut(out, rec.v, rec.n);
			continue;
		}

		setAdd(models, (uint32_t)model);

		if (t == 1) {
			static const uint32_t moves[8][3] = { { 0x84, 0x5c, 1 }, { 0x88, 0x60, 1 }, { 0x8c, 0x64, 1000 }, { 0x90, 0x68, 1000 },
				{ 0x94, 0x6c, 1 }, { 0x98, 0x70, 1 }, { 0x9c, 0x74, 1 }, { 0xa0, 0x78, 1 } };
			int32_t rel;
			int64_t sib;

			rec = objBase(r, g_Carry[t][0], g_Carry[t][1], padnum, doorFlags(flags));
			for (int k = 0; k < 8; ++k) {
				const int64_t v = (int64_t)bes32(r->b, moves[k][0]) * moves[k][2];
				if (v < INT32_MIN || v > INT32_MAX) {
					fail("a door's times do not fit");
				}
				set32(rec.v, moves[k][1], (uint32_t)(int32_t)v);
			}
			rel = bes32(r->b, 0x80);
			sib = (int64_t)i + rel;
			set32(rec.v, 0xbc, (uint32_t)((rel && sib >= 0 && sib < (int64_t)recs.n && newindex[sib] >= 0 && recs.v[sib].type == 1)
					? newindex[sib] - newindex[i] : 0));
			rec.v[0xc6] = r->b[0xa7];
			rec.v[0xcc] = 0xff;
		} else if (t == 47) {
			rec = objBase(r, g_Carry[t][0], g_Carry[t][1], padnum, flags);
			for (int k = 0; k < 4; ++k) {
				const int32_t v = bes32(r->b, 0x80 + 4 * k);
				if (v < -32768 || v > 32767) {
					fail("a tinted glass's numbers do not fit");
				}
				set16(rec.v, 0x5c + 2 * k, (uint16_t)v);
			}
		} else {
			rec = objBase(r, g_Carry[t][0], g_Carry[t][1], padnum, flags);
		}

		bufPut(out, rec.v, rec.n);
	}

	for (size_t k = 0; k < bikepads->n; ++k) {
		buf rec = hoverbike((uint32_t)bikepads->v[k]);
		bufPut(out, rec.v, rec.n);
		setAdd(models, MOTORBIKE);
	}
}

/* the room a position is in (geobjects.Rooms) */

struct roomtile {
	int room;
	int n;
	double p[15][3];
	double x0, x1, z0, z1;
};

struct roombox {
	int room;
	double mn[3], mx[3];
};

struct roomfinder {
	VEC(struct roomtile) tiles;
	VEC(struct roombox) boxes;
};

static int pointInTile(double x, double z, const double (*p)[3], int n)
{
	int inside = 0;
	for (int k = 0; k < n; ++k) {
		const double ax = p[k][0], az = p[k][2], bx = p[(k + 1) % n][0], bz = p[(k + 1) % n][2];
		if ((az > z) != (bz > z) && x < (bx - ax) * (z - az) / (bz - az) + ax) {
			inside = !inside;
		}
	}
	return inside;
}

static double tileMeanY(const double (*p)[3], int n)
{
	double ys[15];
	for (int k = 0; k < n; ++k) {
		ys[k] = p[k][1];
	}
	return mean1d(ys, n);
}

static void roomsInit(struct roomfinder *rf, const tiles *stan, double ls, const double *offset, const struct bg *bg)
{
	memset(rf, 0, sizeof(*rf));

	for (size_t i = 0; i < stan->n; ++i) {
		struct roomtile t;
		t.room = stan->v[i].room;
		t.n = stan->v[i].npts;
		for (int k = 0; k < t.n; ++k) {
			for (int c = 0; c < 3; ++c) {
				t.p[k][c] = (double)stan->v[i].pts[k][c] / ls - offset[c];
			}
			if (!k || t.p[k][0] < t.x0) t.x0 = t.p[k][0];
			if (!k || t.p[k][0] > t.x1) t.x1 = t.p[k][0];
			if (!k || t.p[k][2] < t.z0) t.z0 = t.p[k][2];
			if (!k || t.p[k][2] > t.z1) t.z1 = t.p[k][2];
		}
		if (!t.n) {
			fail("a tile with no corners");
		}
		VECPUSH(rf->tiles, t);
	}

	for (int r = 1; r <= bg->numrooms; ++r) {
		const struct bgroom *room = &bg->rooms[r - 1];
		struct roombox b;
		size_t n;

		if (!room->hasvtx) {
			continue;
		}

		n = room->vtx.n / 16;
		b.room = r;
		for (size_t k = 0; k < n; ++k) {
			for (int c = 0; c < 3; ++c) {
				const double v = ((double)bes16(room->vtx.v, 16 * k + 2 * c) + room->pos[c]) / ls - offset[c];
				if (!k || v < b.mn[c]) b.mn[c] = v;
				if (!k || v > b.mx[c]) b.mx[c] = v;
			}
		}
		if (!n) {
			fail("a room with a partial vertex");
		}
		VECPUSH(rf->boxes, b);
	}
}

/**
 * The floor a pad stands on (geobjects.py's Rooms.floor()): the highest tile
 * whose x/z holds the pad and whose own height does not pass it. 0 when the
 * pad is over a hole, or under the floor, with *found cleared.
 */
static double roomsFloor(const struct roomfinder *rf, const double *pos, int *found)
{
	const double x = pos[0], y = pos[1], z = pos[2];
	double best = 0;

	*found = 0;

	for (size_t i = 0; i < rf->tiles.n; ++i) {
		const struct roomtile *t = &rf->tiles.v[i];
		double fy;

		if (!(t->x0 <= x && x <= t->x1 && t->z0 <= z && z <= t->z1)) {
			continue;
		}
		if (!pointInTile(x, z, t->p, t->n)) {
			continue;
		}

		fy = tileMeanY(t->p, t->n);

		if (fy <= y && (!*found || fy > best)) {
			best = fy;
			*found = 1;
		}
	}

	return best;
}

static int roomsFind(const struct roomfinder *rf, const double *pos)
{
	const double x = pos[0], y = pos[1], z = pos[2];
	int best = -1;
	double bestscore = 0;

	for (size_t i = 0; i < rf->tiles.n; ++i) {
		const struct roomtile *t = &rf->tiles.v[i];
		double dy, score;

		if (!(t->x0 <= x && x <= t->x1 && t->z0 <= z && z <= t->z1)) {
			continue;
		}
		if (!pointInTile(x, z, t->p, t->n)) {
			continue;
		}
		dy = y - tileMeanY(t->p, t->n);
		score = dy >= -60 ? dy : 10000 - dy;
		if (best < 0 || score < bestscore) {
			bestscore = score;
			best = t->room;
		}
	}

	if (best >= 0) {
		return best;
	}

	// the smallest box that holds it, the lowest room among equals
	{
		int found = -1;
		double vol = 0;
		for (size_t i = 0; i < rf->boxes.n; ++i) {
			const struct roombox *b = &rf->boxes.v[i];
			double d[3], v;
			int in = 1;
			for (int c = 0; c < 3; ++c) {
				if (!(pos[c] >= b->mn[c] - 20) || !(pos[c] <= b->mx[c] + 20)) {
					in = 0;
				}
				d[c] = b->mx[c] - b->mn[c];
			}
			if (!in) {
				continue;
			}
			v = d[0] * d[1] * d[2];
			if (found < 0 || v < vol || (v == vol && b->room < found)) {
				vol = v;
				found = b->room;
			}
		}
		if (found >= 0) {
			return found;
		}
	}

	{
		int found = -1;
		double dist = 0;
		for (size_t i = 0; i < rf->boxes.n; ++i) {
			const struct roombox *b = &rf->boxes.v[i];
			double d[3], v;
			for (int c = 0; c < 3; ++c) {
				const double cl = pos[c] < b->mn[c] ? b->mn[c] : (pos[c] > b->mx[c] ? b->mx[c] : pos[c]);
				d[c] = cl - pos[c];
			}
			v = norm3(d);
			if (found < 0 || v < dist || (v == dist && b->room < found)) {
				dist = v;
				found = b->room;
			}
		}
		if (found < 0) {
			fail("a level with no rooms to put a pad in");
		}
		return found;
	}
}

/* ------------------------------------------------------------------------ */
/* pads (write_pads) */

static void bufS32List(buf *b, const s32s *list)
{
	for (size_t i = 0; i < list->n; ++i) {
		bufU32(b, (uint32_t)list->v[i]);
	}
	bufU32(b, 0xffffffff);
}

static buf writePads(const struct setup *setup, double ls, const double *offset, const struct roomfinder *rf, const padrecs *boundpads)
{
	const double inv = 1.0 / ls;
	padrecs records = {0};
	buf head = {0}, body = {0}, padsblob = {0}, wprec = {0}, nbblob = {0}, rec = {0}, wlists = {0}, nlists = {0}, out = {0};
	size_t start, wpstart, wgstart, coverstart;
	uint32_t npos, wpos;

	for (size_t i = 0; i < setup->pads.n; ++i) {
		const struct pad *p = &setup->pads.v[i];
		struct padrec r = {0};
		double fy;
		int onfloor;

		for (int c = 0; c < 3; ++c) {
			r.world[c] = p->pos[c] * inv - offset[c];
		}

		// GoldenEye stands a pad on the floor and is happy with that; Perfect
		// Dark's ground search takes the highest floor *strictly* below the
		// position it is handed and answers -2^32 for none, so a pad exactly
		// level with its floor has no ground under it at all. The player
		// spawned on one of those in Egyptian, Control and Caverns and fell
		// out of the world. One unit - a centimetre - is the whole of the
		// lift, and only 63 of the 5616 pads take it.
		fy = roomsFloor(rf, r.world, &onfloor);

		if (onfloor && fy >= r.world[1]) {
			r.world[1] = fy + 1;
		}

		bufU32(&r.rec, 0);
		for (int c = 0; c < 3; ++c) bufF32(&r.rec, r.world[c]);
		for (int c = 0; c < 3; ++c) bufF32(&r.rec, p->up[c]);
		for (int c = 0; c < 3; ++c) bufF32(&r.rec, p->look[c]);
		VECPUSH(records, r);
	}

	for (size_t i = 0; i < boundpads->n; ++i) {
		VECPUSH(records, boundpads->v[i]);
	}

	start = (0x14 + 2 * records.n + 3) & ~3u;

	for (size_t i = 0; i < records.n; ++i) {
		const struct padrec *r = &records.v[i];
		const int room = roomsFind(rf, r->world);
		uint32_t hdr = be32(r->rec.v, 0);

		bufU16(&head, (uint32_t)(start + body.n));
		hdr = (r->flags << 14) | (((uint32_t)room & 0x3ff) << 4) | (hdr & 0xf);
		bufU32(&body, hdr);
		bufPut(&body, r->rec.v + 4, r->rec.n - 4);
		bufPad(&body, 4);
	}

	bufPad(&head, 4);
	bufPut(&padsblob, head.v, head.n);
	bufPut(&padsblob, body.v, body.n);

	wpstart = 0x14 + padsblob.n;
	npos = (uint32_t)(wpstart + 16 * setup->waypoints.n + 16);
	for (size_t i = 0; i < setup->waypoints.n; ++i) {
		const struct waypoint *w = &setup->waypoints.v[i];
		size_t before = nbblob.n;
		bufU32(&wprec, (uint32_t)w->pad);
		bufU32(&wprec, npos);
		bufU32(&wprec, (uint32_t)(w->group > 0 ? w->group : 0));
		bufU32(&wprec, 0);
		bufS32List(&nbblob, &w->neighbours);
		npos += (uint32_t)(nbblob.n - before);
	}
	bufU32(&wprec, 0xffffffff);
	bufZeros(&wprec, 12);

	wgstart = wpstart + wprec.n + nbblob.n;
	for (size_t i = 0; i < setup->groups.n; ++i) {
		bufS32List(&wlists, &setup->groups.v[i].waypoints);
	}
	wpos = (uint32_t)(wgstart + 12 * setup->groups.n + 12);
	npos = wpos + (uint32_t)wlists.n;
	for (size_t i = 0; i < setup->groups.n; ++i) {
		const struct waygroup *g = &setup->groups.v[i];
		size_t before = nlists.n;
		bufU32(&rec, npos);
		bufU32(&rec, wpos);
		bufU32(&rec, 0);
		bufS32List(&nlists, &g->neighbours);
		npos += (uint32_t)(nlists.n - before);
		wpos += (uint32_t)(4 * g->waypoints.n + 4);
	}
	bufZeros(&rec, 12);

	coverstart = wgstart + rec.n + wlists.n + nlists.n;

	bufU32(&out, (uint32_t)records.n);
	bufU32(&out, 0);
	bufU32(&out, (uint32_t)wpstart);
	bufU32(&out, (uint32_t)wgstart);
	bufU32(&out, (uint32_t)coverstart);
	bufPut(&out, padsblob.v, padsblob.n);
	bufPut(&out, wprec.v, wprec.n);
	bufPut(&out, nbblob.v, nbblob.n);
	bufPut(&out, rec.v, rec.n);
	bufPut(&out, wlists.v, wlists.n);
	bufPut(&out, nlists.v, nlists.n);
	bufPad(&out, 16);
	return rzip1173(out.v, out.n);
}

/* ------------------------------------------------------------------------ */
/* multiplayer setup (floored_pads, spread, bike_pads, write_mpsetup) */

static s32s flooredPads(const struct setup *setup, const tiles *stan, const struct bg *bg)
{
	s32s out = {0};
	VEC(struct roomtile) tl = {0};

	for (size_t i = 0; i < stan->n; ++i) {
		const struct tile *s = &stan->v[i];
		struct roomtile t;

		if (!(s->room > 0 && s->room <= bg->numrooms && bg->rooms[s->room - 1].hasvtx)) {
			continue;
		}

		t.room = s->room;
		t.n = s->npts;
		for (int k = 0; k < t.n; ++k) {
			for (int c = 0; c < 3; ++c) {
				t.p[k][c] = s->pts[k][c];
			}
			if (!k || t.p[k][0] < t.x0) t.x0 = t.p[k][0];
			if (!k || t.p[k][0] > t.x1) t.x1 = t.p[k][0];
			if (!k || t.p[k][2] < t.z0) t.z0 = t.p[k][2];
			if (!k || t.p[k][2] > t.z1) t.z1 = t.p[k][2];
		}
		VECPUSH(tl, t);
	}

	for (size_t i = 0; i < setup->pads.n; ++i) {
		const double x = setup->pads.v[i].pos[0], y = setup->pads.v[i].pos[1], z = setup->pads.v[i].pos[2];

		for (size_t j = 0; j < tl.n; ++j) {
			const struct roomtile *t = &tl.v[j];
			double dy;
			if (!(t->x0 <= x && x <= t->x1 && t->z0 <= z && z <= t->z1)) {
				continue;
			}
			if (!pointInTile(x, z, t->p, t->n)) {
				continue;
			}
			dy = y - tileMeanY(t->p, t->n);
			if (-30 <= dy && dy <= 60) {
				VECPUSH(out, (int32_t)i);
				break;
			}
		}
	}

	return out;
}

// k of the points spread out by farthest-point sampling, as indices into them
static s32s spread(const double (*pts)[3], size_t n, size_t k)
{
	s32s chosen = {0};
	double mean[3] = { 0, 0, 0 };
	double *d;
	size_t best = 0;

	if (n <= k) {
		for (size_t i = 0; i < n; ++i) {
			VECPUSH(chosen, (int32_t)i);
		}
		return chosen;
	}

	for (size_t i = 0; i < n; ++i) {
		for (int c = 0; c < 3; ++c) {
			mean[c] += pts[i][c];
		}
	}
	for (int c = 0; c < 3; ++c) {
		mean[c] = mean[c] / (double)n;
	}

	d = gcAlloc(n * sizeof(*d));

	#define ROWNORM(i, q) sqrt(0. + (pts[i][0] - (q)[0]) * (pts[i][0] - (q)[0]) \
		+ (pts[i][1] - (q)[1]) * (pts[i][1] - (q)[1]) + (pts[i][2] - (q)[2]) * (pts[i][2] - (q)[2]))

	for (size_t i = 0; i < n; ++i) {
		const double v = ROWNORM(i, mean);
		if (!i || v < d[0]) {
			d[0] = v;
			best = i;
		}
	}
	VECPUSH(chosen, (int32_t)best);

	for (size_t i = 0; i < n; ++i) {
		d[i] = ROWNORM(i, pts[best]);
	}

	while (chosen.n < k) {
		size_t at = 0;
		for (size_t i = 1; i < n; ++i) {
			if (d[i] > d[at]) {
				at = i;
			}
		}
		VECPUSH(chosen, (int32_t)at);
		for (size_t i = 0; i < n; ++i) {
			const double v = ROWNORM(i, pts[at]);
			if (v < d[i]) {
				d[i] = v;
			}
		}
	}

	#undef ROWNORM
	return chosen;
}

static double norm2d(double a, double b)
{
	return sqrt(0. + a * a + b * b);
}

static s32s bikePads(const struct level *lv, const struct setup *setup, const tiles *stan, const struct bg *bg, const buf *gedata)
{
	s32s chosen = {0};
	records recs;
	int32_t tank = -1;
	double tankpos[3];
	s32s floored;
	VEC(struct { int32_t i; double dist; }) near = {0};

	if (!lv->bikes) {
		return chosen;
	}

	recs = setupRecords(gedata);
	for (size_t i = 0; i < recs.n; ++i) {
		if (recs.v[i].type == 45) {
			tank = be16(recs.v[i].b, 6);
			break;
		}
	}
	if (tank < 0) {
		return chosen;
	}
	if ((size_t)tank >= setup->pads.n) {
		fail("the tank stands on a pad the setup does not have");
	}

	for (int c = 0; c < 3; ++c) {
		tankpos[c] = setup->pads.v[tank].pos[c] / lv->levelscale;
	}

	floored = flooredPads(setup, stan, bg);
	for (size_t j = 0; j < floored.n; ++j) {
		const int32_t i = floored.v[j];
		double pos[3], d;
		for (int c = 0; c < 3; ++c) {
			pos[c] = setup->pads.v[i].pos[c] / lv->levelscale;
		}
		d = norm2d(pos[0] - tankpos[0], pos[2] - tankpos[2]);
		if (300 <= d && d <= 1600 && fabs(pos[1] - tankpos[1]) < 60) {
			__typeof__(*near.v) e;
			double rel[3];
			for (int c = 0; c < 3; ++c) {
				rel[c] = pos[c] - tankpos[c];
			}
			e.i = i;
			e.dist = norm3(rel);
			VECPUSH(near, e);
		}
	}

	// nearest first, a stable sort
	for (size_t a = 1; a < near.n; ++a) {
		__typeof__(*near.v) e = near.v[a];
		size_t b = a;
		while (b > 0 && near.v[b - 1].dist > e.dist) {
			near.v[b] = near.v[b - 1];
			--b;
		}
		near.v[b] = e;
	}

	for (size_t a = 0; a < near.n && chosen.n < (size_t)lv->bikes; ++a) {
		const int32_t i = near.v[a].i;
		int ok = 1;
		for (size_t c = 0; c < chosen.n; ++c) {
			double rel[3];
			for (int q = 0; q < 3; ++q) {
				rel[q] = setup->pads.v[i].pos[q] / lv->levelscale - setup->pads.v[chosen.v[c]].pos[q] / lv->levelscale;
			}
			if (!(norm3(rel) >= 250)) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			VECPUSH(chosen, i);
		}
	}

	return chosen;
}

static const uint8_t g_Ai1000[] = { 0x01, 0x85, 0x01, 0x45, 0x01, 0x46, 0x00, 0x05, 0xfd, 0x00, 0x00, 0x00, 0x04 };
static const uint8_t g_Ai1001[] = { 0x01, 0xb2, 0x16, 0x00, 0x05, 0xfd, 0x00, 0x00, 0x00, 0x04 };

static buf writeMpSetup(const struct setup *setup, const struct setup *mp, const tiles *stan, const struct bg *bg,
		const buf *gedata, const s32s *bikepads, uint8_t *models)
{
	s32s spawns = {0}, ammo = {0};
	VEC(struct weaponpad) weapons = {0};
	buf intro = {0}, props = {0}, out = {0};
	size_t introat, propsat, pathsat, aiat, aicodeat;

	if (mp && mp->spawns.n) {
		spawns = mp->spawns;
		ammo = mp->ammo;
		for (size_t i = 0; i < mp->weapons.n; ++i) {
			__typeof__(*weapons.v) w = { mp->weapons.v[i].pad, mp->weapons.v[i].loc };
			VECPUSH(weapons, w);
		}
	} else {
		s32s ok = flooredPads(setup, stan, bg);
		double (*pts)[3] = gcAlloc((ok.n + 1) * sizeof(*pts));
		s32s idx;
		for (size_t i = 0; i < ok.n; ++i) {
			memcpy(pts[i], setup->pads.v[ok.v[i]].pos, sizeof(pts[i]));
		}
		idx = spread(pts, ok.n, 28);
		for (size_t i = 0; i < idx.n; ++i) {
			const int32_t p = ok.v[idx.v[i]];
			if (i < 12) {
				VECPUSH(spawns, p);
			} else if (i < 24) {
				__typeof__(*weapons.v) w = { p, (int32_t)((i - 12) % 6) };
				VECPUSH(weapons, w);
			} else {
				VECPUSH(ammo, p);
			}
		}
	}

	for (size_t i = 0; i < spawns.n; ++i) {
		bufU32(&intro, 0);
		bufU32(&intro, (uint32_t)spawns.v[i]);
		bufU32(&intro, 0);
	}
	bufU32(&intro, 0x0c);

	for (size_t i = 0; i < weapons.n; ++i) {
		bufU32(&props, (0x0100u << 16) | 0x08);
		bufU32(&props, (uint32_t)weapons.v[i].pad & 0xffff);
		bufU32(&props, 1);
		bufZeros(&props, 4 * 16);
		bufU32(&props, 1000);
		bufZeros(&props, 8);
		bufU32(&props, 0x0fff0000);
		bufU32(&props, (uint32_t)(0xf0 + weapons.v[i].loc) << 24);
		bufU32(&props, 0x00ffffff);
		bufU32(&props, 0);
	}

	for (size_t i = 0; i < ammo.n; ++i) {
		bufU32(&props, (0x00ccu << 16) | 0x14);
		bufU32(&props, (0x00c1u << 16) | ((uint32_t)ammo.v[i] & 0xffff));
		bufU32(&props, 1);
		bufZeros(&props, 4 * 16);
		bufU32(&props, 1000);
		bufZeros(&props, 8);
		bufU32(&props, 0x0fff0000);
		for (int k = 0; k < 19; ++k) {
			bufU32(&props, 0xffff0000);
		}
	}

	objects(gedata, setup->pads.n, (int32_t)(weapons.n + ammo.n), 0x182, bikepads, &props, models);
	bufU32(&props, 0x34);

	introat = 0x20;
	propsat = introat + intro.n;
	pathsat = propsat + props.n;
	aiat = pathsat + 8;
	aicodeat = aiat + 24;

	bufU32(&out, 0);
	bufU32(&out, 0);
	bufU32(&out, 0);
	bufU32(&out, (uint32_t)introat);
	bufU32(&out, (uint32_t)propsat);
	bufU32(&out, (uint32_t)pathsat);
	bufU32(&out, (uint32_t)aiat);
	bufU32(&out, 0);
	bufPut(&out, intro.v, intro.n);
	bufPut(&out, props.v, props.n);
	bufZeros(&out, 8);
	bufU32(&out, (uint32_t)aicodeat);
	bufU32(&out, 0x1000);
	bufU32(&out, (uint32_t)(aicodeat + sizeof(g_Ai1000)));
	bufU32(&out, 0x1001);
	bufZeros(&out, 8);
	bufPut(&out, g_Ai1000, sizeof(g_Ai1000));
	bufPut(&out, g_Ai1001, sizeof(g_Ai1001));
	bufPad(&out, 16);
	return rzip1173(out.v, out.n);
}

/* ------------------------------------------------------------------------ */
/* solo missions (gesolo.py) */

/**
 * GoldenEye's twenty solo missions in the folder's order (gexfront.c): the
 * level they stand on and the setup file that is the mission. Surface and
 * Bunker are two missions each on one level.
 */
static const struct { const char *key, *setup, *name; } g_Missions[] = {
	{ "dam",   "UsetupdamZ",       "Dam" },
	{ "ark",   "UsetuparkZ",       "Facility" },
	{ "run",   "UsetuprunZ",       "Runway" },
	{ "sevx",  "UsetupsevxZ",      "Surface" },
	{ "sev",   "UsetupsevbunkerZ", "Bunker" },
	{ "silo",  "UsetupsiloZ",      "Silo" },
	{ "dest",  "UsetupdestZ",      "Frigate" },
	{ "sevxb", "UsetupsevxbZ",     "Surface 2" },
	{ "sevb",  "UsetupsevbZ",      "Bunker 2" },
	{ "stat",  "UsetupstatueZ",    "Statue Park" },
	{ "arch",  "UsetuparchZ",      "Archives" },
	{ "pete",  "UsetuppeteZ",      "Streets" },
	{ "depo",  "UsetupdepoZ",      "Depot" },
	{ "tra",   "UsetuptraZ",       "Train" },
	{ "jun",   "UsetupjunZ",       "Jungle" },
	{ "arec",  "UsetupcontrolZ",   "Control" },
	{ "cave",  "UsetupcaveZ",      "Caverns" },
	{ "crad",  "UsetupcradZ",      "Cradle" },
	{ "azt",   "UsetupaztZ",       "Aztec" },
	{ "cryp",  "UsetupcrypZ",      "Egyptian" },
};

#define NUM_MISSIONS (sizeof(g_Missions) / sizeof(g_Missions[0]))

// Perfect Dark's own record sizes in words, as port/src/preprocess/filesetup.c
// sizes them (objSizeN64(), the n64_* structs). 0 is a type it has no record for.
static const uint8_t g_PdSizes[0x35] = {
	[0x01] = 55, [0x02] = 2, [0x03] = 23, [0x04] = 24, [0x05] = 23, [0x06] = 49, [0x07] = 24,
	[0x08] = 26, [0x09] = 11, [0x0a] = 53, [0x0b] = 140, [0x0c] = 23, [0x0d] = 43, [0x0e] = 2,
	[0x0f] = 23, [0x11] = 23, [0x12] = 2, [0x13] = 5, [0x14] = 42, [0x15] = 26, [0x16] = 4,
	[0x17] = 4, [0x18] = 1, [0x19] = 2, [0x1a] = 2, [0x1b] = 2, [0x1c] = 2, [0x1d] = 2,
	[0x1e] = 4, [0x1f] = 1, [0x20] = 4, [0x21] = 5, [0x22] = 1, [0x23] = 4, [0x24] = 23,
	[0x25] = 10, [0x26] = 4, [0x27] = 34, [0x28] = 35, [0x2a] = 24, [0x2b] = 23, [0x2c] = 5,
	[0x2d] = 32, [0x2e] = 7, [0x2f] = 26, [0x30] = 37, [0x31] = 5, [0x32] = 4, [0x33] = 56,
	[0x34] = 1,
};

// GoldenEye types with no Perfect Dark record of the same shape: they keep
// their place in the list as a one-word OBJTYPE_22
// Hats (0x11) are left out with them: GoldenEye's hat is its own model, and a
// converted one is a rigid prop - one matrix, a position node at its root -
// which Perfect Dark cannot pose on a head. See gesolo.py.
#define SOLO_AS_NOTHING(t) ((t) == 0x0e || (t) == 0x11 || (t) == 0x12 || (t) == 0x13 || (t) == 0x14)

#define SOLO_NO_PAD 0xffff
#define SOLO_WEAPON_GE_FIRST 0x5e
#define SOLO_NUM_GE_WEAPONS (0x76 - 0x5e + 1)

/**
 * A GoldenEye pad id in the converted level: its own pads keep their index and
 * its bound pads are written after them, so a bound pad - one at 10000 and up,
 * and a door's pad field, which is always one - is numpads plus its index.
 * 0xffff is not a pad at all (a collectable a guard carries has nowhere to
 * stand), and must stay that way or the loader reads past the pad table.
 */
static uint32_t padNum(uint32_t p, size_t numpads, int bound)
{
	if (p == SOLO_NO_PAD) {
		return SOLO_NO_PAD;
	}
	if (bound) {
		return p + (uint32_t)numpads;
	}
	return p >= 10000 ? p + (uint32_t)numpads - 10000 : p;
}

/** GoldenEye's ObjectRecord as Perfect Dark's defaultobj. */
static void baseRecord(uint8_t *out, const uint8_t *raw, uint32_t pdtype, uint32_t padnum)
{
	memcpy(out, raw, 3);
	out[3] = (uint8_t)pdtype;
	set16(out, 4, (uint32_t)(MODEL_REMAKE_FIRST + bes16(raw, 4)));
	set16(out, 6, padnum);
	set32(out, 8, be32(raw, 8));
	set32(out, 12, be32(raw, 12));
	set16(out, 0x4c, 0);
	set16(out, 0x4e, 1000);
	set32(out, 0x58, 0x0fff0000);
}

/** The fields a door moved between the two formats (geobjects.py's rules). */
static void doorRecord(uint8_t *out, const uint8_t *raw, size_t numpads, const records *recs, size_t index)
{
	static const struct { uint32_t ge, pd, mul; } fields[] = {
		{ 0x84, 0x5c, 1 }, { 0x88, 0x60, 1 }, { 0x8c, 0x64, 1000 }, { 0x90, 0x68, 1000 },
		{ 0x94, 0x6c, 1 }, { 0x98, 0x70, 1 }, { 0x9c, 0x74, 1 }, { 0xa0, 0x78, 1 },
	};
	const int32_t rel = bes32(raw, 0x80);
	const int64_t sib = (int64_t)index + rel;

	baseRecord(out, raw, 0x01, padNum(be16(raw, 6), numpads, 1));
	set32(out, 8, doorFlags(be32(raw, 8)));

	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
		set32(out, fields[i].pd, (uint32_t)(bes32(raw, fields[i].ge) * (int32_t)fields[i].mul));
	}

	set32(out, 0xbc, (uint32_t)(rel && sib >= 0 && (size_t)sib < recs->n && recs->v[sib].type == 1 ? rel : 0));
	out[0xc6] = raw[0xa7];
	out[0xcc] = 0xff;
}

/**
 * GoldenEye's GuardRecord as Perfect Dark's packedchr.
 *
 * The two name most of the same things. GoldenEye's own setup flags are Perfect
 * Dark's spawn flags for the three it uses - sunglasses (0x01), sunglasses half
 * the time (0x02) and invincible (0x08); its 0x04 is "this is a clone", which
 * Perfect Dark has no spawn flag for. The two fields the decomp calls health and
 * reaction time are its hearing scale and vision range (chraction.c reads them
 * into hearingscale and visionrange), which are Perfect Dark's own two.
 *
 * The body and head are GoldenEye's own character numbers, left as they are:
 * what there is to wear is not known until the mission loads, so the port maps
 * them then (gexplus.c's gexPlusMissionChr()).
 */
static void guardRecord(uint8_t *out, const uint8_t *raw, size_t numpads)
{
	const int16_t chrnum = bes16(raw, 4);
	const uint32_t padid = be16(raw, 6);
	const uint32_t body = be16(raw, 8);
	const uint32_t ailist = be16(raw, 10);
	const uint32_t preset = be16(raw, 12);
	const uint32_t chrpreset = be16(raw, 14);
	const uint32_t hearscale = be16(raw, 16);
	const uint32_t viewdist = be16(raw, 18);
	const uint32_t flags = be16(raw, 20);
	const int16_t head = bes16(raw, 22);

	memcpy(out, raw, 3);
	out[3] = 0x09;
	set32(out, 0x04, flags & 0x000b);
	set16(out, 0x08, (uint32_t)chrnum);
	set16(out, 0x0a, padNum(padid, numpads, 0));
	out[0x0c] = (uint8_t)body;
	out[0x0d] = head >= 0 ? (uint8_t)head : 0xff;
	set16(out, 0x0e, ailist);
	set16(out, 0x10, padNum(preset, numpads, 0));
	set16(out, 0x12, chrpreset);
	set16(out, 0x14, hearscale);
	set16(out, 0x16, viewdist);
	set16(out, 0x22, 0xffff);   // no chair
}

/** A GoldenEye collectable as a Perfect Dark weapon prop, on the port's own
 * GoldenEye weapons (geguns.c, WEAPON_GE_FIRST + GoldenEye's item). */
static void weaponRecord(uint8_t *out, const uint8_t *raw, size_t numpads)
{
	const uint32_t item = raw[0x80];

	baseRecord(out, raw, 0x08, padNum(be16(raw, 6), numpads, 0));
	out[0x5c] = item < SOLO_NUM_GE_WEAPONS ? (uint8_t)(SOLO_WEAPON_GE_FIRST + item) : 0;
	out[0x5d] = 0xff;
	out[0x5e] = 0xff;
	set16(out, 0x62, be16(raw, 0x82));
}

/**
 * A GoldenEye text id as a Perfect Dark one (gesolo.py's text_id).
 *
 * GoldenEye's is `bank * 0x400 + slot` and the bank is always the mission's
 * own - the file the converter copies to menu/ and the port loads into
 * LANGBANK_GEMISSION - so only the slot carries. Perfect Dark gives a slot
 * nine bits; no mission's bank holds more than 108 strings, and a slot that
 * would not fit becomes no text rather than another bank's string.
 */
#define SOLO_LANGBANK_GEMISSION 0x45

static uint32_t soloTextId(uint32_t geid)
{
	const uint32_t slot = geid & 0x3ff;

	if (!geid || slot >= 0x200) {
		return 0;
	}

	return (SOLO_LANGBANK_GEMISSION << 9) | slot;
}

/**
 * GoldenEye's objective heading as Perfect Dark's: it gives an objective the
 * lowest difficulty it appears at and Perfect Dark keeps a bit per difficulty,
 * so the bits from that one up are set. GoldenEye's 007 is Perfect Dark's PD
 * Mode over the hardest difficulty, so it takes 00 Agent's bit.
 *
 * GoldenEye's own 0x100 there is not a difficulty. objectiveIsAllComplete()
 * tests `objdiff <= curdiff`, so an objective marked with it is never required
 * and never listed in the mission - its briefing file gives it a difficulty of
 * its own, and the briefing screen does show it. Perfect Dark's test is the
 * same shape over difficulty bits, so it gets none.
 */
static void objectiveRecord(uint8_t *out, const uint8_t *raw)
{
	const int32_t mindiff = bes32(raw, 12);
	uint32_t bits = 0;

	memcpy(out, raw, 3);
	out[3] = 0x17;
	set32(out, 4, be32(raw, 4));
	set32(out, 8, soloTextId(be32(raw, 8)));

	if (mindiff < 3) {
		for (int d = 0; d < 3; ++d) {
			if (d >= (mindiff < 2 ? mindiff : 2)) {
				bits |= 1u << d;
			}
		}
	}

	out[0x0f] = (uint8_t)bits;
}

struct solostats {
	int props, dropped, aikept, aidropped;
};

static buf writeSoloProps(const buf *f, size_t numpads, uint8_t *models, struct solostats *st)
{
	// the tails of the ObjectRecord types Perfect Dark keeps in the same order:
	// GoldenEye's 0x80 onwards against Perfect Dark's 0x5c
	static const struct { uint8_t type, ge, pd, width; } tails[] = {
		{ 0x04, 0x80, 0x5c, 4 },                            // key: the key flags
		{ 0x07, 0x80, 0x5c, 4 },                            // ammo crate: the ammo type
		{ 0x15, 0x80, 0x5c, 4 }, { 0x15, 0x84, 0x60, 4 },   // armour: initial and current
		// glass (0x2a) has no tail: GoldenEye's record is the ObjectRecord and
		// nothing more, and Perfect Dark finds a pane's portal at the load
	};
	records recs = setupRecords(f);
	buf out = {0};

	for (size_t i = 0; i < recs.n; ++i) {
		const uint32_t t = recs.v[i].type;
		const uint8_t *raw = recs.v[i].b;
		const uint32_t words = t < sizeof(g_PdSizes) ? g_PdSizes[t] : 0;
		uint8_t *rec;

		if (SOLO_AS_NOTHING(t) || !words) {
			bufU32(&out, 0x22);
			st->dropped++;
			continue;
		}

		if (g_GeSizes[t] >= 32) {
			// every ObjectRecord names a model, doors and collectables included
			setAdd(models, (uint32_t)bes16(raw, 4));
		}

		bufZeros(&out, 4 * words);
		rec = out.v + out.n - 4 * words;

		if (t == 1) {
			doorRecord(rec, raw, numpads, &recs, i);
		} else if (t == 9) {
			guardRecord(rec, raw, numpads);
		} else if (t == 8) {
			weaponRecord(rec, raw, numpads);
		} else if (t == 0x17) {
			objectiveRecord(rec, raw);
		} else if (g_GeSizes[t] >= 32) {
			baseRecord(rec, raw, t, padNum(be16(raw, 6), numpads, 0));
			for (size_t k = 0; k < sizeof(tails) / sizeof(tails[0]); ++k) {
				if (tails[k].type == t && tails[k].ge + tails[k].width <= recs.v[i].len) {
					memcpy(rec + tails[k].pd, raw + tails[k].ge, tails[k].width);
				}
			}
		} else {
			// a short record: the same fields in the same order on both sides
			const size_t keep = 4 * (size_t)words < recs.v[i].len ? 4 * (size_t)words : recs.v[i].len;
			memcpy(rec, raw, keep);
			rec[3] = (uint8_t)t;
		}

		st->props++;
	}

	bufU32(&out, 0x34);
	return out;
}

/**
 * GoldenEye's intro commands are Perfect Dark's own: the same types in the same
 * order at the same widths, and only the end differs - GoldenEye stops at 9 and
 * Perfect Dark at 12. Type 6 is ten words, which is what Perfect Dark's is
 * (modrandom.c sizes INTROCMD_6 at 40 bytes).
 */
static buf writeSoloIntro(const buf *f)
{
	static const uint8_t words[9] = { 3, 4, 4, 8, 2, 2, 10, 3, 2 };
	const uint32_t at = be32(f->v, 8);
	buf out = {0};
	size_t o = at;

	while (at && o + 4 <= f->n) {
		const uint32_t t = be32(f->v, o) & 0xff;

		if (t == 9 || t >= sizeof(words)) {
			break;
		}
		if (o + 4 * (size_t)words[t] > f->n) {
			break;
		}
		bufPut(&out, f->v + o, 4 * (size_t)words[t]);
		o += 4 * (size_t)words[t];
	}

	bufU32(&out, 12);
	return out;
}

/** GoldenEye's patrol paths, which are Perfect Dark's own record. */
static void writeSoloPaths(const buf *f, size_t at, buf *head, buf *body)
{
	const uint32_t start = be32(f->v, 16);
	size_t n = 0, pos;

	if (!start) {
		bufU32(head, 0);
		bufU32(head, 0);
		return;
	}

	for (size_t o = start; o + 8 <= f->n && be32(f->v, o); o += 8) {
		++n;
	}

	pos = at + 8 * (n + 1);

	for (size_t i = 0; i < n; ++i) {
		const size_t o = start + 8 * i;
		const s32s pads = readS32List(f, be32(f->v, o));

		bufU32(head, (uint32_t)pos);
		bufU8(head, f->v[o + 4]);
		bufU8(head, f->v[o + 5]);
		bufU16(head, be16(f->v, o + 6));

		for (size_t k = 0; k < pads.n; ++k) {
			bufU32(body, (uint32_t)pads.v[k]);
		}
		bufU32(body, 0xffffffff);
		pos += 4 * (pads.n + 1);
	}

	bufU32(head, 0);
	bufU32(head, 0);
}

/**
 * The length of the GoldenEye AI command at `at`. Every command has its own
 * fixed length but one: PRINT is a debug comment whose text follows the opcode
 * and runs to a NUL (chrai.c's chraiitemsize()). A walk that does not measure
 * it lands in the middle of the next command.
 */
static size_t aiLength(const buf *f, size_t at)
{
	const uint32_t op = f->v[at];
	size_t end;

	if (op >= GEAI_NUM_COMMANDS) {
		return 0;
	}
	if (g_GeAiCommands[op].len) {
		return g_GeAiCommands[op].len;
	}
	for (end = at + 1; end < f->n && f->v[end]; ++end) {
		;
	}
	return end - at + 1;
}

/** One GoldenEye AI list as Perfect Dark bytecode (geaitable.h). */
static void writeSoloAilist(const buf *f, size_t at, size_t numpads, buf *out, struct solostats *st)
{
	while (at < f->n) {
		const uint32_t op = f->v[at];
		const size_t len = aiLength(f, at);
		const struct geaicmd *cmd;
		uint32_t vals[GEAI_MAX_ARGS];
		size_t o;

		if (!len || at + len > f->n) {
			break;
		}

		cmd = &g_GeAiCommands[op];

		if (cmd->pd < 0) {
			st->aidropped++;
		} else {
			// GoldenEye's own arguments, in order and each at its own width;
			// a pad moves the way a record's does, and a text id becomes one
			// of the mission's own bank
			o = at + 1;
			for (int i = 0; i < cmd->numge && i < GEAI_MAX_ARGS; ++i) {
				uint32_t v = 0;
				for (int k = 0; k < cmd->gewidth[i]; ++k) {
					v = (v << 8) | f->v[o + k];
				}
				o += cmd->gewidth[i];
				if (cmd->gepad & (1u << i)) {
					v = padNum(v, numpads, 0);
				} else if (cmd->getext & (1u << i)) {
					v = soloTextId(v);
				}

				vals[i] = v;
			}

			bufU16(out, (uint32_t)cmd->pd);

			for (int i = 0; i < cmd->numargs; ++i) {
				const struct geaiarg *a = &cmd->args[i];
				const uint32_t v = a->from < 0 ? a->value : vals[a->from];

				for (int k = a->width - 1; k >= 0; --k) {
					bufU8(out, (v >> (8 * k)) & 0xff);
				}
			}

			st->aikept++;
		}

		at += len;

		if (op == 4) {   // EndList
			return;
		}
	}

	bufU16(out, 4);
}

static void writeSoloAilists(const buf *f, size_t at, size_t numpads, buf *head, buf *code, struct solostats *st)
{
	const uint32_t start = be32(f->v, 20);
	size_t n = 0, pos;

	if (!start) {
		bufU32(head, 0);
		bufU32(head, 0);
		return;
	}

	for (size_t o = start; o + 8 <= f->n && (be32(f->v, o) || be32(f->v, o + 4)); o += 8) {
		++n;
	}

	pos = at + 8 * (n + 1);

	for (size_t i = 0; i < n; ++i) {
		const size_t o = start + 8 * i;
		const size_t before = code->n;

		writeSoloAilist(f, be32(f->v, o), numpads, code, st);
		bufU32(head, (uint32_t)pos);
		bufU32(head, be32(f->v, o + 4));
		pos += code->n - before;
	}

	bufU32(head, 0);
	bufU32(head, 0);
}

/** A GoldenEye solo setup as a Perfect Dark one (gesolo.py's convert()). */
static buf writeSoloSetup(const buf *f, size_t numpads, uint8_t *models, struct solostats *st)
{
	buf intro = writeSoloIntro(f);
	buf props = writeSoloProps(f, numpads, models, st);
	buf paths = {0}, pathpads = {0}, ailists = {0}, aicode = {0}, out = {0};
	const size_t introat = 0x20;
	const size_t propsat = introat + intro.n;
	const size_t pathsat = propsat + props.n;
	size_t aiat;

	writeSoloPaths(f, pathsat, &paths, &pathpads);
	aiat = pathsat + paths.n + pathpads.n;
	writeSoloAilists(f, aiat, numpads, &ailists, &aicode, st);

	bufU32(&out, 0);
	bufU32(&out, 0);
	bufU32(&out, 0);
	bufU32(&out, (uint32_t)introat);
	bufU32(&out, (uint32_t)propsat);
	bufU32(&out, (uint32_t)pathsat);
	bufU32(&out, (uint32_t)aiat);
	bufU32(&out, 0);
	bufPut(&out, intro.v, intro.n);
	bufPut(&out, props.v, props.n);
	bufPut(&out, paths.v, paths.n);
	bufPut(&out, pathpads.v, pathpads.n);
	bufPut(&out, ailists.v, ailists.n);
	bufPut(&out, aicode.v, aicode.n);
	bufPad(&out, 16);
	return rzip1173(out.v, out.n);
}

/* ------------------------------------------------------------------------ */
/* prop models (gemodelconv.py) */

struct node {
	uint32_t at;
	uint32_t type;
	uint32_t rodata;
	uint32_t parent;
	uint32_t child;
	uint32_t next;
};

struct gdlfix {
	uint32_t src;
	u32s words;
	size_t fixup;
};

typedef VEC(struct node) nodes;

static void modelWalk(const buf *d, uint32_t o, uint32_t parent, nodes *out, int depth)
{
	if (depth > 64) {
		fail("a model nested too deep");
	}

	while (o) {
		struct node n;
		uint32_t child, next;

		if ((size_t)o + 24 > d->n) {
			fail("a model node outside its file");
		}
		child = be32(d->v, o + 20);
		next = be32(d->v, o + 12);
		n.at = o;
		n.type = be16(d->v, o);
		n.rodata = be32(d->v, o + 4);
		n.parent = parent;
		n.child = child ? child - SEG_MODEL : 0;
		n.next = next ? next - SEG_MODEL : 0;
		VECPUSH(*out, n);
		if (n.child) {
			modelWalk(d, n.child, o, out, depth + 1);
		}
		o = n.next;
	}
}

static size_t bufPutAligned(buf *w, const uint8_t *data, size_t n, size_t align)
{
	size_t at;
	bufPad(w, align);
	at = w->n;
	bufPut(w, data, n);
	return at;
}

static void modelLists(const buf *d, uint32_t vtxptr, const uint32_t *lists, int nlists, buf *w, int fours,
		size_t *vat, size_t *nv, size_t *cat, size_t *nc, u32s *wordsof, int *haswords)
{
	outvtxs newv = {0};
	u32s cols = {0};

	for (int li = 0; li < nlists; ++li) {
		size_t o;
		haswords[li] = 0;
		memset(&wordsof[li], 0, sizeof(wordsof[li]));

		if (!lists[li]) {
			continue;
		}

		haswords[li] = 1;
		o = lists[li] - SEG_MODEL;

		for (int step = 0; step < 100000; ++step) {
			uint32_t w0, w1, op;

			if (o + 8 > d->n) {
				fail("a model list runs off its file");
			}

			w0 = be32(d->v, o);
			w1 = be32(d->v, o + 4);
			o += 8;
			op = w0 >> 24;

			if (op == 0x04) {
				const uint32_t n = ((w0 >> 20) & 0xf) + 1;
				const uint32_t v0 = (w0 >> 16) & 0xf;
				const int64_t src = (w1 >> 24) == 0x04 ? (int64_t)((w1 & 0xffffff) / 16) : floordiv((int64_t)w1 - (int64_t)vtxptr, 16);
				const uint32_t start = (uint32_t)newv.n;

				for (uint32_t i = 0; i < n; ++i) {
					const int64_t at = (int64_t)vtxptr - SEG_MODEL + 16 * (src + i);
					struct outvtx v;
					if (at < 0 || (size_t)at + 16 > d->n) {
						fail("a model list loads a vertex outside its file");
					}
					v.x = bes16(d->v, at);
					v.y = bes16(d->v, at + 2);
					v.z = bes16(d->v, at + 4);
					v.c = (uint8_t)(i << 2);
					v.s = bes16(d->v, at + 8);
					v.t = bes16(d->v, at + 10);
					VECPUSH(newv, v);
					VECPUSH(cols, be32(d->v, at + 12));
				}

				if (!fours) {
					VECPUSH(wordsof[li], (0x07u << 24) | ((((n - 1) << 2) & 0xff) << 16) | (n * 4));
					VECPUSH(wordsof[li], 0x06000000u | (start * 4));
				}
				VECPUSH(wordsof[li], (0x04u << 24) | ((n - 1) << 20) | (v0 << 16) | (n * 12));
				VECPUSH(wordsof[li], 0x04000000u | (start * 12));
			} else {
				if (op == 0xc0) {
					w1 = (w1 & ~0xfffu) | texRemap(w1 & 0xfff);
					if ((w0 & 7) == 1) {
						w1 = (w1 & ~0xfff000u) | (texRemap((w1 >> 12) & 0xfff) << 12);
					}
				}
				VECPUSH(wordsof[li], w0);
				VECPUSH(wordsof[li], w1);
				if (op == 0xb8) {
					break;
				}
			}
		}
	}

	while (newv.n % (fours ? 4 : 2)) {
		struct outvtx v = { 0 };
		if (newv.n) {
			v.x = newv.v[newv.n - 1].x;
			v.y = newv.v[newv.n - 1].y;
			v.z = newv.v[newv.n - 1].z;
		}
		VECPUSH(newv, v);
		VECPUSH(cols, 0);
	}

	bufPad(w, 8);
	*vat = w->n;
	for (size_t k = 0; k < newv.n; ++k) {
		bufU16(w, (uint16_t)newv.v[k].x);
		bufU16(w, (uint16_t)newv.v[k].y);
		bufU16(w, (uint16_t)newv.v[k].z);
		bufU8(w, 0);
		bufU8(w, newv.v[k].c);
		bufU16(w, (uint16_t)newv.v[k].s);
		bufU16(w, (uint16_t)newv.v[k].t);
	}
	bufPad(w, 8);
	*cat = w->n;
	for (size_t k = 0; k < cols.n; ++k) {
		bufU32(w, cols.v[k]);
	}
	*nv = newv.n;
	*nc = cols.n;
}

static int cmpGdl(const void *pa, const void *pb)
{
	const struct gdlfix *a = pa, *b = pb;
	size_t n;

	if (a->src != b->src) {
		return a->src < b->src ? -1 : 1;
	}

	// equal lists compare word pair by word pair, a shorter prefix first
	n = a->words.n < b->words.n ? a->words.n : b->words.n;
	for (size_t i = 0; i < n; ++i) {
		if (a->words.v[i] != b->words.v[i]) {
			return a->words.v[i] < b->words.v[i] ? -1 : 1;
		}
	}
	if (a->words.n != b->words.n) {
		return a->words.n < b->words.n ? -1 : 1;
	}
	return a->fixup < b->fixup ? -1 : a->fixup > b->fixup;
}

static buf modelConvert(int32_t num, uint8_t *images, double *scale)
{
	const struct prop *p;
	buf d, w = {0};
	uint32_t textab, root;
	nodes nds = {0};
	VEC(struct gdlfix) gdls = {0};
	size_t texat, partsat, nodesat;
	uint32_t *switches;
	buf rows = {0};

	if (num < 0 || num >= NUM_PROPS) {
		fail("model %d is not one of GoldenEye's props", num);
	}

	p = &g_Props[num];
	*scale = p->scale;
	d = romFile(p->file);
	textab = 4 * (uint32_t)p->numswitches;
	root = textab + 12 * (uint32_t)p->numtextures;

	if (root + 24 > d.n) {
		fail("%s is shorter than its header says", p->file);
	}

	switches = gcAlloc((p->numswitches + 1) * sizeof(*switches));
	for (int i = 0; i < p->numswitches; ++i) {
		switches[i] = be32(d.v, 4 * i);
	}

	modelWalk(&d, root, 0, &nds, 0);

	#define ADDR(fileoff) ({ uint32_t _r = 0, _o = (fileoff); \
		for (size_t _i = 0; _i < nds.n; ++_i) if (nds.v[_i].at == _o) { _r = SEG_MODEL + (uint32_t)nodesat + 0x18 * (uint32_t)_i; break; } _r; })
	#define RELOC_NODE(ptr) ({ uint32_t _p = (ptr); _p ? ADDR(_p - SEG_MODEL) : 0; })
	#define RELOC_TEX(ptr) ({ uint32_t _p = (ptr); _p ? SEG_MODEL + (uint32_t)texat + (_p - SEG_MODEL - textab) : 0; })

	bufZeros(&w, 0x1c);

	bufPut(&rows, d.v + textab, root - textab);
	for (int i = 0; i < p->numtextures; ++i) {
		const uint32_t image = be32(rows.v, 12 * i);
		set32(rows.v, 12 * i, texRemap(image));
		setAdd(images, image);
	}
	texat = bufPutAligned(&w, rows.v, rows.n, 4);

	{
		buf z = {0};
		bufZeros(&z, 6 * (size_t)p->numswitches);
		partsat = bufPutAligned(&w, z.v, z.n, 4);
		z.n = 0;
		bufZeros(&z, 0x18 * nds.n);
		nodesat = bufPutAligned(&w, z.v, z.n, 4);
	}

	for (size_t i = 0; i < nds.n; ++i) {
		struct node *n = &nds.v[i];
		const uint32_t ro = n->rodata - SEG_MODEL;
		size_t rat;
		buf rec = {0};

		#define NEED(len) if ((size_t)ro + (len) > d.n) fail("%s: a node's data runs off the file", p->file)

		switch (n->type) {
		case 0x01:
			// a vehicle's header: a position node on the same matrix at the origin
			NEED(4);
			bufF32(&rec, 0);
			bufF32(&rec, 0);
			bufF32(&rec, 0);
			bufU16(&rec, be16(d.v, ro));
			bufU16(&rec, be16(d.v, ro + 2));
			bufU16(&rec, 0xffff);
			bufU16(&rec, 0xffff);
			bufF32(&rec, p->radius);
			rat = bufPutAligned(&w, rec.v, rec.n, 4);
			n->type = 0x02;
			break;
		case 0x02:
			NEED(0x14);
			bufPut(&rec, d.v + ro, 0x14);
			bufF32(&rec, p->radius);
			rat = bufPutAligned(&w, rec.v, rec.n, 4);
			break;
		case 0x08:
		case 0x12: {
			const size_t len = n->type == 0x08 ? 0x10 : 0x08;
			const size_t at = n->type == 0x08 ? 8 : 0;
			NEED(len);
			bufPut(&rec, d.v + ro, len);
			set32(rec.v, at, RELOC_NODE(be32(rec.v, at)));
			rat = bufPutAligned(&w, rec.v, rec.n, 4);
			break;
		}
		case 0x09:
			NEED(0x24);
			bufPut(&rec, d.v + ro, 0x24);
			set32(rec.v, 0x18, RELOC_NODE(be32(rec.v, 0x18)));
			set32(rec.v, 0x1c, RELOC_NODE(be32(rec.v, 0x1c)));
			rat = bufPutAligned(&w, rec.v, rec.n, 4);
			break;
		case 0x0a:
			NEED(0x1c);
			rat = bufPutAligned(&w, d.v + ro, 0x1c, 4);
			break;
		case 0x0c:
			NEED(0x28);
			bufPut(&rec, d.v + ro, 0x28);
			set32(rec.v, 0x18, RELOC_TEX(be32(rec.v, 0x18)));
			set32(rec.v, 0x24, 0);
			rat = bufPutAligned(&w, rec.v, rec.n, 4);
			break;
		case 0x15:
			NEED(0x14);
			rat = bufPutAligned(&w, d.v + ro, 0x14, 4);
			break;
		case 0x04:
		case 0x18: {
			uint32_t lists[2], vtx;
			int32_t mode;
			size_t vat, nv, cat, nc;
			u32s words[2];
			int has[2];

			NEED(n->type == 0x04 ? 0x14 : 0x1a);
			lists[0] = be32(d.v, ro);
			lists[1] = be32(d.v, ro + 4);
			if (n->type == 0x04) {
				vtx = be32(d.v, ro + 12);
				mode = be16(d.v, ro + 0x12);
			} else {
				vtx = be32(d.v, ro + 8);
				mode = bes16(d.v, ro + 0x18);
			}
			modelLists(&d, vtx, lists, 2, &w, 0, &vat, &nv, &cat, &nc, words, has);
			if (nv > 32767 || mode < -32768 || mode > 32767) {
				fail("%s: a list with too many vertices", p->file);
			}
			bufU32(&rec, 0);
			bufU32(&rec, 0);
			bufU32(&rec, SEG_MODEL + (uint32_t)cat);
			bufU32(&rec, SEG_MODEL + (uint32_t)vat);
			bufU16(&rec, (uint32_t)nv);
			bufU16(&rec, (uint16_t)mode);
			bufU16(&rec, 0);
			bufU16(&rec, (uint32_t)nc);
			rat = bufPutAligned(&w, rec.v, rec.n, 4);
			for (int k = 0; k < 2; ++k) {
				if (has[k]) {
					struct gdlfix g = { lists[k], words[k], rat + 4 * k };
					VECPUSH(gdls, g);
				}
			}
			n->type = 0x18;
			break;
		}
		case 0x16: {
			uint32_t lists[1], vtx;
			size_t vat, nv, cat, nc;
			u32s words[1];
			int has[1];

			NEED(12);
			vtx = be32(d.v, ro + 4);
			lists[0] = be32(d.v, ro + 8);
			if (!lists[0]) {
				fail("%s: a muzzle flash with no list", p->file);
			}
			modelLists(&d, vtx, lists, 1, &w, 1, &vat, &nv, &cat, &nc, words, has);
			// a standing prop never fires, so it draws no stars
			bufU32(&rec, 0);
			bufU32(&rec, SEG_MODEL + (uint32_t)vat);
			bufU32(&rec, 0);
			bufU32(&rec, SEG_MODEL + (uint32_t)cat);
			rat = bufPutAligned(&w, rec.v, rec.n, 4);
			{
				struct gdlfix g = { lists[0], words[0], rat + 8 };
				VECPUSH(gdls, g);
			}
			break;
		}
		default:
			fail("%s: node type %#x", p->file, n->type);
			rat = 0;
		}
		#undef NEED

		{
			const size_t at = nodesat + 0x18 * i;
			set16(w.v, at, n->type);
			set16(w.v, at + 2, 0);
			set32(w.v, at + 4, SEG_MODEL + (uint32_t)rat);
			set32(w.v, at + 8, ADDR(n->parent));
			set32(w.v, at + 12, ADDR(n->next));
			set32(w.v, at + 16, 0);
			set32(w.v, at + 20, ADDR(n->child));
		}
	}

	qsort(gdls.v, gdls.n, sizeof(*gdls.v), cmpGdl);

	for (size_t i = 0; i < gdls.n; ++i) {
		buf gw = {0};
		size_t at;
		for (size_t k = 0; k < gdls.v[i].words.n; ++k) {
			bufU32(&gw, gdls.v[i].words.v[k]);
		}
		at = bufPutAligned(&w, gw.v, gw.n, 8);
		set32(w.v, gdls.v[i].fixup, SEG_MODEL + (uint32_t)at);
	}

	// prev pointers
	for (size_t i = 0; i < nds.n; ++i) {
		if (nds.v[i].next) {
			const uint32_t a = ADDR(nds.v[i].next);
			if (!a) {
				fail("%s: a node's next is not a node", p->file);
			}
			set32(w.v, a - SEG_MODEL + 0x10, SEG_MODEL + (uint32_t)nodesat + 0x18 * (uint32_t)i);
		}
	}

	for (int i = 0; i < p->numswitches; ++i) {
		set32(w.v, partsat + 4 * i, RELOC_NODE(switches[i]));
		set16(w.v, partsat + 4 * p->numswitches + 2 * i, (uint32_t)i);
	}

	set32(w.v, 0, SEG_MODEL + (uint32_t)nodesat);
	set32(w.v, 4, 2);
	set32(w.v, 8, p->numswitches ? SEG_MODEL + (uint32_t)partsat : 0);
	set16(w.v, 12, (uint32_t)p->numswitches);
	set16(w.v, 14, (uint32_t)p->nummatrices);
	setf32(w.v, 16, p->radius);
	set16(w.v, 20, 0);
	set16(w.v, 22, (uint32_t)p->numtextures);
	set32(w.v, 24, SEG_MODEL + (uint32_t)texat);
	bufPad(&w, 16);

	#undef ADDR
	#undef RELOC_NODE
	#undef RELOC_TEX

	return w;
}

/* ------------------------------------------------------------------------ */
/* output */

static int makeDir(const char *path)
{
#ifdef _WIN32
	return _mkdir(path);
#else
	return mkdir(path, 0755);
#endif
}

static void makeDirs(const char *path)
{
	char tmp[1024];

	snprintf(tmp, sizeof(tmp), "%s", path);

	for (char *p = tmp + 1; *p; ++p) {
		if (*p == '/' || *p == '\\') {
			const char c = *p;
			*p = '\0';
			makeDir(tmp);
			*p = c;
		}
	}

	makeDir(tmp);
}

static void writeFile(const char *outdir, const char *rel, const uint8_t *data, size_t len)
{
	char path[1024];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", outdir, rel);
	f = fopen(path, "wb");

	if (!f || fwrite(data, 1, len, f) != len) {
		if (f) {
			fclose(f);
		}
		fail("cannot write %s", path);
	}

	if (fclose(f)) {
		fail("cannot write %s", path);
	}
}

struct textbuf {
	char *s;
	size_t n, cap;
};

static void textf(struct textbuf *t, const char *fmt, ...)
{
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);

	if (t->n + len + 1 > t->cap) {
		size_t cap = t->cap ? t->cap : 4096;
		char *s;
		while (cap < t->n + len + 1) {
			cap *= 2;
		}
		s = realloc(t->s, cap);
		if (!s) {
			fail("out of memory");
		}
		t->s = s;
		t->cap = cap;
	}

	va_start(ap, fmt);
	vsnprintf(t->s + t->n, len + 1, fmt, ap);
	va_end(ap);
	t->n += len;
}

// a fog row as the maps block's `fog` string (modloader.c)
static void fogValue(struct textbuf *t, double *r, const double *offset)
{
	r[13] -= offset[1];
	r[23] -= offset[1];

	#define I(k) (long)rnd(r[(k) - 1])
	textf(t, "%ld %ld %ld %ld %ld %ld %ld %02lx%02lx%02lx %ld %ld %ld %02lx%02lx%02lx %ld %ld %ld %02lx%02lx%02lx %ld",
		I(1), I(2), I(3), I(4), I(5), I(8), I(9), I(10), I(11), I(12),
		I(13), I(14), I(15), I(17), I(18), I(19), I(20), I(24), I(25), I(27), I(28), I(29), I(30));
	#undef I
}

/* ------------------------------------------------------------------------ */

static volatile int g_Progress;

int geconvertProgress(void)
{
	return g_Progress;
}

int geconvertTotal(void)
{
	return (int)NUM_LEVELS + 2;
}

static void noteDefault(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
}

static void (*g_Note)(const char *msg) = noteDefault;

static void note(const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	g_Note(msg);
}

void geconvertSetLog(void (*fn)(const char *msg))
{
	g_Note = fn ? fn : noteDefault;
}

int geconvertRun(uint8_t *rom, size_t romlen, const char *outdir, char *err, size_t errlen)
{
	static uint8_t alltex[SETBITS / 8];
	static uint8_t allmodels[SETBITS / 8];
	struct textbuf maps = {0}, missions = {0}, modellines = {0}, config = {0};
	volatile int ok = 0;
	char sub[1024];

	g_Progress = 0;
	memset(alltex, 0, sizeof(alltex));
	memset(allmodels, 0, sizeof(allmodels));
	g_FailMsg[0] = '\0';

	if (setjmp(g_Fail)) {
		goto done;
	}

	if (!geconvertIsGoldenEyeUs(rom, romlen)) {
		fail("not GoldenEye 007 (US)");
	}

	g_Rom = rom;
	g_RomLen = romlen;
	romOpen();

	snprintf(sub, sizeof(sub), "%s/files/bgdata", outdir);
	makeDirs(sub);
	snprintf(sub, sizeof(sub), "%s/textures", outdir);
	makeDirs(sub);

	for (size_t li = 0; li < NUM_LEVELS; ++li) {
		const struct level *lv = &g_Levels[li];
		static uint8_t leveltex[SETBITS / 8];
		struct bg bg;
		buf bgfile, stanfile, setupfile, gedata, bgdata, tilesdata, padsdata, mpsetup;
		tiles stan;
		double offset[3], mn[3], mx[3], fog[30];
		struct setup setup, mpsetupsrc;
		int havemp = lv->mp != NULL, numlights, walls, first = 1;
		struct roomfinder rf;
		padrecs boundpads;
		s32s bikepads;
		char stem[64], rel[256];

		// everything before this level's first allocation is the ROM's, kept
		const size_t keep = g_NumAllocs;

		memset(leveltex, 0, sizeof(leveltex));

		snprintf(stem, sizeof(stem), "%s_all_p", lv->bg);
		bgfile = romFile(stem);
		bgRead(&bgfile, &bg);
		snprintf(stem, sizeof(stem), "%s_all_p_stanZ", lv->stan);
		stanfile = romFile(stem);
		stan = stanRead(&stanfile);

		// the level is moved so the middle of its walkable area is the origin
		for (size_t i = 0; i < stan.n; ++i) {
			for (int k = 0; k < stan.v[i].npts; ++k) {
				for (int c = 0; c < 3; ++c) {
					const double v = (double)stan.v[i].pts[k][c] / lv->levelscale;
					if (first || v < mn[c]) mn[c] = v;
					if (first || v > mx[c]) mx[c] = v;
				}
				first = 0;
			}
		}
		if (first) {
			fail("%s has no tiles", lv->name);
		}
		for (int c = 0; c < 3; ++c) {
			offset[c] = rnd((mn[c] + mx[c]) / 2);
		}

		bgdata = writeBg(&bg, lv->levelscale, offset, leveltex, &numlights,
				roomTileBounds(&stan, bg.numrooms, lv->levelscale, offset));
		tilesdata = writeTiles(&stan, bg.numrooms, lv->levelscale, offset, &walls);

		setupfile = romFile(lv->solo ? lv->solo : lv->mp);
		setupRead(&setupfile, &setup);
		if (havemp) {
			buf mpfile = romFile(lv->mp);
			setupRead(&mpfile, &mpsetupsrc);
			setup = mpsetupsrc;
		}

		roomsInit(&rf, &stan, lv->levelscale, offset, &bg);
		gedata = romFile(lv->mp ? lv->mp : lv->solo);
		boundpads = boundPads(&gedata, lv->levelscale, offset);
		padsdata = writePads(&setup, lv->levelscale, offset, &rf, &boundpads);
		bikepads = bikePads(lv, &setup, &stan, &bg, &gedata);
		mpsetup = writeMpSetup(&setup, havemp ? &mpsetupsrc : NULL, &stan, &bg, &gedata, &bikepads, allmodels);

		snprintf(rel, sizeof(rel), "files/bgdata/bg_gx%s.seg", lv->key);
		writeFile(outdir, rel, bgdata.v, bgdata.n);
		snprintf(rel, sizeof(rel), "files/bgdata/bg_gx%s_tilesZ", lv->key);
		writeFile(outdir, rel, tilesdata.v, tilesdata.n);
		snprintf(rel, sizeof(rel), "files/bgdata/bg_gx%s_padsZ", lv->key);
		writeFile(outdir, rel, padsdata.v, padsdata.n);
		snprintf(rel, sizeof(rel), "files/Ump_setupgx%sZ", lv->key);
		writeFile(outdir, rel, mpsetup.v, mpsetup.n);

		textf(&maps, "%s  map \"%s\" bg \"bgdata/bg_gx%s.seg\" tiles \"bgdata/bg_gx%s_tilesZ\" pads \"bgdata/bg_gx%s_padsZ\" mpsetup \"Ump_setupgx%sZ\"",
			maps.n ? "\n" : "", lv->name, lv->key, lv->key, lv->key, lv->key);
		if (romFogRow(lv->levelid, fog)) {
			textf(&maps, " fog \"");
			fogValue(&maps, fog, offset);
			textf(&maps, "\"");
		}

		note("geconvert: %s: %d rooms, %d portals, %d tiles (+%d walls), %d pads, %d lights",
			lv->name, bg.numrooms, (int)bg.portals.n, (int)stan.n, walls, (int)setup.pads.n, numlights);

		// and the solo mission on this level, where there is one: its own pads
		// (a mission's pad list is not the arena's) and its own setup, over the
		// same rooms and tiles
		for (size_t mi = 0; mi < NUM_MISSIONS; ++mi) {
			buf mfile, mpads, mprops;
			struct setup msetup;
			padrecs mbound;
			struct solostats st = {0};

			if (strcmp(g_Missions[mi].key, lv->key) != 0) {
				continue;
			}

			mfile = romFile(g_Missions[mi].setup);
			setupRead(&mfile, &msetup);
			mbound = boundPads(&mfile, lv->levelscale, offset);
			mpads = writePads(&msetup, lv->levelscale, offset, &rf, &mbound);
			mprops = writeSoloSetup(&mfile, msetup.pads.n, allmodels, &st);

			snprintf(rel, sizeof(rel), "files/bgdata/bg_gs%s_padsZ", lv->key);
			writeFile(outdir, rel, mpads.v, mpads.n);
			snprintf(rel, sizeof(rel), "files/Usetupgs%sZ", lv->key);
			writeFile(outdir, rel, mprops.v, mprops.n);

			textf(&missions, "%s  mission %d \"%s\" bg \"bgdata/bg_gx%s.seg\" tiles \"bgdata/bg_gx%s_tilesZ\""
					" pads \"bgdata/bg_gs%s_padsZ\" setup \"Usetupgs%sZ\"",
				missions.n ? "\n" : "", (int)mi, g_Missions[mi].name, lv->key, lv->key, lv->key, lv->key);
			if (romFogRow(lv->levelid, fog)) {
				textf(&missions, " fog \"");
				fogValue(&missions, fog, offset);
				textf(&missions, "\"");
			}

			note("geconvert: %s: mission %d, %d props (+%d left out), %d ai commands (+%d)",
				g_Missions[mi].name, (int)mi, st.props, st.dropped, st.aikept, st.aidropped);
		}

		for (size_t i = 0; i < sizeof(leveltex); ++i) {
			alltex[i] |= leveltex[i];
		}

		// let go of the level's allocations
		for (size_t i = keep; i < g_NumAllocs; ++i) {
			free(g_Allocs[i]);
		}
		g_NumAllocs = keep;

		++g_Progress;
	}

	// GE-X Plus's menus are GoldenEye's own folder screens: the folder is a prop
	// model, the cursor and the stage pictures global images, and the fonts, the
	// music and the title screen's strings are copied as GoldenEye stores them
	{
		const size_t keep = g_NumAllocs;
		buf title;

		setAdd(allmodels, MENU_FOLDER_MODEL);
		for (size_t i = 0; i < sizeof(g_MenuImages) / sizeof(g_MenuImages[0]); ++i) {
			for (uint32_t n = 0; n < g_MenuImages[i].count; ++n) {
				setAdd(alltex, g_MenuImages[i].first + n);
			}
		}
		snprintf(sub, sizeof(sub), "%s/menu", outdir);
		makeDirs(sub);

		for (size_t i = 0; i < sizeof(g_MenuRaw) / sizeof(g_MenuRaw[0]); ++i) {
			char rel[64];

			if (g_MenuRaw[i].at + g_MenuRaw[i].size > g_RomLen) {
				fail("%s runs off the ROM", g_MenuRaw[i].name);
			}
			snprintf(rel, sizeof(rel), "menu/%s", g_MenuRaw[i].name);
			writeFile(outdir, rel, g_Rom + g_MenuRaw[i].at, g_MenuRaw[i].size);
		}

		title = romFile("LtitleE");
		writeFile(outdir, "menu/LtitleE", title.v, title.n);

		// and the solo missions' briefings, with the text bank each one indexes
		for (size_t i = 0; i < sizeof(g_MenuText) / sizeof(g_MenuText[0]); ++i) {
			const char *names[2] = { g_MenuText[i].brief, g_MenuText[i].lang };

			for (int j = 0; j < 2; ++j) {
				char rel[64];
				buf f = romFile(names[j]);

				snprintf(rel, sizeof(rel), "menu/%s", names[j]);
				writeFile(outdir, rel, f.v, f.n);
			}
		}

		for (size_t i = keep; i < g_NumAllocs; ++i) {
			free(g_Allocs[i]);
		}
		g_NumAllocs = keep;
	}

	// the remake's prop models: GoldenEye's own, converted
	{
		int count = 0;
		for (uint32_t num = 0; num < SETBITS; ++num) {
			const size_t keep = g_NumAllocs;
			double scale;
			buf data, z;
			char fname[32], rel[64];

			if (!setHas(allmodels, num)) {
				continue;
			}

			data = modelConvert((int32_t)num, alltex, &scale);
			z = rzip1173(data.v, data.n);
			snprintf(fname, sizeof(fname), "Pgx%03uZ", num);
			snprintf(rel, sizeof(rel), "files/%s", fname);
			writeFile(outdir, rel, z.v, z.n);
			textf(&modellines, "%s  %u \"%s\" %ld", modellines.n ? "\n" : "", num, fname, (long)rnd(scale * 4096));
			++count;

			for (size_t i = keep; i < g_NumAllocs; ++i) {
				free(g_Allocs[i]);
			}
			g_NumAllocs = keep;
		}
		note("geconvert: %d models", count);
		++g_Progress;
	}

	{
		int count = 0, missing = 0;
		for (uint32_t num = 0; num < SETBITS; ++num) {
			const uint8_t *data;
			size_t len = 0;
			char rel[64];

			if (!setHas(alltex, num)) {
				continue;
			}

			data = romImage((int32_t)num, &len);
			if (!data || len > MAX_TEXTURE_SIZE) {
				++missing;
				continue;
			}

			snprintf(rel, sizeof(rel), "textures/%04x.bin", texRemap(num));
			writeFile(outdir, rel, data, len);
			++count;
		}
		note("geconvert: %d textures, %d missing", count, missing);
	}

	textf(&config, "# GoldenEye levels converted from the GoldenEye ROM (port/src/geconvert.c, tools/geconvert)\nmaps {\n%s\n}\n",
		maps.s ? maps.s : "");
	textf(&config, "# GoldenEye's prop models: slot (GoldenEye model number), file, scale (4096 = 1.0)\nmodels {\n%s\n}\n",
		modellines.s ? modellines.s : "");
	textf(&config, "# GoldenEye's solo missions, in its own mission order (port/src/gexfront.c)\nmissions {\n%s\n}\n",
		missions.s ? missions.s : "");
	writeFile(outdir, "modconfig.txt", (const uint8_t *)config.s, config.n);
	++g_Progress;
	ok = 1;

done:
	if (!ok && err && errlen) {
		snprintf(err, errlen, "%s", g_FailMsg[0] ? g_FailMsg : "failed");
	}

	free(maps.s);
	free(modellines.s);
	free(config.s);
	gcFreeAll();
	g_Rom = NULL;
	g_Data = NULL;
	g_Files = NULL;
	return ok;
}

#ifdef GECONVERT_MAIN
int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *rom;
	long len;
	char err[256];

	if (argc != 3) {
		fprintf(stderr, "usage: %s ROM OUTDIR\n", argv[0]);
		return 2;
	}

	f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	rom = malloc(len);
	if (!rom || fread(rom, 1, len, f) != (size_t)len) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	fclose(f);

	if (!geconvertRun(rom, (size_t)len, argv[2], err, sizeof(err))) {
		fprintf(stderr, "geconvert: %s\n", err);
		return 1;
	}

	free(rom);
	return 0;
}
#endif
