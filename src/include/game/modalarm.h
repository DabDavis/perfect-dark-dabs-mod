#ifndef _IN_GAME_MODALARM_H
#define _IN_GAME_MODALARM_H

#include <ultra64.h>
#include "types.h"

/**
 * Guards Alerted!: the alarm as a permanent condition, and the guards it keeps
 * sending. The setting itself lives in modoptions.h; this is the machinery.
 */

void modAlarmReset(void);
s32 modAlarmSetReserve(void);
s32 modAlarmGetReserve(void);
void modAlarmTick(void);
bool modAlarmIsGuard(struct chrdata *chr);
void modAlarmRecordGuardKill(s32 aplayernum);
void modAlarmRecordGuardDeath(s32 vplayernum);
s32 modAlarmGetGuardKills(s32 mpindex);
s32 modAlarmGetGuardDeaths(s32 mpindex);
bool modAlarmHasMatchStats(void);
struct chrdata *modAlarmFindGuardForBot(struct chrdata *botchr, f32 maxdist);

#endif
