/**
 * GoldenEye's own intro, played when the player enters GE Plus, drawn the way
 * GoldenEye's front end draws it (the decomp's src/game/title.c and front.c).
 *
 * GoldenEye's chain is Nintendo logo, Rare logo, gun barrel, GoldenEye logo,
 * cast reel, file select. The two boot logos are not here - the game the player
 * is in has already booted, and the player asked for GE Plus, not for a boot -
 * so it starts at the gun barrel and ends by opening the folder screens
 * (gexfront.c).
 *
 * - **the gun barrel** (`renderGunbarrelEyeIntroSequence()`, its seven modes
 *   kept as they are): the lens slides across black, then the sniper sight -
 *   GoldenEye's own 440x299 backdrop scrolling under it - Bond walks in from
 *   the right, turns and fires, the lens fills with blood and the screen fades.
 *   Bond is GoldenEye's `CdjbondZ` on its `CheadbrosnanZ` with the PP7
 *   (`PROP_CHRWPPK`, model 191) in his right hand, playing GoldenEye's own
 *   `bond_eye_walk` and `bond_eye_fire`, drawn shaded and untextured as
 *   GoldenEye draws him;
 * - **the GoldenEye logo** (`constructor_menu04_goldeneyelogo()`): model 277,
 *   which carries its own gold texture inside the file, turned under a
 *   reflected LookAt for three seconds;
 * - **the cast reel** (`constructor_menu18_displaycast()`): a character every
 *   three seconds under a camera on a random arc, with their part's name in
 *   GoldenEye's Zurich Bold, fading in and out. The characters, their
 *   animations and the guns they hold are `intro_char_table` and
 *   `intro_animation_table`, GoldenEye's own.
 *
 * Any button skips a stage, as GoldenEye's does; the music is GoldenEye's
 * M_INTRO (sequence 2) on its own instrument bank.
 *
 * Everything is converted out of the player's ROM (geconvert.c): the characters
 * are `files/Cgx%03dZ` and their scales, animations, backdrop and blood are
 * `menu/intro.bin`, `menu/introbg.bin` and `menu/introblood.bin`. The fonts and
 * the strings come from the folder screens, which load them for both
 * (`gexFrontLoadShared()`).
 *
 * GoldenEye lays its screens out on 440x330 and so does this; the gun barrel
 * has an ortho of its own, GoldenEye's 1280x960, widened at the sides on a
 * wide window so the lens stays round. Its backdrop and its blood are drawn
 * through the same box (`introFrameBox()`), which is what keeps the mouth of
 * the barrel round the lens.
 */
#include <stdlib.h>
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
#include "input.h"
#include "mod.h"
#include "modloader.h"
#include "romdata.h"
#include "system.h"
#include "video.h"
#include "geintro.h"
#include "gexfront.h"
#include "preprocess.h"
#include "game/file.h"
#include "game/gfxmemory.h"
#include "game/menu.h"
#include "game/modeldef.h"
#include "game/modelmgr.h"
#include "game/music.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/setup.h"
#include "lib/anim.h"
#include "lib/vi.h"
#include "lib/joy.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "lib/rng.h"
#include "lib/snd.h"

#define GEINTRO_W 440.0f
#define GEINTRO_H 330.0f

// GoldenEye's gun barrel ortho, and the lens's own units in it
#define BARREL_W 1280.0f
#define BARREL_H 960.0f
#define BARREL_SEGMENTS 30
#define BARREL_VERTICES 30

// the backdrop the sniper sight scrolls: the folder screens' own picture
#define BG_W 440
#define BG_H 299

// GoldenEye's M_INTRO
#define INTRO_SEQUENCE 2

// the blood wash down the lens, 4-bit intensity
#define BLOOD_W 80
#define BLOOD_H 96

/* ------------------------------------------------------------------------ */
/* GoldenEye's own tables */

// the animations menu/intro.bin carries, in the order geconvert writes them
enum {
	GEANIM_BOND_EYE_WALK,
	GEANIM_BOND_EYE_FIRE,
	GEANIM_IDLE,
	GEANIM_CAST_FIRST,
};

static const char *const g_AnimNames[] = {
	"bond_eye_walk", "bond_eye_fire", "idle",
	// intro_animation_table, in its own order
	"spotting_bond", "fire_standing_draw_fast", "fire_standing_draw_slow",
	"fire_step_right", "fire_kneel_forward_fast", "running_one_handed",
	"draw_and_stand_up", "aim_left_right", "cock_and_turn_around",
	"cock_turn_stand_up", "draw_and_turn_around", "drop_weapon_fight",
	"laughing", "fire_hip_forward", "fire_standing_left_fast",
	"fire_kneel_left_fast", "draw_and_look_around", "aim_left", "aim_right",
	"conversation", "conversation_listener", "conversation_cleaned",
};

#define NUM_ANIMS ((s32)(sizeof(g_AnimNames) / sizeof(g_AnimNames[0])))
#define NUM_CAST_ANIMS (NUM_ANIMS - GEANIM_CAST_FIRST)

// what a cast animation holds: nothing, a pistol or a rifle (front.c's
// INTRO_WEAPON_TYPE_*), the frame it starts on and how fast it plays
static const struct { s16 weapon; f32 startframe, speed; } g_CastAnims[] = {
	{ 0, 98.0f, 1.0f },   { 1, 21.0f, 1.0f },   { 1, 26.0f, 1.0f },
	{ 1, 0.0f, 1.0f },    { 1, 0.0f, 1.0f },    { 1, 0.0f, 0.91000003f },
	{ 1, 31.0f, 1.0f },   { 1, 0.0f, 1.0f },    { 1, 0.0f, 1.0f },
	{ 1, 0.0f, 1.0f },    { 1, 0.0f, 1.0f },    { 0, 248.0f, 1.0f },
	{ 0, 150.0f, 1.0f },  { 1, 0.0f, 0.89999998f }, { 1, 0.0f, 0.89999998f },
	{ 1, 0.0f, 0.89999998f }, { 1, 51.0f, 1.0f }, { 1, 0.0f, 0.89999998f },
	{ 1, 0.0f, 0.89999998f }, { 2, 37.0f, 1.0f }, { 2, 300.0f, 1.0f },
	{ 2, 120.0f, 1.0f },
};

// GoldenEye's character numbers (c_item_entries rows, which is what the
// conversion writes files/Cgx%03dZ against)
#define BODY_BROSNAN_TUXEDO   5
#define BODY_SPECIAL_OPS      22
#define BODY_FORMAL_WEAR      23
#define BODY_JUNGLE_FATIGUES  24
#define BODY_PARKA            25
#define BODY_NATALYA_SKIRT    16
#define BODY_TREVELYAN_006     9
#define BODY_TREVELYAN_JANUS   8
#define HEAD_BROSNAN_DEFAULT  75
#define HEAD_BROSNAN_BOILER   74
#define HEAD_BROSNAN_TUXEDO   78
#define HEAD_MISHKIN          69
// the pool a HEAD_RANDOM character draws from: GoldenEye's male heads, then its
// female ones
#define HEAD_MALE_FIRST       42
#define HEAD_FEMALE_FIRST     70
#define HEAD_FEMALE_END       74

#define HEAD_OWN  (-1)
#define HEAD_ANY  (-2)

// the guns a cast animation puts in the hand (front.c's random_rifles_in_intro
// and random_pistols_in_intro, GoldenEye's prop model numbers)
static const s16 g_CastRifles[] = { 184, 188, 197, 207, 185, 210 };
static const s16 g_CastPistols[] = { 191, 204, 193, 195, 195, 205, 205, 190, 187, 208 };

/**
 * front.c's intro_char_table: the character, the head (its own, or any of the
 * pool), the three lines of its caption as indices into LtitleE, and whether it
 * is one of the extras the reel leaves out - GoldenEye shows those only in the
 * long version it plays after the credits, and so does this.
 */
