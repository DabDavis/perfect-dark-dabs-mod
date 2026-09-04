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
};

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
