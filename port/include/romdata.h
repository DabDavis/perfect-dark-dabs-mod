#ifndef _IN_ROMDATA_H
#define _IN_ROMDATA_H

#include <PR/ultratypes.h>

extern u8 *g_RomFile;
extern u32 g_RomFileSize;

s32 romdataInit(void);

// The ROM file name, with --rom-file honoured; valid before romdataInit().
const char *romdataGetRomName(void);
// The segment table as declared for this ROM version: name, stock ROM offset
// (0 for a segment this version lacks) and size (0 = runs to the next one).
s32 romdataGetNumSegments(void);
const char *romdataGetSegmentInfo(s32 index, u32 *romofs, u32 *size);

u8 *romdataFileLoad(s32 fileNum, u32 *outSize);
void romdataFilePreprocess(s32 fileNum, s32 loadType, u8 *data, u32 size, u32 *outSize);
void romdataFileFree(s32 fileNum);
const char *romdataFileGetName(s32 fileNum);
s32 romdataRegisterModFile(const char *name, s32 modDirIndex);

// Drops every loaded file and rebuilds the slots from the ROM, so that the next
// load of each one searches the mod directories again. For switching mods at
// runtime; segments are not affected and cannot be.
void romdataResetFiles(void);

u8 *romdataFileGetData(s32 fileNum);
s32 romdataFileGetSize(s32 fileNum);

s32 romdataFileGetNumForName(const char *name);

// The stock ROM's inflated data segment, for reading its tables at their ROM addresses.
const u8 *romdataGetDataSeg(u32 *outSize);

u8 *romdataSegGetData(const char *segName);
u8 *romdataSegGetDataEnd(const char *segName);
u32 romdataSegGetSize(const char *segName);
u32 romdataFileGetEstimatedSize(const u32 size, const u32 loadtype);

s32 romdataCheckGbcRom(void);

#endif
