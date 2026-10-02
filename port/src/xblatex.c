/**
 * The XBLA release's own textures, handed to the renderer by address.
 * See xblatex.h for what this is and why it cannot go through a texture pack.
 *
 * Everything here is one texture at a time. Textures.raw is 166MB and is never
 * held in memory - the two record tables are read once when the package is
 * opened, and a picture is streamed, decompressed and decoded only when the
 * renderer asks for it, which is once per texture per fill of its cache.
 */

#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "xblaswitch.h"
#include "constants.h"
#include "platform.h"
#include "config.h"
#include "system.h"
#include "video.h"
#include "x360.h"
#include "xblaimport.h"
#include "texpack.h"
#include "xblatex.h"
#include "trace.h"

// Inside the package. The only file this reads.
#define XBLATEX_TEXTURES "DataFiles/Textures.raw"

// The two 52 byte tables: the metadata, then the D3DTexture structs whose last
// six dwords are the GPU fetch constant. Same shape xblaimport.c reads.
#define XBLATEX_RECORD 52
#define XBLATEX_FETCH_DWORD 7

// A record whose dimensions are past this is not something to allocate for.
#define XBLATEX_MAXDIM 4096

// How many decodes are named in the log before it stops.
#define XBLATEX_DECODELOG 16

// Bound records, open addressed on the address of the stand-in. The meshes'
// materials name records 3741 to 5746, so 2006 is every texture the release
// could ever ask for; this is sized so half of it is comfortably past that and
// the table never fills or slows down.
#define XBLATEX_HASHSIZE 8192

// What a stand-in tile holds if anything ever reads it - which only happens
// when the decode below fails, since a picture that arrives replaces it. White
// is the one wrong answer that cannot darken what it multiplies.
#define XBLATEX_TILE_TEXELS (XBLATEX_TILE * XBLATEX_TILE)
#define XBLATEX_TILE_BYTES  (XBLATEX_TILE_TEXELS * 2)

// A stand-in that holds a picture of its own rather than naming a record: a
// model pack's PNG, or one of the ROM's numbered textures decoded for a mesh
// that draws with it. See xblaTexBindImage().
#define XBLATEX_NOREC 0xffffffffu

struct xblatexentry {
	u8 *addr;      // the stand-in the display list binds; the key
	u32 record;    // or XBLATEX_NOREC for a picture of its own
	u8 *image;     // that picture, RGBA32 in the game's row order, kept for good
	s32 width;
	s32 height;
	u8 alpha;      // whether any of its texels is not opaque
	u8 soft;       // whether next to none are - see xblaTexRecordIsSoft()
	s32 texnum;    // the ROM texture the picture is of, or -1: see xblaTexBindTexture()
	u8 kept;       // drawn with the art switched off: see xblaTexBindKept()
	char *key;     // what it was bound as, so the same picture binds once
};

static s32 opened; // 0 untried, 1 open, -1 no package
static s32 numBound;
static s32 numDecoded;

/**
 * Mod.XblaMeshTextures: the release's own art, on the release's own meshes and
 * on the game's own numbered textures (xblaTexLoadNumbered()).
 *
 * On, because an untextured mesh is a flat pale solid and is not what anyone
 * turns the meshes on to see; off is how a shape that is wrong is told apart
 * from a texture that is.
 *
 * The flag lives here, at the point a picture is handed over, rather than
 * where a display list is built - which is what makes it a live toggle. A
 * material always binds its stand-in, and this only decides whether the
 * release's picture arrives in the stand-in's place; the stand-in's own texels
 * are white, so with this off a material draws white times shade, which is the
 * same flat solid as a list built with no texture at all. Nothing has to be
 * built again, and the menu only has to drop the texture cache.
 */
static s32 optEnabled = 1;

static struct x360stfs stfs;
static struct x360stfsstream stream;
static u8 *tables;
static u32 numRecords;
static u32 dataBase;

static struct xblatexentry hash[XBLATEX_HASHSIZE];
static u8 **byRecord;  // one stand-in per record, or NULL
static u8 *badRecord;  // a record that did not decode, so it is not tried again
static u8 *softRecord; // XBLATEX_SOFT_* per record, see xblaTexRecordIsSoft()
static u8 **alphaMap;  // per record, see xblaTexRecordAlphaMap()
static u8 *alphaTried; // and whether it has been looked for
static u8 *flatPane;   // per record, see xblaTexRecordIsFlatPane()

// What xblaTexRecordIsSoft() has found out about a record so far.
#define XBLATEX_SOFT_UNKNOWN 0
#define XBLATEX_SOFT_NO      1
#define XBLATEX_SOFT_YES     2

// A texel at or above this alpha is opaque, allowing for what DXT5 does to a
// flat 255 - the same line xblamesh.c's XBLAMESH_FADE_ALPHA draws for a vertex.
#define XBLATEX_OPAQUE_ALPHA 0xf0

// A record with fewer opaque texels than this, in hundredths, is a soft one:
// a glow, a glass, a haze. Counted over the 59 alpha records the meshes use,
// nine are under it and every one of them is a picture with no edge to cut at
// (the least opaque of the rest is a lamp with 15% of its texels at 255).
#define XBLATEX_SOFT_PERCENT 1

// How narrow a record's partial alpha has to be to be a pane: see
// xblaTexRecordIsFlatPane().
#define XBLATEX_FLATPANE_BAND 48

// The meshes are built on the game thread and uploaded on the render thread,
// so both the registry and the package handle are shared. Everything below
// that touches either takes this. Made before either thread exists, so that
// nothing has to decide whether it is the one to make it.
static SDL_mutex *lock;

PD_CONSTRUCTOR static void xblaTexInit(void)
{
	lock = SDL_CreateMutex();

	configRegisterInt("Mod.XblaMeshTextures", &optEnabled, 0, 1);
}

static u32 xblaTexBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u8 *xblaTexDecode(u32 record, s32 *outWidth, s32 *outHeight);

static void xblaTexCloseUp(void)
{
	x360StfsStreamClose(&stream);
	x360StfsClose(&stfs);
	free(tables);
	free(byRecord);
	free(badRecord);
	free(softRecord);

	for (u32 i = 0; alphaMap && i < numRecords; i++) {
		free(alphaMap[i]);
	}

	free(alphaMap);
	free(alphaTried);
	free(flatPane);
	tables = NULL;
	byRecord = NULL;
	badRecord = NULL;
	softRecord = NULL;
	alphaMap = NULL;
	alphaTried = NULL;
	flatPane = NULL;
	numRecords = 0;
	opened = -1;
}

