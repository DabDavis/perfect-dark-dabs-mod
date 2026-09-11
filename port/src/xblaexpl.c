/**
 * The release's explosion frames, bound in place of the ROM's.
 * See xblaexpl.h for what this is; this file is the how, and it is small on
 * purpose: the only thing that changes about an explosion is the picture.
 */

#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "config.h"
#include "system.h"
#include "video.h"
#include "xblaimport.h"
#include "xblatex.h"
#include "xblaexpl.h"

// The release's own animation. 0e4f and 0e50 are empty and 0e51 is the smoke
// puff that follows it, which the game draws from its own smoke system and is
// left alone.
#define XBLAEXPL_FIRST  0x0e1f
#define XBLAEXPL_FRAMES 48

// The release's frames are all this size; a record that is not says the
// package is not the one this was written against.
#define XBLAEXPL_DIM 256

// What the game has: fifteen frames a part ages through (the `i` loop in
// explosionRender()), of which the first is blank in the ROM and stays that
// way, so fourteen are replaced.
#define XBLAEXPL_ROMFRAMES 15
#define XBLAEXPL_FIRSTROM  1

static s32 optEnabled = 1;

static const void *tiles[XBLAEXPL_ROMFRAMES];
static u8 tried[XBLAEXPL_ROMFRAMES];
static s32 numDecoded;

PD_CONSTRUCTOR static void xblaExplInit(void)
{
	configRegisterInt("Mod.XblaExplosions", &optEnabled, 0, 1);
}

s32 xblaExplGetEnabled(void)
{
	return optEnabled;
}

void xblaExplSetEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	if (enabled == optEnabled) {
		return;
	}

	optEnabled = enabled;

	// Turning it on is where a player's archive comes apart, on the game
	// thread and on the frame they asked for it: the draw itself never
	// unpacks anything.
	if (enabled) {
		xblaTexGetNumRecords();
	}

	// A frame already uploaded is what is on screen until its cache entry
	// goes. The stand-ins stay bound either way - what the switch decides is
	// whether the draw asks for one at all.
	videoResetTextureCache();
}

s32 xblaExplHaveFrames(void)
{
	return optEnabled && xblaImportIsAvailable();
}

/**
 * Which release frame one of the game's is.
 *
 * A part ages through the game's frames 1 to 14 whatever the explosion's type
 * is - the type only decides how fast (`flarespeed`) - so the two runs line up
 * end to end: the game's first drawn frame is the release's first and its last
 * is the release's last. Rounded, so the ends meet rather than falling three
 * frames short.
 */
static u32 xblaExplRecordOf(s32 frame)
{
	const s32 n = frame - XBLAEXPL_FIRSTROM;
	const s32 last = XBLAEXPL_ROMFRAMES - 1 - XBLAEXPL_FIRSTROM;

	return XBLAEXPL_FIRST + (u32)((n * (XBLAEXPL_FRAMES - 1) + last / 2) / last);
}

const void *xblaExplBindFrame(s32 frame)
{
	char key[32];
	u8 *rgba;
	s32 width = 0;
	s32 height = 0;
	u32 record;

	if (!xblaExplHaveFrames() || frame < XBLAEXPL_FIRSTROM || frame >= XBLAEXPL_ROMFRAMES) {
		return NULL;
	}

	if (tried[frame]) {
		return tiles[frame];
	}

	tried[frame] = 1;

	// Never unpacks: a draw is not a path anybody asked to wait on. Until
	// something else has unpacked the archive, or the switch has, the ROM's
	// own explosion is drawn.
	if (!xblaImportGetReadyStfsPath()) {
		tried[frame] = 0; // ask again once there is a package
		return NULL;
	}

	record = xblaExplRecordOf(frame);
	rgba = xblaTexDecodeRecord(record, &width, &height);

	if (!rgba) {
		sysLogPrintf(LOG_ERROR, "xblaexpl: record %04x will not decode", record);
		return NULL;
	}

	if (width != XBLAEXPL_DIM || height != XBLAEXPL_DIM) {
		sysLogPrintf(LOG_ERROR, "xblaexpl: record %04x is %dx%d, not an explosion frame",
				record, width, height);
		free(rgba);
		return NULL;
	}

	snprintf(key, sizeof(key), "xblaexpl%02x", record);

	// The picture is taken over by the registry and kept for the life of the
	// game, which is what makes this fifteen decodes in total rather than
	// fifteen per explosion.
	tiles[frame] = xblaTexBindImage(key, rgba, width, height);
	numDecoded++;

	if (numDecoded == XBLAEXPL_ROMFRAMES - XBLAEXPL_FIRSTROM) {
		sysLogPrintf(LOG_NOTE, "xblaexpl: %d frames bound, %04x to %04x",
				numDecoded, xblaExplRecordOf(XBLAEXPL_FIRSTROM),
				xblaExplRecordOf(XBLAEXPL_ROMFRAMES - 1));
	}

	return tiles[frame];
}
