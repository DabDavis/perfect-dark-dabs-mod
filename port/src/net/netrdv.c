#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "types.h"
#include "system.h"
#include "sha256.h"
#include "net/nettransport.h"
#include "netint.h"
#include "netrdv.h"

/**
 * See netrdv.h for the ladder; tools/pdlobbyd/README.md for every datagram
 * the lobby side of it speaks. The two games' own:
 *
 *   0x20 PUNCH        cookie 8, nonce u32, sent ms u32, flags u8
 *   0x21 PUNCH_REPLY  the same, nonce and sent ms echoed
 *
 * after the six-byte PDLB header, 23 bytes each, so a reply is never larger
 * than what it answers. The cookie is the one the lobby gave both ends in
 * PEER: anything without a cookie this machine holds is dropped unanswered.
 * flags bit 0: from the host; bit 1, in a host's reply: the host had already
 * sprayed this joiner (so the path is a punch, not the host simply being
 * reachable).
 *
 * The host waits DIRECT_WINDOW_MS after PEER before it sprays: a joiner that
 * gets through in that window reached a host that needed no help. Then it
 * sprays every endpoint PEER named for SPRAY_MS. A joiner that has heard
 * nothing by PUNCH_DEADLINE_MS asks the lobby for a relay.
 */

#define PDLB_HEADER         6
#define T_REGISTER          0x01
#define T_REGISTERED        0x02
#define T_PEER              0x03
#define T_ERROR             0x04
#define T_ECHO              0x05
#define T_ECHO_REPLY        0x06
#define T_PROBE             0x07
#define T_PROBE_REPLY       0x08
#define T_RELAY_REQUEST     0x10
#define T_RELAY_OFFER       0x11
#define T_RELAY_BIND        0x12
#define T_RELAY_BOUND       0x13
#define T_PUNCH             0x20
#define T_PUNCH_REPLY       0x21

#define PUNCH_LEN           (PDLB_HEADER + 8 + 4 + 4 + 1)
#define PUNCHF_HOST         0x01
#define PUNCHF_SPRAYED      0x02

#define REG_FAST_MS         1000
#define REG_SLOW_MS         12000
#define ECHO_MS             5000
#define DIRECT_WINDOW_MS    1000
#define DIRECT_WINDOW_PUBLIC_MS 2500 // a host the lobby sees at its own address
#define SPRAY_MS            3000
#define PUNCH_EVERY_MS      100
#define PUNCH_DEADLINE_MS   4500   // after PEER: nothing answered, ask for a relay
#define RELAY_REQ_EVERY_MS  1000
#define RELAY_BIND_EVERY_MS 250
#define LADDER_DEADLINE_MS  16000  // after PEER: nothing at all, give up
#define LADDER_RETRY_MS     20000  // FAILED: the ladder again this long after
#define RELAY_REFUSED_MS    5000   // a refused relay request: ask again this much later
#define WAIT_NOREG_MS       5000   // in a room with no REGISTERED back: rendezvous unreachable
#define WAIT_NOPEER_MS      10000  // registered, but no PEER for the host this long
#define PING_EVERY_MS       2000
#define PATH_LOST_MS        20000  // no ping answered this long: the ladder again
#define HOST_RELAY_BIND_MS  10000  // a host binds an offered relay this long

#define MAXCANDS    10
#define MAXPEERS    16
#define MAXRELAYS   16

struct rdvcand {
	struct netaddr a;
	s32 lan;
};

// The host's view of a joiner it heard of in PEER
struct rdvpeer {
	s32 used;
	u8 cookie[8];
	char name[33];
	s32 role;
	struct rdvcand eps[MAXCANDS];
	s32 neps;
	u64 peerat;      // when its PEER came
	u64 sprayfrom;
	u64 sprayuntil;
	u64 lastspray;
	s32 sprayed;
	u64 heard;       // the last punch from it
	u64 lastuse;
};

// A relay the lobby offered the host for one of its joiners
struct rdvrelay {
	s32 used;
	u8 rid[8];
	u16 port;
	char name[16];
	u8 proof[8];
	s32 proofhave;
	s32 ok;
	u64 lastbind;
	u64 until;
};

static struct netaddr s_Lobby;
static s32 s_LobbyKnown = 0;
static struct nethost *s_Sock = NULL; // the joiner's (and lister's) socket
static s32 s_SockTransport = 0;

static s32 s_InRoom = 0;
static s32 s_Host = 0;
static s32 s_NoNat = 0;  // the lobby saw this socket at its own address and port: no NAT here
static u32 s_RoomNum = 0;
static u8 s_UdpId[8];
static u8 s_UdpKey[32];
static u32 s_Seq = 0;
static s32 s_Registered = 0;
static s32 s_RegErrors = 0;
static u64 s_LastReg = 0;
static struct netaddr s_Public;

static u64 s_LastEcho = 0;
static u8 s_EchoNonce[8];
static u64 s_EchoSent = 0;
static s32 s_EchoRtt = -1;

// the joiner's ladder
static s32 s_Ladder = NETRDV_LADDER_IDLE;
static s32 s_Path = NETRDV_PATH_NONE;
static u8 s_Cookie[8];
static s32 s_HavePeer = 0;
static char s_HostName[33];
static struct rdvcand s_Cands[MAXCANDS];
static s32 s_NumCands = 0;
static u64 s_PeerAt = 0;
static u64 s_EnteredAt = 0;
static u64 s_FailedAt = 0;
static u64 s_RelayRefusedAt = 0;
static u64 s_LastPunch = 0;
static struct netaddr s_Chosen;
static s32 s_Ping = -1;
static u64 s_LastPing = 0;
static u64 s_LastPong = 0;
static u32 s_PunchNonce = 0;

