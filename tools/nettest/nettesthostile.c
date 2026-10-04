/**
 * pd-nettest: what a hostile or careless sender can do to a host.
 *
 * Unlike nettest.c this file includes enet.h, to build ENet commands by hand
 * and to run a bare ENet host as the far end. It must not include types.h
 * (see nettransport.h on bool), and nothing in it reaches the game.
 *
 * Each test returns its count of failed checks; nettest.c prints the verdicts.
 */

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#define ENET_NO_PRAGMA_LINK 1

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "external/enet.h"
#pragma GCC diagnostic pop
#include "net/nettransport.h"

#ifdef _WIN32
#include <psapi.h>
#endif

#define HCHECK(cond) do { \
	if (!(cond)) { \
		if (fails < 10) { \
			printf("  %s:%d: %s: check failed: %s\n", __FILE__, __LINE__, name, #cond); \
		} \
		fails++; \
	} \
} while (0)

// Resident memory in KB, or -1 where it cannot be had
static s32 rssKb(void)
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS pmc;

	if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		return (s32)(pmc.WorkingSetSize / 1024);
	}

	return -1;
#else
	FILE *f = fopen("/proc/self/statm", "r");
	long size = 0, resident = 0;

	if (!f) {
		return -1;
	}

	if (fscanf(f, "%ld %ld", &size, &resident) != 2) {
		resident = -1;
	}

	fclose(f);

	return resident < 0 ? -1 : (s32)(resident * 4);
#endif
}

static void drain(struct nethost *srv, s32 *connected, s32 *disconnected, s32 *errors, u32 waitms)
{
	struct netevent ev;
	s32 r;

	while ((r = netHostService(srv, &ev, waitms)) != 0) {
		if (r < 0) {
			(*errors)++;
			break;
		}

		if (ev.type == NETEVENT_CONNECT && connected) {
			(*connected)++;
		} else if (ev.type == NETEVENT_DISCONNECT && disconnected) {
			(*disconnected)++;
		}

		waitms = 0;
	}
}

/**
 * A bare ENet client connected to srv. Returns the peer or NULL. mtu, if
 * nonzero, is what the client offers in its connect.
 */
static ENetPeer *bareConnect(struct nethost *srv, ENetHost **outcli, u32 mtu, s32 *srvconnected)
{
	ENetHost *cli = enet_host_create(NULL, 1, NET_NUMCHANNELS, 0, 0, 0);
	ENetAddress addr;
	ENetPeer *peer;
	ENetEvent e;
	s32 errors = 0;
	s32 i;

	*outcli = cli;

	if (!cli) {
		return NULL;
	}

	if (mtu) {
		cli->mtu = mtu;
	}

	memset(&addr, 0, sizeof(addr));
	enet_address_set_hostname(&addr, "127.0.0.1");
	addr.port = netHostPort(srv);
	peer = enet_host_connect(cli, &addr, NET_NUMCHANNELS, 0);

	if (!peer) {
		return NULL;
	}

	if (mtu) {
		peer->mtu = mtu;
	}

	for (i = 0; i < 400 && !(*srvconnected && peer->state == ENET_PEER_STATE_CONNECTED); i++) {
		drain(srv, srvconnected, NULL, &errors, 5);

		while (enet_host_service(cli, &e, 5) > 0) {
			if (e.type == ENET_EVENT_TYPE_RECEIVE) {
				enet_packet_destroy(e.packet);
			}
		}
	}

	return peer->state == ENET_PEER_STATE_CONNECTED ? peer : NULL;
}

static s32 putU16(u8 *buf, s32 o, u32 v)
{
	buf[o++] = (v >> 8) & 0xff;
	buf[o++] = v & 0xff;
	return o;
}

static s32 putU32(u8 *buf, s32 o, u32 v)
{
	o = putU16(buf, o, v >> 16);
	return putU16(buf, o, v & 0xffff);
}

/**
 * Sends one hand-built SEND_FRAGMENT command from peer's socket: a fragment
 * of a packet the server will never see whole.
 */
