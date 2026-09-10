#ifndef _IN_GHOSTNET_H
#define _IN_GHOSTNET_H

#include <stdio.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "game/modghost.h"

/**
 * Which transport this build talks HTTPS over.
 *
 * Windows has one in the box. WinHTTP has shipped with every version since XP,
 * verifies against the certificate store Windows already keeps and updates,
 * and adds nothing to the package - where libcurl on Windows means shipping
 * libcurl and the dozen DLLs underneath it (OpenSSL, nghttp2, brotli, idn2,
 * psl, zstd) and then answering for a CA bundle that OpenSSL looks for at a
 * path no player's machine has. That is a lot of moving parts for four
 * requests, and every one of them is a thing that can be missing from a zip.
 *
 * Everywhere else libcurl is the answer, because everywhere else there is no
 * system HTTP API to use instead: macOS carries libcurl in the SDK so there is
 * nothing to bundle, and on Linux libcurl is on every desktop already.
 *
 * WinHTTP wins where both are available - a build on Windows that happens to
 * find libcurl still uses the one with nothing to ship.
 */
#if defined(PLATFORM_WIN32)
#define PD_GHOST_WINHTTP 1
#endif

#if defined(PD_GHOST_WINHTTP) || defined(PD_HAVE_CURL)
#define PD_GHOST_NET 1
#endif


/**
 * The ghost server client: accounts, uploads, leaderboards and downloads.
 *
 * Every call here is asynchronous. The menus that use it are ticked inside the
 * render loop, and a leaderboard fetch across the internet is tens to hundreds
 * of milliseconds on a good day - long enough that doing it inline would show
 * as a freeze. So a request starts a worker, the menu polls ghostnetGetState()
 * and draws what it finds, and exactly one request is in flight at a time
 * because a menu cannot ask two questions at once.
 *
 * The whole thing is optional at build time. libcurl brings TLS, redirects and
 * timeouts that are not worth reimplementing, but it also brings a dependency
 * the Windows package would have to carry, so a build without it still
 * compiles and the network menus say so rather than being missing.
 */

#define GHOSTNET_IDLE  0
#define GHOSTNET_BUSY  1
#define GHOSTNET_OK    2
#define GHOSTNET_ERROR 3

#define GHOSTNET_MAXBOARD 100

/**
 * The port's HTTP client, which lives here because the ghost server was the
 * first thing that needed one.
 *
 * It is not only the ghost client's any more: Check for Updates asks GitHub
 * for the latest release over the same two backends. A second copy of the
 * WinHTTP and libcurl halves for the sake of a tidier name would be two places
 * for a certificate or a timeout to be wrong, so the transport is shared and
 * the things that differ between the two callers are fields on the request.
 */
struct ghostnetbuf {
	char *data;
	size_t len;
	// Where a reply goes when it is too big to hold. A ghost and a board fit
	// in memory and are wanted there; a new copy of the game does not and is
	// not, so the update download hands over an open file and the reply is
	// written through to it as it arrives. len still counts every byte either
	// way, which is what the caller checks against the size it was promised.
	FILE *sink;
	// The most len may reach, or zero for the in-memory cap. A caller that was
	// promised a size sets it, so a reply that runs past what was promised is
	// refused as it arrives rather than after it has all been written.
	size_t maxlen;
};

/**
 * One request, in the terms both backends can answer.
 *
 * Four shapes are all this ever needs: a GET, a GET whose reply is a file, a
 * POST of a small JSON body, and a POST of a ghost. So the request is a URL, an
 * optional body with a type, and whether the account headers go on it.
 */
struct ghostnetreq {
	const char *url;
	const void *body;   // NULL for a GET
	u32 bodylen;
	const char *type;   // Content-Type, when there is a body
	bool auth;          // send X-Ghost-User and X-Ghost-Pin
	// Follow a redirect rather than treating one as the reply. Off for the
	// ghost server, whose endpoints are exact paths on a machine this file was
	// written against; on for the updater, which asks GitHub for "the latest
	// release" and is answered with a redirect to wherever that release's
	// files actually live. The two reasons the ghost side refuses are both
	// about the account headers, and the updater sends none.
	bool redirect;
	// Seconds for the whole exchange, or zero for the ordinary budget. A reply
	// that is a copy of the game takes longer than one that is a leaderboard.
	s32 timeout;
	// Somewhere to look for a request to stop, or NULL. Read between reads
	// of the reply; set from another thread by whoever wants the transfer
	// over with, which is the game shutting down under a download.
	volatile bool *cancel;
};

/**
 * Carry out one request. Defined once per backend, in ghostnet.c.
 *
 * Returns false only when the exchange did not happen at all - no route, no
 * name, a refused certificate - with why in err. A server that answered is a
 * true return whatever it said, and what it said is in status and buf, because
 * "the board is empty" and "the network is down" are different things to tell
 * a player and only the caller knows which reply means which.
 */
bool ghostnetSend(const struct ghostnetreq *req, struct ghostnetbuf *buf,
		s32 *status, char *err, u32 errsize);

/**
 * Pull one value out of a flat JSON object, into a bounded buffer.
 *
 * A scanner rather than a parser, for replies that are small and flat - see
 * the comment on the definition. end bounds the search to one object, which is
 * what makes it usable over an array: without it a field missing from one
 * element is answered with the next element's. NULL means "to the end of the
 * string".
 *
 * Here rather than private to ghostnet.c because the ghost server is no longer
 * the only thing answered in JSON: Community Packs reads a GitHub release the
 * same way.
 */
