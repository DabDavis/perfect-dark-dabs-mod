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
	MODCOLOUR_AMMOFN,       // the ammo HUD's function name
	MODCOLOUR_AMMOFNFADE,   // and as it fades, with the fader shifted up by
	MODCOLOUR_AMMOFNSHIFT,  // this many bits (not a colour; stock 16)
	MODCOLOUR_AMMOGUNNAME,  // the gun's name
	MODCOLOUR_AMMOFUNC,     // the function's name
	MODCOLOUR_AMMOFUNCALT,  // and while the other function is being switched to
	MODCOLOUR_AMMOCLIPBG,   // the clip gauge's back, fill and digits
	MODCOLOUR_AMMOCLIPFG,
	MODCOLOUR_AMMOCLIPTEXT,
	MODCOLOUR_AMMORESBG,    // the reserve gauge's back, fill and digits
	MODCOLOUR_AMMORESFG,
	MODCOLOUR_AMMORESTEXT,
	MODCOLOUR_AMPULSE,      // the inventory menu's selected slot, pulsing
	MODCOLOUR_AMPULSESHIFT, // by the pulse shifted up by this many bits (not a colour; stock 16)
	MODCOLOUR_AMHEALTH,     // the inventory menu's health bar
	MODCOLOUR_AMSHIELD,     // and shield bar
	MODCOLOUR_MISSIONTIMER, // the mission timer's text
	MODCOLOUR_HTBHUD,       // Hold the Briefcase's timer
	MODCOLOUR_PACHUD,       // Pop a Cap's timer
	MODCOLOUR_HTMHUD0,      // Hack That Mac's progress bar, back
	MODCOLOUR_HTMHUD1,      // and front
	MODCOLOUR_RADAR,        // the radar's dots
	MODCOLOUR_SCENRADAR,    // a scenario's objective on the radar
	MODCOLOUR_HIGHLIGHTG,   // a scenario's highlighted prop, green channel (not a colour)
	MODCOLOUR_SCENHIGHLIGHTG, // a highlighted pickup, green and blue channels (not colours)
	MODCOLOUR_SCENHIGHLIGHTB,
	MODCOLOUR_KBGRID,       // the name keyboard's grid lines
	MODCOLOUR_SLIDERLINE,   // a slider's line past the marker
	MODCOLOUR_LISTHDR0,     // a list group header's seven vertex colours (0-2 and 6 take the alpha)
	MODCOLOUR_LISTHDR1,
	MODCOLOUR_LISTHDR2,
	MODCOLOUR_LISTHDR3,
	MODCOLOUR_LISTHDR4,
	MODCOLOUR_LISTHDR5,
	MODCOLOUR_LISTHDR6,
	MODCOLOUR_DROPDOWN0,    // a dropdown's background, three vertex colours
	MODCOLOUR_DROPDOWN1,
	MODCOLOUR_DROPDOWN2,
	MODCOLOUR_SLIDERFILL,   // a slider's fill
	MODCOLOUR_SLIDERLEFT,   // a slider's line before the marker, where it starts
	MODCOLOUR_KBFIELD,      // the name keyboard's input field
	MODCOLOUR_KBCURSOR,     // and the colour its cursor blends from
	MODCOLOUR_RADARBG,      // the radar's background, 0xRRGGBB00
	MODCOLOUR_NUM
};
extern u32 g_ModColours[MODCOLOUR_NUM];

// The model a chr holds for a weapon - in third person, dropped on death, in a
// simulant's hand - by weapon number, or MODRULES_STOCKMODEL for the port's
// own switch in playermgrGetModelOfWeapon() (a mod's `weapon N { chrmodel M }`)
// A mod's own AI commands that go to their label on one of the game's own
// options, by command type (0 for none): GE-X's 0xe6 when the Language Filter
// - its "Additional Dialogue" - is off, and its 0xe7 when the Alternative
// Title Screen - its "Disable Female NPCs" - is on (aicommands { }, read from
// the mod's handlers by the importer; chraiExecute() runs them)
enum {
	MODAICMD_IFLANGFILTERON,
	MODAICMD_IFLANGFILTEROFF,
	MODAICMD_IFALTTITLEON,
	MODAICMD_IFALTTITLEOFF,
	MODAICMD_NUM
};
extern s32 g_ModAiCommands[MODAICMD_NUM];

// The type a menu dialog is drawn as, by the type its definition has
// (MENUDIALOGTYPE_*; identity unless a mod's menudialogtypes { }): GE-X turns
// its ordinary dialogs into success dialogs and its success dialogs into
// ordinary ones, which is what makes its menus green
extern u8 g_ModDialogTypeMap[8];

#define MODRULES_NUMWEAPONS 256
#define MODRULES_STOCKMODEL (-2)
extern s32 g_ModWeaponChrModel[MODRULES_NUMWEAPONS];

// The sight a weapon draws (SIGHT_*), or -1 for the port's own switch in
// currentPlayerGetSight() (a mod's `weapon N { sight S }`); GE-X gives nearly
// every gun the classic sight. And the rules around it (sights { }): whether
// a melee function hides the sight (stock 1), what the Classic Sight cheat
// gives (stock SIGHT_CLASSIC; GE-X turns it off), the player count at which a
// split screen forces the default sight (stock 2; GE-X 5, never), and the
// weapon that shows the zoom range with no zoom (stock the sniper rifle)
extern s32 g_ModWeaponSight[MODRULES_NUMWEAPONS];
extern s32 g_ModSightMeleeNone;
extern s32 g_ModSightCheat;
extern s32 g_ModSightSplitMin;
extern s32 g_ModZoomRangeWeapon;

// A head number the game tests literally (the Maian and Joanna grunts, the
// Maian eyes, the quips), as the mod's code has it: GE-X gives three of the
// four Maian heads to humans and tests head 5 alone (and head 4 alone for
// Joanna's), and read through the stock numbers its guards grunted like
// Elvis. The `headconst` lines of a datasegment block.
// And the Combat Simulator bodies players 2 to 4 start as (`mpbodyconst`):
// GE-X's own list numbers them differently, so stock's defaults were Baron
// Samedi, May Day and a Russian Commandant.
#ifdef PLATFORM_N64
#define MOD_HEADNUM(x) (x)
#define MOD_MPBODY(x) (x)
#else
s32 modDataHeadNum(s32 def);
s32 modDataMpBodyConst(s32 def);
#define MOD_HEADNUM(x) modDataHeadNum(x)
#define MOD_MPBODY(x) modDataMpBodyConst(x)
#endif

void modRulesReset(void);

#endif
