/**
 * See nettransport.h. This is the one game file that includes enet.h, and
 * it must never include types.h (or anything that does): enet.h's bool is
 * <stdbool.h>'s, types.h's is an s32.
 */

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L // getaddrinfo and friends under -std=c11, as enet.c
#endif
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#define ENET_NO_PRAGMA_LINK 1 // ws2_32 and winmm are in CMakeLists.txt

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "external/enet.h"
#pragma GCC diagnostic pop
#include "net/nettransport.h"

// port/external/enet.c: feeds ENet a datagram the simulator held back
int enet_host_inject_received(ENetHost *host, const ENetAddress *address, const void *data, size_t len);

#define RAWQUEUE_LEN   32  // raw datagrams waiting for netHostService; more are dropped
#define SIMQUEUE_LEN   2048 // datagrams the simulator is holding back; more are dropped

struct rawpacket {
	ENetAddress addr;
	s32 len;
	u8 data[NET_MTU];
};

struct simpacket {
	u32 due;
	ENetAddress addr;
	s32 len;
	u8 data[NET_MTU]; // the receive buffer is the host's MTU, so none is longer
};

struct nethost {
	ENetHost *enet;

	// The packet the last RECEIVE event pointed into, freed on the next service
	ENetPacket *held;

	struct rawpacket raw[RAWQUEUE_LEN];
	s32 rawhead;
	s32 rawcount;
	struct rawpacket rawout; // the one handed out, kept until the next service

	s32 simdrop;
	s32 simdelay;
	s32 simjitter;
	u32 simrng;
	struct simpacket *simq; // allocated when the simulator is first turned on
	s32 simcount;
};

static s32 g_NetTransportRefs = 0;

// The host inside enet_host_service, for the intercept callback, which ENet
// calls without any way to say whose it is
static struct nethost *g_NetServicing = NULL;

s32 netTransportInit(void)
{
	if (g_NetTransportRefs == 0 && enet_initialize() != 0) {
		return -1;
	}

	g_NetTransportRefs++;

	return 0;
}

void netTransportShutdown(void)
{
	if (g_NetTransportRefs > 0 && --g_NetTransportRefs == 0) {
		enet_deinitialize();
	}
}

u32 netTransportTime(void)
{
	return enet_time_get();
}

static void addrToNet(const ENetAddress *in, struct netaddr *out)
{
	memcpy(out->ip, &in->ipv6, 16);
	out->port = in->port;
}

static void addrFromNet(const struct netaddr *in, ENetAddress *out)
{
	memset(out, 0, sizeof(*out));
	memcpy(&out->ipv6, in->ip, 16);
	out->port = in->port;
}

static u32 simRoll(struct nethost *h)
{
	// xorshift32
	u32 x = h->simrng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	h->simrng = x;

	return x;
}

/**
 * Every datagram the socket receives comes through here before ENet looks at
 * it. 1 means "taken, ENet need not see it"; 0 lets ENet have it.
 */
static int ENET_CALLBACK netInterceptCallback(ENetEvent *event, ENetAddress *address, uint8_t *data, int len)
{
	struct nethost *h = g_NetServicing;

	if (h == NULL || data == NULL || len <= 0) {
		return 0;
	}

	if (len >= NET_RAWMAGICLEN && memcmp(data, NET_RAWMAGIC, NET_RAWMAGICLEN) == 0) {
		if (h->rawcount < RAWQUEUE_LEN && len - NET_RAWMAGICLEN <= NET_MAXRAW) {
			struct rawpacket *pkt = &h->raw[(h->rawhead + h->rawcount) % RAWQUEUE_LEN];

			pkt->addr = *address;
			pkt->len = len - NET_RAWMAGICLEN;
			memcpy(pkt->data, data + NET_RAWMAGICLEN, pkt->len);
			h->rawcount++;
		}

		return 1;
	}

	if (h->simdrop > 0 || h->simdelay > 0 || h->simjitter > 0) {
		struct simpacket *pkt;

		if (h->simdrop > 0 && (s32)(simRoll(h) % 100) < h->simdrop) {
			return 1;
		}

		if (h->simdelay <= 0 && h->simjitter <= 0) {
			return 0;
		}

		if (h->simq == NULL || h->simcount >= SIMQUEUE_LEN || len > (s32)sizeof(pkt->data)) {
			return 1;
		}

		pkt = &h->simq[h->simcount++];
		pkt->due = enet_time_get() + h->simdelay + (h->simjitter > 0 ? simRoll(h) % (u32)(h->simjitter + 1) : 0);
		pkt->addr = *address;
		pkt->len = len;
		memcpy(pkt->data, data, len);

		return 1;
	}

	return 0;
}