static const struct castrow {
	s16 body, head, text1, text2, text3;
	u8 extra;
} g_Cast[] = {
	{ BODY_BROSNAN_TUXEDO, HEAD_BROSNAN_TUXEDO, 227, 228, 227, 1 },
	{ BODY_SPECIAL_OPS,    HEAD_BROSNAN_BOILER, 229, 232, 233, 0 },
	{ BODY_NATALYA_SKIRT,  HEAD_OWN,            229, 234, 227, 0 },
	{ BODY_TREVELYAN_006,  HEAD_OWN,            229, 235, 236, 0 },
	{ 11,                  HEAD_OWN,            230, 237, 238, 0 },
	{ 7,                   HEAD_OWN,            230, 239, 240, 0 },
	{ 6,                   HEAD_OWN,            230, 241, 227, 0 },
	{ 10,                  HEAD_OWN,            230, 242, 243, 0 },
	{ 19,                  HEAD_MISHKIN,        230, 244, 245, 0 },
	{ 2,                   HEAD_ANY,            227, 253, 227, 1 },
	{ 3,                   HEAD_ANY,            227, 252, 227, 1 },
	{ 35,                  HEAD_ANY,            227, 263, 227, 1 },
	{ 28,                  HEAD_ANY,            227, 263, 227, 1 },
	{ 18,                  HEAD_ANY,            227, 256, 227, 1 },
	{ 17,                  HEAD_ANY,            227, 254, 227, 1 },
	{ 20,                  HEAD_ANY,            227, 257, 227, 1 },
	{ 36,                  HEAD_OWN,            227, 262, 227, 1 },
	{ 1,                   HEAD_ANY,            227, 251, 227, 1 },
	{ 29,                  HEAD_ANY,            227, 264, 227, 1 },
	{ 33,                  HEAD_ANY,            227, 264, 227, 1 },
	{ 34,                  HEAD_ANY,            227, 264, 227, 1 },
	{ 32,                  HEAD_ANY,            227, 264, 227, 1 },
	{ 19,                  HEAD_ANY,            227, 258, 227, 1 },
	{ 38,                  HEAD_ANY,            227, 259, 227, 1 },
	{ 37,                  HEAD_ANY,            227, 258, 227, 1 },
	{ 21,                  HEAD_OWN,            227, 260, 227, 1 },
	{ 0,                   HEAD_ANY,            227, 250, 227, 1 },
	{ 4,                   HEAD_ANY,            227, 255, 227, 1 },
	{ 39,                  HEAD_ANY,            227, 261, 227, 1 },
	{ 40,                  HEAD_ANY,            227, 261, 227, 1 },
	{ 14,                  HEAD_OWN,            231, 246, 227, 0 },
	{ 13,                  HEAD_OWN,            231, 247, 227, 0 },
	{ 15,                  HEAD_OWN,            231, 248, 227, 0 },
	{ 12,                  HEAD_OWN,            231, 249, 227, 0 },
};

#define NUM_CAST ((s32)(sizeof(g_Cast) / sizeof(g_Cast[0])))

// front.c's CAST_INTRO_LEN and CAST_INTRO_FADE, at the game's 60Hz
#define CAST_LEN     (60 * 3)
#define CAST_FADE    (60 / 2)
#define CAST_FADEOUT (CAST_LEN - CAST_FADE + 1)

/* ------------------------------------------------------------------------ */
/* state */

enum {
	STAGE_BARREL,
	STAGE_LOGO,
	STAGE_CAST,
	STAGE_DONE,
};

struct introanim {
	struct animtableentry entry;
	s32 animnum;
	u8 looping;
};

struct intromodel {
	u8 *buf;
	u32 buflen;
	struct modeldef *modeldef;
	struct model *model;
};

static struct {
	s32 active;
	s32 loaded;
	s32 moddir;
	s32 stage;
	s32 inputdelay;

	// the conversion's files
	u8 *bg;              // BG_W * BG_H, expanded
	u8 *blood;           // the encoded stream
	u32 bloodlen;
	f32 chrscale[256];
	u8 chrflags[256];
	s32 numchrs;
	struct introanim anims[NUM_ANIMS];

	// the gun barrel (title.c's own names)
	s32 mode;
	f32 titlex, titley, transx, transy;
	s16 barreltimer;
	s32 counter;
	s32 gunbarreltimer;
	s32 shotplayed;
	u8 *bloodframe;      // BLOOD_W * BLOOD_H, one texel a byte
	const u8 *bloodnext;
	s32 blooddone;

	struct intromodel body, head, gun, logo;

	// the cast reel
	s32 castindex;
	s32 casttimer;
	s32 castanim;
	s32 castflip;
	s32 castweapon;
	f32 camdist0, camdist1, camangle0, camangle1, camheight0, camheight1;
} g_Intro;

/* ------------------------------------------------------------------------ */
/* loading */

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 be16(const u8 *p)
{
	return ((u32)p[0] << 8) | p[1];
}

static u8 *introLoad(const char *rel, u32 *len)
{
	char path[FS_MAXPATH + 1];
	const char *dir = fsGetModDirAt(g_Intro.moddir);

	if (!dir) {
		return NULL;
	}

	snprintf(path, sizeof(path), "%s/menu/%s", dir, rel);

	return fsFileLoad(path, len);
}

/** rle.c's rle_expand_8bit(): {u16 w, u16 h, six bytes, then count/value}. */
static u8 *introExpandRle(const u8 *src, u32 srclen, s32 wantw, s32 wanth)
{
	u32 at = 10;
	s32 left;
	u8 *out;
	u8 *p;

	if (srclen < 12 || (s32)be16(src) != wantw || (s32)be16(src + 2) != wanth) {
		return NULL;
	}

	left = wantw * wanth;
	out = sysMemZeroAlloc(left);

	if (!out) {
		return NULL;
	}

	p = out;

	while (left > 0 && at + 2 <= srclen) {
		s32 count = src[at];
		const u8 value = src[at + 1];

		at += 2;

		if (count > left) {
			count = left;
		}

		left -= count;

		while (count-- > 0) {
			*p++ = value;
		}
	}

	return out;
}

/**
 * menu/intro.bin: "GEI1", the characters' scales, then a row an animation - its
 * name and the fields Perfect Dark's animation table wants. Each animation is
 * appended after the game's own (animAppendExternal()), which is what a
 * borrowed mod's animations do.
 */
static s32 introLoadAnims(void)
{
	u32 len = 0;
	u8 *d = introLoad("intro.bin", &len);
	s32 numchrs, numanims;
	const u8 *rows;

	if (!d || len < 8 || memcmp(d, "GEI1", 4)) {
		sysMemFree(d);
		return 0;
	}

	numchrs = (s32)be16(d + 4);
	numanims = (s32)be16(d + 6);

	if (numchrs > (s32)(sizeof(g_Intro.chrscale) / sizeof(g_Intro.chrscale[0]))
			|| len < 8 + 8u * numchrs + 36u * numanims) {
		sysMemFree(d);
		return 0;
	}

	g_Intro.numchrs = numchrs;

	for (s32 i = 0; i < numchrs; i++) {
		const u32 bits = be32(d + 8 + 8 * i + 4);

		g_Intro.chrflags[i] = (u8)be16(d + 8 + 8 * i + 2);
		memcpy(&g_Intro.chrscale[i], &bits, sizeof(f32));
	}

	rows = d + 8 + 8 * numchrs;

	for (s32 i = 0; i < NUM_ANIMS; i++) {
		g_Intro.anims[i].animnum = -1;
	}

	for (s32 r = 0; r < numanims; r++) {
		const u8 *row = rows + 36 * r;
		const u32 at = be32(row + 28);
		const u32 size = be32(row + 32);
		struct introanim *a = NULL;
		u8 *copy;

		for (s32 i = 0; i < NUM_ANIMS; i++) {
			if (!strncmp((const char *)row, g_AnimNames[i], 20)) {
				a = &g_Intro.anims[i];
				break;
			}
		}

		if (!a || a->animnum >= 0 || at + size > len || !size) {
			continue;
		}

		a->entry.numframes = be16(row + 20);
		a->entry.bytesperframe = be16(row + 22);
		a->entry.headerlen = be16(row + 24);
		a->entry.framelen = row[26];
		a->entry.flags = 0;
		a->looping = row[27];

		// the header and the frames are read into the slot buffers the ROM's
		// own sizes made, and the bit reader runs off the end of the last frame
		copy = sysMemAlloc(size + 64);

		if (!copy) {
			continue;
		}

		memcpy(copy, d + at, size);
		memset(copy + size, 0, 64);
		a->entry.data = 0;
		a->animnum = animAppendExternal(&a->entry, copy);

		if (a->animnum < 0) {
			sysLogPrintf(LOG_WARNING, "geintro: no room for GoldenEye's `%s`", g_AnimNames[r < NUM_ANIMS ? r : 0]);
			sysMemFree(copy);
		}
	}

	sysMemFree(d);

	return g_Intro.anims[GEANIM_BOND_EYE_WALK].animnum >= 0;
}

