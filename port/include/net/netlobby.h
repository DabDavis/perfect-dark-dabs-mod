#ifndef _IN_NET_NETLOBBY_H
#define _IN_NET_NETLOBBY_H

#include <PR/ultratypes.h>

/**
 * The lobby client (PLANS/NETPLAY.md "Lobby", phase 6a): rooms on pdlobbyd
 * (tools/pdlobbyd/README.md is the API) for the Online Game menus.
 *
 * Every request runs on one of the lobby's own two threads - one for the
 * room's long-poll, which parks for up to 20 s, and one for everything else
 * (sign-in, the list, create, join, ready, team, chat, settings, kick,
 * launch, the host's heartbeat) - over ghostnet's HTTPS transport, so the
 * ghost worker is never held up. The menus call the actions here and read
 * what the threads last heard; netLobbyTick (main thread, every loop pass
 * while the lobby is in use) moves their results over and acts on the
 * room: a host opens its session when the room is made, starts the match
 * once everyone launched has connected, and reopens the room after it; a
 * member connects to the host with its ticket at launch.
 *
 * Game code includes this, so nothing here is a bool or ENet's.
 */

#define NETLOBBY_MAXROOMS    48
#define NETLOBBY_MAXMEMBERS  16
#define NETLOBBY_MAXCHAT     6
#define NETLOBBY_MAXRULES    12
#define NETLOBBY_MAXUSER     15
#define NETLOBBY_MAXROOMNAME 32
#define NETLOBBY_MAXPASSWORD 16
#define NETLOBBY_MAXCHATTEXT 120

// Why a room in the list cannot be joined from here (or 0)
#define NETLOBBY_COMPAT_OK      0
#define NETLOBBY_COMPAT_PROTO   1 // another netplay protocol
#define NETLOBBY_COMPAT_CONTENT 2 // another ROM, mod or added-content set
#define NETLOBBY_COMPAT_BUILD   3 // another build: joinable, but the host will refuse it

struct netlobbyroomsum {
	char id[9];
	char name[NETLOBBY_MAXROOMNAME + 1];
	char host[NETLOBBY_MAXUSER + 1];
	char stage[33];
	char scenario[33];
	char build[41];
	char content[65];
	char state[12];    // open, countdown, launched
	char region[17];
	s32 humans;
	s32 maxhumans;
	s32 spectators;
	s32 maxspectators;
	s32 sims;
	s32 locked;
	s32 proto;
	s32 dedicated;
	s32 hostrtt;       // ms, -1 unknown
	s32 compat;        // NETLOBBY_COMPAT_*
};

struct netlobbymember {
	char user[NETLOBBY_MAXUSER + 1];
	s32 team;
	s32 ready;
	s32 spectator;
	s32 host;
	s32 udp;
};

struct netlobbychat {
	char user[NETLOBBY_MAXUSER + 1];
	char text[NETLOBBY_MAXCHATTEXT + 1];
};

struct netlobbyrule {
	char key[25];
	char value[33];
};

struct netlobbyroom {
	s32 valid;
	s32 version;
	struct netlobbyroomsum sum;
	struct netlobbymember members[NETLOBBY_MAXMEMBERS];
	s32 nmembers;
	struct netlobbychat chat[NETLOBBY_MAXCHAT]; // oldest first
	s32 nchat;
	struct netlobbyrule rules[NETLOBBY_MAXRULES];
	s32 nrules;
	s32 countdownms;   // -1 unless counting down
	s32 launched;
	u32 launchat;
	char endpoints[4][48];
	s32 nendpoints;
	char publicep[48];
	s32 youhost;
	s32 youspectator;
	char you[NETLOBBY_MAXUSER + 1];
};

// Room settings for create (and the host's settings change)
struct netlobbycreate {
	char name[NETLOBBY_MAXROOMNAME + 1];
	char password[NETLOBBY_MAXPASSWORD + 1];
	s32 maxhumans; // 2-4 for now (MAX_PLAYERS)
};

extern s32 g_NetLobbyActive; // the lobby is in use (signed in or in a room): netLobbyTick runs
extern s32 g_NetLobbyRoom;   // this machine is in a room (a match it plays came from it)

void netLobbyArgs(void);      // --net-lobby-script, --net-lobby
void netLobbyTick(void);      // main thread, every loop pass while g_NetLobbyActive
void netLobbyShutdown(void);  // main.c cleanup: leave the room

// The menus
s32 netLobbyAvailable(void);         // a build with the HTTPS transport and an account set
const char *netLobbyAccount(void);   // the ghost account the lobby signs in as
s32 netLobbySignedIn(void);
s32 netLobbyBusy(void);              // a sign-in, list, create or join in flight
const char *netLobbyMessage(void);   // what the last of those said ("" if nothing)
void netLobbyClearMessage(void);

void netLobbyRefresh(void);
s32 netLobbyNumRooms(void);
const struct netlobbyroomsum *netLobbyRoomAt(s32 index);
const char *netLobbyCompatText(s32 compat);

void netLobbyCreate(const struct netlobbycreate *c);   // from the current Combat Simulator setup
void netLobbyJoin(const char *roomid, const char *password);
s32 netLobbyInRoom(void);
const struct netlobbyroom *netLobbyGetRoom(void);
s32 netLobbyIsHost(void);
s32 netLobbyMyReady(void);
s32 netLobbyMyTeam(void);            // 0, 1, or -1 a spectator
void netLobbySetReady(s32 ready);
void netLobbyCycleTeam(void);        // team 1 -> team 2 -> spectator -> team 1
void netLobbyChat(const char *text);
void netLobbySendSettings(void);     // host: the Combat Simulator setup again
void netLobbyKick(const char *user);
void netLobbyLaunch(s32 force);
void netLobbyCancelLaunch(void);
void netLobbyLeave(void);
void netLobbyWarm(void);           // the Online Game page opened: hash off the frame
void netLobbyCancelPending(void);  // the create/join page closed before the reply
s32 netLobbyLaunchState(void);       // 0 open, 1 counting down, 2 launched/connecting, 3 in the match
s32 netLobbyCountdownMs(void);       // the launch countdown left, -1 if none

// The session's side (netsession.c): the room's roster for ticket check 5
s32 netLobbyRosterHas(const char *user); // a non-host member, case-insensitive

// Back from a match that came from a room (menutick.c, mainmenu.c)
void netLobbyMenuAfterMatch(void);

#endif
