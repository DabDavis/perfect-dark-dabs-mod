#ifndef _IN_GAME_WALLHIT_H
#define _IN_GAME_WALLHIT_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

void wallhitReset(void);

s16 wallhitFinaliseAxis(f32 arg0);
void wallhitFree(struct wallhit *wallhit);
void wallhitsFreeByProp(struct prop *prop, s8 layer);
bool chrIsUsingPaintball(struct chrdata *chr);
void wallhitChooseBloodColour(struct prop *prop);
void wallhitFade(struct wallhit *wallhit, u32 arg1);
bool wallhitRemoveOneInRoom(s32 arg0);
void wallhitRemoveOne(void);
void wallhitsTick(void);

void wallhitCreate(struct coord *relpos, struct coord *arg1, struct coord *arg2, s16 arg3[3],
		s16 arg4[3], s16 texnum, RoomNum room, struct prop *objprop,
		s8 mtxindex, s8 arg9, struct chrdata *chr, bool xlu);

void wallhitCreateWith20Args(struct coord *relpos, struct coord *arg1, struct coord *arg2, s16 arg3[3],
		s16 arg4[3], s16 texnum, RoomNum room, struct prop *objprop,
		struct prop *chrprop, s8 mtxindex, s8 arg10, struct chrdata *chr,
		f32 width, f32 height, u8 minalpha, u8 maxalpha,
		s32 rotdeg, u32 timermax, u32 timerspeed, bool xlu);

s32 wallhit0f140750(struct coord *coord);
Gfx *wallhitRenderPropHits(Gfx *gdl, struct prop *prop, bool xlupass);
Gfx *wallhitRenderBgHits(s32 roomnum, Gfx *gdl);
void wallhitsRecolour(void);
void wallhitFadeSplatsForRemovedChr(struct prop *chrprop);
void wallhitRemoveOldestWoundedSplatByChr(struct prop *chrprop);

#ifndef PLATFORM_N64
/**
 * Netplay (protocol 29, networld.c): a settled mark on the bg (a bullet
 * hole, a scorch, a splat; none on a prop or a chr), as a joiner is given
 * the host's
 */
struct wallhitnet {
	f32 relpos[3];
	s16 room;
	u8 texnum;
	u8 bits;      // 1 xlu, 2 unk6f_01
	s8 mtxindex;
	s16 xyz[4][3];
	s16 st[4][2];
	u8 base[4][4];
	u8 final[4][4];
	u32 frame;    // createdframe: oldest first
};

s32 wallhitNetList(struct wallhitnet *out, s32 max);
s32 wallhitNetAdd(const struct wallhitnet *w, u32 *frame); // its pool slot, -1 none
bool wallhitNetGet(s32 i, u32 frame, struct wallhitnet *out);
void wallhitNetRemove(s32 i, u32 frame, bool now);

// networld.c
void netWorldMarkMade(s32 slot, u32 frame);
s32 netWorldClientOwnMark(struct chrdata *chr, s32 texnum);
#endif

#endif