static void introFreeModel(struct intromodel *m)
{
	if (m->model) {
		modelmgrFreeModel(m->model);
		m->model = NULL;
	}

	if (m->buf) {
		videoFreeCachedTextures(m->buf, m->buf + m->buflen);
		sysMemFree(m->buf);
		m->buf = NULL;
	}

	m->modeldef = NULL;
}

/**
 * A converted model by its GoldenEye number: a character (files/Cgx%03dZ) or a
 * prop (files/Pgx%03dZ), loaded the way the folder loads its own.
 */
static s32 introLoadModeldef(struct intromodel *m, s32 num, s32 ischr)
{
	char name[16];
	s32 fileid;
	s32 size;

	introFreeModel(m);
	snprintf(name, sizeof(name), ischr ? "Cgx%03dZ" : "Pgx%03dZ", num);
	fileid = romdataRegisterModFile(name, g_Intro.moddir);

	if (fileid <= 0) {
		return 0;
	}

	size = fileGetInflatedSize(fileid, LOADTYPE_MODEL);

	if (size <= 0) {
		return 0;
	}

	// the loader takes the file's textures' room from the same buffer
	m->buflen = ALIGN64(size) + 0x20000;
	m->buf = sysMemZeroAlloc(m->buflen);

	if (!m->buf) {
		return 0;
	}

	m->modeldef = modeldefLoad(fileid, m->buf, m->buflen, NULL);

	if (!m->modeldef) {
		introFreeModel(m);
		return 0;
	}

	return 1;
}

static f32 introChrScale(s32 num)
{
	return num >= 0 && num < g_Intro.numchrs ? g_Intro.chrscale[num] : 1.0f;
}

static s32 introChrIsMale(s32 num)
{
	return num >= 0 && num < g_Intro.numchrs ? (g_Intro.chrflags[num] & 1) : 1;
}

/**
 * A character with a head on it, as GoldenEye's setup_chr_instance() builds
 * one: the head's read/write data lives in the body's block, and the head is
 * attached at the body's headspot part.
 */
static s32 introLoadChr(struct intromodel *body, struct intromodel *head, s32 bodynum, s32 headnum, f32 scale, f32 animscale)
{
	struct modelnode *spot;

	if (!introLoadModeldef(body, bodynum, 1)) {
		return 0;
	}

	if (headnum >= 0 && !introLoadModeldef(head, headnum, 1)) {
		headnum = -1;
	}

	modelAllocateRwData(body->modeldef);
	spot = modelGetPart(body->modeldef, MODELPART_CHR_HEADSPOT);

	if (headnum >= 0 && head->modeldef && spot) {
		modelAllocateRwData(head->modeldef);
		body->modeldef->rwdatalen += head->modeldef->rwdatalen;
		body->model = modelmgrInstantiateModelWithAnim(body->modeldef);
		body->modeldef->rwdatalen -= head->modeldef->rwdatalen;

		if (body->model) {
			modelmgrAttachHead(body->model, spot, head->modeldef);
		}
	} else {
		body->model = modelmgrInstantiateModelWithAnim(body->modeldef);
	}

	if (!body->model) {
		introFreeModel(body);
		introFreeModel(head);
		return 0;
	}

	modelSetScale(body->model, scale);
	modelSetAnimScale(body->model, animscale);

	return 1;
}

static void introUnload(void)
{
	introFreeModel(&g_Intro.body);
	introFreeModel(&g_Intro.head);
	introFreeModel(&g_Intro.gun);
	introFreeModel(&g_Intro.logo);
	sysMemFree(g_Intro.bg);
	sysMemFree(g_Intro.blood);
	sysMemFree(g_Intro.bloodframe);
	g_Intro.bg = NULL;
	g_Intro.blood = NULL;
	g_Intro.bloodframe = NULL;
	g_Intro.loaded = 0;
}

static s32 introLoadAll(void)
{
	u32 len = 0;
	u8 *packed;

	g_Intro.moddir = -1;

	for (s32 i = 0; i < mpGetNumStages(); i++) {
		if (modloaderStageIsRemake(g_MpArenas[i].stagenum)) {
			g_Intro.moddir = modloaderGetStageModDirIndex(g_MpArenas[i].stagenum);
			break;
		}
	}

	if (g_Intro.moddir < 0 || !gexFrontLoadShared() || !introLoadAnims()) {
		sysLogPrintf(LOG_WARNING, "geintro: the conversion's intro files are missing; GE Plus opens on the folder");
		introUnload();
		return 0;
	}

	packed = introLoad("introbg.bin", &len);
	g_Intro.bg = packed ? introExpandRle(packed, len, BG_W, BG_H) : NULL;
	sysMemFree(packed);

	g_Intro.blood = introLoad("introblood.bin", &g_Intro.bloodlen);
	g_Intro.bloodframe = sysMemZeroAlloc(BLOOD_W * BLOOD_H);

	g_Intro.loaded = 1;

	return 1;
}

/* ------------------------------------------------------------------------ */
/* the blood down the lens (blood_decrypt.c) */

/**
 * One frame of the wash, decoded from where the last one ended: runs of lit and
 * unlit texels down a column, or a count of solid ones and how many columns
 * repeat it. Returns where the next frame starts, NULL at the end.
 */
static const u8 *introBloodDecode(const u8 *in, const u8 *end, u8 *out)
{
	u8 *o = out;
	u8 *const olimit = out + BLOOD_W * BLOOD_H;
	s32 rows = BLOOD_H;
	u8 first;

	if (!in || in >= end) {
		return NULL;
	}

	first = *in++;

	do {
		u8 value = 0xff;
		u8 run;

		if (in >= end) {
			return NULL;
		}

		run = *in++;

		if (run == 0xff) {
			u8 written = 0;

			for (run = (in < end) ? *in++ : 0xff; run != 0xff; value ^= 0xff, run = (in < end) ? *in++ : 0xff) {
				written += run;

				while (run-- > 0 && o < olimit) {
					*o++ = value;
				}
			}

			while (written++ < BLOOD_W && o < olimit) {
				*o++ = value;
			}

			rows--;
		} else {
			u8 lit = first + (run & 0x1f);
			u8 columns = (run >> 5) + 1;

			rows -= columns;

			do {
				u8 n = lit;

				while (n-- > 0 && o < olimit) {
					*o++ = 0xff;
				}

				n = BLOOD_W - lit;

				while (n-- > 0 && o < olimit) {
					*o++ = 0;
				}
			} while (--columns > 0);
		}
	} while (rows > 0 && o < olimit);

	return in < end ? in : NULL;
}

/** The decoded frame is column major; the texture wants it row major. */
static void introBloodTranspose(const u8 *src, u8 *dst)
{
	for (s32 y = 0; y < BLOOD_H; y++) {
		for (s32 x = 0; x < BLOOD_W; x++) {
			dst[x * BLOOD_H + y] = src[y * BLOOD_W + x];
		}
	}
}

/** The two four-texel averages GoldenEye softens the wash with. */
static void introBloodBlur(u8 *p)
{
	for (s32 i = 1; i < BLOOD_W - 1; i++) {
		for (s32 j = 1; j < BLOOD_H - 1; j++) {
			const s32 at = i * BLOOD_H + j;
			p[at] = (p[at + 1] + p[at] + p[at + BLOOD_H + 1] + p[at + BLOOD_H] + 2) >> 2;
		}
	}

	for (s32 i = 1; i < BLOOD_W - 1; i++) {
		for (s32 j = 1; j < BLOOD_H - 1; j++) {
			const s32 at = i * BLOOD_H + j;
			p[at] = (p[at - 1] + p[at] + p[at - BLOOD_H - 1] + p[at - BLOOD_H] + 2) >> 2;
		}
	}
}

/**
 * The next frame of the wash into g_Intro.bloodframe, ready to draw as a 4-bit
 * intensity texture (two texels a byte). `restart` begins it again.
 */
