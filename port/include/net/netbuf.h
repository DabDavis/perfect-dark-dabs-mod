#ifndef _IN_NET_NETBUF_H
#define _IN_NET_NETBUF_H

#include <PR/ultratypes.h>

/**
 * A byte writer and reader over a buffer the caller owns.
 *
 * Every multi-byte value is little-endian, written a byte at a time, so the
 * wire never depends on the host's layout. No struct is ever copied onto the
 * wire whole: a struct's size and padding are the compiler's business, and
 * the bool in one of them was once a different size in two files.
 *
 * Errors are sticky. A write that does not fit, a read past the end, a varint
 * that runs too long or a string longer than its bound sets `error` and from
 * then on every write is dropped and every read returns zero (a string reads
 * as ""). A caller writes or reads a whole message and checks netBufOk() once
 * at the end instead of after every field.
 *
 * Bits: netBufWriteBits/ReadBits pack values least significant bit first into
 * whole bytes, for field masks. A run of bit writes shares bytes; any byte
 * write after them starts on the next whole byte, and the reader does the
 * same, so the two stay in step as long as both sides make the same calls.
 */

struct netbuf {
	u8 *data;
	s32 size;    // capacity when writing, length when reading
	s32 pos;     // bytes written or read so far
	s32 error;
	s32 bitbyte; // index of the byte the open bit run is packing into
	s32 bitpos;  // bits used in that byte, 0 when no run is open
};

void netBufInitWrite(struct netbuf *b, void *data, s32 capacity);
void netBufInitRead(struct netbuf *b, const void *data, s32 len);

static inline s32 netBufOk(const struct netbuf *b) { return !b->error; }
static inline s32 netBufLen(const struct netbuf *b) { return b->pos; }
static inline s32 netBufRemaining(const struct netbuf *b) { return b->error ? 0 : b->size - b->pos; }

void netBufWriteU8(struct netbuf *b, u8 v);
void netBufWriteU16(struct netbuf *b, u16 v);
void netBufWriteU32(struct netbuf *b, u32 v);
void netBufWriteS8(struct netbuf *b, s8 v);
void netBufWriteS16(struct netbuf *b, s16 v);
void netBufWriteS32(struct netbuf *b, s32 v);
void netBufWriteF32(struct netbuf *b, f32 v);
void netBufWriteVarU32(struct netbuf *b, u32 v);
void netBufWriteBytes(struct netbuf *b, const void *src, s32 len);
void netBufWriteString(struct netbuf *b, const char *s, s32 maxlen);
void netBufWriteBits(struct netbuf *b, u32 v, s32 nbits);

u8 netBufReadU8(struct netbuf *b);
u16 netBufReadU16(struct netbuf *b);
u32 netBufReadU32(struct netbuf *b);
s8 netBufReadS8(struct netbuf *b);
s16 netBufReadS16(struct netbuf *b);
s32 netBufReadS32(struct netbuf *b);
f32 netBufReadF32(struct netbuf *b);
u32 netBufReadVarU32(struct netbuf *b);
void netBufReadBytes(struct netbuf *b, void *dst, s32 len);
s32 netBufReadString(struct netbuf *b, char *dst, s32 dstsize);
u32 netBufReadBits(struct netbuf *b, s32 nbits);

/**
 * Points at len bytes in place and steps past them, or returns NULL and sets
 * the error. Writing: reserves them for the caller to fill (a length to patch
 * in later). Reading: the bytes without a copy.
 */
u8 *netBufReserve(struct netbuf *b, s32 len);
const u8 *netBufSkip(struct netbuf *b, s32 len);

#endif
