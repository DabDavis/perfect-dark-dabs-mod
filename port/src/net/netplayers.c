#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "video.h"
#include "input.h"
#include "lib/joy.h"
#include "lib/mtx.h"
#include "game/camera.h"
#include "net/net.h"
#include "net/nettransport.h"
#include "net/netsnap.h"
#include "netint.h"

/**
 * Remote players on the host and the client's usercmds
 * (PLANS/netplay/spec-players.md, spec-tick.md §3d; the wire in netproto.h).
 *
 * A client captures one command a tick from the pad sample its own tick
 * consumed (its player's pad, START cleared) and the tick's mouse, and sends
 * every command the host has not acked, at most NET_MAXCMDSEND, unreliably,
 * once a tick. The host keeps a queue per remote pad and plays exactly one
 * command a host tick into that pad of the tick's sample, so bmove, amTick
 * and playerTick run the remote player unchanged:
 *
 *   - in tick order, never one twice;
 *   - dry: the last buttons and sticks held, no mouse, no new edges;
 *   - lost for good (older than the client still sends): skipped, counted;
 *   - too deep: the next one folded in (buttons ORed, mouse summed, sticks
 *     the newest), only while its buttons are the same unless far behind,
 *     so no press is ever folded away;
 *   - START always cleared: a remote player never opens a menu here.
 *
 * Each host tick tells each client how many of its commands wait (CMDACK);
 * the client trims its clock by up to 5% to keep one or two waiting
 * (Overwatch's time dilation, overwatch-netcode/NOTES.md §1.4).
 *
 * Slot numbering is the same on every machine: a client allocates all the
 * match's players, its own is g_NetLocalSlot (a playernum) on pad s_LocalPad
 * (its mpindex), and every other pad there is neutral (puppets are phase 4).
 */

#define NETCMD_RING        128
#define NET_DEPTH_FOLD     4    // fold a command in past this many waiting
#define NET_DEPTH_FOLDANY  12   // ... whatever its buttons past this many
#define NET_DEPTH_TARGET   384  // 1.5 commands waiting, in 1/256ths
#define NET_PPM_PER_DEPTH  130  // clock trim per 1/256th of a command off target
#define NET_PPM_MAX        50000

struct netcmd {
	u32 buttons;
	s8 sx;
	s8 sy;
	s8 rsx;
	s8 rsy;
	f32 mdx;
	f32 mdy;
	u8 flags;
	u32 viewtick;            // the host tick the client's puppets were drawn at
	u8 viewfrac;             // ... and the fraction past it, in 1/256ths (NETCMD_NOVIEW: none)
	u8 viewdelay;            // how far its render clock sat behind the newest snapshot, 1/8 ticks
};

#define NETCMD_NOVIEW 0xffffffff

// A remote pad on the host
struct netpadq {
	s32 remote;              // a client's player in this match
	u32 next;                // the next tick to play
	u32 skipto;              // ticks below this that never came are lost
	s32 haveack;
	u32 ack;                 // every command to this tick is here (or lost)
	u32 ringtick[NETCMD_RING]; // tick + 1 of the command held, 0 for none
	struct netcmd ring[NETCMD_RING];
	struct netcmd cur;       // what this tick plays
	struct netslotcfg cfg;
	s32 hascfg;
	// counts for the log and the harness
	u32 played;
	u32 folded;
	u32 dry;
	u32 lost;
	u32 presses;             // Z (fire) press edges played
	u32 outoforder;
	s32 lastplayed;          // the last tick played, -1 none
	u32 depthsum;
};

struct extplayerconfig g_NetExtCfg[MAX_PLAYERS];
s32 g_NetExtCfgOn = 0;
s32 g_NetRemotePass = 0;
s32 g_NetPassPlayer = -1;

static struct netpadq s_Pads[MAX_PLAYERS];
static s32 s_LocalPad = 0;       // this machine's player's pad in the match

// client
static struct netcmd s_Sent[NETCMD_RING];
static u32 s_SentTick[NETCMD_RING];
static s32 s_HaveNewest = 0;
static u32 s_Newest = 0;
static u32 s_AckNext = 0;        // the first tick the host does not have
static s32 s_DepthAvg = NET_DEPTH_TARGET;
static s32 s_Ppm = 0;
static u32 s_HostTick = 0;
static u32 s_AcksHeard = 0;
static u32 s_CmdsSent = 0;
static u32 s_ZPresses = 0;
static u32 s_PrevButtons = 0;
static s32 s_PpmMin = 0;
static s32 s_PpmMax = 0;

// --net-test-input FILE: a per-tick script for the local pad and mouse
struct netscriptline {
	u32 from;
	u32 to;
	u32 buttons;
	s8 sx;
	s8 sy;
	s8 rsx;
	s8 rsy;
	f32 mdx;
	f32 mdy;
};

#define NET_MAXSCRIPT 256

static struct netscriptline s_Script[NET_MAXSCRIPT];
static s32 s_ScriptLen = 0;
static s32 s_TestTrace = 0;      // --net-test-trace N: every player's state every N ticks

static u8 s_CmdBuf[NET_MAXUNRELIABLE];

// --net-test-aimat N[,Y[,GAIN]] (with --net-test-input): the script's mouse
// replaced by a turn toward player N as this machine last drew it (Y units
// above its prop's place), for the lag compensation harness
static s32 s_AimAt = -1;
static f32 s_AimY = 0;
static f32 s_AimGain = 0.5f;
static u32 s_AimTick1 = 0;   // the tick the aim below is for, + 1
static f32 s_AimDx = 0;
static f32 s_AimDy = 0;
static f32 s_AimNear = 0;    // farther than this from the target: walk toward it instead of the script's moves (until within it once)

extern s32 g_StageNum;

/*
 * Boot
 */

