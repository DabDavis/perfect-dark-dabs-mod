#ifndef _IN_GEMONITOR_H
#define _IN_GEMONITOR_H

#include <ultra64.h>
#include <PR/ultratypes.h>
#include "types.h"

// A GoldenEye remake mission's TVs and projection screens run GoldenEye's own
// programmes over GoldenEye's own pictures (gemonitor.c).

// Every stage load, from setup.c: the conversion's menu/gemonitors.bin for a
// remake mission, and nothing for any other stage.
void geMonitorStageStart(s32 stagenum);
// tvscreenSetImageByNum(): GoldenEye's programme of that number, or NULL for
// Perfect Dark's own.
u32 *geMonitorProgram(s32 imagenum);
// tvscreenTick()'s two jumps: where a jump in one of GoldenEye's programmes
// goes, its argument being a word of the block rather than a pointer. NULL
// when the list is not one of them and the argument is what it always was.
u32 *geMonitorJump(u32 *cmdlist, u32 arg);
// tvscreenRender(): GoldenEye's picture of that index for a screen running one
// of GoldenEye's programmes, or NULL.
struct textureconfig *geMonitorImage(u32 *cmdlist, u32 index);

// GE Plus's folder shows the programmes on a page of their own: the tables out
// of that mod directory's conversion (the count, or 0) and a programme by
// number.
s32 geMonitorOpen(s32 moddir, const char *dir);
void geMonitorClose(void);
u32 *geMonitorProgramAt(s32 n);

#endif
