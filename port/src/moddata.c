/**
 * A mod's weapon definitions and model tables, read out of its ROM's data
 * segment and rebuilt in the port's own layout.
 *
 * A console mod that brings its own guns does it by rewriting the tables in
 * the ROM's data segment: g_Weapons[] says which definition each weapon number
 * is, and the definitions - struct weapon and everything hanging off it: the
 * functions, ammo, aim settings, gun command lists, gunviscmds, part
 * visibility, noise and recoil settings - are edited in place or repointed at
 * each other. GE-X does both to nearly every slot. None of that is in a file
 * the port loads; it is .data, compiled into the game.
 *
 * So the imported mod carries the whole inflated segment (segs/data, written
 * by tools/importmod), and this walks the graph from each g_Weapons slot,
 * converting every object it reaches into a freshly allocated port struct.
 * The N64 layouts are fixed here as byte offsets, since the port's structs
 * are the same fields at different offsets (pointers are 8 bytes) and a
 * bitfield packs the other way round. An object reached twice is converted
 * once, so what the mod shares stays shared - two slots on one definition are
 * still one definition.
 *
 * What the segment cannot say: file ids are the mod's own table, so they go
 * through the mod's name list (segs/data.names) to the port's slot for that
 * name; and flags2, unequippedreloadindex and pickupsound are the port's own
 * fields, not the ROM's. A definition sitting at the same address as a stock
 * one - a patched-in-place mod editing the shotgun where the shotgun was -
 * inherits those from the stock definition, and a modconfig weapon block
 * after this one can say otherwise.
 */

#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "fs.h"
#include "utils.h"
#include "romdata.h"
#include "mod.h"
#include "data.h"
#include "game/stagetable.h"
#include "game/env.h"
#include "game/mplayer/setup.h"

#define GUNCMD_MAX_LEN     512
#define GUNVISCMD_MAX_LEN  256
#define PARTVIS_MAX_LEN    256
#define VIBRATION_LEN      12

// N64 struct sizes, checked against the symbol spacing in the decomp's map
#define N64_WEAPON_SIZE            0x50
#define N64_GUNCMD_SIZE            0x08
#define N64_GUNVISCMD_SIZE         0x0a
#define N64_MODELSTATE_SIZE        0x08
#define N64_MPWEAPON_SIZE          0x0a
#define N64_MPWEAPONSET_SIZE       0x12
#define N64_MPARENA_SIZE           0x06
#define N64_HEADORBODY_SIZE        0x14
#define N64_MPHEAD_SIZE            0x04
#define N64_MPBODY_SIZE            0x08

// g_MpFeaturesUnlocked has 80 entries and the game defines features up to
// MPFEATURE_STAGE_GRID (0x29); the last slot is never set by an unlock
#define MODDATA_FEATURE_NEVER      79

struct modseg {
	const u8 *data;
	u32 len;
	u32 base;
	char **names;    // mod file id -> name, from segs/data.names
	s32 numnames;
	s32 *fileids;    // mod file id -> port file id, resolved on first use (-2 = not yet)
	s32 badreads;
	s32 badfiles;
};

static struct modseg seg;

// --moddata-trace: one log line per imported slot and Combat Simulator entry
static bool modDataTrace;

struct modmemo {
	u32 addr;
	void *ptr;
};

static struct modmemo *memo;
static s32 numMemo;
static s32 maxMemo;
static s32 numObjects;

/* ---- reading the segment ---------------------------------------------- */

static inline bool inseg(u32 addr, u32 n)
{
	return addr >= seg.base && addr - seg.base + n <= seg.len;
}

static u32 rd32(u32 addr)
{
	if (!inseg(addr, 4)) {
		++seg.badreads;
		return 0;
	}
	const u8 *p = seg.data + (addr - seg.base);
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u16 rd16(u32 addr)
{
	if (!inseg(addr, 2)) {
		++seg.badreads;
		return 0;
	}
	const u8 *p = seg.data + (addr - seg.base);
	return (u16)(((u32)p[0] << 8) | p[1]);
}

static u8 rd8(u32 addr)
{
	if (!inseg(addr, 1)) {
		++seg.badreads;
		return 0;
	}
	return seg.data[addr - seg.base];
}

static f32 rdf32(u32 addr)
{
	u32 v = rd32(addr);
	f32 f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

/* ---- one conversion per object ---------------------------------------- */

static void *memoGet(u32 addr)
{
	for (s32 i = 0; i < numMemo; ++i) {
		if (memo[i].addr == addr) {
			return memo[i].ptr;
		}
	}
	return NULL;
}

static void memoPut(u32 addr, void *ptr)
{
	if (numMemo == maxMemo) {
		s32 newmax = maxMemo ? maxMemo * 2 : 1024;
		struct modmemo *grown = sysMemAlloc(sizeof(struct modmemo) * newmax);
		if (!grown) {
			return;
		}
		if (memo) {
			memcpy(grown, memo, sizeof(struct modmemo) * numMemo);
			sysMemFree(memo);
		}
		memo = grown;
		maxMemo = newmax;
	}
	memo[numMemo].addr = addr;
	memo[numMemo].ptr = ptr;
	++numMemo;
}

static void *newobj(u32 size)
{
	void *p = sysMemZeroAlloc(size);
	if (p) {
		++numObjects;
	}
	return p;
}

/* ---- file ids ----------------------------------------------------------- */

/**
 * The mod's file id, as the port's. The segment's ids index the mod ROM's
 * own file table; the port's slots follow the stock table plus whatever the
 * mod added, so the name is what carries across.
 */
static u16 modFileId(u32 modid)
{
	if (modid == 0) {
		return 0;
	}

	if (!seg.names) {
		return (u16)modid; // no list: assume the mod kept the stock table
	}

	if ((s32)modid >= seg.numnames) {
		++seg.badfiles;
		return 0;
	}

	if (seg.fileids[modid] == -2) {
		s32 id = romdataFileGetNumForName(seg.names[modid]);
		if (id < 0) {
			// a file the mod added: it is in the mod's files/, so give it a slot
			id = romdataRegisterModFile(seg.names[modid], 0);
			if (id > 0) {
				sysLogPrintf(LOG_NOTE, "moddata: file %d `%s` is the mod's own, now slot %d", modid, seg.names[modid], id);
			} else {
				sysLogPrintf(LOG_WARNING, "moddata: file %d `%s` is not in the port's file table", modid, seg.names[modid]);
				++seg.badfiles;
				id = 0;
			}
		}
		seg.fileids[modid] = id;
	}

	return (u16)seg.fileids[modid];
}

/* ---- the graph ---------------------------------------------------------- */

static struct guncmd *cvGuncmds(u32 addr)
{
	if (!addr) {
		return NULL;
	}

	struct guncmd *out = memoGet(addr);
	if (out) {
		return out;
	}

	s32 n = 0;
	while (n < GUNCMD_MAX_LEN && rd8(addr + n * N64_GUNCMD_SIZE) != GUNCMD_END) {
		++n;
	}

	out = newobj(sizeof(struct guncmd) * (n + 1));
	if (!out) {
		return NULL;
	}

	// in the memo before the includes are followed: a list that includes
	// itself, or two that include each other, must not recurse forever
	memoPut(addr, out);

	for (s32 i = 0; i <= n; ++i) {
		u32 at = addr + i * N64_GUNCMD_SIZE;
		out[i].type = rd8(at);
		out[i].unk01 = rd8(at + 1);
		out[i].unk02 = rd16(at + 2);
		u32 v = rd32(at + 4);
		if (out[i].type == GUNCMD_INCLUDE || out[i].type == GUNCMD_RANDOM) {
			out[i].unk04 = (intptr_t)cvGuncmds(v);
		} else {
			out[i].unk04 = (intptr_t)v;
		}
	}

	return out;
}

static struct noisesettings *cvNoise(u32 addr)
{
	if (!addr) {
		return NULL;
	}
	struct noisesettings *out = memoGet(addr);
	if (out) {
		return out;
	}
	out = newobj(sizeof(*out));
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);
	out->minradius = rdf32(addr);
	out->maxradius = rdf32(addr + 4);
	out->incradius = rdf32(addr + 8);
	out->decbasespeed = rdf32(addr + 0xc);
	out->decremspeed = rdf32(addr + 0x10);
	return out;
}

static struct recoilsettings *cvRecoil(u32 addr)
{
	if (!addr) {
		return NULL;
	}
	struct recoilsettings *out = memoGet(addr);
	if (out) {
		return out;
	}
	out = newobj(sizeof(*out));
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);
	out->xrange = rdf32(addr);
	out->yrange = rdf32(addr + 4);
	out->zrange = rdf32(addr + 8);
	out->unk0c = rdf32(addr + 0xc);
	out->unk10 = rd8(addr + 0x10);
	return out;
}

static f32 *cvVibration(u32 addr)
{
	if (!addr) {
		return NULL;
	}
	f32 *out = memoGet(addr);
	if (out) {
		return out;
	}
	out = newobj(sizeof(f32) * VIBRATION_LEN);
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);
	for (s32 i = 0; i < VIBRATION_LEN; ++i) {
		out[i] = rdf32(addr + i * 4);
	}
	return out;
}

static void cvFuncShoot(struct weaponfunc_shoot *f, u32 addr)
{
	f->recoilsettings = cvRecoil(rd32(addr + 0x14));
	f->recoverytime60 = (s8)rd8(addr + 0x18);
	f->damage = rdf32(addr + 0x1c);
	f->spread = rdf32(addr + 0x20);
	f->unk24 = (s8)rd8(addr + 0x24);
	f->unk25 = (s8)rd8(addr + 0x25);
	f->unk26 = (s8)rd8(addr + 0x26);
	f->unk27 = (s8)rd8(addr + 0x27);
	f->recoildist = rdf32(addr + 0x28);
	f->recoilangle = rdf32(addr + 0x2c);
	f->slidemax = rdf32(addr + 0x30);
	f->impactforce = rdf32(addr + 0x34);
	f->duration60 = rd8(addr + 0x38);
	f->shootsound = rd16(addr + 0x3a);
	f->penetration = rd8(addr + 0x3c);
}

