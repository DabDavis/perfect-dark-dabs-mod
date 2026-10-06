#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "config.h"
#include "system.h"
#include "modloader.h"
#include "gexplus.h"
#include "game/challenge.h"
#include "game/savebuffer.h"
#include "game/modunlocks.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "net/net.h"
#include "netint.h"

/**
 * The match's rules (spec-stage.md §3): the host's match state and its
 * gameplay ini values, written into RULES at the match start; a client takes
 * them over its own for the match (H3) and gives them back after (H12), and
 * pd.ini never sees them (H13).
 *
 * The ini values go by key through configGetEntry(), since many of them are
 * file statics. Classes:
 *   SYNC     the host's value is used on every machine for the match
 *   MUST     must already match (a startup-only state, or content): refused
 *   MUST_GE  the same, on a GoldenEye stage only (checked at the match start)
 *   REFUSE   a net game only when the value is stock on both sides
 */

extern u8 g_MpFeaturesUnlocked[80];
extern s32 g_MpWeaponSetNum;

static const struct {
	const char *key;
	s32 cls;
	const char *stock; // REFUSE: the stock value, as text
} s_NetKeys[] = {
	{ "Mod.JumpHeight",              NETKEY_SYNC },
	{ "Mod.JumpFor",                 NETKEY_SYNC },
	{ "Mod.CombatRoll",              NETKEY_SYNC },
	{ "Mod.MeleeCombos",             NETKEY_SYNC },
	{ "Mod.FlinchWhenShot",          NETKEY_SYNC },
	{ "Mod.StartArmed",              NETKEY_SYNC },
	{ "Mod.StartArmedFor",           NETKEY_SYNC },
	{ "Mod.Bodies",                  NETKEY_SYNC },
	{ "Mod.BodyTime",                NETKEY_SYNC },
	{ "Mod.BodiesDrawn",             NETKEY_SYNC },
	{ "Mod.Akimbo",                  NETKEY_SYNC },
	{ "Mod.CodAiming",               NETKEY_SYNC },
	{ "Mod.CodAimLock",              NETKEY_SYNC },
	{ "Mod.SkipDeathScreen",         NETKEY_SYNC },
	{ "Mod.QuickWeaponSwap",         NETKEY_SYNC },
	{ "Mod.TranquilizerEffect",      NETKEY_SYNC },
	{ "Mod.DisableFog",              NETKEY_SYNC },
	{ "Mod.GlassSeeThrough",         NETKEY_SYNC },
	{ "Mod.GePlusPdGuns",            NETKEY_SYNC },
	{ "Mod.GePlusRegion",            NETKEY_SYNC },
	{ "Mod.BorrowGoldenEyeGuns",     NETKEY_MUST },
	{ "Mod.GePlusRevisionFixes",     NETKEY_MUST_GE },
	{ "Mod.XblaMeshes",              NETKEY_MUST_GE },
	{ "Mod.GeXblaCommunityEdition",  NETKEY_MUST_GE },
	{ "Mod.SimBrain",                NETKEY_REFUSE, "stock" },
};

/**
 * RULES as a client received it, kept from its arrival to STAGE_LOAD
 */
struct netrulessim {
	u8 on;
	u8 type;
	u8 difficulty;
	u8 mpheadnum;
	u8 mpbodynum;
	u8 team;
	u32 displayoptions;
	char name[15];
	s8 stats[BOTSTAT_COUNT];
};

struct netruleshuman {
	char name[15];
	u8 mpheadnum;
	u8 mpbodynum;
	u8 team;
	u32 displayoptions;
	u8 handicap;
	u16 options;
	u8 gunfuncs[6];
};

static struct {
	s32 valid;
	u32 matchid;
	char name[MPSETUP_MAXNAME + 1];
	u32 options;
	u8 scenario;
	u8 timelimit;
	u8 scorelimit;
	u16 teamscorelimit;
	u16 chrslots;
	u16 humanslotshi; // protocol 10: human slots 4-11 (g_MpHumanSlotsHi)
	u8 weapons[NUM_MPWEAPONSLOTS];
	u32 scenariobits;
	s32 weaponsetnum;
	u64 filters;
	u8 simslots[MAX_BOTS];
	struct netrulessim sims[MAX_BOTS];
	u8 difficulties[MAX_BOTS][MAX_LOCAL_PLAYERS];
	struct netruleshuman humans[MAX_PLAYERS];
	char teamnames[MAX_TEAMS][12];
	u8 unlocked[80];
	u32 modunlocks;
	u8 gexplusmode;
	u8 gexplusscenario;
	u8 endless;
	s32 maxexplosions;
	struct netkeyvalue keys[NET_MAXKEYS];
	s32 nkeys;
} s_NetRules;

