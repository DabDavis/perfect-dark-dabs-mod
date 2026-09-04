/**
 * ROM patch formats: xdelta (VCDIFF), BPS and IPS.
 *
 * What console mods are distributed as. Every xdelta in the mod archive is
 * plain VCDIFF - no secondary compression, the default code table - so that
 * is what is decoded here; the rest of the format is refused by name rather
 * than carrying a decoder for every compressor an author might have chosen.
 * tools/importmod shells out to xdelta3 for the same reason.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <PR/ultratypes.h>
#include "rompatch.h"

static u32 rd24be(const u8 *p) { return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; }
static u32 rd16be(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }
static u32 rd32le(const u8 *p) { return p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void seterr(char *err, u32 errlen, const char *msg)
{
	if (err && errlen) {
		snprintf(err, errlen, "%s", msg);
	}
}

s32 rompatchIdentify(const u8 *head, u32 len)
{
	if (len >= 4 && head[0] == 0xd6 && head[1] == 0xc3 && head[2] == 0xc4 && head[3] == 0x00) {
		return ROMPATCH_XDELTA;
	}
	if (len >= 4 && !memcmp(head, "BPS1", 4)) {
		return ROMPATCH_BPS;
	}
	if (len >= 5 && !memcmp(head, "PATCH", 5)) {
		return ROMPATCH_IPS;
	}
	return ROMPATCH_NONE;
}

s32 rompatchIsPatchName(const char *name)
{
	const char *dot = strrchr(name, '.');

	if (!dot) {
		return 0;
	}

	return !strcasecmp(dot, ".xdelta") || !strcasecmp(dot, ".vcdiff")
		|| !strcasecmp(dot, ".bps") || !strcasecmp(dot, ".ips");
}

const char *rompatchKindName(s32 kind)
{
	switch (kind) {
	case ROMPATCH_XDELTA: return "xdelta";
	case ROMPATCH_BPS: return "bps";
	case ROMPATCH_IPS: return "ips";
	default: return "unknown";
	}
}

/* -- VCDIFF ---------------------------------------------------------------- */

#define VCD_DECOMPRESS 0x01
#define VCD_CODETABLE  0x02
#define VCD_APPHEADER  0x04 // xdelta3 extension

#define VCD_SOURCE  0x01
#define VCD_TARGET  0x02
#define VCD_ADLER32 0x04 // xdelta3 extension

#define VCD_NOOP 0
#define VCD_ADD  1
#define VCD_RUN  2
#define VCD_COPY 3

#define VCD_NEAR 4
#define VCD_SAME 3

struct vcdreader {
	const u8 *p;
	const u8 *end;
	s32 bad;
};

static u32 vcdByte(struct vcdreader *r)
{
	if (r->p >= r->end) {
		r->bad = 1;
		return 0;
	}
	return *r->p++;
}

// base 128, big endian, continuation bit set on every byte but the last
static u32 vcdVarint(struct vcdreader *r)
{
	u32 v = 0;

	for (s32 i = 0; i < 5; ++i) {
		const u32 b = vcdByte(r);
		v = (v << 7) | (b & 0x7f);
		if (!(b & 0x80)) {
			return v;
		}
	}

	r->bad = 1;
	return 0;
}

struct vcdcode {
	u8 inst1, size1, mode1;
	u8 inst2, size2, mode2;
};

// RFC 3284 section 5.6, the default code table
static void vcdBuildCodeTable(struct vcdcode *t)
{
	s32 i = 0;

	memset(t, 0, sizeof(*t) * 256);

	t[i].inst1 = VCD_RUN; ++i;

	for (s32 size = 0; size <= 17; ++size) {
		t[i].inst1 = VCD_ADD; t[i].size1 = size; ++i;
	}

	for (s32 mode = 0; mode <= 8; ++mode) {
		t[i].inst1 = VCD_COPY; t[i].size1 = 0; t[i].mode1 = mode; ++i;
		for (s32 size = 4; size <= 18; ++size) {
			t[i].inst1 = VCD_COPY; t[i].size1 = size; t[i].mode1 = mode; ++i;
		}
	}

	for (s32 mode = 0; mode <= 5; ++mode) {
		for (s32 addsize = 1; addsize <= 4; ++addsize) {
			for (s32 copysize = 4; copysize <= 6; ++copysize) {
				t[i].inst1 = VCD_ADD; t[i].size1 = addsize;
				t[i].inst2 = VCD_COPY; t[i].size2 = copysize; t[i].mode2 = mode; ++i;
			}
		}
	}

	for (s32 mode = 6; mode <= 8; ++mode) {
		for (s32 addsize = 1; addsize <= 4; ++addsize) {
			t[i].inst1 = VCD_ADD; t[i].size1 = addsize;
			t[i].inst2 = VCD_COPY; t[i].size2 = 4; t[i].mode2 = mode; ++i;
		}
	}

	for (s32 mode = 0; mode <= 8; ++mode) {
		t[i].inst1 = VCD_COPY; t[i].size1 = 4; t[i].mode1 = mode;
		t[i].inst2 = VCD_ADD; t[i].size2 = 1; ++i;
	}
}

