#ifndef _IN_STATEHASH_H
#define _IN_STATEHASH_H

#include <PR/ultratypes.h>

// --state-hash N: every N level frames, log one line hashing the simulation's
// state, so two binaries (or two builds of one) can be checked for playing the
// identical game. Read-only: nothing here may touch what it hashes.
extern s32 g_StateHashEvery;

u64 stateHashCompute(void);
void stateHashTick(void);

#endif
