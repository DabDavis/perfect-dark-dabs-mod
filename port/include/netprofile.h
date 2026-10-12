#ifndef _IN_NETPROFILE_H
#define _IN_NETPROFILE_H

#include <PR/ultratypes.h>
#include "types.h"

/*
 * The online player profile (netprofile.c): the character, Control Style and
 * the Control and Player Options pages' choices this game plays online with,
 * kept in pd.ini's [Net] apart from the offline player 1's, and played as
 * player 1 while the online pages are up (Name Entry opens them, its Back
 * closes them). Online, player 1's name is the lobby account's.
 */

extern struct menudialogdef g_NetPlayerSetupMenuDialog; // the online pages' Player Setup...

void netProfileEnter(void); // Name Entry opens: player 1 plays the online profile
void netProfileLeave(void); // Name Entry closes back to where it was opened from
void netProfileTick(void);  // the online pages' tick: player 1's name is the account's
s32 netProfileActive(void); // player 1 is the online profile now

#endif
