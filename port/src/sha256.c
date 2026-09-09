/**
 * SHA-256 over a file, for the two things the game downloads and then trusts.
 *
 * TLS says the bytes came from where they were asked for and were not changed
 * on the way, which is most of it. What it does not say is that all of them
 * arrived: a connection that dies two thirds of the way through a download
 * leaves a file that is a perfectly good prefix of what was wanted, and a
 * prefix of a program is an install that will not start with nothing to
 * explain why. The size would catch that one; the hash catches it and
 * everything else, and both GitHub and the release job know the answer
 * already.
 *
 * It lives here rather than in update.c, where it was written, because Check
 * for Updates is no longer the only caller - a community texture pack is
 * downloaded and then unpacked over the same wire, and a second copy of a hash
 * is two places for one to be wrong.
 *
 * The plain FIPS 180-4 implementation, which is short enough that reaching for
 * a dependency to avoid writing it would cost more than it saved.
 */

#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "types.h"
#include "sha256.h"

struct sha256 {
	u32 h[8];
	u64 len;
	u8 block[64];
	u32 fill;
};

static const u32 g_Sha256K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define SHA256_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256Block(struct sha256 *ctx, const u8 *p)
{
	u32 w[64];
	u32 a, b, c, d, e, f, g, h;
	u32 t1, t2;
	s32 i;

	for (i = 0; i < 16; i++) {
		w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
	}

	for (i = 16; i < 64; i++) {
		t1 = SHA256_ROR(w[i - 15], 7) ^ SHA256_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		t2 = SHA256_ROR(w[i - 2], 17) ^ SHA256_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + t1 + w[i - 7] + t2;
	}

	a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
	e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

	for (i = 0; i < 64; i++) {
		t1 = h + (SHA256_ROR(e, 6) ^ SHA256_ROR(e, 11) ^ SHA256_ROR(e, 25)) + ((e & f) ^ (~e & g)) + g_Sha256K[i] + w[i];
		t2 = (SHA256_ROR(a, 2) ^ SHA256_ROR(a, 13) ^ SHA256_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
	ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256Init(struct sha256 *ctx)
{
	ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85; ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
	ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c; ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
	ctx->len = 0;
	ctx->fill = 0;
}

static void sha256Update(struct sha256 *ctx, const void *ptr, u32 len)
{
	const u8 *p = ptr;

	ctx->len += len;

	while (len > 0) {
		u32 take = 64 - ctx->fill;

		if (take > len) {
			take = len;
		}

		memcpy(ctx->block + ctx->fill, p, take);
		ctx->fill += take;
		p += take;
		len -= take;

		if (ctx->fill == 64) {
			sha256Block(ctx, ctx->block);
			ctx->fill = 0;
		}
	}
}

static void sha256Final(struct sha256 *ctx, char *out)
{
	static const char hex[] = "0123456789abcdef";
	u64 bits = ctx->len * 8;
	u8 tail[8];
	u8 pad = 0x80;
	u8 zero = 0;
	s32 i;

	sha256Update(ctx, &pad, 1);

	while (ctx->fill != 56) {
		sha256Update(ctx, &zero, 1);
	}

	for (i = 0; i < 8; i++) {
		tail[i] = (u8)(bits >> (56 - i * 8));
	}

	// Not through sha256Update(), which would count these eight bytes into the
	// length that is being written.
	memcpy(ctx->block + ctx->fill, tail, 8);
	sha256Block(ctx, ctx->block);

	for (i = 0; i < 32; i++) {
		u8 byte = (u8)(ctx->h[i / 4] >> (24 - (i % 4) * 8));
		out[i * 2] = hex[byte >> 4];
		out[i * 2 + 1] = hex[byte & 15];
	}

	out[64] = '\0';
}

/**
 * The hash of a file already on disk, which is the one that matters: the bytes
 * checked are the bytes that will be used, rather than the bytes that were
 * meant to have been written.
 */
bool sha256File(const char *path, char *out)
{
	struct sha256 ctx;
	u8 chunk[16384];
	size_t got;
	FILE *f = fopen(path, "rb");

	if (f == NULL) {
		return false;
	}

	sha256Init(&ctx);

	while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		sha256Update(&ctx, chunk, (u32)got);
	}

	if (ferror(f)) {
		fclose(f);
		return false;
	}

	fclose(f);
	sha256Final(&ctx, out);

	return true;
}
