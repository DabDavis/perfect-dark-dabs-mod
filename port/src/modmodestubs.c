// TEMPORARY (feat/pdmods-mode): wave 1's swaps until feat/pdmods-segs and
// feat/pdmods-audio are merged. Deleted at that merge.
#include <PR/ultratypes.h>
#include "mod.h"
#include "modaudio.h"

void modAudioEnter(const char *moddir) { (void)moddir; }
void modAudioLeave(void) {}
s32 modAudioQuiesce(void) { return 1; }
