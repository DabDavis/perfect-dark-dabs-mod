#ifndef _IN_PORT_MODLOADER_H
#define _IN_PORT_MODLOADER_H

#include <PR/ultratypes.h>

void modloaderInit(void);
const char *modloaderGetStageModDir(s32 stagenum);
const char *modloaderGetStageAllocation(s32 stagenum);

#endif