/**
 * What a match changed, as it was before: a client's whole match state and
 * its own values of the SYNC keys; the host's player slots 1-11
 */
static struct {
	s32 client;  // a client's whole state is saved
	s32 host;    // the host's remote slots are saved
	struct mpsetup mpsetup;
	struct mpbotconfig bots[MAX_BOTS];
	u8 simslots[MAX_BOTS];
	u8 difficulties[MAX_BOTS][MAX_LOCAL_PLAYERS];
	struct mpplayerconfig players[MAX_PLAYERS];
	struct mplockinfo lockinfo; // a net match's winner/loser can be slot 4-11
	char teamnames[MAX_TEAMS][12];
	s32 weaponsetnum;
	u8 filters[NUM_MPWEAPONS];
	u8 unlocked[80];
	u32 modunlocks;
	s32 gexplusmode;
	s32 gexplusscenario;
	s32 endless;
	s32 maxexplosions;
	u8 locktype;
	struct netkeyvalue keys[NET_MAXKEYS];
	s32 nkeys;
	s32 swapped; // H13: the own values are in while pd.ini is written
} s_NetSaved;

static s32 s_NetRulesLocked = 0;
static u32 s_NetRulesAppliedId = 0; // the matchid of the RULES netRulesApply took

/*
 * Ini values by key
 */

s32 netRulesReadKey(const char *key, struct netkeyvalue *out)
{
	s32 type;
	void *ptr;
	u32 maxstr;

	memset(out, 0, sizeof(*out));
	snprintf(out->key, sizeof(out->key), "%s", key);

	if (!configGetEntry(key, &type, &ptr, &maxstr)) {
		return 0;
	}

	out->type = type;

	switch (type) {
	case CONFIG_TYPE_S32: out->s = *(s32 *)ptr; break;
	case CONFIG_TYPE_F32: out->f = *(f32 *)ptr; break;
	case CONFIG_TYPE_U32: out->u = *(u32 *)ptr; break;
	case CONFIG_TYPE_STR: snprintf(out->str, sizeof(out->str), "%s", (const char *)ptr); break;
	default: return 0;
	}

	return 1;
}

static void netRulesWriteKey(const struct netkeyvalue *kv)
{
	s32 type;
	void *ptr;
	u32 maxstr;

	if (!configGetEntry(kv->key, &type, &ptr, &maxstr) || type != kv->type) {
		sysLogPrintf(LOG_WARNING, "net: rules: %s is not a %d setting here; left alone", kv->key, kv->type);
		return;
	}

	switch (type) {
	case CONFIG_TYPE_S32: *(s32 *)ptr = kv->s; break;
	case CONFIG_TYPE_F32: *(f32 *)ptr = kv->f; break;
	case CONFIG_TYPE_U32: *(u32 *)ptr = kv->u; break;
	case CONFIG_TYPE_STR: snprintf((char *)ptr, maxstr ? maxstr : 1, "%s", kv->str); break;
	}

	// within the range the setting was registered with, whatever was sent
	configClampEntry(kv->key);
}

void netRulesWriteValue(struct netbuf *b, const struct netkeyvalue *kv)
{
	netBufWriteU8(b, (u8)kv->type);

	switch (kv->type) {
	case CONFIG_TYPE_S32: netBufWriteS32(b, kv->s); break;
	case CONFIG_TYPE_F32: netBufWriteF32(b, kv->f); break;
	case CONFIG_TYPE_U32: netBufWriteU32(b, kv->u); break;
	default: netWriteStr(b, kv->str, NET_MAXSTRVAL); break;
	}
}

void netRulesReadValue(struct netbuf *b, struct netkeyvalue *kv)
{
	kv->type = netBufReadU8(b);

	switch (kv->type) {
	case CONFIG_TYPE_S32: kv->s = netBufReadS32(b); break;
	case CONFIG_TYPE_F32: kv->f = netBufReadF32(b); break;
	case CONFIG_TYPE_U32: kv->u = netBufReadU32(b); break;
	case CONFIG_TYPE_STR: netBufReadString(b, kv->str, sizeof(kv->str)); break;
	default: b->error = 1; break;
	}
}

s32 netRulesValuesEqual(const struct netkeyvalue *a, const struct netkeyvalue *b)
{
	if (a->type != b->type) {
		return 0;
	}

	switch (a->type) {
	case CONFIG_TYPE_S32: return a->s == b->s;
	case CONFIG_TYPE_F32: return a->f == b->f;
	case CONFIG_TYPE_U32: return a->u == b->u;
	default: return strcasecmp(a->str, b->str) == 0;
	}
}

