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

#ifdef __cplusplus
}
#endif

#endif
