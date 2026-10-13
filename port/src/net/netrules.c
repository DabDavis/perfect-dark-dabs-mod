#include <stdio.h>
#include <stdlib.h>
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
#include "gebean.h"
#include "xblaagent4.h"
#include "game/challenge.h"
#include "game/savebuffer.h"
#include "game/modunlocks.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "game/mplayer/setup.h"
#include "game/lv.h"
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
 *   PLAYER   each player's own (protocol 20): a client keeps its value for
 *            the match and sends it in SLOTCFG, and the host plays that
 *            client's player by it (netPlayersOwnBegin around its
 *            playerTick, netSlotOwnS32 for a read outside it)
 *
 * A setting in none of them is each machine's own and never goes on the
 * wire: the picture and the sound (Disable Fog, Glass See-Through, the
 * Tranquilizer Effect's drugged screen, the camera's body fade...). The host
 * is the only machine that simulates, so a client's picture can differ from
 * the host's without the match going out of step.
 *
 * Mod.XblaMeshes is in none of them either (nor the Community Edition, built
 * into the HD look since 2026-10-10):
 * a look is each machine's own picture, and the data a look chooses at a
 * load (the Community Edition's copies, the release's collision) is the
 * host's on every machine (netcontent.c, NETLOOK_*).
 *
 * Mod.SimBrain is in none of them: simulants are ticked on the host alone
 * (a client's are puppets) and modern draws no random number (simnav.md),
 * so a host plays its own setting and a client goes stock
 * (simbrainWanted()).
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
	{ "Mod.GePlusPdGuns",            NETKEY_SYNC },
	// GoldenEye's cheat guns: the host's, put in step before its weapon set's
	// number is read (netRulesApplyCheatGuns)
	{ "Mod.GePlusCheatGuns",         NETKEY_SYNC },
	// a ROM hack's guns in Perfect Dark's Combat Simulator: the host's, put
	// in step before its weapon set's number is read (netRulesApplyCsHackGuns)
	{ "Mod.CsHackGuns",              NETKEY_SYNC },
	{ "Mod.GePlusRegion",            NETKEY_SYNC },
	{ "Mod.BorrowGoldenEyeGuns",     NETKEY_MUST },
	// read at a stage load (the setup and portals a conversion wrote both of)
	// and in the host's simulation, like the region: the host's for the
	// match. MUST_GE kept two players with it set differently out of each
	// other's GoldenEye rooms whoever hosted (F3 20261009-053550, -060337)
	{ "Mod.GePlusRevisionFixes",     NETKEY_SYNC },
	// a mission's guards (spec-coop.md): the host's, as the AI is
	{ "Mod.GuardsAlerted",           NETKEY_SYNC },
	{ "Mod.AlertedGuards",           NETKEY_SYNC },
	{ "Mod.GuardSpawnSpeed",         NETKEY_SYNC },
	{ "Mod.GuardWeapons",            NETKEY_SYNC },
	{ "Mod.MissionRespawn",          NETKEY_SYNC },
	{ "Mod.MissionLives",            NETKEY_SYNC },
	// the host's (pass 37, the owner): Skip Death Screen decides when a
	// dead player can respawn, so two machines with it set differently
	// disagreed about a death's timing; third person sees round corners
	// and over cover, so whether it is allowed (Mod.NetThirdPerson, online
	// only; stock: first person) and its camera (distance, clearance, side,
	// height, forward, tether) are the room's, one camera for everybody
	{ "Mod.SkipDeathScreen",         NETKEY_SYNC },
	{ "Mod.NetThirdPerson",          NETKEY_SYNC },
	{ "Mod.ThirdPersonDistance",     NETKEY_SYNC },
	{ "Mod.ThirdPersonClearance",    NETKEY_SYNC },
	{ "Mod.ThirdPersonSideways",     NETKEY_SYNC },
	{ "Mod.ThirdPersonForward",      NETKEY_SYNC },
	{ "Mod.ThirdPersonHeight",       NETKEY_SYNC },
	{ "Mod.ThirdPersonTether",       NETKEY_SYNC },
	// a player's own (protocol 20): how its controls and its gun behave,
	// read inside playerTick (the host swaps a client's in around its tick,
	// netPlayersOwnBegin) or through netSlotOwnS32, so the host simulates
	// the client's seat by them as the client predicts it (Quick Weapon
	// Swap's skipped animations included). The camera's tilt is the view's
	{ "Mod.CodAiming",               NETKEY_PLAYER },
	{ "Mod.CodAimLock",              NETKEY_PLAYER },
	{ "Mod.AkimboTriggers",          NETKEY_PLAYER },
	{ "Mod.QuickWeaponSwap",         NETKEY_PLAYER },
	{ "Mod.CameraTilt",              NETKEY_PLAYER },
	{ "Mod.InvertCameraTilt",        NETKEY_PLAYER },
	{ "Mod.ForwardAndBackTilt",      NETKEY_PLAYER },
};

/**
 * RULES as a client received it, kept from its arrival to STAGE_LOAD
 */
struct netrulessim {
	u8 on;
	u8 type;
	u8 difficulty;
	u8 mpheadnum; // the host's list places, and its rows (netReadMpCharRows)
	u8 mpbodynum;
	s16 headrow;
	s16 bodyrow;
	u8 team;
	u32 displayoptions;
	char name[15];
	s8 stats[BOTSTAT_COUNT];
};

struct netruleshuman {
	char name[15];
	u8 mpheadnum; // as netrulessim's
	u8 mpbodynum;
	s16 headrow;
	s16 bodyrow;
	u8 team;
	u32 displayoptions;
	u8 handicap;
	u16 options;
	u8 gunfuncs[6];
};

struct netrulesmsg {
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
	u32 cheats[2];            // protocol 28: the host's g_CheatsEnabledBank0/1
	struct netcooprules coop; // protocol 12: a mission (spec-coop.md)
	struct netcontentneed content; // protocol 13: the host's mod and ROM hack mode (netcontent.c)
	struct netkeyvalue keys[NET_MAXKEYS];
	s32 nkeys;
};

static struct netrulesmsg s_NetRules;

static void netRulesTestCheats(void);

/**
 * What a match changed, as it was before: a client's whole match state and
 * its own values of the SYNC keys; the host's player slots 1-11
 */
struct netrulessaved {
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
	const char *gexplusvariant; // g_GexPlusVariant (a static name, or NULL)
	s32 endless;
	s32 maxexplosions;
	u8 locktype;
	struct missionconfig mission; // a client's own mission settings (co-op)
	s32 difficulty;
	s32 coopradaron;
	s32 coopfriendlyfire;
	s32 numaibuddies;
	u8 mphilltime; // the scenario's own option (KOH's, its save bits in RULES)
	struct netkeyvalue keys[NET_MAXKEYS];
	s32 nkeys;
	s32 swapped; // H13: the own values are in while pd.ini is written
};

static struct netrulessaved s_NetSaved;

// A client: each seat's option bits as netRulesPut put them, so what the
// match itself changed in its own seat (the pause menu's Control page) can
// be told from what RULES brought (netRulesKeepSeatOptions)
static u16 s_PutOptions[MAX_PLAYERS];

