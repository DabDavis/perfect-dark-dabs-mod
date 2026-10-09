#ifndef _IN_MODMODE_H
#define _IN_MODMODE_H

#include <PR/ultratypes.h>

/**
 * A Perfect Dark mod entered from the Perfect Menu ("Perfect Dark Mods"),
 * the way GoldenEye and its ROM hacks are: no restart, and "Back to Perfect
 * Dark" puts stock back. port/src/modmode.c.
 */

// Whether a mod is entered, and which (its list name and its folder; "" when none)
s32 modModeIsActive(void);
const char *modModeName(void);
const char *modModePath(void);

// Enter PATH (an installed mod's folder, or a "$N/" memory folder a host
// served) under NAME, or leave for stock. Queued: the swap happens at the next
// stage boundary, after the sound has stopped, and the Perfect Menu comes back
// over the reloaded Institute. False when a swap is already under way or the
// mods came from --moddir.
s32 modModeRequestEnter(const char *path, const char *name);
s32 modModeRequestLeave(void);
s32 modModeIsBusy(void);

// Online (netcontent.c): the same swap, at the session's next stage change
// (the host's STAGE_LOAD) with no reload or menu of its own; PATH "" or NULL
// is stock. A later call replaces one still waiting. True when queued or
// already there.
s32 modModeRequestAtNextStage(const char *path, const char *name);
s32 modModeIsPending(void);

// Whether the Perfect Menu may offer the mods at all (--moddir runs and online
// sessions may not)
s32 modModeCanChange(void);

// The Perfect Menu's "Perfect Dark Mods" list
struct menudialogdef;
extern struct menudialogdef g_ModModeMenuDialog;

// Whether the Institute was reloaded for a mod's swap and the Perfect Menu is
// to come back over it (menutick.c), and that it has
s32 modModeWantsMenu(void);
void modModeMenuShown(void);

// lvTick (port side): pumps a queued swap until the sound is quiet
void modModeTick(void);
// mainLoop, between one stage's teardown and the next one's load
void modModeStageBoundary(void);

#endif
