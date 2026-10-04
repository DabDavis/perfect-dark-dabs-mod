#ifndef _IN_NET_NETTRANSPORT_H
#define _IN_NET_NETTRANSPORT_H

#include <PR/ultratypes.h>

/**
 * The netplay transport: ENet behind a plain C face.
 *
 * nettransport.c is the only file in the game that includes enet.h (besides
 * port/external/enet.c, which builds it). enet.h brings <stdbool.h>, whose
 * bool is one byte, where types.h makes bool an s32; a struct with a bool in
 * it seen through both is two different sizes (bool-two-sizes). So nothing
 * here is a bool or an ENet type: peers are small integers, a host is an
 * opaque pointer, and events come out through struct netevent.
 *
 * Several hosts can live in one process (the loopback test runs a server and
 * two clients side by side). All calls for all hosts must come from one
 * thread: the raw-datagram intercept finds its host through a static set for
 * the length of netHostService.
 *
 * Channels:
 *   NET_CHAN_UNRELIABLE  snapshots and usercmds, newest wins
 *   NET_CHAN_RELIABLE    events, chat, stage flow: reliable and in order
 *   NET_CHAN_BULK        rules blobs and manifests: reliable, own order, so a
 *                        big one does not hold up the events behind it
 *
 * Raw datagrams: anything starting with the four bytes NET_RAWMAGIC is
 * taken off the socket before ENet sees it and comes out as NETEVENT_RAW
 * with the sender's address. netHostSendRaw sends one from the same socket.
 * That is what hole punching and the lobby's rendezvous need: the NAT
 * mapping they open is the one ENet's own packets then use. ENet never sends
 * a datagram that starts 0xff (its first byte holds a 12-bit peer id and two
 * flag bits below the top one), so the two cannot be confused.
 */

#define NET_CHAN_UNRELIABLE 0
#define NET_CHAN_RELIABLE   1
#define NET_CHAN_BULK       2
#define NET_NUMCHANNELS     3

#define NET_MTU             1200            // UDP payload ENet will build up to
#define NET_MAXUNRELIABLE   1100            // an unreliable send that fits one datagram whole
#define NET_MAXPACKET       (128 * 1024)    // a reliable send, fragmented by ENet
#define NET_MAXWAITING      (256 * 1024)    // per peer, received and not yet taken
#define NET_RAWMAGIC        "\xff\xffPD"
#define NET_RAWMAGICLEN     4
#define NET_MAXRAW          (NET_MTU - NET_RAWMAGICLEN)

// netHostSend flags
#define NET_SEND_RELIABLE    0x01
#define NET_SEND_UNSEQUENCED 0x02 // unreliable only: late ones are delivered, not dropped

#define NETEVENT_NONE       0
#define NETEVENT_CONNECT    1 // peer is connected (either end)
#define NETEVENT_DISCONNECT 2 // peer is gone: closed, refused or timed out
#define NETEVENT_RECEIVE    3 // data from peer on channel
#define NETEVENT_RAW        4 // a raw datagram from `from`

// An IPv6 address (IPv4 as ::ffff:a.b.c.d), network byte order, and a port
struct netaddr {
	u8 ip[16];
	u16 port;
};

struct netevent {
	s32 type;
	s32 peer;        // CONNECT, DISCONNECT, RECEIVE
	s32 channel;     // RECEIVE
	const u8 *data;  // RECEIVE, RAW: valid until the next netHostService
	s32 len;
	u32 userdata;    // CONNECT: the connector's data; DISCONNECT: the reason
	s32 timedout;    // DISCONNECT: 1 if it timed out rather than closed
	struct netaddr from; // RAW: the sender; other events: the peer's address
};

struct netpeerstats {
	s32 connected;
	u32 rtt;          // ms, smoothed
	u32 rttvar;       // ms
	u32 mtu;
	u32 packetssent;  // commands sent since connect
	u32 resent;       // reliable commands resent after their ack was late
	f32 resentfrac;   // resent / packetssent: loss as ENet sees it, an upper
	                  // bound, since a late ack counts as a loss
	u32 bytessent;
	u32 bytesreceived;
};

struct nethost;

s32 netTransportInit(void);
void netTransportShutdown(void);
u32 netTransportTime(void); // ms, ENet's clock

/**
 * Opens a host bound to bindaddr (NULL: every interface) and port (0: one
 * the system picks; netHostPort says which). maxpeers is how many may be
 * connected at once, in either direction: a client wants 1.
 */
struct nethost *netHostCreate(const char *bindaddr, u16 port, s32 maxpeers);
void netHostDestroy(struct nethost *h);
u16 netHostPort(const struct nethost *h);

// Starts connecting; the peer id, or -1. NETEVENT_CONNECT or _DISCONNECT follows
s32 netHostConnect(struct nethost *h, const char *hostname, u16 port, u32 userdata);

/**
 * Sends and receives, waiting up to timeoutms for something to happen.
 * Returns 1 with *ev filled, 0 if nothing happened, -1 on a socket error.
 * Call it until it returns 0 each frame. A -1 is not the end of the host:
 * what it had not yet read waits for the next call, and peers that are gone
 * come out as NETEVENT_DISCONNECT, so stop for this frame and carry on.
 * Datagrams that are not ours or are too long are dropped, never an error.
 *
 * A peer that stops answering is dropped (DISCONNECT, timedout 1) once a
 * reliable command to it has gone unanswered ENET_PD_RESEND_LIMIT (10) times
 * and at least 5 s, or after 30 s whatever the count.
 */
s32 netHostService(struct nethost *h, struct netevent *ev, u32 timeoutms);
void netHostFlush(struct nethost *h);

/**
 * Queues len bytes for peer on channel. Unreliable sends over
 * NET_MAXUNRELIABLE are refused rather than fragmented (one lost fragment
 * would lose the lot). Returns 0, or -1 if refused.
 */
s32 netHostSend(struct nethost *h, s32 peer, s32 channel, const void *data, s32 len, s32 flags);

// Closes politely: NETEVENT_DISCONNECT follows once the peer acknowledges
void netHostDisconnect(struct nethost *h, s32 peer, u32 reason);
// Drops the peer at once; the peer is told once, unreliably, and no event follows here
void netHostDisconnectNow(struct nethost *h, s32 peer, u32 reason);

s32 netHostPeerStats(const struct nethost *h, s32 peer, struct netpeerstats *out);
s32 netHostPeerAddr(const struct nethost *h, s32 peer, struct netaddr *out);

s32 netAddrResolve(const char *hostname, u16 port, struct netaddr *out);
void netAddrToString(const struct netaddr *addr, char *buf, s32 bufsize);

// Sends NET_RAWMAGIC then len bytes (at most NET_MAXRAW) to `to`. 0, or -1
s32 netHostSendRaw(struct nethost *h, const struct netaddr *to, const void *data, s32 len);

/**
 * The loss and latency simulator, for tests: every ENet datagram this host
 * receives is dropped droppct times in a hundred, and the rest held back
 * delayms plus 0..jitterms. The rolls come from seed, so a run is repeatable
 * as far as the order datagrams arrive in is. Raw datagrams are not touched.
 * All zero turns it off.
 */
void netHostSetSim(struct nethost *h, s32 droppct, s32 delayms, s32 jitterms, u32 seed);

#endif
