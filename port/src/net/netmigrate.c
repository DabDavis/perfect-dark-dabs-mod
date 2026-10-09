#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "lib/main.h"
#include "game/bondgun.h"
#include "game/chraction.h"
#include "game/inv.h"
#include "game/lv.h"
#include "game/menu.h"
#include "game/pdmode.h"
#include "game/player.h"
#include "game/title.h"
#include "game/mplayer/mplayer.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "net/nettransport.h"
#include "netint.h"

/**
 * Host migration (2026-10-08; CLAUDE-notes/netplay.md "Host migration"): a
 * lobby room outlives its host. pdlobbyd hands the room to the member best
 * placed to host it (tools/pdlobbyd/README.md "Host migration"); this file is
 * what the game does about it.
 *
 * Every client of a lobby room keeps what it knows of the room's match as
 * its stage stops, and, when the host went away mid-match, of the match
 * itself:
 *
 *  - the match's RULES (netRulesKeep), the room's setup from then on on
 *    whichever machine hosts it next, and the stage it was played on here;
 *  - when the host went mid-match: the level clock, the kill table, the
 *    scenario's per-player counts (netScenKeep), who sat in which seat, and
 *    this machine's own player as the host last had it (the local-player
 *    block: where it stood, its health and shield, its guns and ammo).
 *
 * The member the room goes to takes its kept setup as its own (netRulesAdopt,
 * its player in slot 0 and the old host's in the seat it had), listens, and
 * relaunches the room. A Combat Simulator match is then started again on the
 * same stage with the same rules, nothing rerolled, as a resume: its seats
 * the accounts' seats (0 and the new host's exchanged), the ones whose player
 * has not reconnected held for them as a drop is, the clock (GO's stagetime)
 * and the kill table where they were, and every player that sends its
 * record (RESUME) given a new life where it stood with what it carried. A
 * co-op mission is started again from its beginning (its guards, doors and
 * objectives were the old host's alone).
 */

#define MIGRATE_KEEP_MS 180000 // what was kept from a match goes stale this long after

static struct {
	s32 valid;          // a lobby room's last match is kept
	char room[9];
	u64 at;
	s32 stage;          // the stage as it resolved here
	s32 myslot;         // this machine's seat in it (-1 a spectator)
	s32 rules;          // its RULES are kept (netrules.c)
	s32 resume;         // the host went mid-match: the match is carried on
	u32 matchid;        // the old host's id of it
	s32 stagetime60;
	u8 seat[MAX_PLAYERS];
	char account[MAX_PLAYERS][NET_MAXNAME + 1];
	s16 numdeaths[MAX_MPCHRS];
	s16 numpoints[MAX_MPCHRS];
	s16 killcounts[MAX_MPCHRS][MAX_MPCHRS];
	s32 havescen;
	u8 scen[NETSCEN_SIZE];
	s32 havelp;
	u8 lp[NETLP_SIZE];
	char prevhost[NET_MAXNAME + 1];
} s_Kept;

// The host's resumed match
static struct {
	s32 pending;        // the room's next match is a resume (until H1)
	s32 on;             // the match running is one
	s32 swap;           // the seat exchanged with 0: the new host's in the old match
	u32 oldmatch;
	s32 stagetime60;
	s32 applied;        // the kill table and counts are in (the first tick)
	char account[MAX_PLAYERS][NET_MAXNAME + 1]; // by the resumed match's seat
	char prevhost[NET_MAXNAME + 1];
	s32 have[MAX_PLAYERS];   // a record came for the seat (RESUME, or the host's own)
	s32 used[MAX_PLAYERS];   // ... and was given its life
	u8 lp[MAX_PLAYERS][NETLP_SIZE];
	u32 landing;        // players (by number) whose new life is the record's
	u32 asked;          // players whose new life was asked for
	u32 lives;
	u32 records;
	s16 numdeaths[MAX_MPCHRS];
	s16 numpoints[MAX_MPCHRS];
	s16 killcounts[MAX_MPCHRS][MAX_MPCHRS];
	s32 havescen;
	u8 scen[NETSCEN_SIZE];
} s_Res;

