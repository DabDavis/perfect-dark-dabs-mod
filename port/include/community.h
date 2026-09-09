#ifndef _IN_COMMUNITY_H
#define _IN_COMMUNITY_H

#include <PR/ultratypes.h>
#include "platform.h"

// Only ever handed back to menuimage.c, which is where a picture is drawn.
struct menuimage;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Community Packs: texture packs from the people who make them, installed from
 * inside the game.
 *
 * A pack is a few hundred megabytes in an archive on somebody's release page,
 * and installing one by hand means finding the right file among several,
 * knowing which folder this build reads, and knowing that a pack built for an
 * emulator is stored upside down and needs a marker file saying so. Every one
 * of those is a thing a player has got wrong and reported as a bug.
 *
 * So the game carries a short list of packs it knows about - who made them,
 * what they look like, and which file in their release is the one for this
 * port - asks the release page which version is current, and downloads,
 * checks, unpacks and selects it. The list is in the binary because there has
 * to be something on the page before anything has been fetched; what is *not*
 * in the binary is the version, the size or the URL, which come from the
 * release each time so that a build from last month still installs this
 * month's pack.
 *
 * The work is on one worker thread, the way update.c does it, and the menu
 * polls. See port/src/community.c.
 */

#define COMMUNITY_IDLE      0
#define COMMUNITY_ASKING    1 // asking the release page what is current
#define COMMUNITY_FOUND     2 // there is a release, and it is not installed
#define COMMUNITY_DOWNLOAD  3
#define COMMUNITY_UNPACKING 4
#define COMMUNITY_DONE      5
#define COMMUNITY_ERROR     6

/** Whether this build has an HTTP client at all. */
bool communityIsAvailable(void);

/** The catalogue. Fixed at build time; every one of these is a real person's work. */
s32 communityGetNumPacks(void);
s32 communityGetSelected(void);
void communitySetSelected(s32 index);

const char *communityGetName(s32 index);
const char *communityGetAuthor(s32 index);
const char *communityGetBlurb(s32 index);
const char *communityGetSource(s32 index);
struct menuimage *communityGetThumb(s32 index);

/**
 * Whether a pack from this catalogue entry is already in the pack folder.
 *
 * By the folder name it was installed under, which carries the version - so a
 * newer release of a pack that is installed reads as not installed, which is
 * the honest answer: both can sit side by side and be switched between.
 */
s32 communityIsInstalled(s32 index);

s32 communityGetState(void);
const char *communityGetStatus(void);

/** What the last ask found: the version, and how big the download is. */
const char *communityGetVersion(void);
u32 communityGetSize(void);
void communityGetProgress(u32 *done, u32 *total);

/** Ask the release page what is current. */
void communityCheck(void);

/** Download, check, unpack and select it. Nothing without a check first. */
void communityInstall(void);

void communityCancel(void);

/**
 * Driven from the scheduler once a frame rather than from the page, so backing
 * out of the menu does not leave an install half done. The worker does the
 * work; this is where what it produced is handed to the texture pack loader,
 * which is the game thread's business.
 */
void communityTick(void);

void communityShutdown(void);

#ifdef __cplusplus
}
#endif

#endif