/**
 * Opens the package and reads both record tables.
 *
 * The data does not start after the first table but after both of them, which
 * is what makes the offsets chain: reading the base as one table's worth
 * leaves every texture short by exactly one table.
 */
static s32 xblaTexOpen(void)
{
	const char *path;
	s32 index;
	u8 head[4];

	if (opened) {
		return opened > 0;
	}

	opened = -1;

	path = xblaImportGetStfsPath();

	if (!path || !path[0]) {
		return 0;
	}

	if (!x360StfsOpen(&stfs, path)) {
		sysLogPrintf(LOG_ERROR, "xblatex: %s is not a package", path);
		return 0;
	}

	index = x360StfsFind(&stfs, XBLATEX_TEXTURES);

	if (index < 0 || !x360StfsStreamOpen(&stfs, (u32)index, &stream)) {
		sysLogPrintf(LOG_ERROR, "xblatex: no " XBLATEX_TEXTURES " in %s", path);
		x360StfsClose(&stfs);
		return 0;
	}

	if (!x360StfsStreamRead(&stream, 0, sizeof(head), head)) {
		xblaTexCloseUp();
		return 0;
	}

	numRecords = xblaTexBE32(head);
	dataBase = 4 + numRecords * 2 * XBLATEX_RECORD;

	if (numRecords == 0 || numRecords > 0x10000 || dataBase > stream.size) {
		sysLogPrintf(LOG_ERROR, "xblatex: %u records do not fit in %u bytes",
				numRecords, stream.size);
		xblaTexCloseUp();
		return 0;
	}

	tables = malloc(dataBase - 4);
	byRecord = calloc(numRecords, sizeof(u8 *));
	badRecord = calloc(numRecords, 1);
	softRecord = calloc(numRecords, 1);
	alphaMap = calloc(numRecords, sizeof(u8 *));
	alphaTried = calloc(numRecords, 1);
	flatPane = calloc(numRecords, 1);

	if (!tables || !byRecord || !badRecord || !softRecord || !alphaMap || !alphaTried || !flatPane ||
			!x360StfsStreamRead(&stream, 4, dataBase - 4, tables)) {
		sysLogPrintf(LOG_ERROR, "xblatex: could not read the texture tables");
		xblaTexCloseUp();
		return 0;
	}

	opened = 1;

	sysLogPrintf(LOG_NOTE, "xblatex: %u texture records in %s", numRecords, path);

	return 1;
}

/* -------------------------------------------------------------------------
 * The registry
 * ------------------------------------------------------------------------- */

static u32 xblaTexHashOf(const void *addr)
{
	// A malloc'd pointer's low bits are all alignment, so they are shifted off
	// before the mix rather than left to collide.
	u64 x = (u64)(uintptr_t)addr >> 4;

	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 29;

	return (u32)x & (XBLATEX_HASHSIZE - 1);
}

static struct xblatexentry *xblaTexFind(const void *addr)
{
	u32 slot = xblaTexHashOf(addr);

	for (u32 i = 0; i < XBLATEX_HASHSIZE; i++) {
		struct xblatexentry *e = &hash[(slot + i) & (XBLATEX_HASHSIZE - 1)];

		if (!e->addr) {
			return NULL;
		}

		if (e->addr == addr) {
			return e;
		}
	}

	return NULL;
}

const void *xblaTexBind(u32 record)
{
	u8 *addr;
	u32 slot;

	if (!lock) {
		return NULL;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords || numBound >= XBLATEX_HASHSIZE / 2) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	if (byRecord[record]) {
		addr = byRecord[record];
		SDL_UnlockMutex(lock);
		return addr;
	}

	addr = malloc(XBLATEX_TILE_BYTES);

	if (!addr) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	memset(addr, 0xff, XBLATEX_TILE_BYTES);

	slot = xblaTexHashOf(addr);

	while (hash[slot].addr) {
		slot = (slot + 1) & (XBLATEX_HASHSIZE - 1);
	}

	hash[slot].addr = addr;
	hash[slot].record = record;
	hash[slot].texnum = -1;
	hash[slot].kept = 0;
	byRecord[record] = addr;
	numBound++;

	SDL_UnlockMutex(lock);

	return addr;
}

/**
 * xblaTexBind(), for a record painted on a mesh that is drawn with the
 * release's art switched off - a model the release alone has, such as Agent 4
 * (xblaagent4.c), which has no N64 form to fall back to.
 */
const void *xblaTexBindKept(u32 record)
{
	const void *addr = xblaTexBind(record);

	if (addr) {
		struct xblatexentry *e;

		SDL_LockMutex(lock);
		e = xblaTexFind(addr);

		if (e) {
			e->kept = 1;
		}

		SDL_UnlockMutex(lock);
	}

	return addr;
}

/**
 * A stand-in for a picture of its own - see xblatex.h.
 *
 * Bound once per key: the second ask for the same key gets the first tile
 * back and the picture handed in is freed, so a material that several meshes
 * name uploads once. The picture is kept for the life of the game the way the
 * tile is, since the renderer asks for it again whenever its cache evicts it.
 */
