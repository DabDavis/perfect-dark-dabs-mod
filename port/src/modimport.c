/**
 * In-game console mod import.
 *
 * A console mod is a patch against the stock ROM. The port cannot run the
 * patched ROM - it runs decomp code and reads the ROM at fixed offsets - but
 * it can use the mod's content: the asset files it replaced or added, the
 * segments it rebuilt, and the tables in its data segment. This diffs the
 * patched ROM against the stock one and writes out only what differs, in the
 * layout mod.c and romdata.c already load, so a patch dropped into mods/
 * becomes a mod directory without the player doing anything.
 *
 * It is a port of tools/importmod, step for step, and that script's comments
 * are the long form of everything here. Keep the two in step: the script is
 * what runs on a machine without the game, and its report is what gets
 * compared against this one when something looks off.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <zlib.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "versions.h"
#include "fs.h"
#include "system.h"
#include "romdata.h"
#include "rompatch.h"
#include "modimport.h"

#define ROM_SIZE 33554432

// How much of a segment to match on when hunting for it in the patched ROM,
// and how far that is stretched when the short match is not unique.
#define FINGERPRINT_LEN 256
#define FINGERPRINT_MAX 0x10000

// The data segment lives in the first few MB; no point scanning past that.
#define DATA_SEG_SEARCH_END 0x400000

// Bounds on a plausible file table.
#define MIN_FILES 500
#define MAX_FILES 8192

// texdecompress.c reads a replacement texture into a 4096 byte buffer.
#define MAX_TEXTURE_SIZE 4096

// The lib segment: a 1173 blob at a fixed offset, holding the code that
// reaches the audio banks, the sequences and the copyright segment.
#define LIB_OFS 0x3050

// The game binary's chunk table, and where the binary runs.
#define GAME_TABLE_OFS 0x4fc40
#define GAME_VRAM 0x7f000000

#define MAX_SEGS 40
#define MAX_NAME 256

/* -- report ---------------------------------------------------------------- */

static char *report;
static u32 reportLen;
static u32 reportCap;

static void rep(const char *fmt, ...)
{
	char line[1024];
	va_list args;
	u32 n;

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	n = strlen(line);

	if (reportLen + n + 2 > reportCap) {
		u32 cap = reportCap ? reportCap * 2 : 16384;
		while (cap < reportLen + n + 2) {
			cap *= 2;
		}
		char *grown = realloc(report, cap);
		if (!grown) {
			return;
		}
		report = grown;
		reportCap = cap;
	}

	memcpy(report + reportLen, line, n);
	reportLen += n;
	report[reportLen++] = '\n';
	report[reportLen] = '\0';
}

/* -- bytes ----------------------------------------------------------------- */

static char *dupstr(const char *s)
{
	const u32 n = strlen(s);
	char *d = malloc(n + 1);
	if (d) {
		memcpy(d, s, n + 1);
	}
	return d;
}

static u32 be32(const u8 *b, u32 ofs) { return ((u32)b[ofs] << 24) | ((u32)b[ofs + 1] << 16) | ((u32)b[ofs + 2] << 8) | b[ofs + 3]; }
static u32 be16(const u8 *b, u32 ofs) { return ((u32)b[ofs] << 8) | b[ofs + 1]; }
static u32 be24(const u8 *b, u32 ofs) { return ((u32)b[ofs] << 16) | ((u32)b[ofs + 1] << 8) | b[ofs + 2]; }
static u32 align16(u32 n) { return (n + 15) & ~15u; }
static u32 umin(u32 a, u32 b) { return a < b ? a : b; }
static u32 umax(u32 a, u32 b) { return a > b ? a : b; }

static s32 is1173(const u8 *buf, u32 len, u32 ofs)
{
	return ofs + 5 <= len && buf[ofs] == 0x11 && buf[ofs + 1] == 0x73;
}

/**
 * Inflates a 1173 blob: the 5 byte header is 1173 then a 24 bit inflated
 * length, the rest raw deflate. Returns the data, its length, and how many
 * bytes of the input the stream occupied (the ROM pads files past it).
 */
static u8 *inflate1173(const u8 *buf, u32 len, u32 ofs, u32 *outlen, u32 *consumed)
{
	z_stream zs;
	u32 declared;
	u8 *out;
	int ret;

	if (!is1173(buf, len, ofs)) {
		return NULL;
	}

	declared = be24(buf, ofs + 2);
	out = malloc(declared ? declared : 1);

	if (!out) {
		return NULL;
	}

	memset(&zs, 0, sizeof(zs));

	if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
		free(out);
		return NULL;
	}

	zs.next_in = (Bytef *)(buf + ofs + 5);
	zs.avail_in = len - ofs - 5;
	zs.next_out = out;
	zs.avail_out = declared;

	do {
		ret = inflate(&zs, Z_NO_FLUSH);
	} while (ret == Z_OK && zs.avail_out > 0 && zs.avail_in > 0);

	if (ret != Z_STREAM_END && !(ret == Z_OK && zs.avail_out == 0) && ret != Z_BUF_ERROR) {
		inflateEnd(&zs);
		free(out);
		return NULL;
	}

	if (ret == Z_BUF_ERROR && zs.avail_out != 0) {
		inflateEnd(&zs);
		free(out);
		return NULL;
	}

	*outlen = zs.total_out;
	*consumed = 5 + zs.total_in;
	inflateEnd(&zs);

	return out;
}

// bytes.find(needle, from, to): the first match starting in [from, to), or -1
static s32 findBytes(const u8 *hay, u32 haylen, u32 from, u32 to, const u8 *needle, u32 nlen)
{
	if (nlen == 0 || nlen > haylen) {
		return -1;
	}

	to = umin(to, haylen);

	if (to < nlen || from > to - nlen) {
		return -1;
	}

	const u8 *p = hay + from;
	const u8 *end = hay + to - nlen + 1;

	while (p < end) {
		const u8 *q = memchr(p, needle[0], end - p);
		if (!q) {
			return -1;
		}
		if (!memcmp(q, needle, nlen)) {
			return (s32)(q - hay);
		}
		p = q + 1;
	}

	return -1;
}

/* -- ROM parsing ----------------------------------------------------------- */

struct romfiles {
	u32 dataofs;    // ROM offset of the 1173 data segment
	u8 *dataseg;
	u32 dataseglen;
	u32 filesofs;   // the file table in it
	u32 *offsets;   // the table, index 0 unused, last entry the name table
	u32 numoffsets;
	char **names;   // names[0] = ""; owned unless == stockNames
	u32 numnames;
	// every asset, by name, with padding trimmed
	struct fileent {
		const char *name;
		u32 ofs;
		u32 len;
	} *ents;
	u32 numents;
};

// The stock ROM's file names, once it has been read. The game never reads the
// name table, so an old mod tool could drop it; the file ids still mean the
// same files, so the stock names stand in for them.
static char **stockNames;
static u32 numStockNames;
static s32 namesMissingNoted;

static void freeNames(char **names, u32 n)
{
	if (!names || names == stockNames) {
		return;
	}
	for (u32 i = 0; i < n; ++i) {
		free(names[i]);
	}
	free(names);
}

static s32 namesMissing(u32 count)
{
	return stockNames && (count > numStockNames ? count - numStockNames : numStockNames - count) <= 1;
}

/**
 * Names are a zero terminated array of offsets relative to the table,
 * followed by the strings. Anything unreadable ends the table rather than
 * rejecting it: mods leave junk in the spare slots past their last real file.
 */
static char **readNameTable(const u8 *rom, u32 romlen, u32 ofs, u32 *count)
{
	char **names;
	u32 n = 1;
	u32 prev = 0;
	u32 i;

	if (ofs + 8 > romlen || be32(rom, ofs) != 0) {
		return NULL;
	}

	names = calloc(MAX_FILES + 2, sizeof(char *));
	if (!names) {
		return NULL;
	}
	names[0] = dupstr("");

	for (i = ofs + 4; i + 4 <= romlen && n <= MAX_FILES; i += 4) {
		const u32 rel = be32(rom, i);
		u32 end, k;

		if (rel == 0 || rel < prev || ofs + rel >= romlen) {
			break;
		}

		end = ofs + rel;
		while (end < romlen && end < ofs + rel + 256 && rom[end] != 0) {
			++end;
		}
		if (end >= romlen || rom[end] != 0) {
			break;
		}

		for (k = ofs + rel; k < end; ++k) {
			if (rom[k] < 32 || rom[k] >= 127) {
				break;
			}
		}
		if (k < end) {
			break;
		}

		names[n] = malloc(end - (ofs + rel) + 1);
		memcpy(names[n], rom + ofs + rel, end - (ofs + rel));
		names[n][end - (ofs + rel)] = '\0';
		++n;
		prev = rel;
	}

	if (n <= MIN_FILES) {
		freeNames(names, n);
		return NULL;
	}

	*count = n;
	return names;
}

/**
 * The file table is a run of ascending big endian ROM offsets whose last
 * entry points at the name table. Mods move it, so look for that shape.
 */
static s32 findFileTable(const u8 *rom, u32 romlen, const u8 *dataseg, u32 dataseglen)
{
	s32 runstart = -1;
	u32 prev = 0;
	u32 i;

	for (i = 0; i + 4 <= dataseglen; i += 4) {
		const u32 v = be32(dataseg, i);

		if (v >= 0x1000 && v <= romlen && v >= prev) {
			if (runstart < 0) {
				runstart = i;
			}
			prev = v;
		} else {
			if (runstart >= 0 && (i - runstart) / 4 > MIN_FILES) {
				u32 count;
				char **names = readNameTable(rom, romlen, prev, &count);
				if (names || namesMissing((i - runstart) / 4)) {
					freeNames(names, count);
					// step back onto the leading zero entry so that file
					// numbers and name numbers line up
					if (runstart >= 4 && be32(dataseg, runstart - 4) == 0) {
						return runstart - 4;
					}
					return runstart;
				}
			}
			runstart = -1;
			prev = 0;
		}
	}

	return -1;
}

static s32 tryDataSegment(const u8 *rom, u32 romlen, u32 ofs, u8 **dataseg, u32 *dataseglen, u32 *filesofs)
{
	u32 len, consumed;
	u8 *seg = inflate1173(rom, romlen, ofs, &len, &consumed);
	s32 f;

	if (!seg) {
		return 0;
	}
	if (len < 0x10000) {
		free(seg);
		return 0;
	}

	f = findFileTable(rom, romlen, seg, len);
	if (f < 0) {
		free(seg);
		return 0;
	}

	*dataseg = seg;
	*dataseglen = len;
	*filesofs = f;
	return 1;
}

/**
 * The compressed data segment holds the file table. It sits at a fixed offset
 * in the stock ROM, but a mod that rebuilds it moves it - so fall back to
 * hunting for the 1173 blob that actually contains a file table.
 */
static s32 findDataSegment(const u8 *rom, u32 romlen, u32 expected, u32 *dataofs, u8 **dataseg, u32 *dataseglen, u32 *filesofs)
{
	if (tryDataSegment(rom, romlen, expected, dataseg, dataseglen, filesofs)) {
		*dataofs = expected;
		return 1;
	}

	for (u32 i = 0; i + 5 <= umin(romlen, DATA_SEG_SEARCH_END); ++i) {
		if (rom[i] == 0x11 && rom[i + 1] == 0x73) {
			const u32 length = be24(rom, i + 2);
			if (length > 0x10000 && length < 0x400000
					&& tryDataSegment(rom, romlen, i, dataseg, dataseglen, filesofs)) {
				*dataofs = i;
				return 1;
			}
		}
	}

	return 0;
}

/**
 * Mirrors romdataInitFiles(): the table is a run of ascending big endian ROM
 * offsets, and the last of them points at the name table rather than a file.
 * Stops at anything that is not a plausible next offset.
 */
static s32 readFileTable(const u8 *rom, u32 romlen, const u8 *dataseg, u32 dataseglen, u32 filesofs,
		u32 **offsets, u32 *numoffsets, char ***names, u32 *numnames)
{
	u32 *offs = malloc((MAX_FILES + 2) * sizeof(u32));
	u32 n = 1;
	u32 i;

	if (!offs) {
		return 0;
	}

	offs[0] = be32(dataseg, filesofs);

	for (i = filesofs + 4; i + 4 <= dataseglen && n < MAX_FILES + 1; i += 4) {
		const u32 ofs = be32(dataseg, i);
		if (ofs < 0x1000 || ofs < offs[n - 1] || ofs > romlen) {
			break;
		}
		offs[n++] = ofs;
	}

	if (n < MIN_FILES) {
		free(offs);
		return 0;
	}

	*names = readNameTable(rom, romlen, offs[n - 1], numnames);

	if (!*names && namesMissing(n)) {
		if (!namesMissingNoted) {
			rep("note:    this ROM has no file name table (an old mod tool dropped it); using the");
			rep("         stock ROM's names, since its %u file ids line up with the stock %u",
					n - 2, numStockNames - 1);
			rep("");
			namesMissingNoted = 1;
		}
		*names = stockNames;
		*numnames = numStockNames;
	}

	if (!*names) {
		free(offs);
		return 0;
	}

	*offsets = offs;
	*numoffsets = n;
	return 1;
}

/**
 * Files are padded to an alignment in the ROM. Drop whatever the deflate
 * stream did not consume so identical files compare equal.
 */
static u32 trimPadding(const u8 *rom, u32 romlen, u32 ofs, u32 len)
{
	if (is1173(rom, romlen, ofs)) {
		u32 outlen, consumed;
		u8 *tmp = inflate1173(rom, umin(romlen, ofs + len), ofs, &outlen, &consumed);
		if (tmp) {
			free(tmp);
			return umin(consumed, len);
		}
	}
	return len;
}

static int compareEnts(const void *a, const void *b)
{
	return strcmp(((const struct fileent *)a)->name, ((const struct fileent *)b)->name);
}

static void freeRomFiles(struct romfiles *f)
{
	free(f->dataseg);
	free(f->offsets);
	freeNames(f->names, f->numnames);
	free(f->ents);
	memset(f, 0, sizeof(*f));
}

/**
 * Every asset in the ROM, by its ROM filename. Returns 0 with the reason
 * reported when the ROM has no readable file table.
 */