static u64 netMigrateNow(void)
{
	return sysGetMicroseconds() / 1000;
}

static s32 netMigrateSwapSlot(s32 slot, s32 swap)
{
	return slot == 0 ? swap : slot == swap ? 0 : slot;
}

static void netMigrateStale(void)
{
	if (s_Kept.valid && netMigrateNow() - s_Kept.at > MIGRATE_KEEP_MS) {
		sysLogPrintf(LOG_NOTE, "net: migrate: what was kept of room %s's last match is stale; forgotten", s_Kept.room);
		netMigrateForget();
	}
}

void netMigrateForget(void)
{
	memset(&s_Kept, 0, sizeof(s_Kept));
	netRulesKeptForget();
}

/**
 * A client of lobby room `room` at its match's end (H12, the match over) or
 * as its host went away mid-match (`resume`): the setup and, for a resume,
 * the match's state. `seats` and the players' names are the last ROSTER's.
 */
void netMigrateKeep(const char *room, u32 matchid, s32 stage, s32 myslot, s32 resume, const u8 *seats, s32 pastsnap)
{
	s32 i;
	s32 j;

	memset(&s_Kept, 0, sizeof(s_Kept));
	s_Kept.rules = netRulesKeep();

	if (!s_Kept.rules || !room || !room[0] || stage < 0) {
		netMigrateForget();
		return;
	}

	s_Kept.valid = 1;
	snprintf(s_Kept.room, sizeof(s_Kept.room), "%s", room);
	s_Kept.at = netMigrateNow();
	s_Kept.stage = stage;
	s_Kept.myslot = myslot;
	s_Kept.matchid = matchid;

	for (i = 0; i < MAX_PLAYERS; i++) {
		s_Kept.seat[i] = seats ? seats[i] : 0;

		// ROSTER's seat states (netproto.h): 0 not in the match, 3 open;
		// the host's, a client's and a held one are an account's
		if (s_Kept.seat[i] != 0 && s_Kept.seat[i] != 3) {
			snprintf(s_Kept.account[i], sizeof(s_Kept.account[i]), "%s", g_PlayerConfigsArray[i].base.name);
			s_Kept.account[i][strcspn(s_Kept.account[i], "\n")] = '\0';
		}
	}

	snprintf(s_Kept.prevhost, sizeof(s_Kept.prevhost), "%s", s_Kept.account[0]);

	// a co-op mission is started again, not carried on: its state was the
	// host's alone
	if (resume && !netRulesKeptCoop()) {
		s_Kept.resume = 1;

		// the level clock as of the host's last snapshot: a client runs on
		// some ticks after its host has gone (a crash is noticed seconds
		// later), each a 60th of level time
		s_Kept.stagetime60 = lvGetStageTime60() - (pastsnap > 0 ? pastsnap : 0);

		if (s_Kept.stagetime60 < 0) {
			s_Kept.stagetime60 = 0;
		}

		for (i = 0; i < MAX_MPCHRS; i++) {
			struct mpchrconfig *mpchr = MPCHR(i);

			s_Kept.numdeaths[i] = mpchr->numdeaths;
			s_Kept.numpoints[i] = mpchr->numpoints;

			for (j = 0; j < MAX_MPCHRS; j++) {
				s_Kept.killcounts[i][j] = mpchr->killcounts[j];
			}
		}

		s_Kept.havescen = netScenKeep(s_Kept.scen);
		s_Kept.havelp = myslot >= 0 && netEntsClientLastLp(s_Kept.lp);
	} else if (resume) {
		s_Kept.resume = 1; // the mission, from its start
	}

	sysLogPrintf(LOG_NOTE, "net: migrate: kept room %s's %s (match %u on stage 0x%02x, seat %d%s%s)", room,
			!resume ? "setup" : netRulesKeptCoop() ? "mission, to start again" : "match, to carry on",
			matchid, stage, myslot, s_Kept.havelp ? ", this player's state" : "", s_Kept.havescen ? ", the scenario's counts" : "");
}

