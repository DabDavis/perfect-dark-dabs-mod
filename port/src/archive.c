/**
 * Archive extraction for texture packs and mods.
 *
 * Three formats, for three reasons. Zip is here because zlib is already linked
 * and the container around it is a few structures. 7z is here because that is
 * how the packs in the wild are actually distributed, and it needs the LZMA
 * decoder under port/src/external/lzma - the same public domain SDK Project64
 * carries, cut down to what reading an archive needs. RAR is here because two
 * of the console mods in circulation come that way, and its format is
 * proprietary: the decoder is RARLAB's own unrar source under
 * port/src/external/unrar, which its licence allows in any software that
 * reads RAR archives (see license.txt there).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "fs.h"
#include "system.h"
#include "archive.h"

#include "external/lzma/7z.h"
#include "external/lzma/7zAlloc.h"
#include "external/lzma/7zCrc.h"
#include "external/lzma/7zFile.h"
#include "external/lzma/Lzma2Dec.h"
#include "external/lzma/LzmaDec.h"

// RARLAB's unrar, as its library build: its dll.hpp wants the Windows types
// on Windows and defines its own under _UNIX.
#ifdef PLATFORM_WIN32
#include <windows.h>
#else
#ifndef _UNIX
#define _UNIX
#endif
#endif
#include "external/unrar/dll.hpp"

// Enough for any path a pack has business containing, and a bound on what a
// malformed archive can ask to be written.
#define ARCHIVE_MAXNAME 512

// Central directory signatures, little-endian on disk.
#define ZIP_EOCD_SIG   0x06054b50
#define ZIP_CDIR_SIG   0x02014b50
#define ZIP_LOCAL_SIG  0x04034b50

// The end of central directory record sits at the end of the file, behind a
// comment of up to 64k that has to be searched backwards through.
#define ZIP_EOCD_MINLEN 22
#define ZIP_EOCD_MAXBACK (ZIP_EOCD_MINLEN + 0xffff)

static u32 archiveReadU32(const u8 *p) { return p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u32 archiveReadU16(const u8 *p) { return p[0] | (p[1] << 8); }

/**
 * Whether [ofs, ofs + len) lies inside a file of size bytes.
 *
 * Every offset and length below comes out of the archive, so a check written
 * the obvious way round - ofs + len > size - wraps for a large enough offset
 * and lets the read through. Subtracting instead cannot.
 */
static s32 archiveFits(u32 ofs, u32 len, u32 size)
{
	return ofs <= size && len <= size - ofs;
}

static const char *archiveExt(const char *path)
{
	const char *dot = strrchr(path, '.');
	return dot ? dot : "";
}

s32 archiveIsSupported(const char *path)
{
	const char *ext = archiveExt(path);

	return !strcasecmp(ext, ".zip") || !strcasecmp(ext, ".7z") || !strcasecmp(ext, ".pk3") || !strcasecmp(ext, ".rar");
}

/**
 * Rejects a name that would write outside destDir.
 *
 * An archive is data from somewhere else, and "../../.bashrc" is a valid name
 * to put in one. Anything absolute, anything with a .. component and anything
 * over the length bound is skipped instead of sanitised, because a pack has no
 * legitimate reason to contain one and guessing at an intent is worse.
 */
static s32 archiveNameIsSafe(const char *name)
{
	const char *p = name;

	if (!name[0] || strlen(name) >= ARCHIVE_MAXNAME) {
		return 0;
	}

	if (name[0] == '/' || name[0] == '\\' || (name[1] == ':' && name[2])) {
		return 0;
	}

	while (*p) {
		if (p[0] == '.' && p[1] == '.' && (p[2] == '\0' || p[2] == '/' || p[2] == '\\')) {
			return 0;
		}

		while (*p && *p != '/' && *p != '\\') {
			p++;
		}

		while (*p == '/' || *p == '\\') {
			p++;
		}
	}

	return 1;
}

/**
 * Creates every directory along path, which is a real filesystem path with a
 * filename on the end that is not itself created.
 */
static void archiveMakeDirs(char *path)
{
	char *p;

	for (p = path + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			const char sep = *p;
			*p = '\0';
			fsCreateDir(path);
			*p = sep;
		}
	}
}