static const void *xblaTexBindPicture(const char *key, u8 *rgba, s32 width, s32 height, s32 texnum)
{
	u8 *addr;
	u32 slot;
	u32 opaque = 0;
	u32 total;

	if (!lock || !key || !rgba || width <= 0 || height <= 0) {
		free(rgba);
		return NULL;
	}

	SDL_LockMutex(lock);

	if (numBound >= XBLATEX_HASHSIZE / 2) {
		SDL_UnlockMutex(lock);
		free(rgba);
		return NULL;
	}

	for (u32 i = 0; i < XBLATEX_HASHSIZE; i++) {
		if (hash[i].addr && hash[i].key && !strcmp(hash[i].key, key)) {
			addr = hash[i].addr;
			SDL_UnlockMutex(lock);
			free(rgba);
			return addr;
		}
	}

	addr = malloc(XBLATEX_TILE_BYTES);

	if (!addr) {
		SDL_UnlockMutex(lock);
		free(rgba);
		return NULL;
	}

	memset(addr, 0xff, XBLATEX_TILE_BYTES);

	slot = xblaTexHashOf(addr);

	while (hash[slot].addr) {
		slot = (slot + 1) & (XBLATEX_HASHSIZE - 1);
	}

	total = (u32)width * (u32)height;

	for (u32 i = 0; i < total; i++) {
		if (rgba[i * 4 + 3] >= XBLATEX_OPAQUE_ALPHA) {
			opaque++;
		}
	}

	hash[slot].addr = addr;
	hash[slot].record = XBLATEX_NOREC;
	hash[slot].texnum = texnum;
	hash[slot].image = rgba;
	hash[slot].width = width;
	hash[slot].height = height;
	hash[slot].alpha = opaque < total;
	hash[slot].soft = opaque * 100 < total * XBLATEX_SOFT_PERCENT;
	hash[slot].key = malloc(strlen(key) + 1);

	if (hash[slot].key) {
		strcpy(hash[slot].key, key);
	}
	numBound++;

	SDL_UnlockMutex(lock);

	return addr;
}

const void *xblaTexBindImage(const char *key, u8 *rgba, s32 width, s32 height)
{
	return xblaTexBindPicture(key, rgba, width, height, -1);
}

/**
 * The animated pictures (xblaTexBindAnimation()). A handful in the whole
 * release, kept for the life of the game as the tiles are. The renderer reads
 * the table without the lock on every texture it binds, so an entry is filled
 * before the count that publishes it.
 *
 * A picture that only changes frames shows them by number. One the file also
 * scrolls or turns is drawn as XBLATEX_ANIMSTEPS steps a second over the
 * period the whole motion repeats in, each step the picture as the release's
 * texture matrix would sample it at that moment, made when the renderer first
 * asks for it (xblaTexLoadAnimFrame()). The texture matrix turns and slides
 * the coordinates a picture is read at; here the picture is turned and slid
 * under coordinates that stay put, which with the wrap the release draws with
 * is the same wherever the coordinates lie within one repeat of the picture,
 * and for a slide everywhere.
 */
#define XBLATEX_MAXANIMS 64
#define XBLATEX_ANIMSTEPS 30
#define XBLATEX_ANIMMAXPERIOD 60.0f

struct xblatexanim {
	const void *addr;
	u8 **frames;
	s32 numframes;
	s32 width;
	s32 height;
	struct xblatexmotion motion;
	f32 period;  // seconds the whole motion repeats in, 0 for frames alone
	s32 steps;   // and the steps it is drawn in over that
};

static struct xblatexanim anims[XBLATEX_MAXANIMS];
static SDL_atomic_t numAnims;

// The level's clock in seconds, which the frames are shown by
// (xblaTexSetAnimClock())
static volatile f32 animClock;

static s32 xblaTexMoves(const struct xblatexmotion *m)
{
	return m->scroll[0] != 0.0f || m->scroll[1] != 0.0f || m->rotate != 0.0f;
}

/**
 * The time the motion repeats in: the shortest whole multiple of its longest
 * part (the frames' loop, a repeat's slide on each axis, a full turn) that the
 * others divide, up to a minute; past that the longest part's own, and the
 * picture jumps a little each time round.
 */
static f32 xblaTexAnimPeriod(const struct xblatexanim *a)
{
	f32 parts[4];
	s32 n = 0;
	f32 longest = 0.0f;

	if (a->numframes > 1 && a->motion.secondsPerFrame > 0.0f) {
		parts[n++] = a->numframes * a->motion.secondsPerFrame;
	}

	for (s32 i = 0; i < 2; i++) {
		if (a->motion.scroll[i] != 0.0f) {
			parts[n++] = 1.0f / fabsf(a->motion.scroll[i]);
		}
	}

	if (a->motion.rotate != 0.0f) {
		parts[n++] = 360.0f / fabsf(a->motion.rotate);
	}

	for (s32 i = 0; i < n; i++) {
		longest = parts[i] > longest ? parts[i] : longest;
	}

	for (f32 p = longest; p <= XBLATEX_ANIMMAXPERIOD; p += longest) {
		s32 all = 1;

		for (s32 i = 0; i < n && all; i++) {
			const f32 r = p / parts[i];

			all = fabsf(r - roundf(r)) < 0.001f * r;
		}

		if (all) {
			return p;
		}
	}

	return longest;
}

const void *xblaTexBindAnimation(const char *key, u8 **frames, s32 numframes, s32 width, s32 height,
		const struct xblatexmotion *motion)
{
	const size_t bytes = (size_t)width * (size_t)height * 4;
	const void *addr;
	u8 *first;
	s32 n;

	if (!frames || numframes <= 0 || width <= 0 || height <= 0 || !motion) {
		for (s32 i = 0; frames && i < numframes; i++) {
			free(frames[i]);
		}

		free(frames);
		return NULL;
	}

	first = malloc(bytes);

	if (first) {
		memcpy(first, frames[0], bytes);
	}

	addr = xblaTexBindPicture(key, first, width, height, -1);

	if (!addr || !lock || ((numframes < 2 || !(motion->secondsPerFrame > 0.0f)) && !xblaTexMoves(motion))) {
		for (s32 i = 0; i < numframes; i++) {
			free(frames[i]);
		}

		free(frames);
		return addr;
	}

	SDL_LockMutex(lock);

	n = SDL_AtomicGet(&numAnims);

	for (s32 i = 0; i < n; i++) {
		if (anims[i].addr == addr) {
			n = XBLATEX_MAXANIMS; // bound before: it keeps those frames
			break;
		}
	}

	if (n < XBLATEX_MAXANIMS) {
		struct xblatexanim *a = &anims[n];

		a->addr = addr;
		a->frames = frames;
		a->numframes = numframes;
		a->width = width;
		a->height = height;
		a->motion = *motion;
		a->period = xblaTexMoves(motion) ? xblaTexAnimPeriod(a) : 0.0f;
		a->steps = a->period > 0.0f ? (s32)(a->period * XBLATEX_ANIMSTEPS + 0.5f) : 0;

		if (a->period > 0.0f && a->steps < 1) {
			a->steps = 1;
		}

		SDL_AtomicSet(&numAnims, n + 1);
		frames = NULL;
	}

	SDL_UnlockMutex(lock);

	if (frames) {
		for (s32 i = 0; i < numframes; i++) {
			free(frames[i]);
		}

		free(frames);
	}

	return addr;
}