// Kept for this room, and still fresh
s32 netMigrateKeptFor(const char *room)
{
	netMigrateStale();

	return s_Kept.valid && room && strcmp(s_Kept.room, room) == 0;
}

// A match to carry on (or a mission to start again) is kept for this room
s32 netMigrateResumeKept(const char *room)
{
	return netMigrateKeptFor(room) && s_Kept.resume;
}

const char *netMigratePrevHost(void)
{
	return s_Kept.prevhost;
}

/**
 * This machine has just taken room `room` over (netlobby.c): the kept setup
 * becomes its own, played with the kept match's mod and ROM hack mode. 0
 * when it cannot be (the mod would not switch): the room is then declined.
 * With nothing kept, the room plays this machine's own setup.
 */
s32 netMigrateAdopt(const char *room, s32 campaign, char *text, s32 textsize)
{
	const struct netcontentneed *content;
	const struct netcooprules *coop;
	s32 followed;

	text[0] = '\0';

	if (!netMigrateKeptFor(room)) {
		sysLogPrintf(LOG_NOTE, "net: migrate: nothing kept of room %s; its setup is this machine's own", room);
		return 1;
	}

	content = netRulesKeptContent();
	coop = netRulesKeptCoop();

	if (content) {
		followed = netContentFollow(content, text, textsize);

		if (followed != NETCONTENT_OK && followed != NETCONTENT_SWAPPED) {
			sysLogPrintf(LOG_WARNING, "net: migrate: the room's mod cannot be played here: %s", text);
			return 0;
		}
	}

	if (coop) {
		g_NetCoopSetup.on = 1;
		g_NetCoopSetup.campaign = campaign;
		g_NetCoopSetup.stageindex = coop->stageindex;
		g_NetCoopSetup.difficulty = coop->difficulty;
		g_NetCoopSetup.radar = coop->radar;
		g_NetCoopSetup.friendlyfire = coop->friendlyfire;
		snprintf(g_NetCoopSetup.game, sizeof(g_NetCoopSetup.game), "%s", coop->game);
	} else {
		g_NetCoopSetup.on = 0;
		g_NetCoopSetup.campaign = 0;
	}

	netRulesAdopt(s_Kept.myslot >= 0 ? s_Kept.myslot : 0);

	if (!coop) {
		g_MpSetup.stagenum = s_Kept.stage;
	}

	s_Kept.rules = 0;
	memset(&s_Res, 0, sizeof(s_Res));
	s_Res.pending = s_Kept.resume;

	sysLogPrintf(LOG_NOTE, "net: migrate: room %s's setup is this host's now%s", room,
			!s_Kept.resume ? "" : coop ? "; its mission starts again at the relaunch" : "; its match carries on at the relaunch");

	return 1;
}

// This machine stops hosting (left, declined): its own setup and mod back
void netMigrateAdoptEnd(void)
{
	memset(&s_Res, 0, sizeof(s_Res));
	netRulesAdoptEnd();
	netContentRestore(0);
}

// The host: the room's next match is the kept one carried on
s32 netMigrateResumePending(void)
{
	return s_Res.pending && s_Kept.valid && s_Kept.resume;
}

// The host: the match running (or starting) is a resumed one
s32 netMigrateResuming(void)
{
	return s_Res.on;
}

/**
 * The host, a CONNECT before the resumed match: the seat the account had
 * (0 and the new host's exchanged), or -1
 */
s32 netMigrateSeatOf(const char *account)
{
	s32 i;

	if (!netMigrateResumePending() || !account || !account[0]) {
		return -1;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (s_Kept.account[i][0] && strcasecmp(s_Kept.account[i], account) == 0) {
			return netMigrateSwapSlot(i, s_Kept.myslot >= 0 ? s_Kept.myslot : 0);
		}
	}

	return -1;
}

