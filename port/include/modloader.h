#ifndef _IN_PORT_MODLOADER_H
#define _IN_PORT_MODLOADER_H

#include <PR/ultratypes.h>

void modloaderInit(void);
void modloaderGetStats(s32 *registered, s32 *found, s32 *mods);
const char *modloaderGetStageModDir(s32 stagenum);
const char *modloaderGetStageAllocation(s32 stagenum);

#endif