struct vcdcache {
	u32 near[VCD_NEAR];
	u32 same[VCD_SAME * 256];
	u32 nextslot;
};

static void vcdCacheInit(struct vcdcache *c)
{
	memset(c, 0, sizeof(*c));
}

static u32 vcdDecodeAddr(struct vcdcache *c, struct vcdreader *addrs, u32 here, u32 mode)
{
	u32 addr;

	if (mode == 0) {
		addr = vcdVarint(addrs);
	} else if (mode == 1) {
		addr = here - vcdVarint(addrs);
	} else if (mode - 2 < VCD_NEAR) {
		addr = c->near[mode - 2] + vcdVarint(addrs);
	} else {
		const u32 m = mode - 2 - VCD_NEAR;
		if (m >= VCD_SAME) {
			addrs->bad = 1;
			return 0;
		}
		addr = c->same[m * 256 + vcdByte(addrs)];
	}

	c->near[c->nextslot] = addr;
	c->nextslot = (c->nextslot + 1) % VCD_NEAR;
	c->same[addr % (VCD_SAME * 256)] = addr;

	return addr;
}

struct vcdwindow {
	const u8 *src;   // the source segment this window copies from, or NULL
	u32 srclen;      // as declared: addresses past it are in the target window
	u32 srcavail;    // how much of it exists. xdelta3 declares a segment by its
	                 // window size and lets it run off the end of the file
	u8 *tgt;         // the target window being built
	u32 tgtlen;
	u32 tgtpos;
};

// One instruction. Addresses count through the source segment then the
// target window, as the RFC has it.
static s32 vcdRunInst(struct vcdwindow *w, struct vcdcache *c, u32 inst, u32 size,
		u32 mode, struct vcdreader *data, struct vcdreader *addrs)
{
	if (inst == VCD_NOOP) {
		return 1;
	}

	if (size > w->tgtlen - w->tgtpos) {
		return 0;
	}

	if (inst == VCD_ADD) {
		if ((u32)(data->end - data->p) < size) {
			return 0;
		}
		memcpy(w->tgt + w->tgtpos, data->p, size);
		data->p += size;
		w->tgtpos += size;
		return 1;
	}

	if (inst == VCD_RUN) {
		const u32 b = vcdByte(data);
		if (data->bad) {
			return 0;
		}
		memset(w->tgt + w->tgtpos, b, size);
		w->tgtpos += size;
		return 1;
	}

	// COPY
	{
		const u32 here = w->srclen + w->tgtpos;
		const u32 addr = vcdDecodeAddr(c, addrs, here, mode);

		if (addrs->bad) {
			return 0;
		}

		if (addr < w->srclen) {
			// from the source; may run on into what this window has written
			for (u32 i = 0; i < size; ++i) {
				const u32 a = addr + i;
				if (a < w->srclen) {
					if (a >= w->srcavail) {
						return 0;
					}
					w->tgt[w->tgtpos + i] = w->src[a];
				} else if (a - w->srclen < w->tgtpos + i) {
					w->tgt[w->tgtpos + i] = w->tgt[a - w->srclen];
				} else {
					return 0;
				}
			}
		} else {
			const u32 t = addr - w->srclen;
			if (t >= w->tgtpos) {
				return 0;
			}
			// may overlap what it is writing, so byte by byte
			for (u32 i = 0; i < size; ++i) {
				w->tgt[w->tgtpos + i] = w->tgt[t + i];
			}
		}

		w->tgtpos += size;
		return 1;
	}
}

