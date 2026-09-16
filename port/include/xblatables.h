#ifndef _IN_XBLATABLES_H
#define _IN_XBLATABLES_H

#include <PR/ultratypes.h>

/**
 * The game's tables as the XBLA release has them, while the release is on.
 *
 * 4J changed a handful of the game's own tables. Three of them are taken:
 * three smoke types are retuned, Crash Site's fog reaches twice as far, and
 * heads and bodies gained three types of their own (Joanna 6, Carrington 7,
 * Trent 8). The values are read out of the release's image by
 * tools/xblaxex/gentables.py into xblatablesdata.h.
 *
 * Two are deliberately not taken. 4J ran every explosion about a quarter as
 * long with its flare three tenths as fast, and retuned the first spark type;
 * both were tried and players preferred the N64's durations, so explosions and
 * sparks keep the game's own timings with the release on. The release's 48
 * frame explosion is unaffected either way: xblaexpl.c maps it onto the game's
 * fifteen frames end to end, whatever the type's duration.
 *
 * What is taken follows xblaSwitchGetEnabled(): on with the whole release, the
 * N64's again the moment any part of it is off. A field is only moved from the
 * value the other mode expects, so one a mod has set is left alone.
 */

// Each frame: puts the tables right if the release was switched since.
void xblaTablesTick(void);

// Whether the tables hold the release's values now.
s32 xblaTablesGetApplied(void);

#endif