// the joiner's relay
static s32 s_RelayHave = 0;
static u8 s_RelayId[8];
static struct netaddr s_RelayAddr;
static u8 s_RelayProof[8];
static s32 s_RelayProofHave = 0;
static s32 s_RelayFlags = 0;
static u64 s_RelayLastReq = 0;
static u64 s_RelayLastBind = 0;

// the host's
static struct rdvpeer s_Peers[MAXPEERS];
static struct rdvrelay s_Relays[MAXRELAYS];

static u64 rdvNow(void)
{
	return sysGetMicroseconds() / 1000;
}

// Echo nonces: not libc's rand, whose sequence the game's own code may share
static u32 rdvRandom(void)
{
	static u32 x = 0;

	if (x == 0) {
		x = (u32)sysGetMicroseconds() | 1;
	}

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;

	return x;
}

static struct nethost *rdvRoomSocket(void)
{
	return s_Host ? g_NetHostSocket : s_Sock;
}

static void put32(u8 *p, u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static u32 get32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static s32 hexDecode(const char *hex, u8 *out, s32 len)
{
	s32 i;

	if (!hex || (s32)strlen(hex) != len * 2) {
		return -1;
	}

	for (i = 0; i < len * 2; i++) {
		const char c = hex[i];
		const s32 v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

		if (v < 0) {
			return -1;
		}

		if (i & 1) {
			out[i / 2] |= v;
		} else {
			out[i / 2] = v << 4;
		}
	}

	return 0;
}

static const u8 s_V4Prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

static s32 addrIsV4(const struct netaddr *a)
{
	return memcmp(a->ip, s_V4Prefix, 12) == 0;
}

static s32 addrEq(const struct netaddr *a, const struct netaddr *b)
{
	return a->port == b->port && memcmp(a->ip, b->ip, 16) == 0;
}

static s32 addrSameIp(const struct netaddr *a, const struct netaddr *b)
{
	return memcmp(a->ip, b->ip, 16) == 0;
}

/**
 * The lobby's LAN rule (README "LAN endpoints"): private-range only, so
 * neither end can be aimed at a third party's public address through a
 * peer's claims
 */
static s32 addrIsLan(const struct netaddr *a)
{
	if (addrIsV4(a)) {
		const u8 *ip = a->ip + 12;

		return ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xf0) == 16) || (ip[0] == 192 && ip[1] == 168)
			|| (ip[0] == 169 && ip[1] == 254);
	}

	return (a->ip[0] & 0xfe) == 0xfc;
}

static const char *addrStr(const struct netaddr *a)
{
	static char buf[4][64];
	static s32 next = 0;
	char *out = buf[next++ & 3];

	netAddrToString(a, out, 64);

	return out;
}

// family, address, port; the bytes written, or 0 if it does not fit
static s32 putEndpoint(u8 *p, s32 room, const struct netaddr *a)
{
	if (addrIsV4(a)) {
		if (room < 7) {
			return 0;
		}

		p[0] = 4;
		memcpy(p + 1, a->ip + 12, 4);
		p[5] = a->port >> 8;
		p[6] = a->port & 0xff;

		return 7;
	}

	if (room < 19) {
		return 0;
	}

	p[0] = 6;
	memcpy(p + 1, a->ip, 16);
	p[17] = a->port >> 8;
	p[18] = a->port & 0xff;

	return 19;
}

// The bytes read, or 0 if it does not parse
static s32 getEndpoint(const u8 *p, s32 len, struct netaddr *a)
{
	memset(a, 0, sizeof(*a));

	if (len >= 7 && p[0] == 4) {
		memcpy(a->ip, s_V4Prefix, 12);
		memcpy(a->ip + 12, p + 1, 4);
		a->port = (p[5] << 8) | p[6];

		return a->port ? 7 : 0;
	}

	if (len >= 19 && p[0] == 6) {
		memcpy(a->ip, p + 1, 16);
		a->port = (p[17] << 8) | p[18];

		return a->port ? 19 : 0;
	}

	return 0;
}

static void putHeader(u8 *p, u8 type)
{
	memcpy(p, NET_LOBBYMAGIC, NET_LOBBYMAGICLEN);
	p[4] = 1;
	p[5] = type;
}

static void rdvSend(struct nethost *h, const struct netaddr *to, const u8 *data, s32 len)
{
	if (h && to && to->port) {
		netHostSendDatagram(h, to, data, len);
	}
}

// The first 16 bytes of HMAC-SHA256(udp_key, everything before the mac)
static void rdvSign(u8 *msg, s32 len)
{
	u8 mac[32];

	sha256Hmac(s_UdpKey, sizeof(s_UdpKey), msg, len, mac);
	memcpy(msg + len, mac, 16);
}

/*
 * To the lobby
 */

