#ifndef _IN_MPSETUPS_H
#define _IN_MPSETUPS_H

#include "types.h"

s32 mpsetupLoadCurrentFile(void);
s32 mpsetupSaveCurrentFile(void);
void mpsetupLoadSetup(s32 slotindex);
s32 mpsetupSaveSetup(s32 slotindex, u8 savefile);
void mpsetupCopyAllFromPak(void);
// A Perfect Dark mod's own saved setups (modmode.c): PATH, or NULL for the save dir's
void mpsetupSwitchFile(const char *path);
const char *mpsetupDefaultFile(void);

#endif
