/**
 * PNG encoder.
 *
 * PNG is a zlib stream wrapped in chunks, and zlib is already a dependency
 * because the ROM is compressed with it, so the whole encoder is this file
 * rather than another vendored header.
 *
 * Written for screenshots and since shared with the texture dumper, which is
 * why it takes a channel count and a row order rather than assuming either.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "pngwrite.h"

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
 * The rows are copied out regardless, to make room for the per-row filter byte
 * that PNG puts in front of each one, so flipping them costs nothing extra.
 * Filter 0 (none) is used throughout: the image is already going through
 * deflate, and choosing filters per row would cost more than it saves at these
 * sizes.
 */
s32 pngWrite(const char *path, const u8 *pixels, s32 width, s32 height, s32 channels, s32 bottomRowFirst)
{
	const uLong rowSize = (uLong)width * channels;
	const uLong stride = 1 + rowSize;
	const uLong rawSize = stride * (uLong)height;
	uLong zSize;
	u8 *raw;
	u8 *z;
	FILE *f;
	u8 ihdr[13];
	s32 y;

	if (width <= 0 || height <= 0 || (channels != 3 && channels != 4)) {
		sysLogPrintf(LOG_ERROR, "png: refusing to write %s at %dx%d with %d channels",
				path, width, height, channels);
		return 0;
	}

	zSize = compressBound(rawSize);
	raw = malloc(rawSize);
	z = malloc(zSize);

	if (!raw || !z) {
		free(raw);
		free(z);
		sysLogPrintf(LOG_ERROR, "png: could not alloc %lu bytes for %s",
				(unsigned long)(rawSize + zSize), path);
		return 0;
	}

	for (y = 0; y < height; y++) {
		const s32 srcy = bottomRowFirst ? height - 1 - y : y;
		u8 *dst = raw + stride * y;
		*dst++ = 0;
		memcpy(dst, pixels + rowSize * (uLong)srcy, rowSize);
	}

	if (compress2(z, &zSize, raw, rawSize, Z_DEFAULT_COMPRESSION) != Z_OK) {
		free(raw);
		free(z);
		sysLogPrintf(LOG_ERROR, "png: could not compress %s", path);
		return 0;
	}

	free(raw);

	f = fopen(path, "wb");

	if (!f) {
		free(z);
		sysLogPrintf(LOG_ERROR, "png: could not open %s for writing", path);
		return 0;
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
	ihdr[8] = 8;                       // bits per channel
	ihdr[9] = channels == 4 ? 6 : 2;   // colour type: truecolour, with alpha or without
	ihdr[10] = 0;                      // deflate
	ihdr[11] = 0;                      // adaptive filtering
	ihdr[12] = 0;                      // no interlace

	pngWriteChunk(f, "IHDR", ihdr, sizeof(ihdr));
	pngWriteChunk(f, "IDAT", z, zSize);
	pngWriteChunk(f, "IEND", NULL, 0);

	free(z);

	if (ferror(f)) {
		fclose(f);
		sysLogPrintf(LOG_ERROR, "png: could not write %s", path);
		return 0;
	}

	fclose(f);

	return 1;
}
