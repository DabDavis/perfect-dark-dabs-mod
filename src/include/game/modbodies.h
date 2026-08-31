#ifndef _IN_GAME_MODBODIES_H
#define _IN_GAME_MODBODIES_H

#include <ultra64.h>
#include "types.h"

extern s32 g_ModBodiesNoDraw;
extern f32 g_ModBodiesLodScale;
extern s32 g_ModBodiesPoseTooBig;
bool modBodyIsKept(struct chrdata *chr);
void modBodiesClaim(struct chrdata *chr);
void modBodiesAllocatePoses(s32 numchrslots);
bool modBodyPoseApply(struct chrdata *chr, struct modelrenderdata *renderdata);
bool modBodyPoseHold(struct chrdata *chr, struct modelrenderdata *renderdata);
bool modBodyPoseDraw(struct chrdata *chr);
void modBodyPoseDrop(struct chrdata *chr);
void modBodiesTick(void);
s32 modBodiesGetReserve(void);
s32 modBodiesSetReserve(void);

#endif