/**
 * Gives ENet the held-back datagrams that have fallen due, in the order they
 * fall due. Returns the ms until the next one, or -1 if none is waiting.
 */
static s32 simRelease(struct nethost *h)
{
	while (h->simcount > 0) {
		u32 now = enet_time_get();
		s32 best = -1;
		s32 i;

		for (i = 0; i < h->simcount; i++) {
			if (best < 0 || (s32)(h->simq[i].due - h->simq[best].due) < 0) {
				best = i;
			}
		}

		if ((s32)(h->simq[best].due - now) > 0) {
			return (s32)(h->simq[best].due - now);
		}

		enet_host_inject_received(h->enet, &h->simq[best].addr, h->simq[best].data, h->simq[best].len);

		h->simq[best] = h->simq[h->simcount - 1];
		h->simcount--;
	}

	return -1;
}

struct nethost *netHostCreate(const char *bindaddr, u16 port, s32 maxpeers)
{
	struct nethost *h;
	ENetAddress addr;
	size_t i;

	if (maxpeers <= 0 || maxpeers >= ENET_PROTOCOL_MAXIMUM_PEER_ID) {
		return NULL;
	}

	memset(&addr, 0, sizeof(addr));

	if (bindaddr && bindaddr[0]) {
		if (enet_address_set_hostname(&addr, bindaddr) != 0) {
			return NULL;
		}
	} else {
		addr.ipv6 = ENET_HOST_ANY;
	}

	addr.port = port;

	h = calloc(1, sizeof(*h));

	if (!h) {
		return NULL;
	}

	// Bandwidth 0 is "unlimited", ENet's throttle doing the rest; buffer size
	// 0 is ENet's minimum socket buffer, 256 KB
	h->enet = enet_host_create(&addr, maxpeers, NET_NUMCHANNELS, 0, 0, 0);

	if (!h->enet) {
		free(h);
		return NULL;
	}

	// Its defaults are 32 MB for one packet and for a peer's backlog: a
	// stranger could make us hold that much per connection
	h->enet->maximumPacketSize = NET_MAXPACKET;
	h->enet->maximumWaitingData = NET_MAXWAITING;

	// The MTU too, and on every peer: enet_host_create reset them all with
	// the default 1280 already, and a peer's MTU is what it offers in its
	// connect. The receive buffer is the host's MTU, so a peer left at 1280
	// would build datagrams this end cuts short, and every fragment of a big
	// reliable send would be resent forever. (A connector offering more than
	// ours is held to ours in enet_protocol_handle_connect.)
	h->enet->mtu = NET_MTU;

	for (i = 0; i < h->enet->peerCount; i++) {
		h->enet->peers[i].mtu = NET_MTU;
	}

	enet_host_set_intercept_callback(h->enet, netInterceptCallback);

	return h;
}

void netHostDestroy(struct nethost *h)
{
	if (!h) {
		return;
	}

	if (h->held) {
		enet_packet_destroy(h->held);
	}

	if (g_NetServicing == h) {
		g_NetServicing = NULL;
	}

	enet_host_destroy(h->enet);
	free(h->simq);
	free(h);
}

u16 netHostPort(const struct nethost *h)
{
	return h ? h->enet->address.port : 0;
}

static ENetPeer *peerGet(const struct nethost *h, s32 peer)
{
	if (!h || peer < 0 || (size_t)peer >= h->enet->peerCount) {
		return NULL;
	}

	return &h->enet->peers[peer];
}

s32 netHostConnect(struct nethost *h, const char *hostname, u16 port, u32 userdata)
{
	ENetAddress addr;
	ENetPeer *peer;

	if (!h || !hostname) {
		return -1;
	}

	memset(&addr, 0, sizeof(addr));

	if (enet_address_set_hostname(&addr, hostname) != 0) {
		return -1;
	}

	addr.port = port;
	peer = enet_host_connect(h->enet, &addr, NET_NUMCHANNELS, userdata);

	return peer ? (s32)(peer - h->enet->peers) : -1;
}