static s32 archiveWriteFile(const char *destDir, const char *name, const u8 *data, u32 size)
{
	char path[FS_MAXPATH + 1];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", destDir, name);
	archiveMakeDirs(path);

	f = fopen(path, "wb");

	if (!f) {
		sysLogPrintf(LOG_ERROR, "archive: could not write %s", path);
		return 0;
	}

	if (size) {
		fwrite(data, 1, size, f);
	}

	fclose(f);

	return 1;
}

/**
 * Inflates one zip entry. Zip stores a bare deflate stream with no zlib header,
 * which is what the negative window size asks for.
 */
static u8 *archiveInflate(const u8 *src, u32 srcSize, u32 dstSize)
{
	z_stream zs;
	u8 *dst = malloc(dstSize ? dstSize : 1);

	if (!dst) {
		return NULL;
	}

	memset(&zs, 0, sizeof(zs));

	if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
		free(dst);
		return NULL;
	}

	zs.next_in = (Bytef *)src;
	zs.avail_in = srcSize;
	zs.next_out = dst;
	zs.avail_out = dstSize;

	if (inflate(&zs, Z_FINISH) != Z_STREAM_END || zs.total_out != dstSize) {
		inflateEnd(&zs);
		free(dst);
		return NULL;
	}

	inflateEnd(&zs);

	return dst;
}

static s32 archiveExtractZip(const char *path, const char *destDir)
{
	u8 *file;
	long fileSize;
	FILE *f;
	u32 eocd = 0;
	u32 numEntries;
	u32 cdirOfs;
	u32 pos;
	s32 written = 0;
	u32 i;

	f = fopen(path, "rb");

	if (!f) {
		sysLogPrintf(LOG_ERROR, "archive: could not open %s", path);
		return -1;
	}

	if (fseek(f, 0, SEEK_END) != 0 || (fileSize = ftell(f)) < ZIP_EOCD_MINLEN) {
		fclose(f);
		sysLogPrintf(LOG_ERROR, "archive: %s is too small to be a zip", path);
		return -1;
	}

	rewind(f);
	file = malloc((size_t)fileSize);

	if (!file || fread(file, 1, (size_t)fileSize, f) != (size_t)fileSize) {
		fclose(f);
		free(file);
		sysLogPrintf(LOG_ERROR, "archive: could not read %s", path);
		return -1;
	}

	fclose(f);

	{
		const u32 back = (u32)fileSize < ZIP_EOCD_MAXBACK ? (u32)fileSize : ZIP_EOCD_MAXBACK;

		for (i = ZIP_EOCD_MINLEN; i <= back; i++) {
			const u32 at = (u32)fileSize - i;

			if (archiveReadU32(file + at) == ZIP_EOCD_SIG) {
				eocd = at;
				break;
			}
		}
	}

	if (!eocd) {
		free(file);
		sysLogPrintf(LOG_ERROR, "archive: %s has no zip directory", path);
		return -1;
	}

	numEntries = archiveReadU16(file + eocd + 10);
	cdirOfs = archiveReadU32(file + eocd + 16);
	pos = cdirOfs;

	for (i = 0; i < numEntries; i++) {
		char name[ARCHIVE_MAXNAME];
		u32 method, csize, usize, nameLen, extraLen, commentLen, localOfs, dataOfs;
		u8 *data;

		if (!archiveFits(pos, 46, (u32)fileSize) || archiveReadU32(file + pos) != ZIP_CDIR_SIG) {
			break;
		}

		method = archiveReadU16(file + pos + 10);
		csize = archiveReadU32(file + pos + 20);
		usize = archiveReadU32(file + pos + 24);
		nameLen = archiveReadU16(file + pos + 28);
		extraLen = archiveReadU16(file + pos + 30);
		commentLen = archiveReadU16(file + pos + 32);
		localOfs = archiveReadU32(file + pos + 42);

		// A zero length name would be read backwards a moment later, and is
		// not a name a pack has any reason to contain.
		if (nameLen == 0 || nameLen >= sizeof(name)
				|| !archiveFits(pos + 46, nameLen, (u32)fileSize)) {
			break;
		}

		memcpy(name, file + pos + 46, nameLen);
		name[nameLen] = '\0';
		pos += 46 + nameLen + extraLen + commentLen;

		// A trailing separator is how zip stores a directory entry; the
		// directories get made by the files inside them anyway.
		if (name[nameLen - 1] == '/' || name[nameLen - 1] == '\\') {
			continue;
		}

		if (!archiveNameIsSafe(name)) {
			sysLogPrintf(LOG_WARNING, "archive: skipping %s in %s", name, path);
			continue;
		}

		if (!archiveFits(localOfs, 30, (u32)fileSize)
				|| archiveReadU32(file + localOfs) != ZIP_LOCAL_SIG) {
			continue;
		}

		// The local header repeats the name and carries its own extra field,
		// which is not the same length as the one in the directory.
		dataOfs = localOfs + 30 + archiveReadU16(file + localOfs + 26) + archiveReadU16(file + localOfs + 28);

		if (!archiveFits(dataOfs, csize, (u32)fileSize)) {
			continue;
		}

		if (method == 0) {
			written += archiveWriteFile(destDir, name, file + dataOfs, usize < csize ? usize : csize);
		} else if (method == 8) {
			data = archiveInflate(file + dataOfs, csize, usize);

			if (data) {
				written += archiveWriteFile(destDir, name, data, usize);
				free(data);
			} else {
				sysLogPrintf(LOG_WARNING, "archive: could not inflate %s in %s", name, path);
			}
		} else {
			sysLogPrintf(LOG_WARNING, "archive: %s in %s uses compression method %u", name, path, method);
		}
	}

	free(file);

	return written;
}