static void netPlayersLoadScript(const char *path)
{
	char line[256];
	FILE *f = fopen(path, "r");

	if (!f) {
		sysLogPrintf(LOG_ERROR, "net: --net-test-input %s: cannot open it", path);
		return;
	}

	while (fgets(line, sizeof(line), f) && s_ScriptLen < NET_MAXSCRIPT) {
		struct netscriptline *l = &s_Script[s_ScriptLen];
		unsigned int from;
		unsigned int to;
		unsigned int buttons;
		int sx;
		int sy;
		int rsx;
		int rsy;
		float mdx;
		float mdy;

		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
			continue;
		}

		if (sscanf(line, "%u %u %x %d %d %d %d %f %f", &from, &to, &buttons, &sx, &sy, &rsx, &rsy, &mdx, &mdy) != 9) {
			sysLogPrintf(LOG_WARNING, "net: --net-test-input: a line that is not \"from to buttons sx sy rsx rsy mdx mdy\": %s", line);
			continue;
		}

		l->from = from;
		l->to = to;
		l->buttons = buttons;
		l->sx = (s8)(sx < -128 ? -128 : sx > 127 ? 127 : sx);
		l->sy = (s8)(sy < -128 ? -128 : sy > 127 ? 127 : sy);
		l->rsx = (s8)(rsx < -128 ? -128 : rsx > 127 ? 127 : rsx);
		l->rsy = (s8)(rsy < -128 ? -128 : rsy > 127 ? 127 : rsy);
		l->mdx = mdx;
		l->mdy = mdy;
		s_ScriptLen++;
	}

	fclose(f);
	sysLogPrintf(LOG_NOTE, "net: --net-test-input: %d lines from %s", s_ScriptLen, path);
}

void netPlayersArgs(void)
{
	const char *script = sysArgGetString("--net-test-input");

	if (script) {
		netPlayersLoadScript(script);
	}

	s_TestTrace = sysArgGetInt("--net-test-trace", 0);

	{
		const char *aim = sysArgGetString("--net-test-aimat");

		if (aim) {
			float y = 0;
			float gain = 0.5f;

			s_AimAt = atoi(aim);
			aim = strchr(aim, ',');

			if (aim) {
				y = (float)atof(aim + 1);
				aim = strchr(aim + 1, ',');

				if (aim) {
					gain = (float)atof(aim + 1);
					aim = strchr(aim + 1, ',');

					if (aim) {
						s_AimNear = (float)atof(aim + 1);
					}
				}
			}

			s_AimY = y;
			s_AimGain = gain;
			sysLogPrintf(LOG_NOTE, "net: --net-test-aimat: the mouse turns to player %d (%+.0f units up, gain %.2f)", s_AimAt, s_AimY, s_AimGain);
		}
	}
}

/*
 * Who is where
 */

static s32 netInMatch(void)
{
	return netSessionMatchLoading();
}

// The match's ticks are running (after GO, before the stage stops)
static s32 netPlaying(void)
{
	return netSessionMatchActive() && !netSessionBarrierHeld();
}

static s32 netPadOfPlayer(s32 playernum)
{
	if (playernum < 0 || playernum >= PLAYERCOUNT()) {
		return -1;
	}

	return g_Vars.playerstats[playernum].mpindex & 3;
}

static s32 netPadIsRemote(s32 pad)
{
	return g_NetMode == NETMODE_SERVER && pad >= 0 && pad < MAX_PLAYERS && s_Pads[pad].remote && netInMatch();
}

s32 netIsLocalPad(s32 idx)
{
	if (g_NetMode == NETMODE_NONE || !netInMatch()) {
		return 1;
	}

	return !g_NetDedicated && idx == s_LocalPad;
}

s32 netLocalUiIndex(s32 playernum)
{
	return netIsLocalSlot(playernum) ? 0 : -1;
}

s32 netSlotHasMouse(s32 playernum)
{
	if (!g_NetDedicated && playernum == g_NetLocalSlot) {
		return 1;
	}

	return netPadIsRemote(netPadOfPlayer(playernum));
}

/*
 * The test script
 */

static const struct netscriptline *netScriptAt(u32 tick)
{
	s32 i;

	if (!s_ScriptLen || !netPlaying()) {
		return NULL;
	}

	for (i = 0; i < s_ScriptLen; i++) {
		if (tick >= s_Script[i].from && tick <= s_Script[i].to) {
			return &s_Script[i];
		}
	}

	return NULL;
}

/*
 * Hooks V and V2
 */

// --net-test-aimat's fourth number: whether the target is farther than that
static s32 netPlayersTestAimFar(void)
{
	struct player *me;
	struct player *tg;
	f32 dx;
	f32 dz;

	if (s_AimAt < 0 || s_AimNear <= 0 || s_AimAt >= PLAYERCOUNT() || g_NetLocalSlot < 0 || s_AimAt == g_NetLocalSlot) {
		return 0;
	}

	me = g_Vars.players[g_NetLocalSlot];
	tg = g_Vars.players[s_AimAt];

	if (!me || !tg || !me->prop || !tg->prop) {
		return 0;
	}

	dx = tg->prop->pos.x - me->prop->pos.x;
	dz = tg->prop->pos.z - me->prop->pos.z;

	// once there it stays: a hit's knock back is not walked off again
	if (dx * dx + dz * dz <= s_AimNear * s_AimNear) {
		s_AimNear = -1;
		return 0;
	}

	return 1;
}

s32 netContGetReadData(void *pads)
{
	OSContPad *pad = pads;
	OSContPad phys;
	const struct netscriptline *script;
	s32 err;
	s32 i;

	if (!netInMatch()) {
		return 0;
	}

	memset(&phys, 0, sizeof(phys));
	err = inputReadController(0, &phys);

	if (s_ScriptLen) {
		// the script is the whole of the local pad while it runs
		script = netScriptAt(g_NetTick);
		memset(&phys, 0, sizeof(phys));
		err = 0;

		if (script) {
			phys.button = script->buttons;
			phys.stick_x = script->sx;
			phys.stick_y = script->sy;
			phys.rstick_x = script->rsx;
			phys.rstick_y = script->rsy;
		}

		if (netPlayersTestAimFar()) {
			phys.button = (phys.button & Z_TRIG) | U_CBUTTONS;
		}
	}

	for (i = 0; i < MAXCONTROLLERS; i++) {
		memset(&pad[i], 0, sizeof(pad[i]));

		if (!g_NetDedicated && i == s_LocalPad) {
			pad[i] = phys;
			pad[i].errnum = err < 0 ? CONT_NO_RESPONSE_ERROR : 0;
		}
	}

	return 1;
}