void netRulesValueString(const struct netkeyvalue *kv, char *buf, s32 size)
{
	switch (kv->type) {
	case CONFIG_TYPE_S32: snprintf(buf, size, "%d", kv->s); break;
	case CONFIG_TYPE_F32: snprintf(buf, size, "%g", kv->f); break;
	case CONFIG_TYPE_U32: snprintf(buf, size, "%u", kv->u); break;
	case CONFIG_TYPE_STR: snprintf(buf, size, "\"%s\"", kv->str); break;
	default: snprintf(buf, size, "(none)"); break;
	}
}

static s32 netRulesKeyClass(const char *key)
{
	u32 i;

	for (i = 0; i < ARRAYCOUNT(s_NetKeys); i++) {
		if (strcasecmp(s_NetKeys[i].key, key) == 0) {
			return s_NetKeys[i].cls;
		}
	}

	return -1;
}

static const char *netRulesKeyStock(const char *key)
{
	u32 i;

	for (i = 0; i < ARRAYCOUNT(s_NetKeys); i++) {
		if (strcasecmp(s_NetKeys[i].key, key) == 0) {
			return s_NetKeys[i].stock;
		}
	}

	return NULL;
}

s32 netRulesWriteClientKeys(struct netbuf *b)
{
	struct netkeyvalue kv[NET_MAXKEYS];
	s32 n = 0;
	s32 i;
	u32 k;

	for (k = 0; k < ARRAYCOUNT(s_NetKeys) && n < NET_MAXKEYS; k++) {
		if (s_NetKeys[k].cls != NETKEY_SYNC && netRulesReadKey(s_NetKeys[k].key, &kv[n])) {
			n++;
		}
	}

	netBufWriteU8(b, (u8)n);

	for (i = 0; i < n; i++) {
		netWriteStr(b, kv[i].key, NET_MAXKEY);
		netRulesWriteValue(b, &kv[i]);
	}

	return n;
}

static s32 netRulesIsStock(const struct netkeyvalue *kv)
{
	const char *stock = netRulesKeyStock(kv->key);
	char buf[NET_MAXSTRVAL + 4];

	if (!stock) {
		return 1;
	}

	if (kv->type == CONFIG_TYPE_STR) {
		return kv->str[0] == '\0' || strcasecmp(kv->str, stock) == 0;
	}

	netRulesValueString(kv, buf, sizeof(buf));

	return strcmp(buf, stock) == 0;
}

/**
 * gestage: 1 on a GoldenEye stage (the MUST_GE keys count), 0 elsewhere,
 * -1 when no stage is chosen yet (CONNECT: they are checked at the start)
 */
s32 netRulesCheckClientKeys(const struct netkeyvalue *keys, s32 nkeys, s32 gestage, s32 *code,
		char *key, s32 keysize, char *text, s32 textsize)
{
	struct netkeyvalue mine;
	char a[NET_MAXSTRVAL + 4];
	char b[NET_MAXSTRVAL + 4];
	s32 i;

	for (i = 0; i < nkeys; i++) {
		s32 cls = netRulesKeyClass(keys[i].key);

		if (cls < 0 || cls == NETKEY_SYNC) {
			continue;
		}

		netRulesReadKey(keys[i].key, &mine);

		if (cls == NETKEY_REFUSE) {
			if (!netRulesIsStock(&mine) || !netRulesIsStock(&keys[i])) {
				const struct netkeyvalue *off = !netRulesIsStock(&mine) ? &mine : &keys[i];

				netRulesValueString(off, a, sizeof(a));
				*code = NETREFUSE_NOTSTOCK;
				snprintf(key, keysize, "%s", keys[i].key);
				snprintf(text, textsize, "%s is %s on the %s; a net game needs it at \"%s\"",
						keys[i].key, a, off == &mine ? "host" : "joining game", netRulesKeyStock(keys[i].key));
				return 1;
			}

			continue;
		}

		if (cls == NETKEY_MUST_GE && gestage <= 0) {
			continue;
		}

		if (!netRulesValuesEqual(&mine, &keys[i])) {
			netRulesValueString(&mine, a, sizeof(a));
			netRulesValueString(&keys[i], b, sizeof(b));
			*code = NETREFUSE_MUST;
			snprintf(key, keysize, "%s", keys[i].key);
			snprintf(text, textsize, "%s must match: the host has %s, the joining game %s", keys[i].key, a, b);
			return 1;
		}
	}

	return 0;
}

/*
 * RULES
 */

