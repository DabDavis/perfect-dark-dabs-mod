#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include <PR/ultratypes.h>
#include <PR/os_thread.h>
#include <PR/os_cont.h>
#include "platform.h"
#include "config.h"
#include "fs.h"
#include "input.h"
#include "screenshot.h"
#include "system.h"
#include "video.h"

// Beside the executable where that can be written, and in the save directory
// where it cannot - see fsChooseOutputDir(). Worked out on the first shot,
// because it creates the directory and there is no sense doing that for a
// session where nobody presses the key.
#define SCREENSHOT_DIR_NAME "screenshots"
#define SCREENSHOT_KEYNAME_LEN 32
#define SCREENSHOT_DEFAULT_KEY "F12"

// How many names one second's worth of shots may take before giving up. The
// name carries a timestamp to the second, so this only bites when the key is
// held down or mashed.
#define SCREENSHOT_MAX_DUPES 100

static char keyName[SCREENSHOT_KEYNAME_LEN] = SCREENSHOT_DEFAULT_KEY;
static s32 keyVk = -1; // -1 until keyName has been looked up
static bool pending;

/**
 * PNG is a zlib stream wrapped in chunks, and zlib is already a dependency
 * because the ROM is compressed with it, so the whole encoder is these two
 * functions rather than another vendored header.
 */
static void pngWriteU32(FILE *f, u32 val)
{
	const u8 buf[4] = { val >> 24, val >> 16, val >> 8, val };
	fwrite(buf, 1, sizeof(buf), f);
}

static void pngWriteChunk(FILE *f, const char *type, const u8 *data, u32 len)
{
	u32 crc = crc32(0, (const u8 *)type, 4);

	if (len) {
		crc = crc32(crc, data, len);
	}

	pngWriteU32(f, len);
	fwrite(type, 1, 4, f);

	if (len) {
		fwrite(data, 1, len, f);
	}

	pngWriteU32(f, crc);
}

/**
 * rgb holds tightly packed RGB triples with the bottom row first, which is what
 * the renderer reads back and the reverse of the order PNG stores. The flip
 * happens here, while the rows are being copied anyway to make room for the
 * per-row filter byte that PNG puts in front of each one. Filter 0 (none) is
 * used throughout: the frame is already going through deflate, and choosing
 * filters per row would cost more than it saves on a picture this size.
 */
static bool screenshotWritePng(const char *filename, const u8 *rgb, s32 width, s32 height)
{
	const uLong stride = 1 + (uLong)width * 3;
	const uLong rawSize = stride * (uLong)height;
	uLong zSize = compressBound(rawSize);
	u8 *raw = malloc(rawSize);
	u8 *z = malloc(zSize);
	FILE *f = NULL;
	u8 ihdr[13];
	s32 y;

	if (!raw || !z) {
		free(raw);
		free(z);
		sysLogPrintf(LOG_ERROR, "screenshot: could not alloc %lu bytes", (unsigned long)(rawSize + zSize));
		return false;
	}

	for (y = 0; y < height; y++) {
		u8 *dst = raw + stride * y;
		*dst++ = 0;
		memcpy(dst, rgb + (uLong)(height - 1 - y) * width * 3, (uLong)width * 3);
	}

	if (compress2(z, &zSize, raw, rawSize, Z_DEFAULT_COMPRESSION) != Z_OK) {
		free(raw);
		free(z);
		sysLogPrintf(LOG_ERROR, "screenshot: could not compress %s", filename);
		return false;
	}

	free(raw);

	f = fopen(filename, "wb");

	if (!f) {
		free(z);
		sysLogPrintf(LOG_ERROR, "screenshot: could not open %s for writing", filename);
		return false;
	}

	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);

	ihdr[0] = width >> 24;
	ihdr[1] = width >> 16;
	ihdr[2] = width >> 8;
	ihdr[3] = width;
	ihdr[4] = height >> 24;
	ihdr[5] = height >> 16;
	ihdr[6] = height >> 8;
	ihdr[7] = height;
	ihdr[8] = 8;  // bits per channel
	ihdr[9] = 2;  // colour type: truecolour, no alpha
	ihdr[10] = 0; // deflate
	ihdr[11] = 0; // adaptive filtering
	ihdr[12] = 0; // no interlace

	pngWriteChunk(f, "IHDR", ihdr, sizeof(ihdr));
	pngWriteChunk(f, "IDAT", z, zSize);
	pngWriteChunk(f, "IEND", NULL, 0);

	free(z);

	if (ferror(f)) {
		fclose(f);
		sysLogPrintf(LOG_ERROR, "screenshot: could not write %s", filename);
		return false;
	}

	fclose(f);

	return true;
}

