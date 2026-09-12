#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>

#include "data.h"
#include "bss.h"
#include "game/setuputils.h"
#include "game/texdecompress.h"
#include "mod.h"

#include "preprocess/common.h"

u8 *preprocessAnimations(u8* data, u32 size, u32* outSize)
{
	// set the anim table pointers as well
	extern u8 *_animationsTableRomStart;
	extern u8 *_animationsTableRomEnd;

	// the animation table is at the end of the segment
	u32 *animtbl = (void *)(data + size - 0x38a0);
	_animationsTableRomStart = (u8 *)animtbl;
	_animationsTableRomEnd = data + size;

	PD_SWAP_VAL(*animtbl);
	const u32 count = *animtbl++;

	struct animtableentry *anim = (struct animtableentry *)animtbl;
	for (u32 i = 0; i < count; ++i, ++anim) {
		PD_SWAP_VAL(anim->numframes);
		PD_SWAP_VAL(anim->bytesperframe);
		PD_SWAP_VAL(anim->headerlen);
		PD_SWAP_VAL(anim->data);
		// if an external replacement exists, replace the table entry and mark the offset
		if (modAnimationLoadDescriptor(i, anim) > 0) {
			anim->data = 0xffffffff;
		}
	}

	return NULL;
}

/**
 * The ROM's mpconfigs segment, left as it is.
 *
 * This used to walk the segment as an array of struct mpconfig and byte-swap
 * each record's fields. It cannot: a record in the ROM is 104 bytes and this
 * build's struct is 116 (a longer setup name, and storedbotbits), so every
 * record after the first was swapped at offsets that are not its fields -
 * writing into the ROM image, or into a mod's own segs/mpconfigs where one
 * replaced it - and the last five records of the 44 were not reached at all.
 *
 * Nothing reads the segment. The game's 44 configs are g_MpConfigs in
 * src/game/mpconfigs.c, in this build's own layout, and challengeLoadConfig()
 * takes each one from there. Converting the segment properly would mean a
 * struct for the ROM's layout, for data with no reader.
 */
u8 *preprocessMpConfigs(u8* data, u32 size, u32* outSize)
{
	return NULL;
}

u8 *preprocessTexturesList(u8* data, u32 size, u32* outSize)
{
	struct texture *tex = (struct texture *)data;
	const u32 count = size / sizeof(*tex);
	for (u32 i = 0; i < count; ++i, ++tex) {
		// TODO: it sure looks like none of the fields except soundsurfacetype, surfacetype and dataoffset are set
		// just swap the last 3 bytes of the first word...
		const u32 dofs = (u32)tex->dataoffset << 8;
		tex->dataoffset = PD_BE32(dofs);
		// ...and the surface types in the first byte
		const u8 tmp = tex->soundsurfacetype;
		tex->soundsurfacetype = tex->surfacetype;
		tex->surfacetype = tmp;
	}

	return NULL;
}