static s32 introBloodStep(s32 restart)
{
	u8 *frame;

	if (!g_Intro.blood || !g_Intro.bloodframe) {
		return 1;
	}

	if (restart) {
		g_Intro.bloodnext = g_Intro.blood;
	}

	if (!g_Intro.bloodnext) {
		return 1;
	}

	frame = sysMemZeroAlloc(BLOOD_W * BLOOD_H);

	if (!frame) {
		return 1;
	}

	g_Intro.bloodnext = introBloodDecode(g_Intro.bloodnext, g_Intro.blood + g_Intro.bloodlen, frame);
	introBloodTranspose(frame, g_Intro.bloodframe);
	introBloodBlur(g_Intro.bloodframe);
	sysMemFree(frame);

	// two texels a byte, the high nibble first
	for (s32 i = 0; i < BLOOD_W * BLOOD_H / 2; i++) {
		g_Intro.bloodframe[i] = (g_Intro.bloodframe[i * 2] & 0xf0) | (g_Intro.bloodframe[i * 2 + 1] >> 4);
	}

	return g_Intro.bloodnext == NULL;
}

/* ------------------------------------------------------------------------ */
/* drawing helpers */

static f32 introScaleY(void)
{
	return viGetHeight() / GEINTRO_H;
}

/**
 * GoldenEye's own 440x330 frame inside this one: how wide a column of it is
 * drawn here, and where its left edge falls. Its rows are introScaleY().
 *
 * This frame's pixels are not square - Perfect Dark's 320x220 is shown as 4:3,
 * as GoldenEye's own 440x330 is - so the width is not the frame's own 320/440
 * but that narrowed by however much wider than 4:3 the window is, which leaves
 * the picture the shape GoldenEye drew and centres it. That is the box the gun
 * barrel's ortho already uses (introBarrelOrtho(), whose 1280x960 widens at
 * the sides by exactly the same aspect), so anything drawn through here lines
 * up with the lens on any window: a wide one gets black beside the picture on
 * both sides, a narrow one loses the same from each.
 */
static f32 introFrameBox(f32 *left)
{
	const f32 scale = (viGetWidth() / GEINTRO_W) * ((4.0f / 3.0f) / videoGetAspect());

	*left = (viGetWidth() - GEINTRO_W * scale) * 0.5f;

	return scale;
}

static Gfx *introClearBlack(Gfx *gdl)
{
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_FILL);
	gDPSetScissor(gdl++, G_SC_NON_INTERLACE, 0, 0, viGetWidth(), viGetHeight());
	gDPSetFillColor(gdl++, GPACK_RGBA5551(0, 0, 0, 1) << 16 | GPACK_RGBA5551(0, 0, 0, 1));
	gDPFillRectangle(gdl++, 0, 0, viGetWidth() - 1, viGetHeight() - 1);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);

	return gdl;
}

/** A flat colour over the whole screen (sub_GAME_7F007E70() and 7F01CA18()). */
static Gfx *introWash(Gfx *gdl, s32 r, s32 g, s32 b, s32 a)
{
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, G_RM_CLD_SURF, G_RM_CLD_SURF2);
	gDPSetCombineMode(gdl++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
	gDPSetPrimColor(gdl++, 0, 0, r, g, b, a);
	gDPFillRectangle(gdl++, 0, 0, viGetWidth(), viGetHeight());
	gDPPipeSync(gdl++);

	return gdl;
}

/**
 * GoldenEye's gun barrel ortho, its 1280x960 widened at the sides on a wide
 * window so the lens stays round and stays where it is.
 */
static Gfx *introBarrelOrtho(Gfx *gdl)
{
	Mtx *m = gfxAllocateMatrix();
	Mtxf f;
	const f32 halfw = (BARREL_W / 2.0f) * videoGetAspect() / (4.0f / 3.0f);

	guOrthoF(f.m, BARREL_W / 2.0f - halfw, BARREL_W / 2.0f + halfw, 0.0f, BARREL_H, 1.0f, 8.0f, 1.0f);
	guMtxF2L(f.m, m);

	gSPMatrix(gdl++, m, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPClearGeometryMode(gdl++, G_ZBUFFER | G_LIGHTING | G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);

	return gdl;
}

/**
 * The lens: createGunbarrelRenderHole()'s half circle of radius 64, its mirror
 * down the other side, shaded light at the top and dark at the bottom, drawn as
 * the strip sub_GAME_7F01BFF8() makes of it, at (x, y) scaled by (sx, sy).
 */
static Gfx *introBarrelLens(Gfx *gdl, f32 x, f32 y, f32 sx, f32 sy)
{
	Vtx *v = gfxAllocate(BARREL_VERTICES * sizeof(Vtx));
	Col *c = gfxAllocate(BARREL_VERTICES * sizeof(Col));
	Mtx *m = gfxAllocateMatrix();
	Mtxf f;
	s32 n = 0;

	for (s32 i = 0; i <= BARREL_SEGMENTS; i += 2) {
		const f32 t = (f32)i * (f32)M_PI / (f32)BARREL_SEGMENTS;
		const s16 sinval = (s16)(sinf(t) * 64.0f);
		const s16 cosval = (s16)(cosf(t) * -64.0f);
		const u8 shade = (u8)(s32)(143.0f - (cosf(t) * -111.0f));

		for (s32 side = 0; side < 2; side++) {
			if (side && (i == 0 || i >= BARREL_SEGMENTS)) {
				continue;
			}

			if (n >= BARREL_VERTICES) {
				break;
			}

			v[n].x = side ? -sinval : sinval;
			v[n].y = cosval;
			v[n].z = 0;
			v[n].flags = 0;
			v[n].colour = n * 4;
			v[n].s = 0;
			v[n].t = 0;
			c[n].r = shade;
			c[n].g = shade;
			c[n].b = shade;
			c[n].a = 0xff;
			n++;
		}
	}

	mtx4LoadIdentity(&f);
	f.m[0][0] = sx;
	f.m[1][1] = sy;
	f.m[3][0] = x;
	f.m[3][1] = y;
	f.m[3][2] = -5.0f;
	guMtxF2L(f.m, m);

	gSPMatrix(gdl++, m, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

	// a Perfect Dark vertex names its colour in the table a G_COL loads, which
	// is how the converted models carry theirs
	gDma1p(gdl++, G_COL, c, n * 4, (n - 1) << 2);

	for (s32 at = 0; at + 3 <= n; at += 14) {
		const s32 count = n - at > 16 ? 16 : n - at;

		gSPVertex(gdl++, v + at, count, 0);

		for (s32 j = count - 3; j >= 0; j--) {
			gSP1Triangle(gdl++, j, j + 1, j + 2, 0);
		}
	}

	return gdl;
}

/**
 * The sniper sight's backdrop: GoldenEye's 440x299 picture, scrolled by
 * `xoffset` and shaded from black at the top to white at the bottom
 * (titleRenderFolderMenuBackgroundLines()).
 *
 * GoldenEye draws it as 299 rectangles a row tall because a 440 byte row is all
 * of the N64's texture memory it can hold at once, and shades each row as it
 * goes; this does the same. One 440x299 texture and 299 rectangles off it draws
 * black - the renderer keeps the N64's tile limit - and the rows are not what
 * the sequence costs anyway.
 */
static Gfx *introBackdrop(Gfx *gdl, s32 xoffset)
{
	f32 left;
	const f32 scale = introFrameBox(&left);
	const f32 rows = introScaleY();
	// the picture's own left edge in this frame, the texel it starts at there,
	// and its right edge, all of them GoldenEye's own 440 wide row scaled
	f32 x0 = left + (xoffset > 0 ? xoffset * scale : 0.0f);
	f32 x1 = left + BG_W * scale;
	f32 s0 = xoffset < 0 ? -xoffset : 0;

	if (!g_Intro.bg) {
		return gdl;
	}

	// a window narrower than 4:3 cuts the picture off rather than squeezing it,
	// which is what the ortho does with the lens, and a texture rectangle's own
	// coordinates cannot go negative
	if (x0 < 0.0f) {
		s0 -= x0 / scale;
		x0 = 0.0f;
	}

	if (x1 > viGetWidth()) {
		x1 = viGetWidth();
	}

	if (x1 <= x0) {
		return gdl;
	}

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetTextureFilter(gdl++, G_TF_POINT);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetCombineMode(gdl++, G_CC_MODULATEI_PRIM, G_CC_MODULATEI_PRIM);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);

	// GoldenEye's frame here is its own 440x330 (viSetXY(440, 330) on the way
	// into the front end) and this one is Perfect Dark's 320x220, so the
	// picture is GoldenEye's own pixels scaled into ours rather than drawn at
	// 440 wide: at 440 in *this* frame it is a third too big and only its dark
	// top rows are on the screen.
	//
	// The rectangle being scaled is not enough on its own: a texture rectangle
	// steps its texel per *pixel* (`dsdx`), so at GoldenEye's own 1 << 10 the
	// picture was drawn a texel a pixel whatever the rectangle said - 440 of
	// this frame's 320 pixels, with the last column smeared over the rest - and
	// the barrel's mouth sat two thirds of the way across while the lens, which
	// has an ortho and is scaled, sat at GoldenEye's half. The step is the
	// scale's own reciprocal.
	for (s32 i = 0; i + 1 < BG_H; i++) {
		const s32 shade = (255 * i) / (BG_H - 1);
		const s32 y0 = (s32)((i + 0x10) * rows);
		const s32 y1 = (s32)((i + 0x11) * rows);

		gDPLoadTextureBlock(gdl++, g_Intro.bg + i * BG_W, G_IM_FMT_I, G_IM_SIZ_8b, BG_W, 1, 0,
				G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
				G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
		gDPSetPrimColor(gdl++, 0, 0, shade, shade, shade, 255);
		gSPTextureRectangle(gdl++, (s32)(x0 * 4.0f), y0 << 2, (s32)(x1 * 4.0f), y1 << 2,
				G_TX_RENDERTILE, (s32)(s0 * 32.0f), 0, (s32)((1 << 10) / scale), 1 << 10);
	}

	gDPPipeSync(gdl++);

	return gdl;
}

/**
 * A model drawn under `base`, the way the menus draw one (menuRenderModel()):
 * its matrices start as the identity with the base in the first, an animated
 * one is posed through modelSetMatricesWithAnim() and a still one through
 * modelUpdateRelations(), and once everything hanging off it has been drawn
 * too, introFinishModel() turns its matrices into what the renderer reads.
 * They have to stay floats until then, since a gun's own matrix is one of the
 * body's.
 */
static Gfx *introDrawModel(Gfx *gdl, struct model *model, struct modeldef *def, Mtxf *base, s32 zbuffer)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };
	Mtxf *matrices = gfxAllocate(def->nummatrices * sizeof(Mtxf));

	// GoldenEye draws the intro's models with the distance checks off
	// (modelSetDistanceDisabled(1)), since the camera is far enough from them
	// in model units that every level of detail would test as out of range
	modelSetDistanceChecksDisabled(true);

	for (s32 i = 0; i < def->nummatrices; i++) {
		mtx4LoadIdentity(&matrices[i]);
	}

	mtx4Copy(base, matrices);
	model->matrices = matrices;

	renderdata.unk00 = base;
	renderdata.unk10 = matrices;

	if (model->anim) {
		modelSetMatricesWithAnim(&renderdata, model);
	} else {
		modelUpdateRelations(model);
	}

	renderdata.flags = 3;
	renderdata.zbufferenabled = zbuffer;
	renderdata.gdl = gdl;

	modelRender(&renderdata, model);
	modelSetDistanceChecksDisabled(false);

	return renderdata.gdl;
}

