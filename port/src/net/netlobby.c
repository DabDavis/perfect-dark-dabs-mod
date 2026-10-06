#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include <SDL2/SDL.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "config.h"
#include "system.h"
#include "versioninfo.h"
#include "ghostnet.h"
#include "modloader.h"
#include "game/menu.h"
#include "game/mplayer/mplayer.h"
#include "lib/main.h"
#include "net/net.h"
#include "net/netlobby.h"
#include "net/nettransport.h"
#include "netint.h"
#include "netrdv.h"

/**
 * The lobby client: tools/pdlobbyd/README.md is the API it speaks, and
 * netlobby.h says how the pieces fit.
 *
 * Two worker threads of its own, never ghostnet's one-job worker (a 20 s
 * long-poll there would hold up a ghost upload, and the reverse): the
 * action thread takes jobs off a short queue - sign in, list, create, join,
 * the member actions - and sends the host's heartbeat every 5 s; the poll
 * thread sits in GET /rooms/<id>/state while this machine is in a room.
 * Both reach the network through ghostnetSend, the HTTPS transport the ghost
 * client, the updater and the crash reporter share.
 *
 * What the threads hear is parsed on the thread, with a bounded scanner
 * (every read is limited by the reply's end and every string copy by its
 * buffer), into "pending" copies under s_Lock. netLobbyTick, on the main
 * thread, moves those into the copies the menus read and does whatever the
 * room asks of this machine: the host's session, the start of the match,
 * a member's connect with its ticket, the room reopened after the match.
 *
 * Identity is the Ghost Trials account: POST /login with its name and PIN
 * becomes a lobby session token (Authorization: Bearer); create and join
 * hand back a member token for the room (X-PD-Member). A 401 signs in again
 * once; a 410 means the seat is gone, with the lobby's reason.
 */

#define LOBBY_QUEUE        16
#define LOBBY_POLL_WAIT    20   // s the lobby parks a state poll (it caps at 25)
#define LOBBY_HEARTBEAT_MS 5000
#define LOBBY_LAUNCH_WAIT_MS 20000 // the host waits this long for the launched to connect
#define LOBBY_REPLY_MAX    (64 * 1024)
#define LOBBY_LADDER_WAIT_MS 15000 // a member at launch waits this long for its path to the host
#define LOBBY_NETINFO_MS   5000    // a member reports its path and ping at most this often

#define JOB_NONE     0
#define JOB_LIST     1
#define JOB_CREATE   2
#define JOB_JOIN     3
#define JOB_ACTION   4 // POST /rooms/<id>/<path> as the member
#define JOB_LEAVE    5

struct lobbyjob {
	s32 kind;
	u32 epoch;          // the room it was for (an action for a room left since is dropped)
	char path[64];
	char body[1600];
};

s32 g_NetLobbyActive = 0;
s32 g_NetLobbyRoom = 0;

static char s_LobbyUrl[256] = "";

#ifdef PD_GHOST_NET

static SDL_mutex *s_Lock = NULL;
static SDL_cond *s_Wake = NULL;
static SDL_Thread *s_ActionThread = NULL;
static SDL_Thread *s_PollThread = NULL;
static volatile s32 s_Quit = 0;
static s32 s_Threads = 0;        // running, under s_Lock: shutdown waits a moment for them

// under s_Lock: the job queue
static struct lobbyjob s_Queue[LOBBY_QUEUE];
static s32 s_QueueHead = 0;
static s32 s_QueueLen = 0;
static s32 s_Busy = 0;           // user-facing jobs queued or running

// under s_Lock: who this machine is in the lobby
static char s_Session[40] = "";  // the action thread's; the poll thread reads it
static char s_SessionUser[NETLOBBY_MAXUSER + 1] = "";
static char s_RoomId[9] = "";
static char s_Token[40] = "";
static char s_Secret[72] = "";
static s32 s_IsHost = 0;
static u32 s_Epoch = 1;          // bumped on every enter and leave
static s32 s_InRoomShared = 0;

// under s_Lock: what the threads heard, for netLobbyTick
static char s_Message[200] = "";
static u32 s_MessageSeq = 0;
static struct netlobbyroomsum s_PendRooms[NETLOBBY_MAXROOMS];
static s32 s_PendNumRooms = 0;
static s32 s_PendHttpRtt = -1;  // the list request's round trip (worker), halved: the PING estimate when the ECHO is mute
static s32 s_HttpRtt = -1;
static u32 s_PendRoomsSeq = 0;
static struct netlobbyroom s_PendRoom;
static u32 s_PendRoomSeq = 0;
static s64 s_PendTime = 0;        // the lobby's clock in the reply that brought it
static s64 s_PendOffset = 0;      // that clock less ours, when the reply came
static u32 s_EnteredSeq = 0;      // a create or join succeeded
static u32 s_GoneSeq = 0;         // the seat went away (410), or the room closed
static char s_GoneText[200] = "";
static s32 s_LastStatus = 0;
static SDL_atomic_t s_Unreachable; // the last request on either thread got no answer at all
static char s_PendTicket[NET_MAXTICKET + 2] = "";
static char s_JobUser[GHOSTNET_MAXUSER + 2] = "";  // the account as the last job was queued
static char s_JobPin[GHOSTNET_MAXPIN + 2] = "";     // (Ghost Trials edits g_GhostNet* meanwhile)
static char s_UdpId[17] = "";     // this seat's rendezvous id and key (hex), from create/join
static char s_UdpKey[65] = "";
static struct netaddr s_PendUdpAddr; // the lobby's rendezvous, from /ping (action thread)
static u32 s_PendUdpSeq = 0;
static u64 s_UdpTried = 0;        // action thread only
static s32 s_UdpDone = 0;         // action thread only: learnt, or the lobby offers none

#endif

// the main thread's copies
static struct netlobbyroomsum s_Rooms[NETLOBBY_MAXROOMS];
static s32 s_NumRooms = 0;
static struct netlobbyroom s_Room;
static s32 s_CreateMaxHumans = 0; // the size this machine made its room with
static char s_MainMessage[200] = "";
static u32 s_SeenRoomsSeq = 0;
static u32 s_SeenRoomSeq = 0;
static u32 s_SeenEnteredSeq = 0;
static u32 s_SeenGoneSeq = 0;
static u32 s_SeenMessageSeq = 0;
static s32 s_InRoom = 0;
static s32 s_MainIsHost = 0;
static s32 s_CreatePending = 0; // the host's socket is open for a create in flight
static char s_PendTicketMain[NET_MAXTICKET + 2] = ""; // this member's latest ticket
static u64 s_RoomAtMs = 0;      // when s_Room came: the countdown runs on from there
static u32 s_SeenUdpSeq = 0;
static u64 s_LaunchRetryAt = 0;  // a member refused STARTED (the host still loading or ending): connect again then
static s32 s_LaunchRetries = 0;
static u64 s_LaunchSeenMs = 0;  // a member: when this launch was first seen (the path may still be coming)
static u32 s_LaunchSeenAt = 0;
#define LOBBY_MAXCANDS 6
#define LOBBY_CAND_WINDOW_MS 8000   // each fallback address but the last gets this long to answer (the ladder's path: the full window)
static char s_CandAddr[LOBBY_MAXCANDS][64]; // a member at launch: the ways to the host, in order
static u16 s_CandPort[LOBBY_MAXCANDS];
static const char *s_CandHow[LOBBY_MAXCANDS];
static s32 s_CandFull[LOBBY_MAXCANDS];  // the path the ladder found: the full connect window
static s32 s_NumCands = 0;
static s32 s_CandAt = 0;
static s32 s_CandNext = 0;      // the last one found no host: the next one, for the same launch
static u32 s_CandLaunch = 0;    // the launch ("at") the list was made for
static s32 s_ReportedPath = -1; // a member: the path and ping last sent to the roster
static s32 s_ReportedPing = -1;
static u64 s_ReportedAt = 0;

// the launch, as this machine follows it
static u32 s_LaunchHandled = 0;    // the launch (its "at") this machine acted on
static s32 s_LobbyConnSpectator = 0; // a member: the connection it made was a spectator's
static u64 s_LaunchDeadline = 0;
static s32 s_HostWaitStart = 0;    // host: launched, waiting for the members to connect
static s32 s_MatchSeen = 0;        // a match from the room has begun loading here
static s32 s_StopPending = 0;      // the room is gone: close the session once off the match stage
static s32 s_WantRoomMenu = 0;     // back from a match: the room's menu to come up

// the content hash and build the lobby compares
static char s_Content[24] = "";
static SDL_atomic_t s_ContentReady; // s_Content written (by whichever thread hashed first)
static SDL_SpinLock s_ContentLock;
static void lobbyComputeContent(void);
static void lobbyLeaveForNew(void);

// --net-lobby-script host|join: the lobby test (tools/ci/netlobbytest.sh)
#define SCRIPT_NONE 0
#define SCRIPT_HOST 1
#define SCRIPT_JOIN 2
static s32 s_Script = SCRIPT_NONE;
static s32 s_ScriptStep = 0;
static u64 s_ScriptAt = 0;
static s32 s_ScriptLeaveFrame = 600;
static s32 s_ScriptClients = 0;
static s32 s_ScriptSkipLadder = 0; // --net-test-skip-ladder: a launch takes the advertised endpoints
static s32 s_ScriptNoEcho = 0;     // --net-test-no-echo: the list's PING as if UDP to the lobby were blocked; the join script lists only
static s32 s_ScriptShots = 0;      // --net-lobby-shots: the menus up, screenshots of them (netlobbyuitest.sh)
static s32 s_ScriptShotStep = 0;   // 0 nothing yet, 1 menu up, 2 shot taken
static u64 s_ScriptShotAt = 0;

void netLobbyMenuPushRoom(void);     // netlobbymenu.c
void netLobbyMenuPushBriefing(void); // netlobbymenu.c
s32 netLobbyMenuPageUp(s32 room);     // netlobbymenu.c
void screenshotRequest(void);        // screenshot.h
extern struct menudialogdef g_MpEndscreenSavePlayerMenuDialog;

extern s32 g_StageNum;
extern s32 g_MainChangeToStageNum;

PD_CONSTRUCTOR static void netLobbyConfigInit(void)
{
	configRegisterString("Net.LobbyServer", s_LobbyUrl, sizeof(s_LobbyUrl) - 1);
}

static u64 lobbyNowMs(void)
{
	return sysGetMicroseconds() / 1000;
}

/**
 * Net.LobbyServer, or else Mod.GhostServer's host with /pdlobby in place
 * of its last path part ("https://texturepacks.art/pdghosts" ->
 * "https://texturepacks.art/pdlobby", where nginx strips the prefix)
 */
static void lobbyBaseUrl(char *out, s32 size)
{
	const char *slash;
	s32 len;

	if (s_LobbyUrl[0]) {
		snprintf(out, size, "%s", s_LobbyUrl);
	} else {
		slash = strrchr(g_GhostNetUrl, '/');
		len = slash && slash > strstr(g_GhostNetUrl, "//") + 1 ? (s32)(slash - g_GhostNetUrl) : (s32)strlen(g_GhostNetUrl);
		snprintf(out, size, "%.*s/pdlobby", len, g_GhostNetUrl);
	}

	len = strlen(out);

	while (len > 0 && out[len - 1] == '/') {
		out[--len] = '\0';
	}
}

/*
 * A bounded JSON reader: a value is a span [p, e) of the reply, and every
 * step stays inside the span it was given
 */

struct jspan {
	const char *p;
	const char *e;
};

static const char *jsonWs(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
		p++;
	}

	return p;
}

// Past one value starting at p, or NULL if it does not parse
static const char *jsonSkip(const char *p, const char *e, s32 depth)
{
	p = jsonWs(p, e);

	if (p >= e || depth > 16) {
		return NULL;
	}

	if (*p == '"') {
		for (p++; p < e; p++) {
			if (*p == '\\') {
				p++;
			} else if (*p == '"') {
				return p + 1;
			}
		}

		return NULL;
	}

	if (*p == '{' || *p == '[') {
		const char close = *p == '{' ? '}' : ']';

		p = jsonWs(p + 1, e);

		if (p < e && *p == close) {
			return p + 1;
		}

		while (p < e) {
			if (close == '}') {
				p = jsonSkip(p, e, depth + 1); // the key

				if (!p) {
					return NULL;
				}

				p = jsonWs(p, e);

				if (p >= e || *p != ':') {
					return NULL;
				}

				p++;
			}

			p = jsonSkip(p, e, depth + 1);

			if (!p) {
				return NULL;
			}

			p = jsonWs(p, e);

			if (p < e && *p == ',') {
				p++;
				continue;
			}

			if (p < e && *p == close) {
				return p + 1;
			}

			return NULL;
		}

		return NULL;
	}

	// a number, true, false or null
	while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
		p++;
	}

	return p;
}

static s32 jsonParse(const char *text, s32 len, struct jspan *out)
{
	const char *e = text + len;
	const char *p = jsonWs(text, e);
	const char *end;

	if (p >= e || *p != '{') {
		return 0;
	}

	end = jsonSkip(p, e, 0);

	if (!end) {
		return 0;
	}

	out->p = p;
	out->e = end;

	return 1;
}

