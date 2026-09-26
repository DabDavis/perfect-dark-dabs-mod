/**
 * GoldenEye's levels as the GoldenEye remake's arenas, converted from the
 * player's own GoldenEye ROM (US): GoldenEye's data cannot be shipped, so GE-X
 * Plus is built on the player's machine from the cartridge they have.
 *
 * The output is a maps-only mod directory the Stage Loader registers
 * (CLAUDE-notes/mods.md "The Stage Loader"): per level files/bgdata/bg_gxNAME.seg,
 * _tilesZ, _padsZ and files/Ump_setupgxNAMEZ, the remake's prop models as
 * files/PgxNNNZ, GoldenEye's textures in textures/, GE Plus's menu fonts
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
#include "geanimtable.h"
#include "gemonitortable.h"

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
#define CHRS_AT 0x1d080
#define NUM_CHRS 80

/**
 * `gitem_structs`, the models a hand holds: 56-byte rows from 0x12b94, the
 * header and the file name its first two words, the rest the weapon's own
 * numbers. Only one of them is converted - the watch, which the remake's
 * pause puts on the player's own wrist (gewatch.c) - and its row is checked
 * by name, since a row that moved would otherwise convert whatever is there.
 */
#define ITEMS_AT 0x12b94
#define ITEM_ROW 56
#define NUM_ITEMS 120
#define ITEM_WATCH 56
#define ITEM_CONTROLLER 0x55
#define ITEM_WATCH_FILE "GwatchidentifierZ"

// the animations: two segments of their own, raw in the ROM. A record in the
// data one is a 0x14 header {entry, u16 numframes, u8 width, u8 loop,
// bitDescriptors, u16 joints, u16 bitsperframe, bitStream}, then its four
// root-motion descriptors {u16 bitoffset, u8 bitcount, pad, u16 base} and then
// their bit stream. The two pointers are relocated when the segment loads and
// say nothing here beyond their difference, which is the 24 bytes the four
// descriptors take. `entry` is where the animation's frames are in the entry
// segment, bitsperframe/8 bytes a frame of joint rotations, `width` bits a
// channel, in joint order.
#define ANIM_ENTRY_ROM 0x124ac0
#define ANIM_DATA_ROM 0x28e980
// GoldenEye's guard skeleton, which every character in the ROM has: the header
// node the animation plays on and fifteen parts, which is Perfect Dark's
// g_SkelChrJoints joint for joint
#define ANIM_PARTS 15

// GE Plus's menus (gexfront.c): the menu folder (PROP_WALLETBOND), its
// pictures, and the two fonts and the music, raw in the ROM
#define MENU_FOLDER_MODEL 278
#define MENU_TV_MODEL 75

// the crosshair cursor (IMAGE_CROSSHAIR1), the film strip's holes (IMAGE_DOT),
// a stage picture for every level (IMAGE_MP_ARCHIVES..TRAIN, TEMPLE..CAVES, RANDOM)
// and the character portraits' tiles (IMAGE_BROSNAN_UL..DALTON_LR,
// BORIS_UL..ODDJOB_LR, RANDOM_UL..LR, MISHKIN)
static const struct { uint32_t first, count; } g_MenuImages[] = {
	{ 2236, 1 }, { 2631, 1 }, { 2578, 20 }, { 2686, 4 }, { 2695, 1 },
	{ 2602, 16 }, { 2632, 40 }, { 2682, 4 }, { 2691, 4 },
	// and the HUD's ammunition pictures (gehud.c): IMAGE_ROCKETAMMO..SHOTAMMO,
	// IMAGE_9MMAMMO..PROXAMMO either side of the crosshair, TIMEAMMO, TANKAMMO
	{ 2161, 7 }, { 2231, 5 }, { 2238, 1 }, { 2464, 1 },
	// and its radar's disc (mpradarimages)
	{ 200, 1 },
};

static const struct { const char *name; size_t at, size; } g_MenuRaw[] = {
	{ "fontbankgothic.bin", 0x2e63f0, 0x24b0 },
	{ "fontzurichbold.bin", 0x2e88a0, 0x3540 },
	// its music: the instrument bank, and the sequence table ({u16 count, pad,
	// then u32 offset, u16 inflated, u16 zipped} a sequence) with the sequences
	{ "instrumentsctl", 0x3b4450, 0x43a0 },
	{ "instrumentstbl", 0x3b87f0, 0x60fa0 },
	{ "sequences", 0x419790, 0x1eed0 },
	// its sound effects: the sfx bank and its wave table, which a sound is
	// appended out of when something of GoldenEye's plays one (gesfx.c)
	{ "sfxctl", 0x2ebde0, 0x5bc0 },
	{ "sfxtbl", 0x2f19a0, 0xc2ab0 },
	// and the gun barrel's sniper-sight backdrop, the folder screens' 440x299
	// 8-bit background run-length encoded ({u16 w, u16 h, six bytes, then
	// count/value pairs}, rle.c's rle_expand_8bit)
	{ "introbg.bin", 0x2a4d50, 107890 },
};

// the gun barrel's blood, in the data segment rather than the ROM: the frames
// of the wash down the lens, each decoded from the last (blood_decrypt.c)
#define INTRO_BLOOD_AT 0xada0
#define INTRO_BLOOD_SIZE 2524

/**
 * The missions' text, in GoldenEye's mission order: each mission's briefing file
 * (front.h's struct BriefStruct - four paragraph text ids then ten objectives of
 * {text id, the difficulty it starts at}) and the level's own text bank, which
 * every id in that file indexes (a text id is bank * 0x400 + slot).
 */
/**
 * GE Plus's intro (port/src/geintro.c) is GoldenEye's own: the gun barrel, the
 * GoldenEye logo and the cast reel. It needs the logo's model, the guns the
 * cast holds and the PP7 Bond fires (PROP_CHRWPPK, 191), every character in the
 * ROM (all 80 - the cast is 33 bodies and a head pool of 33, and the rest cost
 * 430KB together), and these animations, whose records are where GoldenEye's
 * animation_data segment has them. menu/intro.bin names each one, so nothing
 * depends on the order.
 */
#define INTRO_LOGO_MODEL 277

static const uint32_t g_IntroGuns[] = {
	184, 185, 187, 188, 190, 191, 193, 195, 197, 204, 205, 207, 208, 210,
};

// GoldenEye's own model of each of its guns in a hand (player.c's
// getPropForHeldItem()), PROP_CHRKALASH to PROP_CHRROCKETLAUNCH less the
// thrown ones and the two rounds
static const uint32_t g_HeldGuns[] = {
	184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 197,
	204, 205, 206, 207, 208, 210, 211,
};

static const struct { const char *name; size_t at; } g_IntroAnims[] = {
	// the gun barrel: Bond walks in, turns and fires
	{ "bond_eye_walk", 0x292ac4 }, { "bond_eye_fire", 0x292c18 },
	// the cast reel: front.c's intro_animation_table, and idle for a character
	// whose animation is still loading
	{ "idle", 0x28e99c }, { "spotting_bond", 0x294690 },
	{ "fire_standing_draw_fast", 0x294bd4 }, { "fire_standing_draw_slow", 0x294cfc },
	{ "fire_step_right", 0x295188 }, { "fire_kneel_forward_fast", 0x2956d0 },
	{ "running_one_handed", 0x2960fc }, { "draw_and_stand_up", 0x296428 },
	{ "aim_left_right", 0x2965cc }, { "cock_and_turn_around", 0x296684 },
	{ "cock_turn_stand_up", 0x29688c }, { "draw_and_turn_around", 0x296934 },
	{ "drop_weapon_fight", 0x299af4 }, { "laughing", 0x29ad90 },
	{ "fire_hip_forward", 0x294fc4 }, { "fire_standing_left_fast", 0x295398 },
	{ "fire_kneel_left_fast", 0x295c84 }, { "draw_and_look_around", 0x296248 },
	{ "aim_left", 0x2992cc }, { "aim_right", 0x29935c },
	{ "conversation", 0x29962c }, { "conversation_listener", 0x29a900 },
	{ "conversation_cleaned", 0x29a5c0 },
};

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

// the watch's own banks (gewatch.c): LoptionsE is the solo watch's screens,
// LmpmenuE the multiplayer watch's, LgunE the guns' names and LpropobjE the
// pickups' - what its inventory page names a carried item by
static const char *g_WatchLang[] = { "LoptionsE", "LmpmenuE", "LgunE", "LpropobjE" };

const char *geconvertMissionLangFile(int mission)
{
	const int n = (int)(sizeof(g_MenuText) / sizeof(g_MenuText[0]));

	return mission >= 0 && mission < n ? g_MenuText[mission].lang : NULL;
}

#define MAX_TEXTURE_SIZE 4096
#define WALL_BELOW 50.0
#define WALL_ABOVE 400.0

/* The geo flags a floor tile and a wall carry (constants.h, GEOFLAG_*). Perfect
 * Dark reads a tile's flags for the question being asked and nothing else -
 * cdCollectGeoForCylFromList() takes a tile only where `geo->flags & geoflags` -
 * so a wall that carries GEOFLAG_WALL alone stops a body walking into it and is
 * not there at all to a line of sight (GEOFLAG_BLOCK_SIGHT, chrHasLosToChr) or
 * to a bullet (GEOFLAG_BLOCK_SHOOT). GoldenEye's own sight is its stan graph,
 * which walks from link to link (chrCanSeeBond's stanTestLineUnobstructed) and
 * so cannot cross an unlinked edge either: a wall standing on one blocks sight
 * and shots there in GoldenEye too, and the walls carry both. */
#define FLOOR_FLAGS (0x0001 | 0x0002 | 0x0008 | 0x0010)   /* FLOOR1 FLOOR2 SIGHT SHOOT */
#define WALL_FLAGS  (0x0004 | 0x0008 | 0x0010)            /* WALL SIGHT SHOOT */

/* A wall is raised round every unlinked tile edge, and GoldenEye's own walls
 * are the edge and nothing more: its collision walks out from the tile the
 * player stands on through the links alone (stan.c's sub_GAME_7F0B1DDC), so an
 * edge belonging to another storey's floor can never stop them. Perfect Dark's
 * is a quad in the world, so a wall raised WALL_ABOVE over a staircase's own
 * tiles stands in the air across the flight above it, and one dropped
 * WALL_BELOW under a ledge's own tiles stands in the way of a player walking
 * beneath. Every wall stops under the lowest walkable surface that passes over
 * it, and over the head of anyone standing on one that passes under it. */
#define WALL_HEADROOM 60.0 /* a surface this far from the edge is another
                            * floor, not the step or kerb the wall itself
                            * belongs to */
#define WALL_STEP 20.0     /* world units between the samples along an edge */
#define WALL_SIDE 4.0      /* the slack on the box a surface is looked for in */
#define WALL_REACH 30.0    /* the player's own radius: they stand this far from
                            * a wall, and on a slope that is lower ground than
                            * the surface over the wall itself */
#define WALL_HEAD 160.0    /* and their collision box reaches this far over the
                            * floor they stand on (playerGetBbox(): a chr's is
                            * less) */
#define WALL_CLEAR 2.0     /* the foot goes this much further, since the
                            * collision's own comparison against a tile's ymin
                            * is inclusive */
#define WALL_RISE 50.0     /* a wall's foot may be lifted this far over its own
                            * edge and still meet the box of a walker on its
                            * own tile. However deep a player crouches - and
                            * they crouch twice - playerGetBbox() holds their
                            * box at manground+30 to manground+80 at the least,
                            * and the deepest a chr ducks is chr->height 90
                            * over manground+20. A lift that would have to go
                            * further clears nobody, so it is not made at all:
                            * the surface under such a wall is a platform
                            * beside its own tile rather than a floor under it,
                            * and a player beside a platform belongs against
                            * its side */

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
	double pov;
	int32_t numswitches, nummatrices, numtextures;
	double radius;
	uint32_t skeleton, flags;
};

static struct prop g_Props[NUM_PROPS];
static struct prop g_Chrs[NUM_CHRS];
static struct prop g_Items[NUM_ITEMS];

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
/**
 * Faults in GoldenEye's own data, mended as the file is read (geconvert.py's
 * ROM_PATCHES, which is this). Each is the ROM's bytes and what they become,
 * and a file whose bytes are not the ROM's is left as it is. All are faults
 * the community found in the XBLA release's copy of the same data and mended
 * in its Community Edition; none is a change of design.
 */
static const struct {
	const char *stem;
	size_t at;
	uint8_t n;
	uint8_t old[4], new[4];
} g_RomPatches[] = {
	// Bunker ii: two tiles meeting on the stairs (room 0x14, y 93) are
	// unlinked from both sides, and a body stops at the edge as at a wall;
	// each link names the other tile
	{ "Tbg_sevb_all_p_stanZ", 0x65c2, 2, { 0x00, 0x00 }, { 0x0c, 0xcd } },
	{ "Tbg_sevb_all_p_stanZ", 0x6612, 2, { 0x00, 0x00 }, { 0x0c, 0xc4 } },
	// Silo: armour 59 gives half, as the others do, drawn as the full suit
	{ "UsetupsiloZ", 0x4efc, 2, { 0x00, 0x73 }, { 0x00, 0x74 } },
	// Control: the blast door on pad 146 (object 184) slides up without its
	// clip to the box, through the ceiling (DOORFLAG_0004)
	{ "UsetupcontrolZ", 0xb8d4, 4, { 0x00, 0x00, 0x00, 0x04 }, { 0x00, 0x04, 0x00, 0x04 } },
	// Surface: a guard's pair of Klobbs (the decomp's ai_31, TRYGiveMeItem) without
	// the paired flag, which Surface 2 and Runway give theirs
	{ "UsetupsevxZ", 0x10794, 1, { 0x00 }, { 0x80 } },
	{ "UsetupsevxZ", 0x107a3, 1, { 0x10 }, { 0x90 } },
	// Surface and Surface 2: the path pad by the outside railing (288, 279)
	// stands past the rail, at z -5001, off the tile it names; -4968 is on
	// the tile beside its neighbour
	{ "UsetupsevxZ", 0x31b0, 4, { 0xc5, 0x9c, 0x48, 0x00 }, { 0xc5, 0x9b, 0x40, 0x00 } },
	{ "UsetupsevxbZ", 0x3024, 4, { 0xc5, 0x9c, 0x48, 0x00 }, { 0xc5, 0x9b, 0x40, 0x00 } },
	// Egyptian: the Golden Gun's glass case, four door_win panes (objects
	// 45-48, pads 53-56) that sink into the plinth, lacked DOORFLAG_0004 as
	// Control's blast door did, so a pane was not clipped to its box as it
	// went down ("Egypt: Fix glass doors for golden gun room")
	{ "UsetupcrypZ", 0x4b5c, 2, { 0x00, 0x08 }, { 0x00, 0x0c } },
	{ "UsetupcrypZ", 0x4c5c, 2, { 0x00, 0x08 }, { 0x00, 0x0c } },
	{ "UsetupcrypZ", 0x4d5c, 2, { 0x00, 0x08 }, { 0x00, 0x0c } },
	{ "UsetupcrypZ", 0x4e5c, 2, { 0x00, 0x08 }, { 0x00, 0x0c } },
};