static void rdvRegister(void)
{
	struct nethost *h = rdvRoomSocket();
	struct netaddr lan;
	char lobbyip[64];
	char *colon;
	u8 msg[96];
	s32 len = 0;
	s32 n;

	if (!h || !s_LobbyKnown) {
		return;
	}

	putHeader(msg, T_REGISTER);
	len = PDLB_HEADER;
	put32(msg + len, s_RoomNum);
	len += 4;
	memcpy(msg + len, s_UdpId, 8);
	len += 8;
	put32(msg + len, ++s_Seq);
	len += 4;
	msg[len++] = 0;

	// the LAN address this socket's traffic to the lobby leaves from
	netAddrToString(&s_Lobby, lobbyip, sizeof(lobbyip));
	colon = strrchr(lobbyip, ':');

	if (colon && lobbyip[0] != '[') {
		*colon = '\0';

		if (netLocalAddrFor(lobbyip, &lan) == 0 && addrIsLan(&lan)) {
			lan.port = netHostPort(h);
			n = putEndpoint(msg + len, sizeof(msg) - len - 16, &lan);

			if (n) {
				len += n;
				msg[PDLB_HEADER + 16] = 1;
			}
		}
	}

	rdvSign(msg, len);
	rdvSend(h, &s_Lobby, msg, len + 16);
	s_LastReg = rdvNow();
}

static void rdvEcho(void)
{
	u8 msg[40];
	s32 i;

	if (!s_Sock || !s_LobbyKnown) {
		return;
	}

	memset(msg, 0, sizeof(msg));
	putHeader(msg, T_ECHO);

	for (i = 0; i < 8; i++) {
		s_EchoNonce[i] = rdvRandom() & 0xff;
	}

	memcpy(msg + PDLB_HEADER, s_EchoNonce, 8);
	rdvSend(s_Sock, &s_Lobby, msg, sizeof(msg));
	s_EchoSent = s_LastEcho = rdvNow();
}

static void rdvRelayRequest(void)
{
	u8 msg[PDLB_HEADER + 4 + 8 + 4 + 16];

	putHeader(msg, T_RELAY_REQUEST);
	put32(msg + 6, s_RoomNum);
	memcpy(msg + 10, s_UdpId, 8);
	put32(msg + 18, ++s_Seq);
	rdvSign(msg, 22);
	rdvSend(s_Sock, &s_Lobby, msg, sizeof(msg));
	s_RelayLastReq = rdvNow();
}

static void rdvRelayBind(struct nethost *h, const u8 *rid, u16 port, const u8 *proof)
{
	u8 msg[PDLB_HEADER + 4 + 8 + 8 + 8 + 4 + 16];
	struct netaddr to = s_Lobby;

	to.port = port;
	putHeader(msg, T_RELAY_BIND);
	put32(msg + 6, s_RoomNum);
	memcpy(msg + 10, s_UdpId, 8);
	memcpy(msg + 18, rid, 8);

	if (proof) {
		memcpy(msg + 26, proof, 8);
	} else {
		memset(msg + 26, 0, 8);
	}

	put32(msg + 34, ++s_Seq);
	rdvSign(msg, 38);
	rdvSend(h, &to, msg, sizeof(msg));
}

/*
 * Between the games
 */

static void rdvPunch(struct nethost *h, const struct netaddr *to, const u8 *cookie, u8 flags)
{
	u8 msg[PUNCH_LEN];

	putHeader(msg, T_PUNCH);
	memcpy(msg + 6, cookie, 8);
	put32(msg + 14, ++s_PunchNonce);
	put32(msg + 18, (u32)rdvNow());
	msg[22] = flags;
	rdvSend(h, to, msg, sizeof(msg));
}

static void rdvPunchReply(struct nethost *h, const struct netaddr *to, const u8 *punch, u8 flags)
{
	u8 msg[PUNCH_LEN];

	memcpy(msg, punch, PUNCH_LEN);
	msg[5] = T_PUNCH_REPLY;
	msg[22] = flags;
	rdvSend(h, to, msg, sizeof(msg));
}

static void rdvAddCand(const struct netaddr *a, s32 lan)
{
	s32 i;

	for (i = 0; i < s_NumCands; i++) {
		if (addrEq(&s_Cands[i].a, a)) {
			return;
		}
	}

	if (s_NumCands < MAXCANDS) {
		s_Cands[s_NumCands].a = *a;
		s_Cands[s_NumCands].lan = lan;
		s_NumCands++;
	}
}

static s32 rdvCandIsLan(const struct netaddr *a)
{
	s32 i;

	for (i = 0; i < s_NumCands; i++) {
		if (addrEq(&s_Cands[i].a, a)) {
			return s_Cands[i].lan;
		}
	}

	return 0;
}

static void rdvChoose(const struct netaddr *a, s32 path, s32 rtt)
{
	s_Chosen = *a;
	s_Path = path;
	s_Ladder = NETRDV_LADDER_DONE;
	s_Ping = rtt;
	s_LastPong = s_LastPing = rdvNow();
	sysLogPrintf(LOG_NOTE, "rdv: path to the host: %s via %s (%d ms)", netRdvPathName(path), addrStr(a), rtt);
}

static void rdvStartLadder(void)
{
	s_Ladder = NETRDV_LADDER_RUNNING;
	s_Path = NETRDV_PATH_NONE;
	s_Ping = -1;
	s_PeerAt = rdvNow();
	s_LastPunch = 0;
	s_RelayHave = 0;
	s_RelayProofHave = 0;
	s_RelayFlags = 0;
	s_RelayLastReq = 0;
	s_RelayLastBind = 0;
	s_RelayRefusedAt = 0;
}

static void rdvFail(const char *why)
{
	s_Ladder = NETRDV_LADDER_FAILED;
	s_Path = NETRDV_PATH_NONE;
	s_Ping = -1;
	s_FailedAt = rdvNow();
	sysLogPrintf(LOG_WARNING, "rdv: %s", why);
}