static void sendFragment(ENetHost *cli, ENetPeer *peer, u8 channel, u16 seq, u32 count, u32 number, u32 total, u32 offset)
{
	u8 buf[64];
	ENetBuffer b;
	s32 o = 0;

	o = putU16(buf, o, peer->outgoingPeerID | (peer->outgoingSessionID << ENET_PROTOCOL_HEADER_SESSION_SHIFT));
	buf[o++] = ENET_PROTOCOL_COMMAND_SEND_FRAGMENT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	buf[o++] = channel;
	o = putU16(buf, o, seq);  // reliableSequenceNumber
	o = putU16(buf, o, seq);  // startSequenceNumber
	o = putU16(buf, o, 1);    // dataLength
	o = putU32(buf, o, count);
	o = putU32(buf, o, number);
	o = putU32(buf, o, total);
	o = putU32(buf, o, offset);
	buf[o++] = 0xaa;

	b.data = buf;
	b.dataLength = o;
	enet_socket_send(cli->socket, &peer->address, &b, 1);
}

/**
 * A connected stranger sends fragment starts that can never complete: first
 * claiming a million fragments of a one-byte packet (each once held a 128 KB
 * bitmap no limit saw), then tens of thousands of one-byte fragments queued
 * out of order (each held a command and a packet the waiting limit counted as
 * one byte). The server's memory must stay near NET_MAXWAITING, and it must
 * go on serving.
 */
s32 hostileFragmentFlood(void)
{
	const char *name = "hostile fragment flood";
	struct nethost *srv;
	ENetHost *cli = NULL;
	ENetPeer *peer;
	s32 srvconnected = 0, srvdisconnected = 0, errors = 0;
	s32 fails = 0;
	s32 before, grew1 = 0, grew2 = 0;
	s32 i, ch;

	srv = netHostCreate("127.0.0.1", 0, 4);
	HCHECK(srv != NULL);

	if (!srv) {
		goto done;
	}

	peer = bareConnect(srv, &cli, 0, &srvconnected);
	HCHECK(peer != NULL);

	if (!peer) {
		goto done;
	}

	before = rssKb();

	for (i = 0; i < 4000; i++) {
		sendFragment(cli, peer, NET_CHAN_RELIABLE, (u16)(i + 2), 1000000, 0, 1, 0);

		if ((i & 63) == 63) {
			drain(srv, &srvconnected, &srvdisconnected, &errors, 0);
		}
	}

	drain(srv, &srvconnected, &srvdisconnected, &errors, 20);

	if (before >= 0) {
		grew1 = rssKb() - before;
	}

	// Out of order (sequence 1 never comes) and inside the reliable windows
	before = rssKb();

	for (ch = 0; ch < NET_NUMCHANNELS; ch++) {
		for (i = 0; i < 28000; i++) {
			sendFragment(cli, peer, ch, (u16)(i + 2), 1, 0, 1, 0);

			if ((i & 63) == 63) {
				drain(srv, &srvconnected, &srvdisconnected, &errors, 0);
			}
		}
	}

	drain(srv, &srvconnected, &srvdisconnected, &errors, 20);

	if (before >= 0) {
		grew2 = rssKb() - before;
	}

	printf("  fragment flood: server memory +%d KB (bitmaps), +%d KB (one-byte commands)\n", grew1, grew2);

	// Before: 4000 bogus bitmaps were ~500 MB; 84000 one-byte commands ~17 MB
	HCHECK(grew1 < 8 * 1024);
	HCHECK(grew2 < 8 * 1024);
	HCHECK(errors == 0);

done:
	if (cli) {
		enet_host_destroy(cli);
	}

	netHostDestroy(srv);

	return fails;
}

/**
 * Datagrams longer than the host's MTU arrive from a stranger, and a peer
 * offers a bigger MTU than ours in its connect. The oversized datagrams must
 * be dropped quietly (on Windows they once failed the whole service with
 * WSAEMSGSIZE), the service never report an error, a raw datagram behind them
 * still come through, and the peer be held to our MTU, so its big reliable
 * send arrives whole.
 */