s32 netVpadConnected(s32 idx)
{
	if (!netInMatch() || idx < 0 || idx >= MAX_PLAYERS) {
		return 0;
	}

	return (g_MpSetup.chrslots >> idx) & 1;
}

/*
 * Settings
 */

static f32 netClampF(f32 v, f32 lo, f32 hi, f32 def)
{
	if (!(v >= lo && v <= hi)) {
		return v != v ? def : v < lo ? lo : hi;
	}

	return v;
}

static s32 netClampS(s32 v, s32 lo, s32 hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

// CONTROLMODE_2x read the host's pad 0 as their second controller in MP
static u8 netClampControlMode(u8 mode)
{
	if (mode >= CONTROLMODE_21 && mode <= CONTROLMODE_24) {
		return mode - CONTROLMODE_21 + CONTROLMODE_11;
	}

	if (mode > CONTROLMODE_PC) {
		return CONTROLMODE_PC;
	}

	return mode;
}

static void netCfgToExt(const struct netslotcfg *cfg, struct extplayerconfig *ext)
{
	ext->fovy = netClampF(cfg->fovy, 30.f, 150.f, 60.f);
	ext->fovzoommult = netClampF(cfg->fovzoommult, 0.1f, 4.f, 1.f);
	ext->fovzoom = cfg->fovzoom ? 1 : 0;
	ext->mouseaimmode = netClampS(cfg->mouseaimmode, 0, 1);
	ext->mouseaimspeedx = netClampF(cfg->mouseaimspeedx, 0.f, 10.f, 0.7f);
	ext->mouseaimspeedy = netClampF(cfg->mouseaimspeedy, 0.f, 10.f, 0.7f);
	ext->crouchmode = netClampS(cfg->crouchmode, 0, 3);
	ext->radialmenuspeed = netClampF(cfg->radialmenuspeed, 0.f, 10.f, 4.f);
	ext->crosshairsway = netClampF(cfg->crosshairsway, 0.f, 10.f, 1.f);
	ext->extcontrols = cfg->extcontrols ? 1 : 0;
	ext->crosshaircolour = cfg->crosshaircolour;
	ext->crosshairsize = cfg->crosshairsize > 4 ? 4 : cfg->crosshairsize;
	ext->crosshairedgeboundary = netClampF(cfg->crosshairedgeboundary, 0.f, 1.f, 0.f);
	ext->crosshairhealth = netClampS(cfg->crosshairhealth, 0, 2);
	ext->usereloads = cfg->usereloads ? 1 : 0;
}

static void netPlayersApplyRemoteCfg(s32 slot)
{
	struct netpadq *q = &s_Pads[slot];
	struct mpplayerconfig *p = &g_PlayerConfigsArray[slot];

	p->controlmode = netClampControlMode(q->cfg.controlmode);
	p->options = q->cfg.options;
	netCfgToExt(&q->cfg, &g_NetExtCfg[slot]);
	g_NetExtCfg[slot].extcontrols = p->controlmode == CONTROLMODE_PC;

	// the aspect the client draws at; 0 (no window yet) reads as 4:3
	q->cfg.aspect = netClampF(q->cfg.aspect, 0.5f, 4.f, 4.f / 3.f);

	if (q->cfg.aspect == 0.5f) {
		q->cfg.aspect = 4.f / 3.f;
	}
}

/**
 * The local player's own settings, refreshed each tick: the options menu
 * mid-match changes g_PlayerExtCfg[0], whatever slot the player is in here
 */
static void netPlayersRefreshLocalCfg(void)
{
	if (g_NetExtCfgOn && !g_NetDedicated && s_LocalPad >= 0 && s_LocalPad < MAX_PLAYERS) {
		g_NetExtCfg[s_LocalPad] = g_PlayerExtCfg[0];
	}
}

f32 netSlotAspect(s32 playernum)
{
	const s32 pad = netPadOfPlayer(playernum);

	if (netPadIsRemote(pad)) {
		return s_Pads[pad].cfg.aspect;
	}

	return videoGetAspect();
}

/**
 * Aim Lock and akimbo triggers are the player's own (modoptions.c): a
 * remote player's come from its SLOTCFG, Aim Lock still only under the
 * host's COD Style Aiming; anyone else's are this machine's
 */
s32 netSlotAimLock(s32 playernum, s32 codaiming, s32 mine)
{
	const s32 pad = netPadOfPlayer(playernum);

	if (netPadIsRemote(pad)) {
		return codaiming && s_Pads[pad].cfg.aimlock;
	}

	return mine;
}

s32 netSlotAkimboTriggers(s32 playernum, s32 mine)
{
	const s32 pad = netPadOfPlayer(playernum);

	if (netPadIsRemote(pad)) {
		return s_Pads[pad].cfg.akimbotriggers != 0;
	}

	return mine;
}

struct player *netLocalPlayer(struct player *fallback)
{
	if (g_NetLocalSlot >= 0 && g_NetLocalSlot < PLAYERCOUNT() && g_Vars.players[g_NetLocalSlot]) {
		return g_Vars.players[g_NetLocalSlot];
	}

	return fallback;
}

void netRemotePassBegin(void)
{
	g_NetRemotePass = !netIsLocalSlot(g_Vars.currentplayernum);
	g_NetPassPlayer = g_NetRemotePass ? g_Vars.currentplayernum : -1;
}

/*
 * The mouse
 */

void netMouseDelta(s32 playernum, f32 *dx, f32 *dy)
{
	const s32 pad = netPadOfPlayer(playernum);
	const struct netscriptline *script;

	*dx = 0;
	*dy = 0;

	// a prediction replay's tick turns by its own command's mouse
	if (g_NetReplaying && netIsLocalSlot(playernum)) {
		netPredictReplayMouse(dx, dy);
		return;
	}

	if (netPadIsRemote(pad)) {
		// once a tick, as the client spends it: never again on a frame drawn
		// between ticks (the client's own reads none there, net.c), where the
		// aim's crosshair would be moved by it once more each frame
		if (g_NetPass == NETPASS_PRESENT_ONLY) {
			return;
		}

		if (s_Pads[pad].cur.flags & NETCMD_MOUSELOCKED) {
			*dx = s_Pads[pad].cur.mdx;
			*dy = s_Pads[pad].cur.mdy;
		}

		return;
	}

	if (!netIsLocalSlot(playernum) || g_NetDedicated) {
		return;
	}

	if (s_ScriptLen && netInMatch()) {
		script = g_NetPass == NETPASS_PRESENT_ONLY ? NULL : netScriptAt(g_NetTick);

		if (script) {
			*dx = script->mdx;
			*dy = script->mdy;
		}

		if (g_NetPass != NETPASS_PRESENT_ONLY && s_AimTick1 == g_NetTick + 1) {
			*dx = s_AimDx;
			*dy = s_AimDy;
		}

		return;
	}

	inputMouseGetScaledDelta(dx, dy);
}

void netMouseAbsDelta(s32 playernum, f32 *dx, f32 *dy)
{
	const s32 pad = netPadOfPlayer(playernum);

	if (netPadIsRemote(pad)) {
		// the delta came scaled by the client's signed speed: |speed| is
		// speed * its sign
		netMouseDelta(playernum, dx, dy);
		*dx *= s_Pads[pad].cfg.sensxsign;
		*dy *= s_Pads[pad].cfg.sensysign;
		return;
	}

	if (!netIsLocalSlot(playernum) || g_NetDedicated) {
		*dx = 0;
		*dy = 0;
		return;
	}

	if (s_ScriptLen && netInMatch()) {
		netMouseDelta(playernum, dx, dy);
		return;
	}

	inputMouseGetAbsScaledDelta(dx, dy);
}

s32 netMouseLocked(s32 playernum)
{
	const s32 pad = netPadOfPlayer(playernum);

	if (netPadIsRemote(pad)) {
		return (s_Pads[pad].cur.flags & NETCMD_MOUSELOCKED) != 0;
	}

	if (!netIsLocalSlot(playernum) || g_NetDedicated) {
		return 0;
	}

	if (s_ScriptLen && netInMatch()) {
		return 1;
	}

	return inputMouseIsLocked();
}

/*
 * Host
 */

static s32 netQHas(const struct netpadq *q, u32 tick)
{
	return q->ringtick[tick % NETCMD_RING] == tick + 1;
}

static u32 netQDepth(const struct netpadq *q)
{
	u32 n = 0;

	while (n < NETCMD_RING && netQHas(q, q->next + n)) {
		n++;
	}

	return n;
}

void netPlayersHostMatchStart(void)
{
	s32 i;

	memset(s_Pads, 0, sizeof(s_Pads));

	for (i = 0; i < MAX_PLAYERS; i++) {
		s_Pads[i].lastplayed = -1;
		g_NetExtCfg[i] = g_PlayerExtCfg[i];
	}

	s_LocalPad = g_NetDedicated ? -1 : 0;
	g_NetExtCfgOn = 1;
}

void netPlayersHostSlotStart(s32 slot, const struct netslotcfg *cfg)
{
	struct netpadq *q;

	if (slot < 0 || slot >= MAX_PLAYERS) {
		return;
	}

	q = &s_Pads[slot];
	memset(q, 0, sizeof(*q));
	q->remote = 1;
	q->lastplayed = -1;
	q->cfg = *cfg;
	q->hascfg = 1;
	netPlayersApplyRemoteCfg(slot);

	sysLogPrintf(LOG_NOTE, "net: slot %d is remote: control mode %d, fov %.0f, aspect %.2f, mouse signs %d %d",
			slot, g_PlayerConfigsArray[slot].controlmode, g_NetExtCfg[slot].fovy, q->cfg.aspect,
			q->cfg.sensxsign, q->cfg.sensysign);
}

void netPlayersHostSlotCfg(s32 slot, const struct netslotcfg *cfg)
{
	if (slot < 0 || slot >= MAX_PLAYERS || !s_Pads[slot].remote || !netInMatch()) {
		return;
	}

	s_Pads[slot].cfg = *cfg;
	netPlayersApplyRemoteCfg(slot);
}

static void netPlayersLogSlot(s32 slot, const char *why)
{
	const struct netpadq *q = &s_Pads[slot];

	sysLogPrintf(LOG_NOTE, "net: slot %d commands %s: played %u (ticks 0..%d in order, %u out of order), folded %u, dry %u, lost %u, fire presses %u, mean depth %.2f",
			slot, why, q->played, q->lastplayed, q->outoforder, q->folded, q->dry, q->lost, q->presses,
			q->played + q->dry ? (f32)q->depthsum / (f32)(q->played + q->dry - q->folded) : 0.f);
}

void netPlayersHostSlotGone(s32 slot)
{
	if (slot < 0 || slot >= MAX_PLAYERS || !s_Pads[slot].remote) {
		return;
	}

	netPlayersLogSlot(slot, "at leaving");
	// its player stays in the match on a neutral pad
	s_Pads[slot].remote = 0;
}

void netPlayersHostOnCmd(s32 slot, struct netbuf *b)
{
	struct netpadq *q;
	struct netcmd cmds[NET_MAXCMDSEND];
	struct netsnapack ack;
	u32 matchid;
	u32 first;
	u32 t;
	s32 count;
	s32 i;

	if (slot < 0 || slot >= MAX_PLAYERS) {
		return;
	}

	q = &s_Pads[slot];
	matchid = netBufReadU32(b);
	netSnapAckRead(b, &ack);
	first = netBufReadU32(b);
	count = netBufReadU8(b);

	if (count < 1 || count > NET_MAXCMDSEND) {
		b->error = 1;
	}

	for (i = 0; i < count && netBufOk(b); i++) {
		cmds[i].buttons = netBufReadU32(b);
		cmds[i].sx = netBufReadS8(b);
		cmds[i].sy = netBufReadS8(b);
		cmds[i].rsx = netBufReadS8(b);
		cmds[i].rsy = netBufReadS8(b);
		cmds[i].mdx = netBufReadF32(b);
		cmds[i].mdy = netBufReadF32(b);
		cmds[i].flags = netBufReadU8(b);
		cmds[i].viewtick = netBufReadU32(b);
		cmds[i].viewfrac = netBufReadU8(b);
		cmds[i].viewdelay = netBufReadU8(b);

		// nothing from the wire is trusted: a mouse that is not a number,
		// or a turn of more than a whole screen in one tick, is none
		if (!(cmds[i].mdx >= -1000.f && cmds[i].mdx <= 1000.f)) {
			cmds[i].mdx = 0;
		}

		if (!(cmds[i].mdy >= -1000.f && cmds[i].mdy <= 1000.f)) {
			cmds[i].mdy = 0;
		}

		cmds[i].buttons &= ~START_BUTTON;
	}

	if (!netBufOk(b) || netBufRemaining(b) != 0) {
		// a bad packet is dropped (unreliable: one could be cut short)
		return;
	}

	if (!q->remote || matchid != netSessionMatchId() || !netInMatch()) {
		return;
	}

	// the snapshots it has (netents.c)
	netEntsHostOnAck(slot, &ack);

	// a client's ticks run with the host's (the clock keeps it 1-2 ahead):
	// one more than a ring ahead is a broken or hostile client, never a
	// skip of billions of ticks that would leave the queue past every
	// honest command
	if (first > g_NetTick + NETCMD_RING) {
		return;
	}

	// older ones the client no longer sends (past its cap) will never come
	if (first > q->next && first > (q->haveack ? q->ack + 1 : 0) && first > q->skipto) {
		q->skipto = first;
	}

	for (i = 0; i < count; i++) {
		t = first + (u32)i;

		if (t < q->next || t >= q->next + NETCMD_RING) {
			continue;
		}

		if (!netQHas(q, t)) {
			q->ring[t % NETCMD_RING] = cmds[i];
			q->ringtick[t % NETCMD_RING] = t + 1;
		}
	}

	// what has come (or is lost) without a gap
	t = q->haveack && q->ack + 1 > q->next ? q->ack + 1 : q->next;

	while (t < q->next + NETCMD_RING && (netQHas(q, t) || t < q->skipto)) {
		t++;
	}

	if (t > 0) {
		q->ack = t - 1;
		q->haveack = 1;
	}
}

static void netFold(struct netcmd *into, const struct netcmd *c)
{
	// the view the fire was pressed on: the first command's, unless the fire
	// is newly pressed in a later one (folded past NET_DEPTH_FOLDANY)
	if (c->buttons & ~into->buttons & Z_TRIG) {
		into->viewtick = c->viewtick;
		into->viewfrac = c->viewfrac;
		into->viewdelay = c->viewdelay;
	}

	into->buttons |= c->buttons;
	into->sx = c->sx;
	into->sy = c->sy;
	into->rsx = c->rsx;
	into->rsy = c->rsy;
	into->mdx += c->mdx;
	into->mdy += c->mdy;
	into->flags |= c->flags;
}

/**
 * The host's tick: one command for each remote pad into the tick's sample
 */
static void netPlayersHostPlay(void)
{
	struct netpadq *q;
	OSContPad pad;
	u32 prevbuttons;
	u32 depth;
	u32 lost;
	s32 slot;

	for (slot = 0; slot < MAX_PLAYERS; slot++) {
		q = &s_Pads[slot];

		if (!q->remote) {
			continue;
		}

		prevbuttons = q->cur.buttons;

		// what never came is lost: only the ring can hold one, so past a
		// ring of empty ticks the rest are skipped at once
		lost = 0;

		while (lost < NETCMD_RING && q->next + lost < q->skipto && !netQHas(q, q->next + lost)) {
			lost++;
		}

		if (lost == NETCMD_RING && q->next + lost < q->skipto) {
			lost = q->skipto - q->next;
		}

		q->lost += lost;
		q->next += lost;

		if (netQHas(q, q->next)) {
			q->cur = q->ring[q->next % NETCMD_RING];
			q->ringtick[q->next % NETCMD_RING] = 0;

			if ((s32)q->next != q->lastplayed + 1) {
				q->outoforder += (s32)q->next < q->lastplayed + 1;
			}

			q->lastplayed = q->next;
			q->next++;
			q->played++;

			// behind: fold the next ones in while that loses no edge
			depth = netQDepth(q);

			while (depth > NET_DEPTH_FOLD
					&& (q->ring[q->next % NETCMD_RING].buttons == q->cur.buttons || depth > NET_DEPTH_FOLDANY)) {
				netFold(&q->cur, &q->ring[q->next % NETCMD_RING]);
				q->ringtick[q->next % NETCMD_RING] = 0;
				q->lastplayed = q->next;
				q->next++;
				q->played++;
				q->folded++;
				depth--;
			}
		} else {
			// dry: hold what was held, no mouse, nothing new pressed
			q->cur.mdx = 0;
			q->cur.mdy = 0;
			q->dry++;
		}

		q->depthsum += netQDepth(q);
		q->presses += (q->cur.buttons & ~prevbuttons & Z_TRIG) ? 1 : 0;

		memset(&pad, 0, sizeof(pad));
		pad.button = q->cur.buttons & ~START_BUTTON;
		pad.stick_x = q->cur.sx;
		pad.stick_y = q->cur.sy;
		pad.rstick_x = q->cur.rsx;
		pad.rstick_y = q->cur.rsy;
		pad.errnum = 0;
		joyNetSetPendingPad(slot, &pad);
	}
}

static void netPlayersHostSendAcks(void)
{
	struct netbuf b;
	s32 slot;
	u32 depth;

	for (slot = 0; slot < MAX_PLAYERS; slot++) {
		struct netpadq *q = &s_Pads[slot];

		if (!q->remote) {
			continue;
		}

		depth = netQDepth(q);

		netBufInitWrite(&b, s_CmdBuf, sizeof(s_CmdBuf));
		netBufWriteU8(&b, NETMSG_CMDACK);
		netBufWriteU32(&b, netSessionMatchId());
		netBufWriteU32(&b, g_NetTick);
		netBufWriteU32(&b, q->haveack ? q->ack : 0xffffffff);
		netBufWriteU8(&b, (u8)(depth > 255 ? 255 : depth));
		netSessionSendSlot(slot, NET_CHAN_UNRELIABLE, s_CmdBuf, netBufLen(&b), 0);
	}
}

/*
 * Client
 */

void netPlayersClientMatchStart(s32 pad)
{
	s32 i;

	s_LocalPad = pad & 3;
	s_HaveNewest = 0;
	s_Newest = 0;
	s_AckNext = 0;
	s_DepthAvg = NET_DEPTH_TARGET;
	s_Ppm = 0;
	s_PpmMin = 0;
	s_PpmMax = 0;
	s_HostTick = 0;
	s_AcksHeard = 0;
	s_CmdsSent = 0;
	s_ZPresses = 0;
	s_PrevButtons = 0;
	memset(s_SentTick, 0, sizeof(s_SentTick));
	memset(s_Pads, 0, sizeof(s_Pads));

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_NetExtCfg[i] = g_PlayerExtCfg[i];
	}

	// this machine's player is the client's own first player, whatever slot
	// the host gave it (spec-players.md, correction 15)
	g_NetExtCfg[s_LocalPad] = g_PlayerExtCfg[0];

	if (s_LocalPad != 0) {
		g_PlayerConfigsArray[s_LocalPad].controlmode = g_PlayerConfigsArray[0].controlmode;
	}

	g_PlayerConfigsArray[s_LocalPad].controlmode = netClampControlMode(g_PlayerConfigsArray[s_LocalPad].controlmode);
	g_NetExtCfg[s_LocalPad].extcontrols = g_PlayerConfigsArray[s_LocalPad].controlmode == CONTROLMODE_PC;
	g_NetExtCfgOn = 1;
}

