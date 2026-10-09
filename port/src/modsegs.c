/**
 * A PD mod's segments, swapped in and out between stages: the live mode's
 * half that is not files, tables or sound (modsegs.h).
 *
 * romdataSegSwap() points a segment at another copy of itself; this file
 * rebuilds what the game made from the copy it had:
 *
 * - animations: the table's rows (animsTableSwapped(): appended rows kept,
 *   slots grown, every number's cache forgotten), with GoldenEye's overrides
 *   taken off first (geChrAnimsOff()) and the strides measured again
 *   (var80067fdc, race.c's loop at boot)
 * - textureslist/texturesdata: g_Textures and g_NumListTextures, GoldenEye's
 *   surface bytes taken out of the list going (geTexSurfaceTableChanged()),
 *   the art cache texLoad() asks (modSegsTexArtIsRom()), the renderer's cache
 * - the fonts: loaded per stage by textReset() from the segment globals; the
 *   pack's glyph checksums and xblafont's measurements were taken from the
 *   old ones, so the pack index goes (texpackReload(), whose serial xblafont
 *   follows)
 * - the Japanese fonts: read on demand, so the character cache is emptied
 * - mpconfigs, mpstrings, firingrange: read per call or per stage from the
 *   segment as it is, so the swap is all there is
 *
 * The sound's segments are modaudio.c's, the copyright is a boot screen.
 */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "lib/anim.h"
#include "game/race.h"
#include "romdata.h"
#include "modsegs.h"
#include "gechranims.h"
#include "getexsurface.h"
#include "texpack.h"
#include "video.h"
#include "system.h"
#include "fs.h"

static char *segStrDup(const char *str)
{
	const u32 len = strlen(str) + 1;
	char *copy = malloc(len);

	if (copy) {
		memcpy(copy, str, len);
	}

	return copy;
}

enum {
	MODSEGS_ANIMS,
	MODSEGS_TEXTURES,
	MODSEGS_FONTS,
	MODSEGS_JPNFONTS,
	MODSEGS_OTHER,
};

static const struct { const char *name; s32 group; } g_ModSegs[] = {
	{ "animations",         MODSEGS_ANIMS },
	{ "textureslist",       MODSEGS_TEXTURES },
	{ "texturesdata",       MODSEGS_TEXTURES },
	{ "fonttahoma",         MODSEGS_FONTS },
	{ "fontnumeric",        MODSEGS_FONTS },
	{ "fonthandelgothicsm", MODSEGS_FONTS },
	{ "fonthandelgothicxs", MODSEGS_FONTS },
	{ "fonthandelgothicmd", MODSEGS_FONTS },
	{ "fonthandelgothiclg", MODSEGS_FONTS },
	{ "fontjpn",            MODSEGS_JPNFONTS },
	{ "fontjpnsingle",      MODSEGS_JPNFONTS },
	{ "fontjpnmulti",       MODSEGS_JPNFONTS },
	{ "mpconfigs",          MODSEGS_OTHER },
	{ "mpstringsE",         MODSEGS_OTHER },
	{ "mpstringsJ",         MODSEGS_OTHER },
	{ "mpstringsP",         MODSEGS_OTHER },
	{ "mpstringsG",         MODSEGS_OTHER },
	{ "mpstringsF",         MODSEGS_OTHER },
	{ "mpstringsS",         MODSEGS_OTHER },
	{ "mpstringsI",         MODSEGS_OTHER },
	{ "firingrange",        MODSEGS_OTHER },
};

#define MODSEGS_NUM (s32)(sizeof(g_ModSegs) / sizeof(g_ModSegs[0]))

// The path each segment was last swapped to; NULL = the ROM's, or the boot's
// copy before the first swap (romdataSegIsBoot())
static char *g_ModSegPath[MODSEGS_NUM];
static s32 g_ModSegSwapped[MODSEGS_NUM];

// The boot's g_Textures (a copy in the permanent pool) and the list it was made from
static struct texture *g_BootTextures;
static s32 g_BootNumListTextures;

