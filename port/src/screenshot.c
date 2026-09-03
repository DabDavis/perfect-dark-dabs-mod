#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <PR/ultratypes.h>
#include <PR/os_thread.h>
#include <PR/os_cont.h>
#include "platform.h"
#include "config.h"
#include "fs.h"
#include "input.h"
#include "pngwrite.h"
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
			&& pngWrite(filename, rgb, width, height, 3, true)) {
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
