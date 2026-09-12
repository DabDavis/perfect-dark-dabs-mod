#ifndef _IN_PORT_MODLOADER_H
#define _IN_PORT_MODLOADER_H

#include <PR/ultratypes.h>

void modloaderInit(void);
void modloaderGetStats(s32 *registered, s32 *found, s32 *mods);
const char *modloaderGetStageModDir(s32 stagenum);
// The same directory as an index into the mounted list, or -1. What a texture
// loaded for that stage is registered under, so its own pack can be found
// again at the draw - see texpackTextureArt().
s32 modloaderGetStageModDirIndex(s32 stagenum);
const char *modloaderGetStageAllocation(s32 stagenum);

#endif