static void introFinishModel(struct model *model, struct modeldef *def)
{
	Mtxf tmp;

	if (!model || !model->matrices) {
		return;
	}

	for (s32 i = 0; i < def->nummatrices; i++) {
		mtx4Copy((Mtxf *)((uintptr_t)model->matrices + i * sizeof(Mtxf)), &tmp);
		mtxF2L(&tmp, model->matrices + i);
	}
}

/* ------------------------------------------------------------------------ */
/* the gun barrel */

static void introBarrelStart(void)
{
	const f32 scale = introChrScale(BODY_BROSNAN_TUXEDO) * 0.18779343f;

	g_Intro.mode = 2;
	g_Intro.titlex = -30.0f;
	g_Intro.titley = 482.0f;
	g_Intro.transx = -100.0f;
	g_Intro.transy = 482.0f;
	g_Intro.barreltimer = 0x42;
	g_Intro.counter = 0;
	g_Intro.gunbarreltimer = 0;
	g_Intro.shotplayed = 0;
	g_Intro.blooddone = 0;
	g_Intro.bloodnext = NULL;

	// modelSetScale() and modelSetAnimTranslationScale(1.0f), GoldenEye's own
	if (!introLoadChr(&g_Intro.body, &g_Intro.head, BODY_BROSNAN_TUXEDO, HEAD_BROSNAN_TUXEDO, scale, 1.0f)) {
		return;
	}

	{
		struct coord zero = { 0.0f, 0.0f, 0.0f };

		// setsuboffset() and setsubroty(): the root starts at the origin
		// facing down GoldenEye's zero, and the animation turns it from there
		modelSetRootPosition(g_Intro.body.model, &zero);
		modelSetChrRotY(g_Intro.body.model, 0.0f);
		modelSetAnimPlaySpeed(g_Intro.body.model, 0.5f, 0.0f);
	}

	// he walks in on the frame 0x44 before the end of the walk, as GoldenEye
	// starts him
	if (g_Intro.anims[GEANIM_BOND_EYE_WALK].animnum >= 0) {
		const s32 frames = g_Intro.anims[GEANIM_BOND_EYE_WALK].entry.numframes;
		s32 start = frames - 0x44;

		while (start < 0) {
			start += frames;
		}

		modelSetAnimation(g_Intro.body.model, g_Intro.anims[GEANIM_BOND_EYE_WALK].animnum, 0, (f32)start, 0.91f, 0.0f);
		modelSetAnimLooping(g_Intro.body.model, 0.0f, 0.0f);
	}

	if (introLoadModeldef(&g_Intro.gun, 191, 0)) {
		modelAllocateRwData(g_Intro.gun.modeldef);
		g_Intro.gun.model = modelmgrInstantiateModel(g_Intro.gun.modeldef, false);

		if (g_Intro.gun.model) {
			modelSetScale(g_Intro.gun.model, scale);
		}
	}
}

/** sub_GAME_7F007F30(): the walk, the turn and the shot, a frame at a time. */
static void introBarrelTickBond(void)
{
	// title.c's BOND_EYE_ANIM_START, _SPEEDUP and _FIRE_SHOT
	if (!g_Intro.body.model) {
		return;
	}

	for (s32 i = 0; i < 2; i++) {
		if (g_Intro.gunbarreltimer >= 0) {
			g_Intro.gunbarreltimer++;

			if (g_Intro.gunbarreltimer == 137 && g_Intro.anims[GEANIM_BOND_EYE_FIRE].animnum >= 0) {
				modelSetAnimation(g_Intro.body.model, g_Intro.anims[GEANIM_BOND_EYE_FIRE].animnum, 0, 2.0f, 0.910000026f, 16.0f);
			}

			if (g_Intro.gunbarreltimer == 212) {
				modelSetAnimSpeed(g_Intro.body.model, 1.6f, 8.0f);
			}
		}

		modelTickAnim(g_Intro.body.model, 1, 1);

		// GoldenEye fires GUN_RIFLE7BIG_1 here, out of its own sound bank,
		// which the conversion does not carry: the shot is silent for now
		if (g_Intro.gunbarreltimer == 230) {
			g_Intro.shotplayed = 1;
		}
	}

	// subcalcpos(), once the two ticks are done and not once each - it is what
	// carries the animation's root motion into the model, and GoldenEye calls
	// it outside the loop. Twice a frame walked Bond in at double speed and
	// left him past his mark when the sight closed
	modelUpdateInfo(g_Intro.body.model);
}

