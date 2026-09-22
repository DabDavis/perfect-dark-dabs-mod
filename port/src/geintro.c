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
 * has an ortho of its own, GoldenEye's 1280x960. Its backdrop, its lens and its
 * blood all go through one box (`introFrameBox()`), which keeps the mouth of
 * the barrel round the lens and **fills** the window - a wide one loses the top
 * and bottom of GoldenEye's picture rather than showing black beside it.
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
#include "gbiex.h"
#include "geblood.h"
#include "gebean.h"
#include "gefolder.h"
#include "xblamesh.h"
#include "gesfx.h"
#include "geintro.h"
#include "gexfront.h"
#include "gemusic.h"
#include "preprocess.h"
#include "game/file.h"
#include "game/gfxmemory.h"
#include "game/menu.h"
#include "game/modeldef.h"
#include "game/modelmgr.h"
#include "game/music.h"
#include "game/tex.h"
#include "game/zbuf.h"
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

// front.c's gelogolight, which lights the logo and the cast reel
static Lights1 g_CastLight = gdSPDefLights1(0x96, 0x96, 0x96, 0xff, 0xff, 0xff, 0x4d, 0x4d, 0x2e);

// title.c's gunbarrelLights, which the Rare logo leaves set for the gun barrel
static Lights1 g_BarrelLight = gdSPDefLights1(0xdc, 0xdc, 0xdc, 0xff, 0xff, 0xff, 0x00, 0x7f, 0x00);

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

// front.c's own 315, the x the three lines of the caption are centred on
#define CAST_TEXT_X 315

// front.c's CAST_DAMP and CAST_DAMP_COMP, the spring the camera follows the
// character's root through
#define CAST_DAMP      0.94999999f
#define CAST_DAMP_COMP 0.050000012f

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
	// c_item_entries[]'s own scale, which is what the game scales a character
	// by and what the intro does not: both of its screens are flat (0.1 and
	// 0.18779343). Read because the conversion writes it beside the flags
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
	f32 tickacc; // 60ths of a second owed to introTickBarrel()
	struct geblood blood; // the wash down the lens (geblood.c)
	s32 blooddone;

	struct intromodel body, head, gun, logo;

	// the cast reel
	s32 castindex;
	s32 casttimer;
	s32 castanim;
	s32 castflip;
	s32 castweapon;
	f32 camdist0, camdist1, camangle0, camangle1, camheight0, camheight1;

	// front.c's cast_rootpos_smoothed, cast_rootvel_accumulator,
	// cast_target_accumulator and cast_target_smoothed: the camera follows the
	// character's root through a spring rather than being glued to it
	struct coord camroot;
	struct coord camrootacc;
	struct coord camtargetacc;
	struct coord camtarget;
	s32 camreset;
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

// a row of menu/intro.bin: the animation's name, then the fields Perfect Dark's
// animation table wants. The name is what a row is matched by, so the field has
// to hold the longest of them whole - `fire_standing_draw_fast` is 23 - and a
// 20-byte one truncated ten of the twenty-five, two of them
// (`fire_standing_draw_fast` and `_slow`) to the same 19 characters
#define INTRO_NAME 32
#define INTRO_ROW  (INTRO_NAME + 16)

/**
 * menu/intro.bin: "GEI2", the characters' scales, then a row an animation - its
 * name and the fields Perfect Dark's animation table wants. Each animation is
 * appended after the game's own (animAppendExternal()), which is what a
 * borrowed mod's animations do.
 */
static s32 g_IntroAnimsAppended;