/**
 * --net-test-aimat: the turn this tick toward the target as the last frame
 * drew it, in this camera (whatever the angle conventions): the yaw and
 * pitch to it, a share of each a tick (a turn of 3.5 degrees a tick per
 * unit of mouse, bondmove.c)
 */
static void netPlayersTestAim(void)
{
	struct player *me;
	struct player *tg;
	f32 yaw;
	f32 pitch;

	s_AimTick1 = 0;

	if (s_AimAt < 0 || s_AimAt >= PLAYERCOUNT() || s_AimAt == g_NetLocalSlot || g_NetLocalSlot < 0) {
		return;
	}

	me = g_Vars.players[g_NetLocalSlot];
	tg = g_Vars.players[s_AimAt];

	if (!me || !tg || !tg->prop || !me->prop || me->isdead) {
		return;
	}

	// the eye's own frame: forward, right and up
	{
		const struct coord *f0 = &me->cam_look;
		const struct coord *u0 = &me->cam_up;
		f32 fl = sqrtf(f0->x * f0->x + f0->y * f0->y + f0->z * f0->z);
		f32 f[3];
		f32 r[3];
		f32 u[3];
		f32 d[3];
		f32 rl;
		f32 x;
		f32 y;
		f32 z;

		if (!(fl > 0.0001f)) {
			return;
		}

		f[0] = f0->x / fl; f[1] = f0->y / fl; f[2] = f0->z / fl;
		r[0] = f[1] * u0->z - f[2] * u0->y;
		r[1] = f[2] * u0->x - f[0] * u0->z;
		r[2] = f[0] * u0->y - f[1] * u0->x;
		rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);

		if (!(rl > 0.0001f)) {
			return;
		}

		r[0] /= rl; r[1] /= rl; r[2] /= rl;
		u[0] = r[1] * f[2] - r[2] * f[1];
		u[1] = r[2] * f[0] - r[0] * f[2];
		u[2] = r[0] * f[1] - r[1] * f[0];
		d[0] = tg->prop->pos.x - me->cam_pos.x;
		d[1] = tg->prop->pos.y + s_AimY - me->cam_pos.y;
		d[2] = tg->prop->pos.z - me->cam_pos.z;
		x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
		y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
		z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];

		// (the game's atan2f is its own, 0..2pi: libm's, in doubles)
		yaw = (f32)(atan2((double)x, (double)z) * 57.29577951);
		pitch = (f32)(atan2((double)y, sqrt((double)x * x + (double)z * z)) * 57.29577951);
	}

	if (!isfinite(yaw) || !isfinite(pitch)) {
		return;
	}

	s_AimDx = yaw * s_AimGain / 3.5f;
	s_AimDy = -pitch * s_AimGain / 3.5f;
	s_AimDx = s_AimDx > 8 ? 8 : s_AimDx < -8 ? -8 : s_AimDx;
	s_AimDy = s_AimDy > 8 ? 8 : s_AimDy < -8 ? -8 : s_AimDy;
	s_AimTick1 = g_NetTick + 1;

	if (s_TestTrace && g_NetTick % (u32)s_TestTrace == 0) {
		sysLogPrintf(LOG_NOTE, "net: aim tick %u at player %d: yaw %.2f pitch %.2f deg -> mouse %.3f %.3f (theta %.1f verta %.1f)", g_NetTick, s_AimAt, yaw, pitch, s_AimDx, s_AimDy, me->vv_theta, me->vv_verta);
	}
}

