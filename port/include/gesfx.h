#ifndef _IN_GESFX_H
#define _IN_GESFX_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GoldenEye's own sound effects, out of the player's ROM (port/src/gesfx.c).
 *
 * The conversion writes GoldenEye's sfx bank as menu/sfxctl and menu/sfxtbl,
 * and a sound of it is appended after the game's own the first time it is
 * asked for. An id is GoldenEye's own SFX_ID (bondconstants.h), which indexes
 * its bank's one instrument directly.
 */

#define GESFX_GUN_RIFLE7BIG_1 111 // the gun barrel's shot

// This game's sound number for GoldenEye's, or 0: no conversion, a conversion
// from before the bank was written, or no sound at all (--no-sound).
s32 geSfxGet(s32 id);

// Plays it as the menus play theirs, at `volume` of AL_VOL_FULL (-1 for the
// whole of it). 1 when it started.
s32 geSfxPlay(s32 id, s32 volume);

#endif