static s32 readFiles(const u8 *rom, u32 romlen, u32 expectedDataOfs, const char *label, struct romfiles *f)
{
	memset(f, 0, sizeof(*f));

	if (!findDataSegment(rom, romlen, expectedDataOfs, &f->dataofs, &f->dataseg, &f->dataseglen, &f->filesofs)) {
		rep("error: could not find a file table in the %s ROM.", label);
		rep("       Either it is not a Perfect Dark ROM or the mod rebuilt the data");
		rep("       segment in a shape this tool does not recognise.");
		return 0;
	}

	if (!readFileTable(rom, romlen, f->dataseg, f->dataseglen, f->filesofs, &f->offsets, &f->numoffsets, &f->names, &f->numnames)) {
		rep("error: could not read the file table in the %s ROM.", label);
		freeRomFiles(f);
		return 0;
	}

	if (!strcmp(label, "stock")) {
		stockNames = f->names;
		numStockNames = f->numnames;
	}

	f->ents = malloc(f->numoffsets * sizeof(struct fileent));
	f->numents = 0;

	// index 0 is unused, and the final offset is the name table
	for (u32 n = 1; n + 1 < f->numoffsets; ++n) {
		const char *name;

		if (n >= f->numnames) {
			break;
		}

		name = f->names[n];

		if (!name[0]) {
			continue; // unnamed spare slot
		}

		if (name[0] == '/' || strstr(name, "..")) {
			rep("warning: skipping file %u with suspicious name '%s'", n, name);
			continue;
		}

		f->ents[f->numents].name = name;
		f->ents[f->numents].ofs = f->offsets[n];
		f->ents[f->numents].len = trimPadding(rom, romlen, f->offsets[n], f->offsets[n + 1] - f->offsets[n]);
		++f->numents;
	}

	qsort(f->ents, f->numents, sizeof(struct fileent), compareEnts);

	return 1;
}

