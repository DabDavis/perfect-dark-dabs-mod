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
#include "lib/snd.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/explosions.h"
#include "game/hudmsg.h"
#include "game/mpstats.h"
#include "game/nbomb.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/propobj.h"
#include "game/sparks.h"
#include "game/mplayer/mplayer.h"
#include "gesfx.h"
#include "audio.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "net/nettransport.h"
#include "netint.h"

/**
 * Events: the reliable, tick-stamped channel (PLANS/netplay/spec-entities.md
 * §5; the wire is EVENTS in netproto.h).
 *
 * Host: the hooks E1-E9 record what happened during the tick into one
 * buffer, each event marked with the player it is for (a hudmsg, a pickup
 * sound) or the player whose machine already made it itself (its own shot,
 * the sparks and flames of that shot). At the tick's end (netTickEnd, before
 * the snapshots) each remote slot gets one EVENTS message with the events
 * that are its.
 *
 * Client: decoded events wait in a queue until the puppets' render clock
 * reaches their tick, so they are applied after that tick's records were
 * posed (the shot of a sim whose gun the record changed fires the new gun),
 * in the order the host made them. Applying calls the same game function
 * the host ran (chrUpdateFireslot on the puppet, explosionCreate, sparksCreate,
 * chrChoke, objDeform, glassDestroy, mpstatsRecordDeath, hudmsgCreateFromArgs,
 * nbombCreateStorm, gasReleaseFromPos); the authority gates (C8, C9) keep
 * every one of them cosmetic here.
 */

#define NETEV_FIRESLOT   1  // E1: a chr's shot (sim or NPC)
#define NETEV_PLAYERSHOT 2  // E2: a player's shot
#define NETEV_EXPLOSION  3  // E3
#define NETEV_MAXHUDTIME (60 * 60) // a forwarded hudmsg's longest time, in 60ths
#define NETEV_SPARKS     4  // E2/E4: an impact's sparks, blood and flesh
#define NETEV_CHRDAMAGE  5  // E4: a chr the host hit
#define NETEV_CHOKE      6  // E4: the hit chr's grunt or death cry
#define NETEV_DEFORM     7  // E5
#define NETEV_GLASS      8  // E5
#define NETEV_DEATH      9  // E6: mpstatsRecordDeath
#define NETEV_HUDMSG     10 // E7: for one player
#define NETEV_PICKUPSFX  11 // E8: for one player
#define NETEV_NBOMB      12 // E9
#define NETEV_GAS        13 // E9
#define NETEV_SCORES     14 // protocol 9: the kill table, to a joiner or everyone
#define NETEV_COUNT      15

static const char *const s_EvNames[NETEV_COUNT] = {
	"?", "fireslot", "playershot", "explosion", "sparks", "chrdamage", "choke",
	"deform", "glass", "death", "hudmsg", "pickupsfx", "nbomb", "gas", "scores",
};

#define NETEV_SCORES_CATCHUP 0x01

// A reference to a prop on the wire
#define NETREF_NONE   0
#define NETREF_PLAYER 1 // u8 playernum: the same slots on every machine
#define NETREF_ENT    2 // u16 entity id, u16 its generation on the host
#define NETREF_SETUP  3 // the same, then u16 the setup command it is the object of (protocol 8)

struct netref {
	u8 kind;
	u8 pn;
	u16 id;
	u16 gen;
	u16 cmd; // NETREF_SETUP
};

#define NETEV_MAXROOMS   8
#define NETEV_MAXTEXT    255
#define NETEV_MAXMSG     16384   // one EVENTS message's events, at most
#define NETEV_ARENA      (256 * 1024)
#define NETEV_QUEUE      4096
#define NETEV_KILLEVERY  300     // ticks between kill-table samples (logs)
#define NETEV_KILLSETTLE 60      // a sample is taken this far behind the render clock with nothing queued

/*
 * Host
 */

static u8 s_Arena[NETEV_ARENA];  // [s8 target][s8 exclude][u16 len][payload] ...
static s32 s_ArenaLen = 0;
static u8 s_Msg[NETEV_MAXMSG + 64];
static u8 s_One[NETEV_MAXMSG]; // one event as it is recorded (a SCORES table is the largest)
static u32 s_HostRecorded[NETEV_COUNT];
static u32 s_HostSentBytes = 0;
static u32 s_HostMsgs = 0;
static u32 s_HostSeq[NET_MAXVIEWS]; // EVENTS messages sent to each view this match (the SNAP's evseq)
static u32 s_HostDroppedFull = 0;
static u32 s_HostRecordedTotal = 0;
static char s_Detail[160];     // what the host's log line adds to the next event
static s32 s_HostEnded = 0;    // MATCH_END has gone: nothing after it is the match's
static u32 s_HudHash[MAX_PLAYERS];  // the last hudmsg sent to each player, and its tick:
static u32 s_HudTick[MAX_PLAYERS];  // the same again within a second is not sent
static u32 s_HudDupes = 0;

/*
 * Client
 */

struct netevc {
	u32 tick;
	u8 type;
	u8 flags;
	s8 a;
	s8 b;
	s16 s0;
	s16 s1;
	struct netref r0;
	struct netref r1;
	f32 p0[3];
	f32 p1[3];
	f32 p2[3];
	s16 rooms[NETEV_MAXROOMS + 1];
	u32 u[4];
	s32 i[8];
	u32 uflags;
	char *text;
	u8 *blob;   // SCORES: the table's bytes (malloc'd, freed once applied)
	s32 bloblen;
};

static struct netevc *s_Q = NULL;     // [NETEV_QUEUE]
static char *s_QText = NULL;          // [NETEV_QUEUE][NETEV_MAXTEXT + 1]
static s32 s_QHead = 0;               // the oldest
static s32 s_QCount = 0;
static s32 s_QMax = 0;
static u32 s_LastTick = 0;
static s32 s_HaveTick = 0;
static s32 s_Applying = 0;
static s32 s_SuppressHud = 0;
static s32 s_ClientEnded = 0;  // MATCH_END came: the match's events are all in

static u32 s_Received[NETEV_COUNT];
static u32 s_Applied[NETEV_COUNT];
static u32 s_RecvMsgs = 0;
static u32 s_RecvSeq = 0;      // EVENTS messages of this match received (against the SNAP's evseq)
static u32 s_RecvBytes = 0;
static u32 s_OutOfOrder = 0;
static u32 s_Malformed = 0;
static u32 s_OtherMatch = 0;
extern struct fogenvironment *g_EnvTransitionFrom;
extern struct fogenvironment *g_EnvTransitionTo;

static u32 s_Unresolved = 0;
static u32 s_UnresolvedBy[NETEV_COUNT];
static s32 s_ViaSetup = 0;      // the last REF resolved by its setup command, never in scope here
static u32 s_GlassViaSetup = 0; // glass broken that this machine never had in scope
static u32 s_GasReleased = 0;   // gas let go here (not dropped for want of a sky)
static u32 s_Overflow = 0;
static u32 s_HitsOnMe = 0;      // CHRDAMAGE with this machine's player the victim
static u32 s_MyHits = 0;        // ... the attacker
static u32 s_HudForMe = 0;
static u32 s_ShotSounds = 0;    // fireslot/playershot events with a sound applied
static u32 s_ShotNoGun = 0;     // ... whose puppet held nothing in that hand
static u32 s_ShotPlayed = 0;    // ... that started the gun's sound (not one still sounding)
static f64 s_LagSum = 0;
static u32 s_LagN = 0;
static u32 s_NextKill = NETEV_KILLEVERY;
static u32 s_LocalSkipped = 0;  // this machine's own mpstatsRecordDeath calls refused
#define NETEV_MYSHOTS 64
static u32 s_MyShots[NETEV_MYSHOTS]; // the command ticks this machine's player fired on, + 1
static u32 s_MyShotsN = 0;
static u32 s_OwnDupes = 0;      // E2s for this machine's player that it had fired itself (dropped)
static u32 s_OwnUnpredicted = 0; // ... that it had not

static s32 s_Quiet = 0;         // --net-test-quiet-events: events make no sound here (the audio check's null)
static FILE *s_Log = NULL;      // --net-event-log FILE: every event recorded (host) or applied (client)
static s32 s_LogOn = 0;

extern struct hudmsgtype g_HudmsgTypes[];

static void netEvLogOpen(void)
{
	const char *path = sysArgGetString("--net-event-log");

	if (s_Log || !path) {
		return;
	}

	s_Log = fopen(path, "w");

	if (s_Log) {
		setvbuf(s_Log, NULL, _IOLBF, 0);
		s_LogOn = 1;
	}
}

/*
 * Both
 */

static s32 netEvHostOn(void)
{
	s32 slot;

	if (g_NetMode != NETMODE_SERVER || !netSessionMatchActive() || netSessionBarrierHeld() || s_HostEnded) {
		return 0;
	}

	for (slot = 0; slot < NET_MAXVIEWS; slot++) {
		if (slot < MAX_PLAYERS ? netPlayersHostSlotIsRemote(slot) : netSessionViewLive(slot)) {
			return 1;
		}
	}

	return 0;
}

