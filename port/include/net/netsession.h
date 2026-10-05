#ifndef _IN_NET_NETSESSION_H
#define _IN_NET_NETSESSION_H

#include <PR/ultratypes.h>

/**
 * The session: --host/--connect, the handshake, the match's rules and stage,
 * the load barrier and the match end (PLANS/netplay/spec-stage.md, the wire
 * in netproto.h). Game code includes this through net.h, so it holds no bool
 * and nothing of ENet's. Every hook named H1..H14/HA..HD in the spec is one
 * of these calls, made behind g_NetMode != NETMODE_NONE.
 */

extern s32 g_NetMode;

static inline s32 netIsClient(void)
{
	return g_NetMode == NETMODE_CLIENT;
}

// main.c: after the mods are mounted (the session hash needs them)
void netSessionInit(void);
// main.c cleanup(): a polite LEAVE to whoever is connected
void netShutdown(void);
// pdmain.c: once a loop pass on a stage that is not a net match (title, menus)
void netIdleFrame(void);

// H1 mplayer.c mpStartMatch: the host builds RULES and STAGE_LOAD; returns
// the human count it decided (its own player plus the connected clients)
s32 netHostMatchStarting(s32 stagenum, s32 numplayers);
// H2 menutick.c: a client never starts a match from its own menus
void netClientRefuseLocalStart(void);
// H4 lv.c lvReset after psReset: the match's seeds
void netStageSeed(void);
// H5/H6 pdmain.c around lvReset: the stage hash window
void netStageHashBegin(s32 stagenum);
void netStageHashEnd(void);
// HA/HB file.c, HC xblastage.c: what the stage loads
void netStageHashFile(s32 filenum, s32 loadtype, const void *data, u32 len);
void netStageHashNote(s32 loadtype, u32 crc, u32 len);
// H7 lv.c lvTick: held at 0 level ticks until every machine has loaded
s32 netStageBarrierHold(void);
// H9 pdmain.c mainEndStage: the host tells the clients how it ended
void netHostMatchEnded(void);
// H10 mplayer.c mpEndMatch: a client takes the host's scores and awards
void netClientApplyMatchEnd(void);
// H11 ingame.c End Game: a client leaves the session
void netClientLeave(void);
// H12 pdmain.c after lvStop: the rules come off after a net match
void netStageStopped(void);
// H13 config.c configSave: the player's own values while pd.ini is written
void netRulesConfigSaveBegin(void);
void netRulesConfigSaveEnd(void);
// H14 options that change the game mid-match
s32 netRulesLocked(void);

// Main menu: the reason a session ended, shown once
extern s32 g_NetNoticePending;
void netMainMenuTick(void);

#endif
