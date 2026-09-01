#ifndef _IN_GAME_MODSPECTATE_H
#define _IN_GAME_MODSPECTATE_H

#include <ultra64.h>
#include "types.h"

extern f32 g_ModSpectateSpeed;
extern s32 g_ModSpectateStart;
extern s32 g_ModSpectateStartArg;

bool modSpectateIsOn(void);
void modSpectateSetOn(bool on);
void modSpectateToggle(void);
void modSpectateStartNext(void);
void modSpectateClearStartNext(void);
void modSpectateApplyStart(void);
void modSpectateTick(void);
void modSpectateReset(void);

#endif
