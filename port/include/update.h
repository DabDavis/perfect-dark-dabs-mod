#ifndef _IN_UPDATE_H
#define _IN_UPDATE_H

#include <PR/ultratypes.h>
#include "platform.h"

/**
 * Check for Updates: fetch the latest release and put it in place.
 *
 * The mod builds itself on every push and cuts a release on every v* tag, so
 * there is always a newer copy somewhere and nobody who is not watching the
 * repository knows. This asks, downloads one file, checks it is the file it
 * was promised, and puts it where the running one is.
 *
 * Like follows like. A build cut from a v* tag looks at the stable release; a
 * build off the branch - which is what the rolling dabs-mod-dev prerelease is,
 * and what anything compiled locally is - looks at the rolling one. Nobody is
 * moved between the two by pressing a button that says "update": a player on
 * stable is never handed whatever the last commit happened to be, and somebody
 * testing the dev build is not quietly put back onto a release from three
 * weeks ago. VERSION_CHANNEL says which this build is, and CMakeLists.txt says
 * why that has to be baked in.
 *
 * Asynchronous for the same reason the ghost client is: this is ticked from
 * the render loop and a download is seconds rather than milliseconds. One
 * worker, one job, one result, and a menu that polls updateGetState().
 */

#define UPDATE_IDLE    0
#define UPDATE_BUSY    1
#define UPDATE_CURRENT 2 // asked, and this build is the latest release
#define UPDATE_FOUND   3 // asked, and there is a newer one
#define UPDATE_STAGED  4 // downloaded and swapped in; it runs on the next start
#define UPDATE_ERROR   5

// A release name is a tag - "v1.2.0" - and the commit is the short hash the
// build carries in VERSION_HASH.
#define UPDATE_MAXVERSION 32
#define UPDATE_MAXCOMMIT  16

// One release as its manifest describes it, plus the newest entry in its
// patch notes (0 when it has none), which is how two channels are put in order.
struct updaterel {
	char version[UPDATE_MAXVERSION + 1];
	char commit[UPDATE_MAXCOMMIT + 1];
	char asset[64];
	char sha[65];
	u32 size;
	s32 notesnum;
	char notesdate[16];
};

// See the note on the definition: empty for the real release URL, set only by
// somebody testing the updater against a server of their own.
extern char g_UpdateUrl[256];

bool updateIsAvailable(void);
s32 updateGetState(void);
const char *updateGetMessage(void);
const char *updateGetVersion(void);
void updateGetProgress(u32 *done, u32 *total);

// The patch notes the last check fetched from the release, beside its manifest.
// NULL when it found none, which is an ordinary answer rather than an error;
// the caller frees the copy. The generation changes whenever they do.
char *updateCopyNotes(void);
u32 updateGetNotesGeneration(void);

void updateForceRedownload(void);
bool updateIsForced(void);

void updateCheck(void);

/**
 * The other channel: stable for a dev build, dev for a stable one.
 *
 * A check the player makes also asks for the other channel's latest build, and
 * updateSwitchChannel() downloads and puts that one in place through the same
 * path as an update - same size and hash checks, same two renames - except
 * that the build it replaces is kept as <exe>.prev rather than removed. The
 * channel is baked into every build, so the build that arrives follows its own
 * channel from then on: nothing else needs to remember the switch.
 *
 * updateBackupSaves() copies pd.ini and the saves into a dated folder first;
 * the confirm dialog calls it, on the main thread, before the switch.
 */
const char *updateOtherChannel(void);
bool updateGetOther(struct updaterel *out);
bool updateOtherWasAsked(void);
void updateGetNotesNewest(s32 *num, char *date, u32 datesize);
void updateSwitchChannel(void);
bool updateBackupSaves(char *outdir, u32 outsize);

// The startup check the update notice makes (updatemenu.c). The same request,
// but a failure goes back to idle without a word, and quitting does not wait
// for it. Only starts from idle.
void updateCheckInBackground(void);

// The short commit the last check's release was built from, "" before one.
const char *updateGetCommit(void);
void updateInstall(void);

void updateInit(void);
void updateShutdown(void);

/**
 * Whether a downloaded build is waiting, and the way out to it.
 *
 * The swap happens while the game is still running - it has to, because the
 * player is the one who said yes and the menu has to be able to tell them it
 * worked - so by the time the game exits the new binary is already in place
 * and the old one is beside it under another name. updateRelaunchIfStaged() is
 * the last thing main() does: it starts the new copy and lets this one end.
 *
 * updateCleanUp() is the other half, called at startup, and removes the copy
 * that was moved aside. It cannot be done by the process that moved it,
 * because on Windows that file is the running program.
 */
/**
 * Whether this is the first run of a build Check for Updates put in place.
 *
 * True for exactly one start, however long after the install that start is.
 * The title screen uses it to say so - the version turns gold and a guard says
 * target sighted - and the run after that looks like every other run.
 */
bool updateWasJustInstalled(void);

bool updateIsStaged(void);
void updateRelaunchIfStaged(void);

// Starts this same build again with the same arguments. For a setting that only
// takes hold at startup; see sysRequestRestart().
void updateRelaunchSelf(void);
void updateCleanUp(void);

// The update notice (updatemenu.c): "a newer build is out" on the Perfect Menu.
// updatenoticeInit() straight after configInit(), updatenoticeStart() after
// updateInit(), the tick from the Perfect Menu's dialog handler.
struct menudialogdef;
struct menuinputs;
void updatenoticeInit(void);
void updatenoticeStart(void);
void updatenoticeMainMenuTick(struct menudialogdef *menudef, struct menuinputs *inputs, bool ontop);

#endif
