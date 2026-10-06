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

// Views (protocol 9): the host sends snapshots and events to a remote
// player's slot (its mpindex, 0..MAX_PLAYERS-1) or to a spectator's
// (MAX_PLAYERS..NET_MAXVIEWS-1), who has no player
#define NET_MAXVIEWS (MAX_PLAYERS + NET_MAXSPECS)
s32 netSessionViewLive(s32 view);    // host: a client in that view plays (snapshots and events go to it)
s32 netSessionSpectating(void);      // client: this machine watches the match, no player of its own

// netsession.c: a lobby room's session, opened at run time (netlobby.c)
s32 netSessionLobbyHost(const char *name);
u16 netSessionLobbyPort(void);
void netSessionLobbySetRoom(const char *roomid, const char *secret);
void netSessionLobbyClock(s64 offset);
s32 netSessionLobbyConnect(const char *addr, u16 port, const char *ticket, const char *name, struct nethost *sock, s32 windowms); // windowms 0: NET_CONNECT_WINDOW_MS
void netSessionLobbyStop(void);
s32 netSessionLobbyRole(void);       // 1 host, 2 client, 0 none
s32 netSessionClientJoined(void);
s32 netSessionClientGone(void);
s32 netSessionClientUnreached(void); // client: the last connect found no host at the address
const char *netSessionWireName(s32 slot); // host: the name a slot goes out under (a lobby host's is its account)
s32 netSessionLastRefuse(void);     // client: NETREFUSE_* its last session ended on, -1 none
s32 netSessionSeatOutOfPlay(s32 slot); // host: that seat (mpindex) is open, its player out of play
s32 netSessionHostSlotOf(const char *user);
s32 netSessionHostUserSpectating(const char *user); // 1 a spectator's connection, 0 a player's, -1 none
void netSessionSetSpectate(s32 on);                 // client: connect as a spectator
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
void netPlayersHostSlotJoin(s32 slot, const struct netslotcfg *cfg, u32 tick); // a join in progress, its commands from tick
void netPlayersHostSpecCmd(s32 view, struct netbuf *b); // a spectator's CMD: only its snapshot ack
void netPlayersHostSpecGone(s32 view);
void netPlayersClientJoinAt(u32 tick); // a join in progress: the client's commands start at tick
void netPlayersHostOnCmd(s32 slot, struct netbuf *b);
void netPlayersHostMatchStart(void);  // H1, before the slots
void netPlayersClientMatchStart(s32 pad); // H3, after the rules are on
void netPlayersClientOnAck(struct netbuf *b);
void netPlayersMatchStopped(void);    // H12
void netPlayersTickReadPad(void);     // the remote pads into the tick's sample
void netPlayersTickBegin(void);
void netPlayersTickEnd(void);
s32 netPlayersClockPpm(void);
s32 netPlayersHostLastPlayed(s32 slot);   // the last command tick played, -1 none
s32 netPlayersHostSlotIsRemote(s32 slot);

s32 netPlayersLocalPad(void);         // the client's own player's pad (its mpindex)

// netpredict.c: the client's own player run ahead and reconciled
struct player;
struct netlpstate;
struct netmove;
void netPredictArgs(void);
void netPredictStageStart(void);
void netPredictMatchStopped(void);
void netPredictRecordCmd(u32 tick, u32 buttons, s8 sx, s8 sy, s8 rsx, s8 rsy, f32 mdx, f32 mdy, u8 flags);
void netPredictReplayMouse(f32 *dx, f32 *dy);
u32 netPlayersClientNewest(void);     // netplayers.c: the newest command the client made, 0 none
void netPredictTickEnd(void);         // the client's tick has run: its player's state for the tick
void netPredictCaptureMove(struct player *p, struct netmove *mv); // host: the block's movement state
// the pose step: the newest block (after command cmd; abs: a respawn or
// teleport, taken whatever it says) against this machine's own run
s32 netPredictReconcile(struct player *p, const struct netlpstate *lp, u32 cmd, s32 abs); // 1 the player was moved (a replay or a snap)
void netPredictLog(const char *why);
void netPredictHostTickEnd(s32 slot, struct player *p); // host: a remote player after the tick (--net-predict-log)
u32 netPlayersHostCurButtons(s32 slot, s8 *sx, s8 *sy); // the pad the host played for the slot this tick
void netPendingMouse(s32 *dx, s32 *dy); // net.c: the mouse not yet spent by a tick

// netents.c: the entity table and snapshots, the game's side (netsnap.h the wire)
struct netsnapack;
void netEntsArgs(void);
void netEntsStageStart(void);
void netEntsHostTickEnd(void);
void netEntsHostOnAck(s32 slot, const struct netsnapack *a);
void netEntsHostViewReset(s32 view);   // a new client in the view: keyframes from scratch
void netEntsClientOnSnap(const u8 *data, s32 len);
void netEntsClientWriteAck(struct netbuf *b);
void netEntsClientTickEnd(void);
void netEntsMatchStopped(void);
s32 netEntsHostile(void);
void netEntsHostileCmd(s32 slot, const u8 *data, s32 len);

// netents.c, for netpuppets.c: the client's decoded snapshots and its map
struct prop;
struct netsnapclient;
struct netdesc;
const struct netsnapclient *netEntsClient(void);
s32 netEntsMaxIds(void);
// host id's local prop if it is mapped to this host generation and the
// local prop is still the one mapped; NULL otherwise
struct prop *netEntsMapped(u16 id, u16 hostgen);
// the last descriptor that came for host id, or NULL
const struct netdesc *netEntsDesc(u16 id);
s32 netEntsPropIndex(const struct prop *prop);
void netSessionContentLog(const char *why, u32 ticks, u32 geticks); // netsession.c
struct prop *netTestLooseCrate(s32 pn); // netsession.c, the content gate's staging
s32 netEntsSetupCmdOf(s32 idx);           // host: its setup command, or -1
struct prop *netEntsSetupLocal(s32 cmd);  // client: this machine's prop of a setup command
u16 netEntsPropGen(s32 idx);
void netEntsClientApplyLocal(void); // the local-player block (pose step)

