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

#ifdef __cplusplus
}
#endif

#endif
