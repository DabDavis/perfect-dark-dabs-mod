#ifndef _IN_GAME_MODRULES_H
#define _IN_GAME_MODRULES_H

#include <PR/ultratypes.h>

/**
 * The rules and colours a console mod's code changes that the port carries as
 * settings (CLAUDE-notes/lua.md, "The tail"): each has the stock value as its
 * default, is set by a modconfig block or the pd call of the same name, and
 * is put back by modRulesReset() when a mod is swapped out.
 */

// Combat Simulator fast movement multiplies the walk speed by this (stock
// 1.25), and so does the mission cheat below when one is named
extern f32 g_ModFastMoveScale;
extern s32 g_ModFastMoveCheat;    // the cheat that gives fast movement in a mission, -1 for none

// the cheat that gives slow motion in a mission (stock CHEAT_SLOMO), -1 for none
extern s32 g_ModSlowMotionCheat;

// the poison a hit gives: ticks added to a chr's counter in a match, and set
// in a mission; 0 for none (GE-X has no poison)
extern s32 g_ModPoisonMatch;
extern s32 g_ModPoisonMission;

// King of the Hill: the hill's colour when a match starts, and when no team holds it
extern f32 g_ModKohHillColour[3];
extern f32 g_ModKohFreeColour[3];

// colour constants the game draws with, 0xRRGGBBAA, named in mod.c
enum {
	MODCOLOUR_KOHHUD,       // the hill timer's text
	MODCOLOUR_TIMER,        // the countdown timer's digits
	MODCOLOUR_SCANNERIN0,   // the horizon scanner's lens, even lines
	MODCOLOUR_SCANNERIN1,   // and odd
	MODCOLOUR_SCANNEROUT0,  // outside the lens, even
	MODCOLOUR_SCANNEROUT1,  // and odd
	MODCOLOUR_JOINTEXT,     // the menu's "press start" for a joining player
	MODCOLOUR_JOINBLEND,    // and the colour it blends from
	MODCOLOUR_INTERLACE0,   // the Slayer rocket view's two line colours
	MODCOLOUR_INTERLACE1,
	MODCOLOUR_NUM
};
extern u32 g_ModColours[MODCOLOUR_NUM];

void modRulesReset(void);

#endif
