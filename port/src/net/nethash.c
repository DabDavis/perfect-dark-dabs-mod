#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "files.h"
#include "fs.h"
#include "romdata.h"
#include "system.h"
#include "sha256.h"
#include "gestan.h"
#include "geconvert.h"
#include "net/net.h"
#include "netint.h"

/**
 * What two machines must agree on before they play (spec-stage.md §5,
 * NETPLAY.md "Content safety"), as named components so that a refusal can
 * say which part differs.
 *
 * The session hash, sent in CONNECT, is taken once at startup over what
 * decides the simulation: the ROM, the overlay mod's files (contents), the
 * maps-only mods' files (names and sizes: the stage hash checks the contents
 * of the one played), the added-content set (names and sizes) and the
 * GoldenEye converter's version. Never texture packs, model packs, music or
 * text: nothing that only changes what a player sees or hears.
 *
 * The stage hash, sent in LOADED, is taken between H5 and H6 (lvReset) at
 * the file-load chokepoints: every setup, pads, tiles and bg file the stage
 * loads (raw bytes as they are on disk, before inflating), the XBLA collision
 * that skips fileLoad (HC), the GoldenEye tile graph that is read lazily
 * (HD), models as a diagnostic only, and g_RngSeed as lvReset leaves it.
 * Each component is the sum of its files' hashes, so the order things load
 * in does not matter.
 */

/*
 * The session hash
 */

#define NETHASH_MAXDEPTH 8
#define NETHASH_MAXFILES 40000
#define NETHASH_CONTENTMAX (64 * 1024 * 1024)

struct nethashwalk {
	struct sha256ctx *ctx;
	s32 contents;   // hash each file's bytes, not only its name and size
	s32 numfiles;
};

struct nethashnames {
	char **names;
	s32 count;
	s32 max;
};

extern u64 g_RngSeed;

static struct nethashcomp s_NetSessionComps[NET_MAXCOMPS];
static s32 s_NetSessionNumComps = -1;

void netWriteU64(struct netbuf *b, u64 v)
{
	netBufWriteU32(b, (u32)v);
	netBufWriteU32(b, (u32)(v >> 32));
}

u64 netReadU64(struct netbuf *b)
{
	u64 lo = netBufReadU32(b);
	u64 hi = netBufReadU32(b);

	return lo | (hi << 32);
}

void netWriteStr(struct netbuf *b, const char *s, s32 maxlen)
{
	char tmp[512];

	if (!s) {
		s = "";
	}

	if (maxlen > (s32)sizeof(tmp) - 1) {
		maxlen = sizeof(tmp) - 1;
	}

	snprintf(tmp, (size_t)maxlen + 1, "%s", s);
	netBufWriteString(b, tmp, maxlen);
}

u64 netDigestU64(const u8 *digest)
{
	u64 v = 0;
	s32 i;

	for (i = 0; i < 8; i++) {
		v = (v << 8) | digest[i];
	}

	return v;
}

static void netHashNamesAdd(const char *name, void *arg)
{
	struct nethashnames *n = arg;

	if (n->count == n->max) {
		s32 max = n->max ? n->max * 2 : 64;
		char **names = realloc(n->names, sizeof(char *) * max);

		if (!names) {
			return;
		}

		n->names = names;
		n->max = max;
	}

	n->names[n->count] = malloc(strlen(name) + 1);

	if (n->names[n->count]) {
		strcpy(n->names[n->count], name);
		n->count++;
	}
}

static int netHashNameCmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/**
 * What a player may have that the simulation never reads: text, pictures,
 * sound, and folders of the same.
 */
