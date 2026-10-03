#ifndef _IN_GETEXSHRINK_H
#define _IN_GETEXSHRINK_H
#include <ultra64.h>
#include "types.h"

s32 geTexShrinkPaletted(u8 *src, u8 *dst, s32 srcwidth, s32 srcheight, s32 format, u16 *palette, s32 numcolours);
s32 geTexShrinkNonPaletted(u8 *src, u8 *dst, s32 srcwidth, s32 srcheight, s32 format);

#endif
