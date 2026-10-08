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
 * Mod.XblaMeshes and Mod.GeXblaCommunityEdition are in none of them either:
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
	{ "Mod.GePlusRegion",            NETKEY_SYNC },
	{ "Mod.BorrowGoldenEyeGuns",     NETKEY_MUST },
	{ "Mod.GePlusRevisionFixes",     NETKEY_MUST_GE },
	// a mission's guards (spec-coop.md): the host's, as the AI is
	{ "Mod.GuardsAlerted",           NETKEY_SYNC },
	{ "Mod.AlertedGuards",           NETKEY_SYNC },
	{ "Mod.GuardSpawnSpeed",         NETKEY_SYNC },
	{ "Mod.GuardWeapons",            NETKEY_SYNC },
	{ "Mod.MissionRespawn",          NETKEY_SYNC },
	{ "Mod.MissionLives",            NETKEY_SYNC },
	// a player's own (protocol 20): how its controls, its gun and its third
	// person camera behave. The camera's are read inside playerTick, and a
	// third person shot is fired from the camera (protocol 19's fix)
	{ "Mod.CodAiming",               NETKEY_PLAYER },
	{ "Mod.CodAimLock",              NETKEY_PLAYER },
	{ "Mod.AkimboTriggers",          NETKEY_PLAYER },
	{ "Mod.QuickWeaponSwap",         NETKEY_PLAYER },
	{ "Mod.SkipDeathScreen",         NETKEY_PLAYER },
	{ "Mod.ThirdPersonDistance",     NETKEY_PLAYER },
	{ "Mod.ThirdPersonClearance",    NETKEY_PLAYER },
	{ "Mod.ThirdPersonSideways",     NETKEY_PLAYER },
	{ "Mod.ThirdPersonForward",      NETKEY_PLAYER },
	{ "Mod.ThirdPersonHeight",       NETKEY_PLAYER },
	{ "Mod.ThirdPersonTether",       NETKEY_PLAYER },
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
	struct netcooprules coop; // protocol 12: a mission (spec-coop.md)
	struct netcontentneed content; // protocol 13: the host's mod and ROM hack mode (netcontent.c)
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
	const char *gexplusvariant; // g_GexPlusVariant (a static name, or NULL)
	s32 endless;
	s32 maxexplosions;
	u8 locktype;
	struct missionconfig mission; // a client's own mission settings (co-op)
	s32 difficulty;
	s32 coopradaron;
	s32 coopfriendlyfire;
	s32 numaibuddies;
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

	return sent;
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

	return sent;
}

void netWriteMpChar(struct netbuf *b, s32 mpheadnum, s32 mpbodynum)
{
	netBufWriteU8(b, (u8)mpheadnum);
	netBufWriteU8(b, (u8)mpbodynum);
	netBufWriteS16(b, (s16)netMpHeadRow(mpheadnum));
	netBufWriteS16(b, (s16)netMpBodyRow(mpbodynum));
}

void netReadMpChar(struct netbuf *b, u8 *mpheadnum, u8 *mpbodynum)
{
	const s32 head = netBufReadU8(b);
	const s32 body = netBufReadU8(b);
	const s32 headrow = netBufReadS16(b);
	const s32 bodyrow = netBufReadS16(b);
	const s32 h = netMpHeadIndex(headrow, head);
	const s32 d = netMpBodyIndex(bodyrow, body);

	*mpheadnum = (u8)(h >= 0 && h <= 0xff ? h : head);
	*mpbodynum = (u8)(d >= 0 && d <= 0xff ? d : body);
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
		// the content block (protocol 13, netcontent.c): the mod and the
		// ROM hack mode the client is to play this match in
		struct netcontentneed need;

		netContentHostNeed(&need);
		netContentWrite(b, &need, 1);
	}

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
		netReadMpChar(b, &sim.mpheadnum, &sim.mpbodynum);
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
		netReadMpChar(b, &h->mpheadnum, &h->mpbodynum);
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
	s_NetRules.coop.on = netBufReadU8(b);
	s_NetRules.coop.stageindex = netBufReadU8(b);
	s_NetRules.coop.difficulty = netBufReadU8(b);
	s_NetRules.coop.radar = netBufReadU8(b);
	s_NetRules.coop.friendlyfire = netBufReadU8(b);
	netBufReadString(b, s_NetRules.coop.game, sizeof(s_NetRules.coop.game));
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

const struct netcontentneed *netRulesContent(void)
{
	return &s_NetRules.content;
}

// the RULES received say a co-op mission (0 a match, or none received)
s32 netRulesCoopOn(void)
{
	return s_NetRules.valid && s_NetRules.coop.on;
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
	s_NetSaved.gexplusvariant = g_GexPlusVariant;
	s_NetSaved.endless = g_MpEndlessMatch;
	s_NetSaved.maxexplosions = g_MaxExplosionsSetting;
	s_NetSaved.locktype = g_BossFile.locktype;
	s_NetSaved.lockinfo = g_MpLockInfo;
	s_NetSaved.mission = g_MissionConfig;
	s_NetSaved.difficulty = lvGetDifficulty();
	s_NetSaved.coopradaron = g_Vars.coopradaron;
	s_NetSaved.coopfriendlyfire = g_Vars.coopfriendlyfire;
	s_NetSaved.numaibuddies = g_Vars.numaibuddies;

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

	// the mode first, through its setter: a ROM hack's mode (protocol 13)
	// puts the hack's weapon sets in the list's block, which the set number
	// and weapons below index (gexplus.c); the setter's own choice of set
	// and scenario is overwritten by the host's right after
	if (!netContentVariantApply(s_NetRules.content.gevariant)) {
		sysLogPrintf(LOG_WARNING, "net: rules: the host's ROM hack mode \"%s\" is not converted here", s_NetRules.content.gevariant);
	}

	mpSetGexPlusMode(s_NetRules.gexplusmode != 0);
	gexPlusSetScenario(s_NetRules.gexplusscenario);

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
			netNameSet(bot->base.name, sizeof(bot->base.name), sim->name);

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

		netNameSet(p->base.name, sizeof(p->base.name), h->name);
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

	// a mission's settings (spec-coop.md): the host's mission and difficulty
	if (s_NetRules.coop.on) {
		netCoopClientApplyRules(&s_NetRules.coop);
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
			const char *name = g_PlayerConfigsArray[i].base.name;
			const s32 len = netNameLen(name);

			// (a name without its newline draws with no height: netNameSet)
			sysLogPrintf(LOG_NOTE, "net: the match's players: \"%.*s\" in slot %d%s", len, name, i,
					len > 0 && name[len] != '\n' ? " (its name has no newline)" : "");
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
		g_GexPlusVariant = s_NetSaved.gexplusvariant;
		mpSetGexPlusMode(s_NetSaved.gexplusmode != 0);
		gexPlusSetScenario(s_NetSaved.gexplusscenario);
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
		g_MissionConfig = s_NetSaved.mission;
		lvSetDifficulty(s_NetSaved.difficulty);
		g_Vars.coopradaron = s_NetSaved.coopradaron;
		g_Vars.coopfriendlyfire = s_NetSaved.coopfriendlyfire;
		g_Vars.numaibuddies = s_NetSaved.numaibuddies;

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
