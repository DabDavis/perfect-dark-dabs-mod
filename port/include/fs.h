#ifndef _IN_FS_H
#define _IN_FS_H

#include <stdio.h>
#include <PR/ultratypes.h>

#define FS_MAXPATH 1024

s32 fsInit(void);

const char *fsFullPath(const char *relPath);

s32 fsPathIsAbsolute(const char *path);
s32 fsPathIsCwdRelative(const char *path);

void *fsFileLoad(const char *name, u32 *outSize);
s32 fsFileLoadTo(const char *name, void *dst, u32 dstSize);
s32 fsFileSize(const char *name);

FILE *fsFileOpenWrite(const char *name);
FILE *fsFileOpenRead(const char *name);
void fsFileFree(FILE *f);

const char *fsGetModDir(void);
// A directory mounted for its maps alone: reached by pinned slots and the
// mod loader, never by the file search. How many of the mounted dirs overlay
// (0 or 1): those are the ones a texture pack or a modconfig is read from.
s32 fsAddMapsDir(const char *path);
s32 fsGetNumOverlayModDirs(void);
typedef void (*fsScanCallback)(const char *name, void *arg);
s32 fsScanDir(const char *path, fsScanCallback cb, void *arg);
s32 fsAddModDir(const char *path);
void fsReplaceModDir(const char *path);
s32 fsGetNumModDirs(void);
const char *fsGetModDirAt(s32 index);
s32 fsCreateDir(const char *path);
// Removes an empty directory. Returns 0 on success.
s32 fsRemoveDir(const char *path);
// Renames a file or directory; the destination must not exist. Returns 0 on success.
s32 fsRename(const char *from, const char *to);
// Removes a file. Returns 0 on success.
s32 fsRemoveFile(const char *path);

// Picks and creates the directory a player's own files should go in: beside the
// executable where that can be written, and in the save directory where it
// cannot. dst gets the "$E/name" or "$S/name" form. Returns 0 on success.
s32 fsChooseOutputDir(const char *name, char *dst, u32 dstSize);

#endif