s32 netHostService(struct nethost *h, struct netevent *ev, u32 timeoutms)
{
	u32 start;

	memset(ev, 0, sizeof(*ev));
	ev->peer = -1;

	if (!h) {
		return -1;
	}

	if (h->held) {
		enet_packet_destroy(h->held);
		h->held = NULL;
	}

	start = enet_time_get();

	while (1) {
		ENetEvent e;
		s32 next;
		u32 wait = timeoutms;
		u32 spent;
		s32 result;

		if (h->rawcount > 0) {
			h->rawout = h->raw[h->rawhead];
			h->rawhead = (h->rawhead + 1) % RAWQUEUE_LEN;
			h->rawcount--;

			ev->type = NETEVENT_RAW;
			ev->data = h->rawout.data;
			ev->len = h->rawout.len;
			addrToNet(&h->rawout.addr, &ev->from);

			return 1;
		}

		spent = enet_time_get() - start;
		wait = spent < timeoutms ? timeoutms - spent : 0;

		// Wake for the simulator's next datagram rather than sleeping past it
		next = simRelease(h);

		if (next >= 0 && (u32)next < wait) {
			wait = next;
		}

		memset(&e, 0, sizeof(e));
		g_NetServicing = h;
		result = enet_host_service(h->enet, &e, wait);
		g_NetServicing = NULL;

		if (result < 0) {
			return -1;
		}

		if (result > 0 && e.peer != NULL) {
			ev->peer = (s32)(e.peer - h->enet->peers);
			addrToNet(&e.peer->address, &ev->from);

			switch (e.type) {
			case ENET_EVENT_TYPE_CONNECT:
				ev->type = NETEVENT_CONNECT;
				ev->userdata = e.data;
				return 1;
			case ENET_EVENT_TYPE_DISCONNECT:
			case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
				ev->type = NETEVENT_DISCONNECT;
				ev->userdata = e.data;
				ev->timedout = e.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT;
				return 1;
			case ENET_EVENT_TYPE_RECEIVE:
				if (e.packet == NULL) {
					break;
				}

				h->held = e.packet;
				ev->type = NETEVENT_RECEIVE;
				ev->channel = e.channelID;
				ev->data = e.packet->data;
				ev->len = (s32)e.packet->dataLength;
				return 1;
			default:
				break;
			}

			continue;
		}

		if (h->rawcount > 0) {
			continue;
		}

		if (enet_time_get() - start >= timeoutms) {
			return 0;
		}
	}
}

void netHostFlush(struct nethost *h)
{
	if (h) {
		enet_host_flush(h->enet);
	}
}

s32 netHostSend(struct nethost *h, s32 peer, s32 channel, const void *data, s32 len, s32 flags)
{
	ENetPeer *p = peerGet(h, peer);
	ENetPacket *packet;
	u32 enetflags = 0;

	if (!p || p->state != ENET_PEER_STATE_CONNECTED || channel < 0 || channel >= NET_NUMCHANNELS
			|| !data || len <= 0 || len > NET_MAXPACKET) {
		return -1;
	}

	if (flags & NET_SEND_RELIABLE) {
		enetflags |= ENET_PACKET_FLAG_RELIABLE;
	} else {
		if (len > NET_MAXUNRELIABLE) {
			return -1;
		}

		// ENet's throttle drops unreliable packets at the sender when it
		// thinks the link is lossy. Snapshots and usercmds are already paced
		// by their senders, who also choose what to leave out; a packet
		// thrown away under them would only be a gap nobody decided on.
		enetflags |= ENET_PACKET_FLAG_UNTHROTTLED;

		if (flags & NET_SEND_UNSEQUENCED) {
			enetflags |= ENET_PACKET_FLAG_UNSEQUENCED;
		}
	}

	packet = enet_packet_create(data, len, enetflags);

	if (!packet) {
		return -1;
	}

	if (enet_peer_send(p, (u8)channel, packet) < 0) {
		enet_packet_destroy(packet);
		return -1;
	}

	return 0;
}

void netHostDisconnect(struct nethost *h, s32 peer, u32 reason)
{
	ENetPeer *p = peerGet(h, peer);

	if (p && p->state != ENET_PEER_STATE_DISCONNECTED) {
		enet_peer_disconnect(p, reason);
	}
}

