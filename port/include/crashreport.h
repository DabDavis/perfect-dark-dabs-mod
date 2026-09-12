#ifndef _IN_CRASHREPORT_H
#define _IN_CRASHREPORT_H

#include <stdbool.h>
#include <PR/ultratypes.h>
#include "platform.h"

/**
 * Crash reports: written where the game died, sent when somebody says so.
 *
 * A player who crashes has the one thing nobody here has - the machine it
 * happened on, with their mods, their settings and their log. Every crash so
 * far has been read out of a screenshot of the dialog, which is a stack and
 * nothing else: not which stage, not which mod, not whether the Randomizer was
 * on. This is that same dialog with somewhere for the rest of it to go.
 *
 * Two halves, because a crashed process is the worst place to do anything:
 *
 * - crashReportSave() writes the report to disk first, from inside the
 *   handler. It composes rather than transmits, and what it needs is a file
 *   handle and the strings it already has.
 * - crashReportSend() puts one on the wire, and is called either from the
 *   dialog's button - the player is looking at it, so this is the moment they
 *   will say yes - or from the menu page next time the game starts, which is
 *   where a note can be typed and where a send that failed can be retried.
 *
 * Nothing is ever sent without being asked for. A report that is never sent is
 * a file in the save directory and nothing else.
 */

#define CRASHREPORT_DIR      "$S/crashreports"
#define CRASHREPORT_MAXNOTE  200

// What a report may grow to. The log tail is the only part that is not bounded
// by what wrote it, and it is cut to fit this.
#define CRASHREPORT_MAXTEXT  32768

// How much of pd.log goes in. A level load is a few dozen lines and the
// interesting ones are the last of them - which stage, which mod, what the
// loaders said - so this is deep enough to hold the load that crashed and
// shallow enough not to send somebody's whole session.
#define CRASHREPORT_LOGLINES 200

/**
 * Compose a report for this crash and write it into the save directory.
 *
 * text is what the dialog is about to show: the exception, the stack, and on
 * Windows the faulting address. Around it go the build, the platform, the mod
 * that was loaded, the [Mod] section of pd.ini and the tail of pd.log.
 *
 * Returns the path it was written to, or NULL if it could not be written -
 * which is not worth failing the dialog over, so the caller carries on either
 * way. Safe to call from a signal handler or an exception filter in the sense
 * that matters: it touches no game state and allocates nothing that the crash
 * could already be holding.
 */
const char *crashReportSave(const char *text);

/**
 * Keep one line of the log for the next report.
 *
 * Called from sysLogPrintf() for every line, because pd.log is only written
 * when the game was started with --log and the lines are worth having either
 * way. The ring is fixed and the copy is bounded; nothing here allocates.
 */
void crashReportLogLine(const char *line);

/**
 * The report waiting to be sent, or NULL.
 *
 * Set by crashReportSave() for this run's crash, and by crashReportScan() at
 * startup for one left over from a previous run. The newest is the one that
 * is offered: a player who has crashed twice wants to send the one they just
 * had, and the older files are still on disk.
 */
const char *crashReportPending(void);

/**
 * When the pending report was written, as "2026-09-12 21:40", for the page to
 * say which crash it is about to send. Empty when there is none.
 */
const char *crashReportPendingWhen(void);

/**
 * How many reports are on disk, sent or not.
 */
s32 crashReportCount(void);

/**
 * Look for reports left by a previous run. Called once at startup.
 */
void crashReportScan(void);

/**
 * Send one report, with an optional note the player typed.
 *
 * Blocking, and deliberately so in both of its callers: the dialog has nothing
 * else to do and the menu page is a page with one button on it. Returns false
 * with a sentence in err - the server's, or the transport's.
 *
 * A report that is sent is deleted. Keeping it would mean a second copy of
 * something the player has already given away, and the only thing anyone could
 * do with it afterwards is send it again.
 */
bool crashReportSend(const char *path, const char *note, char *err, u32 errsize);

/**
 * Whether this build can send at all. False in a build without the HTTP
 * client, where the report is written and nothing offers to send it.
 */
bool crashReportCanSend(void);

/**
 * Delete the pending report unsent.
 */
void crashReportDiscard(void);

#endif
