#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/memp.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "game/modoptions.h"
#endif

void vtxstoreReset(void)
{
	s32 i;
	s32 j;
	s32 num;
	s32 val;

	for (i = 0; i < ARRAYCOUNT(g_VtxstoreTypes); i++) {
		if (PLAYERCOUNT() >= 2) {
			val = g_VtxstoreTypes[i].valifmp;
			num = g_VtxstoreTypes[i].numifmp;
		} else if (!STAGE_IS_LEVEL(g_Vars.stagenum)) {
			val = g_VtxstoreTypes[i].valifspecial;
			num = g_VtxstoreTypes[i].numifspecial;
		} else {
			val = g_VtxstoreTypes[i].valifsp;
			num = g_VtxstoreTypes[i].numifsp;
		}

#ifndef PLATFORM_N64
		// Room for the blood on kept bodies. A hit on a chr in view copies
		// that part's vertices or colours into this store, and the copy lives
		// as long as the model does, so a pool of bodies fills a store sized
		// for the N64's handful of corpses in a minute and vtxstoreAllocate()
		// starts reaping. Two vertex blocks and three colour blocks a body,
		// of the size a body part comes in; the mema heap they are cut from
		// grows by the same amount (pdmain.c). The object types are not
		// touched. Sized from the cap asked for rather than the reserve
		// granted, which is not decided until setupLoadFiles(), after this.
		if (modKeepsBodies()) {
			s32 cap = modGetBodiesKept();

			if (i == VTXSTORETYPE_CHRVTX) {
				num += cap * 2;
				val += cap * 200;
			} else if (i == VTXSTORETYPE_CHRCOL) {
				num += cap * 3;
				val += cap * 300;
			}
		}
#endif

		if (num > 0) {
			g_VtxstoreTypes[i].unk24 = mempAlloc(num * sizeof(struct var8007e3d0_data), MEMPOOL_STAGE);
		}

		for (j = 0; j < num; j++) {
			g_VtxstoreTypes[i].unk24[j].unk0e = 0;
		}

		g_VtxstoreTypes[i].numallocated = num;
		g_VtxstoreTypes[i].val1 = val;
		g_VtxstoreTypes[i].val2 = val;
	}
}