static s32 netEvPlayerOfSlot(s32 slot)
{
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if ((g_Vars.playerstats[i].mpindex & 3) == slot && g_Vars.players[i]) {
			return i;
		}
	}

	return -1;
}

// The kill table, one line: per mpchr in the match, deaths then its kill counts
static void netEvKillLine(const char *when, u32 tick)
{
	s32 i;
	s32 j;

	if (!s_Log) {
		return;
	}

	fprintf(s_Log, "K %s %u", when, tick);

	for (i = 0; i < MAX_MPCHRS; i++) {
		struct mpchrconfig *mpchr = MPCHR(i);
		s32 any = mpchr->numdeaths;

		for (j = 0; j < MAX_MPCHRS; j++) {
			any |= mpchr->killcounts[j];
		}

		if (!any) {
			continue;
		}

		fprintf(s_Log, " %d:%d:", i, mpchr->numdeaths);

		for (j = 0; j < MAX_MPCHRS; j++) {
			if (mpchr->killcounts[j]) {
				fprintf(s_Log, "%d=%d,", j, mpchr->killcounts[j]);
			}
		}
	}

	fprintf(s_Log, "\n");
}

/*
 * Host: recording
 */

static void netWriteRef(struct netbuf *b, struct prop *prop)
{
	s32 idx;

	if (!prop) {
		netBufWriteU8(b, NETREF_NONE);
		return;
	}

	if (prop->type == PROPTYPE_PLAYER) {
		const s32 pn = playermgrGetPlayerNumByProp(prop);

		if (pn >= 0 && pn < PLAYERCOUNT()) {
			netBufWriteU8(b, NETREF_PLAYER);
			netBufWriteU8(b, (u8)pn);
			return;
		}
	}

	idx = netEntsPropIndex(prop);

	if (idx < 0) {
		netBufWriteU8(b, NETREF_NONE);
		return;
	}

	// a setup object also names its setup command: a client that never had
	// it in scope (glass broken across the map) finds its own copy by it
	if (netEntsSetupCmdOf(idx) >= 0 && netEntsSetupCmdOf(idx) < 0xffff) {
		netBufWriteU8(b, NETREF_SETUP);
		netBufWriteU16(b, (u16)idx);
		netBufWriteU16(b, netEntsPropGen(idx));
		netBufWriteU16(b, (u16)netEntsSetupCmdOf(idx));
		return;
	}

	netBufWriteU8(b, NETREF_ENT);
	netBufWriteU16(b, (u16)idx);
	netBufWriteU16(b, netEntsPropGen(idx));
}

static void netWritePos(struct netbuf *b, const struct coord *c)
{
	netBufWriteF32(b, c ? c->x : 0);
	netBufWriteF32(b, c ? c->y : 0);
	netBufWriteF32(b, c ? c->z : 0);
}

static s16 netDirQ(f32 v)
{
	f32 q = v * 8192.f;

	if (q != q) {
		q = 0;
	}

	return (s16)(q > 32767.f ? 32767 : q < -32767.f ? -32767 : (s32)q);
}

static void netWriteDir(struct netbuf *b, const struct coord *c)
{
	netBufWriteS16(b, netDirQ(c->x));
	netBufWriteS16(b, netDirQ(c->y));
	netBufWriteS16(b, netDirQ(c->z));
}

/**
 * A normal, normalised first: sparksCreate's is the raw bg edge cross
 * product (it divides by the length itself), so its components can be far
 * outside netDirQ's range and clamping them would turn it
 */
static void netWriteNormal(struct netbuf *b, const struct coord *c)
{
	f32 x = c->x;
	f32 y = c->y;
	f32 z = c->z;
	f32 len = sqrtf(x * x + y * y + z * z);

	if (len > 0.000001f && len == len) {
		x /= len;
		y /= len;
		z /= len;
	} else {
		x = 0;
		y = 1;
		z = 0;
	}

	netBufWriteS16(b, netDirQ(x));
	netBufWriteS16(b, netDirQ(y));
	netBufWriteS16(b, netDirQ(z));
}

/**
 * The player whose machine already made what the current code makes: the
 * pass's player when that is another machine's (its own shot runs there
 * too), else -1
 */
static s32 netEvOrigin(void)
{
	return g_NetPassPlayer;
}

static void netEvBegin(struct netbuf *b, s32 type)
{
	netBufInitWrite(b, s_One, sizeof(s_One));
	netBufWriteU8(b, (u8)type);
}

static void netEvEnd(struct netbuf *b, s32 type, s32 target, s32 exclude)
{
	const s32 len = netBufLen(b);

	if (!netBufOk(b) || len <= 0) {
		s_Detail[0] = '\0';
		return;
	}

	if (s_ArenaLen + 4 + len > NETEV_ARENA) {
		s_HostDroppedFull++;
		s_Detail[0] = '\0';
		return;
	}

	s_Arena[s_ArenaLen + 0] = (u8)(s8)target;
	s_Arena[s_ArenaLen + 1] = (u8)(s8)exclude;
	s_Arena[s_ArenaLen + 2] = (u8)(len & 0xff);
	s_Arena[s_ArenaLen + 3] = (u8)(len >> 8);
	memcpy(s_Arena + s_ArenaLen + 4, s_One, len);
	s_ArenaLen += 4 + len;

	s_HostRecorded[type]++;
	s_HostRecordedTotal++;

	if (s_Log) {
		fprintf(s_Log, "E %u %s target %d exclude %d len %d%s%s\n", g_NetTick, s_EvNames[type], target, exclude, len,
				s_Detail[0] ? " " : "", s_Detail);
	}

	s_Detail[0] = '\0';
}

static const char *netRefText(struct prop *prop, char *buf, s32 size)
{
	if (!prop) {
		snprintf(buf, size, "none");
	} else if (prop->type == PROPTYPE_PLAYER) {
		snprintf(buf, size, "player %d", playermgrGetPlayerNumByProp(prop));
	} else {
		snprintf(buf, size, "id %d", netEntsPropIndex(prop));
	}

	return buf;
}

void netEvChrFireslot(struct chrdata *chr, s32 handnum, s32 withsound, s32 withbeam, struct coord *from, struct coord *to)
{
	struct netbuf b;

	if ((!withsound && !withbeam) || !chr || !chr->prop || !netEvHostOn()) {
		return;
	}

	netEvBegin(&b, NETEV_FIRESLOT);
	netWriteRef(&b, chr->prop);
	netBufWriteU8(&b, (u8)((handnum & 1) | (withsound ? 2 : 0) | (withbeam ? 4 : 0)));
	netWritePos(&b, from);
	netWritePos(&b, to);
	netEvEnd(&b, NETEV_FIRESLOT, -1, -1);
}

// The guns whose shots draw a tracer (chrTickShoot's list)
static s32 netEvWeaponBeam(s32 weaponnum)
{
	switch (weaponHost(weaponnum)) {
	case WEAPON_FALCON2:
	case WEAPON_FALCON2_SILENCER:
	case WEAPON_FALCON2_SCOPE:
	case WEAPON_MAGSEC4:
	case WEAPON_MAULER:
	case WEAPON_PHOENIX:
	case WEAPON_DY357MAGNUM:
	case WEAPON_DY357LX:
	case WEAPON_CMP150:
	case WEAPON_CYCLONE:
	case WEAPON_CALLISTO:
	case WEAPON_RCP120:
	case WEAPON_LAPTOPGUN:
	case WEAPON_DRAGON:
	case WEAPON_K7AVENGER:
	case WEAPON_AR34:
	case WEAPON_SUPERDRAGON:
	case WEAPON_REAPER:
	case WEAPON_FARSIGHT:
	case WEAPON_TRANQUILIZER:
	case WEAPON_LASER:
	case WEAPON_PP9I:
	case WEAPON_CC13:
	case WEAPON_KL01313:
	case WEAPON_KF7SPECIAL:
	case WEAPON_ZZT:
	case WEAPON_DMC:
	case WEAPON_AR53:
	case WEAPON_RCP45:
		return 1;
	}

	return 0;
}

/**
 * E2 (handTickAttack, a shot fired): the shooter's machine made its own
 * muzzle, sound and tracer when it fired; everybody else gets them on its
 * puppet. The shooter's machine gets the event too, with the command it
 * came from, and drops it if it fired on that command (a client keeps the
 * commands it fired on here)
 */
void netEvPlayerShot(s32 handnum)
{
	struct player *player = g_Vars.currentplayer;
	struct netbuf b;
	s32 weaponnum;
	u32 cmd;

	if (g_NetMode == NETMODE_CLIENT) {
		if (player && g_Vars.currentplayernum == g_NetLocalSlot && !g_NetReplaying && netSessionMatchActive()) {
			s_MyShots[s_MyShotsN++ % NETEV_MYSHOTS] = g_NetTick + 1;
		}

		return;
	}

	if (!player || !player->prop || !netEvHostOn()) {
		return;
	}

	cmd = netPlayersHostSlotIsRemote(g_Vars.currentplayernum) ? (u32)netPlayersHostLastPlayed(g_Vars.currentplayernum) : g_NetTick;

	weaponnum = player->hands[handnum].gset.weaponnum;

	netEvBegin(&b, NETEV_PLAYERSHOT);
	netBufWriteU8(&b, (u8)g_Vars.currentplayernum);
	netBufWriteU8(&b, (u8)((handnum & 1) | (netEvWeaponBeam(weaponnum) ? 4 : 0)));
	netWritePos(&b, &player->chrmuzzlelastpos[handnum & 1]);
	netWritePos(&b, &player->hands[handnum & 1].hitpos);
	netBufWriteU32(&b, cmd);
	netEvEnd(&b, NETEV_PLAYERSHOT, -1, -1);
}

