#include <string.h>
#include <PR/ultratypes.h>
#include "net/netbuf.h"

/**
 * See netbuf.h. Everything funnels through netBufReserve and netBufSkip,
 * which are the only places that compare against the end, so there is one
 * bounds check to get right rather than one per type.
 */

void netBufInitWrite(struct netbuf *b, void *data, s32 capacity)
{
	b->data = data;
	b->size = (data && capacity > 0) ? capacity : 0;
	b->pos = 0;
	b->error = 0;
	b->bitbyte = 0;
	b->bitpos = 0;
}

void netBufInitRead(struct netbuf *b, const void *data, s32 len)
{
	// The reader never writes through data; the one struct serves both ways
	b->data = (u8 *)data;
	b->size = (data && len > 0) ? len : 0;
	b->pos = 0;
	b->error = 0;
	b->bitbyte = 0;
	b->bitpos = 0;
}

u8 *netBufReserve(struct netbuf *b, s32 len)
{
	u8 *ptr;

	b->bitpos = 0;

	if (b->error || len < 0 || len > b->size - b->pos) {
		b->error = 1;
		return NULL;
	}

	ptr = b->data + b->pos;
	b->pos += len;

	return ptr;
}

const u8 *netBufSkip(struct netbuf *b, s32 len)
{
	return netBufReserve(b, len);
}

void netBufWriteU8(struct netbuf *b, u8 v)
{
	u8 *p = netBufReserve(b, 1);

	if (p) {
		p[0] = v;
	}
}

void netBufWriteU16(struct netbuf *b, u16 v)
{
	u8 *p = netBufReserve(b, 2);

	if (p) {
		p[0] = v & 0xff;
		p[1] = v >> 8;
	}
}

void netBufWriteU32(struct netbuf *b, u32 v)
{
	u8 *p = netBufReserve(b, 4);

	if (p) {
		p[0] = v & 0xff;
		p[1] = (v >> 8) & 0xff;
		p[2] = (v >> 16) & 0xff;
		p[3] = v >> 24;
	}
}

void netBufWriteS8(struct netbuf *b, s8 v)
{
	netBufWriteU8(b, (u8)v);
}

void netBufWriteS16(struct netbuf *b, s16 v)
{
	netBufWriteU16(b, (u16)v);
}

void netBufWriteS32(struct netbuf *b, s32 v)
{
	netBufWriteU32(b, (u32)v);
}

void netBufWriteF32(struct netbuf *b, f32 v)
{
	u32 bits;

	// The float's own bits, exactly: usercmds carry stick and mouse values
	// the host must simulate on unchanged
	memcpy(&bits, &v, sizeof(bits));
	netBufWriteU32(b, bits);
}

/**
 * Seven bits a byte, low first, the top bit set on every byte but the last.
 * A u32 takes one to five bytes.
 */
void netBufWriteVarU32(struct netbuf *b, u32 v)
{
	while (v >= 0x80) {
		netBufWriteU8(b, (v & 0x7f) | 0x80);
		v >>= 7;
	}

	netBufWriteU8(b, v);
}

void netBufWriteBytes(struct netbuf *b, const void *src, s32 len)
{
	u8 *p = netBufReserve(b, len);

	if (p && len > 0) {
		memcpy(p, src, len);
	}
}

/**
 * A varint length and the bytes, no terminator. A string longer than maxlen
 * is an error rather than cut short: a name or a hash that arrived truncated
 * would be a different one without anybody being told.
 */
void netBufWriteString(struct netbuf *b, const char *s, s32 maxlen)
{
	s32 len = 0;

	// strnlen is POSIX, not C11
	while (s && len <= maxlen && s[len] != '\0') {
		len++;
	}

	if (maxlen < 0 || len > maxlen) {
		b->error = 1;
		return;
	}

	netBufWriteVarU32(b, len);
	netBufWriteBytes(b, s, len);
}