// texLoad()'s question per texture number: 0 not asked, 1 the ROM's art, 2 the mod's
static u8 *g_TexArt;
static s32 g_TexArtCount;

/* ---- the rebuilds ------------------------------------------------------- */

static void modSegsAnimsSwapped(u8 *oldseg)
{
	extern u8 *_animationsSegmentRomStart;
	s32 race;
	s32 i;

	(void)_animationsSegmentRomStart;

	animsTableSwapped(oldseg);

	// race.c's boot loop: the strides chraction.c times a route by
	for (race = 0; race < 5; race++) {
		for (i = 0; var80067fdc[race][i].animnum >= 0; i++) {
			var80067fdc[race][i].value = race0f0005c0(var80067fdc[race][i].animnum);
		}
	}
}

static void modSegsTexturesSwapped(void)
{
	const u8 *list = romdataSegGetData("textureslist");
	const u32 size = romdataSegGetSize("textureslist");

	if (romdataSegIsBoot("textureslist")) {
		g_Textures = g_BootTextures;
		g_NumListTextures = g_BootNumListTextures;
	} else {
		g_Textures = (struct texture *)list;
		g_NumListTextures = size / sizeof(struct texture) - 1;
	}

	free(g_TexArt);
	g_TexArt = NULL;
	g_TexArtCount = 0;

	sysLogPrintf(LOG_NOTE, "modsegs: %d textures in the list", g_NumListTextures);
}

static void modSegsJpnFontsSwapped(void)
{
	extern struct jpncacheitem *g_JpnCacheCacheItems;

	if (g_JpnCacheCacheItems) {
		for (s32 i = 0; i < MAX_JPN_CACHE_ITEMS(); i++) {
			g_JpnCacheCacheItems[i].ttl = 0;
			g_JpnCacheCacheItems[i].codepoint = -1;
		}
	}
}

/* ---- the swap ----------------------------------------------------------- */

static void modSegsApply(const char *moddir)
{
	extern u8 *_animationsSegmentRomStart;
	s32 changed[MODSEGS_OTHER + 1] = { 0 };
	u8 *oldanimseg = _animationsSegmentRomStart;
	s32 i;

	if (!g_BootTextures) {
		g_BootTextures = g_Textures;
		g_BootNumListTextures = g_NumListTextures;
	}

	for (i = 0; i < MODSEGS_NUM; i++) {
		const char *name = g_ModSegs[i].name;
		char path[FS_MAXPATH + 1];
		const char *want = NULL;
		s32 result;

		if (moddir && moddir[0]) {
			snprintf(path, sizeof(path), "%s/segs/%s", moddir, name);

			if (fsFileSize(path) > 0) {
				want = path;
			}
		}

		if (!g_ModSegSwapped[i]) {
			// still the boot's: nothing to do when that is what is wanted
			if (!want && romdataSegIsStock(name)) {
				continue;
			}
		} else if ((!want && !g_ModSegPath[i]) || (want && g_ModSegPath[i] && !strcmp(want, g_ModSegPath[i]))) {
			continue;
		}

		// what is built from the segment going is put right before it goes
		if (!changed[g_ModSegs[i].group]) {
			if (g_ModSegs[i].group == MODSEGS_ANIMS) {
				geChrAnimsOff();
			} else if (g_ModSegs[i].group == MODSEGS_TEXTURES) {
				geTexSurfaceTableChanged();
			}
		}

		result = romdataSegSwap(name, want);

		if (result < 0) {
			sysLogPrintf(LOG_WARNING, "modsegs: %s stays as it was", name);
			continue;
		}

		free(g_ModSegPath[i]);
		g_ModSegPath[i] = want ? segStrDup(want) : NULL;
		g_ModSegSwapped[i] = 1;

		if (result > 0) {
			changed[g_ModSegs[i].group] = 1;
		}
	}

	if (changed[MODSEGS_ANIMS]) {
		modSegsAnimsSwapped(oldanimseg);
	}

	if (changed[MODSEGS_TEXTURES]) {
		modSegsTexturesSwapped();
	}

	if (changed[MODSEGS_FONTS]) {
		// the glyph checksums a pack is matched by, and xblafont's
		// measurements (it follows the pack's serial), were the old fonts'
		texpackReload();
	}

	if (changed[MODSEGS_JPNFONTS]) {
		modSegsJpnFontsSwapped();
	}

	if (changed[MODSEGS_ANIMS] || changed[MODSEGS_TEXTURES] || changed[MODSEGS_FONTS] || changed[MODSEGS_JPNFONTS]) {
		videoResetTextureCache();
	}

	sysLogPrintf(LOG_NOTE, "modsegs: segments are %s%s (animations %d, textures %d, fonts %d, jpn fonts %d, other %d)",
			moddir ? "the mod's: " : "the ROM's", moddir ? moddir : "",
			changed[MODSEGS_ANIMS], changed[MODSEGS_TEXTURES], changed[MODSEGS_FONTS],
			changed[MODSEGS_JPNFONTS], changed[MODSEGS_OTHER]);
}