// The member `key` of the object `obj` (top level only), 1 if found
static s32 jsonGet(struct jspan obj, const char *key, struct jspan *out)
{
	const char *p = obj.p;
	const char *e = obj.e;
	const s32 keylen = strlen(key);

	if (p >= e || *p != '{') {
		return 0;
	}

	p = jsonWs(p + 1, e);

	while (p < e && *p == '"') {
		const char *k = p + 1;
		const char *kend = jsonSkip(p, e, 1);
		const char *v;
		const char *vend;

		if (!kend) {
			return 0;
		}

		p = jsonWs(kend, e);

		if (p >= e || *p != ':') {
			return 0;
		}

		v = jsonWs(p + 1, e);
		vend = jsonSkip(v, e, 1);

		if (!vend) {
			return 0;
		}

		if (kend - 1 - k == keylen && memcmp(k, key, keylen) == 0) {
			out->p = v;
			out->e = vend;
			return 1;
		}

		p = jsonWs(vend, e);

		if (p < e && *p == ',') {
			p = jsonWs(p + 1, e);
		} else {
			break;
		}
	}

	return 0;
}

// Element `index` of the array `arr`
static s32 jsonAt(struct jspan arr, s32 index, struct jspan *out)
{
	const char *p = arr.p;
	const char *e = arr.e;
	s32 i = 0;

	if (p >= e || *p != '[') {
		return 0;
	}

	p = jsonWs(p + 1, e);

	while (p < e && *p != ']') {
		const char *end = jsonSkip(p, e, 1);

		if (!end) {
			return 0;
		}

		if (i++ == index) {
			out->p = p;
			out->e = end;
			return 1;
		}

		p = jsonWs(end, e);

		if (p < e && *p == ',') {
			p = jsonWs(p + 1, e);
		}
	}

	return 0;
}

static s32 jsonIsNull(struct jspan v)
{
	return v.e - v.p == 4 && memcmp(v.p, "null", 4) == 0;
}

/**
 * A string value into out, printable ASCII only (what the game's font can
 * draw; the lobby sends nothing else, but a reply is not trusted for it)
 */
static s32 jsonStr(struct jspan v, char *out, s32 size)
{
	const char *p = v.p;
	s32 i = 0;

	if (size <= 0) {
		return 0;
	}

	out[0] = '\0';

	if (p >= v.e || *p != '"') {
		return 0;
	}

	for (p++; p < v.e && *p != '"' && i + 1 < size; p++) {
		char c = *p;

		if (c == '\\' && p + 1 < v.e) {
			p++;

			switch (*p) {
			case 'n': case 'r': case 't': c = ' '; break;
			case 'u': c = '?'; p += 4 < v.e - p ? 4 : 0; break;
			default: c = *p; break;
			}
		}

		out[i++] = (c >= 0x20 && c < 0x7f) ? c : '?';
	}

	out[i] = '\0';

	return 1;
}

static s64 jsonInt(struct jspan v, s64 def)
{
	char buf[32];
	s32 n = v.e - v.p;

	if (n <= 0 || n >= (s32)sizeof(buf) || jsonIsNull(v)) {
		return def;
	}

	memcpy(buf, v.p, n);
	buf[n] = '\0';

	if (strcmp(buf, "true") == 0) {
		return 1;
	}

	if (strcmp(buf, "false") == 0) {
		return 0;
	}

	if (!((buf[0] >= '0' && buf[0] <= '9') || buf[0] == '-')) {
		return def;
	}

	return (s64)strtod(buf, NULL);
}

static f64 jsonNum(struct jspan v, f64 def)
{
	char buf[32];
	s32 n = v.e - v.p;

	if (n <= 0 || n >= (s32)sizeof(buf) || jsonIsNull(v)) {
		return def;
	}

	memcpy(buf, v.p, n);
	buf[n] = '\0';

	return strtod(buf, NULL);
}

static s64 jsonGetInt(struct jspan obj, const char *key, s64 def)
{
	struct jspan v;

	return jsonGet(obj, key, &v) ? jsonInt(v, def) : def;
}

static s32 jsonGetStr(struct jspan obj, const char *key, char *out, s32 size)
{
	struct jspan v;

	if (size > 0) {
		out[0] = '\0';
	}

	return jsonGet(obj, key, &v) && jsonStr(v, out, size);
}

/*
 * Parsing the replies (on the worker threads)
 */

static s32 lobbyCompat(const struct netlobbyroomsum *r)
{
	if (r->proto != NET_PROTOCOL_VERSION) {
		return NETLOBBY_COMPAT_PROTO;
	}

	// until the hash is in (the action thread makes it first thing) no
	// room is marked for content
	if (SDL_AtomicGet(&s_ContentReady) && strcasecmp(r->content, s_Content) != 0) {
		return NETLOBBY_COMPAT_CONTENT;
	}

	if (strcmp(r->build, VERSION_HASH) != 0) {
		return NETLOBBY_COMPAT_BUILD;
	}

	return NETLOBBY_COMPAT_OK;
}

static void lobbyReadSummary(struct jspan o, struct netlobbyroomsum *r)
{
	struct jspan v;

	memset(r, 0, sizeof(*r));
	jsonGetStr(o, "id", r->id, sizeof(r->id));
	jsonGetStr(o, "name", r->name, sizeof(r->name));
	jsonGetStr(o, "host", r->host, sizeof(r->host));
	jsonGetStr(o, "stage", r->stage, sizeof(r->stage));
	jsonGetStr(o, "scenario", r->scenario, sizeof(r->scenario));
	jsonGetStr(o, "build", r->build, sizeof(r->build));
	jsonGetStr(o, "content", r->content, sizeof(r->content));
	jsonGetStr(o, "state", r->state, sizeof(r->state));
	jsonGetStr(o, "region", r->region, sizeof(r->region));
	r->humans = (s32)jsonGetInt(o, "humans", 0);
	r->maxhumans = (s32)jsonGetInt(o, "max_humans", 0);
	r->spectators = (s32)jsonGetInt(o, "spectators", 0);
	r->maxspectators = (s32)jsonGetInt(o, "max_spectators", 0);
	r->sims = (s32)jsonGetInt(o, "sims", 0);
	r->locked = (s32)jsonGetInt(o, "locked", 0);
	r->proto = (s32)jsonGetInt(o, "proto", 0);
	r->dedicated = (s32)jsonGetInt(o, "dedicated", 0);
	r->hostrtt = jsonGet(o, "host_rtt_ms", &v) ? (s32)jsonInt(v, -1) : -1;
	r->compat = lobbyCompat(r);
}

static void lobbyReadState(struct jspan o, struct netlobbyroom *st)
{
	struct jspan v;
	struct jspan el;
	struct jspan arr;
	s32 i;

	memset(st, 0, sizeof(*st));
	st->valid = 1;
	st->version = (s32)jsonGetInt(o, "version", 0);
	st->countdownms = -1;

	if (jsonGet(o, "room", &v)) {
		lobbyReadSummary(v, &st->sum);
	}

	if (jsonGet(o, "members", &arr)) {
		for (i = 0; i < NETLOBBY_MAXMEMBERS && jsonAt(arr, i, &el); i++) {
			struct netlobbymember *m = &st->members[st->nmembers++];

			jsonGetStr(el, "user", m->user, sizeof(m->user));
			m->team = (s32)jsonGetInt(el, "team", 0);
			m->ready = (s32)jsonGetInt(el, "ready", 0);
			m->spectator = (s32)jsonGetInt(el, "spectator", 0);
			m->host = (s32)jsonGetInt(el, "host", 0);
			m->udp = (s32)jsonGetInt(el, "udp", 0);
			jsonGetStr(el, "path", m->path, sizeof(m->path));
			m->ping = jsonGet(el, "ping", &v) && !jsonIsNull(v) ? (s32)jsonInt(v, -1) : -1;
		}
	}

	if (jsonGet(o, "rules", &v) && v.p < v.e && *v.p == '{') {
		// the object's members in order: key, value
		const char *p = jsonWs(v.p + 1, v.e);

		while (p < v.e && *p == '"' && st->nrules < NETLOBBY_MAXRULES) {
			struct jspan k;
			struct jspan val;
			struct netlobbyrule *r = &st->rules[st->nrules];

			k.p = p;
			k.e = jsonSkip(p, v.e, 1);

			if (!k.e) {
				break;
			}

			p = jsonWs(k.e, v.e);

			if (p >= v.e || *p != ':') {
				break;
			}

			val.p = jsonWs(p + 1, v.e);
			val.e = jsonSkip(val.p, v.e, 1);

			if (!val.e) {
				break;
			}

			jsonStr(k, r->key, sizeof(r->key));

			if (!jsonStr(val, r->value, sizeof(r->value))) {
				// not a string: a number or true/false, kept as written;
				// anything else in the token goes as '?' (the menu font has
				// no glyph below 0x21)
				s32 n = val.e - val.p;
				s32 j;

				if (n > (s32)sizeof(r->value) - 1) {
					n = sizeof(r->value) - 1;
				}

				for (j = 0; j < n; j++) {
					const char c = val.p[j];

					r->value[j] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == 'E' || c == '-' || c == '+' || c == '.' ? c : '?';
				}

				r->value[n] = '\0';
			}

			st->nrules++;
			p = jsonWs(val.e, v.e);

			if (p < v.e && *p == ',') {
				p = jsonWs(p + 1, v.e);
			}
		}
	}

	if (jsonGet(o, "countdown", &v) && !jsonIsNull(v)) {
		struct jspan rem;

		if (jsonGet(v, "remaining", &rem)) {
			st->countdownms = (s32)(jsonNum(rem, 0) * 1000.0);
		}
	}

	if (jsonGet(o, "launch", &v) && !jsonIsNull(v)) {
		st->launched = 1;
		st->launchat = (u32)jsonGetInt(v, "at", 0);
		jsonGetStr(v, "public", st->publicep, sizeof(st->publicep));

		if (jsonGet(v, "endpoints", &arr)) {
			for (i = 0; i < 4 && jsonAt(arr, i, &el); i++) {
				jsonStr(el, st->endpoints[st->nendpoints++], sizeof(st->endpoints[0]));
			}
		}
	}

	if (jsonGet(o, "you", &v)) {
		jsonGetStr(v, "user", st->you, sizeof(st->you));
		st->youhost = (s32)jsonGetInt(v, "host", 0);
		st->youspectator = (s32)jsonGetInt(v, "spectator", 0);
	}
}

#ifdef PD_GHOST_NET

/*
 * The worker side
 */

static void lobbySetMessage(const char *text)
{
	SDL_LockMutex(s_Lock);
	snprintf(s_Message, sizeof(s_Message), "%s", text);
	s_MessageSeq++;
	SDL_UnlockMutex(s_Lock);
}

#define AUTH_NONE    0
#define AUTH_SESSION 1
#define AUTH_MEMBER  2

/**
 * One request. Returns the status (0 when no exchange happened, with why in
 * err), the reply in *reply (malloc'd, NUL-terminated, or NULL) and its JSON
 * object in *obj when it parses.
 */
static s32 lobbyRequest(const char *path, const char *body, s32 auth, s32 timeout,
		char **reply, struct jspan *obj, s32 *objok, char *err, s32 errsize)
{
	struct ghostnetbuf buf;
	struct ghostnetreq req;
	char url[400];
	char base[256];
	char headers[160];
	s32 status = 0;

	lobbyBaseUrl(base, sizeof(base));
	snprintf(url, sizeof(url), "%s%s", base, path);

	headers[0] = '\0';
	SDL_LockMutex(s_Lock);

	if (auth == AUTH_SESSION && s_Session[0]) {
		snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", s_Session);
	} else if (auth == AUTH_MEMBER && s_Token[0]) {
		snprintf(headers, sizeof(headers), "X-PD-Member: %s\r\n", s_Token);
	}

	SDL_UnlockMutex(s_Lock);

	memset(&buf, 0, sizeof(buf));
	buf.maxlen = LOBBY_REPLY_MAX;
	memset(&req, 0, sizeof(req));
	req.url = url;
	req.body = body;
	req.bodylen = body ? strlen(body) : 0;
	req.type = body ? "application/json" : NULL;
	req.timeout = timeout;
	req.cancel = &s_Quit;
	req.headers = headers[0] ? headers : NULL;

	*reply = NULL;
	*objok = 0;

	if (!ghostnetSend(&req, &buf, &status, err, errsize)) {
		free(buf.data);

		if (!s_Quit) {
			SDL_AtomicSet(&s_Unreachable, 1);
		}

		return 0;
	}

	SDL_AtomicSet(&s_Unreachable, 0);
	*reply = buf.data;

	if (buf.data) {
		*objok = jsonParse(buf.data, (s32)buf.len, obj);
	}

	if (status == 0) {
		status = 1;
	}

	return status;
}

