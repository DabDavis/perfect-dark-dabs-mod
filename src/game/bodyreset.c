#include <ultra64.h>
#include "constants.h"
#include "game/body.h"
#include "game/cheats.h"
#include "game/chrai.h"
#include "game/game_00b820.h"
#include "game/playerreset.h"
#include "game/setuputils.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "gexplus.h"
#include "xblaagent4.h"
#include "xblamesh.h"
#include "files.h"

/**
 * The male guard heads as the XBLA release deals them. 4J remodelled Penny's
 * head as a woman (the release's table makes her head type female) and took
 * her out of both male guard lists: the full list drops her, the team list
 * has Ben R in her place (read from the release's image). With the release's
 * meshes on, her head is that woman, so a male guard dealt her comes out a
 * woman's face on a man's body. Only while her row is still the stock file,
 * so a mod's own table is left alone.
 */
static s32 bodyReleaseMaleHeads(s32 *list, s32 len, bool team, s32 **out)
{
	static s32 heads[256];
	s32 n = 0;
	s32 i;

	*out = list;

	if (!xblaMeshGetEnabled() || g_HeadsAndBodies[HEAD_PENNY].filenum != FILE_CHEADPENNY
			|| len <= 1 || len > (s32)ARRAYCOUNT(heads)) {
		return len;
	}

	for (i = 0; i < len; i++) {
		if (list[i] != HEAD_PENNY) {
			heads[n++] = list[i];
		} else if (team) {
			heads[n++] = HEAD_BEN_R;
		}
	}

	if (n == len && !team) {
		return len;
	}

	*out = heads;

	return n;
}
#endif

void bodiesReset(s32 stagenum)
{
	s32 *headsavailablelist;
	s32 headsavailablelen;
	bool done;
	s32 i;
	s32 j;
	s32 whichteamlist = 1;
	s32 index;

#ifdef PLATFORM_N64
	for (i = 0; g_HeadsAndBodies[i].filenum != 0; i++) {
#else
	// Every row: the GoldenEye characters' are past the stock terminator
	for (i = 0; i < ARRAYCOUNT(g_HeadsAndBodies); i++) {
#endif
		g_HeadsAndBodies[i].modeldef = NULL;
	}

#ifndef PLATFORM_N64
	xblaAgent4StageReset();
#endif

	var80062c80 = rngRandom() % g_NumBondBodies;
	var80062b14 = 0;
	var80062b18 = 0;

	if (PLAYERCOUNT() >= 2) {
		g_NumActiveHeadsPerGender = 4;
	} else {
		s32 len = 3;

		static u8 overrides[3][2] = {
			{ STAGE_INFILTRATION, 5 },
			{ STAGE_RESCUE,       4 },
			{ STAGE_ESCAPE,       5 },
		};

		g_NumActiveHeadsPerGender = 8;

		for (i = 0; i < len; i++) {
			if (overrides[i][0] == stagenum) {
				g_NumActiveHeadsPerGender = overrides[i][1];
			}
		}
	}

	// Male heads
	if (cheatIsActive(CHEAT_TEAMHEADSONLY)) {
		if (whichteamlist) {
			headsavailablelist = g_MaleGuardTeamHeads;
			headsavailablelen = g_NumMaleGuardTeamHeads;
		} else {
			headsavailablelist = g_MaleGuardTeamHeads;
			headsavailablelen = var80062b14;
		}
	} else {
		headsavailablelist = g_MaleGuardHeads;
		headsavailablelen = g_NumMaleGuardHeads;
	}

#ifndef PLATFORM_N64
	headsavailablelen = bodyReleaseMaleHeads(headsavailablelist, headsavailablelen,
			headsavailablelist == g_MaleGuardTeamHeads, &headsavailablelist);
#endif

	for (i = 0; i < g_NumActiveHeadsPerGender; i++) {
		do {
			done = true;
			g_ActiveMaleHeads[i] = headsavailablelist[rngRandom() % headsavailablelen];

			if (headsavailablelen > g_NumActiveHeadsPerGender) {
				for (j = 0; j < i; j++) {
					if (g_ActiveMaleHeads[j] == g_ActiveMaleHeads[i]) {
						done = false;
					}
				}
				if (j && j && j);
			}
		} while (!done);
	}

	// Female heads
	if (cheatIsActive(CHEAT_TEAMHEADSONLY)) {
		if (whichteamlist) {
			headsavailablelist = g_FemaleGuardTeamHeads;
			headsavailablelen = g_NumFemaleGuardTeamHeads;
		} else {
			headsavailablelist = g_FemaleGuardTeamHeads;
			headsavailablelen = var80062b18;
		}
	} else {
		headsavailablelist = g_FemaleGuardHeads;
		headsavailablelen = g_NumFemaleGuardHeads;
	}

	for (i = 0; i < g_NumActiveHeadsPerGender; i++) {
		do {
			done = true;
			g_ActiveFemaleHeads[i] = headsavailablelist[rngRandom() % headsavailablelen];

			if (headsavailablelen > g_NumActiveHeadsPerGender) {
				for (j = 0; j < i; j++) {
					if (g_ActiveFemaleHeads[j] == g_ActiveFemaleHeads[i]) {
						done = false;
					}
				}
			}
		} while (!done);
	}

	g_ActiveMaleHeadsIndex = 0;
	g_ActiveFemaleHeadsIndex = 0;

	for (i = 0; i < g_NumActiveHeadsPerGender; i++);
	for (i = 0; i < g_NumActiveHeadsPerGender; i++);

#ifndef PLATFORM_N64
	// A converted GoldenEye mission wears GoldenEye's own heads: the same four
	// for a level that its own bodyChooseHead() takes, in the same two lists
	gexPlusMissionHeads();
#endif
}