static void romPatch(const char *stem, buf *file)
{
	for (size_t i = 0; i < sizeof(g_RomPatches) / sizeof(g_RomPatches[0]); ++i) {
		const size_t at = g_RomPatches[i].at;
		const size_t n = g_RomPatches[i].n;

		if (!strcmp(stem, g_RomPatches[i].stem) && at + n <= file->n
				&& !memcmp(file->v + at, g_RomPatches[i].old, n)) {
			memcpy(file->v + at, g_RomPatches[i].new, n);
		}
	}
}

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

	{
		buf out = inflate1172(g_Rom + f->addr, f->size);
		romPatch(stem, &out);
		return out;
	}
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
		p->skeleton = be32(g_Data, h + 4);
		p->numswitches = bes16(g_Data, h + 12);
		p->nummatrices = bes16(g_Data, h + 14);
		p->radius = bef32(g_Data, h + 16);
		p->numtextures = bes16(g_Data, h + 22);
	}

	// c_item_entries, the same header behind a 20-byte row: the bodies, then
	// the heads (gechr.py)
	for (size_t k = 0; k < NUM_CHRS; ++k) {
		const size_t o = CHRS_AT + 20 * k;
		const size_t h = be32(g_Data, o) - DATA_VRAM;
		struct prop *p = &g_Chrs[k];

		if (h + 0x18 > g_DataLen) {
			fail("character %d's header is outside the data segment", (int)k);
		}

		p->file = dataString(be32(g_Data, o + 4));
		p->scale = bef32(g_Data, o + 8);
		// makeonebody()'s modelSetAnimTranslationScale(), which is Perfect
		// Dark's own animscale
		p->pov = bef32(g_Data, o + 12);
		// whether it is male, and whether it wears a head of its own
		p->flags = ((be32(g_Data, o + 16) >> 24) & 1) | (((be32(g_Data, o + 16) >> 16) & 1) << 1);
		p->skeleton = be32(g_Data, h + 4);
		p->numswitches = bes16(g_Data, h + 12);
		p->nummatrices = bes16(g_Data, h + 14);
		p->radius = bef32(g_Data, h + 16);
		p->numtextures = bes16(g_Data, h + 22);
	}

	// gitem_structs: a hand's models, of which the remake takes the watch
	for (size_t k = 0; k < NUM_ITEMS; ++k) {
		const size_t o = ITEMS_AT + ITEM_ROW * k;
		const uint32_t hp = be32(g_Data, o);
		struct prop *p = &g_Items[k];
		size_t h;

		if (o + ITEM_ROW > g_DataLen || !hp || hp < DATA_VRAM || hp - DATA_VRAM + 0x18 > g_DataLen) {
			continue;
		}

		h = hp - DATA_VRAM;
		p->file = dataString(be32(g_Data, o + 4));
		p->scale = 1.0;
		p->numswitches = bes16(g_Data, h + 12);
		p->nummatrices = bes16(g_Data, h + 14);
		p->radius = bef32(g_Data, h + 16);
		p->numtextures = bes16(g_Data, h + 22);
	}

	if (!g_Items[ITEM_WATCH].file || strcmp(g_Items[ITEM_WATCH].file, ITEM_WATCH_FILE)) {
		fail("item %d is %s and not %s", (int)ITEM_WATCH,
			g_Items[ITEM_WATCH].file ? g_Items[ITEM_WATCH].file : "nothing", ITEM_WATCH_FILE);
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

// GoldenEye's three sky pictures (image_bank.c's skywaterimages: clouds,
// grey water, blue water), which are Perfect Dark's three sky/water texture
// configs in the same order but different pictures - Perfect Dark redrew its
// water. They go out with every conversion and the fog rows name them as rows
// 3-5 of g_TcSkyWaterConfigs (src/textureconfig.c).
#define GE_IMAGE_CLOUDS 2228
#define GE_IMAGE_WATER_GREY 1508
#define GE_IMAGE_WATER_BLUE 1509
#define GE_SKYTEX_FIRST 3

// a level's row of GoldenEye's *fogless* table (bgfog.c's fog_tables2, which
// follows the fog table's end row: Frigate and Cuba, drawn with no fog and a
// fixed z range) - the 18 values after the id, as bgfog.c lists them
#define FOGLESS_ROW 56
static int romFoglessRow(uint32_t levelid, double *r)
{
	size_t o = FOG_AT;

	// past the fog table's end row
	for (;;) {
		if (o + FOG_ROW > g_DataLen) {
			return 0;
		}

		const uint32_t lid = be32(g_Data, o);
		o += FOG_ROW;

		if (lid == 0 && o > FOG_AT + FOG_ROW) {
			break;
		}

		if (lid >= 0x10000) {
			return 0;
		}
	}

	for (; o + FOGLESS_ROW <= g_DataLen; o += FOGLESS_ROW) {
		const uint32_t lid = be32(g_Data, o);
		const uint8_t *d = g_Data + o + 4;

		if (lid == 0) {
			break;
		}

		if (lid != levelid) {
			continue;
		}

		for (int i = 0; i < 4; ++i) r[i] = d[i];		// sky rgb, clouds
		r[4] = bef32(d, 4);					// cloud plane height
		r[5] = (int16_t)be16(d, 8);				// sky image id
		r[6] = be16(d, 10);
		for (int i = 0; i < 3; ++i) r[7 + i] = bef32(d, 12 + 4 * i);	// cloud rgb
		r[10] = d[24];						// is water
		r[11] = bef32(d, 28);					// water plane height
		r[12] = (int16_t)be16(d, 32);				// water image id
		r[13] = be16(d, 34);
		for (int i = 0; i < 3; ++i) r[14 + i] = bef32(d, 36 + 4 * i);	// water rgb
		r[17] = bef32(d, 48);					// water concavity
		return 1;
	}

	return 0;
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

/**
 * `music_setup_entries`, the music each level plays: rows of four s16 from
 * 0x2dd80 - {level id, main theme, background, X theme}, -1 for none - ended by
 * a level id of 0, and `random_tracks` straight after it, the sequences a level
 * with no row of its own draws one of, ended by M_NONE. A level's row goes on
 * its line of the maps and missions blocks as GoldenEye's own sequence numbers,
 * a main theme of -1 for a level that draws; the game appends the sequences
 * (gemusic.c).
 *
 * `g_musicDefaultTrackVolume` is how loud each sequence plays, an s16 of 0x7fff
 * a sequence from 0x35c8, ended by -1.
 */
#define MUSIC_VOLUMES_AT 0x35c8
#define MUSIC_AT 0x2dd80
#define MUSIC_ROW 8
#define MUSIC_MAX_ROWS 64

static size_t romMusicRandomAt(void)
{
	size_t o = MUSIC_AT;

	for (int i = 0; i < MUSIC_MAX_ROWS && o + MUSIC_ROW <= g_DataLen; ++i, o += MUSIC_ROW) {
		if (be16(g_Data, o) == 0) {
			return o + MUSIC_ROW;
		}
	}

	fail("the levels' music table has no end");
	return 0;
}

static void romMusicRow(uint32_t levelid, int *tracks)
{
	const size_t end = romMusicRandomAt() - MUSIC_ROW;

	tracks[0] = tracks[1] = tracks[2] = -1;

	for (size_t o = MUSIC_AT; o < end; o += MUSIC_ROW) {
		if (be16(g_Data, o) == levelid) {
			for (int i = 0; i < 3; ++i) {
				tracks[i] = bes16(g_Data, o + 2 + 2 * i);
			}
			return;
		}
	}
}

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
	uint32_t vtxptr;      // the address of its vertices, which is what a vis command names it by
	uint8_t flags;        // GoldenEye's controlbytes1
	uint8_t thickness;    // and controlbytes2, in GoldenEye's code and the file's units
};

struct viscmd {
	uint8_t type, len;
	uint32_t arg;
};

struct bg {
	int numrooms;
	struct bgroom *rooms;
	VEC(struct portal) portals;
	VEC(struct viscmd) vis;
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
		p.vtxptr = be32(d, o);
		p.flags = d[o + 6];
		p.thickness = d[o + 7];
		VECPUSH(bg->portals, p);
	}

	// GoldenEye's global visibility commands, which are Perfect Dark's own.
	//
	// A bg file carries a script the game runs every frame over the rooms the
	// portals found (bg.c's parse_global_vis_command_list()): "if the camera is
	// in rooms 110 to 113, show room 119", "if portal N is in view, show room
	// M". It is how a level draws what no chain of portals reaches - Dam's
	// cliffs and mountains, which are rooms of their own with no portal into
	// them, and which a converted Dam drew as sky. Perfect Dark kept the whole
	// of it (bgExecuteCommands()): the same eight-byte record - a type, a
	// length in records, an argument - and the same opcode numbers, 0x64 a
	// portal and 0x65 a room. The rooms keep their numbers through the
	// conversion and so do the portals, so the script is carried as it is, up
	// to its END; one naming a portal the file has not got is left out whole,
	// since half a script shows and hides the wrong rooms. geconvert.py's
	// vis_commands().
	{
		const uint32_t visat = segoff(be32(d, 12));
		int whole = 1;

		for (size_t o = visat; visat && o + 8 <= len && d[o] != 0; o += 8) {
			struct viscmd c = { d[o], d[o + 1], be32(d, o + 4) };

			if (c.type == 0x64) {
				int found = 0;

				for (size_t k = 0; k < bg->portals.n; ++k) {
					found |= bg->portals.v[k].vtxptr == c.arg;
				}

				whole &= found;
			}

			VECPUSH(bg->vis, c);
		}

		if (!whole) {
			bg->vis.n = 0;
		}
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
	int32_t neighbour[15];   // the tile across the edge from point k to point k + 1, or -1
	size_t offset;
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
			t.neighbour[k] = -1;
		}

		t.offset = o;
		VECPUSH(out, t);
		o += 8 + 8 * t.npts;
	}

	// A link names its neighbour by where it is: stan.c keeps standTileStart at
	// the first tile less 0x80 and follows a link as standTileStart + (link <<
	// 3), the low four bits being part of the address and not an edge number.
	// Every one of the 88128 links in the game's 26 levels resolves to a tile
	// that shares the edge's two points. geconvert.py's read_stan().
	{
		const size_t first = be32(d, 4);

		for (size_t i = 0; i < out.n; ++i) {
			struct tile *t = &out.v[i];

			for (int k = 0; k < t->npts; ++k) {
				const size_t at = first - 0x80 + ((size_t)t->link[k] << 3);

				if (!(t->link[k] >> 4)) {
					continue;
				}

				for (size_t j = 0; j < out.n; ++j) {
					if (out.v[j].offset == at) {
						t->neighbour[k] = (int32_t)j;
						break;
					}
				}
			}
		}
	}

	return out;
}

/* A link that climbs.
 *
 * GoldenEye joins a floor to one far over it with tiles that stand on edge -
 * no area in plan - and its collision, which is the plan and nothing else,
 * walks through them: that is how Bond drops off a deck onto the stair beside
 * it, and by the same link he can walk from the stair into the deck's wall and
 * be lifted onto the deck (bondview's only say on height is an edge 175 over
 * his eye). Perfect Dark lifts nobody: a player who crosses such an edge is
 * inside the wall, on whatever its upright tiles make of a floor. So where the
 * floor across a link - through any tiles that stand on edge - is more than a
 * step over this edge, the low side gets a wall as high as the climb, and the
 * link stays a link in the graph (STAN_CLIMBWALL), since from the top it is
 * still the way down. A stair's risers are the same construction and climb a
 * step. geconvert.py's stan_climb().
 */
#define WALL_CLIMB 60.0
#define STAN_CLIMBWALL 0x4000

static int tileFlatInPlan(const struct tile *t)
{
	int64_t area = 0;

	for (int k = 0; k < t->npts; ++k) {
		const int16_t *p = t->pts[k], *q = t->pts[(k + 1) % t->npts];

		area += (int64_t)p[0] * q[2] - (int64_t)q[0] * p[2];
	}

	return area == 0;
}

static double stanClimb(const tiles *stan, size_t i, int k, double inv)
{
	const struct tile *t = &stan->v[i];
	const int16_t *a = t->pts[k], *b = t->pts[(k + 1) % t->npts];
	int32_t queue[32];
	int head = 0, tail = 0;
	int found = 0;
	int32_t climb = 0;

	if (t->neighbour[k] < 0 || tileFlatInPlan(t)
			|| (fabs((a[0] - b[0]) * inv) < 0.5 && fabs((a[2] - b[2]) * inv) < 0.5)) {
		return 0.0;
	}

	queue[tail++] = t->neighbour[k];

	while (head < tail) {
		const struct tile *u = &stan->v[queue[head++]];

		if (!tileFlatInPlan(u)) {
			// the floor across: how far its corners over this edge's two
			// ends are over them
			int32_t ya = INT32_MIN, yb = INT32_MIN, c;

			for (int m = 0; m < u->npts; ++m) {
				if (u->pts[m][0] == a[0] && u->pts[m][2] == a[2] && u->pts[m][1] > ya) ya = u->pts[m][1];
				if (u->pts[m][0] == b[0] && u->pts[m][2] == b[2] && u->pts[m][1] > yb) yb = u->pts[m][1];
			}

			if (ya == INT32_MIN || yb == INT32_MIN) {
				continue;
			}

			c = ya - a[1] < yb - b[1] ? ya - a[1] : yb - b[1];

			if (!found || c < climb) {
				climb = c;
			}

			found = 1;
			continue;
		}

		// a ladder is a tile on edge too, and the way up it is Perfect Dark's
		// own (the floor made from it carries the ladder flag): no wall
		if (u->special == 3) {
			return 0.0;
		}

		// and so is the panel at a ladder's side: a wall on it stands under
		// anyone who comes down the ladder at that end, and they hang on
		// its top (the longer of Dam's two ladders down the dam's face)
		for (int m = 0; m < u->npts; ++m) {
			if (u->neighbour[m] >= 0 && stan->v[u->neighbour[m]].special == 3) {
				return 0.0;
			}
		}

		// a tile on edge: on through its edges that have a length in plan
		// (one that goes straight up is the way to the panel beside it)
		for (int m = 0; m < u->npts; ++m) {
			const int16_t *p = u->pts[m], *q = u->pts[(m + 1) % u->npts];
			const int32_t n = u->neighbour[m];
			int seen = n < 0 || (size_t)n == i;

			if (fabs((p[0] - q[0]) * inv) < 0.5 && fabs((p[2] - q[2]) * inv) < 0.5) {
				continue;
			}

			for (int v = 0; v < tail && !seen; ++v) {
				seen = queue[v] == n;
			}

			if (!seen && tail < 32) {
				queue[tail++] = n;
			}
		}
	}

	return found && climb * inv > WALL_CLIMB ? climb * inv : 0.0;
}

/**
 * GoldenEye's own tile graph, for its own collision (port/src/gestan.c).
 *
 * "GST1", the number of tiles, and a tile a record in the stan file's own
 * order: its room, its special, its number of points, and for each point its
 * place in the converted level - the same rounded place writeTiles() gives the
 * floor made from it - and what is across the edge from it to the next: the
 * index of the tile linked there, -1 for an unlinked edge writeTiles() raised a
 * wall on, or -2 for an unlinked edge that has no length in plan and so no
 * wall. A link that climbs (stanClimb()) has STAN_CLIMBWALL set in its index: a
 * wall was raised on it too. The walls are in the tiles file in this order,
 * which is how the port tells which tile a wall belongs to. geconvert.py's
 * write_stan().
 */