/*
 * What comes in
 */

static s32 fromLobby(const struct netaddr *from)
{
	return s_LobbyKnown && addrEq(from, &s_Lobby);
}

static void rdvOnPeer(struct nethost *h, const u8 *d, s32 len)
{
	u8 cookie[8];
	char name[33];
	struct rdvcand eps[MAXCANDS];
	s32 neps = 0;
	s32 role;
	s32 nlen;
	s32 count;
	s32 off;
	s32 i;

	if (len < 21 || get32(d + 6) != s_RoomNum) {
		return;
	}

	memset(eps, 0, sizeof(eps)); // compared whole below, padding included
	memcpy(cookie, d + 10, 8);
	role = d[18];
	nlen = d[19];

	if (nlen > 32 || 20 + nlen + 1 > len) {
		return;
	}

	for (i = 0; i < nlen; i++) {
		const char c = d[20 + i];
		name[i] = c >= 0x21 && c < 0x7f ? c : '?';
	}

	name[nlen] = '\0';
	off = 20 + nlen;
	count = d[off++];

	if (count < 1 || count > 5) {
		return;
	}

	for (i = 0; i < count; i++) {
		struct netaddr a;
		const s32 n = getEndpoint(d + off, len - off, &a);

		if (!n) {
			return;
		}

		off += n;

		// the first is the peer's public address as the lobby saw it; the
		// rest LAN candidates, kept only if private-range (the lobby's own rule)
		if (i == 0 || addrIsLan(&a)) {
			eps[neps].a = a;
			eps[neps].lan = i > 0;
			neps++;
		}
	}

	if (off != len) {
		return;
	}

	if (s_Host) {
		struct rdvpeer *p = NULL;
		struct rdvpeer *oldest = &s_Peers[0];
		s32 changed;

		if (role == 0) {
			return;
		}

		for (i = 0; i < MAXPEERS; i++) {
			if (s_Peers[i].used && memcmp(s_Peers[i].cookie, cookie, 8) == 0) {
				p = &s_Peers[i];
				break;
			}

			if (!s_Peers[i].used || s_Peers[i].lastuse < oldest->lastuse) {
				oldest = &s_Peers[i];
			}
		}

		if (!p) {
			p = oldest;
			memset(p, 0, sizeof(*p));
			p->used = 1;
			memcpy(p->cookie, cookie, 8);
			changed = 1;
		} else {
			changed = p->neps != neps || memcmp(p->eps, eps, sizeof(eps[0]) * neps) != 0;
		}

		snprintf(p->name, sizeof(p->name), "%s", name);
		p->role = role;
		memcpy(p->eps, eps, sizeof(eps[0]) * neps);
		p->neps = neps;
		p->lastuse = rdvNow();

		// a new joiner (or one whose addresses moved) that has not reached
		// us yet: the direct window, then the spray
		if (changed && !p->heard) {
			// a host with no NAT of its own is reachable as it is: its spray
			// only opens a stateful firewall, so it waits longer for a joiner
			// to come by itself (a busy joiner can be a second late)
			p->peerat = rdvNow();
			p->sprayfrom = p->peerat + (s_NoNat ? DIRECT_WINDOW_PUBLIC_MS : DIRECT_WINDOW_MS);
			p->sprayuntil = p->sprayfrom + SPRAY_MS;
			sysLogPrintf(LOG_NOTE, "rdv: PEER %s, %d endpoint%s (public %s)", name, neps, neps == 1 ? "" : "s", addrStr(&eps[0].a));
		}

		return;
	}

	if (role != 0) {
		return;
	}

	snprintf(s_HostName, sizeof(s_HostName), "%s", name);

	if (!s_HavePeer || memcmp(s_Cookie, cookie, 8) != 0) {
		memcpy(s_Cookie, cookie, 8);
		s_HavePeer = 1;
		s_NumCands = 0;

		// LAN first: a same-LAN pair should meet there
		for (i = 1; i < neps; i++) {
			rdvAddCand(&eps[i].a, 1);
		}

		rdvAddCand(&eps[0].a, 0);
		sysLogPrintf(LOG_NOTE, "rdv: PEER host %s, %d endpoint%s (public %s); punching", name, neps, neps == 1 ? "" : "s", addrStr(&eps[0].a));
		rdvStartLadder();
	} else {
		for (i = 1; i < neps; i++) {
			rdvAddCand(&eps[i].a, 1);
		}

		rdvAddCand(&eps[0].a, 0);
	}
}

static void rdvOnPunch(struct nethost *h, const struct netaddr *from, const u8 *d, s32 len)
{
	s32 i;

	if (len != PUNCH_LEN) {
		return;
	}

	if (s_Host) {
		for (i = 0; i < MAXPEERS; i++) {
			struct rdvpeer *p = &s_Peers[i];

			if (p->used && memcmp(p->cookie, d + 6, 8) == 0) {
				if (!p->heard) {
					sysLogPrintf(LOG_NOTE, "rdv: %s reached us from %s%s, %u ms after its PEER", p->name, addrStr(from),
							p->sprayed ? " (after our spray)" : "", (u32)(rdvNow() - p->peerat));
				}

				p->heard = p->lastuse = rdvNow();
				// SPRAYED tells the joiner its path is a punch: never from a
				// host with no NAT, which a joiner reaches without one
				rdvPunchReply(h, from, d, PUNCHF_HOST | (p->sprayed && !s_NoNat ? PUNCHF_SPRAYED : 0));
				return;
			}
		}

		return;
	}

	if (!s_HavePeer || memcmp(s_Cookie, d + 6, 8) != 0) {
		return;
	}

	// the host's spray got through: answer, and punch back at where it came from
	rdvPunchReply(h, from, d, 0);

	if (s_Ladder == NETRDV_LADDER_RUNNING && !s_RelayHave) {
		rdvAddCand(from, 0);
		rdvPunch(h, from, s_Cookie, 0);
	}
}

