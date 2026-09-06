#ifndef _IN_GAME_MODRESPAWN_H
#define _IN_GAME_MODRESPAWN_H

#include <ultra64.h>
#include "types.h"

/**
 * Mission Respawn: a death in a mission is a new life where the player fell,
 * not Mission Failed. The setting and the lives count live in modoptions.h;
 * this is the machinery. See modrespawn.c.
 */

void modRespawnReset(void);
bool modRespawnCanRespawn(void);
void modRespawnRecordDeath(void);
void modRespawnBegin(void);
bool modRespawnIsRespawning(void);
s32 modRespawnGetWeapon(s32 handnum);
void modRespawnEnd(void);

#endif
