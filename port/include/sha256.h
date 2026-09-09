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

#endif
