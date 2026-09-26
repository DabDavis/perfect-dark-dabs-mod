#ifndef IN_GAME_SHARDS_H
#define IN_GAME_SHARDS_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

void shardCreate(RoomNum room, struct coord *pos, f32 rotx, f32 size, s32 type);
void shardsCreate(struct coord *pos, f32 *rotx, f32 *roty, f32 *rotz, f32 xmin, f32 xmax, f32 ymin, f32 ymax, s32 type, struct prop *prop);
void shardsReset(void);
Gfx *shardsRender(Gfx *gdl);
void shardsStop(void);
void shardsTick(void);

#endif