static s32 introLoadAnims(void)
{
	u32 len = 0;
	u8 *d = introLoad("intro.bin", &len);
	s32 numchrs, numanims;
	const u8 *rows;

	if (!d || len < 8 || memcmp(d, "GEI2", 4)) {
		sysMemFree(d);
		return 0;
	}

	numchrs = (s32)be16(d + 4);
	numanims = (s32)be16(d + 6);

	if (numchrs > (s32)(sizeof(g_Intro.chrscale) / sizeof(g_Intro.chrscale[0]))
			|| len < 8 + 8u * numchrs + INTRO_ROW * numanims) {
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

	// Appending is permanent - an appended animation counts as one of the
	// ROM's and animsReset() keeps it - so it happens on the first way into
	// GE Plus and not again. It used to happen every time, which spent
	// twenty-five of the thousand rows there are (ANIM_EXTRA_CAPACITY) on each
	// visit and left the fortieth without its cast reel. The chr scales above
	// are read every time, since the screens are freed between visits.
	if (g_IntroAnimsAppended) {
		sysMemFree(d);
		return 1;
	}

	g_IntroAnimsAppended = 1;

	for (s32 i = 0; i < NUM_ANIMS; i++) {
		g_Intro.anims[i].animnum = -1;
	}

	for (s32 r = 0; r < numanims; r++) {
		const u8 *row = rows + INTRO_ROW * r;
		const u32 at = be32(row + INTRO_NAME + 8);
		const u32 size = be32(row + INTRO_NAME + 12);
		struct introanim *a = NULL;
		u8 *copy;

		for (s32 i = 0; i < NUM_ANIMS; i++) {
			if (!strncmp((const char *)row, g_AnimNames[i], INTRO_NAME)) {
				a = &g_Intro.anims[i];
				break;
			}
		}

		if (!a || a->animnum >= 0 || at + size > len || !size) {
			continue;
		}

		a->entry.numframes = be16(row + INTRO_NAME);
		a->entry.bytesperframe = be16(row + INTRO_NAME + 2);
		a->entry.headerlen = be16(row + INTRO_NAME + 4);
		a->entry.framelen = row[INTRO_NAME + 6];

		// GoldenEye's own loop bit (its record's `unk07 & 1`, which the
		// conversion writes into the row) is Perfect Dark's ANIMFLAG_LOOP: it
		// is what makes modelConstrainOrWrapAnimFrame() wrap a frame past the end
		// round to the front instead of holding the last one. Neither game
		// asks its intro to loop anything explicitly - title.c and front.c
		// both just set the animation - so dropping the bit froze every
		// looping animation at its end: the cast reel's `running_one_handed`
		// is 26 frames and the reel holds a character for 82, so the runner
		// ran for a second and then stood still in mid-stride for two
		a->entry.flags = row[INTRO_NAME + 7] ? ANIMFLAG_LOOP : 0;

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
			sysLogPrintf(LOG_WARNING, "geintro: no room for GoldenEye's `%s`",
					g_AnimNames[a - g_Intro.anims]);
			sysMemFree(copy);
		}
	}

	sysMemFree(d);

	// one the conversion did not carry, or whose row did not match its name, is
	// a character standing still where GoldenEye has it drawing a gun: the cast
	// reel falls back to `idle` and nothing else says so. Ten of them were
	// missing for a day this way
	for (s32 i = 0; i < NUM_ANIMS; i++) {
		if (g_Intro.anims[i].animnum < 0) {
			sysLogPrintf(LOG_WARNING, "geintro: the conversion has no `%s`", g_AnimNames[i]);
		}
	}

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
			struct modelnode *glasses;

			modelmgrAttachHead(body->model, spot, head->modeldef);

			// makeonebody(): a head's sunglasses are a toggle of its own and
			// come off unless the chr was asked for wearing them, which
			// nothing in the intro does. Perfect Dark starts a toggle visible,
			// so left alone every face in the reel wore a pair of lenses.
			glasses = modelGetPart(head->modeldef, MODELPART_HEAD_SUNGLASSES);

			if (glasses && (glasses->type & 0xff) == MODELNODETYPE_TOGGLE) {
				union modelrwdata *rwdata = modelGetNodeRwData(body->model, glasses);

				if (rwdata) {
					rwdata->toggle.visible = false;
				}
			}
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

static void introLogoMetalForget(void);

static void introUnload(void)
{
	introFreeModel(&g_Intro.body);
	introFreeModel(&g_Intro.head);
	introFreeModel(&g_Intro.gun);
	introLogoMetalForget();
	introFreeModel(&g_Intro.logo);
	sysMemFree(g_Intro.bg);
	geBloodDrop(&g_Intro.blood);
	g_Intro.bg = NULL;
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

	g_Intro.loaded = 1;

	return 1;
}

/* ------------------------------------------------------------------------ */
/* drawing helpers */

/**
 * GoldenEye's own 440x330 frame inside this one: a point (x, y) of GoldenEye's
 * is drawn at (left + x * scale, top + y * rows).
 *
 * This frame's pixels are not square - Perfect Dark's 320x220 is shown as 4:3,
 * as GoldenEye's own 440x330 is - so the two scales are whatever leaves the
 * picture the shape GoldenEye drew it. The box **covers** the window rather
 * than fitting inside it: both directions are scaled by the same amount as they
 * appear on the screen, and whichever one is then too big runs off the edges -
 * the top and bottom on a window wider than 4:3, the sides on a narrower one.
 *
 * Fitted instead, a 16:9 window drew the picture in its middle three quarters
 * with black either side, and the picture's own edges slid across that black as
 * the sight scrolled. Covering loses the top and bottom rows of GoldenEye's
 * picture, which are its margins: the barrel and the lens are in the middle.
 *
 * The gun barrel's ortho is the same box (introBarrelOrtho() shows just as much
 * of GoldenEye's 1280x960 as this shows of the picture), which is what keeps
 * the mouth of the barrel round the lens on any window.
 */
struct introbox {
	f32 scale;   // a column of GoldenEye's 440, in this frame's pixels
	f32 rows;    // a row of its 330
	f32 left;    // where its left edge falls, negative when it is off the side
	f32 top;     // and its top
	f32 coverx;  // how much wider than the window the box is, and taller
	f32 covery;
};

static void introFrameBox(struct introbox *box)
{
	// how much narrower than the window 4:3 is, which is the shape of both
	// GoldenEye's frame and the 320x220 this one is shown in
	const f32 narrow = (4.0f / 3.0f) / videoGetAspect();

	box->coverx = narrow > 1.0f ? narrow : 1.0f;
	box->covery = box->coverx / narrow;

	box->scale = box->coverx * viGetWidth() / GEINTRO_W;
	box->rows = box->covery * viGetHeight() / GEINTRO_H;
	box->left = (viGetWidth() - GEINTRO_W * box->scale) * 0.5f;
	box->top = (viGetHeight() - GEINTRO_H * box->rows) * 0.5f;
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
	struct introbox box;
	f32 halfw, halfh;

	// as much of GoldenEye's 1280x960 as introFrameBox() shows of its picture,
	// so the lens sits in the mouth of the barrel on any window
	introFrameBox(&box);
	halfw = (BARREL_W / 2.0f) / box.coverx;
	halfh = (BARREL_H / 2.0f) / box.covery;

	guOrthoF(f.m, BARREL_W / 2.0f - halfw, BARREL_W / 2.0f + halfw,
			BARREL_H / 2.0f - halfh, BARREL_H / 2.0f + halfh, 1.0f, 8.0f, 1.0f);
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
 * A config naming one of the release's pictures (geFolderMenuPicture()): the
 * renderer draws a stand-in's whole picture over the config's nominal square,
 * whatever its real size, as the folder screens' frontReleasePicture() does.
 */
#define RELEASE_TEXELS 32

static struct textureconfig *introReleaseTexture(struct textureconfig *tex, const void *tile)
{
	memset(tex, 0, sizeof(*tex));
	tex->textureptr = (u8 *)tile;
	tex->width = RELEASE_TEXELS;
	tex->height = RELEASE_TEXELS;
	tex->format = G_IM_FMT_RGBA;
	tex->depth = G_IM_SIZ_32b;
	tex->s = G_TX_CLAMP;
	tex->t = G_TX_CLAMP;

	return tex;
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
	struct introbox box;
	f32 scale, rows;
	// the picture's own left edge in this frame, the texel it starts at there,
	// and its right edge, all of them GoldenEye's own 440 wide row scaled
	f32 x0, x1, s0;
	struct textureconfig tex;
	const void *tile;
	s32 release, rw, rh;

	introFrameBox(&box);
	scale = box.scale;
	rows = box.rows;
	x0 = box.left + (xoffset > 0 ? xoffset * scale : 0.0f);
	x1 = box.left + BG_W * scale;
	s0 = xoffset < 0 ? -xoffset : 0;

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

	// The release's own picture of it (texture/attract/barrel), 1052x715 -
	// GoldenEye's framing at 2.39 times the texels - when the release is there
	// and its look is on; GoldenEye's rows else.
	tile = geFolderMenuPicture("attract/barrel", &rw, &rh);
	release = tile != NULL;

	gDPPipeSync(gdl++);

	// texSelect() sets modes of its own, so it goes ahead of this frame's
	if (release) {
		texSelect(&gdl, introReleaseTexture(&tex, tile), 1, 0, 2, 1, NULL);
	}

	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetTextureFilter(gdl++, release ? G_TF_BILERP : G_TF_POINT);
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
		// a row of the picture, clipped to the screen: on a wide window the box
		// is taller than the frame and a texture rectangle's own coordinates
		// cannot go negative
		s32 y0 = (s32)(box.top + (i + 0x10) * rows);
		s32 y1 = (s32)(box.top + (i + 0x11) * rows);

		if (y1 <= 0 || y0 >= viGetHeight()) {
			continue;
		}

		if (y0 < 0) {
			y0 = 0;
		}

		if (y1 > viGetHeight()) {
			y1 = viGetHeight();
		}

		gDPSetPrimColor(gdl++, 0, 0, shade, shade, shade, 255);

		if (release) {
			// The same row of the release's picture, shaded as GoldenEye
			// shades its own. The renderer draws a stand-in's whole picture
			// over its config's nominal square (RELEASE_TEXELS), so both
			// steps are counted in that square's texels per GoldenEye texel;
			// and the release's rows run bottom to top (as its portraits do,
			// gexfront.c), so t counts down from the picture's far edge.
			const f32 perx = (f32)RELEASE_TEXELS / BG_W;
			const f32 pery = (f32)RELEASE_TEXELS / BG_H;
			const f32 t0 = RELEASE_TEXELS - (i + (y0 - (box.top + (i + 0x10) * rows)) / rows) * pery;

			gSPTextureRectangle(gdl++, (s32)(x0 * 4.0f), y0 << 2, (s32)(x1 * 4.0f), y1 << 2,
					G_TX_RENDERTILE, (s32)(s0 * perx * 32.0f), (s32)(t0 * 32.0f),
					(s32)(perx * 1024.0f / scale), -(s32)(pery * 1024.0f / rows));
			continue;
		}

		gDPLoadTextureBlock(gdl++, g_Intro.bg + i * BG_W, G_IM_FMT_I, G_IM_SIZ_8b, BG_W, 1, 0,
				G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
				G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
		gSPTextureRectangle(gdl++, (s32)(x0 * 4.0f), y0 << 2, (s32)(x1 * 4.0f), y1 << 2,
				G_TX_RENDERTILE, (s32)(s0 * 32.0f), 0, (s32)((1 << 10) / scale), 1 << 10);
	}

	gDPPipeSync(gdl++);

	return gdl;
}

/**
 * The one light and the reflected LookAt GoldenEye lights the logo and the cast
 * reel by (`gelogolight`, and `guLookAtReflect()` from 4000 in front of the
 * origin). Without them a model took whatever the menu drawn before it had left
 * behind, which lit one character in flat white and the next in black.
 */
static Gfx *introSetLightsWith(Gfx *gdl, Lights1 *lights)
{
	LookAt *lookat = gfxAllocateLookAt(2);
	Mtx lookatmtx;

	gSPNumLights(gdl++, NUMLIGHTS_1);
	gSPLight(gdl++, &lights->l[0], 1);
	gSPLight(gdl++, &lights->a, 2);
	guLookAtReflect(&lookatmtx, lookat, 0.0f, 0.0f, 4000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
	gSPLookAt(gdl++, lookat);

	return gdl;
}

static Gfx *introSetLights(Gfx *gdl)
{
	return introSetLightsWith(gdl, &g_CastLight);
}

/**
 * A model drawn under `base`, the way the menus draw one (menuRenderModel()):
 * its matrices start as the identity with the base in the first, an animated
 * one is posed through modelSetMatricesWithAnim() and a still one through
 * modelSetMatrices(), and once everything hanging off it has been drawn
 * too, introFinishModel() turns its matrices into what the renderer reads.
 * They have to stay floats until then, since a gun's own matrix is one of the
 * body's.
 */
static Gfx *introDrawModel(Gfx *gdl, struct model *model, struct modeldef *def, Mtxf *base, s32 zbuffer)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };
	Mtxf *matrices = gfxAllocate(def->nummatrices * sizeof(Mtxf));

	// The texture state a model is drawn under, which every 2D picture on
	// these screens leaves set to its own (introDrawPicture() draws the
	// sight's backdrop with G_TP_NONE and G_TF_POINT, geblood.c's wash with
	// G_TP_NONE and G_TF_BILERP) and none of them put back - the game's own
	// 2D passes do put both back, as zbufSaveArtifacts() does.
	//
	// Perspective was already set for the gun barrel and is set here for all
	// three screens: without it the texture coordinates collapse and Bond's
	// tuxedo comes out flat black with a smeared face.
	//
	// **The filter is the release's meshes' (xblamesh.c).** A posed one drawn
	// under G_TF_POINT draws black - the whole of a character's body, in the
	// barrel and in the cast reel alike, while the rigid head mesh grafted on
	// top of it draws correctly - which is what "his body is all black only"
	// was. Nothing of the mesh's own state is wrong there: its picture is
	// bound and uploaded, its texture coordinates span the atlas and its
	// colours are white. The N64 look survives point sampling, which is why
	// only the release's look showed it.
	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);

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

	// subcalcmatrices() for the animated chr, instcalcmatrices() for the gun and
	// the logo, which carry no animation. modelSetMatrices() is the call that
	// builds the joints; modelUpdateRelations() only resolves a node's distance,
	// reorder, toggle and head relations and computes no matrix at all, so a
	// model given only that was drawn with every joint but its root left at
	// identity - which is what took the gun off the hand it hangs from
	if (model->anim) {
		modelSetMatricesWithAnim(&renderdata, model);
	} else {
		modelUpdateRelations(model);
		modelSetMatrices(&renderdata, model);
	}

	// GoldenEye draws both of the intro's screens with `PropType` 7
	// (PROP_TYPE_EXPLOSION, which is what the cast reel names it), and that
	// number picks the render mode a display list node is drawn under. Left at
	// zero the models were drawn through the preset for a prop type the game
	// never uses, whose combine is `G_CC_TRILERP` - a blend of two mipmap
	// levels, and nothing here loads a second one - so Bond's tuxedo came out
	// black with no shirt in it, mixed with whatever was left in the second
	// tile. Seven is `G_CC_CUSTOM_17/18`: the texture faded towards the
	// environment colour by the shade, which is how a chr is drawn in a level.
	renderdata.unk30 = 7;
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
	// title.c's own modelSetScale(chrModelInstance, 0.18779343f), and the same
	// number again for the gun: GoldenEye scales the intro's models flat and
	// never applies a character's own scale from c_item_entries[] here
	const f32 scale = 0.18779343f;

	g_Intro.mode = 2;
	g_Intro.titlex = -30.0f;
	g_Intro.titley = 482.0f;
	g_Intro.transx = -100.0f;
	g_Intro.transy = 482.0f;
	g_Intro.barreltimer = 0x42;
	g_Intro.counter = 0;
	g_Intro.gunbarreltimer = 0;
	g_Intro.shotplayed = 0;
	g_Intro.tickacc = 0.0f;
	g_Intro.blooddone = 0;
	g_Intro.blood.next = NULL;

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

		// the walk's own loop bit carries it round the seam, which is what
		// GoldenEye leaves it to: modelSetAnimLooping() stood here instead and
		// restarted the animation at the end of every cycle, which skips the
		// tween across the seam and resets the root the walk has accumulated
		modelSetAnimation(g_Intro.body.model, g_Intro.anims[GEANIM_BOND_EYE_WALK].animnum, 0, (f32)start, 0.91f, 0.0f);
	}

	if (introLoadModeldef(&g_Intro.gun, 191, 0)) {
		modelAllocateRwData(g_Intro.gun.modeldef);
		g_Intro.gun.model = modelmgrInstantiateModel(g_Intro.gun.modeldef, false);

		if (g_Intro.gun.model) {
			modelSetScale(g_Intro.gun.model, scale);
		}
	}
}