/** insert_bond_eye_intro(): 46 degrees from GoldenEye's own camera. */
static Gfx *introDrawBond(Gfx *gdl)
{
	Mtx *projection = gfxAllocateMatrix();
	Mtxf persp;
	Mtxf camera;
	u16 perspnorm;

	if (!g_Intro.body.model) {
		return gdl;
	}

	guPerspectiveF(persp.m, &perspnorm, 46.0f, videoGetAspect(), 10.0f, 10000.0f, 1.0f);
	guMtxF2L(persp.m, projection);

	gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, perspnorm);
	gSPClearGeometryMode(gdl++, G_ZBUFFER);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gDPSetRenderMode(gdl++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);

	// gunbarrelPosition1..3
	mtx00016ae4(&camera, 1758.2957f, 220.0f, 684.28143f,
			1758.2957f - 0.97f, 220.0f, 684.28143f + 0.24f, 0.0f, 1.0f, 0.0f);

	gdl = introDrawModel(gdl, g_Intro.body.model, g_Intro.body.modeldef, &camera, false);

	if (g_Intro.gun.model) {
		struct modelnode *hand = modelGetPart(g_Intro.body.modeldef, MODELPART_CHR_RIGHTHAND);
		Mtxf *mtx = hand ? modelFindNodeMtx(g_Intro.body.model, hand, 0) : NULL;

		if (mtx) {
			gdl = introDrawModel(gdl, g_Intro.gun.model, g_Intro.gun.modeldef, mtx, false);
			introFinishModel(g_Intro.gun.model, g_Intro.gun.modeldef);
		}
	}

	introFinishModel(g_Intro.body.model, g_Intro.body.modeldef);

	return gdl;
}

/**
 * gunbarrelBloodOverlayDL(): the frame of the wash over the whole screen, a
 * 4-bit intensity texture tinted GoldenEye's own dark red.
 */