static const struct fileent *findEnt(const struct romfiles *f, const char *name)
{
	s32 lo = 0;
	s32 hi = (s32)f->numents - 1;

	while (lo <= hi) {
		const s32 mid = (lo + hi) / 2;
		const int c = strcmp(f->ents[mid].name, name);
		if (c == 0) {
			return &f->ents[mid];
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	return NULL;
}

/* -- file format checks ---------------------------------------------------- */

// Each returns a static reason string, or NULL when the file reads right.

static const char *checkPads(const u8 *d, u32 n)
{
	static char reason[128];
	u32 npads, ncovers, first;

	if (n < 24) {
		return "too short for a pads file";
	}

	npads = be32(d, 0);
	ncovers = be32(d, 4);

	if (npads > 20000 || ncovers > 20000) {
		snprintf(reason, sizeof(reason), "implausible pad/cover counts (%u/%u)", npads, ncovers);
		return reason;
	}

	for (u32 k = 8; k <= 16; k += 4) {
		const u32 ofs = be32(d, k);
		if (!(ofs > 0 && ofs <= n)) {
			snprintf(reason, sizeof(reason), "header pointer %#x is outside the file", ofs);
			return reason;
		}
	}

	first = 20 + npads * 2;
	if (first > n) {
		return "offset table does not fit";
	}

	for (u32 i = 0; i < npads; ++i) {
		const u32 ofs = be16(d, 20 + i * 2);
		if (!(first <= ofs && ofs < n)) {
			return "the pad offset table is not the stock 16 bit one";
		}
	}

	return NULL;
}

static const char *checkSetup(const u8 *d, u32 n)
{
	static char reason[128];
	static const char *const fields[] = { "intro", "props", "paths", "ailists" };

	if (n < 32) {
		return "too short for a setup file";
	}

	for (u32 k = 0; k < 4; ++k) {
		const u32 ofs = be32(d, (3 + k) * 4);
		if (!(ofs > 0 && ofs <= n)) {
			snprintf(reason, sizeof(reason), "%s pointer %#x is outside the file", fields[k], ofs);
			return reason;
		}
	}

	if (n < 0x80) {
		snprintf(reason, sizeof(reason), "stub setup (%u bytes, no objects); not a Combat Simulator arena", n);
		return reason;
	}

	return NULL;
}

static const char *checkBg(const u8 *d, u32 n)
{
	static char reason[128];
	u32 inflated;

	if (n < 16) {
		return "too short for a background file";
	}

	inflated = be32(d, 0);

	if (inflated < 0x100) {
		snprintf(reason, sizeof(reason), "stub background (%u byte primary section); the mod removed this stage", inflated);
		return reason;
	}

	if (!is1173(d, n, 12)) {
		return "primary section is not 1173-compressed";
	}

	return NULL;
}

// model node type -> rodata size, as filemodel.c has them; 0 = no rodata
static const struct { u8 type; const char *name; u8 size; } modelNodeRodata[] = {
	{ 0x01, "chrinfo", 0x0c }, { 0x02, "position", 0x18 }, { 0x04, "gundl", 0x14 },
	{ 0x08, "distance", 0x10 }, { 0x09, "reorder", 0x24 }, { 0x0a, "bbox", 0x1c },
	{ 0x0c, "chrgunfire", 0x28 }, { 0x11, "type11", 0x20 }, { 0x12, "toggle", 0x08 },
	{ 0x15, "positionheld", 0x14 }, { 0x16, "stargunfire", 0x10 }, { 0x17, "headspot", 0x04 },
	{ 0x18, "dl", 0x18 }, { 0x19, "type19", 0x04 },
};

struct modelwalk {
	const u8 *d;
	u32 n;
	u8 *nodeseen;   // bitmaps over the file
	u8 *gdlseen;
	struct { u32 ofs; s32 kind; } *rodatas;
	u32 numrodatas;
	u32 caprodatas;
	char reason[160];
	s32 bad;
};

static s32 modelBad(struct modelwalk *w, const char *fmt, ...)
{
	va_list args;
	if (!w->bad) {
		va_start(args, fmt);
		vsnprintf(w->reason, sizeof(w->reason), fmt, args);
		va_end(args);
		w->bad = 1;
	}
	return 0;
}

static s32 seenTest(u8 *bits, u32 i) { return bits[i >> 3] & (1 << (i & 7)); }
static void seenSet(u8 *bits, u32 i) { bits[i >> 3] |= (1 << (i & 7)); }

// A segmented pointer into the file, as an offset; 0 stays 0. -1 when bad.
static s32 modelPtr(struct modelwalk *w, u32 v, const char *what)
{
	if (v == 0) {
		return 0;
	}
	if ((v >> 24) != 0x05) {
		modelBad(w, "%s pointer %08x is not in segment 5", what, v);
		return -1;
	}
	if ((v & 0xffffff) >= w->n) {
		modelBad(w, "%s pointer %08x is past the end of the file (%u bytes)", what, v, w->n);
		return -1;
	}
	return (s32)(v & 0xffffff);
}

static s32 modelFits(struct modelwalk *w, u32 o, u32 size, const char *what)
{
	if (o + size > w->n) {
		modelBad(w, "%s at %#x runs past the end of the file", what, o);
		return 0;
	}
	return 1;
}

static s32 modelRodataIndex(u32 kind)
{
	for (u32 i = 0; i < sizeof(modelNodeRodata) / sizeof(modelNodeRodata[0]); ++i) {
		if (modelNodeRodata[i].type == kind) {
			return (s32)i;
		}
	}
	return -1;
}

static void modelNode(struct modelwalk *w, s32 o, const char *what)
{
	u32 t, kind, rodata;
	s32 ri;

	if (w->bad || o <= 0 || seenTest(w->nodeseen, o)) {
		return;
	}
	seenSet(w->nodeseen, o);

	if (!modelFits(w, o, 0x18, what)) {
		return;
	}

	t = be16(w->d, o);
	kind = t & 0xff;
	rodata = be32(w->d, o + 4);
	ri = modelRodataIndex(kind);

	if (ri < 0) {
		if (rodata) {
			modelBad(w, "node at %#x has type %#x, which the port has no rodata for, yet points at rodata", o, t);
			return;
		}
		if (kind > 0x19) {
			modelBad(w, "node at %#x has type %#x, past the port's table", o, t);
			return;
		}
	} else {
		char rname[48];
		snprintf(rname, sizeof(rname), "%s rodata", modelNodeRodata[ri].name);
		const s32 r = modelPtr(w, rodata, rname);
		if (r < 0) {
			return;
		}
		if (r) {
			if (!modelFits(w, r, modelNodeRodata[ri].size, rname)) {
				return;
			}
			if (w->numrodatas == w->caprodatas) {
				w->caprodatas = w->caprodatas ? w->caprodatas * 2 : 256;
				w->rodatas = realloc(w->rodatas, w->caprodatas * sizeof(w->rodatas[0]));
			}
			w->rodatas[w->numrodatas].ofs = r;
			w->rodatas[w->numrodatas].kind = ri;
			++w->numrodatas;
		}
	}

	{
		static const struct { u8 field; const char *what; } links[] = {
			{ 8, "parent" }, { 12, "next" }, { 16, "prev" }, { 20, "child" },
		};
		for (u32 k = 0; k < 4 && !w->bad; ++k) {
			const s32 p = modelPtr(w, be32(w->d, o + links[k].field), links[k].what);
			if (p < 0) {
				return;
			}
			modelNode(w, p, links[k].what);
		}
	}
}

static void modelGdl(struct modelwalk *w, s32 o, const char *what)
{
	u32 p;

	if (w->bad || o <= 0 || seenTest(w->gdlseen, o)) {
		return;
	}
	seenSet(w->gdlseen, o);

	for (p = o;; p += 8) {
		u8 op;
		if (p + 8 > w->n) {
			modelBad(w, "%s display list at %#x runs off the file without G_ENDDL", what, o);
			return;
		}
		op = w->d[p];
		if (op == 0xb8) { // G_ENDDL
			return;
		}
		if (op == 0x06) { // G_DL: branch or call to another list in the file
			const s32 t = modelPtr(w, be32(w->d, p + 4), "G_DL target");
			if (t < 0) {
				return;
			}
			modelGdl(w, t, what);
			if (w->bad) {
				return;
			}
		}
	}
}

/**
 * See port/src/preprocess/filemodel.c: populateMarkers() follows every
 * pointer in the file with no bounds check, and convertContent() then reads
 * at each one. A model built for a mod's own code can point outside the file
 * and the port crashes reading it. This walks the same graph and says where
 * it leaves the file.
 */
static const char *checkModel(const u8 *d, u32 n)
{
	static char reason[160];
	struct modelwalk w;
	s32 rootnode, parts, texconfigs;
	u32 numparts, numtexconfigs;

	if (n < 0x1c) {
		snprintf(reason, sizeof(reason), "too short for a model file (%u bytes)", n);
		return reason;
	}

	memset(&w, 0, sizeof(w));
	w.d = d;
	w.n = n;
	w.nodeseen = calloc((n + 7) / 8 + 1, 1);
	w.gdlseen = calloc((n + 7) / 8 + 1, 1);

	rootnode = modelPtr(&w, be32(d, 0), "root node");
	parts = modelPtr(&w, be32(d, 8), "parts");
	numparts = be16(d, 12);
	numtexconfigs = be16(d, 22);
	texconfigs = modelPtr(&w, be32(d, 24), "texture configs");

	if (!w.bad && numparts > 512) {
		modelBad(&w, "%u parts", numparts);
	}
	if (!w.bad && numtexconfigs > 512) {
		modelBad(&w, "%u texture configs", numtexconfigs);
	}
	if (!w.bad && numtexconfigs && texconfigs > 0) {
		modelFits(&w, texconfigs, numtexconfigs * 12, "texture configs");
	}
	if (!w.bad && parts > 0) {
		modelFits(&w, parts, numparts * 6, "parts table");
	}

	if (!w.bad) {
		modelNode(&w, rootnode, "root");
	}

	if (!w.bad && parts > 0) {
		for (u32 i = 0; i < numparts && !w.bad; ++i) {
			char what[32];
			snprintf(what, sizeof(what), "part %u", i);
			const s32 p = modelPtr(&w, be32(d, parts + i * 4), what);
			if (p >= 0) {
				modelNode(&w, p, what);
			}
		}
	}

	for (u32 i = 0; i < w.numrodatas && !w.bad; ++i) {
		const u32 r = w.rodatas[i].ofs;
		const char *name = modelNodeRodata[w.rodatas[i].kind].name;
		s32 v;

		if (!strcmp(name, "gundl")) {
			modelGdl(&w, modelPtr(&w, be32(d, r), "opaque"), "gundl");
			modelGdl(&w, modelPtr(&w, be32(d, r + 4), "translucent"), "gundl");
			v = modelPtr(&w, be32(d, r + 12), "gundl vertices");
			if (v >= 0) {
				modelFits(&w, v, be16(d, r + 16) * 12, "gundl vertices");
			}
		} else if (!strcmp(name, "dl")) {
			modelGdl(&w, modelPtr(&w, be32(d, r), "opaque"), "dl");
			modelGdl(&w, modelPtr(&w, be32(d, r + 4), "translucent"), "dl");
			modelPtr(&w, be32(d, r + 8), "dl colours");
			v = modelPtr(&w, be32(d, r + 12), "dl vertices");
			if (v >= 0) {
				modelFits(&w, v, be16(d, r + 16) * 12, "dl vertices");
			}
		} else if (!strcmp(name, "stargunfire")) {
			v = modelPtr(&w, be32(d, r + 4), "stargunfire vertices");
			if (v >= 0) {
				modelFits(&w, v, be32(d, r) * 4 * 12, "stargunfire vertices");
			}
			modelGdl(&w, modelPtr(&w, be32(d, r + 8), "stargunfire"), "stargunfire");
		} else if (!strcmp(name, "distance")) {
			modelNode(&w, modelPtr(&w, be32(d, r + 8), "distance target"), "distance target");
		} else if (!strcmp(name, "toggle")) {
			modelNode(&w, modelPtr(&w, be32(d, r), "toggle target"), "toggle target");
		} else if (!strcmp(name, "reorder")) {
			modelNode(&w, modelPtr(&w, be32(d, r + 24), "reorder node"), "reorder node");
			modelNode(&w, modelPtr(&w, be32(d, r + 28), "reorder node"), "reorder node");
		} else if (!strcmp(name, "chrgunfire")) {
			v = modelPtr(&w, be32(d, r + 24), "chrgunfire texture");
			if (v >= 0) {
				modelFits(&w, v, 12, "chrgunfire texture config");
			}
		} else if (!strcmp(name, "type19")) {
			modelFits(&w, r, 4 + be32(d, r) * 12, "type19 vertices");
		}
	}

	for (u32 i = 0; i < numtexconfigs && !w.bad && texconfigs > 0; ++i) {
		const u32 o = texconfigs + i * 12;
		const u32 v = be32(d, o);
		if ((v >> 24) == 0x05) {
			modelPtr(&w, v, "texture data");
		} else if (v >= 0x10000) {
			modelBad(&w, "texture config %u refers to texture %#x", i, v);
		}
	}

	free(w.nodeseen);
	free(w.gdlseen);
	free(w.rodatas);

	if (w.bad) {
		snprintf(reason, sizeof(reason), "%s", w.reason);
		return reason;
	}

	return NULL;
}

/**
 * Is this file in the format the port's preprocessors expect? A mod built
 * for its own modified game code can ship assets in a format only that code
 * reads, and the port will crash on them rather than reject them.
 */
static const char *checkFile(const char *name, const u8 *content, u32 len)
{
	const u8 *data = content;
	u32 datalen = len;
	u8 *inflated = NULL;
	const char *base = strrchr(name, '/');
	const char *reason = NULL;
	u32 baselen;

	base = base ? base + 1 : name;
	baselen = strlen(base);

	if (is1173(content, len, 0)) {
		u32 consumed;
		inflated = inflate1173(content, len, 0, &datalen, &consumed);
		if (!inflated) {
			return "not decompressible";
		}
		data = inflated;
	}

	if (baselen >= 5 && !strcmp(base + baselen - 5, "padsZ")) {
		reason = checkPads(data, datalen);
	} else if (base[0] == 'U') {
		reason = checkSetup(data, datalen);
	} else if (!strncmp(base, "bg_", 3) && baselen >= 4 && !strcmp(base + baselen - 4, ".seg")) {
		reason = checkBg(data, datalen);
	} else if ((base[0] == 'C' || base[0] == 'P' || base[0] == 'G') && baselen && base[baselen - 1] == 'Z') {
		// chr bodies and heads, props, guns and hands: one converter
		reason = checkModel(data, datalen);
	}

	free(inflated);
	return reason;
}

static s32 contentEqual(const u8 *a, u32 alen, const u8 *b, u32 blen)
{
	if (alen == blen && !memcmp(a, b, alen)) {
		return 1;
	}

	// different compressed bytes can still be the same asset
	if (is1173(a, alen, 0) && is1173(b, blen, 0)) {
		u32 la, lb, ca, cb;
		u8 *ia = inflate1173(a, alen, 0, &la, &ca);
		u8 *ib = inflate1173(b, blen, 0, &lb, &cb);
		const s32 eq = ia && ib && la == lb && !memcmp(ia, ib, la);
		free(ia);
		free(ib);
		return eq;
	}

	return 0;
}

/* -- segments -------------------------------------------------------------- */

struct seg {
	const char *name;
	u32 ofs;
	u32 size;
	s32 modofs;      // -1: not located
	u32 modsize;
	s32 hasmodsize;
	const char *note;
	char found[64];
	s32 assumed;
	s32 resized;
};

static int compareSegs(const void *a, const void *b)
{
	const u32 x = ((const struct seg *)a)->ofs;
	const u32 y = ((const struct seg *)b)->ofs;
	return x < y ? -1 : x > y ? 1 : 0;
}

/**
 * The port's segment table for this ROM, sorted by offset, with each
 * segment's stock size resolved the way romdataInitSegment() resolves it.
 */
static s32 stockSegments(struct seg *segs)
{
	s32 n = 0;

	for (s32 i = 0; i < romdataGetNumSegments() && n < MAX_SEGS; ++i) {
		u32 ofs, size;
		const char *name = romdataGetSegmentInfo(i, &ofs, &size);
		if (!name || !ofs) {
			continue;
		}
		memset(&segs[n], 0, sizeof(segs[n]));
		segs[n].name = name;
		segs[n].ofs = ofs;
		segs[n].size = size;
		segs[n].modofs = -1;
		++n;
	}

	qsort(segs, n, sizeof(struct seg), compareSegs);

	for (s32 i = 0; i < n; ++i) {
		if (!segs[i].size) {
			segs[i].size = (i + 1 < n ? segs[i + 1].ofs : ROM_SIZE) - segs[i].ofs;
		}
	}

	return n;
}

static struct seg *segByName(struct seg *segs, s32 n, const char *name)
{
	for (s32 i = 0; i < n; ++i) {
		if (!strcmp(segs[i].name, name)) {
			return &segs[i];
		}
	}
	return NULL;
}

/**
 * Find where a segment moved to. Its bytes are copied verbatim by the patch
 * wherever the mod did not touch it, so an exact match pins it down.
 */
static s32 locateSegment(const u8 *stock, const u8 *mod, u32 modlen, const struct seg *seg, s32 hint, const char **note)
{
	static const u32 lengths[] = { FINGERPRINT_LEN, FINGERPRINT_LEN * 16, FINGERPRINT_MAX };
	s32 at = -1;

	*note = NULL;

	for (u32 li = 0; li < 3; ++li) {
		const u32 fplen = umin(lengths[li], seg->size);
		const u8 *fp = stock + seg->ofs;
		s32 near;

		if (fplen < 16) {
			*note = "too small to fingerprint";
			return -1;
		}

		// try the shift the previous segment used, then no shift at all
		{
			const s32 cands[2] = { (s32)seg->ofs + hint, (s32)seg->ofs };
			for (s32 c = 0; c < 2; ++c) {
				const s32 cand = cands[c];
				if (cand >= 0 && (u32)cand + fplen <= modlen && !memcmp(mod + cand, fp, fplen)) {
					return cand;
				}
			}
		}

		near = seg->ofs > 0x200000 ? (s32)(seg->ofs - 0x200000) : 0;
		at = findBytes(mod, modlen, near, seg->ofs + 0x200000, fp, fplen);
		if (at < 0) {
			at = findBytes(mod, modlen, 0, modlen, fp, fplen);
		}
		if (at < 0) {
			*note = "not found";
			return -1;
		}
		if (findBytes(mod, modlen, at + 1, modlen, fp, fplen) < 0) {
			return at;
		}
		if (fplen >= seg->size) {
			break;
		}
	}

	*note = "ambiguous";
	return at;
}

/* -- code following -------------------------------------------------------- */

struct pairs {
	u32 nwords;
	u32 *addr;
	u8 *has;
};

/**
 * Every lui and the instruction after it that completes an address, as
 * {index of the lui: address formed}. What the game code reaches its tables
 * through.
 */
static void addressPairs(const u8 *code, u32 len, struct pairs *p)
{
	p->nwords = len / 4;
	p->addr = calloc(p->nwords ? p->nwords : 1, sizeof(u32));
	p->has = calloc(p->nwords ? p->nwords : 1, 1);

	for (u32 i = 0; i < p->nwords; ++i) {
		const u32 x = be32(code, i * 4);
		u32 rt;

		if (((x >> 16) & 0xfc00) != 0x3c00) {
			continue;
		}

		rt = (x >> 16) & 0x1f;

		for (u32 j = i + 1; j < i + 4 && j < p->nwords; ++j) {
			const u32 y = be32(code, j * 4);
			const u32 op = y >> 26;
			if (((y >> 21) & 0x1f) == rt && (op == 0x08 || op == 0x09 || op == 0x20 || op == 0x21
					|| op == 0x23 || op == 0x24 || op == 0x25 || op == 0x28 || op == 0x29 || op == 0x2b)) {
				u32 addr = ((x & 0xffff) << 16) + (y & 0xffff);
				if (y & 0x8000) {
					addr -= 0x10000;
				}
				p->addr[i] = addr;
				p->has[i] = 1;
				break;
			}
		}
	}
}

static void freePairs(struct pairs *p)
{
	free(p->addr);
	free(p->has);
	memset(p, 0, sizeof(*p));
}

struct votes {
	u32 addr[64];
	s32 n[64];
	s32 count;
};

static void vote(struct votes *v, u32 addr, s32 n)
{
	for (s32 i = 0; i < v->count; ++i) {
		if (v->addr[i] == addr) {
			v->n[i] += n;
			return;
		}
	}
	if (v->count < 64) {
		v->addr[v->count] = addr;
		v->n[v->count] = n;
		++v->count;
	}
}

static s32 bestVote(const struct votes *v, u32 *addr)
{
	s32 best = -1;
	for (s32 i = 0; i < v->count; ++i) {
		if (best < 0 || v->n[i] > v->n[best]) {
			best = i;
		}
	}
	if (best < 0) {
		return 0;
	}
	*addr = v->addr[best];
	return v->n[best];
}

/**
 * Where the mod's code keeps a table the stock code kept at stockaddr. Any
 * pair landing inside the table counts and the offset is taken back off.
 * Returns the vote count, 0 for none.
 */
static s32 followTable(const struct pairs *sp, const struct pairs *mp, u32 stockaddr, u32 size, u32 *addr)
{
	struct votes v;
	v.count = 0;

	for (u32 i = 0; i < sp->nwords && i < mp->nwords; ++i) {
		if (sp->has[i] && mp->has[i]) {
			const u32 delta = sp->addr[i] - stockaddr;
			if (delta < size) {
				vote(&v, mp->addr[i] - delta, 1);
			}
		}
	}

	return bestVote(&v, addr);
}

/**
 * The immediate a function loads, as the mod's code has it. Returns -1 when
 * the stock instruction is not there or the mod's word at that spot is not
 * the same instruction with another number.
 */
static s32 followImmediate(const u8 *stockcode, u32 stocklen, const u8 *modcode, u32 modlen,
		u32 start, u32 end, u32 stockvalue)
{
	for (u32 ofs = start; ofs + 4 <= end && ofs + 4 <= stocklen && ofs + 4 <= modlen; ofs += 4) {
		const u32 x = be32(stockcode, ofs);
		if (((x >> 26) == 0x08 || (x >> 26) == 0x09) && ((x >> 21) & 0x1f) == 0 && (x & 0xffff) == stockvalue) {
			const u32 y = be32(modcode, ofs);
			if ((y >> 16) == (x >> 16)) {
				return (s32)(y & 0xffff);
			}
			return -1;
		}
	}
	return -1;
}

/**
 * The chunk table is a run of offsets, each landing on a 1173 header a
 * couple of bytes along. Distinctive enough to find when a mod has moved it.
 */
static s32 gameTableValid(const u8 *rom, u32 romlen, u32 at)
{
	for (u32 i = 0; i < 4; ++i) {
		u32 v;
		if (at + i * 4 + 4 > romlen) {
			return 0;
		}
		v = be32(rom, at + i * 4);
		if (!(v > 0 && v < 0x400000) || at + v + 4 > romlen) {
			return 0;
		}
		if (rom[at + v + 2] != 0x11 || rom[at + v + 3] != 0x73) {
			return 0;
		}
	}
	return 1;
}

static s32 findGameTable(const u8 *rom, u32 romlen, u32 expected)
{
	if (gameTableValid(rom, romlen, expected)) {
		return (s32)expected;
	}
	for (u32 at = 0x20000; at < 0x200000; at += 4) {
		if (gameTableValid(rom, romlen, at)) {
			return (s32)at;
		}
	}
	return -1;
}

// The binary, inflated from its chunks.
static u8 *extractGame(const u8 *rom, u32 romlen, u32 table, u32 *outlen)
{
	u8 *out = NULL;
	u32 len = 0, cap = 0;

	for (u32 i = table; i + 4 <= romlen; i += 4) {
		const u32 ofs = table + be32(rom, i) + 2;
		u32 partlen, consumed;
		u8 *part;

		if (ofs + 5 > romlen || rom[ofs] != 0x11 || rom[ofs + 1] != 0x73) {
			break;
		}

		part = inflate1173(rom, umin(romlen, ofs + 0x1000), ofs, &partlen, &consumed);
		if (!part) {
			break;
		}

		if (len + partlen > cap) {
			cap = cap ? cap * 2 : 0x100000;
			while (cap < len + partlen) {
				cap *= 2;
			}
			out = realloc(out, cap);
		}

		memcpy(out + len, part, partlen);
		len += partlen;
		free(part);

		if (partlen != 0x1000) {
			break;
		}
	}

	*outlen = len;
	return out;
}

struct blob {
	const char *name;
	struct pairs sp;
	struct pairs mp;
};

static s32 followBlobs(const struct blob *blobs, s32 nblobs, u32 value, const char *only, u32 *addr)
{
	struct votes v;
	v.count = 0;

	for (s32 b = 0; b < nblobs; ++b) {
		u32 got;
		s32 n;
		if (only && strcmp(blobs[b].name, only)) {
			continue;
		}
		n = followTable(&blobs[b].sp, &blobs[b].mp, value, 1, &got);
		if (n) {
			vote(&v, got, n);
		}
	}

	return bestVote(&v, addr);
}

/**
 * Where the mod's own code says its segments are. A mod tool that moves a
 * segment rewrites the lui/addiu pair holding its ROM offset, so a mod whose
 * code is patched in place tells us exactly where everything went.
 */
static void locateByCode(const u8 *stock, u32 stocklen, const u8 *mod, u32 modlen, struct seg *segs, s32 nsegs,
		const u8 *stockgame, u32 stockgamelen, const u8 *modgame, u32 modgamelen)
{
	struct blob blobs[2];
	s32 nblobs = 0;

	if (is1173(stock, stocklen, LIB_OFS) && is1173(mod, modlen, LIB_OFS)) {
		u32 sl, ml, c;
		u8 *stocklib = inflate1173(stock, stocklen, LIB_OFS, &sl, &c);
		u8 *modlib = inflate1173(mod, modlen, LIB_OFS, &ml, &c);
		if (stocklib && modlib && sl == ml) {
			blobs[nblobs].name = "lib";
			addressPairs(stocklib, sl, &blobs[nblobs].sp);
			addressPairs(modlib, ml, &blobs[nblobs].mp);
			++nblobs;
		}
		free(stocklib);
		free(modlib);
	}

	if (stockgame && modgame && stockgamelen == modgamelen && stockgamelen) {
		blobs[nblobs].name = "game";
		addressPairs(stockgame, stockgamelen, &blobs[nblobs].sp);
		addressPairs(modgame, modgamelen, &blobs[nblobs].mp);
		++nblobs;
	}

	if (!nblobs) {
		return;
	}

	for (s32 i = 0; i < nsegs; ++i) {
		struct seg *seg = &segs[i];
		u32 got;
		const s32 n = followBlobs(blobs, nblobs, seg->ofs, NULL, &got);

		if (!n || got >= modlen) {
			continue;
		}

		if (got == seg->ofs && seg->modofs != (s32)got
				&& (got + 64 > modlen || memcmp(mod + got, stock + got, 64))) {
			// The code still says the stock offset, but the stock bytes are
			// not there: a reference the mod tool did not know to update.
			continue;
		}

		if (seg->modofs >= 0 && seg->modofs != (s32)got && (!seg->note || strcmp(seg->note, "ambiguous"))) {
			rep("note:    %s fingerprints at %08x but the mod's code reads it from %08x; using the code",
					seg->name, seg->modofs, got);
		}

		seg->modofs = (s32)got;
		seg->note = NULL;
		snprintf(seg->found, sizeof(seg->found), "code (%d reference%s)", n, n == 1 ? "" : "s");
	}

	// texinit.c measures the list from its start to its end, and the mod's
	// code has both: that is the list's size, whatever follows it.
	for (s32 i = 0; i < nsegs; ++i) {
		struct seg *seg = &segs[i];
		if (!strcmp(seg->name, "textureslist") && seg->modofs >= 0) {
			u32 end;
			if (followBlobs(blobs, nblobs, seg->ofs + seg->size, "game", &end)
					&& (s32)end > seg->modofs && end <= modlen) {
				seg->modsize = end - seg->modofs;
				seg->hasmodsize = 1;
			}
		}
	}

	for (s32 b = 0; b < nblobs; ++b) {
		freePairs(&blobs[b].sp);
		freePairs(&blobs[b].mp);
	}
}

/* -- self-describing segments ---------------------------------------------- */

#define ALBANK_MAGIC "\x42\x31\x00\x01"

struct albankwalk {
	const u8 *rom;
	u32 romlen;
	u32 ofs;
	u32 limit;
	u8 *seen;
	u32 ctlend;
	u32 tblend;
	s32 bad;
};

static void albankMark(struct albankwalk *w, u32 o, u32 n)
{
	if (o + n > w->limit || w->ofs + o + n > w->romlen) {
		w->bad = 1;
		return;
	}
	w->ctlend = umax(w->ctlend, o + n);
}

static u32 albankU16(struct albankwalk *w, u32 o)
{
	albankMark(w, o, 2);
	return w->bad ? 0 : be16(w->rom, w->ofs + o);
}

static u32 albankU32(struct albankwalk *w, u32 o)
{
	albankMark(w, o, 4);
	return w->bad ? 0 : be32(w->rom, w->ofs + o);
}

static s32 albankSeen(struct albankwalk *w, u32 o)
{
	if (o >= w->limit) {
		w->bad = 1;
		return 1;
	}
	if (seenTest(w->seen, o)) {
		return 1;
	}
	seenSet(w->seen, o);
	return 0;
}

/**
 * Walk an ALBankFile the way preprocessALBankFile() does and return the
 * bytes it occupies and the bytes of sample data it references in its .tbl.
 * Returns 0 when what is there does not read as one.
 */
static s32 albankExtents(const u8 *rom, u32 romlen, u32 ofs, u32 limit, u32 *ctlend, u32 *tblend)
{
	struct albankwalk w;
	u32 nbanks;

	memset(&w, 0, sizeof(w));
	w.rom = rom;
	w.romlen = romlen;
	w.ofs = ofs;
	w.limit = umin(limit, romlen > ofs ? romlen - ofs : 0);
	w.seen = calloc((w.limit + 7) / 8 + 1, 1);

	nbanks = albankU16(&w, 2);
	if (w.bad || !(nbanks > 0 && nbanks < 256)) {
		free(w.seen);
		return 0;
	}
	albankMark(&w, 0, 4 + 4 * nbanks);

	for (u32 i = 0; i < nbanks && !w.bad; ++i) {
		const u32 bank = albankU32(&w, 4 + 4 * i);
		u32 ninst;

		if (w.bad || albankSeen(&w, bank)) {
			continue;
		}

		ninst = albankU16(&w, bank);
		albankMark(&w, bank, 12 + 4 * ninst);

		for (u32 k = 0; k <= ninst && !w.bad; ++k) {
			// k == 0 is the percussion instrument at bank + 8, then the list
			const u32 inst = k == 0 ? albankU32(&w, bank + 8) : albankU32(&w, bank + 12 + 4 * (k - 1));
			u32 nsnd;

			if (w.bad || !inst || albankSeen(&w, inst)) {
				continue;
			}

			nsnd = albankU16(&w, inst + 14);
			albankMark(&w, inst, 16 + 4 * nsnd);

			for (u32 s = 0; s < nsnd && !w.bad; ++s) {
				const u32 sound = albankU32(&w, inst + 16 + 4 * s);
				u32 env, keymap, wave;

				if (w.bad || !sound || albankSeen(&w, sound)) {
					continue;
				}

				albankMark(&w, sound, 16);
				env = albankU32(&w, sound);
				keymap = albankU32(&w, sound + 4);
				wave = albankU32(&w, sound + 8);

				if (env) {
					albankMark(&w, env, 14);
				}
				if (keymap) {
					albankMark(&w, keymap, 6);
				}
				if (w.bad || !wave || albankSeen(&w, wave)) {
					continue;
				}

				albankMark(&w, wave, 20);
				if (w.bad) {
					break;
				}
				w.tblend = umax(w.tblend, albankU32(&w, wave) + albankU32(&w, wave + 4));

				if (rom[ofs + wave + 8] == 0) { // AL_ADPCM_WAVE
					const u32 loop = albankU32(&w, wave + 12);
					const u32 book = albankU32(&w, wave + 16);
					if (loop) {
						albankMark(&w, loop, 44);
					}
					if (book) {
						albankMark(&w, book, 8 + 16 * albankU32(&w, book) * albankU32(&w, book + 4));
					}
				} else {
					const u32 loop = albankU32(&w, wave + 12);
					if (loop) {
						albankMark(&w, loop, 12);
					}
				}
			}
		}
	}

	free(w.seen);

	if (w.bad) {
		return 0;
	}

	*ctlend = align16(w.ctlend);
	*tblend = align16(w.tblend);
	return 1;
}

/**
 * (base, len) of every wave an ALBankFile references, in .tbl terms.
 * Returns how many, 0 when it does not read as one.
 */
struct albankwave {
	u32 base;
	u32 len;
};

static u32 albankWaves(const u8 *rom, u32 romlen, u32 ofs, u32 limit, struct albankwave *out, u32 max)
{
	struct albankwalk w;
	u32 nbanks;
	u32 n = 0;

	memset(&w, 0, sizeof(w));
	w.rom = rom;
	w.romlen = romlen;
	w.ofs = ofs;
	w.limit = umin(limit, romlen > ofs ? romlen - ofs : 0);
	w.seen = calloc((w.limit + 7) / 8 + 1, 1);

	nbanks = albankU16(&w, 2);
	if (w.bad || !(nbanks > 0 && nbanks < 256)) {
		free(w.seen);
		return 0;
	}

	for (u32 i = 0; i < nbanks && !w.bad; ++i) {
		const u32 bank = albankU32(&w, 4 + 4 * i);
		u32 ninst;

		if (w.bad || albankSeen(&w, bank)) {
			continue;
		}

		ninst = albankU16(&w, bank);

		for (u32 k = 0; k <= ninst && !w.bad; ++k) {
			const u32 inst = k == 0 ? albankU32(&w, bank + 8) : albankU32(&w, bank + 12 + 4 * (k - 1));
			u32 nsnd;

			if (w.bad || !inst || albankSeen(&w, inst)) {
				continue;
			}

			nsnd = albankU16(&w, inst + 14);

			for (u32 s = 0; s < nsnd && !w.bad; ++s) {
				const u32 sound = albankU32(&w, inst + 16 + 4 * s);
				u32 wave;

				if (w.bad || !sound || albankSeen(&w, sound)) {
					continue;
				}

				wave = albankU32(&w, sound + 8);

				if (w.bad || !wave || albankSeen(&w, wave)) {
					continue;
				}

				if (n < max) {
					out[n].base = albankU32(&w, wave);
					out[n].len = albankU32(&w, wave + 4);
					if (!w.bad) {
						++n;
					}
				}
			}
		}
	}

	free(w.seen);
	return w.bad ? 0 : n;
}

#define MAX_WAVES 4096

/**
 * Where a mod's .tbl starts, from the stock samples it kept.
 *
 * Measuring the table back from the segment after it assumes it ends where
 * its last referenced sample does - and GE-X's does not: its bank file drops
 * waves and leaves their data in the table, 81KB of it at the end, so the
 * measured start landed 81KB late and every sample played as noise. A kept
 * sample's data is at the same offset from the true start in both ROMs, so
 * each one found in the patched ROM votes for a start, and a mod that kept
 * any of the stock sounds elects it by hundreds to one.
 *
 * Returns the start with *votes set, or -1.
 */
static s32 tblStartBySamples(const u8 *stock, u32 stocklen, const u8 *mod, u32 modlen,
		u32 stockctl, u32 stocktbl, u32 modctl, u32 lo, u32 hi, s32 *votes)
{
	struct albankwave *stockwaves = malloc(MAX_WAVES * sizeof(struct albankwave));
	struct albankwave *modwaves = malloc(MAX_WAVES * sizeof(struct albankwave));
	struct votes v;
	u32 nstock, nmod;
	u32 addr;
	s32 n, total = 0;

	*votes = 0;
	v.count = 0;

	if (!stockwaves || !modwaves) {
		free(stockwaves);
		free(modwaves);
		return -1;
	}

	nstock = albankWaves(stock, stocklen, stockctl, hi > stockctl ? hi - stockctl : 0, stockwaves, MAX_WAVES);
	nmod = albankWaves(mod, modlen, modctl, hi > modctl ? hi - modctl : 0, modwaves, MAX_WAVES);

	for (u32 i = 0; i < nstock && nmod; ++i) {
		const u32 base = stockwaves[i].base;
		const u32 len = stockwaves[i].len;
		s32 at;

		if (len < 64 || stocktbl + base + 48 > stocklen) {
			continue;
		}

		at = findBytes(mod, modlen, lo, hi, stock + stocktbl + base, 48);
		if (at < 0) {
			continue;
		}

		for (u32 j = 0; j < nmod; ++j) {
			if (modwaves[j].len == len && (u32)at >= modwaves[j].base) {
				const u32 start = (u32)at - modwaves[j].base;
				if (start >= lo && start < hi) {
					vote(&v, start, 1);
					++total;
				}
			}
		}
	}

	free(stockwaves);
	free(modwaves);

	n = bestVote(&v, &addr);

	// one sample matching by chance is one vote, and samples of one length
	// spread a hit over several starts; the table is the one far ahead
	{
		s32 second = 0;
		for (s32 i = 0; i < v.count; ++i) {
			if (v.addr[i] != addr && v.n[i] > second) {
				second = v.n[i];
			}
		}
		if (n < 4 || n < second * 3) {
			return -1;
		}
	}
	(void)total;

	*votes = n;
	return (s32)addr;
}

// sfxctl and seqctl both start with the ALBankFile revision word
static s32 findAlbankFile(const u8 *rom, u32 romlen, u32 near)
{
	const u32 span = 0x400000;
	const u32 ranges[2][2] = { { near > span ? near - span : 0, near + span }, { 0, romlen } };

	for (u32 r = 0; r < 2; ++r) {
		s32 at = findBytes(rom, romlen, ranges[r][0], ranges[r][1], (const u8 *)ALBANK_MAGIC, 4);
		while (at >= 0) {
			u32 c, t;
			if (albankExtents(rom, romlen, at, umin(0x800000, romlen - at), &c, &t)) {
				return at;
			}
			at = findBytes(rom, romlen, at + 1, ranges[r][1], (const u8 *)ALBANK_MAGIC, 4);
		}
	}

	return -1;
}

/**
 * The sequences segment is a count, then {romaddr, binlen, ziplen} entries
 * at offset 4, with the sequence data behind them. Returns the total size,
 * or 0 when it does not read as one.
 */
static u32 seqtableExtent(const u8 *rom, u32 romlen, u32 ofs)
{
	u32 count, end, prev;

	if (ofs + 8 > romlen) {
		return 0;
	}

	count = be16(rom, ofs);
	if (!(count > 0 && count < 0x400)) {
		return 0;
	}

	end = 4 + count * 8;
	prev = end;

	if (be32(rom, ofs + 4) != end) {
		return 0;
	}

	for (u32 i = 0; i < count; ++i) {
		u32 addr, binlen, ziplen;
		if (ofs + 12 + i * 8 > romlen) {
			return 0;
		}
		addr = be32(rom, ofs + 4 + i * 8);
		binlen = be16(rom, ofs + 8 + i * 8);
		ziplen = be16(rom, ofs + 10 + i * 8);
		if (addr < prev || !(ziplen > 0 && ziplen <= binlen) || addr + ziplen > 0x400000) {
			return 0;
		}
		prev = end = addr + ziplen;
	}

	return align16(end);
}

static s32 findSeqtable(const u8 *rom, u32 romlen, s32 lo, s32 hi)
{
	const s32 top = umin(romlen > 16 ? romlen - 16 : 0, hi < 0 ? 0 : hi);
	for (s32 at = lo < 0 ? 0 : lo; at < top; at += 4) {
		if (seqtableExtent(rom, romlen, at)) {
			return at;
		}
	}
	return -1;
}

/**
 * textureslist is an array of 8 byte entries whose second, third and fourth
 * bytes are an ascending offset into texturesdata, starting at zero, with
 * the runtime pointer word zero in the ROM.
 */
static s32 textureTableValid(const u8 *rom, u32 romlen, u32 at, u32 count)
{
	u32 prev = 0;

	if (at + count * 8 > romlen) {
		return 0;
	}

	if (be24(rom, at + 1) != 0) {
		return 0;
	}

	for (u32 i = 1; i < umin(count, 256); ++i) {
		const u32 ofs = be24(rom, at + i * 8 + 1);
		if (ofs < prev) {
			return 0;
		}
		if (be32(rom, at + i * 8 + 4) != 0) {
			return 0;
		}
		prev = ofs;
	}

	return prev > 0;
}

static s32 findTextureTable(const u8 *rom, u32 romlen, u32 near, u32 count)
{
	const u32 span = 0x400000;
	const u32 lo = near > span ? near - span : 0;
	const u32 hi = umin(romlen > count * 8 ? romlen - count * 8 : 0, near + span);

	for (u32 at = lo; at < hi; at += 4) {
		if (textureTableValid(rom, romlen, at, count)) {
			return (s32)at;
		}
	}

	return -1;
}

static void locateStructurally(const u8 *stock, u32 stocklen, const u8 *mod, u32 modlen, struct seg *segs, s32 nsegs)
{
	struct seg *s;

	for (u32 k = 0; k < 2; ++k) {
		s = segByName(segs, nsegs, k == 0 ? "sfxctl" : "seqctl");
		if (s && s->modofs < 0) {
			s->modofs = findAlbankFile(mod, modlen, s->ofs);
			if (s->modofs >= 0) {
				snprintf(s->found, sizeof(s->found), "ALBankFile header");
			}
		}
	}

	s = segByName(segs, nsegs, "sequences");
	if (s && s->modofs < 0) {
		// it follows seqctl and seqtbl, so start looking past the bank file
		const struct seg *seqctl = segByName(segs, nsegs, "seqctl");
		const s32 lo = seqctl && seqctl->modofs >= 0 ? seqctl->modofs : (s32)s->ofs - 0x400000;
		s->modofs = findSeqtable(mod, modlen, lo + 0x1000, lo + 0x800000);
		if (s->modofs >= 0) {
			snprintf(s->found, sizeof(s->found), "sequence table");
		}
	}
	if (s && s->modofs >= 0) {
		const u32 ext = seqtableExtent(mod, modlen, s->modofs);
		if (ext) {
			s->modsize = ext;
			s->hasmodsize = 1;
		}
	}

	// a .tbl holds nothing but samples. A fingerprint pins it; failing that,
	// the stock samples the mod kept say where it starts (see
	// tblStartBySamples); failing that, measure it back from the segment
	// after it using the byte count its .ctl accounts for
	{
		static const char *const trios[2][3] = { { "sfxctl", "sfxtbl", "seqctl" }, { "seqctl", "seqtbl", "sequences" } };
		for (u32 k = 0; k < 2; ++k) {
			struct seg *ctl = segByName(segs, nsegs, trios[k][0]);
			struct seg *tbl = segByName(segs, nsegs, trios[k][1]);
			struct seg *nxt = segByName(segs, nsegs, trios[k][2]);
			u32 ctlend, tblsize, start;
			s32 voted, votes;

			if (!(ctl && tbl && nxt) || ctl->modofs < 0 || nxt->modofs < 0 || nxt->modofs <= ctl->modofs) {
				continue;
			}
			if (tbl->modofs >= 0 && (!tbl->note || strcmp(tbl->note, "ambiguous"))) {
				ctl->modsize = tbl->modofs - ctl->modofs;
				ctl->hasmodsize = 1;
				tbl->modsize = nxt->modofs - tbl->modofs;
				tbl->hasmodsize = 1;
				continue;
			}
			if (!albankExtents(mod, modlen, ctl->modofs, nxt->modofs - ctl->modofs, &ctlend, &tblsize)) {
				continue;
			}
			voted = tblStartBySamples(stock, stocklen, mod, modlen, ctl->ofs, tbl->ofs, ctl->modofs,
					ctl->modofs + ctlend, nxt->modofs, &votes);
			if (voted >= 0) {
				start = (u32)voted;
				snprintf(tbl->found, sizeof(tbl->found), "%d kept samples", votes);
				tbl->modsize = nxt->modofs - start;
			} else {
				if (tblsize >= (u32)nxt->modofs) {
					continue;
				}
				start = nxt->modofs - tblsize;
				if (start <= (u32)ctl->modofs) {
					continue;
				}
				snprintf(tbl->found, sizeof(tbl->found), "%s wave table sizes", ctl->name);
				tbl->modsize = tblsize;
			}
			tbl->modofs = (s32)start;
			tbl->hasmodsize = 1;
			ctl->modsize = start - ctl->modofs;
			ctl->hasmodsize = 1;
		}
	}

	{
		struct seg *tl = segByName(segs, nsegs, "textureslist");
		struct seg *td = segByName(segs, nsegs, "texturesdata");
		struct seg *cp = segByName(segs, nsegs, "copyright");

		if (tl && tl->modofs < 0) {
			tl->modofs = findTextureTable(mod, modlen, tl->ofs, tl->size / 8);
			if (tl->modofs >= 0) {
				snprintf(tl->found, sizeof(tl->found), "texture table");
			}
		}
		if (tl && tl->modofs >= 0 && td && td->modofs < 0) {
			// the last entry's offset is the length of the data it indexes,
			// and the table sits directly behind that data
			const u32 count = tl->size / 8;
			const u32 at = tl->modofs + (count - 1) * 8 + 1;
			if (at + 3 <= modlen) {
				const u32 last = be24(mod, at);
				if (align16(last) <= (u32)tl->modofs) {
					td->modofs = tl->modofs - align16(last);
					td->modsize = align16(last);
					td->hasmodsize = 1;
					snprintf(td->found, sizeof(td->found), "texture table offsets");
				}
			}
		}
		if (tl && tl->modofs >= 0 && cp && cp->modofs < 0) {
			// the copyright segment is a 1173 blob just past the texture table
			const s32 at = findBytes(mod, modlen, tl->modofs + tl->size - 64, tl->modofs + tl->size + 0x1000, (const u8 *)"\x11\x73", 2);
			if (at >= 0) {
				cp->modofs = at;
				snprintf(cp->found, sizeof(cp->found), "1173 header after the texture table");
				tl->modsize = at - tl->modofs;
				tl->hasmodsize = 1;
			}
		}
	}
}

/**
 * Place what is left from a neighbour that was found. Two segments that
 * touch in the stock ROM still touch in the patched one.
 */
static void locateByAdjacency(struct seg *segs, s32 nsegs)
{
	for (s32 pass = 0; pass < nsegs; ++pass) {
		for (s32 n = 0; n < nsegs; ++n) {
			struct seg *seg = &segs[n];
			const struct seg *after = n + 1 < nsegs ? &segs[n + 1] : NULL;
			const struct seg *before = n ? &segs[n - 1] : NULL;

			if (seg->modofs >= 0) {
				continue;
			}

			if (after && after->modofs >= 0 && after->ofs == seg->ofs + seg->size) {
				seg->modofs = after->modofs - (s32)seg->size;
				snprintf(seg->found, sizeof(seg->found), "position against %s", after->name);
			} else if (before && before->modofs >= 0 && !before->hasmodsize && seg->ofs == before->ofs + before->size) {
				seg->modofs = before->modofs + (s32)before->size;
				snprintf(seg->found, sizeof(seg->found), "position against %s", before->name);
			}
		}
	}
}

/**
 * A resized segment has to come out whole rather than truncated at its stock
 * length. Where its own structure did not give a size, the gap to the next
 * segment is the only measure - but only where the two really are adjacent.
 */
static void sizeSegments(struct seg *segs, s32 nsegs)
{
	for (s32 n = 0; n < nsegs; ++n) {
		struct seg *seg = &segs[n];

		if (seg->hasmodsize && seg->modsize) {
			seg->resized = seg->modsize != seg->size;
			continue;
		}

		seg->modsize = seg->size;

		if (n + 1 < nsegs && !segs[n + 1].assumed && !seg->assumed) {
			const u32 gapstock = segs[n + 1].ofs - seg->ofs;
			s32 gapmod;
			if (gapstock != seg->size) {
				continue;
			}
			gapmod = segs[n + 1].modofs - seg->modofs;
			if (gapmod != (s32)gapstock && gapmod > 0) {
				seg->modsize = (u32)gapmod;
				seg->resized = 1;
			}
		}
	}
}

static s32 matchSegments(const u8 *stock, u32 stocklen, const u8 *mod, u32 modlen, struct seg *segs,
		const u8 *stockgame, u32 stockgamelen, const u8 *modgame, u32 modgamelen)
{
	const s32 nsegs = stockSegments(segs);
	s32 hint = 0;

	for (s32 i = 0; i < nsegs; ++i) {
		const char *note;
		const s32 at = locateSegment(stock, mod, modlen, &segs[i], hint, &note);
		segs[i].modofs = at;
		segs[i].note = note;
		if (at >= 0 && (!note || strcmp(note, "ambiguous"))) {
			hint = at - (s32)segs[i].ofs;
		}
	}

	locateByCode(stock, stocklen, mod, modlen, segs, nsegs, stockgame, stockgamelen, modgame, modgamelen);
	locateStructurally(stock, stocklen, mod, modlen, segs, nsegs);
	locateByAdjacency(segs, nsegs);

	// anything still missing moved with its neighbour, as far as we know
	hint = 0;
	for (s32 i = 0; i < nsegs; ++i) {
		if (segs[i].modofs < 0) {
			segs[i].modofs = (s32)segs[i].ofs + hint;
			segs[i].assumed = 1;
		} else {
			hint = segs[i].modofs - (s32)segs[i].ofs;
		}
	}

	sizeSegments(segs, nsegs);

	return nsegs;
}

/* -- output ---------------------------------------------------------------- */

static void makeDirs(const char *path)
{
	char tmp[FS_MAXPATH + 1];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);

	for (p = tmp + 1; *p; ++p) {
		if (*p == '/' || *p == '\\') {
			const char c = *p;
			*p = '\0';
			fsCreateDir(tmp);
			*p = c;
		}
	}
}

static u32 writeOut(const char *outdir, const char *relpath, const u8 *data, u32 len)
{
	char path[FS_MAXPATH + 1];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", outdir, relpath);
	makeDirs(path);

	f = fopen(path, "wb");
	if (!f) {
		sysLogPrintf(LOG_ERROR, "modimport: could not write %s", path);
		return 0;
	}
	if (len) {
		fwrite(data, 1, len, f);
	}
	fclose(f);

	return len;
}

static u8 *loadWhole(const char *path, u32 *len)
{
	FILE *f = fopen(path, "rb");
	long n;
	u8 *buf;

	if (!f) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	buf = malloc(n ? (u32)n : 1);
	if (!buf || fread(buf, 1, n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (u32)n;
	return buf;
}

/* -- the data segment ------------------------------------------------------ */

#if VERSION == VERSION_NTSC_FINAL
// Cut from tools/pd.ntsc-final.datasym and pd.ntsc-final.sym: each table's
// stock address and the size its symbol spacing gives it, and the range of
// each function whose immediate is a table's length.
static const struct { const char *name; u32 addr; u32 size; } dataSyms[] = {
	{ "g_Weapons",        0x8006ff18, 0x178 },
	{ "g_ModelStates",    0x8007b06c, 0xdc8 },
	{ "g_HeadsAndBodies", 0x8007cf04, 0xbe0 },
	{ "g_FileTable",      0x80082060, 0x1f80 },
	{ "g_MpArenas",       0x80084b98, 0x8c },
	{ "g_MpWeapons",      0x80087268, 0x188 },
	{ "g_MpWeaponSets",   0x800873f0, 0xd8 },
	{ "g_MpBeauHeads",    0x80087518, 0x14 },
	{ "g_MpHeads",        0x8008752c, 0x12c },
	{ "g_BotHeads",       0x80087658, 0xd4 },
	{ "g_MpBodies",       0x800877bc, 0x1e8 },
	{ "g_MpMaleHeads",    0x800879a4, 0xb0 },
	{ "g_MpFemaleHeads",  0x80087a54, 0x1c },
	// the solo guards' random heads (body.c), -1 terminated
	{ "g_MaleGuardHeads",  0x80062b68, 0xac },
	{ "g_MaleGuardTeamHeads", 0x80062c14, 0x44 },
	{ "g_FemaleGuardHeads", 0x80062c58, 0x14 },
	{ "g_FemaleGuardTeamHeads", 0x80062c6c, 0x14 },
	// the sky, fog and clouds of each stage (env.c)
	{ "g_FogEnvironments",   0x80081164, 0x268 },
	{ "g_NoFogEnvironments", 0x800813cc, 0xc84 },
	{ "g_Stages",         0x8007fcc0, 0xd60 },
	{ "g_CommandLengths", 0x80068c14, 0x3e4 },
	{ "g_SoloStages",     0x80071e6c, 0xfc },
	{ "g_StageTracks",    0x80084500, 0xd0 },
	{ "g_MpTracks",       0x80087a70, 0xfc },
};
static const struct { const char *name; u32 start; u32 end; } codeSyms[] = {
	{ "mp_get_num_stages",         0x7f1790fc, 0x7f179104 },
	{ "mp_get_num_unlocked_tracks", 0x7f18c200, 0x7f18c220 },
	{ "mp_get_num_mpweapons",      0x7f188bcc, 0x7f188bd4 },
	{ "mp_get_num_weaponset_slots", 0x7f189058, 0x7f189088 },
	{ "mp_get_num_heads",          0x7f18bb24, 0x7f18bb2c },
	{ "mp_get_num_bodies",         0x7f18bb88, 0x7f18bb90 },
	{ "player_choose_body_and_head", 0x7f0b872c, 0x7f0b8ba0 },
	{ "tex_load_from_gdl",         0x7f1756c0, 0x7f175ef4 },
	{ "room_populate_mtx",         0x7f166a6c, 0x7f166bc0 },
	{ "bg_render_scene",           0x7f15a6f4, 0x7f15b114 },
};
#define HAVE_DATASYMS 1
#else
#define HAVE_DATASYMS 0
#endif

#if HAVE_DATASYMS
static s32 dataSym(const char *name, u32 *addr, u32 *size)
{
	for (u32 i = 0; i < sizeof(dataSyms) / sizeof(dataSyms[0]); ++i) {
		if (!strcmp(dataSyms[i].name, name)) {
			*addr = dataSyms[i].addr;
			*size = dataSyms[i].size;
			return 1;
		}
	}
	return 0;
}

static s32 codeSym(const char *name, u32 *start, u32 *end)
{
	for (u32 i = 0; i < sizeof(codeSyms) / sizeof(codeSyms[0]); ++i) {
		if (!strcmp(codeSyms[i].name, name)) {
			*start = codeSyms[i].start - GAME_VRAM;
			*end = codeSyms[i].end - GAME_VRAM;
			return 1;
		}
	}
	return 0;
}

struct tablectx {
	const u8 *seg;
	u32 seglen;
	u32 base;
	s32 followed;
	struct pairs sp, mp;
	const u8 *stockcode;
	u32 stockcodelen;
	const u8 *modcode;
	u32 modcodelen;
};

// The entry at addr + n * elem, or NULL when it runs off the segment
static const u8 *tableEntry(const struct tablectx *t, u32 addr, u32 n, u32 elem)
{
	const u32 ofs = addr - t->base + n * elem;
	if (addr < t->base || ofs + elem > t->seglen) {
		return NULL;
	}
	return t->seg + ofs;
}

static u32 countMpWeapons(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 10);
		u32 weaponnum, pri, sec;
		s32 model;
		if (!e) {
			break;
		}
		weaponnum = e[0];
		pri = e[1];
		sec = e[3];
		model = (s16)be16(e, 6);
		if (weaponnum > 0x5d || (pri > 0x21 && pri < 0x80) || (sec > 0x21 && sec < 0x80) || !(model >= 0 && model < 0x1b9)) {
			break;
		}
		++n;
	}
	return n;
}

static u32 countMpWeaponSets(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 18);
		u32 k;
		if (!e) {
			break;
		}
		if ((be16(e, 0) >> 9) > 0x40) {
			break;
		}
		for (k = 2; k < 8; ++k) {
			if (e[k] > 0x5d) {
				break;
			}
		}
		if (k < 8) {
			break;
		}
		++n;
	}
	return n;
}