static void rdvOnPunchReply(struct nethost *h, const struct netaddr *from, const u8 *d, s32 len)
{
	s32 rtt;
	s32 i;

	if (len != PUNCH_LEN) {
		return;
	}

	if (s_Host) {
		for (i = 0; i < MAXPEERS; i++) {
			if (s_Peers[i].used && memcmp(s_Peers[i].cookie, d + 6, 8) == 0) {
				s_Peers[i].heard = s_Peers[i].lastuse = rdvNow();
			}
		}

		return;
	}

	if (!s_HavePeer || memcmp(s_Cookie, d + 6, 8) != 0) {
		return;
	}

	rtt = (s32)((u32)rdvNow() - get32(d + 18));

	if (rtt < 0 || rtt > 10000) {
		return;
	}

	if (s_Ladder == NETRDV_LADDER_RUNNING) {
		// the first answer is the path (a late direct answer while the relay
		// is being set up still wins: it is the shorter way)
		if (s_RelayHave && addrEq(from, &s_RelayAddr)) {
			rdvChoose(from, NETRDV_PATH_RELAY, rtt);
		} else {
			rdvChoose(from, rdvCandIsLan(from) ? NETRDV_PATH_LAN : (d[22] & PUNCHF_SPRAYED) ? NETRDV_PATH_PUNCH : NETRDV_PATH_DIRECT, rtt);
		}
	} else if (s_Ladder == NETRDV_LADDER_DONE && addrEq(from, &s_Chosen)) {
		s_Ping = s_Ping < 0 ? rtt : (s_Ping * 7 + rtt * 3 + 5) / 10;
		s_LastPong = rdvNow();
	}
}

static void rdvOnOffer(struct nethost *h, const u8 *d, s32 len)
{
	u8 rid[8];
	u16 port;
	s32 nlen;
	s32 i;

	if (len < 21 || get32(d + 6) != s_RoomNum) {
		return;
	}

	memcpy(rid, d + 10, 8);
	port = (d[18] << 8) | d[19];
	nlen = d[20];

	if (port == 0 || nlen > 15 || 21 + nlen != len) {
		return;
	}

	if (s_Host) {
		struct rdvrelay *r = NULL;
		struct rdvrelay *spare = NULL;

		for (i = 0; i < MAXRELAYS; i++) {
			if (s_Relays[i].used && memcmp(s_Relays[i].rid, rid, 8) == 0) {
				r = &s_Relays[i];
				break;
			}

			if (!spare && (!s_Relays[i].used || s_Relays[i].until < rdvNow())) {
				spare = &s_Relays[i];
			}
		}

		if (!r) {
			if (!spare) {
				return;
			}

			r = spare;
			memset(r, 0, sizeof(*r));
			r->used = 1;
			memcpy(r->rid, rid, 8);

			for (i = 0; i < nlen; i++) {
				const char c = d[21 + i];
				r->name[i] = c >= 0x21 && c < 0x7f ? c : '?';
			}

			r->name[nlen] = '\0';
			sysLogPrintf(LOG_NOTE, "rdv: relay offered for %s on the lobby's port %u; binding", r->name, port);
		}

		r->port = port;
		r->until = rdvNow() + HOST_RELAY_BIND_MS;

		if (!r->ok) {
			r->lastbind = rdvNow();
			rdvRelayBind(h, r->rid, r->port, r->proofhave ? r->proof : NULL);
		}

		return;
	}

	if (s_Ladder != NETRDV_LADDER_RUNNING) {
		return;
	}

	if (!s_RelayHave || memcmp(s_RelayId, rid, 8) != 0) {
		memcpy(s_RelayId, rid, 8);
		s_RelayAddr = s_Lobby;
		s_RelayAddr.port = port;
		s_RelayHave = 1;
		s_RelayProofHave = 0;
		s_RelayFlags = 0;
		sysLogPrintf(LOG_NOTE, "rdv: relay offered on the lobby's port %u; binding", port);
		s_RelayLastBind = rdvNow();
		rdvRelayBind(h, s_RelayId, port, NULL);
	}
}

