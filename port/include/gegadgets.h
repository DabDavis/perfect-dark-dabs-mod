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

s32 gegadgetsIsGadget(s32 weaponnum);
// GoldenEye's ITEM_IDS for the weapon on the mission loaded, 0 for none
s32 gegadgetsItem(s32 weaponnum);
void gegadgetsStageLoad(s32 stagenum);
void gegadgetsTick(void);
// the trigger pulled with one in the hand
void gegadgetsFire(s32 weaponnum);
// lvRender(), after the player's props: the camera's photograph is judged here
void gegadgetsAfterProps(void);
// bgunRender(): 1 when the hand has been drawn (or is empty, as GoldenEye has
// it) and the host's model and hand are to be left out
s32 gegadgetsRenderHand(struct modelrenderdata *renderdata, struct model *hostmodel, s32 weaponnum);
// a weapon prop picked up and kept by the game because it is tagged, and a
// gadget thrown from the hand
struct prop;
void gegadgetsKept(struct prop *prop);
void gegadgetsThrown(s32 weaponnum);
// the thrown prop's model state, -1 for the host's own
s32 gegadgetsPropModel(s32 weaponnum);

#endif