static s32 netHashSkipName(const char *name, s32 isdir)
{
	static const char *dirs[] = { "cache", "textures", "texture-packs", "model-packs", "screenshots", "traces", "music", "crashreports" };
	static const char *exts[] = { ".png", ".jpg", ".jpeg", ".dds", ".bmp", ".tga", ".txt", ".md", ".log", ".ini",
		".ogg", ".wav", ".mp3", ".flac", ".htc", ".hts", ".pdf", ".html" };
	const char *dot;
	u32 i;

	if (isdir) {
		for (i = 0; i < ARRAYCOUNT(dirs); i++) {
			if (strcasecmp(name, dirs[i]) == 0) {
				return 1;
			}
		}

		return 0;
	}

	dot = strrchr(name, '.');

	if (dot) {
		for (i = 0; i < ARRAYCOUNT(exts); i++) {
			if (strcasecmp(dot, exts[i]) == 0) {
				return 1;
			}
		}
	}

	return 0;
}

static void netHashFileBytes(struct sha256ctx *ctx, const char *path)
{
	u8 chunk[16384];
	size_t got;
	FILE *f = fopen(path, "rb");

	if (!f) {
		return;
	}

	while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		sha256Add(ctx, chunk, (u32)got);
	}

	fclose(f);
}

/**
 * A folder, sorted by name so every system walks it alike, as relative
 * names: two installs in different places hash the same.
 */
static void netHashTree(struct nethashwalk *w, const char *full, const char *rel, s32 depth)
{
	struct nethashnames n = { NULL, 0, 0 };
	char childfull[FS_MAXPATH + 1];
	char childrel[FS_MAXPATH + 1];
	s32 i;

	if (depth > NETHASH_MAXDEPTH || fsScanDir(full, netHashNamesAdd, &n) < 0) {
		return;
	}

	qsort(n.names, n.count, sizeof(char *), netHashNameCmp);

	for (i = 0; i < n.count; i++) {
		s32 isdir;
		s32 size;

		snprintf(childfull, sizeof(childfull), "%s/%s", full, n.names[i]);
		snprintf(childrel, sizeof(childrel), "%s%s%s", rel, rel[0] ? "/" : "", n.names[i]);

		isdir = fsScanDir(childfull, NULL, NULL) >= 0;

		if (netHashSkipName(n.names[i], isdir) || w->numfiles >= NETHASH_MAXFILES) {
			continue;
		}

		if (isdir) {
			netHashTree(w, childfull, childrel, depth + 1);
			continue;
		}

		size = fsFileSize(childfull);
		w->numfiles++;

		sha256Add(w->ctx, childrel, strlen(childrel) + 1);
		sha256Add(w->ctx, &size, sizeof(size));

		if (w->contents && size > 0 && size <= NETHASH_CONTENTMAX) {
			netHashFileBytes(w->ctx, childfull);
		}
	}

	for (i = 0; i < n.count; i++) {
		free(n.names[i]);
	}

	free(n.names);
}

static const char *netHashBasename(const char *path)
{
	const char *s = strrchr(path, '/');
	const char *b = strrchr(path, '\\');

	if (b && (!s || b > s)) {
		s = b;
	}

	return s ? s + 1 : path;
}

static void netHashAddComp(const char *name, struct sha256ctx *ctx)
{
	u8 digest[32];
	struct nethashcomp *c = &s_NetSessionComps[s_NetSessionNumComps++];

	sha256End(ctx, digest);
	snprintf(c->name, sizeof(c->name), "%s", name);
	c->hash = netDigestU64(digest);
}

static s32 netHashDirCmp(const void *a, const void *b)
{
	return strcasecmp(netHashBasename(*(char *const *)a), netHashBasename(*(char *const *)b));
}

