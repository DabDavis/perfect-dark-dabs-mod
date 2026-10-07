#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "config.h"
#include "fs.h"
#include "system.h"
#include "video.h"
#include "input.h"
#include "versioninfo.h"
#include "geconvert.h"
#include "modloader.h"
#include "lib/main.h"
#include "game/lang.h"
#include "game/menu.h"
#include "game/title.h"
#include "game/pdmode.h"
#include "game/challenge.h"
#include "game/modoptions.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "game/mplayer/setup.h"
#include "gexplus.h"
#include "gehud.h"
#include "xblamesh.h"
#include "game/lv.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/setuputils.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/mpstats.h"

extern s32 g_MpTimeLimit60;
extern s32 g_MpScoreLimit;
extern s32 g_NumReasonsToEndMpMatch;
#include "net/net.h"
#include "net/netsnap.h"
#include "net/nettransport.h"
#include "net/netlobby.h"
#include "netint.h"

/**
 * The session (PLANS/netplay/spec-stage.md; every message in netproto.h):
 *
 *   --host [port]          a listen host (dedicated with --dedicated) on
 *                          Net.Port, 27100 unless set
 *   --connect addr[:port]  join one
 *
 * The host checks a joiner's build, content and settings in CONNECT and
 * gives it a player slot. When the host starts a match (mpStartMatch, H1)
 * every random choice is already made; RULES and STAGE_LOAD carry them,
 * and the clients load the stage by its key, never by its local id (H3).
 * Both seed the RNG at the same point of lvReset (H4) and hash what the
 * stage loads (H5/H6, HA-HD); the host compares each client's LOADED with
 * its own and releases everyone together with GO (H7). The host's match end
 * goes to the clients in MATCH_END (H8-H10), and the rules come off again
 * when the stage stops (H12).
 *
 * Phase 4a: the host's snapshots reach the clients (netents.c), which decode
 * and buffer them but still run their own whole simulation.
 */

#define NETROLE_NONE   0
#define NETROLE_HOST   1
#define NETROLE_CLIENT 2

// A host's view of one connected peer
#define NETCL_FREE       0
#define NETCL_CONNECTING 1 // connected, no CONNECT yet
#define NETCL_JOINED     2 // accepted, in the lobby
#define NETCL_LOADING    3 // sent STAGE_LOAD
#define NETCL_LOADED     4 // its LOADED matched
#define NETCL_PLAYING    5 // sent GO
#define NETCL_REFUSED    6 // refused or kicked, disconnect pending
#define NETCL_AWAY       7 // in the session, still in the last match's stage (no LOBBY yet)

// A client's own state
#define NETCS_IDLE       0
#define NETCS_CONNECTING 1
#define NETCS_HELLO      2 // CONNECT sent
#define NETCS_JOINED     3
#define NETCS_LOADING    4
#define NETCS_LOADED     5
#define NETCS_PLAYING    6
#define NETCS_GONE       7

#define NET_MAXPEERS (MAX_PLAYERS + 2)

struct netclient {
	s32 state;
	s32 slot;
	u64 since;
	char name[NET_MAXNAME + 1];
	struct netslotcfg cfg;
	struct netkeyvalue keys[NET_MAXKEYS];
	s32 nkeys;
	struct nethashcomp loaded[NET_MAXCOMPS];
	s32 nloaded;
	s32 gotloaded;
	u32 lobbymatch; // the match its LOBBY said it has left
	s32 spectator;  // a spectator: slot is its view, MAX_PLAYERS + k (protocol 9)
	s32 late;       // joined a match in progress: GO once LOADED (protocol 9)
	s32 resumed;    // ... into the seat its account held
	s32 ticketed;   // its CONNECT had a lobby ticket (the user is the account)
	u64 lastrecv;   // when its last message came (a drop's hold runs from it)
};

/**
 * The match's seats on the host (protocol 9; netproto.h "Join in
 * progress"), by mpindex
 */
#define NETSEAT_NONE  0 // not a player of this match (or no match)
#define NETSEAT_HOST  1
#define NETSEAT_TAKEN 2 // a client's (peer)
#define NETSEAT_OPEN  3 // nobody's: its player out of play
#define NETSEAT_HELD  4 // its client dropped: kept for its account until `until`

struct netseat {
	s32 state;
	s32 peer;
	char account[NET_MAXNAME + 1];
	u64 until;
	s32 vacate;   // its player is to go out of play at the next tick
	s32 drop;     // ... dropping what it carries (it was in play)
	s32 clear;    // ... and its row of the kill table cleared
	s32 ticketed; // its account came with a lobby ticket: only then is a drop held
};

static struct netseat s_Seats[MAX_PLAYERS];
static s32 s_JoinInProgress = 0; // Net.JoinInProgress: every match starts with all its seats
static s32 s_MaxPlayers = 4;     // Net.MaxPlayers: humans a direct host seats (2-12; lobby rooms seat their own size)
static s32 s_ReconnectHold = 30; // Net.ReconnectHold: seconds a dropped player's seat is kept
static s32 s_HostEnded = 0;      // the host's MATCH_END has gone
static s32 s_Spectate = 0;       // client: --net-spectate, or a lobby room's spectator
static s32 s_Spectating = 0;     // client: this match is watched, not played
static u32 s_JoinGoTick = 0;     // client: the host tick a join in progress started at (0 none)
static u32 s_Joins = 0, s_Resumes = 0, s_SpecJoins = 0, s_HoldsExpired = 0, s_Vacated = 0;
static s32 s_LastRefuse = -1;
static s32 s_TestGiveSlot = -1, s_TestGiveN = 0; // --net-test-givekills
static u32 s_TestGiveTick = 0;    // client: the code the session last ended on

struct nethost *g_NetHostSocket = NULL;
static s32 s_SockLent = 0; // g_NetHostSocket is the lobby's (netrdv.c), not ours to close
s32 g_NetNoticePending = 0;

static s32 s_Role = NETROLE_NONE;
static char s_Name[NET_MAXNAME + 1] = "";
static s32 s_Port = NET_DEFAULT_PORT;
static s32 s_RequireTicket = 0;
static char s_RoomId[16] = "";
static char s_RoomSecret[72] = "";
static s32 s_LobbyClockOffset = 0;

// A room from the lobby (netlobby.c): set at runtime, never saved to pd.ini
static s32 s_LobbyRoomOn = 0;
static char s_LobbyRoomId[16] = "";
static char s_LobbyRoomSecret[72] = "";
static s64 s_LobbyClock = 0; // the lobby's unix clock minus this machine's

static char s_ConnectAddr[256];
static u16 s_ConnectPort;
static char s_Ticket[NET_MAXTICKET + 1];
static u32 s_ProtocolSent = NET_PROTOCOL_VERSION;

// host
static struct netclient s_Clients[NET_MAXPEERS];
static u32 s_MatchId = 0;
static u64 s_LoadDeadline = 0;
static s32 s_HostLoaded = 0;
static struct nethashcomp s_HostComps[NET_MAXCOMPS];
static s32 s_HostNumComps = 0;

// client
static s32 s_ClientState = NETCS_IDLE;
static s32 s_ServerPeer = -1;
static u64 s_ConnectStart = 0;  // the first attempt
static s32 s_ConnectTries = 0;
static s32 s_ConnectWindowMs = NET_CONNECT_WINDOW_MS; // a lobby member trying several endpoints shortens it
static s32 s_Unreached = 0;        // the last connect never reached a host (no ENet connect in the window)
static char s_HostOwnName[16];     // a lobby host: its profile's name while it plays a match as its account
static s32 s_HostNameOn = 0;
static s32 s_Leaving = 0;
static s32 s_DropToMenus = 0;
static s32 s_EndPending = 0;
static s32 s_HostDedicated = 0;
static s32 s_ClientSlot = 0;      // the slot (pad, mpindex) ACCEPT gave this machine
static s32 s_ServerClosed = 0;    // the host's peer is disconnected for good
static u64 s_ClientBarrierDeadline = 0;

// a join in progress's STAGE_LOAD that came before its ACCEPT (another
// channel): taken once the ACCEPT is
static u8 s_PendStage[512];
static s32 s_PendStageLen = 0;

// the match, both sides
static s32 s_MatchActive = 0;  // from the start (H1 / STAGE_LOAD) to H12
static s32 s_MatchLoaded = 0;  // its stage has begun loading (H5): H12 then ends it
static s32 s_MatchStage = -1;
static s32 s_MatchKeyKind = -1;                 // the stage key's kind (client: as it came)
static char s_MatchWhat[NET_MAXMAPDIR + NET_MAXMAPNAME + 32];
static u32 s_MatchIdCur = 0;
static s32 s_SeedPending = 0;
static u64 s_Seed = 0;
static u64 s_Seed2 = 0;
static s32 s_BarrierHeld = 0;

// MATCH_END as a client received it
static struct {
	s32 valid;
	u8 numplayers;
	u8 award1[MAX_PLAYERS];
	u8 award2[MAX_PLAYERS];
	u8 medals[MAX_PLAYERS];
	u8 title[MAX_PLAYERS];
	s8 placement[MAX_MPCHRS];
	s32 rankablescore[MAX_MPCHRS];
	s16 numdeaths[MAX_MPCHRS];
	s16 numpoints[MAX_MPCHRS];
	s16 killcounts[MAX_MPCHRS][MAX_MPCHRS];
} s_End;
static u8 s_EndScen[NETSCEN_SIZE]; // MATCH_END's scenario block (protocol 7)

// harness
static s32 s_TestHost = 0;    // --net-test-host N: start the match once N have joined
static s32 s_TestJoin = 0;    // --net-test-join: exit 3 when the session ends badly
static u64 s_TestReadyAt = 0;
static s32 s_TestStarted = 0;

static char s_NoticeText[NET_MAXTEXT + 64];

static u8 s_Buf[64 * 1024];

extern u16 g_AwardNames[];
extern s32 g_StageNum;
extern s32 g_MainChangeToStageNum;
extern u64 g_RngSeed;
void rngSetSeed(u64 seed);
void rng2SetSeed(u64 seed);
s32 mainStageCanLoad(s32 stagenum);

static const char *s_RefuseNames[NETREFUSE_COUNT] = {
	"none", "protocol", "build", "content", "region", "geconvert", "full", "started", "ticket",
	"must", "notstock", "stagehash", "nostage", "timeout", "badmsg", "shutdown", "left",
};

#define NET_CLIENT_BARRIER_TIMEOUT_MS (NET_LOAD_TIMEOUT_MS * 4)
#define NET_JOIN_LEAD 3 // ticks a join in progress starts ahead of the host's GO
extern s32 g_StageTimeElapsed60;

PD_CONSTRUCTOR static void netSessionConfigInit(void)
{
	configRegisterInt("Net.Port", &s_Port, 1, 65535);
	configRegisterString("Net.Name", s_Name, sizeof(s_Name));
	configRegisterInt("Net.RequireTicket", &s_RequireTicket, 0, 1);
	configRegisterString("Net.RoomId", s_RoomId, sizeof(s_RoomId));
	configRegisterString("Net.RoomSecret", s_RoomSecret, sizeof(s_RoomSecret));
	configRegisterInt("Net.LobbyClockOffset", &s_LobbyClockOffset, -86400, 86400);
	configRegisterInt("Net.JoinInProgress", &s_JoinInProgress, 0, 1);
	configRegisterInt("Net.ReconnectHold", &s_ReconnectHold, 0, 600);
	configRegisterInt("Net.MaxPlayers", &s_MaxPlayers, 2, MAX_PLAYERS);
}

s32 netSessionMaxPlayers(void)
{
	return s_MaxPlayers < 2 ? 2 : s_MaxPlayers > MAX_PLAYERS ? MAX_PLAYERS : s_MaxPlayers;
}

/**
 * The seats a match this host starts has (phase 8): a lobby room's size, or
 * Net.MaxPlayers. Every seat is a player pass on the host, so a 2-player
 * room seats 2, not MAX_PLAYERS.
 */
static s32 netHostSeatCount(void)
{
	s32 n = s_LobbyRoomOn ? netLobbyRoomMaxHumans() : 0;

	if (n <= 0) {
		n = netSessionMaxPlayers();
	}

	return n < 1 ? 1 : n > MAX_PLAYERS ? MAX_PLAYERS : n;
}

// The lobby's clock as the host keeps it: a lobby room's offset from the
// times in its own state replies, else Net.LobbyClockOffset
static u64 netSessionLobbyNow(void)
{
	return (u64)((s64)time(NULL) + (s_LobbyRoomOn ? s_LobbyClock : (s64)s_LobbyClockOffset));
}

static u64 netNowMs(void)
{
	return sysGetMicroseconds() / 1000;
}

static const char *netRefuseName(s32 code)
{
	return code >= 0 && code < NETREFUSE_COUNT ? s_RefuseNames[code] : "?";
}

static s32 netIsDedicatedHost(void)
{
	return g_NetDedicated;
}

/*
 * Boot
 */

/**
 * From netInitArgs, before the window: what the session will be. The socket
 * opens in netSessionInit, once the mods (which the session hash reads) are
 * mounted.
 */
void netSessionArgs(void)
{
	const char *connect = sysArgGetString("--connect");
	const char *ticket = sysArgGetString("--net-ticket");

	s_TestHost = sysArgGetInt("--net-test-host", 0);

	// --net-test-givekills SLOT,TICK,N: at host tick TICK, N kills of the
	// last sim recorded for SLOT's player through the stock path (its E6
	// events reach the clients), so a reconnect has kills to keep
	{
		const char *gk = sysArgGetString("--net-test-givekills");

		if (gk && sscanf(gk, "%d,%u,%d", &s_TestGiveSlot, &s_TestGiveTick, &s_TestGiveN) != 3) {
			s_TestGiveTick = 0;
		}
	}
	s_TestJoin = sysArgCheck("--net-test-join");
	s_Spectate = sysArgCheck("--net-spectate");
	s_ProtocolSent = (u32)sysArgGetInt("--net-test-protocol", NET_PROTOCOL_VERSION);

	if (ticket) {
		snprintf(s_Ticket, sizeof(s_Ticket), "%s", ticket);
	}

	if (sysArgCheck("--host") || g_NetDedicated) {
		const char *port = sysArgGetString("--host");

		if (port && port[0] >= '0' && port[0] <= '9') {
			s_Port = atoi(port);
		}

		s_Role = NETROLE_HOST;
		g_NetMode = NETMODE_SERVER;

		if (!g_NetDedicated) {
			g_NetLocalSlot = 0;
		}
	} else if (connect) {
		const char *colon = strrchr(connect, ':');
		const char *bracket = strrchr(connect, ']');

		snprintf(s_ConnectAddr, sizeof(s_ConnectAddr), "%s", connect);
		s_ConnectPort = NET_DEFAULT_PORT;

		// addr:port, [v6]:port, or a bare address (a v6 one has colons of its own)
		if (colon && (!bracket || colon > bracket) && strchr(connect, ':') == colon) {
			s_ConnectAddr[colon - connect] = '\0';
			s_ConnectPort = (u16)atoi(colon + 1);
		} else if (colon && bracket && colon > bracket) {
			s_ConnectAddr[colon - connect] = '\0';
			s_ConnectPort = (u16)atoi(colon + 1);
		}

		if (s_ConnectAddr[0] == '[') {
			memmove(s_ConnectAddr, s_ConnectAddr + 1, strlen(s_ConnectAddr));

			if (strchr(s_ConnectAddr, ']')) {
				*strchr(s_ConnectAddr, ']') = '\0';
			}
		}

		s_Role = NETROLE_CLIENT;
		g_NetMode = NETMODE_CLIENT;
		g_NetLocalSlot = 0;
	}
}

static void netClientEnd(s32 code, const char *text);
static void netSessionOpenSocket(void);
static void netHostPeerGone(s32 peer, s32 held);
static void netHostLogSeats(const char *why);
static void netHostSendStage(s32 peer);
static s32 netHostFreeView(s32 peer);
static s32 netSeatFor(s32 peer, const char *account, s32 ticketed, s32 *resumed, s32 *oldpeer);

static void netSessionNotice(const char *fmt, const char *a, const char *b)
{
	snprintf(s_NoticeText, sizeof(s_NoticeText), fmt, a ? a : "", b ? b : "");
	g_NetNoticePending = 1;
}

void netSessionInit(void)
{
	struct nethashcomp comps[NET_MAXCOMPS];

	if (s_Role == NETROLE_NONE) {
		return;
	}

	if (s_Name[0] == '\0') {
		snprintf(s_Name, sizeof(s_Name), "%s", g_PlayerConfigsArray[0].base.name[0] ? g_PlayerConfigsArray[0].base.name : "Player");
		s_Name[strcspn(s_Name, "\n")] = '\0';

		if (s_Name[0] == '\0') {
			snprintf(s_Name, sizeof(s_Name), "%s", "Player");
		}
	}

	if (netTransportInit() != 0) {
		sysLogPrintf(LOG_ERROR, "net: the transport did not start; playing offline");
		s_Role = NETROLE_NONE;
		g_NetMode = g_NetDedicated ? NETMODE_SERVER : NETMODE_NONE;
		return;
	}

	netSessionHash(comps, NET_MAXCOMPS);
	netSessionOpenSocket();
}