static s32 applyXdelta(const u8 *rom, u32 romlen, const u8 *patch, u32 patchlen,
		u8 **out, u32 *outlen, char *err, u32 errlen)
{
	static struct vcdcode codetable[256];
	static s32 codetableBuilt;
	struct vcdreader r = { patch, patch + patchlen, 0 };
	u8 *dst = NULL;
	u32 dstlen = 0;
	u32 dstcap = 0;

	if (!codetableBuilt) {
		vcdBuildCodeTable(codetable);
		codetableBuilt = 1;
	}

	r.p += 4;

	{
		const u32 hdr = vcdByte(&r);

		if (hdr & VCD_DECOMPRESS) {
			const u32 id = vcdByte(&r);
			char msg[128];
			snprintf(msg, sizeof(msg), "this xdelta uses secondary compression (%s), which is not supported here; "
					"re-create it with `xdelta3 -S none` or convert it with tools/importmod",
					id == 1 ? "djw" : id == 2 ? "fgk" : id == 16 ? "lzma" : "unknown");
			seterr(err, errlen, msg);
			return -1;
		}
		if (hdr & VCD_CODETABLE) {
			seterr(err, errlen, "this xdelta uses a custom code table, which is not supported here");
			return -1;
		}
		if (hdr & VCD_APPHEADER) {
			const u32 n = vcdVarint(&r);
			if (r.bad || (u32)(r.end - r.p) < n) {
				seterr(err, errlen, "xdelta header is truncated");
				return -1;
			}
			r.p += n;
		}
	}

	while (r.p < r.end && !r.bad) {
		const u32 win = vcdByte(&r);
		u32 srclen = 0, srcpos = 0;
		const u8 *src = NULL;
		u32 deltalen, tgtlen, deltaind, datalen, instlen, addrlen;
		u32 adler = 0;
		struct vcdreader data, inst, addrs;
		struct vcdwindow w;
		struct vcdcache cache;

		if (win & (VCD_SOURCE | VCD_TARGET)) {
			srclen = vcdVarint(&r);
			srcpos = vcdVarint(&r);
		}

		deltalen = vcdVarint(&r);
		(void)deltalen;
		tgtlen = vcdVarint(&r);
		deltaind = vcdByte(&r);
		datalen = vcdVarint(&r);
		instlen = vcdVarint(&r);
		addrlen = vcdVarint(&r);

		if (win & VCD_ADLER32) {
			adler = ((u32)vcdByte(&r) << 24) | ((u32)vcdByte(&r) << 16) | ((u32)vcdByte(&r) << 8) | vcdByte(&r);
		}

		if (r.bad || deltaind != 0) {
			seterr(err, errlen, deltaind ? "xdelta window uses secondary compression" : "xdelta window header is truncated");
			free(dst);
			return -1;
		}

		if ((u32)(r.end - r.p) < datalen || (u32)(r.end - r.p) - datalen < instlen
				|| (u32)(r.end - r.p) - datalen - instlen < addrlen) {
			seterr(err, errlen, "xdelta window runs past the end of the patch");
			free(dst);
			return -1;
		}

		u32 srcavail = 0;

		if (win & VCD_SOURCE) {
			if (srcpos > romlen) {
				seterr(err, errlen, "xdelta window reads outside the ROM: is this patch for a different base ROM?");
				free(dst);
				return -1;
			}
			src = rom + srcpos;
			srcavail = srclen < romlen - srcpos ? srclen : romlen - srcpos;
		} else if (win & VCD_TARGET) {
			if (srcpos > dstlen) {
				seterr(err, errlen, "xdelta window reads outside the output so far");
				free(dst);
				return -1;
			}
			src = dst + srcpos; // fixed up below after the grow
			srcavail = srclen < dstlen - srcpos ? srclen : dstlen - srcpos;
		}

		if (tgtlen > 0x10000000 || dstlen + tgtlen < dstlen) {
			seterr(err, errlen, "xdelta output is implausibly large");
			free(dst);
			return -1;
		}

		if (dstlen + tgtlen > dstcap) {
			u32 cap = dstcap ? dstcap : 0x100000;
			while (cap < dstlen + tgtlen) {
				cap *= 2;
			}
			u8 *grown = realloc(dst, cap);
			if (!grown) {
				seterr(err, errlen, "out of memory");
				free(dst);
				return -1;
			}
			if (win & VCD_TARGET) {
				src = grown + srcpos;
			}
			dst = grown;
			dstcap = cap;
		}

		data.p = r.p; data.end = r.p + datalen; data.bad = 0;
		inst.p = data.end; inst.end = inst.p + instlen; inst.bad = 0;
		addrs.p = inst.end; addrs.end = addrs.p + addrlen; addrs.bad = 0;
		r.p = addrs.end;

		w.src = src;
		w.srclen = srclen;
		w.srcavail = srcavail;
		w.tgt = dst + dstlen;
		w.tgtlen = tgtlen;
		w.tgtpos = 0;
		vcdCacheInit(&cache);

		while (inst.p < inst.end) {
			const struct vcdcode *code = &codetable[vcdByte(&inst)];
			u32 size1 = code->size1;
			u32 size2 = code->size2;

			if (code->inst1 != VCD_NOOP && size1 == 0) {
				size1 = vcdVarint(&inst);
			}
			if (!vcdRunInst(&w, &cache, code->inst1, size1, code->mode1, &data, &addrs)) {
				seterr(err, errlen, "xdelta instruction stream is corrupt");
				free(dst);
				return -1;
			}
			if (code->inst2 != VCD_NOOP) {
				if (size2 == 0) {
					size2 = vcdVarint(&inst);
				}
				if (!vcdRunInst(&w, &cache, code->inst2, size2, code->mode2, &data, &addrs)) {
					seterr(err, errlen, "xdelta instruction stream is corrupt");
					free(dst);
					return -1;
				}
			}
			if (inst.bad || data.bad || addrs.bad) {
				seterr(err, errlen, "xdelta window is truncated");
				free(dst);
				return -1;
			}
		}

		if (w.tgtpos != tgtlen) {
			seterr(err, errlen, "xdelta window came out the wrong size");
			free(dst);
			return -1;
		}

		if ((win & VCD_ADLER32) && adler32(adler32(0L, Z_NULL, 0), w.tgt, tgtlen) != adler) {
			seterr(err, errlen, "xdelta window checksum does not match: is this patch for a different base ROM?");
			free(dst);
			return -1;
		}

		dstlen += tgtlen;
	}

	if (r.bad) {
		seterr(err, errlen, "xdelta patch is truncated");
		free(dst);
		return -1;
	}

	*out = dst;
	*outlen = dstlen;
	return ROMPATCH_XDELTA;
}

