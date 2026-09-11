#ifndef IN_GAME_MODRUN_H
#define IN_GAME_MODRUN_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

/**
 * The Randomizer's run: one room at a time, across every map in the game.
 * See modrun.c for what it is and why it is shaped this way.
 */

// Where a run is allowed to land. The pool is a setting rather than a fixed
// list because a map that faults takes an hour-old run with it, and narrowing
// the pool is the only thing a player can do about that from the menu.
#define MODRUN_POOL_SOLO 0 // the missions: guards, weapons, crates, tags
#define MODRUN_POOL_MP   1 // and the Combat Simulator arenas
#define MODRUN_POOL_ALL  2 // and every map a mod brought with it
#define MODRUN_POOL_MAX  MODRUN_POOL_ALL

// What a room can ask for. One per kind of thing the run can check itself,
// because the stage's own objectives are written for a mission and this is
// one room of one.
#define MODRUN_OBJ_KILL    0 // put down the guards that come
#define MODRUN_OBJ_SURVIVE 1 // still standing when the clock runs out
#define MODRUN_OBJ_COLLECT 2 // the gun lying in this room

#ifndef PLATFORM_N64
extern bool g_ModRunAutoStart; // --random-run: begin one without a menu press
extern s32 g_ModRunAutoHop;    // --run-autohop N: hop after N frames, for a headless chain
#endif

bool modRunIsOn(void);
bool modRunIsPlaying(void);
bool modRunIsOver(void);

void modRunStart(void);
void modRunStop(void);

void modRunRoll(void);
void modRunTick(void);

// The seal: the room's doors are shut until its objective is done. The move
// test is called by the movement code (bondwalk.c, bondbike.c) before it asks
// the collision system anything, and answers as a wall would.
bool modRunIsSealed(void);
bool modRunSealMove(RoomNum *fromrooms, struct coord *frompos, RoomNum *torooms, struct coord *dstpos);
bool modRunTakeSpawn(struct coord *pos, RoomNum *rooms, f32 *angle);
bool modRunIsLanding(void);
void modRunRestoreInventory(void);
s32 modRunGetHandWeapon(s32 handnum);
void modRunRestoreHealth(void);
void modRunInsertObjectives(void);
char *modRunGetObjectiveText(s32 index);
s32 modRunChooseBody(void);
s32 modRunGetGuardCount(void);
s32 modRunGetGuardSpeed(void);

// Where a run's guards may appear. A sealed room is a fight the player cannot
// walk away from, so a guard placed where it cannot walk in is a fight that
// never happens: modalarm.c deals into the zone first while one is sealed.
bool modRunGuardsWantZone(void);
bool modRunGuardRoomOk(s32 room);
f32 modRunGuardMinDist(void);

s32 modRunGetScore(void);
s32 modRunGetRooms(void);
s32 modRunGetBestScore(void);
s32 modRunGetBestRooms(void);
u32 modRunGetSeed(void);
const char *modRunGetStageName(s32 stagenum);

#endif