static void netPlayersClientCapture(void)
{
	struct netcmd *c;
	OSContPad pad;
	f32 mdx;
	f32 mdy;

	if (g_NetDedicated) {
		return;
	}

	joyNetGetCurrentPad(s_LocalPad, &pad);

	if (s_AimAt >= 0 && s_ScriptLen) {
		netPlayersTestAim();
	}

	netMouseDelta(g_NetLocalSlot, &mdx, &mdy);

	// a menu open here has the pad: the player stands still on the host
	// meanwhile (spec-players.md §6)
	if (g_NetLocalSlot >= 0 && g_NetLocalSlot < MAX_PLAYERS && g_Menus[g_NetLocalSlot].curdialog) {
		memset(&pad, 0, sizeof(pad));
		mdx = 0;
		mdy = 0;
	}

	c = &s_Sent[g_NetTick % NETCMD_RING];
	c->buttons = pad.button & ~START_BUTTON;
	c->sx = pad.stick_x;
	c->sy = pad.stick_y;
	c->rsx = pad.rstick_x;
	c->rsy = pad.rstick_y;
	c->mdx = mdx;
	c->mdy = mdy;
	c->flags = netMouseLocked(g_NetLocalSlot) ? NETCMD_MOUSELOCKED : 0;

	// what this tick's pose step will draw the others at (netlagcomp.c)
	{
		f64 view;
		f32 delay;

		if (netPuppetsViewTick(&view, &delay) && view >= 0 && view < 4.0e9) {
			c->viewtick = (u32)floor(view);
			c->viewfrac = (u8)((view - floor(view)) * 256.0);
			c->viewdelay = (u8)(delay * 8.f > 255.f ? 255 : delay < 0 ? 0 : delay * 8.f);
		} else {
			c->viewtick = NETCMD_NOVIEW;
			c->viewfrac = 0;
			c->viewdelay = 0;
		}
	}

	s_SentTick[g_NetTick % NETCMD_RING] = g_NetTick + 1;
	s_Newest = g_NetTick;
	s_HaveNewest = 1;

	s_ZPresses += (c->buttons & ~s_PrevButtons & Z_TRIG) ? 1 : 0;
	s_PrevButtons = c->buttons;

	// and kept for a replay of this tick (netpredict.c)
	netPredictRecordCmd(g_NetTick, c->buttons, c->sx, c->sy, c->rsx, c->rsy, c->mdx, c->mdy, c->flags);
}