static struct weaponfunc *cvFunc(u32 addr)
{
	if (!addr) {
		return NULL;
	}

	struct weaponfunc *out = memoGet(addr);
	if (out) {
		return out;
	}

	s32 type = (s32)rd32(addr);
	u32 size;

	switch (type) {
	case INVENTORYFUNCTYPE_SHOOT_SINGLE:     size = sizeof(struct weaponfunc_shootsingle); break;
	case INVENTORYFUNCTYPE_SHOOT_AUTOMATIC:  size = sizeof(struct weaponfunc_shootauto); break;
	case INVENTORYFUNCTYPE_SHOOT_PROJECTILE: size = sizeof(struct weaponfunc_shootprojectile); break;
	case INVENTORYFUNCTYPE_THROW:            size = sizeof(struct weaponfunc_throw); break;
	case INVENTORYFUNCTYPE_MELEE:            size = sizeof(struct weaponfunc_melee); break;
	case INVENTORYFUNCTYPE_SPECIAL:          size = sizeof(struct weaponfunc_special); break;
	case INVENTORYFUNCTYPE_DEVICE:           size = sizeof(struct weaponfunc_device); break;
	case INVENTORYFUNCTYPE_NONE:
		size = sizeof(struct weaponfunc);
		break;
	default:
		sysLogPrintf(LOG_WARNING, "moddata: weapon function at %08x has unknown type %04x", addr, type);
		size = sizeof(struct weaponfunc);
		break;
	}

	out = newobj(size);
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);

	out->type = type;
	out->name = rd16(addr + 4);
	out->unk06 = rd8(addr + 6);
	out->ammoindex = (s8)rd8(addr + 7);
	out->noisesettings = cvNoise(rd32(addr + 8));
	out->fire_animation = cvGuncmds(rd32(addr + 0xc));
	out->flags = rd32(addr + 0x10);

	switch (type) {
	case INVENTORYFUNCTYPE_SHOOT_SINGLE:
		cvFuncShoot((struct weaponfunc_shoot *)out, addr);
		break;
	case INVENTORYFUNCTYPE_SHOOT_AUTOMATIC: {
		struct weaponfunc_shootauto *f = (struct weaponfunc_shootauto *)out;
		cvFuncShoot(&f->base, addr);
		f->initialrpm = rdf32(addr + 0x40);
		f->maxrpm = rdf32(addr + 0x44);
		f->vibrationstart = cvVibration(rd32(addr + 0x48));
		f->vibrationmax = cvVibration(rd32(addr + 0x4c));
		f->turretaccel = (s8)rd8(addr + 0x50);
		f->turretdecel = (s8)rd8(addr + 0x51);
		break;
	}
	case INVENTORYFUNCTYPE_SHOOT_PROJECTILE: {
		struct weaponfunc_shootprojectile *f = (struct weaponfunc_shootprojectile *)out;
		cvFuncShoot(&f->base, addr);
		f->projectilemodelnum = (s32)rd32(addr + 0x40);
		f->unk44 = rd32(addr + 0x44);
		f->scale = rdf32(addr + 0x48);
		f->speed = (s32)rd32(addr + 0x4c);
		f->unk50 = rdf32(addr + 0x50);
		f->traveldist = (s32)rd32(addr + 0x54);
		f->timer60 = (s32)rd32(addr + 0x58);
		f->reflectangle = rdf32(addr + 0x5c);
		f->soundnum = (s16)rd16(addr + 0x60);
		break;
	}
	case INVENTORYFUNCTYPE_THROW: {
		struct weaponfunc_throw *f = (struct weaponfunc_throw *)out;
		f->projectilemodelnum = (s32)rd32(addr + 0x14);
		f->activatetime60 = (s16)rd16(addr + 0x18);
		f->recoverytime60 = (s32)rd32(addr + 0x1c);
		f->damage = rdf32(addr + 0x20);
		break;
	}
	case INVENTORYFUNCTYPE_MELEE: {
		struct weaponfunc_melee *f = (struct weaponfunc_melee *)out;
		f->damage = rdf32(addr + 0x14);
		f->range = rdf32(addr + 0x18);
		f->unk1c = rd32(addr + 0x1c);
		f->unk20 = rd32(addr + 0x20);
		f->unk24 = rd32(addr + 0x24);
		f->unk28 = rdf32(addr + 0x28);
		f->unk2c = rdf32(addr + 0x2c);
		f->unk30 = rdf32(addr + 0x30);
		f->unk34 = rdf32(addr + 0x34);
		f->unk38 = rdf32(addr + 0x38);
		f->unk3c = rdf32(addr + 0x3c);
		f->unk40 = rdf32(addr + 0x40);
		f->unk44 = rdf32(addr + 0x44);
		f->unk48 = rd32(addr + 0x48);
		break;
	}
	case INVENTORYFUNCTYPE_SPECIAL: {
		struct weaponfunc_special *f = (struct weaponfunc_special *)out;
		f->specialfunc = (s32)rd32(addr + 0x14);
		f->recoverytime60 = (s32)rd32(addr + 0x18);
		f->soundnum = rd16(addr + 0x1c);
		break;
	}
	case INVENTORYFUNCTYPE_DEVICE: {
		struct weaponfunc_device *f = (struct weaponfunc_device *)out;
		f->device = rd32(addr + 0x14);
		break;
	}
	}

	return out;
}

static struct inventory_ammo *cvAmmo(u32 addr)
{
	if (!addr) {
		return NULL;
	}
	struct inventory_ammo *out = memoGet(addr);
	if (out) {
		return out;
	}
	out = newobj(sizeof(*out));
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);
	out->type = rd32(addr);
	out->casingeject = rd32(addr + 4);
	out->clipsize = (s16)rd16(addr + 8);
	out->reload_animation = cvGuncmds(rd32(addr + 0xc));
	out->flags = rd8(addr + 0x10);
	return out;
}

static struct invaimsettings *cvAim(u32 addr)
{
	if (!addr) {
		return NULL;
	}
	struct invaimsettings *out = memoGet(addr);
	if (out) {
		return out;
	}
	out = newobj(sizeof(*out));
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);
	out->zoomfov = rdf32(addr);
	out->guntransup = rdf32(addr + 4);
	out->guntransdown = rdf32(addr + 8);
	out->guntransside = rdf32(addr + 0xc);
	out->aimdamppal = rdf32(addr + 0x10);
	out->aimdamp = rdf32(addr + 0x14);
	// the two bitfields sit at the top of the word on MIPS
	u32 bits = rd32(addr + 0x18);
	out->tracktype = bits >> 28;
	out->unk18_04 = (bits >> 24) & 0xf;
	out->flags = rd32(addr + 0x1c);
	return out;
}

static struct gunviscmd *cvGunvis(u32 addr)
{
	if (!addr) {
		return NULL;
	}
	struct gunviscmd *out = memoGet(addr);
	if (out) {
		return out;
	}

	s32 n = 0;
	while (n < GUNVISCMD_MAX_LEN && rd8(addr + n * N64_GUNVISCMD_SIZE) != GUNVISCMD_END) {
		++n;
	}

	out = newobj(sizeof(struct gunviscmd) * (n + 1));
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);

	for (s32 i = 0; i <= n; ++i) {
		u32 at = addr + i * N64_GUNVISCMD_SIZE;
		out[i].type = rd8(at);
		out[i].param = rd16(at + 2);
		out[i].op = rd8(at + 4);
		out[i].partnum = rd16(at + 6);
		out[i].unk08 = rd16(at + 8);
	}

	return out;
}

static struct modelpartvisibility *cvPartvis(u32 addr)
{
	if (!addr) {
		return NULL;
	}
	struct modelpartvisibility *out = memoGet(addr);
	if (out) {
		return out;
	}

	s32 n = 0;
	while (n < PARTVIS_MAX_LEN && rd8(addr + n * 2) != 255) {
		++n;
	}

	out = newobj(sizeof(struct modelpartvisibility) * (n + 1));
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);

	for (s32 i = 0; i <= n; ++i) {
		out[i].part = rd8(addr + i * 2);
		out[i].visible = rd8(addr + i * 2 + 1);
	}

	return out;
}

static struct weapon *cvWeapon(u32 addr)
{
	if (!addr) {
		return NULL;
	}
	struct weapon *out = memoGet(addr);
	if (out) {
		return out;
	}
	out = newobj(sizeof(*out));
	if (!out) {
		return NULL;
	}
	memoPut(addr, out);

	out->hi_model = modFileId(rd16(addr));
	out->lo_model = modFileId(rd16(addr + 2));
	out->equip_animation = cvGuncmds(rd32(addr + 4));
	out->unequip_animation = cvGuncmds(rd32(addr + 8));
	out->pritosec_animation = cvGuncmds(rd32(addr + 0xc));
	out->sectopri_animation = cvGuncmds(rd32(addr + 0x10));
	out->functions[0] = cvFunc(rd32(addr + 0x14));
	out->functions[1] = cvFunc(rd32(addr + 0x18));
	out->ammos[0] = cvAmmo(rd32(addr + 0x1c));
	out->ammos[1] = cvAmmo(rd32(addr + 0x20));
	out->aimsettings = cvAim(rd32(addr + 0x24));
	out->muzzlez = rdf32(addr + 0x28);
	out->posx = rdf32(addr + 0x2c);
	out->posy = rdf32(addr + 0x30);
	out->posz = rdf32(addr + 0x34);
	out->sway = rdf32(addr + 0x38);
	out->gunviscmds = cvGunvis(rd32(addr + 0x3c));
	out->partvisibility = cvPartvis(rd32(addr + 0x40));
	out->shortname = rd16(addr + 0x44);
	out->name = rd16(addr + 0x46);
	out->manufacturer = rd16(addr + 0x48);
	out->description = rd16(addr + 0x4a);
	out->flags = rd32(addr + 0x4c);
	// the port's own fields; inherited or configured, never read from the ROM
	out->flags2 = 0;
	out->unequippedreloadindex = 0;
	out->pickupsound = 0;

