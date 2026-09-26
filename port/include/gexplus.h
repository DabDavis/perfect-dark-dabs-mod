#ifndef _IN_GEXPLUS_H
#define _IN_GEXPLUS_H

#include <PR/ultratypes.h>

struct chrdata;
struct headorbody;

/**
 * GE Plus, the GoldenEye remake's Combat Simulator: GoldenEye's own
 * multiplayer scenarios, each one of Perfect Dark's scenarios with its options
 * and, for You Only Live Twice, a rule of its own. Only while g_GexPlusMode is on
 * (modloader.h). port/src/gexplus.c.
 */
#define GEXPLUS_NORMAL        0
#define GEXPLUS_YOLT          1 // You Only Live Twice
#define GEXPLUS_FLAGTAG       2 // The Living Daylights
#define GEXPLUS_LICENCETOKILL 3
#define GEXPLUS_GOLDENGUN     4 // The Man with the Golden Gun
#define GEXPLUS_NUMSCENARIOS  5

s32 gexPlusGetScenario(void);
void gexPlusSetScenario(s32 scenario);
const char *gexPlusScenarioName(s32 scenario);

// You Only Live Twice: whether a chr has died its two times and stays down
s32 gexPlusLivesSpent(struct chrdata *chr);
// and whether the match is over, only one chr having a life left
s32 gexPlusMatchOver(void);

// The Man with the Golden Gun: keeps its one Golden Gun in the arena, each frame
void gexPlusTick(void);

// Simulants wear GoldenEye X's borrowed GoldenEye characters, at match start
void gexPlusThemeSimulants(void);


// A converted GoldenEye mission's chr, before Perfect Dark makes it: its body
// is GoldenEye's own character number, which becomes whatever the player has
// installed to wear - the XBLA release's characters, GoldenEye X's, or Perfect
// Dark's own.
// A converted GoldenEye mission's props, once and before anything reads them:
// a chr's body is GoldenEye's own character number and a weapon's model is the
// pickup prop it was converted with, and both become what the player has
// installed.
void gexPlusMissionSetup(u32 *props);

// and the bodies its own lists spawn, once the ailists have been pointed at
// themselves
void gexPlusMissionAilists(void);

// The heads a converted mission's guards wear: GoldenEye's own four, into the
// game's own active lists, from the end of bodiesReset()
void gexPlusMissionHeads(void);

// The head a converted mission's body wears where its record named one rather
// than taking GoldenEye's pool, or -1. bodyChooseHead() asks.
s32 gexPlusRomOwnHead(s32 bodynum);
// Bond as GoldenEye dresses him for this mission's outfit; false off a mission
s32 gexPlusMissionBond(s32 outfit, s32 *bodynum, s32 *headnum);

// Whether a row of g_HeadsAndBodies is one of GoldenEye's own characters, in a
// mission: headfit.c leaves a pair GoldenEye made for itself alone
s32 gexPlusRomIsPoolRow(s32 num);

// GoldenEye's own character number behind a row of g_HeadsAndBodies, or -1 for
// a row that is not one of its characters. The watch asks, to dress its arm in
// the sleeve the player's own character is wearing (gewatch.c).
s32 gexPlusRomChrForRow(s32 row);

// GoldenEye's own characters for the Combat Simulator's lists when there is no
// XBLA release to take them from (gebean.c's pool): Begin finds the ROM's
// conversion and reads its table, answering how many characters it has (0 for
// none); Fill makes `hb` GoldenEye's character `num` (its own c_item_entries
// number) on its converted model and answers the model's file, or 0
s32 gexPlusRomMpBegin(void);
s32 gexPlusRomMpFill(s32 num, struct headorbody *hb);

// A converted GoldenEye mission's own text bank - its objectives and its radio
// messages - loaded out of the mod's menu/ into LANGBANK_GEMISSION. Cleared
// when the stage is not one of the missions.
void gexPlusMissionLangLoad(s32 stagenum);

// The animations a converted mission's PlayAnimation commands name, appended
// after the game's own, and what GoldenEye's own animation id is once they are
void gexPlusMissionAnimLoad(s32 stagenum);
s32 gexPlusMissionAnim(s32 geid);

// GoldenEye's own end of a mission (its TriggerFadeAndExitLevelOnButtonPress):
// the list says the level is over, and the next button press fades the screen
// out and leaves. The tick runs every frame of a level, from lvTick().
void gexPlusExitOnButtonPress(void);
void gexPlusMissionExitTick(void);

/**
 * GE Plus's guns are GoldenEye's: the weapon sets its arenas list (GoldenEye's
 * own fourteen out of the ROM, else GoldenEye X's borrowed ones; 0 for the
 * whole list), and Mod.GePlusPdGuns, which lists Perfect Dark's beside them.
 */
s32 gexPlusWeaponSets(s32 *first);
void gexPlusWeaponSetsAppend(void); // GoldenEye's own sets in the whole list, at boot and after a swap
s32 gexPlusGetPdGuns(void);
void gexPlusSetPdGuns(s32 on);

/** The explosion a converted GoldenEye prop makes when destroyed (GoldenEye's own table), or -1. */
s32 gexPlusPropExplosionType(s32 modelnum);

#endif