static void rdvOnBound(struct nethost *h, const struct netaddr *from, const u8 *d, s32 len)
{
	s32 i;

	if (len != PDLB_HEADER + 4 + 8 + 1 + 8 || get32(d + 6) != s_RoomNum || !s_LobbyKnown || !addrSameIp(from, &s_Lobby)) {
		return;
	}

	if (s_Host) {
		for (i = 0; i < MAXRELAYS; i++) {
			struct rdvrelay *r = &s_Relays[i];

			if (r->used && memcmp(r->rid, d + 10, 8) == 0 && from->port == r->port) {
				memcpy(r->proof, d + 19, 8);
				r->proofhave = 1;

				if ((d[18] & 1) && !r->ok) {
					r->ok = 1;
					sysLogPrintf(LOG_NOTE, "rdv: relay for %s bound on port %u", r->name, r->port);
				} else if (!(d[18] & 1)) {
					// the proof straight back: this address is ours
					r->lastbind = rdvNow();
					rdvRelayBind(h, r->rid, r->port, r->proof);
				}
			}
		}

		return;
	}

	if (!s_RelayHave || memcmp(s_RelayId, d + 10, 8) != 0 || from->port != s_RelayAddr.port) {
		return;
	}

	memcpy(s_RelayProof, d + 19, 8);

	if (!s_RelayProofHave || !(d[18] & 2)) {
		s_RelayProofHave = 1;
		s_RelayLastBind = rdvNow();
		rdvRelayBind(h, s_RelayId, s_RelayAddr.port, s_RelayProof);
	}

	if (d[18] != s_RelayFlags) {
		s_RelayFlags = d[18];

		if (s_RelayFlags == 3) {
			sysLogPrintf(LOG_NOTE, "rdv: relay bound at both ends; checking the path through it");
		}
	}
}

void netRdvRaw(struct nethost *h, const struct netevent *ev)
{
	const u8 *d = ev->data;
	const s32 len = ev->len;

	if (!h || len < PDLB_HEADER || memcmp(d, NET_LOBBYMAGIC, NET_LOBBYMAGICLEN) != 0 || d[4] != 1) {
		return;
	}

	// only the room's socket takes the room's datagrams; the lister's
	// socket answers ECHO and nothing else
	if (h != rdvRoomSocket() && d[5] != T_ECHO_REPLY) {
		return;
	}

	switch (d[5]) {
	case T_REGISTERED:
		if (fromLobby(&ev->from) && s_InRoom && len >= 17 && get32(d + 6) == s_RoomNum) {
			struct netaddr seen;

			if (getEndpoint(d + 10, len - 10, &seen) && (!s_Registered || !addrEq(&seen, &s_Public))) {
				sysLogPrintf(LOG_NOTE, "rdv: registered with the lobby as the room's %s; seen as %s", s_Host ? "host" : "member", addrStr(&seen));
				s_Public = seen;

				{
					// the address this socket leaves from toward the lobby,
					// against where the lobby saw it
					struct netaddr me;
					char lobbyip[80];
					char *colon;

					netAddrToString(&s_Lobby, lobbyip, sizeof(lobbyip));
					colon = strrchr(lobbyip, ':');
					s_NoNat = 0;

					if (colon && lobbyip[0] != '[') {
						*colon = '\0';

						if (netLocalAddrFor(lobbyip, &me) == 0) {
							me.port = netHostPort(h);
							s_NoNat = addrEq(&me, &seen);
						}
					}

					if (s_NoNat) {
						sysLogPrintf(LOG_NOTE, "rdv: no NAT here (the lobby sees this socket at its own address)");
					}
				}
			}

			s_Registered = 1;
		}
		break;
	case T_PEER:
		if (fromLobby(&ev->from) && s_InRoom) {
			rdvOnPeer(h, d, len);
		}
		break;
	case T_ERROR:
		if (fromLobby(&ev->from) && s_InRoom && len == 11 && get32(d + 7) == s_RoomNum) {
			if (s_RegErrors++ < 5) {
				sysLogPrintf(LOG_WARNING, "rdv: the lobby refused a datagram (code %d)", d[6]);
			}

			// the relay is full: ask again later, not every second
			if (d[6] == 3 && !s_Host && s_Ladder == NETRDV_LADDER_RUNNING && !s_RelayHave) {
				s_RelayRefusedAt = rdvNow();
			}
		}
		break;
	case T_ECHO_REPLY:
		if (fromLobby(&ev->from) && len >= 14 && memcmp(d + 6, s_EchoNonce, 8) == 0 && s_EchoSent) {
			const s32 rtt = (s32)(rdvNow() - s_EchoSent);

			s_EchoRtt = s_EchoRtt < 0 ? rtt : (s_EchoRtt * 7 + rtt * 3 + 5) / 10;
			s_EchoSent = 0;
		}
		break;
	case T_PROBE:
		if (fromLobby(&ev->from) && s_InRoom && s_Host && len == 18 && get32(d + 6) == s_RoomNum) {
			u8 msg[18];

			memcpy(msg, d, 18);
			msg[5] = T_PROBE_REPLY;
			rdvSend(h, &s_Lobby, msg, 18);
		}
		break;
	case T_RELAY_OFFER:
		if (fromLobby(&ev->from) && s_InRoom) {
			rdvOnOffer(h, d, len);
		}
		break;
	case T_RELAY_BOUND:
		if (s_InRoom) {
			rdvOnBound(h, &ev->from, d, len);
		}
		break;
	case T_PUNCH:
		if (s_InRoom) {
			rdvOnPunch(h, &ev->from, d, len);
		}
		break;
	case T_PUNCH_REPLY:
		if (s_InRoom) {
			rdvOnPunchReply(h, &ev->from, d, len);
		}
		break;
	}
}

/*
 * The calls
 */

void netRdvSetLobby(const struct netaddr *udp)
{
	if (!udp || !udp->port) {
		return;
	}

	if (!s_LobbyKnown || !addrEq(&s_Lobby, udp)) {
		s_Lobby = *udp;
		s_LobbyKnown = 1;
		sysLogPrintf(LOG_NOTE, "rdv: the lobby's rendezvous is %s", addrStr(udp));
	}
}

s32 netRdvHasLobby(void)
{
	return s_LobbyKnown;
}

