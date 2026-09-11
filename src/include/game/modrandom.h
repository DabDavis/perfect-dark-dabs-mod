#ifndef IN_GAME_MODRANDOM_H
#define IN_GAME_MODRANDOM_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

// The generator this build deals with; a run's own version lives in
// g_ModOptions.randomversion and may be older. See modrandom.c.
#define MODRANDOM_VERSION_DEFAULT 3

void modRandomArmMission(void);
void modRandomDisarmMission(void);
bool modRandomIsArmed(void);
bool modRandomIsOn(void);
bool modRandomIsEndless(void);
s32 modRandomGetRooms(void);
s32 modRandomGetCleared(void);
s32 modRandomGetVersion(void);
u32 modRandomGetSeed(void);
void modRandomRoll(s32 stagenum);
void modRandomTick(void);
bool modRandomTakeSpawn(struct coord *pos, RoomNum *rooms, f32 *angle);

// Where a player put down on this pad would stand (false only when there is no
// floor under it at all), and whether it is a pad worth dealing in the first
// place. The Randomizer's start and the run's landing both ask; see
// modrandom.c.
bool modRandomPadSpawnPos(s32 padnum, struct coord *pos, RoomNum *room);
bool modRandomPadCanSpawn(s32 padnum);
void modRandomInsertObjectives(void);
char *modRandomGetObjectiveText(s32 index);

#endif
