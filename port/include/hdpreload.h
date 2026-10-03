#ifndef _IN_HDPRELOAD_H
#define _IN_HDPRELOAD_H

#include <PR/ultratypes.h>

/**
 * A GoldenEye stage's HD meshes built while it loads (hdpreload.c). Begin
 * before the setup loads its models, end once the players are spawned, and
 * tick every lvTick(). All three do nothing outside a GE Plus or converted
 * stage in the HD look.
 */
void hdPreloadBegin(s32 stagenum);
void hdPreloadEnd(void);
// Each lvTick(): the stage's first ticks build what its intro AI hands out
void hdPreloadTick(void);

#endif