/**
 * screenshots/pd-20260901-143012.png, with -2, -3 and so on appended when a
 * second holds more than one shot.
 */
static bool screenshotPickFilename(char *out, u32 outSize)
{
	static char dir[FS_MAXPATH + 1];
	const time_t now = time(NULL);
	const struct tm *lt = localtime(&now);
	char stamp[32];
	s32 dupe;

	if (!dir[0] && fsChooseOutputDir(SCREENSHOT_DIR_NAME, dir, sizeof(dir)) != 0) {
		sysLogPrintf(LOG_ERROR, "screenshot: nowhere to put %s that can be written",
				SCREENSHOT_DIR_NAME);
		return false;
	}

	if (lt) {
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", lt);
	} else {
		snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)now);
	}

	for (dupe = 1; dupe <= SCREENSHOT_MAX_DUPES; dupe++) {
		char rel[FS_MAXPATH + 1];

		if (dupe == 1) {
			snprintf(rel, sizeof(rel), "%s/pd-%s.png", dir, stamp);
		} else {
			snprintf(rel, sizeof(rel), "%s/pd-%s-%d.png", dir, stamp, dupe);
		}

		if (fsFileSize(rel) < 0) {
			strncpy(out, fsFullPath(rel), outSize - 1);
			out[outSize - 1] = '\0';
			return true;
		}
	}

	sysLogPrintf(LOG_WARNING, "screenshot: too many shots in the same second");

	return false;
}

/**
 * Runs with the frame drawn and not yet presented, which is the only point the
 * finished image is still readable.
 */
static void screenshotPreSwap(void)
{
	char filename[FS_MAXPATH + 1];
	const s32 width = videoGetWindowWidth();
	const s32 height = videoGetWindowHeight();
	u8 *rgb;

	if (!pending) {
		return;
	}

	pending = false;

	if (width <= 0 || height <= 0) {
		return;
	}

	rgb = malloc((size_t)width * height * 3);

	if (!rgb) {
		sysLogPrintf(LOG_ERROR, "screenshot: could not alloc %dx%d frame", width, height);
		return;
	}

	if (!videoReadScreenPixels(rgb, width, height)) {
		free(rgb);
		sysLogPrintf(LOG_ERROR, "screenshot: could not read the frame back");
		return;
	}

	if (screenshotPickFilename(filename, sizeof(filename))
			&& screenshotWritePng(filename, rgb, width, height)) {
		sysLogPrintf(LOG_NOTE, "screenshot: %s", filename);
	}

	free(rgb);
}

s32 screenshotGetKey(void)
{
	if (keyVk < 0) {
		// First use. The name is resolved this late rather than at config load
		// because inputInit() is what fills the table it is looked up in.
		// inputGetKeyByName() warns about a name it does not know and returns
		// -1; NONE is the name the bind rows write for nothing bound at all.
		if (!keyName[0] || !strcmp(keyName, "NONE")) {
			keyVk = 0;
		} else {
			keyVk = inputGetKeyByName(keyName);

			if (keyVk < 0) {
				keyVk = 0;
			}
		}
	}

	return keyVk;
}

void screenshotSetKey(s32 vk)
{
	if (vk <= 0 || vk >= VK_TOTAL_COUNT) {
		keyName[0] = '\0';
		keyVk = 0;
		return;
	}

	strncpy(keyName, inputGetKeyName(vk), sizeof(keyName) - 1);
	keyName[sizeof(keyName) - 1] = '\0';
	keyVk = vk;
}

void screenshotRequest(void)
{
	pending = true;
}

void screenshotTick(void)
{
	const s32 vk = screenshotGetKey();

	// inputKeyJustPressed() consumes the edge, so it wants calling exactly once
	// a frame and only for a key that is actually bound.
	if (vk > 0 && inputKeyJustPressed(vk)) {
		screenshotRequest();
	}
}

void screenshotInit(void)
{
	videoAddPreSwapCallback(screenshotPreSwap);
}

PD_CONSTRUCTOR static void screenshotConfigInit(void)
{
	configRegisterString("Mod.ScreenshotKey", keyName, sizeof(keyName));
}