/**
 * --net-sim DROP,DELAY[,JITTER]: the transport's loss and latency simulator
 * on what this machine receives (tools/ci/netplayertest.sh)
 */
static void netSessionApplySim(void)
{
	const char *sim = sysArgGetString("--net-sim");
	int drop = 0;
	int delay = 0;
	int jitter = 0;

	if (!sim || !g_NetHostSocket || sscanf(sim, "%d,%d,%d", &drop, &delay, &jitter) < 1) {
		return;
	}

	netHostSetSim(g_NetHostSocket, drop, delay, jitter, 12345);
	sysLogPrintf(LOG_NOTE, "net: --net-sim: %d%% of datagrams in dropped, the rest %d ms late (+0..%d)", drop, delay, jitter);
}

static void netSessionOpenSocket(void)
{
	if (s_Role == NETROLE_HOST) {
		g_NetHostSocket = netHostCreate(NULL, (u16)s_Port, NET_MAXPEERS);

		if (!g_NetHostSocket) {
			sysLogPrintf(LOG_ERROR, "net: could not listen on UDP port %d", s_Port);
			return;
		}

		sysLogPrintf(LOG_NOTE, "net: hosting on UDP port %u as \"%s\"%s, protocol %d, build %s",
				netHostPort(g_NetHostSocket), s_Name, g_NetDedicated ? " (dedicated)" : "",
				NET_PROTOCOL_VERSION, VERSION_HASH);

		if (s_RequireTicket && (strlen(s_RoomId) != 8 || strlen(s_RoomSecret) != 64)) {
			sysLogPrintf(LOG_WARNING, "net: Net.RequireTicket is on but Net.RoomId/Net.RoomSecret are not a room's; every join will be refused");
		}
	} else {
		g_NetHostSocket = netHostCreate(NULL, 0, 1);

		if (!g_NetHostSocket) {
			sysLogPrintf(LOG_ERROR, "net: could not open a UDP socket");
			return;
		}

		// the connect itself waits for the main loop (netSessionTick): until
		// it runs nothing services the socket, and a slow boot would use up
		// the handshake's retries
		s_ClientState = NETCS_IDLE;
	}

	netSessionApplySim();
}

static void netClientStartConnect(void)
{
	s_PendStageLen = 0;
	s_LastRefuse = -1;
	s_ServerPeer = netHostConnect(g_NetHostSocket, s_ConnectAddr, s_ConnectPort, 0);
	s_ClientState = s_ServerPeer >= 0 ? NETCS_CONNECTING : NETCS_GONE;
	s_ServerClosed = 0;

	if (s_ConnectTries++ == 0) {
		s_ConnectStart = netNowMs();
	}

	sysLogPrintf(LOG_NOTE, "net: connecting to %s port %u as \"%s\"%s", s_ConnectAddr, s_ConnectPort, s_Name,
			s_ConnectTries > 1 ? " (again)" : "");

	if (s_ServerPeer < 0) {
		netClientEnd(NETREFUSE_SHUTDOWN, "Could not reach the host.");
	}
}

/*
 * Sending
 */

static void netSend(s32 peer, s32 channel, struct netbuf *b)
{
	if (!g_NetHostSocket || peer < 0 || !netBufOk(b)) {
		if (!netBufOk(b)) {
			sysLogPrintf(LOG_ERROR, "net: a message did not fit (%d bytes)", netBufLen(b));
		}

		return;
	}

	netHostSend(g_NetHostSocket, peer, channel, b->data, netBufLen(b), NET_SEND_RELIABLE);
}

static void netSendRefuse(s32 peer, s32 code, const char *component, const char *text)
{
	struct netbuf b;

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(&b, NETMSG_REFUSE);
	netBufWriteU8(&b, (u8)code);
	netWriteStr(&b, component, NET_MAXKEY);
	netWriteStr(&b, text, NET_MAXTEXT);
	netSend(peer, NET_CHAN_RELIABLE, &b);
	netHostFlush(g_NetHostSocket);
}

static void netSendLeave(s32 peer, s32 code, const char *text)
{
	struct netbuf b;

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(&b, NETMSG_LEAVE);
	netBufWriteU8(&b, (u8)code);
	netWriteStr(&b, text, NET_MAXTEXT);
	netSend(peer, NET_CHAN_RELIABLE, &b);
	netHostFlush(g_NetHostSocket);
}

/**
 * The host turns a peer away: the reason first, then a polite disconnect,
 * which ENet sends after what is already queued
 */
static void netHostKick(s32 peer, s32 code, const char *component, const char *text)
{
	struct netclient *c = &s_Clients[peer];

	sysLogPrintf(LOG_NOTE, "net: refusing peer %d%s%s: [%s%s%s] %s", peer,
			c->name[0] ? " " : "", c->name, netRefuseName(code), component && component[0] ? " " : "",
			component ? component : "", text);

	netSendRefuse(peer, code, component, text);
	netHostDisconnectLater(g_NetHostSocket, peer, (u32)code);

	// kicked mid-match: its player is left on a neutral pad, as on a leave
	// (the DISCONNECT that follows finds it REFUSED and does nothing)
	if (c->state >= NETCL_LOADING && c->state <= NETCL_PLAYING) {
		netPlayersHostSlotGone(c->slot);
		netHostPeerGone(peer, 1);
	}

	c->state = NETCL_REFUSED;
}

/*
 * Host: CONNECT
 */

static const char *netCompDescription(const char *name)
{
	if (strcmp(name, "rom") == 0) return "Perfect Dark ROM";
	if (strcmp(name, "mod") == 0) return "loaded mod (--moddir / the Mods menu)";
	if (strcmp(name, "mapmods") == 0) return "Stage Loader map mods (Mod.MapMods)";
	if (strcmp(name, "borrow") == 0) return "GoldenEye X borrowed for GoldenEye's guns (Mod.BorrowGoldenEyeGuns)";
	if (strcmp(name, "added") == 0) return "added content (GoldenEye ROM, XBLA releases)";
	if (strcmp(name, "geconv") == 0) return "GoldenEye conversion";
	return name;
}

void netReadSlotCfg(struct netbuf *b, struct netslotcfg *cfg)
{
	cfg->mpheadnum = netBufReadU8(b);
	cfg->mpbodynum = netBufReadU8(b);
	cfg->controlmode = netBufReadU8(b);
	cfg->options = netBufReadU16(b);
	cfg->fovy = netBufReadF32(b);
	cfg->fovzoommult = netBufReadF32(b);
	cfg->fovzoom = netBufReadS32(b);
	cfg->mouseaimmode = netBufReadS32(b);
	cfg->mouseaimspeedx = netBufReadF32(b);
	cfg->mouseaimspeedy = netBufReadF32(b);
	cfg->crouchmode = netBufReadS32(b);
	cfg->radialmenuspeed = netBufReadF32(b);
	cfg->crosshairsway = netBufReadF32(b);
	cfg->extcontrols = netBufReadS32(b);
	cfg->crosshaircolour = netBufReadU32(b);
	cfg->crosshairsize = netBufReadU32(b);
	cfg->crosshairedgeboundary = netBufReadF32(b);
	cfg->crosshairhealth = netBufReadS32(b);
	cfg->usereloads = netBufReadS32(b);
	cfg->aspect = netBufReadF32(b);
	cfg->sensxsign = netBufReadS8(b);
	cfg->sensysign = netBufReadS8(b);
	cfg->aimlock = netBufReadU8(b);
	cfg->akimbotriggers = netBufReadU8(b);
}

void netWriteSlotCfg(struct netbuf *b)
{
	struct extplayerconfig *ext = &g_PlayerExtCfg[0];
	struct mpplayerconfig *p = &g_PlayerConfigsArray[0];
	f32 sx = 0;
	f32 sy = 0;

	inputMouseGetSpeed(&sx, &sy);

	netBufWriteU8(b, p->base.mpheadnum);
	netBufWriteU8(b, p->base.mpbodynum);
	netBufWriteU8(b, p->controlmode);
	netBufWriteU16(b, p->options);
	netBufWriteF32(b, ext->fovy);
	netBufWriteF32(b, ext->fovzoommult);
	netBufWriteS32(b, ext->fovzoom);
	netBufWriteS32(b, ext->mouseaimmode);
	netBufWriteF32(b, ext->mouseaimspeedx);
	netBufWriteF32(b, ext->mouseaimspeedy);
	netBufWriteS32(b, ext->crouchmode);
	netBufWriteF32(b, ext->radialmenuspeed);
	netBufWriteF32(b, ext->crosshairsway);
	netBufWriteS32(b, ext->extcontrols);
	netBufWriteU32(b, ext->crosshaircolour);
	netBufWriteU32(b, ext->crosshairsize);
	netBufWriteF32(b, ext->crosshairedgeboundary);
	netBufWriteS32(b, ext->crosshairhealth);
	netBufWriteS32(b, ext->usereloads);
	netBufWriteF32(b, g_NetDedicated ? 4.f / 3.f : videoGetAspect());
	netBufWriteS8(b, sx < 0 ? -1 : sx > 0 ? 1 : 0);
	netBufWriteS8(b, sy < 0 ? -1 : sy > 0 ? 1 : 0);
	netBufWriteU8(b, modIsCodAimLockOn() ? 1 : 0);
	netBufWriteU8(b, g_ModOptions.akimbotriggers ? 1 : 0);
}

static s32 netHostFreeSlot(s32 peer)
{
	s32 slot;
	s32 i;

	for (slot = netIsDedicatedHost() ? 0 : 1; slot < netHostSeatCount(); slot++) {
		s32 taken = 0;

		for (i = 0; i < NET_MAXPEERS; i++) {
			if (i != peer && s_Clients[i].state >= NETCL_JOINED && s_Clients[i].state != NETCL_REFUSED
					&& s_Clients[i].slot == slot) {
				taken = 1;
			}
		}

		if (!taken) {
			return slot;
		}
	}

	return -1;
}

static void netHostOnConnect(s32 peer, struct netbuf *b)
{
	struct netclient *c = &s_Clients[peer];
	struct nethashcomp mine[NET_MAXCOMPS];
	struct nethashcomp theirs[NET_MAXCOMPS];
	char build[NET_MAXBUILD + 1];
	char geconv[NET_MAXCOMPNAME + 1];
	char ticket[NET_MAXTICKET + 1];
	char nonce[33];
	u64 nonceexpiry = 0;
	char text[NET_MAXTEXT + 1];
	char key[NET_MAXKEY + 1];
	char addr[64];
	struct netaddr na;
	u32 protocol;
	u32 romversion;
	s32 ncomps;
	s32 nmine;
	s32 ticketlen;
	s32 code;
	s32 i;
	s32 j;
	s32 oldseat = -1;
	s32 connflags;
	s32 inprogress;
	s32 ticketed = 0;
	s32 oldpeer = -1;
	u8 accflags = 0;

	if (c->state != NETCL_CONNECTING) {
		netHostKick(peer, NETREFUSE_BADMSG, "", "CONNECT sent twice");
		return;
	}

	protocol = netBufReadU16(b);

	// the protocol first and alone: past it a different version's fields
	// need not parse
	if (protocol != NET_PROTOCOL_VERSION) {
		snprintf(text, sizeof(text), "This host runs netplay protocol %d and your game has %u - one of you needs to update.",
				NET_PROTOCOL_VERSION, protocol);
		netHostKick(peer, NETREFUSE_PROTOCOL, "protocol", text);
		return;
	}

	netBufReadString(b, build, sizeof(build));
	netBufReadString(b, geconv, sizeof(geconv));
	romversion = netBufReadU8(b);
	ncomps = netBufReadU8(b);

	if (ncomps > NET_MAXCOMPS) {
		b->error = 1;
	}

	for (i = 0; i < ncomps && netBufOk(b); i++) {
		netBufReadString(b, theirs[i].name, sizeof(theirs[i].name));
		theirs[i].hash = netReadU64(b);
	}

	netBufReadString(b, c->name, sizeof(c->name));
	netReadSlotCfg(b, &c->cfg);
	c->nkeys = netBufReadU8(b);

	if (c->nkeys > NET_MAXKEYS) {
		b->error = 1;
	}

	for (i = 0; i < c->nkeys && netBufOk(b); i++) {
		netBufReadString(b, c->keys[i].key, sizeof(c->keys[i].key));
		netRulesReadValue(b, &c->keys[i]);
	}

	ticketlen = netBufReadU16(b);

	if (ticketlen > NET_MAXTICKET) {
		b->error = 1;
	} else {
		netBufReadBytes(b, ticket, ticketlen);
		ticket[netBufOk(b) ? ticketlen : 0] = '\0';
	}

	connflags = netBufReadU8(b);
	c->spectator = (connflags & NETCONN_SPECTATE) != 0;

	if (!netBufOk(b) || netBufRemaining(b) != 0) {
		netHostKick(peer, NETREFUSE_BADMSG, "", "Your game's CONNECT did not parse.");
		return;
	}

	for (i = 0; c->name[i]; i++) {
		if ((u8)c->name[i] < 0x20) {
			c->name[i] = '?';
		}
	}

	if (strcmp(build, VERSION_HASH) != 0) {
		snprintf(text, sizeof(text), "The host's build is %s and yours is %s - both need the same build.", VERSION_HASH, build);
		netHostKick(peer, NETREFUSE_BUILD, "build", text);
		return;
	}

	if (romversion != VERSION) {
		snprintf(text, sizeof(text), "The host's Perfect Dark ROM is a different region or revision (%d) from yours (%u).", VERSION, romversion);
		netHostKick(peer, NETREFUSE_REGION, "region", text);
		return;
	}

	if (strcmp(geconv, GECONVERT_VERSION_STR) != 0) {
		snprintf(text, sizeof(text), "The host's GoldenEye conversion is version %s and yours is %s.", GECONVERT_VERSION_STR, geconv);
		netHostKick(peer, NETREFUSE_GECONVERT, "geconv", text);
		return;
	}

	nmine = netSessionHash(mine, NET_MAXCOMPS);

	for (i = 0; i < nmine; i++) {
		s32 found = 0;

		if (strcmp(mine[i].name, "all") == 0) {
			continue;
		}

		for (j = 0; j < ncomps; j++) {
			if (strcmp(theirs[j].name, mine[i].name) == 0) {
				found = 1;

				// the Stage Loader's other mods may differ: a match on a map
				// the client lacks is refused at STAGE_LOAD with the map and
				// its mod named (NOSTAGE), and the stage hash checks the
				// contents of the one played (protocol 8)
				if (theirs[j].hash != mine[i].hash && strcmp(mine[i].name, "mapmods") == 0) {
					sysLogPrintf(LOG_NOTE, "net: peer %d's Stage Loader map mods differ from this machine's (%016llx here, %016llx theirs); a map is checked when it is played",
							peer, (unsigned long long)mine[i].hash, (unsigned long long)theirs[j].hash);
				} else if (theirs[j].hash != mine[i].hash) {
					snprintf(text, sizeof(text), "Your %s differs from the host's (%s %016llx here, %016llx yours). Load the same ROM, mods and added content as the host.",
							netCompDescription(mine[i].name), mine[i].name,
							(unsigned long long)mine[i].hash, (unsigned long long)theirs[j].hash);
					netHostKick(peer, NETREFUSE_CONTENT, mine[i].name, text);
					return;
				}
			}
		}

		if (!found) {
			snprintf(text, sizeof(text), "Your game sent no %s hash.", mine[i].name);
			netHostKick(peer, NETREFUSE_CONTENT, mine[i].name, text);
			return;
		}
	}

	if (s_RequireTicket || s_LobbyRoomOn) {
		// a lobby room's own id and secret, else Net.RoomId/Net.RoomSecret
		const char *roomid = s_LobbyRoomOn ? s_LobbyRoomId : s_RoomId;
		const char *roomsecret = s_LobbyRoomOn ? s_LobbyRoomSecret : s_RoomSecret;
		u8 secret[32];
		char user[16];
		char why[96];

		for (i = 0; i < 32; i++) {
			unsigned int v = 0;

			if (sscanf(roomsecret + i * 2, "%2x", &v) != 1) {
				break;
			}

			secret[i] = (u8)v;
		}

		if (i != 32 || netTicketVerify(ticket, ticketlen, secret, roomid,
					netSessionLobbyNow(), user, sizeof(user), nonce, &nonceexpiry, why, sizeof(why)) != 0) {
			snprintf(text, sizeof(text), "This room needs a join ticket from the lobby: %s.", i != 32 ? "the host has no room secret" : why);
			netHostKick(peer, NETREFUSE_TICKET, "ticket", text);
			return;
		}

		// check 5: a lobby room's current roster, as the host's own state
		// poll last saw it (a kick or a leave is off it within a round trip)
		if (s_LobbyRoomOn && !netLobbyRosterHas(user)) {
			snprintf(text, sizeof(text), "This room needs a join ticket from the lobby: %s is not in the room.", user);
			netHostKick(peer, NETREFUSE_TICKET, "roster", text);
			return;
		}

		// the ticket's user is the player's name, not anything else CONNECT says
		snprintf(c->name, sizeof(c->name), "%s", user);
		ticketed = 1;
		c->ticketed = 1;
		sysLogPrintf(LOG_NOTE, "net: peer %d: lobby ticket for \"%s\" in room %s verified", peer, user, roomid);

		// one seat per user: a CONNECT for a user who holds a seat replaces
		// it (a restarted game) rather than taking a second, but only once
		// it has passed every check below
		if (s_LobbyRoomOn && !s_MatchActive) {
			for (j = 0; j < NET_MAXPEERS; j++) {
				if (j != peer && s_Clients[j].state >= NETCL_JOINED && s_Clients[j].state != NETCL_REFUSED
						&& strcasecmp(s_Clients[j].name, user) == 0 && s_Clients[j].spectator == c->spectator) {
					oldseat = j;
				}
			}
		}
	}

	// a match running already: its stage's keys too (a GoldenEye stage's)
	if (netRulesCheckClientKeys(c->keys, c->nkeys, s_MatchActive ? (modloaderStageIsRemake(s_MatchStage) ? 1 : 0) : -1,
				&code, key, sizeof(key), text, sizeof(text))) {
		netHostKick(peer, code, key, text);
		return;
	}

	// a match under way takes joiners once it runs (protocol 9): not while
	// it loads, nor once its end has gone out
	inprogress = s_MatchActive;

	if (inprogress && (!s_MatchLoaded || s_BarrierHeld || s_HostEnded || g_StageNum != s_MatchStage)) {
		netHostKick(peer, NETREFUSE_STARTED, "", "A match is starting or ending; try again in a moment.");
		return;
	}

	if (c->spectator) {
		c->slot = netHostFreeView(peer);

		if (c->slot < 0 && oldseat >= 0) {
			c->slot = s_Clients[oldseat].slot;
		}

		if (c->slot < 0) {
			netHostKick(peer, NETREFUSE_FULL, "", "No spectator places are left.");
			return;
		}
	} else if (inprogress) {
		c->slot = netSeatFor(peer, c->name, ticketed, &c->resumed, &oldpeer);

		if (c->slot < 0) {
			netHostKick(peer, NETREFUSE_FULL, "", "The match is full; join when it is over.");
			return;
		}
	} else {
		c->slot = netHostFreeSlot(peer);

		if (c->slot < 0 && oldseat >= 0) {
			c->slot = s_Clients[oldseat].slot; // the seat it replaces
		}

		if (c->slot < 0) {
			netHostKick(peer, NETREFUSE_FULL, "", "The game is full.");
			return;
		}
	}

	// the join is accepted: only now is the ticket spent
	if ((s_RequireTicket || s_LobbyRoomOn) && netTicketUse(nonce, nonceexpiry, netSessionLobbyNow()) != 0) {
		netHostKick(peer, NETREFUSE_TICKET, "ticket", "This room needs a join ticket from the lobby: the ticket was used already.");
		return;
	}

	c->state = NETCL_JOINED;

	// a seat of the running match: this peer's from now (an old peer of
	// the same account that still holds it is dropped below, and finds the
	// seat no longer its own)
	if (inprogress && !c->spectator) {
		struct netseat *seat = &s_Seats[c->slot];

		if (c->resumed) {
			netHostLogSeats("before a seat is taken back");
		}

		seat->state = NETSEAT_TAKEN;
		seat->peer = peer;
		seat->until = 0;
		seat->ticketed = c->ticketed;
		snprintf(seat->account, sizeof(seat->account), "%s", c->name);
		netNameSet(g_PlayerConfigsArray[c->slot].base.name, sizeof(g_PlayerConfigsArray[c->slot].base.name), c->name);
	}

	if (oldseat >= 0) {
		netHostKick(oldseat, NETREFUSE_TICKET, "roster", "You connected again from another game.");
	}

	if (oldpeer >= 0) {
		netHostKick(oldpeer, NETREFUSE_TICKET, "roster", "You connected again from another game.");
	}

	accflags = (inprogress ? NETACC_INPROGRESS : 0) | (c->resumed ? NETACC_RESUMED : 0) | (c->spectator ? NETACC_SPECTATOR : 0);

	netBufInitWrite(b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(b, NETMSG_ACCEPT);
	netBufWriteU8(b, c->spectator ? NETSLOT_SPECTATOR : (u8)c->slot);
	netBufWriteU32(b, g_NetTick);
	netBufWriteU8(b, netIsDedicatedHost() ? 1 : 0);
	netWriteStr(b, s_Name, NET_MAXNAME);
	netBufWriteU8(b, accflags);
	netSend(peer, NET_CHAN_RELIABLE, b);

	addr[0] = '\0';

	if (netHostPeerAddr(g_NetHostSocket, peer, &na) == 0) {
		netAddrToString(&na, addr, sizeof(addr));
	}

	sysLogPrintf(LOG_NOTE, "net: %s %d: \"%s\" joined from %s (fov %.0f, aspect %.2f, head %d body %d)%s",
			c->spectator ? "spectator view" : "slot", c->slot, c->name, addr, c->cfg.fovy, c->cfg.aspect, c->cfg.mpheadnum, c->cfg.mpbodynum,
			!inprogress ? "" : c->spectator ? "; spectating the match in progress" : c->resumed ? "; the seat it held, back in the match in progress" : "; an open seat of the match in progress");

	if (inprogress) {
		netHostSendStage(peer);
		c->late = 1;
		c->state = NETCL_LOADING;
		c->gotloaded = 0;
		c->nloaded = 0;
		c->lobbymatch = 0;
		c->since = netNowMs();
	}

	if (s_TestHost) {
		s_TestReadyAt = netNowMs() + 1000;
	}
}

/*
 * Host: the match
 */

static s32 netHostJoinedSlots(void)
{
	s32 bits = netIsDedicatedHost() ? 0 : 1;
	s32 i;

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state == NETCL_JOINED && s_Clients[i].slot >= 0 && !s_Clients[i].spectator) {
			bits |= 1 << s_Clients[i].slot;
		}
	}

	return bits;
}