static u32 countMpArenas(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 6);
		s32 stagenum;
		u32 feature, name;
		if (!e) {
			break;
		}
		stagenum = (s16)be16(e, 0);
		feature = e[2];
		name = be16(e, 4);
		if (!(stagenum >= 1 && stagenum <= 0x5d) || feature > 0x7f || name == 0 || (name >> 9) > 0x44) {
			break;
		}
		++n;
	}
	return n;
}

static f32 beFloat(const u8 *p)
{
	union { u32 u; f32 f; } v;
	v.u = be32(p, 0);
	return v.f;
}

static u32 countHeadsAndBodies(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 0x14);
		f32 scale, animscale;
		if (!e || be16(e, 2) == 0) {
			break;
		}
		scale = beFloat(e + 4);
		animscale = beFloat(e + 8);
		if (!(scale > 0.01f && scale < 100.0f && animscale > 0.01f && animscale < 100.0f)) {
			return 0;
		}
		++n;
	}
	return n;
}

static u32 countMpHeads(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 4);
		s32 headnum;
		if (!e) {
			break;
		}
		headnum = (s16)be16(e, 0);
		if (!(headnum >= 0 && headnum < 151) || e[2] > 0x7f) {
			break;
		}
		++n;
	}
	return n;
}

static u32 countMpBodies(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 8);
		s32 bodynum, headnum;
		if (!e) {
			break;
		}
		bodynum = (s16)be16(e, 0);
		headnum = (s16)be16(e, 4);
		if (!(bodynum >= 0 && bodynum < 151) || !((headnum >= -1 && headnum < 151) || headnum == 1000)
				|| e[6] > 0x7f || (be16(e, 2) >> 9) > 0x44) {
			break;
		}
		++n;
	}
	return n;
}