// The reply's "error", or a stand-in naming the status
static void lobbyReplyError(s32 status, const char *reply, struct jspan obj, s32 objok, const char *err, char *out, s32 size)
{
	char text[160];

	if (status == 0) {
		snprintf(out, size, "Could not reach the lobby: %s.", err && err[0] ? err : "no answer");
	} else if (objok && jsonGetStr(obj, "error", text, sizeof(text)) && text[0]) {
		snprintf(out, size, "%c%s.", text[0] >= 'a' && text[0] <= 'z' ? text[0] - 32 : text[0], text + 1);
	} else {
		snprintf(out, size, "The lobby answered %d.", status);
	}
}

static s32 lobbyLogin(void)
{
	char body[200];
	char user[GHOSTNET_MAXUSER * 6 + 2];
	char pin[GHOSTNET_MAXPIN * 6 + 2];
	char err[128];
	char msg[200];
	char *reply;
	struct jspan obj;
	s32 objok;
	s32 status;
	char session[40];

	// the account as it stood when the job was queued, copied on the main
	// thread: Ghost Trials' pages write g_GhostNetUser/Pin as they are typed
	SDL_LockMutex(s_Lock);
	ghostnetJsonEscape(s_JobUser, user, sizeof(user));
	ghostnetJsonEscape(s_JobPin, pin, sizeof(pin));
	SDL_UnlockMutex(s_Lock);
	snprintf(body, sizeof(body), "{\"username\":\"%s\",\"pin\":\"%s\"}", user, pin);

	status = lobbyRequest("/login", body, AUTH_NONE, 0, &reply, &obj, &objok, err, sizeof(err));

	if (status == 200 && objok && jsonGetStr(obj, "session", session, sizeof(session)) && strlen(session) == 32) {
		SDL_LockMutex(s_Lock);
		snprintf(s_Session, sizeof(s_Session), "%s", session);
		jsonGetStr(obj, "user", s_SessionUser, sizeof(s_SessionUser));
		SDL_UnlockMutex(s_Lock);
		sysLogPrintf(LOG_NOTE, "lobby: signed in as %s", s_SessionUser);
		free(reply);
		return 1;
	}

	lobbyReplyError(status, reply, obj, objok, err, msg, sizeof(msg));
	sysLogPrintf(LOG_WARNING, "lobby: sign-in failed (%d): %s", status, msg);
	lobbySetMessage(msg);
	free(reply);

	return 0;
}

/**
 * A request that needs the session: signs in first if there is none, and
 * once more on a 401 (the lobby restarted, or the session expired)
 */
static s32 lobbySessionRequest(const char *path, const char *body, char **reply, struct jspan *obj, s32 *objok, char *err, s32 errsize)
{
	s32 status;
	s32 have;

	SDL_LockMutex(s_Lock);
	have = s_Session[0] != '\0';
	SDL_UnlockMutex(s_Lock);

	if (!have && !lobbyLogin()) {
		*reply = NULL;
		return -1;
	}

	status = lobbyRequest(path, body, AUTH_SESSION, 0, reply, obj, objok, err, errsize);

	if (status == 401) {
		free(*reply);
		*reply = NULL;

		SDL_LockMutex(s_Lock);
		s_Session[0] = '\0';
		SDL_UnlockMutex(s_Lock);

		if (!lobbyLogin()) {
			return -1;
		}

		status = lobbyRequest(path, body, AUTH_SESSION, 0, reply, obj, objok, err, errsize);
	}

	return status;
}

// The seat is gone (410, or the room closed): the main thread hears why
static void lobbyGone(u32 epoch, const char *why)
{
	SDL_LockMutex(s_Lock);

	if (epoch == s_Epoch && s_InRoomShared) {
		s_InRoomShared = 0;
		s_Epoch++;
		s_Token[0] = '\0';
		snprintf(s_GoneText, sizeof(s_GoneText), "%s", why);
		s_GoneSeq++;
		SDL_CondBroadcast(s_Wake);
	}

	SDL_UnlockMutex(s_Lock);
}

static void lobbyRunJob(struct lobbyjob *job)
{
	char err[128];
	char msg[200];
	char path[96];
	char *reply = NULL;
	struct jspan obj;
	struct jspan v;
	struct jspan el;
	s32 objok = 0;
	s32 status;
	s32 i;

	switch (job->kind) {
	case JOB_LIST:
		{
			const u64 t0 = lobbyNowMs();
			s32 rtt;

			snprintf(path, sizeof(path), "/rooms?build=%s", VERSION_HASH);
			status = lobbyRequest(path, NULL, AUTH_NONE, 0, &reply, &obj, &objok, err, sizeof(err));

			// a fresh connection and the request: about two round trips
			// (more over TLS, so it reads high, never low); the lowest seen
			rtt = (s32)((lobbyNowMs() - t0) / 2);
			rtt = rtt < 1 ? 1 : rtt;

			SDL_LockMutex(s_Lock);

			if (status == 200 && (s_PendHttpRtt < 0 || rtt < s_PendHttpRtt)) {
				s_PendHttpRtt = rtt;
			}

			SDL_UnlockMutex(s_Lock);
		}

		if (status == 200 && objok && jsonGet(obj, "rooms", &v)) {
			SDL_LockMutex(s_Lock);
			s_PendNumRooms = 0;

			for (i = 0; i < NETLOBBY_MAXROOMS && jsonAt(v, i, &el); i++) {
				lobbyReadSummary(el, &s_PendRooms[s_PendNumRooms++]);
			}

			s_PendRoomsSeq++;
			SDL_UnlockMutex(s_Lock);
			sysLogPrintf(LOG_NOTE, "lobby: %d room%s listed", i, i == 1 ? "" : "s");
		} else {
			lobbyReplyError(status, reply, obj, objok, err, msg, sizeof(msg));
			lobbySetMessage(msg);
		}
		break;
	case JOB_CREATE:
	case JOB_JOIN:
		status = lobbySessionRequest(job->path, job->body, &reply, &obj, &objok, err, sizeof(err));

		if (status == 200 && objok) {
			char room[9];
			char token[40];
			char secret[72];

			char udpid[20];
			char udpkey[70];

			jsonGetStr(obj, "room", room, sizeof(room));
			jsonGetStr(obj, "token", token, sizeof(token));
			jsonGetStr(obj, "secret", secret, sizeof(secret));

			if (!jsonGetStr(obj, "udp_id", udpid, sizeof(udpid))) {
				udpid[0] = '\0';
			}

			if (!jsonGetStr(obj, "udp_key", udpkey, sizeof(udpkey))) {
				udpkey[0] = '\0';
			}

			if (strlen(room) == 8 && strlen(token) == 32) {
				SDL_LockMutex(s_Lock);

				if (job->epoch != s_Epoch) {
					// the player left the page (or the room) while this was
					// in flight: the seat is given straight back, and the
					// main thread never hears of it
					snprintf(s_Token, sizeof(s_Token), "%s", token);
					SDL_UnlockMutex(s_Lock);
					snprintf(path, sizeof(path), "/rooms/%s/leave", room);
					free(reply);
					reply = NULL;
					lobbyRequest(path, "{}", AUTH_MEMBER, 0, &reply, &obj, &objok, err, sizeof(err));
					SDL_LockMutex(s_Lock);

					if (!s_InRoomShared) {
						s_Token[0] = '\0';
					}

					SDL_UnlockMutex(s_Lock);
					sysLogPrintf(LOG_NOTE, "lobby: %s room %s after it was called off; left it again", job->kind == JOB_CREATE ? "made" : "joined", room);
					break;
				}

				snprintf(s_RoomId, sizeof(s_RoomId), "%s", room);
				snprintf(s_Token, sizeof(s_Token), "%s", token);
				snprintf(s_Secret, sizeof(s_Secret), "%s", secret);
				snprintf(s_UdpId, sizeof(s_UdpId), "%s", udpid);
				snprintf(s_UdpKey, sizeof(s_UdpKey), "%s", udpkey);
				s_IsHost = job->kind == JOB_CREATE;
				s_InRoomShared = 1;
				s_Epoch++;
				s_PendTime = jsonGetInt(obj, "time", 0);
				s_PendOffset = s_PendTime - (s64)time(NULL);
				s_EnteredSeq++;
				SDL_CondBroadcast(s_Wake);
				SDL_UnlockMutex(s_Lock);
				sysLogPrintf(LOG_NOTE, "lobby: %s room %s", job->kind == JOB_CREATE ? "made" : "joined", room);
				break;
			}
		}

		if (status != -1) {
			lobbyReplyError(status, reply, obj, objok, err, msg, sizeof(msg));
			sysLogPrintf(LOG_WARNING, "lobby: %s failed (%d): %s", job->kind == JOB_CREATE ? "create" : "join", status, msg);
			lobbySetMessage(msg);
		}

		if (job->kind == JOB_CREATE) {
			SDL_LockMutex(s_Lock);

			// with s_InRoomShared 0: the create failed (a create called off
			// meanwhile has nothing left to hear, and a newer one must not
			// read this as its own failure)
			if (job->epoch == s_Epoch) {
				s_EnteredSeq++;
			}

			SDL_UnlockMutex(s_Lock);
		}
		break;
	case JOB_ACTION:
	case JOB_LEAVE:
		SDL_LockMutex(s_Lock);
		i = job->epoch == s_Epoch || job->kind == JOB_LEAVE;
		snprintf(path, sizeof(path), "/rooms/%s/%s", s_RoomId, job->path);
		SDL_UnlockMutex(s_Lock);

		if (!i) {
			break;
		}

		status = lobbyRequest(path, job->body, AUTH_MEMBER, 0, &reply, &obj, &objok, err, sizeof(err));

		if (status == 200) {
			if (strcmp(job->path, "heartbeat") == 0 && objok) {
				SDL_LockMutex(s_Lock);
				if (jsonGetInt(obj, "time", 0) > 0) {
					s_PendTime = jsonGetInt(obj, "time", 0);
					s_PendOffset = s_PendTime - (s64)time(NULL);
				}
				SDL_UnlockMutex(s_Lock);
			}
		} else if (job->kind == JOB_LEAVE) {
			// gone already, or the lobby is down: either way not in it
		} else if (status == 410) {
			lobbyReplyError(status, reply, obj, objok, err, msg, sizeof(msg));
			lobbyGone(job->epoch, msg);
		} else {
			lobbyReplyError(status, reply, obj, objok, err, msg, sizeof(msg));

			// launch refused while some are not ready: name them
			if (objok && jsonGet(obj, "waiting", &v)) {
				s32 len = strlen(msg);

				for (i = 0; i < 4 && jsonAt(v, i, &el) && len < (s32)sizeof(msg) - 24; i++) {
					char who[NETLOBBY_MAXUSER + 1];

					jsonStr(el, who, sizeof(who));
					len += snprintf(msg + len, sizeof(msg) - len, "%s%s", i ? ", " : " Waiting for: ", who);
				}
			}

			sysLogPrintf(LOG_WARNING, "lobby: %s refused (%d): %s", job->path, status, msg);
			lobbySetMessage(msg);
		}

		SDL_LockMutex(s_Lock);
		s_LastStatus = status;
		SDL_UnlockMutex(s_Lock);
		break;
	}

	free(reply);
}

/**
 * The lobby's rendezvous address: the lobby server's host (from the URL)
 * and the UDP port its /ping names. Resolving may block, so it is done
 * here and not on the main thread.
 */
static void lobbyLearnUdp(void)
{
	char base[256];
	char host[200];
	char err[128];
	char *reply = NULL;
	struct jspan obj;
	struct netaddr addr;
	const char *p;
	s32 objok = 0;
	s32 len = 0;
	s32 status;
	s64 port;

	s_UdpTried = lobbyNowMs();
	lobbyBaseUrl(base, sizeof(base));
	p = strstr(base, "://");
	p = p ? p + 3 : base;

	if (*p == '[') {
		p++;

		while (p[len] && p[len] != ']' && len < (s32)sizeof(host) - 1) {
			len++;
		}
	} else {
		while (p[len] && p[len] != ':' && p[len] != '/' && len < (s32)sizeof(host) - 1) {
			len++;
		}
	}

	snprintf(host, sizeof(host), "%.*s", len, p);
	status = lobbyRequest("/ping", NULL, AUTH_NONE, 3, &reply, &obj, &objok, err, sizeof(err));
	port = status == 200 && objok ? jsonGetInt(obj, "udp_port", 0) : 0;
	free(reply);

	if (status == 200 && objok && (port <= 0 || port > 65535)) {
		// the lobby answered and runs no rendezvous: never ask again
		s_UdpDone = 1;
		sysLogPrintf(LOG_NOTE, "lobby: the lobby offers no UDP rendezvous; joins use the advertised endpoints");
		return;
	}

	if (port <= 0 || port > 65535 || !host[0]) {
		return;
	}

	if (netAddrResolve(host, (u16)port, &addr) != 0) {
		sysLogPrintf(LOG_WARNING, "lobby: could not resolve %s for the rendezvous", host);
		return;
	}

	SDL_LockMutex(s_Lock);
	s_PendUdpAddr = addr;
	s_PendUdpSeq++;
	SDL_UnlockMutex(s_Lock);
	s_UdpDone = 1;
}