static s32 archiveExtract7z(const char *path, const char *destDir)
{
	CFileInStream stream;
	CLookToRead look;
	CSzArEx db;
	ISzAlloc allocImp;
	ISzAlloc allocTempImp;
	UInt32 blockIndex = 0xffffffff;
	Byte *outBuffer = NULL;
	size_t outBufferSize = 0;
	s32 written = 0;
	UInt32 i;

	allocImp.Alloc = SzAlloc;
	allocImp.Free = SzFree;
	allocTempImp.Alloc = SzAllocTemp;
	allocTempImp.Free = SzFreeTemp;

	if (InFile_Open(&stream.file, path)) {
		sysLogPrintf(LOG_ERROR, "archive: could not open %s", path);
		return -1;
	}

	FileInStream_CreateVTable(&stream);
	LookToRead_CreateVTable(&look, False);
	look.realStream = &stream.s;
	LookToRead_Init(&look);

	CrcGenerateTable();
	SzArEx_Init(&db);

	if (SzArEx_Open(&db, &look.s, &allocImp, &allocTempImp) != SZ_OK) {
		SzArEx_Free(&db, &allocImp);
		File_Close(&stream.file);
		sysLogPrintf(LOG_ERROR, "archive: could not read %s as a 7z", path);
		return -1;
	}

	for (i = 0; i < db.db.NumFiles; i++) {
		const CSzFileItem *item = db.db.Files + i;
		UInt16 utf16[ARCHIVE_MAXNAME];
		char name[ARCHIVE_MAXNAME];
		size_t offset = 0;
		size_t outSize = 0;
		size_t len;
		size_t j;

		if (item->IsDir) {
			continue;
		}

		len = SzArEx_GetFileNameUtf16(&db, i, NULL);

		if (len == 0 || len > ARCHIVE_MAXNAME) {
			continue;
		}

		SzArEx_GetFileNameUtf16(&db, i, utf16);

		// Names are UTF-16. Anything outside ASCII is replaced rather than
		// encoded: a texture pack names its files after hex digits, and a
		// name this cannot represent is a name the loader would not match.
		for (j = 0; j + 1 < len; j++) {
			name[j] = utf16[j] < 0x80 ? (char)utf16[j] : '_';
		}

		name[len - 1] = '\0';

		if (!archiveNameIsSafe(name)) {
			sysLogPrintf(LOG_WARNING, "archive: skipping %s in %s", name, path);
			continue;
		}

		// Files sharing a solid block come out together and the decoder holds
		// on to it, so walking the archive in order decompresses each block
		// once. Out of order, this would be the whole archive every time.
		if (SzArEx_Extract(&db, &look.s, i, &blockIndex, &outBuffer, &outBufferSize,
				&offset, &outSize, &allocImp, &allocTempImp) != SZ_OK) {
			sysLogPrintf(LOG_WARNING, "archive: could not extract %s from %s", name, path);
			continue;
		}

		written += archiveWriteFile(destDir, name, outBuffer + offset, (u32)outSize);
	}

	IAlloc_Free(&allocImp, outBuffer);
	SzArEx_Free(&db, &allocImp);
	File_Close(&stream.file);

	return written;
}

