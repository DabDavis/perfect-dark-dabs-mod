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
#include "config.h"
#include "system.h"
#include "versioninfo.h"
#include "net/net.h"
#include "net/netsession.h"
#include "net/netlobby.h"
#include "net/netproto.h"
#include "net/nettransport.h"
#include "netint.h"

/**
 * LAN and Direct Connect: rooms with no lobby server (2026-10-14, the owner:
 * "when entering online add an extra menu before name screen. LAN, Direct
 * Connect, Online Lobbies. So even if Lobbies browser goes offline players
 * can still play LAN and Direct Connect"; "they can reuse the Lobby Browser
 * screen for those modes"; "may have to build the server into the client").
 *
 * The room server is built into the host's game. A LAN or Direct Connect
 * room is kept here the way pdlobbyd keeps a lobby room - its members, their
 * teams and ready marks, its chat, the launch's countdown, who was kicked -
 * and goes to each member over the session as ROOM (netproto.h, protocol
 * 33), whose fields are pdlobbyd's state reply's. netlobby.c gives the
 * Briefing Room and the Game Lobby that room in place of the lobby's, so the
 * pages are the same pages. A member's actions come back as ROOMACT.
 *
 * Unlike a lobby room, the session is the room: the host listens on Net.Port
 * from the moment it makes the room, a member connects to it as it joins
 * (CONNECT's ticket is the room's password), and a member whose connection
 * ends is out of the room. The launch needs nobody to connect: the host
 * starts the match on everyone it has (netlobby.c's lobbyHostTick, as for a
 * lobby room once its members are in).
 *
 * Finding a room: the LAN finder asks every game on the LAN (a broadcast to
 * Net.Port and the three ports after it, and loopback for a game on this
 * machine); Direct Connect asks the addresses typed, kept in pd.ini's
 * Net.DirectRecent. A host answers from its session socket with the room's
 * summary (the lobby list's fields), the finder's round trip is its ping.
 */

#define LAN_ASK_MS        3000 // the finder asks again this often while it is up
#define LAN_KEEP_ROUNDS   2    // a LAN room not heard for this many rounds is gone
#define LAN_COUNTDOWN_MS  5000 // pdlobbyd's countdown
#define LAN_CHAT_WINDOW_MS 10000 // pdlobbyd's chat_window: 6 lines in 10 s
#define LAN_CHAT_BURST    6
#define LAN_MAXKICKED     16
#define LAN_MAXDIRECT     6
#define LAN_DIRECT_WAIT_MS 4000 // a typed address that has not answered in this long has failed
#define LAN_BADPW_WINDOW_MS 60000
#define LAN_BADPW_MAX     5
#define LAN_ROOM_PING_MS  2000 // a member's ping on the roster is sent again this often between matches

#define LANJOIN_NONE       0
#define LANJOIN_CONNECTING 1
#define LANJOIN_IN         2
#define LANJOIN_FAILED     3

static char s_LanName[NETLOBBY_MAXUSER + 1] = "";
static char s_DirectRecent[LAN_MAXDIRECT * 64] = "";

PD_CONSTRUCTOR static void netLanConfigInit(void)
{
	configRegisterString("Net.LanName", s_LanName, sizeof(s_LanName) - 1);
	configRegisterString("Net.DirectRecent", s_DirectRecent, sizeof(s_DirectRecent) - 1);
}

static u64 lanNowMs(void)
{
	return sysGetMicroseconds() / 1000;
}

static u32 lanRandom(void)
{
	static u32 x = 0;

	if (x == 0) {
		x = (u32)sysGetMicroseconds() ^ (u32)time(NULL) ^ (u32)(uintptr_t)&x;
	}

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;

	return x ? x : 0x9e3779b9;
}

// A name a room shows: printable, no leading or trailing spaces, cut to fit
static void lanCleanName(const char *in, char *out, s32 size)
{
	s32 n = 0;

	while (*in == ' ') {
		in++;
	}

	while (*in && *in != '\n' && n < size - 1) {
		out[n++] = (*in >= 0x20 && *in < 0x7f) ? *in : '?';
		in++;
	}

	while (n > 0 && out[n - 1] == ' ') {
		n--;
	}

	out[n] = '\0';
}

/*
 * The name this game plays LAN and Direct Connect as (Name Entry's box in
 * those modes): Net.LanName, else the Ghost Trials account's, else the
 * offline player's
 */

const char *netLanName(void)
{
	static char name[NETLOBBY_MAXUSER + 1];

	if (s_LanName[0]) {
		lanCleanName(s_LanName, name, sizeof(name));

		if (name[0]) {
			return name;
		}
	}

	if (netLobbyAccount() && netLobbyAccount()[0]) {
		lanCleanName(netLobbyAccount(), name, sizeof(name));

		if (name[0]) {
			return name;
		}
	}

	return "";
}

void netLanSetName(const char *text)
{
	char name[NETLOBBY_MAXUSER + 1];

	lanCleanName(text ? text : "", name, sizeof(name));
	snprintf(s_LanName, sizeof(s_LanName), "%s", name);
	configSave(CONFIG_PATH);
}

/*
 * Addresses: private (a LAN's) or not, and parsing what is typed
 */

static s32 lanAddrIsV4(const struct netaddr *a)
{
	static const u8 v4prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

	return memcmp(a->ip, v4prefix, 12) == 0;
}

// Loopback, a private range, link-local: a member on the host's LAN
static s32 lanAddrIsLocal(const struct netaddr *a)
{
	static const u8 v6loop[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

	if (lanAddrIsV4(a)) {
		const u8 *ip = &a->ip[12];

		return ip[0] == 10 || ip[0] == 127 || (ip[0] == 172 && (ip[1] & 0xf0) == 16) || (ip[0] == 192 && ip[1] == 168)
			|| (ip[0] == 169 && ip[1] == 254);
	}

	return memcmp(a->ip, v6loop, 16) == 0 || (a->ip[0] & 0xfe) == 0xfc || (a->ip[0] == 0xfe && (a->ip[1] & 0xc0) == 0x80);
}

/**
 * What a player typed: "host", "host:port", "host port" (the on-screen
 * keyboard has a space and no colon), "[v6]:port" or a bare v6 address.
 * 0 with the host and port (Net.Port's default when none), -1 if it is not
 * an address.
 */
static s32 lanParseAddress(const char *text, char *host, s32 size, u16 *port)
{
	char buf[96];
	char *p;
	char *colon;
	s32 n;

	lanCleanName(text, buf, sizeof(buf));
	*port = NET_DEFAULT_PORT;
	host[0] = '\0';

	if (!buf[0]) {
		return -1;
	}

	// "host port"
	p = strchr(buf, ' ');

	if (p) {
		*p++ = '\0';

		while (*p == ' ') {
			p++;
		}

		n = atoi(p);

		if (n <= 0 || n > 65535) {
			return -1;
		}

		*port = (u16)n;
	}

	if (buf[0] == '[') {
		char *close = strchr(buf, ']');

		if (!close) {
			return -1;
		}

		*close = '\0';

		if (close[1] == ':') {
			n = atoi(close + 2);

			if (n <= 0 || n > 65535) {
				return -1;
			}

			*port = (u16)n;
		}

		snprintf(host, size, "%s", buf + 1);
	} else {
		colon = strchr(buf, ':');

		// one colon: host:port; more: a bare v6 address
		if (colon && !strchr(colon + 1, ':')) {
			*colon = '\0';
			n = atoi(colon + 1);

			if (n <= 0 || n > 65535) {
				return -1;
			}

			*port = (u16)n;
		}

		snprintf(host, size, "%s", buf);
	}

	for (p = host; *p; p++) {
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '.' || *p == '-' || *p == ':')) {
			return -1;
		}
	}

	return host[0] ? 0 : -1;
}

/*
 * The summary on the wire: ROOM's and the finder's answer's (netproto.h)
 */