/* -- BPS ------------------------------------------------------------------- */

static u32 bpsVarint(const u8 *p, u32 len, u32 *pos, s32 *bad)
{
	u32 result = 0;
	u32 shift = 1;

	for (;;) {
		u32 x;
		if (*pos >= len) {
			*bad = 1;
			return 0;
		}
		x = p[(*pos)++];
		result += (x & 0x7f) * shift;
		if (x & 0x80) {
			return result;
		}
		shift <<= 7;
		result += shift;
	}
}

static s32 applyBps(const u8 *rom, u32 romlen, const u8 *patch, u32 patchlen,
		u8 **out, u32 *outlen, char *err, u32 errlen)
{
	u32 pos = 4;
	s32 bad = 0;
	u32 srclen, dstlen, metalen, end;
	u32 outpos = 0, srcpos = 0, dstpos = 0;
	u8 *dst;

	if (patchlen < 16) {
		seterr(err, errlen, "BPS patch is too short");
		return -1;
	}

	if (crc32(0L, patch, patchlen - 4) != rd32le(patch + patchlen - 4)) {
		seterr(err, errlen, "BPS patch is corrupt (checksum mismatch)");
		return -1;
	}

	srclen = bpsVarint(patch, patchlen, &pos, &bad);
	dstlen = bpsVarint(patch, patchlen, &pos, &bad);
	metalen = bpsVarint(patch, patchlen, &pos, &bad);
	pos += metalen;

	if (bad || pos > patchlen) {
		seterr(err, errlen, "BPS header is corrupt");
		return -1;
	}

	if (srclen != romlen) {
		seterr(err, errlen, "BPS patch expects a source ROM of a different size");
		return -1;
	}

	if (crc32(0L, rom, romlen) != rd32le(patch + patchlen - 12)) {
		seterr(err, errlen, "BPS patch is for a different base ROM (source checksum mismatch)");
		return -1;
	}

	if (dstlen > 0x10000000) {
		seterr(err, errlen, "BPS output is implausibly large");
		return -1;
	}

	dst = calloc(dstlen ? dstlen : 1, 1);
	if (!dst) {
		seterr(err, errlen, "out of memory");
		return -1;
	}

	end = patchlen - 12;

	while (pos < end && !bad) {
		const u32 cmd = bpsVarint(patch, patchlen, &pos, &bad);
		const u32 action = cmd & 3;
		const u32 length = (cmd >> 2) + 1;

		if (bad || length > dstlen - outpos) {
			bad = 1;
			break;
		}

		if (action == 0) { // SourceRead
			if (length > romlen - outpos) { bad = 1; break; }
			memcpy(dst + outpos, rom + outpos, length);
			outpos += length;
		} else if (action == 1) { // TargetRead
			if (length > end - pos) { bad = 1; break; }
			memcpy(dst + outpos, patch + pos, length);
			pos += length;
			outpos += length;
		} else {
			const u32 raw = bpsVarint(patch, patchlen, &pos, &bad);
			const s32 delta = (s32)(raw >> 1) * ((raw & 1) ? -1 : 1);
			if (bad) { break; }
			if (action == 2) { // SourceCopy
				srcpos += delta;
				if (srcpos > romlen || length > romlen - srcpos) { bad = 1; break; }
				memcpy(dst + outpos, rom + srcpos, length);
				srcpos += length;
				outpos += length;
			} else { // TargetCopy
				dstpos += delta;
				if (dstpos >= outpos) { bad = 1; break; }
				for (u32 i = 0; i < length; ++i) {
					dst[outpos++] = dst[dstpos++];
				}
			}
		}
	}

	if (bad) {
		seterr(err, errlen, "BPS patch is corrupt");
		free(dst);
		return -1;
	}

	if (crc32(0L, dst, dstlen) != rd32le(patch + patchlen - 8)) {
		seterr(err, errlen, "BPS patch applied but the result checksum does not match");
		free(dst);
		return -1;
	}

	*out = dst;
	*outlen = dstlen;
	return ROMPATCH_BPS;
}