/**
 * unrar does the walking and the writing itself: RARProcessFile() extracts
 * the current entry under destDir with its own name, creating the folders
 * on the way. What is checked here is the name, the same as for the others.
 */
static s32 archiveExtractRar(const char *path, const char *destDir)
{
	struct RAROpenArchiveDataEx open;
	struct RARHeaderDataEx header;
	char arcName[FS_MAXPATH + 1];
	char dest[FS_MAXPATH + 1];
	HANDLE h;
	s32 written = 0;
	int ret;

	snprintf(arcName, sizeof(arcName), "%s", path);
	snprintf(dest, sizeof(dest), "%s", destDir);

	memset(&open, 0, sizeof(open));
	open.ArcName = arcName;
	open.OpenMode = RAR_OM_EXTRACT;

	h = RAROpenArchiveEx(&open);

	if (!h || open.OpenResult != ERAR_SUCCESS) {
		sysLogPrintf(LOG_ERROR, "archive: could not open %s as a RAR archive (unrar error %u)", path, open.OpenResult);
		if (h) {
			RARCloseArchive(h);
		}
		return -1;
	}

	memset(&header, 0, sizeof(header));

	while ((ret = RARReadHeaderEx(h, &header)) == ERAR_SUCCESS) {
		const char *name = header.FileName;
		int op = RAR_EXTRACT;

		if (header.Flags & RHDF_DIRECTORY) {
			op = RAR_SKIP; // made by the files inside it
		} else if (header.Flags & RHDF_ENCRYPTED) {
			sysLogPrintf(LOG_WARNING, "archive: %s in %s is encrypted; skipped", name, path);
			op = RAR_SKIP;
		} else if (!archiveNameIsSafe(name)) {
			sysLogPrintf(LOG_WARNING, "archive: skipping %s in %s", name, path);
			op = RAR_SKIP;
		}

		ret = RARProcessFile(h, op, op == RAR_EXTRACT ? dest : NULL, NULL);

		if (ret != ERAR_SUCCESS) {
			sysLogPrintf(LOG_WARNING, "archive: could not extract %s from %s (unrar error %d)", name, path, ret);
			continue;
		}

		if (op == RAR_EXTRACT) {
			written++;
		}
	}

	if (ret != ERAR_END_ARCHIVE) {
		sysLogPrintf(LOG_WARNING, "archive: %s ended early (unrar error %d)", path, ret);
	}

	RARCloseArchive(h);

	return written;
}

/* -------------------------------------------------------------------------
 * Looking into an archive, and taking part of one
 * ------------------------------------------------------------------------- */

// The LZMA SDK's method ids for the two coders a solid block is streamed with.
#define ARCHIVE_7Z_LZMA2 0x21
#define ARCHIVE_7Z_LZMA  0x30101

// How much decoded data is handed to the files at a time.
#define ARCHIVE_7Z_CHUNK (1 << 20)

/** needle in name, without case, with '\\' read as '/'. */
static s32 archiveNameHas(const char *name, const char *needle)
{
	const size_t nlen = strlen(needle);

	for (const char *p = name; *p; p++) {
		size_t i;

		for (i = 0; i < nlen && p[i]; i++) {
			char a = p[i] == '\\' ? '/' : p[i];
			char b = needle[i] == '\\' ? '/' : needle[i];

			if (a >= 'A' && a <= 'Z') {
				a += 'a' - 'A';
			}

			if (b >= 'A' && b <= 'Z') {
				b += 'a' - 'A';
			}

			if (a != b) {
				break;
			}
		}

		if (i == nlen) {
			return 1;
		}
	}

	return 0;
}

/** Entry i's name as ASCII, the way archiveExtract7z() spells it. 0 if it has none. */
static s32 archive7zName(const CSzArEx *db, UInt32 i, char *name)
{
	UInt16 utf16[ARCHIVE_MAXNAME];
	const size_t len = SzArEx_GetFileNameUtf16(db, i, NULL);

	if (len == 0 || len > ARCHIVE_MAXNAME) {
		return 0;
	}

	SzArEx_GetFileNameUtf16(db, i, utf16);

	for (size_t j = 0; j + 1 < len; j++) {
		name[j] = utf16[j] < 0x80 ? (char)utf16[j] : '_';
	}

	name[len - 1] = '\0';

	return 1;
}