static int lobbyActionThread(void *arg)
{
	u64 lastbeat = 0;

	lobbyComputeContent();

	for (;;) {
		struct lobbyjob job;
		s32 beat = 0;
		s32 user = 0;
		s32 learn = 0;

		SDL_LockMutex(s_Lock);

		for (;;) {
			const u64 now = lobbyNowMs();

			if (s_Quit) {
				s_Threads--;
				SDL_CondBroadcast(s_Wake);
				SDL_UnlockMutex(s_Lock);
				return 0;
			}

			if (s_QueueLen > 0) {
				break;
			}

			if (s_InRoomShared && s_IsHost && now - lastbeat >= LOBBY_HEARTBEAT_MS) {
				beat = 1;
				break;
			}

			// the rendezvous address, only while nothing the player asked
			// for is waiting (so a dead lobby does not hold their requests
			// up): at once, then every 10 s until learnt
			if (!s_UdpDone && (s_UdpTried == 0 || now - s_UdpTried >= 10000)) {
				learn = 1;
				break;
			}

			SDL_CondWaitTimeout(s_Wake, s_Lock, 500);
		}

		if (learn) {
			SDL_UnlockMutex(s_Lock);
			lobbyLearnUdp();
			continue;
		}

		if (beat) {
			memset(&job, 0, sizeof(job));
			job.kind = JOB_ACTION;
			job.epoch = s_Epoch;
			snprintf(job.path, sizeof(job.path), "heartbeat");
			snprintf(job.body, sizeof(job.body), "{}");
			lastbeat = lobbyNowMs();
		} else {
			job = s_Queue[s_QueueHead];
			s_QueueHead = (s_QueueHead + 1) % LOBBY_QUEUE;
			s_QueueLen--;
			user = job.kind == JOB_LIST || job.kind == JOB_CREATE || job.kind == JOB_JOIN;
		}

		SDL_UnlockMutex(s_Lock);

		lobbyRunJob(&job);

		if (user) {
			SDL_LockMutex(s_Lock);
			s_Busy--;
			SDL_UnlockMutex(s_Lock);
		}
	}
}

static int lobbyPollThread(void *arg)
{
	s32 since = 0;
	u32 epoch = 0;
	s32 failures = 0;

	for (;;) {
		char path[96];
		char err[128];
		char msg[200];
		char *reply = NULL;
		struct jspan obj;
		s32 objok = 0;
		s32 status;

		SDL_LockMutex(s_Lock);

		while (!s_Quit && !s_InRoomShared) {
			SDL_CondWaitTimeout(s_Wake, s_Lock, 1000);
		}

		if (s_Quit) {
			s_Threads--;
			SDL_CondBroadcast(s_Wake);
			SDL_UnlockMutex(s_Lock);
			return 0;
		}

		if (epoch != s_Epoch) {
			epoch = s_Epoch;
			since = 0;
			failures = 0;
		}

		snprintf(path, sizeof(path), "/rooms/%s/state?since=%d&wait=%d", s_RoomId, since, since ? LOBBY_POLL_WAIT : 0);
		SDL_UnlockMutex(s_Lock);

		status = lobbyRequest(path, NULL, AUTH_MEMBER, LOBBY_POLL_WAIT + 15, &reply, &obj, &objok, err, sizeof(err));

		if (status == 200 && objok) {
			struct netlobbyroom *st = malloc(sizeof(*st));

			if (st) {
				lobbyReadState(obj, st);

				SDL_LockMutex(s_Lock);

				if (epoch == s_Epoch) {
					// chat only carries lines after `since`: keep the ones
					// already shown and add the new ones at the end
					if (since && s_PendRoom.valid) {
						s32 n = s_PendRoom.nchat;
						struct netlobbychat old[NETLOBBY_MAXCHAT];
						struct jspan arr;
						struct jspan el;
						s32 i;

						memcpy(old, s_PendRoom.chat, sizeof(old));

						if (jsonGet(obj, "chat", &arr)) {
							for (i = 0; jsonAt(arr, i, &el); i++) {
								if (n == NETLOBBY_MAXCHAT) {
									memmove(old, old + 1, sizeof(old[0]) * (NETLOBBY_MAXCHAT - 1));
									n--;
								}

								jsonGetStr(el, "user", old[n].user, sizeof(old[n].user));
								jsonGetStr(el, "text", old[n].text, sizeof(old[n].text));
								n++;
							}
						}

						memcpy(st->chat, old, sizeof(old));
						st->nchat = n;
					} else {
						struct jspan arr;
						struct jspan el;
						s32 total = 0;
						s32 i;

						// the first reply carries every kept line: the last few
						if (jsonGet(obj, "chat", &arr)) {
							while (jsonAt(arr, total, &el)) {
								total++;
							}

							for (i = total > NETLOBBY_MAXCHAT ? total - NETLOBBY_MAXCHAT : 0; jsonAt(arr, i, &el); i++) {
								jsonGetStr(el, "user", st->chat[st->nchat].user, sizeof(st->chat[0].user));
								jsonGetStr(el, "text", st->chat[st->nchat].text, sizeof(st->chat[0].text));
								st->nchat++;
							}
						}
					}

					s_PendRoom = *st;
					if (jsonGetInt(obj, "time", 0) > 0) {
						s_PendTime = jsonGetInt(obj, "time", 0);
						s_PendOffset = s_PendTime - (s64)time(NULL);
					}

					// the ticket, which only a member gets, from the countdown on
					{
						struct jspan you;

						if (jsonGet(obj, "you", &you)) {
							char ticket[NET_MAXTICKET + 2];

							if (jsonGetStr(you, "ticket", ticket, sizeof(ticket))) {
								snprintf(s_PendTicket, sizeof(s_PendTicket), "%s", ticket);
							}
						}
					}

					s_PendRoomSeq++;
					since = st->version;
				}

				SDL_UnlockMutex(s_Lock);
				free(st);
			}

			failures = 0;
		} else if (status == 410) {
			lobbyReplyError(status, reply, obj, objok, err, msg, sizeof(msg));
			lobbyGone(epoch, msg);
		} else {
			// the lobby is down or the network blinked: try again, slower
			// each time, up to 8 s apart; the seat holds 30 s without a poll
			failures++;

			if (failures == 1 || failures % 10 == 0) {
				lobbyReplyError(status, reply, obj, objok, err, msg, sizeof(msg));
				sysLogPrintf(LOG_WARNING, "lobby: state poll failed (%d): %s", status, msg);
			}

			SDL_LockMutex(s_Lock);

			if (!s_Quit && epoch == s_Epoch) {
				SDL_CondWaitTimeout(s_Wake, s_Lock, failures < 8 ? 500 * failures : 4000);
			}

			SDL_UnlockMutex(s_Lock);
		}

		free(reply);
	}
}

static void lobbyStartThreads(void)
{
	if (s_Lock) {
		return;
	}

	// the lobby socket first: it brings the transport up (on Windows,
	// Winsock), which the action thread's resolving of the lobby needs
	netRdvOpen();

	s_Lock = SDL_CreateMutex();
	s_Wake = SDL_CreateCond();
	s_Threads = 2;
	s_ActionThread = SDL_CreateThread(lobbyActionThread, "lobby", NULL);
	s_PollThread = SDL_CreateThread(lobbyPollThread, "lobbypoll", NULL);
}

static void lobbyQueue(s32 kind, const char *path, const char *body)
{
	lobbyStartThreads();
	g_NetLobbyActive = 1;

	SDL_LockMutex(s_Lock);

	// a different account from the one signed in: sign in again as it
	if (strcmp(s_JobUser, g_GhostNetUser) != 0 || strcmp(s_JobPin, g_GhostNetPin) != 0) {
		snprintf(s_JobUser, sizeof(s_JobUser), "%s", g_GhostNetUser);
		snprintf(s_JobPin, sizeof(s_JobPin), "%s", g_GhostNetPin);
		s_Session[0] = '\0';
	}

	if (s_QueueLen < LOBBY_QUEUE) {
		struct lobbyjob *job = &s_Queue[(s_QueueHead + s_QueueLen) % LOBBY_QUEUE];

		memset(job, 0, sizeof(*job));
		job->kind = kind;
		job->epoch = s_Epoch;
		snprintf(job->path, sizeof(job->path), "%s", path);
		snprintf(job->body, sizeof(job->body), "%s", body ? body : "{}");
		s_QueueLen++;

		if (kind == JOB_LIST || kind == JOB_CREATE || kind == JOB_JOIN) {
			s_Busy++;
		}

		SDL_CondBroadcast(s_Wake);
	} else {
		sysLogPrintf(LOG_WARNING, "lobby: the request queue is full; %s dropped", path);
	}

	SDL_UnlockMutex(s_Lock);
}

#else // PD_GHOST_NET

static void lobbyQueue(s32 kind, const char *path, const char *body)
{
	snprintf(s_MainMessage, sizeof(s_MainMessage), "%s", "This build has no network support.");
}

#endif // PD_GHOST_NET

/*
 * The menus' side (main thread)
 */

/**
 * The Online Game page opening: the threads start, so the content hash is
 * made off the frame before Browse or Create needs it
 */
void netLobbyWarm(void)
{
#ifdef PD_GHOST_NET
	if (netLobbyAvailable()) {
		// the socket the list's pings and a member's ladder go from
		netRdvOpen();
		lobbyStartThreads();
		g_NetLobbyActive = 1; // and stopped on the way out
	}
#endif
}

/**
 * The content hash rooms are matched by. Hashing the ROM and the mods takes
 * some hundreds of ms, so the action thread does it as it starts, not a
 * menu frame; a create or join made before it is in waits for it here.
 */
static void lobbyComputeContent(void)
{
	struct nethashcomp comps[NET_MAXCOMPS];
	char content[sizeof(s_Content)];
	s32 n;
	s32 i;

	if (SDL_AtomicGet(&s_ContentReady)) {
		return;
	}

	SDL_AtomicLock(&s_ContentLock);

	if (!SDL_AtomicGet(&s_ContentReady)) {
		n = netSessionHash(comps, NET_MAXCOMPS);
		snprintf(content, sizeof(content), "%016llx", 0ULL);

		for (i = 0; i < n; i++) {
			if (strcmp(comps[i].name, "all") == 0) {
				snprintf(content, sizeof(content), "%016llx", (unsigned long long)comps[i].hash);
			}
		}

		memcpy(s_Content, content, sizeof(s_Content));
		SDL_AtomicSet(&s_ContentReady, 1);
	}

	SDL_AtomicUnlock(&s_ContentLock);
}

s32 netLobbyAvailable(void)
{
#ifdef PD_GHOST_NET
	return ghostnetHasAccount() && ghostnetAccountIsValid();
#else
	return 0;
#endif
}

const char *netLobbyAccount(void)
{
	return g_GhostNetUser;
}

s32 netLobbySignedIn(void)
{
#ifdef PD_GHOST_NET
	s32 in = 0;

	if (s_Lock) {
		SDL_LockMutex(s_Lock);
		in = s_Session[0] != '\0' && strcasecmp(s_SessionUser, g_GhostNetUser) == 0;
		SDL_UnlockMutex(s_Lock);
	}

	return in;
#else
	return 0;
#endif
}

s32 netLobbyBusy(void)
{
#ifdef PD_GHOST_NET
	s32 busy = 0;

	if (s_Lock) {
		SDL_LockMutex(s_Lock);
		busy = s_Busy > 0;
		SDL_UnlockMutex(s_Lock);
	}

	return busy;
#else
	return 0;
#endif
}

const char *netLobbyMessage(void)
{
	return s_MainMessage;
}

void netLobbyClearMessage(void)
{
	s_MainMessage[0] = '\0';
}

void netLobbyRefresh(void)
{
	lobbyQueue(JOB_LIST, "", NULL);
}

s32 netLobbyNumRooms(void)
{
	return s_NumRooms;
}

const struct netlobbyroomsum *netLobbyRoomAt(s32 index)
{
	return index >= 0 && index < s_NumRooms ? &s_Rooms[index] : NULL;
}

const char *netLobbyCompatText(s32 compat)
{
	switch (compat) {
	// one status line: the menus' small font fits about 42 characters
	case NETLOBBY_COMPAT_PROTO:
		return "Can't join: another netplay version.";
	case NETLOBBY_COMPAT_CONTENT:
		return "Can't join: other ROM, mods or content.";
	case NETLOBBY_COMPAT_BUILD:
		return "Can't join: the host runs another build.";
	}

	return "";
}

/**
 * The arena as the session's stage key names it: a stock stage by number,
 * a Stage Loader map by its name and mod
 */
static void lobbyStageKey(char *out, s32 size)
{
	const char *dir = modloaderGetStageModDir(g_MpSetup.stagenum);
	const char *map = dir ? modloaderGetStageMapName(g_MpSetup.stagenum) : NULL;

	if (map) {
		snprintf(out, size, "map:%s", map);
	} else {
		snprintf(out, size, "stage:%02x", g_MpSetup.stagenum);
	}
}

