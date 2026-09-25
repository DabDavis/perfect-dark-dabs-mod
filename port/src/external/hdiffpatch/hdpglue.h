#ifndef _IN_HDPGLUE_H
#define _IN_HDPGLUE_H

/**
 * The patch half of HDiffPatch (sisong/HDiffPatch, MIT - see LICENSE here),
 * cut down to the one thing the port asks of it: applying a directory patch
 * made by hdiffz -  lzma compressed, fadler64 checksummed - to a folder.
 *
 * Only the files the patch makes, changes or renames are written; the ones it
 * leaves as they are ("same" pairs under their own name) are not copied, so the output is an overlay to
 * be read before the old folder, not a second copy of it.
 */

typedef void (*hdpnamefn)(const char *name, void *arg);

/**
 * Calls fn with every file of the old folder the patch reads from - its
 * reference files and the old names of the files it renames - as paths
 * relative to the folder. 0 when the patch cannot
 * be read.
 */
int hdpListOldRefs(const char *diffPath, hdpnamefn fn, void *arg);

/**
 * Applies it: oldDir and outDir end in a separator. The patch's own checksums
 * of what it reads and writes are checked. Answers the number of files written,
 * or a negative step that failed: -1 the patch, -2 its folder data, -3 its
 * checksum set, -4 the old files, -5 the output, -6 the patch itself (a
 * checksum failed here means the old folder is not the one it was made from).
 */
int hdpApplyOverlay(const char *oldDir, const char *diffPath, const char *outDir);

/**
 * A single-file patch (hdiffz's compressed diff, lzma), old and patch in
 * memory: *out is malloc'd and holds the new file. 1 when it applied, 0 when
 * the patch cannot be read, is not for a file of oldLen bytes, or failed.
 */
#include <stddef.h>
int hdpApplyMem(const unsigned char *old, size_t oldLen, const unsigned char *diff, size_t diffLen,
		unsigned char **out, size_t *outLen);

#endif