	return out;
}

/* ---- the tables --------------------------------------------------------- */

/**
 * The stock ROM's g_Weapons, read from the port's own copy of the stock data
 * segment at the same addresses, so a mod definition at a stock definition's
 * address can be paired with the port's struct for it.
 */
static u32 stockDefinitionAddr(const struct moddataspec *spec, s32 slot)
{
	u32 size = 0;
	const u8 *stock = romdataGetDataSeg(&size);
	u32 at = spec->weapons + slot * 4;

	if (!stock || at < spec->base || at - spec->base + 4 > size) {
		return 0;
	}

	const u8 *p = stock + (at - spec->base);
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/**
 * A converted definition against the port's own struct for the same stock
 * address, field by field. With the stock ROM's segment fed in, every field
 * must match; a mod's will differ where the mod edited it, which is the
 * trace of what it changed.
 */
#define CMPF(fmt, a, b, name) \
	if ((a) != (b)) { \
		sysLogPrintf(LOG_NOTE, "moddata: slot %d %s: " fmt " vs stock " fmt, slot, name, (a), (b)); \
		++diffs; \
	}

static s32 verifyFunc(s32 slot, s32 which, struct weaponfunc *a, struct weaponfunc *b)
{
	s32 diffs = 0;
	char name[32];

	if (!a || !b) {
		if (a != b) {
			sysLogPrintf(LOG_NOTE, "moddata: slot %d functions[%d]: %p vs stock %p", slot, which, a, b);
			++diffs;
		}
		return diffs;
	}

	snprintf(name, sizeof(name), "functions[%d].type", which);
	CMPF("%04x", a->type, b->type, name);
	snprintf(name, sizeof(name), "functions[%d].name", which);
	CMPF("%04x", a->name, b->name, name);
	snprintf(name, sizeof(name), "functions[%d].ammoindex", which);
	CMPF("%d", a->ammoindex, b->ammoindex, name);
	snprintf(name, sizeof(name), "functions[%d].flags", which);
	CMPF("%08x", a->flags, b->flags, name);

	CMPF("%d", !!a->noisesettings, !!b->noisesettings, "noisesettings set");
	CMPF("%d", !!a->fire_animation, !!b->fire_animation, "fire_animation set");
	if (a->noisesettings && b->noisesettings) {
		CMPF("%g", a->noisesettings->minradius, b->noisesettings->minradius, "noise.minradius");
		CMPF("%g", a->noisesettings->decremspeed, b->noisesettings->decremspeed, "noise.decremspeed");
	}

	if ((a->type & 0xff) == INVENTORYFUNCTYPE_SHOOT && a->type == b->type) {
		struct weaponfunc_shoot *x = (struct weaponfunc_shoot *)a;
		struct weaponfunc_shoot *y = (struct weaponfunc_shoot *)b;
		CMPF("%d", !!x->recoilsettings, !!y->recoilsettings, "recoilsettings set");
		if (x->recoilsettings && y->recoilsettings) {
			CMPF("%g", x->recoilsettings->xrange, y->recoilsettings->xrange, "recoil.xrange");
			CMPF("%d", x->recoilsettings->unk10, y->recoilsettings->unk10, "recoil.unk10");
		}
		CMPF("%g", x->recoilangle, y->recoilangle, "recoilangle");
		CMPF("%g", x->slidemax, y->slidemax, "slidemax");
		CMPF("%g", x->impactforce, y->impactforce, "impactforce");
		CMPF("%d", x->duration60, y->duration60, "duration60");
		CMPF("%d", x->penetration, y->penetration, "penetration");
		CMPF("%d", x->unk24, y->unk24, "unk24");
		if (a->type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
			struct weaponfunc_shootauto *p = (struct weaponfunc_shootauto *)a;
			struct weaponfunc_shootauto *q = (struct weaponfunc_shootauto *)b;
			CMPF("%g", p->initialrpm, q->initialrpm, "initialrpm");
			CMPF("%g", p->maxrpm, q->maxrpm, "maxrpm");
			CMPF("%d", !!p->vibrationstart, !!q->vibrationstart, "vibrationstart set");
			CMPF("%d", p->turretaccel, q->turretaccel, "turretaccel");
		}
		if (a->type == INVENTORYFUNCTYPE_SHOOT_PROJECTILE) {
			struct weaponfunc_shootprojectile *p = (struct weaponfunc_shootprojectile *)a;
			struct weaponfunc_shootprojectile *q = (struct weaponfunc_shootprojectile *)b;
			CMPF("%d", p->projectilemodelnum, q->projectilemodelnum, "projectilemodelnum");
			CMPF("%g", p->scale, q->scale, "proj.scale");
			CMPF("%d", p->speed, q->speed, "proj.speed");
			CMPF("%d", p->timer60, q->timer60, "proj.timer60");
			CMPF("%d", p->soundnum, q->soundnum, "proj.soundnum");
		}
	} else if ((a->type & 0xff) == INVENTORYFUNCTYPE_THROW && a->type == b->type) {
		struct weaponfunc_throw *p = (struct weaponfunc_throw *)a;
		struct weaponfunc_throw *q = (struct weaponfunc_throw *)b;
		CMPF("%d", p->projectilemodelnum, q->projectilemodelnum, "throw.projectilemodelnum");
		CMPF("%d", p->activatetime60, q->activatetime60, "throw.activatetime60");
		CMPF("%d", p->recoverytime60, q->recoverytime60, "throw.recoverytime60");
		CMPF("%g", p->damage, q->damage, "throw.damage");
	} else if ((a->type & 0xff) == INVENTORYFUNCTYPE_MELEE && a->type == b->type) {
		struct weaponfunc_melee *p = (struct weaponfunc_melee *)a;
		struct weaponfunc_melee *q = (struct weaponfunc_melee *)b;
		CMPF("%g", p->damage, q->damage, "melee.damage");
		CMPF("%g", p->range, q->range, "melee.range");
	} else if ((a->type & 0xff) == INVENTORYFUNCTYPE_SPECIAL && a->type == b->type) {
		struct weaponfunc_special *p = (struct weaponfunc_special *)a;
		struct weaponfunc_special *q = (struct weaponfunc_special *)b;
		CMPF("%d", p->specialfunc, q->specialfunc, "specialfunc");
		CMPF("%d", p->recoverytime60, q->recoverytime60, "special.recoverytime60");
	} else if ((a->type & 0xff) == INVENTORYFUNCTYPE_DEVICE && a->type == b->type) {
		CMPF("%08x", ((struct weaponfunc_device *)a)->device, ((struct weaponfunc_device *)b)->device, "device");
	}

	return diffs;
}

static s32 verifyWeapon(s32 slot, struct weapon *a, struct weapon *b)
{
	s32 diffs = 0;

	CMPF("%d", a->hi_model, b->hi_model, "hi_model");
	CMPF("%d", a->lo_model, b->lo_model, "lo_model");
	CMPF("%g", a->muzzlez, b->muzzlez, "muzzlez");
	CMPF("%g", a->posx, b->posx, "posx");
	CMPF("%g", a->posy, b->posy, "posy");
	CMPF("%g", a->posz, b->posz, "posz");
	CMPF("%g", a->sway, b->sway, "sway");
	CMPF("%04x", a->shortname, b->shortname, "shortname");
	CMPF("%04x", a->name, b->name, "name");
	CMPF("%04x", a->manufacturer, b->manufacturer, "manufacturer");
	CMPF("%04x", a->description, b->description, "description");
	CMPF("%08x", a->flags, b->flags, "flags");
	CMPF("%d", !!a->equip_animation, !!b->equip_animation, "equip_animation set");
	CMPF("%d", !!a->unequip_animation, !!b->unequip_animation, "unequip_animation set");
	CMPF("%d", !!a->gunviscmds, !!b->gunviscmds, "gunviscmds set");
	CMPF("%d", !!a->partvisibility, !!b->partvisibility, "partvisibility set");
	CMPF("%d", !!a->aimsettings, !!b->aimsettings, "aimsettings set");

	if (a->aimsettings && b->aimsettings) {
		CMPF("%g", a->aimsettings->zoomfov, b->aimsettings->zoomfov, "aimsettings.zoomfov");
		CMPF("%g", a->aimsettings->guntransup, b->aimsettings->guntransup, "aimsettings.guntransup");
		CMPF("%g", a->aimsettings->guntransdown, b->aimsettings->guntransdown, "aimsettings.guntransdown");
		CMPF("%g", a->aimsettings->guntransside, b->aimsettings->guntransside, "aimsettings.guntransside");
		CMPF("%g", a->aimsettings->aimdamppal, b->aimsettings->aimdamppal, "aimsettings.aimdamppal");
		CMPF("%g", a->aimsettings->aimdamp, b->aimsettings->aimdamp, "aimsettings.aimdamp");
		CMPF("%d", a->aimsettings->tracktype, b->aimsettings->tracktype, "aimsettings.tracktype");
		CMPF("%d", a->aimsettings->unk18_04, b->aimsettings->unk18_04, "aimsettings.unk18_04");
		CMPF("%08x", a->aimsettings->flags, b->aimsettings->flags, "aimsettings.flags");
	}
	CMPF("%d", !!a->pritosec_animation, !!b->pritosec_animation, "pritosec_animation set");
	CMPF("%d", !!a->sectopri_animation, !!b->sectopri_animation, "sectopri_animation set");

	for (s32 i = 0; i < 2; ++i) {
		diffs += verifyFunc(slot, i, a->functions[i], b->functions[i]);
		if (a->ammos[i] && b->ammos[i]) {
			CMPF("%d", a->ammos[i]->type, b->ammos[i]->type, "ammo.type");
			CMPF("%d", a->ammos[i]->clipsize, b->ammos[i]->clipsize, "ammo.clipsize");
			CMPF("%d", a->ammos[i]->casingeject, b->ammos[i]->casingeject, "ammo.casingeject");
			CMPF("%d", a->ammos[i]->flags, b->ammos[i]->flags, "ammo.flags");
		} else {
			CMPF("%d", !!a->ammos[i], !!b->ammos[i], "ammo set");
		}
	}

	if (a->equip_animation && b->equip_animation) {
		for (s32 i = 0; i < 8; ++i) {
			CMPF("%d", a->equip_animation[i].type, b->equip_animation[i].type, "equip_animation.type");
			CMPF("%d", a->equip_animation[i].unk02, b->equip_animation[i].unk02, "equip_animation.unk02");
			if (a->equip_animation[i].type != GUNCMD_INCLUDE && a->equip_animation[i].type != GUNCMD_RANDOM) {
				CMPF("%lx", (long)a->equip_animation[i].unk04, (long)b->equip_animation[i].unk04, "equip_animation.unk04");
			}
			if (a->equip_animation[i].type == GUNCMD_END || b->equip_animation[i].type == GUNCMD_END) {
				break;
			}
		}
	}

	if (a->gunviscmds && b->gunviscmds) {
		for (s32 i = 0; i < 32; ++i) {
			CMPF("%d", a->gunviscmds[i].type, b->gunviscmds[i].type, "gunviscmds.type");
			CMPF("%d", a->gunviscmds[i].param, b->gunviscmds[i].param, "gunviscmds.param");
			CMPF("%d", a->gunviscmds[i].op, b->gunviscmds[i].op, "gunviscmds.op");
			CMPF("%d", a->gunviscmds[i].partnum, b->gunviscmds[i].partnum, "gunviscmds.partnum");
			if (a->gunviscmds[i].type == 0 || b->gunviscmds[i].type == 0) {
				break;
			}
		}
	}

	if (a->partvisibility && b->partvisibility) {
		for (s32 i = 0; i < 32; ++i) {
			CMPF("%d", a->partvisibility[i].part, b->partvisibility[i].part, "partvisibility.part");
			CMPF("%d", a->partvisibility[i].visible, b->partvisibility[i].visible, "partvisibility.visible");
			if (a->partvisibility[i].part == 255 || b->partvisibility[i].part == 255) {
				break;
			}
		}
	}

	return diffs;
}

/**
 * The port edits some stock flags itself - a function flag moved out of a
 * weapon-number test, a device bit - and no ROM has those. Where the mod
 * kept a definition at its stock address, apply the port's delta over the
 * ROM's stock flags to the mod's, function by function too.
 */
static u32 stockRd32(const struct moddataspec *spec, u32 addr)
{
	u32 size = 0;
	const u8 *stock = romdataGetDataSeg(&size);

	if (!stock || addr < spec->base || addr - spec->base + 4 > size) {
		return 0;
	}

	const u8 *p = stock + (addr - spec->base);
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void carryPortFlags(struct weapon *w, u32 addr, const struct weapon *stock, const struct moddataspec *spec)
{
	u32 romflags = stockRd32(spec, addr + 0x4c);
	w->flags = (w->flags | (stock->flags & ~romflags)) & ~(romflags & ~stock->flags);

	for (s32 i = 0; i < 2; ++i) {
		u32 funcaddr = rd32(addr + 0x14 + i * 4);
		struct weaponfunc *f = w->functions[i];
		const struct weaponfunc *sf = stock->functions[i];

		if (f && sf && funcaddr && funcaddr == stockRd32(spec, addr + 0x14 + i * 4)) {
			u32 rom = stockRd32(spec, funcaddr + 0x10);
			f->flags = (f->flags | (sf->flags & ~rom)) & ~(rom & ~sf->flags);
		}
	}
}

static s32 importWeapons(const struct moddataspec *spec)
{
	s32 count = spec->numweapons;
	s32 inherited = 0;

	if (count > WEAPON_SUICIDEPILL + 1) {
		count = WEAPON_SUICIDEPILL + 1;
	}

	// which port definition sits at each stock address - taken before the
	// loop below starts replacing them, since slot 72 may inherit from what
	// slot 2 was
	u32 stockaddrs[WEAPON_SUICIDEPILL + 1];
	struct weapon *stockdefs[WEAPON_SUICIDEPILL + 1];
	for (s32 i = 0; i <= WEAPON_SUICIDEPILL; ++i) {
		stockaddrs[i] = stockDefinitionAddr(spec, i);
		stockdefs[i] = g_Weapons[i];
	}

	for (s32 i = 0; i < count; ++i) {
		u32 addr = rd32(spec->weapons + i * 4);
		struct weapon *w = cvWeapon(addr);

		if (!w) {
			continue;
		}

		if (w->flags2 == 0 && w->pickupsound == 0) {
			for (s32 k = 0; k <= WEAPON_SUICIDEPILL; ++k) {
				if (stockaddrs[k] == addr && stockdefs[k]) {
					if (modDataTrace) {
						verifyWeapon(i, w, stockdefs[k]);
					}
					w->flags2 = stockdefs[k]->flags2;
					w->unequippedreloadindex = stockdefs[k]->unequippedreloadindex;
					w->pickupsound = stockdefs[k]->pickupsound;
					carryPortFlags(w, addr, stockdefs[k], spec);
					if (w->flags2 || w->pickupsound) {
						++inherited;
					}
					break;
				}
			}
		}

		g_Weapons[i] = w;

		if (modDataTrace) {
			sysLogPrintf(LOG_NOTE, "moddata: slot %2d <- %08x model %d/%d (%s) name %04x funcs %04x/%04x ammo %d/%d flags %08x flags2 %08x",
					i, addr, w->hi_model, w->lo_model, romdataFileGetName(w->hi_model) ? romdataFileGetName(w->hi_model) : "?",
					w->name, w->functions[0] ? ((struct weaponfunc *)w->functions[0])->type : -1,
					w->functions[1] ? ((struct weaponfunc *)w->functions[1])->type : -1,
					w->ammos[0] ? (s32)w->ammos[0]->type : -1, w->ammos[1] ? (s32)w->ammos[1]->type : -1,
					w->flags, w->flags2);
		}
	}

	sysLogPrintf(LOG_NOTE, "moddata: %d weapon slots from %08x, %d objects, %d inherited port flags",
			count, spec->weapons, numObjects, inherited);

	return count;
}

static s32 importModelStates(const struct moddataspec *spec)
{
	s32 count = spec->nummodelstates;

	if (count > NUM_MODELS) {
		count = NUM_MODELS;
	}

	for (s32 i = 0; i < count; ++i) {
		u32 at = spec->modelstates + i * N64_MODELSTATE_SIZE;
		u16 fileid = modFileId(rd16(at + 4));
		u16 scale = rd16(at + 6);
		if (modDataTrace && (g_ModelStates[i].fileid != fileid || g_ModelStates[i].scale != scale)) {
			sysLogPrintf(LOG_NOTE, "moddata: model 0x%x: file %d scale %d vs port %d %d", i, fileid, scale,
					g_ModelStates[i].fileid, g_ModelStates[i].scale);
		}
		g_ModelStates[i].fileid = fileid;
		g_ModelStates[i].scale = scale;
	}

	return count;
}

/**
 * Does what is at this address read as a Combat Simulator weapon list? A mod
 * that moved the table leaves something else at the stock address, and
 * importing that would hand the match a weapon set of nonsense.
 */
static bool looksLikeMpWeapons(u32 addr, s32 count)
{
	for (s32 i = 0; i < count; ++i) {
		u32 at = addr + i * N64_MPWEAPON_SIZE;
		if (rd8(at) > WEAPON_SUICIDEPILL) {
			return false;
		}
		if ((s8)rd8(at + 1) > 0x21 || (s8)rd8(at + 3) > 0x21) {
			return false;
		}
		if ((s16)rd16(at + 6) < 0 || (s16)rd16(at + 6) >= NUM_MODELS) {
			return false;
		}
	}
	return true;
}

static bool looksLikeMpWeaponSets(u32 addr, s32 count)
{
	for (s32 i = 0; i < count; ++i) {
		u32 at = addr + i * N64_MPWEAPONSET_SIZE;
		if ((rd16(at) >> 9) > 0x40) {
			return false; // no such text bank
		}
		for (s32 j = 0; j < NUM_MPWEAPONSLOTS; ++j) {
			// weapon numbers, with the shield and "disabled" as sentinels past the guns
			if (rd8(at + 2 + j) > WEAPON_SUICIDEPILL) {
				return false;
			}
		}
	}
	return true;
}

/**
 * The port's list is not laid out like the ROM's. It is longer - the classic
 * guns and two scanners are added in the middle - and the shield and the
 * "disabled" entry sit at fixed indexes (MPWEAPON_SHIELD, MPWEAPON_DISABLED)
 * that mpconfigs.c and the setup menus name directly. So the mod's entries go
 * in order into the slots before the shield, its own shield and disabled
 * entries are skipped (the port's stay where they are), and whatever is left
 * before the shield is put behind a feature nothing unlocks, so the port's
 * extras do not show up naming the mod's guns.
 */
static s32 importMpWeapons(const struct moddataspec *spec)
{
	s32 count = spec->nummpweapons;
	s32 out = 0;

	if (!looksLikeMpWeapons(spec->mpweapons, count)) {
		sysLogPrintf(LOG_WARNING, "moddata: what is at %08x is not a Combat Simulator weapon list; the mod moved it, and the port keeps its own",
				spec->mpweapons);
		return 0;
	}

	for (s32 i = 0; i < count && out < MPWEAPON_SHIELD; ++i) {
		u32 at = spec->mpweapons + i * N64_MPWEAPON_SIZE;
		u8 weaponnum = rd8(at);

		if (weaponnum == WEAPON_MPSHIELD || weaponnum == WEAPON_DISABLED) {
			continue;
		}

		struct mpweapon *w = &g_MpWeapons[out];
		if (modDataTrace) {
			struct mpweapon old = *w;
			u8 bits = rd8(at + 5);
			if (old.weaponnum != weaponnum || old.priammotype != (s8)rd8(at + 1) || old.priammoqty != rd8(at + 2)
					|| old.secammotype != (s8)rd8(at + 3) || old.secammoqty != rd8(at + 4)
					|| old.hasweapon != (bits >> 7) || old.unlockfeature != (bits & 0x7f)
					|| old.model != (s16)rd16(at + 6) || old.extrascale != (s16)rd16(at + 8)) {
				sysLogPrintf(LOG_NOTE, "moddata: mpweapon %d differs from the port's: weapon %d/%d ammo %d/%d model %x/%x feature %d/%d",
						out, weaponnum, old.weaponnum, (s8)rd8(at + 1), old.priammotype, (s16)rd16(at + 6), old.model,
						bits & 0x7f, old.unlockfeature);
			}
		}
		w->weaponnum = weaponnum;
		w->priammotype = (s8)rd8(at + 1);
		w->priammoqty = rd8(at + 2);
		w->secammotype = (s8)rd8(at + 3);
		w->secammoqty = rd8(at + 4);
		// hasweapon is the top bit on MIPS, unlockfeature the low seven
		u8 bits = rd8(at + 5);
		w->hasweapon = bits >> 7;
		w->unlockfeature = bits & 0x7f;
		w->model = (s16)rd16(at + 6);
		w->extrascale = (s16)rd16(at + 8);

		if (modDataTrace) {
			sysLogPrintf(LOG_NOTE, "moddata: mpweapon %2d: weapon %d ammo %d x%d / %d x%d has %d feature %d model 0x%x (file %d %s) scale %d",
					out, w->weaponnum, w->priammotype, w->priammoqty, w->secammotype, w->secammoqty,
					w->hasweapon, w->unlockfeature, w->model,
					w->model >= 0 && w->model < NUM_MODELS ? g_ModelStates[w->model].fileid : -1,
					w->model >= 0 && w->model < NUM_MODELS && romdataFileGetName(g_ModelStates[w->model].fileid) ? romdataFileGetName(g_ModelStates[w->model].fileid) : "?",
					w->extrascale);
		}
		++out;
	}

	for (s32 i = out; i < MPWEAPON_SHIELD; ++i) {
		g_MpWeapons[i].weaponnum = WEAPON_NONE;
		g_MpWeapons[i].hasweapon = 0;
		g_MpWeapons[i].unlockfeature = MODDATA_FEATURE_NEVER;
	}

	return out;
}

static s32 importMpWeaponSets(const struct moddataspec *spec)
{
	s32 count = spec->nummpweaponsets;

	if (count > (s32)ARRAYCOUNT(g_MpWeaponSets)) {
		sysLogPrintf(LOG_WARNING, "moddata: the mod has %d weapon sets and the port's table holds %d; the rest are dropped",
				count, (s32)ARRAYCOUNT(g_MpWeaponSets));
		count = ARRAYCOUNT(g_MpWeaponSets);
	}

	if (!looksLikeMpWeaponSets(spec->mpweaponsets, count)) {
		sysLogPrintf(LOG_WARNING, "moddata: what is at %08x is not a weapon set table; the mod moved it, and the port keeps its own",
				spec->mpweaponsets);
		return 0;
	}

	for (s32 i = 0; i < count; ++i) {
		u32 at = spec->mpweaponsets + i * N64_MPWEAPONSET_SIZE;
		struct mpweaponset *s = &g_MpWeaponSets[i];
		if (modDataTrace) {
			bool same = s->name == rd16(at);
			for (s32 j = 0; j < NUM_MPWEAPONSLOTS; ++j) {
				same = same && s->slots[j] == rd8(at + 2 + j);
			}
			if (!same) {
				sysLogPrintf(LOG_NOTE, "moddata: weapon set %d differs from the port's: name %04x/%04x slots %d %d %d %d %d %d / %d %d %d %d %d %d",
						i, rd16(at), s->name, rd8(at + 2), rd8(at + 3), rd8(at + 4), rd8(at + 5), rd8(at + 6), rd8(at + 7),
						s->slots[0], s->slots[1], s->slots[2], s->slots[3], s->slots[4], s->slots[5]);
			}
		}
		s->name = rd16(at);
		for (s32 j = 0; j < NUM_MPWEAPONSLOTS; ++j) {
			s->slots[j] = rd8(at + 2 + j);
		}
		for (s32 j = 0; j < 4; ++j) {
			s->requirefeatures[j] = rd8(at + 8 + j);
		}
		s->unk0c = rd8(at + 0x0c);
		s->unk0d = rd8(at + 0x0d);
		s->unk0e = rd8(at + 0x0e);
		s->unk0f = rd8(at + 0x0f);
		s->unk10 = rd8(at + 0x10);
		s->unk11 = rd8(at + 0x11);
	}

	return count;
}

/* ---- entry -------------------------------------------------------------- */

/**
 * The arena list, whole: stage, unlock feature and name text id per entry, in
 * the mod's order. Every "solo stages in multi" mod rewrites it, most of them
 * in place over the sixteen stock arenas with the names taken from LoptionsE's
 * mission titles; the ones that add arenas move it. An entry that does not
 * read as one (CSMP rebuilt the struct as four bytes) leaves the whole list
 * alone - a list half-read is a menu of nonsense.
 */
/**
 * The stage table: which background, tiles, pads and setup each stage loads,
 * and its lighting and fog. A mod that puts its own levels into the stock
 * slots points them at its own files - GE-X's Runway sits in Extraction's
 * slot with a background of its own - and with the port's table it would load
 * the slot's stock background under the mod's setup, which is what crashed
 * setupCreateProps() there. Entries are matched by stage number, the file
 * ids go through the mod's names, and the port's own two fields at the end
 * (alarm, extragunmem) are kept.
 */
#define N64_STAGE_SIZE 0x38

static s32 modPlayerBody = -1;
static s32 modPlayerHead = -1;

/**
 * The suns an environment entry points at: numsuns of them, 20 bytes each in
 * the ROM, at a segment address. NULL when there are none.
 */
static struct sun *importSuns(u32 addr, s32 num)
{
	struct sun *suns;

	if (!addr || num <= 0 || num > 8) {
		return NULL;
	}

	suns = sysMemZeroAlloc(sizeof(struct sun) * num);

	if (!suns) {
		return NULL;
	}

	for (s32 i = 0; i < num; ++i) {
		const u32 at = addr + i * 0x14;
		suns[i].lens_flare = rd8(at + 0);
		suns[i].red = rd8(at + 1);
		suns[i].green = rd8(at + 2);
		suns[i].blue = rd8(at + 3);
		suns[i].pos[0] = rdf32(at + 4);
		suns[i].pos[1] = rdf32(at + 8);
		suns[i].pos[2] = rdf32(at + 12);
		suns[i].texture_size = (s16)rd16(at + 16);
		suns[i].orb_size = (s16)rd16(at + 18);
	}

	return suns;
}

/**
 * The sky, fog and clouds of each stage: env.c's two tables, rebuilt from
 * the segment in the port's layout (the ROM's differs by the suns pointer)
 * and handed to envChooseAndApply() in place of the port's. Each ends at a
 * zeroed entry, as the walk there expects.
 */
static s32 importEnvs(const struct moddataspec *spec)
{
	struct fogenvironment *fog = NULL;
	struct nofogenvironment *nofog = NULL;
	s32 nf = 0, nn = 0;

	if (spec->fogenvs && spec->numfogenvs > 0) {
		fog = sysMemZeroAlloc(sizeof(struct fogenvironment) * (spec->numfogenvs + 1));
		for (s32 i = 0; fog && i < spec->numfogenvs; ++i) {
			const u32 at = spec->fogenvs + i * 44;
			struct fogenvironment *e = &fog[i];
			e->stage = (s16)rd16(at + 0x00);
			e->near = (s16)rd16(at + 0x02);
			e->far = (s16)rd16(at + 0x04);
			e->opaperc = (s16)rd16(at + 0x06);
			e->xluperc = (s16)rd16(at + 0x08);
			e->refdist = (s16)rd16(at + 0x0a);
			e->fogmin = (s16)rd16(at + 0x0c);
			e->fogmax = (s16)rd16(at + 0x0e);
			e->sky_r = rd8(at + 0x10);
			e->sky_g = rd8(at + 0x11);
			e->sky_b = rd8(at + 0x12);
			e->numsuns = rd8(at + 0x13);
			e->suns = importSuns(rd32(at + 0x14), e->numsuns);
			if (!e->suns) {
				e->numsuns = 0;
			}
			e->clouds_enabled = rd8(at + 0x18);
			e->clouds_scale = (s16)rd16(at + 0x1a);
			e->clouds_type = rd8(at + 0x1c);
			e->clouds_r = rd8(at + 0x1d);
			e->clouds_g = rd8(at + 0x1e);
			e->clouds_b = rd8(at + 0x1f);
			e->water_enabled = rd8(at + 0x20);
			e->water_scale = (s16)rd16(at + 0x22);
			e->water_type = rd8(at + 0x24);
			e->water_r = rd8(at + 0x25);
			e->water_g = rd8(at + 0x26);
			e->water_b = rd8(at + 0x27);
			e->clouds_height = rd8(at + 0x28);
			++nf;
			if (modDataTrace) {
				sysLogPrintf(LOG_NOTE, "moddata: fog env %d: stage %d sky %02x%02x%02x fog %d-%d clouds %d suns %d",
						i, e->stage, e->sky_r, e->sky_g, e->sky_b, e->fogmin, e->fogmax, e->clouds_enabled, e->numsuns);
			}
		}
	}

	if (spec->nofogenvs && spec->numnofogenvs > 0) {
		nofog = sysMemZeroAlloc(sizeof(struct nofogenvironment) * (spec->numnofogenvs + 1));
		for (s32 i = 0; nofog && i < spec->numnofogenvs; ++i) {
			const u32 at = spec->nofogenvs + i * 56;
			struct nofogenvironment *e = &nofog[i];
			e->stage = (s32)rd32(at + 0x00);
			e->near = (s16)rd16(at + 0x04);
			e->far = (s16)rd16(at + 0x06);
			e->opaperc = (s16)rd16(at + 0x08);
			e->xluperc = (s16)rd16(at + 0x0a);
			e->refdist = (s16)rd16(at + 0x0c);
			e->sky_r = rd8(at + 0x0e);
			e->sky_g = rd8(at + 0x0f);
			e->sky_b = rd8(at + 0x10);
			e->numsuns = rd8(at + 0x11);
			e->suns = importSuns(rd32(at + 0x14), e->numsuns);
			if (!e->suns) {
				e->numsuns = 0;
			}
			e->clouds_enabled = rd8(at + 0x18);
			e->clouds_r = rd8(at + 0x19);
			e->clouds_g = rd8(at + 0x1a);
			e->clouds_b = rd8(at + 0x1b);
			e->clouds_scale = rdf32(at + 0x1c);
			e->clouds_type = (s16)rd16(at + 0x20);
			e->water_enabled = rd8(at + 0x22);
			e->water_r = rd8(at + 0x23);
			e->water_g = rd8(at + 0x24);
			e->water_b = rd8(at + 0x25);
			e->water_scale = rdf32(at + 0x28);
			e->water_type = (s16)rd16(at + 0x2c);
			e->clouds_height = rdf32(at + 0x30);
			e->transparency = rd8(at + 0x34);
			++nn;
			if (modDataTrace) {
				sysLogPrintf(LOG_NOTE, "moddata: no-fog env %d: stage %d sky %02x%02x%02x clouds %d suns %d",
						i, e->stage, e->sky_r, e->sky_g, e->sky_b, e->clouds_enabled, e->numsuns);
			}
		}
	}

	envSetTables(fog, nofog);
	return nf + nn;
}

static s32 importStages(const struct moddataspec *spec)
{
	s32 count = spec->numstages;
	s32 n = 0;

	for (s32 i = 0; i < count; ++i) {
		const u32 at = spec->stages + i * N64_STAGE_SIZE;
		const s32 stagenum = (s16)rd16(at);
		const s32 index = stageGetIndex(stagenum);
		struct stagetableentry *e;

		if (index < 0) {
			if (modDataTrace) {
				sysLogPrintf(LOG_NOTE, "moddata: stage %#x is not one the port has; skipped", stagenum);
			}
			continue;
		}

		e = &g_Stages[index];
		e->light_type = rd8(at + 2);
		e->light_alpha = rd8(at + 3);
		e->light_width = rd8(at + 4);
		e->light_height = rd8(at + 5);
		e->unk06 = rd16(at + 6);
		e->bgfileid = modFileId(rd16(at + 8));
		e->tilefileid = modFileId(rd16(at + 0xa));
		e->padsfileid = modFileId(rd16(at + 0xc));
		e->setupfileid = modFileId(rd16(at + 0xe));
		e->mpsetupfileid = modFileId(rd16(at + 0x10));
		e->unk14 = rdf32(at + 0x14);
		e->unk18 = rdf32(at + 0x18);
		e->unk1c = rdf32(at + 0x1c);
		e->unk20 = rd16(at + 0x20);
		e->unk22 = rd8(at + 0x22);
		e->unk23 = (s8)rd8(at + 0x23);
		e->unk24 = rd32(at + 0x24);
		e->unk28 = rd32(at + 0x28);
		e->unk2c = (s16)rd16(at + 0x2c);
		e->eraserpropdist = (s16)rd16(at + 0x2e);
		e->unk30 = (s16)rd16(at + 0x30);
		e->unk34 = rdf32(at + 0x34);
		++n;

		if (modDataTrace) {
			sysLogPrintf(LOG_NOTE, "moddata: stage %#x: bg %d (%s) tiles %d pads %d setup %d mpsetup %d", stagenum,
					e->bgfileid, romdataFileGetName(e->bgfileid) ? romdataFileGetName(e->bgfileid) : "?",
					e->tilefileid, e->padsfileid, e->setupfileid, e->mpsetupfileid);
		}
	}

	return n;
}

/**
 * The AI command length table. A mod's own code adds commands in slots this
 * game has no handler for (GE-X: 0xe6 and 0xe7, three bytes each, one byte
 * in the stock table), and the port can at least step its lists over them
 * once it knows how long they are. Only slots without a handler are taken:
 * the port's own commands read their operands at fixed places.
 */
static s32 importCommandLengths(const struct moddataspec *spec)
{
	extern s32 chraiSetModCommandLength(s32 type, u16 len);
	s32 n = 0;

	for (s32 i = 0; i < spec->numcommandlengths; ++i) {
		const u16 len = rd16(spec->commandlengths + i * 2);

		if (len && len < 256 && chraiSetModCommandLength(i, len)) {
			++n;
			if (modDataTrace) {
				sysLogPrintf(LOG_NOTE, "moddata: AI command %#x is the mod's own, %d bytes", i, len);
			}
		}
	}

	return n;
}

/**
 * The mission list: the stage each solo slot loads and its three title ids.
 * GE-X keeps the stock order but sends six missions to stage ids the stock
 * menu never lists (0x24, 0x25, 0x23, 0x2b, 0x2e, 0x1a), each with its own
 * entry in the stage table; through the stock list "Aztec" loaded stage 0x09,
 * where GE-X's files for that slot are a leftover pair that do not belong
 * together, and the level fell over on a pad its pads file does not have.
 */
static s32 importSoloStages(const struct moddataspec *spec)
{
	extern struct solostage g_SoloStages[NUM_SOLOSTAGES];
	s32 count = spec->numsolostages;
	s32 n = 0;

	if (count > NUM_SOLOSTAGES) {
		sysLogPrintf(LOG_WARNING, "moddata: %d solo missions is more than the port's list (%d); the rest are dropped", count, NUM_SOLOSTAGES);
		count = NUM_SOLOSTAGES;
	}

	for (s32 i = 0; i < count; ++i) {
		const u32 at = spec->solostages + i * 12;
		const u32 stagenum = rd32(at);

		if (!(stagenum > 0 && stagenum < 0x60)) {
			sysLogPrintf(LOG_WARNING, "moddata: solo mission %d names stage %#x; the list stops there", i, stagenum);
			break;
		}

		g_SoloStages[i].stagenum = stagenum;
		g_SoloStages[i].unk04 = rd8(at + 4);
		g_SoloStages[i].name1 = rd16(at + 6);
		g_SoloStages[i].name2 = rd16(at + 8);
		g_SoloStages[i].name3 = rd16(at + 10);
		++n;

		if (modDataTrace) {
			sysLogPrintf(LOG_NOTE, "moddata: solo mission %d: stage %#x names %#x %#x %#x", i, stagenum,
					g_SoloStages[i].name1, g_SoloStages[i].name2, g_SoloStages[i].name3);
		}
	}

	return n;
}

static s32 modNumPlayerConsts;
static u16 modPlayerConsts[64][2];

// The outfit chooser's constant as the mod's code has it, or def
static s32 modPlayerConst(s32 def)
{
	for (s32 i = 0; i < modNumPlayerConsts; ++i) {
		if (modPlayerConsts[i][0] == (u16)def) {
			return modPlayerConsts[i][1];
		}
	}
	return def;
}

static s32 modNumTexConsts;
static u16 modTexConsts[16][2];

// The animated texture number texLoadFromGdl() tests for, as the mod's code
// has it. Only ever called when a mod loaded; the stock number otherwise.
s32 modDataTexNum(s32 def)
{
	for (s32 i = 0; i < modNumTexConsts; ++i) {
		if (modTexConsts[i][0] == (u16)def) {
			return modTexConsts[i][1];
		}
	}
	return def;
}

static s32 modNumRoomNums;
static u16 modRoomNums[16][2];
static s32 modNumRoomStages;
static u16 modRoomStages[16][2];

// A pinned room's number as the mod's room code has it
s32 modDataRoomNum(s32 def)
{
	for (s32 i = 0; i < modNumRoomNums; ++i) {
		if (modRoomNums[i][0] == (u16)def) {
			return modRoomNums[i][1];
		}
	}
	return def;
}

// The stage id a pinned room's site compares against, for the stock stage
// index that site reads; defid is what the port's own table says there
s32 modDataRoomStage(s32 stockindex, s32 defid)
{
	for (s32 i = 0; i < modNumRoomStages; ++i) {
		if (modRoomStages[i][0] == (u16)stockindex) {
			return modRoomStages[i][1];
		}
	}
	return defid;
}

// The default outfit's body and head come through the map like the rest;
// playerbody/playerhead in the block are the older form of the same fact
s32 modDataPlayerBody(s32 def)
{
	const s32 mapped = modPlayerConst(def);
	return mapped != def ? mapped : (def == 0x56 && modPlayerBody >= 0 ? modPlayerBody : def);
}

s32 modDataPlayerHead(s32 def)
{
	const s32 mapped = modPlayerConst(def);
	return mapped != def ? mapped : (def == 0x04 && modPlayerHead >= 0 ? modPlayerHead : def);
}

static s32 importMpArenas(const struct moddataspec *spec)
{
	struct mparena arenas[ARRAYCOUNT(g_MpArenas)];
	s32 count = spec->nummparenas;

	if (count > (s32)ARRAYCOUNT(arenas)) {
		sysLogPrintf(LOG_WARNING, "moddata: %d arenas is more than the port's table holds (%d); the last are dropped",
				count, (s32)ARRAYCOUNT(arenas));
		count = ARRAYCOUNT(arenas);
	}

	for (s32 i = 0; i < count; ++i) {
		u32 at = spec->mparenas + i * N64_MPARENA_SIZE;
		s16 stagenum = (s16)rd16(at);
		u8 feature = rd8(at + 2);
		u16 name = rd16(at + 4);

		// 1 is "Random"; the rest must be stages, in a text bank that exists
		if (stagenum < 1 || stagenum > STAGE_4MBMENU || feature > 0x7f || name == 0 || (name >> 9) >= 69) {
			sysLogPrintf(LOG_WARNING, "moddata: what is at %08x is not an arena list (entry %d: stage %x feature %d name %x); the port keeps its own",
					spec->mparenas, i, stagenum, feature, name);
			return 0;
		}

		if (stagenum != 1 && stageGetIndex(stagenum) < 0) {
			sysLogPrintf(LOG_WARNING, "moddata: arena %d is stage %x, which the port has no stage table entry for", i, stagenum);
		}

		arenas[i].stagenum = stagenum;
		arenas[i].requirefeature = feature;
		arenas[i].name = name;

		if (modDataTrace) {
			sysLogPrintf(LOG_NOTE, "moddata: arena %2d: stage 0x%02x feature %d name 0x%04x (bank %d index %d)",
					i, stagenum, feature, name, name >> 9, name & 0x1ff);
		}
	}

	return mpImportArenas(arenas, count);
}

/**
 * g_HeadsAndBodies, index for index. A mod's setup files, its Combat Simulator
 * lists and its bot profiles all name characters by index into this table,
 * and a mod with its own roster (Mario Characters: 148 of 151 entries) rewrites
 * the table in place - the same file numbers, but the files at them are now
 * other characters, some of them bodies where the port has heads. The file
 * numbers go through the mod's name table like the model states do; the flags
 * are a MIPS bitfield, allocated from the top of the halfword.
 */
static s32 importHeadsAndBodies(const struct moddataspec *spec)
{
	s32 count = spec->numheadsandbodies;
	s32 out = 0;

	// the last entry is the terminator bodyreset.c walks to
	if (count > (s32)ARRAYCOUNT(g_HeadsAndBodies) - 1) {
		sysLogPrintf(LOG_WARNING, "moddata: %d heads and bodies is more than the port's table holds (%d); the last are dropped",
				count, (s32)ARRAYCOUNT(g_HeadsAndBodies) - 1);
		count = ARRAYCOUNT(g_HeadsAndBodies) - 1;
	}

	for (s32 i = 0; i < count; ++i) {
		u32 at = spec->headsandbodies + i * N64_HEADORBODY_SIZE;
		u16 bits = rd16(at);
		u16 modfile = rd16(at + 2);
		f32 scale = rdf32(at + 4);
		f32 animscale = rdf32(at + 8);
		u16 modhand = rd16(at + 16);
		struct headorbody *e = &g_HeadsAndBodies[i];

		if (modfile == 0 || !(scale > 0.01f && scale < 100.0f) || !(animscale > 0.01f && animscale < 100.0f)) {
			sysLogPrintf(LOG_WARNING, "moddata: what is at %08x is not the heads and bodies table (entry %d: file %d scale %g/%g); the port keeps its own from here",
					spec->headsandbodies, i, modfile, scale, animscale);
			break;
		}

		u16 fileid = modFileId(modfile);
		u16 handfileid = modFileId(modhand);
		if (!fileid) {
			sysLogPrintf(LOG_WARNING, "moddata: head/body %d: the mod's file %d has no port file; keeping the port's entry", i, modfile);
			continue;
		}

		if (modDataTrace && (e->filenum != fileid || e->handfilenum != handfileid || e->height != ((bits >> 2) & 0xff)
					|| e->ismale != (bits >> 15) || e->type != ((bits >> 10) & 7))) {
			sysLogPrintf(LOG_NOTE, "moddata: head/body %3d: file %4d (%s) hand %d male %d type %d height %3d scale %g/%g; port had file %d (%s)",
					i, fileid, romdataFileGetName(fileid) ? romdataFileGetName(fileid) : "?", handfileid,
					bits >> 15, (bits >> 10) & 7, (bits >> 2) & 0xff, scale, animscale,
					e->filenum, romdataFileGetName(e->filenum) ? romdataFileGetName(e->filenum) : "?");
		}

		e->ismale = bits >> 15;
		e->unk00_01 = (bits >> 14) & 1;
		e->canvaryheight = (bits >> 13) & 1;
		e->type = (bits >> 10) & 7;
		e->height = (bits >> 2) & 0xff;
		e->filenum = fileid;
		e->scale = scale;
		e->animscale = animscale;
		e->modeldef = NULL;
		e->handfilenum = handfileid;
		++out;
	}

	return out;
}

/**
 * The Combat Simulator's head list: entries of a g_HeadsAndBodies index and an
 * unlock feature. Fills the port's fixed array and sets the count the getters
 * use, so a shorter list does not leave the port's tail showing.
 */
static s32 importMpHeads(u32 addr, s32 count, struct mphead *dst, s32 room, s32 *outcount, const char *what)
{
	if (count > room) {
		sysLogPrintf(LOG_WARNING, "moddata: %d %s is more than the port holds (%d); the last are dropped", count, what, room);
		count = room;
	}

	for (s32 i = 0; i < count; ++i) {
		u32 at = addr + i * N64_MPHEAD_SIZE;
		s16 headnum = (s16)rd16(at);
		u8 feature = rd8(at + 2);

		if (headnum < 0 || headnum >= (s32)ARRAYCOUNT(g_HeadsAndBodies) - 1 || feature > 0x7f) {
			sysLogPrintf(LOG_WARNING, "moddata: what is at %08x is not a %s list (entry %d: head %d feature %d); the port keeps its own",
					addr, what, i, headnum, feature);
			return 0;
		}

		if (modDataTrace) {
			sysLogPrintf(LOG_NOTE, "moddata: %s %2d: head %3d (%s) feature %d", what, i, headnum,
					romdataFileGetName(g_HeadsAndBodies[headnum].filenum) ? romdataFileGetName(g_HeadsAndBodies[headnum].filenum) : "?", feature);
		}

		dst[i].headnum = headnum;
		dst[i].requirefeature = feature;
	}

	*outcount = count;
	return count;
}

/**
 * The Combat Simulator's body list: body index, name text id, the head that
 * goes with it, unlock feature.
 */
static s32 importMpBodies(const struct moddataspec *spec)
{
	s32 count = spec->nummpbodies;

	if (count > (s32)ARRAYCOUNT(g_MpBodies)) {
		sysLogPrintf(LOG_WARNING, "moddata: %d Combat Simulator bodies is more than the port holds (%d); the last are dropped",
				count, (s32)ARRAYCOUNT(g_MpBodies));
		count = ARRAYCOUNT(g_MpBodies);
	}

	for (s32 i = 0; i < count; ++i) {
		u32 at = spec->mpbodies + i * N64_MPBODY_SIZE;
		s16 bodynum = (s16)rd16(at);
		s16 name = (s16)rd16(at + 2);
		s16 headnum = (s16)rd16(at + 4);
		u8 feature = rd8(at + 6);

		if (bodynum < 0 || bodynum >= (s32)ARRAYCOUNT(g_HeadsAndBodies) - 1
				|| (headnum != 1000 && (headnum < -1 || headnum >= (s32)ARRAYCOUNT(g_HeadsAndBodies) - 1)) // 1000: the body has its own head
				|| feature > 0x7f || ((u16)name >> 9) >= 69) {
			sysLogPrintf(LOG_WARNING, "moddata: what is at %08x is not the Combat Simulator body list (entry %d: body %d head %d name %x); the port keeps its own",
					spec->mpbodies, i, bodynum, headnum, name);
			return 0;
		}

		if (modDataTrace) {
			sysLogPrintf(LOG_NOTE, "moddata: mpbody %2d: body %3d (%s) head %3d name 0x%04x feature %d", i, bodynum,
					romdataFileGetName(g_HeadsAndBodies[bodynum].filenum) ? romdataFileGetName(g_HeadsAndBodies[bodynum].filenum) : "?",
					headnum, (u16)name, feature);
		}

		g_MpBodies[i].bodynum = bodynum;
		g_MpBodies[i].name = name;
		g_MpBodies[i].headnum = headnum;
		g_MpBodies[i].requirefeature = feature;
	}

	g_MpListCounts.bodies = count;
	return count;
}

/**
 * A plain list of indexes: g_BotHeads (into g_MpHeads), g_MpMaleHeads and
 * g_MpFemaleHeads (into g_HeadsAndBodies).
 */
static s32 importHeadList(u32 addr, s32 count, u32 *dst, s32 room, s32 limit, s32 *outcount, const char *what)
{
	if (count > room) {
		sysLogPrintf(LOG_WARNING, "moddata: %d %s is more than the port holds (%d); the last are dropped", count, what, room);
		count = room;
	}

	for (s32 i = 0; i < count; ++i) {
		u32 v = rd32(addr + i * 4);
		if (v >= (u32)limit) {
			sysLogPrintf(LOG_WARNING, "moddata: what is at %08x is not the %s list (entry %d is %u); the port keeps its own", addr, what, i, v);
			return 0;
		}
		dst[i] = v;
	}

	*outcount = count;
	return count;
}

/**
 * The heads a solo stage hands its guards at random: g_MaleGuardHeads and
 * the team and female lists beside it (body.c), each -1 terminated in an
 * array with one slot more than the stock list, and counted again by
 * bodiesInit(). Everything is read before anything is written, so a bad
 * list leaves the port's own in place.
 */
static s32 importGuardHeads(u32 addr, s32 count, s32 *dst, s32 room, s32 *num, const char *what)
{
	const s32 limit = ARRAYCOUNT(g_HeadsAndBodies) - 1;

	if (count > room - 1) {
		sysLogPrintf(LOG_WARNING, "moddata: %d %s is more than the port holds (%d); the last are dropped", count, what, room - 1);
		count = room - 1;
	}

	for (s32 i = 0; i < count; ++i) {
		const u32 v = rd32(addr + i * 4);
		if (v >= (u32)limit) {
			sysLogPrintf(LOG_WARNING, "moddata: what is at %08x is not the %s list (entry %d is %u); the port keeps its own", addr, what, i, v);
			return 0;
		}
	}

	for (s32 i = 0; i < count; ++i) {
		dst[i] = (s32)rd32(addr + i * 4);
	}

	dst[count] = -1;
	*num = count;
	return count;
}

// The arrays in body.c, terminator included
#define GUARDHEADS_MALE_ROOM        43
#define GUARDHEADS_MALETEAM_ROOM    17
#define GUARDHEADS_FEMALE_ROOM      5
#define GUARDHEADS_FEMALETEAM_ROOM  5

static bool loadNames(const char *path)
{
	u32 len = 0;
	char *loaded = fsFileLoad(path, &len);

	if (!loaded) {
		sysLogPrintf(LOG_WARNING, "moddata: no file name list at %s; file ids are taken as the port's", path);
		return false;
	}

	// with a terminator, which the file does not promise
	char *text = sysMemAlloc(len + 1);
	if (!text) {
		return false;
	}
	memcpy(text, loaded, len);
	text[len] = '\0';
	sysMemFree(loaded);

	// one name per line, in file id order; index 0 is the unused slot
	s32 lines = 1;
	for (u32 i = 0; i < len; ++i) {
		if (text[i] == '\n') {
			++lines;
		}
	}

	seg.names = sysMemZeroAlloc(sizeof(char *) * lines);
	seg.fileids = sysMemAlloc(sizeof(s32) * lines);
	if (!seg.names || !seg.fileids) {
		seg.names = NULL;
		return false;
	}

	seg.numnames = 0;
	char *p = text;
	while (seg.numnames < lines && p < text + len) {
		char *end = memchr(p, '\n', (text + len) - p);
		if (end) {
			*end = '\0';
		}
		seg.names[seg.numnames] = p;
		seg.fileids[seg.numnames] = -2;
		++seg.numnames;
		if (!end) {
			break;
		}
		p = end + 1;
	}

	return true;
}

s32 modDataImport(const struct moddataspec *spec)
{
	if (seg.data) {
		sysLogPrintf(LOG_ERROR, "moddata: a data segment was already imported; restart to load another");
		return false;
	}

	u32 len = 0;
	u8 *data = fsFileLoad(spec->file, &len);
	if (!data || !len) {
		sysLogPrintf(LOG_ERROR, "moddata: could not load %s", spec->file);
		return false;
	}

	modDataTrace = sysArgCheck("--moddata-trace");

	memset(&seg, 0, sizeof(seg));
	seg.data = data;
	seg.len = len;
	seg.base = spec->base;

	if (spec->names[0]) {
		loadNames(spec->names);
	}

	sysLogPrintf(LOG_NOTE, "moddata: %s is %u bytes at %08x", spec->file, len, spec->base);

	if (spec->modelstates && spec->nummodelstates > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d model states from %08x",
				importModelStates(spec), spec->modelstates);
	}

	if (spec->weapons && spec->numweapons > 0) {
		importWeapons(spec);
	}

	if (spec->mpweapons && spec->nummpweapons > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d Combat Simulator weapons from %08x",
				importMpWeapons(spec), spec->mpweapons);
	}

	if (spec->mpweaponsets && spec->nummpweaponsets > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d weapon sets from %08x",
				importMpWeaponSets(spec), spec->mpweaponsets);
	}

	if (spec->mparenas && spec->nummparenas > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d arenas from %08x",
				importMpArenas(spec), spec->mparenas);
	}

	if (spec->headsandbodies && spec->numheadsandbodies > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d heads and bodies from %08x",
				importHeadsAndBodies(spec), spec->headsandbodies);
	}

	if (spec->stages && spec->numstages > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d stages from %08x", importStages(spec), spec->stages);
	}

	if ((spec->fogenvs && spec->numfogenvs > 0) || (spec->nofogenvs && spec->numnofogenvs > 0)) {
		sysLogPrintf(LOG_NOTE, "moddata: %d stage environments (sky, fog, clouds) from %08x and %08x",
				importEnvs(spec), spec->fogenvs, spec->nofogenvs);
	}

	if (spec->solostages && spec->numsolostages > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d solo missions from %08x", importSoloStages(spec), spec->solostages);
	}

	if (spec->commandlengths && spec->numcommandlengths > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d AI command lengths of the mod's own from %08x",
				importCommandLengths(spec), spec->commandlengths);
	}

	modPlayerBody = spec->playerbody;
	modPlayerHead = spec->playerhead;
	modNumPlayerConsts = spec->numplayerconsts;
	memcpy(modPlayerConsts, spec->playerconsts, sizeof(modPlayerConsts));

	if (modNumPlayerConsts) {
		sysLogPrintf(LOG_NOTE, "moddata: %d of the outfit chooser's body/head constants are the mod's", modNumPlayerConsts);
	}

	modNumTexConsts = spec->numtexconsts;
	memcpy(modTexConsts, spec->texconsts, sizeof(modTexConsts));

	if (modNumTexConsts) {
		sysLogPrintf(LOG_NOTE, "moddata: %d of the animated texture numbers are the mod's", modNumTexConsts);
	}

	modNumRoomNums = spec->numroomnums;
	memcpy(modRoomNums, spec->roomnums, sizeof(modRoomNums));
	modNumRoomStages = spec->numroomstages;
	memcpy(modRoomStages, spec->roomstages, sizeof(modRoomStages));

	if (modNumRoomNums || modNumRoomStages) {
		sysLogPrintf(LOG_NOTE, "moddata: the camera-pinned rooms are the mod's (%d stages, %d room numbers changed)", modNumRoomStages, modNumRoomNums);
	}

	if (modPlayerBody >= 0 || modPlayerHead >= 0) {
		sysLogPrintf(LOG_NOTE, "moddata: the solo player is body %d head %d", modPlayerBody, modPlayerHead);
	}

	// the lists index the table, so they come after it
	if (spec->mpheads && spec->nummpheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d Combat Simulator heads from %08x",
				importMpHeads(spec->mpheads, spec->nummpheads, g_MpHeads, ARRAYCOUNT(g_MpHeads), &g_MpListCounts.heads, "mphead"), spec->mpheads);
	}

	if (spec->mpbodies && spec->nummpbodies > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d Combat Simulator bodies from %08x",
				importMpBodies(spec), spec->mpbodies);
	}

	if (spec->mpbeauheads && spec->nummpbeauheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d Beau heads from %08x",
				importMpHeads(spec->mpbeauheads, spec->nummpbeauheads, g_MpBeauHeads, ARRAYCOUNT(g_MpBeauHeads), &g_MpListCounts.beauheads, "beau head"), spec->mpbeauheads);
	}

	if (spec->botheads && spec->numbotheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d bot heads from %08x",
				importHeadList(spec->botheads, spec->numbotheads, g_BotHeads, ARRAYCOUNT(g_BotHeads), g_MpListCounts.heads, &g_MpListCounts.botheads, "bot head"), spec->botheads);
	}

	if (spec->mpmaleheads && spec->nummpmaleheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d male heads from %08x",
				importHeadList(spec->mpmaleheads, spec->nummpmaleheads, g_MpMaleHeads, ARRAYCOUNT(g_MpMaleHeads), ARRAYCOUNT(g_HeadsAndBodies) - 1, &g_MpListCounts.maleheads, "male head"), spec->mpmaleheads);
	}

	if (spec->mpfemaleheads && spec->nummpfemaleheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d female heads from %08x",
				importHeadList(spec->mpfemaleheads, spec->nummpfemaleheads, g_MpFemaleHeads, ARRAYCOUNT(g_MpFemaleHeads), ARRAYCOUNT(g_HeadsAndBodies) - 1, &g_MpListCounts.femaleheads, "female head"), spec->mpfemaleheads);
	}

	if (spec->maleguardheads && spec->nummaleguardheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d guard heads from %08x",
				importGuardHeads(spec->maleguardheads, spec->nummaleguardheads, g_MaleGuardHeads, GUARDHEADS_MALE_ROOM, &g_NumMaleGuardHeads, "guard head"), spec->maleguardheads);
	}

	if (spec->maleguardteamheads && spec->nummaleguardteamheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d guard team heads from %08x",
				importGuardHeads(spec->maleguardteamheads, spec->nummaleguardteamheads, g_MaleGuardTeamHeads, GUARDHEADS_MALETEAM_ROOM, &g_NumMaleGuardTeamHeads, "guard team head"), spec->maleguardteamheads);
	}

	if (spec->femaleguardheads && spec->numfemaleguardheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d female guard heads from %08x",
				importGuardHeads(spec->femaleguardheads, spec->numfemaleguardheads, g_FemaleGuardHeads, GUARDHEADS_FEMALE_ROOM, &g_NumFemaleGuardHeads, "female guard head"), spec->femaleguardheads);
	}

	if (spec->femaleguardteamheads && spec->numfemaleguardteamheads > 0) {
		sysLogPrintf(LOG_NOTE, "moddata: %d female guard team heads from %08x",
				importGuardHeads(spec->femaleguardteamheads, spec->numfemaleguardteamheads, g_FemaleGuardTeamHeads, GUARDHEADS_FEMALETEAM_ROOM, &g_NumFemaleGuardTeamHeads, "female guard team head"), spec->femaleguardteamheads);
	}

	if (seg.badreads || seg.badfiles) {
		sysLogPrintf(LOG_WARNING, "moddata: %d reads outside the segment, %d file ids with no port file",
				seg.badreads, seg.badfiles);
	}

	return true;
}
