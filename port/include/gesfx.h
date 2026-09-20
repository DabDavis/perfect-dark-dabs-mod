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

#define GESFX_OPTION_CLICK2     18
#define GESFX_PAPER_TURN        77  // a difficulty picked
#define GESFX_GUN_RIFLE7BIG_1   111 // the gun barrel's shot
#define GESFX_CAMERA_BEEP1      159 // the watch's beep
#define GESFX_DOOR_METAL_CLOSE  197 // the mode select's three choices
#define GESFX_DOOR_METAL_CLOSE2 199 // every other accept and back on the folder
#define GESFX_WATCH_STATIC      236
#define GESFX_WATCH_ON          237
#define GESFX_WATCH_OFF         238

// GoldenEye plays a sound effect at full volume over music at its track's own
// default (g_musicDefaultTrackVolume[]: M_INTRO 0x7332, the folders, the watch
// and the levels 0x6665). Its music plays here at Perfect Dark's menu scale,
// 0x4ccc (SEQ_APPENDED_VOLUME), so a sound keeps GoldenEye's balance against
// it at the same share of full: 0x4ccc / 0x7332 and 0x4ccc / 0x6665
#define GESFX_VOLUME_INTRO 0x5555
#define GESFX_VOLUME       0x5fff

// This game's sound number for GoldenEye's, or 0: no conversion, a conversion
// from before the bank was written, or no sound at all (--no-sound).
s32 geSfxGet(s32 id);

// Plays it as the menus play theirs, at `volume` of AL_VOL_FULL (-1 for the
// whole of it). 1 when it started.
s32 geSfxPlay(s32 id, s32 volume);

#endif
