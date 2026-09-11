/**
 * The XBLA release's interface art. See xblaui.h for what is in the package
 * and what is drawn; this file is the how, and it is one picture.
 *
 * The logo goes up through menuimage.c, which exists for exactly this - a
 * picture the menus draw that is not one of the game's own textures - and the
 * only thing new here is where the picture comes from: a Textures.raw record
 * rather than a PNG compiled into the binary.
 */

#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "xblaimport.h"
#include "xblatex.h"
#include "menuimage.h"
#include "xblaui.h"

// 4J's logo, 420x255. Their frontend's, not the ROM's: the title screen's is
// four models that assemble themselves (MODEL_PDONE and friends in title.c)
// and this is a flat picture, so it stands in for nothing.
#define XBLAUI_LOGO_RECORD 0x0dd9
#define XBLAUI_LOGO_WIDTH  420
#define XBLAUI_LOGO_HEIGHT 255

/**
 * The record, decoded on the render thread the first time the picture is
 * drawn.
 *
 * Never unpacks a player's archive - xblaTexDecodeRecord() opens a package
 * that is on disk and this is not a path anybody asked to wait on. A machine
 * whose copy is still inside its .7z therefore has no logo until something
 * else has unpacked it, which is the same bargain the meshes make.
 */
static u8 *xblaUiLoadLogo(s32 *outWidth, s32 *outHeight)
{
	u8 *rgba;
	s32 width = 0;
	s32 height = 0;

	if (!xblaImportGetReadyStfsPath()) {
		return NULL;
	}

	rgba = xblaTexDecodeRecord(XBLAUI_LOGO_RECORD, &width, &height);

	if (!rgba) {
		return NULL;
	}

	// The size is the check that this is the picture and not whatever a
	// different package has at that record.
	if (width != XBLAUI_LOGO_WIDTH || height != XBLAUI_LOGO_HEIGHT) {
		sysLogPrintf(LOG_ERROR, "xblaui: record %04x is %dx%d, not the logo",
				XBLAUI_LOGO_RECORD, width, height);
		free(rgba);
		return NULL;
	}

	// Turned over, which the font atlases are not: those are 4J's own 2D art
	// and are stored the way a screen is drawn, top row first, and their .abc
	// cells are measured that way too. This record is stored the way the
	// game's textures are, bottom row first, and menuimage.c hands the
	// renderer a top-first picture - so the logo came up upside down in the
	// page that was meant to show it off. There is no rule covering both:
	// a record's row order is a fact about that record.
	{
		const s32 stride = width * 4;
		u8 *row = malloc(stride);
		s32 y;

		if (!row) {
			free(rgba);
			return NULL;
		}

		for (y = 0; y < height / 2; y++) {
			u8 *a = rgba + y * stride;
			u8 *b = rgba + (height - 1 - y) * stride;

			memcpy(row, a, stride);
			memcpy(a, b, stride);
			memcpy(b, row, stride);
		}

		free(row);
	}

	*outWidth = width;
	*outHeight = height;

	return rgba;
}

static struct menuimage logo = {
	NULL, 0,
	xblaUiLoadLogo,
	"xbla logo",
};

PD_CONSTRUCTOR static void xblaUiInit(void)
{
	menuImageRegister(&logo);
}

struct menuimage *xblaUiGetLogo(void)
{
	// The picture is asked for by the renderer, so what this answers is only
	// whether it is worth a row: a package the player has, or one that has
	// already been read.
	if (!logo.registered || (!logo.rgba && !xblaImportIsAvailable())) {
		return NULL;
	}

	return &logo;
}