// Host migration (netmigrate.c): the RULES of the match whose host went
// away, kept past its H12; and, on a machine that took the room over and
// plays on with them as its own setup, its own state until it stops hosting
static struct netrulesmsg s_Kept;
static struct netrulessaved s_Own;

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

/**
 * Mod.GePlusCheatGuns out of a list of keys, put in step at once
 * (gexPlusSetCheatGuns()): GoldenEye's cheat sets are three more in the
 * weapon set list, which a set's number counts by and Random Five, Random and
 * Custom follow the end of, so the list must be the host's before its number
 * is taken - and this machine's own again before its own is. Otherwise a
 * client with them off would read the host's Silver PP7s (26 after
 * GoldenEye's fourteen) as its own Random Five, and under Auto Random roll
 * guns of its own at the match's start. The keys loop after writes the same.
 */
static void netRulesApplyCheatGuns(const struct netkeyvalue *keys, s32 nkeys, const char *why)
{
	for (s32 i = 0; i < nkeys; i++) {
		if (strcasecmp(keys[i].key, "Mod.GePlusCheatGuns") == 0 && keys[i].type == CONFIG_TYPE_S32) {
			if (gexPlusGetCheatGuns() != (keys[i].s ? 1 : 0)) {
				sysLogPrintf(LOG_NOTE, "net: rules: Mod.GePlusCheatGuns %d -> %d (%s)", gexPlusGetCheatGuns(), keys[i].s ? 1 : 0, why);
			}

			gexPlusSetCheatGuns(keys[i].s);
			return;
		}
	}
}

/**
 * Mod.CsHackGuns out of a list of keys, put in step at once
 * (gexPlusSetCsHackGuns()): the hack it names puts its weapon sets in the
 * block GoldenEye's are in, which the set number counts in, and its guns in
 * GoldenEye's slots at the stage's load. One not mounted here (a guest
 * mounts or fetches the host's at STAGE_LOAD first:
 * netContentCsHackGunsFollow()) leaves GoldenEye's own, which is said.
 */