static void netSessionHashCompute(void)
{
	static const char *addedsearch[] = { FS_ADDED_CONTENT_SEARCH };
	struct sha256ctx ctx;
	struct sha256ctx all;
	struct nethashwalk w;
	const char *overlay = fsGetModDir();
	const char *dirs[64];
	char full[FS_MAXPATH + 1];
	s32 ndirs = 0;
	s32 i;
	const u64 t0 = sysGetMicroseconds();

	s_NetSessionNumComps = 0;

	// rom: Perfect Dark's own ROM, every byte
	sha256Begin(&ctx);
	sha256Add(&ctx, g_RomFile, g_RomFileSize);
	netHashAddComp("rom", &ctx);

	// mod: the mod that overlays the game's files, every byte that is not
	// a picture, a sound or text
	sha256Begin(&ctx);

	if (overlay) {
		snprintf(full, sizeof(full), "%s", fsFullPath(overlay));
		w.ctx = &ctx;
		w.contents = 1;
		w.numfiles = 0;
		netHashTree(&w, full, "", 0);
	}

	netHashAddComp("mod", &ctx);

	// mapmods: every other mounted dir (the Stage Loader's maps), names and
	// sizes, in name order
	for (i = 0; i < fsGetNumModDirs() && ndirs < (s32)ARRAYCOUNT(dirs); i++) {
		const char *d = fsGetModDirAt(i);

		if (d && (!overlay || strcmp(d, overlay) != 0)) {
			dirs[ndirs++] = d;
		}
	}

	qsort(dirs, ndirs, sizeof(char *), netHashDirCmp);
	sha256Begin(&ctx);

	for (i = 0; i < ndirs; i++) {
		const char *base = netHashBasename(dirs[i]);

		sha256Add(&ctx, base, strlen(base) + 1);
		snprintf(full, sizeof(full), "%s", fsFullPath(dirs[i]));
		w.ctx = &ctx;
		w.contents = 0;
		w.numfiles = 0;
		netHashTree(&w, full, "", 0);
	}

	netHashAddComp("mapmods", &ctx);

	// added: the GoldenEye ROM and the XBLA releases, names and sizes
	sha256Begin(&ctx);

	for (i = 0; i < (s32)ARRAYCOUNT(addedsearch); i++) {
		snprintf(full, sizeof(full), "%s", fsFullPath(addedsearch[i]));

		if (fsScanDir(full, NULL, NULL) >= 0) {
			w.ctx = &ctx;
			w.contents = 0;
			w.numfiles = 0;
			netHashTree(&w, full, "", 0);
			break;
		}
	}

	netHashAddComp("added", &ctx);

	// geconv: the GoldenEye converter, which makes GE Plus's arenas
	sha256Begin(&ctx);
	sha256Add(&ctx, GECONVERT_VERSION_STR, strlen(GECONVERT_VERSION_STR));
	netHashAddComp("geconv", &ctx);

	// all: the lot, the one value a lobby lists rooms by
	sha256Begin(&all);

	for (i = 0; i < s_NetSessionNumComps; i++) {
		u8 b[8];
		s32 k;

		for (k = 0; k < 8; k++) {
			b[k] = (u8)(s_NetSessionComps[i].hash >> (56 - k * 8));
		}

		sha256Add(&all, s_NetSessionComps[i].name, strlen(s_NetSessionComps[i].name) + 1);
		sha256Add(&all, b, 8);
	}

	netHashAddComp("all", &all);

	for (i = 0; i < s_NetSessionNumComps; i++) {
		sysLogPrintf(LOG_NOTE, "net: session hash %-8s %016llx", s_NetSessionComps[i].name,
				(unsigned long long)s_NetSessionComps[i].hash);
	}

	sysLogPrintf(LOG_NOTE, "net: session hash took %u ms", (u32)((sysGetMicroseconds() - t0) / 1000));
}

s32 netSessionHash(struct nethashcomp *comps, s32 max)
{
	s32 i;

	if (s_NetSessionNumComps < 0) {
		netSessionHashCompute();
	}

	for (i = 0; i < s_NetSessionNumComps && i < max; i++) {
		comps[i] = s_NetSessionComps[i];
	}

	return i;
}

/*
 * The stage hash
 */

#define NETSTAGE_SETUP     0
#define NETSTAGE_PADS      1
#define NETSTAGE_TILES     2
#define NETSTAGE_BG        3
#define NETSTAGE_XBLATILES 4
#define NETSTAGE_STAN      5
#define NETSTAGE_MODELS    6
#define NETSTAGE_RNG       7
#define NETSTAGE_COUNT     8

static const char *s_NetStageCompNames[NETSTAGE_COUNT] = {
	"setup", "pads", "tiles", "bg", "xblatiles", "stan", "models", "rng",
};