void xblaTexSetAnimClock(f32 seconds)
{
	animClock = seconds;
}

s32 xblaTexHaveAnimations(void)
{
	return SDL_AtomicGet(&numAnims) > 0;
}

s32 xblaTexAnimFrame(const void *addr)
{
	const s32 n = SDL_AtomicGet(&numAnims);

	for (s32 i = 0; i < n; i++) {
		if (anims[i].addr == addr) {
			const f32 clock = animClock;
			const struct xblatexanim *a = &anims[i];

			if (a->steps > 0) {
				const f32 into = clock > 0.0f ? fmodf(clock, a->period) : 0.0f;
				const s32 step = (s32)(into / a->period * a->steps);

				return step < 0 ? 0 : step >= a->steps ? a->steps - 1 : step;
			}

			return (clock > 0.0f ? (s32)(clock / a->motion.secondsPerFrame) : 0) % a->numframes;
		}
	}

	return -1;
}

/** Texel (x, y) of a picture, bilinear between the four round it, wrapping. */
static void xblaTexSampleWrap(const u8 *rgba, s32 w, s32 h, f32 x, f32 y, u8 *out)
{
	const f32 fx = floorf(x), fy = floorf(y);
	const f32 ax = x - fx, ay = y - fy;
	const s32 x0 = ((s32)fx % w + w) % w, y0 = ((s32)fy % h + h) % h;
	const s32 x1 = (x0 + 1) % w, y1 = (y0 + 1) % h;
	const u8 *p00 = rgba + ((size_t)y0 * w + x0) * 4;
	const u8 *p10 = rgba + ((size_t)y0 * w + x1) * 4;
	const u8 *p01 = rgba + ((size_t)y1 * w + x0) * 4;
	const u8 *p11 = rgba + ((size_t)y1 * w + x1) * 4;

	for (s32 c = 0; c < 4; c++) {
		const f32 top = p00[c] + (p10[c] - p00[c]) * ax;
		const f32 bot = p01[c] + (p11[c] - p01[c]) * ax;

		out[c] = (u8)(top + (bot - top) * ay + 0.5f);
	}
}

u8 *xblaTexLoadAnimFrame(const void *addr, s32 frame, s32 *outWidth, s32 *outHeight)
{
	const s32 n = SDL_AtomicGet(&numAnims);

	for (s32 i = 0; i < n; i++) {
		const struct xblatexanim *a = &anims[i];

		if (a->addr != addr || frame < 0) {
			continue;
		}

		{
			const s32 w = a->width, h = a->height;
			const size_t bytes = (size_t)w * (size_t)h * 4;
			u8 *rgba = malloc(bytes);
			const u8 *src;

			if (!rgba) {
				return NULL;
			}

			*outWidth = w;
			*outHeight = h;

			if (a->steps <= 0) {
				if (frame >= a->numframes) {
					free(rgba);
					return NULL;
				}

				memcpy(rgba, a->frames[frame], bytes);
				return rgba;
			}

			// The step's moment, its frame and where the motion has it. The
			// picture's rows run up its v as the file's UVs do (the decode
			// leaves the bottom row first), so a turn is anticlockwise for a
			// positive rate as the picture is drawn, as Maya shows it
			{
				const f32 t = (f32)frame * a->period / a->steps;
				const s32 f = a->numframes > 1 && a->motion.secondsPerFrame > 0.0f
						? (s32)(t / a->motion.secondsPerFrame) % a->numframes : 0;
				const f32 ang = a->motion.rotate * t * (M_PI / 180.0f);
				const f32 c = cosf(ang), sn = sinf(ang);
				const f32 du = a->motion.scroll[0] * t, dv = a->motion.scroll[1] * t;

				src = a->frames[f];

				for (s32 y = 0; y < h; y++) {
					for (s32 x = 0; x < w; x++) {
						const f32 pu = (x + 0.5f) / w - 0.5f;
						const f32 pv = (y + 0.5f) / h - 0.5f;
						// the picture turned by ang: read where it came from
						const f32 qu = c * pu + sn * pv + 0.5f + du;
						const f32 qv = -sn * pu + c * pv + 0.5f + dv;

						xblaTexSampleWrap(src, w, h, qu * w - 0.5f, qv * h - 0.5f, rgba + ((size_t)y * w + x) * 4);
					}
				}
			}

			return rgba;
		}
	}

	return NULL;
}
/**
 * A picture for a texture the game is already holding, bound at that texture's
 * own address - see xblaTexBindPictureAt() in xblatex.h.
 *
 * Everything else here allocates a stand-in of its own and hands the address
 * back for a display list to bind. This one is handed the address instead: the
 * folder screens' pictures (gefolder.c) are the ROM's, loaded with the
 * conversion's own model and named by lists that are already built, and what
 * the release has is the same picture at eight to sixteen times the size. An
 * entry against that address is the whole of the swap, and forgetting it puts
 * the ROM's own texels back without anything being reloaded.
 *
 * The picture is taken over and freed here. A second bind at the same address
 * replaces what was there.
 */
const void *xblaTexBindPictureAt(const void *addr, u8 *rgba, s32 width, s32 height)
{
	struct xblatexentry *e;
	u32 slot;
	u32 opaque = 0;
	u32 total;

	if (!lock || !addr || !rgba || width <= 0 || height <= 0) {
		free(rgba);
		return NULL;
	}

	SDL_LockMutex(lock);

	e = xblaTexFind(addr);

	if (!e) {
		if (numBound >= XBLATEX_HASHSIZE / 2) {
			SDL_UnlockMutex(lock);
			free(rgba);
			return NULL;
		}

		slot = xblaTexHashOf(addr);

		while (hash[slot].addr) {
			slot = (slot + 1) & (XBLATEX_HASHSIZE - 1);
		}

		e = &hash[slot];
		e->addr = (u8 *)addr;
		e->record = XBLATEX_NOREC;
		e->texnum = -1;
		e->key = NULL;
		numBound++;
	}

	total = (u32)width * (u32)height;

	for (u32 i = 0; i < total; i++) {
		if (rgba[i * 4 + 3] >= XBLATEX_OPAQUE_ALPHA) {
			opaque++;
		}
	}

	free(e->image);
	e->image = rgba;
	e->width = width;
	e->height = height;
	e->alpha = opaque < total;
	e->soft = opaque * 100 < total * XBLATEX_SOFT_PERCENT;

	SDL_UnlockMutex(lock);

	return addr;
}