/**
 * A spectator's view (MAX_PLAYERS + k) no other client has, or -1
 */
static s32 netHostFreeView(s32 peer)
{
	s32 view;
	s32 i;

	for (view = MAX_PLAYERS; view < NET_MAXVIEWS; view++) {
		s32 taken = 0;

		for (i = 0; i < NET_MAXPEERS; i++) {
			if (i != peer && s_Clients[i].state >= NETCL_JOINED && s_Clients[i].state != NETCL_REFUSED
					&& s_Clients[i].slot == view) {
				taken = 1;
			}
		}

		if (!taken) {
			return view;
		}
	}

	return -1;
}

/**
 * A seat of the running match for a joiner: the one its account holds
 * (held since a drop, or still taken by a peer of the same account that has
 * not timed out yet), else the first open one no other client still has as
 * its slot (one on the last match's end screen keeps its own), else -1.
 * Only a lobby ticket names an account: CONNECT's name is anyone's to send,
 * so a joiner without one is never given a seat back.
 */
static s32 netSeatFor(s32 peer, const char *account, s32 ticketed, s32 *resumed, s32 *oldpeer)
{
	s32 i;
	s32 j;

	*resumed = 0;
	*oldpeer = -1;

	for (i = 0; i < MAX_PLAYERS && ticketed; i++) {
		struct netseat *seat = &s_Seats[i];

		if (!seat->ticketed || strcasecmp(seat->account, account) != 0) {
			continue;
		}

		if (seat->state == NETSEAT_HELD) {
			*resumed = 1;
			return i;
		}

		if (seat->state == NETSEAT_TAKEN && seat->peer != peer) {
			*resumed = 1;
			*oldpeer = seat->peer;
			return i;
		}
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		s32 owned = 0;

		if (s_Seats[i].state != NETSEAT_OPEN) {
			continue;
		}

		for (j = 0; j < NET_MAXPEERS; j++) {
			if (j != peer && s_Clients[j].state >= NETCL_JOINED && s_Clients[j].state != NETCL_REFUSED
					&& !s_Clients[j].spectator && s_Clients[j].slot == i) {
				owned = 1;
			}
		}

		if (!owned) {
			return i;
		}
	}

	return -1;
}

// The match's player of a seat (mpindex), or -1
static s32 netSeatPlayer(s32 slot)
{
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Vars.players[i] && g_Vars.playerstats[i].mpindex == slot) {
			return i;
		}
	}

	return -1;
}

/**
 * A mod dir's last name, without the trailing slashes a typed or completed
 * --moddir may carry ("mod_allinone/" and "mod_allinone" are one mod)
 */
static const char *netBasename(const char *path, char *buf, s32 size)
{
	s32 end = strlen(path);
	s32 start;

	while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
		end--;
	}

	start = end;

	while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') {
		start--;
	}

	snprintf(buf, size, "%.*s", end - start, path + start);

	return buf;
}

static void netWriteStageKey(struct netbuf *b, s32 stagenum, char *label, s32 labelsize)
{
	const char *dir = modloaderGetStageModDir(stagenum);
	char base[NET_MAXMAPDIR + 1];

	if (dir) {
		const char *map = modloaderGetStageMapName(stagenum);

		netBasename(dir, base, sizeof(base));
		netBufWriteU8(b, 1);
		netWriteStr(b, base, NET_MAXMAPDIR);
		netWriteStr(b, map ? map : "", NET_MAXMAPNAME);
		snprintf(label, labelsize, "%s (%s)", map ? map : "?", base);
	} else if (fsGetModDir()) {
		netBasename(fsGetModDir(), base, sizeof(base));
		netBufWriteU8(b, 2);
		netWriteStr(b, base, NET_MAXMAPDIR);
		netBufWriteU8(b, (u8)stagenum);
		snprintf(label, labelsize, "stage 0x%02x of %s", stagenum, base);
	} else {
		netBufWriteU8(b, 0);
		netBufWriteU8(b, (u8)stagenum);
		snprintf(label, labelsize, "stage 0x%02x", stagenum);
	}
}

static u64 netMix(u64 x)
{
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;

	return x;
}

/**
 * RULES and STAGE_LOAD for the match (s_MatchId on s_MatchStage) to one
 * client: at H1, or to a join in progress. The players are chrslots' bits
 * 0-3 and g_MpHumanSlotsHi's 4-11 (mpHumanSlotMask); a spectator is none of them.
 */
static void netHostSendStage(s32 peer)
{
	const struct netclient *c = &s_Clients[peer];
	const s32 bits = mpHumanSlotMask();
	char label[NET_MAXMAPDIR + NET_MAXMAPNAME + 8];
	struct netbuf b;
	s32 numplayers = 0;
	s32 yourplayer = 0;
	s32 k;

	for (k = 0; k < MAX_PLAYERS; k++) {
		numplayers += (bits >> k) & 1;
	}

	if (c->spectator) {
		yourplayer = NETSLOT_SPECTATOR;
	} else {
		for (k = 0; k < c->slot; k++) {
			yourplayer += (bits >> k) & 1;
		}
	}

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netRulesWrite(&b, s_MatchId);
	netSend(peer, NET_CHAN_BULK, &b);

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(&b, NETMSG_STAGE_LOAD);
	netBufWriteU32(&b, s_MatchId);
	netWriteStageKey(&b, s_MatchStage, label, sizeof(label));
	netWriteStr(&b, label, NET_MAXNAME);
	netWriteU64(&b, s_Seed);
	netWriteU64(&b, s_Seed2);
	netBufWriteU8(&b, (u8)numplayers);
	netBufWriteU8(&b, (u8)yourplayer);
	netSend(peer, NET_CHAN_BULK, &b);
}

/**
 * H1: every random choice of the match is made (mpStartMatch did them just
 * before), so the host sends the result. Returns the human count: the host
 * (unless dedicated) and every joined client, each in its own slot.
 */
s32 netHostMatchStarting(s32 stagenum, s32 numplayers)
{
	char label[NET_MAXMAPDIR + NET_MAXMAPNAME + 8];
	char text[NET_MAXTEXT + 1];
	char key[NET_MAXKEY + 1];
	struct netbuf b;
	s32 bits;
	s32 gestage = modloaderStageIsRemake(stagenum) ? 1 : 0;
	s32 seedarg = sysArgGetInt("--rng-seed", -1);
	s32 i;

	if (s_Role != NETROLE_HOST || !g_NetHostSocket) {
		return numplayers;
	}

	// a GoldenEye stage: the keys that matter only there must match now
	for (i = 0; i < NET_MAXPEERS; i++) {
		s32 code;

		if (s_Clients[i].state == NETCL_JOINED
				&& netRulesCheckClientKeys(s_Clients[i].keys, s_Clients[i].nkeys, gestage, &code, key, sizeof(key), text, sizeof(text))) {
			netHostKick(i, code, key, text);
		}
	}

	netRulesSaveHost();

	bits = netHostJoinedSlots();

	// Net.JoinInProgress: every seat is in the match; the ones nobody has
	// go out of play once it runs, and wait for a joiner
	// A lobby room's too: pdlobbyd lets a launched room be joined
	if (s_JoinInProgress || s_LobbyRoomOn) {
		bits = (1 << netHostSeatCount()) - 1;
	}

	memset(s_Seats, 0, sizeof(s_Seats));
	s_HostEnded = 0;

	for (i = 0; i < MAX_PLAYERS; i++) {
		s_Seats[i].peer = -1;

		if (!(bits & (1 << i))) {
			continue;
		}

		if (i == 0 && !netIsDedicatedHost()) {
			s_Seats[i].state = NETSEAT_HOST;
			snprintf(s_Seats[i].account, sizeof(s_Seats[i].account), "%s", s_Name);

			// a lobby room's host plays as its account on its own machine
			// too: the HUD text it builds and forwards ("Killed by %s", "Get
			// %s!") names the same player the members' scoreboards do. Its
			// profile's name comes back at the match's end (H9), before any
			// end screen or profile save.
			if (s_LobbyRoomOn && s_Name[0]) {
				if (!s_HostNameOn) {
					snprintf(s_HostOwnName, sizeof(s_HostOwnName), "%s", g_PlayerConfigsArray[0].base.name);
					s_HostNameOn = 1;
				}

				netNameSet(g_PlayerConfigsArray[0].base.name, sizeof(g_PlayerConfigsArray[0].base.name), s_Name);
				sysLogPrintf(LOG_NOTE, "net: lobby host: slot 0 plays as \"%.*s\" (its profile \"%.*s\" back at the end)",
						netNameLen(g_PlayerConfigsArray[0].base.name), g_PlayerConfigsArray[0].base.name,
						netNameLen(s_HostOwnName), s_HostOwnName);
			}
		} else {
			s_Seats[i].state = NETSEAT_OPEN;
			s_Seats[i].vacate = 1;
			netNameSet(g_PlayerConfigsArray[i].base.name, sizeof(g_PlayerConfigsArray[i].base.name), "(open)");
		}
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		struct netclient *c = &s_Clients[i];

		if (c->state == NETCL_JOINED && !c->spectator && c->slot >= 0 && c->slot < MAX_PLAYERS) {
			s_Seats[c->slot].state = NETSEAT_TAKEN;
			s_Seats[c->slot].peer = i;
			s_Seats[c->slot].vacate = 0;
			s_Seats[c->slot].ticketed = c->ticketed;
			snprintf(s_Seats[c->slot].account, sizeof(s_Seats[c->slot].account), "%s", c->name);
		}
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state == NETCL_AWAY) {
			sysLogPrintf(LOG_NOTE, "net: slot %d (\"%s\") is still on the last match's end screen; it sits this match out",
					s_Clients[i].slot, s_Clients[i].name);
		}
	}

	// a challenge is never marked complete from a match with remote players
	// (the lock comes back at H12)
	if ((bits & ~1) && g_BossFile.locktype == MPLOCKTYPE_CHALLENGE) {
		sysLogPrintf(LOG_NOTE, "net: the challenge lock is off for this net match");
		g_BossFile.locktype = MPLOCKTYPE_NONE;
	}
	numplayers = 0;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (bits & (1 << i)) {
			numplayers++;
		}
	}

	netPlayersHostMatchStart();

	for (i = 0; i < NET_MAXPEERS; i++) {
		struct netclient *c = &s_Clients[i];

		if (c->state == NETCL_JOINED && !c->spectator) {
			struct mpplayerconfig *p = &g_PlayerConfigsArray[c->slot];

			netNameSet(p->base.name, sizeof(p->base.name), c->name);
			p->base.mpheadnum = c->cfg.mpheadnum;
			p->base.mpbodynum = c->cfg.mpbodynum;
			memset(&p->fileguid, 0, sizeof(p->fileguid));

			// its controls, settings and screen (before RULES: they carry
			// the slot's options)
			netPlayersHostSlotStart(c->slot, &c->cfg);
		}
	}

	mpSetHumanSlotMask(bits);

	s_MatchId++;
	s_MatchIdCur = s_MatchId;

	if (seedarg >= 0) {
		s_Seed = netMix((u64)seedarg * 0x9e3779b97f4a7c15ULL + s_MatchId);
	} else {
		s_Seed = netMix(sysGetMicroseconds() ^ ((u64)osGetCount() << 32) ^ (u64)time(NULL));
	}

	s_Seed2 = netMix(s_Seed ^ 0x5bd1e9955bd1e995ULL);

	s_MatchStage = stagenum;

	for (i = 0; i < NET_MAXPEERS; i++) {
		struct netclient *c = &s_Clients[i];

		if (c->state != NETCL_JOINED) {
			continue;
		}

		netHostSendStage(i);
		c->state = NETCL_LOADING;
		c->late = 0;
		c->gotloaded = 0;
		c->nloaded = 0;
		c->lobbymatch = 0;
	}

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netWriteStageKey(&b, stagenum, label, sizeof(label));

	s_MatchActive = 1;
	s_MatchStage = stagenum;
	s_SeedPending = 1;
	s_BarrierHeld = 1;
	s_HostLoaded = 0;
	s_LoadDeadline = 0;
	netRulesSetLocked(1);

	sysLogPrintf(LOG_NOTE, "net: match %u starting on %s, %d players (slots 0x%x), seeds %016llx %016llx",
			s_MatchId, label, numplayers, bits, (unsigned long long)s_Seed, (unsigned long long)s_Seed2);

	return numplayers;
}