void netEvExplosion(struct prop *source, struct coord *pos, RoomNum *rooms, s32 type, s32 playernum,
		s32 makescorch, struct coord *arg6, RoomNum room, struct coord *arg8)
{
	struct netbuf b;
	s32 n = 0;
	s32 i;

	// a bullet hole's flame is made by distance from each viewer's camera
	if (type == EXPLOSIONTYPE_BULLETHOLE || !netEvHostOn()) {
		return;
	}

	while (n < NETEV_MAXROOMS && rooms[n] != -1) {
		n++;
	}

	netEvBegin(&b, NETEV_EXPLOSION);
	netWriteRef(&b, source);
	netWritePos(&b, pos);
	netBufWriteU8(&b, (u8)n);

	for (i = 0; i < n; i++) {
		netBufWriteS16(&b, rooms[i]);
	}

	// the scorch's place, normal and room are read only with makescorch
	// (and without one the place can be the floor finder's "no floor")
	if (!makescorch || !arg6 || !arg8) {
		makescorch = 0;
		arg6 = NULL;
		arg8 = NULL;
		room = 0;
	}

	netBufWriteS16(&b, (s16)type);
	netBufWriteS8(&b, (s8)(playernum < -1 || playernum >= MAX_MPCHRS ? -1 : playernum));
	netBufWriteU8(&b, (u8)((makescorch ? 1 : 0) | (arg6 ? 2 : 0) | (arg8 ? 4 : 0)));

	if (arg6) {
		netWritePos(&b, arg6);
	}

	netBufWriteS16(&b, room);

	if (arg8) {
		netWritePos(&b, arg8);
	}

	// only a Phoenix round's blast is made by the shooter's own machine (its
	// shot runs there); a destroyed object's blast and the damage puffs come
	// from objDamage, which runs on the host only
	netEvEnd(&b, NETEV_EXPLOSION, -1, type == EXPLOSIONTYPE_PHOENIX ? netEvOrigin() : -1);
}

void netEvSparks(s32 room, struct prop *prop, struct coord *pos, struct coord *arg3, struct coord *arg4, s32 type)
{
	struct netbuf b;

	// a pad's own sparks and the rain's splashes are made by every machine
	if ((type >= SPARKTYPE_ENVIRONMENTAL1 && type <= SPARKTYPE_ENVIRONMENTAL5)
			|| (type == SPARKTYPE_SHALLOWWATER && !arg4)
			|| !netEvHostOn()) {
		return;
	}

	netEvBegin(&b, NETEV_SPARKS);
	netBufWriteS16(&b, (s16)room);
	netWriteRef(&b, prop);
	netWritePos(&b, pos);
	netBufWriteU8(&b, (u8)type);
	netBufWriteU8(&b, (u8)((arg3 ? 1 : 0) | (arg4 ? 2 : 0)));

	if (arg3) {
		netWriteDir(&b, arg3);
	}

	if (arg4) {
		netWriteNormal(&b, arg4);
	}

	netEvEnd(&b, NETEV_SPARKS, -1, netEvOrigin());
}

void netEvChrDamage(struct chrdata *chr, struct prop *aprop, s32 hitpart, s32 damageshield, s32 explosion)
{
	struct netbuf b;

	if (!chr || !chr->prop || !netEvHostOn()) {
		return;
	}

	if (s_Log) {
		char v[32];
		char a[32];

		snprintf(s_Detail, sizeof(s_Detail), "victim %s attacker %s hitpart %d", netRefText(chr->prop, v, sizeof(v)),
				netRefText(aprop, a, sizeof(a)), hitpart);
	}

	netEvBegin(&b, NETEV_CHRDAMAGE);
	netWriteRef(&b, chr->prop);
	netWriteRef(&b, aprop);
	netBufWriteS8(&b, (s8)hitpart);
	netBufWriteU8(&b, (u8)((damageshield ? 1 : 0) | (explosion ? 2 : 0) | (chrGetShield(chr) > 0 ? 4 : 0)));
	netEvEnd(&b, NETEV_CHRDAMAGE, -1, -1);
}

void netEvChoke(struct chrdata *chr, s32 choketype)
{
	struct netbuf b;

	if (!chr || !chr->prop || !netEvHostOn()) {
		return;
	}

	netEvBegin(&b, NETEV_CHOKE);
	netWriteRef(&b, chr->prop);
	netBufWriteS8(&b, (s8)choketype);
	netEvEnd(&b, NETEV_CHOKE, -1, -1);
}

void netEvObjDeform(struct defaultobj *obj, s32 level)
{
	struct netbuf b;

	if (!obj || !obj->prop || !netEvHostOn()) {
		return;
	}

	netEvBegin(&b, NETEV_DEFORM);
	netWriteRef(&b, obj->prop);
	netBufWriteS16(&b, (s16)level);
	netEvEnd(&b, NETEV_DEFORM, -1, -1);
}

void netEvGlassDestroy(struct defaultobj *obj)
{
	struct netbuf b;

	if (!obj || !obj->prop || !netEvHostOn()) {
		return;
	}

	netEvBegin(&b, NETEV_GLASS);
	netWriteRef(&b, obj->prop);
	netEvEnd(&b, NETEV_GLASS, -1, -1);
}

/**
 * E6 (mpstatsRecordDeath): 1 when the caller must return. The host records
 * the death by mpchr slot (its g_MpAllChrPtrs order is its own) and lets it
 * run. A client counts only the host's deaths: its own call (its player's
 * death applied from the local block) is refused.
 */
s32 netEvRecordDeath(s32 aplayernum, s32 vplayernum)
{
	if (g_NetMode == NETMODE_SERVER) {
		struct netbuf b;
		s32 a = aplayernum >= 0 && aplayernum < g_MpNumChrs ? func0f18d074(aplayernum) : -1;
		s32 v = vplayernum >= 0 && vplayernum < g_MpNumChrs ? func0f18d074(vplayernum) : -1;

		if (!netEvHostOn()) {
			return 0;
		}

		if (s_Log) {
			snprintf(s_Detail, sizeof(s_Detail), "attacker %d victim %d", a, v);
		}

		netEvBegin(&b, NETEV_DEATH);
		netBufWriteS8(&b, (s8)a);
		netBufWriteS8(&b, (s8)v);
		netBufWriteS8(&b, (s8)(aplayernum < 0 ? -1 : a < 0 ? -2 : 0)); // an attacker outside the match (a guard)
		netBufWriteS8(&b, (s8)(vplayernum < 0 ? -1 : v < 0 ? -2 : 0));
		netEvEnd(&b, NETEV_DEATH, -1, -1);
		return 0;
	}

	if (NET_CLIENT && !s_Applying) {
		s_LocalSkipped++;
		return 1;
	}

	return 0;
}

/**
 * E7 (hudmsgCreateFromArgs, where every hudmsg is made): 1 when the caller
 * must return. The host sends a message for another machine's player to
 * that machine; a client drops the ones its replay of a death makes (the
 * host has sent those)
 */
s32 netEvHudmsg(char *text, s32 type, s32 conf00, s32 conf01, s32 conf02, u32 textcolour, u32 glowcolour,
		u32 alignh, s32 conf16, u32 alignv, s32 conf18, s32 arg14, u32 flags)
{
	if (g_NetMode == NETMODE_SERVER) {
		struct netbuf b;
		const s32 pn = g_Vars.currentplayernum;

		if (netIsLocalSlot(pn) || !netEvHostOn() || !text) {
			return 0;
		}

		// a message made again while the last is still up (the match start's
		// scenario name is made every pass while lvframenum is 5): the
		// player's machine would drop it as hudmsgCreateFromArgs drops one
		// still showing
		if (pn >= 0 && pn < MAX_PLAYERS) {
			u32 hash = 2166136261u;
			const char *c;

			for (c = text; *c; c++) {
				hash = (hash ^ (u8)*c) * 16777619u;
			}

			hash ^= (u32)type << 24;

			if (s_HudTick[pn] && s_HudHash[pn] == hash && g_NetTick - (s_HudTick[pn] - 1) < 60) {
				s_HudDupes++;
				return 1;
			}

			s_HudHash[pn] = hash;
			s_HudTick[pn] = g_NetTick + 1;
		}

		if (s_Log) {
			char *p;

			snprintf(s_Detail, sizeof(s_Detail), "\"%.100s\"", text);

			for (p = s_Detail; *p; p++) {
				if (*p == '\n' || *p == '\r') {
					*p = ' ';
				}
			}
		}

		netEvBegin(&b, NETEV_HUDMSG);
		netWriteStr(&b, text, NETEV_MAXTEXT);
		netBufWriteU8(&b, (u8)type);
		netBufWriteS32(&b, conf00);
		netBufWriteS32(&b, conf01);
		netBufWriteS32(&b, conf02);
		netBufWriteU32(&b, textcolour);
		netBufWriteU32(&b, glowcolour);
		netBufWriteU32(&b, alignh);
		netBufWriteS32(&b, conf16);
		netBufWriteU32(&b, alignv);
		netBufWriteS32(&b, conf18);

		// a subtitle's audio channel is this machine's: the player's machine
		// shows it for the type's own time instead
		if (!(flags & HUDMSGFLAG_NOCHANNEL)) {
			flags |= HUDMSGFLAG_NOCHANNEL;
			arg14 = type >= 0 && type <= HUDMSGTYPE_CUTSCENESUBTITLE ? g_HudmsgTypes[type].duration : 0;
		}

		netBufWriteS32(&b, arg14);
		netBufWriteU32(&b, flags);
		netEvEnd(&b, NETEV_HUDMSG, pn, -1);
		return 1;
	}

	return NET_CLIENT && s_SuppressHud;
}

