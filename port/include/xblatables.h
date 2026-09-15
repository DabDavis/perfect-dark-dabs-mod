#ifndef _IN_XBLATABLES_H
#define _IN_XBLATABLES_H

#include <PR/ultratypes.h>

/**
 * The game's tables as the XBLA release has them, while the release is on.
 *
 * 4J changed a handful of the game's own tables: every explosion runs about a
 * quarter as long with its flare three tenths as fast (what the release's 48
 * frame explosion is timed to), three smoke types and the first spark type
 * were retuned, Crash Site's fog reaches twice as far, and heads and bodies
 * gained three types of their own (Joanna 6, Carrington 7, Trent 8). The
 * values are read out of the release's image by tools/xblaxex/gentables.py
 * into xblatablesdata.h.
 *
 * They follow xblaSwitchGetEnabled(): on with the whole release, the N64's
 * again the moment any part of it is off. A field is only moved from the value
 * the other mode expects, so one a mod has set is left alone.
 */

// Each frame: puts the tables right if the release was switched since.
void xblaTablesTick(void);

// Whether the tables hold the release's values now.
s32 xblaTablesGetApplied(void);

#endif