static void lanWriteSummary(struct netbuf *b, const struct netlobbyroomsum *r)
{
	s32 state = strcmp(r->state, "launched") == 0 ? NETROOM_LAUNCHED : strcmp(r->state, "countdown") == 0 ? NETROOM_COUNTDOWN : NETROOM_OPEN;

	netWriteStr(b, r->id, 8);
	netWriteStr(b, r->name, NETLOBBY_MAXROOMNAME);
	netWriteStr(b, r->host, NETLOBBY_MAXUSER);
	netWriteStr(b, r->stage, 32);
	netWriteStr(b, r->scenario, 32);
	netWriteStr(b, r->mod, NETLOBBY_MAXMOD);
	netWriteStr(b, r->ge, 15);
	netBufWriteU8(b, (u8)state);
	netBufWriteU8(b, (u8)r->humans);
	netBufWriteU8(b, (u8)r->maxhumans);
	netBufWriteU8(b, (u8)r->spectators);
	netBufWriteU8(b, (u8)r->maxspectators);
	netBufWriteU8(b, (u8)r->sims);
	netBufWriteU8(b, r->locked ? 1 : 0);
}

static void lanReadSummary(struct netbuf *b, struct netlobbyroomsum *r)
{
	s32 state;

	netBufReadString(b, r->id, sizeof(r->id));
	netBufReadString(b, r->name, sizeof(r->name));
	netBufReadString(b, r->host, sizeof(r->host));
	netBufReadString(b, r->stage, sizeof(r->stage));
	netBufReadString(b, r->scenario, sizeof(r->scenario));
	netBufReadString(b, r->mod, sizeof(r->mod));
	netBufReadString(b, r->ge, sizeof(r->ge));
	state = netBufReadU8(b);
	r->humans = netBufReadU8(b);
	r->maxhumans = netBufReadU8(b);
	r->spectators = netBufReadU8(b);
	r->maxspectators = netBufReadU8(b);
	r->sims = netBufReadU8(b);
	r->locked = netBufReadU8(b) != 0;

	snprintf(r->state, sizeof(r->state), "%s", state == NETROOM_LAUNCHED ? "launched" : state == NETROOM_COUNTDOWN ? "countdown" : "open");

	// text from the wire the font will draw
	netTextPrintable(r->name, 0);
	netTextPrintable(r->host, 0);
	netTextPrintable(r->stage, 0);
	netTextPrintable(r->scenario, 0);
	netTextPrintable(r->mod, 0);
	netTextPrintable(r->ge, 0);
	r->hostepoch = 1;
	r->hostrtt = -1;
}

/*
 * The host's room
 */

struct lanmember {
	s32 peer; // the session's peer, -1 the host itself
	char user[NETLOBBY_MAXUSER + 1];
	s32 team;
	s32 ready;
	s32 spectator;
	s32 host;
	u64 chatfrom;  // its chat burst's window started then
	s32 chatn;     // ... lines in it
};

static struct {
	s32 on;
	char id[9];
	char name[NETLOBBY_MAXROOMNAME + 1];
	char password[NETLOBBY_MAXPASSWORD + 1];
	s32 maxhumans;
	s32 status;          // NETROOM_*
	u64 countdownend;
	s32 forced;
	u32 launchat;        // the launch's number while launched
	u32 launches;
	u32 version;
	struct lanmember members[NETLOBBY_MAXMEMBERS];
	s32 nmembers;
	struct netlobbychat chat[NETLOBBY_MAXCHAT];
	u32 chatv[NETLOBBY_MAXCHAT];
	s32 nchat;
	char kicked[LAN_MAXKICKED][NETLOBBY_MAXUSER + 1];
	s32 nkicked;
	struct netlobbyroom setup; // the arena, mode, mod, simulants and rules (netLobbyPreviewRoom)
	u32 sent;            // the version last sent
	u64 sentat;
	u64 answerfrom;      // the finder's answers: this second's start ...
	s32 answers;         // ... and how many went in it
	struct {
		u8 ip[16];
		u64 from;
		s32 n;
	} badpw[8];          // wrong passwords by address
	struct {
		char user[NETLOBBY_MAXUSER + 1];
		s32 team;
	} gone[8];           // members gone lately, by name: one back (End Game's reconnect) keeps its team
	s32 gonenext;
	u32 seq;             // the host's own view moved
} s_Host;

static void lanHostChanged(void)
{
	s_Host.version++;
	s_Host.seq++;
}

static s32 lanHostHumans(void)
{
	s32 n = 0;
	s32 i;

	for (i = 0; i < s_Host.nmembers; i++) {
		n += !s_Host.members[i].spectator;
	}

	return n;
}

static s32 lanHostSpectators(void)
{
	return s_Host.nmembers - lanHostHumans();
}

static struct lanmember *lanHostMemberByPeer(s32 peer)
{
	s32 i;

	for (i = 0; i < s_Host.nmembers; i++) {
		if (!s_Host.members[i].host && s_Host.members[i].peer == peer) {
			return &s_Host.members[i];
		}
	}

	return NULL;
}

static struct lanmember *lanHostMemberByName(const char *user)
{
	s32 i;

	for (i = 0; i < s_Host.nmembers; i++) {
		if (strcasecmp(s_Host.members[i].user, user) == 0) {
			return &s_Host.members[i];
		}
	}

	return NULL;
}

static void lanHostCancelCountdown(s32 unlessforced)
{
	if (s_Host.status != NETROOM_COUNTDOWN || (unlessforced && s_Host.forced)) {
		return;
	}

	s_Host.status = NETROOM_OPEN;
	sysLogPrintf(LOG_NOTE, "lan: room %s: the launch's countdown is called off", s_Host.id);
}

static void lanHostSay(const char *user, const char *text, s32 system)
{
	struct netlobbychat *c;

	lanHostChanged();

	if (s_Host.nchat == NETLOBBY_MAXCHAT) {
		memmove(&s_Host.chat[0], &s_Host.chat[1], sizeof(s_Host.chat[0]) * (NETLOBBY_MAXCHAT - 1));
		memmove(&s_Host.chatv[0], &s_Host.chatv[1], sizeof(s_Host.chatv[0]) * (NETLOBBY_MAXCHAT - 1));
		s_Host.nchat--;
	}

	c = &s_Host.chat[s_Host.nchat];
	memset(c, 0, sizeof(*c));
	snprintf(c->user, sizeof(c->user), "%s", user);
	snprintf(c->text, sizeof(c->text), "%s", text);
	c->system = system;
	s_Host.chatv[s_Host.nchat] = s_Host.version;
	s_Host.nchat++;
}

// The arena, the mode and the rest of the summary from the setup as it stands
static void lanHostReadSetup(void)
{
	netLobbyPreviewRoom(&s_Host.setup);
}

/**
 * Made by this machine (Create Room in LAN or Direct Connect): its session
 * listens on Net.Port, and the room is in at once - no server to answer.
 * 0, or -1 with why in err.
 */
s32 netLanHostOpen(const struct netlobbycreate *c, const char *name, char *err, s32 errsize)
{
	struct lanmember *me;

	if (netSessionLanHost(name) != 0) {
		snprintf(err, errsize, "%s", "Could not open the UDP port for the room's game (Net.Port in pd.ini: another game hosting on this computer?).");
		return -1;
	}

	memset(&s_Host, 0, sizeof(s_Host));
	s_Host.on = 1;
	snprintf(s_Host.id, sizeof(s_Host.id), "%08x", lanRandom());
	lanCleanName(c->name[0] ? c->name : "Room", s_Host.name, sizeof(s_Host.name));
	snprintf(s_Host.password, sizeof(s_Host.password), "%s", c->password);
	s_Host.maxhumans = c->maxhumans < 2 ? 2 : c->maxhumans > MAX_PLAYERS ? MAX_PLAYERS : c->maxhumans;
	s_Host.status = NETROOM_OPEN;
	s_Host.version = 1;
	s_Host.sent = 0;

	me = &s_Host.members[s_Host.nmembers++];
	me->peer = -1;
	me->host = 1;
	me->ready = 1;
	snprintf(me->user, sizeof(me->user), "%s", name);

	lanHostReadSetup();
	lanHostChanged();

	sysLogPrintf(LOG_NOTE, "lan: room %s \"%s\" made (%d players%s) on UDP port %u", s_Host.id, s_Host.name, s_Host.maxhumans,
			s_Host.password[0] ? ", a password" : "", netSessionLobbyPort());

	return 0;
}

s32 netLanHosting(void)
{
	return s_Host.on;
}