static s32 netCompsDiffer(const struct nethashcomp *a, s32 na, const struct nethashcomp *b, s32 nb, char *which, s32 whichsize)
{
	s32 i;
	s32 j;

	for (i = 0; i < na; i++) {
		s32 found = 0;

		// model packs and XBLA meshes may differ: a diagnostic only
		if (strcmp(a[i].name, "models") == 0) {
			continue;
		}

		for (j = 0; j < nb; j++) {
			if (strcmp(a[i].name, b[j].name) == 0) {
				found = 1;

				if (a[i].hash != b[j].hash) {
					snprintf(which, whichsize, "%s", a[i].name);
					return 1;
				}
			}
		}

		if (!found) {
			snprintf(which, whichsize, "%s", a[i].name);
			return 1;
		}
	}

	return 0;
}

static void netHostCheckLoaded(s32 peer)
{
	struct netclient *c = &s_Clients[peer];
	char which[NET_MAXCOMPNAME + 1];
	char text[NET_MAXTEXT + 1];
	s32 i;

	if (c->state != NETCL_LOADING || !c->gotloaded || !s_HostLoaded) {
		return;
	}

	if (netCompsDiffer(s_HostComps, s_HostNumComps, c->loaded, c->nloaded, which, sizeof(which))) {
		u64 h = 0;
		u64 t = 0;

		for (i = 0; i < s_HostNumComps; i++) {
			if (strcmp(s_HostComps[i].name, which) == 0) {
				h = s_HostComps[i].hash;
			}
		}

		for (i = 0; i < c->nloaded; i++) {
			if (strcmp(c->loaded[i].name, which) == 0) {
				t = c->loaded[i].hash;
			}
		}

		snprintf(text, sizeof(text), "The stage loaded differently here: its %s is %016llx on the host and %016llx on yours.",
				which, (unsigned long long)h, (unsigned long long)t);
		netHostKick(peer, NETREFUSE_STAGEHASH, which, text);
		return;
	}

	c->state = NETCL_LOADED;
	sysLogPrintf(LOG_NOTE, "net: slot %d (\"%s\") loaded the stage, every component the host's", c->slot, c->name);
}

static void netHostOnLoaded(s32 peer, struct netbuf *b)
{
	struct netclient *c = &s_Clients[peer];
	u32 matchid = netBufReadU32(b);
	s32 i;

	c->nloaded = netBufReadU8(b);

	if (c->nloaded > NET_MAXCOMPS) {
		b->error = 1;
	}

	for (i = 0; i < c->nloaded && netBufOk(b); i++) {
		netBufReadString(b, c->loaded[i].name, sizeof(c->loaded[i].name));
		c->loaded[i].hash = netReadU64(b);
	}

	if (!netBufOk(b) || netBufRemaining(b) != 0) {
		netHostKick(peer, NETREFUSE_BADMSG, "", "Your game's LOADED did not parse.");
		return;
	}

	if (matchid != s_MatchIdCur || c->state != NETCL_LOADING) {
		return;
	}

	c->gotloaded = 1;
	netHostCheckLoaded(peer);
}

static void netHostBarrierTick(void)
{
	struct netbuf b;
	s32 waiting = 0;
	s32 i;

	if (!s_MatchActive || !s_BarrierHeld || !s_HostLoaded) {
		return;
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state == NETCL_LOADING && !s_Clients[i].late) {
			if (netNowMs() >= s_LoadDeadline) {
				netHostKick(i, NETREFUSE_TIMEOUT, "", "Your game took too long to load the stage.");
			} else {
				waiting++;
			}
		}
	}

	if (waiting) {
		return;
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state == NETCL_LOADED) {
			netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
			netBufWriteU8(&b, NETMSG_GO);
			netBufWriteU32(&b, s_MatchIdCur);
			netBufWriteU32(&b, 0);
			netBufWriteS32(&b, 0);
			netSend(i, NET_CHAN_RELIABLE, &b);
			s_Clients[i].state = NETCL_PLAYING;

			// a spectator's view: its acks are taken from now
			if (s_Clients[i].spectator) {
				netPlayersHostSlotJoin(s_Clients[i].slot, &s_Clients[i].cfg, 0);
			}
		}
	}

	s_BarrierHeld = 0;
	netHostFlush(g_NetHostSocket);
	sysLogPrintf(LOG_NOTE, "net: match %u: every machine has loaded; GO", s_MatchIdCur);
}

/*
 * Host: joins in progress, spectators and held seats (protocol 9)
 */

static const char *const s_SeatNames[] = { "none", "host", "taken", "open", "held" };

// ROSTER to every client in the match: who sits where
static void netHostSendRoster(void)
{
	struct netbuf b;
	s32 i;

	if (!s_MatchActive) {
		return;
	}

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(&b, NETMSG_ROSTER);
	netBufWriteU32(&b, s_MatchIdCur);
	netBufWriteU8(&b, MAX_PLAYERS);

	for (i = 0; i < MAX_PLAYERS; i++) {
		netBufWriteU8(&b, (u8)s_Seats[i].state);
		netWriteStr(&b, netSessionWireName(i), 14);
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state >= NETCL_LOADING && s_Clients[i].state <= NETCL_PLAYING) {
			netSend(i, NET_CHAN_RELIABLE, &b);
		}
	}
}

static void netHostLogSeats(const char *why)
{
	char line[1024];
	s32 len = 0;
	s32 i;

	line[0] = '\0';

	for (i = 0; i < MAX_PLAYERS && len < (s32)sizeof(line); i++) {
		const s32 pn = netSeatPlayer(i);
		const struct mpchrconfig *mpchr = MPCHR(i);
		char name[16];
		s32 kills = 0;
		s32 j;

		// seats past the room's size are never in the match
		if (i >= MAX_LOCAL_PLAYERS && s_Seats[i].state == NETSEAT_NONE) {
			continue;
		}

		for (j = 0; j < MAX_MPCHRS; j++) {
			kills += j != i ? mpchr->killcounts[j] : 0;
		}

		// a name in the game's text ends at its newline
		snprintf(name, sizeof(name), "%s", s_Seats[i].state == NETSEAT_NONE ? "" : g_PlayerConfigsArray[i].base.name);
		name[strcspn(name, "\n")] = '\0';

		len += snprintf(line + len, sizeof(line) - len, "%s%d %s \"%s\" points %d deaths %d kills %d%s", i ? "; " : "", i,
				s_SeatNames[s_Seats[i].state], name,
				mpchr->numpoints, mpchr->numdeaths, kills, pn >= 0 && g_Vars.players[pn]->isdead ? " (dead)" : "");
	}

	sysLogPrintf(LOG_NOTE, "net: seats %s (tick %u): %s; joins %u (back to a held seat %u), spectators %u, holds run out %u, out of play %u",
			why, g_NetTick, line, s_Joins, s_Resumes, s_SpecJoins, s_HoldsExpired, s_Vacated);
}

/**
 * A seat's player out of play: dead without a death on anyone's score,
 * hidden and solid to nothing. Its pad is nobody's, so it never asks to
 * respawn; a joiner's seat respawns it (netHostLateGo). One that was in
 * play (its player left, or its hold ran out) drops what it carries first,
 * as a death does, so a briefcase, an uplink or a case stays in the match;
 * Pop a Cap passes over it (netScenHostPacCheck).
 */
static void netSeatVacate(s32 slot, s32 drop)
{
	const s32 pn = netSeatPlayer(slot);
	struct player *p = pn >= 0 ? g_Vars.players[pn] : NULL;
	s32 prev;

	if (!p || !p->prop || !p->prop->chr) {
		return;
	}

	prev = g_Vars.currentplayernum;
	setCurrentPlayerNum(pn);

	if (!p->isdead) {
		const bool running = g_Vars.mplayerisrunning;

		// mpstatsRecordDeath and the dropped guns go with mplayerisrunning
		if (drop) {
			currentPlayerDropAllItems();
		}

		g_Vars.mplayerisrunning = false;
		playerDieByShooter(pn, true);
		g_Vars.mplayerisrunning = running;
	}

	p->prop->chr->chrflags |= CHRCFLAG_HIDDEN;
	propSetPerimEnabled(p->prop, false);
	setCurrentPlayerNum(prev);
	s_Vacated++;
}

/**
 * At the head of each host tick: seats whose player goes out of play, holds
 * that ran out
 */
static void netHostSeatsTick(void)
{
	s32 changed = 0;
	s32 clear = 0;
	s32 i;

	if (s_Role != NETROLE_HOST || !netSessionMatchActive() || s_BarrierHeld || s_HostEnded) {
		return;
	}

	if (s_TestGiveTick && g_NetTick == s_TestGiveTick && netSeatPlayer(s_TestGiveSlot) >= 0 && g_MpNumChrs > PLAYERCOUNT()) {
		for (i = 0; i < s_TestGiveN; i++) {
			mpstatsRecordDeath(netSeatPlayer(s_TestGiveSlot), g_MpNumChrs - 1);
		}

		sysLogPrintf(LOG_NOTE, "net: --net-test-givekills: %d kills for slot %d at tick %u", s_TestGiveN, s_TestGiveSlot, g_NetTick);
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct netseat *seat = &s_Seats[i];

		if (seat->state == NETSEAT_HELD && netNowMs() >= seat->until) {
			sysLogPrintf(LOG_NOTE, "net: seat %d (\"%s\"): the hold ran out (%d s); the seat is open, its score cleared",
					i, seat->account, s_ReconnectHold);
			seat->state = NETSEAT_OPEN;
			seat->vacate = 1;
			seat->drop = 1;
			seat->clear = 1;
			seat->ticketed = 0;
			seat->account[0] = '\0';
			netNameSet(g_PlayerConfigsArray[i].base.name, sizeof(g_PlayerConfigsArray[i].base.name), "(open)");
			s_HoldsExpired++;
			changed = 1;
		}

		if (seat->vacate && seat->state == NETSEAT_OPEN) {
			seat->vacate = 0;
			netSeatVacate(i, seat->drop);
			seat->drop = 0;
		}

		if (seat->clear) {
			struct mpchrconfig *mpchr = MPCHR(i);
			s32 j;

			seat->clear = 0;
			mpchr->numdeaths = 0;
			mpchr->numpoints = 0;

			for (j = 0; j < MAX_MPCHRS; j++) {
				mpchr->killcounts[j] = 0;
			}

			// the scenario's own count of it too (the snapshots' block
			// carries it to the clients)
			netScenHostSeatCleared(i);
			clear = 1;
		}
	}

	// Pop a Cap's victim gone out of play: the next one now
	netScenHostPacCheck();

	if (clear) {
		netEventsHostScores();
	}

	if (changed) {
		netHostSendRoster();
		netHostLogSeats("after a hold ran out");
	}
}

/**
 * A client of the match is gone (a drop, a LEAVE, a kick): its seat is held
 * for its account (a drop) or opened (it left), unless another peer has
 * the seat by now (a reconnect that took it over)
 */
static void netHostPeerGone(s32 peer, s32 held)
{
	struct netclient *c = &s_Clients[peer];
	struct netseat *seat;

	if (c->spectator) {
		if (c->state >= NETCL_LOADING && c->state <= NETCL_PLAYING) {
			netPlayersHostSpecGone(c->slot);
			sysLogPrintf(LOG_NOTE, "net: spectator view %d (\"%s\") left", c->slot, c->name);
		}

		return;
	}

	if (!s_MatchActive || c->slot < 0 || c->slot >= MAX_PLAYERS) {
		return;
	}

	seat = &s_Seats[c->slot];

	if (seat->state != NETSEAT_TAKEN || seat->peer != peer) {
		return;
	}

	seat->peer = -1;

	// held only for a ticket's account (nothing else can claim it back),
	// from when its game last spoke: the transport notices a dead peer
	// seconds later
	if (held && seat->ticketed && s_ReconnectHold > 0 && !s_HostEnded) {
		const u64 now = netNowMs();
		const u64 from = c->lastrecv && c->lastrecv <= now ? c->lastrecv : now;

		seat->state = NETSEAT_HELD;
		seat->until = from + (u64)s_ReconnectHold * 1000;
		sysLogPrintf(LOG_NOTE, "net: seat %d (\"%s\") dropped; held for it %d s from its last message (%llu ms ago)",
				c->slot, seat->account, s_ReconnectHold, (unsigned long long)(now - from));
		netHostLogSeats("after a drop");
	} else {
		sysLogPrintf(LOG_NOTE, "net: seat %d is open (\"%s\" %s)", c->slot, c->name,
				!held ? "left" : !seat->ticketed && s_ReconnectHold > 0 && !s_HostEnded ? "dropped; no lobby ticket to come back on" : "dropped");
		seat->state = NETSEAT_OPEN;
		seat->vacate = 1;
		seat->drop = 1;
		seat->clear = 1;
		seat->ticketed = 0;
		seat->account[0] = '\0';
		netNameSet(g_PlayerConfigsArray[c->slot].base.name, sizeof(g_PlayerConfigsArray[c->slot].base.name), "(open)");
	}

	netHostSendRoster();
}

/**
 * A join in progress has loaded the stage and its hash matched: GO with
 * the tick the host runs next, then its first EVENTS (the kill table) and
 * the seats. Its player comes back into play, or a spectator's view opens.
 */
static void netHostLateGo(s32 peer)
{
	struct netclient *c = &s_Clients[peer];
	struct netbuf b;
	const u32 tick = g_NetTick;

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(&b, NETMSG_GO);
	netBufWriteU32(&b, s_MatchIdCur);
	netBufWriteU32(&b, tick);
	netBufWriteS32(&b, lvGetStageTime60());
	netSend(peer, NET_CHAN_RELIABLE, &b);

	c->state = NETCL_PLAYING;
	c->late = 0;

	netEntsHostViewReset(c->slot);
	netEventsHostViewReset(c->slot);

	if (c->spectator) {
		s_SpecJoins++;
		netPlayersHostSlotJoin(c->slot, &c->cfg, tick);
	} else {
		const s32 pn = netSeatPlayer(c->slot);

		netPlayersHostSlotJoin(c->slot, &c->cfg, tick);

		if (pn >= 0 && g_Vars.players[pn]->isdead) {
			// back into play at the next respawn point
			g_Vars.players[pn]->dostartnewlife = true;
		}

		if (pn >= 0 && g_Vars.players[pn]->prop) {
			propSetPerimEnabled(g_Vars.players[pn]->prop, true);
		}

		s_Joins++;
		s_Resumes += c->resumed != 0;
	}

	netEventsHostCatchUp(c->slot);
	netHostSendRoster();
	netHostFlush(g_NetHostSocket);

	sysLogPrintf(LOG_NOTE, "net: %s %d (\"%s\") is in the match in progress from tick %u (%s)",
			c->spectator ? "spectator view" : "slot", c->slot, c->name, tick,
			c->spectator ? "spectating" : c->resumed ? "its held seat, its score kept" : "an open seat");
	netHostLogSeats("after a join");
}

// Between ticks: a join in progress loaded, or too slow to
static void netHostLateTick(void)
{
	s32 i;

	if (!s_MatchActive) {
		return;
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		struct netclient *c = &s_Clients[i];

		if (!c->late) {
			continue;
		}

		if (c->state == NETCL_LOADED && !s_BarrierHeld && !s_HostEnded && netSessionMatchActive()) {
			netHostLateGo(i);
		} else if (c->state == NETCL_LOADING && netNowMs() - c->since > NET_LOAD_TIMEOUT_MS * 4) {
			netHostKick(i, NETREFUSE_TIMEOUT, "", "Your game took too long to load the stage.");
		}
	}
}

// A lobby host: its profile's own name back in slot 0 (see netHostMatchStarting)
static void netHostOwnNameBack(void)
{
	if (s_HostNameOn) {
		snprintf(g_PlayerConfigsArray[0].base.name, sizeof(g_PlayerConfigsArray[0].base.name), "%s", s_HostOwnName);
		s_HostNameOn = 0;
		sysLogPrintf(LOG_NOTE, "net: lobby host: slot 0's profile name is back");
	}
}