s32 netPlayersLocalPad(void)
{
	return s_LocalPad;
}

// The client: the newest command it has made (and sent), 0 before any
u32 netPlayersClientNewest(void)
{
	return s_HaveNewest ? s_Newest : 0;
}

static void netPlayersClientSend(void)
{
	struct netbuf b;
	u32 first;
	u32 t;
	s32 count;

	if (!s_HaveNewest) {
		return;
	}

	first = s_AckNext;

	if (s_Newest + 1 - first > NET_MAXCMDSEND) {
		first = s_Newest + 1 - NET_MAXCMDSEND;
	}

	if (first > s_Newest) {
		return;
	}

	count = (s32)(s_Newest - first + 1);

	netBufInitWrite(&b, s_CmdBuf, sizeof(s_CmdBuf));
	netBufWriteU8(&b, NETMSG_CMD);
	netBufWriteU32(&b, netSessionMatchId());
	netEntsClientWriteAck(&b);
	netBufWriteU32(&b, first);
	netBufWriteU8(&b, (u8)count);

	for (t = first; t <= s_Newest; t++) {
		const struct netcmd *c = &s_Sent[t % NETCMD_RING];

		netBufWriteU32(&b, c->buttons);
		netBufWriteS8(&b, c->sx);
		netBufWriteS8(&b, c->sy);
		netBufWriteS8(&b, c->rsx);
		netBufWriteS8(&b, c->rsy);
		netBufWriteF32(&b, c->mdx);
		netBufWriteF32(&b, c->mdy);
		netBufWriteU8(&b, c->flags);
		netBufWriteU32(&b, c->viewtick);
		netBufWriteU8(&b, c->viewfrac);
		netBufWriteU8(&b, c->viewdelay);
	}

	if (netSessionSendServer(NET_CHAN_UNRELIABLE, s_CmdBuf, netBufLen(&b), 0) == 0) {
		s_CmdsSent++;
	}
}

