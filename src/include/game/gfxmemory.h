#ifndef _IN_GAME_GFXMEMORY_H
#define _IN_GAME_GFXMEMORY_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

extern u8 *g_GfxBuffers[3];

void gfxReset(void);
Gfx *gfxGetMasterDisplayList(void);
Vtx *gfxAllocateVertices(u32 count);
void gfxCheckGfxPool(const Gfx *gdl);
void *gfxAllocateMatrix(void);
LookAt *gfxAllocateLookAt(s32 count);
Col *gfxAllocateColours(s32 count);
void *gfxAllocate(u32 size);
#ifndef PLATFORM_N64
bool gfxHasVtxSpace(u32 size);
#endif
void gfxSwapBuffers(void);
s32 gfxGetFreeGfx(Gfx *gdl);
#ifndef PLATFORM_N64
// The last frame's use of the master display list (in Gfx commands) and the
// vtx pool (in bytes), for the F3 trace dump.
void gfxTraceGetPools(u32 *gfxused, u32 *gfxsize, u32 *vtxused, u32 *vtxsize);
#endif

#endif