// ... whether seat `slot` is kept for another account than `account`
s32 netMigrateSeatReserved(s32 slot, const char *account)
{
	s32 i;

	if (!netMigrateResumePending() || slot < 0 || slot >= MAX_PLAYERS) {
		return 0;
	}

	i = netMigrateSwapSlot(slot, s_Kept.myslot >= 0 ? s_Kept.myslot : 0);

	return s_Kept.account[i][0] && (!account || strcasecmp(s_Kept.account[i], account) != 0);
}

// The host at H1 of the resumed match: seat `slot`'s account, if one sat there
const char *netMigrateSeatAccount(s32 slot)
{
	if (!s_Res.on || slot < 0 || slot >= MAX_PLAYERS || !s_Res.account[slot][0]) {
		return NULL;
	}

	return s_Res.account[slot];
}

/**
 * The host's start of the room's next match when it is the kept one (a
 * lobby launch, netSessionLobbyStartMatch): 1 started. A match: its stage
 * and rules as they were, nothing rerolled (mpStartMatch's tail). A mission:
 * from its start.
 */
s32 netMigrateHostStart(void)
{
	s32 numplayers = 0;
	s32 i;
	s32 j;

	if (!netMigrateResumePending() || g_NetMode != NETMODE_SERVER) {
		return 0;
	}

	s_Res.pending = 0;

	if (g_NetCoopSetup.on) {
		sysLogPrintf(LOG_NOTE, "net: migrate: starting mission %d of %s again for the room", g_NetCoopSetup.stageindex,
				netCoopGameName(g_NetCoopSetup.game));

		if (g_NetCoopSetup.campaign) {
			netCoopCampaignResume(g_NetCoopSetup.game, g_NetCoopSetup.radar, g_NetCoopSetup.friendlyfire);
		}

		netCoopHostStart(g_NetCoopSetup.game, g_NetCoopSetup.stageindex, g_NetCoopSetup.difficulty, g_NetCoopSetup.radar,
				g_NetCoopSetup.friendlyfire);
		menuStop();
		s_Kept.resume = 0;
		return 1;
	}

	// (records that came before the start are kept: a member's RESUME
	// follows its ACCEPT, and the start follows the last connect)
	memset(s_Res.account, 0, sizeof(s_Res.account));
	memset(s_Res.used, 0, sizeof(s_Res.used));
	s_Res.applied = 0;
	s_Res.landing = 0;
	s_Res.asked = 0;
	s_Res.lives = 0;
	s_Res.on = 1;
	s_Res.swap = s_Kept.myslot >= 0 ? s_Kept.myslot : 0;
	s_Res.oldmatch = s_Kept.matchid;
	s_Res.stagetime60 = s_Kept.stagetime60;
	snprintf(s_Res.prevhost, sizeof(s_Res.prevhost), "%s", s_Kept.prevhost);

	for (i = 0; i < MAX_PLAYERS; i++) {
		const s32 to = netMigrateSwapSlot(i, s_Res.swap);

		snprintf(s_Res.account[to], sizeof(s_Res.account[to]), "%s", s_Kept.account[i]);
	}

	// the kill table, its rows and columns 0 and the swap exchanged
	for (i = 0; i < MAX_MPCHRS; i++) {
		const s32 ti = i < MAX_PLAYERS ? netMigrateSwapSlot(i, s_Res.swap) : i;

		s_Res.numdeaths[ti] = s_Kept.numdeaths[i];
		s_Res.numpoints[ti] = s_Kept.numpoints[i];

		for (j = 0; j < MAX_MPCHRS; j++) {
			const s32 tj = j < MAX_PLAYERS ? netMigrateSwapSlot(j, s_Res.swap) : j;

			s_Res.killcounts[ti][tj] = s_Kept.killcounts[i][j];
		}
	}

	s_Res.havescen = s_Kept.havescen;
	memcpy(s_Res.scen, s_Kept.scen, sizeof(s_Res.scen));

	// this machine's own player: its record is its own kept block
	if (s_Kept.havelp) {
		struct netlpstate lp;

		memcpy(s_Res.lp[0], s_Kept.lp, NETLP_SIZE);
		s_Res.records += !s_Res.have[0];
		s_Res.have[0] = 1;
		netLpUnpack(s_Kept.lp, &lp);
		sysLogPrintf(LOG_NOTE, "net: migrate: seat 0's record (this machine's): at %.0f %.0f %.0f, health %.2f shield %.2f, weapon %d%s",
				lp.pos[0], lp.pos[1], lp.pos[2], lp.health, lp.shield, lp.weaponnum, (lp.flags & NETLP_DEAD) ? " (dead)" : "");
	}

	s_Kept.resume = 0;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (mpIsHumanSlotOn(i)) {
			numplayers++;
		}
	}

	sysLogPrintf(LOG_NOTE, "net: migrate: carrying room %s's match on: stage 0x%02x, level time %d, %s's seat %d is 0 here",
			s_Kept.room, s_Kept.stage, s_Res.stagetime60, s_Res.prevhost[0] ? s_Res.prevhost : "the old host", s_Res.swap);

	g_MpSetup.chrslots |= 1;
	numplayers = netHostMatchStarting(s_Kept.stage, numplayers);
	titleSetNextStage(s_Kept.stage);
	mainChangeToStage(s_Kept.stage);
	setNumPlayers(numplayers);
	titleSetNextMode(TITLEMODE_SKIP);
	g_Vars.perfectbuddynum = 1;
	menuStop();

	return 1;
}