void modSegsEnter(const char *moddir)
{
	modSegsApply(moddir);
}

void modSegsLeave(void)
{
	modSegsApply(NULL);
}

/* ---- the art a texture number is ---------------------------------------- */

s32 modSegsTexArtIsRom(s32 num)
{
	const u8 *romlist;
	const u8 *romdata;
	const u8 *data;
	u32 romlistsize;
	u32 romdatasize;
	u32 datasize;
	u32 thisofs, nextofs, romthis, romnext;
	const struct texture *rom;
	s32 same;

	if (romdataSegIsStock("texturesdata") && romdataSegIsStock("textureslist")) {
		return 1;
	}

	if (num < 0 || num >= g_NumListTextures) {
		return 0;
	}

	if (!g_TexArt || g_TexArtCount != g_NumListTextures) {
		free(g_TexArt);
		g_TexArt = calloc(g_NumListTextures, 1);
		g_TexArtCount = g_TexArt ? g_NumListTextures : 0;

		if (!g_TexArt) {
			return 0;
		}
	}

	if (g_TexArt[num]) {
		return g_TexArt[num] == 1;
	}

	romlist = romdataSegGetRomData("textureslist", &romlistsize);
	romdata = romdataSegGetRomData("texturesdata", &romdatasize);
	data = romdataSegGetData("texturesdata");
	datasize = romdataSegGetSize("texturesdata");
	rom = (const struct texture *)romlist;

	same = 0;

	if (romlist && romdata && data && (u32)(num + 2) * sizeof(struct texture) <= romlistsize) {
		thisofs = g_Textures[num].dataoffset;
		nextofs = g_Textures[num + 1].dataoffset;
		romthis = rom[num].dataoffset;
		romnext = rom[num + 1].dataoffset;

		same = nextofs - thisofs == romnext - romthis
			&& nextofs <= datasize && romnext <= romdatasize && nextofs >= thisofs
			&& memcmp(data + thisofs, romdata + romthis, nextofs - thisofs) == 0;
	}

	g_TexArt[num] = same ? 1 : 2;

	return same;
}

/* ---- the dump ----------------------------------------------------------- */

static u64 modSegsHash(const void *p, u32 len)
{
	const u8 *b = p;
	u64 h = 0xcbf29ce484222325ull;

	for (u32 i = 0; b && i < len; i++) {
		h = (h ^ b[i]) * 0x100000001b3ull;
	}

	return h;
}