struct archive7z {
	CFileInStream stream;
	CLookToRead look;
	CSzArEx db;
	ISzAlloc alloc;
	ISzAlloc allocTemp;
};

static s32 archive7zOpen(struct archive7z *a, const char *path)
{
	a->alloc.Alloc = SzAlloc;
	a->alloc.Free = SzFree;
	a->allocTemp.Alloc = SzAllocTemp;
	a->allocTemp.Free = SzFreeTemp;

	if (InFile_Open(&a->stream.file, path)) {
		return 0;
	}

	FileInStream_CreateVTable(&a->stream);
	LookToRead_CreateVTable(&a->look, False);
	a->look.realStream = &a->stream.s;
	LookToRead_Init(&a->look);

	CrcGenerateTable();
	SzArEx_Init(&a->db);

	if (SzArEx_Open(&a->db, &a->look.s, &a->alloc, &a->allocTemp) != SZ_OK) {
		SzArEx_Free(&a->db, &a->alloc);
		File_Close(&a->stream.file);
		return 0;
	}

	return 1;
}

static void archive7zClose(struct archive7z *a)
{
	SzArEx_Free(&a->db, &a->alloc);
	File_Close(&a->stream.file);
}

/** The zip's central directory, read from the end of the file rather than the whole file. */
static s32 archiveZipFindEntry(const char *path, const char *needle)
{
	FILE *f = fopen(path, "rb");
	u8 *tail = NULL;
	u8 *cdir = NULL;
	long size;
	u32 back;
	u32 eocd = 0;
	u32 numEntries, cdirOfs, cdirLen;
	s32 found = 0;

	if (!f) {
		return 0;
	}

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < ZIP_EOCD_MINLEN) {
		fclose(f);
		return 0;
	}

	back = (u32)size < ZIP_EOCD_MAXBACK ? (u32)size : ZIP_EOCD_MAXBACK;
	tail = malloc(back);

	if (!tail || fseek(f, size - back, SEEK_SET) != 0 || fread(tail, 1, back, f) != back) {
		goto done;
	}

	for (u32 i = ZIP_EOCD_MINLEN; i <= back; i++) {
		if (archiveReadU32(tail + back - i) == ZIP_EOCD_SIG) {
			eocd = back - i;
			break;
		}
	}

	if (archiveReadU32(tail + eocd) != ZIP_EOCD_SIG) {
		goto done;
	}

	numEntries = archiveReadU16(tail + eocd + 10);
	cdirLen = archiveReadU32(tail + eocd + 12);
	cdirOfs = archiveReadU32(tail + eocd + 16);

	if (!archiveFits(cdirOfs, cdirLen, (u32)size) || !(cdir = malloc(cdirLen ? cdirLen : 1))
			|| fseek(f, cdirOfs, SEEK_SET) != 0 || fread(cdir, 1, cdirLen, f) != cdirLen) {
		goto done;
	}

	for (u32 i = 0, pos = 0; i < numEntries && !found; i++) {
		char name[ARCHIVE_MAXNAME];
		u32 nameLen;

		if (!archiveFits(pos, 46, cdirLen) || archiveReadU32(cdir + pos) != ZIP_CDIR_SIG) {
			break;
		}

		nameLen = archiveReadU16(cdir + pos + 28);

		if (nameLen == 0 || nameLen >= sizeof(name) || !archiveFits(pos + 46, nameLen, cdirLen)) {
			break;
		}

		memcpy(name, cdir + pos + 46, nameLen);
		name[nameLen] = '\0';
		found = archiveNameHas(name, needle);
		pos += 46 + nameLen + archiveReadU16(cdir + pos + 30) + archiveReadU16(cdir + pos + 32);
	}

done:
	free(cdir);
	free(tail);
	fclose(f);

	return found;
}

s32 archiveFindEntry(const char *path, const char *needle)
{
	const char *ext = archiveExt(path);
	struct archive7z a;
	s32 found = 0;

	if (!strcasecmp(ext, ".zip") || !strcasecmp(ext, ".pk3")) {
		return archiveZipFindEntry(path, needle);
	}

	if (strcasecmp(ext, ".7z") || !archive7zOpen(&a, path)) {
		return 0;
	}

	for (UInt32 i = 0; i < a.db.db.NumFiles && !found; i++) {
		char name[ARCHIVE_MAXNAME];

		if (archive7zName(&a.db, i, name)) {
			found = archiveNameHas(name, needle);
		}
	}

	archive7zClose(&a);

	return found;
}