static Gfx *introDrawBlood(Gfx *gdl)
{
	// over GoldenEye's own frame rather than this one's, since the wash runs
	// down the lens and has to stay on it whatever the window's shape is
	f32 left;
	const f32 scale = introFrameBox(&left);
	const f32 width = GEINTRO_W * scale;
	f32 x0 = left;
	f32 x1 = left + width;
	f32 s0 = 0.0f;

	if (!g_Intro.bloodframe) {
		return gdl;
	}

	if (x0 < 0.0f) {
		s0 = (-x0 / width) * BLOOD_H;
		x0 = 0.0f;
	}

	if (x1 > viGetWidth()) {
		x1 = viGetWidth();
	}

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetRenderMode(gdl++, G_RM_CLD_SURF, G_RM_CLD_SURF2);
	gDPSetCombineMode(gdl++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
	gDPSetPrimColor(gdl++, 0, 0, 0x96, 0x00, 0x00, 0xb4);
	gSPTexture(gdl++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON);
	gDPLoadTextureBlock_4b(gdl++, g_Intro.bloodframe, G_IM_FMT_I, BLOOD_H, BLOOD_W, 0,
			G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
			G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
	gSPTextureRectangle(gdl++, (s32)(x0 * 4.0f), 0, (s32)(x1 * 4.0f) - 1, (viGetHeight() * 4) - 1,
			G_TX_RENDERTILE, (s32)(s0 * 32.0f), 0, (s32)((BLOOD_H << 10) / width),
			0x14000 / viGetHeight());
	gDPPipeSync(gdl++);

	return gdl;
}

/**
 * renderGunbarrelEyeIntroSequence(), its seven modes as GoldenEye has them:
 * 2 the lens crossing black, 3 the sight closing on Bond, 4 the hold, 5 the
 * blood starting, 6 the wobble, 7 the fade, 8 black.
 */
static Gfx *introRenderBarrel(Gfx *gdl)
{
	switch (g_Intro.mode) {
	case 2:
		gdl = introClearBlack(gdl);
		gdl = introBarrelOrtho(gdl);
		gdl = introBarrelLens(gdl, g_Intro.titlex, g_Intro.titley, 1.0f, 1.0f);
		gdl = introBarrelLens(gdl, g_Intro.transx, g_Intro.transy, 1.0f, 1.0f);
		break;
	case 3:
		gdl = introClearBlack(gdl);
		gdl = introBackdrop(gdl, (s32)floorf((GEINTRO_W * g_Intro.titlex) / 1280.0f));
		gdl = introBarrelOrtho(gdl);
		gdl = introBarrelLens(gdl, g_Intro.titlex + 768.0f, g_Intro.titley - 40.0f, 2.7f, 2.57f);

		if (g_Intro.titlex < 600.0f) {
			gdl = introDrawBond(gdl);
		}
		break;
	default:
		gdl = introClearBlack(gdl);
		gdl = introBackdrop(gdl, (s32)floorf((GEINTRO_W * g_Intro.titlex) / 1280.0f));
		gdl = introBarrelOrtho(gdl);
		gdl = introBarrelLens(gdl, g_Intro.titlex + 768.0f, g_Intro.titley - 40.0f, 2.7f, 2.57f);
		gdl = introDrawBond(gdl);

		if (g_Intro.mode == 5 && g_Intro.blood) {
			gdl = introDrawBlood(gdl);
		} else if (g_Intro.mode >= 6) {
			gdl = introWash(gdl, 150, 0, 0, 180);
		}

		if (g_Intro.mode == 7) {
			gdl = introWash(gdl, 0, 0, 0, g_Intro.counter > 255 ? 255 : g_Intro.counter);
		}
		break;
	case 8:
		gdl = introClearBlack(gdl);
		break;
	}

	return gdl;
}

static void introTickBarrel(void)
{
	switch (g_Intro.mode) {
	case 2:
		g_Intro.titlex += 6.0f;

		if (g_Intro.barreltimer < 0) {
			g_Intro.barreltimer = 200;
			g_Intro.transx = g_Intro.titlex - 12.0f;
		} else {
			g_Intro.barreltimer -= 6;
		}

		if (g_Intro.titlex > 1390.0f) {
			g_Intro.mode++;
			g_Intro.titlex = 1276.0f;
		}
		break;
	case 3:
		if (g_Intro.titlex < 600.0f) {
			introBarrelTickBond();
		}

		g_Intro.titlex -= 5.8183274f;

		if (g_Intro.titlex <= -80.0f) {
			g_Intro.mode++;
			g_Intro.counter = 20;
		}
		break;
	case 4:
		introBarrelTickBond();
		g_Intro.counter--;

		if (g_Intro.counter < 0) {
			g_Intro.mode++;
			introBloodStep(1);
			g_Intro.counter = 1;
		}
		break;
	case 5:
		introBarrelTickBond();
		g_Intro.counter--;

		if (g_Intro.counter == 0) {
			g_Intro.blooddone = introBloodStep(0);
			g_Intro.counter = 2;
		}

		if (g_Intro.blooddone) {
			g_Intro.mode++;
			g_Intro.barreltimer = 0;
			g_Intro.transx = g_Intro.titlex;
			g_Intro.counter = 0;
		}
		break;
	case 6:
		introBarrelTickBond();
		g_Intro.barreltimer += 0x38e;
		g_Intro.counter++;
		g_Intro.titlex = sinf(g_Intro.barreltimer * (f32)M_PI / 32768.0f) * 64.0f + g_Intro.transx;

		if (g_Intro.counter >= 108) {
			g_Intro.counter = 0;
			g_Intro.mode++;
		}
		break;
	case 7:
		introBarrelTickBond();
		g_Intro.barreltimer += 0x38e;
		g_Intro.titlex = sinf(g_Intro.barreltimer * (f32)M_PI / 32768.0f) * 64.0f + g_Intro.transx;
		g_Intro.counter += 8;

		if (g_Intro.counter >= 0xf7) {
			g_Intro.counter = 0;
			g_Intro.mode++;
		}
		break;
	case 8:
		if (g_Intro.counter++ >= 0x1e) {
			g_Intro.counter = 0;
			g_Intro.mode++;
		}
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* the GoldenEye logo */

static void introLogoStart(void)
{
	g_Intro.counter = 0;

	introFreeModel(&g_Intro.body);
	introFreeModel(&g_Intro.head);
	introFreeModel(&g_Intro.gun);

	if (introLoadModeldef(&g_Intro.logo, 277, 0)) {
		modelAllocateRwData(g_Intro.logo.modeldef);
		g_Intro.logo.model = modelmgrInstantiateModel(g_Intro.logo.modeldef, false);

		if (g_Intro.logo.model) {
			struct coord zero = { 0.0f, 0.0f, 0.0f };

			modelSetScale(g_Intro.logo.model, 1.0f);
			modelSetRootPosition(g_Intro.logo.model, &zero);
		}
	}
}

/**
 * constructor_menu04_goldeneyelogo(): the logo 3000 in front of the camera at
 * 1.2 times its size, lit by one light and a reflected LookAt - the gold is the
 * model's own texture, which the conversion keeps inside the file.
 */
static Gfx *introRenderLogo(Gfx *gdl)
{
	Mtx *projection = gfxAllocateMatrix();
	Mtxf persp;
	Mtxf camera;
	Mtxf world;
	u16 perspnorm;

	gdl = introClearBlack(gdl);

	if (!g_Intro.logo.model) {
		return gdl;
	}

	guPerspectiveF(persp.m, &perspnorm, 60.0f, videoGetAspect(), 100.0f, 10000.0f, 1.0f);
	guMtxF2L(persp.m, projection);

	gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, perspnorm);
	gSPClearGeometryMode(gdl++, G_ZBUFFER);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);

	mtx00016ae4(&camera, 0.0f, 0.0f, 3000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
	mtx4LoadIdentity(&world);
	mtx00015f04(1.2f, &world);
	mtx4MultMtx4InPlace(&camera, &world);

	gdl = introDrawModel(gdl, g_Intro.logo.model, g_Intro.logo.modeldef, &world, false);
	introFinishModel(g_Intro.logo.model, g_Intro.logo.modeldef);

	return gdl;
}

/* ------------------------------------------------------------------------ */
/* the cast reel */

static s32 introRandom(s32 n)
{
	return n > 0 ? (s32)(rngRandom() % (u32)n) : 0;
}

/**
 * init_menu18_displaycast(): the next character, its animation, the gun it
 * holds and the arc the camera swings through, all rolled as GoldenEye rolls
 * them. The extras it only shows in the long version are stepped over.
 */
static void introCastStart(s32 first)
{
	const struct castrow *row;
	s32 body;
	s32 head;
	s32 anim;

	if (first) {
		g_Intro.castindex = 0;
	}

	while (g_Intro.castindex < NUM_CAST && g_Cast[g_Intro.castindex].extra) {
		g_Intro.castindex++;
	}

	if (g_Intro.castindex >= NUM_CAST) {
		g_Intro.stage = STAGE_DONE;
		return;
	}

	row = &g_Cast[g_Intro.castindex];
	body = row->body;
	head = row->head;
	anim = introRandom(NUM_CAST_ANIMS);

	// the five suits Bond is shown in, Natalya's two and Trevelyan's two
	if (body == BODY_SPECIAL_OPS) {
		switch (introRandom(5)) {
		case 1: body = BODY_FORMAL_WEAR;     head = HEAD_BROSNAN_DEFAULT; break;
		case 2: body = BODY_JUNGLE_FATIGUES; head = HEAD_BROSNAN_DEFAULT; break;
		case 3: body = BODY_PARKA;           head = HEAD_BROSNAN_DEFAULT; break;
		case 4: body = BODY_BROSNAN_TUXEDO;  head = HEAD_BROSNAN_TUXEDO;  break;
		}
	} else if (body == BODY_NATALYA_SKIRT && (rngRandom() & 1)) {
		body = 79; // CspicebondZ, GoldenEye's Natalya in fatigues
	} else if (body == BODY_TREVELYAN_006 && (rngRandom() & 1)) {
		body = BODY_TREVELYAN_JANUS;
	}

	// get_random_head(): a head of the body's own sex
	if (head == HEAD_ANY) {
		head = introChrIsMale(body)
			? HEAD_MALE_FIRST + introRandom(HEAD_FEMALE_FIRST - HEAD_MALE_FIRST)
			: HEAD_FEMALE_FIRST + introRandom(HEAD_FEMALE_END - HEAD_FEMALE_FIRST);
	}

	g_Intro.castflip = rngRandom() & 1;
	g_Intro.castanim = anim;
	g_Intro.casttimer = 0;

	introFreeModel(&g_Intro.gun);

	if (!introLoadChr(&g_Intro.body, &g_Intro.head, body, head < 0 ? -1 : head, introChrScale(body) * 0.1f, 0.1f)) {
		g_Intro.castindex++;
		return;
	}

	{
		struct coord zero = { 0.0f, 0.0f, 0.0f };
		const s32 animnum = g_Intro.anims[GEANIM_CAST_FIRST + anim].animnum >= 0
				? g_Intro.anims[GEANIM_CAST_FIRST + anim].animnum
				: g_Intro.anims[GEANIM_IDLE].animnum;

		modelSetRootPosition(g_Intro.body.model, &zero);
		modelSetChrRotY(g_Intro.body.model, 0.0f);
		modelSetAnimPlaySpeed(g_Intro.body.model, 0.5f, 0.0f);

		if (animnum >= 0) {
			modelSetAnimation(g_Intro.body.model, animnum, g_Intro.castflip,
					g_CastAnims[anim].startframe, g_CastAnims[anim].speed, 0.0f);
		}
	}

	g_Intro.castweapon = 0;

	if (g_CastAnims[anim].weapon == 1) {
		g_Intro.castweapon = g_CastPistols[introRandom(10)];
	} else if (g_CastAnims[anim].weapon == 2) {
		g_Intro.castweapon = g_CastRifles[introRandom(6)];
	}

	if (g_Intro.castweapon && introLoadModeldef(&g_Intro.gun, g_Intro.castweapon, 0)) {
		modelAllocateRwData(g_Intro.gun.modeldef);
		g_Intro.gun.model = modelmgrInstantiateModel(g_Intro.gun.modeldef, false);

		if (g_Intro.gun.model) {
			modelSetScale(g_Intro.gun.model, introChrScale(body) * 0.1f);
		}
	}

	g_Intro.camdist0 = (rngRandom() / (f32)0xffffffffu) * 80.0f + 70.0f;
	g_Intro.camdist1 = (rngRandom() / (f32)0xffffffffu) * 80.0f + 70.0f;
	g_Intro.camangle0 = ((rngRandom() / (f32)0xffffffffu) - 0.5f) * 6.2831855f;
	g_Intro.camangle1 = ((rngRandom() / (f32)0xffffffffu) - 0.5f) * 2.5132742f;
	g_Intro.camheight0 = (rngRandom() / (f32)0xffffffffu) * 200.0f - 100.0f;
	g_Intro.camheight1 = (rngRandom() / (f32)0xffffffffu) * 200.0f - 100.0f;
}

/**
 * constructor_menu18_displaycast(): the character on its arc, fading in and out,
 * with the three lines of its caption centred in GoldenEye's Zurich Bold.
 * GoldenEye's own camera smoothing follows the model's root; here the camera
 * looks at the character's own middle, which it settles on within the frame.
 */
static Gfx *introRenderCast(Gfx *gdl)
{
	const f32 frac = (f32)g_Intro.casttimer / (f32)CAST_LEN;
	const f32 dist = (g_Intro.camdist1 - g_Intro.camdist0) * frac + g_Intro.camdist0;
	const f32 height = (g_Intro.camheight1 - g_Intro.camheight0) * frac + g_Intro.camheight0;
	Mtx *projection = gfxAllocateMatrix();
	Mtxf persp;
	Mtxf camera;
	u16 perspnorm;
	f32 angle = (g_Intro.camangle1 - g_Intro.camangle0) * frac + g_Intro.camangle0;
	f32 fade;
	f32 camx, camz, tarx, tarz;
	struct coord root = { 0.0f, 0.0f, 0.0f };

	gdl = introClearBlack(gdl);

	if (g_Intro.casttimer < 0 || g_Intro.casttimer >= CAST_LEN) {
		fade = 0.0f;
	} else if (g_Intro.casttimer < CAST_FADE) {
		fade = (f32)g_Intro.casttimer / (f32)CAST_FADE;
	} else if (g_Intro.casttimer >= CAST_FADEOUT) {
		fade = (f32)(CAST_LEN - g_Intro.casttimer) / (f32)CAST_FADE;
	} else {
		fade = 1.0f;
	}

	if (angle < 0.0f) {
		angle += 6.2831855f;
	}

	if (g_Intro.body.model) {
		modelGetRootPosition(g_Intro.body.model, &root);
	}

	// GoldenEye's camera swings round the character on its own arc and follows
	// the model's root through a spring; here it swings round the root itself,
	// which the character stays inside over three seconds
	camx = root.x + dist * sinf(angle) + cosf(angle) * 0.2f * dist;
	camz = root.z + dist * cosf(angle) - sinf(angle) * 0.2f * dist;
	tarx = root.x + cosf(angle) * 0.2f * dist;
	tarz = root.z - sinf(angle) * 0.2f * dist;

	if (g_Intro.body.model) {
		guPerspectiveF(persp.m, &perspnorm, 46.0f, videoGetAspect(), 10.0f, 2000.0f, 1.0f);
		guMtxF2L(persp.m, projection);

		gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
		gSPPerspNormalize(gdl++, perspnorm);
		gSPClearGeometryMode(gdl++, G_ZBUFFER);
		gDPPipeSync(gdl++);
		gDPSetCycleType(gdl++, G_CYC_1CYCLE);

		// the camera is 52.5 above the feet and looks 10 below its own height,
		// which is where GoldenEye's smoothed target settles
		mtx00016ae4(&camera, camx, root.y + height + 52.5f, camz,
				tarx, root.y + height - 10.0f, tarz, 0.0f, 1.0f, 0.0f);

		gdl = introDrawModel(gdl, g_Intro.body.model, g_Intro.body.modeldef, &camera, false);

		if (g_Intro.gun.model) {
			struct modelnode *hand = modelGetPart(g_Intro.body.modeldef,
					g_Intro.castflip ? MODELPART_CHR_LEFTHAND : MODELPART_CHR_RIGHTHAND);
			Mtxf *mtx = hand ? modelFindNodeMtx(g_Intro.body.model, hand, 0) : NULL;

			if (mtx) {
				Mtxf turned;

				// a mirrored animation holds the gun in the left hand, which
				// GoldenEye turns half a turn about z to point the right way
				if (g_Intro.castflip) {
					mtx4LoadZRotation(3.1415927f, &turned);
					mtx4MultMtx4InPlace(mtx, &turned);
					mtx = &turned;
				}

				gdl = introDrawModel(gdl, g_Intro.gun.model, g_Intro.gun.modeldef, mtx, false);
				introFinishModel(g_Intro.gun.model, g_Intro.gun.modeldef);
			}
		}

		introFinishModel(g_Intro.body.model, g_Intro.body.modeldef);
	}

	// the fade is a black sheet over everything, thinning in and out
	gdl = introWash(gdl, 0, 0, 0, 255 - (s32)(255.0f * fade));

	{
		const struct castrow *row = &g_Cast[g_Intro.castindex];
		const s32 ids[3] = { row->text1, row->text2, row->text3 };
		// GoldenEye's 108, 152 and 174 on the 640x480 its text renderer measures
		// against, on the 440x330 frame everything else here is laid out on
		const s32 ys[3] = { 74, 105, 120 };
		const u32 colour = 0xffffff00 | (u32)(255.0f * fade);

		gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
		gdl = gexFrontTextSetup(gdl);

		for (s32 i = 0; i < 3; i++) {
			const char *text = gexFrontTitleString(ids[i]);
			s32 w = 0;
			s32 h = 0;

			if (!text || !text[0] || text[0] == '\n') {
				continue;
			}

			gexFrontTextMeasure(0, text, &w, &h);
			gdl = gexFrontTextPrint(gdl, 0, (s32)(GEINTRO_W / 2.0f) - w / 2, ys[i], text, colour);
		}

		gDPPipeSync(gdl++);
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	}

	return gdl;
}

static void introTickCast(void)
{
	if (g_Intro.body.model) {
		modelTickAnim(g_Intro.body.model, 1, 1);
		modelUpdateInfo(g_Intro.body.model);
	}

	g_Intro.casttimer++;

	if (g_Intro.casttimer >= CAST_LEN) {
		g_Intro.castindex++;
		introCastStart(0);
	}
}

/* ------------------------------------------------------------------------ */
/* the music */

/**
 * GoldenEye's M_INTRO as a sequence number of the game's, appended once on
 * GoldenEye's own instrument bank the way the folders theme is; -1 when the
 * conversion has no music.
 */
static s32 introMusic(void)
{
	static s32 seqnum = -2;
	u32 ctllen = 0;
	u32 len = 0;
	u32 seqlen = 0;
	u8 *raw;
	u8 *ctl;
	u8 *tbl;
	u8 *seqs;
	ALBank *bank;
	const u8 *e;

	if (seqnum != -2) {
		return seqnum;
	}

	seqnum = -1;

	raw = introLoad("instrumentsctl", &len);
	ctl = raw ? preprocessALBankFile(raw, len, &ctllen) : NULL;
	sysMemFree(raw);
	tbl = introLoad("instrumentstbl", &len);
	seqs = introLoad("sequences", &seqlen);

	if (!ctl || !tbl || !seqs || seqlen < 4 + (INTRO_SEQUENCE + 1) * 8
			|| ((seqs[0] << 8) | seqs[1]) <= INTRO_SEQUENCE) {
		sysMemFree(ctl);
		sysMemFree(tbl);
		sysMemFree(seqs);
		return -1;
	}

	alBnkfNew((ALBankFile *)ctl, tbl);
	bank = ((ALBankFile *)ctl)->bankArray[0];
	e = seqs + 4 + INTRO_SEQUENCE * 8;

	if (bank && be32(e) < seqlen && ((e[6] << 8) | e[7]) <= seqlen - be32(e)) {
		seqnum = seqAppend(seqs + be32(e), (e[4] << 8) | e[5], (e[6] << 8) | e[7], bank);
	}

	return seqnum;
}

s32 geIntroMusic(void)
{
	return g_Intro.active ? introMusic() : -1;
}

/* ------------------------------------------------------------------------ */
/* the intro */

static void introClose(void)
{
	g_Intro.active = 0;
	introFreeModel(&g_Intro.body);
	introFreeModel(&g_Intro.head);
	introFreeModel(&g_Intro.gun);
	introFreeModel(&g_Intro.logo);
	gexFrontOpen();
}

static void introNextStage(void)
{
	switch (g_Intro.stage) {
	case STAGE_BARREL:
		g_Intro.stage = STAGE_LOGO;
		introLogoStart();
		break;
	case STAGE_LOGO:
		g_Intro.stage = STAGE_CAST;
		introCastStart(1);
		break;
	default:
		g_Intro.stage = STAGE_DONE;
		break;
	}
}

s32 geIntroIsActive(void)
{
	return g_Intro.active;
}

s32 geIntroOpen(void)
{
	if (!g_Intro.loaded && !introLoadAll()) {
		return 0;
	}

	g_Intro.active = 1;
	g_Intro.stage = STAGE_BARREL;
	g_Intro.inputdelay = 2;
	introBarrelStart();

	if (introMusic() >= 0) {
		musicStartTrackAsMenu(introMusic());
	}

	return 1;
}

void geIntroTick(void)
{
	if (!g_Intro.active) {
		return;
	}

	if (g_Intro.inputdelay > 0) {
		g_Intro.inputdelay--;
	} else if (joyGetButtonsPressedThisFrame(0, 0xffff | BUTTON_UI_ACCEPT | BUTTON_UI_CANCEL)) {
		// as GoldenEye's, a press moves the intro on rather than ending it -
		// except from the cast reel, which a press leaves for the folder
		introNextStage();

		if (g_Intro.stage == STAGE_CAST) {
			g_Intro.stage = STAGE_DONE;
		}

		g_Intro.inputdelay = 2;
	}

	switch (g_Intro.stage) {
	case STAGE_BARREL:
		introTickBarrel();

		if (g_Intro.mode >= 9) {
			introNextStage();
		}
		break;
	case STAGE_LOGO:
		// GOLDENEYELOGO_TIMER_1: three seconds of it
		if (++g_Intro.counter >= 60 * 3) {
			introNextStage();
		}
		break;
	case STAGE_CAST:
		introTickCast();
		break;
	}

	if (g_Intro.stage == STAGE_DONE) {
		introClose();
	}
}

Gfx *geIntroRender(Gfx *gdl)
{
	if (!g_Intro.active) {
		return gdl;
	}

	switch (g_Intro.stage) {
	case STAGE_BARREL:
		return introRenderBarrel(gdl);
	case STAGE_LOGO:
		return introRenderLogo(gdl);
	case STAGE_CAST:
		return introRenderCast(gdl);
	}

	return introClearBlack(gdl);
}
