#ifndef IN_GAME_STAGEMUSIC_H
#define IN_GAME_STAGEMUSIC_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

void stageSetTracks(struct stagemusic *tracks);
s32 stageGetPrimaryTrack(s32 stagenum);
s32 stageGetAmbientTrack(s32 stagenum);
s32 stageGetNrgTrack(s32 stagenum);

#endif
