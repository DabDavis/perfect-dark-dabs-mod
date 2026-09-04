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
 * STBI_ONLY_JPEG keeps it to just that: pngRead() stays the PNG path, and the
 * two decoders cannot disagree about a file because only one of them is ever
 * compiled to read it.
 */

#include <stdlib.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "jpegread.h"

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_WRITE
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image.h"

u8 *jpegRead(const char *path, s32 *outWidth, s32 *outHeight)
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
		sysLogPrintf(LOG_ERROR, "jpegRead: %s: %s", path, stbi_failure_reason());
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