void netRulesWrite(struct netbuf *b, u32 matchid)
{
	struct savebuffer sb;
	u64 filters = 0;
	s32 nsims = 0;
	s32 i;
	s32 j;
	u32 k;
	struct netkeyvalue kv;
	s32 nkeys = 0;
	u8 *nkeysat;

	netBufWriteU8(b, NETMSG_RULES);
	netBufWriteU32(b, matchid);

	netWriteStr(b, g_MpSetup.name, MPSETUP_MAXNAME);
	netBufWriteU32(b, g_MpSetup.options);
	netBufWriteU8(b, g_MpSetup.scenario);
	netBufWriteU8(b, g_MpSetup.timelimit);
	netBufWriteU8(b, g_MpSetup.scorelimit);
	netBufWriteU16(b, g_MpSetup.teamscorelimit);
	netBufWriteU16(b, g_MpSetup.chrslots);
	netBufWriteU16(b, g_MpHumanSlotsHi);

	for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		netBufWriteU8(b, g_MpSetup.weapons[i]);
	}

	// the scenario's own bits, as its save writes them (KOH's hill time)
	savebufferClear(&sb);
	scenarioWriteSave(&sb);
	sb.bitpos = 0;
	netBufWriteU32(b, (u32)savebufferReadBits(&sb, 32));

	netBufWriteS32(b, g_MpWeaponSetNum);

	for (i = 0; i < NUM_MPWEAPONS && i < 64; i++) {
		filters |= g_MpWeaponSetRandomFilters[i] ? (1ULL << i) : 0;
	}

	netWriteU64(b, filters);

	for (i = 0; i < MAX_BOTS; i++) {
		netBufWriteU8(b, g_MpSimSlots[i]);
		nsims += g_MpSimSlots[i] ? 1 : 0;
	}

	netBufWriteU8(b, (u8)nsims);

	for (i = 0; i < MAX_BOTS; i++) {
		struct mpbotconfig *bot = &g_BotConfigsArray[i];

		if (!g_MpSimSlots[i]) {
			continue;
		}

		netBufWriteU8(b, (u8)i);
		netBufWriteU8(b, bot->type);
		netBufWriteU8(b, bot->difficulty);
		netBufWriteU8(b, bot->base.mpheadnum);
		netBufWriteU8(b, bot->base.mpbodynum);
		netBufWriteU8(b, bot->base.team);
		netBufWriteU32(b, bot->base.displayoptions);
		netWriteStr(b, bot->base.name, 14);

		for (j = 0; j < BOTSTAT_COUNT; j++) {
			netBufWriteS8(b, bot->stats[j]);
		}
	}

	for (i = 0; i < MAX_BOTS; i++) {
		for (j = 0; j < MAX_LOCAL_PLAYERS; j++) {
			netBufWriteU8(b, g_MpSimulantDifficultiesPerNumPlayers[i][j]);
		}
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct mpplayerconfig *p = &g_PlayerConfigsArray[i];

		netWriteStr(b, netSessionWireName(i), 14);
		netBufWriteU8(b, p->base.mpheadnum);
		netBufWriteU8(b, p->base.mpbodynum);
		netBufWriteU8(b, p->base.team);
		netBufWriteU32(b, p->base.displayoptions);
		netBufWriteU8(b, p->handicap);
		netBufWriteU16(b, p->options);

		for (j = 0; j < 6; j++) {
			netBufWriteU8(b, p->gunfuncs[j]);
		}
	}

	for (i = 0; i < MAX_TEAMS; i++) {
		netWriteStr(b, g_BossFile.teamnames[i], 11);
	}

	for (i = 0; i < 80; i++) {
		netBufWriteU8(b, g_MpFeaturesUnlocked[i]);
	}

	netBufWriteU32(b, g_ModUnlocks);
	netBufWriteU8(b, (u8)(g_GexPlusMode != 0));
	netBufWriteU8(b, (u8)gexPlusGetScenario());
	netBufWriteU8(b, (u8)(g_MpEndlessMatch != 0));
	netBufWriteS32(b, g_MaxExplosionsSetting);

	nkeysat = netBufReserve(b, 1);

	for (k = 0; k < ARRAYCOUNT(s_NetKeys) && nkeys < NET_MAXKEYS; k++) {
		if (s_NetKeys[k].cls == NETKEY_SYNC && netRulesReadKey(s_NetKeys[k].key, &kv)) {
			netWriteStr(b, kv.key, NET_MAXKEY);
			netRulesWriteValue(b, &kv);
			nkeys++;
		}
	}

	if (nkeysat) {
		*nkeysat = (u8)nkeys;
	}

	netWriteStr(b, "", NET_MAXCOMPNAME);
}