static s32 lanBadPasswordsFrom(const struct netaddr *a, s32 add)
{
	const u64 now = lanNowMs();
	s32 free = -1;
	s32 i;

	for (i = 0; i < (s32)ARRAYCOUNT(s_Host.badpw); i++) {
		if (s_Host.badpw[i].n && now - s_Host.badpw[i].from > LAN_BADPW_WINDOW_MS) {
			s_Host.badpw[i].n = 0;
		}

		if (s_Host.badpw[i].n && memcmp(s_Host.badpw[i].ip, a->ip, 16) == 0) {
			s_Host.badpw[i].n += add;
			return s_Host.badpw[i].n;
		}

		if (!s_Host.badpw[i].n && free < 0) {
			free = i;
		}
	}

	if (add && free >= 0) {
		memcpy(s_Host.badpw[free].ip, a->ip, 16);
		s_Host.badpw[free].from = now;
		s_Host.badpw[free].n = add;
		return add;
	}

	return 0;
}

/**
 * CONNECT to this machine's room (netsession.c): the password (in the
 * ticket), not one the host removed, not while the launch counts down, and
 * a name of its own in the room (a name taken already gets a number). 0 in.
 */
s32 netLanAdmit(s32 peer, char *name, s32 namesize, const char *password, char *why, s32 whysize)
{
	char base[NETLOBBY_MAXUSER + 1];
	char clean[NETLOBBY_MAXUSER + 1];
	struct netaddr addr;
	s32 haveaddr = g_NetHostSocket && netHostPeerAddr(g_NetHostSocket, peer, &addr) == 0;
	s32 i;

	if (!s_Host.on) {
		snprintf(why, whysize, "%s", "This game has no room open.");
		return -1;
	}

	if (s_Host.password[0]) {
		if (haveaddr && lanBadPasswordsFrom(&addr, 0) >= LAN_BADPW_MAX) {
			snprintf(why, whysize, "%s", "Too many wrong passwords; wait a minute.");
			return -1;
		}

		if (strcmp(password ? password : "", s_Host.password) != 0) {
			if (haveaddr) {
				lanBadPasswordsFrom(&addr, 1);
			}

			snprintf(why, whysize, "%s", "Wrong room password.");
			return -1;
		}
	}

	if (s_Host.status == NETROOM_COUNTDOWN) {
		snprintf(why, whysize, "%s", "The match is starting; join in a moment.");
		return -1;
	}

	lanCleanName(name, clean, sizeof(clean));

	if (!clean[0]) {
		snprintf(clean, sizeof(clean), "%s", "Player");
	}

	for (i = 0; i < s_Host.nkicked; i++) {
		if (strcasecmp(s_Host.kicked[i], clean) == 0) {
			snprintf(why, whysize, "%s", "The host removed you from this room.");
			return -1;
		}
	}

	snprintf(base, sizeof(base), "%s", clean);

	for (i = 2; lanHostMemberByName(clean) && i < 100; i++) {
		char suffix[8];
		s32 room;

		snprintf(suffix, sizeof(suffix), " %d", i);
		room = (s32)sizeof(clean) - 1 - (s32)strlen(suffix);
		snprintf(clean, sizeof(clean), "%.*s%s", room, base, suffix);
	}

	snprintf(name, namesize, "%s", clean);

	return 0;
}

void netLanMemberJoined(s32 peer, const char *name, s32 spectator)
{
	struct lanmember *m;
	s32 count[2] = { 0, 0 };
	s32 i;

	if (!s_Host.on || lanHostMemberByPeer(peer) || s_Host.nmembers >= NETLOBBY_MAXMEMBERS) {
		return;
	}

	for (i = 0; i < s_Host.nmembers; i++) {
		if (!s_Host.members[i].spectator && s_Host.members[i].team < 2) {
			count[s_Host.members[i].team]++;
		}
	}

	m = &s_Host.members[s_Host.nmembers++];
	memset(m, 0, sizeof(*m));
	m->peer = peer;
	m->spectator = spectator;
	m->team = count[1] < count[0] ? 1 : 0; // pdlobbyd's quietest team
	snprintf(m->user, sizeof(m->user), "%s", name);

	for (i = 0; i < (s32)ARRAYCOUNT(s_Host.gone); i++) {
		if (s_Host.gone[i].user[0] && strcasecmp(s_Host.gone[i].user, name) == 0) {
			m->team = s_Host.gone[i].team;
			s_Host.gone[i].user[0] = '\0';
		}
	}

	sysLogPrintf(LOG_NOTE, "lan: room %s: %s joined (%d in it)", s_Host.id, m->user, s_Host.nmembers);
	lanHostChanged();
}

void netLanMemberGone(s32 peer)
{
	struct lanmember *m = lanHostMemberByPeer(peer);

	if (!s_Host.on || !m) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "lan: room %s: %s is out of it", s_Host.id, m->user);
	snprintf(s_Host.gone[s_Host.gonenext].user, sizeof(s_Host.gone[0].user), "%s", m->user);
	s_Host.gone[s_Host.gonenext].team = m->team;
	s_Host.gonenext = (s_Host.gonenext + 1) % ARRAYCOUNT(s_Host.gone);
	memmove(m, m + 1, (char *)&s_Host.members[s_Host.nmembers] - (char *)(m + 1));
	s_Host.nmembers--;
	lanHostCancelCountdown(1);
	lanHostChanged();
}

/**
 * A member's action (pdlobbyd's act_*): one of the room's members, or the
 * host's own (m the host's). why: what refused it, for the host's own page.
 */
static void lanHostAct(struct lanmember *m, s32 action, s32 value, const char *text, char *why, s32 whysize)
{
	why[0] = '\0';

	switch (action) {
	case NETROOMACT_READY:
		if (m->host || s_Host.status == NETROOM_LAUNCHED || m->spectator) {
			return;
		}

		if ((value != 0) != (m->ready != 0)) {
			m->ready = value != 0;

			if (!m->ready) {
				lanHostCancelCountdown(1);
			}

			lanHostChanged();
		}
		break;
	case NETROOMACT_TEAM:
		if (s_Host.status != NETROOM_OPEN) {
			snprintf(why, whysize, "%s", "Teams are fixed once the countdown starts.");
			return;
		}

		if (value >= 0 && value <= 7 && value != m->team) {
			m->team = value;
			lanHostChanged();
		}
		break;
	case NETROOMACT_SPECTATE:
		if (m->host || s_Host.status != NETROOM_OPEN || (value != 0) == (m->spectator != 0)) {
			return;
		}

		if (value && lanHostSpectators() >= NET_MAXSPECS) {
			snprintf(why, whysize, "%s", "No spectator places left.");
			return;
		}

		if (!value && lanHostHumans() >= s_Host.maxhumans) {
			snprintf(why, whysize, "%s", "The room is full.");
			return;
		}

		if (netSessionHostSetSpectator(m->peer, value) != 0) {
			snprintf(why, whysize, "%s", value ? "No spectator places left." : "The room is full.");
			return;
		}

		m->spectator = value != 0;
		m->ready = 0;
		lanHostChanged();
		break;
	case NETROOMACT_CHAT: {
		char line[NETLOBBY_MAXCHATTEXT + 1];
		const u64 now = lanNowMs();
		s32 n;

		snprintf(line, sizeof(line), "%s", text ? text : "");
		netTextPrintable(line, 0);
		n = strlen(line);

		while (n > 0 && line[n - 1] == ' ') {
			line[--n] = '\0';
		}

		if (!line[0] || line[0] == ' ') {
			return;
		}

		if (now - m->chatfrom > LAN_CHAT_WINDOW_MS) {
			m->chatfrom = now;
			m->chatn = 0;
		}

		if (++m->chatn > LAN_CHAT_BURST) {
			snprintf(why, whysize, "%s", "Slow down.");
			return;
		}

		lanHostSay(m->user, line, 0);
		break;
	}
	}
}

// The host's own actions (netlobby.c), which only the host makes
void netLanHostKick(const char *user)
{
	struct lanmember *m;

	if (!s_Host.on || !user || !(m = lanHostMemberByName(user)) || m->host) {
		return;
	}

	if (s_Host.nkicked < LAN_MAXKICKED) {
		snprintf(s_Host.kicked[s_Host.nkicked++], sizeof(s_Host.kicked[0]), "%s", m->user);
	}

	sysLogPrintf(LOG_NOTE, "lan: room %s: the host removes %s", s_Host.id, m->user);
	netSessionHostDropUser(m->user, "The host removed you from the room.");
	netLanMemberGone(m->peer);
}

/**
 * Launch: everyone not watching is ready (or forced), then the countdown;
 * at its end the room is launched (netLanTick) and the host starts the
 * match. 0, or -1 with why.
 */
