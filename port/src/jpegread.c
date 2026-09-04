/**
 * JPEG decoder, and the only thing in the port that uses stb_image.
 *
 * The other half of pngread.c. PNG is a zlib stream in chunks and zlib is
 * already linked, so that one is a few hundred lines of our own; baseline JPEG
 * is Huffman tables, an inverse DCT and chroma upsampling, and progressive JPEG
 * is more again, none of which is worth writing twice. So this wraps stb_image
 * - the same decoder the VR fork uses, which is also what the packs shipping
 * .jpg were tested against.
 *
 * It is also the fallback for a PNG that pngread.c declines. That decoder
 * handles what image editors write and refuses the rest rather than guessing -
 * and an Adam7 interlaced file is one of the refusals, which the PD Plus pack
 * turns out to ship a few hundred of among its font glyphs. Rather than write
 * a deinterlacer, the exotic ones go to the decoder already linked for JPEG.
 * pngRead() is still tried first, so nothing that worked before changes hands.
 */

#include <stdlib.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "jpegread.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image.h"

/**
 * Decodes anything stb is compiled for here, which is JPEG and PNG.
 */
static u8 *stbRead(const char *path, s32 *outWidth, s32 *outHeight, const char *what)
{
	s32 width = 0;
	s32 height = 0;
	s32 channels = 0;
	u8 *rgba;

	// 4 forces RGBA out of a decoder that has no alpha to give, so the buffer
	// matches pngRead()'s to the byte and every caller stays one path. JPEG is
	// opaque, and stb fills the alpha with 255.
	rgba = stbi_load(path, &width, &height, &channels, 4);

	if (!rgba) {
		sysLogPrintf(LOG_ERROR, "%s: %s: %s", what, path, stbi_failure_reason());
		return NULL;
	}

	if (width <= 0 || height <= 0) {
		stbi_image_free(rgba);
		return NULL;
	}

	*outWidth = width;
	*outHeight = height;

	// stb allocates with plain malloc unless told otherwise, which is what the
	// callers free with. Kept explicit because it is the one thing that would
	// go wrong quietly if stb's defaults ever changed.
	return rgba;
}

u8 *jpegRead(const char *path, s32 *outWidth, s32 *outHeight)
{
	return stbRead(path, outWidth, outHeight, "jpegRead");
}

u8 *pngReadFallback(const char *path, s32 *outWidth, s32 *outHeight)
{
	return stbRead(path, outWidth, outHeight, "pngReadFallback");
}