// The host: GO's clock for the match (the resumed match's level time, else 0)
s32 netMigrateGoTime(void)
{
	return s_Res.on ? s_Res.stagetime60 : 0;
}

/*
 * The players' records (RESUME, protocol 21)
 */

// A client accepted by the room's (new) host: its kept player, if the match is carried on
void netMigrateSendRecord(const char *room)
{
	u8 buf[NETLP_SIZE + 16];
	struct netbuf b;

	if (!netMigrateKeptFor(room) || !s_Kept.havelp || s_Kept.myslot < 0) {
		return;
	}

	netBufInitWrite(&b, buf, sizeof(buf));
	netBufWriteU8(&b, NETMSG_RESUME);
	netBufWriteU32(&b, s_Kept.matchid);
	netBufWriteU16(&b, NETLP_SIZE);
	netBufWriteBytes(&b, s_Kept.lp, NETLP_SIZE);

	if (netBufOk(&b) && netSessionSendServer(NET_CHAN_RELIABLE, buf, netBufLen(&b), NET_SEND_RELIABLE) == 0) {
		sysLogPrintf(LOG_NOTE, "net: migrate: this player's state sent to the new host (match %u)", s_Kept.matchid);
		s_Kept.havelp = 0;
	}
}

static s32 netMigrateRecordSane(const struct netlpstate *lp)
{
	s32 i;

	for (i = 0; i < 3; i++) {
		if (!isfinite(lp->pos[i]) || fabsf(lp->pos[i]) > 1000000.f) {
			return 0;
		}
	}

	// a dead player's: a life of its own follows, nothing of it is used
	if (lp->flags & NETLP_DEAD) {
		return 1;
	}

	return isfinite(lp->theta) && isfinite(lp->health) && isfinite(lp->shield)
		&& fabsf(lp->theta) < 100000.f && lp->health > 0 && lp->health < 100.f && lp->shield >= 0 && lp->shield < 100.f
		&& lp->weaponnum >= 0 && lp->weaponnum < NUM_WEAPONS;
}

/**
 * The host: a client's RESUME (its seat `slot`; spectators have none). Kept
 * for the seat until the player's next life, which it is given.
 */