/**
 * H9, after mpEndMatch has worked out the awards: the clients end theirs on
 * the host's numbers
 */
void netHostMatchEnded(void)
{
	struct netbuf b;
	s32 i;
	s32 j;

	netHostOwnNameBack();

	if (s_Role != NETROLE_HOST || !s_MatchActive || !g_NetHostSocket) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "net: match clock at the end: tick %u, level time %d (60ths), time limit %d, score limit %d, other reasons %d",
			g_NetTick, lvGetStageTime60(), g_MpTimeLimit60, g_MpScoreLimit, g_NumReasonsToEndMpMatch);
	s_HostEnded = 1;
	netHostLogSeats("at the match's end");

	// the last tick's events (its deaths) on the same channel, first
	netEventsHostMatchEnded();

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(&b, NETMSG_MATCH_END);
	netBufWriteU32(&b, s_MatchIdCur);
	netBufWriteU8(&b, (u8)PLAYERCOUNT());

	for (i = 0; i < PLAYERCOUNT(); i++) {
		u8 award[2] = { 0xff, 0xff };
		char *given[2] = { g_Vars.players[i]->award1, g_Vars.players[i]->award2 };
		s32 a;

		for (a = 0; a < 2; a++) {
			for (j = 0; j < 17 && given[a]; j++) {
				if (given[a] == langGet(g_AwardNames[j])) {
					award[a] = (u8)j;
					break;
				}
			}
		}

		netBufWriteU8(&b, award[0]);
		netBufWriteU8(&b, award[1]);
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		netBufWriteU8(&b, g_PlayerConfigsArray[i].medals);
		netBufWriteU8(&b, g_PlayerConfigsArray[i].title);
	}

	netBufWriteU8(&b, MAX_MPCHRS);

	for (i = 0; i < MAX_MPCHRS; i++) {
		struct mpchrconfig *mpchr = MPCHR(i);

		netBufWriteS8(&b, mpchr->placement);
		netBufWriteS32(&b, mpchr->rankablescore);
		netBufWriteS16(&b, mpchr->numdeaths);
		netBufWriteS16(&b, mpchr->numpoints);

		for (j = 0; j < MAX_MPCHRS; j++) {
			netBufWriteS16(&b, mpchr->killcounts[j]);
		}
	}

	// the scenario's state at the end (protocol 7; netscen.c's block)
	{
		static u8 scen[NETSCEN_SIZE];

		netScenHostFinal(scen);
		netBufWriteU16(&b, NETSCEN_SIZE);
		netBufWriteBytes(&b, scen, NETSCEN_SIZE);
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		// a client still loading too: it would otherwise wait at the barrier
		if (s_Clients[i].state >= NETCL_LOADING && s_Clients[i].state <= NETCL_PLAYING) {
			netSend(i, NET_CHAN_RELIABLE, &b);
		}
	}

	netHostFlush(g_NetHostSocket);
	sysLogPrintf(LOG_NOTE, "net: match %u ended; scores and awards sent (%d bytes)", s_MatchIdCur, netBufLen(&b));
}

/*
 * Client
 */

static void netClientSendConnect(void)
{
	struct nethashcomp comps[NET_MAXCOMPS];
	struct netbuf b;
	s32 n = netSessionHash(comps, NET_MAXCOMPS);
	s32 len = strlen(s_Ticket);
	s32 i;

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(&b, NETMSG_CONNECT);
	netBufWriteU16(&b, (u16)s_ProtocolSent);
	netWriteStr(&b, VERSION_HASH, NET_MAXBUILD);
	netWriteStr(&b, GECONVERT_VERSION_STR, NET_MAXCOMPNAME);
	netBufWriteU8(&b, VERSION);
	netBufWriteU8(&b, (u8)n);

	for (i = 0; i < n; i++) {
		netWriteStr(&b, comps[i].name, NET_MAXCOMPNAME);
		netWriteU64(&b, comps[i].hash);
	}

	netWriteStr(&b, s_Name, NET_MAXNAME);
	netWriteSlotCfg(&b);
	netRulesWriteClientKeys(&b);
	netBufWriteU16(&b, (u16)len);
	netBufWriteBytes(&b, s_Ticket, len);
	netBufWriteU8(&b, s_Spectate ? NETCONN_SPECTATE : 0);

	netSend(s_ServerPeer, NET_CHAN_RELIABLE, &b);
	s_ClientState = NETCS_HELLO;
}

/**
 * The player's settings again, now the window is up (CONNECT's aspect is
 * not) and whenever it is back in the lobby, where they may have changed
 */
static u8 s_SlotCfgSent[128];
static s32 s_SlotCfgSentLen = 0;

static void netClientSendSlotCfg(void)
{
	struct netbuf b;

	netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(&b, NETMSG_SLOTCFG);
	netWriteSlotCfg(&b);
	netSend(s_ServerPeer, NET_CHAN_RELIABLE, &b);

	if (netBufOk(&b) && netBufLen(&b) <= (s32)sizeof(s_SlotCfgSent)) {
		memcpy(s_SlotCfgSent, s_Buf, netBufLen(&b));
		s_SlotCfgSentLen = netBufLen(&b);
	}
}

/**
 * Mid-match, from netPlayersTickBegin: the pause menu can change the
 * player's settings (crouch mode, aim, fov, the mouse's signs) and the
 * host simulates this player with them, so a change goes again. Looked
 * at twice a second, which also keeps a window being dragged to a new
 * aspect from sending one every tick.
 */
void netSessionClientCfgTick(void)
{
	u8 buf[sizeof(s_SlotCfgSent)];
	struct netbuf b;

	if (g_NetMode != NETMODE_CLIENT || s_ServerPeer < 0 || s_ServerClosed || g_NetTick % 30 != 0) {
		return;
	}

	netBufInitWrite(&b, buf, sizeof(buf));
	netBufWriteU8(&b, NETMSG_SLOTCFG);
	netWriteSlotCfg(&b);

	if (netBufOk(&b) && (netBufLen(&b) != s_SlotCfgSentLen || memcmp(buf, s_SlotCfgSent, netBufLen(&b)) != 0)) {
		sysLogPrintf(LOG_NOTE, "net: settings changed mid-match; sent to the host");
		netClientSendSlotCfg();
	}
}

/**
 * A client about to go offline services the socket a moment longer, so
 * its goodbye (LEAVE, then the queued disconnect) reaches the host rather
 * than the host timing it out
 */
static void netClientDrain(void)
{
	struct netevent ev;
	u64 until = netNowMs() + 250;
	s32 r;

	if (!g_NetHostSocket || s_ServerPeer < 0 || s_ServerClosed) {
		return;
	}

	netHostFlush(g_NetHostSocket);

	while (netNowMs() < until) {
		r = netHostService(g_NetHostSocket, &ev, 10);

		if (r < 0) {
			break;
		}

		if (r > 0 && ev.type == NETEVENT_DISCONNECT && ev.peer == s_ServerPeer) {
			s_ServerClosed = 1;
			break;
		}
	}
}

/**
 * The session is over for this client: say why, and go back to the menus
 * if a match was running. g_NetMode stays until the stage stops (H12).
 */
static void netClientEnd(s32 code, const char *text)
{
	if (s_ClientState == NETCS_GONE) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "net: session ended [%s]: %s", netRefuseName(code), text);
	s_LastRefuse = code;

	if (code != NETREFUSE_LEFT) {
		netSessionNotice("%s%s", text, NULL);
	}

	s_ClientState = NETCS_GONE;

	if (s_MatchActive) {
		s_DropToMenus = 1;
		s_BarrierHeld = 0;
	} else {
		netClientDrain();
		g_NetMode = NETMODE_NONE;
		g_NetLocalSlot = 0;
	}

	if (s_TestJoin && code != NETREFUSE_LEFT) {
		sysLogPrintf(LOG_NOTE, "net: --net-test-join: exiting 3");
		fflush(stdout);
		exit(3);
	}
}

static s32 netResolveStageKey(struct netbuf *b, char *what, s32 whatsize)
{
	s32 kind = netBufReadU8(b);
	char dir[NET_MAXMAPDIR + 1];
	char map[NET_MAXMAPNAME + 1];
	char base[NET_MAXMAPDIR + 1];
	s32 id;

	s_MatchKeyKind = kind;

	if (kind == 0) {
		id = netBufReadU8(b);
		snprintf(what, whatsize, "stock stage 0x%02x", id);

		return netBufOk(b) && !modloaderGetStageModDir(id) ? id : -1;
	}

	if (kind == 1) {
		netBufReadString(b, dir, sizeof(dir));
		netBufReadString(b, map, sizeof(map));
		snprintf(what, whatsize, "map %s from mod %s", map, dir);

		if (!netBufOk(b)) {
			return -1;
		}

		for (id = 0; id <= 0xff; id++) {
			const char *d = modloaderGetStageModDir(id);
			const char *m = d ? modloaderGetStageMapName(id) : NULL;

			if (d && m && strcasecmp(netBasename(d, base, sizeof(base)), dir) == 0 && strcasecmp(m, map) == 0
					&& !modloaderStageIsMission(id)) {
				return id;
			}
		}

		return -1;
	}

	if (kind == 2) {
		netBufReadString(b, dir, sizeof(dir));
		id = netBufReadU8(b);
		snprintf(what, whatsize, "stage 0x%02x of mod %s", id, dir);

		if (!netBufOk(b) || !fsGetModDir() || strcasecmp(netBasename(fsGetModDir(), base, sizeof(base)), dir) != 0) {
			return -1;
		}

		return id;
	}

	b->error = 1;
	snprintf(what, whatsize, "a stage key of kind %d", kind);

	return -1;
}

/**
 * H3: the client loads what the host chose, with the host's rules and
 * seeds, and none of mpStartMatch's own choices
 */
static void netClientBeginStage(struct netbuf *b)
{
	char what[NET_MAXMAPDIR + NET_MAXMAPNAME + 32];
	char label[NET_MAXNAME + 1];
	char text[NET_MAXTEXT + 1];
	u32 matchid = netBufReadU32(b);
	s32 id = netResolveStageKey(b, what, sizeof(what));
	s32 numplayers;
	s32 yourplayer;

	netBufReadString(b, label, sizeof(label));
	s_Seed = netReadU64(b);
	s_Seed2 = netReadU64(b);
	numplayers = netBufReadU8(b);
	yourplayer = netBufReadU8(b);

	if (!netBufOk(b) || netBufRemaining(b) != 0 || matchid != netRulesMatchId()
			|| numplayers < 1 || numplayers > MAX_PLAYERS || (yourplayer >= numplayers && yourplayer != NETSLOT_SPECTATOR)) {
		netSendLeave(s_ServerPeer, NETREFUSE_BADMSG, "STAGE_LOAD did not parse, or came without its RULES");
		netHostDisconnectLater(g_NetHostSocket, s_ServerPeer, NETREFUSE_BADMSG);
		netClientEnd(NETREFUSE_BADMSG, "The host's stage message did not parse.");
		return;
	}

	if (id < 0 || !mainStageCanLoad(id)) {
		snprintf(text, sizeof(text), "The host chose %s, which is not installed here.", what);
		netSendLeave(s_ServerPeer, NETREFUSE_NOSTAGE, text);
		netHostDisconnectLater(g_NetHostSocket, s_ServerPeer, NETREFUSE_NOSTAGE);
		netClientEnd(NETREFUSE_NOSTAGE, text);
		return;
	}

	netRulesApply();
	netRulesSetLocked(1);

	// a spectator watches through player 0, a puppet like the rest
	// (netspec.c): its pad is that player's, which nothing here reads
	s_Spectating = yourplayer == NETSLOT_SPECTATOR;
	s_JoinGoTick = 0;

	if (s_Spectating) {
		s32 pad = 0;

		while (pad < MAX_PLAYERS - 1 && !mpIsHumanSlotOn(pad)) {
			pad++;
		}

		g_NetLocalSlot = 0;
		netPlayersClientMatchStart(pad);
	} else {
		g_NetLocalSlot = yourplayer;
		netPlayersClientMatchStart(s_ClientSlot);
	}
	s_MatchActive = 1;
	s_MatchStage = id;
	s_MatchIdCur = matchid;
	snprintf(s_MatchWhat, sizeof(s_MatchWhat), "%s", what);
	s_SeedPending = 1;
	s_BarrierHeld = 1;
	s_EndPending = 0;
	s_End.valid = 0;
	s_ClientState = NETCS_LOADING;

	sysLogPrintf(LOG_NOTE, "net: match %u: loading %s as 0x%02x (\"%s\"), %d players, this machine %s %d, seeds %016llx %016llx",
			matchid, what, id, label, numplayers, s_Spectating ? "a spectator through player" : "player",
			s_Spectating ? 0 : yourplayer, (unsigned long long)s_Seed, (unsigned long long)s_Seed2);

	// the end of mpStartMatch only
	titleSetNextStage(id);
	mainChangeToStage(id);
	setNumPlayers(numplayers);
	titleSetNextMode(TITLEMODE_SKIP);
	g_Vars.perfectbuddynum = 1;
	menuStop();
}

// ROSTER: who sits where (the names the scoreboard shows)
static void netClientOnRoster(struct netbuf *b)
{
	static const char *const states[] = { "none", "host", "taken", "open", "held" };
	char line[256];
	char names[MAX_PLAYERS][15];
	u8 st[MAX_PLAYERS];
	const u32 matchid = netBufReadU32(b);
	const s32 n = netBufReadU8(b);
	s32 len = 0;
	s32 i;

	if (n != MAX_PLAYERS) {
		b->error = 1;
	}

	for (i = 0; i < MAX_PLAYERS && netBufOk(b); i++) {
		st[i] = netBufReadU8(b);
		netBufReadString(b, names[i], sizeof(names[i]));

		if (st[i] > NETSEAT_HELD) {
			b->error = 1;
		}
	}

	if (!netBufOk(b) || netBufRemaining(b) != 0 || matchid != s_MatchIdCur || !s_MatchActive) {
		sysLogPrintf(LOG_WARNING, "net: a ROSTER that does not fit this match; ignored");
		return;
	}

	line[0] = '\0';

	for (i = 0; i < MAX_PLAYERS; i++) {
		s32 k;

		for (k = 0; names[i][k]; k++) {
			if ((u8)names[i][k] < 0x20 && names[i][k] != '\n') {
				names[i][k] = '?';
			}
		}

		if (st[i] != NETSEAT_NONE) {
			netNameSet(g_PlayerConfigsArray[i].base.name, sizeof(g_PlayerConfigsArray[i].base.name), names[i]);
		}

		if (len < (s32)sizeof(line)) {
			char shown[15];

			snprintf(shown, sizeof(shown), "%s", names[i]);
			shown[strcspn(shown, "\n")] = '\0';
			len += snprintf(line + len, sizeof(line) - len, "%s%d %s \"%s\"", i ? "; " : "", i, states[st[i]], shown);
		}
	}

	sysLogPrintf(LOG_NOTE, "net: roster (tick %u): %s", g_NetTick, line);
}

static void netClientOnMatchEnd(struct netbuf *b)
{
	u32 matchid = netBufReadU32(b);
	s32 i;
	s32 j;
	s32 nchrs;

	s_End.numplayers = netBufReadU8(b);

	if (s_End.numplayers > MAX_PLAYERS) {
		b->error = 1;
	}

	for (i = 0; i < s_End.numplayers && netBufOk(b); i++) {
		s_End.award1[i] = netBufReadU8(b);
		s_End.award2[i] = netBufReadU8(b);
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		s_End.medals[i] = netBufReadU8(b);
		s_End.title[i] = netBufReadU8(b);
	}

	nchrs = netBufReadU8(b);

	if (nchrs != MAX_MPCHRS) {
		b->error = 1;
	}

	for (i = 0; i < MAX_MPCHRS && netBufOk(b); i++) {
		s_End.placement[i] = netBufReadS8(b);
		s_End.rankablescore[i] = netBufReadS32(b);
		s_End.numdeaths[i] = netBufReadS16(b);
		s_End.numpoints[i] = netBufReadS16(b);

		for (j = 0; j < MAX_MPCHRS; j++) {
			s_End.killcounts[i][j] = netBufReadS16(b);
		}
	}

	if (netBufReadU16(b) != NETSCEN_SIZE) {
		b->error = 1;
	} else {
		netBufReadBytes(b, s_EndScen, NETSCEN_SIZE);
	}

	if (!netBufOk(b) || netBufRemaining(b) != 0 || matchid != s_MatchIdCur || !s_MatchActive) {
		sysLogPrintf(LOG_WARNING, "net: a MATCH_END that does not fit this match; ignored");
		return;
	}

	netScenClientFinal(s_EndScen);

	s_End.valid = 1;
	s_EndPending = 1;

	// the events before it on the channel, all of them, before H10 puts
	// the host's table over this machine's
	netEventsClientMatchEnd();

	// ended while this machine was still loading or at the barrier: run on
	// to the end screen rather than wait for a GO that will not come
	s_BarrierHeld = 0;
	sysLogPrintf(LOG_NOTE, "net: match %u: the host ended it", matchid);
	sysLogPrintf(LOG_NOTE, "net: match clock at the end: tick %u, level time %d (60ths), time limit %d", g_NetTick, lvGetStageTime60(), g_MpTimeLimit60);
}