static s32 s_OriginMade;

/**
 * Around code a remote player's own machine runs too (its CamSpy pickup in
 * eyespyProcessInput is not gated there): its pickup sound is not sent back
 */
void netEvOriginMade(s32 on)
{
	s_OriginMade = on;
}

// E8 (objPlayPickupSfx): the picking player's own sound, to its machine
s32 netEvPickupSound(s32 sound)
{
	struct netbuf b;
	const s32 pn = g_Vars.currentplayernum;

	if (netIsLocalSlot(pn) || !netEvHostOn()) {
		return 0;
	}

	// sndStart would play it for nobody here
	if (s_OriginMade && pn == g_NetPassPlayer) {
		return 1;
	}

	netEvBegin(&b, NETEV_PICKUPSFX);
	netBufWriteS16(&b, (s16)sound);
	netEvEnd(&b, NETEV_PICKUPSFX, pn, -1);

	return 1;
}

void netEvNbomb(struct coord *pos, struct prop *owner)
{
	struct netbuf b;

	if (!pos || !netEvHostOn()) {
		return;
	}

	netEvBegin(&b, NETEV_NBOMB);
	netWritePos(&b, pos);
	netWriteRef(&b, owner);
	netEvEnd(&b, NETEV_NBOMB, -1, -1);
}

void netEvGas(struct coord *pos)
{
	struct netbuf b;

	if (!pos || !netEvHostOn()) {
		return;
	}

	netEvBegin(&b, NETEV_GAS);
	netWritePos(&b, pos);
	netEvEnd(&b, NETEV_GAS, -1, -1);
}

/**
 * The kill table as it stands (protocol 9): every mpchr's deaths, points
 * and kill counts, the rows and counts that are 0 left out
 */
static void netEvWriteScores(struct netbuf *b, s32 flags)
{
	u8 *np;
	s32 n = 0;
	s32 i;
	s32 j;

	netBufWriteU8(b, (u8)flags);
	np = netBufReserve(b, 1);

	for (i = 0; i < MAX_MPCHRS; i++) {
		struct mpchrconfig *mpchr = MPCHR(i);
		u8 *nkp;
		s32 nk = 0;

		if (!mpchr->numdeaths && !mpchr->numpoints) {
			for (j = 0; j < MAX_MPCHRS && !mpchr->killcounts[j]; j++);

			if (j == MAX_MPCHRS) {
				continue;
			}
		}

		netBufWriteU8(b, (u8)i);
		netBufWriteS16(b, mpchr->numdeaths);
		netBufWriteS16(b, mpchr->numpoints);
		nkp = netBufReserve(b, 1);

		for (j = 0; j < MAX_MPCHRS; j++) {
			if (mpchr->killcounts[j]) {
				netBufWriteU8(b, (u8)j);
				netBufWriteS16(b, mpchr->killcounts[j]);
				nk++;
			}
		}

		if (nkp) {
			*nkp = (u8)nk;
		}

		n++;
	}

	if (np) {
		*np = (u8)n;
	}
}

// The kill table to everyone, in this tick's events (a seat's row cleared)
void netEventsHostScores(void)
{
	struct netbuf b;

	if (!netEvHostOn()) {
		return;
	}

	netEvBegin(&b, NETEV_SCORES);
	netEvWriteScores(&b, 0);
	netEvEnd(&b, NETEV_SCORES, -1, -1);
}

// A new client in the view: its events count from the first it gets
void netEventsHostViewReset(s32 view)
{
	if (view >= 0 && view < NET_MAXVIEWS) {
		s_HostSeq[view] = 0;
	}

	if (view >= 0 && view < MAX_PLAYERS) {
		s_HudHash[view] = 0;
		s_HudTick[view] = 0;
	}
}

/**
 * A join in progress's first EVENTS, before the tick the host runs next:
 * one SCORES, the kill table up to now
 */
void netEventsHostCatchUp(s32 view)
{
	struct netbuf ev;
	struct netbuf b;
	s32 len;

	if (g_NetMode != NETMODE_SERVER || view < 0 || view >= NET_MAXVIEWS) {
		return;
	}

	netEvBegin(&ev, NETEV_SCORES);
	netEvWriteScores(&ev, NETEV_SCORES_CATCHUP);
	len = netBufLen(&ev);

	if (!netBufOk(&ev)) {
		return;
	}

	netBufInitWrite(&b, s_Msg, sizeof(s_Msg));
	netBufWriteU8(&b, NETMSG_EVENTS);
	netBufWriteU32(&b, netSessionMatchId());
	netBufWriteU32(&b, g_NetTick);
	netBufWriteU16(&b, 1);
	netBufWriteVarU32(&b, (u32)len);
	netBufWriteBytes(&b, s_One, len);

	if (netBufOk(&b)) {
		netSessionSendSlot(view, NET_CHAN_RELIABLE, s_Msg, netBufLen(&b), NET_SEND_RELIABLE);
		s_HostSeq[view]++;
		s_HostSentBytes += netBufLen(&b);
		s_HostMsgs++;
		s_HostRecorded[NETEV_SCORES]++;

		if (s_Log) {
			fprintf(s_Log, "E %u scores target view%d exclude -1 len %d catch-up\n", g_NetTick, view, len);
		}
	}
}

/*
 * Host: sending
 */

static void netEvSendSlot(s32 slot, s32 pn)
{
	struct netbuf b;
	u8 *countp = NULL;
	s32 count = 0;
	s32 off = 0;

	while (off < s_ArenaLen) {
		const s32 target = (s8)s_Arena[off];
		const s32 exclude = (s8)s_Arena[off + 1];
		const s32 len = s_Arena[off + 2] | (s_Arena[off + 3] << 8);
		const u8 *ev = s_Arena + off + 4;

		off += 4 + len;

		if ((target >= 0 && target != pn) || (exclude >= 0 && exclude == pn)) {
			continue;
		}

		if (countp && (netBufLen(&b) + len + 3 > NETEV_MAXMSG || count == 0xffff)) {
			countp[0] = (u8)(count & 0xff);
			countp[1] = (u8)(count >> 8);
			netSessionSendSlot(slot, NET_CHAN_RELIABLE, s_Msg, netBufLen(&b), NET_SEND_RELIABLE);
			s_HostSentBytes += netBufLen(&b);
			s_HostMsgs++;
			s_HostSeq[slot]++;
			countp = NULL;
		}

		if (!countp) {
			netBufInitWrite(&b, s_Msg, sizeof(s_Msg));
			netBufWriteU8(&b, NETMSG_EVENTS);
			netBufWriteU32(&b, netSessionMatchId());
			netBufWriteU32(&b, g_NetTick);
			countp = netBufReserve(&b, 2);
			count = 0;
		}

		netBufWriteVarU32(&b, (u32)len);
		netBufWriteBytes(&b, ev, len);
		count++;
	}

	if (countp && count && netBufOk(&b)) {
		countp[0] = (u8)(count & 0xff);
		countp[1] = (u8)(count >> 8);
		netSessionSendSlot(slot, NET_CHAN_RELIABLE, s_Msg, netBufLen(&b), NET_SEND_RELIABLE);
		s_HostSentBytes += netBufLen(&b);
		s_HostMsgs++;
		s_HostSeq[slot]++;
	}
}

/**
 * How many EVENTS messages have gone to a slot this match: a snapshot
 * carries it with its scenario block (netscen.c), so the client applies the
 * block only once every event the host had sent before it is in
 */
u32 netEventsHostSeq(s32 slot)
{
	return slot >= 0 && slot < NET_MAXVIEWS ? s_HostSeq[slot] : 0;
}

// ... and how many of this match's have come here (the channel is ordered)
u32 netEventsClientSeq(void)
{
	return s_RecvSeq;
}

static void netEvHostLog(const char *why)
{
	s32 i;
	char counts[512];
	s32 len = 0;

	counts[0] = '\0';

	for (i = 1; i < NETEV_COUNT; i++) {
		len += snprintf(counts + len, sizeof(counts) - len, "%s%s %u", i > 1 ? ", " : "", s_EvNames[i], s_HostRecorded[i]);

		if (len >= (s32)sizeof(counts)) {
			break;
		}
	}

	sysLogPrintf(LOG_NOTE, "net: events host %s (tick %u): recorded %u (%s); %u messages, %u bytes, %u dropped (buffer full), %u hudmsgs again within a second not sent",
			why, g_NetTick, s_HostRecordedTotal, counts, s_HostMsgs, s_HostSentBytes, s_HostDroppedFull, s_HudDupes);
}

