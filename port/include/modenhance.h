#ifndef _IN_MODENHANCE_H
#define _IN_MODENHANCE_H

#include <PR/ultratypes.h>

// Enhancements On/Off: every addition off at once and back as it was
// (optionsmenu.c, "Enhancements On/Off").
#define MODENHANCE_SAVED_LEN 512

extern char g_ModEnhancementsSaved[MODENHANCE_SAVED_LEN];
extern char g_ModEnhancementsKeyName[32];

s32 modEnhancementsAreOn(void);
void modEnhancementsSetOn(s32 on);
s32 modEnhancementsGetKey(void);
void modEnhancementsSetKey(s32 vk);
void modEnhancementsTick(void);

#endif