void netBufWriteBits(struct netbuf *b, u32 v, s32 nbits)
{
	s32 i;

	if (nbits < 0 || nbits > 32) {
		b->error = 1;
		return;
	}

	for (i = 0; i < nbits; i++) {
		if (b->error) {
			return;
		}

		if (b->bitpos == 0) {
			u8 *p = netBufReserve(b, 1);

			if (!p) {
				return;
			}

			*p = 0;
			b->bitbyte = p - b->data;
		}

		if ((v >> i) & 1) {
			b->data[b->bitbyte] |= 1 << b->bitpos;
		}

		b->bitpos = (b->bitpos + 1) & 7;
	}
}

u8 netBufReadU8(struct netbuf *b)
{
	const u8 *p = netBufSkip(b, 1);

	return p ? p[0] : 0;
}

u16 netBufReadU16(struct netbuf *b)
{
	const u8 *p = netBufSkip(b, 2);

	return p ? (u16)(p[0] | (p[1] << 8)) : 0;
}

u32 netBufReadU32(struct netbuf *b)
{
	const u8 *p = netBufSkip(b, 4);

	return p ? (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24) : 0;
}

s8 netBufReadS8(struct netbuf *b)
{
	return (s8)netBufReadU8(b);
}

s16 netBufReadS16(struct netbuf *b)
{
	return (s16)netBufReadU16(b);
}

s32 netBufReadS32(struct netbuf *b)
{
	return (s32)netBufReadU32(b);
}

f32 netBufReadF32(struct netbuf *b)
{
	u32 bits = netBufReadU32(b);
	f32 v;

	memcpy(&v, &bits, sizeof(v));

	return v;
}

/**
 * Five bytes at most, and the fifth may only carry the top four bits: a
 * longer run, or one that would overflow, is malformed rather than wrapped.
 */
u32 netBufReadVarU32(struct netbuf *b)
{
	u32 v = 0;
	s32 i;

	for (i = 0; i < 5; i++) {
		u8 byte = netBufReadU8(b);

		if (b->error) {
			return 0;
		}

		if (i == 4 && byte > 0x0f) {
			b->error = 1;
			return 0;
		}

		v |= (u32)(byte & 0x7f) << (7 * i);

		if ((byte & 0x80) == 0) {
			return v;
		}
	}

	b->error = 1;

	return 0;
}

void netBufReadBytes(struct netbuf *b, void *dst, s32 len)
{
	const u8 *p = netBufSkip(b, len);

	if (len <= 0) {
		return;
	}

	if (p) {
		memcpy(dst, p, len);
	} else {
		memset(dst, 0, len);
	}
}

/**
 * Reads a string written by netBufWriteString into dst, terminated. One that
 * does not fit dst with its terminator is an error, and dst is left "".
 * Returns the length, or -1 on error. Embedded NULs are refused too, so what
 * the caller sees is what strlen will say.
 */
s32 netBufReadString(struct netbuf *b, char *dst, s32 dstsize)
{
	u32 len = netBufReadVarU32(b);
	const u8 *p;

	if (dstsize > 0) {
		dst[0] = '\0';
	}

	if (b->error || dstsize <= 0 || len >= (u32)dstsize) {
		b->error = 1;
		return -1;
	}

	p = netBufSkip(b, (s32)len);

	if (!p || memchr(p, 0, len) != NULL) {
		b->error = 1;
		return -1;
	}

	memcpy(dst, p, len);
	dst[len] = '\0';

	return (s32)len;
}

u32 netBufReadBits(struct netbuf *b, s32 nbits)
{
	u32 v = 0;
	s32 i;

	if (nbits < 0 || nbits > 32) {
		b->error = 1;
		return 0;
	}

	for (i = 0; i < nbits; i++) {
		if (b->error) {
			return 0;
		}

		if (b->bitpos == 0) {
			const u8 *p = netBufSkip(b, 1);

			if (!p) {
				return 0;
			}

			b->bitbyte = p - b->data;
		}

		if ((b->data[b->bitbyte] >> b->bitpos) & 1) {
			v |= 1u << i;
		}

		b->bitpos = (b->bitpos + 1) & 7;
	}

	return v;
}
