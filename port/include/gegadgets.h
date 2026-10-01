#ifndef _IN_GEGADGETS_H
#define _IN_GEGADGETS_H

#include <PR/ultratypes.h>

/**
 * GoldenEye's gadgets in a converted mission (gegadgets.c): the covert modem,
 * plastique and the GoldenEye key, thrown as its mines are; the camera and the
 * watch magnet; and the six it gives no model in the hand, which share
 * WEAPON_GE_GADGETA and WEAPON_GE_GADGETB by the mission.
 */

struct model;
struct modelrenderdata;

/**
 * Where the watch magnet's charges are kept: Perfect Dark's AMMOTYPE_1C, held
 * by nothing of its own (bondgun.c gives it ten at most, the magnet's
 * magazine). The conversion writes GoldenEye's AMMO_WATCH_MAGNET as it.
 */
#define GEGADGET_MAGNET_AMMO 0x1c

s32 gegadgetsIsGadget(s32 weaponnum);
// GoldenEye's ITEM_IDS for the weapon on the mission loaded, 0 for none
s32 gegadgetsItem(s32 weaponnum);
void gegadgetsStageLoad(s32 stagenum);
// a mission's rename of one of the gadgets, as the setup inserts it
struct textoverride;
void gegadgetsTextOverride(struct textoverride *override);
// the trigger pulled with one in the hand
void gegadgetsFire(s32 weaponnum);
// the watch magnet's running time and hum, every tick of the current player's gun
void gegadgetsTick(void);
// lvRender(), after the player's props: the camera's photograph is judged here
void gegadgetsAfterProps(void);
// bgunRender(): 1 when the hand has been drawn (or is empty, as GoldenEye has
// it) and the host's model and hand are to be left out
s32 gegadgetsRenderHand(struct modelrenderdata *renderdata, struct model *hostmodel, s32 weaponnum);
// a weapon prop picked up and kept by the game because it is tagged, and a
// gadget thrown from the hand
struct prop;
void gegadgetsKept(struct prop *prop);
struct weaponobj;
void gegadgetsThrown(s32 weaponnum, struct weaponobj *thrown);
// the thrown prop's model state, -1 for the host's own
s32 gegadgetsPropModel(s32 weaponnum);

// Where the watch laser's beam starts (at the watch), in the camera's space;
// 0 for any other weapon or before the watch is drawn
s32 gegadgetsWatchLaserMuzzle(s32 weaponnum, f32 *campos);

// The intro gave the player this weapon: the watch laser's charge
void gegadgetsIntroWeapon(s32 weaponnum);

// Whether this weapon number is GoldenEye's watch laser on this stage
s32 gegadgetsWatchLaserActive(s32 weaponnum);

#endif
