#ifndef _IN_GHOSTRECOVERY_H
#define _IN_GHOSTRECOVERY_H

#include <PR/ultratypes.h>
#include "types.h"

/**
 * The security question a PIN is reset with.
 *
 * One category out of ten, and one answer out of that category's list. Both
 * are chosen from dropdowns and neither is typed: the game's on screen
 * keyboard is slow enough that a typed answer would be retyped wrongly a year
 * later, and a list also means the answer that reaches the server is one of a
 * known set rather than whatever the player's keyboard produced.
 *
 * What the server stores is a hash of "<category id>|<answer id>" and nothing
 * else - not the category, so a guesser has to find that too, and not anything
 * readable, so the database is no more use than the leaderboard it sits next
 * to. Ten categories times about fifty answers is not a password and is not
 * meant to be one; the reset endpoint's rate limiter is what stands behind it.
 *
 * THE IDS BELOW ARE THE WIRE FORMAT AND ARE FROZEN. A player's account holds a
 * hash of the two ids they picked, so renaming one, or removing one, or
 * reordering a list in a way that changes which id a name maps to, locks
 * everybody who chose it out of their own recovery. Add to the end of a list
 * freely; change nothing that is already there. The display names may be
 * corrected - only the ids are the contract.
 */
struct ghostrecoveryoption {
	const char *id;
	const char *name;
};

struct ghostrecoverycategory {
	const char *id;
	const char *name;
	const struct ghostrecoveryoption *options;
	s32 numoptions;
};

#define GHOSTRECOVERY_NUMCATEGORIES 10

extern const struct ghostrecoverycategory g_GhostRecoveryCategories[GHOSTRECOVERY_NUMCATEGORIES];

const struct ghostrecoverycategory *ghostRecoveryGetCategory(s32 index);
const char *ghostRecoveryGetCategoryId(s32 index);
const char *ghostRecoveryGetAnswerId(s32 category, s32 answer);
const char *ghostRecoveryGetAnswerName(s32 category, s32 answer);
s32 ghostRecoveryGetNumAnswers(s32 category);

#endif