s32 netRdvOpen(void)
{
	if (s_Sock) {
		return 0;
	}

	if (!s_SockTransport) {
		if (netTransportInit() != 0) {
			return -1;
		}

		s_SockTransport = 1;
	}

	// two peers: a connect can start while the last match's peer is still
	// being let go
	s_Sock = netHostCreate(NULL, 0, 2);

	if (!s_Sock) {
		sysLogPrintf(LOG_ERROR, "rdv: could not open a UDP socket for the lobby");
		return -1;
	}

	// it only dials out (to the host at launch); its public endpoint goes
	// to every host the player joins, and a stranger connecting in could
	// otherwise hold both peer slots and block every later launch
	netHostSetDialOnly(s_Sock, 1);
	sysLogPrintf(LOG_NOTE, "rdv: lobby socket on UDP port %u", netHostPort(s_Sock));

	return 0;
}

struct nethost *netRdvSocket(void)
{
	return s_Sock;
}

void netRdvEnter(s32 host, const char *roomid, const char *udpid, const char *udpkey)
{
	u8 room[4];

	netRdvLeave();

	if (hexDecode(roomid, room, 4) != 0 || hexDecode(udpid, s_UdpId, 8) != 0 || hexDecode(udpkey, s_UdpKey, 32) != 0) {
		sysLogPrintf(LOG_WARNING, "rdv: the room's UDP credentials did not parse; no rendezvous");
		return;
	}

	if (!host && netRdvOpen() != 0) {
		return;
	}

	s_RoomNum = get32(room);
	s_Host = host;
	s_InRoom = 1;
	s_Seq = 0;
	s_Registered = 0;
	s_RegErrors = 0;
	s_LastReg = 0;
	s_Ladder = host ? NETRDV_LADDER_IDLE : NETRDV_LADDER_WAITING;
	s_EnteredAt = rdvNow();
	s_FailedAt = 0;
	s_Path = NETRDV_PATH_NONE;
	s_Ping = -1;
	s_HavePeer = 0;
	s_NumCands = 0;
	s_RelayHave = 0;
	memset(s_Peers, 0, sizeof(s_Peers));
	memset(s_Relays, 0, sizeof(s_Relays));
}

void netRdvLeave(void)
{
	s_InRoom = 0;
	s_Host = 0;
	s_NoNat = 0;
	s_Registered = 0;
	s_Ladder = NETRDV_LADDER_IDLE;
	s_Path = NETRDV_PATH_NONE;
	s_Ping = -1;
	s_HavePeer = 0;
	s_RelayHave = 0;
	memset(s_UdpKey, 0, sizeof(s_UdpKey));
	memset(s_Peers, 0, sizeof(s_Peers));
	memset(s_Relays, 0, sizeof(s_Relays));
}

static void rdvHostTick(struct nethost *h, u64 now)
{
	s32 i;
	s32 j;

	for (i = 0; i < MAXPEERS; i++) {
		struct rdvpeer *p = &s_Peers[i];

		if (!p->used || p->heard || now < p->sprayfrom || now >= p->sprayuntil || now - p->lastspray < PUNCH_EVERY_MS) {
			continue;
		}

		if (!p->sprayed) {
			sysLogPrintf(LOG_NOTE, "rdv: %s has not reached us; spraying its %d endpoint%s (%u ms after its PEER)", p->name, p->neps, p->neps == 1 ? "" : "s",
					(u32)(now - p->peerat));
		}

		p->sprayed = 1;
		p->lastspray = now;

		for (j = 0; j < p->neps; j++) {
			// (the joiner reads SPRAYED from the host's reply alone, never a
			// spray; flagged as the reply is, all the same)
			rdvPunch(h, &p->eps[j].a, p->cookie, PUNCHF_HOST | (s_NoNat ? 0 : PUNCHF_SPRAYED));
		}
	}

	for (i = 0; i < MAXRELAYS; i++) {
		struct rdvrelay *r = &s_Relays[i];

		if (r->used && !r->ok && now < r->until && now - r->lastbind >= RELAY_BIND_EVERY_MS) {
			r->lastbind = now;
			rdvRelayBind(h, r->rid, r->port, r->proofhave ? r->proof : NULL);
		}
	}
}

