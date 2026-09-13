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

// one list for both copies below, so the stock values cannot drift apart
#define MODCOLOURS_STOCK { \
	[MODCOLOUR_KOHHUD]         = 0x00ff00a0, \
	[MODCOLOUR_TIMER]          = 0x00ff00a0, \
	[MODCOLOUR_SCANNERIN0]     = 0x00ffffff, \
	[MODCOLOUR_SCANNERIN1]     = 0x7fffffff, \
	[MODCOLOUR_SCANNEROUT0]    = 0x007f7fff, \
	[MODCOLOUR_SCANNEROUT1]    = 0x7fffffff, \
	[MODCOLOUR_JOINTEXT]       = 0x5070ff00, \
	[MODCOLOUR_JOINBLEND]      = 0x00ffff00, \
	[MODCOLOUR_INTERLACE0]     = 0xffff00ff, \
	[MODCOLOUR_INTERLACE1]     = 0xffffbfff, \
	[MODCOLOUR_AMMOFN]         = 0xff000040, \
	[MODCOLOUR_AMMOFNFADE]     = 0xff000040, \
	[MODCOLOUR_AMMOFNSHIFT]    = 16, \
	[MODCOLOUR_AMMOGUNNAME]    = 0x55ffffff, \
	[MODCOLOUR_AMMOFUNC]       = 0xff5555ff, \
	[MODCOLOUR_AMMOFUNCALT]    = 0xffff55ff, \
	[MODCOLOUR_AMMOCLIPBG]     = 0x00300080, \
	[MODCOLOUR_AMMOCLIPFG]     = 0x00ff0040, \
	[MODCOLOUR_AMMOCLIPTEXT]   = 0x00ff00a0, \
	[MODCOLOUR_AMMORESBG]      = 0x00403080, \
	[MODCOLOUR_AMMORESFG]      = 0x00ffc040, \
	[MODCOLOUR_AMMORESTEXT]    = 0x00ffc0a0, \
	[MODCOLOUR_AMPULSE]        = 0xff0000ff, \
	[MODCOLOUR_AMPULSESHIFT]   = 16, \
	[MODCOLOUR_AMHEALTH]       = 0x00c00060, \
	[MODCOLOUR_AMSHIELD]       = 0x00c00060, \
	[MODCOLOUR_MISSIONTIMER]   = 0x00ff0000, \
	[MODCOLOUR_HTBHUD]         = 0x00ff00a0, \
	[MODCOLOUR_PACHUD]         = 0x00ff00a0, \
	[MODCOLOUR_HTMHUD0]        = 0x60000060, \
	[MODCOLOUR_HTMHUD1]        = 0xc00000d0, \
	[MODCOLOUR_RADAR]          = 0x00ff0000, \
	[MODCOLOUR_SCENRADAR]      = 0x00ff0000, \
	[MODCOLOUR_HIGHLIGHTG]     = 0xff, \
	[MODCOLOUR_SCENHIGHLIGHTG] = 0xcd, \
	[MODCOLOUR_SCENHIGHLIGHTB] = 0xff, \
	[MODCOLOUR_KBGRID]         = 0x00ffff7f, \
	[MODCOLOUR_SLIDERLINE]     = 0x0000ffff, \
	[MODCOLOUR_LISTHDR0]       = 0x00006f00, \
	[MODCOLOUR_LISTHDR1]       = 0x00006f00, \
	[MODCOLOUR_LISTHDR2]       = 0x00003f00, \
	[MODCOLOUR_LISTHDR3]       = 0xffffff00, \
	[MODCOLOUR_LISTHDR4]       = 0x00006f00, \
	[MODCOLOUR_LISTHDR5]       = 0x00003f00, \
	[MODCOLOUR_LISTHDR6]       = 0x6f6f6f00, \
	[MODCOLOUR_DROPDOWN0]      = 0x00006f00, \
	[MODCOLOUR_DROPDOWN1]      = 0x00006f00, \
	[MODCOLOUR_DROPDOWN2]      = 0x00003f00, \
	[MODCOLOUR_SLIDERFILL]     = 0x0000ff4f, \
	[MODCOLOUR_SLIDERLEFT]     = 0x0000ffff, \
	[MODCOLOUR_KBFIELD]        = 0x0000ff7f, \
	[MODCOLOUR_KBCURSOR]       = 0x0000ffff, \
	[MODCOLOUR_RADARBG]        = 0x00ff0000, \
}

static const u32 g_ModColoursStock[MODCOLOUR_NUM] = MODCOLOURS_STOCK;

u32 g_ModColours[MODCOLOUR_NUM] = MODCOLOURS_STOCK;

s32 g_ModWeaponChrModel[MODRULES_NUMWEAPONS] = {
	[0 ... MODRULES_NUMWEAPONS - 1] = MODRULES_STOCKMODEL,
};

s32 g_ModAiCommands[MODAICMD_NUM];

u8 g_ModDialogTypeMap[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

s32 g_ModWeaponSight[MODRULES_NUMWEAPONS] = {
	[0 ... MODRULES_NUMWEAPONS - 1] = -1,
};
s32 g_ModSightMeleeNone = 1;
s32 g_ModSightCheat = SIGHT_CLASSIC;
s32 g_ModSightSplitMin = 2;
s32 g_ModZoomRangeWeapon = WEAPON_SNIPERRIFLE;

void modRulesReset(void)
{
	s32 i;

	for (i = 0; i < MODRULES_NUMWEAPONS; i++) {
		g_ModWeaponSight[i] = -1;
	}

	g_ModSightMeleeNone = 1;
	g_ModSightCheat = SIGHT_CLASSIC;
	g_ModSightSplitMin = 2;
	g_ModZoomRangeWeapon = WEAPON_SNIPERRIFLE;

	for (i = 0; i < MODAICMD_NUM; i++) {
		g_ModAiCommands[i] = 0;
	}

	for (i = 0; i < 8; i++) {
		g_ModDialogTypeMap[i] = i;
	}

	for (i = 0; i < MODRULES_NUMWEAPONS; i++) {
		g_ModWeaponChrModel[i] = MODRULES_STOCKMODEL;
	}

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
