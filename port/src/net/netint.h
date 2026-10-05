#ifndef _IN_NET_NETINT_H
#define _IN_NET_NETINT_H

#include <PR/ultratypes.h>
#include "net/netbuf.h"
#include "net/netproto.h"

/**
 * What port/src/net/'s files share among themselves and nothing else sees:
 * the session (netsession.c), the content hashes (nethash.c), the rules
 * (netrules.c), the lobby ticket (netticket.c) and the players' commands
 * (netplayers.c).
 */

struct nethashcomp {
	char name[NET_MAXCOMPNAME + 1];
	u64 hash;
};

// A player's own settings, as CONNECT and SLOTCFG carry them (netproto.h)
struct netslotcfg {
	u8 mpheadnum;
	u8 mpbodynum;
	u8 controlmode;
	u16 options;
	f32 fovy;
	f32 fovzoommult;
	s32 fovzoom;
	s32 mouseaimmode;
	f32 mouseaimspeedx;
	f32 mouseaimspeedy;
	s32 crouchmode;
	f32 radialmenuspeed;
	f32 crosshairsway;
	s32 extcontrols;
	u32 crosshaircolour;
	u32 crosshairsize;
	f32 crosshairedgeboundary;
	s32 crosshairhealth;
	s32 usereloads;
	f32 aspect;
	s8 sensxsign;
	s8 sensysign;
	u8 aimlock;
	u8 akimbotriggers;
};

// netbuf helpers for the session's messages
void netWriteU64(struct netbuf *b, u64 v);
u64 netReadU64(struct netbuf *b);
void netWriteStr(struct netbuf *b, const char *s, s32 maxlen); // cut to maxlen, never an error
u64 netDigestU64(const u8 *digest); // the first eight bytes of a SHA-256, big-endian

// netsession.c
struct nethost;
struct netevent;
extern struct nethost *g_NetHostSocket;
void netSessionArgs(void);
void netSessionEvent(const struct netevent *ev);
void netSessionTick(void);
void netSessionTickBegin(void);
s32 netSessionMatchActive(void);
s32 netSessionBarrierHeld(void);
s32 netSessionMatchLoading(void);  // a match's stage is the one loaded or loading (from H1/H3 to H12)
u32 netSessionMatchId(void);
void netReadSlotCfg(struct netbuf *b, struct netslotcfg *cfg);
void netWriteSlotCfg(struct netbuf *b);
// host: to the client in slot `slot`; client: to the host. 0, or -1 if nobody
s32 netSessionSendSlot(s32 slot, s32 channel, const void *data, s32 len, s32 flags);
s32 netSessionSendServer(s32 channel, const void *data, s32 len, s32 flags);
void netSessionClientCfgTick(void);  // client: SLOTCFG again when the settings change

// netsession.c: a lobby room's session, opened at run time (netlobby.c)
s32 netSessionLobbyHost(const char *name);
u16 netSessionLobbyPort(void);
void netSessionLobbySetRoom(const char *roomid, const char *secret);
void netSessionLobbyClock(s64 offset);
s32 netSessionLobbyConnect(const char *addr, u16 port, const char *ticket, const char *name, struct nethost *sock);
void netSessionLobbyStop(void);
s32 netSessionLobbyRole(void);       // 1 host, 2 client, 0 none
s32 netSessionClientJoined(void);
s32 netSessionClientGone(void);
s32 netSessionHostSlotOf(const char *user);
s32 netSessionHostNumClients(void);
const char *netSessionHostClientName(s32 index);
void netSessionHostDropUser(const char *user, const char *why);
void netSessionLobbyStartMatch(void);
const char *netSessionNoticeText(void);

// netplayers.c: remote players on the host, the client's usercmds, the
// clock (PLANS/netplay/spec-players.md, the wire in netproto.h)
void netPlayersArgs(void);
void netPlayersHostSlotStart(s32 slot, const struct netslotcfg *cfg); // H1, per joined client
void netPlayersHostSlotCfg(s32 slot, const struct netslotcfg *cfg);   // SLOTCFG mid-match
void netPlayersHostSlotGone(s32 slot);
void netPlayersHostOnCmd(s32 slot, struct netbuf *b);
void netPlayersHostMatchStart(void);  // H1, before the slots
void netPlayersClientMatchStart(s32 pad); // H3, after the rules are on
void netPlayersClientOnAck(struct netbuf *b);
void netPlayersMatchStopped(void);    // H12
void netPlayersTickReadPad(void);     // the remote pads into the tick's sample
void netPlayersTickBegin(void);
void netPlayersTickEnd(void);
s32 netPlayersClockPpm(void);

// nethash.c: the session hash (computed once) and the stage hash
s32 netSessionHash(struct nethashcomp *comps, s32 max);
void netStageHashReset(void);
s32 netStageHashComponents(struct nethashcomp *comps, s32 max);
s32 netStageHashOpen(void);
void netStageHashOpenWindow(void);
void netStageHashCloseWindow(void);
u32 netStageHashCount(s32 comp);

// netrules.c
struct netkeyvalue {
	char key[NET_MAXKEY + 1];
	s32 type;
	s32 s;
	f32 f;
	u32 u;
	char str[NET_MAXSTRVAL + 1];
};

#define NETKEY_SYNC       0
#define NETKEY_MUST       1 // always
#define NETKEY_MUST_GE    2 // on GoldenEye stages only
#define NETKEY_REFUSE     3 // a net game only with the stock value

s32 netRulesReadKey(const char *key, struct netkeyvalue *out);
void netRulesWriteValue(struct netbuf *b, const struct netkeyvalue *kv);
void netRulesReadValue(struct netbuf *b, struct netkeyvalue *kv);
s32 netRulesValuesEqual(const struct netkeyvalue *a, const struct netkeyvalue *b);
void netRulesValueString(const struct netkeyvalue *kv, char *buf, s32 size);
s32 netRulesWriteClientKeys(struct netbuf *b); // CONNECT's MUST and REFUSE keys
// The first MUST/REFUSE key the client's set fails against this host's, or
// 0: the class and key are written out for the refusal
s32 netRulesCheckClientKeys(const struct netkeyvalue *keys, s32 nkeys, s32 gestage, s32 *code, char *key, s32 keysize, char *text, s32 textsize);
void netRulesWrite(struct netbuf *b, u32 matchid);
u32 netRulesMatchId(void);
s32 netRulesRead(struct netbuf *b); // a client keeps the blob until STAGE_LOAD
void netRulesApply(void);           // a client: the stored blob over its own state
void netRulesSaveHost(void);        // the host: what it changes for remote slots
void netRulesRestore(void);         // H12
void netRulesSetLocked(s32 locked);

// netticket.c: tools/pdlobbyd/README.md "Join ticket", checks 1-4 and 6;
// 5 (the roster) is the lobby client's, phase 6. 0 if good, else why not;
// nonce (char[33]) and expiry out for netTicketUse, which spends it
s32 netTicketVerify(const char *ticket, s32 len, const u8 *secret32, const char *roomid, u64 lobbynow, char *user, s32 usersize, char *nonce, u64 *expiry, char *why, s32 whysize);
s32 netTicketUse(const char *nonce, u64 expiry, u64 lobbynow);
s32 netTicketSelfTest(void);

#endif