static u32 countSoloStages(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 12);
		u32 stagenum;
		if (!e) {
			break;
		}
		stagenum = be32(e, 0);
		if (!(stagenum > 0 && stagenum < 0x60)) {
			break;
		}
		++n;
	}
	return n;
}

// Entries of g_StageTracks that read as one: a stage number and three track
// numbers (-1 for none), 8 bytes each, to the 0 stage that ends the table
static u32 countStageTracks(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 8);
		u32 stagenum;
		if (!e) {
			break;
		}
		stagenum = be16(e, 0);
		if (!(stagenum > 0 && stagenum < 0x60)) {
			break;
		}
		for (u32 k = 1; k < 4; ++k) {
			const s16 track = (s16)be16(e, k * 2);
			if (track < -1 || track >= 0x200) {
				return 0;
			}
		}
		++n;
	}
	return n;
}

// Entries of g_MpTracks that read as one: a sequence and duration packed in
// a word, a name text id and the mission index that unlocks it (-1 for none)
static u32 countMpTracks(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 6);
		u32 w, name;
		s16 unlock;
		if (!e) {
			break;
		}
		w = be16(e, 0);
		name = be16(e, 2);
		unlock = (s16)be16(e, 4);
		if ((w & 0x1ff) == 0 || name == 0 || unlock < -1 || unlock >= 0x60) {
			break;
		}
		++n;
	}
	return n;
}