static void lobbyCleanName(const char *in, char *out, s32 size)
{
	s32 i = 0;

	while (*in && i + 1 < size) {
		const char c = *in++;

		if (c == '\n') {
			break;
		}

		out[i++] = (c >= 0x20 && c < 0x7f) ? c : ' ';
	}

	out[i] = '\0';
}

char *mpMenuTextArenaName(struct menuitem *item);
char *mpMenuTextScenarioShortName(struct menuitem *item);

/**
 * The room's fields from the Combat Simulator setup as it stands (create
 * and the host's settings): the arena, the scenario, the sims and the
 * limits the list and the lobby show. The match itself is the host's
 * g_MpSetup, sent in RULES - these are only its summary.
 */
static s32 lobbySettingsJson(char *out, s32 size, s32 withcompat)
{
	char stage[40];
	char scenario[40];
	char key[48];
	char weapons[40];
	char estage[40 * 6];
	char escen[40 * 6];
	char ekey[48 * 6];
	char eweap[40 * 6];
	const s32 sims = mpGetNumSimSlotsOn();

	lobbyCleanName(mpMenuTextArenaName(NULL), stage, 33);
	lobbyCleanName(mpMenuTextScenarioShortName(NULL), scenario, 33);
	lobbyCleanName(mpGetWeaponSetName(mpGetWeaponSet()), weapons, 33);
	lobbyStageKey(key, sizeof(key));
	ghostnetJsonEscape(stage, estage, sizeof(estage));
	ghostnetJsonEscape(scenario, escen, sizeof(escen));
	ghostnetJsonEscape(key, ekey, sizeof(ekey));
	ghostnetJsonEscape(weapons, eweap, sizeof(eweap));

	return snprintf(out, size,
			"\"stage\":\"%s\",\"scenario\":\"%s\",\"sims\":%d,"
			"\"rules\":{\"stage_key\":\"%.32s\",\"time_limit\":%d,\"score_limit\":%d,\"team_score_limit\":%d,\"weapons\":\"%s\"}%s",
			estage, escen, sims, ekey,
			g_MpSetup.timelimit >= 60 ? 0 : g_MpSetup.timelimit + 1,
			g_MpSetup.scorelimit >= 100 ? 0 : g_MpSetup.scorelimit + 1,
			g_MpSetup.teamscorelimit >= 400 ? 0 : g_MpSetup.teamscorelimit + 1,
			eweap, withcompat ? "," : "");
}

static s32 lobbyCompatJson(char *out, s32 size)
{
	lobbyComputeContent();

	return snprintf(out, size, "\"proto\":%d,\"build\":\"%s\",\"content\":\"%s\"", NET_PROTOCOL_VERSION, VERSION_HASH, s_Content);
}

/**
 * The host's endpoints: the LAN address its traffic leaves from, and
 * loopback (which the lobby keeps only when it is on the same machine, as
 * in a test). The rendezvous's public address comes in 6b.
 */
static s32 lobbyEndpointsJson(char *out, s32 size, u16 port)
{
	struct netaddr me;
	char addr[64];
	s32 len = snprintf(out, size, "\"endpoints\":[");
	s32 n = 0;
	const char *test = sysArgGetString("--net-test-endpoint");

	// a test's dead address ahead of the real ones (netlobbyuitest.sh): the
	// joiner must go on to the next
	if (test && strlen(test) < 48 && strspn(test, "0123456789.:") == strlen(test)) {
		len += snprintf(out + len, size - len, "\"%s\"", test);
		n++;
	}

	if (netLocalAddrFor("8.8.8.8", &me) == 0) {
		netAddrToString(&me, addr, sizeof(addr));

		if (strchr(addr, ':') && addr[0] != '[' && strncmp(addr, "0.", 2) != 0 && strncmp(addr, "127.", 4) != 0) {
			*strchr(addr, ':') = '\0';
			len += snprintf(out + len, size - len, "%s\"%s:%u\"", n ? "," : "", addr, port);
			n++;
		}
	}

	len += snprintf(out + len, size - len, "%s\"127.0.0.1:%u\"]", n ? "," : "", port);

	return len;
}

void netLobbyCreate(const struct netlobbycreate *c)
{
	char body[1500];
	char name[NETLOBBY_MAXROOMNAME * 6 + 2];
	char pw[NETLOBBY_MAXPASSWORD * 6 + 2];
	s32 len;
	s32 maxhumans = c->maxhumans;

	if (maxhumans < 2) {
		maxhumans = 2;
	} else if (maxhumans > MAX_PLAYERS) {
		maxhumans = MAX_PLAYERS;
	}

	lobbyLeaveForNew();

	s_CreateMaxHumans = maxhumans;

	// the room's host is a listen server from the moment the room exists:
	// its port goes in the endpoints, and joiners arrive at launch
	if (netSessionLobbyHost(g_GhostNetUser) != 0) {
		snprintf(s_MainMessage, sizeof(s_MainMessage), "%s", "Could not open the UDP port for the room's game (Net.Port in pd.ini).");
		return;
	}

	s_CreatePending = 1;

	ghostnetJsonEscape(c->name[0] ? c->name : "Room", name, sizeof(name));
	ghostnetJsonEscape(c->password, pw, sizeof(pw));

	len = snprintf(body, sizeof(body), "{\"name\":\"%s\",\"password\":\"%s\",\"max_humans\":%d,", name, pw, maxhumans);
	len += lobbySettingsJson(body + len, sizeof(body) - len, 1);
	len += lobbyCompatJson(body + len, sizeof(body) - len);
	len += snprintf(body + len, sizeof(body) - len, ",");
	len += lobbyEndpointsJson(body + len, sizeof(body) - len, netSessionLobbyPort());
	snprintf(body + len, sizeof(body) - len, "}");

	sysLogPrintf(LOG_NOTE, "lobby: making room \"%s\" (%d players) on port %u", c->name, maxhumans, netSessionLobbyPort());
	lobbyQueue(JOB_CREATE, "/rooms", body);
}

void netLobbyJoin(const char *roomid, const char *password)
{
	char path[40];
	char body[400];
	char pw[NETLOBBY_MAXPASSWORD * 6 + 2];
	s32 len;

	lobbyLeaveForNew();

	s_CreateMaxHumans = 0;

	ghostnetJsonEscape(password ? password : "", pw, sizeof(pw));
	snprintf(path, sizeof(path), "/rooms/%.8s/join", roomid);
	len = snprintf(body, sizeof(body), "{\"password\":\"%s\",", pw);
	len += lobbyCompatJson(body + len, sizeof(body) - len);
	snprintf(body + len, sizeof(body) - len, "}");

	sysLogPrintf(LOG_NOTE, "lobby: joining room %.8s", roomid);
	lobbyQueue(JOB_JOIN, path, body);
}

s32 netLobbyInRoom(void)
{
	return s_InRoom;
}

const struct netlobbyroom *netLobbyGetRoom(void)
{
	return &s_Room;
}

s32 netLobbyIsHost(void)
{
	return s_InRoom && s_MainIsHost;
}

static const struct netlobbymember *lobbyMe(void)
{
	s32 i;

	for (i = 0; i < s_Room.nmembers; i++) {
		if (strcasecmp(s_Room.members[i].user, s_Room.you) == 0) {
			return &s_Room.members[i];
		}
	}

	return NULL;
}

s32 netLobbyMyReady(void)
{
	const struct netlobbymember *m = lobbyMe();

	return m ? m->ready : 0;
}

s32 netLobbyMyTeam(void)
{
	const struct netlobbymember *m = lobbyMe();

	return !m ? 0 : m->spectator ? -1 : m->team;
}

static void lobbyAction(const char *what, const char *body)
{
	if (!s_InRoom) {
		return;
	}

	lobbyQueue(JOB_ACTION, what, body);
}

void netLobbySetReady(s32 ready)
{
	lobbyAction("ready", ready ? "{\"ready\":true}" : "{\"ready\":false}");
}

void netLobbyCycleTeam(void)
{
	const s32 team = netLobbyMyTeam();

	if (team == 0) {
		lobbyAction("team", "{\"team\":1,\"spectator\":false}");
	} else if (team == 1 && !s_MainIsHost) {
		lobbyAction("team", "{\"spectator\":true}");
	} else {
		lobbyAction("team", "{\"team\":0,\"spectator\":false}");
	}
}

void netLobbyChat(const char *text)
{
	char body[NETLOBBY_MAXCHATTEXT * 6 + 32];
	char esc[NETLOBBY_MAXCHATTEXT * 6 + 2];

	if (!text || !text[0]) {
		return;
	}

	ghostnetJsonEscape(text, esc, sizeof(esc));
	snprintf(body, sizeof(body), "{\"text\":\"%s\"}", esc);
	lobbyAction("chat", body);
}

void netLobbySendSettings(void)
{
	char body[1200];
	s32 len;

	if (!s_MainIsHost) {
		return;
	}

	len = snprintf(body, sizeof(body), "{");
	len += lobbySettingsJson(body + len, sizeof(body) - len, 0);
	snprintf(body + len, sizeof(body) - len, "}");
	lobbyAction("settings", body);
}

void netLobbyKick(const char *user)
{
	char body[80];

	snprintf(body, sizeof(body), "{\"user\":\"%.15s\"}", user);
	lobbyAction("kick", body);
}

void netLobbyLaunch(s32 force)
{
	sysLogPrintf(LOG_NOTE, "lobby: LAUNCH%s", force ? " (forced)" : "");
	lobbyAction("launch", force ? "{\"force\":true}" : "{}");
}

void netLobbyCancelLaunch(void)
{
	lobbyAction("launch", "{\"cancel\":true}");
}

/**
 * Out of the room: the lobby is told (on the action thread), the poll
 * stops, and the room's session closes once no match is on the stage
 */
void netLobbyLeave(void)
{
	if (!s_InRoom && !s_CreatePending) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "lobby: leaving room %s", s_Room.sum.id[0] ? s_Room.sum.id : "(new)");

	s_CreateMaxHumans = 0;

#ifdef PD_GHOST_NET
	if (s_InRoom) {
		lobbyQueue(JOB_LEAVE, "leave", "{}");
	}

	if (s_Lock) {
		SDL_LockMutex(s_Lock);
		s_InRoomShared = 0;
		s_Epoch++;
		s_PendRoom.valid = 0;
		s_PendTicket[0] = '\0';
		SDL_CondBroadcast(s_Wake);
		SDL_UnlockMutex(s_Lock);
	}
#endif

	netRdvLeave();
	s_InRoom = 0;
	s_MainIsHost = 0;
	s_CreatePending = 0;
	g_NetLobbyRoom = 0;
	memset(&s_Room, 0, sizeof(s_Room));
	s_StopPending = 1;
	s_HostWaitStart = 0;
	s_WantRoomMenu = 0;
}

/**
 * The page a create or join was made from closed before the reply came:
 * the attempt is called off (a reply that lands later gives its seat
 * straight back, see lobbyRunJob), or, if the room was entered already,
 * the room is left
 */
void netLobbyCancelPending(void)
{
	netLobbyTick();

	if (s_InRoom) {
		netLobbyLeave();
		return;
	}

#ifdef PD_GHOST_NET
	if (s_Lock) {
		SDL_LockMutex(s_Lock);
		s_Epoch++;
		s_PendRoom.valid = 0;
		SDL_CondBroadcast(s_Wake);
		SDL_UnlockMutex(s_Lock);
	}
#endif

	if (s_CreatePending) {
		sysLogPrintf(LOG_NOTE, "lobby: room creation called off");
		s_CreatePending = 0;
		s_StopPending = 1;
	}
}

/**
 * Before a new create or join: whatever room session there was closes
 * now, not on a later tick, where it would close the new room's socket
 */
static void lobbyLeaveForNew(void)
{
	if (s_InRoom || s_CreatePending) {
		netLobbyLeave();
	}

	if (s_StopPending && !netSessionMatchLoading() && g_MainChangeToStageNum < 0) {
		s_StopPending = 0;
		netSessionLobbyStop();
	}
}

s32 netLobbyRosterHas(const char *user)
{
	s32 i;

	if (!s_InRoom || !s_Room.valid) {
		return 0;
	}

	for (i = 0; i < s_Room.nmembers; i++) {
		if (!s_Room.members[i].host && strcasecmp(s_Room.members[i].user, user) == 0) {
			return 1;
		}
	}

	return 0;
}

static s32 lobbyRosterSpectator(const char *user)
{
	s32 i;

	if (!s_InRoom || !s_Room.valid) {
		return 0;
	}

	for (i = 0; i < s_Room.nmembers; i++) {
		if (!s_Room.members[i].host && strcasecmp(s_Room.members[i].user, user) == 0) {
			return s_Room.members[i].spectator;
		}
	}

	return 0;
}

// What is left of the launch countdown, counted on from the last state reply
s32 netLobbyCountdownMs(void)
{
	s32 left;

	if (!s_InRoom || s_Room.countdownms < 0) {
		return -1;
	}

	left = s_Room.countdownms - (s32)(lobbyNowMs() - s_RoomAtMs);

	return left > 0 ? left : 0;
}