s32 netLanHostLaunch(s32 force, char *why, s32 whysize)
{
	s32 len = 0;
	s32 n = 0;
	s32 i;

	why[0] = '\0';

	if (!s_Host.on) {
		return -1;
	}

	if (s_Host.status != NETROOM_OPEN) {
		snprintf(why, whysize, "Already %s.", s_Host.status == NETROOM_COUNTDOWN ? "counting down" : "launched");
		return -1;
	}

	if (!force) {
		for (i = 0; i < s_Host.nmembers; i++) {
			const struct lanmember *m = &s_Host.members[i];

			if (!m->host && !m->spectator && !m->ready) {
				if (n++ == 0) {
					len = snprintf(why, whysize, "Not everyone is ready. Waiting for: %s", m->user);
				} else if (n <= 4 && len < whysize - 20) {
					len += snprintf(why + len, whysize - len, ", %s", m->user);
				}
			}
		}

		if (n) {
			return -1;
		}
	}

	s_Host.status = NETROOM_COUNTDOWN;
	s_Host.forced = force;
	s_Host.countdownend = lanNowMs() + LAN_COUNTDOWN_MS;
	lanHostChanged();
	sysLogPrintf(LOG_NOTE, "lan: room %s: launch%s; counting down", s_Host.id, force ? " (forced)" : "");

	return 0;
}

void netLanHostCancelLaunch(void)
{
	if (s_Host.on && s_Host.status == NETROOM_COUNTDOWN) {
		lanHostCancelCountdown(0);
		lanHostChanged();
	}
}

// Back from the match: the room open again, nobody ready (pdlobbyd's reopen)
void netLanHostReopen(void)
{
	s32 i;

	if (!s_Host.on) {
		return;
	}

	s_Host.status = NETROOM_OPEN;
	s_Host.launchat = 0;

	for (i = 0; i < s_Host.nmembers; i++) {
		if (!s_Host.members[i].host) {
			s_Host.members[i].ready = 0;
		}
	}

	lanHostReadSetup();
	lanHostChanged();
}

// The host changed the setup (Room Settings): new terms, everybody readies again
void netLanHostSettings(const char *name, const char *password, s32 maxhumans, s32 withpassword)
{
	s32 i;

	if (!s_Host.on || s_Host.status == NETROOM_LAUNCHED) {
		return;
	}

	if (name && name[0]) {
		lanCleanName(name, s_Host.name, sizeof(s_Host.name));
	}

	if (withpassword) {
		snprintf(s_Host.password, sizeof(s_Host.password), "%s", password ? password : "");
	}

	if (maxhumans >= 2 && maxhumans <= MAX_PLAYERS && maxhumans >= lanHostHumans()) {
		s_Host.maxhumans = maxhumans;
	}

	for (i = 0; i < s_Host.nmembers; i++) {
		if (!s_Host.members[i].host) {
			s_Host.members[i].ready = 0;
		}
	}

	lanHostCancelCountdown(0);
	lanHostReadSetup();
	lanHostChanged();
}

// The host's own ready, team, spectate and chat (as its members' come in ROOMACT)
void netLanHostOwnAct(s32 action, s32 value, const char *text, char *why, s32 whysize)
{
	if (s_Host.on && s_Host.nmembers > 0) {
		lanHostAct(&s_Host.members[0], action, value, text, why, whysize);
	} else {
		why[0] = '\0';
	}
}

static void lanHostSummary(struct netlobbyroomsum *r)
{
	memset(r, 0, sizeof(*r));
	*r = s_Host.setup.sum;
	snprintf(r->id, sizeof(r->id), "%s", s_Host.id);
	snprintf(r->name, sizeof(r->name), "%s", s_Host.name);
	snprintf(r->host, sizeof(r->host), "%s", s_Host.members[0].user);
	snprintf(r->state, sizeof(r->state), "%s", s_Host.status == NETROOM_LAUNCHED ? "launched" : s_Host.status == NETROOM_COUNTDOWN ? "countdown" : "open");
	snprintf(r->build, sizeof(r->build), "%s", VERSION_HASH);
	snprintf(r->content, sizeof(r->content), "%s", netLobbyContentHash());
	r->humans = lanHostHumans();
	r->maxhumans = s_Host.maxhumans;
	r->spectators = lanHostSpectators();
	r->maxspectators = NET_MAXSPECS;
	r->locked = s_Host.password[0] != '\0';
	r->proto = NET_PROTOCOL_VERSION;
	r->hostrtt = -1;
	r->hostepoch = 1;
	r->compat = NETLOBBY_COMPAT_OK;
}

/**
 * The room as member `me` sees it (an index into the members; 0 the host):
 * pdlobbyd's state reply's fields
 */
static void lanHostView(struct netlobbyroom *out, s32 me)
{
	s32 i;

	memset(out, 0, sizeof(*out));
	out->valid = 1;
	out->version = (s32)s_Host.version;
	lanHostSummary(&out->sum);

	for (i = 0; i < s_Host.nmembers && i < NETLOBBY_MAXMEMBERS; i++) {
		const struct lanmember *m = &s_Host.members[i];
		struct netlobbymember *o = &out->members[out->nmembers++];
		struct netpeerstats st;
		struct netaddr a;

		snprintf(o->user, sizeof(o->user), "%s", m->user);
		o->team = m->team;
		o->ready = m->ready;
		o->spectator = m->spectator;
		o->host = m->host;
		o->udp = 1;
		o->ping = -1;

		if (!m->host && g_NetHostSocket) {
			if (netHostPeerAddr(g_NetHostSocket, m->peer, &a) == 0) {
				snprintf(o->path, sizeof(o->path), "%s", lanAddrIsLocal(&a) ? "lan" : "direct");
			}

			if (netHostPeerStats(g_NetHostSocket, m->peer, &st) == 0 && st.connected) {
				o->ping = (s32)st.rtt;
			}
		}
	}

	for (i = 0; i < s_Host.nchat; i++) {
		out->chat[out->nchat++] = s_Host.chat[i];
	}

	for (i = 0; i < s_Host.setup.nrules && i < NETLOBBY_MAXRULES; i++) {
		out->rules[out->nrules++] = s_Host.setup.rules[i];
	}

	out->countdownms = -1;

	if (s_Host.status == NETROOM_COUNTDOWN) {
		const u64 now = lanNowMs();

		out->countdownms = s_Host.countdownend > now ? (s32)(s_Host.countdownend - now) : 0;
	}

	if (s_Host.status == NETROOM_LAUNCHED) {
		out->launched = 1;
		out->launchat = s_Host.launchat;
	}

	if (me >= 0 && me < s_Host.nmembers) {
		snprintf(out->you, sizeof(out->you), "%s", s_Host.members[me].user);
		out->youhost = s_Host.members[me].host;
		out->youspectator = s_Host.members[me].spectator;
	}
}

static void lanWriteRoom(struct netbuf *b, const struct netlobbyroom *r, s32 youslot)
{
	s32 i;

	netBufWriteU8(b, NETMSG_ROOM);
	netBufWriteU32(b, (u32)r->version);
	lanWriteSummary(b, &r->sum);

	netBufWriteU8(b, (u8)r->nrules);

	for (i = 0; i < r->nrules; i++) {
		netWriteStr(b, r->rules[i].key, sizeof(r->rules[i].key) - 1);
		netWriteStr(b, r->rules[i].value, sizeof(r->rules[i].value) - 1);
	}

	netBufWriteU8(b, (u8)r->nmembers);

	for (i = 0; i < r->nmembers; i++) {
		const struct netlobbymember *m = &r->members[i];

		netWriteStr(b, m->user, NETLOBBY_MAXUSER);
		netBufWriteU8(b, (u8)(m->team & 7));
		netBufWriteU8(b, (m->ready ? NETROOMM_READY : 0) | (m->spectator ? NETROOMM_SPECTATOR : 0) | (m->host ? NETROOMM_HOST : 0));
		netBufWriteU8(b, strcmp(m->path, "lan") == 0 ? NETROOMPATH_LAN : strcmp(m->path, "direct") == 0 ? NETROOMPATH_DIRECT : NETROOMPATH_NONE);
		netBufWriteU16(b, m->ping < 0 ? 0xffff : m->ping > 0xfffe ? 0xfffe : (u16)m->ping);
	}

	netBufWriteU8(b, (u8)r->nchat);

	for (i = 0; i < r->nchat; i++) {
		netBufWriteU32(b, s_Host.chatv[i]);
		netWriteStr(b, r->chat[i].user, NETLOBBY_MAXUSER);
		netWriteStr(b, r->chat[i].text, NETLOBBY_MAXCHATTEXT);
		netBufWriteU8(b, r->chat[i].system ? 1 : 0);
	}

	netBufWriteU32(b, (u32)r->countdownms);
	netBufWriteU32(b, r->launched ? r->launchat : 0);
	netWriteStr(b, r->you, NETLOBBY_MAXUSER);
	netBufWriteU8(b, r->youspectator ? NETROOMM_SPECTATOR : 0);
	netBufWriteU8(b, (u8)youslot);
}