static void rdvJoinerTick(struct nethost *h, u64 now)
{
	s32 i;

	if (s_Ladder == NETRDV_LADDER_RUNNING) {
		const u64 since = now - s_PeerAt;

		if (since >= LADDER_DEADLINE_MS) {
			rdvFail(s_RelayRefusedAt ? "no path to the host answered and the relay was refused; trying again later"
					: "no path to the host answered (direct, punch or relay); trying again later");
			return;
		}

		if (since < PUNCH_DEADLINE_MS) {
			if (now - s_LastPunch >= PUNCH_EVERY_MS) {
				s_LastPunch = now;

				for (i = 0; i < s_NumCands; i++) {
					rdvPunch(h, &s_Cands[i].a, s_Cookie, 0);
				}
			}

			return;
		}

		if (!s_RelayHave) {
			if (now - s_RelayLastReq >= RELAY_REQ_EVERY_MS
					&& (!s_RelayRefusedAt || now - s_RelayRefusedAt >= RELAY_REFUSED_MS)) {
				if (s_RelayLastReq == 0) {
					sysLogPrintf(LOG_NOTE, "rdv: no direct or punched path in %d ms; asking the lobby for a relay", PUNCH_DEADLINE_MS);
				}

				rdvRelayRequest();
			}

			return;
		}

		// bound at our end; the host's binding comes with the offer the
		// lobby re-sends it on every request
		if (!(s_RelayFlags & 2) || !(s_RelayFlags & 1)) {
			if (now - s_RelayLastBind >= RELAY_BIND_EVERY_MS) {
				s_RelayLastBind = now;
				rdvRelayBind(h, s_RelayId, s_RelayAddr.port, s_RelayProofHave ? s_RelayProof : NULL);
			}

			if (!(s_RelayFlags & 1) && now - s_RelayLastReq >= RELAY_REQ_EVERY_MS) {
				rdvRelayRequest();
			}

			return;
		}

		if (now - s_LastPunch >= PUNCH_EVERY_MS) {
			s_LastPunch = now;
			rdvPunch(h, &s_RelayAddr, s_Cookie, 0);
		}

		return;
	}

	// failed: the whole ladder again now and then while in the room (a
	// relay may have freed up, the host may have been busy loading)
	if (s_Ladder == NETRDV_LADDER_FAILED) {
		if (s_HavePeer && now - s_FailedAt >= LADDER_RETRY_MS) {
			sysLogPrintf(LOG_NOTE, "rdv: trying for a path to the host again");
			rdvStartLadder();
		}

		return;
	}

	if (s_Ladder == NETRDV_LADDER_DONE) {
		if (now - s_LastPing >= PING_EVERY_MS) {
			s_LastPing = now;
			rdvPunch(h, &s_Chosen, s_Cookie, 0);
		}

		if (now - s_LastPong >= PATH_LOST_MS) {
			sysLogPrintf(LOG_WARNING, "rdv: the %s path to the host stopped answering; trying again", netRdvPathName(s_Path));
			rdvStartLadder();
		}
	}
}

void netRdvTick(void)
{
	struct netevent ev;
	struct nethost *h;
	const u64 now = rdvNow();

	// the lobby socket, while the session is not using it (when it is, the
	// session's service hands its lobby datagrams over through net.c)
	if (s_Sock && g_NetHostSocket != s_Sock) {
		while (netHostService(s_Sock, &ev, 0) > 0) {
			if (ev.type == NETEVENT_RAW) {
				netRdvRaw(s_Sock, &ev);
			} else if (ev.type == NETEVENT_CONNECT || ev.type == NETEVENT_RECEIVE) {
				// no session on it: no peer belongs here
				netHostDisconnectNow(s_Sock, ev.peer, 0);
			}
		}
	}

	if (s_Sock && s_LobbyKnown && now - s_LastEcho >= ECHO_MS) {
		rdvEcho();
	}

	// a joiner waiting for the host's PEER that will not come (the
	// rendezvous unknown or unreachable, the host not registering): FAILED,
	// so a launch does not hold out for it. A PEER later still starts it.
	if (s_InRoom && !s_Host && s_Ladder == NETRDV_LADDER_WAITING) {
		if (!s_LobbyKnown || !s_Registered ? now - s_EnteredAt >= WAIT_NOREG_MS : now - s_EnteredAt >= WAIT_NOPEER_MS) {
			rdvFail(!s_LobbyKnown ? "no rendezvous address for the lobby; no path from it"
					: !s_Registered ? "the lobby's rendezvous did not answer; no path from it"
					: "no word of the host from the rendezvous; no path from it");
		}
	}

	if (!s_InRoom || !s_LobbyKnown) {
		return;
	}

	h = rdvRoomSocket();

	if (!h) {
		return;
	}

	if (now - s_LastReg >= (u64)(s_Registered && (s_Host || s_HavePeer) ? REG_SLOW_MS : REG_FAST_MS)) {
		rdvRegister();
	}

	if (s_Host) {
		rdvHostTick(h, now);
	} else {
		rdvJoinerTick(h, now);
	}
}

void netRdvShutdown(void)
{
	netRdvLeave();

	if (s_Sock) {
		// a session still on it lets go of it first (netlobby.c's shutdown
		// stops the session before this)
		if (g_NetHostSocket != s_Sock) {
			netHostDestroy(s_Sock);
		}

		s_Sock = NULL;
	}

	if (s_SockTransport) {
		s_SockTransport = 0;
		netTransportShutdown();
	}
}

s32 netRdvLadder(void)
{
	return s_Ladder;
}

s32 netRdvPath(void)
{
	return s_Path;
}

s32 netRdvEndpoint(char *addr, s32 size, u16 *port)
{
	char buf[64];
	char *colon;

	if (s_Ladder != NETRDV_LADDER_DONE) {
		return -1;
	}

	netAddrToString(&s_Chosen, buf, sizeof(buf));
	colon = strrchr(buf, ':');

	if (!colon) {
		return -1;
	}

	*colon = '\0';

	if (buf[0] == '[') {
		snprintf(addr, size, "%.*s", (s32)strlen(buf) - 2, buf + 1);
	} else {
		snprintf(addr, size, "%s", buf);
	}

	*port = s_Chosen.port;

	return 0;
}

s32 netRdvPing(void)
{
	return s_Ladder == NETRDV_LADDER_DONE ? s_Ping : -1;
}

s32 netRdvLobbyRtt(void)
{
	return s_EchoRtt;
}

const char *netRdvPathName(s32 path)
{
	switch (path) {
	case NETRDV_PATH_LAN: return "lan";
	case NETRDV_PATH_DIRECT: return "direct";
	case NETRDV_PATH_PUNCH: return "punch";
	case NETRDV_PATH_RELAY: return "relay";
	}

	return "none";
}