/** The next entry at or after i that has data in folder fi, or NumFiles. */
static UInt32 archive7zNextInFolder(const CSzArEx *db, UInt32 i, UInt32 fi)
{
	while (i < db->db.NumFiles && db->FileIndexToFolderIndexMap[i] == (UInt32)-1) {
		i++;
	}

	return i < db->db.NumFiles && db->FileIndexToFolderIndexMap[i] == fi ? i : db->db.NumFiles;
}

/**
 * The file entry i goes to: opened under destDir when it is wanted and its
 * name is one to write, NULL when its bytes are to be decoded past.
 */
// How far archiveExtractMatching() has got, for a notice drawn on another thread
// while it works: the wanted files counted before the first block is decoded,
// and the ones opened for writing since. Plain ints read without a lock - a
// count one file behind on a progress bar is not worth one.
static volatile s32 g_ArchiveProgressDone;
static volatile s32 g_ArchiveProgressTotal;

void archiveGetProgress(s32 *done, s32 *total)
{
	*done = g_ArchiveProgressDone;
	*total = g_ArchiveProgressTotal;
}

static FILE *archive7zOpenEntry(const CSzArEx *db, UInt32 i, const char *path, const char *destDir,
		archivefilter filter, void *arg)
{
	char name[ARCHIVE_MAXNAME];
	char out[FS_MAXPATH + 1];
	FILE *fp;

	if (!archive7zName(db, i, name) || !filter(name, arg)) {
		return NULL;
	}

	if (!archiveNameIsSafe(name)) {
		sysLogPrintf(LOG_WARNING, "archive: skipping %s in %s", name, path);
		return NULL;
	}

	snprintf(out, sizeof(out), "%s/%s", destDir, name);
	archiveMakeDirs(out);
	fp = fopen(out, "wb");

	if (!fp) {
		sysLogPrintf(LOG_ERROR, "archive: could not write %s", out);
	} else {
		g_ArchiveProgressDone++;
	}

	return fp;
}

/**
 * One solid block, decoded as a stream into the wanted files it holds. -2 when
 * the block is not one this can stream, and the caller extracts it the SDK's
 * way; -1 on a fault in the data.
 */