s32 netRulesRead(struct netbuf *b)
{
	s32 nsims;
	s32 i;
	s32 j;
	char spare[NET_MAXCOMPNAME + 1];

	memset(&s_NetRules, 0, sizeof(s_NetRules));

	s_NetRules.matchid = netBufReadU32(b);
	netBufReadString(b, s_NetRules.name, sizeof(s_NetRules.name));
	s_NetRules.options = netBufReadU32(b);
	s_NetRules.scenario = netBufReadU8(b);
	s_NetRules.timelimit = netBufReadU8(b);
	s_NetRules.scorelimit = netBufReadU8(b);
	s_NetRules.teamscorelimit = netBufReadU16(b);
	s_NetRules.chrslots = netBufReadU16(b);
	s_NetRules.humanslotshi = netBufReadU16(b);

	if (s_NetRules.humanslotshi >> (MAX_PLAYERS - MPSETUP_HUMANBITS)) {
		b->error = 1;
	}

	for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		s_NetRules.weapons[i] = netBufReadU8(b);
	}

	s_NetRules.scenariobits = netBufReadU32(b);
	s_NetRules.weaponsetnum = netBufReadS32(b);
	s_NetRules.filters = netReadU64(b);

	for (i = 0; i < MAX_BOTS; i++) {
		s_NetRules.simslots[i] = netBufReadU8(b) ? 1 : 0;
	}

	nsims = netBufReadU8(b);

	for (i = 0; i < nsims && netBufOk(b); i++) {
		s32 index = netBufReadU8(b);
		struct netrulessim sim;

		sim.on = 1;
		sim.type = netBufReadU8(b);
		sim.difficulty = netBufReadU8(b);
		sim.mpheadnum = netBufReadU8(b);
		sim.mpbodynum = netBufReadU8(b);
		sim.team = netBufReadU8(b);
		sim.displayoptions = netBufReadU32(b);
		netBufReadString(b, sim.name, sizeof(sim.name));

		for (j = 0; j < BOTSTAT_COUNT; j++) {
			sim.stats[j] = netBufReadS8(b);
		}

		if (index >= MAX_BOTS || !s_NetRules.simslots[index]) {
			b->error = 1;
			break;
		}

		s_NetRules.sims[index] = sim;
	}

	for (i = 0; i < MAX_BOTS; i++) {
		for (j = 0; j < MAX_LOCAL_PLAYERS; j++) {
			s_NetRules.difficulties[i][j] = netBufReadU8(b);
		}
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct netruleshuman *h = &s_NetRules.humans[i];

		netBufReadString(b, h->name, sizeof(h->name));
		h->mpheadnum = netBufReadU8(b);
		h->mpbodynum = netBufReadU8(b);
		h->team = netBufReadU8(b);
		h->displayoptions = netBufReadU32(b);
		h->handicap = netBufReadU8(b);
		h->options = netBufReadU16(b);

		for (j = 0; j < 6; j++) {
			h->gunfuncs[j] = netBufReadU8(b);
		}
	}

	for (i = 0; i < MAX_TEAMS; i++) {
		netBufReadString(b, s_NetRules.teamnames[i], sizeof(s_NetRules.teamnames[i]));
	}

	for (i = 0; i < 80; i++) {
		s_NetRules.unlocked[i] = netBufReadU8(b);
	}

	s_NetRules.modunlocks = netBufReadU32(b);
	s_NetRules.gexplusmode = netBufReadU8(b);
	s_NetRules.gexplusscenario = netBufReadU8(b);
	s_NetRules.endless = netBufReadU8(b);
	s_NetRules.maxexplosions = netBufReadS32(b);
	s_NetRules.nkeys = netBufReadU8(b);

	if (s_NetRules.nkeys > NET_MAXKEYS) {
		b->error = 1;
	}

	for (i = 0; i < s_NetRules.nkeys && netBufOk(b); i++) {
		netBufReadString(b, s_NetRules.keys[i].key, sizeof(s_NetRules.keys[i].key));
		netRulesReadValue(b, &s_NetRules.keys[i]);

		if (netRulesKeyClass(s_NetRules.keys[i].key) != NETKEY_SYNC) {
			// only the table's SYNC keys are ever applied
			b->error = 1;
		}
	}

	netBufReadString(b, spare, sizeof(spare));

	if (s_NetRules.scenario > MPSCENARIO_CAPTURETHECASE || s_NetRules.timelimit > 60 || s_NetRules.scorelimit > 100) {
		b->error = 1;
	}

	// what the game later uses as an index or an allocation's size
	for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		if (s_NetRules.weapons[i] >= NUM_MPWEAPONS) {
			b->error = 1;
		}
	}

	if (s_NetRules.weaponsetnum < 0 || s_NetRules.weaponsetnum > WEAPONSET_CUSTOM
			|| s_NetRules.maxexplosions < 6 || s_NetRules.maxexplosions > 96
			|| s_NetRules.gexplusscenario >= GEXPLUS_NUMSCENARIOS) {
		b->error = 1;
	}

	for (i = 0; i < MAX_BOTS; i++) {
		struct netrulessim *sim = &s_NetRules.sims[i];

		if (sim->on && (sim->type > BOTTYPE_VENGE || sim->difficulty > BOTDIFF_DISABLED || sim->team >= MAX_TEAMS)) {
			b->error = 1;
		}

		for (j = 0; j < MAX_LOCAL_PLAYERS; j++) {
			if (s_NetRules.difficulties[i][j] > BOTDIFF_DISABLED) {
				b->error = 1;
			}
		}
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (s_NetRules.humans[i].team >= MAX_TEAMS) {
			b->error = 1;
		}
	}

	s_NetRules.valid = netBufOk(b) && netBufRemaining(b) == 0;

	return s_NetRules.valid;
}