s32 hostileBigDatagrams(void)
{
	const char *name = "hostile oversized datagrams";
	struct nethost *srv;
	ENetHost *cli = NULL;
	ENetPeer *peer;
	ENetSocket sock = ENET_SOCKET_NULL;
	ENetAddress to;
	struct netevent ev;
	struct netpeerstats stats;
	static u8 big[4000];
	static u8 msg[3000];
	s32 srvconnected = 0;
	s32 fails = 0;
	s32 errors = 0, raws = 0, got = 0, srvpeer = -1;
	s32 sizes[] = { 1201, 1300, 1472, 1500, 4000 };
	s32 i, r;
	u32 start;

	srv = netHostCreate("127.0.0.1", 0, 4);
	HCHECK(srv != NULL);

	if (!srv) {
		goto done;
	}

	memset(&to, 0, sizeof(to));
	enet_address_set_hostname(&to, "127.0.0.1");
	to.port = netHostPort(srv);

	sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	HCHECK(sock != ENET_SOCKET_NULL);

	if (sock == ENET_SOCKET_NULL) {
		goto done;
	}

	enet_socket_set_option(sock, ENET_SOCKOPT_IPV6_V6ONLY, 0);
	memset(big, 0, sizeof(big));
	memcpy(big, NET_RAWMAGIC, NET_RAWMAGICLEN);

	for (i = 0; i < (s32)(sizeof(sizes) / sizeof(sizes[0])); i++) {
		ENetBuffer b;

		b.data = big;
		b.dataLength = sizes[i];
		enet_socket_send(sock, &to, &b, 1);
	}

	// A small raw one behind them must still be delivered
	{
		ENetBuffer b;

		memcpy(big + NET_RAWMAGICLEN, "ok", 2);
		b.data = big;
		b.dataLength = NET_RAWMAGICLEN + 2;
		enet_socket_send(sock, &to, &b, 1);
	}

	start = netTransportTime();

	while (netTransportTime() - start < 500) {
		while ((r = netHostService(srv, &ev, 10)) != 0) {
			if (r < 0) {
				errors++;
				break;
			}

			if (ev.type == NETEVENT_RAW) {
				raws++;
				HCHECK(ev.len == 2 && memcmp(ev.data, "ok", 2) == 0);
			}
		}
	}

	HCHECK(errors == 0);
	HCHECK(raws == 1);

	// A peer offering 1400 must be held to our MTU
	peer = bareConnect(srv, &cli, 1400, &srvconnected);
	HCHECK(peer != NULL);

	if (!peer) {
		goto done;
	}

	HCHECK(peer->mtu <= NET_MTU);

	for (i = 0; i < (s32)sizeof(msg); i++) {
		msg[i] = (u8)(i * 7);
	}

	enet_peer_send(peer, NET_CHAN_RELIABLE, enet_packet_create(msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE));
	start = netTransportTime();

	while (!got && netTransportTime() - start < 3000) {
		ENetEvent e;

		while (enet_host_service(cli, &e, 5) > 0) {
			if (e.type == ENET_EVENT_TYPE_RECEIVE) {
				enet_packet_destroy(e.packet);
			}
		}

		while ((r = netHostService(srv, &ev, 5)) != 0) {
			if (r < 0) {
				errors++;
				break;
			}

			if (ev.type == NETEVENT_CONNECT) {
				srvpeer = ev.peer;
			} else if (ev.type == NETEVENT_RECEIVE && ev.len == (s32)sizeof(msg)) {
				got = memcmp(ev.data, msg, sizeof(msg)) == 0;
				srvpeer = ev.peer;
			}
		}
	}

	HCHECK(got);
	HCHECK(errors == 0);

	if (srvpeer >= 0 && netHostPeerStats(srv, srvpeer, &stats) == 0) {
		HCHECK(stats.mtu <= NET_MTU);
	}

done:
	if (sock != ENET_SOCKET_NULL) {
		enet_socket_destroy(sock);
	}

	if (cli) {
		enet_host_destroy(cli);
	}

	netHostDestroy(srv);

	return fails;
}