bool ghostnetJsonField(const char *json, const char *end, const char *key,
		char *out, u32 outsize);

/**
 * How many ghost accounts this machine remembers, the active one included.
 *
 * Four because a shared machine is a couch with a few people on it rather than
 * a login server, and every slot is a username and a PIN written into pd.ini.
 */
#define GHOSTNET_MAXACCOUNTS 4
// Fifteen rather than twenty: an account name is written into a ghost's owner
// field, which is MODGHOST_OWNERLEN bytes, and a name that has to be truncated
// to fit is a name that can be confused with somebody else's.
#define GHOSTNET_MAXUSER  (MODGHOST_OWNERLEN - 1)
#define GHOSTNET_MAXPIN   8

// An id out of the security question tables, and what it costs to send one
// as a JSON string with every character escaped.
#define GHOSTNET_MAXQA    32

struct ghostboardentry {
	s32 id;
	u32 time60;
	char user[GHOSTNET_MAXUSER + 1];
	bool have;       // already in the ghosts directory
	bool trialrules; // set with the fork's added moves off
	// The character the run was set as, in the same plus-one encoding the
	// ghost header uses, so a board row can be shown as whoever set it. Zero
	// from a row stored before the board carried one.
	u8 mpbody;
	u8 mphead;
};

extern char g_GhostNetUser[GHOSTNET_MAXUSER + 2];
extern char g_GhostNetPin[GHOSTNET_MAXPIN + 2];

/**
 * The security questions this machine has picked, as indices into the tables
 * in ghostrecovery.h, or -1 for one that has not been chosen.
 *
 * Three of them. One category out of ten and one answer out of fifty is a
 * guess in five hundred, and the reset limiter was all that stood behind it;
 * three different categories is a guess in a hundred million or so, which is
 * a secret rather than a hint. The first pair is what an account made before
 * there were three holds, and travels under the field names it always did.
 *
 * They are not written to pd.ini and are not meant to survive the run. What
 * the server holds is a hash of the pairs, and a copy of the answers sitting
 * in a text file next to the PIN they recover would make the questions a
 * decoration on the PIN rather than a second thing to know. Registering sends
 * them; resetting a PIN sends them; nothing else keeps them.
 */
#define GHOSTNET_NUMQUESTIONS 3

extern s32 g_GhostNetQuestion[GHOSTNET_NUMQUESTIONS];
extern s32 g_GhostNetAnswer[GHOSTNET_NUMQUESTIONS];
extern char g_GhostNetSavedUser[GHOSTNET_MAXACCOUNTS - 1][GHOSTNET_MAXUSER + 2];
extern char g_GhostNetSavedPin[GHOSTNET_MAXACCOUNTS - 1][GHOSTNET_MAXPIN + 2];
/**
 * The trial character each remembered account runs as.
 *
 * The active account's is g_ModGhostBody/g_ModGhostHead, which is where the
 * picker writes and where a recorded run reads: an account's character is not
 * a separate setting, it is the setting, held for whoever is signed in. These
 * are the same two values for the accounts that are not signed in, swapped
 * with the live pair whenever the active account changes, so that switching
 * back to a name brings back the character that name was being played as.
 */
extern s32 g_GhostNetSavedBody[GHOSTNET_MAXACCOUNTS - 1];
extern s32 g_GhostNetSavedHead[GHOSTNET_MAXACCOUNTS - 1];
extern char g_GhostNetUrl[256];

bool ghostnetIsAvailable(void);
const char *ghostnetGetAccountName(void);
s32 ghostnetGetNumAccounts(void);
const char *ghostnetGetAccountAt(s32 index);
void ghostnetSelectAccount(s32 index);
void ghostnetBeginNewAccount(void);
bool ghostnetHasAccount(void);
bool ghostnetAccountIsValid(void);
bool ghostnetIsSignedIn(void);
bool ghostnetRecoveryIsSet(void);
s32 ghostnetRecoveryCount(void);
bool ghostnetRecoveryIsRepeated(void);

/**
 * What the server last said about the account's own security questions.
 *
 * UNKNOWN until something signs in - nothing else can be asked, and a server
 * too old to answer leaves it there for ever, which is what stops an older
 * board being nagged about a feature it does not have. PARTIAL is an account
 * with at least one question and fewer than three: it can be reset, and its
 * owner is told once a run that it could be harder to guess at.
 */
#define GHOSTNET_RECOVERY_UNKNOWN 0
#define GHOSTNET_RECOVERY_MISSING 1
#define GHOSTNET_RECOVERY_SET     2
#define GHOSTNET_RECOVERY_PARTIAL 3

s32 ghostnetGetAccountRecovery(void);
s32 ghostnetGetState(void);
const char *ghostnetGetMessage(void);
void ghostnetClearState(void);

void ghostnetRegister(void);
void ghostnetLogin(void);
void ghostnetSetRecovery(void);
void ghostnetResetPin(void);
void ghostnetUploadMine(void);
void ghostnetFetchBoard(s32 stagenum, s32 difficulty);
void ghostnetDownload(s32 index);

void ghostnetClearBoard(void);
s32 ghostnetGetBoardCount(void);
struct ghostboardentry *ghostnetGetBoardEntry(s32 index);
s32 ghostnetGetBoardStage(void);
s32 ghostnetGetBoardDifficulty(void);

void ghostnetInit(void);
void ghostnetShutdown(void);

#endif
