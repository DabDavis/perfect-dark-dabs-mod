#include <ultra64.h>
#include "constants.h"
#include "game/pdmode.h"
#include "game/bondgun.h"
#include "game/game_0b0fd0.h"
#include "game/inv.h"
#include "game/playermgr.h"
#include "game/options.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "modloader.h"
#endif

f32 pdmodeGetEnemyReactionSpeed(void)
{
#ifndef PLATFORM_N64
	// GE Plus's 007 is this mode, and GoldenEye's reaction slider is the one
	// Perfect Dark dropped: get_007_reaction_speed() speeds up every guard's
	// animations and reactions (chrlvGetGuard007SpeedRating(), the same sum
	// as chrGetRangedSpeed()). Perfect Dark's own PD Mode never sets it.
	if (g_MissionConfig.pdmode && modloaderStageIsRemake(g_Vars.stagenum)) {
		return g_MissionConfig.pdmodereactionf;
	}
#endif

	return 0;
}

f32 pdmodeGetEnemyHealth(void)
{
	if (g_MissionConfig.pdmode) {
		return g_MissionConfig.pdmodehealthf;
	}

	return 1;
}

f32 pdmodeGetEnemyDamage(void)
{
	if (g_MissionConfig.pdmode) {
		return g_MissionConfig.pdmodedamagef;
	}

	return 1;
}

f32 pdmodeGetEnemyAccuracy(void)
{
	if (g_MissionConfig.pdmode) {
		return g_MissionConfig.pdmodeaccuracyf;
	}

	return 1;
}

void func0f01b148(u32 arg0)
{
	var800624e0 = arg0;
}

void titleSetNextStage(s32 stagenum)
{
	g_TitleNextStage = stagenum;
}