/**
 * H10: the host's scores and awards in place of working them out here,
 * where the puppets have no stats to work them from
 */
void netClientApplyMatchEnd(void)
{
	s32 i;
	s32 j;

	if (!s_End.valid) {
		return;
	}

	for (i = 0; i < s_End.numplayers && i < PLAYERCOUNT(); i++) {
		g_Vars.players[i]->award1 = s_End.award1[i] < 17 ? langGet(g_AwardNames[s_End.award1[i]]) : NULL;
		g_Vars.players[i]->award2 = s_End.award2[i] < 17 ? langGet(g_AwardNames[s_End.award2[i]]) : NULL;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_PlayerConfigsArray[i].medals = s_End.medals[i];
		g_PlayerConfigsArray[i].title = s_End.title[i];
	}

	for (i = 0; i < MAX_MPCHRS; i++) {
		struct mpchrconfig *mpchr = MPCHR(i);

		mpchr->placement = s_End.placement[i];
		mpchr->rankablescore = s_End.rankablescore[i];
		mpchr->numdeaths = s_End.numdeaths[i];
		mpchr->numpoints = s_End.numpoints[i];

		for (j = 0; j < MAX_MPCHRS; j++) {
			mpchr->killcounts[j] = s_End.killcounts[i][j];
		}
	}

	// the scenario's own counts (Hacker Central's downloads, Pop a Cap's
	// caps) the scores are worked out from
	netScenClientApplyFinal();
}

/*
 * Events
 */

static void netHostEvent(const struct netevent *ev)
{
	struct netclient *c;
	struct netbuf b;
	s32 type;

	if (ev->peer < 0 || ev->peer >= NET_MAXPEERS) {
		return;
	}

	c = &s_Clients[ev->peer];

	switch (ev->type) {
	case NETEVENT_CONNECT:
		memset(c, 0, sizeof(*c));
		c->state = NETCL_CONNECTING;
		c->slot = -1;
		c->since = netNowMs();
		break;
	case NETEVENT_DISCONNECT:
		if (c->state >= NETCL_JOINED && c->state != NETCL_REFUSED) {
			sysLogPrintf(LOG_NOTE, "net: slot %d (\"%s\") left%s; the slot is free", c->slot, c->name,
					ev->timedout ? " (timed out)" : "");
		}

		if (c->state >= NETCL_LOADING && c->state <= NETCL_PLAYING) {
			netPlayersHostSlotGone(c->slot);
			netHostPeerGone(ev->peer, 1);
		}

		memset(c, 0, sizeof(*c));
		c->slot = -1;
		break;
	case NETEVENT_RECEIVE:
		if (c->state == NETCL_FREE || c->state == NETCL_REFUSED) {
			break;
		}

		c->lastrecv = netNowMs();

		netBufInitRead(&b, ev->data, ev->len);
		type = netBufReadU8(&b);

		if (type == NETMSG_CONNECT) {
			netHostOnConnect(ev->peer, &b);
		} else if (c->state == NETCL_CONNECTING) {
			netHostKick(ev->peer, NETREFUSE_BADMSG, "", "Messages before CONNECT.");
		} else if (type == NETMSG_LOADED) {
			netHostOnLoaded(ev->peer, &b);
		} else if (type == NETMSG_LEAVE) {
			char text[NET_MAXTEXT + 1];
			s32 code = netBufReadU8(&b);

			netBufReadString(&b, text, sizeof(text));
			sysLogPrintf(LOG_NOTE, "net: slot %d (\"%s\") is leaving [%s]: %s; the slot is free", c->slot, c->name, netRefuseName(code), text);

			// gone now, not when ENet notices: the barrier stops waiting for it
			if (c->state >= NETCL_LOADING && c->state <= NETCL_PLAYING) {
				netPlayersHostSlotGone(c->slot);
				netHostPeerGone(ev->peer, 0);
			}

			c->state = NETCL_REFUSED;
			netHostDisconnectLater(g_NetHostSocket, ev->peer, (u32)code);
			netHostBarrierTick();
		} else if (type == NETMSG_CMD) {
			// unreliable: late ones from the last match are dropped there
			if (c->state == NETCL_PLAYING && c->spectator) {
				netPlayersHostSpecCmd(c->slot, &b);
			} else if (c->state == NETCL_PLAYING) {
				// --net-test-hostile: mangled copies first
				if (netEntsHostile()) netEntsHostileCmd(c->slot, ev->data, ev->len);
				netPlayersHostOnCmd(c->slot, &b);
			}
		} else if (type == NETMSG_SLOTCFG) {
			netReadSlotCfg(&b, &c->cfg);

			if (!netBufOk(&b) || netBufRemaining(&b) != 0) {
				netHostKick(ev->peer, NETREFUSE_BADMSG, "", "Your game's SLOTCFG did not parse.");
			} else if (c->state >= NETCL_LOADING && c->state <= NETCL_PLAYING && !c->spectator) {
				netPlayersHostSlotCfg(c->slot, &c->cfg);
			}
		} else if (type == NETMSG_LOBBY) {
			u32 matchid = netBufReadU32(&b);

			if (!netBufOk(&b) || netBufRemaining(&b) != 0) {
				netHostKick(ev->peer, NETREFUSE_BADMSG, "", "Your game's LOBBY did not parse.");
			} else if (c->state == NETCL_AWAY) {
				c->state = NETCL_JOINED;
				sysLogPrintf(LOG_NOTE, "net: slot %d (\"%s\") is back in the lobby", c->slot, c->name);
			} else if (c->state >= NETCL_LOADING && c->state <= NETCL_PLAYING && matchid == s_MatchIdCur) {
				// back before the host: JOINED at the host's own H12
				c->lobbymatch = matchid;
			}
		} else {
			netHostKick(ev->peer, NETREFUSE_BADMSG, "", "A message the host does not take from a client.");
		}
		break;
	}
}

static void netClientEvent(const struct netevent *ev)
{
	struct netbuf b;
	char text[NET_MAXTEXT + 1];
	char comp[NET_MAXKEY + 1];
	s32 type;
	s32 code;

	switch (ev->type) {
	case NETEVENT_CONNECT:
		if (ev->peer == s_ServerPeer && s_ClientState == NETCS_CONNECTING) {
			sysLogPrintf(LOG_NOTE, "net: connected to the host; sending CONNECT");
			netClientSendConnect();
		}
		break;
	case NETEVENT_DISCONNECT:
		if (ev->peer == s_ServerPeer) {
			s_ServerClosed = 1;
		}

		if (ev->peer == s_ServerPeer && s_ClientState != NETCS_GONE) {
			if (s_Leaving) {
				netClientEnd(NETREFUSE_LEFT, "You left the game.");
			} else if (s_ClientState == NETCS_CONNECTING && netNowMs() - s_ConnectStart < (u64)s_ConnectWindowMs) {
				// a host still booting services its socket only once its
				// main loop runs, after ENet's handshake may have given up
				netClientStartConnect();
			} else if (s_ClientState == NETCS_CONNECTING) {
				snprintf(text, sizeof(text), "Could not connect to %s port %u.", s_ConnectAddr, s_ConnectPort);
				s_Unreached = 1;
				netClientEnd(NETREFUSE_SHUTDOWN, text);
			} else {
				netClientEnd(NETREFUSE_SHUTDOWN, ev->timedout ? "The connection to the host was lost." : "The host closed the connection.");
			}
		}
		break;
	case NETEVENT_RECEIVE:
		if (ev->peer != s_ServerPeer || s_ClientState == NETCS_GONE) {
			break;
		}

		netBufInitRead(&b, ev->data, ev->len);
		type = netBufReadU8(&b);

		switch (type) {
		case NETMSG_ACCEPT:
			if (s_ClientState == NETCS_HELLO) {
				s32 slot = netBufReadU8(&b);
				char hostname[NET_MAXNAME + 1];
				s32 flags;

				netBufReadU32(&b);
				s_HostDedicated = netBufReadU8(&b);
				netBufReadString(&b, hostname, sizeof(hostname));
				flags = netBufReadU8(&b);

				if (netBufOk(&b) && (slot < MAX_PLAYERS || (slot == NETSLOT_SPECTATOR && (flags & NETACC_SPECTATOR)))) {
					s_ClientState = NETCS_JOINED;
					s_ClientSlot = slot == NETSLOT_SPECTATOR ? 0 : slot;

					if (flags & NETACC_SPECTATOR) {
						sysLogPrintf(LOG_NOTE, "net: accepted by \"%s\"%s as a spectator%s", hostname,
								s_HostDedicated ? " (dedicated)" : "", (flags & NETACC_INPROGRESS) ? " of the match in progress" : "");
					} else {
						sysLogPrintf(LOG_NOTE, "net: accepted by \"%s\"%s into slot %d%s", hostname,
								s_HostDedicated ? " (dedicated)" : "", slot,
								!(flags & NETACC_INPROGRESS) ? "" : (flags & NETACC_RESUMED) ? " (the seat this account held, back in the match in progress)"
								: " (an open seat of the match in progress)");
					}

					netClientSendSlotCfg();

					if (s_PendStageLen) {
						struct netbuf pb;

						netBufInitRead(&pb, s_PendStage, s_PendStageLen);
						netBufReadU8(&pb);
						s_PendStageLen = 0;
						sysLogPrintf(LOG_NOTE, "net: the STAGE_LOAD that came before the ACCEPT");
						netClientBeginStage(&pb);
					}
				}
			}
			break;
		case NETMSG_REFUSE:
		case NETMSG_LEAVE:
			code = netBufReadU8(&b);
			comp[0] = '\0';

			if (type == NETMSG_REFUSE) {
				netBufReadString(&b, comp, sizeof(comp));
			}

			netBufReadString(&b, text, sizeof(text));
			sysLogPrintf(LOG_NOTE, "net: the host %s [%s%s%s]: %s", type == NETMSG_REFUSE ? "refused" : "is leaving",
					netRefuseName(code), comp[0] ? " " : "", comp, text);
			s_Leaving = 1;
			netClientEnd(code == NETREFUSE_LEFT ? NETREFUSE_SHUTDOWN : code, text);
			break;
		case NETMSG_RULES:
			if (!netRulesRead(&b)) {
				sysLogPrintf(LOG_WARNING, "net: the host's RULES did not parse");
			}
			break;
		case NETMSG_STAGE_LOAD:
			if (s_ClientState == NETCS_JOINED) {
				netClientBeginStage(&b);
			} else if (s_ClientState == NETCS_HELLO && ev->len <= (s32)sizeof(s_PendStage)) {
				memcpy(s_PendStage, ev->data, ev->len);
				s_PendStageLen = ev->len;
			} else {
				// the host sends one only after this machine's LOBBY
				sysLogPrintf(LOG_WARNING, "net: a STAGE_LOAD while not in the lobby (state %d); ignored", s_ClientState);
			}
			break;
		case NETMSG_GO: {
			const u32 matchid = netBufReadU32(&b);
			const u32 hosttick = netBufReadU32(&b);
			const s32 stagetime = netBufReadS32(&b);

			if (netBufOk(&b) && matchid == s_MatchIdCur && s_MatchActive && s_BarrierHeld) {
				s_BarrierHeld = 0;
				s_ClientState = NETCS_PLAYING;

				if (hosttick) {
					// a join in progress: the match's clock, and a few
					// ticks ahead of the host's (the trim keeps it there)
					s_JoinGoTick = hosttick;
					g_NetTick = hosttick + NET_JOIN_LEAD;
					g_StageTimeElapsed60 = stagetime > 0 ? stagetime : 0;
					netPlayersClientJoinAt(g_NetTick);
					sysLogPrintf(LOG_NOTE, "net: match %u: GO, in progress from host tick %u (level time %d), this machine from tick %u%s",
							s_MatchIdCur, hosttick, stagetime, g_NetTick, s_Spectating ? ", spectating" : "");
				} else {
					sysLogPrintf(LOG_NOTE, "net: match %u: GO%s", s_MatchIdCur, s_Spectating ? ", spectating" : "");
				}
			}
			break;
		}
		case NETMSG_ROSTER:
			netClientOnRoster(&b);
			break;
		case NETMSG_MATCH_END:
			netClientOnMatchEnd(&b);
			break;
		case NETMSG_CMDACK:
			netPlayersClientOnAck(&b);
			break;
		case NETMSG_SNAP:
			netEntsClientOnSnap(ev->data, ev->len);
			break;
		case NETMSG_EVENTS:
			netEventsClientOnMsg(ev->data, ev->len);
			break;
		default:
			break;
		}
		break;
	}
}

void netSessionEvent(const struct netevent *ev)
{
	if (s_Role == NETROLE_HOST) {
		netHostEvent(ev);
	} else if (s_Role == NETROLE_CLIENT) {
		netClientEvent(ev);
	}
}

/*
 * Ticking
 */

static void netClientDropToMenus(void)
{
	s_DropToMenus = 0;

	sysLogPrintf(LOG_NOTE, "net: back to the menus");

	mpSetPaused(MPPAUSEMODE_UNPAUSED);
	g_Vars.mplayerisrunning = false;
	g_Vars.normmplayerisrunning = false;
	g_Vars.lvmpbotlevel = 0;
	titleSetNextStage(STAGE_CITRAINING);
	setNumPlayers(1);
	titleSetNextMode(TITLEMODE_SKIP);
	mainChangeToStage(STAGE_CITRAINING);
	menuStop();

	// a match from a lobby room goes back the way a finished match does,
	// to the menus (menutick.c), and from there to the room's lobby
	if (g_NetLobbyRoom) {
		var80087260 = 3;
	}
}

static void netTestStartMatch(void)
{
	const char *mapname = sysArgGetString("--net-test-map");
	const s32 gescen = sysArgGetInt("--net-test-ge", -1);
	const s32 sims = sysArgGetInt("--net-test-sims", 0);
	const char *weapons = sysArgGetString("--mp-weapons");
	s32 stage = sysArgGetInt("--net-test-stage", 0x32);
	s32 s;

	// --net-test-map NAME: a mod's or the Stage Loader's map by its name (its
	// id depends on the mods installed), as --boot-map finds it
	if (mapname) {
		for (s = 1; s <= STAGE_MAX_ID; s++) {
			const char *name = modloaderGetStageMapName(s);

			if (name && strcmp(name, mapname) == 0 && !modloaderStageIsMission(s)) {
				break;
			}
		}

		if (s <= STAGE_MAX_ID) {
			stage = s;
		} else {
			sysLogPrintf(LOG_WARNING, "net: --net-test-map %s: no such map", mapname);
		}
	}

	// --net-test-ge N: the GoldenEye mode's Combat Simulator (its arenas,
	// weapon sets and simulants) with GoldenEye scenario N (gexplus.h)
	if (gescen >= 0 && gescen < GEXPLUS_NUMSCENARIOS) {
		mpSetGexPlusMode(true);
		gexPlusSetScenario(gescen);
		challengeDetermineUnlockedFeatures();
	}

	sysLogPrintf(LOG_NOTE, "net: --net-test-host: starting a match on 0x%02x with %d sims%s", stage, sims,
			gescen >= 0 ? " (GoldenEye mode)" : "");

	g_MpSetup.stagenum = stage;
	mpClearSimSlots();

	for (s = 0; s < sims && s < MAX_BOTS; s++) {
		mpSetSimSlotOn(s, true);
	}

	if (sims > 4) {
		challengeForceUnlockOneFeature(MPFEATURE_8BOTS);
	}

	if (sims > 0) {
		g_Vars.lvmpbotlevel = 1;
	}

	// --mp-weapons a,b,c,d,e,f as the --mpsims boot takes it: slot 1 is
	// what Mod.StartArmed hands out
	if (weapons) {
		for (s = 0; s < NUM_MPWEAPONSLOTS && *weapons; s++) {
			char *end;
			const long v = strtol(weapons, &end, 10);

			if (end == weapons) {
				break;
			}

			g_MpSetup.weapons[s] = (v >= 0 && v < NUM_MPWEAPONS) ? (u8)v : MPWEAPON_NONE;
			weapons = *end == ',' ? end + 1 : end;
		}
	}

	// --net-test-timelimit N (minutes, 1-60) / --net-test-scorelimit N
	// (kills, 1-100): the match ends on its own, the host's MATCH_END
	{
		const s32 mins = sysArgGetInt("--net-test-timelimit", 0);
		const s32 kills = sysArgGetInt("--net-test-scorelimit", 0);

		if (mins >= 1 && mins <= 60) {
			g_MpSetup.timelimit = mins - 1;
		}

		if (kills >= 1 && kills <= 100) {
			g_MpSetup.scorelimit = kills - 1;
		}

		// --net-test-teamscorelimit N (1-400, 400 none): the team limit as
		// g_MpSetup keeps it (a team's kills end a match even without teams
		// on: mpGetTeamRankings)
		if (sysArgGetInt("--net-test-teamscorelimit", 0) >= 1 && sysArgGetInt("--net-test-teamscorelimit", 0) <= 400) {
			g_MpSetup.teamscorelimit = sysArgGetInt("--net-test-teamscorelimit", 0);
		}
	}

	// --net-test-scenario N (MPSCENARIO_*, its radar and highlight options
	// on) / --net-test-teams N (players and sims dealt round the N teams)
	{
		const s32 scen = sysArgGetInt("--net-test-scenario", -1);
		const s32 teams = sysArgGetInt("--net-test-teams", 0);

		if (teams >= 2 && teams <= MAX_TEAMS) {
			g_MpSetup.options |= MPOPTION_TEAMSENABLED;

			for (s = 0; s < MAX_PLAYERS; s++) {
				g_PlayerConfigsArray[s].base.team = s % teams;
			}

			for (s = 0; s < MAX_BOTS; s++) {
				g_BotConfigsArray[s].base.team = (s + 1) % teams;
			}

			// --net-test-simteam K: every sim on team K instead
			if (sysArgGetInt("--net-test-simteam", -1) >= 0 && sysArgGetInt("--net-test-simteam", -1) < teams) {
				for (s = 0; s < MAX_BOTS; s++) {
					g_BotConfigsArray[s].base.team = sysArgGetInt("--net-test-simteam", 0);
				}
			}
		}

		if (scen >= MPSCENARIO_COMBAT && scen <= MPSCENARIO_CAPTURETHECASE) {
			g_MpSetup.scenario = scen;
			g_MpSetup.options |= MPOPTION_HTB_HIGHLIGHTBRIEFCASE | MPOPTION_HTB_SHOWONRADAR | MPOPTION_CTC_SHOWONRADAR
				| MPOPTION_KOH_HILLONRADAR | MPOPTION_HTM_SHOWONRADAR | MPOPTION_PAC_HIGHLIGHTTARGET | MPOPTION_PAC_SHOWONRADAR;
			scenarioInit();

			// --net-test-hilltime N: King of the Hill's hold, N + 10 seconds
			if (sysArgGetInt("--net-test-hilltime", -1) >= 0) {
				g_Vars.mphilltime = sysArgGetInt("--net-test-hilltime", 10);
			}

			// the time limit ends it unless a score limit was asked for
			if (sysArgGetInt("--net-test-scorelimit", 0) <= 0) {
				g_MpSetup.scorelimit = 100;
				g_MpSetup.teamscorelimit = 400;
			}

			sysLogPrintf(LOG_NOTE, "net: --net-test-scenario: scenario %d, teams %s", scen,
					(g_MpSetup.options & MPOPTION_TEAMSENABLED) ? "on" : "off");
		}
	}

	mpStartMatch();
	menuStop();
}