u32 netRulesMatchId(void)
{
	return s_NetRules.valid ? s_NetRules.matchid : 0;
}

/*
 * Applying and restoring
 */

static void netRulesSaveClient(void)
{
	u32 k;

	s_NetSaved.mpsetup = g_MpSetup;
	memcpy(s_NetSaved.bots, g_BotConfigsArray, sizeof(s_NetSaved.bots));
	memcpy(s_NetSaved.simslots, g_MpSimSlots, sizeof(s_NetSaved.simslots));
	memcpy(s_NetSaved.difficulties, g_MpSimulantDifficultiesPerNumPlayers, sizeof(s_NetSaved.difficulties));
	memcpy(s_NetSaved.players, g_PlayerConfigsArray, sizeof(s_NetSaved.players));
	memcpy(s_NetSaved.teamnames, g_BossFile.teamnames, sizeof(s_NetSaved.teamnames));
	s_NetSaved.weaponsetnum = g_MpWeaponSetNum;
	memcpy(s_NetSaved.filters, g_MpWeaponSetRandomFilters, sizeof(s_NetSaved.filters));
	memcpy(s_NetSaved.unlocked, g_MpFeaturesUnlocked, sizeof(s_NetSaved.unlocked));
	s_NetSaved.modunlocks = g_ModUnlocks;
	s_NetSaved.gexplusmode = g_GexPlusMode;
	s_NetSaved.gexplusscenario = gexPlusGetScenario();
	s_NetSaved.endless = g_MpEndlessMatch;
	s_NetSaved.maxexplosions = g_MaxExplosionsSetting;
	s_NetSaved.locktype = g_BossFile.locktype;
	s_NetSaved.lockinfo = g_MpLockInfo;

	s_NetSaved.nkeys = 0;

	for (k = 0; k < ARRAYCOUNT(s_NetKeys); k++) {
		if (s_NetKeys[k].cls == NETKEY_SYNC
				&& netRulesReadKey(s_NetKeys[k].key, &s_NetSaved.keys[s_NetSaved.nkeys])) {
			s_NetSaved.nkeys++;
		}
	}

	s_NetSaved.client = 1;
}

void netRulesSaveHost(void)
{
	if (s_NetSaved.client || s_NetSaved.host) {
		return;
	}

	memcpy(s_NetSaved.players, g_PlayerConfigsArray, sizeof(s_NetSaved.players));
	s_NetSaved.mpsetup = g_MpSetup;
	s_NetSaved.locktype = g_BossFile.locktype;
	s_NetSaved.lockinfo = g_MpLockInfo;
	s_NetSaved.host = 1;
}

/**
 * H3: a client takes the host's rules over its own for the match. The GE
 * scenario first: its setter overwrites g_MpSetup.scenario.
 */