static u8 s_RoomBuf[8192];

static void lanHostSendRoom(void)
{
	struct netlobbyroom view;
	struct netbuf b;
	s32 i;

	for (i = 1; i < s_Host.nmembers; i++) {
		const struct lanmember *m = &s_Host.members[i];
		const s32 slot = netSessionHostPeerSlot(m->peer);

		if (slot < 0) {
			continue;
		}

		lanHostView(&view, i);
		netBufInitWrite(&b, s_RoomBuf, sizeof(s_RoomBuf));
		lanWriteRoom(&b, &view, slot);

		if (netBufOk(&b)) {
			netSessionSendPeer(m->peer, NET_CHAN_RELIABLE, s_RoomBuf, netBufLen(&b));
		}
	}

	s_Host.sent = s_Host.version;
	s_Host.sentat = lanNowMs();
}

// ROOMACT from a member; 0 if it did not parse
s32 netLanOnAct(s32 peer, struct netbuf *b)
{
	char text[NETLOBBY_MAXCHATTEXT + 1];
	char why[160];
	struct lanmember *m;
	s32 action = netBufReadU8(b);
	s32 value = (s8)netBufReadU8(b);

	netBufReadString(b, text, sizeof(text));

	if (!netBufOk(b) || netBufRemaining(b) != 0) {
		return 0;
	}

	m = lanHostMemberByPeer(peer);

	if (s_Host.on && m) {
		lanHostAct(m, action, value, text, why, sizeof(why));

		if (why[0]) {
			sysLogPrintf(LOG_NOTE, "lan: room %s: %s's action %d refused: %s", s_Host.id, m->user, action, why);
		}
	}

	return 1;
}

/*
 * A member's side
 */

static struct {
	s32 state;           // LANJOIN_*
	char why[200];
	char addr[64];       // as shown ("1.2.3.4:27100")
	char host[64];       // as connected to
	u16 port;
	char password[NETLOBBY_MAXPASSWORD + 1];
	char name[NETLOBBY_MAXUSER + 1];
	struct netlobbyroom room;
	u32 seq;
	s32 inmatch;         // the session ended while a match was on the stage
	s32 lobbyonly;       // in the room, out of its running match (its own End Game)
} s_Cl;

/**
 * Join a room the finder heard (roomid) as `name`: the session connects to
 * its host with the password. 0, or -1 with why.
 */
static s32 lanFoundIndex(const char *roomid);
static const struct netaddr *lanFoundAddr(s32 index, char *text, s32 size);

s32 netLanJoin(const char *roomid, const char *password, const char *name, char *err, s32 errsize)
{
	char host[64];
	const struct netaddr *a;
	const s32 index = lanFoundIndex(roomid);

	if (index < 0 || !(a = lanFoundAddr(index, host, sizeof(host)))) {
		snprintf(err, errsize, "%s", "That room is gone.");
		return -1;
	}

	memset(&s_Cl, 0, sizeof(s_Cl));
	netAddrToString(a, s_Cl.addr, sizeof(s_Cl.addr));

	// netSessionLanConnect takes the host and the port apart
	if (lanAddrIsV4(a)) {
		snprintf(host, sizeof(host), "%u.%u.%u.%u", a->ip[12], a->ip[13], a->ip[14], a->ip[15]);
	} else if (strrchr(s_Cl.addr, ']')) {
		snprintf(host, sizeof(host), "%.*s", (s32)(strrchr(s_Cl.addr, ']') - s_Cl.addr - 1), s_Cl.addr + 1);
	}

	snprintf(s_Cl.host, sizeof(s_Cl.host), "%s", host);
	snprintf(s_Cl.password, sizeof(s_Cl.password), "%s", password ? password : "");
	snprintf(s_Cl.name, sizeof(s_Cl.name), "%s", name);
	s_Cl.port = a->port;

	if (netSessionLanConnect(host, a->port, password, name, 0) != 0) {
		snprintf(err, errsize, "%s", "Could not open a UDP socket for the game.");
		return -1;
	}

	s_Cl.state = LANJOIN_CONNECTING;
	sysLogPrintf(LOG_NOTE, "lan: joining room %s at %s as \"%s\"", roomid, s_Cl.addr, name);

	return 0;
}

s32 netLanJoinState(void)
{
	return s_Cl.state;
}

const char *netLanWhy(void)
{
	return s_Cl.why;
}

static void lanReadRoom(struct netbuf *b, struct netlobbyroom *out, s32 *youslot)
{
	s32 n;
	s32 i;

	memset(out, 0, sizeof(*out));
	out->valid = 1;
	out->version = (s32)netBufReadU32(b);
	lanReadSummary(b, &out->sum);

	n = netBufReadU8(b);

	for (i = 0; i < n && netBufOk(b); i++) {
		struct netlobbyrule r;

		netBufReadString(b, r.key, sizeof(r.key));
		netBufReadString(b, r.value, sizeof(r.value));
		netTextPrintable(r.value, 0);

		if (out->nrules < NETLOBBY_MAXRULES) {
			out->rules[out->nrules++] = r;
		}
	}

	n = netBufReadU8(b);

	for (i = 0; i < n && netBufOk(b); i++) {
		struct netlobbymember m;
		s32 flags;
		s32 path;
		s32 ping;

		memset(&m, 0, sizeof(m));
		netBufReadString(b, m.user, sizeof(m.user));
		netTextPrintable(m.user, 0);
		m.team = netBufReadU8(b) & 7;
		flags = netBufReadU8(b);
		path = netBufReadU8(b);
		ping = netBufReadU16(b);
		m.ready = (flags & NETROOMM_READY) != 0;
		m.spectator = (flags & NETROOMM_SPECTATOR) != 0;
		m.host = (flags & NETROOMM_HOST) != 0;
		m.udp = 1;
		m.ping = ping == 0xffff ? -1 : ping;
		snprintf(m.path, sizeof(m.path), "%s", path == NETROOMPATH_LAN ? "lan" : path == NETROOMPATH_DIRECT ? "direct" : "");

		if (out->nmembers < NETLOBBY_MAXMEMBERS) {
			out->members[out->nmembers++] = m;
		}
	}

	n = netBufReadU8(b);

	for (i = 0; i < n && netBufOk(b); i++) {
		struct netlobbychat c;

		memset(&c, 0, sizeof(c));
		netBufReadU32(b);
		netBufReadString(b, c.user, sizeof(c.user));
		netBufReadString(b, c.text, sizeof(c.text));
		c.system = netBufReadU8(b) != 0;
		netTextPrintable(c.user, 0);
		netTextPrintable(c.text, 0);

		if (out->nchat < NETLOBBY_MAXCHAT) {
			out->chat[out->nchat++] = c;
		}
	}

	out->countdownms = (s32)netBufReadU32(b);
	out->launchat = netBufReadU32(b);
	out->launched = out->launchat != 0;
	netBufReadString(b, out->you, sizeof(out->you));
	out->youspectator = (netBufReadU8(b) & NETROOMM_SPECTATOR) != 0;
	*youslot = netBufReadU8(b);
	out->sum.proto = NET_PROTOCOL_VERSION;
	out->sum.compat = NETLOBBY_COMPAT_OK;
}

// ROOM from this member's host
void netLanOnRoom(struct netbuf *b)
{
	struct netlobbyroom room;
	s32 youslot = -1;

	lanReadRoom(b, &room, &youslot);

	if (!netBufOk(b) || netBufRemaining(b) != 0) {
		sysLogPrintf(LOG_WARNING, "lan: a ROOM from the host did not parse");
		return;
	}

	if (s_Cl.state == LANJOIN_CONNECTING) {
		s_Cl.state = LANJOIN_IN;
		sysLogPrintf(LOG_NOTE, "lan: in room %s \"%s\" as %s", room.sum.id, room.sum.name, room.you);
	}

	if (s_Cl.state != LANJOIN_IN) {
		return;
	}

	s_Cl.room = room;
	s_Cl.seq++;

	// the match it left is over: in the room as any member
	if (!room.launched) {
		s_Cl.lobbyonly = 0;
	}

	if (!netSessionMatchLoading()) {
		netSessionClientSetSlot(youslot);
	}
}