/**
 * Content gate staging, host only, called through gdb between ticks
 * (tools/ci/netcontenttest.sh): a loose ammo crate out of the setup
 * multi-ammo crate nearest player pn, made as propobj.c's shot-crate path
 * makes one (a path nothing reaches) and dropped from it, so a client
 * has a NETDESC_AMMOCRATE to make. Returns the crate's prop or NULL.
 */
struct prop *netTestLooseCrate(s32 pn)
{
	struct prop *best = NULL;
	struct prop *prop;
	struct ammocrateobj *crate;
	struct multiammocrateobj *multi;
	struct defaultobj tmp = {
		256, 0, OBJTYPE_AMMOCRATE, 0, -1, OBJFLAG_FALL, 0, 0, NULL, NULL,
		1, 0, 0, 0, 1, 0, 0, 0, 1,
		0, NULL, NULL, 0, 1000,
		0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x0fff, 0,
	};
	f32 bestdist = 0;
	s32 guard = 0;
	s32 i;

	if (g_NetMode != NETMODE_SERVER || pn < 0 || pn >= PLAYERCOUNT() || !g_Vars.players[pn] || !g_Vars.players[pn]->prop) {
		return NULL;
	}

	prop = g_Vars.activeprops ? g_Vars.activeprops : g_Vars.pausedprops;

	for (; prop && guard < 4096; prop = prop->next, guard++) {
		if (prop->type == PROPTYPE_OBJ && prop->obj && prop->obj->type == OBJTYPE_MULTIAMMOCRATE && !prop->child) {
			const f32 dx = prop->pos.x - g_Vars.players[pn]->prop->pos.x;
			const f32 dz = prop->pos.z - g_Vars.players[pn]->prop->pos.z;

			if (!best || dx * dx + dz * dz < bestdist) {
				best = prop;
				bestdist = dx * dx + dz * dz;
			}
		}
	}

	if (!best) {
		return NULL;
	}

	multi = (struct multiammocrateobj *)best->obj;

	for (i = 0; i < ARRAYCOUNT(multi->slots); i++) {
		// a Combat Simulator crate's slots name no model of their own: the
		// loose one then wears the crate's
		s32 modelnum = multi->slots[i].modelnum;

		if (modelnum <= 0 || modelnum >= NUM_MODELS || !g_ModelStates[modelnum].fileid) {
			modelnum = multi->base.modelnum;
		}

		if (multi->slots[i].quantity > 0 && modelnum > 0 && modelnum < NUM_MODELS && g_ModelStates[modelnum].fileid) {
			setupLoadModeldef(modelnum);

			if (!g_ModelStates[modelnum].modeldef || !(crate = ammocrateAllocate())) {
				return NULL;
			}

			crate->base = tmp;
			crate->base.modelnum = modelnum;
			crate->ammotype = i + 1;

			if (!objInitWithModelDef(&crate->base, g_ModelStates[modelnum].modeldef)) {
				return NULL;
			}

			// the drop needs the crate a projectile first (objDrop)
			propReparent(crate->base.prop, best);
			objSetDropped(crate->base.prop, DROPTYPE_DEFAULT);
			objDropRecursively(best, false);
			sysLogPrintf(LOG_NOTE, "net: test: a loose ammo crate (model 0x%x, ammo type %d) out of the crate at %.0f %.0f %.0f",
					modelnum, i + 1, best->pos.x, best->pos.y, best->pos.z);
			return crate->base.prop;
		}
	}

	return NULL;
}

/**
 * The client's content summary (tools/ci/netcontenttest.sh): the stage as
 * the host's key named it, the GoldenEye mode and scenario its RULES set,
 * whether GoldenEye's HUD is drawing, and what this machine's player held
 */
void netSessionContentLog(const char *why, u32 ticks, u32 geticks)
{
	const s32 pn = g_NetLocalSlot;
	const s32 weapon = pn >= 0 && pn < PLAYERCOUNT() && g_Vars.players[pn] ? g_Vars.players[pn]->gunctrl.weaponnum : -1;

	if (s_Role != NETROLE_CLIENT || !s_MatchActive) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "net: content client %s (tick %u): stage 0x%02x, key kind %d (%s); GoldenEye mode %d scenario %d (%s), GoldenEye HUD %s; weapon in hand 0x%02x%s, a GoldenEye gun %u of %u ticks; Mod.Bodies %d, Mod.XblaMeshes %d",
			why, g_NetTick, s_MatchStage, s_MatchKeyKind, s_MatchWhat, g_GexPlusMode != 0, gexPlusGetScenario(),
			gexPlusScenarioName(gexPlusGetScenario()), geHudActive() ? "on" : "off", weapon & 0xff,
			WEAPON_IS_GE(weapon) ? " (GoldenEye's)" : "", geticks, ticks, modGetBodiesKept(), xblaMeshGetEnabled());
}

/**
 * Once a loop pass, outside the ticks: timeouts, the barrier, a client's
 * way back to the menus, and the harness's start
 */
void netSessionTick(void)
{
	s32 i;

	if (s_Role == NETROLE_HOST && g_NetHostSocket) {
		for (i = 0; i < NET_MAXPEERS; i++) {
			if (s_Clients[i].state == NETCL_CONNECTING && netNowMs() - s_Clients[i].since > NET_CONNECT_TIMEOUT_MS) {
				netHostKick(i, NETREFUSE_BADMSG, "", "No CONNECT came.");
			}
		}

		netHostBarrierTick();
		netHostLateTick();

		if (s_TestHost && !s_TestStarted && !s_MatchActive && s_TestReadyAt && netNowMs() >= s_TestReadyAt) {
			s32 joined = 0;

			for (i = 0; i < NET_MAXPEERS; i++) {
				joined += s_Clients[i].state == NETCL_JOINED && !s_Clients[i].spectator;
			}

			if (joined >= s_TestHost && g_MainChangeToStageNum < 0) {
				s_TestStarted = 1;
				netTestStartMatch();
			}
		}
	} else if (s_Role == NETROLE_CLIENT && g_NetHostSocket) {
		if (s_ClientState == NETCS_IDLE) {
			netClientStartConnect();
		}

		if (s_ClientState == NETCS_CONNECTING && netNowMs() - s_ConnectStart > (u64)s_ConnectWindowMs) {
			char text[NET_MAXTEXT + 1];

			snprintf(text, sizeof(text), "Could not connect to %s port %u.", s_ConnectAddr, s_ConnectPort);
			netHostDisconnectNow(g_NetHostSocket, s_ServerPeer, 0);
			s_Unreached = 1;
			netClientEnd(NETREFUSE_SHUTDOWN, text);
		}

		if (s_ClientState == NETCS_LOADED && s_BarrierHeld && netNowMs() >= s_ClientBarrierDeadline) {
			s_Leaving = 1;
			netSendLeave(s_ServerPeer, NETREFUSE_TIMEOUT, "No GO came");
			netHostDisconnectLater(g_NetHostSocket, s_ServerPeer, NETREFUSE_TIMEOUT);
			netClientEnd(NETREFUSE_TIMEOUT, "The host did not start the match.");
		}

		if (s_DropToMenus && g_MainChangeToStageNum < 0) {
			netClientDropToMenus();
		}
	}
}

/**
 * At the head of each tick pass: a client ends the match when the host's
 * MATCH_END came (H8 keeps it from ending one on its own clock)
 */
void netSessionTickBegin(void)
{
	netHostSeatsTick();

	if (s_EndPending && netSessionMatchActive() && !g_MainIsEndscreen) {
		s_EndPending = 0;
		g_NumReasonsToEndMpMatch = 1;
		mainEndStage();
	}
}

void netIdleFrame(void)
{
	netPump();
	netFlush();
}

/*
 * Stage hooks
 */

void netClientRefuseLocalStart(void)
{
	sysLogPrintf(LOG_NOTE, "net: a client cannot start a match; the host does");
	snprintf(s_NoticeText, sizeof(s_NoticeText), "%s", "You are in a net game: the host starts the match.");
	g_NetNoticePending = 1;
}

/**
 * menutick.c (H2): a match this session will not start. A client never
 * starts one; a host starts any Combat Simulator scenario.
 */
s32 netRefuseMatchStart(void)
{
	if (s_Role == NETROLE_CLIENT) {
		netClientRefuseLocalStart();
		return 1;
	}

	// every Combat Simulator scenario is online (phase 7a, netscen.c)
	if (s_Role == NETROLE_HOST && (g_MpSetup.scenario < MPSCENARIO_COMBAT || g_MpSetup.scenario > MPSCENARIO_CAPTURETHECASE)) {
		sysLogPrintf(LOG_NOTE, "net: refused to start a net match with scenario %d", g_MpSetup.scenario);
		snprintf(s_NoticeText, sizeof(s_NoticeText), "%s", "This scenario cannot be played online.");
		g_NetNoticePending = 1;
		return 1;
	}

	return 0;
}

/**
 * H4, lvReset right after psReset: the same numbers from here on, on every
 * machine (music has drawn already, from each machine's own settings)
 */
void netStageSeed(void)
{
	if (!s_MatchActive || !s_MatchLoaded || !s_SeedPending || g_StageNum != s_MatchStage) {
		return;
	}

	rngSetSeed(s_Seed);
	rng2SetSeed(s_Seed2);
	s_SeedPending = 0;
}

void netStageHashBegin(s32 stagenum)
{
	if (s_MatchActive && stagenum == s_MatchStage && !s_MatchLoaded) {
		s_MatchLoaded = 1;
		netStageHashOpenWindow();
	}
}

void netStageHashEnd(void)
{
	struct nethashcomp comps[NET_MAXCOMPS];
	struct netbuf b;
	s32 n;
	s32 i;

	if (!netStageHashOpen()) {
		return;
	}

	netStageHashCloseWindow();
	n = netStageHashComponents(comps, NET_MAXCOMPS);

	for (i = 0; i < n; i++) {
		sysLogPrintf(LOG_NOTE, "net: stage hash %-9s %016llx (%u)", comps[i].name,
				(unsigned long long)comps[i].hash, netStageHashCount(i));
	}

	if (s_Role == NETROLE_HOST) {
		memcpy(s_HostComps, comps, sizeof(comps[0]) * n);
		s_HostNumComps = n;
		s_HostLoaded = 1;
		s_LoadDeadline = netNowMs() + NET_LOAD_TIMEOUT_MS;

		for (i = 0; i < NET_MAXPEERS; i++) {
			netHostCheckLoaded(i);
		}

		netHostBarrierTick();
	} else if (s_Role == NETROLE_CLIENT && s_ClientState == NETCS_LOADING) {
		netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
		netBufWriteU8(&b, NETMSG_LOADED);
		netBufWriteU32(&b, s_MatchIdCur);
		netBufWriteU8(&b, (u8)n);

		for (i = 0; i < n; i++) {
			netWriteStr(&b, comps[i].name, NET_MAXCOMPNAME);
			netWriteU64(&b, comps[i].hash);
		}

		netSend(s_ServerPeer, NET_CHAN_RELIABLE, &b);
		netHostFlush(g_NetHostSocket);
		s_ClientState = NETCS_LOADED;
		s_ClientBarrierDeadline = netNowMs() + NET_CLIENT_BARRIER_TIMEOUT_MS;
	}
}

s32 netStageBarrierHold(void)
{
	return s_MatchActive && s_MatchLoaded && s_BarrierHeld && g_StageNum == s_MatchStage;
}

/**
 * H11: End Game on a client leaves the session
 */
void netClientLeave(void)
{
	if (s_Role != NETROLE_CLIENT || s_ClientState == NETCS_GONE) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "net: leaving the game");
	s_Leaving = 1;
	netSendLeave(s_ServerPeer, NETREFUSE_LEFT, "End Game");
	netHostDisconnectLater(g_NetHostSocket, s_ServerPeer, NETREFUSE_LEFT);
	netClientEnd(NETREFUSE_LEFT, "You left the game.");
}

/**
 * H12, after lvStop: if that was a net match its rules come off, and a
 * client whose session is over plays offline from here
 */
void netStageStopped(void)
{
	s32 i;

	// the stage that stopped was the one before the match (the menus)
	if (!s_MatchActive || !s_MatchLoaded) {
		return;
	}

	netHostOwnNameBack();
	netRulesRestore();
	netEventsMatchStopped();
	netEntsMatchStopped();
	netLagCompMatchStopped();
	netPlayersMatchStopped();
	netScenMatchStopped();
	s_MatchActive = 0;
	s_MatchLoaded = 0;
	s_MatchStage = -1;
	s_BarrierHeld = 0;
	s_SeedPending = 0;
	s_EndPending = 0;

	if (s_Role == NETROLE_HOST) {
		// a client still on its end screen sits out the next match until
		// its LOBBY says it is back (else its STAGE_LOAD would be lost)
		for (i = 0; i < NET_MAXPEERS; i++) {
			if (s_Clients[i].state >= NETCL_LOADING && s_Clients[i].state <= NETCL_PLAYING) {
				s_Clients[i].state = s_Clients[i].lobbymatch == s_MatchIdCur ? NETCL_JOINED : NETCL_AWAY;
			}

			s_Clients[i].late = 0;
			s_Clients[i].resumed = 0;
		}

		memset(s_Seats, 0, sizeof(s_Seats));
		s_HostEnded = 0;
	} else if (s_Role == NETROLE_CLIENT) {
		netSpecStop();
		s_Spectating = 0;
		s_JoinGoTick = 0;

		if (s_ClientState == NETCS_GONE) {
			netClientDrain();
			g_NetMode = NETMODE_NONE;
			g_NetLocalSlot = 0;
		} else {
			struct netbuf b;

			s_ClientState = NETCS_JOINED;

			netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
			netBufWriteU8(&b, NETMSG_LOBBY);
			netBufWriteU32(&b, s_MatchIdCur);
			netSend(s_ServerPeer, NET_CHAN_RELIABLE, &b);
			netClientSendSlotCfg();
			netHostFlush(g_NetHostSocket);
		}
	}
}

s32 netSessionMatchActive(void)
{
	return s_MatchActive && s_MatchLoaded && g_StageNum == s_MatchStage;
}

s32 netSessionBarrierHeld(void)
{
	return netStageBarrierHold();
}

s32 netSessionMatchLoading(void)
{
	return s_MatchActive && g_StageNum == s_MatchStage;
}

s32 netSessionViewLive(s32 view)
{
	s32 i;

	if (s_Role != NETROLE_HOST || view < 0 || view >= NET_MAXVIEWS) {
		return 0;
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state == NETCL_PLAYING && s_Clients[i].slot == view) {
			return 1;
		}
	}

	return 0;
}

