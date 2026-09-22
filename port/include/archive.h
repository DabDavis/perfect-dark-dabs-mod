#ifndef _IN_ARCHIVE_H
#define _IN_ARCHIVE_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether path looks like an archive this can open, by extension.
 */
s32 archiveIsSupported(const char *path);

/**
 * Extracts every file in a .zip or .7z into destDir, recreating the directory
 * structure inside the archive. Returns the number of files written, or -1.
 *
 * Extraction rather than reading entries where they lie, because a .7z is
 * usually solid: its files share one compressed block, so reading a single
 * texture out of it means decompressing the whole pack. Unpacking once turns
 * that into the case the decoder is good at - one pass, in order - and leaves
 * the loader with an ordinary directory.
 *
 * Paths that climb out of destDir are skipped rather than followed.
 */
s32 archiveExtract(const char *path, const char *destDir);

/**
 * Whether any entry of a .7z or .zip has needle in its name, compared without
 * case and with either path separator. 1 if one does, 0 if none does or the
 * archive cannot be read (a .rar is never looked into).
 *
 * Only the archive's directory is read, never the data, so this is what tells
 * two archives in the same folder apart - the Perfect Dark release and the
 * GoldenEye one - without unpacking either.
 */
s32 archiveFindEntry(const char *path, const char *needle);

/** Answers whether an entry, by its name inside the archive, is wanted. */
typedef s32 (*archivefilter)(const char *name, void *arg);

/**
 * archiveExtract() for the entries filter wants and no others. Returns the
 * number of files written, or -1.
 *
 * A .7z block made by one LZMA or LZMA2 coder - which is how 7-Zip writes a
 * solid archive - is decoded as a stream straight into the files, so what is
 * held at once is the decoder's dictionary and not the block: the GoldenEye
 * release is one solid block of 740MB, and SzArEx_Extract() would allocate all
 * of it to write out the 60MB of characters that are asked for. A block of
 * any other kind goes through SzArEx_Extract() as archiveExtract() does. A .zip
 * is filtered entry by entry too; a .rar is extracted whole.
 */
s32 archiveExtractMatching(const char *path, const char *destDir, archivefilter filter, void *arg);

/**
 * The files archiveExtractMatching() has written so far of those it will write,
 * for a notice drawn while it runs on another thread. A .7z only; both are 0
 * before the listing is read.
 */
void archiveGetProgress(s32 *done, s32 *total);

#ifdef __cplusplus
}
#endif

#endif