void netLanAct(s32 action, s32 value, const char *text)
{
	u8 buf[NETLOBBY_MAXCHATTEXT + 16];
	struct netbuf b;
	char why[160];

	if (s_Host.on) {
		netLanHostOwnAct(action, value, text, why, sizeof(why));

		if (why[0]) {
			netLobbySetMessage(why);
		}

		return;
	}

	if (s_Cl.state != LANJOIN_IN) {
		return;
	}

	netBufInitWrite(&b, buf, sizeof(buf));
	netBufWriteU8(&b, NETMSG_ROOMACT);
	netBufWriteU8(&b, (u8)action);
	netBufWriteU8(&b, (u8)(s8)value);
	netWriteStr(&b, text ? text : "", NETLOBBY_MAXCHATTEXT);

	if (netBufOk(&b)) {
		netSessionSendServer(NET_CHAN_RELIABLE, buf, netBufLen(&b), NET_SEND_RELIABLE);
	}
}

// Whichever side this machine is on: the room as it sees it; its seq moves with it
s32 netLanInRoom(void)
{
	return s_Host.on || s_Cl.state == LANJOIN_IN;
}

u32 netLanRoomSeq(void)
{
	return s_Host.on ? s_Host.seq : s_Cl.seq;
}

void netLanRoomView(struct netlobbyroom *out)
{
	if (s_Host.on) {
		lanHostView(out, 0);
	} else if (s_Cl.state == LANJOIN_IN) {
		*out = s_Cl.room;
	} else {
		memset(out, 0, sizeof(*out));
	}
}

/**
 * A member's connection made again, in the same room: after its own End
 * Game, in the room and out of the running match (lobbyonly: the Game
 * Lobby's Back Into the Match is offered, as in a lobby room), or back into
 * that match. The host has it as a member that left and joined again.
 */
static s32 lanReconnect(s32 lobbyonly)
{
	netSessionLobbyStop();

	if (netSessionLanConnect(s_Cl.host, s_Cl.port, s_Cl.password, s_Cl.name, lobbyonly) != 0) {
		return -1;
	}

	s_Cl.lobbyonly = lobbyonly;
	s_Cl.inmatch = 0;
	sysLogPrintf(LOG_NOTE, "lan: connecting to the room's host again (%s)", lobbyonly ? "in the room, out of its match" : "back into its match");

	return 0;
}

s32 netLanCanRejoin(void)
{
	return s_Cl.state == LANJOIN_IN && s_Cl.lobbyonly && s_Cl.room.launched && netSessionClientJoined();
}

void netLanRejoin(void)
{
	if (netLanCanRejoin()) {
		lanReconnect(0);
	}
}

/**
 * Out of the room: a host's closes (its members are told), a member's
 * connection ends. The session closes at once unless a match is on the
 * stage (netlobby.c waits for that, as for a lobby room).
 */
void netLanLeave(void)
{
	if (s_Host.on) {
		sysLogPrintf(LOG_NOTE, "lan: room %s closed by its host", s_Host.id);
	} else if (s_Cl.state != LANJOIN_NONE) {
		sysLogPrintf(LOG_NOTE, "lan: leaving the room");
	}

	memset(&s_Host, 0, sizeof(s_Host));
	memset(&s_Cl, 0, sizeof(s_Cl));
}

/*
 * The finder
 */

struct lanfound {
	struct netlobbyroomsum sum;
	struct netaddr addr;    // the host's session socket
	u32 round;              // the query round it last answered
};

/**
 * A name's lookup, on a thread of its own (DNS may take seconds): the entry
 * holds it by pointer, so the list may move under a lookup still running.
 * state: LANRES_PENDING until the thread's CAS to OK or FAILED; an entry
 * dropped first CASes it to ABANDONED, and the thread frees it then.
 */
#define LANRES_PENDING   0
#define LANRES_OK        1
#define LANRES_FAILED    2
#define LANRES_ABANDONED 3

struct lanresolve {
	char host[64];
	u16 port;
	struct netaddr addr;
	SDL_atomic_t state;
};

struct landirect {
	char text[64];          // as typed
	char host[64];
	u16 port;
	struct lanresolve *res;
	s32 asked;              // a query went to it since it resolved
};

static struct netprobe *s_Probe = NULL;
static s32 s_FinderMode = 0;
static struct lanfound s_Found[NETLOBBY_MAXROOMS];
static s32 s_NumFound = 0;
static u32 s_Nonce = 0;
static u32 s_Round = 0;
static u64 s_AskedAt = 0;
static struct landirect s_Direct[LAN_MAXDIRECT];
static s32 s_NumDirect = 0;
static char s_DirectWant[64] = "";  // a typed address the menu waits to hear from
static u64 s_DirectWantAt = 0;

static s32 lanFoundIndex(const char *roomid)
{
	s32 i;

	for (i = 0; i < s_NumFound; i++) {
		if (strncmp(s_Found[i].sum.id, roomid, 8) == 0) {
			return i;
		}
	}

	return -1;
}

static const struct netaddr *lanFoundAddr(s32 index, char *text, s32 size)
{
	if (index < 0 || index >= s_NumFound) {
		return NULL;
	}

	netAddrToString(&s_Found[index].addr, text, size);

	return &s_Found[index].addr;
}

static int SDLCALL lanResolveThread(void *arg)
{
	struct lanresolve *r = arg;
	struct netaddr a;
	const s32 ok = netAddrResolve(r->host, r->port, &a) == 0;

	if (ok) {
		r->addr = a;
	}

	if (!SDL_AtomicCAS(&r->state, LANRES_PENDING, ok ? LANRES_OK : LANRES_FAILED)) {
		free(r); // its entry went while it looked
	}

	return 0;
}

// An entry's address: an IP at once, a name on a thread
static void lanDirectResolve(struct landirect *d)
{
	struct lanresolve *r = calloc(1, sizeof(*r));
	SDL_Thread *t;
	s32 literal = 1;
	const char *p;

	d->res = r;
	d->asked = 0;

	if (!r) {
		return;
	}

	snprintf(r->host, sizeof(r->host), "%s", d->host);
	r->port = d->port;
	SDL_AtomicSet(&r->state, LANRES_PENDING);

	for (p = d->host; *p; p++) {
		if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == ':' || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
			literal = 0;
		}
	}

	// a dotted quad or a v6 address: no lookup (netAddrResolve parses it)
	if (literal && (strchr(d->host, ':') || strspn(d->host, "0123456789.") == strlen(d->host))) {
		struct netaddr a;

		if (netAddrResolve(d->host, d->port, &a) == 0) {
			r->addr = a;
			SDL_AtomicSet(&r->state, LANRES_OK);
		} else {
			SDL_AtomicSet(&r->state, LANRES_FAILED);
		}

		return;
	}

	t = SDL_CreateThread(lanResolveThread, "lanresolve", r);

	if (t) {
		SDL_DetachThread(t);
	} else {
		lanResolveThread(r);
	}
}

static s32 lanDirectState(const struct landirect *d)
{
	return d->res ? SDL_AtomicGet((SDL_atomic_t *)&d->res->state) : LANRES_FAILED;
}

// An entry leaves the list: its lookup freed, or left to its thread to free
static void lanDirectDrop(struct landirect *d)
{
	if (d->res && !SDL_AtomicCAS(&d->res->state, LANRES_PENDING, LANRES_ABANDONED)) {
		free(d->res);
	}

	d->res = NULL;
}

// The list from Net.DirectRecent, newest first
static void lanDirectLoad(void)
{
	const char *p = s_DirectRecent;

	if (s_NumDirect > 0) {
		return;
	}

	while (*p && s_NumDirect < LAN_MAXDIRECT) {
		const char *end = strchr(p, ';');
		const s32 n = end ? (s32)(end - p) : (s32)strlen(p);
		struct landirect *d = &s_Direct[s_NumDirect];

		memset(d, 0, sizeof(*d));
		snprintf(d->text, sizeof(d->text), "%.*s", n < (s32)sizeof(d->text) - 1 ? n : (s32)sizeof(d->text) - 1, p);

		if (d->text[0] && lanParseAddress(d->text, d->host, sizeof(d->host), &d->port) == 0) {
			s_NumDirect++;
			lanDirectResolve(d);
		}

		p += n;

		if (*p == ';') {
			p++;
		}
	}
}

