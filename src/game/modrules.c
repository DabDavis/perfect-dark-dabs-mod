#include <ultra64.h>
#include "constants.h"
#include "game/modrules.h"

// The stock values, which modRulesReset() puts back (game/modrules.h)

f32 g_ModFastMoveScale = 1.25f;
s32 g_ModFastMoveCheat = -1;
s32 g_ModSlowMotionCheat = CHEAT_SLOMO;
s32 g_ModPoisonMatch = 3360;
s32 g_ModPoisonMission = 1680;
f32 g_ModKohHillColour[3] = { 0.25f, 1.0f, 0.25f };
f32 g_ModKohFreeColour[3] = { 0.25f, 1.0f, 0.25f };

static const u32 g_ModColoursStock[MODCOLOUR_NUM] = {
	[MODCOLOUR_KOHHUD]      = 0x00ff00a0,
	[MODCOLOUR_TIMER]       = 0x00ff00a0,
	[MODCOLOUR_SCANNERIN0]  = 0x00ffffff,
	[MODCOLOUR_SCANNERIN1]  = 0x7fffffff,
	[MODCOLOUR_SCANNEROUT0] = 0x007f7fff,
	[MODCOLOUR_SCANNEROUT1] = 0x7fffffff,
	[MODCOLOUR_JOINTEXT]    = 0x5070ff00,
	[MODCOLOUR_JOINBLEND]   = 0x00ffff00,
	[MODCOLOUR_INTERLACE0]  = 0xffff00ff,
	[MODCOLOUR_INTERLACE1]  = 0xffffbfff,
};

u32 g_ModColours[MODCOLOUR_NUM] = {
	[MODCOLOUR_KOHHUD]      = 0x00ff00a0,
	[MODCOLOUR_TIMER]       = 0x00ff00a0,
	[MODCOLOUR_SCANNERIN0]  = 0x00ffffff,
	[MODCOLOUR_SCANNERIN1]  = 0x7fffffff,
	[MODCOLOUR_SCANNEROUT0] = 0x007f7fff,
	[MODCOLOUR_SCANNEROUT1] = 0x7fffffff,
	[MODCOLOUR_JOINTEXT]    = 0x5070ff00,
	[MODCOLOUR_JOINBLEND]   = 0x00ffff00,
	[MODCOLOUR_INTERLACE0]  = 0xffff00ff,
	[MODCOLOUR_INTERLACE1]  = 0xffffbfff,
};

void modRulesReset(void)
{
	s32 i;

	g_ModFastMoveScale = 1.25f;
	g_ModFastMoveCheat = -1;
	g_ModSlowMotionCheat = CHEAT_SLOMO;
	g_ModPoisonMatch = 3360;
	g_ModPoisonMission = 1680;

	for (i = 0; i < 3; i++) {
		g_ModKohHillColour[i] = i == 1 ? 1.0f : 0.25f;
		g_ModKohFreeColour[i] = i == 1 ? 1.0f : 0.25f;
	}

	for (i = 0; i < MODCOLOUR_NUM; i++) {
		g_ModColours[i] = g_ModColoursStock[i];
	}
}
