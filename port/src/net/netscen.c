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
#include "game/dlights.h"
#include "game/inv.h"
#include "game/lang.h"
#include "game/playermgr.h"
#include "game/propsnd.h"
#include "game/radar.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/setup.h"
#include "game/mplayer/scenarios.h"
#include "lib/snd.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "netint.h"

/**
 * Combat Simulator scenarios online (PLANS/NETPLAY.md phase 7a).
 *
 * The host plays every scenario as it always has: its scenarioTick, every
 * player's scenarioTickChr (the remote players' in their passes on the
 * host), the sims' own. A client runs none of it (C13): its scenario props
 * are the host's entities (SCENOBJ descriptors, netents.c / netpuppets.c),
 * never made by its own scenarioInitProps, so nothing here can reap or
 * duplicate a token; and the scenario's state comes in a block in every
 * snapshot (the layout below), applied at the render clock in tick order
 * with the events (netEventsClientDrain): the block of host tick T after
 * every event up to T, so the scores worked out from it and the kill
 * table are the host's at T. Events are reliable and snapshots are not, so
 * each block comes with the count of EVENTS messages the host had sent this
 * client before it (SNAP's evseq): a block waits in the ring until that
 * many are in (a lost one being sent again), at most NETSCEN_HOLDMAX ticks
 * past its tick. MATCH_END carries the host's last block too.
 *
 * Every index in the block is an mpchr config slot (players 0-11, sims 12+),
 * as the DEATH event's are: g_MpAllChrPtrs' order is each machine's own.
 *
 * Block (NETSCEN_SIZE bytes, little-endian, zeros past what is used):
 *     0  u8  scenario (g_MpSetup.scenario; 0xff outside a match)
 *     4  s16 numpoints[MAX_MPCHRS]        MPCHR(slot)->numpoints (92)
 *   188  u16 tokenheld[MAX_PLAYERS]       playerstats[pn].tokenheldtime (12)
 *   212  u8  holds[MAX_PLAYERS]           pn's inventory: 1 briefcase, 2 uplink
 *   224  per scenario (OFF_BODY; protocol 10, was 184):
 *     HTB  REF token, f32 pos[3]
 *     HTM  s8 dlslot, s8 inrangeslot, s8 dlterminal, u8 0, REF uplink,
 *          REF terminal, u8 terminal team, u8 0, s16 numpoints[MAX_MPCHRS],
 *          u16 dltime240[MAX_MPCHRS]
 *     PAC  s16 victimindex, u16 age240, u8 nvictims, u8 victims[MAX_MPCHRS]
 *          (slots, 0xff none), s16 killcounts[MAX_MPCHRS],
 *          s16 survivalcounts[MAX_MPCHRS]
 *     KOH  s16 occupiedteam, elapsed240, movehill, hillindex, hillcount,
 *          s16 hillrooms[2], f32 hillpos[3], f32 colourfrac[3]
 *     CTC  s16 teamindexes[4], s16 playercountsperteam[4], s16 baserooms[4],
 *          REF tokens[4]
 *   REF (5 bytes): u8 kind (0 none, 1 a chr: u8 slot, 2 an entity: u16 id,
 *   u16 gen)
 */

#define OFF_SCEN    0
#define OFF_POINTS  4
#define OFF_HELD    (OFF_POINTS + MAX_MPCHRS * 2)
#define OFF_HOLDS   (OFF_HELD + MAX_PLAYERS * 2)
#define OFF_BODY    ((OFF_HOLDS + MAX_PLAYERS + 3) & ~3) // 224

#define HTB_TOKEN   (OFF_BODY + 0)
#define HTB_POS     (OFF_BODY + 5)

#define HTM_DL      (OFF_BODY + 0)
#define HTM_UPLINK  (OFF_BODY + 4)
#define HTM_TERM    (OFF_BODY + 9)
#define HTM_TEAM    (OFF_BODY + 14)
#define HTM_POINTS  (OFF_BODY + 16)
#define HTM_TIME    (HTM_POINTS + MAX_MPCHRS * 2)

#define PAC_INDEX   (OFF_BODY + 0)
#define PAC_AGE     (OFF_BODY + 2)
#define PAC_NUM     (OFF_BODY + 4)
#define PAC_VICTIMS (OFF_BODY + 5)
#define PAC_KILLS   (PAC_VICTIMS + MAX_MPCHRS)
#define PAC_SURV    (PAC_KILLS + MAX_MPCHRS * 2)

#define KOH_FIELDS  (OFF_BODY + 0)
#define KOH_ROOMS   (OFF_BODY + 10)
#define KOH_POS     (OFF_BODY + 14)
#define KOH_COLOUR  (OFF_BODY + 26)

#define CTC_INDEXES (OFF_BODY + 0)
#define CTC_COUNTS  (OFF_BODY + 8)
#define CTC_BASES   (OFF_BODY + 16)
#define CTC_TOKENS  (OFF_BODY + 24)

#define NETSCEN_TERMINALS 1 // HTM_NUM_TERMINALS (hackthatmac.inc)
#define NETSCEN_RING      64
#define NETSCEN_HOLDMAX   120 // ticks a block waits for its events past its tick at most

#define HOLD_BRIEFCASE 1
#define HOLD_UPLINK    2

typedef char netscen_body[OFF_BODY == 224 ? 1 : -1]; // the layout netproto.h documents
typedef char netscen_fits[(PAC_SURV + MAX_MPCHRS * 2 <= NETSCEN_SIZE && HTM_TIME + MAX_MPCHRS * 2 <= NETSCEN_SIZE) ? 1 : -1];

// host
static u8 s_HostBlock[NETSCEN_SIZE];
static s32 s_HostHave = 0;

// client
static struct {
	u32 tick;
	u32 evseq;  // the EVENTS messages the host had sent before it
	u8 held;    // it waited for them
	u8 data[NETSCEN_SIZE];
} s_Ring[NETSCEN_RING];
static s32 s_RingHead = 0;
static s32 s_RingCount = 0;
static u32 s_PacSkips = 0;           // host: Pop a Cap turns passed over seats out of play
static u32 s_RingLast = 0;          // the newest tick pushed + 1 (0 none)
static u8 s_Cur[NETSCEN_SIZE];      // the block applied last
static s32 s_HaveCur = 0;
static u32 s_CurTick = 0;
static u8 s_Final[NETSCEN_SIZE];    // MATCH_END's
static s32 s_HaveFinal = 0;