/* -- IPS ------------------------------------------------------------------- */

static s32 applyIps(const u8 *rom, u32 romlen, const u8 *patch, u32 patchlen,
		u8 **out, u32 *outlen, char *err, u32 errlen)
{
	u32 pos = 5;
	u32 dstlen = romlen;
	u8 *dst = malloc(romlen ? romlen : 1);

	if (!dst) {
		seterr(err, errlen, "out of memory");
		return -1;
	}

	memcpy(dst, rom, romlen);

	while (pos + 3 <= patchlen) {
		u32 ofs, size;

		if (!memcmp(patch + pos, "EOF", 3)) {
			if (patchlen - pos == 6) {
				const u32 trunc = rd24be(patch + pos + 3);
				if (trunc < dstlen) {
					dstlen = trunc;
				}
			}
			break;
		}

		if (pos + 5 > patchlen) {
			break;
		}

		ofs = rd24be(patch + pos);
		size = rd16be(patch + pos + 3);
		pos += 5;

		if (size) {
			if (pos + size > patchlen) {
				break;
			}
			if (ofs < dstlen) {
				const u32 n = size < dstlen - ofs ? size : dstlen - ofs;
				memcpy(dst + ofs, patch + pos, n);
			}
			pos += size;
		} else {
			u32 rle;
			if (pos + 3 > patchlen) {
				break;
			}
			rle = rd16be(patch + pos);
			if (ofs < dstlen) {
				const u32 n = rle < dstlen - ofs ? rle : dstlen - ofs;
				memset(dst + ofs, patch[pos + 2], n);
			}
			pos += 3;
		}
	}

	*out = dst;
	*outlen = dstlen;
	return ROMPATCH_IPS;
}

s32 rompatchApply(const u8 *rom, u32 romlen, const u8 *patch, u32 patchlen,
		u8 **out, u32 *outlen, char *err, u32 errlen)
{
	switch (rompatchIdentify(patch, patchlen)) {
	case ROMPATCH_XDELTA:
		return applyXdelta(rom, romlen, patch, patchlen, out, outlen, err, errlen);
	case ROMPATCH_BPS:
		return applyBps(rom, romlen, patch, patchlen, out, outlen, err, errlen);
	case ROMPATCH_IPS:
		return applyIps(rom, romlen, patch, patchlen, out, outlen, err, errlen);
	default:
		seterr(err, errlen, "not a patch format this can read (xdelta, BPS or IPS)");
		return -1;
	}
}