/**
 * The tick's events to each remote slot (netTickEnd, before the snapshots;
 * and H9, before MATCH_END, so the last tick's deaths are counted first)
 */
void netEventsHostFlush(void)
{
	s32 slot;

	if (g_NetMode != NETMODE_SERVER) {
		s_ArenaLen = 0;
		return;
	}

	if (s_ArenaLen) {
		for (slot = 0; slot < MAX_PLAYERS; slot++) {
			const s32 pn = netEvPlayerOfSlot(slot);

			if (pn >= 0 && netPlayersHostSlotIsRemote(slot)) {
				netEvSendSlot(slot, pn);
			}
		}

		// a spectator: what is for everyone (it is no player: -100 is none)
		for (slot = MAX_PLAYERS; slot < NET_MAXVIEWS; slot++) {
			if (netSessionViewLive(slot)) {
				netEvSendSlot(slot, -100);
			}
		}

		s_ArenaLen = 0;
	}

	if (netSessionMatchActive() && !netSessionBarrierHeld() && !s_HostEnded) {
		if (g_NetTick % NETEV_KILLEVERY == 0 && g_NetTick) {
			netEvKillLine("tick", g_NetTick);
		}

		if (g_NetTick % 1800 == 0 && g_NetTick) {
			netEvHostLog("so far");
		}
	}
}

void netEventsHostMatchEnded(void)
{
	s32 slot;

	netEventsHostFlush();

	// an empty EVENTS: the tick the match ended on, so a client knows its
	// events are all in up to there
	for (slot = 0; slot < NET_MAXVIEWS; slot++) {
		struct netbuf b;

		if (slot < MAX_PLAYERS ? netEvPlayerOfSlot(slot) < 0 || !netPlayersHostSlotIsRemote(slot) : !netSessionViewLive(slot)) {
			continue;
		}

		netBufInitWrite(&b, s_Msg, sizeof(s_Msg));
		netBufWriteU8(&b, NETMSG_EVENTS);
		netBufWriteU32(&b, netSessionMatchId());
		netBufWriteU32(&b, g_NetTick);
		netBufWriteU16(&b, 0);
		netSessionSendSlot(slot, NET_CHAN_RELIABLE, s_Msg, netBufLen(&b), NET_SEND_RELIABLE);
		s_HostSeq[slot]++;
	}

	netEvKillLine("end", g_NetTick);
	netEvHostLog("at the match's end");
	s_HostEnded = 1;
}

/*
 * Client: receiving
 */

static void netReadRef(struct netbuf *b, struct netref *r)
{
	memset(r, 0, sizeof(*r));
	r->kind = netBufReadU8(b);

	switch (r->kind) {
	case NETREF_NONE:
		break;
	case NETREF_PLAYER:
		r->pn = netBufReadU8(b);

		if (r->pn >= MAX_PLAYERS) {
			b->error = 1;
		}
		break;
	case NETREF_ENT:
		r->id = netBufReadU16(b);
		r->gen = netBufReadU16(b);
		break;
	case NETREF_SETUP:
		r->id = netBufReadU16(b);
		r->gen = netBufReadU16(b);
		r->cmd = netBufReadU16(b);
		break;
	default:
		b->error = 1;
		break;
	}
}

static f32 netReadF(struct netbuf *b)
{
	const f32 v = netBufReadF32(b);

	// no NaN or infinity gets into the game, nor anything past the world
	if (!(v > -1048576.f && v < 1048576.f)) {
		b->error = 1;
		return 0;
	}

	return v;
}

static void netReadPos(struct netbuf *b, f32 *p)
{
	p[0] = netReadF(b);
	p[1] = netReadF(b);
	p[2] = netReadF(b);
}

static void netReadDir(struct netbuf *b, f32 *p)
{
	p[0] = netBufReadS16(b) / 8192.f;
	p[1] = netBufReadS16(b) / 8192.f;
	p[2] = netBufReadS16(b) / 8192.f;
}

// One event's payload (b holds exactly it); 0 if it does not parse
static s32 netEvParse(struct netbuf *b, struct netevc *e)
{
	s32 i;
	s32 n;

	e->type = netBufReadU8(b);

	switch (e->type) {
	case NETEV_FIRESLOT:
		netReadRef(b, &e->r0);
		e->flags = netBufReadU8(b);
		netReadPos(b, e->p0);
		netReadPos(b, e->p1);
		break;
	case NETEV_PLAYERSHOT:
		e->a = netBufReadS8(b);
		e->flags = netBufReadU8(b);
		netReadPos(b, e->p0);
		netReadPos(b, e->p1);
		e->u[0] = netBufReadU32(b);

		if (e->a < 0 || e->a >= MAX_PLAYERS) {
			return 0;
		}
		break;
	case NETEV_EXPLOSION:
		netReadRef(b, &e->r0);
		netReadPos(b, e->p0);
		n = netBufReadU8(b);

		if (n < 1 || n > NETEV_MAXROOMS) {
			return 0;
		}

		for (i = 0; i < n; i++) {
			e->rooms[i] = netBufReadS16(b);

			if (e->rooms[i] < 0 || e->rooms[i] >= g_Vars.roomcount) {
				return 0;
			}
		}

		e->rooms[n] = -1;
		e->s0 = netBufReadS16(b);
		e->a = netBufReadS8(b);
		e->flags = netBufReadU8(b);

		if (e->flags & 2) {
			netReadPos(b, e->p1);
		}

		e->s1 = netBufReadS16(b);

		if (e->flags & 4) {
			netReadPos(b, e->p2);
		}

		if (e->s0 <= EXPLOSIONTYPE_NONE || e->s0 > EXPLOSIONTYPE_HUGE25 || e->s0 == EXPLOSIONTYPE_BULLETHOLE
				|| e->s1 < 0 || e->s1 >= g_Vars.roomcount || e->a < -1 || e->a >= MAX_MPCHRS
				|| ((e->flags & 1) && (e->flags & 6) != 6)) {
			return 0;
		}
		break;
	case NETEV_SPARKS:
		e->s0 = netBufReadS16(b);
		netReadRef(b, &e->r0);
		netReadPos(b, e->p0);
		e->a = netBufReadS8(b);
		e->flags = netBufReadU8(b);

		if (e->flags & 1) {
			netReadDir(b, e->p1);
		}

		if (e->flags & 2) {
			netReadDir(b, e->p2);

			// sparksCreate divides by the normal's length
			if (e->p2[0] == 0 && e->p2[1] == 0 && e->p2[2] == 0) {
				e->p2[1] = 1;
			}
		}

		if (e->s0 < 0 || e->s0 >= g_Vars.roomcount || e->a < 0 || e->a > SPARKTYPE_DEEPWATER) {
			return 0;
		}
		break;
	case NETEV_CHRDAMAGE:
		netReadRef(b, &e->r0);
		netReadRef(b, &e->r1);
		e->a = netBufReadS8(b);
		e->flags = netBufReadU8(b);
		break;
	case NETEV_CHOKE:
		netReadRef(b, &e->r0);
		e->a = netBufReadS8(b);

		if (e->a < 0 || e->a > 8) {
			return 0;
		}
		break;
	case NETEV_DEFORM:
		netReadRef(b, &e->r0);
		e->s0 = netBufReadS16(b);
		break;
	case NETEV_GLASS:
		netReadRef(b, &e->r0);
		break;
	case NETEV_DEATH:
		e->a = netBufReadS8(b);
		e->b = netBufReadS8(b);
		e->i[0] = netBufReadS8(b);
		e->i[1] = netBufReadS8(b);

		if (e->a < -1 || e->a >= MAX_MPCHRS || e->b < -1 || e->b >= MAX_MPCHRS) {
			return 0;
		}
		break;
	case NETEV_HUDMSG:
		netBufReadString(b, e->text, NETEV_MAXTEXT + 1);
		e->a = netBufReadU8(b);

		for (i = 0; i < 3; i++) {
			e->i[i] = netBufReadS32(b);
		}

		e->u[0] = netBufReadU32(b);
		e->u[1] = netBufReadU32(b);
		e->u[2] = netBufReadU32(b);
		e->i[3] = netBufReadS32(b);
		e->u[3] = netBufReadU32(b);
		e->i[5] = netBufReadS32(b);
		e->i[4] = netBufReadS32(b);
		e->uflags = netBufReadU32(b);

		if ((u8)e->a > HUDMSGTYPE_CUTSCENESUBTITLE) {
			return 0;
		}

		// never an audio channel (hudmsgsTick indexes g_PsChannels by it
		// unchecked, and the host's numbers mean nothing here): arg14 is
		// then a time, kept to a minute
		e->uflags |= HUDMSGFLAG_NOCHANNEL;

		if (e->i[4] < 0) {
			e->i[4] = 0;
		} else if (e->i[4] > NETEV_MAXHUDTIME) {
			e->i[4] = NETEV_MAXHUDTIME;
		}
		break;
	case NETEV_PICKUPSFX:
		e->s0 = netBufReadS16(b);
		break;
	case NETEV_NBOMB:
		netReadPos(b, e->p0);
		netReadRef(b, &e->r0);
		break;
	case NETEV_GAS:
		netReadPos(b, e->p0);
		break;
	case NETEV_SCORES: {
		// checked whole here, kept as bytes until applied
		const s32 start = b->pos;
		s32 n;
		s32 k;

		e->flags = netBufReadU8(b);
		n = netBufReadU8(b);

		for (k = 0; k < n && netBufOk(b); k++) {
			s32 nk;
			s32 j;

			if (netBufReadU8(b) >= MAX_MPCHRS) {
				return 0;
			}

			netBufReadS16(b);
			netBufReadS16(b);
			nk = netBufReadU8(b);

			for (j = 0; j < nk && netBufOk(b); j++) {
				if (netBufReadU8(b) >= MAX_MPCHRS) {
					return 0;
				}

				netBufReadS16(b);
			}
		}

		if (!netBufOk(b) || netBufRemaining(b) != 0) {
			return 0;
		}

		e->bloblen = b->pos - start;
		e->blob = malloc(e->bloblen);

		if (!e->blob) {
			return 0;
		}

		memcpy(e->blob, b->data + start, e->bloblen);
		break;
	}
	default:
		return 0;
	}

	return netBufOk(b) && netBufRemaining(b) == 0;
}

