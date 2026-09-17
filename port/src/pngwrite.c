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

static u8 *pngPutU32(u8 *out, u32 val)
{
	out[0] = val >> 24;
	out[1] = val >> 16;
	out[2] = val >> 8;
	out[3] = val;
	return out + 4;
}

static u8 *pngPutChunk(u8 *out, const char *type, const u8 *data, u32 len)
{
	u32 crc = crc32(0, (const u8 *)type, 4);

	if (len) {
		crc = crc32(crc, data, len);
	}

	out = pngPutU32(out, len);
	memcpy(out, type, 4);
	out += 4;

	if (len) {
		memcpy(out, data, len);
		out += len;
	}

	return pngPutU32(out, crc);
}

/**
 * The rows are copied out regardless, to make room for the per-row filter byte
 * that PNG puts in front of each one, so flipping them costs nothing extra.
 * Filter 0 (none) is used throughout: the image is already going through
 * deflate, and choosing filters per row would cost more than it saves at these
 * sizes.
 */
u8 *pngEncode(const u8 *pixels, s32 width, s32 height, s32 channels, s32 bottomRowFirst, u32 *outSize)
{
	const uLong rowSize = (uLong)width * channels;
	const uLong stride = 1 + rowSize;
	const uLong rawSize = stride * (uLong)height;
	uLong zSize;
	u8 *raw;
	u8 *z;
	u8 *png;
	u8 *out;
	u8 ihdr[13];
	s32 y;

	if (width <= 0 || height <= 0 || (channels != 3 && channels != 4)) {
		sysLogPrintf(LOG_ERROR, "png: refusing to encode %dx%d with %d channels",
				width, height, channels);
		return NULL;
	}

	zSize = compressBound(rawSize);
	raw = malloc(rawSize);
	z = malloc(zSize);

	if (!raw || !z) {
		free(raw);
		free(z);
		sysLogPrintf(LOG_ERROR, "png: could not alloc %lu bytes",
				(unsigned long)(rawSize + zSize));
		return NULL;
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
		sysLogPrintf(LOG_ERROR, "png: could not compress");
		return NULL;
	}

	free(raw);

	// Signature, then IHDR, IDAT and IEND at 12 bytes of framing each.
	png = malloc(8 + 12 + sizeof(ihdr) + 12 + zSize + 12);

	if (!png) {
		free(z);
		sysLogPrintf(LOG_ERROR, "png: could not alloc %lu bytes", (unsigned long)zSize + 57);
		return NULL;
	}

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

	out = png;
	memcpy(out, "\x89PNG\r\n\x1a\n", 8);
	out += 8;
	out = pngPutChunk(out, "IHDR", ihdr, sizeof(ihdr));
	out = pngPutChunk(out, "IDAT", z, zSize);
	out = pngPutChunk(out, "IEND", NULL, 0);

	free(z);

	*outSize = out - png;

	return png;
}

s32 pngWrite(const char *path, const u8 *pixels, s32 width, s32 height, s32 channels, s32 bottomRowFirst)
{
	u32 size = 0;
	u8 *png = pngEncode(pixels, width, height, channels, bottomRowFirst, &size);
	FILE *f;
	s32 ok;

	if (!png) {
		sysLogPrintf(LOG_ERROR, "png: could not encode %s", path);
		return 0;
	}

	f = fopen(path, "wb");

	if (!f) {
		free(png);
		sysLogPrintf(LOG_ERROR, "png: could not open %s for writing", path);
		return 0;
	}

	ok = fwrite(png, 1, size, f) == size;
	free(png);

	if (fclose(f) != 0 || !ok) {
		sysLogPrintf(LOG_ERROR, "png: could not write %s", path);
		return 0;
	}

	return 1;
}