static u32 countStages(const struct tablectx *t, u32 addr, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 0x38);
		s32 stagenum;
		if (!e) {
			break;
		}
		stagenum = (s16)be16(e, 0);
		if (!(stagenum > 0 && stagenum < 0x60)) {
			break;
		}
		++n;
	}
	return n;
}

// Entries of an environment table: a stage number first (s16 in the fog
// table, s32 in the no-fog one), a 0 stage ending it
static u32 countEnvs(const struct tablectx *t, u32 addr, u32 size, u32 fog, u32 limit)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, size);
		s32 stagenum;
		if (!e) {
			break;
		}
		stagenum = fog ? (s16)be16(e, 0) : (s32)be32(e, 0);
		if (stagenum == 0) {
			return n;
		}
		if (!(stagenum >= -1 && stagenum < 0x400)) {
			break;
		}
		++n;
	}
	return 0;
}

static u32 countIndexes(const struct tablectx *t, u32 addr, u32 limit, u32 maxvalue)
{
	u32 n = 0;
	while (n < limit) {
		const u8 *e = tableEntry(t, addr, n, 4);
		if (!e || be32(e, 0) >= maxvalue) {
			break;
		}
		++n;
	}
	return n;
}

// Where the table is now, and a note when the code moved it
static u32 locateTable(const struct tablectx *t, const char *name, char *note, u32 notelen)
{
	u32 addr, size, got;
	s32 votes;

	note[0] = '\0';

	if (!dataSym(name, &addr, &size)) {
		return 0;
	}
	if (!t->followed) {
		return addr;
	}
	votes = followTable(&t->sp, &t->mp, addr, size, &got);
	if (!votes) {
		return addr;
	}
	if (got != addr) {
		snprintf(note, notelen, " (moved from %08x; %d references in the code agree)", addr, votes);
	}
	return got;
}

static u32 stockCount(const char *name, u32 elem)
{
	u32 addr, size;
	return dataSym(name, &addr, &size) ? size / elem : 0;
}

// The immediate the mod's code loads in fn, or 0 when it cannot be read
static u32 codeCount(const struct tablectx *t, const char *fn, u32 stockvalue)
{
	u32 start, end;
	s32 v;
	if (!t->followed || !codeSym(fn, &start, &end)) {
		return 0;
	}
	v = followImmediate(t->stockcode, t->stockcodelen, t->modcode, t->modcodelen, start, end, stockvalue);
	return v > 0 ? (u32)v : 0;
}
#endif

/**
 * The mod's inflated data segment, its file names, and a modconfig block
 * saying where its tables are. Returns bytes written; *note gets why the
 * block was left out, or NULL.
 */
static u32 writeDataSegment(const u8 *stock, u32 stocklen, const u8 *mod, u32 modlen, u32 expectedDataOfs,
		const char *outdir, const struct romfiles *stockfiles, const struct romfiles *modfiles,
		const u8 *stockgame, u32 stockgamelen, const u8 *modgame, u32 modgamelen, char *note, u32 notelen)
{
	u32 written = 0;
	char *namesbuf;
	u32 nameslen = 0;
	char lines[2048];
	u32 lineslen = 0;
	char *block;
	u32 base;

	(void)stock; (void)stocklen; (void)mod; (void)modlen; (void)expectedDataOfs;
	note[0] = '\0';

	if (!modfiles->dataseg) {
		return 0;
	}

	written += writeOut(outdir, "segs/data", modfiles->dataseg, modfiles->dataseglen);

	// the names, one per id
	for (u32 i = 0; i < modfiles->numnames; ++i) {
		nameslen += strlen(modfiles->names[i]) + 1;
	}
	namesbuf = malloc(nameslen + 1);
	nameslen = 0;
	for (u32 i = 0; i < modfiles->numnames; ++i) {
		const u32 l = strlen(modfiles->names[i]);
		memcpy(namesbuf + nameslen, modfiles->names[i], l);
		nameslen += l;
		if (i + 1 < modfiles->numnames) {
			namesbuf[nameslen++] = '\n';
		}
	}
	written += writeOut(outdir, "segs/data.names", (const u8 *)namesbuf, nameslen);
	free(namesbuf);

#if !HAVE_DATASYMS
	snprintf(note, notelen, "no data symbols for this ROM version, so the tables in it cannot be located");
	return written;
#else
	{
		u32 ftaddr, ftsize;
		dataSym("g_FileTable", &ftaddr, &ftsize);
		base = ftaddr - stockfiles->filesofs;
	}

	if (modfiles->dataseglen != stockfiles->dataseglen || modfiles->filesofs != stockfiles->filesofs) {
		snprintf(note, notelen, "the data segment was rebuilt (%u bytes against %u), so its tables are not at the\n"
				"           stock addresses and the port cannot read them yet", modfiles->dataseglen, stockfiles->dataseglen);
		return written;
	}

	struct tablectx t;
	memset(&t, 0, sizeof(t));
	t.seg = modfiles->dataseg;
	t.seglen = modfiles->dataseglen;
	t.base = base;
	t.followed = stockgame && modgame && stockgamelen == modgamelen && stockgamelen > 0;
	if (t.followed) {
		addressPairs(stockgame, stockgamelen, &t.sp);
		addressPairs(modgame, modgamelen, &t.mp);
		t.stockcode = stockgame;
		t.stockcodelen = stockgamelen;
		t.modcode = modgame;
		t.modcodelen = modgamelen;
	}

#define LINE(...) do { lineslen += snprintf(lines + lineslen, sizeof(lines) - lineslen, __VA_ARGS__); } while (0)

	char tnote[96];
	u32 addr;

	addr = locateTable(&t, "g_Weapons", tnote, sizeof(tnote));
	if (addr) {
		LINE("  weapons 0x%08x %u\n", addr, stockCount("g_Weapons", 4));
		rep("  weapons at %08x%s", addr, tnote);
	}

	addr = locateTable(&t, "g_ModelStates", tnote, sizeof(tnote));
	if (addr) {
		LINE("  modelstates 0x%08x %u\n", addr, stockCount("g_ModelStates", 8));
		rep("  model states at %08x%s", addr, tnote);
	}

	// the lengths are numbers in the code, and the code is the authority;
	// counting entries that read as one is the fallback
	u32 nummp = 0;
	addr = locateTable(&t, "g_MpWeapons", tnote, sizeof(tnote));
	if (addr) {
		nummp = codeCount(&t, "mp_get_num_mpweapons", stockCount("g_MpWeapons", 10));
		if (!nummp) {
			nummp = countMpWeapons(&t, addr, 64);
		} else if (countMpWeapons(&t, addr, nummp) < nummp) {
			rep("  the code says %u Combat Simulator weapons but fewer read as one", nummp);
			nummp = countMpWeapons(&t, addr, nummp);
		}
		if (nummp) {
			LINE("  mpweapons 0x%08x %u\n", addr, nummp);
			rep("  %u Combat Simulator weapons at %08x%s", nummp, addr, tnote);
		} else {
			rep("  what is at %08x does not read as the Combat Simulator weapon list; left out", addr);
		}
	}

	addr = locateTable(&t, "g_MpWeaponSets", tnote, sizeof(tnote));
	if (addr) {
		u32 numsets = codeCount(&t, "mp_get_num_weaponset_slots", stockCount("g_MpWeaponSets", 18));
		if (!numsets) {
			numsets = countMpWeaponSets(&t, addr, 32);
		} else if (countMpWeaponSets(&t, addr, numsets) < numsets) {
			rep("  the code says %u weapon sets but fewer read as one", numsets);
			numsets = countMpWeaponSets(&t, addr, numsets);
		}
		if (numsets) {
			LINE("  mpweaponsets 0x%08x %u\n", addr, numsets);
			rep("  %u weapon sets at %08x%s", numsets, addr, tnote);
		} else {
			rep("  what is at %08x does not read as the weapon set table; left out", addr);
		}
	}

	addr = locateTable(&t, "g_MpArenas", tnote, sizeof(tnote));
	if (addr) {
		u32 numarenas = codeCount(&t, "mp_get_num_stages", 17);
		if (!numarenas) {
			numarenas = countMpArenas(&t, addr, 64);
		} else if (countMpArenas(&t, addr, numarenas) < numarenas) {
			rep("  the code says %u arenas but only %u read as one at %08x; the arena list is left out",
					numarenas, countMpArenas(&t, addr, numarenas), addr);
			numarenas = 0;
			addr = 0;
		}
		if (addr && numarenas) {
			LINE("  mparenas 0x%08x %u\n", addr, numarenas);
			rep("  %u arenas at %08x%s", numarenas, addr, tnote);
		} else if (addr) {
			rep("  what is at %08x does not read as the arena list; left out", addr);
		}
	}

	addr = locateTable(&t, "g_HeadsAndBodies", tnote, sizeof(tnote));
	if (addr) {
		const u32 n = countHeadsAndBodies(&t, addr, stockCount("g_HeadsAndBodies", 0x14));
		if (n) {
			LINE("  headsandbodies 0x%08x %u\n", addr, n);
			rep("  %u heads and bodies at %08x%s", n, addr, tnote);
		} else {
			rep("  what is at %08x does not read as the heads and bodies table; left out", addr);
		}
	}

	{
		// terminated: the list ends at its first -1 rather than at a count
		// in the code, and however many read before it are the list
		static const struct { const char *key; const char *sym; const char *fn; u32 elem; u32 limit; u32 terminated; } lists[] = {
			{ "mpheads",       "g_MpHeads",       "mp_get_num_heads",  4, 151, 0 },
			{ "mpbodies",      "g_MpBodies",      "mp_get_num_bodies", 8, 151, 0 },
			{ "mpbeauheads",   "g_MpBeauHeads",   NULL,                4, 151, 0 },
			{ "botheads",      "g_BotHeads",      NULL,                4, 75,  0 },
			{ "mpmaleheads",   "g_MpMaleHeads",   NULL,                4, 151, 0 },
			{ "mpfemaleheads", "g_MpFemaleHeads", NULL,                4, 151, 0 },
			// the heads a solo stage hands its guards at random. GE-X keeps
			// 25 of its own here; the stock list names heads whose texture
			// slots GE-X reused, which is what a guard with a scrambled
			// face was
			{ "maleguardheads",       "g_MaleGuardHeads",       NULL, 4, 151, 1 },
			{ "maleguardteamheads",   "g_MaleGuardTeamHeads",   NULL, 4, 151, 1 },
			{ "femaleguardheads",     "g_FemaleGuardHeads",     NULL, 4, 151, 1 },
			{ "femaleguardteamheads", "g_FemaleGuardTeamHeads", NULL, 4, 151, 1 },
		};
		for (u32 i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
			u32 n, ok;
			addr = locateTable(&t, lists[i].sym, tnote, sizeof(tnote));
			if (!addr) {
				continue;
			}
			n = stockCount(lists[i].sym, lists[i].elem);
			if (lists[i].fn) {
				const u32 c = codeCount(&t, lists[i].fn, n);
				if (c) {
					n = c;
				}
			}
			if (lists[i].elem == 8) {
				ok = countMpBodies(&t, addr, n);
			} else if (!strcmp(lists[i].key, "mpheads") || !strcmp(lists[i].key, "mpbeauheads")) {
				ok = countMpHeads(&t, addr, n);
			} else {
				ok = countIndexes(&t, addr, n, lists[i].limit);
			}
			if (lists[i].terminated && ok > 0) {
				LINE("  %s 0x%08x %u\n", lists[i].key, addr, ok);
				rep("  %u %s at %08x%s", ok, lists[i].key, addr, tnote);
			} else if (!lists[i].terminated && ok == n) {
				LINE("  %s 0x%08x %u\n", lists[i].key, addr, n);
				rep("  %u %s at %08x%s", n, lists[i].key, addr, tnote);
			} else if (lists[i].terminated) {
				rep("  what is at %08x does not read as the %s list; left out", addr, lists[i].key);
			} else {
				rep("  the code says %u %s but only %u read as one at %08x; left out", n, lists[i].key, ok, addr);
			}
		}
	}

	// the stage table: which background, pads and setup each stage loads
	addr = locateTable(&t, "g_Stages", tnote, sizeof(tnote));
	if (addr) {
		const u32 n = countStages(&t, addr, stockCount("g_Stages", 0x38));
		if (n) {
			LINE("  stages 0x%08x %u\n", addr, n);
			rep("  %u stages at %08x%s", n, addr, tnote);
		} else {
			rep("  what is at %08x does not read as the stage table; left out", addr);
		}
	}

	// the sky, fog and clouds of each stage: two tables env.c walks to a 0
	// stage, the fog one first. GE-X grows the fog table over the no-fog
	// one's old place and moves that one; without them its Runway, in
	// Extraction's slot, gets Extraction's black indoor sky
	{
		static const struct { const char *key; const char *sym; u32 size; u32 fog; } envs[] = {
			{ "fogenvs",   "g_FogEnvironments",   44, 1 },
			{ "nofogenvs", "g_NoFogEnvironments", 56, 0 },
		};
		for (u32 i = 0; i < sizeof(envs) / sizeof(envs[0]); ++i) {
			addr = locateTable(&t, envs[i].sym, tnote, sizeof(tnote));
			if (addr) {
				const u32 n = countEnvs(&t, addr, envs[i].size, envs[i].fog, 64);
				if (n) {
					LINE("  %s 0x%08x %u\n", envs[i].key, addr, n);
					rep("  %u %s at %08x%s", n, envs[i].key, addr, tnote);
				} else {
					rep("  what is at %08x does not read as the %s table; left out", addr, envs[i].key);
				}
			}
		}
	}

	// the mission list: which stage each solo mission slot loads and its
	// title ids. GE-X moves six missions to stage ids the menu never used
	addr = locateTable(&t, "g_SoloStages", tnote, sizeof(tnote));
	if (addr) {
		const u32 n = countSoloStages(&t, addr, stockCount("g_SoloStages", 12));
		if (n) {
			LINE("  solostages 0x%08x %u\n", addr, n);
			rep("  %u solo missions at %08x%s", n, addr, tnote);
		}
	}

	// each stage's music: the main theme, the background track and the X
	// theme. GE-X fills the table with its own stage ids; without it every
	// GE-X mission played the Combat Simulator's random pick
	addr = locateTable(&t, "g_StageTracks", tnote, sizeof(tnote));
	if (addr) {
		const u32 n = countStageTracks(&t, addr, stockCount("g_StageTracks", 8));
		if (n) {
			LINE("  stagetracks 0x%08x %u\n", addr, n);
			rep("  %u stages' music at %08x%s", n, addr, tnote);
		} else {
			rep("  what is at %08x does not read as the stage music table; left out", addr);
		}
	}

	// the Combat Simulator's music: which sequence each track plays, for how
	// long, its name and what unlocks it; mp_get_num_unlocked_tracks loads
	// its length. GE-X rewrites the whole list and, for two more tracks,
	// starts it 12 bytes earlier
	addr = locateTable(&t, "g_MpTracks", tnote, sizeof(tnote));
	if (addr) {
		u32 n = codeCount(&t, "mp_get_num_unlocked_tracks", stockCount("g_MpTracks", 6));
		if (!n) {
			n = countMpTracks(&t, addr, 64);
		} else if (countMpTracks(&t, addr, n) < n) {
			rep("  the code says %u Combat Simulator tracks but only %u read as one at %08x; the list is left out",
					n, countMpTracks(&t, addr, n), addr);
			n = 0;
		}
		if (n) {
			LINE("  mptracks 0x%08x %u\n", addr, n);
			rep("  %u Combat Simulator tracks at %08x%s", n, addr, tnote);
		} else {
			rep("  what is at %08x does not read as the Combat Simulator track list; left out", addr);
		}
	}

	// the AI command length table: a mod's own commands sit in slots this
	// game has no handler for, and their lengths let the port step over them
	addr = locateTable(&t, "g_CommandLengths", tnote, sizeof(tnote));
	if (addr) {
		const u32 n = stockCount("g_CommandLengths", 2);
		LINE("  commandlengths 0x%08x %u\n", addr, n);
		rep("  %u AI command lengths at %08x%s", n, addr, tnote);
	}

	// the solo player's body and head, per outfit, are constants in the
	// code that chooses them; a mod with its own hero changed the numbers.
	// Every `li` the function loads that the mod changed goes out as
	// stock=mod, and the port looks each site's constant up.
	//
	// The same for texLoadFromGdl(), which picks the animated textures (the
	// rivers, the ocean, the power juice) by number: GE-X moved two of them
	// (0x6cb -> 0x1c7, 0x90f -> 0xc90), and the port compares against the
	// stock numbers unless told.
	//
	// And roomPopulateMtx(), which pins a room to the camera (the moon, the
	// Attack Ship's backdrop) by stage and room number: the stages are
	// `lh` loads of g_Stages[index].id, followed as stage ids - the stock
	// id at the stock index, the mod's table's id at the mod's - and the
	// rooms are `li`. GE-X points all of them at its own stages.
	//
	// bgRenderScene() tests the same stages and rooms again to draw the
	// pinned room first, and the star field by stage id literal; rows that
	// share a key share one list, so the stages found in both functions
	// come out once.
	static const struct { const char *fn; const char *key; const char *what; u32 stages; } consts[] = {
		{ "player_choose_body_and_head", "playerconst", "body/head constants changed in the mod's outfit code", 0 },
		{ "tex_load_from_gdl",           "texconst",    "animated texture numbers changed in the mod's texture code", 0 },
		{ "room_populate_mtx",           "roomnum",     "pinned room numbers changed in the mod's room code", 0 },
		{ "bg_render_scene",             "bgstage",     "backdrop and star field stage ids changed in the mod's scene code", 0 },
		{ "room_populate_mtx",           "roomstage",   "pinned rooms' stages changed in the mod's room code", 1 },
		{ "bg_render_scene",             "roomstage",   "pinned rooms' stages changed in the mod's scene code", 1 },
	};
	const u32 modstages = t.followed ? locateTable(&t, "g_Stages", tnote, sizeof(tnote)) : 0;
	u32 seen[64], n = 0;
	const char *seenkey = NULL;
	for (u32 c = 0; t.followed && c < sizeof(consts) / sizeof(consts[0]); ++c) {
		u32 start, end;
		if (!seenkey || strcmp(seenkey, consts[c].key)) {
			seenkey = consts[c].key;
			n = 0;
		}
		if (codeSym(consts[c].fn, &start, &end)) {
			const u32 nbefore = n;
			char list[1024];
			u32 listlen = 0;
			for (u32 ofs = start; ofs + 4 <= end && ofs + 4 <= t.stockcodelen && ofs + 4 <= t.modcodelen && n < 64; ofs += 4) {
				const u32 x = be32(t.stockcode, ofs);
				const u32 y = be32(t.modcode, ofs);
				u32 k, v;
				if ((y >> 16) != (x >> 16) || (y & 0xffff) == (x & 0xffff)) {
					continue;
				}
				if (consts[c].stages) {
					// lh rt,OFF(rs): the index into g_Stages, as its id
					const u8 *e;
					if ((x >> 26) != 0x21 || (x & 0xffff) % 0x38 || (y & 0xffff) % 0x38 || !modstages) {
						continue;
					}
					k = (x & 0xffff) / 0x38;
					e = tableEntry(&t, modstages, (y & 0xffff) / 0x38, 0x38);
					if (!e) {
						continue;
					}
					v = be16(e, 0);
				} else {
					if (!(((x >> 26) == 0x08 || (x >> 26) == 0x09) && ((x >> 21) & 0x1f) == 0)) {
						continue;
					}
					k = x & 0xffff;
					v = y & 0xffff;
				}
				{
					u32 j;
					for (j = 0; j < n; ++j) {
						if (seen[j] == k) {
							break;
						}
					}
					if (j == n) {
						seen[n++] = k;
						LINE("  %s 0x%x %u\n", consts[c].key, k, v);
						listlen += snprintf(list + listlen, sizeof(list) - listlen, "%s0x%x->%u", listlen ? ", " : "", k, v);
					}
				}
			}
			if (n > nbefore) {
				rep("  %u %s: %s", n - nbefore, consts[c].what, list);
			}
		}
	}

#undef LINE

	if (t.followed) {
		freePairs(&t.sp);
		freePairs(&t.mp);
	}

	{
		static const char head[] =
			"# The mod's ROM data segment, and where its tables are in it. The port rebuilds\n"
			"# the weapon definitions and the model tables from this at load; weapon blocks\n"
			"# after it edit what it put in place. Written by the game's mod importer.\n"
			"datasegment {\n"
			"  file \"segs/data\"\n"
			"  names \"segs/data.names\"\n";
		char path[FS_MAXPATH + 1];
		u8 *existing = NULL;
		u32 existinglen = 0;
		u32 blocklen;

		snprintf(path, sizeof(path), "%s/modconfig.txt", outdir);
		existing = loadWhole(path, &existinglen);

		if (existing) {
			// replace an earlier block of ours rather than stacking them
			char *text = malloc(existinglen + 1);
			char *at;
			memcpy(text, existing, existinglen);
			text[existinglen] = '\0';
			at = strstr(text, "datasegment {");
			if (at) {
				char *start = at;
				char *end = strchr(at, '}');
				// the comment lines directly above it are ours too
				while (start > text) {
					char *prevline = start - 1;
					while (prevline > text && prevline[-1] != '\n') {
						--prevline;
					}
					if (*prevline != '#') {
						break;
					}
					start = prevline;
				}
				if (end) {
					++end;
					while (*end == '\n') {
						++end;
					}
					memmove(start, end, strlen(end) + 1);
				}
			}
			free(existing);
			existing = (u8 *)text;
			existinglen = strlen(text);
		}

		blocklen = sizeof(head) - 1 + 32 + lineslen + 4 + existinglen + 2;
		block = malloc(blocklen);
		snprintf(block, blocklen, "%s  base 0x%08x\n%s}\n\n%s", head, base, lines, existing ? (const char *)existing : "");
		written += writeOut(outdir, "modconfig.txt", (const u8 *)block, strlen(block));
		free(block);
		free(existing);
	}

	return written;
#endif
}