/**
 * Drops the picture bound at an address, leaving the game's own texels there
 * to draw again. The entry itself stays: the table is open addressed and
 * everything after a hole would be lost to the probe that stops at one.
 */
void xblaTexForgetPicture(const void *addr)
{
	struct xblatexentry *e;

	if (!lock || !addr) {
		return;
	}

	SDL_LockMutex(lock);

	e = xblaTexFind(addr);

	if (e) {
		free(e->image);
		e->image = NULL;
		e->width = 0;
		e->height = 0;
		e->alpha = 0;
		e->soft = 0;
	}

	SDL_UnlockMutex(lock);
}

/**
 * A picture that is one of the ROM's numbered textures, which the texture
 * pack is allowed to repaint - see xblaTexBindTexture() in xblatex.h and the
 * ask in xblaTexLoadReplacement().
 *
 * The ROM's own picture is what is kept here, never the pack's: the pack's is
 * fetched when the renderer asks, so that changing packs changes what is
 * drawn rather than leaving the pack that was selected when the mesh was
 * built painted on it for ever.
 */
const void *xblaTexBindTexture(s32 texturenum, u8 *rgba, s32 width, s32 height)
{
	char key[32];

	if (texturenum < 0) {
		free(rgba);
		return NULL;
	}

	snprintf(key, sizeof(key), "n64_%04x", (u32)texturenum);

	return xblaTexBindPicture(key, rgba, width, height, texturenum);
}

s32 xblaTexImageInfo(const void *addr, s32 *outAlpha, s32 *outSoft)
{
	const struct xblatexentry *e;
	s32 found = 0;

	if (!lock || numBound == 0) {
		return 0;
	}

	SDL_LockMutex(lock);

	e = xblaTexFind(addr);

	if (e && e->image) {
		*outAlpha = e->alpha;
		*outSoft = e->soft;
		found = 1;
	}

	SDL_UnlockMutex(lock);

	return found;
}

/**
 * A bound picture's colour: its mean r, g and b (0-255) over its texels at
 * least half opaque, or over all of them when none is. 0 when the address
 * holds no picture.
 */
s32 xblaTexImageMean(const void *addr, f32 outMean[3])
{
	const struct xblatexentry *e;
	s32 found = 0;

	if (!lock || numBound == 0) {
		return 0;
	}

	SDL_LockMutex(lock);

	e = xblaTexFind(addr);

	if (e && e->image && e->width > 0 && e->height > 0) {
		const u32 total = (u32)e->width * (u32)e->height;
		f64 sum[3] = { 0 }, sumall[3] = { 0 };
		u32 num = 0;

		for (u32 i = 0; i < total; i++) {
			const u8 *p = &e->image[i * 4];

			for (s32 c = 0; c < 3; c++) {
				sumall[c] += p[c];

				if (p[3] >= 0x80) {
					sum[c] += p[c];
				}
			}

			num += p[3] >= 0x80;
		}

		for (s32 c = 0; c < 3; c++) {
			outMean[c] = num ? (f32)(sum[c] / num) : (f32)(sumall[c] / total);
		}
		found = 1;
	}

	SDL_UnlockMutex(lock);

	return found;
}

s32 xblaTexImageEdgeAlpha(const void *addr, s32 *outFirst, s32 *outLast)
{
	const struct xblatexentry *e;
	s32 found = 0;

	if (!lock || numBound == 0) {
		return 0;
	}

	SDL_LockMutex(lock);

	e = xblaTexFind(addr);

	if (e && e->image && e->width > 0 && e->height > 0) {
		const u8 *last = e->image + (size_t)(e->height - 1) * e->width * 4;
		u32 first = 0, lastsum = 0;

		for (s32 x = 0; x < e->width; x++) {
			first += e->image[x * 4 + 3];
			lastsum += last[x * 4 + 3];
		}

		*outFirst = (s32)(first / (u32)e->width);
		*outLast = (s32)(lastsum / (u32)e->width);
		found = 1;
	}

	SDL_UnlockMutex(lock);

	return found;
}

s32 xblaTexHaveTextures(void)
{
	// A picture of its own is not the release's art and is not what the
	// switch is about; the switch is asked again for a record's stand-in in
	// xblaTexLoadReplacement().
	return numBound > 0;
}

u32 xblaTexGetNumRecords(void)
{
	u32 n;

	if (!lock) {
		return 0;
	}

	SDL_LockMutex(lock);
	n = xblaTexOpen() ? numRecords : 0;
	SDL_UnlockMutex(lock);

	return n;
}

u8 *xblaTexDecodeRecord(u32 record, s32 *outWidth, s32 *outHeight)
{
	u8 *rgba;

	if (!lock) {
		return NULL;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords || badRecord[record]) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	rgba = xblaTexDecode(record, outWidth, outHeight);

	SDL_UnlockMutex(lock);

	return rgba;
}