static void netEvQueueAlloc(void)
{
	if (!s_Q) {
		s_Q = calloc(NETEV_QUEUE, sizeof(*s_Q));
		s_QText = calloc(NETEV_QUEUE, NETEV_MAXTEXT + 1);

		if (!s_Q || !s_QText) {
			free(s_Q);
			free(s_QText);
			s_Q = NULL;
			s_QText = NULL;
		}
	}
}

static void netEvApply(struct netevc *e, f64 rt);

void netEventsClientOnMsg(const u8 *data, s32 len)
{
	struct netbuf b;
	u32 matchid;
	u32 tick;
	s32 count;
	s32 i;

	if (!netSessionMatchLoading() || s_ClientEnded) {
		return;
	}

	netEvQueueAlloc();

	if (!s_Q) {
		return;
	}

	netBufInitRead(&b, data + 1, len - 1);
	matchid = netBufReadU32(&b);
	tick = netBufReadU32(&b);
	count = netBufReadU16(&b);

	if (!netBufOk(&b)) {
		s_Malformed++;
		return;
	}

	if (matchid != netSessionMatchId()) {
		s_OtherMatch++;
		return;
	}

	s_RecvMsgs++;
	s_RecvSeq++;
	s_RecvBytes += len;

	if (s_HaveTick && tick < s_LastTick) {
		s_OutOfOrder++;
	}

	s_LastTick = tick;
	s_HaveTick = 1;

	for (i = 0; i < count; i++) {
		const u32 elen = netBufReadVarU32(&b);
		const u8 *ev = netBufSkip(&b, (s32)(elen > 0x7fff ? 0x7fff : elen));
		struct netbuf eb;
		struct netevc *e;
		s32 slot;

		if (!netBufOk(&b) || !ev || elen == 0) {
			s_Malformed++;
			return;
		}

		if (s_QCount == NETEV_QUEUE) {
			// full (no render clock for a long time): the oldest goes now
			s_Overflow++;
			netEvApply(&s_Q[s_QHead], -1);
			s_QHead = (s_QHead + 1) % NETEV_QUEUE;
			s_QCount--;
		}

		slot = (s_QHead + s_QCount) % NETEV_QUEUE;
		e = &s_Q[slot];
		memset(e, 0, sizeof(*e));
		e->text = s_QText + (size_t)slot * (NETEV_MAXTEXT + 1);
		e->text[0] = '\0';
		e->tick = tick;

		netBufInitRead(&eb, ev, (s32)elen);

		if (!netEvParse(&eb, e)) {
			s_Malformed++;
			continue;
		}

		s_Received[e->type]++;
		s_QCount++;

		if (s_QCount > s_QMax) {
			s_QMax = s_QCount;
		}
	}

	if (netBufRemaining(&b) != 0) {
		s_Malformed++;
	}
}

/*
 * Client: applying
 */

static struct prop *netEvResolve(const struct netref *r)
{
	switch (r->kind) {
	case NETREF_PLAYER:
		if (r->pn < PLAYERCOUNT() && g_Vars.players[r->pn]) {
			return g_Vars.players[r->pn]->prop;
		}
		return NULL;
	case NETREF_ENT:
		// mapped to a prop of this machine's, or one the pose step made for
		// it (a dropped gun, a Mod.Bodies corpse)
		return netPuppetsLocalProp(r->id, r->gen);
	case NETREF_SETUP: {
		// mapped, or never in this machine's scope: its own object of that
		// setup command (the stage made the same ones)
		struct prop *prop = netPuppetsLocalProp(r->id, r->gen);

		if (prop) {
			return prop;
		}

		prop = netEntsSetupLocal(r->cmd);
		s_ViaSetup = prop != NULL;
		return prop;
	}
	default:
		return NULL;
	}
}

static struct prop *netLocalProp(void)
{
	if (netSessionSpectating()) {
		return NULL;
	}

	if (g_NetLocalSlot >= 0 && g_NetLocalSlot < PLAYERCOUNT() && g_Vars.players[g_NetLocalSlot]) {
		return g_Vars.players[g_NetLocalSlot]->prop;
	}

	return NULL;
}

// A chr this machine poses from the snapshots (never its own player)
static struct chrdata *netEvPuppetChr(const struct netref *r)
{
	struct prop *prop = netEvResolve(r);

	if (!prop || prop == netLocalProp() || !netIsPuppet(prop) || !prop->chr || !prop->chr->model) {
		return NULL;
	}

	return prop->chr;
}

static s32 netEvMpIndexOf(s32 cfgslot)
{
	s32 i;

	if (cfgslot < 0) {
		return -1;
	}

	for (i = 0; i < g_MpNumChrs; i++) {
		if (func0f18d074(i) == cfgslot) {
			return i;
		}
	}

	return -1;
}

static void netEvLogApplied(const struct netevc *e, f64 rt, const char *extra)
{
	if (!s_Log) {
		return;
	}

	// its time: ENet's clock (ms) and the audio frame the next mix lands at
	fprintf(s_Log, "A %u %.2f %s %u %llu%s%s\n", e->tick, rt, s_EvNames[e->type], netTransportTime(),
			(unsigned long long)audioGetFramesQueued(), extra[0] ? " " : "", extra);
}

/**
 * SCORES: the host's kill table in place of this machine's (deaths, points
 * and kill counts; rows it left out are 0). Returns the rows it had.
 */
static s32 netEvApplyScores(const struct netevc *e)
{
	struct netbuf b;
	s32 n;
	s32 i;
	s32 j;

	if (!e->blob) {
		return 0;
	}

	for (i = 0; i < MAX_MPCHRS; i++) {
		struct mpchrconfig *mpchr = MPCHR(i);

		mpchr->numdeaths = 0;
		mpchr->numpoints = 0;

		for (j = 0; j < MAX_MPCHRS; j++) {
			mpchr->killcounts[j] = 0;
		}
	}

	netBufInitRead(&b, e->blob, e->bloblen);
	netBufReadU8(&b);
	n = netBufReadU8(&b);

	for (i = 0; i < n && netBufOk(&b); i++) {
		const s32 row = netBufReadU8(&b); // MPCHR reads its argument twice
		struct mpchrconfig *mpchr = MPCHR(row);
		s32 nk;

		mpchr->numdeaths = netBufReadS16(&b);
		mpchr->numpoints = netBufReadS16(&b);
		nk = netBufReadU8(&b);

		for (j = 0; j < nk && netBufOk(&b); j++) {
			const s32 k = netBufReadU8(&b);

			mpchr->killcounts[k] = netBufReadS16(&b);
		}
	}

	return n;
}