void netRulesApply(void)
{
	struct savebuffer sb;
	char a[NET_MAXSTRVAL + 4];
	char b[NET_MAXSTRVAL + 4];
	struct netkeyvalue mine;
	s32 i;
	s32 j;

	if (!s_NetRules.valid) {
		return;
	}

	if (!s_NetSaved.client) {
		netRulesSaveClient();
	}

	s_NetRulesAppliedId = s_NetRules.matchid;

	gexPlusSetScenario(s_NetRules.gexplusscenario);
	g_GexPlusMode = s_NetRules.gexplusmode;

	snprintf(g_MpSetup.name, sizeof(g_MpSetup.name), "%s", s_NetRules.name);
	g_MpSetup.options = s_NetRules.options;
	g_MpSetup.scenario = s_NetRules.scenario;
	g_MpSetup.timelimit = s_NetRules.timelimit;
	g_MpSetup.scorelimit = s_NetRules.scorelimit;
	g_MpSetup.teamscorelimit = s_NetRules.teamscorelimit;

	for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		g_MpSetup.weapons[i] = s_NetRules.weapons[i];
	}

	savebufferClear(&sb);
	savebufferOr(&sb, s_NetRules.scenariobits, 32);
	sb.bitpos = 0;
	scenarioReadSave(&sb, 1);

	g_MpWeaponSetNum = s_NetRules.weaponsetnum;

	for (i = 0; i < NUM_MPWEAPONS && i < 64; i++) {
		g_MpWeaponSetRandomFilters[i] = (s_NetRules.filters >> i) & 1;
	}

	for (i = 0; i < MAX_BOTS; i++) {
		if (s_NetRules.sims[i].on) {
			struct mpbotconfig *bot = &g_BotConfigsArray[i];
			struct netrulessim *sim = &s_NetRules.sims[i];

			bot->type = sim->type;
			bot->difficulty = sim->difficulty;
			bot->base.mpheadnum = mpHeadNumSafe(sim->mpheadnum);
			bot->base.mpbodynum = sim->mpbodynum <= mpGetNumBodies() + 1 ? sim->mpbodynum : 0;
			bot->base.team = sim->team;
			bot->base.displayoptions = sim->displayoptions;
			snprintf(bot->base.name, sizeof(bot->base.name), "%s", sim->name);

			for (j = 0; j < BOTSTAT_COUNT; j++) {
				bot->stats[j] = sim->stats[j];
			}
		}

		mpSetSimSlotOn(i, s_NetRules.simslots[i]);
	}

	memcpy(g_MpSimulantDifficultiesPerNumPlayers, s_NetRules.difficulties, sizeof(s_NetRules.difficulties));

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct mpplayerconfig *p = &g_PlayerConfigsArray[i];
		struct netruleshuman *h = &s_NetRules.humans[i];

		snprintf(p->base.name, sizeof(p->base.name), "%s", h->name);
		p->base.mpheadnum = mpHeadNumSafe(h->mpheadnum);
		p->base.mpbodynum = h->mpbodynum <= mpGetNumBodies() + 1 ? h->mpbodynum : 0;
		p->base.team = h->team;
		p->base.displayoptions = h->displayoptions;
		p->handicap = h->handicap;
		p->options = h->options;

		for (j = 0; j < 6; j++) {
			p->gunfuncs[j] = h->gunfuncs[j];
		}

		// a profile is never saved from a net match (menutick.c's endscreen)
		memset(&p->fileguid, 0, sizeof(p->fileguid));
	}

	// the host's chrslots last: mpSetSimSlotOn mirrored the sims into it
	g_MpSetup.chrslots = s_NetRules.chrslots;
	g_MpHumanSlotsHi = (u8)s_NetRules.humanslotshi;

	for (i = 0; i < MAX_TEAMS; i++) {
		snprintf(g_BossFile.teamnames[i], sizeof(g_BossFile.teamnames[i]), "%s", s_NetRules.teamnames[i]);
	}

	memcpy(g_MpFeaturesUnlocked, s_NetRules.unlocked, sizeof(s_NetRules.unlocked));
	g_ModUnlocks = s_NetRules.modunlocks;
	g_MpEndlessMatch = s_NetRules.endless;

	// puppet explosions must fit as many as the host makes
	if (s_NetRules.maxexplosions > g_MaxExplosionsSetting) {
		g_MaxExplosionsSetting = s_NetRules.maxexplosions;
	}

	// a challenge is never marked complete from a net match
	if (g_BossFile.locktype == MPLOCKTYPE_CHALLENGE) {
		g_BossFile.locktype = MPLOCKTYPE_NONE;
	}

	for (i = 0; i < s_NetRules.nkeys; i++) {
		if (netRulesReadKey(s_NetRules.keys[i].key, &mine) && !netRulesValuesEqual(&mine, &s_NetRules.keys[i])) {
			netRulesValueString(&mine, a, sizeof(a));
			netRulesValueString(&s_NetRules.keys[i], b, sizeof(b));
			sysLogPrintf(LOG_NOTE, "net: rules: %s %s -> %s (the host's, for the match)", s_NetRules.keys[i].key, a, b);
		}

		netRulesWriteKey(&s_NetRules.keys[i]);
	}

	sysLogPrintf(LOG_NOTE, "net: rules applied: match %u, scenario %d, chrslots 0x%04x, human slots 4-11 0x%02x, options 0x%08x, %d keys",
			s_NetRules.matchid, g_MpSetup.scenario, g_MpSetup.chrslots, g_MpHumanSlotsHi, g_MpSetup.options, s_NetRules.nkeys);

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (mpIsHumanSlotOn(i)) {
			sysLogPrintf(LOG_NOTE, "net: the match's players: \"%s\" in slot %d", g_PlayerConfigsArray[i].base.name, i);
		}
	}
}