u8 *xblaTexDecodeCube(u32 record, s32 *outSize)
{
	const u8 *a;
	const u8 *b;
	u32 offset, width, height, usize, csize, blockw, bpe, facebytes;
	struct x360fetch fetch;
	u32 dwords[6];
	u8 *compressed = NULL;
	u8 *surface = NULL;
	u8 *rgba = NULL;
	s32 ok = 0;

	if (!lock) {
		return NULL;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords || badRecord[record]) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	a = tables + record * XBLATEX_RECORD;
	b = tables + (numRecords + record) * XBLATEX_RECORD;
	offset = xblaTexBE32(a);
	width = xblaTexBE32(a + 4);
	height = xblaTexBE32(a + 8);
	usize = xblaTexBE32(a + 20);
	csize = xblaTexBE32(a + 24);

	for (u32 k = 0; k < 6; k++) {
		dwords[k] = xblaTexBE32(b + (XBLATEX_FETCH_DWORD + k) * 4);
	}

	x360FetchRead(&fetch, dwords);

	// One face's base level: whole blocks of the stored row width, which for
	// a square power of two is the face itself.
	blockw = fetch.format == X360_FMT_8888 ? 1 : 4;
	bpe = fetch.format == X360_FMT_8888 ? 4 : fetch.format == X360_FMT_DXT1 ? 8 : 16;
	facebytes = ((fetch.pitch + blockw - 1) / blockw) * ((height + blockw - 1) / blockw) * bpe;

	if (width && width == height && width <= XBLATEX_MAXDIM && fetch.width == width &&
			x360FetchSupported(&fetch) && usize >= 6 * facebytes && csize &&
			offset <= stream.size && csize <= stream.size - offset) {
		compressed = malloc(csize);
		surface = malloc(usize);
		rgba = malloc((size_t)width * height * 4 * 6);

		if (compressed && surface && rgba &&
				x360StfsStreamRead(&stream, dataBase + offset, csize, compressed) &&
				x360LzxDecompress(compressed, csize, surface, usize) == usize) {
			ok = 1;

			for (u32 k = 0; k < 6 && ok; k++) {
				ok = x360DecodeTexture(surface + k * facebytes, facebytes, &fetch,
						rgba + (size_t)k * width * height * 4);
			}
		}
	}

	SDL_UnlockMutex(lock);

	free(compressed);
	free(surface);

	if (!ok) {
		sysLogPrintf(LOG_WARNING, "xblatex: record %u is not a cube map this reads", record);
		free(rgba);
		return NULL;
	}

	*outSize = (s32)width;

	return rgba;
}

s32 xblaTexGetEnabled(void)
{
	// (off on a ROM hack's arena: xblaSwitchStageHeld())
	return optEnabled && !xblaSwitchStageHeld();
}

void xblaTexSetEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	if (enabled == optEnabled) {
		return;
	}

	optEnabled = enabled;
	traceNoteEvent("release textures %s", enabled ? "on" : "off");

	// Somebody has just asked for the release's art, so this is where the
	// unpack of their archive belongs - the same trade the meshes' switch
	// makes. Without it the first numbered texture of the next room pays for
	// it on the render thread. Only when there is a package: a failed open is
	// remembered for good, and a player who drops one in later must not find
	// that switching this on and off has closed the door on it.
	if (optEnabled && lock && xblaImportIsAvailable()) {
		SDL_LockMutex(lock);
		xblaTexOpen();
		SDL_UnlockMutex(lock);
	}

	// What a texture holds is decided as it is uploaded, and an upload is kept
	// against the address it came from - so without this the meshes keep the
	// art they already have until something else evicts it.
	videoResetTextureCache();
}

/* -------------------------------------------------------------------------
 * Decoding
 * ------------------------------------------------------------------------- */

/**
 * One record, decompressed and decoded. Called with the lock held.
 *
 * Every failure here is the same failure to the caller - it gets NULL and the
 * stand-in's white is drawn - so the record is marked rather than retried, and
 * the reason is logged once.
 */
static u8 *xblaTexDecode(u32 record, s32 *outWidth, s32 *outHeight)
{
	const u8 *a = tables + record * XBLATEX_RECORD;
	const u8 *b = tables + (numRecords + record) * XBLATEX_RECORD;
	const u32 offset = xblaTexBE32(a);
	const u32 width = xblaTexBE32(a + 4);
	const u32 height = xblaTexBE32(a + 8);
	const u32 usize = xblaTexBE32(a + 20);
	const u32 csize = xblaTexBE32(a + 24);
	struct x360fetch fetch;
	u32 dwords[6];
	u8 *compressed = NULL;
	u8 *surface = NULL;
	u8 *rgba = NULL;

	for (u32 k = 0; k < 6; k++) {
		dwords[k] = xblaTexBE32(b + (XBLATEX_FETCH_DWORD + k) * 4);
	}

	x360FetchRead(&fetch, dwords);

	if (!usize || !csize || !width || !height ||
			width > XBLATEX_MAXDIM || height > XBLATEX_MAXDIM ||
			fetch.width != width || fetch.height != height ||
			!x360FetchSupported(&fetch) ||
			offset > stream.size || csize > stream.size - offset) {
		sysLogPrintf(LOG_WARNING, "xblatex: record %u is %ux%u format %u, "
				"which is not a texture this reads", record, width, height, fetch.format);
		badRecord[record] = 1;
		return NULL;
	}

	compressed = malloc(csize);
	surface = malloc(usize);
	rgba = malloc((size_t)width * height * 4);

	if (!compressed || !surface || !rgba) {
		free(compressed);
		free(surface);
		free(rgba);
		badRecord[record] = 1;
		return NULL;
	}

	if (!x360StfsStreamRead(&stream, dataBase + offset, csize, compressed) ||
			x360LzxDecompress(compressed, csize, surface, usize) != usize ||
			!x360DecodeTexture(surface, usize, &fetch, rgba)) {
		sysLogPrintf(LOG_WARNING, "xblatex: record %u did not decode", record);
		badRecord[record] = 1;
		free(compressed);
		free(surface);
		free(rgba);
		return NULL;
	}

	free(compressed);
	free(surface);

	numDecoded++;

	if (numDecoded <= XBLATEX_DECODELOG) {
		sysLogPrintf(LOG_NOTE, "xblatex: record %u decoded, %ux%u format %u",
				record, width, height, fetch.format);
	}

	*outWidth = (s32)width;
	*outHeight = (s32)height;

	return rgba;
}

s32 xblaTexRecordSize(u32 record, s32 *outWidth, s32 *outHeight)
{
	const u8 *a;

	if (!lock) {
		return 0;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords) {
		SDL_UnlockMutex(lock);
		return 0;
	}

	a = tables + record * XBLATEX_RECORD;
	*outWidth = (s32)xblaTexBE32(a + 4);
	*outHeight = (s32)xblaTexBE32(a + 8);

	SDL_UnlockMutex(lock);

	return *outWidth > 0 && *outHeight > 0;
}

/**
 * The N64 size a record stands in for - equal to its own size for the
 * release's own art, and the ROM tile's size for a replacement.
 */
s32 xblaTexRecordSrcSize(u32 record, s32 *outWidth, s32 *outHeight)
{
	const u8 *a;

	if (!lock) {
		return 0;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords) {
		SDL_UnlockMutex(lock);
		return 0;
	}

	a = tables + record * XBLATEX_RECORD;
	*outWidth = (s32)xblaTexBE32(a + 12);
	*outHeight = (s32)xblaTexBE32(a + 16);

	SDL_UnlockMutex(lock);

	return *outWidth > 0 && *outHeight > 0;
}