static void netEvApply(struct netevc *e, f64 rt)
{
	struct coord c0;
	struct coord c1;
	struct coord c2;
	struct chrdata *chr;
	struct prop *prop;
	char extra[160];
	s32 prevplayernum;

	extra[0] = '\0';
	c0.x = e->p0[0]; c0.y = e->p0[1]; c0.z = e->p0[2];
	c1.x = e->p1[0]; c1.y = e->p1[1]; c1.z = e->p1[2];
	c2.x = e->p2[0]; c2.y = e->p2[1]; c2.z = e->p2[2];

	if (rt >= 0) {
		s_LagSum += rt - e->tick;
		s_LagN++;
	}

	const u32 unresolved = s_Unresolved;

	s_Applying = 1;
	s_ViaSetup = 0;

	switch (e->type) {
	case NETEV_FIRESLOT:
		chr = netEvPuppetChr(&e->r0);

		if (!chr) {
			s_Unresolved++;
			s_UnresolvedBy[e->type]++;
			break;
		}

		{
			const s32 was = chr->hidden2 & CHRH2FLAG_FIRESOUNDDONE;
			struct prop *me = netLocalProp();
			s32 started;

			s_ShotNoGun += chrGetHeldProp(chr, e->flags & 1) == NULL;
			chrUpdateFireslot(chr, e->flags & 1, (e->flags & 2) != 0 && !s_Quiet, (e->flags & 4) != 0, &c0, &c1);
			started = !was && (chr->hidden2 & CHRH2FLAG_FIRESOUNDDONE);

			// the audio check's null: what would have begun a sound
			if (s_Quiet && (e->flags & 2) && !was) {
				started = 1;
				chr->hidden2 |= CHRH2FLAG_FIRESOUNDDONE;
			}
			s_ShotPlayed += started;

			// started: the gun's sound began (else one still sounding);
			// dist: how far from this machine's player, for the audio check
			snprintf(extra, sizeof(extra), "chr %d hand %d sound %d beam %d started %d dist %.0f", e->r0.id, e->flags & 1,
					(e->flags & 2) != 0, (e->flags & 4) != 0, started,
					me ? sqrtf((chr->prop->pos.x - me->pos.x) * (chr->prop->pos.x - me->pos.x)
						+ (chr->prop->pos.y - me->pos.y) * (chr->prop->pos.y - me->pos.y)
						+ (chr->prop->pos.z - me->pos.z) * (chr->prop->pos.z - me->pos.z)) : -1.f);
		}

		s_ShotSounds += (e->flags & 2) != 0;
		break;
	case NETEV_PLAYERSHOT:
		if (e->a == g_NetLocalSlot && !netSessionSpectating()) {
			// this machine's own shot: drawn when it fired, if it did
			s32 mine = 0;
			s32 k;

			for (k = 0; k < NETEV_MYSHOTS; k++) {
				const u32 t = s_MyShots[k];

				if (t && t >= e->u[0] && t - 1 <= e->u[0] + 1) {
					mine = 1;
					break;
				}
			}

			s_OwnDupes += mine;
			s_OwnUnpredicted += !mine;
			snprintf(extra, sizeof(extra), "player %d hand %d own cmd %u %s", e->a, e->flags & 1, e->u[0], mine ? "fired here: dropped" : "not fired here");
			break;
		}

		prop = e->a < PLAYERCOUNT() && g_Vars.players[(s32)e->a] ? g_Vars.players[(s32)e->a]->prop : NULL;

		if (!prop || prop == netLocalProp() || !netIsPuppet(prop) || !prop->chr) {
			s_Unresolved++;
			s_UnresolvedBy[e->type]++;
			break;
		}

		// the puppet's held gun sounds and draws the tracer, as a sim's shot does
		prop->chr->hidden2 &= ~CHRH2FLAG_FIRESOUNDDONE;
		chrUpdateFireslot(prop->chr, e->flags & 1, !s_Quiet, (e->flags & 4) != 0, &c0, &c1);
		s_ShotSounds++;
		snprintf(extra, sizeof(extra), "player %d hand %d beam %d", e->a, e->flags & 1, (e->flags & 4) != 0);
		break;
	case NETEV_EXPLOSION:
		prop = netEvResolve(&e->r0);

		// only a prop this machine keeps for the length of the explosion
		if (prop && prop->type != PROPTYPE_CHR && prop->type != PROPTYPE_PLAYER && prop->type != PROPTYPE_OBJ) {
			prop = NULL;
		}

		explosionCreate(prop, &c0, e->rooms, e->s0, e->a, e->flags & 1, (e->flags & 2) ? &c1 : NULL, e->s1, (e->flags & 4) ? &c2 : NULL);
		snprintf(extra, sizeof(extra), "type %d at %.0f %.0f %.0f", e->s0, c0.x, c0.y, c0.z);
		break;
	case NETEV_SPARKS:
		prop = netEvResolve(&e->r0);

		// a chr's blood colour comes from its body
		if (prop && prop->type == PROPTYPE_CHR && !prop->chr) {
			prop = NULL;
		}

		sparksCreate(e->s0, prop, &c0, (e->flags & 1) ? &c1 : NULL, (e->flags & 2) ? &c2 : NULL, e->a);
		snprintf(extra, sizeof(extra), "type %d", e->a);
		break;
	case NETEV_CHRDAMAGE: {
		struct prop *victim = netEvResolve(&e->r0);
		struct prop *attacker = netEvResolve(&e->r1);
		struct prop *me = netLocalProp();

		// prop->chr shares its place with obj/door/weapon
		if (!victim || (victim->type != PROPTYPE_CHR && victim->type != PROPTYPE_PLAYER) || !victim->chr) {
			s_Unresolved++;
			s_UnresolvedBy[e->type]++;
			break;
		}

		if (victim == me) {
			// the red flash and the health bar; the health is the block's
			s_HitsOnMe++;
			prevplayernum = g_Vars.currentplayernum;
			setCurrentPlayerNum(g_NetLocalSlot);

			if (!g_Vars.currentplayer->isdead) {
				playerDisplayDamage();
				playerDisplayHealth();
			}

			setCurrentPlayerNum(prevplayernum);
		} else if (netIsPuppet(victim) && victim->chr->model) {
			if ((e->flags & 1) && (e->flags & 4)) {
				shieldhitCreate(victim, chrGetShield(victim->chr) > 0 ? chrGetShield(victim->chr) : 1, NULL, NULL, NULL, 0, 0);
			} else if (victim->chr->actiontype != ACT_DIE && victim->chr->actiontype != ACT_DEAD) {
				chrFlinchBody(victim->chr);
			}
		}

		if (attacker && attacker == me) {
			s_MyHits++;
		}

		snprintf(extra, sizeof(extra), "victim %s%d attacker %s%d hitpart %d flags %d%s%s",
				e->r0.kind == NETREF_PLAYER ? "player " : "id ", e->r0.kind == NETREF_PLAYER ? e->r0.pn : e->r0.id,
				e->r1.kind == NETREF_PLAYER ? "player " : e->r1.kind ? "id " : "none ", e->r1.kind == NETREF_PLAYER ? e->r1.pn : e->r1.id,
				e->a, e->flags, victim == me ? " ON-ME" : "", attacker && attacker == me ? " BY-ME" : "");
		break;
	}
	case NETEV_CHOKE:
		prop = netEvResolve(&e->r0);

		if (!prop || (prop->type != PROPTYPE_CHR && prop->type != PROPTYPE_PLAYER) || !prop->chr || !prop->chr->model) {
			s_Unresolved++;
			s_UnresolvedBy[e->type]++;
			break;
		}

		if (!s_Quiet) {
			chrChoke(prop->chr, e->a);
		}
		break;
	case NETEV_DEFORM:
		prop = netEvResolve(&e->r0);

		if (!prop || !prop->obj || (prop->type != PROPTYPE_OBJ && prop->type != PROPTYPE_DOOR && prop->type != PROPTYPE_WEAPON)) {
			s_Unresolved++;
			s_UnresolvedBy[e->type]++;
			break;
		}

		objDeform(prop->obj, e->s0);
		break;
	case NETEV_GLASS:
		prop = netEvResolve(&e->r0);

		// glassDestroy reads the glass's bbox unchecked
		if (!prop || !prop->obj || prop->type != PROPTYPE_OBJ
				|| (prop->obj->type != OBJTYPE_GLASS && prop->obj->type != OBJTYPE_TINTEDGLASS)
				|| !prop->obj->model || !objFindBboxRodata(prop->obj)) {
			s_Unresolved++;
			s_UnresolvedBy[e->type]++;
			break;
		}

		glassDestroy(prop->obj);
		s_GlassViaSetup += s_ViaSetup;
		snprintf(extra, sizeof(extra), "%s", s_ViaSetup ? "by its setup command" : "mapped");
		break;
	case NETEV_DEATH: {
		const s32 a = netEvMpIndexOf(e->a);
		const s32 v = netEvMpIndexOf(e->b);

		// the host's hudmsgs for this machine's player come as their own events
		s_SuppressHud = 1;
		mpstatsRecordDeath(e->i[0] == -1 ? -1 : a, e->i[1] == -1 ? -1 : v);
		s_SuppressHud = 0;
		snprintf(extra, sizeof(extra), "attacker %d (mp %d) victim %d (mp %d)", e->a, a, e->b, v);
		break;
	}
	case NETEV_HUDMSG:
		if (g_NetLocalSlot < 0 || g_NetLocalSlot >= PLAYERCOUNT()) {
			snprintf(extra, sizeof(extra), "dropped: local slot %d of %d", g_NetLocalSlot, PLAYERCOUNT());
			break;
		}

		prevplayernum = g_Vars.currentplayernum;
		setCurrentPlayerNum(g_NetLocalSlot);
		hudmsgCreateFromArgs(e->text, (u8)e->a, e->i[0], e->i[1], e->i[2],
				g_HudmsgTypes[(u8)e->a].unk04, g_HudmsgTypes[(u8)e->a].unk08,
				e->u[0], e->u[1], e->u[2], e->i[3], e->u[3], e->i[5], e->i[4], e->uflags);
		setCurrentPlayerNum(prevplayernum);
		s_HudForMe++;

		if (!s_Quiet) {
			netScenClientHudmsg(e->text); // a sound the host's scenario code makes with it
		}

		snprintf(extra, sizeof(extra), "\"%.100s\"", e->text);

		// a log line wants one line
		for (char *p = extra; *p; p++) {
			if (*p == '\n' || *p == '\r') {
				*p = ' ';
			}
		}
		break;
	case NETEV_PICKUPSFX:
		if (!geSfxPickup(e->s0, NULL)) {
			sndStart(var80095200, e->s0, NULL, -1, -1, -1, -1, -1);
		}

		snprintf(extra, sizeof(extra), "sound %d", e->s0);
		break;
	case NETEV_NBOMB:
		nbombCreateStorm(&c0, netEvResolve(&e->r0));
		break;
	case NETEV_GAS:
		// gasTick fades to the stage's second sky, which a stage with no
		// sky of its own has not got (envApplyTransitionFrac would read
		// NULL): only the host's word for it, so never unchecked
		if (!g_EnvTransitionFrom || !g_EnvTransitionTo) {
			s_Unresolved++;
			s_UnresolvedBy[e->type]++;
			break;
		}

		gasReleaseFromPos(&c0);
		s_GasReleased++;
		break;
	case NETEV_SCORES:
		snprintf(extra, sizeof(extra), "%s, %d rows", (e->flags & NETEV_SCORES_CATCHUP) ? "catch-up" : "a seat cleared",
				netEvApplyScores(e));
		break;
	default:
		break;
	}

	free(e->blob);
	e->blob = NULL;
	s_Applying = 0;

	// applied: the event acted here (one dropped as unresolved did not)
	if (s_Unresolved == unresolved) {
		s_Applied[e->type]++;
	}

	netEvLogApplied(e, rt, extra);
}

