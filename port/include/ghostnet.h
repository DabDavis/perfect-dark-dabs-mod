#ifndef _IN_GHOSTNET_H
#define _IN_GHOSTNET_H

#include <PR/ultratypes.h>
#include "game/modghost.h"

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
#define GHOSTNET_MAXUSER  20
#define GHOSTNET_MAXPIN   8

struct ghostboardentry {
	s32 id;
	u32 time60;
	char user[GHOSTNET_MAXUSER + 1];
	bool have;   // already in the ghosts directory
};

extern char g_GhostNetUser[GHOSTNET_MAXUSER + 2];
extern char g_GhostNetPin[GHOSTNET_MAXPIN + 2];
extern char g_GhostNetUrl[256];

bool ghostnetIsAvailable(void);
bool ghostnetHasAccount(void);
s32 ghostnetGetState(void);
const char *ghostnetGetMessage(void);
void ghostnetClearState(void);

void ghostnetRegister(void);
void ghostnetLogin(void);
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
