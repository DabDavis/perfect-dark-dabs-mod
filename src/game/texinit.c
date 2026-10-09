#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/dma.h"
#include "lib/memp.h"
#include "data.h"
#include "types.h"

#ifndef PLATFORM_N64
// How many textures g_Textures describes: NUM_TEXTURES for the ROM's list, more
// for a mod's longer one (set again when modsegs.c swaps the list)
s32 g_NumListTextures = NUM_TEXTURES;
#endif

void texInit(void)
{
	extern u8 EXT_SEG _textureslistSegmentRomStart;
	extern u8 EXT_SEG _textureslistSegmentRomEnd;

	u32 len = ((REF_SEG _textureslistSegmentRomEnd - REF_SEG _textureslistSegmentRomStart) + 15) & -16;

	g_Textures = mempAlloc(len, MEMPOOL_PERMANENT);

	dmaExec(g_Textures, (romptr_t) REF_SEG _textureslistSegmentRomStart, len);

#ifndef PLATFORM_N64
	// each texture's data runs to the next one's offset, so the last entry
	// only ends the one before it
	g_NumListTextures = (REF_SEG _textureslistSegmentRomEnd - REF_SEG _textureslistSegmentRomStart) / sizeof(struct texture) - 1;
#endif
}