/**
 * After the pose step: every event the render clock has reached, in order.
 * Before the first snapshot set the clock nothing is applied (the queue
 * holds them).
 */
void netEventsClientDrain(s32 haveclock, f64 rt)
{
	if (!s_Q || !haveclock || s_ClientEnded) {
		return;
	}

	while (s_QCount) {
		struct netevc *e = &s_Q[s_QHead];

		if ((f64)e->tick > rt) {
			break;
		}

		// a join in progress: the table starts at its catch-up, so the
		// samples before it are not this machine's to take
		if (e->type == NETEV_SCORES && (e->flags & NETEV_SCORES_CATCHUP) && e->tick > s_NextKill) {
			s_NextKill = (e->tick + NETEV_KILLEVERY - 1) / NETEV_KILLEVERY * NETEV_KILLEVERY;
		}

		// the kill table as it stood after every event up to the sample's tick
		while (e->tick > s_NextKill) {
			netEvKillLine("tick", s_NextKill);
			s_NextKill += NETEV_KILLEVERY;
		}

		// the scenario's blocks from before this tick first (netscen.c)
		netScenClientBeforeEvent(e->tick);
		netEvApply(e, rt);
		s_QHead = (s_QHead + 1) % NETEV_QUEUE;
		s_QCount--;
	}

	// ... and every block the render clock has reached, after its tick's events
	netScenClientUpTo(rt);

	// a sample with nothing after it: once the clock is well past it
	while (!s_QCount && rt >= (f64)(s_NextKill + NETEV_KILLSETTLE)) {
		netEvKillLine("tick", s_NextKill);
		s_NextKill += NETEV_KILLEVERY;
	}
}

static void netEvClientLog(const char *why)
{
	char counts[512];
	char ucounts[256];
	s32 len = 0;
	s32 i;

	counts[0] = '\0';

	for (i = 1; i < NETEV_COUNT; i++) {
		len += snprintf(counts + len, sizeof(counts) - len, "%s%s %u/%u", i > 1 ? ", " : "", s_EvNames[i], s_Applied[i], s_Received[i]);

		if (len >= (s32)sizeof(counts)) {
			break;
		}
	}

	{
		s32 ulen = 0;

		ucounts[0] = '\0';

		for (i = 1; i < NETEV_COUNT; i++) {
			if (s_UnresolvedBy[i] && ulen < (s32)sizeof(ucounts)) {
				ulen += snprintf(ucounts + ulen, sizeof(ucounts) - ulen, "%s%s %u", ulen ? ", " : "", s_EvNames[i], s_UnresolvedBy[i]);
			}
		}
	}

	sysLogPrintf(LOG_NOTE, "net: events client %s (tick %u): applied/received %s; %u messages, %u bytes, out of order %u, malformed %u, other match %u, unresolved %u, overflow %u, queued max %d, lag mean %.1f ticks; shot sounds %u (no gun %u, started %u), hits on me %u, my hits %u, hudmsgs for me %u, own deaths refused %u, own shots back %u (fired here %u, not %u); glass by setup command %u, gas released %u; unresolved by type: %s",
			why, g_NetTick, counts, s_RecvMsgs, s_RecvBytes, s_OutOfOrder, s_Malformed, s_OtherMatch, s_Unresolved, s_Overflow,
			s_QMax, s_LagN ? s_LagSum / s_LagN : 0.0, s_ShotSounds, s_ShotNoGun, s_ShotPlayed, s_HitsOnMe, s_MyHits, s_HudForMe, s_LocalSkipped,
			s_OwnDupes + s_OwnUnpredicted, s_OwnDupes, s_OwnUnpredicted, s_GlassViaSetup, s_GasReleased, ucounts[0] ? ucounts : "none");
}

void netEventsClientTickEnd(void)
{
	if (g_NetTick % 1800 == 0 && g_NetTick && !s_ClientEnded) {
		netEvClientLog("so far");
	}
}

/**
 * MATCH_END came (on the channel the events come on, so after the last of
 * them): everything queued is applied now, and the live table logged before
 * the host's numbers are put over it (H10)
 */
void netEventsClientMatchEnd(void)
{
	while (s_Q && s_QCount) {
		struct netevc *e = &s_Q[s_QHead];

		if (e->type == NETEV_SCORES && (e->flags & NETEV_SCORES_CATCHUP) && e->tick > s_NextKill) {
			s_NextKill = (e->tick + NETEV_KILLEVERY - 1) / NETEV_KILLEVERY * NETEV_KILLEVERY;
		}

		while (e->tick > s_NextKill) {
			netEvKillLine("tick", s_NextKill);
			s_NextKill += NETEV_KILLEVERY;
		}

		netEvApply(e, -1);
		s_QHead = (s_QHead + 1) % NETEV_QUEUE;
		s_QCount--;
	}

	// the samples after the last event, up to the tick the host ended on
	while (s_NextKill <= s_LastTick) {
		netEvKillLine("tick", s_NextKill);
		s_NextKill += NETEV_KILLEVERY;
	}

	netEvKillLine("end", s_LastTick);
	netEvClientLog("at the match's end");
	s_ClientEnded = 1;
}

/*
 * Stage
 */

void netEventsStageStart(void)
{
	s_ArenaLen = 0;
	memset(s_HostRecorded, 0, sizeof(s_HostRecorded));
	s_HostSentBytes = 0;
	s_HostMsgs = 0;
	memset(s_HostSeq, 0, sizeof(s_HostSeq));
	s_HostDroppedFull = 0;
	s_HostRecordedTotal = 0;
	s_HostEnded = 0;
	memset(s_HudHash, 0, sizeof(s_HudHash));
	memset(s_HudTick, 0, sizeof(s_HudTick));
	s_HudDupes = 0;
	memset(s_MyShots, 0, sizeof(s_MyShots));
	s_MyShotsN = 0;
	s_OwnDupes = 0;
	s_OwnUnpredicted = 0;

	s_QHead = 0;
	s_QCount = 0;
	s_QMax = 0;
	s_LastTick = 0;
	s_HaveTick = 0;
	s_Applying = 0;
	s_SuppressHud = 0;
	s_ClientEnded = 0;
	memset(s_Received, 0, sizeof(s_Received));
	memset(s_Applied, 0, sizeof(s_Applied));
	s_RecvMsgs = 0;
	s_RecvSeq = 0;
	s_RecvBytes = 0;
	s_OutOfOrder = 0;
	s_Malformed = 0;
	s_OtherMatch = 0;
	s_Unresolved = 0;
	memset(s_UnresolvedBy, 0, sizeof(s_UnresolvedBy));
	s_GlassViaSetup = s_GasReleased = 0;
	s_Overflow = 0;
	s_HitsOnMe = 0;
	s_MyHits = 0;
	s_HudForMe = 0;
	s_ShotSounds = 0;
	s_ShotNoGun = 0;
	s_ShotPlayed = 0;
	s_LagSum = 0;
	s_LagN = 0;
	s_NextKill = NETEV_KILLEVERY;
	s_LocalSkipped = 0;

	s_Quiet = sysArgCheck("--net-test-quiet-events");

	if (g_NetMode != NETMODE_NONE && netSessionMatchLoading()) {
		netEvLogOpen();

		if (s_Log) {
			fprintf(s_Log, "S %s match %u\n", g_NetMode == NETMODE_SERVER ? "host" : "client", netSessionMatchId());
		}
	}
}

void netEventsMatchStopped(void)
{
	s_ArenaLen = 0;

	while (s_Q && s_QCount) {
		free(s_Q[s_QHead].blob);
		s_Q[s_QHead].blob = NULL;
		s_QHead = (s_QHead + 1) % NETEV_QUEUE;
		s_QCount--;
	}

	s_QCount = 0;
	s_QHead = 0;

	if (s_Log) {
		fflush(s_Log);
	}
}
