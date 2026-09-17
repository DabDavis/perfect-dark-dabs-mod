/**
 * GE-X Plus's scenarios: GoldenEye's multiplayer modes over Perfect Dark's.
 *
 * GoldenEye's are chosen from the Combat Simulator's scenario list while in
 * GE-X Plus (scenarios.c) and are not a scenario number of their own - the
 * setup's scenario is saved, and its numbers are Perfect Dark's - but a choice
 * of one of Perfect Dark's scenarios and options:
 *
 * - Normal: Combat
 * - You Only Live Twice: Combat, and a chr that has died twice stays down
 *   (player.c and chraction.c ask gexPlusLivesSpent()); the match ends when one
 *   chr has a life left (lv.c asks gexPlusMatchOver())
 * - The Living Daylights (flag tag): Hold the Briefcase, which is the same game
 * - License to Kill: Combat with one-hit kills
 *
 * The Man with the Golden Gun is not here yet: its one gun that does not respawn
 * while it is held has nothing in Perfect Dark to stand on.
 */
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "modloader.h"
#include "gexplus.h"

static s32 g_GexPlusScenario = GEXPLUS_NORMAL;

static const char *const g_GexPlusScenarioNames[GEXPLUS_NUMSCENARIOS] = {
	"Normal",
	"You Only Live Twice",
	"The Living Daylights",
	"License to Kill",
};

s32 gexPlusGetScenario(void)
{
	return g_GexPlusScenario;
}

const char *gexPlusScenarioName(s32 scenario)
{
	return scenario >= 0 && scenario < GEXPLUS_NUMSCENARIOS ? g_GexPlusScenarioNames[scenario] : "";
}

void gexPlusSetScenario(s32 scenario)
{
	if (scenario < 0 || scenario >= GEXPLUS_NUMSCENARIOS) {
		scenario = GEXPLUS_NORMAL;
	}

	g_GexPlusScenario = scenario;
	g_MpSetup.scenario = scenario == GEXPLUS_FLAGTAG ? MPSCENARIO_HOLDTHEBRIEFCASE : MPSCENARIO_COMBAT;

	if (scenario == GEXPLUS_LICENCETOKILL) {
		g_MpSetup.options |= MPOPTION_ONEHITKILLS;
	} else {
		g_MpSetup.options &= ~MPOPTION_ONEHITKILLS;
	}
}

static s32 gexPlusYolt(void)
{
	return g_GexPlusMode && g_GexPlusScenario == GEXPLUS_YOLT && g_Vars.normmplayerisrunning;
}

s32 gexPlusLivesSpent(struct chrdata *chr)
{
	if (!gexPlusYolt() || !chr) {
		return 0;
	}

	for (s32 i = 0; i < g_MpNumChrs; i++) {
		if (g_MpAllChrPtrs[i] == chr) {
			return g_MpAllChrConfigPtrs[i]->numdeaths >= 2;
		}
	}

	return 0;
}

s32 gexPlusMatchOver(void)
{
	s32 alive = 0;

	if (!gexPlusYolt() || g_MpNumChrs < 2) {
		return 0;
	}

	for (s32 i = 0; i < g_MpNumChrs; i++) {
		if (g_MpAllChrConfigPtrs[i]->numdeaths < 2) {
			alive++;
		}
	}

	return alive <= 1;
}
