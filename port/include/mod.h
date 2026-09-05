#ifndef _IN_MOD_H
#define _IN_MOD_H

#include <PR/ultratypes.h>

#define MOD_CONFIG_FNAME "modconfig.txt"

struct animtableentry;

s32 modConfigLoad(const char *path);

// A mod's ROM data segment, and where its tables are in it. Filled in from a
// modconfig `datasegment` block, which tools/importmod writes.
struct moddataspec {
	char file[256];      // the inflated segment
	char names[256];     // the mod's file names, one per id (optional)
	u32 base;            // where the segment loads
	u32 weapons;         // g_Weapons[] and its length
	s32 numweapons;
	u32 modelstates;     // g_ModelStates[]
	s32 nummodelstates;
	u32 mpweapons;       // g_MpWeapons[]
	s32 nummpweapons;
	u32 mpweaponsets;    // g_MpWeaponSets[]
	s32 nummpweaponsets;
	u32 mparenas;        // g_MpArenas[]
	s32 nummparenas;
	u32 headsandbodies;  // g_HeadsAndBodies[]
	s32 numheadsandbodies;
	u32 mpheads;         // g_MpHeads[]
	s32 nummpheads;
	u32 mpbodies;        // g_MpBodies[]
	s32 nummpbodies;
	u32 botheads;        // g_BotHeads[]
	s32 numbotheads;
	u32 mpbeauheads;     // g_MpBeauHeads[]
	s32 nummpbeauheads;
	u32 mpmaleheads;     // g_MpMaleHeads[]
	s32 nummpmaleheads;
	u32 mpfemaleheads;   // g_MpFemaleHeads[]
	s32 nummpfemaleheads;
	u32 stages;          // g_Stages[], ROM layout (0x38 bytes an entry)
	s32 numstages;
	s32 playerbody;      // the solo player's body and head, -1 for the port's own
	s32 playerhead;
	// the outfit chooser's body/head constants the mod changed: stock -> mod
	s32 numplayerconsts;
	u16 playerconsts[64][2];
	u32 commandlengths;  // g_CommandLengths[], for the mod's own AI commands
	s32 numcommandlengths;
	u32 solostages;      // g_SoloStages[], the mission list
	s32 numsolostages;
};

// The solo player's body and head as the mod's code has them, or def.
s32 modDataPlayerBody(s32 def);
s32 modDataPlayerHead(s32 def);

// Rebuild the weapon definitions and model tables from that segment. Once per
// run: what it allocates is never given back.
s32 modDataImport(const struct moddataspec *spec);

s32 modTextureLoad(u16 num, void *dst, u32 dstSize);

s32 modAnimationLoadDescriptor(u16 num, struct animtableentry *anim);
void *modAnimationLoadData(u16 num);

void *modSequenceLoad(u16 num, u32 *outSize);

// The list of installed mods, and the one the player picked. A mod is mounted
// at startup and cannot be swapped while the game runs, so setting a new one
// only takes effect on the next start.
void modListRefresh(void);
s32 modListGetCount(void);
const char *modListGetName(s32 index);
s32 modListGetSelected(void);
void modListSetSelected(s32 index);
const char *modListGetSelectedName(void);
const char *modListGetLoadedName(void);
void modListApplySelection(void);

// Whether this mod can be switched to without restarting, and doing it. A mod
// that replaces ROM segments cannot: they are read once at boot into memory
// that is never given back.
s32 modListIsFromArgs(void);
s32 modListSwapIsLive(s32 index);
s32 modListSwap(s32 index);

#endif
