/**
 * Archive extraction for texture packs.
 *
 * Two formats, for opposite reasons. Zip is here because zlib is already linked
 * and the container around it is a few structures. 7z is here because that is
 * how the packs in the wild are actually distributed, and it needs the LZMA
 * decoder under port/src/external/lzma - the same public domain SDK Project64
 * carries, cut down to what reading an archive needs.
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

	return !strcasecmp(ext, ".zip") || !strcasecmp(ext, ".7z") || !strcasecmp(ext, ".pk3");
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

	sysLogPrintf(LOG_ERROR, "archive: %s is not an archive this can open", path);

	return -1;
}
