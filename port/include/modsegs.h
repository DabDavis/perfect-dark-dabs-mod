#ifndef _IN_MODSEGS_H
#define _IN_MODSEGS_H

#include <PR/ultratypes.h>

/*
 * A PD mod's segments swapped in and out between stages (modsegs.c): the
 * animations, the texture list and data, the fonts, the Japanese fonts, the
 * multiplayer configs and strings and the firing range - every one a mod can
 * ship but the sound's (modaudio.c) and the copyright (a boot screen).
 *
 * Only between stages: the old stage torn down, the next lvReset() not yet
 * run (its texReset(), textReset() and animsReset() rebuild what is per
 * stage from the segments as they are then). Neither reloads a stage.
 */

// Every non-audio segment moddir/segs/ holds becomes current; one it does
// not hold is (or goes back to) the ROM's
void modSegsEnter(const char *moddir);
// Every non-audio segment back to the ROM's
void modSegsLeave(void);

// texLoad(): whether texture num as the segments hold it is the ROM's own
// art (the same bytes under the same number), for the texture pack registry
s32 modSegsTexArtIsRom(s32 num);

// A text dump of everything modsegs.c swaps or rebuilds, for comparing two
// runs (--mod-dump-segs FILE; callable from gdb). 0 on success.
s32 modSegsDump(const char *path);

// lvReset(): --mod-segs-enter DIR (repeatable, "-" = leave) at the stage
// load --mod-segs-at N (0, the first, by default), then --mod-dump-segs
void modSegsTestHook(s32 stagenum);

#endif