s32 netMigrateOnRecord(s32 slot, struct netbuf *b)
{
	struct netlpstate lp;
	u8 data[NETLP_SIZE];
	const u32 match = netBufReadU32(b);
	const s32 len = netBufReadU16(b);

	if (len != NETLP_SIZE) {
		return 0;
	}

	netBufReadBytes(b, data, NETLP_SIZE);

	if (!netBufOk(b) || netBufRemaining(b) != 0) {
		return 0;
	}

	if (!s_Res.on && !netMigrateResumePending()) {
		return 1;
	}

	if (slot < 0 || slot >= MAX_PLAYERS || match != (s_Res.on ? s_Res.oldmatch : s_Kept.matchid)) {
		sysLogPrintf(LOG_NOTE, "net: migrate: a record for match %u (seat %d) is not this room's; ignored", match, slot);
		return 1;
	}

	netLpUnpack(data, &lp);

	if (!netMigrateRecordSane(&lp)) {
		sysLogPrintf(LOG_WARNING, "net: migrate: seat %d's record does not hold together; it starts afresh", slot);
		return 1;
	}

	memcpy(s_Res.lp[slot], data, NETLP_SIZE);
	s_Res.records += !s_Res.have[slot];
	s_Res.have[slot] = 1;
	s_Res.used[slot] = 0;
	sysLogPrintf(LOG_NOTE, "net: migrate: seat %d's record: at %.0f %.0f %.0f, health %.2f shield %.2f, weapon %d%s",
			slot, lp.pos[0], lp.pos[1], lp.pos[2], lp.health, lp.shield, lp.weaponnum, (lp.flags & NETLP_DEAD) ? " (dead)" : "");

	return 1;
}

static s32 netMigratePlayerOf(s32 slot)
{
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Vars.players[i] && g_Vars.playerstats[i].mpindex == slot) {
			return i;
		}
	}

	return -1;
}

/**
 * The host, at the head of each tick of a resumed match (netsession.c's
 * seats tick, from GO): at the first, the kill table and the scenario's
 * counts; then each player with a record and a life is given a new one
 * where it stood (playerStartNewLife, through the hooks below)
 */
void netMigrateHostTick(void)
{
	s32 i;
	s32 j;

	if (!s_Res.on || g_NetMode != NETMODE_SERVER) {
		return;
	}

	if (!s_Res.applied) {
		s_Res.applied = 1;

		for (i = 0; i < MAX_MPCHRS; i++) {
			struct mpchrconfig *mpchr = MPCHR(i);

			mpchr->numdeaths = s_Res.numdeaths[i];
			mpchr->numpoints = s_Res.numpoints[i];

			for (j = 0; j < MAX_MPCHRS; j++) {
				mpchr->killcounts[j] = s_Res.killcounts[i][j];
			}
		}

		if (s_Res.havescen) {
			netScenResume(s_Res.scen, s_Res.swap);
		}

		netEventsHostScores();

		{
			char host[NET_MAXNAME + 1];

			snprintf(host, sizeof(host), "%s", netSessionHostTitle());
			netSessionHostNotice(s_Res.prevhost[0] ? "%s left: %s hosts now. The match carries on." : "%s%s hosts now. The match carries on.",
					s_Res.prevhost, host);
		}

		sysLogPrintf(LOG_NOTE, "net: migrate: the match carries on at level time %d with its kill table and %u player record%s",
				lvGetStageTime60(), s_Res.records, s_Res.records == 1 ? "" : "s");

		{
			char line[512];
			s32 len = 0;

			line[0] = '\0';

			for (i = 0; i < MAX_PLAYERS && len < (s32)sizeof(line); i++) {
				s32 kills = 0;

				for (j = 0; j < MAX_MPCHRS; j++) {
					kills += j != i ? s_Res.killcounts[i][j] : 0;
				}

				if (s_Res.account[i][0] || kills || s_Res.numdeaths[i]) {
					len += snprintf(line + len, sizeof(line) - len, "%s%d \"%s\" kills %d deaths %d points %d", len ? "; " : "", i,
							s_Res.account[i], kills, s_Res.numdeaths[i], s_Res.numpoints[i]);
				}
			}

			sysLogPrintf(LOG_NOTE, "net: migrate: the seats' scores carried: %s", line);
		}
	}

	// the stage's first frames are its own (the spawn, the swirl's start)
	if (g_Vars.lvframenum < 2) {
		return;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		const s32 pn = s_Res.have[i] && !s_Res.used[i] ? netMigratePlayerOf(i) : -1;
		struct player *p = pn >= 0 ? g_Vars.players[pn] : NULL;
		struct netlpstate lp;

		if (!p || !p->prop || (s_Res.asked & (1u << pn))) {
			continue;
		}

		netLpUnpack(s_Res.lp[i], &lp);

		if (lp.flags & NETLP_DEAD) {
			// dead when the host went: it plays on from a new life of its own
			s_Res.used[i] = 1;
			continue;
		}

		// alive: a new life now, which the record places; dead (a seat out
		// of play until its player came back): the respawn its return asks for
		if (!p->isdead && !p->dostartnewlife) {
			p->dostartnewlife = true;
		}

		if (p->dostartnewlife) {
			s_Res.asked |= 1u << pn;
		}
	}
}