s32 netLobbyLaunchState(void)
{
	if (!s_InRoom || !s_Room.valid) {
		return 0;
	}

	if (netSessionMatchLoading()) {
		return 3;
	}

	if (s_Room.launched) {
		return 2;
	}

	return s_Room.countdownms >= 0 ? 1 : 0;
}

/*
 * netLobbyTick: what the room asks of this machine
 */

/**
 * "a.b.c.d:port" or "[v6]:port" into its address and port; 0 if it parses
 */
static s32 lobbySplitEndpoint(const char *ep, char *addr, s32 size, u16 *port)
{
	const char *colon = strrchr(ep, ':');

	if (!colon || colon == ep) {
		return -1;
	}

	if (ep[0] == '[') {
		const char *close = strchr(ep, ']');

		if (!close || close + 1 != colon) {
			return -1;
		}

		snprintf(addr, size, "%.*s", (s32)(close - ep - 1), ep + 1);
	} else {
		snprintf(addr, size, "%.*s", (s32)(colon - ep), ep);
	}

	*port = (u16)atoi(colon + 1);

	return *port ? 0 : -1;
}

static void lobbyHostTick(void)
{
	s32 i;

	// the roster is who may be connected: anyone off it is dropped
	for (i = 0; ; i++) {
		const char *name = netSessionHostClientName(i);

		if (!name) {
			break;
		}

		if (s_Room.valid && !netLobbyRosterHas(name)) {
			sysLogPrintf(LOG_NOTE, "lobby: %s is no longer in the room; dropping their connection", name);
			netSessionHostDropUser(name, "You are no longer in the room.");
			break;
		}

		// a member kept connected from the last match who has moved between
		// the players and the spectators: that connection closes (a
		// player's would be counted into the next match, a spectator's
		// would watch it), and the next launch connects as the other; not
		// mid-match
		if (s_Room.valid && netSessionHostUserSpectating(name) >= 0
				&& (lobbyRosterSpectator(name) != 0) != (netSessionHostUserSpectating(name) != 0)
				&& !netSessionMatchLoading() && g_MainChangeToStageNum < 0) {
			sysLogPrintf(LOG_NOTE, "lobby: %s is %s now; closing their connection", name,
					lobbyRosterSpectator(name) ? "spectating" : "playing");
			netSessionHostDropUser(name, lobbyRosterSpectator(name) ? "You are spectating this room's next match." : "You are playing this room's next match.");
			break;
		}
	}

	if (s_Room.launched && s_Room.launchat != s_LaunchHandled && !s_MatchSeen) {
		s_LaunchHandled = s_Room.launchat;
		s_HostWaitStart = 1;
		s_LaunchDeadline = lobbyNowMs() + LOBBY_LAUNCH_WAIT_MS;
		sysLogPrintf(LOG_NOTE, "lobby: room %s launched; waiting for the players to connect", s_Room.sum.id);
	}

	if (s_HostWaitStart && g_MainChangeToStageNum < 0 && !netSessionMatchLoading()) {
		s32 waiting = 0;
		s32 connected = 0;

		for (i = 0; i < s_Room.nmembers; i++) {
			const struct netlobbymember *m = &s_Room.members[i];

			if (m->host || m->spectator) {
				continue;
			}

			if (netSessionHostSlotOf(m->user) >= 0) {
				connected++;
			} else {
				waiting++;
			}
		}

		if (waiting == 0 || lobbyNowMs() >= s_LaunchDeadline) {
			if (waiting) {
				sysLogPrintf(LOG_WARNING, "lobby: %d player%s did not connect in time; starting without them", waiting, waiting == 1 ? "" : "s");
			}

			s_HostWaitStart = 0;

			// the room's teams onto the players' slots
			for (i = 0; i < s_Room.nmembers; i++) {
				const struct netlobbymember *m = &s_Room.members[i];
				const s32 slot = m->host ? 0 : netSessionHostSlotOf(m->user);

				if (slot >= 0 && slot < MAX_PLAYERS && !m->spectator) {
					g_PlayerConfigsArray[slot].base.team = m->team & 7;
				}
			}

			sysLogPrintf(LOG_NOTE, "lobby: all %d launched player%s connected; starting the match", connected, connected == 1 ? "" : "s");
			netSessionLobbyStartMatch();
		}
	}

	// back from the match: the room opens again
	if (s_MatchSeen && !netSessionMatchLoading() && g_MainChangeToStageNum < 0) {
		s_MatchSeen = 0;
		s_WantRoomMenu = 1;
		sysLogPrintf(LOG_NOTE, "lobby: the match is over; reopening room %s", s_Room.sum.id);
		lobbyAction("reopen", "{}");
	}
}

// one more way to the host for this launch, if it is not one already
static void lobbyAddCand(const char *addr, u16 port, const char *how, s32 full)
{
	s32 i;

	for (i = 0; i < s_NumCands; i++) {
		if (s_CandPort[i] == port && strcmp(s_CandAddr[i], addr) == 0) {
			return;
		}
	}

	if (s_NumCands < LOBBY_MAXCANDS) {
		snprintf(s_CandAddr[s_NumCands], sizeof(s_CandAddr[0]), "%s", addr);
		s_CandPort[s_NumCands] = port;
		s_CandHow[s_NumCands] = how;
		s_CandFull[s_NumCands] = full;
		s_NumCands++;
	}
}

static void lobbyClientTick(void)
{
	char addr[64];
	u16 port;
	s32 i;

	if (netSessionClientJoined()) {
		s_LaunchRetries = 0;
	}

	// a session that ended (refused, left the match, the host gone) closes,
	// so the next launch connects afresh
	if (netSessionClientGone() && !netSessionMatchLoading() && g_MainChangeToStageNum < 0) {
		// a refusal (another build, a setting that must match) is the
		// room's to show, not a notice over the main menu later
		const s32 started = netSessionLastRefuse() == NETREFUSE_STARTED;

		if (netSessionClientUnreached() && s_Room.launched && s_Room.launchat == s_LaunchHandled && s_CandAt + 1 < s_NumCands) {
			// nobody at that address (a LAN address from elsewhere, a
			// forwarded port that is not): the host's next one at once
			g_NetNoticePending = 0;
			s_CandAt++;
			s_CandNext = 1;
			s_LaunchHandled = 0;
			sysLogPrintf(LOG_NOTE, "lobby: no host at that address; trying the next (%d of %d)", s_CandAt + 1, s_NumCands);
		} else if (started && s_Room.launched && s_LaunchRetries < 30) {
			// the host was loading the match, or ending it: the room is
			// still launched, so the same launch is tried again shortly
			g_NetNoticePending = 0;
			s_LaunchRetries++;
			s_LaunchRetryAt = lobbyNowMs() + 2000;
			// that address reached the host: the retry goes back to it,
			// not through the dead ones ahead of it in the list
			s_CandNext = 1;
			snprintf(s_MainMessage, sizeof(s_MainMessage), "%s", "The match is starting; joining in a moment.");
			sysLogPrintf(LOG_NOTE, "lobby: the host is starting or ending the match; connecting again in 2 s (try %d)", s_LaunchRetries);
		} else if (g_NetNoticePending) {
			g_NetNoticePending = 0;
			snprintf(s_MainMessage, sizeof(s_MainMessage), "%s", netSessionNoticeText());
		}

		netSessionLobbyStop();
	}

	if (s_LaunchRetryAt && lobbyNowMs() >= s_LaunchRetryAt && netSessionLobbyRole() == 0) {
		s_LaunchRetryAt = 0;

		if (s_Room.launched && s_Room.launchat == s_LaunchHandled) {
			s_LaunchHandled = 0;
		}
	}

	// moved between the players and the spectators while still connected
	// from the last match: that connection closes, and the next launch
	// connects as the other (a spectator connects too, protocol 9)
	if (netSessionLobbyRole() == 2 && (s_Room.youspectator != 0) != (s_LobbyConnSpectator != 0)
			&& !netSessionMatchLoading() && g_MainChangeToStageNum < 0) {
		sysLogPrintf(LOG_NOTE, "lobby: %s in room %s; closing the %s connection", s_Room.youspectator ? "spectating" : "playing",
				s_Room.sum.id, s_LobbyConnSpectator ? "spectator's" : "player's");
		netSessionLobbyStop();
	}

	// a launched room is joined while its match runs too (protocol 9): the
	// first state seen has the launch in it, and the connect is the same
	if (s_Room.launched && s_Room.launchat != s_LaunchHandled) {
		if (netSessionLobbyRole() == 2) {
			// still connected from the last match: the host counts it in
			s_LaunchHandled = s_Room.launchat;
		} else if (s_PendTicketMain[0] && netSessionLobbyRole() == 0) {
			const s32 ladder = netRdvLadder();
			const char *how;

			if (s_LaunchSeenAt != s_Room.launchat) {
				s_LaunchSeenAt = s_Room.launchat;
				s_LaunchSeenMs = lobbyNowMs();

				if (ladder == NETRDV_LADDER_WAITING || ladder == NETRDV_LADDER_RUNNING) {
					sysLogPrintf(LOG_NOTE, "lobby: room %s launched; holding for the path to the host (ladder %d)", s_Room.sum.id, ladder);
				}
			}

			// the ladder still climbing: a moment more for it (a WAITING one
			// gives up by itself within seconds when the rendezvous is mute)
			if ((ladder == NETRDV_LADDER_WAITING || ladder == NETRDV_LADDER_RUNNING)
					&& lobbyNowMs() - s_LaunchSeenMs < LOBBY_LADDER_WAIT_MS) {
				return;
			}

			s_LaunchHandled = s_Room.launchat;

			if (!s_CandNext || s_CandLaunch != s_Room.launchat) {
				// the ways to the host, in order: the path the ladder found,
				// then (that failing, or no path from the rendezvous: its UDP
				// port unreachable, nothing answered) every endpoint the host
				// advertised, LAN first, then where the rendezvous saw it
				s_NumCands = 0;
				s_CandAt = 0;
				s_CandLaunch = s_Room.launchat;

				if (ladder == NETRDV_LADDER_DONE && !s_ScriptSkipLadder && netRdvEndpoint(addr, sizeof(addr), &port) == 0) {
					lobbyAddCand(addr, port, netRdvPathName(netRdvPath()), 1);
				}

				for (i = 0; i < s_Room.nendpoints; i++) {
					if (lobbySplitEndpoint(s_Room.endpoints[i], addr, sizeof(addr), &port) == 0) {
						lobbyAddCand(addr, port, "an advertised endpoint", 0);
					}
				}

				if (s_Room.publicep[0] && lobbySplitEndpoint(s_Room.publicep, addr, sizeof(addr), &port) == 0) {
					lobbyAddCand(addr, port, "the host's public address", 0);
				}

				if (s_NumCands == 0) {
					snprintf(s_MainMessage, sizeof(s_MainMessage), "%s", "The host gave no address to connect to.");
					return;
				}
			}

			s_CandNext = 0;
			snprintf(addr, sizeof(addr), "%s", s_CandAddr[s_CandAt]);
			port = s_CandPort[s_CandAt];
			how = s_CandHow[s_CandAt];

			sysLogPrintf(LOG_NOTE, "lobby: room %s launched; connecting to %s port %u (endpoint %d of %d: %s) with the lobby's ticket",
					s_Room.sum.id, addr, port, s_CandAt + 1, s_NumCands, how);

			s_LobbyConnSpectator = s_Room.youspectator != 0;
			netSessionSetSpectate(s_LobbyConnSpectator);

			if (netSessionLobbyConnect(addr, port, s_PendTicketMain, netLobbyAccount(), netRdvSocket(),
						s_CandAt + 1 < s_NumCands && !s_CandFull[s_CandAt] ? LOBBY_CAND_WINDOW_MS : 0) != 0) {
				snprintf(s_MainMessage, sizeof(s_MainMessage), "%s", "Could not open a UDP socket for the game.");
			}
		}
	}

	if (s_MatchSeen && !netSessionMatchLoading() && g_MainChangeToStageNum < 0) {
		s_MatchSeen = 0;
		s_WantRoomMenu = 1;
		sysLogPrintf(LOG_NOTE, "lobby: back from the match to room %s", s_Room.sum.id);
	}
}

/**
 * A member's path to the host and its ping, for everyone's roster: when the
 * ladder settles, and when the ping moves, at most every few seconds
 */
static void lobbyReportPath(void)
{
	const s32 ladder = netRdvLadder();
	const s32 path = ladder == NETRDV_LADDER_DONE ? netRdvPath() : ladder == NETRDV_LADDER_FAILED ? NETRDV_PATH_NONE : -1;
	const s32 ping = netRdvPing();
	char body[80];

	if (path < 0 || lobbyNowMs() - s_ReportedAt < LOBBY_NETINFO_MS) {
		return;
	}

	if (path == s_ReportedPath && (ping < 0 || (s_ReportedPing >= 0 && abs(ping - s_ReportedPing) < 10))) {
		return;
	}

	s_ReportedPath = path;
	s_ReportedPing = ping;
	s_ReportedAt = lobbyNowMs();

	if (ping >= 0) {
		snprintf(body, sizeof(body), "{\"path\":\"%s\",\"ping\":%d}", netRdvPathName(path), ping > 9999 ? 9999 : ping);
	} else {
		snprintf(body, sizeof(body), "{\"path\":\"%s\",\"ping\":null}", netRdvPathName(path));
	}

	lobbyAction("netinfo", body);
}

