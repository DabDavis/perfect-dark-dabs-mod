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
#include "game/lv.h"

extern s32 g_MpTimeLimit60;
extern s32 g_MpScoreLimit;
extern s32 g_NumReasonsToEndMpMatch;
#include "net/net.h"
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
};

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
static s32 s_Leaving = 0;
static s32 s_DropToMenus = 0;
static s32 s_EndPending = 0;
static s32 s_HostDedicated = 0;
static s32 s_ClientSlot = 0;      // the slot (pad, mpindex) ACCEPT gave this machine
static s32 s_ServerClosed = 0;    // the host's peer is disconnected for good
static u64 s_ClientBarrierDeadline = 0;

// the match, both sides
static s32 s_MatchActive = 0;  // from the start (H1 / STAGE_LOAD) to H12
static s32 s_MatchLoaded = 0;  // its stage has begun loading (H5): H12 then ends it
static s32 s_MatchStage = -1;
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

PD_CONSTRUCTOR static void netSessionConfigInit(void)
{
	configRegisterInt("Net.Port", &s_Port, 1, 65535);
	configRegisterString("Net.Name", s_Name, sizeof(s_Name));
	configRegisterInt("Net.RequireTicket", &s_RequireTicket, 0, 1);
	configRegisterString("Net.RoomId", s_RoomId, sizeof(s_RoomId));
	configRegisterString("Net.RoomSecret", s_RoomSecret, sizeof(s_RoomSecret));
	configRegisterInt("Net.LobbyClockOffset", &s_LobbyClockOffset, -86400, 86400);
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
	s_TestJoin = sysArgCheck("--net-test-join");
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

	for (slot = netIsDedicatedHost() ? 0 : 1; slot < MAX_PLAYERS; slot++) {
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

				if (theirs[j].hash != mine[i].hash) {
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
		sysLogPrintf(LOG_NOTE, "net: peer %d: lobby ticket for \"%s\" in room %s verified", peer, user, roomid);

		// one seat per user: a CONNECT for a user who holds a seat replaces
		// it (a restarted game) rather than taking a second, but only once
		// it has passed every check below
		if (s_LobbyRoomOn) {
			for (j = 0; j < NET_MAXPEERS; j++) {
				if (j != peer && s_Clients[j].state >= NETCL_JOINED && s_Clients[j].state != NETCL_REFUSED
						&& strcasecmp(s_Clients[j].name, user) == 0) {
					oldseat = j;
				}
			}
		}
	}

	if (netRulesCheckClientKeys(c->keys, c->nkeys, -1, &code, key, sizeof(key), text, sizeof(text))) {
		netHostKick(peer, code, key, text);
		return;
	}

	if (s_MatchActive) {
		netHostKick(peer, NETREFUSE_STARTED, "", "A match is under way; join when it is over.");
		return;
	}

	c->slot = netHostFreeSlot(peer);

	if (c->slot < 0 && oldseat >= 0) {
		c->slot = s_Clients[oldseat].slot; // the seat it replaces
	}

	if (c->slot < 0) {
		netHostKick(peer, NETREFUSE_FULL, "", "The game is full.");
		return;
	}

	// the join is accepted: only now is the ticket spent
	if ((s_RequireTicket || s_LobbyRoomOn) && netTicketUse(nonce, nonceexpiry, netSessionLobbyNow()) != 0) {
		netHostKick(peer, NETREFUSE_TICKET, "ticket", "This room needs a join ticket from the lobby: the ticket was used already.");
		return;
	}

	if (oldseat >= 0) {
		netHostKick(oldseat, NETREFUSE_TICKET, "roster", "You connected again from another game.");
	}

	c->state = NETCL_JOINED;

	netBufInitWrite(b, s_Buf, sizeof(s_Buf));
	netBufWriteU8(b, NETMSG_ACCEPT);
	netBufWriteU8(b, (u8)c->slot);
	netBufWriteU32(b, g_NetTick);
	netBufWriteU8(b, netIsDedicatedHost() ? 1 : 0);
	netWriteStr(b, s_Name, NET_MAXNAME);
	netSend(peer, NET_CHAN_RELIABLE, b);

	addr[0] = '\0';

	if (netHostPeerAddr(g_NetHostSocket, peer, &na) == 0) {
		netAddrToString(&na, addr, sizeof(addr));
	}

	sysLogPrintf(LOG_NOTE, "net: slot %d: \"%s\" joined from %s (fov %.0f, aspect %.2f, head %d body %d)",
			c->slot, c->name, addr, c->cfg.fovy, c->cfg.aspect, c->cfg.mpheadnum, c->cfg.mpbodynum);

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
		if (s_Clients[i].state == NETCL_JOINED && s_Clients[i].slot >= 0) {
			bits |= 1 << s_Clients[i].slot;
		}
	}

	return bits;
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

		if (c->state == NETCL_JOINED) {
			struct mpplayerconfig *p = &g_PlayerConfigsArray[c->slot];

			snprintf(p->base.name, sizeof(p->base.name), "%s", c->name);
			p->base.mpheadnum = c->cfg.mpheadnum;
			p->base.mpbodynum = c->cfg.mpbodynum;
			memset(&p->fileguid, 0, sizeof(p->fileguid));

			// its controls, settings and screen (before RULES: they carry
			// the slot's options)
			netPlayersHostSlotStart(c->slot, &c->cfg);
		}
	}

	g_MpSetup.chrslots = (g_MpSetup.chrslots & ~0xf) | bits;

	s_MatchId++;
	s_MatchIdCur = s_MatchId;

	if (seedarg >= 0) {
		s_Seed = netMix((u64)seedarg * 0x9e3779b97f4a7c15ULL + s_MatchId);
	} else {
		s_Seed = netMix(sysGetMicroseconds() ^ ((u64)osGetCount() << 32) ^ (u64)time(NULL));
	}

	s_Seed2 = netMix(s_Seed ^ 0x5bd1e9955bd1e995ULL);

	for (i = 0; i < NET_MAXPEERS; i++) {
		struct netclient *c = &s_Clients[i];
		s32 yourplayer = 0;
		s32 k;

		if (c->state != NETCL_JOINED) {
			continue;
		}

		for (k = 0; k < c->slot; k++) {
			yourplayer += (bits >> k) & 1;
		}

		netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
		netRulesWrite(&b, s_MatchId);
		netSend(i, NET_CHAN_BULK, &b);

		netBufInitWrite(&b, s_Buf, sizeof(s_Buf));
		netBufWriteU8(&b, NETMSG_STAGE_LOAD);
		netBufWriteU32(&b, s_MatchId);
		netWriteStageKey(&b, stagenum, label, sizeof(label));
		netWriteStr(&b, label, NET_MAXNAME);
		netWriteU64(&b, s_Seed);
		netWriteU64(&b, s_Seed2);
		netBufWriteU8(&b, (u8)numplayers);
		netBufWriteU8(&b, (u8)yourplayer);
		netSend(i, NET_CHAN_BULK, &b);

		c->state = NETCL_LOADING;
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
		if (s_Clients[i].state == NETCL_LOADING) {
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
			netSend(i, NET_CHAN_RELIABLE, &b);
			s_Clients[i].state = NETCL_PLAYING;
		}
	}

	s_BarrierHeld = 0;
	netHostFlush(g_NetHostSocket);
	sysLogPrintf(LOG_NOTE, "net: match %u: every machine has loaded; GO", s_MatchIdCur);
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

	if (s_Role != NETROLE_HOST || !s_MatchActive || !g_NetHostSocket) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "net: match clock at the end: tick %u, level time %d (60ths), time limit %d, score limit %d, other reasons %d",
			g_NetTick, lvGetStageTime60(), g_MpTimeLimit60, g_MpScoreLimit, g_NumReasonsToEndMpMatch);

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
			|| numplayers < 1 || numplayers > MAX_PLAYERS || yourplayer >= numplayers) {
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

	g_NetLocalSlot = yourplayer;
	netPlayersClientMatchStart(s_ClientSlot);
	s_MatchActive = 1;
	s_MatchStage = id;
	s_MatchIdCur = matchid;
	s_SeedPending = 1;
	s_BarrierHeld = 1;
	s_EndPending = 0;
	s_End.valid = 0;
	s_ClientState = NETCS_LOADING;

	sysLogPrintf(LOG_NOTE, "net: match %u: loading %s as 0x%02x (\"%s\"), %d players, this machine player %d, seeds %016llx %016llx",
			matchid, what, id, label, numplayers, yourplayer, (unsigned long long)s_Seed, (unsigned long long)s_Seed2);

	// the end of mpStartMatch only
	titleSetNextStage(id);
	mainChangeToStage(id);
	setNumPlayers(numplayers);
	titleSetNextMode(TITLEMODE_SKIP);
	g_Vars.perfectbuddynum = 1;
	menuStop();
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

	if (!netBufOk(b) || netBufRemaining(b) != 0 || matchid != s_MatchIdCur || !s_MatchActive) {
		sysLogPrintf(LOG_WARNING, "net: a MATCH_END that does not fit this match; ignored");
		return;
	}

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
		}

		memset(c, 0, sizeof(*c));
		c->slot = -1;
		break;
	case NETEVENT_RECEIVE:
		if (c->state == NETCL_FREE || c->state == NETCL_REFUSED) {
			break;
		}

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
			}

			c->state = NETCL_REFUSED;
			netHostDisconnectLater(g_NetHostSocket, ev->peer, (u32)code);
			netHostBarrierTick();
		} else if (type == NETMSG_CMD) {
			// unreliable: late ones from the last match are dropped there
			if (c->state == NETCL_PLAYING) {
				// --net-test-hostile: mangled copies first
				if (netEntsHostile()) netEntsHostileCmd(c->slot, ev->data, ev->len);
				netPlayersHostOnCmd(c->slot, &b);
			}
		} else if (type == NETMSG_SLOTCFG) {
			netReadSlotCfg(&b, &c->cfg);

			if (!netBufOk(&b) || netBufRemaining(&b) != 0) {
				netHostKick(ev->peer, NETREFUSE_BADMSG, "", "Your game's SLOTCFG did not parse.");
			} else if (c->state >= NETCL_LOADING && c->state <= NETCL_PLAYING) {
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
			} else if (s_ClientState == NETCS_CONNECTING && netNowMs() - s_ConnectStart < NET_CONNECT_WINDOW_MS) {
				// a host still booting services its socket only once its
				// main loop runs, after ENet's handshake may have given up
				netClientStartConnect();
			} else if (s_ClientState == NETCS_CONNECTING) {
				snprintf(text, sizeof(text), "Could not connect to %s port %u.", s_ConnectAddr, s_ConnectPort);
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

				netBufReadU32(&b);
				s_HostDedicated = netBufReadU8(&b);
				netBufReadString(&b, hostname, sizeof(hostname));

				if (netBufOk(&b) && slot < MAX_PLAYERS) {
					s_ClientState = NETCS_JOINED;
					s_ClientSlot = slot;
					sysLogPrintf(LOG_NOTE, "net: accepted by \"%s\"%s into slot %d", hostname,
							s_HostDedicated ? " (dedicated)" : "", slot);
					netClientSendSlotCfg();
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
			} else {
				// the host sends one only after this machine's LOBBY
				sysLogPrintf(LOG_WARNING, "net: a STAGE_LOAD while not in the lobby (state %d); ignored", s_ClientState);
			}
			break;
		case NETMSG_GO:
			if (netBufReadU32(&b) == s_MatchIdCur && s_MatchActive && s_BarrierHeld) {
				s_BarrierHeld = 0;
				s_ClientState = NETCS_PLAYING;
				sysLogPrintf(LOG_NOTE, "net: match %u: GO", s_MatchIdCur);
			}
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
	const s32 stage = sysArgGetInt("--net-test-stage", 0x32);
	const s32 sims = sysArgGetInt("--net-test-sims", 0);
	const char *weapons = sysArgGetString("--mp-weapons");
	s32 s;

	sysLogPrintf(LOG_NOTE, "net: --net-test-host: starting a match on 0x%02x with %d sims", stage, sims);

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
	}

	mpStartMatch();
	menuStop();
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

		if (s_TestHost && !s_TestStarted && !s_MatchActive && s_TestReadyAt && netNowMs() >= s_TestReadyAt) {
			s32 joined = 0;

			for (i = 0; i < NET_MAXPEERS; i++) {
				joined += s_Clients[i].state == NETCL_JOINED;
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

		if (s_ClientState == NETCS_CONNECTING && netNowMs() - s_ConnectStart > NET_CONNECT_WINDOW_MS) {
			char text[NET_MAXTEXT + 1];

			snprintf(text, sizeof(text), "Could not connect to %s port %u.", s_ConnectAddr, s_ConnectPort);
			netHostDisconnectNow(g_NetHostSocket, s_ServerPeer, 0);
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
 * starts one; a host does not start a scenario yet (phase 7 syncs them),
 * and says so rather than starting a match the clients cannot see.
 */
s32 netRefuseMatchStart(void)
{
	if (s_Role == NETROLE_CLIENT) {
		netClientRefuseLocalStart();
		return 1;
	}

	if (s_Role == NETROLE_HOST && g_MpSetup.scenario != MPSCENARIO_COMBAT) {
		sysLogPrintf(LOG_NOTE, "net: refused to start a net match with scenario %d: only Combat is online yet", g_MpSetup.scenario);
		snprintf(s_NoticeText, sizeof(s_NoticeText), "%s",
				"Scenarios are not online yet: set the scenario to Combat to start a net match.");
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

	netRulesRestore();
	netEventsMatchStopped();
	netEntsMatchStopped();
	netLagCompMatchStopped();
	netPlayersMatchStopped();
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
		}
	} else if (s_Role == NETROLE_CLIENT) {
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
s32 netSessionLobbyConnect(const char *addr, u16 port, const char *ticket, const char *name, struct nethost *sock)
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

	memset(s_Clients, 0, sizeof(s_Clients));
	s_Role = NETROLE_NONE;
	s_LobbyRoomOn = 0;
	s_LobbyRoomId[0] = '\0';
	s_LobbyRoomSecret[0] = '\0';
	s_ClientState = NETCS_IDLE;
	s_ServerPeer = -1;
	s_Ticket[0] = '\0';
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
