#ifndef _IN_GEXPLUS_H
#define _IN_GEXPLUS_H

#include <PR/ultratypes.h>

struct chrdata;

/**
 * GE-X Plus, the GoldenEye remake's Combat Simulator: GoldenEye's own
 * multiplayer scenarios, each one of Perfect Dark's scenarios with its options
 * and, for You Only Live Twice, a rule of its own. Only while g_GexPlusMode is on
 * (modloader.h). port/src/gexplus.c.
 */
#define GEXPLUS_NORMAL        0
#define GEXPLUS_YOLT          1 // You Only Live Twice
#define GEXPLUS_FLAGTAG       2 // The Living Daylights
#define GEXPLUS_LICENCETOKILL 3
#define GEXPLUS_GOLDENGUN     4 // The Man with the Golden Gun
#define GEXPLUS_NUMSCENARIOS  5

s32 gexPlusGetScenario(void);
void gexPlusSetScenario(s32 scenario);
const char *gexPlusScenarioName(s32 scenario);

// You Only Live Twice: whether a chr has died its two times and stays down
s32 gexPlusLivesSpent(struct chrdata *chr);
// and whether the match is over, only one chr having a life left
s32 gexPlusMatchOver(void);

// The Man with the Golden Gun: keeps its one Golden Gun in the arena, each frame
void gexPlusTick(void);

#endif