s32 xblaTexRecordIsSoft(u32 record)
{
	s32 width = 0;
	s32 height = 0;
	u8 *rgba;
	u32 opaque = 0;
	u32 total;

	if (!lock) {
		return 0;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords || badRecord[record]) {
		SDL_UnlockMutex(lock);
		return 0;
	}

	if (softRecord[record] == XBLATEX_SOFT_UNKNOWN) {
		// Decoded once here and thrown away; the render thread decodes it
		// again when the list draws, which it would have done anyway.
		rgba = xblaTexDecode(record, &width, &height);

		if (!rgba) {
			SDL_UnlockMutex(lock);
			return 0;
		}

		total = (u32)width * (u32)height;

		for (u32 i = 0; i < total; i++) {
			if (rgba[i * 4 + 3] >= XBLATEX_OPAQUE_ALPHA) {
				opaque++;
			}
		}

		free(rgba);

		softRecord[record] = (opaque * 100 < total * XBLATEX_SOFT_PERCENT)
				? XBLATEX_SOFT_YES : XBLATEX_SOFT_NO;

		sysLogPrintf(LOG_NOTE, "xblatex: record %u is %ux%u with %u of %u texels opaque%s",
				record, width, height, opaque, total,
				softRecord[record] == XBLATEX_SOFT_YES ? " - soft, drawn blended" : "");
	}

	SDL_UnlockMutex(lock);

	return softRecord[record] == XBLATEX_SOFT_YES;
}

/**
 * Where a record's alpha is, for a mesh to sort its triangles by.
 *
 * xblaTexRecordIsSoft() asks it of the whole picture, and a picture is an
 * atlas: the Villa's tables draw their glass top from a dark pane at a flat
 * 140 in the corner of record 4148 and their shadow from a soft black blob in
 * the corner of record 4117, and both records are otherwise wood and leather
 * at 255. Nine texels in ten opaque says cutout, and a cutout of a pane at 140
 * is a solid pane - the glass drew opaque and the shadow as a black square.
 * So the question has to be asked where a triangle samples, and this is what
 * it is asked of: the record's alpha, point sampled down to a fixed size so a
 * cutout's hard edge stays hard rather than being averaged into a pane.
 *
 * Only a record with a tenth of a percent of its texels between clear and
 * opaque gets one; for the rest there is nothing a triangle could find.
 */
const u8 *xblaTexRecordAlphaMap(u32 record, s32 *outSize)
{
	s32 width = 0;
	s32 height = 0;
	const u8 *map;

	*outSize = XBLATEX_ALPHAMAP;

	if (!lock) {
		return NULL;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || record >= numRecords || badRecord[record]) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	if (!alphaTried[record]) {
		u8 *rgba = xblaTexDecode(record, &width, &height);

		alphaTried[record] = 1;

		if (rgba) {
			const u32 total = (u32)width * (u32)height;
			u32 mid = 0;
			u32 clear = 0;
			u32 midtint = 0;
			u8 midlo = 0xff;
			u8 midhi = 0;

			for (u32 i = 0; i < total; i++) {
				const u8 a = rgba[i * 4 + 3];

				if (a >= 0x10 && a < XBLATEX_OPAQUE_ALPHA) {
					mid++;
					midlo = a < midlo ? a : midlo;
					midhi = a > midhi ? a : midhi;

					if (a < 0x80) {
						midtint++;
					}
				} else if (a < 0x10) {
					clear++;
				}
			}

			// A flat pane: no texel clear, and every one between clear and
			// opaque within a narrow band - glass, not a fringe.
			flatPane[record] = clear == 0 && mid * 100 >= total && midhi - midlo <= XBLATEX_FLATPANE_BAND;

			// Or a record that is glass nearly all over: the dataDyne
			// sniper's visor (4909) is a tint from 0x10 to 0x6f over nine
			// tenths of its picture, a purple stripe at 255 down the middle
			// and a few clear texels at the rim - too wide a band for the
			// test above, so it drew as a solid pale slab (F3 report
			// 20260927-001258). Hair is a fifth or more clear, and the
			// sunglasses' lenses (4870) a third of theirs, all above 0x80.
			if (!flatPane[record] && clear * 1000 <= total && mid * 2 >= total
					&& midtint * 10 >= mid * 9) {
				flatPane[record] = 1;
			}

			if (flatPane[record]) {
				sysLogPrintf(LOG_NOTE, "xblatex: record %u is a flat pane, alpha %u to %u over %u of %u texels",
						record, midlo, midhi, mid, total);
			}

			if (mid * 1000 >= total) {
				u8 *m = malloc(XBLATEX_ALPHAMAP * XBLATEX_ALPHAMAP);

				if (m) {
					for (s32 y = 0; y < XBLATEX_ALPHAMAP; y++) {
						const s32 sy = (y * height + height / 2) / XBLATEX_ALPHAMAP;

						for (s32 x = 0; x < XBLATEX_ALPHAMAP; x++) {
							const s32 sx = (x * width + width / 2) / XBLATEX_ALPHAMAP;

							m[y * XBLATEX_ALPHAMAP + x] = rgba[((u32)sy * (u32)width + (u32)sx) * 4 + 3];
						}
					}

					alphaMap[record] = m;
				}
			}

			free(rgba);
		}
	}

	map = alphaMap[record];

	SDL_UnlockMutex(lock);

	return map;
}

/**
 * Whether a record's partial alpha is a flat pane of glass rather than the
 * fringe of a cutout: no texel is clear, at least one in a hundred is between
 * clear and opaque, and all of those sit within XBLATEX_FLATPANE_BAND of each
 * other. Hair runs smoothly from clear to opaque, and the sunglasses' lenses
 * spread over 80 levels; the DD shock trooper's visor is a quarter of record
 * 0x132c at 119 to 136 beside a helmet and a face at 255. Across the release,
 * the only record a character's or a gun's mesh uses that passes is that one.
 */
s32 xblaTexRecordIsFlatPane(u32 record)
{
	s32 size;

	xblaTexRecordAlphaMap(record, &size);

	return flatPane && record < numRecords && flatPane[record];
}