s32 netSessionSpectating(void)
{
	return s_Role == NETROLE_CLIENT && s_Spectating && s_MatchActive;
}

u32 netSessionMatchId(void)
{
	return s_MatchIdCur;
}

s32 netSessionSendSlot(s32 slot, s32 channel, const void *data, s32 len, s32 flags)
{
	s32 i;

	if (s_Role != NETROLE_HOST || !g_NetHostSocket) {
		return -1;
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state == NETCL_PLAYING && s_Clients[i].slot == slot) {
			return netHostSend(g_NetHostSocket, i, channel, data, len, flags);
		}
	}

	return -1;
}

s32 netSessionSlotRtt(s32 slot, s32 *rtt, s32 *rttvar)
{
	struct netpeerstats st;
	s32 i;

	*rtt = -1;
	*rttvar = 0;

	if (s_Role != NETROLE_HOST || !g_NetHostSocket) {
		return -1;
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state == NETCL_PLAYING && s_Clients[i].slot == slot) {
			if (netHostPeerStats(g_NetHostSocket, i, &st) != 0 || !st.connected) {
				return -1;
			}

			*rtt = (s32)st.rtt;
			*rttvar = (s32)st.rttvar;
			return 0;
		}
	}

	return -1;
}

/**
 * A client in this slot or view that has gone back to its menus from the
 * current match (its LOBBY for it came) while the host is still on the
 * stage, its end screen: it gets no more snapshots, which with nothing acked
 * would all be keyframes
 */
s32 netSessionSlotLeftMatch(s32 slot)
{
	s32 i;

	if (s_Role != NETROLE_HOST || !s_MatchIdCur) {
		return 0;
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state >= NETCL_LOADING && s_Clients[i].state <= NETCL_PLAYING
				&& s_Clients[i].slot == slot && s_Clients[i].lobbymatch == s_MatchIdCur) {
			return 1;
		}
	}

	return 0;
}

/**
 * Phase 8: the host's traffic with each client so far, ENet's totals (every
 * datagram's length, ENet's headers included, UDP/IP's not), for the
 * bandwidth gate (tools/ci/nettwelvetest.sh)
 */
void netSessionLogTraffic(const char *why)
{
	struct netpeerstats st;
	u64 sent = 0;
	u64 recv = 0;
	s32 n = 0;
	s32 i;

	if (s_Role != NETROLE_HOST || !g_NetHostSocket) {
		return;
	}

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state >= NETCL_JOINED && s_Clients[i].state != NETCL_REFUSED
				&& netHostPeerStats(g_NetHostSocket, i, &st) == 0 && st.connected) {
			sysLogPrintf(LOG_NOTE, "net: traffic slot %d %s (tick %u): sent %u bytes, received %u bytes, rtt %u ms",
					s_Clients[i].slot, why, g_NetTick, st.bytessent, st.bytesreceived, st.rtt);
			sent += st.bytessent;
			recv += st.bytesreceived;
			n++;
		}
	}

	sysLogPrintf(LOG_NOTE, "net: traffic host %s (tick %u): %d clients, sent %llu bytes, received %llu bytes",
			why, g_NetTick, n, (unsigned long long)sent, (unsigned long long)recv);
}

s32 netSessionSendServer(s32 channel, const void *data, s32 len, s32 flags)
{
	if (s_Role != NETROLE_CLIENT || !g_NetHostSocket || s_ServerPeer < 0 || s_ClientState == NETCS_GONE) {
		return -1;
	}

	return netHostSend(g_NetHostSocket, s_ServerPeer, channel, data, len, flags);
}

static void netSessionClose(void);

void netShutdown(void)
{
	if (g_NetLobbyActive) {
		netLobbyShutdown();
	}

	if (!g_NetHostSocket) {
		return;
	}

	netSessionClose();
	netTransportShutdown();
}

/**
 * The goodbyes to whoever is connected, a moment for them to go out, and
 * the socket closed (the transport stays up)
 */
static void netSessionClose(void)
{
	struct netevent ev;
	u64 until;
	s32 i;

	if (s_Role == NETROLE_HOST) {
		for (i = 0; i < NET_MAXPEERS; i++) {
			if (s_Clients[i].state != NETCL_FREE && s_Clients[i].state != NETCL_REFUSED) {
				netSendLeave(i, NETREFUSE_SHUTDOWN, "The host has quit.");
				netHostDisconnectLater(g_NetHostSocket, i, NETREFUSE_SHUTDOWN);
			}
		}
	} else if (s_ServerPeer >= 0 && s_ClientState != NETCS_GONE) {
		s_Leaving = 1;
		s_TestJoin = 0;
		netSendLeave(s_ServerPeer, NETREFUSE_LEFT, "Quit");
		netHostDisconnectLater(g_NetHostSocket, s_ServerPeer, NETREFUSE_LEFT);
	}

	// a moment for the goodbyes to go out and be acknowledged
	until = netNowMs() + 250;

	while (netNowMs() < until) {
		if (netHostService(g_NetHostSocket, &ev, 10) < 0) {
			break;
		}
	}

	if (s_SockLent) {
		// the lobby's socket: the path it punched or the relay it bound
		// stays open for the room's next match; only the peer goes
		if (s_ServerPeer >= 0) {
			netHostDisconnectNow(g_NetHostSocket, s_ServerPeer, NETREFUSE_LEFT);
		}

		s_SockLent = 0;
	} else {
		netHostDestroy(g_NetHostSocket);
	}

	g_NetHostSocket = NULL;
}

/*
 * A lobby room's session (netlobby.c): opened and closed at run time, not
 * from --host/--connect
 */

/**
 * The room's host: listens on Net.Port and turns away every CONNECT without
 * a ticket for this room from its roster (check 5). The room's id and secret
 * come once the lobby has made it (netSessionLobbySetRoom); until then no
 * ticket can match. 0, or -1 if the socket did not open.
 */
s32 netSessionLobbyHost(const char *name)
{
	struct nethashcomp comps[NET_MAXCOMPS];

	if (s_Role != NETROLE_NONE) {
		return s_Role == NETROLE_HOST && g_NetHostSocket ? 0 : -1;
	}

	if (netTransportInit() != 0) {
		return -1;
	}

	netSessionHash(comps, NET_MAXCOMPS);
	memset(s_Clients, 0, sizeof(s_Clients));
	snprintf(s_Name, sizeof(s_Name), "%s", name);
	s_Role = NETROLE_HOST;
	s_LobbyRoomOn = 1;
	s_LobbyRoomId[0] = '\0';
	s_LobbyRoomSecret[0] = '\0';
	g_NetMode = NETMODE_SERVER;
	g_NetLocalSlot = 0;
	netSessionOpenSocket();

	if (!g_NetHostSocket) {
		s_Role = NETROLE_NONE;
		s_LobbyRoomOn = 0;
		g_NetMode = NETMODE_NONE;
		netTransportShutdown();
		return -1;
	}

	return 0;
}

u16 netSessionLobbyPort(void)
{
	return g_NetHostSocket && s_Role == NETROLE_HOST ? netHostPort(g_NetHostSocket) : 0;
}

void netSessionLobbySetRoom(const char *roomid, const char *secret)
{
	snprintf(s_LobbyRoomId, sizeof(s_LobbyRoomId), "%s", roomid);
	snprintf(s_LobbyRoomSecret, sizeof(s_LobbyRoomSecret), "%s", secret);
}

void netSessionLobbyClock(s64 offset)
{
	s_LobbyClock = offset;
}

/**
 * A member at launch: connects to the host with its ticket. The connect
 * itself starts from netSessionTick, as for --connect. sock, if given, is
 * the lobby's socket that punched (or bound the relay) to addr: the
 * session uses it and hands it back when it closes.
 */
s32 netSessionLobbyConnect(const char *addr, u16 port, const char *ticket, const char *name, struct nethost *sock, s32 windowms)
{
	struct nethashcomp comps[NET_MAXCOMPS];

	if (s_Role != NETROLE_NONE) {
		return -1;
	}

	if (netTransportInit() != 0) {
		return -1;
	}

	netSessionHash(comps, NET_MAXCOMPS);
	snprintf(s_ConnectAddr, sizeof(s_ConnectAddr), "%s", addr);
	s_ConnectPort = port;
	snprintf(s_Ticket, sizeof(s_Ticket), "%s", ticket);
	snprintf(s_Name, sizeof(s_Name), "%s", name);
	s_ConnectWindowMs = windowms > 0 ? windowms : NET_CONNECT_WINDOW_MS;
	s_Unreached = 0;
	s_Role = NETROLE_CLIENT;
	s_LobbyRoomOn = 1;
	s_ClientState = NETCS_IDLE;
	s_ServerPeer = -1;
	s_ServerClosed = 0;
	s_ConnectTries = 0;
	s_Leaving = 0;
	s_DropToMenus = 0;
	s_EndPending = 0;
	g_NetMode = NETMODE_CLIENT;
	g_NetLocalSlot = 0;

	if (sock) {
		g_NetHostSocket = sock;
		s_SockLent = 1;
		s_ClientState = NETCS_IDLE;
		netSessionApplySim();
	} else {
		netSessionOpenSocket();
	}

	if (!g_NetHostSocket) {
		s_Role = NETROLE_NONE;
		s_LobbyRoomOn = 0;
		g_NetMode = NETMODE_NONE;
		netTransportShutdown();
		return -1;
	}

	sysLogPrintf(LOG_NOTE, "net: lobby: connecting to the room's host at %s port %u", addr, port);

	return 0;
}

/**
 * The room is left or gone: the session it made closes (a host says
 * goodbye to its clients, a client to its host). Not mid-match: the lobby
 * waits for the stage to stop first.
 */
void netSessionLobbyStop(void)
{
	if (!s_LobbyRoomOn) {
		return;
	}

	if (g_NetHostSocket) {
		netSessionClose();
		netTransportShutdown();
	}

	netHostOwnNameBack();
	memset(s_Clients, 0, sizeof(s_Clients));
	s_Role = NETROLE_NONE;
	s_LobbyRoomOn = 0;
	s_LobbyRoomId[0] = '\0';
	s_LobbyRoomSecret[0] = '\0';
	g_MpHumanSlotsHi = 0;
	s_ClientState = NETCS_IDLE;
	s_ServerPeer = -1;
	s_Ticket[0] = '\0';
	s_Spectate = sysArgCheck("--net-spectate");
	g_NetMode = NETMODE_NONE;
	g_NetLocalSlot = 0;

	sysLogPrintf(LOG_NOTE, "net: lobby: the room's session is closed");
}

// 1 host, 2 client, 0 none (a lobby room's session only)
s32 netSessionLobbyRole(void)
{
	return s_LobbyRoomOn ? s_Role : NETROLE_NONE;
}

// A client: connected and accepted by the host, and not gone since
s32 netSessionClientJoined(void)
{
	return s_Role == NETROLE_CLIENT && s_ClientState >= NETCS_JOINED && s_ClientState != NETCS_GONE;
}

// A client whose session is over (refused, left, the host gone)
s32 netSessionClientGone(void)
{
	return s_Role == NETROLE_CLIENT && s_ClientState == NETCS_GONE;
}

/**
 * The name a slot goes out under (RULES, ROSTER). A lobby room's host plays
 * as its account, as its members do, not as whatever its local profile is
 * called ("Player 1" when it has none); its profile keeps its own name, so
 * nothing the endscreen saves changes it.
 */
/**
 * A player's name as PD keeps it: the text and then a newline, which every
 * menu row, ranking and HUD line that draws a name relies on (textMeasure
 * sizes a label by its lines: a name without one measured no height, and the
 * end screen's "Title:" row was drawn over the name row above it). Names from
 * the wire, a lobby account or "(open)" come without one; a profile's has it.
 */
void netNameSet(char *dst, s32 size, const char *src)
{
	s32 n = 0;

	if (size < 2) {
		if (size == 1) {
			dst[0] = '\0';
		}
		return;
	}

	while (src && src[n] && src[n] != '\n' && n < size - 2) {
		dst[n] = src[n];
		n++;
	}

	// an empty name stays empty, as an unnamed one is offline
	if (n > 0) {
		dst[n++] = '\n';
	}

	dst[n] = '\0';
}

s32 netNameLen(const char *name)
{
	return (s32)strcspn(name, "\n");
}

const char *netSessionWireName(s32 slot)
{
	if (slot == 0 && s_Role == NETROLE_HOST && s_LobbyRoomOn && !netIsDedicatedHost() && s_Name[0]) {
		return s_Name;
	}

	return g_PlayerConfigsArray[slot].base.name;
}

// A client whose last connect found no host at its address in the window
s32 netSessionClientUnreached(void)
{
	return s_Unreached;
}

// The code the client's last session ended on (NETREFUSE_*), or -1
s32 netSessionLastRefuse(void)
{
	return s_LastRefuse;
}

// The host: a seat of the running match that nobody plays (its player out
// of play), by mpindex
s32 netSessionSeatOutOfPlay(s32 slot)
{
	return s_Role == NETROLE_HOST && s_MatchActive && slot >= 0 && slot < MAX_PLAYERS
		&& s_Seats[slot].state == NETSEAT_OPEN;
}

// The host: the slot of the joined client called `user`, or -1
s32 netSessionHostSlotOf(const char *user)
{
	s32 i;

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state >= NETCL_JOINED && s_Clients[i].state != NETCL_REFUSED
				&& strcasecmp(s_Clients[i].name, user) == 0) {
			return s_Clients[i].slot;
		}
	}

	return -1;
}

// The host: whether the client called `user` connected as a spectator (1),
// a player (0), or is not connected (-1)
s32 netSessionHostUserSpectating(const char *user)
{
	s32 i;

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state >= NETCL_JOINED && s_Clients[i].state != NETCL_REFUSED
				&& strcasecmp(s_Clients[i].name, user) == 0) {
			return s_Clients[i].spectator;
		}
	}

	return -1;
}

// A client: the next connect is a spectator's (a lobby room's spectator)
void netSessionSetSpectate(s32 on)
{
	s_Spectate = on;
}

// The host: the clients in the session (joined, loading or playing)
s32 netSessionHostNumClients(void)
{
	s32 n = 0;
	s32 i;

	for (i = 0; i < NET_MAXPEERS; i++) {
		n += s_Clients[i].state >= NETCL_JOINED && s_Clients[i].state <= NETCL_PLAYING;
	}

	return n;
}

// The host: the name of the i'th client in the session, or NULL past the last
const char *netSessionHostClientName(s32 index)
{
	s32 i;

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state >= NETCL_JOINED && s_Clients[i].state != NETCL_REFUSED && index-- == 0) {
			return s_Clients[i].name;
		}
	}

	return NULL;
}

// The host: `user` has left the room's roster (left, kicked, timed out)
void netSessionHostDropUser(const char *user, const char *why)
{
	s32 i;

	for (i = 0; i < NET_MAXPEERS; i++) {
		if (s_Clients[i].state >= NETCL_CONNECTING && s_Clients[i].state != NETCL_REFUSED
				&& s_Clients[i].name[0] && strcasecmp(s_Clients[i].name, user) == 0) {
			netHostKick(i, NETREFUSE_TICKET, "roster", why);
		}
	}
}

/**
 * The host, once everyone launched has connected: the match on the
 * Combat Simulator setup the room was made from, as the menus' own start
 * does (menutick.c, prevmenuroot -5)
 */
void netSessionLobbyStartMatch(void)
{
	sysLogPrintf(LOG_NOTE, "net: lobby: starting the room's match on 0x%02x, %d clients", g_MpSetup.stagenum, netSessionHostNumClients());
	g_MpSetup.chrslots |= 1;
	mpStartMatch();
	menuStop();
}

/*
 * The main menu's notice
 */

static char *netMenuTextNotice(struct menuitem *item)
{
	return s_NoticeText;
}

// Why the last session ended, for the lobby's room page (netlobby.c)
const char *netSessionNoticeText(void)
{
	return s_NoticeText;
}

static struct menuitem s_NetNoticeItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&netMenuTextNotice,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"OK\n",
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef s_NetNoticeDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Net Game",
	s_NetNoticeItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * From the Perfect Menu's tick while it is on top: why the last session
 * ended, once
 */
void netMainMenuTick(void)
{
	// a client whose match ended under it lands here: back to the room
	if (g_NetLobbyRoom && !g_NetNoticePending) {
		netLobbyMenuAfterMatch();
		return;
	}

	if (!g_NetNoticePending) {
		return;
	}

	g_NetNoticePending = 0;
	menuPushDialog(&s_NetNoticeDialog);
}

/**
 * menutick.c (H2), after a refused start: the menus had all closed for the
 * match, so the setup menu comes back with the reason on top of it
 */
s32 netMatchStartRefused(void)
{
	if (IS4MB()) {
		menuPushRootDialog(&g_MainMenu4MbMenuDialog, MENUROOT_4MBMAINMENU);
	} else {
		menuPushRootDialog(&g_CombatSimulatorMenuDialog, MENUROOT_MPSETUP);
	}

	netMainMenuTick();

	return true;
}
