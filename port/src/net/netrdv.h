#ifndef _IN_NET_NETRDV_H
#define _IN_NET_NETRDV_H

#include <PR/ultratypes.h>

/**
 * The connectivity ladder (PLANS/NETPLAY.md "Lobby", phase 6b): how a room's
 * joiner reaches its host. netrdv.c speaks pdlobbyd's UDP rendezvous and
 * relay (tools/pdlobbyd/README.md, "UDP rendezvous" and "The relay") from
 * the game's own ENet socket, and the punch packets between the two games.
 * port/src/net/ only: netlobby.c drives it, net.c hands it the lobby's
 * datagrams. All of it runs on the main thread.
 *
 * In order, a joiner tries:
 *   LAN     a private-range address the host registered from its LAN
 *   DIRECT  the host's public address as the lobby saw it, answering before
 *           the host has sent the joiner anything (a host that is reachable)
 *   PUNCH   the same once the host sprays back at the joiner's addresses
 *           (both behind cone NATs)
 *   RELAY   a port on the lobby that forwards between the two (a symmetric
 *           NAT on either side)
 * and the match then connects to whichever answered (netSessionLobbyConnect
 * with this socket). The punch packets that found the path go on as pings,
 * which is the room's measured ping.
 */

#define NETRDV_PATH_NONE   0
#define NETRDV_PATH_LAN    1
#define NETRDV_PATH_DIRECT 2
#define NETRDV_PATH_PUNCH  3
#define NETRDV_PATH_RELAY  4

#define NETRDV_LADDER_IDLE    0 // no room, or this machine is the host
#define NETRDV_LADDER_WAITING 1 // registering; no PEER yet
#define NETRDV_LADDER_RUNNING 2 // punching or relaying
#define NETRDV_LADDER_DONE    3 // a path answered
#define NETRDV_LADDER_FAILED  4 // nothing answered: the launch falls back to the advertised endpoints

struct nethost;
struct netevent;
struct netaddr;

void netRdvSetLobby(const struct netaddr *udp); // the lobby's rendezvous address
s32 netRdvHasLobby(void);
s32 netRdvOpen(void);                // this machine's lobby socket (joiner, lister); 0 or -1
struct nethost *netRdvSocket(void);
void netRdvEnter(s32 host, const char *roomid, const char *udpid, const char *udpkey); // hex
void netRdvLeave(void);
void netRdvTick(void);
void netRdvRaw(struct nethost *h, const struct netevent *ev); // a PDLB datagram
void netRdvShutdown(void);

s32 netRdvLadder(void);
s32 netRdvPath(void);
s32 netRdvEndpoint(char *addr, s32 size, u16 *port); // the chosen path's address; 0 or -1
s32 netRdvPing(void);       // ms over the chosen path, -1 unknown
s32 netRdvLobbyRtt(void);   // ms to the lobby (ECHO), -1 unknown
const char *netRdvPathName(s32 path); // "lan", "direct", "punch", "relay", "none"

#endif
