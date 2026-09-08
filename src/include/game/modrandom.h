#ifndef IN_GAME_MODRANDOM_H
#define IN_GAME_MODRANDOM_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

// The generator this build deals with; a run's own version lives in
// g_ModOptions.randomversion and may be older. See modrandom.c.
#define MODRANDOM_VERSION_DEFAULT 1

bool modRandomIsOn(void);
s32 modRandomGetVersion(void);
u32 modRandomGetSeed(void);
void modRandomRoll(s32 stagenum);
void modRandomTick(void);
bool modRandomTakeSpawn(struct coord *pos, RoomNum *rooms, f32 *angle);
void modRandomInsertObjectives(void);
char *modRandomGetObjectiveText(s32 index);

#endif