static void introSetGunPart(s32 part, s32 visible);

/**
 * sub_GAME_7F007F30(): the walk, the turn and the shot. GoldenEye runs two of
 * these ticks in each of its frames and introTickBarrel() runs one in each of
 * its own, which are half of GoldenEye's.
 */
static void introBarrelTickBond(void)
{
	// title.c's BOND_EYE_ANIM_START, _SPEEDUP and _FIRE_SHOT
	if (!g_Intro.body.model) {
		return;
	}

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

	// GoldenEye's own GUN_RIFLE7BIG_1, out of its own sound bank, and the
	// flash lasts the frame of GoldenEye's the shot goes off in: two ticks
	if (g_Intro.gunbarreltimer == 230) {
		geSfxPlay(GESFX_GUN_RIFLE7BIG_1, GESFX_VOLUME);
	}

	g_Intro.shotplayed = g_Intro.gunbarreltimer == 230 || g_Intro.gunbarreltimer == 231;

	// subcalcpos(), which carries the animation's root motion into the model.
	// It moves the root by what the animation has advanced since it last ran,
	// so after every tick comes to what GoldenEye's one call a frame does
	modelUpdateInfo(g_Intro.body.model);

	// and the PP7's muzzle flash is on for the one frame the shot goes off and
	// off for every other, which is what GoldenEye does with the gun's first
	// switch here (`Gunfire.visible = playedShot`). Perfect Dark starts a
	// muzzle flash hidden, so the shot had none at all
	introSetGunPart(MODELPART_CHRGUN_GUNFIRE, g_Intro.shotplayed);
}