static void lanDirectSave(void)
{
	s32 len = 0;
	s32 i;

	s_DirectRecent[0] = '\0';

	for (i = 0; i < s_NumDirect; i++) {
		len += snprintf(s_DirectRecent + len, sizeof(s_DirectRecent) - len, "%s%s", i ? ";" : "", s_Direct[i].text);

		if (len >= (s32)sizeof(s_DirectRecent)) {
			break;
		}
	}

	configSave(CONFIG_PATH);
}

static void lanQueryPacket(u8 *out)
{
	memset(out, 0, NETLAN_QUERYLEN - NET_RAWMAGICLEN);
	memcpy(out, "LANQ", 4);
	out[4] = 1;
	out[5] = (u8)(s_Nonce >> 24);
	out[6] = (u8)(s_Nonce >> 16);
	out[7] = (u8)(s_Nonce >> 8);
	out[8] = (u8)s_Nonce;
}

// One round of queries: the LAN (and this machine), or the Direct list
static void lanAsk(void)
{
	u8 q[NETLAN_QUERYLEN - NET_RAWMAGICLEN];
	struct netaddr a;
	s32 i;
	s32 j;

	if (!s_Probe) {
		return;
	}

	s_Nonce = lanRandom();
	s_Round++;
	s_AskedAt = lanNowMs();
	lanQueryPacket(q);

	if (s_FinderMode == NETLOBBY_MODE_LAN) {
		const s32 base = netSessionPort();

		for (i = 0; i < NETLAN_PORTS; i++) {
			const u16 port = (u16)(base + i);

			netProbeBroadcast(s_Probe, port, q, sizeof(q));

			// a game on this machine (the broadcast may not loop back);
			// --net-test-lan-broadcast: the broadcast alone (the LAN test)
			if (sysArgCheck("--net-test-lan-broadcast")) {
				continue;
			}

			memset(&a, 0, sizeof(a));
			a.ip[10] = 0xff;
			a.ip[11] = 0xff;
			a.ip[12] = 127;
			a.ip[15] = 1;
			a.port = port;
			netProbeSend(s_Probe, &a, q, sizeof(q));

			// and the default port's three when Net.Port is set elsewhere
			if (base != NET_DEFAULT_PORT) {
				netProbeBroadcast(s_Probe, (u16)(NET_DEFAULT_PORT + i), q, sizeof(q));
			}
		}
	} else if (s_FinderMode == NETLOBBY_MODE_DIRECT) {
		for (i = 0; i < s_NumDirect; i++) {
			if (lanDirectState(&s_Direct[i]) == LANRES_OK) {
				netProbeSend(s_Probe, &s_Direct[i].res->addr, q, sizeof(q));
				s_Direct[i].asked = 1;
			}
		}
	}

	// a room not heard for a while is gone (a Direct room too: its host
	// closed it or the address stopped answering)
	for (i = 0, j = 0; i < s_NumFound; i++) {
		if (s_Round - s_Found[i].round <= LAN_KEEP_ROUNDS) {
			s_Found[j++] = s_Found[i];
		}
	}

	s_NumFound = j;
}

static void lanOnAnswer(const struct netaddr *from, const u8 *data, s32 len)
{
	struct netlobbyroomsum sum;
	struct netbuf b;
	struct netaddr hostaddr;
	u32 nonce;
	u16 port;
	s32 i;

	if (len < 9 || memcmp(data, "LANA", 4) != 0 || data[4] != 1) {
		return;
	}

	nonce = ((u32)data[5] << 24) | ((u32)data[6] << 16) | ((u32)data[7] << 8) | data[8];

	if (nonce != s_Nonce) {
		return; // an old round's, or nobody's
	}

	memset(&sum, 0, sizeof(sum));
	netBufInitRead(&b, data + 9, len - 9);
	port = netBufReadU16(&b);
	sum.proto = netBufReadU16(&b);
	netBufReadString(&b, sum.build, sizeof(sum.build));
	netBufReadString(&b, sum.content, sizeof(sum.content));
	lanReadSummary(&b, &sum);

	if (!netBufOk(&b) || !sum.id[0] || port == 0) {
		return;
	}

	netTextPrintable(sum.build, 0);
	sum.hostrtt = (s32)(lanNowMs() - s_AskedAt);
	sum.compat = netLobbyCompatOf(&sum);

	// the host's session socket: where the answer came from, its own port
	hostaddr = *from;
	hostaddr.port = port;

	i = lanFoundIndex(sum.id);

	if (i < 0) {
		if (s_NumFound >= NETLOBBY_MAXROOMS) {
			return;
		}

		i = s_NumFound++;
		s_Found[i].addr = hostaddr;
	} else if (s_Found[i].round != s_Round) {
		// heard again this round: the first answer's address stays (loopback
		// and the LAN's both reach a host on this machine)
		s_Found[i].addr = hostaddr;
	}

	s_Found[i].sum = sum;
	s_Found[i].round = s_Round;
}

/**
 * The Briefing Room in LAN or Direct Connect (netlobby.c): the finder up,
 * its rooms asked for now
 */
void netLanFinderStart(s32 mode)
{
	if (mode != NETLOBBY_MODE_LAN && mode != NETLOBBY_MODE_DIRECT) {
		return;
	}

	if (!s_Probe) {
		if (netTransportInit() != 0) {
			return;
		}

		s_Probe = netProbeOpen();

		if (!s_Probe) {
			netTransportShutdown();
			sysLogPrintf(LOG_WARNING, "lan: the finder's sockets did not open");
			return;
		}
	}

	if (s_FinderMode != mode) {
		s_NumFound = 0;
		s_FinderMode = mode;
	}

	if (mode == NETLOBBY_MODE_DIRECT) {
		lanDirectLoad();
	}

	lanAsk();
}

void netLanFinderStop(void)
{
	if (s_Probe) {
		netProbeClose(s_Probe);
		netTransportShutdown();
		s_Probe = NULL;
	}

	s_FinderMode = 0;
	s_NumFound = 0;
	s_DirectWant[0] = '\0';
}

void netLanFind(void)
{
	if (s_FinderMode) {
		netLanFinderStart(s_FinderMode);
	}
}

s32 netLanNumRooms(void)
{
	return s_NumFound;
}

const struct netlobbyroomsum *netLanRoomAt(s32 index)
{
	return index >= 0 && index < s_NumFound ? &s_Found[index].sum : NULL;
}

// A query out with no answer back yet (the list's "Asking..." moment)
s32 netLanFinderBusy(void)
{
	return s_FinderMode && lanNowMs() - s_AskedAt < 1000;
}

/**
 * Connect to Address: what was typed goes to the top of the Direct list
 * (and pd.ini) and is asked at once; netLanDirectWait says how it went.
 * 0, or -1 with why.
 */
s32 netLanDirectAdd(const char *text, char *err, s32 errsize)
{
	struct landirect d;
	s32 i;

	memset(&d, 0, sizeof(d));
	lanCleanName(text, d.text, sizeof(d.text));

	if (lanParseAddress(d.text, d.host, sizeof(d.host), &d.port) != 0) {
		snprintf(err, errsize, "\"%s\" is not an address. Type the host's IP (or name), and its port after a colon or a space if it is not %d.",
				d.text, NET_DEFAULT_PORT);
		return -1;
	}

	lanDirectLoad();

	for (i = 0; i < s_NumDirect; i++) {
		if (strcasecmp(s_Direct[i].host, d.host) == 0 && s_Direct[i].port == d.port) {
			lanDirectDrop(&s_Direct[i]);
			memmove(&s_Direct[i], &s_Direct[i + 1], sizeof(s_Direct[0]) * (s_NumDirect - i - 1));
			s_NumDirect--;
			break;
		}
	}

	if (s_NumDirect == LAN_MAXDIRECT) {
		lanDirectDrop(&s_Direct[--s_NumDirect]);
	}

	memmove(&s_Direct[1], &s_Direct[0], sizeof(s_Direct[0]) * s_NumDirect);
	s_NumDirect++;
	s_Direct[0] = d;
	lanDirectResolve(&s_Direct[0]);
	lanDirectSave();

	snprintf(s_DirectWant, sizeof(s_DirectWant), "%s", d.text);
	s_DirectWantAt = lanNowMs();
	sysLogPrintf(LOG_NOTE, "lan: Direct Connect: asking %s port %u", d.host, d.port);

	netLanFinderStart(NETLOBBY_MODE_DIRECT);

	return 0;
}

