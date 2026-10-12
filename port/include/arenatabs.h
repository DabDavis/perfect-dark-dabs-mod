#ifndef _IN_ARENATABS_H
#define _IN_ARENATABS_H

#include <PR/ultratypes.h>
#include "types.h"

/**
 * The Combat Simulator's Arena list in tabs, one per game its maps come from
 * (F3 20261012-114949): Perfect Dark's own list (or the loaded mod's), then
 * GoldenEye, GoldenEye X, each GoldenEye ROM hack's conversion (Goldfinger 64,
 * Tomorrow Never Dies 64) and every other mod's maps, only the tabs that have
 * any. The same dialog is the online Create Room's Arena page. The tabs only
 * choose what the list shows: the arena chosen is g_MpSetup.stagenum, as it
 * always was. port/src/arenatabs.c.
 */

// The tabs row's handler (g_MpArenaMenuItems' MENUITEMTYPE_TABS)
MenuItemHandlerResult arenaTabsMenuHandler(s32 operation, struct menuitem *item, union handlerdata *data);

// The Arena dialog's MENUOP_OPEN: the tabs worked out again and the chosen
// arena's shown, its row picked out in the list
void arenaTabsOpen(void);

// Called before each list operation: the tabs follow the arena table (a map
// registered on request, a mod swapped)
void arenaTabsSync(void);

// Whether the list is in tabs (two or more have maps listed; never in GE Plus,
// whose list is GoldenEye's alone)
s32 arenaTabsActive(void);

// Whether arena row i is in the tab shown (every row without tabs)
s32 arenaTabsRowShown(s32 i);

// Whether the tab shown is a game's other than the game's own list, which
// arenaTabsListHandler() lists in place of mpArenaMenuHandler()
s32 arenaTabsOwnList(void);
MenuItemHandlerResult arenaTabsListHandler(s32 operation, struct menuitem *item, union handlerdata *data);

#endif