s32 modSegsDump(const char *path)
{
	extern struct animtableentry *g_RomAnims;
	extern u8 **g_AnimReplacements;
	extern s16 g_NumRomAnimations;
	extern s32 g_AnimMaxHeaderLength;
	extern s32 g_AnimMaxBytesPerFrame;
	extern u8 *_animationsSegmentRomStart;
	extern u8 *_animationsSegmentRomEnd;
	FILE *f = fopen(path, "w");
	s32 i;

	if (!f) {
		sysLogPrintf(LOG_ERROR, "modsegs: could not write %s", path);
		return -1;
	}

	for (i = 0; i < MODSEGS_NUM; i++) {
		const char *name = g_ModSegs[i].name;
		const u8 *d = romdataSegGetData(name);
		const u32 size = romdataSegGetSize(name);

		fprintf(f, "seg %s size %u %s hash %016llx\n", name, size, romdataSegIsStock(name) ? "rom" : "mod",
				(unsigned long long)modSegsHash(d, d ? size : 0));
	}

	// the animation rows: the table's, then those appended after it
	fprintf(f, "anims rows %d table %d\n", g_NumRomAnimations, animsGetTableRows());
	fprintf(f, "# anim slots %d header %d frame (grown, never shrunk)\n", g_AnimMaxHeaderLength, g_AnimMaxBytesPerFrame);

	for (i = 0; i < g_NumRomAnimations; i++) {
		const struct animtableentry *e = &g_RomAnims[i];
		const u32 len = e->headerlen + (u32)e->numframes * e->bytesperframe;
		const u8 *bytes = NULL;
		const char *from = "seg";

		if (e->data == 0xffffffff) {
			bytes = g_AnimReplacements[i];
			from = bytes ? "ext" : "lazy";
		} else if (_animationsSegmentRomStart + e->data + len <= _animationsSegmentRomEnd) {
			bytes = _animationsSegmentRomStart + e->data;
		} else {
			from = "past";
		}

		fprintf(f, "anim %04x %u %u %08x %u %u %02x %s %016llx\n", i, e->numframes, e->bytesperframe,
				e->data, e->headerlen, e->framelen, e->flags, from,
				(unsigned long long)(bytes ? modSegsHash(bytes, len) : 0));
	}

	for (s32 race = 0; race < 5; race++) {
		for (i = 0; var80067fdc[race][i].animnum >= 0; i++) {
			fprintf(f, "stride %d %04x %.6f\n", race, var80067fdc[race][i].animnum, var80067fdc[race][i].value);
		}
	}

	// the texture list, and each texture's compressed bytes
	{
		const u8 *data = romdataSegGetData("texturesdata");
		const u32 datasize = romdataSegGetSize("texturesdata");

		fprintf(f, "textures %d list %016llx\n", g_NumListTextures,
				(unsigned long long)modSegsHash(g_Textures, (g_NumListTextures + 1) * sizeof(struct texture)));

		for (i = 0; i < g_NumListTextures; i++) {
			const u32 a = g_Textures[i].dataoffset;
			const u32 b = g_Textures[i + 1].dataoffset;

			fprintf(f, "tex %04x %u %u %u %016llx\n", i, a, g_Textures[i].surfacetype, g_Textures[i].soundsurfacetype,
					(unsigned long long)(b >= a && b <= datasize ? modSegsHash(data + a, b - a) : 0));
		}
	}

	fclose(f);

	sysLogPrintf(LOG_NOTE, "modsegs: dumped to %s", path);

	return 0;
}

/* ---- the test switches -------------------------------------------------- */

void modSegsTestHook(s32 stagenum)
{
	static s32 numResets = 0;
	const s32 at = sysArgGetString("--mod-segs-at") ? atoi(sysArgGetString("--mod-segs-at")) : 0;
	const char *dump = sysArgGetString("--mod-dump-segs");
	s32 n;

	if (numResets++ != at) {
		return;
	}

	for (n = 0; ; n++) {
		const char *dir = sysArgGetStringN("--mod-segs-enter", n);

		if (!dir) {
			break;
		}

		if (!strcmp(dir, "-")) {
			modSegsLeave();
		} else {
			char full[FS_MAXPATH + 1];

			// the mod list hands over full paths; a relative one is the working directory's
			if (!fsPathIsAbsolute(dir) && !fsPathIsCwdRelative(dir) && dir[0] != '$') {
				snprintf(full, sizeof(full), "./%s", dir);
				dir = full;
			}

			modSegsEnter(dir);
		}
	}

	if (dump) {
		modSegsDump(dump);
	}

	(void)stagenum;
}
