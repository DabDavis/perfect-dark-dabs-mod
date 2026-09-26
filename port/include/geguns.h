#ifndef _IN_GEGUNS_H
#define _IN_GEGUNS_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GoldenEye's guns as weapons past Perfect Dark's table (WEAPON_GE_FIRST):
 * built at start-up from GoldenEye's own rows, drawn on their hosts' models
 * and run by their hosts' engine (g_GeWeaponHosts, gegunsBuild()). gebean.c shows their Combat Simulator rows and draws the GoldenEye
 * XBLA release's pickups on them; geguns.c has the rest.
 */

/** The stock model state the host of GoldenEye gun index (0 is WEAPON_GE_FIRST) is picked up as. */
s32 gegunsHostModel(s32 index);

struct weapon;

// GoldenEye X's definition of gun index, borrowed (modborrow.c), or NULL to put
// the copy of the host back; pickupfile 0 keeps the host's pickup.
void gegunsBorrow(s32 index, const struct weapon *def, u16 pickupfile, u16 pickupscale);
s32 gegunsIsBorrowed(s32 index);

// Every field of every GoldenEye weapon definition to a text file (gegunsdump.c)
void gegunsDump(const char *path);

// GoldenEye's own SFX_ID for one of its guns' shots, to be played on a
// converted level (gesfx.c); 0 for any other weapon or a silent one.
s32 gegunsShootSound(s32 weaponnum);

// The Moonraker's number as GoldenEye's watch laser (1: watchlaser_stats,
// its ammunition and sound) or as the Moonraker (0); gegadgets.c on stage load
void gegunsSetWatchLaser(s32 on);

// GoldenEye's SoundTriggerRate for one of its guns: the sixtieths between one
// start of its shot sound and the next while the trigger is held, 0 for a
// sound with every shot. -1 for any other weapon.
s32 gegunsShootSoundRate(s32 weaponnum);
s32 gegunsBorrowedPickup(s32 index, u16 *fileid, u16 *scale);
// WEAPONFLAG_HASHANDS as the gun's own definition has it
u32 gegunsHandsFlag(s32 index);
u16 gegunsModelFile(s32 index);

// GoldenEye's own hand item number for gun index: what the conversion names
// its first-person model after (files/Igx%03dZ) and what its gitem_structs row
// is at. 0 for a gun GoldenEye has no item for.
s32 gegunsItemNumber(s32 index);

// Whether the conversion has GoldenEye's own first-person model for this gun.
s32 gegunsHasOwnModel(s32 index);

// That model, or 0: the N64 look's gun (gebean.c decides, since only that look
// wants it).
u16 gegunsOwnModel(s32 index);

// First person on GoldenEye's own model: its placement, its switches, its
// muzzle (bondgun.c asks)
void gegunsSetOwnModelInUse(s32 index, s32 inuse);
s32 gegunsOwnModelInUse(s32 weaponnum);
s32 gegunsOwnModelHidden(s32 weaponnum);
// Where GoldenEye holds the gun in front of the eye, and where its host is held
s32 gegunsViewPlacement(s32 index, f32 *own, f32 *host);

// GoldenEye's own held prop for this gun where its own look is drawn and the
// stage has it (MODEL_REMAKE_FIRST + PROP_CHR*), or -1
s32 gegunsOwnPropModel(s32 weaponnum);
// ... and on the floor, from a weapon row whose model is `fallback`
s32 gegunsFloorModel(s32 weaponnum, s32 fallback);
// Gun `index`'s PROP_CHR* number, thrown ones included, or -1
s32 gegunsChrProp(s32 index);
// GoldenEye's own rocket for its launcher where its own model is drawn, or fallback
s32 gegunsOwnRocketModel(s32 weaponnum, s32 fallback);

// What a guard's GoldenEye launcher fires where its own models are drawn:
// GoldenEye's rocket and grenade round; `fallback` otherwise
s32 gegunsChrProjectileModel(s32 weaponnum, s32 fallback);

// The Enemy Rockets cheat on one of GoldenEye's weapons: whether GoldenEye
// swaps it for its rocket launcher, and the rocket launcher's model to hold
s32 gegunsEnemyRocketsSwaps(s32 weaponnum);
s32 gegunsEnemyRocketModel(void);

// Whether drawing one of GoldenEye's weapons makes no sound: its gadgets
s32 gegunsEquipSilent(s32 weaponnum);

// Whether one of GoldenEye's weapons is never a pair, not even under Akimbo:
// the watch's detonator
s32 gegunsNeverPairs(s32 weaponnum);

// How long one of GoldenEye's mines takes, thrown, to arm or (the timed mine)
// to go off, in sixtieths; 0 for any other weapon, which keeps its function's
s32 gegunsThrownFuse60(s32 weaponnum);
void gegunsOwnModelParts(struct hand *hand, struct model *model);
void gegunsOwnModelFlash(struct hand *hand, struct model *model);
struct modelnode *gegunsOwnModelMuzzle(s32 weaponnum, struct modeldef *modeldef, f32 *offset);

// GoldenEye's knife slash on its own model (the N64 look): begun by a melee
// attack, ticked after the hand's states, as the hand's posrotmtx
void gegunsOwnMeleeStart(struct hand *hand, s32 handnum);
void gegunsOwnMeleeTick(struct hand *hand, s32 handnum, f32 lvupdate60);

// GoldenEye's knife throw on its own model, the same way: begun by a throw,
// the draw back and, once the knife has gone, the follow-through
void gegunsOwnThrowStart(struct hand *hand, s32 handnum);
void gegunsOwnThrowTick(struct hand *hand, s32 handnum, f32 lvupdate60);

// Whether a model is the gun in one of the current player's hands and that
// hand has no rocket loaded (gebean.c's fpRound)
s32 gegunsHandIsSpent(const struct model *model);

// One of GoldenEye's first person keyframes (gun.c's Weapon1PTransformKeyframe):
// a position in the camera's space, three angles in radians, the spline's
// tension and the keyframe's length in sixtieths; `last` ends a track
struct geknifekey {
	s32 last;
	f32 pos[3];
	f32 rot[3];
	f32 tension;
	f32 duration;
};

// gunSample1PTransform(): a track `time` sixtieths in, into `mtx` (an Mtxf),
// mirrored for the left hand; 0 once the track has ended
s32 gegunsSampleTrack(const struct geknifekey *keys, f32 time, void *mtx, s32 left);

#ifdef __cplusplus
}
#endif

#endif
