/**
 * GE Plus from the player's GoldenEye ROM, converted once at startup.
 *
 * A GoldenEye 007 (US) ROM dropped in added-content/ (fs.h; or in data/ beside
 * Perfect Dark's, where it went until 2026-09-21) - any name, any of the three
 * dump byte orders - is found by its contents and converted
 * by geconvert.c into mods/GoldenEye Arenas/, which the Stage Loader then
 * mounts like any other mod's maps. GoldenEye's data cannot be shipped, so
 * this is how the remake's arenas reach a player at all.
 *
 * Done once: CONVERT.txt in the directory names the converter that wrote it,
 * and a directory from an older converter (or none, as the one the Python
 * script wrote before this) is converted again. The conversion writes into a
 * directory of its own and replaces the old one only when it has finished,
 * so a failure or a closed window leaves what was there. It runs before the
 * mods are mounted, on a thread of its own, while the window says what is
 * happening - a second of black at startup with no word of why looks like a
 * hang.
 */
#define _DEFAULT_SOURCE 1 // realpath
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <SDL.h>
#include <ultra64.h>
#include "constants.h"
#include "platform.h"
#include "config.h"
#include "fs.h"
#include "mod.h"
#include "system.h"
#include "video.h"
#include "geconvert.h"
#include "rompatch.h"
#include "archive.h"
#include "gexplusrom.h"

#define GEXPLUSROM_STAMP "CONVERT.txt"
#define GEXPLUSROM_STAMP_LINE "geconvert " GECONVERT_VERSION_STR

// what the GE Plus menu says about it
static s32 g_GexPlusRomState = GEXPLUSROM_NONE;

// Whether this player has been given the arenas' maps once (Mod.GexPlusMapsOffered)
static s32 g_GexPlusMapsOffered;

s32 gexPlusRomGetState(void)
{
	return g_GexPlusRomState;
}

/**
 * The arenas are here. GE Plus lists them through the Stage Loader, which
 * mounts only the mods it is set to (Mod.MapMods), and unmounted they are no
 * use to it: every row of its menu greys out, and until now nothing said why.
 * So the first time the arenas are here, turn them on.
 *
 * "The first time" used to be the conversion writing the directory, which
 * misses two whole cases: arenas converted by a build from before that line,
 * and a conversion that ran again over an existing directory after a geconvert
 * version bump. Neither ever got the offer, and neither had any way to work
 * out what was missing - that is the problem report of 2026-09-17, "cANT
 * SELECT ANY OPTIONS FOR gOLDENEYE X. hAVE EVERYTHING INSTALLED BUT EVERYTHING
 * IS GRAYED OUT", whose Mod.MapMods was empty with the arenas installed. The
 * marker is a setting of its own now, so it is asked once per player and
 * survives both. A player who had turned the arenas off before this setting
 * existed gets them back once, and can turn them off again for good.
 */
static void gexPlusRomSetReady(void)
{
	g_GexPlusRomState = GEXPLUSROM_READY;

	if (g_GexPlusMapsOffered) {
		return;
	}

	g_GexPlusMapsOffered = 1;
	modMapsEnableByName(GEXPLUSROM_DIR);

	// Saved now rather than on exit, which a crash or a killed process never
	// reaches: this is not asked again.
	configSave(CONFIG_PATH);
}

PD_CONSTRUCTOR static void gexPlusRomConfigInit(void)
{
	configRegisterInt("Mod.GexPlusMapsOffered", &g_GexPlusMapsOffered, 0, 1);
}

/* ------------------------------------------------------------------------ */
/* finding the ROM */

struct romsearch {
	char path[FS_MAXPATH + 1];
	const char *dir; // the folder being scanned
};