static s32 archive7zStreamFolder(struct archive7z *a, UInt32 fi, const char *path, const char *destDir,
		archivefilter filter, void *arg)
{
	const CSzArEx *db = &a->db;
	CSzFolder *folder = db->db.Folders + fi;
	const CSzCoderInfo *coder;
	CLzma2Dec lzma2;
	CLzmaDec lzma;
	s32 islzma2;
	UInt64 packLeft;
	UInt64 unpackLeft;
	UInt32 cur;
	UInt64 curLeft;
	FILE *fp = NULL;
	Byte *chunk;
	s32 written = 0;
	s32 failed = 0;
	s32 wantedLeft = 0;

	if (folder->NumCoders != 1 || folder->NumPackStreams != 1 || folder->NumBindPairs != 0) {
		return -2;
	}

	coder = &folder->Coders[0];

	if (coder->MethodID == ARCHIVE_7Z_LZMA2 && coder->Props.size == 1) {
		islzma2 = 1;
	} else if (coder->MethodID == ARCHIVE_7Z_LZMA && coder->Props.size == 5) {
		islzma2 = 0;
	} else {
		return -2;
	}

	packLeft = db->db.PackSizes[db->FolderStartPackStreamIndex[fi]];
	unpackLeft = SzFolder_GetUnpackSize(folder);

	if (LookInStream_SeekTo(&a->look.s, SzArEx_GetFolderStreamPos(db, fi, 0)) != SZ_OK) {
		return -1;
	}

	chunk = malloc(ARCHIVE_7Z_CHUNK);

	if (!chunk) {
		return -1;
	}

	if (islzma2) {
		Lzma2Dec_Construct(&lzma2);

		if (Lzma2Dec_Allocate(&lzma2, coder->Props.data[0], &a->alloc) != SZ_OK) {
			free(chunk);
			return -1;
		}

		Lzma2Dec_Init(&lzma2);
	} else {
		LzmaDec_Construct(&lzma);

		if (LzmaDec_Allocate(&lzma, coder->Props.data, 5, &a->alloc) != SZ_OK) {
			free(chunk);
			return -1;
		}

		LzmaDec_Init(&lzma);
	}

	// How many of the block's files are wanted, so decoding can stop after the
	// last of them rather than run on through the rest of the block: the
	// GoldenEye release is one block of 740MB, and what comes after its last
	// wanted file was most of the wait with nothing left to write.
	for (UInt32 i = archive7zNextInFolder(db, db->FolderStartFileIndex[fi], fi);
			i < db->db.NumFiles; i = archive7zNextInFolder(db, i + 1, fi)) {
		char name[ARCHIVE_MAXNAME];

		if (!db->db.Files[i].IsDir && archive7zName(db, i, name) && filter(name, arg)) {
			wantedLeft++;
		}
	}

	// The block's files in order, each taking the next Size bytes of it.
	cur = archive7zNextInFolder(db, db->FolderStartFileIndex[fi], fi);
	curLeft = cur < db->db.NumFiles ? db->db.Files[cur].Size : 0;

	if (cur < db->db.NumFiles) {
		fp = archive7zOpenEntry(db, cur, path, destDir, filter, arg);
	}

	while (!failed) {
		// Empty files, and the ones just finished, give way to the next.
		while (cur < db->db.NumFiles && curLeft == 0) {
			if (fp) {
				fclose(fp);
				fp = NULL;
				written++;
				wantedLeft--;
			}

			cur = archive7zNextInFolder(db, cur + 1, fi);

			if (cur < db->db.NumFiles) {
				curLeft = db->db.Files[cur].Size;
				fp = archive7zOpenEntry(db, cur, path, destDir, filter, arg);
			}
		}

		if (unpackLeft == 0 || cur >= db->db.NumFiles || (wantedLeft <= 0 && !fp)) {
			break;
		}

		{
			const void *inBuf = NULL;
			size_t lookahead = 1 << 18;
			SizeT inProcessed;
			SizeT outLen = ARCHIVE_7Z_CHUNK;
			ELzmaStatus status;
			SRes res;

			if (lookahead > packLeft) {
				lookahead = (size_t)packLeft;
			}

			if (outLen > unpackLeft) {
				outLen = (SizeT)unpackLeft;
			}

			if (a->look.s.Look(&a->look.s, &inBuf, &lookahead) != SZ_OK) {
				failed = 1;
				break;
			}

			inProcessed = lookahead;
			res = islzma2
				? Lzma2Dec_DecodeToBuf(&lzma2, chunk, &outLen, inBuf, &inProcessed, LZMA_FINISH_ANY, &status)
				: LzmaDec_DecodeToBuf(&lzma, chunk, &outLen, inBuf, &inProcessed, LZMA_FINISH_ANY, &status);

			if (res != SZ_OK || (inProcessed == 0 && outLen == 0)) {
				failed = 1;
				break;
			}

			if (a->look.s.Skip(&a->look.s, inProcessed) != SZ_OK) {
				failed = 1;
				break;
			}

			packLeft -= inProcessed;
			unpackLeft -= outLen;

			for (SizeT pos = 0; pos < outLen && cur < db->db.NumFiles; ) {
				const SizeT take = (UInt64)(outLen - pos) < curLeft ? outLen - pos : (SizeT)curLeft;

				if (fp && fwrite(chunk + pos, 1, take, fp) != take) {
					sysLogPrintf(LOG_ERROR, "archive: a write failed extracting %s", path);
					failed = 1;
					break;
				}

				pos += take;
				curLeft -= take;

				while (cur < db->db.NumFiles && curLeft == 0) {
					if (fp) {
						fclose(fp);
						fp = NULL;
						written++;
						wantedLeft--;
					}

					cur = archive7zNextInFolder(db, cur + 1, fi);

					if (cur < db->db.NumFiles) {
						curLeft = db->db.Files[cur].Size;
						fp = archive7zOpenEntry(db, cur, path, destDir, filter, arg);
					}
				}
			}
		}
	}

	if (fp) {
		fclose(fp);
	}

	if (islzma2) {
		Lzma2Dec_Free(&lzma2, &a->alloc);
	} else {
		LzmaDec_Free(&lzma, &a->alloc);
	}

	free(chunk);

	return failed ? -1 : written;
}