void netPlayersClientOnAck(struct netbuf *b)
{
	u32 matchid = netBufReadU32(b);
	u32 hosttick = netBufReadU32(b);
	u32 ack = netBufReadU32(b);
	s32 depth = netBufReadU8(b);
	s32 err;

	if (!netBufOk(b) || netBufRemaining(b) != 0 || matchid != netSessionMatchId() || !netInMatch()) {
		return;
	}

	s_AcksHeard++;
	s_HostTick = hosttick;

	if (ack != 0xffffffff && ack + 1 > s_AckNext && ack <= s_Newest) {
		s_AckNext = ack + 1;
	}

	// time dilation: run fast while the host's queue is short, slow while
	// it is long; smoothed over about 16 acks, at most 5% either way
	s_DepthAvg += ((depth << 8) - s_DepthAvg) / 16;
	err = NET_DEPTH_TARGET - s_DepthAvg;
	s_Ppm = err * NET_PPM_PER_DEPTH;

	if (s_Ppm > NET_PPM_MAX) {
		s_Ppm = NET_PPM_MAX;
	} else if (s_Ppm < -NET_PPM_MAX) {
		s_Ppm = -NET_PPM_MAX;
	}

	if (s_Ppm < s_PpmMin) {
		s_PpmMin = s_Ppm;
	}

	if (s_Ppm > s_PpmMax) {
		s_PpmMax = s_Ppm;
	}
}

/**
 * The host tick the slot's command for this tick was drawn at on its
 * machine. Nothing from the wire is trusted: a tick from the future is now,
 * and the caller caps how far back it goes.
 */
s32 netPlayersHostView(s32 slot, f64 *view, f32 *delay)
{
	const struct netcmd *c;

	if (slot < 0 || slot >= MAX_PLAYERS || !s_Pads[slot].remote || s_Pads[slot].lastplayed < 0) {
		return 0;
	}

	c = &s_Pads[slot].cur;

	if (c->viewtick == NETCMD_NOVIEW) {
		return 0;
	}

	*view = (f64)c->viewtick + c->viewfrac / 256.0;
	*delay = c->viewdelay / 8.f;

	if (*view > (f64)g_NetTick) {
		*view = (f64)g_NetTick;
	}

	return 1;
}