/**
 * A room's ping in the list: this machine's round trip to the lobby plus
 * the lobby's to the host (both measured, README "Ping hint"), -1 unknown.
 * This machine's leg is the rendezvous ECHO; with UDP to the lobby blocked
 * (no ECHO back) it is estimated from the room list's HTTP round trip
 * instead, and *estimate says so (the list shows it as "~n").
 */
s32 netLobbyRoomPingEx(const struct netlobbyroomsum *r, s32 *estimate)
{
	s32 echo = s_ScriptNoEcho ? -1 : netRdvLobbyRtt();

	if (estimate) {
		*estimate = 0;
	}

	if (echo < 0 && s_HttpRtt >= 0) {
		echo = s_HttpRtt;

		if (estimate) {
			*estimate = 1;
		}
	}

	return r && r->hostrtt >= 0 && echo >= 0 ? r->hostrtt + echo : -1;
}

s32 netLobbyRoomPing(const struct netlobbyroomsum *r)
{
	return netLobbyRoomPingEx(r, NULL);
}

static void lobbyScriptTick(void);

void netLobbyTick(void)
{
#ifdef PD_GHOST_NET
	u32 enteredseq;
	u32 goneseq;
	s32 entered = 0;
	s32 gone = 0;
	char udpid[17] = "";
	char udpkey[65] = "";
	char roomid[9] = "";

	if (s_Lock) {
		SDL_LockMutex(s_Lock);

		if (s_PendUdpSeq != s_SeenUdpSeq) {
			s_SeenUdpSeq = s_PendUdpSeq;
			netRdvSetLobby(&s_PendUdpAddr);
		}

		if (s_MessageSeq != s_SeenMessageSeq) {
			s_SeenMessageSeq = s_MessageSeq;
			snprintf(s_MainMessage, sizeof(s_MainMessage), "%s", s_Message);
		}

		if (s_PendRoomsSeq != s_SeenRoomsSeq) {
			s_SeenRoomsSeq = s_PendRoomsSeq;
			memcpy(s_Rooms, s_PendRooms, sizeof(s_Rooms));
			s_NumRooms = s_PendNumRooms;
			s_HttpRtt = s_PendHttpRtt;
		}

		enteredseq = s_EnteredSeq;
		goneseq = s_GoneSeq;

		if (enteredseq != s_SeenEnteredSeq) {
			s_SeenEnteredSeq = enteredseq;
			entered = s_InRoomShared ? 1 : -1;
			s_MainIsHost = s_IsHost;

			if (entered > 0 && s_IsHost) {
				netSessionLobbySetRoom(s_RoomId, s_Secret);
			}

			if (entered > 0) {
				snprintf(roomid, sizeof(roomid), "%s", s_RoomId);
				snprintf(udpid, sizeof(udpid), "%s", s_UdpId);
				snprintf(udpkey, sizeof(udpkey), "%s", s_UdpKey);
			}
		}

		if (goneseq != s_SeenGoneSeq) {
			s_SeenGoneSeq = goneseq;
			gone = 1;
			snprintf(s_MainMessage, sizeof(s_MainMessage), "%s", s_GoneText);
		}

		if (s_InRoom && s_PendRoomSeq != s_SeenRoomSeq && s_PendRoom.valid) {
			s32 i;
			s32 j;

			// a member's path to the host as the roster shows it, logged
			// when it changes (tools/ci/netnattest.sh reads these)
			for (i = 0; i < s_PendRoom.nmembers; i++) {
				const struct netlobbymember *m = &s_PendRoom.members[i];

				for (j = 0; j < s_Room.nmembers; j++) {
					if (strcmp(s_Room.members[j].user, m->user) == 0) {
						break;
					}
				}

				if (m->path[0] && (j == s_Room.nmembers || strcmp(s_Room.members[j].path, m->path) != 0)) {
					sysLogPrintf(LOG_NOTE, "lobby: roster: %s reaches the host by %s (%d ms)", m->user, m->path, m->ping);
				}
			}

			s_SeenRoomSeq = s_PendRoomSeq;
			s_Room = s_PendRoom;
			s_RoomAtMs = lobbyNowMs();
			snprintf(s_PendTicketMain, sizeof(s_PendTicketMain), "%s", s_PendTicket);
		}

		// the offset as of the last reply: our clock runs it on between
		// replies (and on, should the lobby stop answering)
		if (s_PendTime) {
			netSessionLobbyClock(s_PendOffset);
		}

		SDL_UnlockMutex(s_Lock);
	}

	if (entered > 0) {
		s_InRoom = 1;
		s_CreatePending = 0;
		g_NetLobbyRoom = 1;
		s_LaunchHandled = 0;
		s_LaunchSeenAt = 0;
		s_LaunchRetryAt = 0;
		s_LaunchRetries = 0;
		s_MatchSeen = 0;
		s_StopPending = 0;
		s_ReportedPath = -1;
		s_ReportedPing = -1;
		s_ReportedAt = 0;
		memset(&s_Room, 0, sizeof(s_Room));
		s_SeenRoomSeq = 0;

		// the rendezvous: the host registers its session's socket, a member
		// its lobby socket, and the ladder finds the member's path
		netRdvEnter(s_MainIsHost, roomid, udpid, udpkey);
	} else if (entered < 0 && s_CreatePending) {
		// the create was refused: the socket opened for it closes
		s_CreatePending = 0;
		netSessionLobbyStop();
	}

	if (gone && s_InRoom) {
		sysLogPrintf(LOG_NOTE, "lobby: out of room %s: %s", s_Room.sum.id, s_MainMessage);
		netRdvLeave();
		s_InRoom = 0;
		s_MainIsHost = 0;
		g_NetLobbyRoom = 0;
		s_StopPending = 1;
		s_HostWaitStart = 0;
		s_WantRoomMenu = 0;
	}
#endif

	if (netSessionMatchLoading()) {
		s_MatchSeen = 1;
	}

	if (s_StopPending && !netSessionMatchLoading() && g_MainChangeToStageNum < 0) {
		s_StopPending = 0;
		netSessionLobbyStop();
	}

	netRdvTick();

	if (s_InRoom && s_Room.valid) {
		if (s_MainIsHost) {
			lobbyHostTick();
		} else {
			lobbyClientTick();
			lobbyReportPath();
		}
	}

	if (s_Script) {
		lobbyScriptTick();
	}
}

/**
 * Back on the menus after a match from the room (menutick.c's return from a
 * match, or the Perfect Menu after a client's session ended mid-match):
 * the GAME LOBBY over whatever came up
 */
void netLobbyMenuAfterMatch(void)
{
	if (!s_InRoom) {
		return;
	}

	s_WantRoomMenu = 0;

	// the room's player is player 1 of the setup: the menus pause for it
	g_MpSetup.chrslots |= 1;
	sysLogPrintf(LOG_NOTE, "lobby: back in room %s's lobby", s_Room.sum.id);
	netLobbyMenuPushRoom();
}

void netLobbyShutdown(void)
{
#ifdef PD_GHOST_NET
	if (s_InRoom && s_Lock) {
		char path[48];
		char err[64];
		char *reply = NULL;
		struct jspan obj;
		s32 objok;
		s32 reachable;

		// the leave inline: the threads are about to stop. Not when the
		// last request could not reach the lobby at all: the exit would
		// wait on it for nothing, and the lobby reaps the seat in 30 s
		SDL_LockMutex(s_Lock);
		snprintf(path, sizeof(path), "/rooms/%s/leave", s_RoomId);
		reachable = !SDL_AtomicGet(&s_Unreachable);
		SDL_UnlockMutex(s_Lock);

		if (reachable) {
			lobbyRequest(path, "{}", AUTH_MEMBER, 3, &reply, &obj, &objok, err, sizeof(err));
			free(reply);
			sysLogPrintf(LOG_NOTE, "lobby: left room %s on the way out", s_Room.sum.id);
		} else {
			sysLogPrintf(LOG_NOTE, "lobby: the lobby is unreachable; room %s will drop the seat itself", s_Room.sum.id);
		}
	}

	if (s_Lock) {
		SDL_LockMutex(s_Lock);
		s_Quit = 1;
		SDL_CondBroadcast(s_Wake);
		SDL_UnlockMutex(s_Lock);

		// a request in flight sees s_Quit through its cancel pointer, which
		// curl reads as it waits; WinHTTP only between reads, so a parked
		// poll there could hold the exit for its 20 s: a moment, then they
		// are left to the process's end
		{
			const u64 until = lobbyNowMs() + 1500;

			SDL_LockMutex(s_Lock);

			while (s_Threads > 0 && lobbyNowMs() < until) {
				SDL_CondWaitTimeout(s_Wake, s_Lock, 100);
			}

			SDL_UnlockMutex(s_Lock);
		}

		SDL_DetachThread(s_ActionThread);
		SDL_DetachThread(s_PollThread);
		s_ActionThread = NULL;
		s_PollThread = NULL;
	}
#endif

	// a session on the lobby's socket lets go of it before it closes
	if (netRdvSocket() && g_NetHostSocket == netRdvSocket()) {
		netSessionLobbyStop();
	}

	netRdvShutdown();

	s_InRoom = 0;
	g_NetLobbyRoom = 0;
	g_NetLobbyActive = 0;
}

/*
 * The scripted test (tools/ci/netlobbytest.sh): one host, one joiner,
 * every step through the same calls the menus make
 *
 *   --net-lobby-script host|join   which side
 *   --net-lobby-room NAME          the room's name (made, or looked for)
 *   --net-lobby-matches N          matches to play in the room (1)
 *   --net-lobby-leave-frame F      join: End Game at frame F of each match (0 never)
 *   --net-lobby-end-frame F        host: End Game at frame F of each match (0: when
 *                                  the clients have all left)
 *   --net-lobby-shots              the Briefing Room and Game Lobby menus up on the
 *                                  way, each screenshotted (tools/ci/netlobbyuitest.sh)
 */

static s32 s_ScriptMatches = 1;
static s32 s_ScriptPlayed = 0;
static s32 s_ScriptEndFrame = 0;
static const char *s_ScriptRoom = "Lobby Test";

void netLobbyArgs(void)
{
	const char *script = sysArgGetString("--net-lobby-script");
	const char *url = sysArgGetString("--net-lobby");

	if (url) {
		snprintf(s_LobbyUrl, sizeof(s_LobbyUrl), "%s", url);
	}

	if (script) {
		s_Script = strcmp(script, "host") == 0 ? SCRIPT_HOST : strcmp(script, "join") == 0 ? SCRIPT_JOIN : SCRIPT_NONE;
		s_ScriptLeaveFrame = sysArgGetInt("--net-lobby-leave-frame", 600);
		s_ScriptEndFrame = sysArgGetInt("--net-lobby-end-frame", 0);
		s_ScriptMatches = sysArgGetInt("--net-lobby-matches", 1);
		s_ScriptShots = sysArgCheck("--net-lobby-shots");
		s_ScriptSkipLadder = sysArgCheck("--net-test-skip-ladder");
		s_ScriptNoEcho = sysArgCheck("--net-test-no-echo");

		if (sysArgGetString("--net-lobby-room")) {
			s_ScriptRoom = sysArgGetString("--net-lobby-room");
		}

		if (s_Script) {
			g_NetLobbyActive = 1;
			sysLogPrintf(LOG_NOTE, "lobby: --net-lobby-script %s", script);
		}
	}
}

static void lobbyScriptStep(s32 step, const char *what)
{
	s_ScriptStep = step;
	s_ScriptAt = lobbyNowMs();
	sysLogPrintf(LOG_NOTE, "lobby script: %s", what);
}

static s32 lobbyScriptBackInRoom(void)
{
	return !netSessionMatchLoading() && g_StageNum == STAGE_CITRAINING && s_InRoom && s_Room.valid
		&& !s_Room.launched && s_Room.countdownms < 0;
}