/**
 * The typed address's room: its index in the list once it has answered; -1
 * still asking; -2 nothing answered in time or the name did not resolve
 * (why in err); -3 not waiting for one
 */
s32 netLanDirectWait(char *err, s32 errsize)
{
	s32 i;

	if (!s_DirectWant[0] || s_NumDirect == 0 || strcmp(s_Direct[0].text, s_DirectWant) != 0) {
		return -3;
	}

	if (lanDirectState(&s_Direct[0]) == LANRES_FAILED) {
		snprintf(err, errsize, "Could not find the address %s.", s_Direct[0].host);
		s_DirectWant[0] = '\0';
		return -2;
	}

	if (lanDirectState(&s_Direct[0]) == LANRES_OK) {
		for (i = 0; i < s_NumFound; i++) {
			if (memcmp(s_Found[i].addr.ip, s_Direct[0].res->addr.ip, 16) == 0 && s_Found[i].addr.port == s_Direct[0].res->addr.port
					&& s_Found[i].round == s_Round) {
				s_DirectWant[0] = '\0';
				return i;
			}
		}
	}

	if (lanNowMs() - s_DirectWantAt > LAN_DIRECT_WAIT_MS) {
		snprintf(err, errsize, "No room answered at %s port %u. The host makes a room first, and its UDP port %u must reach it (forwarded on its router).",
				s_Direct[0].host, s_Direct[0].port, s_Direct[0].port);
		s_DirectWant[0] = '\0';
		return -2;
	}

	return -1;
}

/**
 * This machine's address for a Direct Connect host to give its friends: its
 * LAN address and the room's port ("" none). The address the internet sees
 * it at is the router's, which only the router (or a site) can tell.
 */
void netLanHostAddress(char *out, s32 size)
{
	struct netaddr me;
	char addr[64];

	out[0] = '\0';

	if (!s_Host.on) {
		return;
	}

	if (netLocalAddrFor("8.8.8.8", &me) == 0) {
		netAddrToString(&me, addr, sizeof(addr));

		if (strchr(addr, ':') && addr[0] != '[') {
			*strchr(addr, ':') = '\0';
			snprintf(out, size, "%s:%u", addr, netSessionLobbyPort());
			return;
		}
	}

	snprintf(out, size, "port %u", netSessionLobbyPort());
}

/*
 * The host's side of the finder: answering a query on the session's socket
 */

s32 netLanRaw(const struct netevent *ev)
{
	u8 out[NETLAN_QUERYLEN - NET_RAWMAGICLEN];
	struct netlobbyroomsum sum;
	struct netbuf b;
	const u64 now = lanNowMs();

	if (ev->len < 9 || memcmp(ev->data, "LANQ", 4) != 0) {
		return 0;
	}

	// a query is ours whatever comes of it: never the rendezvous's
	if (!s_Host.on || ev->data[4] != 1 || ev->len < NETLAN_QUERYLEN - NET_RAWMAGICLEN || !g_NetHostSocket) {
		return 1;
	}

	if (now - s_Host.answerfrom >= 1000) {
		s_Host.answerfrom = now;
		s_Host.answers = 0;
	}

	if (++s_Host.answers > NETLAN_ANSWERS) {
		return 1;
	}

	lanHostSummary(&sum);
	memcpy(out, "LANA", 4);
	out[4] = 1;
	memcpy(out + 5, ev->data + 5, 4); // the nonce
	netBufInitWrite(&b, out + 9, sizeof(out) - 9);
	netBufWriteU16(&b, netSessionLobbyPort());
	netBufWriteU16(&b, NET_PROTOCOL_VERSION);
	netWriteStr(&b, VERSION_HASH, NET_MAXBUILD);
	netWriteStr(&b, sum.content, 16);
	lanWriteSummary(&b, &sum);

	// never longer than the query: a long mod name is cut to fit
	if (!netBufOk(&b)) {
		sum.mod[0] = '\0';
		netBufInitWrite(&b, out + 9, sizeof(out) - 9);
		netBufWriteU16(&b, netSessionLobbyPort());
		netBufWriteU16(&b, NET_PROTOCOL_VERSION);
		netWriteStr(&b, VERSION_HASH, NET_MAXBUILD);
		netWriteStr(&b, sum.content, 16);
		lanWriteSummary(&b, &sum);
	}

	if (netBufOk(&b)) {
		netHostSendRaw(g_NetHostSocket, &ev->from, out, 9 + netBufLen(&b));
	}

	return 1;
}

/*
 * The tick (netlobby.c's, every loop pass while the lobby is in use)
 */

void netLanTick(void)
{
	u8 data[NET_MTU];
	struct netaddr from;
	s32 len;
	const u64 now = lanNowMs();

	// the finder's answers, and its next round
	if (s_Probe) {
		while ((len = netProbeRecv(s_Probe, &from, data, sizeof(data))) >= 0) {
			lanOnAnswer(&from, data, len);
		}

		if (now - s_AskedAt >= LAN_ASK_MS) {
			lanAsk();
		} else if (s_FinderMode == NETLOBBY_MODE_DIRECT) {
			// a name looked up since the last round: asked now, not in 3 s
			s32 i;

			for (i = 0; i < s_NumDirect; i++) {
				if (!s_Direct[i].asked && lanDirectState(&s_Direct[i]) == LANRES_OK) {
					lanAsk();
					break;
				}
			}
		}
	}

	// the host: the countdown's end, and the room to its members
	if (s_Host.on) {
		if (s_Host.status == NETROOM_COUNTDOWN && now >= s_Host.countdownend) {
			s_Host.status = NETROOM_LAUNCHED;
			s_Host.launchat = ++s_Host.launches;
			lanHostChanged();
			sysLogPrintf(LOG_NOTE, "lan: room %s launched", s_Host.id);
		}

		if (s_Host.sent != s_Host.version
				|| (s_Host.status == NETROOM_COUNTDOWN && now - s_Host.sentat >= 500)
				|| (!netSessionMatchLoading() && now - s_Host.sentat >= LAN_ROOM_PING_MS)) {
			if (!netSessionMatchLoading() && now - s_Host.sentat >= LAN_ROOM_PING_MS) {
				s_Host.seq++; // the pings on the host's own roster too
			}

			lanHostSendRoom();
		}
	}

	// a member: its connection ended (refused, kicked, the host closed the
	// room or went): out, with why - kept for the main menu's notice if it
	// ended mid-match, where no room page will say it
	if ((s_Cl.state == LANJOIN_CONNECTING || s_Cl.state == LANJOIN_IN) && netSessionClientGone()) {
		if (netSessionMatchLoading()) {
			s_Cl.inmatch = 1;
			return;
		}

		// this player's own End Game while the room's match runs: still in
		// the room (a lobby room's member keeps its seat in it the same way)
		if (s_Cl.state == LANJOIN_IN && netSessionLastRefuse() == NETREFUSE_LEFT && s_Cl.room.launched) {
			g_NetNoticePending = 0;

			if (lanReconnect(1) == 0) {
				return;
			}
		}

		snprintf(s_Cl.why, sizeof(s_Cl.why), "%s", netSessionNoticeText()[0] ? netSessionNoticeText()
				: s_Cl.state == LANJOIN_CONNECTING ? "Could not reach the room's host." : "The connection to the host ended.");

		if (netSessionClientUnreached() && s_Cl.state == LANJOIN_CONNECTING) {
			snprintf(s_Cl.why, sizeof(s_Cl.why), "No answer from the host at %s.", s_Cl.addr);
		}

		if (!s_Cl.inmatch) {
			g_NetNoticePending = 0;
		}

		sysLogPrintf(LOG_NOTE, "lan: out of the room: %s", s_Cl.why);
		s_Cl.state = LANJOIN_FAILED;
		memset(&s_Cl.room, 0, sizeof(s_Cl.room));
		s_Cl.seq++;
		netSessionLobbyStop();
	}
}
