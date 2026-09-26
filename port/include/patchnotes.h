#ifndef _IN_PATCHNOTES_H
#define _IN_PATCHNOTES_H

#include <PR/ultratypes.h>
#include "platform.h"

/**
 * Patch notes: what got fixed, in the player's words, and who reported it.
 *
 * The notes are patchnotes.txt at the root of the repository. Every entry has a
 * number, and the newest number is baked into the build when CMake configures
 * it - along with a copy of the whole file - so a build knows which notes it
 * already contains. Check for Updates fetches the release's copy of the same
 * file and shows what the release has that this build does not; the main menu
 * shows, once, what arrived with an update the player has just started.
 *
 * Nothing here ever stands between the player and the game. A release without
 * the file, a file that does not parse, a build configured without one: each
 * of those is no notes, never an error.
 */

// The newest entry this build was configured with. 0 when there was no file.
s32 patchnotesBuildNumber(void);

// The text a DESCRIPTION_PATCHNOTES scrollable shows, for whichever of the two
// pages is open. Formatted for menuitemScrollableRender(): "|" starts a heading.
char *patchnotesGetText(void);

// The Check for Updates page: how many fixes the release has that this build
// does not (0 when there is no newer release or it has no notes), and the page
// itself, opened with the text for what the check found.
s32 patchnotesCountForUpdate(void);
void patchnotesOpenForUpdate(void);

// The "show once after an update" setting, for the checkbox on the same page.
bool patchnotesPopupIsEnabled(void);
void patchnotesPopupSetEnabled(bool enabled);

// Called from the Perfect Menu's own tick while it is the dialog on top. Opens
// the notes the first time that happens after an update, and never again.
void patchnotesMainMenuTick(void);

// After configInit(): works out whether this start is the first after an
// update, before anything has had a chance to write pd.ini.
void patchnotesInit(void);

// Both pages are this one dialog, retitled.
extern struct menudialogdef g_PatchNotesMenuDialog;

#endif