void netHostDisconnectNow(struct nethost *h, s32 peer, u32 reason)
{
	ENetPeer *p = peerGet(h, peer);

	if (p && p->state != ENET_PEER_STATE_DISCONNECTED) {
		enet_peer_disconnect_now(p, reason);
	}
}

s32 netHostPeerStats(const struct nethost *h, s32 peer, struct netpeerstats *out)
{
	ENetPeer *p = peerGet(h, peer);

	memset(out, 0, sizeof(*out));

	if (!p) {
		return -1;
	}

	out->connected = p->state == ENET_PEER_STATE_CONNECTED;
	out->rtt = p->roundTripTime;
	out->rttvar = p->roundTripTimeVariance;
	out->mtu = p->mtu;
	out->packetssent = (u32)p->totalPacketsSent;
	out->resent = (u32)p->totalPacketsLost;
	out->resentfrac = p->totalPacketsSent ? (f32)p->totalPacketsLost / (f32)p->totalPacketsSent : 0.0f;
	out->bytessent = (u32)p->totalDataSent;
	out->bytesreceived = (u32)p->totalDataReceived;

	return 0;
}

s32 netHostPeerAddr(const struct nethost *h, s32 peer, struct netaddr *out)
{
	ENetPeer *p = peerGet(h, peer);

	memset(out, 0, sizeof(*out));

	if (!p || p->state == ENET_PEER_STATE_DISCONNECTED) {
		return -1;
	}

	addrToNet(&p->address, out);

	return 0;
}

s32 netAddrResolve(const char *hostname, u16 port, struct netaddr *out)
{
	ENetAddress addr;

	memset(out, 0, sizeof(*out));
	memset(&addr, 0, sizeof(addr));

	if (!hostname || enet_address_set_hostname(&addr, hostname) != 0) {
		return -1;
	}

	addr.port = port;
	addrToNet(&addr, out);

	return 0;
}

void netAddrToString(const struct netaddr *addr, char *buf, s32 bufsize)
{
	static const u8 v4prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

	if (bufsize <= 0) {
		return;
	}

	if (memcmp(addr->ip, v4prefix, 12) == 0) {
		snprintf(buf, bufsize, "%u.%u.%u.%u:%u", addr->ip[12], addr->ip[13], addr->ip[14], addr->ip[15], addr->port);
	} else {
		ENetAddress e;
		char ip[64];

		addrFromNet(addr, &e);

		if (enet_address_get_ip(&e, ip, sizeof(ip)) != 0) {
			snprintf(ip, sizeof(ip), "?");
		}

		snprintf(buf, bufsize, "[%s]:%u", ip, addr->port);
	}
}

s32 netHostSendRaw(struct nethost *h, const struct netaddr *to, const void *data, s32 len)
{
	u8 packet[NET_MTU];
	ENetAddress addr;
	ENetBuffer buf;

	if (!h || !to || len < 0 || len > NET_MAXRAW || (len > 0 && !data)) {
		return -1;
	}

	memcpy(packet, NET_RAWMAGIC, NET_RAWMAGICLEN);

	if (len > 0) {
		memcpy(packet + NET_RAWMAGICLEN, data, len);
	}

	addrFromNet(to, &addr);
	buf.data = packet;
	buf.dataLength = NET_RAWMAGICLEN + len;

	return enet_socket_send(h->enet->socket, &addr, &buf, 1) == (int)buf.dataLength ? 0 : -1;
}

void netHostSetSim(struct nethost *h, s32 droppct, s32 delayms, s32 jitterms, u32 seed)
{
	if (!h) {
		return;
	}

	h->simdrop = droppct < 0 ? 0 : droppct > 100 ? 100 : droppct;
	h->simdelay = delayms < 0 ? 0 : delayms;
	h->simjitter = jitterms < 0 ? 0 : jitterms;
	h->simrng = seed ? seed : 0x9e3779b9;

	if ((h->simdelay > 0 || h->simjitter > 0) && h->simq == NULL) {
		h->simq = calloc(SIMQUEUE_LEN, sizeof(*h->simq));
	}

	// Turned off: what was held back goes now rather than never
	if (h->simdelay == 0 && h->simjitter == 0) {
		s32 i;

		for (i = 0; i < h->simcount; i++) {
			enet_host_inject_received(h->enet, &h->simq[i].addr, h->simq[i].data, h->simq[i].len);
		}

		h->simcount = 0;
	}
}