s32 xblaTexRecordOf(const void *addr)
{
	const struct xblatexentry *e;
	s32 record;

	if (numBound == 0 || !lock) {
		return -1;
	}

	SDL_LockMutex(lock);

	e = xblaTexFind(addr);
	record = e ? (s32)e->record : -1;

	SDL_UnlockMutex(lock);

	return record;
}

u8 *xblaTexLoadReplacement(const void *addr, s32 *outWidth, s32 *outHeight)
{
	struct xblatexentry *e;
	s32 record;
	s32 kept;
	u8 *rgba;

	if (numBound == 0 || !lock) {
		return NULL;
	}

	// A picture of its own is handed over as it is, whatever the switch says:
	// it is a model pack's, not the release's.
	SDL_LockMutex(lock);

	e = xblaTexFind(addr);

	if (e && e->image) {
		const s32 texnum = e->texnum;

		SDL_UnlockMutex(lock);

		// Unless it is one of the ROM's numbered textures, in which case the
		// texture pack's picture for that number is what belongs on it - asked
		// here, at the point the renderer wants the tile, rather than baked in
		// when the mesh was built. That is what lets a texture pack repaint a
		// model pack's mesh: F8, a pack switched or a pack reloaded all drop
		// the renderer's cache, and every tile comes back through here.
		//
		// Asked outside the lock, as the record's replacement below is: the
		// first ask is what makes texpack read the pack off the disk, and the
		// mesh builder on the game thread wants this lock for every material.
		if (texnum >= 0) {
			rgba = texpackDecodeReplacementNow(texnum, outWidth, outHeight);

			if (rgba) {
				return rgba;
			}

			// Failing that, the release's own picture for that number, which
			// is what the renderer draws on the ROM's copy of this texture
			// while the switch is on - so a model pack's mesh is painted the
			// same way the room around it is.
			rgba = xblaTexLoadNumbered(texnum, outWidth, outHeight);

			if (rgba) {
				return rgba;
			}
		}

		SDL_LockMutex(lock);

		e = xblaTexFind(addr);

		if (e && e->image) {
			const size_t bytes = (size_t)e->width * (size_t)e->height * 4;

			rgba = malloc(bytes);

			if (rgba) {
				memcpy(rgba, e->image, bytes);
				*outWidth = e->width;
				*outHeight = e->height;
			}
		} else {
			rgba = NULL;
		}

		SDL_UnlockMutex(lock);

		return rgba;
	}

	record = e ? (s32)e->record : -1;
	kept = e ? e->kept : 0;

	SDL_UnlockMutex(lock);

	if (record < 0 || (!xblaTexGetEnabled() && !kept)) {
		return NULL;
	}

	// The player's own picture for this record, if a pack ships one. Asked
	// outside the lock: the first ask is what makes texpack read the pack off
	// the disk, and the mesh builder on the game thread wants this lock for
	// every material it writes.
	//
	// It answers NULL while the decode is queued, which is the same answer it
	// gives the numbered textures, and means the release's own art below is
	// drawn until the image lands rather than a frame of nothing.
	if (texpackHaveXblaReplacement(record)) {
		rgba = texpackLoadXblaReplacement(record, outWidth, outHeight);

		if (rgba) {
			return rgba;
		}
	}

	SDL_LockMutex(lock);

	e = xblaTexFind(addr);

	if (!e || opened <= 0 || badRecord[e->record]) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	rgba = xblaTexDecode(e->record, outWidth, outHeight);

	SDL_UnlockMutex(lock);

	// The one picture in the game that nothing else can write out: it is not
	// in the ROM and has no texture number, and somebody painting over the
	// meshes' art needs it and needs to know which record it was.
	if (rgba && texpackDumpEnabled()) {
		texpackDumpXblaRecord(rgba, (u32)*outWidth, (u32)*outHeight, (u32)record);
	}

	return rgba;
}

/**
 * The release's picture for one of the game's own textures. See xblatex.h.
 *
 * Everything a pack's <texnum>.png would have gone through is skipped: this is
 * the decode the conversion writes out, handed over where the renderer would
 * have uploaded the ROM's texels. It is the same decode the meshes' records
 * take, so a texture costs one LZX chunk per fill of the renderer's cache.
 */
s32 xblaTexHaveNumbered(void)
{
	// opened is 0 untried and -1 no package: once a package has failed to
	// open there is nothing to ask for, and asking again per texture would
	// re-scan for one. Availability is a flag read after the startup scan.
	return xblaTexGetEnabled() && opened >= 0 && xblaImportIsAvailable();
}

u8 *xblaTexLoadNumbered(s32 texturenum, s32 *outWidth, s32 *outHeight)
{
	u8 *rgba;

	if (!lock || !xblaTexGetEnabled() || texturenum < 0 || texturenum >= NUM_TEXTURES ||
			texturenum >= XBLAIMPORT_NUM_REPLACED ||
			xblaImportTextureIsLeftOut(texturenum)) {
		return NULL;
	}

	SDL_LockMutex(lock);

	if (!xblaTexOpen() || (u32)texturenum >= numRecords || badRecord[texturenum]) {
		SDL_UnlockMutex(lock);
		return NULL;
	}

	rgba = xblaTexDecode((u32)texturenum, outWidth, outHeight);

	SDL_UnlockMutex(lock);

	return rgba;
}

void xblaTexFreeReplacement(u8 *rgba)
{
	free(rgba);
}

void xblaTexShutdown(void)
{
	if (lock) {
		SDL_LockMutex(lock);
	}

	// Decodes against bound records. A decode is one LZX stream and it happens
	// on the render thread, so the two numbers being far apart means the
	// texture cache is evicting these and paying for them again - which is the
	// thing to look at if a mesh-heavy room stutters.
	if (numBound || numDecoded) {
		sysLogPrintf(LOG_NOTE, "xblatex: %d records bound, %d decodes",
				numBound, numDecoded);
	}

	if (opened > 0) {
		x360StfsStreamClose(&stream);
		x360StfsClose(&stfs);
		free(tables);
		tables = NULL;
		opened = -1;
	}

	if (lock) {
		SDL_UnlockMutex(lock);
	}
}

void xblaTexTrace(FILE *f)
{
	fprintf(f, "xblatex: enabled %d opened %d (0 untried, 1 open, -1 no package), %u records, %d bound, %d decodes, %d replaced by the pack\n",
			xblaTexGetEnabled(), opened, numRecords, numBound, numDecoded,
			texpackGetNumXblaReplacements());
}