static buf writeStan(const tiles *stan, double levelscale, const double *offset)
{
	const double inv = 1.0 / levelscale;
	buf out = {0};

	if (stan->n >= STAN_CLIMBWALL) {
		fail("%d tiles, and a link has fourteen bits", (int)stan->n);
	}

	bufPut(&out, (const uint8_t *)"GST1", 4);
	bufU32(&out, (uint32_t)stan->n);

	for (size_t i = 0; i < stan->n; ++i) {
		const struct tile *t = &stan->v[i];

		bufU16(&out, (uint32_t)t->room);
		bufU8(&out, (uint32_t)t->special);
		bufU8(&out, (uint32_t)t->npts);

		for (int k = 0; k < t->npts; ++k) {
			const int16_t *pa = t->pts[k], *pb = t->pts[(k + 1) % t->npts];
			double a[3], b[3];
			int32_t nb = t->neighbour[k];

			for (int c = 0; c < 3; ++c) {
				a[c] = (double)pa[c] * inv - offset[c];
				b[c] = (double)pb[c] * inv - offset[c];
			}

			if (nb < 0) {
				nb = fabs(a[0] - b[0]) < 0.5 && fabs(a[2] - b[2]) < 0.5 ? -2 : -1;
			} else if (stanClimb(stan, i, k, inv) > 0.0) {
				nb |= STAN_CLIMBWALL;
			}

			for (int c = 0; c < 3; ++c) {
				bufU16(&out, (uint16_t)s16(a[c]));
			}
			bufU16(&out, (uint16_t)(int16_t)nb);
		}
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

/* ------------------------------------------------------------------------ */
/* a portal's front side (portal_room_order) */

#define PORTAL_NEAR 300.0   // a tile or vertex this close to a portal speaks for its room
#define PORTAL_EPS 1.0      // a point this close to the plane says nothing
#define PORTAL_MARGIN 40.0  // how far the room's own geometry must clear the plane

// The normal a portal's winding gives it, and the slab its vertices span,
// exactly as bg.c works them out at the load (g_PortalMetrics in bgSetup()).
static int portalMetric(const double (*v)[3], int n, double *normal, double *lo, double *hi)
{
	double d = 0.0;

	normal[0] = normal[1] = normal[2] = 0.0;

	for (int j = 0; j < n; ++j) {
		const double *a = v[j], *b = v[(j + 1) % n];
		normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
		normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
		normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
	}

	d = -sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);

	if (d == 0.0) {
		return 0;
	}

	for (int c = 0; c < 3; ++c) {
		normal[c] /= d;
	}

	for (int j = 0; j < n; ++j) {
		const double val = v[j][0] * normal[0] + v[j][1] * normal[1] + v[j][2] * normal[2];

		if (!j || val < *lo) *lo = val;
		if (!j || val > *hi) *hi = val;
	}

	return 1;
}

struct portalnear {
	double d;
	int k;
};

static int cmpPortalNear(const void *a, const void *b)
{
	const struct portalnear *x = a, *y = b;
	return x->d < y->d ? -1 : x->d > y->d ? 1 : x->k - y->k;
}

static int cmpDouble(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

// Which side of a portal's plane a room's points near it lie on, as the median
// signed distance of the ones that speak. `keep` is how many of the nearest to
// fall back on when too few are within PORTAL_NEAR.
static int portalSide(const double (*pts)[3], int n, const double *normal, double mid,
		const double *at, int keep, double *out)
{
	struct portalnear *near;
	double *s;
	int npick = 0, ns = 0;

	if (n <= 0) {
		return 0;
	}

	near = gcAlloc(n * sizeof(*near));

	for (int k = 0; k < n; ++k) {
		double q = 0.0;

		for (int c = 0; c < 3; ++c) {
			q += (pts[k][c] - at[c]) * (pts[k][c] - at[c]);
		}

		near[k].d = sqrt(q);
		near[k].k = k;

		if (near[k].d <= PORTAL_NEAR) {
			npick++;
		}
	}

	s = gcAlloc(n * sizeof(*s));

	if (npick < 3) {
		qsort(near, n, sizeof(*near), cmpPortalNear);
		npick = keep < n ? keep : n;

		for (int i = 0; i < npick; ++i) {
			const double *p = pts[near[i].k];
			const double v = p[0] * normal[0] + p[1] * normal[1] + p[2] * normal[2] - mid;

			if (fabs(v) > PORTAL_EPS) {
				s[ns++] = v;
			}
		}
	} else {
		for (int k = 0; k < n; ++k) {
			const double v = pts[k][0] * normal[0] + pts[k][1] * normal[1] + pts[k][2] * normal[2] - mid;

			if (near[k].d <= PORTAL_NEAR && fabs(v) > PORTAL_EPS) {
				s[ns++] = v;
			}
		}
	}

	if (!ns) {
		return 0;
	}

	qsort(s, ns, sizeof(*s), cmpDouble);
	*out = ns & 1 ? s[ns / 2] : (s[ns / 2 - 1] + s[ns / 2]) / 2.0;

	return 1;
}

// A room's own vertices at world scale - scaledRoom()'s `world`.
static double (*roomWorldVtx(const struct bgroom *room, double inv, const double *offset, int *count))[3]
{
	const int n = room->hasvtx ? (int)(room->vtx.n / 16) : 0;
	double (*out)[3];

	*count = n;

	if (!n) {
		return NULL;
	}

	out = gcAlloc(n * sizeof(*out));

	for (int k = 0; k < n; ++k) {
		for (int c = 0; c < 3; ++c) {
			out[k][c] = ((double)bes16(room->vtx.v, 16 * k + 2 * c) + room->pos[c]) * inv - offset[c];
		}
	}

	return out;
}

/**
 * Which of a portal's two rooms goes in room2: the one on its front.
 *
 * Perfect Dark takes the room on the front of a portal's normal to be room2
 * (bg.c bgTestPosInRoomCheap, and the camera's side in the renderer's snake),
 * and GoldenEye's own record does not: over the twenty-six levels its room1 is
 * the front room 668 times and its room2 1068, which is a coin toss. Nearly
 * all of that is repaired at the load, where bgInitPortal() swaps the two when
 * room1's *centre* is in front of the plane - the file's order only survives
 * where both centres fall on the front, and there it is right. Seven portal
 * records in the game are backwards in play even so: Facility's two into the
 * hole at 12/15, Archives' two at 41/42, and the 85/12 that the three Bunker 2
 * levels share.
 *
 * Each room's own geometry settles it. The tiles either side of a doorway are
 * on the sides their rooms are, and where a room has none - or both rooms'
 * tiles fall one side, as Runway's 13/14 does - the room's drawn vertices say
 * the same thing. Where neither says anything, GoldenEye's own order is kept:
 * bgInitPortal() will have it.
 */
static int (*portalRoomOrder(const struct bg *bg, double inv, const double *offset, const tiles *stan))[2]
{
	const int n = bg->numrooms;
	int (*out)[2] = gcAlloc(bg->portals.n * sizeof(*out) + sizeof(*out));
	double (**centroids)[3] = gcAlloc((n + 2) * sizeof(*centroids));
	int *ncentroids = gcAlloc((n + 2) * sizeof(*ncentroids));
	double (**worlds)[3] = gcAlloc((n + 2) * sizeof(*worlds));
	int *nworlds = gcAlloc((n + 2) * sizeof(*nworlds));
	uint8_t *haveworld = gcAlloc(n + 2);

	for (size_t i = 0; i < stan->n; ++i) {
		const struct tile *t = &stan->v[i];

		if (t->room >= 1 && t->room <= n && t->npts >= 3) {
			ncentroids[t->room]++;
		}
	}

	for (int r = 1; r <= n; ++r) {
		if (ncentroids[r]) {
			centroids[r] = gcAlloc(ncentroids[r] * sizeof(*centroids[r]));
			ncentroids[r] = 0;
		}
	}

	for (size_t i = 0; i < stan->n; ++i) {
		const struct tile *t = &stan->v[i];
		double c[3] = {0.0, 0.0, 0.0};

		if (t->room < 1 || t->room > n || t->npts < 3) {
			continue;
		}

		for (int k = 0; k < t->npts; ++k) {
			for (int j = 0; j < 3; ++j) {
				c[j] += t->pts[k][j] * inv - offset[j];
			}
		}

		for (int j = 0; j < 3; ++j) {
			centroids[t->room][ncentroids[t->room]][j] = c[j] / t->npts;
		}

		ncentroids[t->room]++;
	}

	for (size_t i = 0; i < bg->portals.n; ++i) {
		const struct portal *p = &bg->portals.v[i];
		double (*v)[3] = gcAlloc((p->npts ? p->npts : 1) * sizeof(*v));
		double normal[3], lo = 0.0, hi = 0.0, at[3] = {0.0, 0.0, 0.0}, mid, s1, s2;
		int r1 = p->room1, r2 = p->room2, front = 0;

		for (int k = 0; k < p->npts; ++k) {
			for (int c = 0; c < 3; ++c) {
				v[k][c] = p->pts[k][c] * inv - offset[c];
				at[c] += v[k][c];
			}
		}

		out[i][0] = r1;
		out[i][1] = r2;

		if (!p->npts || !portalMetric(v, p->npts, normal, &lo, &hi)) {
			continue;
		}

		for (int c = 0; c < 3; ++c) {
			at[c] /= p->npts;
		}

		mid = (lo + hi) / 2.0;

		if (portalSide(centroids[r1 >= 1 && r1 <= n ? r1 : 0], r1 >= 1 && r1 <= n ? ncentroids[r1] : 0,
					normal, mid, at, 5, &s1)
				&& portalSide(centroids[r2 >= 1 && r2 <= n ? r2 : 0], r2 >= 1 && r2 <= n ? ncentroids[r2] : 0,
					normal, mid, at, 5, &s2)
				&& (s1 > 0.0) != (s2 > 0.0)) {
			front = s1 > 0.0 ? r1 : r2;
		} else {
			int rr[2] = { r1, r2 };
			double d[2];
			int got = 1;

			for (int k = 0; k < 2; ++k) {
				const int r = rr[k];

				if (r >= 1 && r <= n && !haveworld[r]) {
					worlds[r] = roomWorldVtx(&bg->rooms[r - 1], inv, offset, &nworlds[r]);
					haveworld[r] = 1;
				}

				if (r < 1 || r > n || !portalSide(worlds[r], nworlds[r], normal, mid, at, 20, &d[k])) {
					got = 0;
					break;
				}
			}

			if (got && (d[0] > 0.0) != (d[1] > 0.0)
					&& (fabs(d[0]) < fabs(d[1]) ? fabs(d[0]) : fabs(d[1])) > PORTAL_MARGIN) {
				front = d[0] > 0.0 ? r1 : r2;
			}
		}

		if (front == r1) {
			out[i][0] = r2;
			out[i][1] = r1;
		}
	}

	return out;
}

/*
 * GoldenEye's portal record carries two bytes Perfect Dark's does not. The
 * first is flags: DISABLED (1) is cleared at the load, SPECIAL (2) gives the
 * room beyond the whole screen once the portal is in view, and is Perfect
 * Dark's PORTALFLAG_02 - set in the file on Dam and Jungle, and by bg.c's
 * specialportalarray at the load on Control and Jungle. The second is a
 * **thickness**, a four bit mantissa in quarters doubled by a four bit
 * exponent, in the portal's own units: a camera within it of the portal's
 * plane is in both rooms, and the portal's box on the screen is grown by it
 * both ways. It goes in the record's spare eighth byte in the same code, in
 * world units - the smallest code that is not thinner than GoldenEye's
 * (geroom.h; bg.c reads it).
 */
#define GE_PORTALFLAG_SPECIAL 0x02
#define PD_PORTALFLAG_02      0x02

// bg.c's levelinfotable (24 bytes a row, the id first) and the specialportalarray after it
#define LEVELINFO_AT       0x8004448cu
#define LEVELINFO_ROWS     38
#define SPECIALPORTALS_AT  0x80044824u
#define SPECIALPORTALS_END 0x80044838u

static uint8_t portalThickness(uint8_t code, double inv)
{
	const double want = (code & 0xf) * 0.25 * (double)(1u << (code >> 4)) * inv;
	double bestv = 0.0;
	int best = -1;

	if (want <= 0.0) {
		return 0;
	}

	for (int e = 0; e < 16; ++e) {
		for (int m = 1; m < 16; ++m) {
			const double v = m * 0.25 * (double)(1u << e);

			if (v >= want && (best < 0 || v < bestv)) {
				bestv = v;
				best = (e << 4) | m;
			}
		}
	}

	return best < 0 ? 0xff : (uint8_t)best;
}

/** Whether specialportalarray names this portal of the level with this id: {level index, (first, last)..., 0xff} rows. */
static int portalSpecialByCode(uint32_t levelid, size_t portal)
{
	const size_t end = SPECIALPORTALS_END - DATA_VRAM;
	size_t o = SPECIALPORTALS_AT - DATA_VRAM;
	int index = -1;

	if (end > g_DataLen || LEVELINFO_AT - DATA_VRAM + 24 * LEVELINFO_ROWS > g_DataLen) {
		fail("the special portals run off the data segment");
	}

	for (int i = 0; i < LEVELINFO_ROWS && index < 0; ++i) {
		if (be32(g_Data, LEVELINFO_AT - DATA_VRAM + 24 * i) == levelid) {
			index = i;
		}
	}

	while (o < end) {
		const int level = g_Data[o++];

		while (o + 1 < end && g_Data[o] != 0xff) {
			if (level == index && portal >= g_Data[o] && portal <= g_Data[o + 1]) {
				return 1;
			}
			o += 2;
		}
		o++;
	}

	return 0;
}

static buf writeBg(const struct bg *bg, double ls, const double *offset, uint8_t *leveltex, int *numlights,
		const int32_t (*tilebounds)[7], const tiles *stan, uint32_t levelid)
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
	int (*order)[2];

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
	portalsat = cmdsat + 8 * (bg->vis.n + 1);

	order = portalRoomOrder(bg, inv, offset, stan);

	for (size_t i = 0; i < bg->portals.n; ++i) {
		const struct portal *p = &bg->portals.v[i];
		bufU16(&portals, (uint32_t)(i + 1));
		bufU16(&portals, (uint16_t)order[i][0]);
		bufU16(&portals, (uint16_t)order[i][1]);
		bufU8(&portals, (p->flags & GE_PORTALFLAG_SPECIAL) || portalSpecialByCode(levelid, i) ? PD_PORTALFLAG_02 : 0);
		bufU8(&portals, portalThickness(p->thickness, inv));
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
	// the visibility commands, a portal's argument moved to the converted
	// file's own address for the same portal's vertices (bgRead())
	for (size_t i = 0; i < bg->vis.n; ++i) {
		const struct viscmd *c = &bg->vis.v[i];
		uint32_t arg = c->arg;

		if (c->type == 0x64) {
			size_t at = 0;

			for (size_t k = 0; k < bg->portals.n && bg->portals.v[k].vtxptr != c->arg; ++k) {
				at += 4 + 12 * (size_t)bg->portals.v[k].npts;
			}

			arg = SEG_BG + (uint32_t)(groupsat + at);
		}

		primary.v[cmdsat + 8 * i] = c->type;
		primary.v[cmdsat + 8 * i + 1] = c->len;
		set32(primary.v, cmdsat + 8 * i + 4, arg);
	}
	primary.v[cmdsat + 8 * bg->vis.n] = 0;
	primary.v[cmdsat + 8 * bg->vis.n + 1] = 1;
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

/* tile_surface_y(): the tile's own surface at x/z, from the fan triangle that
 * holds the point, or false where the point is outside the tile. */
static int tileSurfaceY(const double (*pts)[3], int n, double x, double z, double *out)
{
	const double *a = pts[0];

	for (int k = 1; k < n - 1; ++k) {
		const double *b = pts[k], *c = pts[k + 1];
		const double det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
		double w0, w1, w2;

		if (det == 0.0) {
			continue;
		}

		w0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det;
		w1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det;
		w2 = 1.0 - w0 - w1;

		if (w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0) {
			*out = w0 * a[1] + w1 * b[1] + w2 * c[1];
			return 1;
		}
	}

	return 0;
}

struct tilegeo {
	int first; /* the tile's points, in the level's own flat array */
	int n;
	double bb[4]; /* x0 x1 z0 z1 */
};

/* wall_span(): how far a wall on this edge may rise and how far it may reach
 * down - up to the lowest walkable surface that passes over it, and down to
 * the head of a player standing on the highest one that passes under it. */
static void wallSpan(const double (*world)[3], const struct tilegeo *geo, size_t ntiles,
		size_t self, const double *a, const double *b, double *aboveout, double *belowout)
{
	const double dx = b[0] - a[0], dz = b[2] - a[2];
	const double length = sqrt(dx * dx + dz * dz);
	double above = WALL_ABOVE, below = WALL_BELOW;
	/* the quad blocks between its own lowest and highest vertex, whatever its
	 * corners are (cdCollectGeoForCylFromList() reads the tile's ymin/ymax), so
	 * what has to stay under the surface over it is its higher end, and what
	 * has to stay over the one under it is its lower */
	const double top = a[1] > b[1] ? a[1] : b[1];
	const double foot = a[1] < b[1] ? a[1] : b[1];
	double x0, x1, z0, z1;
	int nsteps;

	x0 = (a[0] < b[0] ? a[0] : b[0]) - (WALL_REACH + WALL_SIDE);
	x1 = (a[0] > b[0] ? a[0] : b[0]) + (WALL_REACH + WALL_SIDE);
	z0 = (a[2] < b[2] ? a[2] : b[2]) - (WALL_REACH + WALL_SIDE);
	z1 = (a[2] > b[2] ? a[2] : b[2]) + (WALL_REACH + WALL_SIDE);
	/* a riser's own side is an edge that goes straight down, and a point of a
	 * wall in plan; Perfect Dark blocks within the player's radius of one all
	 * the same, so it takes the one sample at its own place */
	nsteps = length >= 1.0 ? (int)(length / WALL_STEP) + 1 : 1;

	for (int s = 0; s < nsteps; ++s) {
		const double f = (double)(s + 1) / (double)(nsteps + 1);
		const double px = a[0] + dx * f;
		const double pz = a[2] + dz * f;

		for (size_t j = 0; j < ntiles; ++j) {
			/* the player stands anywhere within their own radius of the wall,
			 * and where the surface reaches over any of that it is what the
			 * wall has to keep clear of: its lowest for the surface over the
			 * wall, its highest for the one under it */
			static const double reach[5][2] = {
				{ 0.0, 0.0 },
				{ WALL_REACH, 0.0 }, { -WALL_REACH, 0.0 },
				{ 0.0, WALL_REACH }, { 0.0, -WALL_REACH },
			};
			double low = 0.0, high = 0.0, gap, drop;
			int found = 0;

			if (j == self
					|| geo[j].bb[0] > x1 || geo[j].bb[1] < x0
					|| geo[j].bb[2] > z1 || geo[j].bb[3] < z0) {
				continue;
			}

			for (int r = 0; r < 5; ++r) {
				double y;

				if (tileSurfaceY(world + geo[j].first, geo[j].n,
						px + reach[r][0], pz + reach[r][1], &y)) {
					if (!found || y < low) {
						low = y;
					}

					if (!found || y > high) {
						high = y;
					}

					found = 1;
				}
			}

			if (!found) {
				continue;
			}

			gap = low - top;

			if (gap >= WALL_HEADROOM && gap < above) {
				above = gap;
			}

			drop = foot - high - WALL_HEAD - WALL_CLEAR;

			if (high + WALL_HEADROOM <= foot && drop >= -WALL_RISE && drop < below) {
				below = drop;
			}
		}
	}

	*aboveout = above;
	*belowout = below;
}

static buf writeTiles(const tiles *stan, int numrooms, double ls, const double *offset, int *numwalls)
{
	const double inv = 1.0 / ls;
	buf *bodies = gcAlloc((numrooms + 1) * sizeof(*bodies));
	buf out = {0};
	int walls = 0;
	uint32_t pos;
	struct tilegeo *geo = gcAlloc(stan->n * sizeof(*geo));
	double (*world)[3];
	size_t npoints = 0;

	for (size_t i = 0; i < stan->n; ++i) {
		npoints += stan->v[i].npts;
	}

	world = gcAlloc(npoints * sizeof(*world));
	npoints = 0;

	for (size_t i = 0; i < stan->n; ++i) {
		const struct tile *t = &stan->v[i];

		geo[i].first = (int)npoints;
		geo[i].n = t->npts;

		for (int k = 0; k < t->npts; ++k) {
			double *p = world[npoints++];

			for (int c = 0; c < 3; ++c) {
				p[c] = t->pts[k][c] * inv - offset[c];
			}

			if (k == 0) {
				geo[i].bb[0] = geo[i].bb[1] = p[0];
				geo[i].bb[2] = geo[i].bb[3] = p[2];
			} else {
				if (p[0] < geo[i].bb[0]) geo[i].bb[0] = p[0];
				if (p[0] > geo[i].bb[1]) geo[i].bb[1] = p[0];
				if (p[2] < geo[i].bb[2]) geo[i].bb[2] = p[2];
				if (p[2] > geo[i].bb[3]) geo[i].bb[3] = p[2];
			}
		}
	}

	for (size_t i = 0; i < stan->n; ++i) {
		const struct tile *t = &stan->v[i];
		const double (*pts)[3] = world + geo[i].first;
		const int n = t->npts;
		uint32_t flags = FLOOR_FLAGS;

		if (t->room > numrooms) {
			fail("a tile in room %d, past the level's %d", t->room, numrooms);
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
			double above, below;
			double climb = 0.0;

			if (t->link[k] >> 4) {
				climb = stanClimb(stan, i, k, inv);

				if (climb <= 0.0) {
					continue;
				}
			}

			// An edge that goes straight down - the side of a riser, a stair
			// tile's two corners over one another - has no length in plan,
			// and GoldenEye's wall is the edge in plan and nothing more:
			// nothing moving over its tiles can ever cross it, so it blocks
			// nothing there. Raised here it is a pole as tall as the
			// stairwell, and Perfect Dark blocks within a player's radius of
			// one: two of them stand at the corners of the first flight in
			// Dam's guard tower and nobody gets between them. geconvert.py's
			// write_tiles().
			if (fabs(a[0] - b[0]) < 0.5 && fabs(a[2] - b[2]) < 0.5) {
				continue;
			}

			wallSpan(world, geo, stan->n, i, a, b, &above, &below);

			if (climb > 0.0 && climb < above) {
				above = climb;
			}

			quad[0][0] = a[0]; quad[0][1] = a[1] - below; quad[0][2] = a[2];
			quad[1][0] = b[0]; quad[1][1] = b[1] - below; quad[1][2] = b[2];
			quad[2][0] = b[0]; quad[2][1] = b[1] + above; quad[2][2] = b[2];
			quad[3][0] = a[0]; quad[3][1] = a[1] + above; quad[3][2] = a[2];
			EMIT(WALL_FLAGS, quad, 4);
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

typedef VEC(struct weaponpad) weaponpads;

struct setup {
	VEC(struct pad) pads;
	VEC(struct waypoint) waypoints;
	VEC(struct waygroup) groups;
	s32s spawns;
	// the multiplayer items in the setup's order; loc -1 is an ammo crate
	weaponpads items;
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

static const uint8_t g_GeSizes[48] = {
	[1] = 64, [2] = 2, [3] = 32, [4] = 33, [5] = 32, [6] = 0x3b, [7] = 0x21, [8] = 0x22, [9] = 7, [10] = 0x40,
	[11] = 0x95, [12] = 32, [13] = 0x36, [14] = 3, [17] = 32, [18] = 3, [19] = 4, [20] = 0x2d, [21] = 0x22,
	[22] = 4, [23] = 4, [24] = 1, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 4, [31] = 1, [32] = 4,
	[33] = 5, [34] = 1, [35] = 4, [36] = 32, [37] = 10, [38] = 4, [39] = 0x2c, [40] = 0x2d, [42] = 32, [43] = 32,
	[44] = 5, [45] = 0x38, [46] = 7, [47] = 37,
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

	// The multiplayer items, in the setup's order: GoldenEye's arena setups
	// mix doors, glass and props in among the weapon spots, ammo boxes and
	// armour (Archives, Bunker ii and Egyptian put theirs first), so every
	// object is walked and the others skipped. The order is kept because an
	// ammo box takes the ammunition of the weapon spot before it, in
	// GoldenEye (prop.c, lastmpweaponnum) and in Perfect Dark
	// (g_SetupCurMpLocation). PROPFLAG2 0x08 is "don't load in multiplayer".
	// A pad from 10000 is a bound pad, written after the pads (boundPads()).
	if (h[3]) {
		records recs = setupRecords(f);
		for (size_t i = 0; i < recs.n; ++i) {
			const struct record *r = &recs.v[i];
			struct weaponpad w;
			if ((r->type != 8 && r->type != 20) || (be32(r->b, 12) & 0x08)) {
				continue;
			}
			w.pad = be32(r->b, 4) & 0xffff;
			if (w.pad >= 10000) {
				w.pad += (int32_t)s->pads.n - 10000;
			}
			if (r->type == 8) {
				if (r->b[0x80] < 0xf0) {
					continue;
				}
				w.loc = (int32_t)r->b[0x80] - 0xf0;
			} else {
				w.loc = -1;
			}
			VECPUSH(s->items, w);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* objects (geobjects.py) */

// GoldenEye type -> Perfect Dark type and its size in words; 0 is not carried
static const uint8_t g_Carry[48][2] = {
	[1] = { 0x01, 55 }, [3] = { 0x03, 23 }, [5] = { 0x05, 23 }, [10] = { 0x0a, 53 }, [11] = { 0x0b, 140 },
	[12] = { 0x0c, 23 }, [39] = { 0x03, 23 }, [40] = { 0x03, 23 }, [42] = { 0x2a, 24 }, [43] = { 0x2b, 23 },
	[45] = { 0x03, 23 }, [47] = { 0x2f, 26 },
};

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

/**
 * What an object can take before it breaks. GoldenEye's record carries it as a
 * 16.16 word at 0x74, which its loader divides out (prop.c: damage / 65536),
 * and its arithmetic from there is Perfect Dark's own - a shot adds its damage
 * times 250. Most are 1000, which is what every converted object was given;
 * the ones that are not are the point: Dam's padlocks are 200, which one PP7
 * round breaks, and wore 1000.
 */
static uint32_t geObjHealth(const uint8_t *raw)
{
	int32_t h = (int32_t)be32(raw, 0x74) / 65536;
	if (h < 1) {
		h = 1000;
	}
	return h > 32767 ? 32767u : (uint32_t)h;
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
	set16(out.v, 0x4e, geObjHealth(r->b));
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

// A made-up weapon spot's ammo crates, as GoldenEye lays out its own arenas
// (geconvert.py's crate_pads(), same numbers): two after each weapon spot, on
// pads of their own on its floor. Over GoldenEye's 13 multiplayer setups a
// crate stands 210-1160 from its weapon (10th-90th percentile, median 530)
// and within 57 of its height.
#define CRATES_PER_WEAPON 2
#define CRATE_NEAR 150.0
#define CRATE_FAR 1200.0
#define CRATE_WIDE 3000.0
#define CRATE_AIM 450.0
#define CRATE_RISE 60.0
#define CRATE_APART 100.0

struct cratecand {
	int wide;
	double key;
	int32_t pad;
	double x, z;
};

static int crateCandCmp(const void *a, const void *b)
{
	const struct cratecand *p = a, *q = b;
	if (p->wide != q->wide) {
		return p->wide < q->wide ? -1 : 1;
	}
	if (p->key != q->key) {
		return p->key < q->key ? -1 : 1;
	}
	return p->pad < q->pad ? -1 : p->pad > q->pad;
}

/**
 * Up to CRATES_PER_WEAPON floored pads for the crates of the weapon spot on
 * pad `weapon`, into out[]: not used[] (nor `also`), CRATE_NEAR to CRATE_FAR
 * across and within CRATE_RISE up or down, nearest CRATE_AIM first and
 * CRATE_APART from each other; failing that out to CRATE_WIDE unless `near`.
 */
static int cratePads(const struct setup *setup, const s32s *ok, int32_t weapon, const uint8_t *used, int32_t also,
		double ls, int near, int32_t *out)
{
	const double *wp = setup->pads.v[weapon].pos;
	const double wx = wp[0] / ls, wy = wp[1] / ls, wz = wp[2] / ls;
	struct cratecand *cands = gcAlloc((ok->n + 1) * sizeof(*cands));
	double ox[CRATES_PER_WEAPON], oz[CRATES_PER_WEAPON];
	size_t n = 0;
	int got = 0;

	for (size_t k = 0; k < ok->n; ++k) {
		const int32_t i = ok->v[k];
		const double *pp = setup->pads.v[i].pos;
		double x, y, z, dx, dz, d;
		if (used[i] || i == also) {
			continue;
		}
		x = pp[0] / ls;
		y = pp[1] / ls;
		z = pp[2] / ls;
		dx = x - wx;
		dz = z - wz;
		d = sqrt(dx * dx + dz * dz);
		if (d >= CRATE_NEAR && d <= (near ? CRATE_FAR : CRATE_WIDE) && fabs(y - wy) <= CRATE_RISE) {
			cands[n].wide = d > CRATE_FAR;
			cands[n].key = fabs(d - CRATE_AIM);
			cands[n].pad = i;
			cands[n].x = x;
			cands[n].z = z;
			n++;
		}
	}

	qsort(cands, n, sizeof(*cands), crateCandCmp);

	for (size_t k = 0; k < n && got < CRATES_PER_WEAPON; ++k) {
		int apart = 1;
		for (int j = 0; j < got; ++j) {
			const double ax = cands[k].x - ox[j], az = cands[k].z - oz[j];
			if (!(sqrt(ax * ax + az * az) >= CRATE_APART)) {
				apart = 0;
			}
		}
		if (apart) {
			out[got] = cands[k].pad;
			ox[got] = cands[k].x;
			oz[got] = cands[k].z;
			got++;
		}
	}

	return got;
}

static double padFarFrom(const struct setup *setup, int32_t p, const s32s *taken)
{
	const double *q = setup->pads.v[p].pos;
	double best = 0;
	for (size_t k = 0; k < taken->n; ++k) {
		const double *t = setup->pads.v[taken->v[k]].pos;
		const double v = sqrt((q[0] - t[0]) * (q[0] - t[0]) + (q[1] - t[1]) * (q[1] - t[1]) + (q[2] - t[2]) * (q[2] - t[2]));
		if (!k || v < best) {
			best = v;
		}
	}
	return best;
}

/**
 * Up to 12 weapon spots for a level GoldenEye has no multiplayer setup for,
 * each followed by its crates, into items (geconvert.py's weapon_spots()):
 * chosen as the spawns are - farthest from every spot and spawn taken so far -
 * but only among pads with room for their crates left; where fewer than six
 * fit, the rest spread over what is left with whatever crates they can have.
 */
static void weaponSpots(const struct setup *setup, const s32s *ok, const s32s *spawns, double ls, weaponpads *items)
{
	uint8_t *used = gcAlloc(setup->pads.n + 1);
	s32s taken = {0};
	int nweapons = 0;

	for (size_t k = 0; k < spawns->n; ++k) {
		used[spawns->v[k]] = 1;
		VECPUSH(taken, spawns->v[k]);
	}

	for (int pass = 0; pass < 2; ++pass) {
		while (nweapons < (pass ? 6 : 12)) {
			int32_t best = -1, bestcrates[CRATES_PER_WEAPON];
			double bestd = 0;
			int bestgot = 0;
			for (size_t k = 0; k < ok->n; ++k) {
				const int32_t p = ok->v[k];
				int32_t got[CRATES_PER_WEAPON] = {0};
				int n = 0;
				double d;
				if (used[p]) {
					continue;
				}
				if (!pass) {
					n = cratePads(setup, ok, p, used, p, ls, 1, got);
					if (n < CRATES_PER_WEAPON) {
						continue;
					}
				}
				d = padFarFrom(setup, p, &taken);
				if (best < 0 || d > bestd) {
					best = p;
					bestd = d;
					bestgot = n;
					memcpy(bestcrates, got, sizeof(got));
				}
			}
			if (best < 0) {
				break;
			}
			used[best] = 1;
			if (pass) {
				bestgot = cratePads(setup, ok, best, used, -1, ls, 0, bestcrates);
			}
			{
				struct weaponpad w = { best, nweapons % 6 };
				VECPUSH(*items, w);
			}
			for (int j = 0; j < bestgot; ++j) {
				struct weaponpad c = { bestcrates[j], -1 };
				used[bestcrates[j]] = 1;
				VECPUSH(*items, c);
			}
			VECPUSH(taken, best);
			nweapons++;
		}
	}
}

static const uint8_t g_Ai1000[] = { 0x01, 0x85, 0x01, 0x45, 0x01, 0x46, 0x00, 0x05, 0xfd, 0x00, 0x00, 0x00, 0x04 };
static const uint8_t g_Ai1001[] = { 0x01, 0xb2, 0x16, 0x00, 0x05, 0xfd, 0x00, 0x00, 0x00, 0x04 };

static buf writeMpSetup(const struct setup *setup, const struct setup *mp, const tiles *stan, const struct bg *bg,
		double ls, const buf *gedata, const s32s *bikepads, uint8_t *models)
{
	s32s spawns = {0};
	weaponpads items = {0};
	buf intro = {0}, props = {0}, out = {0};
	size_t introat, propsat, pathsat, aiat, aicodeat;

	if (mp && mp->spawns.n) {
		spawns = mp->spawns;
		// GoldenEye's own order: a crate follows the weapon spot it serves
		items = mp->items;
	} else {
		s32s ok = flooredPads(setup, stan, bg);
		double (*pts)[3] = gcAlloc((ok.n + 1) * sizeof(*pts));
		s32s idx;
		for (size_t i = 0; i < ok.n; ++i) {
			memcpy(pts[i], setup->pads.v[ok.v[i]].pos, sizeof(pts[i]));
		}
		idx = spread(pts, ok.n, 12);
		for (size_t i = 0; i < idx.n; ++i) {
			VECPUSH(spawns, ok.v[idx.v[i]]);
		}
		weaponSpots(setup, &ok, &spawns, ls, &items);
	}

	for (size_t i = 0; i < spawns.n; ++i) {
		bufU32(&intro, 0);
		bufU32(&intro, (uint32_t)spawns.v[i]);
		bufU32(&intro, 0);
	}
	bufU32(&intro, 0x0c);

	for (size_t i = 0; i < items.n; ++i) {
		if (items.v[i].loc >= 0) {
			bufU32(&props, (0x0100u << 16) | 0x08);
			bufU32(&props, (uint32_t)items.v[i].pad & 0xffff);
			bufU32(&props, 1);
			bufZeros(&props, 4 * 16);
			bufU32(&props, 1000);
			bufZeros(&props, 8);
			bufU32(&props, 0x0fff0000);
			bufU32(&props, (uint32_t)(0xf0 + items.v[i].loc) << 24);
			bufU32(&props, 0x00ffffff);
			bufU32(&props, 0);
		} else {
			bufU32(&props, (0x00ccu << 16) | 0x14);
			bufU32(&props, (0x00c1u << 16) | ((uint32_t)items.v[i].pad & 0xffff));
			bufU32(&props, 1);
			bufZeros(&props, 4 * 16);
			bufU32(&props, 1000);
			bufZeros(&props, 8);
			bufU32(&props, 0x0fff0000);
			for (int k = 0; k < 19; ++k) {
				bufU32(&props, 0xffff0000);
			}
		}
	}

	objects(gedata, setup->pads.n, (int32_t)items.n, 0x182, bikepads, &props, models);
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
// A switch (0x13) is kept: GoldenEye's "activating this console activates that
// door" is Perfect Dark's OBJTYPE_LINKLIFTDOOR a word shorter, and the branch
// of doorCallLift() where the lift is a door is GoldenEye's own behaviour. It
// is what opens Dam's gates. gesolo.py's AS_NOTHING.
#define SOLO_AS_NOTHING(t) ((t) == 0x0e || (t) == 0x11 || (t) == 0x12)

#define SOLO_NO_PAD 0xffff
/**
 * GoldenEye's own item ids (bondconstants.h, ITEM_IDS) as the port's weapon
 * numbers, which are not in the same order: GoldenEye's list starts with the
 * unarmed hand and the two knives and names its guns after the real ones, and
 * the port's twenty-five are GoldenEye's in-game names in Perfect Dark's own
 * order. Everything past the last mine is a gadget, a key or a document, which
 * Perfect Dark cannot hold as a weapon at all. gesolo.py's GE_ITEM_WEAPON.
 */
static const uint8_t g_GeItemWeapon[] = {
	0x01,  /* 0  UNARMED       WEAPON_UNARMED */
	0x01,  /* 1  FIST          WEAPON_UNARMED */
	0x71,  /* 2  KNIFE         hunting knife */
	0x72,  /* 3  THROWKNIFE    throwing knife */
	0x5e,  /* 4  WPPK          PP7 */
	0x5f,  /* 5  WPPKSIL       PP7 (silenced) */
	0x60,  /* 6  TT33          DD44 Dostovei */
	0x61,  /* 7  SKORPION      Klobb */
	0x62,  /* 8  AK47          KF7 Soviet */
	0x63,  /* 9  UZI           ZMG (9mm) */
	0x64,  /* 10 MP5K          D5K Deutsche */
	0x65,  /* 11 MP5KSIL       D5K (silenced) */
	0x66,  /* 12 SPECTRE       Phantom */
	0x67,  /* 13 M16           AR33 Assault Rifle */
	0x68,  /* 14 FNP90         RC-P90 */
	0x69,  /* 15 SHOTGUN       Shotgun */
	0x6a,  /* 16 AUTOSHOT      Automatic Shotgun */
	0x6b,  /* 17 SNIPERRIFLE   Sniper Rifle */
	0x6c,  /* 18 RUGER         Cougar Magnum */
	0x6d,  /* 19 GOLDENGUN     Golden Gun */
	0x5e,  /* 20 SILVERWPPK    a PP7; the port has no silver one of its own */
	0x5e,  /* 21 GOLDWPPK      a PP7; nor a gold one */
	0x6e,  /* 22 LASER         Moonraker laser */
	0x6e,  /* 23 WATCHLASER    the watch laser is the same beam */
	0x6f,  /* 24 GRENADELAUNCH Grenade Launcher */
	0x70,  /* 25 ROCKETLAUNCH  Rocket Launcher */
	0x73,  /* 26 GRENADE       Grenade */
	0x74,  /* 27 TIMEDMINE     Timed Mine */
	0x75,  /* 28 PROXIMITYMINE Proximity Mine */
	0x76,  /* 29 REMOTEMINE    Remote Mine */
};

/** Whether an item past the guns is one of the gadgets a mission hands Bond (gesolo.py's GE_GADGET_WEAPON). */
static int soloGadgetItem(int32_t item)
{
	switch (item) {
	case 34: case 38: case 39: case 40: case 46: case 47: case 50: case 55: case 60: case 61: case 73:
		return 1;
	}

	return 0;
}

/**
 * GoldenEye's gadgets as weapons of the port's own past its guns (gesolo.py's
 * GE_GADGET_WEAPON, gegadgets.c): three thrown and sticking, the camera, the
 * watch magnet, and the six with no model in the hand, which share two numbers
 * by the mission. 0 for anything else.
 */
static uint32_t soloGadgetWeapon(uint32_t item)
{
	switch (item) {
	case 47: return 0x77; // BUG: the covert modem, the tracker bug
	case 34: return 0x78; // PLASTIQUE
	case 61: return 0x79; // GOLDENEYEKEY
	case 40: return 0x7a; // CAMERA
	case 60: return 0x7b; // WATCHMAGNETATTRACT
	case 38: case 39: case 46: case 50: return 0x7c; // door decoder, bomb defuser, key analyser, guidance data
	case 55: case 73: return 0x7d; // data thief, DAT tape
	}

	return 0;
}

/** A GoldenEye item id as the weapon Perfect Dark equips for it. */
static uint32_t soloItemWeapon(uint32_t item)
{
	if (soloGadgetWeapon(item)) {
		return soloGadgetWeapon(item);
	}

	return item < sizeof(g_GeItemWeapon) ? g_GeItemWeapon[item] : 0;
}

/**
 * GoldenEye's ammunition types (bondconstants.h, AMMOTYPES) as the types the
 * port's GoldenEye guns draw on. Its one pool of 9mm was two here while each
 * gun took its host's type (Perfect Dark's pistol rounds for the PP7 and the
 * DD44, its submachine gun rounds for the rest), so a grant of it fills both;
 * since every 9mm gun draws on the submachine gun's (geguns.c, geammotypes[])
 * the pistol half is left over and harmless. Everything past the golden
 * bullet is a gadget's count, which nothing in the port holds. gesolo.py's
 * GE_AMMO_TYPES.
 */
static const uint8_t g_GeAmmoTypes[24][2] = {
	{ 0, 0 },
	{ 0x01, 0x02 },    // 9MM            pistol and SMG
	{ 0x01, 0x02 },    // 9MM_2
	{ 0x04, 0 },       // RIFLE
	{ 0x05, 0 },       // SHOTGUN
	{ 0x07, 0 },       // GRENADE
	{ 0x08, 0 },       // ROCKETS
	{ 0x0c, 0 },       // REMOTEMINE
	{ 0x0d, 0 },       // PROXMINE
	{ 0x0e, 0 },       // TIMEDMINE
	{ 0x09, 0 },       // KNIFE
	{ 0x0b, 0 },       // GRENADEROUND   the grenade launcher stands on the Devastator
	{ 0x0a, 0 },       // MAGNUM
	{ 0x11, 0 },       // GGUN           golden bullets, a pool of their own (AMMOTYPE_GOLDENGUN): Egyptian's crate
	[20] = { 0x20, 0 }, // BUG           the covert modem stands on the ECM mine
	[22] = { 0x20, 0 }, // GEKEY         and so do the GoldenEye key
	[23] = { 0x20, 0 }, // PLASTIQUE     and the plastique
};

/** The port's type for one of GoldenEye's, the first or the second; 0 for none. */
static uint32_t soloAmmoType(uint32_t getype, int which)
{
	return getype < sizeof(g_GeAmmoTypes) / sizeof(g_GeAmmoTypes[0]) ? g_GeAmmoTypes[getype][which] : 0;
}

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
	set16(out, 0x4e, geObjHealth(raw));
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
 * A GoldenEye AI list id as the converted level's.
 *
 * GoldenEye's own **global** AI lists - chraidata.c's g_GlobalAILists, the
 * eighteen every level shares: the standard guard, the simple guard, the attack,
 * the idle animations, the keyboard basher, the alarm raiser - are named by an
 * id of 1024 or less (bondconstants.h's isGlobalAIListID), and a level's own
 * lists start at 1025. Perfect Dark draws that line in the same place
 * (lib/ailist.c: 0x401 and up is the stage's, below it the game's own
 * g_GlobalAilists), so a guard whose list was GoldenEye's global 2 ran *Perfect
 * Dark's* global 2 - ten of Dam's thirty-six did, and every mission was running
 * Perfect Dark's guard AI over GoldenEye's levels.
 *
 * The eighteen are converted into every mission (writeSoloAilists) with ids of
 * their own, and every reference to one is moved with them. **The ids have to
 * sit under 0x1000**: Perfect Dark makes a background chr of every stage list
 * from there up (game_00b820.c) and ticks it from the first frame, so at 0x2000
 * the eighteen were each ticked as a chr of their own and m_RunToBond took the
 * game down in chrGoToRoomPos() with no prop to move. 0x800 is above the highest
 * id any of the twenty missions gives a list of its own (1066) and below the
 * background lists' 0x1000.
 */
#define GE_GLOBAL_AI_AT     0x8003744c
#define GE_GLOBAL_AI_FIRST  0x0800
#define GE_GLOBAL_AI_LAST   1024

static uint32_t soloGlobalAiId(uint32_t id)
{
	return id <= GE_GLOBAL_AI_LAST ? GE_GLOBAL_AI_FIRST + id : id;
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
	set16(out, 0x0e, soloGlobalAiId(ailist));
	set16(out, 0x10, padNum(preset, numpads, 0));
	set16(out, 0x12, chrpreset);
	set16(out, 0x14, hearscale);
	set16(out, 0x16, viewdist);
	set16(out, 0x22, 0xffff);   // no chair
}

/**
 * A GoldenEye collectable as a Perfect Dark weapon prop, on the port's own
 * GoldenEye weapons (geguns.c).
 *
 * The record's `weaponnum` is one of GoldenEye's item ids - its own code
 * compares it against ITEM_GRENADE and ITEM_TIMEDMINE (chr.c) - and those are
 * not the order the port's twenty-five are in, so it goes through
 * g_GeItemWeapon. gesolo.py's weapon_record().
 */
static void weaponRecord(uint8_t *out, const uint8_t *raw, size_t numpads)
{
	const uint32_t item = raw[0x80];

	baseRecord(out, raw, 0x08, padNum(be16(raw, 6), numpads, 0));
	out[0x5c] = item >= 2 ? (uint8_t)soloItemWeapon(item) : 0;
	// no second gun: dualweaponnum (0x61) is -1, as the stock weapon() macro
	// writes it. Until converter 72 this wrote 0x5d/0x5e (the gset's two
	// spare bytes) and left dualweaponnum 0, so invGiveWeaponsByProp() took
	// every converted pickup for one half of a pair: a gun that cannot be
	// held in two hands counted as "not given" and the player was told only
	// of its ammunition ("Picked up an ." for Bunker's GoldenEye key), and
	// one that can was handed over as a pair with WEAPON_NONE
	out[0x61] = 0xff;
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
	int props, dropped, aikept, aidropped, aiduplicate;
	uint8_t *anims;   // one byte per GoldenEye animation id, set when named
	uint8_t *models;  // the conversion's set of prop models, for a gun a list hands out
};

/**
 * GoldenEye's cutscene camera - the thing a CameraSwitch turns the view to -
 * moved into the converted level.
 *
 * The record is GoldenEye's CutsceneRecord and Perfect Dark's cameraposobj,
 * and Perfect Dark's own setup load still does GoldenEye's conversion on it
 * (setup.c: the position over 100 and the angles over 65536, out of the
 * integers the file holds). So it stays in GoldenEye's encoding and only the
 * position and a bound pad's number move. gesolo.py's camera_record().
 */
static void cameraRecord(uint8_t *out, const uint8_t *raw, size_t len, size_t numpads, const double *offset)
{
	memcpy(out, raw, len < 28 ? len : 28);
	out[3] = 0x2e;

	for (int i = 0; i < 3; ++i) {
		int32_t p = (int32_t)be32(raw, 4 + 4 * i);

		if (offset) {
			p -= (int32_t)lround(offset[i] * 100.0);
		}

		set32(out, 4 + 4 * i, (uint32_t)p);
	}

	set32(out, 0x18, padNum(be32(raw, 0x18) & 0xffff, numpads, 0));
}

/**
 * GoldenEye's crate of several kinds of ammunition as Perfect Dark's. Both keep
 * a (model, quantity) pair for every ammunition type, indexed by the type less
 * one - thirteen of GoldenEye's, nineteen of Perfect Dark's - so a pair moves
 * to the slot of the type the port's guns draw on, 9mm filling both of its
 * pools. The model is left at none: setupCreateProps() loads a slot's model
 * only to have it ready, and a crate is picked up whole. gesolo.py's
 * multi_crate_record().
 */
static void multiCrateRecord(uint8_t *out, const uint8_t *raw, size_t len, size_t numpads)
{
	baseRecord(out, raw, 0x14, padNum(be16(raw, 6), numpads, 0));

	for (size_t i = 0; i < 19; ++i) {
		set16(out, 0x5c + 4 * i, 0xffff);
		set16(out, 0x5c + 4 * i + 2, 0);
	}

	for (size_t k = 0; k < 13 && 0x80 + 4 * k + 4 <= len; ++k) {
		const uint32_t qty = be16(raw, 0x80 + 4 * k + 2);

		for (int which = 0; qty && which < 2; ++which) {
			const uint32_t pdtype = soloAmmoType((uint32_t)k + 1, which);

			if (pdtype) {
				const size_t at = 0x5c + 4 * (pdtype - 1);
				const uint32_t sum = be16(out, at + 2) + qty;

				set16(out, at + 2, sum > 0xffff ? 0xffff : sum);
			}
		}
	}
}

/**
 * menu/gesets.bin: GoldenEye's own multiplayer weapon sets, which GE Plus's
 * arenas are played with (gexplusrom.c) - "GES1", a count, and a row a set of
 * its name and its eight weapons as the port's own GoldenEye guns.
 *
 * The table is mp_weapon.c's mp_weapon_set_text_table: fourteen rows of a text
 * id and a pointer to eight 24-byte slots, whose first word is the item
 * (GE_ITEM_WEAPON again; the unarmed hand of Slappers Only stays the unarmed
 * hand). The name is the row's own string out of LmpweaponsE.
 */
#define MPSETS_AT   0x800490f0u
#define MPSETS_NUM  14
#define MPSET_SLOTS 8
#define MPSET_NAME  32

static void writeFile(const char *outdir, const char *rel, const uint8_t *data, size_t len);

static void writeWeaponSets(const char *outdir)
{
	const size_t table = MPSETS_AT - DATA_VRAM;
	buf lang = romFile("LmpweaponsE");
	buf out = {0};

	if (table + 8 * MPSETS_NUM > g_DataLen) {
		fail("the weapon sets run off the data segment");
	}

	bufPut(&out, (const uint8_t *)"GES1", 4);
	bufU32(&out, MPSETS_NUM);

	for (size_t i = 0; i < MPSETS_NUM; ++i) {
		const uint32_t textid = be16(g_Data, table + 8 * i);
		const uint32_t ptr = be32(g_Data, table + 8 * i + 4);
		const size_t slots = ptr - DATA_VRAM;
		const size_t index = textid & 0x3ff;
		char name[MPSET_NAME] = {0};

		if (ptr < DATA_VRAM || slots + 24 * MPSET_SLOTS > g_DataLen) {
			fail("a weapon set outside the data segment");
		}

		if ((index + 1) * 4 <= lang.n) {
			const uint32_t at = be32(lang.v, index * 4);

			if (at && at < lang.n) {
				size_t k = 0;

				while (k + 1 < sizeof(name) && at + k < lang.n && lang.v[at + k] && lang.v[at + k] != '\n') {
					name[k] = (char)lang.v[at + k];
					++k;
				}
			}
		}

		bufPut(&out, (const uint8_t *)name, sizeof(name));

		for (size_t k = 0; k < MPSET_SLOTS; ++k) {
			bufU8(&out, soloItemWeapon(be32(g_Data, slots + 24 * k)));
		}
	}

	writeFile(outdir, "menu/gesets.bin", out.v, out.n);
}

static buf writeSoloProps(const buf *f, size_t numpads, uint8_t *models, struct solostats *st,
		const double *offset)
{
	// the tails of the ObjectRecord types Perfect Dark keeps in the same order:
	// GoldenEye's 0x80 onwards against Perfect Dark's 0x5c
	static const struct { uint8_t type, ge, pd, width; } tails[] = {
		{ 0x04, 0x80, 0x5c, 4 },                            // key: the key flags
		{ 0x07, 0x80, 0x5c, 4 },                            // ammo crate: the ammo type
		{ 0x15, 0x80, 0x5c, 4 }, { 0x15, 0x84, 0x60, 4 },   // armour: initial and current
		// a truck and an aircraft each own an AI list, and its id is what both
		// games keep in the record - Perfect Dark's setupCreateProps() reads
		// the field as one and zeroes the rest of the tail itself. Without it
		// ailistFindById(0) handed every vehicle Perfect Dark's own global
		// list 0. gesolo.py's OBJ_TAILS.
		{ 0x27, 0x80, 0x5c, 4 }, { 0x28, 0x80, 0x5c, 4 },   // truck, aircraft: the AI list
		// a tank's shells: the one word of its tail the setup sets (0xd8, thirty
		// on Runway and on Streets), which is what Bond is handed as he climbs
		// in. Dropped until converter 57, and the tank had none
		{ 0x2d, 0xd8, 0x5c, 4 },
		// An autogun's turn limits, turn speed and range. GoldenEye's
		// AutogunRecord and Perfect Dark's autogunobj are the same fields in
		// the same order, 0x24 further on here, and both loads convert these
		// four out of the setup's 16.16 integers the same way (prop.c's
		// setupAutogun(), setup.c's setupCreateAutogun()). Left at nought -
		// until converter 67 - every autogun had a range of nought and could
		// not turn, and none of the 35 on eleven missions ever fired
		// (Egyptian's four round the Golden Gun among them). The pad it faces
		// is below, since it is a pad number.
		{ 0x0d, 0x88, 0x64, 4 }, { 0x0d, 0x8c, 0x68, 4 },  // ymaxleft, ymaxright
		{ 0x0d, 0xa4, 0x80, 4 }, { 0x0d, 0xa8, 0x84, 4 },  // maxspeed, aimdist
		// A security camera's sweep: GoldenEye's CCTVRecord and Perfect
		// Dark's cctvobj hold the same fields, but GoldenEye keeps its s32
		// toleft among them (0xd4) where Perfect Dark moved it beside the pad
		// (0x5e), so the run after it is 0x28 on rather than 0x24. Both loads
		// convert yleft, yright and ymaxspeed out of 16.16 turns and maxdist
		// out of an integer (prop.c's setupCctv(), setup.c's
		// setupCreateCctv()). Left at nought until converter 73, with the pad
		// below: every camera stood still, and looked at pad 0 - the tester's
		// "cameras do not rotate" (Bunker) and "facing backwards" (Surface)
		{ 0x06, 0xcc, 0xa8, 4 }, { 0x06, 0xd0, 0xac, 4 },  // yleft, yright
		{ 0x06, 0xdc, 0xb4, 4 }, { 0x06, 0xe8, 0xbc, 4 },  // ymaxspeed, maxdist
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

		if (t == 0x22) {
			// GoldenEye's "copy the item" objective, which Perfect Dark has no
			// record for: a complete-on-flag one on a stage flag the key
			// analyser sets (gesolo.py's GE_COPYITEM_FLAG)
			bufZeros(&out, 8);
			rec = out.v + out.n - 8;
			memcpy(rec, raw, 3);
			rec[3] = 0x1a;
			set32(rec, 4, 0x80000000);
			st->props++;
			continue;
		}

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
		} else if (t == 0x2e) {
			cameraRecord(rec, raw, recs.v[i].len, numpads, offset);
		} else if (t == 0x14) {
			multiCrateRecord(rec, raw, recs.v[i].len, numpads);
		} else if (g_GeSizes[t] >= 32) {
			baseRecord(rec, raw, t, padNum(be16(raw, 6), numpads, 0));
			for (size_t k = 0; k < sizeof(tails) / sizeof(tails[0]); ++k) {
				if (tails[k].type == t && tails[k].ge + tails[k].width <= recs.v[i].len) {
					memcpy(rec + tails[k].pd, raw + tails[k].ge, tails[k].width);
				}
			}
			if (t == 0x0d && recs.v[i].len >= 0x84) {
				// the pad the autogun rests facing: GoldenEye's s32 at 0x80,
				// Perfect Dark's s16 at 0x5c, -1 for none (gesolo.py's
				// AUTOGUN)
				const int32_t target = bes32(raw, 0x80);

				set16(rec, 0x5c, target < 0 ? 0xffff : padNum((uint32_t)target, numpads, 0));
			}
			if (t == 0x06 && recs.v[i].len >= 0x84) {
				// the pad a camera looks at: GoldenEye's s32 at 0x80, Perfect
				// Dark's s16 lookatpadnum at 0x5c, -1 for none (gesolo.py's CCTV)
				const int32_t look = bes32(raw, 0x80);

				set16(rec, 0x5c, look < 0 ? 0xffff : padNum((uint32_t)look, numpads, 0));
			}
			if (t == 0x0a && recs.v[i].len >= 0xfc) {
				// A hanging TV's mount. GoldenEye's MonitorObjRecord ends
				// OwnerOffset, OwnerPart, ImageNum, a word each at 0xf4, and
				// Perfect Dark's singlemonitorobj ends s16 owneroffset, s8
				// ownerpart, u8 imagenum at 0xd0. A monitor with a negative pad
				// hangs from the record that many commands away, and with the
				// offset left at nought it hung from itself - a prop that is its
				// own child, which objFree() frees for ever on the way out of
				// the level: both Bunkers ended the game when they were left.
				// gesolo.py's MONITOR.
				set16(rec, 0xd0, be32(raw, 0xf4) & 0xffff);
				rec[0xd2] = (uint8_t)(be32(raw, 0xf8) & 0xff);
				// and the programme it shows, one of GoldenEye's fifty-two
				// (gemonitortable.h), which the port plays on a remake stage
				rec[0xd3] = (uint8_t)(be32(raw, 0xfc) & 0xff);
			}
			if (t == 0x0b && recs.v[i].len >= 0x254) {
				// the four screens of a bank of monitors: a byte each after the
				// four MonitorRecords, which are 0x74 in both games
				memcpy(rec + 0x22c, raw + 0x250, 4);
			}
			if (t == 0x07 && recs.v[i].len >= 0x84) {
				// the crate's one type, in the port's numbering
				set32(rec, 0x5c, soloAmmoType(be32(raw, 0x80), 0));
			}
			if (t == 0x2f && recs.v[i].len >= 0x94) {
				// A tinted pane's distances and its portal. GoldenEye's
				// TintedGlassRecord ends TintDist, CullDist, the opacity and
				// the portal as a word each at 0x80, then a 16.16 fraction,
				// and Perfect Dark's tintedglassobj is the same five with the
				// first four as s16 at 0x5c. Every one of GoldenEye's carries
				// a portal of -1, and one that has a portal finds it at the
				// load in both games. Left at nought, a pane was opaque at any
				// distance and its portal was **portal 0**, which both games
				// shut while the pane is opaque: whatever rooms the level's
				// first portal joins stopped being drawn through it, on every
				// mission that has a pane at all. gesolo.py's TINTED_GLASS.
				for (int k = 0; k < 4; ++k) {
					int32_t v = bes32(raw, 0x80 + 4 * k);

					v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
					set16(rec, 0x5c + 2 * k, (uint16_t)v);
				}
				memcpy(rec + 0x64, raw + 0x90, 4);
			}
		} else {
			// a short record: the same fields in the same order on both sides
			const size_t keep = 4 * (size_t)words < recs.v[i].len ? 4 * (size_t)words : recs.v[i].len;
			memcpy(rec, raw, keep);
			rec[3] = (uint8_t)t;

			if (t == 0x20 || t == 0x21) {
				// the two objectives that name a pad's room, which Perfect
				// Dark reads as a room number unless it is a pad plus 10000;
				// the second names its item too (gesolo.py's
				// objective_room_record())
				const size_t at = t == 0x21 ? 8 : 4;
				const int32_t pad = bes32(raw, at);

				if (t == 0x21) {
					set32(rec, 4, soloItemWeapon(be32(raw, 4)));
				}

				if (pad >= 0 && pad < 0xffff) {
					set32(rec, at, padNum((uint32_t)pad, numpads, 0) + 10000);
				}
			} else if (t == 0x25) {
				// a rename: the item as the port's weapon and the five texts
				// out of the mission's own bank (gesolo.py's rename_record())
				const int32_t item = bes32(raw, 8);

				set32(rec, 8, item > 0 ? soloItemWeapon((uint32_t)item) : 0);

				for (size_t k = 0; k < 5; ++k) {
					set32(rec, 12 + 4 * k, soloTextId(be32(raw, 12 + 4 * k) & 0xffff));
				}
			}
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
static buf writeSoloIntro(const buf *f, size_t numpads, double levelscale, const double *offset)
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

		if (t == 6) {
			// GoldenEye's own opening camera shot, moved into the converted
			// level: where the camera stands (hundredths of a GoldenEye unit),
			// the yaw and pitch it looks along (16.16 radians), the pad whose
			// room it is in, and the one or two lines it shows. Nothing in
			// Perfect Dark reads the record - it steps over it by its length -
			// so the fields are rewritten as the port wants them. gesolo.py's
			// intro_camera().
			const uint8_t *raw = f->v + o;
			const size_t start = out.n;

			bufPut(&out, raw, 40);

			// a hundredth of one of these is already a unit of the
			// *converted* level - GoldenEye's own units times its level
			// scale - so only the level's offset is taken off
			for (int i = 0; i < 3; ++i) {
				const double p = (double)(int32_t)be32(raw, 4 + 4 * i) / 100.0;
				setf32(out.v, start + 4 + 4 * i, offset ? p - offset[i] : p);
			}

			setf32(out.v, start + 0x10, (double)(int32_t)be32(raw, 0x10) / 65536.0);
			setf32(out.v, start + 0x14, (double)(int32_t)be32(raw, 0x14) / 65536.0);
			set32(out.v, start + 0x18, padNum(be32(raw, 0x18) & 0xffff, numpads, 0));
			set32(out.v, start + 0x1c, soloTextId(be32(raw, 0x1c) & 0xffff));
			set32(out.v, start + 0x20, soloTextId(be32(raw, 0x20) & 0xffff));
		} else if (t == 3) {
			// One point of the camera's swirl down to Bond, as floats. GoldenEye
			// converts the record in place when the level loads (bondview_r.c:
			// the offset from Bond, the spline's scale and the leg's duration,
			// each a 16.16 word over 65536) and nothing in Perfect Dark reads an
			// INTROCMD_3 at all, so it is done here and the port reads floats
			// (gecinema.c). The offset is in GoldenEye's own runtime units,
			// which are the converted level's; the last word is a pad or -1.
			// gesolo.py's intro_swirl().
			const uint8_t *raw = f->v + o;
			const size_t start = out.n;
			const int32_t pad = (int32_t)be32(raw, 0x1c);

			bufPut(&out, raw, 32);

			for (int i = 0; i < 5; ++i) {
				setf32(out.v, start + 8 + 4 * i, (double)(int32_t)be32(raw, 8 + 4 * i) / 65536.0);
			}

			set32(out.v, start + 0x1c, pad >= 0 ? padNum((uint32_t)pad & 0xffff, numpads, 0) : 0xffffffff);
		} else if (t == 1) {
			// What Bond starts with, as the port's own GoldenEye guns. The
			// command's two items are GoldenEye's item ids, as a collectable's
			// is, and copied as they were they are read as Perfect Dark's
			// weapon numbers: Dam's silenced PP7, item 5, was a MagSec 4. An
			// item that is not a weapon - the covert modem, the bomb case - is
			// nothing the port can put in a hand, and its command is left out.
			// gesolo.py's intro_item().
			const uint8_t *raw = f->v + o;
			const int32_t rightitem = (int32_t)be32(raw, 4);
			const int32_t leftitem = (int32_t)be32(raw, 8);
			const uint32_t right = rightitem >= 0 ? soloItemWeapon((uint32_t)rightitem) : 0;
			const uint32_t left = leftitem >= 0 ? soloItemWeapon((uint32_t)leftitem) : 0;

			if (right) {
				bufU32(&out, be32(raw, 0));
				bufU32(&out, right);
				bufU32(&out, left ? left : 0xffffffff);
				bufU32(&out, be32(raw, 12));
			}
		} else if (t == 2) {
			// and his ammunition, in the pools the port's guns draw on.
			// gesolo.py's intro_ammo().
			const uint8_t *raw = f->v + o;

			for (int which = 0; which < 2; ++which) {
				const uint32_t pdtype = soloAmmoType(be32(raw, 4), which);

				if (pdtype) {
					bufU32(&out, be32(raw, 0));
					bufU32(&out, pdtype);
					bufU32(&out, be32(raw, 8));
					bufU32(&out, be32(raw, 12));
				}
			}
		} else {
			bufPut(&out, f->v + o, 4 * (size_t)words[t]);
		}

		o += 4 * (size_t)words[t];
	}

	bufU32(&out, 12);
	return out;
}

/**
 * GoldenEye's patrol paths as Perfect Dark's: the same record - a pointer to a
 * -1 terminated list, an id, a loop flag and a length - but **not the same
 * list**. GoldenEye's is of *waypoints* (chraction.c's chrlvGetPatrolStepPad():
 * `pads[pathwaypoints[path->data[step]].padID]`, and the truck's tick reads its
 * path the same way) and Perfect Dark's is of *pads* (`path->pads[step]`,
 * straight into padUnpack()). Copied as it was, a waypoint's index was read as a
 * pad's number, so every guard on patrol walked for pads that were never on its
 * route and Dam's truck turned round and drove the wrong way down the road.
 * Each entry goes through the setup's own waypoint table to the pad it stands
 * on. gesolo.py's convert_paths().
 */
static void writeSoloPaths(const buf *f, size_t at, size_t numpads, buf *head, buf *body)
{
	const uint32_t start = be32(f->v, 16);
	const uint32_t ways = be32(f->v, 0);
	size_t numways = 0;
	size_t n = 0, pos;

	if (!start) {
		bufU32(head, 0);
		bufU32(head, 0);
		return;
	}

	// the waypoints: 16 bytes each, the pad first, to a pad below zero
	for (size_t o = ways; ways && o + 16 <= f->n && (int32_t)be32(f->v, o) >= 0; o += 16) {
		++numways;
	}

	for (size_t o = start; o + 8 <= f->n && be32(f->v, o); o += 8) {
		++n;
	}

	pos = at + 8 * (n + 1);

	for (size_t i = 0; i < n; ++i) {
		const size_t o = start + 8 * i;
		const s32s list = readS32List(f, be32(f->v, o));
		size_t kept = 0;

		bufU32(head, (uint32_t)pos);
		bufU8(head, f->v[o + 4]);
		bufU8(head, f->v[o + 5]);
		bufU16(head, be16(f->v, o + 6));

		for (size_t k = 0; k < list.n; ++k) {
			if (list.v[k] >= 0 && (size_t)list.v[k] < numways) {
				bufU32(body, padNum(be32(f->v, ways + 16 * (size_t)list.v[k]), numpads, 0));
				++kept;
			}
		}
		bufU32(body, 0xffffffff);
		pos += 4 * (kept + 1);
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

/**
 * One GoldenEye AI list as Perfect Dark bytecode (geaitable.h).
 *
 * `vehicle` says the list belongs to a truck, helicopter or tank rather than to
 * a guard. PlayAnimation means a different table there - the three of
 * `animation_table_ptrs2[]`, played straight on the vehicle's own model - so it
 * becomes the port's own command with an id out of the vehicles' own space.
 */
static void writeSoloAilist(const buf *f, size_t at, size_t numpads, int vehicle, buf *out,
		struct solostats *st, const double *offset)
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

		if (op == 0x0a) {   // PlayAnimation
			const uint32_t anim = ((uint32_t)f->v[at + 1] << 8) | f->v[at + 2];

			if (vehicle) {
				// the list belongs to a truck or an aircraft, so the id means
				// one of animation_table_ptrs2[]'s three, played straight on
				// the vehicle's model. It becomes the port's own command with
				// the id taken out of the vehicles' own space; GoldenEye's
				// bitfield has no meaning here, its own aircraft branch
				// reading nothing but the interpolation time.
				if (anim >= (uint32_t)GEVEH_NUM_ANIMS) {
					st->aidropped++;
				} else {
					bufU16(out, GEVEH_ANIM_CMD);
					bufU16(out, GEAI_ANIM_TAG | (uint32_t)(GEVEH_ANIM_FIRST + anim));
					bufU16(out, ((uint32_t)f->v[at + 3] << 8) | f->v[at + 4]);
					bufU16(out, ((uint32_t)f->v[at + 5] << 8) | f->v[at + 6]);
					bufU8(out, f->v[at + 8]);
					st->aikept++;
				}

				at += len;
				continue;
			}

			if (anim >= (uint32_t)GEANIM_NUM_ANIMS) {
				st->aidropped++;
				at += len;
				continue;
			}

			if (st->anims) {
				st->anims[anim] = 1;
			}
		}

		if (op == 0xd6) {
			// IFBondYPosLessThan has no twin in Perfect Dark and becomes the
			// port's own command (gesolo.py's GE_IFBONDY_CMD): the height is
			// in GoldenEye's runtime world and moves by the level's offset,
			// into four bytes since a moved one need not fit two
			int32_t y = bes16(f->v, at + 1);
			if (offset) {
				y -= (int32_t)lround(offset[1]);
			}
			bufU16(out, GEAI_IFBONDY_CMD);
			bufU32(out, (uint32_t)y);
			bufU8(out, f->v[at + 3]);
			st->aikept++;
			at += len;
			continue;
		}

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
					// the four that ask about a pad's *room* (gesolo.py's
					// GE_PADROOM_OPS): Perfect Dark reads the argument as a
					// room number unless it is a pad plus 10000
					if ((op == 0x44 || op == 0x54 || op == 0x55 || op == 0xe6) && v != 0xffff) {
						v += 10000;
					}
				} else if (cmd->getext & (1u << i)) {
					v = soloTextId(v);
				} else if (cmd->geanim & (1u << i)) {
					v = GEAI_ANIM_TAG | v;
				} else if (cmd->gelist & (1u << i)) {
					v = soloGlobalAiId(v);
				} else if ((op == 0xe3 || op == 0xe4) && i == 0) {
					// the two commands that put an item in Bond's hands
					// (geaitable.h rows e3 and e4); everything else
					// GoldenEye calls an ITEM_NUM is left as it is
					v = soloItemWeapon(v);
				} else if ((op == 0x7f || op == 0x80) && i == 0) {
					// the two that ask about Bond's own health: GoldenEye's
					// threshold is a byte where 255 is a full one, Perfect
					// Dark's aiIfChrHealth*Than scales its own by a tenth and
					// compares it against bondhealth * 8, where a full one is
					// 80 (gesolo.py's GE_BOND_HEALTH_FULL)
					v = v * 80 / 255;
				} else if (op >= 0x9d && op <= 0xa2 && cmd->gewidth[i] == 4 && (v & 0x1000)) {
					// GoldenEye's CHRFLAG_LOCK_Y_POS is Perfect Dark's
					// CHRCFLAG_UNEXPLODABLE: it goes to the port's own
					// CHRCFLAG_GE_LOCKY (gesolo.py's GE_CHRFLAG_OPS)
					v = (v & ~0x1000u) | 0x40000000u;
				}

				vals[i] = v;
			}

			// The commands that name a gun somebody holds (gesolo.py's
			// GE_EQUIP_OPS and GE_GIVE_OPS). IFBondHasItemEquipped's item is one
			// of GoldenEye's ids like the two equips' above; and TRYGiveMeItem
			// and TRYDroppingItem name the gun twice, by its prop number and by
			// its item id, both of which were copied as they stood until
			// converter 42 - and both are numbers Perfect Dark has a meaning of
			// its own for. GoldenEye's prop 0xbf is the PP7 and Perfect Dark's
			// model 0xbf a dataDyne lab door; its item 4 the PP7 and Perfect
			// Dark's weapon 4 the Mauler. A hundred and five commands over the
			// twenty missions: every guard a list armed carried a door for a
			// gun, and so did Bond in Archives' ending. The prop becomes the
			// remake's own model, as a setup record's does.
			if (op == 0x59 || op == 0x57 || op == 0x58) {
				vals[0] = soloItemWeapon(vals[0]);
			} else if (op == 0xbf || op == 0x1b) {
				if (st->models) {
					setAdd(st->models, vals[0]);
				}

				vals[0] = (uint32_t)MODEL_REMAKE_FIRST + vals[0];
				vals[1] = soloItemWeapon(vals[1]);
			}

			bufU16(out, (uint32_t)cmd->pd);

			for (int i = 0; i < cmd->numargs; ++i) {
				const struct geaiarg *a = &cmd->args[i];
				uint32_t v = a->from < 0 ? a->value : vals[a->from];

				if (a->mask) {
					v &= a->mask;
				}

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

// The propdefs that run an AI list on a vehicle rather than on a guard: truck,
// helicopter and tank, each with its list's id where a guard record has none
// (bondtypes.h, VehichleRecord/AircraftRecord `ailist` at 0x80)
static int soloListIsVehicle(const records *recs, uint32_t lid)
{
	for (size_t i = 0; i < recs->n; ++i) {
		const uint32_t t = recs->v[i].type;

		if ((t == 39 || t == 40 || t == 45) && recs->v[i].len >= 0x84
				&& (be32(recs->v[i].b, 0x80) & 0xffff) == lid) {
			return 1;
		}
	}

	return 0;
}

/**
 * The mission's own AI lists, and GoldenEye's global ones after them.
 *
 * The table is **sorted by id and holds each id once**, which GoldenEye's own
 * file is not obliged to be: its ailistFindById() walks the rows and takes the
 * first of a duplicate, while Perfect Dark binary-searches them (lib/ailist.c)
 * and can miss a list altogether - Facility carries 1063 twice and Surface has
 * 1051 before 1049 and 4106 twice. Keeping the first of each id and sorting is
 * GoldenEye's own answer in the order Perfect Dark has to have it in.
 */
#define GE_MAX_AILISTS 256

struct gesololist {
	const buf *from;   // the setup file, or the data segment for a global list
	uint32_t at;
	uint32_t id;
	int vehicle;
};

static void writeSoloAilists(const buf *f, size_t at, size_t numpads, buf *head, buf *code, struct solostats *st,
		const double *offset)
{
	const uint32_t start = be32(f->v, 20);
	records recs = setupRecords(f);
	struct gesololist rows[GE_MAX_AILISTS];
	buf seg = { g_Data, g_DataLen };
	size_t n = 0, pos;

	if (!start) {
		bufU32(head, 0);
		bufU32(head, 0);
		return;
	}

	for (size_t o = start; o + 8 <= f->n && (be32(f->v, o) || be32(f->v, o + 4)); o += 8) {
		if (n >= GE_MAX_AILISTS) {
			fail("a mission has more than %d AI lists", (int)GE_MAX_AILISTS);
		}

		rows[n].from = f;
		rows[n].at = be32(f->v, o);
		rows[n].id = be32(f->v, o + 4) & 0xffff;
		rows[n].vehicle = soloListIsVehicle(&recs, rows[n].id);
		++n;
	}

	// GoldenEye's own eighteen, out of the data segment (soloGlobalAiId)
	for (size_t i = 0;; ++i) {
		const size_t o = GE_GLOBAL_AI_AT - DATA_VRAM + 8 * i;
		uint32_t ptr;

		if (o + 8 > g_DataLen) {
			fail("GoldenEye's global AI lists run off the data segment");
		}

		ptr = be32(g_Data, o);

		if (!ptr) {
			break;
		}

		if (n >= GE_MAX_AILISTS) {
			fail("a mission has more than %d AI lists", (int)GE_MAX_AILISTS);
		}

		rows[n].from = &seg;
		rows[n].at = ptr - DATA_VRAM;
		rows[n].id = soloGlobalAiId(be32(g_Data, o + 4) & 0xffff);
		rows[n].vehicle = 0;
		++n;
	}

	// the first of each id, then sorted: a stable insertion sort, so what is
	// kept of a duplicate is GoldenEye's own first row
	for (size_t i = 0; i < n; ++i) {
		for (size_t j = 0; j < i; ++j) {
			if (rows[j].id == rows[i].id) {
				st->aiduplicate++;
				memmove(&rows[i], &rows[i + 1], (n - i - 1) * sizeof(rows[0]));
				--n;
				--i;
				break;
			}
		}
	}

	for (size_t i = 1; i < n; ++i) {
		const struct gesololist row = rows[i];
		size_t j = i;

		while (j > 0 && rows[j - 1].id > row.id) {
			rows[j] = rows[j - 1];
			--j;
		}

		rows[j] = row;
	}

	pos = at + 8 * (n + 1);

	for (size_t i = 0; i < n; ++i) {
		const size_t before = code->n;

		writeSoloAilist(rows[i].from, rows[i].at, numpads, rows[i].vehicle, code, st, offset);
		bufU32(head, (uint32_t)pos);
		bufU32(head, rows[i].id);
		pos += code->n - before;
	}

	bufU32(head, 0);
	bufU32(head, 0);
}

/** A GoldenEye solo setup as a Perfect Dark one (gesolo.py's convert()). */
static buf writeSoloSetup(const buf *f, size_t numpads, uint8_t *models, struct solostats *st,
		double levelscale, const double *offset)
{
	buf intro = writeSoloIntro(f, numpads, levelscale, offset);
	buf props = writeSoloProps(f, numpads, models, st, offset);
	buf paths = {0}, pathpads = {0}, ailists = {0}, aicode = {0}, out = {0};
	const size_t introat = 0x20;
	const size_t propsat = introat + intro.n;
	const size_t pathsat = propsat + props.n;
	size_t aiat;

	writeSoloPaths(f, pathsat, numpads, &paths, &pathpads);
	aiat = pathsat + paths.n + pathpads.n;
	writeSoloAilists(f, aiat, numpads, &ailists, &aicode, st, offset);

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

		// a character's shadow (the blob it stands on): Perfect Dark's model
		// format has no such node and the port's model preprocessing refuses
		// one, and every one of the 42 in the ROM is a childless leaf at the
		// end of its chain, so leaving it out relinks nothing
		if ((n.type & 0xff) != 0x0d) {
			VECPUSH(*out, n);

			if (n.child) {
				modelWalk(d, n.child, o, out, depth + 1);
			}
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

/**
 * Where a texture the model file carries itself ended up when it was copied
 * into the converted file: a list loads one by its own address (`G_SETTIMG`)
 * and not through its texture row, so the lists have to be moved with it.
 */
struct texmove {
	uint32_t at;    // its address in GoldenEye's file
	size_t len;
	size_t to;      // its offset in the converted one
};

static void modelLists(const buf *d, uint32_t vtxptr, const uint32_t *lists, int nlists, buf *w, int fours,
		const struct texmove *moved, int nmoved,
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
				} else if (op == 0xfd && (w1 & 0xff000000u) == SEG_MODEL) {
					// a texture the file carries itself, named by the address it
					// had in GoldenEye's file: left alone it reads whatever the
					// new layout put there, which is what drew the GoldenEye
					// logo without its red ellipse - the ring's own texel landed
					// inside the node table, where its alpha is zero - and
					// sampled the letters a few texels into their own picture
					const uint32_t a = w1 - SEG_MODEL;

					for (int m = 0; m < nmoved; ++m) {
						if (a >= moved[m].at && a - moved[m].at < moved[m].len) {
							w1 = SEG_MODEL + (uint32_t)(moved[m].to + (a - moved[m].at));
							break;
						}
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

/**
 * The bytes a texture stored in a model file takes: every mipmap level of it,
 * `depth` bytes a texel (the GoldenEye logo's is 32x32 over six levels).
 */
static size_t texDataSize(uint32_t width, uint32_t height, uint32_t level, uint32_t depth)
{
	size_t n = 0;

	for (uint32_t i = 0; i < (level ? level : 1); ++i) {
		const uint32_t w = width >> i, h = height >> i;
		n += (size_t)(w ? w : 1) * (h ? h : 1) * depth;
	}

	return (n + 7) & ~(size_t)7;
}

/**
 * The Perfect Dark skeleton id for a GoldenEye prop's skeleton pointer
 * (gemodelconv.py's GE_SKELETONS, which is this).
 *
 * A model header's second word is its skeleton: GoldenEye stores a pointer into
 * its data segment and Perfect Dark an id it resolves through g_Skeletons[] at
 * the load, so a converted prop was written with SKEL_BASIC whatever GoldenEye
 * gave it - and Perfect Dark poses a *door* by skeleton: doorInitMatrices()
 * writes matrix 0 and then, only for g_Skel11 and g_Skel13, the leaves.
 * Caverns' eyelid and iris doors have three and thirteen matrices, so twelve of
 * them were left as whatever gfxAllocate() handed over that frame: the doors
 * were not where they belonged and a leaf landed across the view often enough
 * for a tester to call it a triangle popping on screen.
 *
 * Perfect Dark kept both skeletons and poses them exactly as GoldenEye does
 * (doorInitMatrices() against GoldenEye's propobj.c render), so the two are
 * carried across.
 *
 * So is GoldenEye's windowed door (skeleton_door), which is Perfect Dark's
 * g_SkelWindowedDoor switch for switch: box, glass toggle, box, glass list, and
 * a converted model's parts are numbered by switch. GoldenEye lets a bullet
 * through the glass and breaks it on the third hit (propobj.c, Switches[3]),
 * and Perfect Dark does the same by that skeleton, so as SKEL_BASIC the glass
 * of Facility's, Bunker's, Surface's and Train's doors stopped every shot.
 *
 * The rest are named here for what they are and left at SKEL_BASIC: their
 * models are one matrix or are posed by their object type (a CCTV, an autogun,
 * a mount).
 */
static uint32_t propSkel(uint32_t skeleton, int32_t numswitches)
{
	switch (skeleton) {
	case 0x8003a100: return 0x11;  // eyelid_door -> g_Skel11 (Pdoor_eyelidZ, 3 matrices)
	case 0x8003a15c: return 0x13;  // iris_door   -> g_Skel13 (Pdoor_irisZ, 13 matrices)
	case 0x8003a1dc:               // door (windowed) -> g_SkelWindowedDoor
		return numswitches >= 4 ? 0x10 : 2;
	case 0x8003c4fc:               // prop_weapon -> g_SkelChrGun
		// switch for switch: 0 the gunfire sprite, 2 the toggle over the
		// star, which weaponSetGunfireVisible() turns on and off by
		// skeleton just as GoldenEye's does by skeleton_prop_weapon - as
		// SKEL_BASIC a guard's gun never flashed
		return numswitches >= 3 ? 0x03 : 2;
	case 0x8003a05c:               // cctv
	case 0x8003a070:               // console_one_screen
	case 0x8003a084:               // console_four_screen
	case 0x8003a0b0:               // tv_holder
	case 0x8003a0e0:               // rotating_stuff (the autoguns)
	case 0x8003a170:               // walletbond
	case 0x8003a19c:               // car
	case 0x8003a1c8:               // flying
	case 0x8003a208:               // tank
	case 0x8003a21c:               // hat
	case 0x8003c4d8:               // standard_object
	default: break;
	}

	return 2;  // SKEL_BASIC
}

/**
 * GoldenEye's prop model `num` (ischr: its character `num`) as a Perfect Dark
 * model file. A character is a prop with three more node types and its own
 * skeleton - see gechr.py, which is this.
 */
static buf modelConvertOne(int32_t num, uint8_t *images, double *scale, int ischr)
{
	const struct prop *p;
	buf d, w = {0};
	uint32_t textab, root;
	nodes nds = {0};
	VEC(struct gdlfix) gdls = {0};
	size_t texat, partsat, nodesat;
	uint32_t *switches;
	buf rows = {0};
	struct texmove *moved;
	int nmoved = 0;

	if (num < 0 || num >= (ischr == 1 ? NUM_CHRS : (ischr == 2 ? NUM_ITEMS : NUM_PROPS))) {
		fail("model %d is not one of GoldenEye's %s", num,
			ischr == 1 ? "characters" : (ischr == 2 ? "hand items" : "props"));
	}

	p = ischr == 1 ? &g_Chrs[num] : (ischr == 2 ? &g_Items[num] : &g_Props[num]);
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

		// a row whose first word is a 0x05 segment pointer is a texture stored
		// in the file itself rather than a global image (the GoldenEye logo's
		// two): its bytes are copied over below and the row repointed, which is
		// what the port's model preprocessing expects of one (CT_TEXDATA)
		if ((image & 0xff000000u) != SEG_MODEL) {
			set32(rows.v, 12 * i, texRemap(image));
			setAdd(images, image);
		}
	}
	texat = bufPutAligned(&w, rows.v, rows.n, 4);

	moved = gcAlloc((p->numtextures + 1) * sizeof(*moved));

	for (int i = 0; i < p->numtextures; ++i) {
		const uint32_t image = be32(rows.v, 12 * i);
		size_t len, at;

		if ((image & 0xff000000u) != SEG_MODEL) {
			continue;
		}

		len = texDataSize(rows.v[12 * i + 4], rows.v[12 * i + 5], rows.v[12 * i + 6], rows.v[12 * i + 8]);

		if (image - SEG_MODEL + len > d.n) {
			fail("%s: a texture in the file runs off it", p->file);
		}

		at = bufPutAligned(&w, d.v + (image - SEG_MODEL), len, 8);
		set32(w.v, texat + 12 * i, SEG_MODEL + (uint32_t)at);
		moved[nmoved].at = image - SEG_MODEL;
		moved[nmoved].len = len;
		moved[nmoved].to = at;
		nmoved++;
	}

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
		// GoldenEye sets 0x100 on a group whose matrix the animation drives;
		// Perfect Dark reads `type & 0xff` and keeps the flags, so the whole
		// u16 is written back out
		const uint32_t flags = n->type & 0xff00;
		size_t rat;
		buf rec = {0};

		#define NEED(len) if ((size_t)ro + (len) > d.n) fail("%s: a node's data runs off the file", p->file)

		n->type &= 0xff;

		switch (n->type) {
		case 0x01:
			NEED(0x10);
			// The node an animation plays on: GoldenEye's animpart and
			// matrix, then the f32 Perfect Dark reads where GoldenEye keeps
			// its first group, and the rwdata index (which the game rewrites
			// at the load anyway, modelCalculateRwDataIndexes()).
			//
			// A character has one and so does an **aircraft** - the four
			// flying prop models carry one and nothing else in the ROM does -
			// and it stays a chrinfo node for both. It used to be demoted to a
			// position node on a prop, because modelUpdateChrNodeMtx() reads
			// model->anim with no guard and a standing aircraft has none; the
			// guard is in the port now (model.c), which is GoldenEye's own
			// answer.
			bufU16(&rec, be16(d.v, ro));
			bufU16(&rec, be16(d.v, ro + 2));
			bufF32(&rec, 0);
			bufU16(&rec, be16(d.v, ro + 0x0c));
			bufU16(&rec, 0);
			rat = bufPutAligned(&w, rec.v, rec.n, 4);
			break;
		case 0x02:
			NEED(0x14);
			bufPut(&rec, d.v + ro, 0x14);
			if (ischr == 1) {
				// GoldenEye numbers a character's animated parts from 1, with
				// the header node above them as 0, and its skeleton's joint j
				// holds the channel of part j; Perfect Dark numbers them from 0
				// and reads the channels in part order, so every part moves
				// down one and its skeleton is then g_SkelChrJoints joint for
				// joint
				const uint32_t part = be16(rec.v, 0x0c);

				if (part == 1) {
					// the hips, and the one node that must not keep its
					// channel: Perfect Dark's chrinfo *is* the hip node and
					// turns on part 0 itself, while GoldenEye's header node
					// applies no joint rotation at all and leaves the turn to
					// this group. Shifted to 0 the channel was applied at
					// both, so bond_eye_fire's ninety degrees came out as a
					// hundred and eighty and Bond finished the gun barrel
					// facing away from the camera. It keeps its place and its
					// matrix as a held position and loses only the rotation;
					// its origin is (0, 0, 0) in all 43 bodies, and it is the
					// header's own child in every one of them.
					const uint32_t mtx = be16(rec.v, 0x0e);

					rec.n = 0x0c;
					bufU16(&rec, mtx);
					rat = bufPutAligned(&w, rec.v, rec.n, 4);
					n->type = 0x15;
					break;
				}

				set16(rec.v, 0x0c, part ? part - 1 : 0);
			}
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
		case 0x17:
			// where a head goes: one rwdata index, the same
			NEED(2);
			rat = bufPutAligned(&w, d.v + ro, 2, 4);
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
				// the plain list record keeps its render mode in one byte
				// (s8 ModelType) where the record with collisions below
				// keeps a word: read as a word it is 0x0300 for a 3, which
				// no case of modelRenderNodeDl() answers to - no render
				// mode, and the second list never drawn (ge-bean.md)
				vtx = be32(d.v, ro + 12);
				mode = (int8_t)d.v[ro + 0x12];
			} else {
				vtx = be32(d.v, ro + 8);
				mode = bes16(d.v, ro + 0x18);
			}
			modelLists(&d, vtx, lists, 2, &w, 0, moved, nmoved, &vat, &nv, &cat, &nc, words, has);
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
			modelLists(&d, vtx, lists, 1, &w, 1, moved, nmoved, &vat, &nv, &cat, &nc, words, has);
			// GoldenEye's own count of quads, dorottex() copying that many:
			// with nought its list still ran, on vertices never written - a
			// yellow wedge across the screen, or a crash (converter 61). A
			// guard's gun is a prop, and its star sits under switch 2, the
			// toggle weaponSetGunfireVisible() shows only while the chr fires
			// - once the gun has GoldenEye's prop_weapon skeleton as
			// SKEL_CHRGUN (propSkel()). Before that nothing switched it off
			// and it hung at the muzzle of every rifle a guard held
			// (converter 62, which wrote nought for props and so drew no
			// flash at all; converter 64)
			bufU32(&rec, be32(d.v, ro));
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
			set16(w.v, at, n->type | flags);
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
				// a character's next can be the shadow that was left out, which
				// ends its chain there and has nothing to point back
				if (ischr != 1) {
					fail("%s: a node's next is not a node", p->file);
				}
				continue;
			}
			set32(w.v, a - SEG_MODEL + 0x10, SEG_MODEL + (uint32_t)nodesat + 0x18 * (uint32_t)i);
		}
	}

	for (int i = 0; i < p->numswitches; ++i) {
		set32(w.v, partsat + 4 * i, RELOC_NODE(switches[i]));
		set16(w.v, partsat + 4 * p->numswitches + 2 * i, (uint32_t)i);
	}

	set32(w.v, 0, SEG_MODEL + (uint32_t)nodesat);
	// A prop keeps GoldenEye's own skeleton where Perfect Dark has it
	// (propSkel(): the two Caverns doors, the windowed door and the
	// guards' guns); for
	// a character SKEL_CHR where it has GoldenEye's guard skeleton, which is
	// Perfect Dark's own joint for joint, and SKEL_HEAD where it has none (a
	// head is one list on one matrix)
	set32(w.v, 4, ischr == 1 ? (p->skeleton ? 0x09u : 0x0du) : (ischr == 2 ? 2u : propSkel(p->skeleton, p->numswitches)));
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

static buf modelConvert(int32_t num, uint8_t *images, double *scale)
{
	return modelConvertOne(num, images, scale, 0);
}

/* ------------------------------------------------------------------------ */
/* the intro's animations (geanim.py) */

struct animout {
	uint32_t numframes, bytesperframe, headerlen, framelen, looping;
};

struct bits {
	buf b;
	size_t nbits;
};

static void bitsPut(struct bits *w, uint32_t value, uint32_t n)
{
	for (int32_t i = (int32_t)n - 1; i >= 0; --i) {
		if (w->nbits % 8 == 0) {
			bufZeros(&w->b, 1);
		}
		if ((value >> i) & 1) {
			w->b.v[w->b.n - 1] |= 0x80 >> (w->nbits % 8);
		}
		++w->nbits;
	}
}

static void bitsCopy(struct bits *w, const uint8_t *src, size_t len, size_t bitoff, uint32_t n)
{
	for (uint32_t i = 0; i < n; ++i) {
		const size_t at = bitoff + i;

		if (at >> 3 >= len) {
			fail("an animation's bits run off its stream");
		}
		bitsPut(w, (src[at >> 3] >> (7 - (at & 7))) & 1, 1);
	}
}

/**
 * The animation whose record is at ROM address `at` as a Perfect Dark
 * animation: a header that says how to read a frame, then the frames, each
 * GoldenEye's root-motion bits for that frame followed by its frame's rotation
 * bits unchanged. geanim.py is this, and says why.
 */
static buf animConvert(size_t at, struct animout *out, uint32_t parts)
{
	uint32_t entry, numframes, width, loop, bitsperframe, rootbits = 0, framebytes, rotbits;
	uint32_t off[4], cnt[4], base[4];
	const uint8_t *root, *frames;
	size_t rootlen;
	buf w = {0};

	size_t descat, streamat;

	if (at + 0x14 > g_RomLen) {
		fail("an animation record runs off the ROM");
	}

	entry = be32(g_Rom, at);
	numframes = be16(g_Rom, at + 4);
	width = g_Rom[at + 6];
	loop = g_Rom[at + 7];
	bitsperframe = be16(g_Rom, at + 14);
	framebytes = bitsperframe / 8;
	rotbits = 3 * width * parts;

	// The record's third and fifth words are where its four root-motion
	// descriptors and their bit stream live: offsets into animation_data,
	// relocated into pointers when the segment loads. The blocks sit *between*
	// the records rather than after their own, so reading them at at+0x14 and
	// at+0x2c gives the next animation's - which gave bond_eye_walk the stride
	// of bond_eye_fire, and bond_eye_fire no root motion at all, so Bond
	// stopped walking and sank to the floor the moment he turned to fire.
	descat = ANIM_DATA_ROM + (be32(g_Rom, at + 8) & 0xffffff);
	streamat = ANIM_DATA_ROM + (be32(g_Rom, at + 16) & 0xffffff);

	if (descat + 24 > g_RomLen) {
		fail("an animation's descriptors run off the ROM");
	}

	for (int i = 0; i < 4; ++i) {
		off[i] = be16(g_Rom, descat + 6 * i);
		cnt[i] = g_Rom[descat + 6 * i + 2];
		base[i] = be16(g_Rom, descat + 6 * i + 4);
		rootbits += cnt[i];
	}

	if (!width || !numframes || rotbits > bitsperframe) {
		fail("the animation at %#x is not a character animation", (unsigned)at);
	}

	rootlen = ((size_t)rootbits * numframes + 7) / 8;
	root = g_Rom + streamat;
	frames = g_Rom + ANIM_ENTRY_ROM + entry;

	if (streamat + rootlen > g_RomLen
			|| (size_t)ANIM_ENTRY_ROM + entry + (size_t)numframes * framebytes > g_RomLen) {
		fail("an animation's data runs off the ROM");
	}

	// the header: a record a part, the first carrying the four root-motion
	// channels the renderer skips and the game reads
	for (uint32_t part = 0; part < parts; ++part) {
		if (part == 0) {
			bufU8(&w, 0x09);
			for (int i = 0; i < 4; ++i) {
				bufU16(&w, base[i]);
				bufU8(&w, cnt[i]);
			}
		} else {
			bufU8(&w, 0x01);
		}
		for (int i = 0; i < 3; ++i) {
			bufU16(&w, 0);
			bufU8(&w, width);
		}
	}

	out->headerlen = (uint32_t)w.n;

	for (uint32_t f = 0; f < numframes; ++f) {
		struct bits b = {{0}, 0};

		for (int i = 0; i < 4; ++i) {
			bitsCopy(&b, root, rootlen, (size_t)rootbits * f + off[i], cnt[i]);
		}
		bitsCopy(&b, frames + (size_t)f * framebytes, framebytes, 0, rotbits);
		while (b.nbits % 8) {
			bitsPut(&b, 0, 1);
		}
		bufPut(&w, b.b.v, b.b.n);
	}

	out->numframes = numframes;
	out->bytesperframe = (rootbits + rotbits + 7) / 8;
	out->framelen = width;
	out->looping = loop & 1;

	if (w.n != out->headerlen + (size_t)out->numframes * out->bytesperframe) {
		fail("an animation came out the wrong size");
	}

	return w;
}

static buf chrConvert(int32_t num, uint8_t *images, double *scale)
{
	return modelConvertOne(num, images, scale, 1);
}

static buf itemConvert(int32_t num, uint8_t *images, double *scale)
{
	return modelConvertOne(num, images, scale, 2);
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
/**
 * GoldenEye's own render scale for a level (bg.c's levelinfotable, the
 * `visibility` column): Dam and the two Surfaces are drawn at a fifth of their
 * size and every other level at its own.
 */
static double levelVisibility(const char *key)
{
	return !strcmp(key, "dam") || !strcmp(key, "sevx") || !strcmp(key, "sevxb") ? 0.2 : 1.0;
}

static void fogValue(struct textbuf *t, double *r, const double *offset, double vis)
{
	r[13] -= offset[1];
	r[23] -= offset[1];

	// The row's distances are in GoldenEye's *drawn* space: bgfog.c sets the z
	// range from near and far as they are and divides it by the level's render
	// scale for every question asked in the world's units. The converted level
	// is in the world's units and is drawn at its own size, so the near and far
	// planes and the three distances objects fade over are divided here. Left
	// as they were Dam's far plane stood at 15000 where GoldenEye's is at 75000
	// - the cliffs and the mountains round the dam were never drawn - and its
	// fog, which is a fraction of that range, began a fifth as far away and
	// washed the level blue. geconvert.py's fog_value().
	for (int k = 0; k < 5; ++k) {
		r[k] /= vis;
	}

	// the sky and water image ids name GoldenEye's own pictures
	r[14] += GE_SKYTEX_FIRST;
	r[24] += GE_SKYTEX_FIRST;

	#define I(k) (long)rnd(r[(k) - 1])
	textf(t, "%ld %ld %ld %ld %ld %ld %ld %02lx%02lx%02lx %ld %ld %ld %02lx%02lx%02lx %ld %ld %ld %02lx%02lx%02lx %ld",
		I(1), I(2), I(3), I(4), I(5), I(8), I(9), I(10), I(11), I(12),
		I(13), I(14), I(15), I(17), I(18), I(19), I(20), I(24), I(25), I(27), I(28), I(29), I(30));
	#undef I
}

// a fogless row as the maps block's `fog` string: "nofog", the near and far
// planes GoldenEye draws every fogless level with (bgfog.c sets the z range to
// 15..10000 where no fog row is found, divided by the render scale as above),
// then the sky, clouds and water as fogValue() writes them. GoldenEye's three
// sky/water images are Perfect Dark's three, in the same order, so the two
// image ids carry over as they are.
static void foglessValue(struct textbuf *t, double *r, const double *offset, double vis)
{
	r[4] -= offset[1];
	r[11] -= offset[1];
	r[5] += GE_SKYTEX_FIRST;
	r[12] += GE_SKYTEX_FIRST;

	#define I(k) (long)rnd(r[k])
	textf(t, "nofog %ld %ld %02lx%02lx%02lx %ld %ld %ld %02lx%02lx%02lx %ld %ld %ld %02lx%02lx%02lx %ld",
		(long)rnd(15 / vis), (long)rnd(10000 / vis),
		I(0), I(1), I(2), I(3), I(4), I(5), I(7), I(8), I(9), I(10), I(11), I(12), I(14), I(15), I(16), I(17));
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
	return (int)NUM_LEVELS + 3;
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
	static uint8_t allanims[GEANIM_NUM_ANIMS];
	struct textbuf maps = {0}, missions = {0}, modellines = {0}, config = {0};
	volatile int ok = 0;
	char sub[1024];

	g_Progress = 0;
	memset(alltex, 0, sizeof(alltex));
	memset(allmodels, 0, sizeof(allmodels));
	memset(allanims, 0, sizeof(allanims));

	// Every one of GoldenEye's guard animations, and not only those the
	// missions' lists, the watch and the openings name: a GE Plus character
	// walks, fires, flinches and dies in GoldenEye's own (gechranims.c). The
	// rows the decompilation names nullNN are one byte into the segment and
	// hold nothing.
	for (int i = 0; i < GEANIM_NUM_ANIMS; ++i) {
		allanims[i] = g_GeAnims[i].at > 1;
	}
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
		int music[3];
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
				roomTileBounds(&stan, bg.numrooms, lv->levelscale, offset), &stan, lv->levelid);
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
		mpsetup = writeMpSetup(&setup, havemp ? &mpsetupsrc : NULL, &stan, &bg, lv->levelscale, &gedata, &bikepads, allmodels);

		snprintf(rel, sizeof(rel), "files/bgdata/bg_gx%s.seg", lv->key);
		writeFile(outdir, rel, bgdata.v, bgdata.n);
		snprintf(rel, sizeof(rel), "files/bgdata/bg_gx%s_tilesZ", lv->key);
		writeFile(outdir, rel, tilesdata.v, tilesdata.n);
		{
			buf standata = writeStan(&stan, lv->levelscale, offset);

			snprintf(rel, sizeof(rel), "files/bgdata/bg_gx%s_stan", lv->key);
			writeFile(outdir, rel, standata.v, standata.n);
		}
		snprintf(rel, sizeof(rel), "files/bgdata/bg_gx%s_padsZ", lv->key);
		writeFile(outdir, rel, padsdata.v, padsdata.n);
		snprintf(rel, sizeof(rel), "files/Ump_setupgx%sZ", lv->key);
		writeFile(outdir, rel, mpsetup.v, mpsetup.n);

		textf(&maps, "%s  map \"%s\" bg \"bgdata/bg_gx%s.seg\" tiles \"bgdata/bg_gx%s_tilesZ\" pads \"bgdata/bg_gx%s_padsZ\" mpsetup \"Ump_setupgx%sZ\"",
			maps.n ? "\n" : "", lv->name, lv->key, lv->key, lv->key, lv->key);
		if (romFogRow(lv->levelid, fog)) {
			textf(&maps, " fog \"");
			fogValue(&maps, fog, offset, levelVisibility(lv->key));
			textf(&maps, "\"");
		} else if (romFoglessRow(lv->levelid, fog)) {
			textf(&maps, " fog \"");
			foglessValue(&maps, fog, offset, levelVisibility(lv->key));
			textf(&maps, "\"");
		}
		romMusicRow(lv->levelid, music);
		textf(&maps, " music \"%d %d %d\"", music[0], music[1], music[2]);

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

			st.anims = allanims;
			st.models = allmodels;

			if (strcmp(g_Missions[mi].key, lv->key) != 0) {
				continue;
			}

			mfile = romFile(g_Missions[mi].setup);
			setupRead(&mfile, &msetup);
			mbound = boundPads(&mfile, lv->levelscale, offset);
			mpads = writePads(&msetup, lv->levelscale, offset, &rf, &mbound);
			mprops = writeSoloSetup(&mfile, msetup.pads.n, allmodels, &st, lv->levelscale, offset);

			snprintf(rel, sizeof(rel), "files/bgdata/bg_gs%s_padsZ", lv->key);
			writeFile(outdir, rel, mpads.v, mpads.n);
			snprintf(rel, sizeof(rel), "files/Usetupgs%sZ", lv->key);
			writeFile(outdir, rel, mprops.v, mprops.n);

			textf(&missions, "%s  mission %d \"%s\" bg \"bgdata/bg_gx%s.seg\" tiles \"bgdata/bg_gx%s_tilesZ\""
					" pads \"bgdata/bg_gs%s_padsZ\" setup \"Usetupgs%sZ\"",
				missions.n ? "\n" : "", (int)mi, g_Missions[mi].name, lv->key, lv->key, lv->key, lv->key);
			if (romFogRow(lv->levelid, fog)) {
				textf(&missions, " fog \"");
				fogValue(&missions, fog, offset, levelVisibility(lv->key));
				textf(&missions, "\"");
			} else if (romFoglessRow(lv->levelid, fog)) {
				textf(&missions, " fog \"");
				foglessValue(&missions, fog, offset, levelVisibility(lv->key));
				textf(&missions, "\"");
			}
			textf(&missions, " music \"%d %d %d\"", music[0], music[1], music[2]);

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

	// GE Plus's menus are GoldenEye's own folder screens: the folder is a prop
	// model, the cursor and the stage pictures global images, and the fonts, the
	// music and the title screen's strings are copied as GoldenEye stores them
	{
		const size_t keep = g_NumAllocs;
		buf title;

		setAdd(allmodels, MENU_FOLDER_MODEL);
		// and the TV set the folder's Monitor Programmes page shows them on
		// (PROP_TV1, gexfront.c)
		setAdd(allmodels, MENU_TV_MODEL);
		setAdd(allmodels, INTRO_LOGO_MODEL);
		// the thrown gadgets' props (gesolo.py's GE_GADGET_MODELS): PROP_CHRBUG,
		// PROP_CHRGOLDENEYEKEY and PROP_CHRPLASTIQUE
		setAdd(allmodels, 245);
		setAdd(allmodels, 248);
		setAdd(allmodels, 273);
		for (size_t i = 0; i < sizeof(g_IntroGuns) / sizeof(g_IntroGuns[0]); ++i) {
			setAdd(allmodels, g_IntroGuns[i]);
		}
		// and every gun Bond can hold, in his hand in third person and through a
		// mission's opening swirl (gegunsOwnPropModel()) - the three no setup
		// puts in a guard's hand as well: PROP_CHRKNIFE, PROP_CHRSHOTGUN and
		// PROP_CHRMP5KSIL, the silenced D5K Frigate starts him with
		for (size_t i = 0; i < sizeof(g_HeldGuns) / sizeof(g_HeldGuns[0]); ++i) {
			setAdd(allmodels, g_HeldGuns[i]);
		}
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

		// and the banks the watch reads (gewatch.c): its own screens' strings,
		// the multiplayer watch's, the guns' names and the pickups'
		for (size_t i = 0; i < sizeof(g_WatchLang) / sizeof(g_WatchLang[0]); ++i) {
			char rel[64];
			buf f = romFile(g_WatchLang[i]);

			snprintf(rel, sizeof(rel), "menu/%s", g_WatchLang[i]);
			writeFile(outdir, rel, f.v, f.n);
		}

		writeWeaponSets(outdir);

		// random_tracks, for the levels with no music of their own (gemusic.c)
		{
			const size_t at = romMusicRandomAt();
			size_t n = 0;

			while (at + 2 * n + 2 <= g_DataLen && n < 256 && be16(g_Data, at + 2 * n) != 0) {
				++n;
			}
			writeFile(outdir, "menu/musicrandom.bin", g_Data + at, 2 * (n + 1));

			// and how loud each sequence plays
			n = 0;
			while (MUSIC_VOLUMES_AT + 2 * n + 2 <= g_DataLen && n < 256 && bes16(g_Data, MUSIC_VOLUMES_AT + 2 * n) >= 0) {
				++n;
			}
			if (n < 24 || be16(g_Data, MUSIC_VOLUMES_AT) != 0x6665) {
				fail("the music's volumes are not where they were");
			}
			writeFile(outdir, "menu/musicvolumes.bin", g_Data + MUSIC_VOLUMES_AT, 2 * n);
		}

		if (INTRO_BLOOD_AT + INTRO_BLOOD_SIZE > g_DataLen) {
			fail("the blood runs off the data segment");
		}
		writeFile(outdir, "menu/introblood.bin", g_Data + INTRO_BLOOD_AT, INTRO_BLOOD_SIZE);

		// gitem_structs as it stands, for where the watch holds each item up on
		// its face (gewatch.c): a row's two pointers mean nothing out of the
		// ROM, its positions and turns are read as they are
		if (ITEMS_AT + ITEM_ROW * NUM_ITEMS > g_DataLen) {
			fail("the hand items run off the data segment");
		}
		writeFile(outdir, "menu/geitems.bin", g_Data + ITEMS_AT, ITEM_ROW * NUM_ITEMS);

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

	// the intro's characters: every one in the ROM, converted (gechr.py), and
	// its animations in one file with a table naming each (geanim.py)
	{
		const size_t numanims = sizeof(g_IntroAnims) / sizeof(g_IntroAnims[0]);
		const size_t keepall = g_NumAllocs;
		buf index = {0}, blob = {0}, head = {0};
		size_t base;

		for (uint32_t num = 0; num < NUM_CHRS; ++num) {
			const size_t keep = g_NumAllocs;
			double scale;
			buf data, z;
			char rel[64];

			data = chrConvert((int32_t)num, alltex, &scale);
			z = rzip1173(data.v, data.n);
			snprintf(rel, sizeof(rel), "files/Cgx%03uZ", num);
			writeFile(outdir, rel, z.v, z.n);

			for (size_t i = keep; i < g_NumAllocs; ++i) {
				free(g_Allocs[i]);
			}
			g_NumAllocs = keep;
		}

		// menu/intro.bin: "GEI2", the characters' scales, then a row an
		// animation (its 32-byte name, Perfect Dark's animtableentry fields,
		// and where its bytes are). The name is what the port matches the row
		// by, and a 20-byte field truncated ten of the twenty-five - two of
		// them to the same 19 characters - so it has to be wide enough for the
		// longest of them
		bufPut(&head, (const uint8_t *)"GEI2", 4);
		bufU16(&head, NUM_CHRS);
		bufU16(&head, (uint32_t)numanims);
		for (uint32_t num = 0; num < NUM_CHRS; ++num) {
			bufU16(&head, num);
			bufU16(&head, g_Chrs[num].flags);
			bufF32(&head, g_Chrs[num].scale);
		}

		base = head.n + 48 * numanims;

		for (size_t i = 0; i < numanims; ++i) {
			struct animout e;
			buf data = animConvert(g_IntroAnims[i].at, &e, ANIM_PARTS);
			uint8_t name[32] = {0};

			snprintf((char *)name, sizeof(name), "%s", g_IntroAnims[i].name);
			bufPut(&index, name, sizeof(name));
			bufU16(&index, e.numframes);
			bufU16(&index, e.bytesperframe);
			bufU16(&index, e.headerlen);
			bufU8(&index, e.framelen);
			bufU8(&index, e.looping);
			bufU32(&index, (uint32_t)(base + blob.n));
			bufU32(&index, (uint32_t)data.n);
			bufPut(&blob, data.v, data.n);
		}

		bufPut(&head, index.v, index.n);
		bufPut(&head, blob.v, blob.n);
		writeFile(outdir, "menu/intro.bin", head.v, head.n);
		note("geconvert: %d characters, %d animations in %d bytes",
			(int)NUM_CHRS, (int)numanims, (int)blob.n);

		// menu/gechrs.bin: "GEC1" and a row a character - the two
		// c_item_entries flags, its scale and its pov, which are what
		// makeonebody() gives a chr's model (modelSetScale(scale * 0.1) and
		// modelSetAnimTranslationScale(pov), Perfect Dark's own two fields).
		// A mission reads this to dress its guards in GoldenEye's own
		// characters (gexplus.c); intro.bin carries the same scales, but a
		// third of a megabyte of animation with them.
		{
			buf chrs = {0};

			bufPut(&chrs, (const uint8_t *)"GEC1", 4);
			bufU16(&chrs, NUM_CHRS);
			bufU16(&chrs, 0);

			for (uint32_t num = 0; num < NUM_CHRS; ++num) {
				bufU16(&chrs, num);
				bufU16(&chrs, g_Chrs[num].flags);
				bufF32(&chrs, g_Chrs[num].scale);
				bufF32(&chrs, g_Chrs[num].pov);
			}

			writeFile(outdir, "menu/gechrs.bin", chrs.v, chrs.n);
		}

		// menu/gemonitors.bin: what GoldenEye's TVs and its big projection
		// screens show (gemonitortable.h, port/src/gemonitor.c). "GEM1", the
		// three counts, a word offset a programme, a texture config an image
		// - with the image's number as the conversion writes its texture - and
		// the programmes' block out of the data segment as it stands, but for a
		// jump's target, an address inside the block, which becomes the word it
		// is at. geconvert.py writes the same bytes.
		{
			static const uint8_t lens[16] = { 1, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3, 1, 1, 3, 2, 2 };
			const size_t at = GEMON_BLOCK_AT - DATA_VRAM;
			buf mon = {0};

			if (at + 4 * (size_t)GEMON_BLOCK_WORDS > g_DataLen) {
				fail("the monitor programmes are not where they should be");
			}

			bufPut(&mon, (const uint8_t *)"GEM1", 4);
			bufU16(&mon, GEMON_NUM_PROGRAMS);
			bufU16(&mon, GEMON_NUM_IMAGES);
			bufU32(&mon, GEMON_BLOCK_WORDS);

			for (int i = 0; i < GEMON_NUM_PROGRAMS; ++i) {
				bufU32(&mon, g_GeMonPrograms[i]);
			}

			for (int i = 0; i < GEMON_NUM_IMAGES; ++i) {
				setAdd(alltex, g_GeMonImages[i].image);
				bufU32(&mon, texRemap(g_GeMonImages[i].image));
				bufU8(&mon, g_GeMonImages[i].w);
				bufU8(&mon, g_GeMonImages[i].h);
				bufU8(&mon, g_GeMonImages[i].level);
				bufU8(&mon, g_GeMonImages[i].format);
				bufU8(&mon, g_GeMonImages[i].depth);
				bufU8(&mon, g_GeMonImages[i].s);
				bufU8(&mon, g_GeMonImages[i].t);
				bufU8(&mon, 0);
			}

			for (size_t w = 0; w < GEMON_BLOCK_WORDS; ) {
				const uint32_t op = be32(g_Data, at + 4 * w);
				const size_t n = op < 16 ? lens[op] : 1;

				for (size_t k = 0; k < n && w + k < GEMON_BLOCK_WORDS; ++k) {
					uint32_t v = be32(g_Data, at + 4 * (w + k));

					if (k == 1 && (op == 9 || op == 10)) {
						v = (v - GEMON_BLOCK_AT) / 4;
					}

					bufU32(&mon, v);
				}

				w += n;
			}

			writeFile(outdir, "menu/gemonitors.bin", mon.v, mon.n);
		}

		// the watch the remake's pause wears (gewatch.c): GoldenEye's own
		// hand item, converted the way a prop is and written under its item
		// number, since the player's own character wears it now and their
		// body model has no watch of its own
		{
			const size_t keep = g_NumAllocs;
			double scale;
			buf data = itemConvert(ITEM_WATCH, alltex, &scale);
			buf z = rzip1173(data.v, data.n);
			char rel[64];

			snprintf(rel, sizeof(rel), "files/Igx%03uZ", (unsigned)ITEM_WATCH);
			writeFile(outdir, rel, z.v, z.n);
			note("geconvert: the watch (%s) as %s", ITEM_WATCH_FILE, rel);

			for (size_t i = keep; i < g_NumAllocs; ++i) {
				free(g_Allocs[i]);
			}
			g_NumAllocs = keep;
		}

		// and the guns a hand holds (geguns.c): GoldenEye's own first person
		// models, each with the hand that holds it, converted the same way
		// and written under its item number - ITEM_KNIFE (2) to
		// ITEM_REMOTEMINE (29), whichever of them the table gives a model
		{
			int written = 0;

			// ...and the gadgets a mission puts in the hand (gegadgets.c),
			// which are hand items like any gun: ITEM_PLASTIQUE (34) to
			// ITEM_DATTAPE (73), whichever of them the table gives a model
			for (int32_t item = 2; item <= 73; ++item) {
				const size_t keep = g_NumAllocs;
				double scale;
				buf data, z;
				char rel[64];

				// the silver and gold PP7s and the watch laser are no gun of
				// the port's (g_GeItemWeapon gives them another's), and the
				// watch laser's model is a node type nothing here reads
				if (!g_Items[item].file || item == 20 || item == 21 || item == 23
						|| (item > 29 && !soloGadgetItem(item))) {
					continue;
				}

				note("geconvert: item %d is %s", (int)item, g_Items[item].file);

				data = itemConvert(item, alltex, &scale);
				z = rzip1173(data.v, data.n);
				snprintf(rel, sizeof(rel), "files/Igx%03uZ", (unsigned)item);
				writeFile(outdir, rel, z.v, z.n);
				++written;

				for (size_t i = keep; i < g_NumAllocs; ++i) {
					free(g_Allocs[i]);
				}
				g_NumAllocs = keep;
			}

			note("geconvert: %d of GoldenEye's own first person guns", written);
		}

		// and the controller the watch shows on its control page, which is a
		// hand item too (0x55, what watchRenderController() puts in the hand)
		if (g_Items[ITEM_CONTROLLER].file) {
			const size_t keep = g_NumAllocs;
			double scale;
			buf data = itemConvert(ITEM_CONTROLLER, alltex, &scale);
			buf z = rzip1173(data.v, data.n);
			char rel[64];

			snprintf(rel, sizeof(rel), "files/Igx%03uZ", (unsigned)ITEM_CONTROLLER);
			writeFile(outdir, rel, z.v, z.n);
			note("geconvert: the controller (%s) as %s", g_Items[ITEM_CONTROLLER].file, rel);

			for (size_t i = keep; i < g_NumAllocs; ++i) {
				free(g_Allocs[i]);
			}
			g_NumAllocs = keep;
		}

		// menu/geanims.bin: the animations the missions' PlayAnimation commands
		// name (geanimtable.h), each under GoldenEye's own id. The port appends
		// them to Perfect Dark's table and gives the id its number there
		// (gexplusanim.c), which is what the converted aiChrDoAnimation asks for
		{
			buf aindex = {0}, ablob = {0}, ahead = {0};
			int numrows = 0;
			size_t abase;

			for (int i = 0; i < GEANIM_NUM_ANIMS; ++i) {
				numrows += allanims[i] ? 1 : 0;
			}

			// the three vehicle animations below take a row each too
			abase = 8 + 20 * (size_t)(numrows + GEVEH_NUM_ANIMS);

			for (int i = 0; i < GEANIM_NUM_ANIMS; ++i) {
				struct animout e;
				buf data;

				if (!allanims[i]) {
					continue;
				}

				data = animConvert(GEANIM_BASE + g_GeAnims[i].at, &e, ANIM_PARTS);
				bufU16(&aindex, (uint32_t)i);
				bufU16(&aindex, e.numframes);
				bufU16(&aindex, e.bytesperframe);
				bufU16(&aindex, e.headerlen);
				bufU8(&aindex, e.framelen);
				bufU8(&aindex, e.looping);
				bufZeros(&aindex, 2);
				bufU32(&aindex, (uint32_t)(abase + ablob.n));
				bufU32(&aindex, (uint32_t)data.n);
				bufPut(&ablob, data.v, data.n);
			}

			// and GoldenEye's three vehicle animations, which its aircraft
			// play (animation_table_ptrs2[]). They share their numbering with
			// the guards' table and only the AI list's owner tells the two
			// apart, so here they take an id space of their own at
			// GEVEH_ANIM_FIRST. All three go in whether or not a mission names
			// one: they are one part and four root channels apiece.
			for (int i = 0; i < GEVEH_NUM_ANIMS; ++i) {
				struct animout e;
				buf data = animConvert(GEANIM_BASE + g_GeVehicleAnims[i].at, &e, 1);

				bufU16(&aindex, (uint32_t)(GEVEH_ANIM_FIRST + i));
				bufU16(&aindex, e.numframes);
				bufU16(&aindex, e.bytesperframe);
				bufU16(&aindex, e.headerlen);
				bufU8(&aindex, e.framelen);
				bufU8(&aindex, e.looping);
				bufZeros(&aindex, 2);
				bufU32(&aindex, (uint32_t)(abase + ablob.n));
				bufU32(&aindex, (uint32_t)data.n);
				bufPut(&ablob, data.v, data.n);
				++numrows;
			}

			bufPut(&ahead, (const uint8_t *)"GEA1", 4);
			bufU16(&ahead, (uint32_t)numrows);
			bufU16(&ahead, 0);
			bufPut(&ahead, aindex.v, aindex.n);
			bufPut(&ahead, ablob.v, ablob.n);
			writeFile(outdir, "menu/geanims.bin", ahead.v, ahead.n);
			note("geconvert: %d mission animations in %d bytes", numrows, (int)ablob.n);
		}

		for (size_t i = keepall; i < g_NumAllocs; ++i) {
			free(g_Allocs[i]);
		}
		g_NumAllocs = keepall;
		++g_Progress;
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

		setAdd(alltex, GE_IMAGE_CLOUDS);
		setAdd(alltex, GE_IMAGE_WATER_GREY);
		setAdd(alltex, GE_IMAGE_WATER_BLUE);

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