static void netRulesApplyCsHackGuns(const struct netkeyvalue *keys, s32 nkeys, const char *why)
{
	for (s32 i = 0; i < nkeys; i++) {
		if (strcasecmp(keys[i].key, "Mod.CsHackGuns") == 0 && keys[i].type == CONFIG_TYPE_STR) {
			if (strcmp(gexPlusGetCsHackGuns(), keys[i].str) != 0) {
				sysLogPrintf(LOG_NOTE, "net: rules: Mod.CsHackGuns \"%s\" -> \"%s\" (%s)", gexPlusGetCsHackGuns(), keys[i].str, why);
			}

			gexPlusSetCsHackGuns(keys[i].str);

			if (keys[i].str[0] && gexPlusCsHackDirIndex() < 0 && !g_GexPlusMode) {
				sysLogPrintf(LOG_WARNING, "net: rules: Mod.CsHackGuns \"%s\" (%s) is no ROM hack mounted here: GoldenEye's own guns in their place",
						keys[i].str, why);
			}

			return;
		}
	}
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
		if ((s_NetKeys[k].cls == NETKEY_MUST || s_NetKeys[k].cls == NETKEY_MUST_GE || s_NetKeys[k].cls == NETKEY_REFUSE)
				&& netRulesReadKey(s_NetKeys[k].key, &kv[n])) {
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

/**
 * A player's own settings (NETKEY_PLAYER), by key and value: SLOTCFG goes at
 * connect and again only when its bytes change, so a setting costs nothing
 * on the wire while it stays as it is. By key rather than position, so a
 * build with a key the other lacks reads past it.
 */
void netRulesWritePlayerKeys(struct netbuf *b)
{
	struct netkeyvalue kv[NET_MAXPLAYERKEYS];
	s32 n = 0;
	s32 i;
	u32 k;

	for (k = 0; k < ARRAYCOUNT(s_NetKeys) && n < NET_MAXPLAYERKEYS; k++) {
		if (s_NetKeys[k].cls == NETKEY_PLAYER && netRulesReadKey(s_NetKeys[k].key, &kv[n])) {
			n++;
		}
	}

	netBufWriteU8(b, (u8)n);

	for (i = 0; i < n; i++) {
		netWriteStr(b, kv[i].key, NET_MAXKEY);
		netRulesWriteValue(b, &kv[i]);
	}
}

/**
 * The host's copy of a client's: each key found among this machine's
 * NETKEY_PLAYER settings, its variable looked up once here rather than at
 * every tick, the value clamped to the setting's registered range. A key
 * this build does not list as a player's own, a string, a type that
 * differs and a float that is not a number are left out: the host plays
 * that one by its own value.
 */
void netRulesReadPlayerKeys(struct netbuf *b, struct netplayerkeys *out)
{
	struct netkeyvalue kv;
	s32 n = netBufReadU8(b);
	s32 i;

	memset(out, 0, sizeof(*out));

	for (i = 0; i < n && netBufOk(b); i++) {
		struct netplayerkey *pk;
		s32 type;
		void *ptr;

		memset(&kv, 0, sizeof(kv));
		netBufReadString(b, kv.key, sizeof(kv.key));
		netRulesReadValue(b, &kv);

		if (!netBufOk(b) || out->n >= NET_MAXPLAYERKEYS
				|| netRulesKeyClass(kv.key) != NETKEY_PLAYER
				|| !configGetEntry(kv.key, &type, &ptr, NULL)
				|| type != kv.type || type == CONFIG_TYPE_STR
				|| (type == CONFIG_TYPE_F32 && kv.f != kv.f)) {
			continue;
		}

		pk = &out->k[out->n++];
		pk->ptr = ptr;
		pk->type = type;

		switch (type) {
		case CONFIG_TYPE_S32: pk->v.s = kv.s; break;
		case CONFIG_TYPE_F32: pk->v.f = kv.f; break;
		default: pk->v.u = kv.u; break;
		}

		configClampValue(kv.key, &pk->v);
	}
}

static void netRulesPlayerKeyPut(const struct netplayerkey *pk)
{
	switch (pk->type) {
	case CONFIG_TYPE_S32: *(s32 *)pk->ptr = pk->v.s; break;
	case CONFIG_TYPE_F32: *(f32 *)pk->ptr = pk->v.f; break;
	default: *(u32 *)pk->ptr = pk->v.u; break;
	}
}

void netRulesPlayerKeysSwap(const struct netplayerkeys *in, struct netplayerkeys *saved)
{
	s32 i;

	saved->n = in->n;

	for (i = 0; i < in->n; i++) {
		struct netplayerkey *sk = &saved->k[i];

		sk->ptr = in->k[i].ptr;
		sk->type = in->k[i].type;

		switch (sk->type) {
		case CONFIG_TYPE_S32: sk->v.s = *(s32 *)sk->ptr; break;
		case CONFIG_TYPE_F32: sk->v.f = *(f32 *)sk->ptr; break;
		default: sk->v.u = *(u32 *)sk->ptr; break;
		}

		netRulesPlayerKeyPut(&in->k[i]);
	}
}

void netRulesPlayerKeysRestore(const struct netplayerkeys *saved)
{
	s32 i;

	for (i = saved->n - 1; i >= 0; i--) {
		netRulesPlayerKeyPut(&saved->k[i]);
	}
}

/*
 * F3's [netplay] section (nettrace.c)
 */

static void netRulesTraceKv(FILE *f, const struct netkeyvalue *kv)
{
	char buf[NET_MAXSTRVAL + 4];

	netRulesValueString(kv, buf, sizeof(buf));
	fprintf(f, " %s=%s", kv->key, buf);
}

// the match's rules as in force on this machine (a client's are the host's)
void netRulesTraceSync(FILE *f)
{
	struct netkeyvalue kv;
	u32 k;

	fprintf(f, "rules in force:");

	for (k = 0; k < ARRAYCOUNT(s_NetKeys); k++) {
		if (s_NetKeys[k].cls == NETKEY_SYNC && netRulesReadKey(s_NetKeys[k].key, &kv)) {
			netRulesTraceKv(f, &kv);
		}
	}

	fprintf(f, "\n");

	// the cheats played by (protocol 28: the host's everywhere) and this
	// machine's own: a cheat on one machine alone had the host's word put
	// its guns and ammo back all match
	fprintf(f, "cheats in force: %08x %08x; this machine's own enabled %08x %08x\n",
			g_CheatsActiveBank0, g_CheatsActiveBank1, g_CheatsEnabledBank0, g_CheatsEnabledBank1);
}

// Room Settings (netlobbymenu.c): did the Room Rules page change a rule
u32 netRulesSyncHash(void)
{
	struct netkeyvalue kv;
	char buf[NET_MAXSTRVAL + 4];
	u32 h = 2166136261u;
	u32 k;
	const char *c;

	for (k = 0; k < ARRAYCOUNT(s_NetKeys); k++) {
		if (s_NetKeys[k].cls == NETKEY_SYNC && netRulesReadKey(s_NetKeys[k].key, &kv)) {
			netRulesValueString(&kv, buf, sizeof(buf));

			for (c = buf; *c; c++) {
				h = (h ^ (u8)*c) * 16777619u;
			}

			h = (h ^ ';') * 16777619u;
		}
	}

	// and the simulants' AI, the host's alone (no key class: never sent)
	if (netRulesReadKey("Mod.SimBrain", &kv)) {
		netRulesValueString(&kv, buf, sizeof(buf));

		for (c = buf; *c; c++) {
			h = (h ^ (u8)*c) * 16777619u;
		}
	}

	return h;
}

// this machine's own NETKEY_PLAYER values (what its SLOTCFG says)
void netRulesTraceOwnHere(FILE *f)
{
	struct netkeyvalue kv;
	u32 k;

	fprintf(f, "own settings here:");

	for (k = 0; k < ARRAYCOUNT(s_NetKeys); k++) {
		if (s_NetKeys[k].cls == NETKEY_PLAYER && netRulesReadKey(s_NetKeys[k].key, &kv)) {
			netRulesTraceKv(f, &kv);
		}
	}

	fprintf(f, "\n");
}

// a remote player's own values as the host keeps them, by key
void netRulesTracePlayerKeys(FILE *f, const struct netplayerkeys *in)
{
	struct netkeyvalue kv;
	s32 type;
	void *ptr;
	u32 k;
	s32 i;

	for (k = 0; k < ARRAYCOUNT(s_NetKeys); k++) {
		if (s_NetKeys[k].cls != NETKEY_PLAYER || !configGetEntry(s_NetKeys[k].key, &type, &ptr, NULL)) {
			continue;
		}

		for (i = 0; i < in->n; i++) {
			if (in->k[i].ptr == ptr) {
				memset(&kv, 0, sizeof(kv));
				snprintf(kv.key, sizeof(kv.key), "%s", s_NetKeys[k].key);
				kv.type = in->k[i].type;
				kv.s = in->k[i].v.s;
				kv.f = in->k[i].v.f;
				kv.u = in->k[i].v.u;
				netRulesTraceKv(f, &kv);
			}
		}
	}
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

		if (cls < 0 || cls == NETKEY_SYNC || cls == NETKEY_PLAYER) {
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
 * A character by its rows (protocol 15)
 *
 * The Combat Simulator's head and body lists are not the same on every
 * machine: Agent 4 is appended where the XBLA release is unpacked (and so
 * wherever the XBLA look was ever switched on), ahead of GoldenEye's
 * characters, which then stand one further on. An index is sent with the row
 * it names, and the reader takes its own index of that row. An index past the
 * list (the personal heads, the hidden bodies) is sent as how far past it is.
 * A row the reader does not list is Agent 4's (his N64 look, the Shock
 * Trooper) or else the index as sent.
 */

static s32 netMpHeadRow(s32 mpheadnum)
{
	return mpheadnum < mpGetNumHeads() ? mpGetHeadId(mpheadnum) : -1 - (mpheadnum - mpGetNumHeads());
}

static s32 netMpBodyRow(s32 mpbodynum)
{
	return mpbodynum < (s32)mpGetNumBodies() ? g_MpBodies[mpbodynum].bodynum : -1 - (mpbodynum - (s32)mpGetNumBodies());
}

static s32 netMpHeadIndex(s32 row, s32 sent)
{
	s32 i;

	if (row < 0) {
		return mpGetNumHeads() + (-1 - row);
	}

	if (row == XBLA_AGENT4_HEADROW && !xblaAgent4IsListed()) {
		row = HEAD_DDSHOCK;
	}

	for (i = 0; i < mpGetNumHeads(); i++) {
		if (g_MpHeads[i].headnum == row) {
			return i;
		}
	}

	// a row not in this machine's list (GoldenEye's characters with no
	// conversion here): the host's place is someone else here, or past the
	// list's end
	return sent < mpGetNumHeads() ? sent : 0;
}

static s32 netMpBodyIndex(s32 row, s32 sent)
{
	s32 i;

	if (row < 0) {
		return (s32)mpGetNumBodies() + (-1 - row);
	}

	if (row == XBLA_AGENT4_BODYROW && !xblaAgent4IsListed()) {
		row = BODY_DDSHOCK;
	}

	for (i = 0; i < (s32)mpGetNumBodies(); i++) {
		if (g_MpBodies[i].bodynum == row) {
			return i;
		}
	}

	// as a head's: past this list's end is Dr. Caroll (one past) or Dark
	// Combat (mpGetBodyId()), and Dr. Caroll's skeleton has no hand for a
	// gun (a Goldfinger 64 room's guest with no GoldenEye conversion, its
	// puppets unarmed, netcontenttest gffetch 2026-10-08)
	return sent < (s32)mpGetNumBodies() ? sent : 0;
}

void netWriteMpChar(struct netbuf *b, s32 mpheadnum, s32 mpbodynum)
{
	netBufWriteU8(b, (u8)mpheadnum);
	netBufWriteU8(b, (u8)mpbodynum);
	netBufWriteS16(b, (s16)netMpHeadRow(mpheadnum));
	netBufWriteS16(b, (s16)netMpBodyRow(mpbodynum));
}

// this machine's list places for the host's rows
static void netMpCharResolve(s32 head, s32 body, s32 headrow, s32 bodyrow, u8 *mpheadnum, u8 *mpbodynum)
{
	const s32 h = netMpHeadIndex(headrow, head);
	const s32 d = netMpBodyIndex(bodyrow, body);

	*mpheadnum = (u8)(h >= 0 && h <= 0xff ? h : head);
	*mpbodynum = (u8)(d >= 0 && d <= 0xff ? d : body);
}

void netReadMpChar(struct netbuf *b, u8 *mpheadnum, u8 *mpbodynum)
{
	const s32 head = netBufReadU8(b);
	const s32 body = netBufReadU8(b);
	const s32 headrow = netBufReadS16(b);
	const s32 bodyrow = netBufReadS16(b);

	netMpCharResolve(head, body, headrow, bodyrow, mpheadnum, mpbodynum);
}

/**
 * RULES' characters, kept as the host sent them: its places and rows. They
 * are put in this machine's places when the rules are applied, after
 * STAGE_LOAD's content is here: GoldenEye's characters (gebean.c's pool)
 * are rows that only a machine with GoldenEye's conversion lists, and a
 * guest mounts or fetches it first (netContentGeCharsFollow())
 */
static void netReadMpCharRows(struct netbuf *b, u8 *mpheadnum, u8 *mpbodynum, s16 *headrow, s16 *bodyrow)
{
	*mpheadnum = netBufReadU8(b);
	*mpbodynum = netBufReadU8(b);
	*headrow = netBufReadS16(b);
	*bodyrow = netBufReadS16(b);
}

// a row this machine's lists lack (a positive row: the list's own tail
// rows travel as negative offsets): 1 a head's, 2 a body's, 3 both
static s32 netMpRowMissing(s32 headrow, s32 bodyrow)
{
	s32 missing = 0;
	s32 i;

	if (bodyrow >= 0) {
		for (i = 0; i < (s32)mpGetNumBodies() && g_MpBodies[i].bodynum != bodyrow; i++);

		if (i == (s32)mpGetNumBodies() && !(bodyrow == XBLA_AGENT4_BODYROW && !xblaAgent4IsListed())) {
			missing |= 2;
		}
	}

	if (headrow >= 0) {
		for (i = 0; i < mpGetNumHeads() && g_MpHeads[i].headnum != headrow; i++);

		if (i == mpGetNumHeads() && !(headrow == XBLA_AGENT4_HEADROW && !xblaAgent4IsListed())) {
			missing |= 1;
		}
	}

	return missing;
}

// "sim 2 0x1a3/0x1b0" onto a list of the missing, while it fits
static void netMpRowNote(char *rows, s32 size, const char *who, s32 i, s32 headrow, s32 bodyrow)
{
	const s32 len = rows ? (s32)strlen(rows) : 0;

	if (rows && len < size - 1) {
		snprintf(rows + len, size - len, "%s%s %d body 0x%x head 0x%x", len ? ", " : "", who, i, bodyrow, headrow);
	}
}

/**
 * The host's value of a key in the RULES this client holds for its next
 * STAGE_LOAD, before netRulesApply() writes them (the content a match needs is
 * followed first: netContentCsHackGunsFollow()); 1 when it sent one. And
 * whether the host is in GoldenEye's own Combat Simulator.
 */
s32 netRulesHostKey(const char *key, struct netkeyvalue *out)
{
	for (s32 i = 0; i < s_NetRules.nkeys && i < NET_MAXKEYS; i++) {
		if (strcasecmp(s_NetRules.keys[i].key, key) == 0) {
			*out = s_NetRules.keys[i];
			return 1;
		}
	}

	return 0;
}

s32 netRulesHostGexPlusMode(void)
{
	return s_NetRules.gexplusmode != 0;
}

/**
 * Client: how many of the RULES' sims and players wear GoldenEye's
 * characters (rows from GEBEAN_POOL_BASE) this machine's lists lack; rows
 * (if not NULL) names them, and bodies (if not NULL) counts those whose
 * body is missing - a head missing is a face, a body the skeleton
 */
s32 netRulesGeCharsMissing(char *rows, s32 size, s32 *bodies)
{
	const struct netrulesmsg *r = &s_NetRules;
	s32 n = 0;
	s32 m;
	s32 i;

	if (rows && size > 0) {
		rows[0] = '\0';
	}

	if (bodies) {
		*bodies = 0;
	}

	if (!r->valid) {
		return 0;
	}

	for (i = 0; i < MAX_BOTS; i++) {
		const struct netrulessim *sim = &r->sims[i];

		if (r->simslots[i] && sim->on && (sim->bodyrow >= GEBEAN_POOL_BASE || sim->headrow >= GEBEAN_POOL_BASE)
				&& (m = netMpRowMissing(sim->headrow, sim->bodyrow)) != 0) {
			netMpRowNote(rows, size, "sim", i, sim->headrow, sim->bodyrow);
			n++;

			if (bodies && (m & 2)) {
				(*bodies)++;
			}
		}
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		const struct netruleshuman *h = &r->humans[i];
		const s32 on = i < MPSETUP_HUMANBITS ? (r->chrslots >> i) & 1 : (r->humanslotshi >> (i - MPSETUP_HUMANBITS)) & 1;

		if (on && (h->bodyrow >= GEBEAN_POOL_BASE || h->headrow >= GEBEAN_POOL_BASE) && (m = netMpRowMissing(h->headrow, h->bodyrow)) != 0) {
			netMpRowNote(rows, size, "player", i, h->headrow, h->bodyrow);
			n++;

			if (bodies && (m & 2)) {
				(*bodies)++;
			}
		}
	}

	return n;
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
		netWriteMpChar(b, bot->base.mpheadnum, bot->base.mpbodynum);
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
		netWriteMpChar(b, p->base.mpheadnum, p->base.mpbodynum);
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

	// the mission (protocol 12): what the host set before H1 (netcoop.c)
	netBufWriteU8(b, (u8)(netCoopHostMatch() != 0));
	netBufWriteU8(b, (u8)g_MissionConfig.stageindex);
	netBufWriteU8(b, (u8)g_MissionConfig.difficulty);
	netBufWriteU8(b, (u8)(g_Vars.coopradaron != 0));
	netBufWriteU8(b, (u8)(g_Vars.coopfriendlyfire != 0));
	netWriteStr(b, netCoopHostGame(), NET_MAXCOMPNAME); // protocol 14: the mission's set

	{
		// protocol 27: the Cinema page's cinema the match is, watched by the room
		u8 kind;
		u8 loop;
		u8 minutes;

		netCoopHostCinema(&kind, &loop, &minutes);
		netBufWriteU8(b, kind);
		netBufWriteU8(b, loop);
		netBufWriteU8(b, minutes);
	}

	// protocol 32: a counter-op mission and its sides (netcoop.c)
	netBufWriteU8(b, (u8)(netCoopHostAnti() != 0));
	netBufWriteU16(b, netCoopHostAntiMask());

	{
		// the content block (protocol 13, netcontent.c): the mod and the
		// ROM hack mode the client is to play this match in
		struct netcontentneed need;

		netContentHostNeed(&need);
		netContentWrite(b, &need, 1);
	}

	nkeysat = netBufReserve(b, 1);

	for (k = 0; k < ARRAYCOUNT(s_NetKeys) && nkeys < NET_MAXKEYS; k++) {
		if (s_NetKeys[k].cls == NETKEY_SYNC && netRulesReadKey(s_NetKeys[k].key, &kv)) {
			// the ROM hack guns the host plays with, not one its pd.ini
			// names that is not mounted here (its matches have GoldenEye's
			// then, and a guest would fetch and play the hack's)
			if (strcasecmp(kv.key, "Mod.CsHackGuns") == 0 && gexPlusCsHackDirIndex() < 0) {
				kv.str[0] = '\0';
			}

			netWriteStr(b, kv.key, NET_MAXKEY);
			netRulesWriteValue(b, &kv);
			nkeys++;
		}
	}

	if (nkeysat) {
		*nkeysat = (u8)nkeys;
	}

	netWriteStr(b, "", NET_MAXCOMPNAME);

	// (protocol 28) the cheats the match plays by: the host's
	netRulesTestCheats();
	netBufWriteU32(b, g_CheatsEnabledBank0);
	netBufWriteU32(b, g_CheatsEnabledBank1);
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
	netTextPrintable(s_NetRules.name, 0);
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
		netReadMpCharRows(b, &sim.mpheadnum, &sim.mpbodynum, &sim.headrow, &sim.bodyrow);
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
		netReadMpCharRows(b, &h->mpheadnum, &h->mpbodynum, &h->headrow, &h->bodyrow);
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
		netTextPrintable(s_NetRules.teamnames[i], 0);
	}

	for (i = 0; i < 80; i++) {
		s_NetRules.unlocked[i] = netBufReadU8(b);
	}

	s_NetRules.modunlocks = netBufReadU32(b);
	s_NetRules.gexplusmode = netBufReadU8(b);
	s_NetRules.gexplusscenario = netBufReadU8(b);
	s_NetRules.endless = netBufReadU8(b);
	s_NetRules.maxexplosions = netBufReadS32(b);
	s_NetRules.coop.on = netBufReadU8(b);
	s_NetRules.coop.stageindex = netBufReadU8(b);
	s_NetRules.coop.difficulty = netBufReadU8(b);
	s_NetRules.coop.radar = netBufReadU8(b);
	s_NetRules.coop.friendlyfire = netBufReadU8(b);
	netBufReadString(b, s_NetRules.coop.game, sizeof(s_NetRules.coop.game));
	s_NetRules.coop.cinema = netBufReadU8(b);
	s_NetRules.coop.cinemaloop = netBufReadU8(b);
	s_NetRules.coop.cinemaminutes = netBufReadU8(b);
	s_NetRules.coop.anti = netBufReadU8(b);
	s_NetRules.coop.antimask = netBufReadU16(b);
	netContentRead(b, &s_NetRules.content, 1);

	if (s_NetRules.coop.on && !netCoopRulesOk(&s_NetRules.coop)) {
		b->error = 1;
	}
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
	s_NetRules.cheats[0] = netBufReadU32(b);
	s_NetRules.cheats[1] = netBufReadU32(b);

	if (s_NetRules.scenario > MPSCENARIO_CAPTURETHECASE || s_NetRules.timelimit > 60 || s_NetRules.scorelimit > 100) {
		b->error = 1;
	}

	// what the game later uses as an index or an allocation's size
	for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		if (s_NetRules.weapons[i] >= NUM_MPWEAPONS) {
			b->error = 1;
		}
	}

	// the set's number is the host's: its Random and Custom follow however
	// many sets its own list holds (a PD mod's, GoldenEye's appended), so
	// this machine's WEAPONSET_CUSTOM is no bound for it - a guest inside the
	// host's Perfect Dark mod lacked GoldenEye's fourteen and refused the
	// host's Custom (28) as past its own (14): "the host's RULES did not
	// parse" between two matches (F3 20261009-215118); netRulesApply puts it
	// in this machine's list
	if (s_NetRules.weaponsetnum < 0 || s_NetRules.weaponsetnum > MP_MAX_WEAPONSETS + 2
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

const struct netcontentneed *netRulesContent(void)
{
	return &s_NetRules.content;
}

/**
 * --net-test-cheats HEX: this machine's own enabled cheats (bank 0) for the
 * gates, as the Cheats menu would have set them (netpredicttest's cheats)
 */
static void netRulesTestCheats(void)
{
	const char *arg = sysArgGetString("--net-test-cheats");

	if (arg) {
		g_CheatsEnabledBank0 = (u32)strtoul(arg, NULL, 0);
	}
}

s32 netCheatsEnabled(u32 *bank0, u32 *bank1)
{
	netRulesTestCheats();

	if (g_NetMode != NETMODE_CLIENT || !s_NetRules.valid) {
		*bank0 = g_CheatsEnabledBank0;
		*bank1 = g_CheatsEnabledBank1;
		return 0;
	}

	if (g_CheatsEnabledBank0 != s_NetRules.cheats[0] || g_CheatsEnabledBank1 != s_NetRules.cheats[1]) {
		sysLogPrintf(LOG_NOTE, "net: cheats: the host's %08x %08x in force, not this machine's own %08x %08x",
				s_NetRules.cheats[0], s_NetRules.cheats[1], g_CheatsEnabledBank0, g_CheatsEnabledBank1);
	}

	*bank0 = s_NetRules.cheats[0];
	*bank1 = s_NetRules.cheats[1];

	return 1;
}

// the RULES received say a co-op mission (0 a match, or none received)
s32 netRulesCoopOn(void)
{
	return s_NetRules.valid && s_NetRules.coop.on;
}

/*
 * Applying and restoring
 */

static void netRulesSaveInto(struct netrulessaved *out)
{
	u32 k;

	out->mpsetup = g_MpSetup;
	memcpy(out->bots, g_BotConfigsArray, sizeof(out->bots));
	memcpy(out->simslots, g_MpSimSlots, sizeof(out->simslots));
	memcpy(out->difficulties, g_MpSimulantDifficultiesPerNumPlayers, sizeof(out->difficulties));
	memcpy(out->players, g_PlayerConfigsArray, sizeof(out->players));
	memcpy(out->teamnames, g_BossFile.teamnames, sizeof(out->teamnames));
	out->weaponsetnum = g_MpWeaponSetNum;
	memcpy(out->filters, g_MpWeaponSetRandomFilters, sizeof(out->filters));
	memcpy(out->unlocked, g_MpFeaturesUnlocked, sizeof(out->unlocked));
	out->modunlocks = g_ModUnlocks;
	out->gexplusmode = g_GexPlusMode;
	out->gexplusscenario = gexPlusGetScenario();
	out->gexplusvariant = g_GexPlusVariant;
	out->endless = g_MpEndlessMatch;
	out->maxexplosions = g_MaxExplosionsSetting;
	out->locktype = g_BossFile.locktype;
	out->lockinfo = g_MpLockInfo;
	out->mission = g_MissionConfig;
	out->difficulty = lvGetDifficulty();
	out->coopradaron = g_Vars.coopradaron;
	out->coopfriendlyfire = g_Vars.coopfriendlyfire;
	out->numaibuddies = g_Vars.numaibuddies;
	out->mphilltime = g_Vars.mphilltime;

	out->nkeys = 0;

	for (k = 0; k < ARRAYCOUNT(s_NetKeys); k++) {
		if (s_NetKeys[k].cls == NETKEY_SYNC
				&& netRulesReadKey(s_NetKeys[k].key, &out->keys[out->nkeys])) {
			out->nkeys++;
		}
	}

	// and Simulant AI, a Room Rule the host alone plays by (a client never
	// changes its own: simbrainWanted())
	if (out->nkeys < NET_MAXKEYS && netRulesReadKey("Mod.SimBrain", &out->keys[out->nkeys])) {
		out->nkeys++;
	}

	out->client = 1;
	out->swapped = 0;
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

static void netRulesPut(const struct netrulesmsg *rules, s32 coop);

/**
 * A client under the host's rules: its own first player's config as it was
 * before them (netRulesPut puts the host's slot 0 human over
 * g_PlayerConfigsArray[0]); NULL when its own setup is in place
 */
struct mpplayerconfig *netRulesOwnPlayer(void)
{
	return s_NetSaved.client ? &s_NetSaved.players[0] : NULL;
}

/**
 * H3: a client takes the host's rules over its own for the match. The GE
 * scenario first: its setter overwrites g_MpSetup.scenario.
 */
void netRulesApply(void)
{
	if (!s_NetRules.valid) {
		return;
	}

	if (!s_NetSaved.client) {
		netRulesSaveInto(&s_NetSaved);
	}

	s_NetRulesAppliedId = s_NetRules.matchid;
	netRulesPut(&s_NetRules, 1);
}

/**
 * The rules over this machine's own setup (netRulesApply's, and a migrated
 * host's adoption of the last match's); coop takes a mission block's
 * settings as a client
 */
static void netRulesPut(const struct netrulesmsg *rules, s32 coop)
{
	struct savebuffer sb;
	char a[NET_MAXSTRVAL + 4];
	char b[NET_MAXSTRVAL + 4];
	struct netkeyvalue mine;
	s32 i;
	s32 j;

	// the mode first, through its setter: a ROM hack's mode (protocol 13)
	// puts the hack's weapon sets in the list's block, which the set number
	// and weapons below index (gexplus.c); the setter's own choice of set
	// and scenario is overwritten by the host's right after
	if (!netContentVariantApply(rules->content.gevariant)) {
		sysLogPrintf(LOG_WARNING, "net: rules: the host's ROM hack mode \"%s\" is not converted here", rules->content.gevariant);
	}

	mpSetGexPlusMode(rules->gexplusmode != 0);
	gexPlusSetScenario(rules->gexplusscenario);

	snprintf(g_MpSetup.name, sizeof(g_MpSetup.name), "%s", rules->name);
	g_MpSetup.options = rules->options;
	g_MpSetup.scenario = rules->scenario;
	g_MpSetup.timelimit = rules->timelimit;
	g_MpSetup.scorelimit = rules->scorelimit;
	g_MpSetup.teamscorelimit = rules->teamscorelimit;

	netRulesApplyCheatGuns(rules->keys, rules->nkeys, "the host's, for the match");
	netRulesApplyCsHackGuns(rules->keys, rules->nkeys, "the host's, for the match");

	for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		g_MpSetup.weapons[i] = rules->weapons[i];
	}

	savebufferClear(&sb);
	savebufferOr(&sb, rules->scenariobits, 32);
	sb.bitpos = 0;
	scenarioReadSave(&sb, 1);

	// the host's set by its own list's numbering (netRulesRead): one past
	// this machine's list is its guns as RULES give them, which is Custom
	// here (the slots above are the host's whatever the set is called)
	g_MpWeaponSetNum = rules->weaponsetnum;

	if (g_MpWeaponSetNum > WEAPONSET_CUSTOM) {
		sysLogPrintf(LOG_NOTE, "net: rules: the host's weapon set %d is past this machine's list of %d: its guns as sent, Custom here",
				rules->weaponsetnum, g_MpNumWeaponSets);
		g_MpWeaponSetNum = WEAPONSET_CUSTOM;
	}

	for (i = 0; i < NUM_MPWEAPONS && i < 64; i++) {
		g_MpWeaponSetRandomFilters[i] = (rules->filters >> i) & 1;
	}

	for (i = 0; i < MAX_BOTS; i++) {
		if (rules->sims[i].on) {
			struct mpbotconfig *bot = &g_BotConfigsArray[i];
			const struct netrulessim *sim = &rules->sims[i];
			u8 head;
			u8 body;

			netMpCharResolve(sim->mpheadnum, sim->mpbodynum, sim->headrow, sim->bodyrow, &head, &body);
			bot->type = sim->type;
			bot->difficulty = sim->difficulty;
			bot->base.mpheadnum = mpHeadNumSafe(head);
			bot->base.mpbodynum = body <= mpGetNumBodies() + 1 ? body : 0;
			bot->base.team = sim->team;
			bot->base.displayoptions = sim->displayoptions;
			netNameSet(bot->base.name, sizeof(bot->base.name), sim->name);

			for (j = 0; j < BOTSTAT_COUNT; j++) {
				bot->stats[j] = sim->stats[j];
			}
		}

		mpSetSimSlotOn(i, rules->simslots[i]);
	}

	memcpy(g_MpSimulantDifficultiesPerNumPlayers, rules->difficulties, sizeof(rules->difficulties));

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct mpplayerconfig *p = &g_PlayerConfigsArray[i];
		const struct netruleshuman *h = &rules->humans[i];
		u8 head;
		u8 body;

		netMpCharResolve(h->mpheadnum, h->mpbodynum, h->headrow, h->bodyrow, &head, &body);
		netNameSet(p->base.name, sizeof(p->base.name), h->name);
		p->base.mpheadnum = mpHeadNumSafe(head);
		p->base.mpbodynum = body <= mpGetNumBodies() + 1 ? body : 0;
		p->base.team = h->team;
		p->base.displayoptions = h->displayoptions;
		p->handicap = h->handicap;
		p->options = h->options;
		s_PutOptions[i] = h->options;

		for (j = 0; j < 6; j++) {
			p->gunfuncs[j] = h->gunfuncs[j];
		}

		// a profile is never saved from a net match (menutick.c's endscreen)
		memset(&p->fileguid, 0, sizeof(p->fileguid));
	}

	// the host's chrslots last: mpSetSimSlotOn mirrored the sims into it
	g_MpSetup.chrslots = rules->chrslots;
	g_MpHumanSlotsHi = (u8)rules->humanslotshi;

	for (i = 0; i < MAX_TEAMS; i++) {
		snprintf(g_BossFile.teamnames[i], sizeof(g_BossFile.teamnames[i]), "%s", rules->teamnames[i]);
	}

	memcpy(g_MpFeaturesUnlocked, rules->unlocked, sizeof(rules->unlocked));
	g_ModUnlocks = rules->modunlocks;
	g_MpEndlessMatch = rules->endless;

	// puppet explosions must fit as many as the host makes
	if (rules->maxexplosions > g_MaxExplosionsSetting) {
		g_MaxExplosionsSetting = rules->maxexplosions;
	}

	// a challenge is never marked complete from a net match
	if (g_BossFile.locktype == MPLOCKTYPE_CHALLENGE) {
		g_BossFile.locktype = MPLOCKTYPE_NONE;
	}

	// a mission's settings (spec-coop.md): the host's mission and difficulty
	if (rules->coop.on && coop) {
		netCoopClientApplyRules(&rules->coop);
	}

	for (i = 0; i < rules->nkeys; i++) {
		if (netRulesReadKey(rules->keys[i].key, &mine) && !netRulesValuesEqual(&mine, &rules->keys[i])) {
			netRulesValueString(&mine, a, sizeof(a));
			netRulesValueString(&rules->keys[i], b, sizeof(b));
			sysLogPrintf(LOG_NOTE, "net: rules: %s %s -> %s (the host's, for the match)", rules->keys[i].key, a, b);
		}

		netRulesWriteKey(&rules->keys[i]);
	}

	sysLogPrintf(LOG_NOTE, "net: rules applied: match %u, scenario %d, chrslots 0x%04x, human slots 4-11 0x%02x, options 0x%08x, %d keys",
			rules->matchid, g_MpSetup.scenario, g_MpSetup.chrslots, g_MpHumanSlotsHi, g_MpSetup.options, rules->nkeys);

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (mpIsHumanSlotOn(i)) {
			const char *name = g_PlayerConfigsArray[i].base.name;
			const s32 len = netNameLen(name);

			// (a name without its newline draws with no height: netNameSet)
			sysLogPrintf(LOG_NOTE, "net: the match's players: \"%.*s\" in slot %d%s", len, name, i,
					len > 0 && name[len] != '\n' ? " (its name has no newline)" : "");
		}
	}
}

/**
 * A client whose match is over, before netRulesRestore: what its pause menu
 * changed in its own seat during the match (Reverse Pitch, Look Ahead, Auto
 * Aim, ... - the Control and Player Options pages edit the seat's config,
 * ingame.c) goes into its own player 1 as saved, so the restore does not
 * undo it. Before, a Reverse Pitch ticked mid-match was unticked again at
 * the match's end and the next match played without it (F3 20261012-225041:
 * "randomly untick reverse tilt ... leaving and joining"). Only the bits
 * that changed since RULES put them are taken: in GoldenEye mode the seat's
 * Reverse Pitch bit is GoldenEye's LOOK UP/DOWN (netWriteSlotCfg), which is
 * not the player's own Perfect Dark setting.
 */
void netRulesKeepSeatOptions(s32 slot)
{
	u16 changed;
	u16 cur;

	if (!s_NetSaved.client || slot < 0 || slot >= MAX_PLAYERS) {
		return;
	}

	cur = g_PlayerConfigsArray[slot].options;
	changed = cur ^ s_PutOptions[slot];

	if (changed) {
		struct mpplayerconfig *own = &s_NetSaved.players[0];

		own->options = (own->options & ~changed) | (cur & changed);
		sysLogPrintf(LOG_NOTE, "net: rules: the match changed this machine's options 0x%04x -> 0x%04x in seat %d; kept for its own player 1 (options 0x%04x, forward pitch %d)",
				s_PutOptions[slot], cur, slot, own->options, (own->options & OPTION_FORWARDPITCH) != 0);
	}
}

static void netRulesRestoreFrom(const struct netrulessaved *saved);

/**
 * H12: everything a net match changed goes back. A client's whole state; on
 * the host only its slots 1-11, which the remote players' names went into.
 */
void netRulesRestore(void)
{
	s32 i;

	if (s_NetSaved.client) {
		netRulesRestoreFrom(&s_NetSaved);
		sysLogPrintf(LOG_NOTE, "net: rules restored: this machine's own setup and settings are back (player 1 options 0x%04x, forward pitch %d)",
				g_PlayerConfigsArray[0].options, (g_PlayerConfigsArray[0].options & OPTION_FORWARDPITCH) != 0);
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

// A client's whole state back as it was saved (netRulesSaveInto)
static void netRulesRestoreFrom(const struct netrulessaved *saved)
{
	s32 i;

	g_GexPlusVariant = saved->gexplusvariant;
	mpSetGexPlusMode(saved->gexplusmode != 0);
	gexPlusSetScenario(saved->gexplusscenario);
	netRulesApplyCheatGuns(saved->keys, saved->nkeys, "this machine's own again");
	netRulesApplyCsHackGuns(saved->keys, saved->nkeys, "this machine's own again");
	g_MpSetup = saved->mpsetup;
	memcpy(g_BotConfigsArray, saved->bots, sizeof(saved->bots));
	memcpy(g_MpSimSlots, saved->simslots, sizeof(saved->simslots));
	memcpy(g_MpSimulantDifficultiesPerNumPlayers, saved->difficulties, sizeof(saved->difficulties));
	memcpy(g_PlayerConfigsArray, saved->players, sizeof(saved->players));
	memcpy(g_BossFile.teamnames, saved->teamnames, sizeof(saved->teamnames));
	g_MpWeaponSetNum = saved->weaponsetnum;
	memcpy(g_MpWeaponSetRandomFilters, saved->filters, sizeof(saved->filters));
	memcpy(g_MpFeaturesUnlocked, saved->unlocked, sizeof(saved->unlocked));
	g_ModUnlocks = saved->modunlocks;
	g_MpEndlessMatch = saved->endless;
	g_MaxExplosionsSetting = saved->maxexplosions;
	g_BossFile.locktype = saved->locktype;
	g_MpLockInfo = saved->lockinfo;
	g_MpHumanSlotsHi = 0;
	g_MissionConfig = saved->mission;
	lvSetDifficulty(saved->difficulty);
	g_Vars.coopradaron = saved->coopradaron;
	g_Vars.coopfriendlyfire = saved->coopfriendlyfire;
	g_Vars.numaibuddies = saved->numaibuddies;
	g_Vars.mphilltime = saved->mphilltime;

	for (i = 0; i < saved->nkeys; i++) {
		netRulesWriteKey(&saved->keys[i]);
	}

	// the unlock table is the profile's again
	challengeDetermineUnlockedFeatures();
}

/**
 * H13: while pd.ini is written the player's own values are in, then the
 * host's go back for the rest of the match
 */
static void netRulesSwap(struct netrulessaved *saved)
{
	struct netkeyvalue cur;
	s32 maxexplosions = g_MaxExplosionsSetting;
	s32 i;

	// Game.MaxExplosions too: a client raised it to the host's
	g_MaxExplosionsSetting = saved->maxexplosions;
	saved->maxexplosions = maxexplosions;

	for (i = 0; i < saved->nkeys; i++) {
		if (netRulesReadKey(saved->keys[i].key, &cur)) {
			netRulesWriteKey(&saved->keys[i]);
			saved->keys[i] = cur;
		}
	}
}

// a client's match, or a migrated host's adopted rules (s_Own), whose own
// values go into pd.ini
static struct netrulessaved *netRulesOwnValues(void)
{
	return s_NetSaved.client ? &s_NetSaved : s_Own.client ? &s_Own : NULL;
}

void netRulesConfigSaveBegin(void)
{
	struct netrulessaved *saved = netRulesOwnValues();

	netSessionNameSaveSwap(1);

	if (saved && !saved->swapped) {
		netRulesSwap(saved);
		saved->swapped = 1;
	}
}

void netRulesConfigSaveEnd(void)
{
	struct netrulessaved *saved = netRulesOwnValues();

	netSessionNameSaveSwap(0);

	if (saved && saved->swapped) {
		netRulesSwap(saved);
		saved->swapped = 0;
	}
}

/*
 * Host migration (netmigrate.c)
 */

/**
 * A client whose host went away mid-match: the match's RULES, as it took
 * them at H3, kept past the H12 that is coming (which forgets them)
 */
s32 netRulesKeep(void)
{
	if (!s_NetRules.valid || s_NetRules.matchid != s_NetRulesAppliedId) {
		s_Kept.valid = 0;
		return 0;
	}

	s_Kept = s_NetRules;
	return 1;
}

void netRulesKeptForget(void)
{
	s_Kept.valid = 0;
}

// The kept match's mission block (a co-op mission's), or NULL
const struct netcooprules *netRulesKeptCoop(void)
{
	return s_Kept.valid && s_Kept.coop.on ? &s_Kept.coop : NULL;
}

// The kept match's content: the mod and the ROM hack mode it was played in
const struct netcontentneed *netRulesKeptContent(void)
{
	return s_Kept.valid ? &s_Kept.content : NULL;
}

/**
 * This machine took the room over: the kept match's rules become its own
 * setup, the room's from here, with this machine's player in slot 0 and the
 * old host's in `swapslot` (this machine's in the kept match). Its own setup
 * and settings are saved first (once) and come back at netRulesAdoptEnd; a
 * pd.ini write meanwhile writes its own values (H13).
 */
s32 netRulesAdopt(s32 swapslot)
{
	struct netruleshuman t;

	if (!s_Kept.valid || swapslot < 0 || swapslot >= MAX_PLAYERS) {
		return 0;
	}

	if (!s_Own.client) {
		netRulesSaveInto(&s_Own);
	}

	t = s_Kept.humans[0];
	s_Kept.humans[0] = s_Kept.humans[swapslot];
	s_Kept.humans[swapslot] = t;

	netRulesPut(&s_Kept, 0);
	s_Kept.valid = 0;

	sysLogPrintf(LOG_NOTE, "net: rules: the last match's rules adopted as this host's setup (slot %d its player there, now 0)", swapslot);

	return 1;
}

/*
 * Room Rules (owner, 2026-10-10, F3 20261010-053136): a room's rules are its
 * own. From the Create Room page on, this machine's own setup and settings
 * are saved (s_Own, as a taken-over room's host keeps them) and the Room
 * Rules keys start at their stock values - what they were registered with,
 * before pd.ini was read - whatever this machine plays with offline. The
 * Room Rules page and the room's Combat Simulator pages edit the room's
 * copies, which its matches' RULES carry, for as long as the room lasts;
 * pd.ini is written with this machine's own all along (configSave's swap),
 * and leaving the room (netRulesAdoptEnd, at its session's close) puts its
 * own setup and settings back. The next room starts at stock again.
 */
static struct netkeyvalue s_Stock[NET_MAXKEYS];
static s32 s_NumStock;

void netRulesCaptureStock(void)
{
	u32 k;

	s_NumStock = 0;

	for (k = 0; k < ARRAYCOUNT(s_NetKeys) && s_NumStock < NET_MAXKEYS; k++) {
		if (s_NetKeys[k].cls == NETKEY_SYNC && netRulesReadKey(s_NetKeys[k].key, &s_Stock[s_NumStock])) {
			s_NumStock++;
		}
	}

	if (s_NumStock < NET_MAXKEYS && netRulesReadKey("Mod.SimBrain", &s_Stock[s_NumStock])) {
		s_NumStock++;
	}
}

// A room this machine is making or hosts holds its own rules (or it took
// one over): its own setup and settings are kept aside
s32 netRulesRoomOn(void)
{
	return s_Own.client;
}

void netRulesRoomBegin(void)
{
	char a[64];
	char b[64];
	struct netkeyvalue cur;
	s32 i;

	if (s_Own.client) {
		return;
	}

	netRulesSaveInto(&s_Own);

	for (i = 0; i < s_NumStock; i++) {
		if (netRulesReadKey(s_Stock[i].key, &cur) && !netRulesValuesEqual(&cur, &s_Stock[i])) {
			netRulesValueString(&cur, a, sizeof(a));
			netRulesValueString(&s_Stock[i], b, sizeof(b));
			sysLogPrintf(LOG_NOTE, "net: rules: %s %s -> %s (the new room's)", s_Stock[i].key, a, b);
		}

		netRulesWriteKey(&s_Stock[i]);
	}

	netRulesApplyCheatGuns(s_Stock, s_NumStock, "the new room's");
	netRulesApplyCsHackGuns(s_Stock, s_NumStock, "the new room's");
	sysLogPrintf(LOG_NOTE, "net: rules: a new room: its Room Rules start at the stock values; this machine's own setup and settings come back when it leaves");
}

/**
 * A Room Rule set as its page sets one, by key and text (tests:
 * --net-lobby-rules). 1 when set.
 */
s32 netRulesRoomSet(const char *key, const char *value)
{
	struct netkeyvalue kv;

	if (!s_Own.client || !netRulesReadKey(key, &kv)) {
		return 0;
	}

	switch (kv.type) {
	case CONFIG_TYPE_S32: kv.s = strtol(value, NULL, 0); break;
	case CONFIG_TYPE_U32: kv.u = strtoul(value, NULL, 0); break;
	case CONFIG_TYPE_F32: kv.f = strtof(value, NULL); break;
	case CONFIG_TYPE_STR: snprintf(kv.str, sizeof(kv.str), "%s", value); break;
	default: return 0;
	}

	netRulesWriteKey(&kv);
	netRulesApplyCheatGuns(&kv, 1, "the room's");
	netRulesApplyCsHackGuns(&kv, 1, "the room's");
	sysLogPrintf(LOG_NOTE, "net: rules: the room's %s set to %s", key, value);

	return 1;
}

// This machine stops hosting the room it took over, made, or was making:
// its own setup back
void netRulesAdoptEnd(void)
{
	if (!s_Own.client) {
		return;
	}

	if (s_Own.swapped) {
		netRulesSwap(&s_Own);
		s_Own.swapped = 0;
	}

	netRulesRestoreFrom(&s_Own);
	s_Own.client = 0;
	sysLogPrintf(LOG_NOTE, "net: rules: hosting over; this machine's own setup and settings are back");
}

void netRulesSetLocked(s32 locked)
{
	s_NetRulesLocked = locked;
}

s32 netRulesLocked(void)
{
	return s_NetRulesLocked;
}