static void lobbyScriptTick(void)
{
	const u64 now = lobbyNowMs();
	char text[96];
	s32 i;

	if (s_ScriptAt == 0) {
		s_ScriptAt = now;
	}

	// an end screen goes after a moment, as a player's button press would
	// (this machine's player's menus: a client's are not g_Menus[0])
	if (g_MenuData.root == MENUROOT_MPENDSCREEN) {
		static u64 endsince = 0;
		const s32 prev = g_MpPlayerNum;
		s32 open = 0;

		for (i = 0; i < MAX_PLAYERS; i++) {
			open |= g_Menus[i].curdialog != NULL;
		}

		if (!open) {
			endsince = 0;
		} else if (endsince == 0) {
			endsince = now;
		} else if (now - endsince > 1500) {
			// a profile prompt over a net match's end screen is a bug (spec-stage trap 13)
			for (i = 0; i < MAX_PLAYERS; i++) {
				if (g_Menus[i].curdialog && g_Menus[i].curdialog->definition == &g_MpEndscreenSavePlayerMenuDialog) {
					sysLogPrintf(LOG_WARNING, "lobby script: a Save Player prompt is up for player %d", i);
				}
			}

			sysLogPrintf(LOG_NOTE, "lobby script: closing the end screen");

			for (i = 0; i < MAX_PLAYERS; i++) {
				g_MpPlayerNum = i;

				while (g_Menus[i].curdialog) {
					menuPopDialog();
				}
			}

			g_MpPlayerNum = prev;
			endsince = 0;
		}
	}

	if (s_Script == SCRIPT_HOST) {
		switch (s_ScriptStep) {
		case 0:
			if (now - s_ScriptAt > 3000 && g_MainChangeToStageNum < 0) {
				struct netlobbycreate c;
				const s32 sims = sysArgGetInt("--net-test-sims", 2);

				g_MpSetup.stagenum = sysArgGetInt("--net-test-stage", 0x32);
				mpClearSimSlots();

				for (i = 0; i < sims && i < MAX_BOTS; i++) {
					mpSetSimSlotOn(i, true);
				}

				if (sims > 0) {
					g_Vars.lvmpbotlevel = 1;
				}

				memset(&c, 0, sizeof(c));
				snprintf(c.name, sizeof(c.name), "%s", s_ScriptRoom);
				c.maxhumans = 2;
				netLobbyCreate(&c);
				lobbyScriptStep(1, "host: create room");
			}
			break;
		case 1:
			if (s_InRoom && s_Room.valid && !s_Room.launched && s_Room.countdownms < 0) {
				if (s_ScriptShots && s_ScriptShotStep == 0) {
					netLobbyMenuPushRoom();
					s_ScriptShotStep = 1;
					s_ScriptShotAt = now;
				}

				for (i = 0; i < s_Room.nmembers; i++) {
					if (!s_Room.members[i].host && s_Room.members[i].ready) {
						if (s_ScriptShots && s_ScriptShotStep == 1) {
							// the member's path and ping on the roster first (or 20 s)
							if (!netLobbyMenuPageUp(1)) {
								netLobbyMenuPushRoom();
								s_ScriptShotAt = now;
							} else if ((s_Room.members[i].path[0] && now - s_ScriptShotAt > 3000) || now - s_ScriptShotAt > 20000) {
								screenshotRequest();
								sysLogPrintf(LOG_NOTE, "lobby script: shot the host's Game Lobby: %s path %s ping %d",
										s_Room.members[i].user, s_Room.members[i].path[0] ? s_Room.members[i].path : "-", s_Room.members[i].ping);
								s_ScriptShotStep = 2;
								s_ScriptShotAt = now;
							}
							break;
						}

						if (s_ScriptShots && now - s_ScriptShotAt < 1500) {
							break;
						}

						netLobbyLaunch(0);
						lobbyScriptStep(2, "host: a member is ready; LAUNCH");
						break;
					}
				}
			} else if (!s_CreatePending && !s_InRoom && now - s_ScriptAt > 3000) {
				sysLogPrintf(LOG_ERROR, "lobby script: the room was not made: %s", s_MainMessage);
				fflush(stdout);
				exit(4);
			}
			break;
		case 2:
			if (netSessionMatchLoading()) {
				s_ScriptClients = 0;
				snprintf(text, sizeof(text), "host: in the room's match %d", s_ScriptPlayed + 1);
				lobbyScriptStep(3, text);
			}
			break;
		case 3:
			if (netSessionHostNumClients() > 0) {
				s_ScriptClients = 1;
			}

			if (netSessionMatchActive() && !g_MainIsEndscreen && !netSessionBarrierHeld()) {
				if (s_ScriptEndFrame > 0 && g_Vars.lvframenum >= s_ScriptEndFrame) {
					snprintf(text, sizeof(text), "host: frame %d; End Game", g_Vars.lvframenum);
					lobbyScriptStep(4, text);
					g_Vars.currentplayer->aborted = true;
					mainEndStage();
				} else if (s_ScriptEndFrame <= 0 && s_ScriptClients && netSessionHostNumClients() == 0) {
					// the client left the match: the host ends it too
					lobbyScriptStep(4, "host: the client left the match; End Game");
					g_Vars.currentplayer->aborted = true;
					mainEndStage();
				}
			}
			break;
		case 4:
			if (lobbyScriptBackInRoom()) {
				s_ScriptPlayed++;
				sysLogPrintf(LOG_NOTE, "lobby script: host back in room %s after match %d (version %d, %d members, state %s)",
						s_Room.sum.id, s_ScriptPlayed, s_Room.version, s_Room.nmembers, s_Room.sum.state);

				if (s_ScriptPlayed < s_ScriptMatches) {
					lobbyScriptStep(1, "host: waiting for READY again");
				} else {
					lobbyScriptStep(5, "host: waiting for the client's word");
				}
			}
			break;
		case 5:
			for (i = 0; i < s_Room.nchat; i++) {
				if (strcmp(s_Room.chat[i].text, "back in the room") == 0) {
					sysLogPrintf(LOG_NOTE, "lobby script: host heard %s: \"%s\"; done", s_Room.chat[i].user, s_Room.chat[i].text);
					lobbyScriptStep(6, "host: done");
					break;
				}
			}
			break;
		case 6:
			if (now - s_ScriptAt > 3000) {
				fflush(stdout);
				exit(0);
			}
			break;
		}
	} else if (s_Script == SCRIPT_JOIN) {
		switch (s_ScriptStep) {
		case 0:
			if (now - s_ScriptAt > 3000 && !netLobbyBusy()) {
				netLobbyRefresh();
				lobbyScriptStep(1, "join: list the rooms");
			}
			break;
		case 1:
			if (!netLobbyBusy()) {
				for (i = 0; i < s_NumRooms; i++) {
					if (strcmp(s_Rooms[i].name, s_ScriptRoom) == 0) {
						if (s_ScriptShots && s_ScriptShotStep == 0) {
							// the Briefing Room up until the room's ping is in (or 30 s),
							// over the boot's menus once they are up (Choose Your Reality)
							if (!g_Menus[g_MpPlayerNum].curdialog || g_MenuData.root == 0) {
								break;
							}

							netLobbyMenuPushBriefing();
							s_ScriptShotStep = 1;
							s_ScriptShotAt = now;
							sysLogPrintf(LOG_NOTE, "lobby script: Briefing Room up");
						}

						if (s_ScriptShots && s_ScriptShotStep == 1) {
							if (!netLobbyMenuPageUp(0)) {
								// something of the boot's came up over it
								netLobbyMenuPushBriefing();
								s_ScriptShotAt = now;
								sysLogPrintf(LOG_NOTE, "lobby script: Briefing Room up again");
							} else if ((netLobbyRoomPing(&s_Rooms[i]) >= 0 && now - s_ScriptShotAt > 4000) || now - s_ScriptShotAt > 30000) {
								s32 est = 0;
								const s32 ping = netLobbyRoomPingEx(&s_Rooms[i], &est);

								screenshotRequest();
								sysLogPrintf(LOG_NOTE, "lobby script: shot the Briefing Room: %d rooms, \"%s\" ping %d%s (lobby echo %d, http %d, host %d)",
										s_NumRooms, s_Rooms[i].name, ping, est ? " by HTTP" : "", s_ScriptNoEcho ? -1 : netRdvLobbyRtt(), s_HttpRtt, s_Rooms[i].hostrtt);
								s_ScriptShotStep = 2;
								s_ScriptShotAt = now;
							} else if (now - s_ScriptAt > 2000) {
								netLobbyRefresh();
								s_ScriptAt = now;
							}
							break;
						}

						if (s_ScriptShots && s_ScriptShotStep == 2 && now - s_ScriptShotAt < 1500) {
							break;
						}

						if (s_ScriptNoEcho) {
							// a lister only: the Briefing Room's HTTP-estimated PING was the test
							sysLogPrintf(LOG_NOTE, "lobby script: listed only (--net-test-no-echo); quitting");
							exit(0);
						}

						sysLogPrintf(LOG_NOTE, "lobby script: found room %s \"%s\" by %s, %d/%d, %s, %s, compat %d",
								s_Rooms[i].id, s_Rooms[i].name, s_Rooms[i].host, s_Rooms[i].humans, s_Rooms[i].maxhumans,
								s_Rooms[i].stage, s_Rooms[i].scenario, s_Rooms[i].compat);
						s_ScriptShotStep = 0;
						netLobbyJoin(s_Rooms[i].id, "");
						lobbyScriptStep(2, "join: join the room");
						break;
					}
				}

				if (s_ScriptStep == 1 && s_ScriptShotStep == 0 && now - s_ScriptAt > 2000) {
					netLobbyRefresh();
					s_ScriptAt = now;
				}
			}
			break;
		case 2:
			// READY once the ladder has found the way to the host (or given
			// up), so the launch never waits on it
			if (s_InRoom && s_Room.valid && !s_Room.launched
					&& (netRdvLadder() == NETRDV_LADDER_DONE || netRdvLadder() == NETRDV_LADDER_FAILED || now - s_ScriptAt > 20000)) {
				if (netRdvLadder() == NETRDV_LADDER_DONE) {
					snprintf(text, sizeof(text), "join: path to the host is %s, %d ms; READY", netRdvPathName(netRdvPath()), netRdvPing());
				} else {
					snprintf(text, sizeof(text), "join: no path from the rendezvous (ladder %d); READY", netRdvLadder());
				}

				netLobbySetReady(1);
				lobbyScriptStep(3, text);

				if (s_ScriptShots) {
					netLobbyMenuPushRoom();
					s_ScriptShotStep = 1;
					s_ScriptShotAt = now;
				}
			} else if (!netLobbyBusy() && !s_InRoom && now - s_ScriptAt > 3000) {
				sysLogPrintf(LOG_ERROR, "lobby script: could not join: %s", s_MainMessage);
				fflush(stdout);
				exit(4);
			}
			break;
		case 3:
			// the Game Lobby as the joiner sees it, before the 5 s countdown is out
			if (s_ScriptShots && s_ScriptShotStep == 1 && !netLobbyMenuPageUp(1) && !netSessionMatchLoading()) {
				netLobbyMenuPushRoom();
				s_ScriptShotAt = now;
			} else if (s_ScriptShots && s_ScriptShotStep == 1 && now - s_ScriptShotAt > 2500 && !netSessionMatchLoading()) {
				screenshotRequest();
				sysLogPrintf(LOG_NOTE, "lobby script: shot the joiner's Game Lobby (%d members, path %s, %d ms)",
						s_Room.nmembers, netRdvLadder() == NETRDV_LADDER_DONE ? netRdvPathName(netRdvPath()) : "-", netRdvPing());
				s_ScriptShotStep = 2;
			}

			if (netSessionMatchLoading()) {
				snprintf(text, sizeof(text), "join: in the room's match %d", s_ScriptPlayed + 1);
				lobbyScriptStep(4, text);
			}
			break;
		case 4:
			if (netSessionMatchActive() && s_ScriptLeaveFrame > 0 && g_Vars.lvframenum >= s_ScriptLeaveFrame) {
				snprintf(text, sizeof(text), "join: frame %d; End Game", g_Vars.lvframenum);
				lobbyScriptStep(5, text);
				netClientLeave();
			} else if (!netSessionMatchLoading() && g_StageNum == STAGE_CITRAINING) {
				lobbyScriptStep(5, "join: the host ended the match");
			}
			break;
		case 5:
			if (lobbyScriptBackInRoom() && now - s_ScriptAt > 1000) {
				s_ScriptPlayed++;
				sysLogPrintf(LOG_NOTE, "lobby script: client back in room %s after match %d (version %d, %d members, state %s, session %s)",
						s_Room.sum.id, s_ScriptPlayed, s_Room.version, s_Room.nmembers, s_Room.sum.state,
						netSessionLobbyRole() == 2 ? "still connected" : "closed");

				if (s_ScriptPlayed < s_ScriptMatches) {
					netLobbySetReady(1);
					lobbyScriptStep(3, "join: READY again");
				} else {
					netLobbyChat("back in the room");
					lobbyScriptStep(6, "join: said so in the chat");
				}
			}
			break;
		case 6:
			if (now - s_ScriptAt > 3000) {
				fflush(stdout);
				exit(0);
			}
			break;
		}
	}
}

/**
 * Phase 8: the size of the room this machine hosts, which its matches seat
 * (netsession.c), or 0 when it hosts none
 */
s32 netLobbyRoomMaxHumans(void)
{
	if (s_InRoom && s_Room.valid && s_Room.sum.maxhumans > 0) {
		// Remember the room's latest size (Room Settings can change it) for
		// a moment its record is not valid
		s_CreateMaxHumans = s_Room.sum.maxhumans;
	}

	return s_CreateMaxHumans;
}
