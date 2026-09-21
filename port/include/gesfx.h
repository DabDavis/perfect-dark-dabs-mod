#ifndef _IN_GESFX_H
#define _IN_GESFX_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GoldenEye's own sound effects, out of the player's ROM (port/src/gesfx.c).
 *
 * The conversion writes GoldenEye's sfx bank as menu/sfxctl and menu/sfxtbl,
 * and a sound of it is appended after the game's own the first time it is
 * asked for. An id is GoldenEye's own SFX_ID (bondconstants.h), which is the
 * bank's sound id - 1 (gesfx.c).
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
// and most levels 0x6665). Its music plays here at three quarters of that -
// the folders theme at Perfect Dark's menu scale, 0x4ccc (gemusic.c) - so a
// sound keeps GoldenEye's balance against it at three quarters of full
#define GESFX_VOLUME 0x5fff

// This game's sound number for GoldenEye's, or 0: no conversion, a conversion
// from before the bank was written, or no sound at all (--no-sound).
s32 geSfxGet(s32 id);

// Plays it as the menus play theirs, at `volume` of AL_VOL_FULL (-1 for the
// whole of it). 1 when it started.
s32 geSfxPlay(s32 id, s32 volume);

struct prop;

// The four moments a door makes a sound at, in both games
#define GESFX_DOOR_OPENING 0
#define GESFX_DOOR_CLOSING 1
#define GESFX_DOOR_OPENED  2
#define GESFX_DOOR_CLOSED  3

// A converted level's door keeps GoldenEye's own sound type, and this plays
// GoldenEye's sounds for it from where the door is. 1 when the door is
// GoldenEye's to answer for - whether or not its type has a sound at that
// moment - and 0 when Perfect Dark's table should: a stage that is not a
// conversion, or no sound bank in the conversion.
s32 geSfxDoor(s32 moment, s32 soundtype, struct prop *prop);

// One of Perfect Dark's pickup sounds (SFX_PICKUP_*) as GoldenEye's own on a
// converted level: the body armour, a key, a gun, ammunition. From `prop`, or
// as the player's own with none. 1 when it was played, 0 for Perfect Dark's.
s32 geSfxPickup(s32 pdsound, struct prop *prop);

#endif