static struct {
	s32 open;
	u64 sum[NETSTAGE_COUNT];
	u32 count[NETSTAGE_COUNT];
	u8 seen[NUM_FILE_SLOTS / 8];
} s_NetStage;

s32 netStageHashOpen(void)
{
	return s_NetStage.open;
}

void netStageHashReset(void)
{
	memset(&s_NetStage, 0, sizeof(s_NetStage));
}

static void netStageAdd(s32 comp, const u8 *digest)
{
	s_NetStage.sum[comp] += netDigestU64(digest);
	s_NetStage.count[comp]++;
}

void netStageHashFile(s32 filenum, s32 loadtype, const void *data, u32 len)
{
	struct sha256ctx ctx;
	u8 digest[32];
	const char *name;
	char numname[16];
	s32 comp;

	if (!s_NetStage.open || filenum <= 0 || filenum >= NUM_FILE_SLOTS || !data) {
		return;
	}

	switch (loadtype) {
	case LOADTYPE_SETUP: comp = NETSTAGE_SETUP; break;
	case LOADTYPE_PADS:  comp = NETSTAGE_PADS; break;
	case LOADTYPE_TILES: comp = NETSTAGE_TILES; break;
	case LOADTYPE_BG:    comp = NETSTAGE_BG; break;
	case LOADTYPE_MODEL:
	case LOADTYPE_GUN:   comp = NETSTAGE_MODELS; break;
	default:
		// LOADTYPE_LANG follows Game.Language; untyped loads are not the stage's
		return;
	}

	if (s_NetStage.seen[filenum >> 3] & (1 << (filenum & 7))) {
		return;
	}

	s_NetStage.seen[filenum >> 3] |= 1 << (filenum & 7);

	name = romdataFileGetName(filenum);

	if (!name) {
		snprintf(numname, sizeof(numname), "#%d", filenum);
		name = numname;
	}

	sha256Begin(&ctx);
	sha256Add(&ctx, name, strlen(name) + 1);
	sha256Add(&ctx, &len, sizeof(len));
	sha256Add(&ctx, data, len);
	sha256End(&ctx, digest);
	netStageAdd(comp, digest);
}

void netStageHashNote(s32 loadtype, u32 crc, u32 len)
{
	struct sha256ctx ctx;
	u8 digest[32];

	if (!s_NetStage.open || loadtype != LOADTYPE_TILES) {
		return;
	}

	sha256Begin(&ctx);
	sha256Add(&ctx, &crc, sizeof(crc));
	sha256Add(&ctx, &len, sizeof(len));
	sha256End(&ctx, digest);
	netStageAdd(NETSTAGE_XBLATILES, digest);
}

void netStageHashOpenWindow(void)
{
	netStageHashReset();
	s_NetStage.open = 1;
}

/**
 * H6: the window closes; the tile graph (read lazily, never through
 * fileLoad) and the RNG as lvReset leaves it go in last.
 */
void netStageHashCloseWindow(void)
{
	u8 digest[32];

	if (!s_NetStage.open) {
		return;
	}

	if (geStanFileHash(digest)) {
		netStageAdd(NETSTAGE_STAN, digest);
	}

	// the seed itself: two machines that drew differently since H4 differ here
	s_NetStage.sum[NETSTAGE_RNG] = g_RngSeed;
	s_NetStage.count[NETSTAGE_RNG] = 1;

	s_NetStage.open = 0;
}

s32 netStageHashComponents(struct nethashcomp *comps, s32 max)
{
	s32 i;

	for (i = 0; i < NETSTAGE_COUNT && i < max; i++) {
		snprintf(comps[i].name, sizeof(comps[i].name), "%s", s_NetStageCompNames[i]);
		comps[i].hash = s_NetStage.sum[i];
	}

	return i;
}

u32 netStageHashCount(s32 comp)
{
	return comp >= 0 && comp < NETSTAGE_COUNT ? s_NetStage.count[comp] : 0;
}