static s32 archiveExtract7zMatching(const char *path, const char *destDir, archivefilter filter, void *arg)
{
	struct archive7z a;
	UInt32 blockIndex = 0xffffffff;
	Byte *outBuffer = NULL;
	size_t outBufferSize = 0;
	s32 written = 0;

	g_ArchiveProgressDone = 0;
	g_ArchiveProgressTotal = 0;

	if (!archive7zOpen(&a, path)) {
		sysLogPrintf(LOG_ERROR, "archive: could not read %s as a 7z", path);
		return -1;
	}

	for (UInt32 i = 0; i < a.db.db.NumFiles; i++) {
		char name[ARCHIVE_MAXNAME];

		if (!a.db.db.Files[i].IsDir && archive7zName(&a.db, i, name) && filter(name, arg)) {
			g_ArchiveProgressTotal++;
		}
	}

	for (UInt32 fi = 0; fi < a.db.db.NumFolders; fi++) {
		s32 wanted = 0;
		s32 got;

		for (UInt32 i = archive7zNextInFolder(&a.db, a.db.FolderStartFileIndex[fi], fi);
				i < a.db.db.NumFiles && !wanted; i = archive7zNextInFolder(&a.db, i + 1, fi)) {
			char name[ARCHIVE_MAXNAME];
			wanted = archive7zName(&a.db, i, name) && filter(name, arg);
		}

		if (!wanted) {
			continue;
		}

		got = archive7zStreamFolder(&a, fi, path, destDir, filter, arg);

		if (got >= 0) {
			written += got;
			continue;
		}

		if (got == -1) {
			sysLogPrintf(LOG_ERROR, "archive: block %u of %s would not decode", fi, path);
			written = -1;
			break;
		}

		// Not a block this streams: the SDK's own way, a file at a time.
		for (UInt32 i = archive7zNextInFolder(&a.db, a.db.FolderStartFileIndex[fi], fi);
				i < a.db.db.NumFiles; i = archive7zNextInFolder(&a.db, i + 1, fi)) {
			char name[ARCHIVE_MAXNAME];
			size_t offset = 0;
			size_t outSize = 0;

			if (!archive7zName(&a.db, i, name) || !filter(name, arg) || !archiveNameIsSafe(name)) {
				continue;
			}

			if (SzArEx_Extract(&a.db, &a.look.s, i, &blockIndex, &outBuffer, &outBufferSize,
					&offset, &outSize, &a.alloc, &a.allocTemp) != SZ_OK) {
				sysLogPrintf(LOG_WARNING, "archive: could not extract %s from %s", name, path);
				continue;
			}

			written += archiveWriteFile(destDir, name, outBuffer + offset, (u32)outSize);
			g_ArchiveProgressDone++;
		}
	}

	IAlloc_Free(&a.alloc, outBuffer);
	archive7zClose(&a);

	return written;
}

s32 archiveExtractMatching(const char *path, const char *destDir, archivefilter filter, void *arg)
{
	if (strcasecmp(archiveExt(path), ".7z")) {
		return archiveExtract(path, destDir);
	}

	if (fsCreateDir(destDir) != 0 && fsFileSize(destDir) < 0) {
		sysLogPrintf(LOG_ERROR, "archive: could not create %s", destDir);
		return -1;
	}

	return archiveExtract7zMatching(path, destDir, filter, arg);
}

s32 archiveExtract(const char *path, const char *destDir)
{
	const char *ext = archiveExt(path);

	if (fsCreateDir(destDir) != 0 && fsFileSize(destDir) < 0) {
		sysLogPrintf(LOG_ERROR, "archive: could not create %s", destDir);
		return -1;
	}

	if (!strcasecmp(ext, ".7z")) {
		return archiveExtract7z(path, destDir);
	}

	if (!strcasecmp(ext, ".zip") || !strcasecmp(ext, ".pk3")) {
		return archiveExtractZip(path, destDir);
	}

	if (!strcasecmp(ext, ".rar")) {
		return archiveExtractRar(path, destDir);
	}

	sysLogPrintf(LOG_ERROR, "archive: %s is not an archive this can open", path);

	return -1;
}