// netevents.c: the event channel (spec-entities.md §5)
void netEventsStageStart(void);
void netEventsMatchStopped(void);
void netEventsHostFlush(void);       // the tick's events to each remote slot (netTickEnd)
void netEventsHostMatchEnded(void);  // H9, before MATCH_END
void netEventsClientOnMsg(const u8 *data, s32 len);
void netEventsClientDrain(s32 haveclock, f64 rt); // after the pose step: what the render clock reached
void netEventsClientTickEnd(void);
void netEventsClientMatchEnd(void);  // MATCH_END: everything queued, then the table
u32 netEventsHostSeq(s32 slot);     // EVENTS messages sent to a slot this match (SNAP's evseq)
u32 netEventsClientSeq(void);       // ... and received here
void netEventsHostViewReset(s32 view); // a new client in the view: its EVENTS count from 0
void netEventsHostCatchUp(s32 view);   // its first EVENTS: the kill table (a SCORES event)
void netEventsHostScores(void);        // the kill table to everyone (a seat's row cleared)

// netpuppets.c
void netPuppetsStageStart(void);  // the client's match stage began (after netEntsStageStart)
void netPuppetsStop(void);        // H12, or the tables went
void netPuppetsOnSnap(u32 hosttick, s32 rate); // a snapshot decoded: the render clock's sample
void netPuppetsLog(const char *why);
void netPuppetsTraceFlush(void); // after the frame's lvRender: the traced poses as drawn

// netlagcomp.c: lag-compensated hits on the host
extern s32 g_NetLagComp;
void netLagCompArgs(void);
void netLagCompStageStart(void);
void netLagCompHostTickEnd(void);   // netTickEnd: every chr's pose for the tick
void netLagCompMatchStopped(void);
void netLagCompLog(const char *why);
void netLagCompPresentBegin(void);  // a frame drawn between ticks: the aim it moves is put back
void netLagCompPresentEnd(void);
// netplayers.c: the host tick (and fraction) the slot's current command was
// drawn at on its machine, and how far that sat behind the newest snapshot
// there (the interpolation delay, ticks); 0 if it said none
s32 netPlayersHostView(s32 slot, f64 *view, f32 *delay);
// netplayers.c: the slot's commands waiting in the host's queue (ticks)
s32 netPlayersHostDepth(s32 slot);
// netsession.c: the round trip ENet measures to the slot's peer and its
// variance, ms (rtt -1: no such peer); 0 found
s32 netSessionSlotRtt(s32 slot, s32 *rtt, s32 *rttvar);
// netlagcomp.c: the client's own aim as a correction replayed on a frame
// between ticks left it, kept for that frame's end
void netLagCompAimResave(s32 pn);
// netpuppets.c: the host tick (and fraction) this tick's pose step draws, 0
// none yet; delay (may be NULL) the interpolation delay in it
s32 netPuppetsViewTick(f64 *view, f32 *delay);

// netspec.c: a spectator's camera (protocol 9)
void netSpecStageStart(void);
void netSpecStop(void);
s32 netSpecCameraTick(void);   // the camera player's tick: 1 done (the player is not simulated)
void netSpecLog(const char *why);

// netscen.c: Combat Simulator scenarios online (phase 7a)
void netScenArgs(void);
void netScenStageStart(void);
void netScenMatchStopped(void);
void netScenHostTickEnd(void);           // netTickEnd: the tick's block, before the snapshots
const u8 *netScenHostBlock(void);        // the tick's block (NETSCEN_SIZE), NULL outside a match
void netScenHostFinal(u8 *out);          // H9: the block for MATCH_END
s32 netScenObjDesc(struct prop *prop, struct netdesc *d); // a scenario's prop: SCENOBJ's fields
void netScenClientOnSnap(u32 hosttick, const u8 *scen, u32 evseq); // evseq: EVENTS messages the host had sent before it
void netScenClientHudmsg(const char *text); // a hudmsg event shown: the scenario sound that goes with it
void netScenClientBeforeEvent(u32 tick); // netEventsClientDrain: the blocks before an event's tick
void netScenClientUpTo(f64 rt);          // ... and the ones the render clock has reached
void netScenClientFinal(const u8 *scen); // MATCH_END's block
void netScenClientApplyFinal(void);      // H10
void netScenHostSeatCleared(s32 slot);   // a seat opened: its scenario counts go with its kill table row
void netScenHostPacCheck(void);          // a seat's tick: Pop a Cap's victim gone out of play, the next one
// netpuppets.c: the local prop for a host entity, mapped or made here; NULL none
struct prop *netPuppetsLocalProp(u16 id, u16 gen);

// nethash.c: the session hash (computed once) and the stage hash
s32 netSessionHash(struct nethashcomp *comps, s32 max);
void netStageHashReset(void);
s32 netStageHashComponents(struct nethashcomp *comps, s32 max);
s32 netStageHashOpen(void);
void netStageHashOpenWindow(void);
void netStageHashCloseWindow(void);
u32 netStageHashCount(s32 comp);
void netStageHashNoteRng(void); // the RNG as it is now goes in at H6 (before a scenario's props draw)
s32 netEntsClientLpHolds(s32 weaponnum); // the newest local-player block: 1 holds it, 0 not, -1 no block

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