/* -- the import ------------------------------------------------------------ */

static const struct { const char *romid; const char *title; const char *code; } romIds[] = {
	{ "ntsc-final", "Perfect Dark", "NPDE" },
	{ "pal-final",  "Perfect Dark", "NPDP" },
	{ "jpn-final",  "PERFECT DARK", "NPDJ" },
};

static const char *identifyRom(const u8 *rom, u32 len)
{
	if (len < 0x40) {
		return NULL;
	}
	for (u32 i = 0; i < 3; ++i) {
		if (!memcmp(rom + 0x20, romIds[i].title, strlen(romIds[i].title)) && !memcmp(rom + 0x3b, romIds[i].code, 4)) {
			return romIds[i].romid;
		}
	}
	return NULL;
}

static void writeReport(const char *outdir)
{
	if (report) {
		writeOut(outdir, "IMPORT.txt", (const u8 *)report, reportLen);
	}
}

static void resetState(void)
{
	free(report);
	report = NULL;
	reportLen = reportCap = 0;
	stockNames = NULL;
	numStockNames = 0;
	namesMissingNoted = 0;
}

s32 modImportPatch(const char *patchPath, const char *outDir, const char *basePatchPath)
{
	u8 *stock = NULL, *patch = NULL, *mod = NULL;
	u32 stocklen = 0, patchlen = 0, modlen = 0;
	u8 *stockgame = NULL, *modgame = NULL;
	u32 stockgamelen = 0, modgamelen = 0;
	struct romfiles stockfiles, modfiles;
	struct seg segs[MAX_SEGS];
	s32 nsegs;
	s32 kind;
	char err[256];
	const char *romid, *modid;
	const char *patchname = strrchr(patchPath, '/');
	const char *basename = basePatchPath ? strrchr(basePatchPath, '/') : NULL;
	u8 *base = NULL;
	u32 baselen = 0;
	s32 result = -1;
	u32 written = 0;
	u32 numchanged = 0, numnew = 0, numsegs = 0, numtextures = 0;
	u32 numincompatible = 0, numunlocated = 0;
	u32 expectedDataOfs = 0x39850;

	patchname = patchname ? patchname + 1 : patchPath;
	basename = basePatchPath ? (basename ? basename + 1 : basePatchPath) : NULL;
	resetState();
	memset(&stockfiles, 0, sizeof(stockfiles));
	memset(&modfiles, 0, sizeof(modfiles));

	if (basename) {
		sysLogPrintf(LOG_NOTE, "modimport: importing %s on top of %s into %s", patchname, basename, outDir);
	} else {
		sysLogPrintf(LOG_NOTE, "modimport: importing %s into %s", patchname, outDir);
	}

	rep("%s", MODIMPORT_VERSION_LINE);

	// -- the ROMs
	stock = fsFileLoad(romdataGetRomName(), &stocklen);
	if (!stock) {
		rep("error: could not open the stock ROM %s to apply %s to.", romdataGetRomName(), patchname);
		goto done;
	}
	if (stocklen != ROM_SIZE) {
		rep("error: %s is %u bytes; a Perfect Dark ROM is %u.", romdataGetRomName(), stocklen, ROM_SIZE);
		goto done;
	}
	romid = identifyRom(stock, stocklen);
	if (!romid || strcmp(romid, VERSION_ROMID)) {
		rep("error: %s is not the %s ROM this game is built for.", romdataGetRomName(), VERSION_ROMID);
		goto done;
	}

	if (basePatchPath) {
		// a patch against another mod: build that mod's ROM first
		u8 *basepatch = loadWhole(basePatchPath, &patchlen);
		if (!basepatch) {
			rep("error: could not read %s.", basePatchPath);
			goto done;
		}
		kind = rompatchApply(stock, stocklen, basepatch, patchlen, &base, &baselen, err, sizeof(err));
		free(basepatch);
		if (kind < 0) {
			rep("error: could not apply the base patch %s: %s", basename, err);
			goto done;
		}
	}

	patch = loadWhole(patchPath, &patchlen);
	if (!patch) {
		rep("error: could not read %s.", patchPath);
		goto done;
	}

	kind = rompatchApply(base ? base : stock, base ? baselen : stocklen, patch, patchlen, &mod, &modlen, err, sizeof(err));
	if (kind < 0) {
		rep("error: could not apply %s%s%s: %s", patchname, basename ? " on top of " : "", basename ? basename : "", err);
		rep("       (is this patch built against a different base ROM?)");
		if (!basename && rompatchIdentify(patch, patchlen) != ROMPATCH_IPS) {
			// a patch for another mod's ROM applies on top of that mod's
			// patch; the caller can try the ones beside it
			result = MODIMPORT_NEEDS_BASE;
		}
		goto done;
	}
	free(patch);
	patch = NULL;

	rep("stock ROM:   %s (%s, crc32 %08x)", romdataGetRomName(), romid, (u32)crc32(0L, stock, stocklen));
	if (basename) {
		rep("base patch:  %s (applied first; this mod was made on top of it)", basename);
	}
	rep("patch:       %s (%s)", patchname, rompatchKindName(kind));
	rep("patched ROM: %u bytes, crc32 %08x", modlen, (u32)crc32(0L, mod, modlen));

	if (kind == ROMPATCH_IPS) {
		rep("warning: IPS offsets are 24 bit, so nothing past 16MB of the ROM can be patched.");
		rep("         Parts of this mod may be missing.");
	}

	if (modlen != stocklen) {
		rep("warning: the patched ROM is %u bytes, not %u. Offsets past the end will be skipped.", modlen, stocklen);
	}

	modid = identifyRom(mod, modlen);
	if (!modid) {
		rep("note:    the patched ROM has a modified header; the port would reject it directly.");
	} else if (strcmp(modid, romid)) {
		rep("error: the patched ROM identifies as %s but the stock ROM is %s.", modid, romid);
		goto done;
	}
	rep("");

	// -- asset files
	if (!readFiles(stock, stocklen, expectedDataOfs, "stock", &stockfiles)) {
		goto done;
	}
	if (!readFiles(mod, modlen, expectedDataOfs, "patched", &modfiles)) {
		goto done;
	}

	if (modfiles.dataofs != stockfiles.dataofs || modfiles.filesofs != stockfiles.filesofs) {
		rep("note:    this mod moved the data segment to %06x (port expects %06x) and its",
				modfiles.dataofs, stockfiles.dataofs);
		rep("         file table to %05x (port expects %05x). Both are hardcoded in",
				modfiles.filesofs, stockfiles.filesofs);
		rep("         romdata.c, so the port could not read this ROM directly in any case.");
		rep("");
	}

	rep("files: %u in the stock ROM, %u in the patched one", stockfiles.numents, modfiles.numents);

	{
		struct { const char *name; const char *reason; } *incompatible = malloc(modfiles.numents * sizeof(*incompatible));
		u32 stripped = 0;
		u32 missing = 0;

		for (u32 i = 0; i < modfiles.numents; ++i) {
			const struct fileent *e = &modfiles.ents[i];
			const struct fileent *se = findEnt(&stockfiles, e->name);
			const u8 *content = mod + e->ofs;
			const char *reason;
			char rel[MAX_NAME + 32];

			if (e->ofs + e->len > modlen) {
				continue;
			}
			if (se && se->ofs + se->len <= stocklen && contentEqual(stock + se->ofs, se->len, content, e->len)) {
				continue;
			}
			if (!e->len) {
				// Mods empty the files they do not need to make room in the
				// ROM. The port ignores an empty override, so leave it out.
				++stripped;
				continue;
			}

			reason = checkFile(e->name, content, e->len);
			if (reason) {
				// the port would not reject this, it would crash on it
				incompatible[numincompatible].name = e->name;
				incompatible[numincompatible].reason = dupstr(reason);
				++numincompatible;
				snprintf(rel, sizeof(rel), "files.incompatible/%s", e->name);
				writeOut(outDir, rel, content, e->len);
				continue;
			}

			if (se) {
				++numchanged;
			} else {
				++numnew;
			}
			snprintf(rel, sizeof(rel), "files/%s", e->name);
			written += writeOut(outDir, rel, content, e->len);
			rep("  %s  files/%s (%u bytes)", se ? "changed" : "new    ", e->name, e->len);
		}

		if (stripped) {
			rep("  %u stock file(s) are emptied in the mod (space reclaimed); the port keeps its own copies", stripped);
		}

		for (u32 i = 0; i < stockfiles.numents; ++i) {
			if (!findEnt(&modfiles, stockfiles.ents[i].name)) {
				++missing;
			}
		}
		if (missing) {
			rep("  %u stock file(s) are absent from the mod; the port keeps its own copies", missing);
		}

		if (numincompatible) {
			rep("");
			rep("  %u changed file(s) are not in the format the port reads:", numincompatible);
			// grouped by reason, most common first
			u8 *done = calloc(numincompatible, 1);
			for (;;) {
				s32 best = -1;
				u32 bestn = 0;
				for (u32 i = 0; i < numincompatible; ++i) {
					u32 n = 0;
					if (done[i]) {
						continue;
					}
					for (u32 j = 0; j < numincompatible; ++j) {
						if (!done[j] && !strcmp(incompatible[i].reason, incompatible[j].reason)) {
							++n;
						}
					}
					if (n > bestn) {
						bestn = n;
						best = (s32)i;
					}
				}
				if (best < 0) {
					break;
				}
				{
					char shown[512];
					u32 shownlen = 0;
					u32 listed = 0;
					for (u32 j = 0; j < numincompatible; ++j) {
						if (done[j] || strcmp(incompatible[best].reason, incompatible[j].reason)) {
							continue;
						}
						done[j] = 1;
						if (listed < 4) {
							shownlen += snprintf(shown + shownlen, sizeof(shown) - shownlen, "%s%s", listed ? ", " : "", incompatible[j].name);
						} else if (listed == 4) {
							shownlen += snprintf(shown + shownlen, sizeof(shown) - shownlen, ", ...");
						}
						++listed;
					}
					rep("    %u file(s): %s", bestn, incompatible[best].reason);
					rep("      %s", shown);
				}
			}
			free(done);
			rep("  This mod built them for its own copy of the game code, which reads them");
			rep("  differently. The port would crash on them rather than ignore them, so they");
			rep("  are in files.incompatible/, which the port does not read.");
		}
		rep("");

		for (u32 i = 0; i < numincompatible; ++i) {
			free((void *)incompatible[i].reason);
		}
		free(incompatible);
	}

	// -- the game binary, for following the mod's code
	{
		const s32 stocktab = findGameTable(stock, stocklen, GAME_TABLE_OFS);
		const s32 modtab = findGameTable(mod, modlen, GAME_TABLE_OFS);
		if (stocktab >= 0 && modtab >= 0) {
			stockgame = extractGame(stock, stocklen, stocktab, &stockgamelen);
			modgame = extractGame(mod, modlen, modtab, &modgamelen);
		}
	}

	// -- segments
	nsegs = matchSegments(stock, stocklen, mod, modlen, segs, stockgame, stockgamelen, modgame, modgamelen);

	{
		s32 changed[MAX_SEGS];
		s32 numchangedsegs = 0;
		s32 shifts[MAX_SEGS];
		s32 numshifts = 0;
		struct seg *tl = segByName(segs, nsegs, "textureslist");
		struct seg *td = segByName(segs, nsegs, "texturesdata");
		s32 texsplit = 0;
		char shiftstr[512];
		u32 shiftlen = 0;

		for (s32 i = 0; i < nsegs; ++i) {
			struct seg *seg = &segs[i];
			const s32 shift = seg->modofs - (s32)seg->ofs;
			const u32 have = (u32)seg->modofs < modlen ? umin(seg->modsize, modlen - seg->modofs) : 0;
			s32 k;

			changed[i] = seg->size != have || memcmp(stock + seg->ofs, mod + seg->modofs, have) != 0;
			if (changed[i]) {
				++numchangedsegs;
			}

			for (k = 0; k < numshifts; ++k) {
				if (shifts[k] == shift) {
					break;
				}
			}
			if (k == numshifts) {
				shifts[numshifts++] = shift;
			}
		}

		// sorted, the way the tool prints them
		for (s32 a = 0; a < numshifts; ++a) {
			for (s32 b = a + 1; b < numshifts; ++b) {
				if (shifts[b] < shifts[a]) {
					const s32 t = shifts[a];
					shifts[a] = shifts[b];
					shifts[b] = t;
				}
			}
		}
		if (numshifts == 1 && shifts[0] == 0) {
			snprintf(shiftstr, sizeof(shiftstr), "nothing (same layout)");
		} else {
			for (s32 k = 0; k < numshifts && shiftlen < sizeof(shiftstr) - 16; ++k) {
				shiftlen += snprintf(shiftstr + shiftlen, sizeof(shiftstr) - shiftlen, "%s%+d", k ? ", " : "", shifts[k]);
			}
		}
		rep("segments: %d, shifted by %s", nsegs, shiftstr);

		for (s32 i = 0; i < nsegs; ++i) {
			const struct seg *seg = &segs[i];
			if (seg->found[0]) {
				rep("  found %s by its %s at %08x", seg->name, seg->found, seg->modofs);
			} else if (seg->note && !strcmp(seg->note, "ambiguous")) {
				rep("  warning: %s matched in more than one place; took the first at %08x", seg->name, seg->modofs);
			}
		}

		// per texture replacements, but only when the mod kept the texture
		// table identical: once the offsets move, an overridden list and a
		// stock data segment disagree for every texture the mod did not
		// replace, so both segments ship whole instead
		if (tl && td && (changed[tl - segs] || changed[td - segs]) && !tl->assumed && !td->assumed
				&& tl->modsize == tl->size && (u32)tl->modofs + tl->modsize <= modlen
				&& !memcmp(stock + tl->ofs, mod + tl->modofs, tl->size)) {
			const u32 count = tl->size / 8;
			const u8 *stockdata = stock + td->ofs;
			const u8 *moddata = mod + td->modofs;
			const u32 stockdatalen = td->size;
			const u32 moddatalen = (u32)td->modofs < modlen ? umin(td->modsize, modlen - td->modofs) : 0;

			texsplit = 1;

			for (u32 n = 0; n + 1 < count; ++n) {
				const u32 start = be24(stock, tl->ofs + n * 8 + 1);
				const u32 end = be24(stock, tl->ofs + (n + 1) * 8 + 1);
				if (start == end || end > stockdatalen || end > moddatalen || end < start) {
					continue;
				}
				if (memcmp(stockdata + start, moddata + start, end - start)) {
					if (end - start > MAX_TEXTURE_SIZE) {
						texsplit = 0; // too big for the port's load buffer; ship the segment
						break;
					}
				}
			}

			if (texsplit) {
				for (u32 n = 0; n + 1 < count; ++n) {
					const u32 start = be24(stock, tl->ofs + n * 8 + 1);
					const u32 end = be24(stock, tl->ofs + (n + 1) * 8 + 1);
					char rel[64];
					if (start == end || end > stockdatalen || end > moddatalen || end < start) {
						continue;
					}
					if (memcmp(stockdata + start, moddata + start, end - start)) {
						++numtextures;
						snprintf(rel, sizeof(rel), "textures/%04x.bin", n);
						written += writeOut(outDir, rel, moddata + start, end - start);
					}
				}
			}
		}

		for (s32 i = 0; i < nsegs; ++i) {
			struct seg *seg = &segs[i];
			const u8 *data;
			u32 have;
			s32 guessed;
			char rel[64];

			if (!changed[i]) {
				continue;
			}
			if (texsplit && (!strcmp(seg->name, "texturesdata") || !strcmp(seg->name, "textureslist"))) {
				continue;
			}

			data = mod + seg->modofs;
			have = (u32)seg->modofs < modlen ? umin(seg->modsize, modlen - seg->modofs) : 0;

			// placed against a neighbour, somewhere other than where it was,
			// and not holding the stock bytes: nothing actually found it
			guessed = seg->assumed || (!strncmp(seg->found, "position against", 16)
					&& seg->modofs != (s32)seg->ofs
					&& (have < 64 || memcmp(data, stock + seg->ofs, 64) != 0));

			snprintf(rel, sizeof(rel), "%s/%s", guessed ? "segs.unlocated" : "segs", seg->name);

			if (guessed) {
				++numunlocated;
				writeOut(outDir, rel, data, have);
				continue;
			}

			++numsegs;
			written += writeOut(outDir, rel, data, have);
			if (seg->resized) {
				rep("  changed  segs/%s (%u bytes) (resized %+d)", seg->name, have, (s32)seg->modsize - (s32)seg->size);
			} else {
				rep("  changed  segs/%s (%u bytes)", seg->name, have);
			}
		}

		if (texsplit) {
			rep("  changed  %u texture(s) -> textures/*.bin (the texture table is unchanged,", numtextures);
			rep("           so they can go in one at a time instead of as whole segments)");
		}

		rep("");

		{
			char note[256];
			const u32 datawritten = writeDataSegment(stock, stocklen, mod, modlen, expectedDataOfs, outDir,
					&stockfiles, &modfiles, stockgame, stockgamelen, modgame, modgamelen, note, sizeof(note));
			written += datawritten;
			if (datawritten) {
				rep("data segment: segs/data (%u bytes), the weapon definitions and model tables", datawritten);
				if (note[0]) {
					rep("  note:    %s", note);
				} else {
					rep("  and a datasegment block in modconfig.txt saying where its tables are");
				}
			}
		}

		if (numunlocated) {
			char names[512];
			u32 len = 0;
			for (s32 i = 0; i < nsegs; ++i) {
				const struct seg *seg = &segs[i];
				if (changed[i] && (seg->assumed || (!strncmp(seg->found, "position against", 16) && seg->modofs != (s32)seg->ofs))) {
					// the same test as above, minus the byte compare, which
					// only matters for the count already made
					len += snprintf(names + len, sizeof(names) - len, "%s%s", len ? ", " : "", seg->name);
					if (len >= sizeof(names) - 1) {
						break;
					}
				}
			}
			rep("");
			rep("  %u segment(s) changed but could not be found in the patched ROM:", numunlocated);
			rep("    %s", names);
			rep("  Nothing left in them to fingerprint and no structure to go by, so their");
			rep("  offsets are only inferred from the segments around them. They are in");
			rep("  segs.unlocated/, which the port ignores. Move one into segs/ to try it.");
		}
		rep("");

		// -- what could not come across
		{
			u32 firstseg = ROM_SIZE;
			u32 common;
			for (s32 i = 0; i < nsegs; ++i) {
				firstseg = umin(firstseg, segs[i].ofs);
			}
			firstseg = umin(firstseg, umin(stocklen, modlen));
			for (common = 0; common < firstseg && stock[common] == mod[common]; ++common) {
			}
			if (common < firstseg) {
				rep("the first %u bytes of the ROM - boot code, game code and the compressed data", firstseg);
				rep("segment - differ from 0x%06x onwards. The port runs decomp code rather than the", common);
				rep("ROM's, so whatever this mod changed there is NOT imported and will have no effect.");
				rep("");
			}
		}
	}

	rep("imported %u changed file(s), %u new file(s), %u segment(s), %u texture(s) - %.1f MB",
			numchanged, numnew, numsegs, numtextures, written / 1048576.0);
	if (numincompatible || numunlocated) {
		rep("set aside %u file(s) and %u segment(s) the port cannot use", numincompatible, numunlocated);
	}

	if (!(numchanged || numnew || numsegs || numtextures)) {
		rep("");
		rep("Nothing in this mod carries over. What it changes is either ROM code, which the");
		rep("port does not run, or assets in a format only its own code reads.");
		result = 0;
	} else {
		result = 1;
	}

	sysLogPrintf(LOG_NOTE, "modimport: %s: %u changed, %u new, %u segment(s), %u texture(s); %u file(s) and %u segment(s) set aside",
			patchname, numchanged, numnew, numsegs, numtextures, numincompatible, numunlocated);

done:
	if (result < 0 && report) {
		// the last line is the reason; the log should carry it too
		const char *last = report + reportLen - 1;
		while (last > report && last[-1] != '\n') {
			--last;
		}
		sysLogPrintf(LOG_ERROR, "modimport: %s: %s", patchname, report);
		(void)last;
	}

	writeReport(outDir);

	freeRomFiles(&modfiles);
	freeRomFiles(&stockfiles);
	free(stockgame);
	free(modgame);
	free(mod);
	free(base);
	free(patch);
	free(stock);
	resetState();

	return result;
}