/** insert_bond_eye_intro(): 46 degrees from GoldenEye's own camera. */
static Gfx *introDrawBond(Gfx *gdl)
{
	Mtx *projection = gfxAllocateMatrix();
	Mtxf persp;
	Mtxf camera;
	struct introbox box;
	f32 fovy;
	u16 perspnorm;

	if (!g_Intro.body.model) {
		return gdl;
	}

	// GoldenEye's 46 degrees are over the height of its 4:3 frame, and on a
	// wide window the barrel shows less than that height (introFrameBox()
	// covers the window), so Bond's camera has to show as much less. Left at
	// 46 he kept his size while the lens round him grew, and stood small and
	// left of its middle, the lens being off centre by a share of the *width*
	introFrameBox(&box);
	fovy = atanf(tanf(46.0f * 0.5f * (f32)M_PI / 180.0f) / box.covery) * 2.0f * 180.0f / (f32)M_PI;

	guPerspectiveF(persp.m, &perspnorm, fovy, videoGetAspect(), 10.0f, 10000.0f, 1.0f);
	guMtxF2L(persp.m, projection);

	gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, perspnorm);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gDPSetRenderMode(gdl++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);

	// The Rare logo before this screen is what leaves GoldenEye its light
	// (load_display_rare_logo() sets gunbarrelLights and turns G_LIGHTING on),
	// and the gun barrel never sets one of its own - so the light has to be set
	// here, where the boot screens are not played. Unlit, Bond's tuxedo is
	// drawn out of a chr's colour table, which for a lit model holds its
	// normals: a black suit with no shirt in it and a smeared face.
	//
	// The texture state that same logo leaves - perspective, and the filter -
	// is set per model in introDrawModel(), since all three screens need it.
	gSPSetGeometryMode(gdl++, G_LIGHTING);
	gdl = introSetLightsWith(gdl, &g_BarrelLight);

	// gunbarrelPosition1..3
	mtx00016ae4(&camera, 1758.2957f, 220.0f, 684.28143f,
			1758.2957f - 0.97f, 220.0f, 684.28143f + 0.24f, 0.0f, 1.0f, 0.0f);

	// GoldenEye draws this screen with renderData.zbufferenabled = FALSE and
	// keeps Bond solid another way: sub_GAME_7F06B120() gathers his joints and
	// the gun's into one list, sub_GAME_7F06BB28() sorts it and drawjointlist()
	// runs it twice - a painter's order this port has no equivalent of, since
	// modelRender() draws a model's own lists in their own order. Without it
	// the arm hanging at his far side was painted over his chest and the gun
	// with it. The cast reel is already drawn into a z buffer, which is
	// GoldenEye's own answer on the one screen it gives one to, so the barrel
	// takes the same here
	gdl = zbufClear(gdl);
	gSPSetGeometryMode(gdl++, G_ZBUFFER);

	gdl = introDrawModel(gdl, g_Intro.body.model, g_Intro.body.modeldef, &camera, true);

	if (g_Intro.gun.model) {
		struct modelnode *hand = modelGetPart(g_Intro.body.modeldef, MODELPART_CHR_RIGHTHAND);
		Mtxf *mtx = hand ? modelFindNodeMtx(g_Intro.body.model, hand, 0) : NULL;

		if (mtx) {
			gdl = introDrawModel(gdl, g_Intro.gun.model, g_Intro.gun.modeldef, mtx, true);
			introFinishModel(g_Intro.gun.model, g_Intro.gun.modeldef);
		}
	}

	introFinishModel(g_Intro.body.model, g_Intro.body.modeldef);

	// the lens, the blood and the fades that follow are flat sheets again
	gSPClearGeometryMode(gdl++, G_ZBUFFER);

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
	struct introbox box;
	f32 width, height;
	f32 x0, x1, y0, y1;
	f32 s0 = 0.0f;
	f32 t0 = 0.0f;

	if (!g_Intro.blood.frame) {
		return gdl;
	}

	introFrameBox(&box);
	width = GEINTRO_W * box.scale;
	height = GEINTRO_H * box.rows;
	x0 = box.left;
	x1 = box.left + width;
	y0 = box.top;
	y1 = box.top + height;

	if (x0 < 0.0f) {
		s0 = (-x0 / width) * GEBLOOD_H;
		x0 = 0.0f;
	}

	if (x1 > viGetWidth()) {
		x1 = viGetWidth();
	}

	if (y0 < 0.0f) {
		t0 = (-y0 / height) * GEBLOOD_W;
		y0 = 0.0f;
	}

	if (y1 > viGetHeight()) {
		y1 = viGetHeight();
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
	gDPLoadTextureBlock_4b(gdl++, g_Intro.blood.frame, G_IM_FMT_I, GEBLOOD_H, GEBLOOD_W, 0,
			G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
			G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
	gSPTextureRectangle(gdl++, (s32)(x0 * 4.0f), (s32)(y0 * 4.0f), (s32)(x1 * 4.0f) - 1, (s32)(y1 * 4.0f) - 1,
			G_TX_RENDERTILE, (s32)(s0 * 32.0f), (s32)(t0 * 32.0f), (s32)((GEBLOOD_H << 10) / width),
			(s32)((GEBLOOD_W << 10) / height));
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

		if (g_Intro.mode == 5 && geBloodAvailable()) {
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

/**
 * One sixtieth of a second of the gun barrel.
 *
 * GoldenEye steps this sequence once a frame, whatever a frame took, so how
 * fast it plays is how fast the N64 drew it: the two dots at sixty frames a
 * second, and everything from the sight on at thirty (measured off a capture
 * of the console - the sight crosses at 5.8183274 * 30 a second, Bond fires
 * 3.8 seconds after he appears and the blood takes 2.8 to run down). Its own
 * PAL numbers say the same, being these scaled from thirty to fifty. M_INTRO
 * starts with the dots and is written to that, so stepped once per frame of
 * this port the sight, the walk and the blood all ran at twice their speed and
 * ahead of the music.
 *
 * So from mode 3 on a tick here is half a frame of GoldenEye's: half its
 * steps, twice its counts, and one of Bond's two animation ticks - which keeps
 * the motion as smooth as this port draws it rather than stepping it at
 * thirty.
 */
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

		g_Intro.titlex -= 5.8183274f * 0.5f;

		if (g_Intro.titlex <= -80.0f) {
			g_Intro.mode++;
			g_Intro.counter = 20 * 2;
		}
		break;
	case 4:
		introBarrelTickBond();
		g_Intro.counter--;

		if (g_Intro.counter < 0) {
			g_Intro.mode++;
			geBloodStep(&g_Intro.blood, 1);
			g_Intro.counter = 1 * 2;
		}
		break;
	case 5:
		introBarrelTickBond();
		g_Intro.counter--;

		if (g_Intro.counter == 0) {
			g_Intro.blooddone = geBloodStep(&g_Intro.blood, 0);
			g_Intro.counter = 2 * 2;
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
		g_Intro.barreltimer += 0x38e / 2;
		g_Intro.counter++;
		g_Intro.titlex = sinf(g_Intro.barreltimer * (f32)M_PI / 32768.0f) * 64.0f + g_Intro.transx;

		if (g_Intro.counter >= 108 * 2) {
			g_Intro.counter = 0;
			g_Intro.mode++;
		}
		break;
	case 7:
		introBarrelTickBond();
		g_Intro.barreltimer += 0x38e / 2;
		g_Intro.titlex = sinf(g_Intro.barreltimer * (f32)M_PI / 32768.0f) * 64.0f + g_Intro.transx;
		g_Intro.counter += 8 / 2;

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
 * The GoldenEye XBLA look's logo in the levels' metal (Mod.XblaReflectStyle's
 * Level Metal, as the title's own logos take it - xblaMeshBuildLogo()):
 * Defection's grey walkway metal (0x006d) sphere-mapped off the logo's normals
 * with the eye ray bending the lookup, added over the gold. The logo is not
 * one of the release's meshes but GoldenEye's own model, so the pass is its
 * lists drawn a second time: a copy of each, built once, that opens with the
 * metal's texture and blend and has the list's own texture loads, combiner
 * and render modes taken out so that state holds through it.
 */
#define LOGO_METAL_SCALE 0x0800 // the rooms' share of the picture (XBLAMESH_METAL_SCALE)
#define LOGO_METAL_SHARE 0xff   // xblaLogoAddMetalShare
#define LOGO_METAL_PREFIX 32
#define LOGO_METAL_MAXNODES 32
#define LOGO_METAL_MAXCMDS 4096

static struct {
	struct modelnode *node;
	Gfx *copy;
} logoMetal[LOGO_METAL_MAXNODES];
static s32 numLogoMetal;
static s32 logoMetalTried;

static void introLogoMetalForget(void)
{
	for (s32 i = 0; i < numLogoMetal; i++) {
		free(logoMetal[i].copy);
	}

	numLogoMetal = 0;
	logoMetalTried = 0;
}

static s32 introLogoMetalWanted(void)
{
	return gebeanGetEnabled() && xblaMeshGetEnabled() && xblaMeshLevelMetalTile() != NULL;
}

static Gfx *introLogoMetalCopy(const Gfx *src, const void *tile)
{
	struct textureconfig tex;
	Gfx *copy;
	Gfx *p;
	s32 n = 0;

	while (n < LOGO_METAL_MAXCMDS && (u8)(src[n].words.w0 >> 24) != (u8)G_ENDDL) {
		n++;
	}

	if (n >= LOGO_METAL_MAXCMDS) {
		return NULL;
	}

	copy = malloc((size_t)(LOGO_METAL_PREFIX + n + 1) * sizeof(Gfx));

	if (!copy) {
		return NULL;
	}

	// The metal's state, ahead of the list: its picture on the stand-in's
	// 32x32 at the rooms' scale, lit and sphere-mapped, added unlit at the
	// share (0, 0, 0, TEXEL0 in colour: the texel alone).
	p = copy;
	gDPPipeSync(p++);
	texSelect(&p, introReleaseTexture(&tex, tile), 1, 0, 2, 1, NULL);
	gDPSetCycleType(p++, G_CYC_1CYCLE);
	gDPSetTexturePersp(p++, G_TP_PERSP);
	gDPSetTextureFilter(p++, G_TF_BILERP);
	gDPSetRenderMode(p++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetEnvColor(p++, 0, 0, 0, LOGO_METAL_SHARE);
	gDPSetCombineLERP(p++, 0, 0, 0, TEXEL0, 0, 0, 0, ENVIRONMENT, 0, 0, 0, TEXEL0, 0, 0, 0, ENVIRONMENT);
	gSPTexture(p++, LOGO_METAL_SCALE, LOGO_METAL_SCALE, 0, G_TX_RENDERTILE, G_ON);
	gSPSetGeometryMode(p++, G_LIGHTING | G_TEXTURE_GEN);
	gSPSetExtraGeometryModeEXT(p++, G_TEXGEN_EYE_EXT | G_ADDITIVE_EXT);

	for (s32 i = 0; i < n; i++) {
		Gfx g = src[i];

		switch ((u8)(g.words.w0 >> 24)) {
		case (u8)G_SETTIMG:
		case (u8)G_LOADBLOCK:
		case (u8)G_LOADTILE:
		case (u8)G_LOADTLUT:
		case (u8)G_SETTILE:
		case (u8)G_SETTILESIZE:
		case (u8)G_TEXTURE:
		case (u8)G_SETCOMBINE:
		case (u8)G_RDPSETOTHERMODE:
		case (u8)G_SETOTHERMODE_H:
		case (u8)G_SETOTHERMODE_L:
		case (u8)G_SETPRIMCOLOR:
		case (u8)G_SETENVCOLOR:
			gDPNoOp(&g);
			break;
		case (u8)G_CLEARGEOMETRYMODE:
			g.words.w1 &= ~(uintptr_t)(G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
			break;
		case (u8)G_DL:
			sysLogPrintf(LOG_NOTE, "geintro: the logo's list calls another (%lx), which keeps its own state",
					(unsigned long)g.words.w1);
			break;
		}

		*p++ = g;
	}

	gSPEndDisplayList(p++);

	return copy;
}

/**
 * Every list node of the logo, its copy swapped in for a second modelRender()
 * over the same matrices, and put back.
 */
static Gfx *introLogoMetal(Gfx *gdl)
{
	struct modelrenderdata renderdata = { NULL, false, 3 };
	struct model *model = g_Intro.logo.model;
	const void *tile = xblaMeshLevelMetalTile();
	Gfx *saved[LOGO_METAL_MAXNODES];

	if (!logoMetalTried) {
		logoMetalTried = 1;

		for (struct modelnode *node = g_Intro.logo.modeldef->rootnode; node; ) {
			if ((node->type & 0xff) == MODELNODETYPE_DL && numLogoMetal < LOGO_METAL_MAXNODES) {
				union modelrwdata *rwdata = modelGetNodeRwData(model, node);

				if (rwdata && rwdata->dl.gdl) {
					// the list is behind the node's colours in the file, and named
					// by its offset in segment 5 (SPSEGMENT_MODEL_COL1), which is
					// what modelRenderNodeDl() points at them - the low bit marks
					// a segmented address (gfx_pc.cpp's seg_addr())
					const uintptr_t addr = (uintptr_t)rwdata->dl.gdl;
					const Gfx *list = rwdata->dl.gdl;

					if ((addr & 1) && ((addr >> 24) & 0xf) == SPSEGMENT_MODEL_COL1) {
						list = (const Gfx *)((u8 *)node->rodata->dl.colours + (addr & 0x00fffffe));
					}

					logoMetal[numLogoMetal].node = node;
					logoMetal[numLogoMetal].copy = introLogoMetalCopy(list, tile);

					if (logoMetal[numLogoMetal].copy) {
						numLogoMetal++;
					}
				}
			}

			if (node->child) {
				node = node->child;
			} else {
				while (node && !node->next) {
					node = node->parent;
				}

				node = node ? node->next : NULL;
			}
		}
	}

	if (numLogoMetal == 0) {
		return gdl;
	}

	for (s32 i = 0; i < numLogoMetal; i++) {
		union modelrwdata *rwdata = modelGetNodeRwData(model, logoMetal[i].node);

		saved[i] = rwdata->dl.gdl;
		rwdata->dl.gdl = logoMetal[i].copy;
	}

	renderdata.unk30 = 7;
	renderdata.flags = MODELRENDERFLAG_OPA;
	renderdata.zbufferenabled = false;
	renderdata.gdl = gdl;

	modelSetDistanceChecksDisabled(true);
	modelRender(&renderdata, model);
	modelSetDistanceChecksDisabled(false);

	gdl = renderdata.gdl;

	for (s32 i = 0; i < numLogoMetal; i++) {
		((union modelrwdata *)modelGetNodeRwData(model, logoMetal[i].node))->dl.gdl = saved[i];
	}

	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_TEXGEN_EYE_EXT | G_ADDITIVE_EXT);
	gSPClearGeometryMode(gdl++, G_LIGHTING | G_TEXTURE_GEN);

	return gdl;
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

	gdl = introSetLights(gdl);

	mtx00016ae4(&camera, 0.0f, 0.0f, 3000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
	mtx4LoadIdentity(&world);
	mtx00015f04(1.2f, &world);
	mtx4MultMtx4InPlace(&camera, &world);

	gdl = introDrawModel(gdl, g_Intro.logo.model, g_Intro.logo.modeldef, &world, false);

	// the GoldenEye XBLA look's metal over the gold, on the same matrices,
	// which introFinishModel() turns for both passes
	if (introLogoMetalWanted()) {
		gdl = introLogoMetal(gdl);
	}

	introFinishModel(g_Intro.logo.model, g_Intro.logo.modeldef);

	return gdl;
}

/* ------------------------------------------------------------------------ */
/* the cast reel */

static s32 introRandom(s32 n)
{
	return n > 0 ? (s32)(rngRandom() % (u32)n) : 0;
}

/** A switch of the gun the character is holding, hidden or shown. */
static void introSetGunPart(s32 part, s32 visible)
{
	struct modelnode *node = modelGetPart(g_Intro.gun.modeldef, part);
	union modelrwdata *rwdata = node ? modelGetNodeRwData(g_Intro.gun.model, node) : NULL;

	if (!rwdata) {
		return;
	}

	switch (node->type & 0xff) {
	case MODELNODETYPE_TOGGLE:
		rwdata->toggle.visible = visible;
		break;
	case MODELNODETYPE_CHRGUNFIRE:
		rwdata->chrgunfire.visible = visible;
		break;
	}
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

	// init_menu18_displaycast()'s own modelSetScale(cast_model, 0.1f) and
	// modelSetAnimTranslationScale(cast_model, 0.1f), flat for every character
	if (!introLoadChr(&g_Intro.body, &g_Intro.head, body, head < 0 ? -1 : head, 0.1f, 0.1f)) {
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

		// modelTickAnim() and subcalcpos(), which GoldenEye's constructor runs
		// before the camera's spring reads the character: the root the spring
		// snaps to has to be the one the frame is drawn with. Left to the next
		// frame's tick the root was still at the origin setsuboffset() put it,
		// so the camera snapped to the floor and then climbed to the
		// character's own height over the fade - the bounce at every switch
		modelTickAnim(g_Intro.body.model, 1, 1);
		modelUpdateInfo(g_Intro.body.model);
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
			modelSetScale(g_Intro.gun.model, 0.1f);

			// constructor_menu18_displaycast() turns the gun's first two
			// switches off every frame: the muzzle flash, which Perfect Dark
			// starts hidden anyway, and the toggled part beside it, which it
			// starts visible - so the cast carried a spare piece of gun
			introSetGunPart(MODELPART_CHRGUN_GUNFIRE, false);
			introSetGunPart(MODELPART_CHRGUN_0002, false);
		}
	}

	g_Intro.camdist0 = (rngRandom() / (f32)0xffffffffu) * 80.0f + 70.0f;
	g_Intro.camdist1 = (rngRandom() / (f32)0xffffffffu) * 80.0f + 70.0f;
	g_Intro.camangle0 = ((rngRandom() / (f32)0xffffffffu) - 0.5f) * 6.2831855f;
	g_Intro.camangle1 = ((rngRandom() / (f32)0xffffffffu) - 0.5f) * 2.5132742f;
	g_Intro.camheight0 = (rngRandom() / (f32)0xffffffffu) * 200.0f - 100.0f;
	g_Intro.camheight1 = (rngRandom() / (f32)0xffffffffu) * 200.0f - 100.0f;

	// cast_camera_reset: the spring starts on the character it is given
	g_Intro.camroot.x = g_Intro.camroot.y = g_Intro.camroot.z = 0.0f;
	g_Intro.camtarget.x = g_Intro.camtarget.y = g_Intro.camtarget.z = 0.0f;
	g_Intro.camreset = 1;
}

/**
 * Where the character's root matrix has got to, which is the point the camera
 * aims at: GoldenEye poses the character once under an identity base before it
 * poses it under the camera, and reads the translation out of the first matrix
 * (`mtx4TransformVecInPlace(cast_model->render_pos, &vec)` on a vector that is
 * all but zero). It is **not** the chr's own position - that is where the
 * character stands, and its root joint is about half a metre above it, which is
 * the difference between framing a chest and framing a waist.
 */
static void introCastRootMtx(struct coord *pos)
{
	struct model *model = g_Intro.body.model;
	struct modeldef *def = g_Intro.body.modeldef;
	struct modelrenderdata renderdata = { NULL, false, 3 };
	Mtxf *matrices;
	Mtxf identity;

	pos->x = pos->y = pos->z = 0.0f;

	if (!model || !def || def->nummatrices <= 0) {
		return;
	}

	matrices = gfxAllocate(def->nummatrices * sizeof(Mtxf));
	mtx4LoadIdentity(&identity);

	for (s32 i = 0; i < def->nummatrices; i++) {
		mtx4LoadIdentity(&matrices[i]);
	}

	model->matrices = matrices;
	renderdata.unk00 = &identity;
	renderdata.unk10 = matrices;

	if (model->anim) {
		modelSetMatricesWithAnim(&renderdata, model);
	} else {
		modelUpdateRelations(model);
		modelSetMatrices(&renderdata, model);
	}

	pos->x = matrices[0].m[3][0];
	pos->y = matrices[0].m[3][1];
	pos->z = matrices[0].m[3][2];
}

/**
 * The spring constructor_menu18_displaycast() follows the character through,
 * run once a frame: the smoothed position the camera stands off from, which
 * follows the chr's own, and the smoothed lag between that and the root matrix,
 * which is what the camera aims at. Both are low passes of the same shape - an
 * accumulator fed the difference and read back at CAST_DAMP_COMP - and both
 * start on the character the reel has just put up (cast_camera_reset).
 *
 * Glued straight to the character instead, the camera carried every step and
 * swing of the animation's root motion with it and nothing ever moved inside
 * the frame.
 */
static void introCastCamera(void)
{
	struct coord root = { 0.0f, 0.0f, 0.0f };
	struct coord mtxpos;
	struct coord vec;

	if (g_Intro.body.model) {
		modelGetRootPosition(g_Intro.body.model, &root);
	}

	introCastRootMtx(&mtxpos);

	if (g_Intro.camreset) {
		g_Intro.camroot.y = root.y;
	}

	// g_GlobalTimerDelta is one frame here, so the difference is the velocity
	vec.x = root.x - g_Intro.camroot.x;
	vec.y = root.y - g_Intro.camroot.y;
	vec.z = root.z - g_Intro.camroot.z;

	if (g_Intro.camreset) {
		g_Intro.camrootacc.x = vec.x / CAST_DAMP_COMP;
		g_Intro.camrootacc.y = vec.y / CAST_DAMP_COMP;
		g_Intro.camrootacc.z = vec.z / CAST_DAMP_COMP;
	}

	g_Intro.camrootacc.x = vec.x + CAST_DAMP * g_Intro.camrootacc.x;
	g_Intro.camrootacc.y = vec.y + CAST_DAMP * g_Intro.camrootacc.y;
	g_Intro.camrootacc.z = vec.z + CAST_DAMP * g_Intro.camrootacc.z;

	g_Intro.camroot.x += g_Intro.camrootacc.x * CAST_DAMP_COMP;
	g_Intro.camroot.y += g_Intro.camrootacc.y * CAST_DAMP_COMP;
	g_Intro.camroot.z += g_Intro.camrootacc.z * CAST_DAMP_COMP;

	// the lag between the smoothed position and the root matrix
	vec.x = mtxpos.x - g_Intro.camroot.x;
	vec.y = mtxpos.y - g_Intro.camroot.y;
	vec.z = mtxpos.z - g_Intro.camroot.z;

	if (g_Intro.camreset) {
		g_Intro.camtargetacc.x = vec.x / CAST_DAMP_COMP;
		g_Intro.camtargetacc.y = vec.y / CAST_DAMP_COMP;
		g_Intro.camtargetacc.z = vec.z / CAST_DAMP_COMP;
	}

	g_Intro.camtargetacc.x = vec.x + CAST_DAMP * g_Intro.camtargetacc.x;
	g_Intro.camtargetacc.y = vec.y + CAST_DAMP * g_Intro.camtargetacc.y;
	g_Intro.camtargetacc.z = vec.z + CAST_DAMP * g_Intro.camtargetacc.z;

	g_Intro.camtarget.x = g_Intro.camtargetacc.x * CAST_DAMP_COMP;
	g_Intro.camtarget.y = g_Intro.camtargetacc.y * CAST_DAMP_COMP;
	g_Intro.camtarget.z = g_Intro.camtargetacc.z * CAST_DAMP_COMP;

	g_Intro.camreset = 0;
}

/**
 * constructor_menu18_displaycast(): the character on its arc, fading in and out,
 * with the three lines of its caption centred in GoldenEye's Zurich Bold.
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
	const struct coord *root = &g_Intro.camroot;

	gdl = introClearBlack(gdl);

	// GoldenEye runs the camera from its own constructor rather than from its
	// tick, and the pose it reads the root matrix out of wants a frame's memory
	introCastCamera();

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

	// the camera swings round the smoothed position on its own arc and aims at
	// where the character's own middle has got to (introCastCamera())
	camx = root->x + dist * sinf(angle) + cosf(angle) * 0.2f * dist;
	camz = root->z + dist * cosf(angle) - sinf(angle) * 0.2f * dist;
	tarx = root->x + g_Intro.camtarget.x + cosf(angle) * 0.2f * dist;
	tarz = root->z + g_Intro.camtarget.z - sinf(angle) * 0.2f * dist;

	if (g_Intro.body.model) {
		guPerspectiveF(persp.m, &perspnorm, 46.0f, videoGetAspect(), 10.0f, 2000.0f, 1.0f);
		guMtxF2L(persp.m, projection);

		gSPMatrix(gdl++, projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
		gSPPerspNormalize(gdl++, perspnorm);
		gDPPipeSync(gdl++);
		gDPSetCycleType(gdl++, G_CYC_1CYCLE);

		gdl = introSetLights(gdl);

		// the cast reel is the one screen of GoldenEye's front end drawn into a
		// z buffer (init_menu18_displaycast() sets one up and asks for it),
		// since a character drawn in list order paints its far side over its
		// near one - a face over its own nose, a cap through the head under it
		gdl = zbufClear(gdl);
		gSPSetGeometryMode(gdl++, G_ZBUFFER);

		// the camera stands 52.5 above the smoothed position and looks 10 below
		// the root matrix, which is the character's middle: the height swings
		// the camera alone, which is what tips the shot up and down. Added to
		// the target as well it tipped nothing, and framed the legs
		mtx00016ae4(&camera, camx, root->y + height + 52.5f, camz,
				tarx, root->y + g_Intro.camtarget.y - 10.0f, tarz, 0.0f, 1.0f, 0.0f);

		gdl = introDrawModel(gdl, g_Intro.body.model, g_Intro.body.modeldef, &camera, true);

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

				gdl = introDrawModel(gdl, g_Intro.gun.model, g_Intro.gun.modeldef, mtx, true);
				introFinishModel(g_Intro.gun.model, g_Intro.gun.modeldef);
			}
		}

		introFinishModel(g_Intro.body.model, g_Intro.body.modeldef);

		gSPClearGeometryMode(gdl++, G_ZBUFFER);
	}

	// the fade is a black sheet over everything, thinning in and out
	gdl = introWash(gdl, 0, 0, 0, 255 - (s32)(255.0f * fade));

	{
		const struct castrow *row = &g_Cast[g_Intro.castindex];
		const s32 ids[3] = { row->text1, row->text2, row->text3 };
		// GoldenEye's own 108, 152 and 174, which are already on the 440x330
		// frame its text renderer lays everything out on - scaling them as if
		// they were 640x480 put the caption a third of the way up the screen
		const s32 ys[3] = { 108, 152, 174 };
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

			// centred on GoldenEye's own 315, which is right of the middle of
			// the frame: the character is drawn to the left of the caption and
			// a caption centred on the frame was drawn across its face
			gexFrontTextMeasure(0, text, &w, &h);
			gdl = gexFrontTextPrint(gdl, 0, CAST_TEXT_X - w / 2, ys[i], text, colour);
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

/** GoldenEye's M_INTRO as a sequence number of the game's; -1 when the conversion has no music. */
static s32 introMusic(void)
{
	return geMusicSequence(GEMUSIC_INTRO);
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
	introLogoMetalForget();
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

	// the fonts and the strings the cast reel's captions are drawn with belong
	// to the folder screens, which free them when GE Plus is left: what the
	// first intro loaded is not still there the second time round, and asking
	// only when the intro's own files load left the reel silent
	if (!gexFrontLoadShared()) {
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
		// by the clock and not by the frame, so the barrel keeps GoldenEye's
		// time and its place in the music at any frame rate; a hitch is not
		// made up, as it never was on the console
		g_Intro.tickacc += g_Vars.diffframe60freal;

		if (g_Intro.tickacc > 4.0f) {
			g_Intro.tickacc = 4.0f;
		}

		while (g_Intro.tickacc >= 1.0f && g_Intro.mode < 9) {
			g_Intro.tickacc -= 1.0f;
			introTickBarrel();
		}

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