static void gexPlusRomScanEntry(const char *name, void *arg)
{
	struct romsearch *search = arg;
	char path[FS_MAXPATH + 1];
	u8 head[0x40];
	FILE *f;

	if (search->path[0]) {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", search->dir, name);

	// only a file of GoldenEye's size is opened, and only its header read
	if (fsFileSize(path) != GECONVERT_ROM_SIZE) {
		return;
	}

	f = fsFileOpenRead(path);
	if (!f) {
		return;
	}

	if (fread(head, 1, sizeof(head), f) == sizeof(head) && geconvertHeaderIsGoldenEyeUs(head, sizeof(head))) {
		snprintf(search->path, sizeof(search->path), "%s", path);
	}

	fclose(f);
}

/* ------------------------------------------------------------------------ */
/* the notice: a pixel font drawn with fill rectangles, since nothing of the
 * game - its fonts included - is loaded yet */

static const struct {
	char c;
	u8 rows[7];
} g_Glyphs[] = {
	{ 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
	{ 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
	{ 'D', { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } },
	{ 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
	{ 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
	{ 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
	{ 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
	{ 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
	{ 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
	{ 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } },
	{ 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
	{ 'Y', { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 } },
	{ 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
	{ '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
	{ '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
	{ '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
	{ '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
	{ '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
	{ '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
	{ '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
	{ '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
	{ '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
	{ '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
	{ '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
};

#define NOTICE_MAX_GFX 4096

static Gfx g_NoticeGfx[NOTICE_MAX_GFX];
static u16 g_NoticeColourImage[4]; // any address that is not the depth buffer's

static u32 rgba5551(u32 r, u32 g, u32 b)
{
	return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
}

static Gfx *noticeRect(Gfx *gdl, s32 x0, s32 y0, s32 x1, s32 y1)
{
	if (gdl - g_NoticeGfx < NOTICE_MAX_GFX - 2 && x1 >= x0 && y1 >= y0) {
		gDPFillRectangle(gdl++, x0, y0, x1, y1);
	}
	return gdl;
}

static Gfx *noticeText(Gfx *gdl, const char *text, s32 y)
{
	const s32 width = (s32)strlen(text) * 6 - 1;
	s32 x = (320 - width) / 2;

	for (const char *p = text; *p; ++p, x += 6) {
		const u8 *rows = NULL;

		for (u32 i = 0; i < ARRAYCOUNT(g_Glyphs); ++i) {
			if (g_Glyphs[i].c == *p) {
				rows = g_Glyphs[i].rows;
				break;
			}
		}

		if (!rows) {
			continue;
		}

		// a rectangle a run of lit pixels
		for (s32 r = 0; r < 7; ++r) {
			for (s32 c = 0; c < 5;) {
				s32 end = c;

				if (!(rows[r] & (0x10 >> c))) {
					++c;
					continue;
				}

				while (end + 1 < 5 && (rows[r] & (0x10 >> (end + 1)))) {
					++end;
				}

				gdl = noticeRect(gdl, x + c, y + r, x + end, y + r);
				c = end + 1;
			}
		}
	}

	return gdl;
}

void gexPlusRomNotice(const char *title, const char *line, s32 done, s32 total)
{
	Gfx *gdl = g_NoticeGfx;
	const s32 barw = 160;
	const s32 fill = total > 0 ? barw * done / total : 0;

	gDPSetColorImage(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, g_NoticeColourImage);
	gDPSetScissor(gdl++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
	gDPSetCycleType(gdl++, G_CYC_FILL);
	gDPSetRenderMode(gdl++, G_RM_NOOP, G_RM_NOOP2);

	gDPSetFillColor(gdl++, rgba5551(0, 0, 0));
	gdl = noticeRect(gdl, 0, 0, 319, 239);

	gDPSetFillColor(gdl++, rgba5551(255, 255, 255));
	gdl = noticeText(gdl, title, 100);

	gDPSetFillColor(gdl++, rgba5551(160, 160, 160));
	gdl = noticeText(gdl, line, 114);

	gDPSetFillColor(gdl++, rgba5551(90, 90, 90));
	gdl = noticeRect(gdl, 80, 130, 80 + barw - 1, 133);
	gDPSetFillColor(gdl++, rgba5551(200, 160, 40));
	gdl = noticeRect(gdl, 80, 130, 80 + fill - 1, 133);

	gSPEndDisplayList(gdl++);

	videoStartFrame();
	videoSubmitCommands(g_NoticeGfx);
	videoEndFrame();
}

// what the notice says is being converted: GE Plus's GoldenEye or a ROM hack
static char g_NoticeTitle[64] = "CONVERTING GOLDENEYE 007 FOR GE PLUS";

static void noticeDraw(s32 done, s32 total)
{
	char line[64];

	snprintf(line, sizeof(line), "ONCE ONLY - %d/%d", done, total);
	gexPlusRomNotice(g_NoticeTitle, line, done, total);
}

/* ------------------------------------------------------------------------ */
/* the conversion's thread */

struct job {
	u8 *rom;
	u32 romlen;
	char outdir[FS_MAXPATH + 1];
	char err[256];
	SDL_atomic_t done;
	s32 ok;
};

// the converter's report lines, handed from its thread to the log on this one
static SDL_mutex *g_LogLock;
static char g_LogLines[64][160];
static s32 g_NumLogLines;

static void gexPlusRomLog(const char *msg)
{
	SDL_LockMutex(g_LogLock);
	if (g_NumLogLines < (s32)ARRAYCOUNT(g_LogLines)) {
		snprintf(g_LogLines[g_NumLogLines++], sizeof(g_LogLines[0]), "%s", msg);
	}
	SDL_UnlockMutex(g_LogLock);
}

static void gexPlusRomFlushLog(void)
{
	SDL_LockMutex(g_LogLock);
	for (s32 i = 0; i < g_NumLogLines; ++i) {
		sysLogPrintf(LOG_NOTE, "%s", g_LogLines[i]);
	}
	g_NumLogLines = 0;
	SDL_UnlockMutex(g_LogLock);
}

static int gexPlusRomWorker(void *arg)
{
	struct job *job = arg;

	job->ok = geconvertRun(job->rom, job->romlen, job->outdir, job->err, sizeof(job->err));
	SDL_AtomicSet(&job->done, 1);
	return 0;
}

/* ------------------------------------------------------------------------ */

static s32 gexPlusRomIsCurrent(const char *dir)
{
	char path[FS_MAXPATH + 1];
	char first[64] = "";
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, GEXPLUSROM_STAMP);
	f = fsFileOpenRead(path);

	if (!f) {
		return 0;
	}

	if (!fgets(first, sizeof(first), f)) {
		first[0] = '\0';
	}

	fclose(f);
	return !strncmp(first, GEXPLUSROM_STAMP_LINE, strlen(GEXPLUSROM_STAMP_LINE))
		&& (first[strlen(GEXPLUSROM_STAMP_LINE)] == '\n' || first[strlen(GEXPLUSROM_STAMP_LINE)] == '\0');
}

// the ROM the startup scan found, "" for none: read again by the asset dump
// for GoldenEye's own model names (gexPlusRomReadNames())
static char g_GexPlusRomPath[FS_MAXPATH + 1];

s32 gexPlusRomReadNames(void (*fn)(void *arg, int kind, int num, const char *file), void *arg)
{
	u32 romlen = 0;
	u8 *rom;
	s32 ok;

	if (!g_GexPlusRomPath[0]) {
		return 0;
	}

	rom = fsFileLoad(g_GexPlusRomPath, &romlen);

	if (!rom) {
		return 0;
	}

	ok = geconvertReadNames(rom, romlen, fn, arg);
	sysMemFree(rom);

	return ok;
}

/**
 * Converts rom into dest - by way of dest.converting, which replaces dest only
 * once it is finished - with the notice drawn while the converter works on a
 * thread of its own, and stamps it as converted from `from`. 1 when dest holds
 * the new conversion, 0 when the conversion failed (err says why; dest is as
 * it was), -1 when it could not be moved into place.
 */
static s32 gexPlusRomConvertInto(u8 *rom, u32 romlen, const char *dest, const char *from, char *err, u32 errlen)
{
	char temp[FS_MAXPATH + 1];
	struct job *job;
	SDL_Thread *thread;
	s32 result;

	snprintf(temp, sizeof(temp), "%s.converting", dest);

	job = calloc(1, sizeof(*job));
	job->rom = rom;
	job->romlen = romlen;
	snprintf(job->outdir, sizeof(job->outdir), "%s", fsFullPath(temp));
	g_LogLock = SDL_CreateMutex();
	geconvertSetLog(gexPlusRomLog);

	thread = SDL_CreateThread(gexPlusRomWorker, "geconvert", job);

	if (!thread) {
		gexPlusRomWorker(job);
	}

	// the window's first frames, with the notice, while it works
	videoUpdateNativeResolution(320, 240);

	while (!SDL_AtomicGet(&job->done)) {
		noticeDraw(geconvertProgress(), geconvertTotal());
		gexPlusRomFlushLog();
		SDL_Delay(16);
	}

	if (thread) {
		SDL_WaitThread(thread, NULL);
	}

	noticeDraw(geconvertTotal(), geconvertTotal());
	gexPlusRomFlushLog();
	geconvertSetLog(NULL);
	SDL_DestroyMutex(g_LogLock);
	g_LogLock = NULL;

	if (job->ok) {
		char stamp[FS_MAXPATH + 1];
		FILE *f;

		snprintf(stamp, sizeof(stamp), "%s/" GEXPLUSROM_STAMP, temp);
		f = fsFileOpenWrite(stamp);
		if (f) {
			fprintf(f, GEXPLUSROM_STAMP_LINE "\nConverted from %s\n", from);
			fclose(f);
		}

		if (fsFileSize(dest) >= 0) {
			modRemoveDirTree(dest);
		}

		if (fsRename(temp, dest) == 0) {
			result = 1;
		} else {
			sysLogPrintf(LOG_WARNING, "gexplus: could not move %s to %s", temp, dest);
			result = -1;
		}
	} else {
		snprintf(err, errlen, "%s", job->err);
		modRemoveDirTree(temp);
		result = 0;
	}

	free(job);
	return result;
}

static void gexPlusRomConvertGoldenEye(void)
{
	static const char *const containers[] = { "$E/mods", "$H/mods" };
	// data/ is where the ROM went before added-content/ existed
	static const char *const romdirs[] = { FS_ADDED_CONTENT_SEARCH, "$B" };
	struct romsearch search = { "" };
	char dest[FS_MAXPATH + 1] = "";
	char temp[FS_MAXPATH + 1];
	char job_err[256] = "";
	u32 romlen = 0;
	u8 *rom;

	// what a conversion that did not finish left, whether or not the ROM is
	// still here: it has files/ and would be listed as a mod
	for (u32 i = 0; i < ARRAYCOUNT(containers); ++i) {
		snprintf(temp, sizeof(temp), "%s/" GEXPLUSROM_DIR ".converting", containers[i]);
		if (fsFileSize(temp) >= 0) {
			modRemoveDirTree(temp);
		}
	}

	// added-content/ is made here, the first thing at startup that looks in it
	if (fsAddedContentDir(temp, sizeof(temp)) != 0) {
		temp[0] = '\0';
	}

	for (u32 i = 0; i < ARRAYCOUNT(romdirs) && !search.path[0]; ++i) {
		search.dir = romdirs[i];
		fsScanDir(romdirs[i], gexPlusRomScanEntry, &search);
	}

	// A ROM still in data/ from before added-content/ existed moves into it,
	// so the player has one folder and the note in it is true. Left where it
	// is when the rename is refused: data/ is still searched.
	if (search.path[0] && !strcmp(search.dir, "$B") && temp[0]) {
		char to[FS_MAXPATH + 1];

		snprintf(to, sizeof(to), "%s/%s", temp, search.path + strlen("$B/"));

		if (fsFileSize(to) < 0 && fsRename(search.path, to) == 0) {
			sysLogPrintf(LOG_NOTE, "gexplus: moved %s into %s", search.path, fsFullPath(to));
			snprintf(search.path, sizeof(search.path), "%s", to);
		} else {
			sysLogPrintf(LOG_WARNING, "gexplus: could not move %s into added-content/; it is still read where it is", search.path);
		}
	}

	snprintf(g_GexPlusRomPath, sizeof(g_GexPlusRomPath), "%s", search.path);

	if (!search.path[0]) {
		// The arenas may be there from before, converted where the ROM was
		for (u32 i = 0; i < ARRAYCOUNT(containers); ++i) {
			char dir[FS_MAXPATH + 1];

			snprintf(dir, sizeof(dir), "%s/" GEXPLUSROM_DIR, containers[i]);
			snprintf(dest, sizeof(dest), "%s/modconfig.txt", dir);

			if (fsFileSize(dest) >= 0) {
				gexPlusRomSetReady();

				// and say so when they are older than this build: everything a
				// newer converter adds is missing from them and nothing can put
				// it there while there is no ROM to convert again from, which
				// is silent otherwise - the arenas work and the rest does not
				if (!gexPlusRomIsCurrent(dir)) {
					g_GexPlusRomState = GEXPLUSROM_OLD;
					sysLogPrintf(LOG_WARNING, "gexplus: the arenas in %s were converted by an older build"
							" and there is no GoldenEye 007 (US) ROM in " FS_ADDED_CONTENT_DIR "/ to convert again from;"
							" GoldenEye's intro and folder screens need what the newer one writes", dir);
				}

				return;
			}
		}
		sysLogPrintf(LOG_NOTE, "gexplus: no GoldenEye 007 (US) ROM in " FS_ADDED_CONTENT_DIR "/; GoldenEye has no arenas");
		return;
	}

	// done already where it was done before, else beside the executable where
	// that can be written
	for (u32 i = 0; i < ARRAYCOUNT(containers); ++i) {
		char dir[FS_MAXPATH + 1];
		snprintf(dir, sizeof(dir), "%s/" GEXPLUSROM_DIR, containers[i]);
		if (gexPlusRomIsCurrent(dir)) {
			gexPlusRomSetReady();
			return;
		}
		if (!dest[0] && fsFileSize(dir) >= 0) {
			snprintf(dest, sizeof(dest), "%s", dir);
		}
	}

	if (!dest[0]) {
		for (u32 i = 0; i < ARRAYCOUNT(containers); ++i) {
			char probe[FS_MAXPATH + 1];
			FILE *f;

			fsCreateDir(containers[i]);
			snprintf(probe, sizeof(probe), "%s/.gexplus", containers[i]);
			f = fsFileOpenWrite(probe);
			if (f) {
				fclose(f);
				fsRemoveFile(probe);
				snprintf(dest, sizeof(dest), "%s/" GEXPLUSROM_DIR, containers[i]);
				break;
			}
		}
	}

	if (!dest[0]) {
		sysLogPrintf(LOG_WARNING, "gexplus: nowhere to write the GoldenEye arenas");
		g_GexPlusRomState = GEXPLUSROM_FAILED;
		return;
	}

	rom = fsFileLoad(search.path, &romlen);
	if (!rom) {
		g_GexPlusRomState = GEXPLUSROM_FAILED;
		return;
	}

	sysLogPrintf(LOG_NOTE, "gexplus: converting %s into %s, this happens once", fsFullPath(search.path), dest);

	switch (gexPlusRomConvertInto(rom, romlen, dest, fsFullPath(search.path), job_err, sizeof(job_err))) {
	case 1:
		sysLogPrintf(LOG_NOTE, "gexplus: the GoldenEye arenas are in %s", dest);
		gexPlusRomSetReady();
		break;
	case -1:
		g_GexPlusRomState = GEXPLUSROM_FAILED;
		break;
	default:
		sysLogPrintf(LOG_WARNING, "gexplus: the GoldenEye conversion failed: %s", job_err);

		if (fsFileSize(dest) >= 0) {
			// what a previous conversion left is still playable
			gexPlusRomSetReady();
		} else {
			g_GexPlusRomState = GEXPLUSROM_FAILED;
		}
		break;
	}

	sysMemFree(rom);
}

/* ------------------------------------------------------------------------ */
/* GoldenEye ROM hacks: their arenas beside GE Plus's, never part of it */

/*
 * A ROM hack of GoldenEye the converter knows the layout of
 * (geconvertVariantName(): Goldfinger 64) is converted to its arenas into a
 * folder of its own name under mods/, which the Stage Loader mounts like the
 * GoldenEye arenas. It is found in added-content/ as the ROM itself, or as
 * the patch it is passed around as - an .xdelta, .bps or .ips, or a zip of
 * one - which is applied to the GoldenEye 007 (US) ROM there first: the hack's
 * data is no more shippable than GoldenEye's. Done once per source, like
 * GE Plus's: the folder's CONVERT.txt names the converter and the file it
 * came from.
 */
#define VARIANTS_MAX 8

static void variantAdoptFromMods(void);

static char g_Variants[VARIANTS_MAX][64];
static s32 g_NumVariants;

s32 gexPlusRomGetNumVariants(void)
{
	return g_NumVariants;
}

const char *gexPlusRomGetVariant(s32 index)
{
	return index >= 0 && index < g_NumVariants ? g_Variants[index] : NULL;
}

s32 gexPlusRomIsConversionDir(const char *name)
{
	if (!strcasecmp(name, GEXPLUSROM_DIR)) {
		return 1;
	}

	for (s32 i = 0; geconvertVariantNameAt(i); ++i) {
		if (!strcasecmp(name, geconvertVariantNameAt(i))) {
			return 1;
		}
	}

	return 0;
}

static void variantReady(const char *name)
{
	for (s32 i = 0; i < g_NumVariants; ++i) {
		if (!strcmp(g_Variants[i], name)) {
			return;
		}
	}

	if (g_NumVariants < VARIANTS_MAX) {
		snprintf(g_Variants[g_NumVariants++], sizeof(g_Variants[0]), "%s", name);
	}
}

static const char *const g_VariantContainers[] = { "$E/mods", "$H/mods" };

// the second line of a conversion's CONVERT.txt: the file it was made from
static s32 variantStampFrom(const char *dir, char *from, u32 fromlen)
{
	char path[FS_MAXPATH + 1];
	char line[FS_MAXPATH + 64];
	FILE *f;
	s32 ok = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, GEXPLUSROM_STAMP);
	f = fsFileOpenRead(path);

	if (!f) {
		return 0;
	}

	if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f) && !strncmp(line, "Converted from ", 15)) {
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(from, fromlen, "%s", line + 15);
		ok = 1;
	}

	fclose(f);
	return ok;
}

// the ROM hack a current conversion was made from this file, or NULL
static const char *variantConvertedFrom(const char *from)
{
	for (s32 i = 0; geconvertVariantNameAt(i); ++i) {
		for (u32 c = 0; c < ARRAYCOUNT(g_VariantContainers); ++c) {
			char dir[FS_MAXPATH + 1];
			char was[FS_MAXPATH + 1];

			snprintf(dir, sizeof(dir), "%s/%s", g_VariantContainers[c], geconvertVariantNameAt(i));

			if (gexPlusRomIsCurrent(dir) && variantStampFrom(dir, was, sizeof(was)) && !strcmp(was, from)) {
				return geconvertVariantNameAt(i);
			}
		}
	}

	return NULL;
}

// GoldenEye's own ROM in .z64 order, loaded once for the patches that want it
static u8 *g_UsRom;
static u32 g_UsRomLen;

static s32 variantUsRom(void)
{
	if (g_UsRom) {
		return 1;
	}

	if (!g_GexPlusRomPath[0]) {
		return 0;
	}

	g_UsRom = fsFileLoad(g_GexPlusRomPath, &g_UsRomLen);

	if (g_UsRom && !geconvertIsGoldenEyeUs(g_UsRom, g_UsRomLen)) {
		sysMemFree(g_UsRom);
		g_UsRom = NULL;
	}

	return g_UsRom != NULL;
}

/** Converts a ROM hack's ROM (malloc'd or loaded; freed by the caller) into mods/<its name>/. */
static void variantConvert(u8 *rom, u32 romlen, const char *name, const char *from)
{
	char dest[FS_MAXPATH + 1] = "";
	char err[256] = "";
	s32 existed;

	// once a start, whichever of its sources was seen first: two (its zip and
	// the patch out of it) would otherwise convert over each other every time,
	// each finding the folder made from the other
	for (s32 i = 0; i < g_NumVariants; ++i) {
		if (!strcmp(g_Variants[i], name)) {
			return;
		}
	}

	for (u32 c = 0; c < ARRAYCOUNT(g_VariantContainers) && !dest[0]; ++c) {
		char dir[FS_MAXPATH + 1];

		snprintf(dir, sizeof(dir), "%s/%s", g_VariantContainers[c], name);

		if (fsFileSize(dir) >= 0) {
			snprintf(dest, sizeof(dest), "%s", dir);
		}
	}

	for (u32 c = 0; c < ARRAYCOUNT(g_VariantContainers) && !dest[0]; ++c) {
		char probe[FS_MAXPATH + 1];
		FILE *f;

		fsCreateDir(g_VariantContainers[c]);
		snprintf(probe, sizeof(probe), "%s/.gexplus", g_VariantContainers[c]);
		f = fsFileOpenWrite(probe);

		if (f) {
			fclose(f);
			fsRemoveFile(probe);
			snprintf(dest, sizeof(dest), "%s/%s", g_VariantContainers[c], name);
		}
	}

	if (!dest[0]) {
		sysLogPrintf(LOG_WARNING, "gexplus: nowhere to write %s's arenas", name);
		return;
	}

	existed = fsFileSize(dest) >= 0;
	sysLogPrintf(LOG_NOTE, "gexplus: converting %s (%s) into %s, this happens once", name, from, dest);

	snprintf(g_NoticeTitle, sizeof(g_NoticeTitle), "CONVERTING %s", name);
	for (char *c = g_NoticeTitle; *c; ++c) {
		*c = (*c >= 'a' && *c <= 'z') ? *c - 'a' + 'A' : *c;
	}

	if (gexPlusRomConvertInto(rom, romlen, dest, from, err, sizeof(err)) == 1) {
		sysLogPrintf(LOG_NOTE, "gexplus: %s's arenas are in %s", name, dest);
		variantReady(name);

		if (!existed) {
			// listed among the Stage Loader's maps the first time, as the
			// GoldenEye arenas are (gexPlusRomSetReady())
			modMapsEnableByName(name);
			configSave(CONFIG_PATH);
		}
	} else {
		sysLogPrintf(LOG_WARNING, "gexplus: %s's conversion failed: %s", name, err);

		if (existed) {
			variantReady(name);
		}
	}
}

/** A patch at path (from names the file the player put in added-content/). */
static void variantFromPatch(const char *path, const char *from)
{
	char err[256];
	u8 *patch, *out = NULL;
	u32 patchlen = 0, outlen = 0;
	const char *name;

	if (!variantUsRom()) {
		sysLogPrintf(LOG_NOTE, "gexplus: %s may be a GoldenEye ROM hack; it needs the GoldenEye 007 (US) ROM in "
				FS_ADDED_CONTENT_DIR "/ to be applied to", from);
		return;
	}

	patch = fsFileLoad(path, &patchlen);

	if (!patch) {
		return;
	}

	if (rompatchApply(g_UsRom, g_UsRomLen, patch, patchlen, &out, &outlen, err, sizeof(err)) < 0) {
		// a patch for something else: added-content/ holds the releases' too
		sysMemFree(patch);
		return;
	}

	sysMemFree(patch);
	name = geconvertVariantName(out, outlen);

	if (name) {
		variantConvert(out, outlen, name, from);
	} else {
		sysLogPrintf(LOG_NOTE, "gexplus: %s applies to GoldenEye 007 (US) but makes no ROM hack this can convert", from);
	}

	free(out);
}

static s32 variantIsDir(const char *path)
{
	struct stat st;
	return stat(fsFullPath(path), &st) == 0 && S_ISDIR(st.st_mode);
}

struct variantlist {
	char (*names)[256];
	s32 count;
	s32 cap;
};

static void variantListAdd(const char *name, void *arg)
{
	struct variantlist *list = arg;

	if (strlen(name) >= 256) {
		return;
	}

	if (list->count == list->cap) {
		list->cap = list->cap ? list->cap * 2 : 16;
		list->names = realloc(list->names, list->cap * 256);
	}

	snprintf(list->names[list->count++], 256, "%s", name);
}

// the patches in an unpacked zip, a few folders down
static void variantPatchesIn(const char *dir, const char *from, s32 depth)
{
	struct variantlist list = { NULL, 0, 0 };

	if (fsScanDir(dir, variantListAdd, &list) >= 0) {
		for (s32 i = 0; i < list.count; ++i) {
			char path[FS_MAXPATH + 1];

			snprintf(path, sizeof(path), "%s/%s", dir, list.names[i]);

			if (rompatchIsPatchName(list.names[i])) {
				variantFromPatch(path, from);
			} else if (depth < 3 && variantIsDir(path)) {
				variantPatchesIn(path, from, depth + 1);
			}
		}
	}

	free(list.names);
}

// whether an archive holds a ROM patch (archiveFindEntry() wants the path expanded)
static s32 variantArchiveHasPatch(const char *path)
{
	char full[FS_MAXPATH + 1];

	snprintf(full, sizeof(full), "%s", fsFullPath(path));

	return archiveFindEntry(full, ".xdelta") || archiveFindEntry(full, ".vcdiff")
		|| archiveFindEntry(full, ".bps") || archiveFindEntry(full, ".ips");
}

static s32 variantPatchFilter(const char *name, void *arg)
{
	return rompatchIsPatchName(name);
}

/** An archive that holds a patch, unpacked once into cache/romhacks/<its name>/. */
static void variantFromArchive(const char *path, const char *name, const char *from)
{
	char cache[FS_MAXPATH + 1];
	char dir[FS_MAXPATH + 1];
	char stem[128];
	const char *dot = strrchr(name, '.');

	if (!variantArchiveHasPatch(path)) {
		return;
	}

	if (fsChooseOutputDir("cache", cache, sizeof(cache)) != 0) {
		return;
	}

	snprintf(stem, sizeof(stem), "%.*s", (int)(dot ? dot - name : (s32)strlen(name)), name);
	snprintf(dir, sizeof(dir), "%s/romhacks", cache);
	fsCreateDir(dir);
	snprintf(dir, sizeof(dir), "%s/romhacks/%s", cache, stem);

	if (fsFileSize(dir) < 0) {
		char full[FS_MAXPATH + 1];

		fsCreateDir(dir);
		snprintf(full, sizeof(full), "%s", fsFullPath(dir));

		if (archiveExtractMatching(fsFullPath(path), full, variantPatchFilter, NULL) <= 0) {
			modRemoveDirTree(dir);
			return;
		}
	}

	variantPatchesIn(dir, from, 0);
}

struct variantsearch {
	const char *dir;
	struct variantlist files;
};

// a file's path spelt one way however its folder was reached ($E, ./, $H)
static void variantCanonical(const char *path, char *out, u32 outlen)
{
#ifdef PLATFORM_WIN32
	if (_fullpath(out, fsFullPath(path), outlen)) {
		return;
	}
#else
	char *full = realpath(fsFullPath(path), NULL);

	if (full) {
		snprintf(out, outlen, "%s", full);
		free(full);
		return;
	}
#endif
	snprintf(out, outlen, "%s", fsFullPath(path));
}

static void variantScanEntry(const char *name, void *arg)
{
	variantListAdd(name, &((struct variantsearch *)arg)->files);
}

// a ROM hack's patch, zipped, is a few megabytes; the releases beside it in
// added-content/ are hundreds, and their listings are not read for one
#define VARIANT_ARCHIVE_MAX (128 * 1024 * 1024)

static void gexPlusRomConvertVariants(void)
{
	static const char *const dirs[] = { FS_ADDED_CONTENT_SEARCH };
	char seen[ARRAYCOUNT(dirs)][FS_MAXPATH + 1];

	variantAdoptFromMods();

	for (u32 d = 0; d < ARRAYCOUNT(dirs); ++d) {
		struct variantsearch search = { dirs[d], { NULL, 0, 0 } };
		s32 again = 0;

		// $E/added-content and ./added-content are one folder when the game
		// is run from its own: looked through once
		variantCanonical(dirs[d], seen[d], sizeof(seen[d]));

		for (u32 e = 0; e < d && !again; ++e) {
			again = !strcmp(seen[e], seen[d]);
		}

		if (again || fsScanDir(dirs[d], variantScanEntry, &search) < 0) {
			free(search.files.names);
			continue;
		}

		for (s32 i = 0; i < search.files.count; ++i) {
			const char *name = search.files.names[i];
			char path[FS_MAXPATH + 1];
			char from[FS_MAXPATH + 1];
			const char *done;
			s32 size;

			snprintf(path, sizeof(path), "%s/%s", dirs[d], name);
			variantCanonical(path, from, sizeof(from));
			size = fsFileSize(path);

			if (size <= 0 || variantIsDir(path)) {
				continue;
			}

			if ((done = variantConvertedFrom(from)) != NULL) {
				variantReady(done);
				continue;
			}

			if (rompatchIsPatchName(name)) {
				variantFromPatch(path, from);
			} else if (archiveIsSupported(name) && size <= VARIANT_ARCHIVE_MAX) {
				variantFromArchive(path, name, from);
			} else if ((u32)size > GECONVERT_ROM_SIZE) {
				// the hack's ROM itself, patched already
				u8 head[0x40];
				FILE *f = fsFileOpenRead(path);
				s32 isvariant = 0;

				if (f) {
					isvariant = fread(head, 1, sizeof(head), f) == sizeof(head) && geconvertHeaderVariantName(head, sizeof(head));
					fclose(f);
				}

				if (isvariant) {
					u32 romlen = 0;
					u8 *rom = fsFileLoad(path, &romlen);
					const char *variant = rom ? geconvertVariantName(rom, romlen) : NULL;

					if (variant) {
						variantConvert(rom, romlen, variant, from);
					}

					if (rom) {
						sysMemFree(rom);
					}
				}
			}
		}

		free(search.files.names);
	}

	if (g_UsRom) {
		sysMemFree(g_UsRom);
		g_UsRom = NULL;
	}
}

/*
 * A ROM hack's patch dropped where mods go (mods/) rather than in
 * added-content/. The Mod list would import it as a Perfect Dark console mod,
 * which it is not, and leave an IMPORT.txt saying it applies to no Perfect Dark
 * ROM. So a new drop there - a patch nothing has imported yet, an archive
 * nothing has unpacked yet - is tried on GoldenEye's ROM first, and one that
 * makes a hack this converts is moved to added-content/ before the scan below
 * (variantAdoptFromMods()). The Mod list's import asks too, for what this did
 * not see (modListPrepareDir(): an archive unpacked by a build before this, a
 * drop while the game runs), and that converts at the next start.
 */
static s32 variantMoveToAddedContent(const char *path)
{
	char dir[FS_MAXPATH + 1];
	char to[FS_MAXPATH + 1];
	const char *base = strrchr(path, '/');

	if (fsAddedContentDir(dir, sizeof(dir)) != 0) {
		return 0;
	}

	snprintf(to, sizeof(to), "%s/%s", dir, base ? base + 1 : path);

	if (fsFileSize(to) >= 0 || fsRename(path, to) != 0) {
		return 0;
	}

	sysLogPrintf(LOG_NOTE, "gexplus: moved %s into " FS_ADDED_CONTENT_DIR "/, where a GoldenEye ROM hack goes", path);
	return 1;
}

// the hack a patch makes of GoldenEye 007 (US), or NULL
static const char *variantPatchMakes(const char *path)
{
	char err[256];
	u8 *patch, *out = NULL;
	u32 patchlen = 0, outlen = 0;
	const char *name = NULL;

	if (!variantUsRom()) {
		return NULL;
	}

	patch = fsFileLoad(path, &patchlen);

	if (patch && rompatchApply(g_UsRom, g_UsRomLen, patch, patchlen, &out, &outlen, err, sizeof(err)) >= 0) {
		name = geconvertVariantName(out, outlen);
	}

	if (patch) {
		sysMemFree(patch);
	}

	free(out);
	return name;
}

s32 gexPlusRomPatchIsHack(const char *path)
{
	const s32 hadrom = g_UsRom != NULL;
	const char *name = variantPatchMakes(path);

	if (!hadrom && g_UsRom) {
		sysMemFree(g_UsRom);
		g_UsRom = NULL;
	}

	if (name) {
		sysLogPrintf(LOG_NOTE, "gexplus: %s is %s, a GoldenEye ROM hack and not a Perfect Dark mod; it goes in "
				FS_ADDED_CONTENT_DIR "/ and is converted at the next start", path, name);
	}

	return name != NULL;
}

s32 gexPlusRomAdoptFile(const char *path)
{
	return variantMoveToAddedContent(path);
}

// whether container holds a folder of this name (what an import or an unpack made)
static s32 variantHasDir(const char *container, const char *name, s32 stripext)
{
	char path[FS_MAXPATH + 1];
	const char *dot = stripext ? strrchr(name, '.') : NULL;

	snprintf(path, sizeof(path), "%s/%.*s", container, (int)(dot ? dot - name : (s32)strlen(name)), name);
	return fsFileSize(path) >= 0;
}

static void variantAdoptFromMods(void)
{
	for (u32 c = 0; c < ARRAYCOUNT(g_VariantContainers); ++c) {
		struct variantlist list = { NULL, 0, 0 };
		const char *container = g_VariantContainers[c];

		if (fsScanDir(container, variantListAdd, &list) < 0) {
			free(list.names);
			continue;
		}

		for (s32 i = 0; i < list.count; ++i) {
			const char *name = list.names[i];
			char path[FS_MAXPATH + 1];

			snprintf(path, sizeof(path), "%s/%s", container, name);

			if (variantIsDir(path) || variantHasDir(container, name, 1)) {
				continue; // imported or unpacked before: the Mod list's
			}

			if (rompatchIsPatchName(name)) {
				if (variantPatchMakes(path)) {
					variantMoveToAddedContent(path);
				}
			} else if (archiveIsSupported(name) && fsFileSize(path) > 0 && fsFileSize(path) <= VARIANT_ARCHIVE_MAX
					&& variantArchiveHasPatch(path)) {
				char cache[FS_MAXPATH + 1];
				char dir[FS_MAXPATH + 1];
				char full[FS_MAXPATH + 1];
				s32 hack = 0;

				if (fsChooseOutputDir("cache", cache, sizeof(cache)) != 0) {
					continue;
				}

				// unpacked to look at where variantFromArchive() would
				snprintf(dir, sizeof(dir), "%s/romhacks", cache);
				fsCreateDir(dir);
				snprintf(dir, sizeof(dir), "%s/romhacks/%.*s", cache,
						(int)(strrchr(name, '.') ? strrchr(name, '.') - name : (s32)strlen(name)), name);
				fsCreateDir(dir);
				snprintf(full, sizeof(full), "%s", fsFullPath(dir));

				if (archiveExtractMatching(fsFullPath(path), full, variantPatchFilter, NULL) > 0) {
					struct variantlist patches = { NULL, 0, 0 };

					if (fsScanDir(dir, variantListAdd, &patches) >= 0) {
						for (s32 k = 0; k < patches.count && !hack; ++k) {
							char ppath[FS_MAXPATH + 1];

							snprintf(ppath, sizeof(ppath), "%s/%s", dir, patches.names[k]);
							hack = rompatchIsPatchName(patches.names[k]) && variantPatchMakes(ppath) != NULL;
						}
					}

					free(patches.names);
				}

				if (hack) {
					variantMoveToAddedContent(path);
				} else {
					modRemoveDirTree(dir);
				}
			}
		}

		free(list.names);
	}
}

void gexPlusRomConvert(void)
{
	if (sysArgCheck("--no-ge-convert")) {
		return;
	}

	gexPlusRomConvertGoldenEye();
	gexPlusRomConvertVariants();
}
