#ifndef _IN_MOD_H
#define _IN_MOD_H

#include <PR/ultratypes.h>

#define MOD_CONFIG_FNAME "modconfig.txt"

struct animtableentry;

s32 modConfigLoad(const char *path);

// Parse modconfig text from a scratch buffer (the tokeniser writes into it);
// `what` names the source in the log.
s32 modConfigParse(char *data, u32 dataLen, const char *what);

// The settings a modconfig block can make, one function a key, so a key's name
// and range are written once. The
// key names and ranges live here so both say the same thing. Return 1 when
// applied, 0 for a key or flag name the game does not know, -1 for a value
// out of range, -2 for a weapon, function or slot that does not exist.
s32 modWeaponFlagLookup(const char *name, u32 *flag, u32 *word);
const char *modWeaponFlagName(s32 index);        // the names in order, NULL past the end
s32 modWeaponFuncFlagLookup(const char *name, u32 *flag);
const char *modWeaponFuncFlagName(s32 index);
void modWeaponFlagClearAll(u32 flag, u32 word);
s32 modWeaponFlagSet(s32 weaponnum, u32 flag, u32 word, s32 on);
s32 modWeaponFlagGet(s32 weaponnum, u32 flag, u32 word);
void modWeaponFuncFlagClearAll(u32 flag);
s32 modWeaponFuncFlagSet(s32 weaponnum, s32 funcnum, u32 flag, s32 on);
s32 modWeaponFuncFlagGet(s32 weaponnum, s32 funcnum, u32 flag);
s32 modWeaponSetKey(s32 weaponnum, const char *key, s32 value);
s32 modWeaponFuncSetKey(s32 weaponnum, s32 funcnum, const char *key, s32 value);
s32 modUnlockSetKey(const char *key, s32 value);
s32 modDamageSetKey(const char *key, f32 value);
s32 modPickupQtySet(s32 mode, s32 ammotype, s32 qty); // mode 0 mp, 1 solo
s32 modAmmoTypeWeaponSet(s32 ammotype, s32 weaponnum);
s32 modTvScreenSetSameAs(s32 num, s32 src);
s32 modMovementSetKey(const char *key, f32 value);   // fastspeed, fastcheat (-1 none)
s32 modCheatsSetKey(const char *key, s32 value);     // slowmotion: the cheat, -1 none
s32 modKohSetColour(const char *key, const f32 *rgb); // hillcolour, freecolour
s32 modColourLookup(const char *name);               // g_ModColours[] index, -1 unknown
const char *modColourName(s32 index);                // the names in order, NULL past the end
s32 modColourSet(const char *name, u32 rgba);

// A stage's settings, as the `stage` block has them (-2: no such stage or entry)
struct stagetableentry;
struct stageallocation;
struct weathercfg;
s32 modStageLookup(s32 stagenum, struct stagetableentry **stab, struct stageallocation **salloc);
s32 modStageSetKey(s32 stagenum, const char *key, s32 value);            // alarm, extragunmem
s32 modStageSetFile(s32 stagenum, const char *key, const char *nameOrNum); // bgfile, tilesfile, padsfile, setupfile, mpsetupfile
s32 modStageSetAllocation(s32 stagenum, const char *str);
s32 modStageMusicSetKey(s32 stagenum, const char *key, s32 value);       // primarytrack, ambienttrack, xtrack
struct weathercfg *modStageWeatherBegin(s32 stagenum);                    // NULL when the table is full
s32 modStageWeatherSetKey(struct weathercfg *wcfg, const char *key, f32 value); // windspeed, ymin, ymax, zmax, cutscene_only
s32 modStageWeatherSetConstantWind(struct weathercfg *wcfg, f32 anglerad, f32 speedx, f32 speedz);
s32 modStageWeatherSetRooms(struct weathercfg *wcfg, s32 include, s32 clear, const s32 *rooms, s32 count);

// The data segment spec, key by key (-3: the list is full, the entry dropped)
struct moddataspec;
void modDataSpecInit(struct moddataspec *spec);
s32 modDataSpecIsTableKey(const char *key);
s32 modDataSpecIsPairKey(const char *key);   // the pair's width: 2, or 3 with a kind word first
s32 modDataSpecSetTable(struct moddataspec *spec, const char *key, u32 addr, s32 count);
s32 modDataSpecAddPair(struct moddataspec *spec, const char *key, const char *kind, s32 stock, s32 mod);
s32 modDataSpecSetValue(struct moddataspec *spec, const char *key, s32 value);   // base, playerbody, playerhead
s32 modDataSpecSetString(struct moddataspec *spec, const char *key, const char *value); // file, names
s32 modDataSpecApply(const struct moddataspec *spec);

