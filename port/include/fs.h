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

// The one folder for what a player adds to the game that is not a mod: a
// GoldenEye 007 ROM, the Perfect Dark XBLA release, the GoldenEye XBLA release.
// Mods stay in mods/, and Perfect Dark's own ROM in data/.
#define FS_ADDED_CONTENT_DIR "added-content"

// Every place that folder can be, in the order they are searched; the folders
// these files went in before it existed (data/, xbla/) are the caller's to add
// after them, so an install that was working keeps working.
#define FS_ADDED_CONTENT_SEARCH \
	"$E/" FS_ADDED_CONTENT_DIR, \
	"$H/" FS_ADDED_CONTENT_DIR, \
	"./" FS_ADDED_CONTENT_DIR, \
	"$S/" FS_ADDED_CONTENT_DIR

// Makes the folder, with a note in it saying what goes there, where
// fsChooseOutputDir() would put it. dst gets the "$E/..." or "$S/..." form.
// Returns 0 on success.
s32 fsAddedContentDir(char *dst, u32 dstSize);

// Moves every non-dot entry of oldDir into dst (an added-content/ path in the
// "$E/..." form), skipping names dst already has, and removes oldDir once
// empty. Says what it did in the log.
void fsMoveIntoAddedContent(const char *oldDir, const char *dst);

#endif