/*
 * The new life (player.c's hooks, the host; g_Vars.currentplayer is the
 * player): where its record says, with what it carried
 */

static struct netlpstate s_Landing;

s32 netMigrateTakeSpawn(struct coord *pos, s16 *rooms, f32 *angle)
{
	const s32 pn = g_Vars.currentplayernum;
	const s32 slot = pn >= 0 && pn < MAX_PLAYERS ? g_Vars.playerstats[pn].mpindex : -1;
	s32 n = 0;
	s32 i;

	if (!s_Res.on || slot < 0 || slot >= MAX_PLAYERS || !s_Res.have[slot] || s_Res.used[slot] || !(s_Res.asked & (1u << pn))) {
		return 0;
	}

	s_Res.used[slot] = 1;
	s_Res.asked &= ~(1u << pn);
	netLpUnpack(s_Res.lp[slot], &s_Landing);

	for (i = 0; i < 8 && n < 7; i++) {
		const s32 r = s_Landing.rooms[i];

		if (r < 0) {
			break;
		}

		if (r > 0 && r < g_Vars.roomcount) {
			rooms[n++] = r;
		}
	}

	if (n == 0) {
		sysLogPrintf(LOG_NOTE, "net: migrate: seat %d's record names no room of this stage; a spawn point instead", slot);
		return 0;
	}

	rooms[n] = -1;
	pos->x = s_Landing.pos[0];
	pos->y = s_Landing.pos[1];
	pos->z = s_Landing.pos[2];
	*angle = s_Landing.theta * M_BADTAU / 360.0f;
	s_Res.landing |= 1u << pn;
	s_Res.lives++;

	return 1;
}

static s32 netMigrateLanding(void)
{
	const s32 pn = g_Vars.currentplayernum;

	return s_Res.on && pn >= 0 && pn < 32 && (s_Res.landing & (1u << pn));
}

// Before playerSpawn: the guns and the ammo (the new life emptied both)
void netMigrateRestoreInventory(void)
{
	struct player *p = g_Vars.currentplayer;
	s32 w;
	s32 i;

	if (!netMigrateLanding() || !p) {
		return;
	}

	for (w = 1; w < 256 && w < NUM_WEAPONS; w++) {
		if ((s_Landing.inv[w >> 3] >> (w & 7)) & 1) {
			invGiveSingleWeapon(w);
		}

		if ((s_Landing.invdual[w >> 3] >> (w & 7)) & 1) {
			invGiveDoubleWeapon(w, w);
		}
	}

	for (i = 0; i < NETLP_NUMAMMO && i < (s32)ARRAYCOUNT(p->ammoheldarr); i++) {
		const s32 cap = bgunGetCapacityByAmmotype(i);
		s32 a = s_Landing.ammo[i];

		if (cap > 0 && a > cap) {
			a = cap;
		}

		p->ammoheldarr[i] = a;
	}
}