// The shield flash colour rows, checked and set (chr.h)
struct shieldcolour;
s32 modShieldColourSiteLookup(const char *name);   // "hit" or "player", else -1
s32 modShieldColourApply(s32 site, const struct shieldcolour *rows, s32 numrows);

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
	// the solo guards' random heads: g_MaleGuardHeads[] and the team and
	// female lists beside it, -1 terminated in the ROM
	u32 maleguardheads;
	s32 nummaleguardheads;
	u32 maleguardteamheads;
	s32 nummaleguardteamheads;
	u32 femaleguardheads;
	s32 numfemaleguardheads;
	u32 femaleguardteamheads;
	s32 numfemaleguardteamheads;
	u32 stages;          // g_Stages[], ROM layout (0x38 bytes an entry)
	s32 numstages;
	s32 playerbody;      // the solo player's body and head, -1 for the port's own
	s32 playerhead;
	// the outfit chooser's body/head constants the mod changed: stock -> mod
	s32 numplayerconsts;
	u16 playerconsts[64][2];
	// the animated texture numbers texLoadFromGdl() compares against that
	// the mod changed: stock -> mod
	s32 numtexconsts;
	u16 texconsts[16][2];
	// the co-operative buddies' bodies, heads, gun models and guns, constants
	// in playerTick() the mod changed: kind (BUDDYCONST_*), stock, mod
	s32 numbuddyconsts;
	u16 buddyconsts[32][3];
	// the rooms roomPopulateMtx() pins to the camera: room numbers the mod
	// changed (stock -> mod) and the stages, as stock stage index -> the
	// mod's stage id
	s32 numroomnums;
	u16 roomnums[16][2];
	s32 numroomstages;
	u16 roomstages[32][2];
	// the stage ids bgRenderScene() draws its star field for: stock -> mod
	s32 numbgstages;
	u16 bgstages[16][2];
	u32 commandlengths;  // g_CommandLengths[], for the mod's own AI commands
	s32 numcommandlengths;
	u32 solostages;      // g_SoloStages[], the mission list
	s32 numsolostages;
	u32 fogenvs;         // g_FogEnvironments[], ROM layout (44 bytes an entry)
	s32 numfogenvs;
	u32 nofogenvs;       // g_NoFogEnvironments[], ROM layout (56 bytes an entry)
	s32 numnofogenvs;
	u32 stagetracks;     // g_StageTracks[], each stage's music (8 bytes an entry)
	s32 numstagetracks;
	u32 mptracks;        // g_MpTracks[], the Combat Simulator's music (6 bytes an entry)
	s32 nummptracks;
	u32 ammotypes;       // g_AmmoTypes[], capacity and pickup size (12 bytes an entry)
	s32 numammotypes;
	u32 explosiontypes;  // g_PropExplosionTypes[], one s8 a model
	s32 numexplosiontypes;
	u32 autoswitchprimary;   // g_AutoSwitchWeaponsPrimary[], weapon numbers, best first
	s32 numautoswitchprimary;
	u32 autoswitchsecondary; // g_AutoSwitchWeaponsSecondary[]
	s32 numautoswitchsecondary;
	u32 botweaponprefs;  // g_AibotWeaponPreferences[], a simulant's view of each weapon (16 bytes)
	s32 numbotweaponprefs;
	u32 hudmsgtypes;     // g_HudmsgTypes[], ROM layout (32 bytes an entry)
	s32 numhudmsgtypes;
	u32 globalailists;   // g_GlobalAilists[], {list, id} pairs to a NULL list
	s32 numglobalailists;
};

// The solo player's body and head as the mod's code has them, or def.
s32 modDataPlayerBody(s32 def);

// the co-operative buddies' constants in playerTick(), as the mod's code has them
#define BUDDYCONST_BODY   1
#define BUDDYCONST_HEAD   2
#define BUDDYCONST_MODEL  3
#define BUDDYCONST_WEAPON 4
s32 modDataBuddyConst(s32 kind, s32 def);
s32 modDataPlayerHead(s32 def);

// An animated texture number as the mod's texture code has it, or def.
s32 modDataTexNum(s32 def);

// The rooms roomPopulateMtx() pins: a room number as the mod's code has it,
// and the stage id a stock stage index's site compares against.
s32 modDataRoomNum(s32 def);
s32 modDataRoomStage(s32 stockindex, s32 defid);

// A stage id bgRenderScene() tests for its star field, as the mod's code has it
s32 modDataBgStage(s32 def);

// Rebuild the weapon definitions and model tables from that segment. Once per
// run: what it allocates is never given back.
s32 modDataImport(const struct moddataspec *spec);

/**
 * Reads texture num out of the mods, if one of them has it.
 *
 * outstagemod, when given, comes back as the mounted directory index of the
 * running stage's own mod when the texture came from there, and -1 when it came
 * from the overlay or the base directory. A number means something else to that
 * mod, so nothing keyed on the stock numbering may repaint the texels, and its
 * own pack is looked for under that directory - texpackTextureArt().
 */
s32 modTextureLoad(u16 num, void *dst, u32 dstSize, s32 *outstagemod);
s32 modSetTextureFromStage(s32 on);

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

// The Stage Loader: every installed mod's maps as extra Combat Simulator
// arenas, each mod mounted for its maps alone beside the mod loaded (fs.h,
// fsAddMapsDir). Mod.MapMods holds the choice.
s32 modMapsAllEnabled(void);
s32 modMapsIsEnabled(const char *name);
void modMapsSetAll(s32 on);
void modMapsSetEnabled(const char *name, s32 on);
s32 modMapsNumMounted(void);
s32 modMapsPending(void);   // the setting changed and could not be applied where we stand
s32 modMapsApply(void);     // apply it now; false when a restart is needed

#endif