s32 netPlayersHostDepth(s32 slot)
{
	return slot >= 0 && slot < MAX_PLAYERS && s_Pads[slot].remote ? (s32)netQDepth(&s_Pads[slot]) : 0;
}

s32 netPlayersHostLastPlayed(s32 slot)
{
	return slot >= 0 && slot < MAX_PLAYERS && s_Pads[slot].remote ? s_Pads[slot].lastplayed : -1;
}

u32 netPlayersHostCurButtons(s32 slot, s8 *sx, s8 *sy)
{
	if (slot < 0 || slot >= MAX_PLAYERS) {
		*sx = *sy = 0;
		return 0;
	}

	*sx = s_Pads[slot].cur.sx;
	*sy = s_Pads[slot].cur.sy;

	return s_Pads[slot].cur.buttons;
}

s32 netPlayersHostSlotIsRemote(s32 slot)
{
	return g_NetMode == NETMODE_SERVER && slot >= 0 && slot < MAX_PLAYERS && s_Pads[slot].remote && netPlaying();
}

/**
 * The client's clock trim. Past NET_LEAD_HOLD ticks ahead of the host's last
 * word (a host that stalled, under a debugger or a hitch, and dropped the
 * ticks it could not catch up on) the client holds still until the host is
 * near again: its commands would otherwise run more than a ring ahead of the
 * host's queue and be thrown away as a broken client's, every one after
 * (netPlayersHostOnCmd), and its prediction more than a ring ahead of any
 * snapshot. 5% either way could never win back a stall of seconds.
 */
#define NET_LEAD_HOLD 90

s32 netPlayersClockPpm(void)
{
	static u32 holds = 0;
	static s32 holding = 0;

	if (g_NetMode != NETMODE_CLIENT || !netPlaying()) {
		holding = 0;
		return 0;
	}

	if (s_AcksHeard && s_HaveNewest && (s32)(s_Newest - s_HostTick) > NET_LEAD_HOLD) {
		if (!holding) {
			holding = 1;

			if (holds++ < 8) {
				sysLogPrintf(LOG_NOTE, "net: %d ticks ahead of the host (tick %u, the host's %u): holding until it comes near",
						(s32)(s_Newest - s_HostTick), s_Newest, s_HostTick);
			}
		}

		return -1000000; // a rate of nothing: frametimeNetTicksDue's floor
	}

	holding = 0;

	return s_Ppm;
}

/*
 * The tick
 */

/**
 * Before joyDebugJoy consumes the tick's sample: the remote players' pads
 * go into it (into the pending one too: the first tick's)
 */
void netPlayersTickReadPad(void)
{
	if (g_NetMode == NETMODE_SERVER && netPlaying()) {
		netPlayersHostPlay();
	}
}

static void netPlayersTrace(void)
{
	s32 i;

	if (!s_TestTrace || g_NetTick % (u32)s_TestTrace != 0) {
		return;
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct player *p = g_Vars.players[i];
		const s32 pad = netPadOfPlayer(i);

		if (!p || !p->prop) {
			continue;
		}

		sysLogPrintf(LOG_NOTE, "net: trace tick %u player %d pad %d%s pos %.1f %.1f %.1f theta %.2f verta %.2f weapon %d ammo %d dead %d",
				g_NetTick, i, pad, netPadIsRemote(pad) ? " (remote)" : netIsLocalSlot(i) ? " (local)" : "",
				p->prop->pos.x, p->prop->pos.y, p->prop->pos.z, p->vv_theta, p->vv_verta,
				p->gunctrl.weaponnum, p->hands[HAND_RIGHT].loadedammo[0], p->isdead);
	}

	if (g_NetMode == NETMODE_CLIENT) {
		sysLogPrintf(LOG_NOTE, "net: trace tick %u client: host tick %u, acked to %u, clock %d ppm (seen %d..%d), depth %.2f, fire presses sent %u",
				g_NetTick, s_HostTick, s_AckNext, s_Ppm, s_PpmMin, s_PpmMax, s_DepthAvg / 256.f, s_ZPresses);
	} else {
		for (i = 0; i < MAX_PLAYERS; i++) {
			if (s_Pads[i].remote) {
				netPlayersLogSlot(i, "so far");
			}
		}
	}
}

// After joyDebugJoy, at the head of the tick
void netPlayersTickBegin(void)
{
	netPlayersRefreshLocalCfg();

	if (g_NetMode == NETMODE_CLIENT && netPlaying()) {
		netPlayersClientCapture();
		netPlayersClientSend();
		netSessionClientCfgTick();
	}
}

// After the tick's lvRender, before g_NetTick moves on
void netPlayersTickEnd(void)
{
	if (!netPlaying()) {
		return;
	}

	if (g_NetMode == NETMODE_SERVER) {
		netPlayersHostSendAcks();
	}

	netPlayersTrace();
}

/**
 * H12: the match's settings come off, and a client's player is player 0
 * again in the menus
 */
void netPlayersMatchStopped(void)
{
	s32 i;

	if (g_NetMode == NETMODE_SERVER) {
		for (i = 0; i < MAX_PLAYERS; i++) {
			if (s_Pads[i].remote) {
				netPlayersLogSlot(i, "at the match's end");
			}
		}
	} else if (g_NetMode == NETMODE_CLIENT) {
		sysLogPrintf(LOG_NOTE, "net: commands sent: %u packets to tick %u, host acked to %u, clock %d ppm (seen %d..%d), fire presses %u",
				s_CmdsSent, s_Newest, s_AckNext, s_Ppm, s_PpmMin, s_PpmMax, s_ZPresses);
		g_NetLocalSlot = 0;
	}

	memset(s_Pads, 0, sizeof(s_Pads));
	g_NetExtCfgOn = 0;
	g_NetRemotePass = 0;
	g_NetPassPlayer = -1;
	s_LocalPad = 0;
	s_Ppm = 0;
}
