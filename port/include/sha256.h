#ifndef _IN_SHA256_H
#define _IN_SHA256_H

#include <PR/ultratypes.h>
#include "platform.h"
#include "types.h"

/**
 * The SHA-256 of a file on disk, written to out as 64 lowercase hex digits and
 * a terminator - so out is 65 bytes. False if the file could not be read
 * through to the end.
 *
 * For anything the game downloads and then acts on: see the comment in
 * port/src/sha256.c for why the transport's own guarantees are not this one.
 */
bool sha256File(const char *path, char *out);

/**
 * A hash built up in pieces, and HMAC-SHA256 over it (netplay's content
 * hashes and the lobby's join tickets). out is 32 raw bytes.
 */
struct sha256ctx {
	u32 h[8];
	u64 len;
	u8 block[64];
	u32 fill;
};

void sha256Begin(struct sha256ctx *ctx);
void sha256Add(struct sha256ctx *ctx, const void *data, u32 len);
void sha256End(struct sha256ctx *ctx, u8 *out);
void sha256Hmac(const u8 *key, u32 keylen, const void *msg, u32 msglen, u8 *out);

#endif
