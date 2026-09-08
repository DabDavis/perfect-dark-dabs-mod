/**
 * S3TC block decoding. See dxt.h for why it lives on its own.
 *
 * Lifted out of texpack.c unchanged apart from the format enum, so a Glide
 * pack decodes exactly as it did before.
 */

#include <PR/ultratypes.h>
#include "dxt.h"

static u32 dxtReadLE16(const u8 *p) { return p[0] | (p[1] << 8); }

static u32 dxtReadLE32(const u8 *p)
{
	return p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void dxtRgb565(u32 c, u8 *rgb)
{
	rgb[0] = (u8)(((c >> 11) & 0x1f) * 255 / 31);
	rgb[1] = (u8)(((c >> 5) & 0x3f) * 255 / 63);
	rgb[2] = (u8)((c & 0x1f) * 255 / 31);
}

u32 dxtBlockSize(s32 kind)
{
	return kind == DXT_KIND_1 ? 8 : 16;
}

void dxtBlock(const u8 *block, s32 kind, u8 *dst, u32 stride, s32 w, s32 h)
{
	// The colour half is shared by every DXT: two 565 endpoints and 2 bit
	// indexes. It follows the alpha half in the 16 byte kinds.
	const u8 *cb = (kind == DXT_KIND_1) ? block : block + 8;
	const u32 c0 = dxtReadLE16(cb);
	const u32 c1 = dxtReadLE16(cb + 2);
	const u32 idx = dxtReadLE32(cb + 4);
	u8 colours[4][4];
	u8 alpha[16];

	dxtRgb565(c0, colours[0]);
	dxtRgb565(c1, colours[1]);
	colours[0][3] = colours[1][3] = colours[2][3] = colours[3][3] = 255;

	if (kind != DXT_KIND_1 || c0 > c1) {
		for (s32 k = 0; k < 3; k++) {
			colours[2][k] = (u8)((2 * colours[0][k] + colours[1][k]) / 3);
			colours[3][k] = (u8)((colours[0][k] + 2 * colours[1][k]) / 3);
		}
	} else {
		// DXT1 with the endpoints the other way round: three colours and a
		// transparent fourth.
		for (s32 k = 0; k < 3; k++) {
			colours[2][k] = (u8)((colours[0][k] + colours[1][k]) / 2);
			colours[3][k] = 0;
		}
		colours[3][3] = 0;
	}

	for (s32 i = 0; i < 16; i++) {
		alpha[i] = 255;
	}

	if (kind == DXT_KIND_3) {
		for (s32 i = 0; i < 16; i++) {
			const u32 a = (block[i / 2] >> ((i & 1) * 4)) & 0xf;
			alpha[i] = (u8)(a * 17);
		}
	} else if (kind == DXT_KIND_5) {
		const u32 a0 = block[0];
		const u32 a1 = block[1];
		u8 table[8];
		u64 bits = 0;

		table[0] = (u8)a0;
		table[1] = (u8)a1;

		if (a0 > a1) {
			for (s32 k = 1; k < 7; k++) {
				table[k + 1] = (u8)(((7 - k) * a0 + k * a1) / 7);
			}
		} else {
			for (s32 k = 1; k < 5; k++) {
				table[k + 1] = (u8)(((5 - k) * a0 + k * a1) / 5);
			}
			table[6] = 0;
			table[7] = 255;
		}

		for (s32 k = 5; k >= 0; k--) {
			bits = (bits << 8) | block[2 + k];
		}

		for (s32 i = 0; i < 16; i++) {
			alpha[i] = table[(bits >> (i * 3)) & 7];
		}
	}

	for (s32 y = 0; y < 4 && y < h; y++) {
		for (s32 x = 0; x < 4 && x < w; x++) {
			const s32 i = y * 4 + x;
			const u8 *c = colours[(idx >> (i * 2)) & 3];
			u8 *o = dst + y * stride + x * 4;

			o[0] = c[0];
			o[1] = c[1];
			o[2] = c[2];
			o[3] = (kind == DXT_KIND_1) ? c[3] : alpha[i];
		}
	}
}