// counts (client)
static u32 s_Applied = 0;
static u32 s_Skipped = 0;           // a block of another scenario, or one with a bad field
static u32 s_Dropped = 0;           // the ring was full
static u32 s_Held = 0;              // blocks that waited for their events
static u32 s_GaveUp = 0;            // ... and were applied without them after NETSCEN_HOLDMAX
static u32 s_Refs = 0;
static u32 s_Unresolved = 0;        // an entity reference with no local prop yet
static u32 s_InvSyncs = 0;
static u32 s_Sounds = 0;

static FILE *s_Log = NULL;

/*
 * Bytes
 */

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static s16 gets16(const u8 *p) { return (s16)get16(p); }

static void putf(u8 *p, f32 f)
{
	u32 v;
	memcpy(&v, &f, 4);
	p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static f32 getf(const u8 *p)
{
	u32 v = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
	f32 f;
	memcpy(&f, &v, 4);
	return isfinite(f) && fabsf(f) < 1e7f ? f : 0;
}

/*
 * Slots: g_MpAllChrPtrs' index on this machine <-> the mpchr config slot
 */

static s32 netScenSlotOf(s32 playernum)
{
	s32 slot;

	if (playernum < 0 || playernum >= g_MpNumChrs) {
		return -1;
	}

	slot = func0f18d074(playernum);

	return slot >= 0 && slot < MAX_MPCHRS ? slot : -1;
}

static s32 netScenIndexOf(s32 slot)
{
	if (slot < 0 || slot >= MAX_MPCHRS) {
		return -1;
	}

	return func0f18d0e8(slot);
}

static void netScenWriteRef(u8 *p, struct prop *prop)
{
	memset(p, 0, 5);

	if (!prop) {
		return;
	}

	if ((prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) && prop->chr) {
		const s32 slot = netScenSlotOf(mpPlayerGetIndex(prop->chr));

		if (slot >= 0) {
			p[0] = 1;
			p[1] = (u8)slot;
		}

		return;
	}

	{
		const s32 idx = netEntsPropIndex(prop);
		const u16 gen = idx >= 0 ? netEntsPropGen(idx) : 0;

		if (gen) {
			p[0] = 2;
			put16(p + 1, (u32)idx);
			put16(p + 3, gen);
		}
	}
}

static struct prop *netScenResolve(const u8 *p)
{
	struct prop *prop = NULL;

	if (p[0] == 1) {
		const s32 k = netScenIndexOf(p[1]);

		if (k >= 0 && k < g_MpNumChrs && g_MpAllChrPtrs[k]) {
			prop = g_MpAllChrPtrs[k]->prop;
		}
	} else if (p[0] == 2) {
		s_Refs++;
		prop = netPuppetsLocalProp(get16(p + 1), get16(p + 3));

		if (!prop) {
			s_Unresolved++;
		}
	}

	return prop;
}

/*
 * What the HUD reads, as text both machines write the same way: the
 * harness compares the host's at tick T with the client's after its block
 * of T (tools/ci/netscenariotest.sh). ref: the block's reference on a
 * client (an entity counts by its kind, made here yet or not), NULL on the
 * host.
 */
static s32 netScenHolder(char *buf, s32 size, struct prop *prop, const u8 *ref)
{
	if (ref && ref[0] == 2) {
		return snprintf(buf, size, "w");
	}

	if (!prop) {
		return snprintf(buf, size, "-");
	}

	if ((prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) && prop->chr) {
		return snprintf(buf, size, "c%d", netScenSlotOf(mpPlayerGetIndex(prop->chr)));
	}

	return snprintf(buf, size, "w");
}

static void netScenCanon(s32 pn, char *buf, s32 size, s32 client)
{
	s32 len = 0;
	s32 prev = g_Vars.currentplayernum;
	s32 scores[MAX_MPCHRS];
	s32 deaths[MAX_MPCHRS];
	u8 in[MAX_MPCHRS];
	s32 i;

#define ADD(...) do { if (len < size) len += snprintf(buf + len, size - len, __VA_ARGS__); } while (0)

	memset(in, 0, sizeof(in));
	ADD("sc=%d", g_MpSetup.scenario);

	if (pn < 0 || pn >= PLAYERCOUNT() || !g_Vars.players[pn] || !g_Vars.players[pn]->prop) {
		ADD(" nopl");
		return;
	}

	setCurrentPlayerNum(pn);

	switch (g_MpSetup.scenario) {
	case MPSCENARIO_HOLDTHEBRIEFCASE: {
		char h[16];

		netScenHolder(h, sizeof(h), g_ScenarioData.htb.token, client ? s_Cur + HTB_TOKEN : NULL);
		ADD(" tok=%s hb=%d ht=%d", h, invHasBriefcase() ? 1 : 0, g_Vars.currentplayerstats->tokenheldtime);
		break;
	}
	case MPSCENARIO_HACKERCENTRAL: {
		struct scenariodata_htm *d = &g_ScenarioData.htm;
		char h[16];

		netScenHolder(h, sizeof(h), d->uplink, client ? s_Cur + HTM_UPLINK : NULL);
		ADD(" up=%s dl=%d term=%d inr=%d dlt=%d hu=%d", h, netScenSlotOf(d->dlplayernum), d->dlterminalnum,
				netScenSlotOf(d->playernuminrange), d->dltime240[pn], invHasDataUplink() ? 1 : 0);
		break;
	}
	case MPSCENARIO_POPACAP: {
		struct scenariodata_pac *d = &g_ScenarioData.pac;
		const s32 v = d->victimindex >= 0 && d->victimindex < g_MpNumChrs ? netScenSlotOf(d->victims[d->victimindex]) : -1;

		ADD(" vi=%d v=%d age=%d", d->victimindex, v, d->age240);
		break;
	}
	case MPSCENARIO_KINGOFTHEHILL: {
		struct scenariodata_koh *d = &g_ScenarioData.koh;
		const s32 team = g_Vars.currentplayer->prop->chr ? radarGetTeamIndex(g_Vars.currentplayer->prop->chr->team) : -1;

		ADD(" occ=%d el=%d mv=%d hill=%d room=%d hud=%d", d->occupiedteam, d->elapsed240, d->movehill, d->hillindex,
				d->hillrooms[0], team == d->occupiedteam && !d->movehill);
		break;
	}
	case MPSCENARIO_CAPTURETHECASE: {
		struct scenariodata_ctc *d = &g_ScenarioData.ctc;

		for (i = 0; i < 4; i++) {
			char h[16];

			netScenHolder(h, sizeof(h), d->tokens[i], client ? s_Cur + CTC_TOKENS + i * 5 : NULL);
			ADD(" t%d=%s/%d/%d", i, h, d->teamindexes[i], d->baserooms[i]);
		}

		ADD(" hb=%d", invHasBriefcase() ? 1 : 0);
		break;
	}
	default:
		break;
	}

	setCurrentPlayerNum(prev);

	// the scores the end screen and the scoreboard work out
	for (i = 0; i < g_MpNumChrs; i++) {
		const s32 slot = netScenSlotOf(i);

		if (slot >= 0) {
			in[slot] = 1;
			scenarioCalculatePlayerScore(MPCHR(slot), slot, &scores[slot], &deaths[slot]);
		}
	}

	ADD(" |");

	for (i = 0; i < MAX_MPCHRS; i++) {
		if (in[i]) {
			ADD(" %d:%d/%d/%d", i, scores[i], deaths[i], MPCHR(i)->numpoints);
		}
	}

#undef ADD
}

static void netScenLogLine(const char *tag, u32 tick, s32 pn, s32 client)
{
	char buf[1024];

	if (!s_Log) {
		return;
	}

	netScenCanon(pn, buf, sizeof(buf), client);
	fprintf(s_Log, "%s %u %d %s\n", tag, tick, pn, buf);
}

/*
 * Host
 */

static void netScenCapture(u8 *out)
{
	s32 prev = g_Vars.currentplayernum;
	s32 i;

	memset(out, 0, NETSCEN_SIZE);

	if (!g_Vars.normmplayerisrunning) {
		out[OFF_SCEN] = 0xff;
		return;
	}

	out[OFF_SCEN] = (u8)g_MpSetup.scenario;

	for (i = 0; i < MAX_MPCHRS; i++) {
		put16(out + OFF_POINTS + i * 2, (u16)MPCHR(i)->numpoints);
	}

	for (i = 0; i < PLAYERCOUNT() && i < MAX_PLAYERS; i++) {
		const s32 t = g_Vars.playerstats[i].tokenheldtime;
		u8 holds = 0;

		put16(out + OFF_HELD + i * 2, (u32)(t < 0 ? 0 : t > 0xffff ? 0xffff : t));

		if (g_Vars.players[i] && g_Vars.players[i]->prop) {
			setCurrentPlayerNum(i);
			holds |= invHasBriefcase() ? HOLD_BRIEFCASE : 0;
			holds |= invHasDataUplink() ? HOLD_UPLINK : 0;
		}

		out[OFF_HOLDS + i] = holds;
	}

	setCurrentPlayerNum(prev);

	switch (g_MpSetup.scenario) {
	case MPSCENARIO_HOLDTHEBRIEFCASE:
		netScenWriteRef(out + HTB_TOKEN, g_ScenarioData.htb.token);
		putf(out + HTB_POS, g_ScenarioData.htb.pos.x);
		putf(out + HTB_POS + 4, g_ScenarioData.htb.pos.y);
		putf(out + HTB_POS + 8, g_ScenarioData.htb.pos.z);
		break;
	case MPSCENARIO_HACKERCENTRAL: {
		struct scenariodata_htm *d = &g_ScenarioData.htm;

		out[HTM_DL] = (u8)(s8)netScenSlotOf(d->dlplayernum);
		out[HTM_DL + 1] = (u8)(s8)netScenSlotOf(d->playernuminrange);
		out[HTM_DL + 2] = (u8)(s8)(d->dlterminalnum >= 0 && d->dlterminalnum < NETSCEN_TERMINALS ? d->dlterminalnum : -1);
		netScenWriteRef(out + HTM_UPLINK, d->uplink);
		netScenWriteRef(out + HTM_TERM, d->terminals[0].prop);
		out[HTM_TEAM] = d->terminals[0].team;

		for (i = 0; i < g_MpNumChrs; i++) {
			const s32 slot = netScenSlotOf(i);
			const s32 t = d->dltime240[i];

			if (slot >= 0) {
				put16(out + HTM_POINTS + slot * 2, (u16)d->numpoints[i]);
				put16(out + HTM_TIME + slot * 2, (u32)(t < 0 ? 0 : t > 0xffff ? 0xffff : t));
			}
		}
		break;
	}
	case MPSCENARIO_POPACAP: {
		struct scenariodata_pac *d = &g_ScenarioData.pac;

		put16(out + PAC_INDEX, (u16)(d->victimindex >= -1 && d->victimindex < g_MpNumChrs ? d->victimindex : -1));
		put16(out + PAC_AGE, d->age240);
		out[PAC_NUM] = (u8)g_MpNumChrs;
		memset(out + PAC_VICTIMS, 0xff, MAX_MPCHRS);

		for (i = 0; i < g_MpNumChrs; i++) {
			const s32 vslot = netScenSlotOf(d->victims[i]);
			const s32 slot = netScenSlotOf(i);

			out[PAC_VICTIMS + i] = vslot >= 0 ? (u8)vslot : 0xff;

			if (slot >= 0) {
				put16(out + PAC_KILLS + slot * 2, (u16)d->killcounts[i]);
				put16(out + PAC_SURV + slot * 2, (u16)d->survivalcounts[i]);
			}
		}
		break;
	}
	case MPSCENARIO_KINGOFTHEHILL: {
		struct scenariodata_koh *d = &g_ScenarioData.koh;

		put16(out + KOH_FIELDS, (u16)d->occupiedteam);
		put16(out + KOH_FIELDS + 2, (u16)d->elapsed240);
		put16(out + KOH_FIELDS + 4, (u16)d->movehill);
		put16(out + KOH_FIELDS + 6, (u16)d->hillindex);
		put16(out + KOH_FIELDS + 8, (u16)d->hillcount);
		put16(out + KOH_ROOMS, (u16)d->hillrooms[0]);
		put16(out + KOH_ROOMS + 2, (u16)d->hillrooms[1]);
		putf(out + KOH_POS, d->hillpos.x);
		putf(out + KOH_POS + 4, d->hillpos.y);
		putf(out + KOH_POS + 8, d->hillpos.z);
		putf(out + KOH_COLOUR, d->colourfracr);
		putf(out + KOH_COLOUR + 4, d->colourfracg);
		putf(out + KOH_COLOUR + 8, d->colourfracb);
		break;
	}
	case MPSCENARIO_CAPTURETHECASE: {
		struct scenariodata_ctc *d = &g_ScenarioData.ctc;

		for (i = 0; i < 4; i++) {
			put16(out + CTC_INDEXES + i * 2, (u16)d->teamindexes[i]);
			put16(out + CTC_COUNTS + i * 2, (u16)d->playercountsperteam[i]);
			put16(out + CTC_BASES + i * 2, (u16)d->baserooms[i]);
			netScenWriteRef(out + CTC_TOKENS + i * 5, d->tokens[i]);
		}
		break;
	}
	default:
		break;
	}
}

/**
 * A seat opened mid-match (its player left, or its hold ran out): the
 * counts the scenario keeps of it by player number go too, so the next
 * player in the seat starts on nothing (Hack That Mac's uploads, Pop a
 * Cap's caps and survivals, the time a briefcase was held)
 */
void netScenHostSeatCleared(s32 slot)
{
	const s32 idx = netScenIndexOf(slot);
	s32 pn;

	if (g_NetMode != NETMODE_SERVER || !g_Vars.normmplayerisrunning || idx < 0 || idx >= MAX_MPCHRS) {
		return;
	}

	g_ScenarioData.htm.numpoints[idx] = 0;
	g_ScenarioData.htm.dltime240[idx] = 0;
	g_ScenarioData.pac.killcounts[idx] = 0;
	g_ScenarioData.pac.survivalcounts[idx] = 0;

	for (pn = 0; pn < PLAYERCOUNT(); pn++) {
		if (g_Vars.playerstats[pn].mpindex == slot) {
			g_Vars.playerstats[pn].tokenheldtime = 0;
		}
	}

	sysLogPrintf(LOG_NOTE, "net: seat %d's scenario counts cleared", slot);
}

static s32 netScenPacOut(s32 index)
{
	const struct scenariodata_pac *d = &g_ScenarioData.pac;

	return index >= 0 && index < g_MpNumChrs && netSessionSeatOutOfPlay(netScenSlotOf(d->victims[index]));
}

/**
 * pacApplyNextVictim's hook (popacap.inc): the turn passes over a seat out
 * of play, whose player is dead and hidden for good, so nobody could cap it
 * and it never earns a point for living (the scenario would stall on it)
 */
s32 netPacVictimIndex(s32 index)
{
	s32 k;

	if (g_NetMode != NETMODE_SERVER || g_MpNumChrs <= 0) {
		return index;
	}

	for (k = 0; k < g_MpNumChrs; k++) {
		const s32 n = (index + k) % g_MpNumChrs;

		if (!netScenPacOut(n)) {
			if (k) {
				s_PacSkips += k;
				sysLogPrintf(LOG_NOTE, "net: Pop a Cap (tick %u): the turn passes over %d seat%s out of play, to slot %d",
						g_NetTick, k, k == 1 ? "" : "s", netScenSlotOf(g_ScenarioData.pac.victims[n]));
			}

			return n;
		}
	}

	return index;
}

void netScenHostPacCheck(void)
{
	struct scenariodata_pac *d = &g_ScenarioData.pac;

	if (g_NetMode != NETMODE_SERVER || !g_Vars.normmplayerisrunning || g_MpSetup.scenario != MPSCENARIO_POPACAP) {
		return;
	}

	if (d->victimindex >= 0 && netScenPacOut(d->victimindex)) {
		sysLogPrintf(LOG_NOTE, "net: Pop a Cap (tick %u): the victim's seat %d went out of play",
				g_NetTick, netScenSlotOf(d->victims[d->victimindex]));
		pacApplyNextVictim();
	}
}

/**
 * netTickEnd, before the snapshots: the tick's block, and the harness's
 * line per remote player
 */
void netScenHostTickEnd(void)
{
	s32 pn;

	if (!g_Vars.normmplayerisrunning) {
		s_HostHave = 0;
		return;
	}

	netScenCapture(s_HostBlock);
	s_HostHave = 1;

	if (s_Log) {
		for (pn = 0; pn < PLAYERCOUNT(); pn++) {
			if (netPlayersHostSlotIsRemote(g_Vars.playerstats[pn].mpindex)) {
				netScenLogLine("S", g_NetTick, pn, 0);
			}
		}
	}
}

const u8 *netScenHostBlock(void)
{
	return s_HostHave ? s_HostBlock : NULL;
}

/**
 * H9: the block at the match's end for MATCH_END, and the harness's line
 */
void netScenHostFinal(u8 *out)
{
	s32 pn;

	netScenCapture(out);

	if (s_Log) {
		for (pn = 0; pn < PLAYERCOUNT(); pn++) {
			if (netPlayersHostSlotIsRemote(g_Vars.playerstats[pn].mpindex)) {
				netScenLogLine("E", g_NetTick, pn, 0);
			}
		}

		fflush(s_Log);
	}
}

/**
 * Host capture (netents.c): whether this object is a scenario's prop, and
 * its descriptor's extra fields if so. A briefcase or the uplink by weapon
 * number, a terminal by its flag: what the object is from its making to its
 * end, so its descriptor kind never changes under the same id.
 */
s32 netScenObjDesc(struct prop *prop, struct netdesc *d)
{
	struct defaultobj *obj;

	if (!g_Vars.normmplayerisrunning || !prop || !(obj = prop->obj)) {
		return 0;
	}

	if (obj->type == OBJTYPE_WEAPON) {
		struct weaponobj *weapon = (struct weaponobj *)obj;

		if (weapon->weaponnum != WEAPON_BRIEFCASE2 && weapon->weaponnum != WEAPON_DATAUPLINK) {
			return 0;
		}

		d->weaponnum = weapon->weaponnum;
		d->team = g_MpSetup.scenario == MPSCENARIO_CAPTURETHECASE ? (u8)(weapon->team & 3) : 0;
	} else if (obj->flags3 & OBJFLAG3_HTMTERMINAL) {
		d->scenflags = NETSCENOBJ_TERMINAL;
	} else {
		return 0;
	}

	d->kind = NETDESC_SCENOBJ;
	d->extrascale = obj->extrascale ? obj->extrascale : 256;

	if (d->extrascale > 4096) {
		d->extrascale = 4096;
	}

	return 1;
}

/*
 * Client
 */

/**
 * setup.c's scenarioInitProps hook (both sides, in a net match's stage):
 * for a scenario with props the stage hash takes the RNG here, as a client
 * draws nothing for props it does not make. 1 on a client: its own scenarioInitProps is skipped and
 * the scenario's state set to what it is before the host's first block.
 */
s32 netScenInitProps(void)
{
	s32 k;
	s32 i;

	if (!netSessionMatchLoading()) {
		return 0;
	}

	// only a scenario with props (Combat has none: its RNGs agree through
	// H6, and the stage hash keeps checking all of it)
	if (g_MpSetup.scenario != MPSCENARIO_COMBAT) {
		netStageHashNoteRng();
	}

	if (g_NetMode != NETMODE_CLIENT || !netClientInMatch()) {
		return 0;
	}

	s_RingHead = s_RingCount = 0;
	s_RingLast = 0;
	s_HaveCur = 0;
	s_HaveFinal = 0;
	memset(s_Cur, 0, sizeof(s_Cur));

	switch (g_MpSetup.scenario) {
	case MPSCENARIO_HOLDTHEBRIEFCASE:
		g_ScenarioData.htb.token = NULL;
		g_ScenarioData.htb.pos.x = g_ScenarioData.htb.pos.y = g_ScenarioData.htb.pos.z = 0;
		break;
	case MPSCENARIO_HACKERCENTRAL:
		g_ScenarioData.htm.uplink = NULL;
		g_ScenarioData.htm.dlplayernum = -1;
		g_ScenarioData.htm.playernuminrange = -1;
		g_ScenarioData.htm.dlterminalnum = -1;

		for (i = 0; i < ARRAYCOUNT(g_ScenarioData.htm.terminals); i++) {
			g_ScenarioData.htm.terminals[i].prop = NULL;
			g_ScenarioData.htm.terminals[i].team = 255;
		}

		for (i = 0; i < MAX_MPCHRS; i++) {
			g_ScenarioData.htm.numpoints[i] = 0;
			g_ScenarioData.htm.dltime240[i] = 0;
		}
		break;
	case MPSCENARIO_POPACAP:
		g_ScenarioData.pac.victimindex = -1;
		g_ScenarioData.pac.age240 = 0;

		for (i = 0; i < MAX_MPCHRS; i++) {
			g_ScenarioData.pac.victims[i] = 0;
			g_ScenarioData.pac.killcounts[i] = 0;
			g_ScenarioData.pac.survivalcounts[i] = 0;
		}
		break;
	case MPSCENARIO_KINGOFTHEHILL:
		g_ScenarioData.koh.hillindex = -1;
		g_ScenarioData.koh.occupiedteam = -1;
		g_ScenarioData.koh.elapsed240 = 0;
		g_ScenarioData.koh.movehill = false;
		g_ScenarioData.koh.hillrooms[0] = -1;
		g_ScenarioData.koh.hillrooms[1] = -1;
		g_ScenarioData.koh.colourfracr = g_ScenarioData.koh.colourfracg = g_ScenarioData.koh.colourfracb = 1;
		break;
	case MPSCENARIO_CAPTURETHECASE:
		for (i = 0; i < 4; i++) {
			g_ScenarioData.ctc.tokens[i] = NULL;
			g_ScenarioData.ctc.baserooms[i] = -1;
			g_ScenarioData.ctc.teamindexes[i] = -1;
			g_ScenarioData.ctc.playercountsperteam[i] = 0;
		}

		// ctcInitProps' teams, which draw nothing: four at most, each chr
		// on its config's
		for (k = 0; k < MAX_MPCHRS; k++) {
			if (mpIsChrSlotOn(k)) {
				struct mpchrconfig *mpchr = MPCHR(k);
				const s32 index = func0f18d0e8(k);

				while (mpchr->team >= scenarioGetMaxTeams()) {
					mpchr->team -= scenarioGetMaxTeams();
				}

				if (index >= 0) {
					struct chrdata *chr = mpGetChrFromPlayerIndex(index);

					if (chr) {
						chr->team = 1 << mpchr->team;
					}
				}
			}
		}
		break;
	default:
		break;
	}

	sysLogPrintf(LOG_NOTE, "net: scenario %d: its props are the host's (C13)", g_MpSetup.scenario);

	return 1;
}

void netScenStageStart(void)
{
	s_Applied = s_Skipped = s_Dropped = s_Refs = s_Unresolved = s_InvSyncs = s_Sounds = s_Held = s_GaveUp = 0;
	s_HostHave = 0;
}

void netScenClientOnSnap(u32 hosttick, const u8 *scen, u32 evseq)
{
	s32 at;

	if (!scen || (s_RingLast && hosttick < s_RingLast)) {
		return;
	}

	if (s_RingCount == NETSCEN_RING) {
		s_RingHead = (s_RingHead + 1) % NETSCEN_RING;
		s_RingCount--;
		s_Dropped++;
	}

	at = (s_RingHead + s_RingCount) % NETSCEN_RING;
	s_Ring[at].tick = hosttick;
	s_Ring[at].evseq = evseq;
	s_Ring[at].held = 0;
	memcpy(s_Ring[at].data, scen, NETSCEN_SIZE);
	s_RingCount++;
	s_RingLast = hosttick + 1;
}

static s32 netScenRoomOk(s32 room)
{
	return room >= -1 && room < g_Vars.roomcount;
}

// What a block may not say: a slot or index past this match, a room past the stage's
static s32 netScenBlockOk(const u8 *b)
{
	s32 i;

	if (b[OFF_SCEN] != (u8)g_MpSetup.scenario) {
		return 0;
	}

	switch (g_MpSetup.scenario) {
	case MPSCENARIO_HACKERCENTRAL:
		if ((s8)b[HTM_DL] >= MAX_MPCHRS || (s8)b[HTM_DL + 1] >= MAX_MPCHRS
				|| (s8)b[HTM_DL + 2] < -1 || (s8)b[HTM_DL + 2] >= NETSCEN_TERMINALS) {
			return 0;
		}
		break;
	case MPSCENARIO_POPACAP:
		if (gets16(b + PAC_INDEX) < -1 || gets16(b + PAC_INDEX) >= g_MpNumChrs || b[PAC_NUM] != g_MpNumChrs) {
			return 0;
		}
		break;
	case MPSCENARIO_KINGOFTHEHILL:
		if (!netScenRoomOk(gets16(b + KOH_ROOMS)) || gets16(b + KOH_FIELDS) < -1 || gets16(b + KOH_FIELDS) >= MAX_TEAMS) {
			return 0;
		}
		break;
	case MPSCENARIO_CAPTURETHECASE:
		// a team index picks the spawn pads a respawn here reads (ctcChooseSpawnLocation)
		for (i = 0; i < 4; i++) {
			if (!netScenRoomOk(gets16(b + CTC_BASES + i * 2))
					|| gets16(b + CTC_INDEXES + i * 2) < -1 || gets16(b + CTC_INDEXES + i * 2) > 3) {
				return 0;
			}
		}
		break;
	}

	return 1;
}

// The scenario's pointers from the block: entities made since are found
static void netScenRefresh(void)
{
	s32 i;

	if (!s_HaveCur) {
		return;
	}

	switch (g_MpSetup.scenario) {
	case MPSCENARIO_HOLDTHEBRIEFCASE:
		g_ScenarioData.htb.token = netScenResolve(s_Cur + HTB_TOKEN);
		break;
	case MPSCENARIO_HACKERCENTRAL:
		g_ScenarioData.htm.uplink = netScenResolve(s_Cur + HTM_UPLINK);
		g_ScenarioData.htm.terminals[0].prop = netScenResolve(s_Cur + HTM_TERM);
		break;
	case MPSCENARIO_CAPTURETHECASE:
		for (i = 0; i < 4; i++) {
			struct prop *prop = netScenResolve(s_Cur + CTC_TOKENS + i * 5);

			g_ScenarioData.ctc.tokens[i] = prop;

			// the briefcase's colour is its team's (ctcHighlightProp)
			if (prop && prop->type == PROPTYPE_WEAPON && prop->weapon) {
				prop->weapon->team = (u8)i;
			}
		}
		break;
	}
}

static s32 netScenSumPoints(const u8 *b, s32 from, s32 to)
{
	s32 sum = 0;
	s32 i;

	for (i = from; i < to && i < MAX_MPCHRS; i++) {
		sum += gets16(b + OFF_POINTS + i * 2);
	}

	return sum;
}

// The sounds the host's scenario code made that the client's never runs to make
static void netScenSounds(const u8 *prev, const u8 *cur)
{
	const s32 localslot = netScenSlotOf(g_NetLocalSlot);

	switch (g_MpSetup.scenario) {
	case MPSCENARIO_HOLDTHEBRIEFCASE:
	case MPSCENARIO_KINGOFTHEHILL:
		// every point is heard by everyone (htbTickChr, kohTick)
		if (netScenSumPoints(cur, 0, MAX_MPCHRS) > netScenSumPoints(prev, 0, MAX_MPCHRS)) {
			sndStart(var80095200, SFX_MP_SCOREPOINT, NULL, -1, -1, -1, -1, -1);
			s_Sounds++;
		}

		if (g_MpSetup.scenario == MPSCENARIO_KINGOFTHEHILL) {
			const s16 was = gets16(prev + KOH_FIELDS);
			const s16 now = gets16(cur + KOH_FIELDS);

			if (now != was && now >= 0) {
				sndStart(var80095200, SFX_MP_HILLENTERED, NULL, -1, -1, -1, -1, -1);
				s_Sounds++;
			}
		}
		break;
	case MPSCENARIO_CAPTURETHECASE:
		// a player's capture (scenarioPickUpBriefcase)
		if (netScenSumPoints(cur, 0, MAX_PLAYERS) > netScenSumPoints(prev, 0, MAX_PLAYERS)) {
			sndStart(var80095200, SFX_MP_SCOREPOINT, NULL, -1, -1, -1, -1, -1);
			s_Sounds++;
		}
		break;
	case MPSCENARIO_HACKERCENTRAL: {
		// this machine's player's download (htmTickChr)
		const s32 wasdl = localslot >= 0 && (s8)prev[HTM_DL] == localslot && (s8)prev[HTM_DL + 2] >= 0;
		const s32 nowdl = localslot >= 0 && (s8)cur[HTM_DL] == localslot && (s8)cur[HTM_DL + 2] >= 0;
		struct prop *terminal = g_ScenarioData.htm.terminals[0].prop;

		if (!terminal) {
			break;
		}

		if (nowdl && !wasdl) {
			psCreate(NULL, terminal, SFX_01BF, -1, -1, PSFLAG_REPEATING, PSFLAG2_MPPAUSABLE, PSTYPE_NONE, NULL, -1, NULL, -1, -1, -1, -1);
			s_Sounds++;
		} else if (wasdl && !nowdl) {
			const s32 scored = gets16(cur + HTM_POINTS + localslot * 2) > gets16(prev + HTM_POINTS + localslot * 2);

			psStopSound(terminal, PSTYPE_GENERAL, 0xffff);
			snd00010718(NULL, 0, AL_VOL_FULL, AL_PAN_CENTER, scored ? SFX_01C1 : SFX_01CC, 1, 1, -1, 1);
			s_Sounds++;
		}
		break;
	}
	default:
		break;
	}
}

/**
 * A hudmsg event shown (netevents.c): Hacker Central's buzz when a player
 * uses the terminal without the uplink in hand (htmTickChr), which changes
 * nothing in the block for netScenSounds to hear
 */
void netScenClientHudmsg(const char *text)
{
	if (NET_CLIENT && g_Vars.normmplayerisrunning && g_MpSetup.scenario == MPSCENARIO_HACKERCENTRAL
			&& text && strcmp(text, langGet(L_MPWEAPONS_019)) == 0) { // "You need to use the Data Uplink."
		snd00010718(NULL, 0, AL_VOL_FULL, AL_PAN_CENTER, SFX_01CC, 1, 1, -1, 1);
		s_Sounds++;
	}
}

// The local player's token in its inventory as the host has it (a briefcase
// slows the walk the client predicts, and the HUD's timer shows on it)
static void netScenLocalInventory(const u8 *b)
{
	struct player *p;
	s32 prev;
	s32 i;
	u8 holds;

	if (g_NetLocalSlot < 0 || g_NetLocalSlot >= PLAYERCOUNT() || g_NetLocalSlot >= MAX_PLAYERS || netSessionSpectating()) {
		return;
	}

	p = g_Vars.players[g_NetLocalSlot];

	if (!p || !p->prop || p->isdead) {
		return;
	}

	holds = b[OFF_HOLDS + g_NetLocalSlot];
	prev = g_Vars.currentplayernum;
	setCurrentPlayerNum(g_NetLocalSlot);

	for (i = 0; i < 2; i++) {
		const s32 weaponnum = i == 0 ? WEAPON_BRIEFCASE2 : WEAPON_DATAUPLINK;
		const s32 want = (holds & (i == 0 ? HOLD_BRIEFCASE : HOLD_UPLINK)) != 0;
		const s32 has = i == 0 ? invHasBriefcase() : invHasDataUplink();

		// only where the newest local-player block says the same, so the
		// two never take turns (netLpInventory has the rest, later), and
		// never out of the hands, as netLpInventory never takes it
		if (want == has || netEntsClientLpHolds(weaponnum) != want) {
			continue;
		}

		if (want) {
			invGiveSingleWeapon(weaponnum);
			s_InvSyncs++;
		} else if (p->gunctrl.weaponnum != weaponnum) {
			invRemoveItemByNum(weaponnum);
			s_InvSyncs++;
		}
	}

	setCurrentPlayerNum(prev);
}

static void netScenApply(const u8 *b, u32 tick, const char *tag)
{
	static u8 prev[NETSCEN_SIZE];
	s32 i;

	if (!netScenBlockOk(b)) {
		s_Skipped++;
		return;
	}

	for (i = 0; i < MAX_MPCHRS; i++) {
		MPCHR(i)->numpoints = gets16(b + OFF_POINTS + i * 2);
	}

	for (i = 0; i < PLAYERCOUNT() && i < MAX_PLAYERS; i++) {
		g_Vars.playerstats[i].tokenheldtime = get16(b + OFF_HELD + i * 2);
	}

	switch (g_MpSetup.scenario) {
	case MPSCENARIO_HOLDTHEBRIEFCASE:
		g_ScenarioData.htb.pos.x = getf(b + HTB_POS);
		g_ScenarioData.htb.pos.y = getf(b + HTB_POS + 4);
		g_ScenarioData.htb.pos.z = getf(b + HTB_POS + 8);
		break;
	case MPSCENARIO_HACKERCENTRAL: {
		struct scenariodata_htm *d = &g_ScenarioData.htm;

		d->dlplayernum = netScenIndexOf((s8)b[HTM_DL]);
		d->playernuminrange = netScenIndexOf((s8)b[HTM_DL + 1]);
		d->dlterminalnum = (s8)b[HTM_DL + 2];
		d->terminals[0].team = b[HTM_TEAM];

		for (i = 0; i < g_MpNumChrs; i++) {
			const s32 slot = netScenSlotOf(i);

			if (slot >= 0) {
				d->numpoints[i] = gets16(b + HTM_POINTS + slot * 2);
				d->dltime240[i] = get16(b + HTM_TIME + slot * 2);
			}
		}
		break;
	}
	case MPSCENARIO_POPACAP: {
		struct scenariodata_pac *d = &g_ScenarioData.pac;

		d->victimindex = gets16(b + PAC_INDEX);
		d->age240 = get16(b + PAC_AGE);

		for (i = 0; i < g_MpNumChrs; i++) {
			const s32 k = b[PAC_VICTIMS + i] == 0xff ? -1 : netScenIndexOf(b[PAC_VICTIMS + i]);
			const s32 slot = netScenSlotOf(i);

			// never -1: the HUD and the highlight index g_MpAllChrPtrs with it
			d->victims[i] = k >= 0 && k < g_MpNumChrs ? k : 0;

			if (slot >= 0) {
				d->killcounts[i] = gets16(b + PAC_KILLS + slot * 2);
				d->survivalcounts[i] = gets16(b + PAC_SURV + slot * 2);
			}
		}
		break;
	}
	case MPSCENARIO_KINGOFTHEHILL: {
		struct scenariodata_koh *d = &g_ScenarioData.koh;
		const s16 room = gets16(b + KOH_ROOMS);

		// the hill's room lit as kohTick lights it
		if (room != d->hillrooms[0]) {
			if (d->hillrooms[0] >= 0 && d->hillrooms[0] < g_Vars.roomcount) {
				roomSetLightOp(d->hillrooms[0], LIGHTOP_NONE, 0, 0, 0);
			}

			if (room >= 0) {
				roomSetLightOp(room, LIGHTOP_HIGHLIGHT, 0, 0, 0);
			}
		}

		d->occupiedteam = gets16(b + KOH_FIELDS);
		d->elapsed240 = gets16(b + KOH_FIELDS + 2);
		d->movehill = gets16(b + KOH_FIELDS + 4);
		d->hillindex = gets16(b + KOH_FIELDS + 6);
		d->hillcount = gets16(b + KOH_FIELDS + 8);
		d->hillrooms[0] = room;
		d->hillrooms[1] = -1;
		d->hillpos.x = getf(b + KOH_POS);
		d->hillpos.y = getf(b + KOH_POS + 4);
		d->hillpos.z = getf(b + KOH_POS + 8);
		d->colourfracr = getf(b + KOH_COLOUR);
		d->colourfracg = getf(b + KOH_COLOUR + 4);
		d->colourfracb = getf(b + KOH_COLOUR + 8);
		break;
	}
	case MPSCENARIO_CAPTURETHECASE: {
		struct scenariodata_ctc *d = &g_ScenarioData.ctc;

		for (i = 0; i < 4; i++) {
			const s16 base = gets16(b + CTC_BASES + i * 2);
			const s16 count = gets16(b + CTC_COUNTS + i * 2);

			d->teamindexes[i] = gets16(b + CTC_INDEXES + i * 2);
			d->playercountsperteam[i] = count;

			// the bases lit as ctcInitProps lights them
			if (base != d->baserooms[i]) {
				d->baserooms[i] = base;

				if (base >= 0 && count) {
					roomSetLightOp(base, LIGHTOP_HIGHLIGHT, 0, 0, 0);
				}
			}
		}
		break;
	}
	default:
		break;
	}

	netScenLocalInventory(b);

	if (s_HaveCur) {
		memcpy(prev, s_Cur, NETSCEN_SIZE);
	}

	memcpy(s_Cur, b, NETSCEN_SIZE);
	netScenRefresh();

	if (s_HaveCur) {
		netScenSounds(prev, b);
	}

	s_HaveCur = 1;
	s_CurTick = tick;
	s_Applied++;

	netScenLogLine(tag, tick, g_NetLocalSlot, 1);
}

static void netScenApplyWhile(f64 upto, s32 strict)
{
	while (s_RingCount) {
		const u32 tick = s_Ring[s_RingHead].tick;
		static u8 b[NETSCEN_SIZE];

		if (strict ? (f64)tick >= upto : (f64)tick > upto) {
			break;
		}

		// its events not all in yet: it waits for the one sent again (an
		// event of a later tick being here says they are: the channel is
		// ordered, so the strict call never waits)
		if (!strict && (s32)(netEventsClientSeq() - s_Ring[s_RingHead].evseq) < 0) {
			if (upto < (f64)tick + NETSCEN_HOLDMAX) {
				if (!s_Ring[s_RingHead].held) {
					s_Ring[s_RingHead].held = 1;
					s_Held++;
				}

				break;
			}

			if (s_GaveUp++ < 4) {
				sysLogPrintf(LOG_WARNING, "net: scenario block of host tick %u applied without its events (%u of %u in after %d ticks)",
						tick, netEventsClientSeq(), s_Ring[s_RingHead].evseq, NETSCEN_HOLDMAX);
			}
		}

		memcpy(b, s_Ring[s_RingHead].data, NETSCEN_SIZE);
		s_RingHead = (s_RingHead + 1) % NETSCEN_RING;
		s_RingCount--;
		netScenApply(b, tick, "S");
	}
}

/**
 * netEventsClientDrain, before an event of host tick `tick` is applied: the
 * blocks from before it (the host captured each after its tick's events)
 */
void netScenClientBeforeEvent(u32 tick)
{
	if (NET_CLIENT) {
		netScenApplyWhile((f64)tick, 1);
	}
}

// ... and after the drain: every block the render clock has reached
void netScenClientUpTo(f64 rt)
{
	if (!NET_CLIENT) {
		return;
	}

	netScenApplyWhile(rt, 0);
	netScenRefresh();
}

// MATCH_END's block (netsession.c): applied at H10 with the host's table
void netScenClientFinal(const u8 *scen)
{
	memcpy(s_Final, scen, NETSCEN_SIZE);
	s_HaveFinal = 1;
}

void netScenClientApplyFinal(void)
{
	if (s_HaveFinal) {
		s_RingCount = 0;
		netScenApply(s_Final, 0xffffffff, "F");
		s_HaveFinal = 0;
	}

	if (s_Log) {
		netScenLogLine("E", g_NetTick, g_NetLocalSlot, 1);
		fflush(s_Log);
	}

	sysLogPrintf(LOG_NOTE, "net: scenario client: %u blocks applied (last at host tick %u), %u skipped, %u dropped, %u held for their events (%u given up), %u of %u entity references unresolved, %u token inventory syncs, %u sounds",
			s_Applied, s_CurTick, s_Skipped, s_Dropped, s_Held, s_GaveUp, s_Unresolved, s_Refs, s_InvSyncs, s_Sounds);
}

void netScenMatchStopped(void)
{
	if (g_NetMode == NETMODE_CLIENT && s_Applied) {
		sysLogPrintf(LOG_NOTE, "net: scenario client at the match's end: %u blocks applied, %u skipped, %u of %u references unresolved",
				s_Applied, s_Skipped, s_Unresolved, s_Refs);
	}

	s_RingCount = 0;
	s_RingLast = 0;
	s_HaveCur = 0;
	s_HaveFinal = 0;
	s_HostHave = 0;

	if (s_Log) {
		fflush(s_Log);
	}
}

// --net-scen-log FILE: the harness's lines (S tick player state, E at the end)
void netScenArgs(void)
{
	const char *path = sysArgGetString("--net-scen-log");

	if (path && *path && !s_Log) {
		s_Log = fopen(path, "w");

		if (s_Log) {
			setvbuf(s_Log, NULL, _IOLBF, 0);
		}
	}
}
