#ifndef _IN_GEGUNS_H
#define _IN_GEGUNS_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GoldenEye's guns as weapons past Perfect Dark's table (WEAPON_GE_FIRST):
 * copies of their hosts (g_GeWeaponHosts) under GoldenEye's names, made at
 * start-up. gebean.c shows their Combat Simulator rows and draws the GoldenEye
 * XBLA release's pickups on them; geguns.c has the rest.
 */

/** The stock model state the host of GoldenEye gun index (0 is WEAPON_GE_FIRST) is picked up as. */
s32 gegunsHostModel(s32 index);

struct weapon;

// GoldenEye X's definition of gun index, borrowed (modborrow.c), or NULL to put
// the copy of the host back; pickupfile 0 keeps the host's pickup.
void gegunsBorrow(s32 index, const struct weapon *def, u16 pickupfile, u16 pickupscale);
s32 gegunsIsBorrowed(s32 index);

// GoldenEye's own SFX_ID for one of its guns' shots, to be played on a
// converted level (gesfx.c); 0 for any other weapon or a silent one.
s32 gegunsShootSound(s32 weaponnum);
s32 gegunsBorrowedPickup(s32 index, u16 *fileid, u16 *scale);
// WEAPONFLAG_HASHANDS as the gun's own definition has it
u32 gegunsHandsFlag(s32 index);
u16 gegunsModelFile(s32 index);

#ifdef __cplusplus
}
#endif

#endif
