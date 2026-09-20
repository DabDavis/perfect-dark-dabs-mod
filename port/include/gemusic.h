#ifndef _IN_GEMUSIC_H
#define _IN_GEMUSIC_H

#include <PR/ultratypes.h>

/**
 * GoldenEye's own music, out of the player's ROM (port/src/gemusic.c).
 *
 * The conversion writes GoldenEye's instrument bank and its sequences as they
 * stand (menu/instrumentsctl, instrumentstbl, sequences), how loud each one
 * plays (menu/musicvolumes.bin), and on each level's line of the maps and
 * missions blocks the row `music_setup_entries` gives it. A sequence is
 * appended after the game's own the first time it is asked for. A number is
 * GoldenEye's own MUSIC_TRACKS (bondconstants.h).
 */
#define GEMUSIC_INTRO     2
#define GEMUSIC_FOLDERS   23
#define GEMUSIC_WATCH     24
#define GEMUSIC_DEATHSOLO 27
#define GEMUSIC_MPDEATH   58

#define GEMUSIC_MAIN       0
#define GEMUSIC_BACKGROUND 1
#define GEMUSIC_X          2

// what geMusicStageTrack() answers for a stage whose music is not GoldenEye's
#define GEMUSIC_NOTOURS (-2)

// This game's sequence number for GoldenEye's, or -1: no conversion, or no
// sound at all (--no-sound).
s32 geMusicSequence(s32 geseq);

// A stage's main theme, background or X theme as this game's sequence number,
// -1 where GoldenEye gives the level none, and GEMUSIC_NOTOURS for a stage that
// is not one of the remake's, or an arena the player has picked the music of.
// A level with no theme of its own draws one of GoldenEye's `random_tracks`,
// and keeps it until the next level starts.
s32 geMusicStageTrack(s32 stagenum, s32 which);

// Whether the music playing is a remake level's, so that what plays over it -
// the watch, a death - is GoldenEye's too
s32 geMusicIsStage(s32 stagenum);

// A level is starting: the next one with no theme of its own draws again
void geMusicStageReset(void);

#endif