/**
 * playerSpawnWeapons: the guns in the hands as they were, if still held; 1
 * when the hands are this file's
 */
s32 netMigrateSpawnHands(void)
{
	s32 right;
	s32 left;

	if (!netMigrateLanding()) {
		return 0;
	}

	right = s_Landing.weaponnum > WEAPON_NONE && invHasSingleWeaponIncAllGuns(s_Landing.weaponnum) ? s_Landing.weaponnum : WEAPON_UNARMED;
	left = s_Landing.dual && right != WEAPON_UNARMED ? right : WEAPON_NONE;

	g_Vars.currentplayer->spawnweaponnums[HAND_LEFT] = left;
	g_Vars.currentplayer->spawnweaponnums[HAND_RIGHT] = right;
	bgunEquipWeapon2(HAND_LEFT, left);
	bgunEquipWeapon2(HAND_RIGHT, right);

	return 1;
}

// After playerSpawn (which sets the shield back to nothing): health and shield
void netMigrateRestoreHealth(void)
{
	struct player *p = g_Vars.currentplayer;

	if (!netMigrateLanding() || !p) {
		return;
	}

	s_Res.landing &= ~(1u << g_Vars.currentplayernum);
	p->bondhealth = s_Landing.health;
	p->oldhealth = 0;
	p->apparenthealth = 0;
	p->oldarmour = 0;
	p->apparentarmour = 0;

	if (p->prop && p->prop->chr) {
		p->prop->chr->cshield = s_Landing.shield;
	}

	p->hands[HAND_RIGHT].loadedammo[0] = s_Landing.loaded[0];
	p->hands[HAND_RIGHT].loadedammo[1] = s_Landing.loaded[1];
	p->hands[HAND_LEFT].loadedammo[0] = s_Landing.loaded[2];
	p->hands[HAND_LEFT].loadedammo[1] = s_Landing.loaded[3];

	sysLogPrintf(LOG_NOTE, "net: migrate: player %d (seat %d) back where it was: %.0f %.0f %.0f, health %.2f, shield %.2f, weapon %d",
			g_Vars.currentplayernum, g_Vars.playerstats[g_Vars.currentplayernum].mpindex, p->prop->pos.x, p->prop->pos.y, p->prop->pos.z,
			p->bondhealth, s_Landing.shield, s_Landing.weaponnum);
}

/**
 * H12: the resumed match is over (or a client's match stage stopped): what
 * was its own goes
 */
void netMigrateMatchStopped(void)
{
	if (s_Res.on) {
		sysLogPrintf(LOG_NOTE, "net: migrate: the resumed match is over (%u record%s, %u given their life back)", s_Res.records,
				s_Res.records == 1 ? "" : "s", s_Res.lives);
		s_Res.on = 0;
		s_Res.landing = 0;
		s_Res.asked = 0;
		s_Res.records = 0;
		memset(s_Res.have, 0, sizeof(s_Res.have));
	}
}

// F3 ([netplay]): the migration's state
void netMigrateTrace(FILE *f)
{
	netMigrateStale();
	fprintf(f, "host migration: kept %s%s%s (room %s, stage 0x%02x, seat %d, match %u, level time %d, prev host \"%s\"); resume %s%s, swap %d, records %u, lives %u\n",
			s_Kept.valid ? "yes" : "no", s_Kept.resume ? ", to carry on" : "", s_Kept.havelp ? ", own state" : "",
			s_Kept.room, s_Kept.stage, s_Kept.myslot, s_Kept.matchid, s_Kept.stagetime60, s_Kept.prevhost,
			s_Res.pending ? "pending" : s_Res.on ? "on" : "off", s_Res.applied ? " (applied)" : "", s_Res.swap, s_Res.records, s_Res.lives);
}