/**
 * H12: everything a net match changed goes back. A client's whole state; on
 * the host only its slots 1-11, which the remote players' names went into.
 */
void netRulesRestore(void)
{
	s32 i;

	if (s_NetSaved.client) {
		gexPlusSetScenario(s_NetSaved.gexplusscenario);
		g_GexPlusMode = s_NetSaved.gexplusmode;
		g_MpSetup = s_NetSaved.mpsetup;
		memcpy(g_BotConfigsArray, s_NetSaved.bots, sizeof(s_NetSaved.bots));
		memcpy(g_MpSimSlots, s_NetSaved.simslots, sizeof(s_NetSaved.simslots));
		memcpy(g_MpSimulantDifficultiesPerNumPlayers, s_NetSaved.difficulties, sizeof(s_NetSaved.difficulties));
		memcpy(g_PlayerConfigsArray, s_NetSaved.players, sizeof(s_NetSaved.players));
		memcpy(g_BossFile.teamnames, s_NetSaved.teamnames, sizeof(s_NetSaved.teamnames));
		g_MpWeaponSetNum = s_NetSaved.weaponsetnum;
		memcpy(g_MpWeaponSetRandomFilters, s_NetSaved.filters, sizeof(s_NetSaved.filters));
		memcpy(g_MpFeaturesUnlocked, s_NetSaved.unlocked, sizeof(s_NetSaved.unlocked));
		g_ModUnlocks = s_NetSaved.modunlocks;
		g_MpEndlessMatch = s_NetSaved.endless;
		g_MaxExplosionsSetting = s_NetSaved.maxexplosions;
		g_BossFile.locktype = s_NetSaved.locktype;
		g_MpLockInfo = s_NetSaved.lockinfo;
		g_MpHumanSlotsHi = 0;

		for (i = 0; i < s_NetSaved.nkeys; i++) {
			netRulesWriteKey(&s_NetSaved.keys[i]);
		}

		// the unlock table is the profile's again
		challengeDetermineUnlockedFeatures();

		sysLogPrintf(LOG_NOTE, "net: rules restored: this machine's own setup and settings are back");
	} else if (s_NetSaved.host) {
		for (i = 1; i < MAX_PLAYERS; i++) {
			g_PlayerConfigsArray[i] = s_NetSaved.players[i];
		}

		g_MpSetup.chrslots = s_NetSaved.mpsetup.chrslots;
		g_BossFile.locktype = s_NetSaved.locktype;
		g_MpLockInfo = s_NetSaved.lockinfo;
		g_MpHumanSlotsHi = 0;
	}

	s_NetSaved.client = 0;
	s_NetSaved.host = 0;
	s_NetSaved.swapped = 0;
	s_NetRulesLocked = 0;

	// RULES for a later match that came already are kept for its STAGE_LOAD
	if (s_NetRules.matchid == s_NetRulesAppliedId) {
		s_NetRules.valid = 0;
	}
}

/**
 * H13: while pd.ini is written the player's own values are in, then the
 * host's go back for the rest of the match
 */
static void netRulesSwap(void)
{
	struct netkeyvalue cur;
	s32 maxexplosions = g_MaxExplosionsSetting;
	s32 i;

	// Game.MaxExplosions too: a client raised it to the host's
	g_MaxExplosionsSetting = s_NetSaved.maxexplosions;
	s_NetSaved.maxexplosions = maxexplosions;

	for (i = 0; i < s_NetSaved.nkeys; i++) {
		if (netRulesReadKey(s_NetSaved.keys[i].key, &cur)) {
			netRulesWriteKey(&s_NetSaved.keys[i]);
			s_NetSaved.keys[i] = cur;
		}
	}
}

void netRulesConfigSaveBegin(void)
{
	if (s_NetSaved.client && !s_NetSaved.swapped) {
		netRulesSwap();
		s_NetSaved.swapped = 1;
	}
}

void netRulesConfigSaveEnd(void)
{
	if (s_NetSaved.client && s_NetSaved.swapped) {
		netRulesSwap();
		s_NetSaved.swapped = 0;
	}
}

void netRulesSetLocked(s32 locked)
{
	s_NetRulesLocked = locked;
}

s32 netRulesLocked(void)
{
	return s_NetRulesLocked;
}
