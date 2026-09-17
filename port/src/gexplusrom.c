/**
 * GE-X Plus from the player's GoldenEye ROM, converted once at startup.
 *
 * A GoldenEye 007 (US) ROM dropped in data/ beside Perfect Dark's - any name,
 * any of the three dump byte orders - is found by its contents and converted
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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
#include "gexplusrom.h"

#define GEXPLUSROM_DIR "GoldenEye Arenas"
#define GEXPLUSROM_STAMP "CONVERT.txt"
#define GEXPLUSROM_STAMP_LINE "geconvert " GECONVERT_VERSION_STR

// what the GE-X Plus menu says about it
static s32 g_GexPlusRomState = GEXPLUSROM_NONE;

s32 gexPlusRomGetState(void)
{
	return g_GexPlusRomState;
}

/* ------------------------------------------------------------------------ */
/* finding the ROM */

struct romsearch {
	char path[FS_MAXPATH + 1];
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

	snprintf(path, sizeof(path), "$B/%s", name);

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

static void noticeDraw(s32 done, s32 total)
{
	char line[64];
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
	gdl = noticeText(gdl, "CONVERTING GOLDENEYE 007 FOR GE-X PLUS", 100);

	gDPSetFillColor(gdl++, rgba5551(160, 160, 160));
	snprintf(line, sizeof(line), "ONCE ONLY - %d/%d", done, total);
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

void gexPlusRomConvert(void)
{
	static const char *const containers[] = { "$E/mods", "$H/mods" };
	struct romsearch search = { "" };
	char dest[FS_MAXPATH + 1] = "";
	char temp[FS_MAXPATH + 1];
	struct job *job;
	SDL_Thread *thread;
	u32 romlen = 0;
	u8 *rom;

	if (sysArgCheck("--no-ge-convert")) {
		return;
	}

	// what a conversion that did not finish left, whether or not the ROM is
	// still here: it has files/ and would be listed as a mod
	for (u32 i = 0; i < ARRAYCOUNT(containers); ++i) {
		snprintf(temp, sizeof(temp), "%s/" GEXPLUSROM_DIR ".converting", containers[i]);
		if (fsFileSize(temp) >= 0) {
			modRemoveDirTree(temp);
		}
	}

	fsScanDir("$B", gexPlusRomScanEntry, &search);

	if (!search.path[0]) {
		// The arenas may be there from before, converted where the ROM was
		for (u32 i = 0; i < ARRAYCOUNT(containers); ++i) {
			snprintf(dest, sizeof(dest), "%s/" GEXPLUSROM_DIR "/modconfig.txt", containers[i]);
			if (fsFileSize(dest) >= 0) {
				g_GexPlusRomState = GEXPLUSROM_READY;
				return;
			}
		}
		sysLogPrintf(LOG_NOTE, "gexplus: no GoldenEye 007 (US) ROM in data/; GE-X Plus has no arenas");
		return;
	}

	// done already where it was done before, else beside the executable where
	// that can be written
	for (u32 i = 0; i < ARRAYCOUNT(containers); ++i) {
		char dir[FS_MAXPATH + 1];
		snprintf(dir, sizeof(dir), "%s/" GEXPLUSROM_DIR, containers[i]);
		if (gexPlusRomIsCurrent(dir)) {
			g_GexPlusRomState = GEXPLUSROM_READY;
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
			fprintf(f, GEXPLUSROM_STAMP_LINE "\nConverted from %s\n", fsFullPath(search.path));
			fclose(f);
		}

		const s32 existed = fsFileSize(dest) >= 0;

		if (existed) {
			modRemoveDirTree(dest);
		}

		if (fsRename(temp, dest) == 0) {
			sysLogPrintf(LOG_NOTE, "gexplus: the GoldenEye arenas are in %s", dest);
			g_GexPlusRomState = GEXPLUSROM_READY;

			// GE-X Plus lists them through the Stage Loader, which mounts only
			// the mods it is set to: on the first conversion, this one. A
			// player who turns it off later keeps that across a reconversion.
			// Saved now rather than on exit, which a crash or a killed
			// process never reaches: the directory exists from here on, so
			// this is not asked again.
			if (!existed) {
				modMapsEnableByName(GEXPLUSROM_DIR);
				configSave(CONFIG_PATH);
			}
		} else {
			sysLogPrintf(LOG_WARNING, "gexplus: could not move %s to %s", temp, dest);
			g_GexPlusRomState = GEXPLUSROM_FAILED;
		}
	} else {
		sysLogPrintf(LOG_WARNING, "gexplus: the GoldenEye conversion failed: %s", job->err);
		modRemoveDirTree(temp);
		g_GexPlusRomState = fsFileSize(dest) >= 0 ? GEXPLUSROM_READY : GEXPLUSROM_FAILED;
	}

	free(job);
	sysMemFree(rom);
}
