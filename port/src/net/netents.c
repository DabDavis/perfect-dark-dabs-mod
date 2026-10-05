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
#include "lib/memp.h"
#include "lib/model.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/playermgr.h"
#include "game/setuputils.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "net/nettransport.h"
#include "netint.h"

/**
 * The entity table and snapshots, the game's side (PLANS/netplay/
 * spec-entities.md §1, §6, §7; the wire in netsnap.c and netproto.h).
 *
 * Stage 4a: the host enumerates its props, quantizes each into a fixed
 * record, and sends every client a snapshot every two or three ticks,
 * delta'd against what that client acked. The client decodes into its
 * baseline ring, which is the interpolation buffer, keyed by host tick, and
 * maps host ids to its own props by descriptor with a generation check.
 * Nothing decoded is applied to the client's world yet (4b).
 *
 * Entity id = prop pool index; its generation lives in g_NetPropGen, bumped
 * by propAllocate and propFree (hooks H1-H3), never a field of struct prop.
 */

u16 *g_NetPropGen = NULL;
static s32 s_PropGenCount = 0;

#define NETENT_NEAR       2500.f // an object this close to a client's player is in its scope
#define NETENT_PROJNEAR   8000.f
#define NETENT_SEENTICKS  60     // ... or seen on its screen within this many ticks
#define NETENT_TELEPORT   300.f  // a jump this far in a tick is a teleport

// The host's view of one prop for this tick
struct netcap {
	u8 valid;
	u8 isproj;
	s8 playernum;    // a player's prop: its playernum
	struct netdesc desc;
	struct netentstate st;
	u8 record[NETREC_MAX];
};

// The client's map from a host id to a prop of its own
#define NETMAP_NONE       0
#define NETMAP_MAPPED     1
#define NETMAP_UNRESOLVED 2 // nothing here to map it to (4b creates it)
#define NETMAP_STALE      3 // the local prop went: descriptor asked for again

struct netmap {
	struct prop *local;
	u16 localgen;
	u16 hostgen;
	u8 state;
	u8 nackqueued;
	u16 seen;        // the last snapshot it was present in
};

// both sides
static s32 s_MaxIds = 0;
static s32 *s_SetupCmdOfProp = NULL;  // [maxids]: setup command index, -1
static u16 *s_SetupGenOfProp = NULL;  // [maxids]: the prop's gen when the table was built
static s32 *s_SetupPropOfCmd = NULL;  // [numcmds]: prop index, -1
static s32 s_NumCmds = 0;

// host
static struct netcap *s_Cap = NULL;           // [maxids]
static u32 *s_Seen[MAX_PLAYERS];              // [maxids]: tick + 1 last on that player's screen
static struct netsnaphost s_Hosts[MAX_PLAYERS];
static u32 s_NextSnap[MAX_PLAYERS];
static struct netsnapent *s_Ents = NULL;      // [maxids]
static u8 s_LpRespawns[MAX_PLAYERS];
static u8 s_LpTeleports[MAX_PLAYERS];
static s32 s_LpWasDead[MAX_PLAYERS];
static struct coord s_LpPrevPos[MAX_PLAYERS];
static s32 s_LpHavePrev[MAX_PLAYERS];
static f32 *s_TelePrev = NULL;                // [maxids * 3]: a chr's position at its last capture
static u8 *s_TeleCount = NULL;                  // [maxids]
static u16 *s_TeleGen = NULL;                 // [maxids]
static u32 s_OfferedSum[MAX_PLAYERS];
static u8 s_Pkt[NET_MAXUNRELIABLE];

// client
static struct netsnapclient s_Client;
static struct netmap *s_Map = NULL;           // [maxids]
static u32 s_Mapped = 0;
static u32 s_Unresolved = 0;
static u32 s_StaleSeen = 0;
static u32 s_NacksSent = 0;
static u32 s_PresentUnmapped = 0; // in the newest snapshot: present, not mapped or unresolved
static u32 s_NoneNacked = 0;      // present with no mapping and no descriptor: asked
static u32 s_KeptOverGap = 0;     // mappings kept for ids back after a gap
static u32 s_Misfits = 0;         // descriptors naming a local prop of the wrong kind
static u16 s_LastMapSeq = 0;
static u16 s_NackRing[64];
static s32 s_NackCount = 0;
struct netlocalhist {
	u32 tick;
	f32 pos[3];
	s32 valid;
};
#define NET_LOCALHIST 256
static struct netlocalhist s_LocalHist[NET_LOCALHIST];
static u32 s_ErrN = 0;
static f64 s_ErrSum = 0;
static f32 s_ErrMax = 0;
static u32 s_ErrOver = 0;
static u32 s_ErrMissing = 0;
static u32 s_ErrHuge = 0;
static u8 s_LastRespawns = 0;
static u8 s_LastTeleports = 0;
static s32 s_LpSeen = 0;
static u32 s_RespawnsSeen = 0;
static u32 s_TeleportsSeen = 0;

#define NET_HARDCORRECT 200.f   // phase 4: a local error past this would be corrected outright

// debug
static FILE *s_DumpFile = NULL;
static char s_DumpPath[512];
static u32 s_DumpFrom = 300;
static u32 s_DumpTo = 1200;
static s32 s_Hostile = 0;
static u32 s_TestStaleTick = 0;  // --net-test-stale TICK: the client drops a mapped prop's generation
static u32 s_HostileFed = 0;
static u32 s_HostileRejected = 0;
static u32 s_HostileAccepted = 0; // mangled snapshots the client's decoder parsed whole
static u32 s_HostileDropped = 0;  // old, another match, no baseline

extern s32 g_StageNum;
bool chrIsGunfireVisible(struct chrdata *chr, s32 hand);

/*
 * Hooks H1-H3: the prop generation side table
 */

void netPropGenAlloc(s32 maxprops)
{
	s32 i;

	g_NetPropGen = NULL;
	s_PropGenCount = 0;

	if (maxprops <= 0) {
		return;
	}

	g_NetPropGen = mempAlloc(ALIGN64(maxprops * sizeof(u16)), MEMPOOL_STAGE);

	if (!g_NetPropGen) {
		return;
	}

	// 1, so 0 can mean "never" on the wire
	for (i = 0; i < maxprops; i++) {
		g_NetPropGen[i] = 1;
	}

	s_PropGenCount = maxprops;
}

static s32 netPropIndex(const struct prop *prop)
{
	intptr_t idx;

	if (!prop || !g_Vars.props) {
		return -1;
	}

	idx = prop - g_Vars.props;

	return idx >= 0 && idx < g_Vars.maxprops ? (s32)idx : -1;
}

void netPropGenBump(struct prop *prop)
{
	const s32 idx = netPropIndex(prop);

	if (g_NetPropGen && idx >= 0 && idx < s_PropGenCount) {
		g_NetPropGen[idx]++;

		if (g_NetPropGen[idx] == 0) {
			g_NetPropGen[idx] = 1;
		}
	}
}

static u16 netPropGen(s32 idx)
{
	return g_NetPropGen && idx >= 0 && idx < s_PropGenCount ? g_NetPropGen[idx] : 0;
}

/*
 * Boot
 */

void netEntsArgs(void)
{
	const char *dump = sysArgGetString("--net-snap-dump");
	const char *ticks = sysArgGetString("--net-snap-dump-ticks");

	if (dump) {
		snprintf(s_DumpPath, sizeof(s_DumpPath), "%s", dump);
	}

	if (ticks) {
		unsigned int a;
		unsigned int b;

		if (sscanf(ticks, "%u,%u", &a, &b) == 2 && a <= b) {
			s_DumpFrom = a;
			s_DumpTo = b;
		}
	}

	s_Hostile = sysArgCheck("--net-test-hostile");
	s_TestStaleTick = (u32)sysArgGetInt("--net-test-stale", 0);
}

static FILE *netDumpFile(void)
{
	if (!s_DumpFile && s_DumpPath[0]) {
		s_DumpFile = fopen(s_DumpPath, "w");

		if (!s_DumpFile) {
			sysLogPrintf(LOG_ERROR, "net: --net-snap-dump %s: cannot open it", s_DumpPath);
			s_DumpPath[0] = '\0';
		}
	}

	return s_DumpFile;
}

static void netDumpState(FILE *f, const struct netentstate *st)
{
	fprintf(f, " %.4f %.4f %.4f %.5f %d %.4f %.6f %.6f %.6f %.6f %.6f %u\n",
			st->pos[0], st->pos[1], st->pos[2], st->yaw, st->animnum, st->frame, st->doorfrac,
			st->quat[0], st->quat[1], st->quat[2], st->quat[3], st->teleports);
}

/*
 * The stage's setup table: setup command index <-> prop index, built on
 * both sides at the stage's start (after lvReset). setupGetCmdIndexByProp
 * is not used: it reads padding on x86_64 (spec-entities.md correction 2).
 */

static s32 netCmdIsObj(u8 type)
{
	switch (type) {
	case OBJTYPE_DOOR:
	case OBJTYPE_BASIC:
	case OBJTYPE_KEY:
	case OBJTYPE_ALARM:
	case OBJTYPE_CCTV:
	case OBJTYPE_AMMOCRATE:
	case OBJTYPE_WEAPON:
	case OBJTYPE_SINGLEMONITOR:
	case OBJTYPE_MULTIMONITOR:
	case OBJTYPE_HANGINGMONITORS:
	case OBJTYPE_AUTOGUN:
	case OBJTYPE_DEBRIS:
	case OBJTYPE_HAT:
	case OBJTYPE_MULTIAMMOCRATE:
	case OBJTYPE_SHIELD:
	case OBJTYPE_GASBOTTLE:
	case OBJTYPE_TRUCK:
	case OBJTYPE_HELI:
	case OBJTYPE_29:
	case OBJTYPE_GLASS:
	case OBJTYPE_SAFE:
	case OBJTYPE_TANK:
	case OBJTYPE_TINTEDGLASS:
	case OBJTYPE_LIFT:
	case OBJTYPE_HOVERBIKE:
	case OBJTYPE_HOVERPROP:
	case OBJTYPE_FAN:
	case OBJTYPE_HOVERCAR:
	case OBJTYPE_CHOPPER:
	case OBJTYPE_ESCASTEP:
		return 1;
	}

	return 0;
}

static void netEntsBuildSetupTable(void)
{
	u32 *cmd = g_StageSetup.props;
	s32 cmdindex = 0;
	s32 i;

	for (i = 0; i < s_MaxIds; i++) {
		s_SetupCmdOfProp[i] = -1;
		s_SetupGenOfProp[i] = 0;
	}

	s_NumCmds = 0;

	if (!cmd) {
		return;
	}

	// count first, then fill
	while ((u8)PD_BE32(cmd[0]) != OBJTYPE_END && cmdindex < 0xffff) {
		cmd += setupGetCmdLength(cmd);
		cmdindex++;
	}

	s_NumCmds = cmdindex;
	s_SetupPropOfCmd = calloc(s_NumCmds + 1, sizeof(s32));

	if (!s_SetupPropOfCmd) {
		s_NumCmds = 0;
		return;
	}

	cmd = g_StageSetup.props;

	for (cmdindex = 0; cmdindex < s_NumCmds; cmdindex++) {
		s_SetupPropOfCmd[cmdindex] = -1;

		if (netCmdIsObj((u8)PD_BE32(cmd[0]))) {
			struct defaultobj *obj = (struct defaultobj *)cmd;
			const s32 idx = netPropIndex(obj->prop);

			if (idx >= 0 && obj->prop->obj == obj) {
				s_SetupCmdOfProp[idx] = cmdindex;
				s_SetupGenOfProp[idx] = netPropGen(idx);
				s_SetupPropOfCmd[cmdindex] = idx;
			}
		}

		cmd += setupGetCmdLength(cmd);
	}
}

static void netEntsFreeAll(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (s_Hosts[i].maxids) {
			netSnapHostFree(&s_Hosts[i]);
		}

		free(s_Seen[i]);
		s_Seen[i] = NULL;
	}

	netSnapClientFree(&s_Client);
	free(s_SetupCmdOfProp);
	free(s_SetupGenOfProp);
	free(s_SetupPropOfCmd);
	free(s_Cap);
	free(s_Ents);
	free(s_Map);
	free(s_TelePrev);
	free(s_TeleCount);
	free(s_TeleGen);
	s_SetupCmdOfProp = NULL;
	s_SetupGenOfProp = NULL;
	s_SetupPropOfCmd = NULL;
	s_Cap = NULL;
	s_Ents = NULL;
	s_Map = NULL;
	s_TelePrev = NULL;
	s_TeleCount = NULL;
	s_TeleGen = NULL;
	s_NumCmds = 0;
	s_MaxIds = 0;
}

/**
 * After lvReset on every stage (netStageStart): a match's stage gets its
 * tables, anything else none
 */
void netEntsStageStart(void)
{
	s32 i;

	netEntsFreeAll();

	if (!netSessionMatchLoading() || !g_NetPropGen || g_Vars.maxprops <= 0 || g_Vars.maxprops > NETSNAP_MAXIDS) {
		return;
	}

	s_MaxIds = g_Vars.maxprops;
	s_SetupCmdOfProp = calloc(s_MaxIds, sizeof(s32));
	s_SetupGenOfProp = calloc(s_MaxIds, sizeof(u16));
	s_Cap = calloc(s_MaxIds, sizeof(struct netcap));
	s_Ents = calloc(s_MaxIds, sizeof(struct netsnapent));
	s_TelePrev = calloc(s_MaxIds * 3, sizeof(f32));
	s_TeleCount = calloc(s_MaxIds, 1);
	s_TeleGen = calloc(s_MaxIds, sizeof(u16));

	if (!s_SetupCmdOfProp || !s_SetupGenOfProp || !s_Cap || !s_Ents || !s_TelePrev || !s_TeleCount || !s_TeleGen) {
		sysLogPrintf(LOG_ERROR, "net: out of memory for the entity table");
		netEntsFreeAll();
		return;
	}

	netEntsBuildSetupTable();

	for (i = 0; i < MAX_PLAYERS; i++) {
		s_NextSnap[i] = 0;
		s_LpRespawns[i] = 0;
		s_LpTeleports[i] = 0;
		s_LpWasDead[i] = 0;
		s_LpHavePrev[i] = 0;
		s_OfferedSum[i] = 0;

		if (g_NetMode == NETMODE_SERVER) {
			s_Seen[i] = calloc(s_MaxIds, sizeof(u32));
		}
	}

	memset(s_LocalHist, 0, sizeof(s_LocalHist));
	s_Mapped = s_Unresolved = s_StaleSeen = s_NacksSent = 0;
	s_PresentUnmapped = s_NoneNacked = s_KeptOverGap = s_Misfits = 0;
	s_LastMapSeq = 0;
	s_HostileAccepted = s_HostileDropped = 0;
	s_NackCount = 0;
	s_ErrN = 0;
	s_ErrSum = 0;
	s_ErrMax = 0;
	s_ErrOver = 0;
	s_ErrMissing = 0;
	s_ErrHuge = 0;
	s_LpSeen = 0;
	s_RespawnsSeen = 0;
	s_TeleportsSeen = 0;
	s_HostileFed = 0;
	s_HostileRejected = 0;

	sysLogPrintf(LOG_NOTE, "net: entity table: %d prop slots, %d setup commands", s_MaxIds, s_NumCmds);
}

/*
 * Host: H5, what each player's pass put on its screen
 */

void netHostNoteVisible(s32 playernum)
{
	struct prop *prop;
	u32 *seen;
	s32 guard = 0;

	if (playernum < 0 || playernum >= MAX_PLAYERS || !s_Seen[playernum]) {
		return;
	}

	seen = s_Seen[playernum];

	// only the active props were ticked by this pass; the paused ones hold
	// whatever flags they last had
	for (prop = g_Vars.activeprops; prop && prop != g_Vars.pausedprops && guard < s_MaxIds; prop = prop->next, guard++) {
		const s32 idx = netPropIndex(prop);

		if (idx >= 0 && idx < s_MaxIds && (prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
			seen[idx] = g_NetTick + 1;
		}
	}
}

/*
 * Host: capture
 */

static u8 netWeaponOf(struct prop *prop)
{
	if (prop && (prop->type == PROPTYPE_WEAPON || (prop->obj && prop->obj->type == OBJTYPE_WEAPON)) && prop->weapon) {
		return prop->weapon->weaponnum;
	}

	return 0xff;
}

static void netCaptureChr(struct netcap *c, struct prop *prop, s32 idx)
{
	struct chrdata *chr = prop->chr;
	struct model *model = chr->model;
	struct netentstate *st = &c->st;
	struct coord root;
	f32 angleoffset = 0;
	u16 flags = 0;
	s32 pn = c->playernum;

	if (chr->actiontype == ACT_DIE) {
		flags |= 1;
	} else if (chr->actiontype == ACT_DEAD) {
		flags |= 2;
	}

	if (pn >= 0 && g_Vars.players[pn] && g_Vars.players[pn]->isdead) {
		flags = (flags & ~NETCHR_LIFEMASK) | 2;
	}

	if (chr->hidden & CHRHFLAG_CLOAKED) flags |= NETCHR_CLOAKED;
	if (chrIsGunfireVisible(chr, HAND_LEFT)) flags |= NETCHR_FIRINGL;
	if (chrIsGunfireVisible(chr, HAND_RIGHT)) flags |= NETCHR_FIRINGR;
	if (chr->chrflags & CHRCFLAG_HIDDEN) flags |= NETCHR_HIDDEN;
	if (chr->chrflags & CHRCFLAG_PERIMDISABLEDTMP) flags |= NETCHR_PERIMOFF;
	if (chr->hidden2 & CHRH2FLAG_AUTOANIM) flags |= NETCHR_AUTOANIM;
	if (chr->onladder) flags |= NETCHR_LADDER;
	if (prop->flags & PROPFLAG_ENABLED) flags |= NETCHR_ENABLED;

	// a teleport counts up (wrapping), so neither a lost snapshot nor two
	// teleports between two decoded ones can hide one
	if (s_TeleGen[idx] != c->desc.gen) {
		s_TeleGen[idx] = c->desc.gen;
		s_TeleCount[idx] = 0;
	} else {
		const f32 dx = prop->pos.x - s_TelePrev[idx * 3];
		const f32 dy = prop->pos.y - s_TelePrev[idx * 3 + 1];
		const f32 dz = prop->pos.z - s_TelePrev[idx * 3 + 2];

		if (dx * dx + dy * dy + dz * dz > NETENT_TELEPORT * NETENT_TELEPORT * 9) {
			s_TeleCount[idx]++;
		}
	}

	s_TelePrev[idx * 3] = prop->pos.x;
	s_TelePrev[idx * 3 + 1] = prop->pos.y;
	s_TelePrev[idx * 3 + 2] = prop->pos.z;

	st->flags = flags;
	st->teleports = s_TeleCount[idx];
	st->pos[0] = prop->pos.x;
	st->pos[1] = prop->pos.y;
	st->pos[2] = prop->pos.z;
	st->room = prop->rooms[0];
	st->yaw = modelGetChrRotY(model);
	modelGetRootPosition(model, &root);
	st->rooty = root.y - prop->pos.y;
	st->groundy = chr->ground - prop->pos.y;

	if (model->anim) {
		struct anim *anim = model->anim;

		st->animnum = anim->animnum;
		st->animflags = (anim->flip ? NETANIM_FLIP : 0) | (anim->flip2 ? NETANIM_FLIP2 : 0)
			| (anim->looping ? NETANIM_LOOP : 0) | (anim->speed < 0 ? NETANIM_REV : 0)
			| (anim->speed2 < 0 ? NETANIM_REV2 : 0) | (anim->animnum2 ? NETANIM_HAS2 : 0);
		st->frame = anim->frame;
		st->animnum2 = anim->animnum2;
		st->frame2 = anim->frame2;
		st->fracmerge = anim->fracmerge;
	}

	if (chr->aibot) {
		angleoffset = chr->aibot->angleoffset;
	} else if (pn >= 0 && g_Vars.players[pn]) {
		angleoffset = g_Vars.players[pn]->angleoffset;
	}

	st->aim[0] = chr->aimendlshoulder;
	st->aim[1] = chr->aimendrshoulder;
	st->aim[2] = chr->aimendback;
	st->aim[3] = chr->aimendsideback + angleoffset;
	st->weapon[0] = netWeaponOf(chr->weapons_held[HAND_RIGHT]);
	st->weapon[1] = netWeaponOf(chr->weapons_held[HAND_LEFT]);
	st->hat = chr->weapons_held[2] && chr->weapons_held[2]->obj ? (u16)chr->weapons_held[2]->obj->modelnum : 0xffff;
	st->fadealpha = chr->fadealpha;
	st->cloakfrac = chr->cloakfadefrac | (chr->cloakfadefinished ? 0x80 : 0);
	st->cshield = chr->cshield;
	st->drugheadsway = chr->drugheadsway;
	st->fadeintimer = chr->aibot ? chr->aibot->fadeintimer60 : 0;
}

static void netCaptureObj(struct netcap *c, struct prop *prop)
{
	struct defaultobj *obj = prop->obj;
	struct netentstate *st = &c->st;

	st->pos[0] = prop->pos.x;
	st->pos[1] = prop->pos.y;
	st->pos[2] = prop->pos.z;
	st->room = prop->rooms[0];

	if (c->desc.rec == NETREC_DOOR) {
		struct doorobj *door = (struct doorobj *)obj;

		st->doorfrac = door->maxfrac > 0 ? door->frac / door->maxfrac : 0;
		st->doormode = door->mode;
		st->laserfade = door->laserfade;
		st->flags = (prop->flags & PROPFLAG_ENABLED) ? 1 : 0;
	} else if (c->desc.rec == NETREC_LIFT) {
		struct liftobj *lift = (struct liftobj *)obj;

		st->levelcur = lift->levelcur;
		st->levelaim = lift->levelaim;
		st->flags = lift->speed != 0 ? 1 : 0;
	} else {
		u16 flags = 0;

		if (prop->flags & PROPFLAG_ENABLED) flags |= NETOBJ_ENABLED;
		if (obj->hidden & OBJHFLAG_GONE) flags |= NETOBJ_GONE;
		if (obj->flags2 & OBJFLAG2_INVISIBLE) flags |= NETOBJ_INVISIBLE;
		if (obj->hidden & OBJHFLAG_PROJECTILE) flags |= NETOBJ_PROJECTILE;
		if (obj->hidden & OBJHFLAG_EMBEDDED) flags |= NETOBJ_EMBEDDED;

		st->flags = flags;
		netQuatFromMatrix(obj->realrot, st->quat);
		st->damage = obj->damage;
		st->extra[0] = netWeaponOf(prop) == 0xff ? 0 : netWeaponOf(prop);
	}
}

/**
 * Every prop that is an entity, into s_Cap: listed (active then paused,
 * one chain), not a child (held items are their chr's record; inventory is
 * parented to its player), and a chr, player, object, door or weapon
 */
static void netCaptureAll(void)
{
	struct prop *prop;
	s32 guard = 0;
	s32 i;

	for (i = 0; i < s_MaxIds; i++) {
		s_Cap[i].valid = 0;
	}

	prop = g_Vars.activeprops ? g_Vars.activeprops : g_Vars.pausedprops;

	for (; prop && guard < s_MaxIds; prop = prop->next, guard++) {
		const s32 idx = netPropIndex(prop);
		struct netcap *c;

		if (idx < 0 || idx >= s_MaxIds || prop->parent) {
			continue;
		}

		c = &s_Cap[idx];
		memset(c, 0, sizeof(*c));
		c->playernum = -1;
		c->desc.gen = netPropGen(idx);

		if (c->desc.gen == 0) {
			continue;
		}

		if (prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) {
			struct chrdata *chr = prop->chr;

			if (!chr || !chr->model || !chr->model->definition || !chr->model->definition->rootnode) {
				continue;
			}

			c->desc.rec = NETREC_CHR;
			c->desc.bodynum = chr->bodynum;
			c->desc.headnum = chr->headnum;

			if (prop->type == PROPTYPE_PLAYER) {
				const s32 pn = playermgrGetPlayerNumByProp(prop);

				if (pn < 0 || pn >= PLAYERCOUNT()) {
					continue;
				}

				c->playernum = (s8)pn;
				c->desc.kind = NETDESC_PLAYER;
				c->desc.key = g_Vars.playerstats[pn].mpindex & 0xff;
			} else if (chr->aibot && chr->aibot->config) {
				const intptr_t slot = chr->aibot->config - g_BotConfigsArray;

				if (slot < 0 || slot >= MAX_BOTS) {
					continue;
				}

				c->desc.kind = NETDESC_SIM;
				c->desc.key = (u16)slot;
			} else {
				c->desc.kind = NETDESC_BODY;
			}

			netCaptureChr(c, prop, idx);
		} else if (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_DOOR || prop->type == PROPTYPE_WEAPON) {
			struct defaultobj *obj = prop->obj;

			if (!obj) {
				continue;
			}

			c->desc.objtype = obj->type;
			c->desc.modelnum = obj->modelnum;

			if (prop->type == PROPTYPE_DOOR) {
				c->desc.rec = NETREC_DOOR;
			} else if (obj->type == OBJTYPE_LIFT) {
				c->desc.rec = NETREC_LIFT;
			} else {
				c->desc.rec = NETREC_OBJ;
			}

			if (s_SetupCmdOfProp[idx] >= 0 && s_SetupGenOfProp[idx] == c->desc.gen) {
				c->desc.kind = NETDESC_SETUPOBJ;
				c->desc.key = (u16)s_SetupCmdOfProp[idx];
			} else if (prop->type == PROPTYPE_WEAPON || obj->type == OBJTYPE_WEAPON) {
				c->desc.kind = NETDESC_DYNWEAPON;
				c->desc.weaponnum = prop->weapon->weaponnum;
				c->desc.gunfunc = prop->weapon->gunfunc;
			} else if (obj->type == OBJTYPE_AMMOCRATE || obj->type == OBJTYPE_MULTIAMMOCRATE) {
				c->desc.kind = NETDESC_AMMOCRATE;
			} else if (obj->type == OBJTYPE_HAT) {
				c->desc.kind = NETDESC_HAT;
			} else {
				c->desc.kind = NETDESC_DYNOBJ;
			}

			c->isproj = (obj->hidden & OBJHFLAG_PROJECTILE) != 0;
			netCaptureObj(c, prop);
		} else {
			continue;
		}

		netRecPack(c->desc.rec, &c->st, c->record);
		c->valid = 1;
	}
}

/*
 * Host: the local-player block
 */

static s32 netPlayerOfSlot(s32 slot)
{
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if ((g_Vars.playerstats[i].mpindex & 3) == slot && g_Vars.players[i]) {
			return i;
		}
	}

	return -1;
}

// Once a tick per remote slot: respawns and teleports are counted as they happen
static void netLpTrack(s32 slot, s32 pn)
{
	struct player *p = g_Vars.players[pn];

	if (!p->prop) {
		return;
	}

	if (s_LpWasDead[slot] && !p->isdead) {
		s_LpRespawns[slot]++;
		s_LpHavePrev[slot] = 0;
		s_LpTeleports[slot]++;
	}

	s_LpWasDead[slot] = p->isdead;

	if (s_LpHavePrev[slot]) {
		const f32 dx = p->prop->pos.x - s_LpPrevPos[slot].x;
		const f32 dy = p->prop->pos.y - s_LpPrevPos[slot].y;
		const f32 dz = p->prop->pos.z - s_LpPrevPos[slot].z;

		if (dx * dx + dy * dy + dz * dz > NETENT_TELEPORT * NETENT_TELEPORT) {
			s_LpTeleports[slot]++;
		}
	}

	s_LpPrevPos[slot] = p->prop->pos;
	s_LpHavePrev[slot] = 1;
}

static void netLpCapture(s32 slot, s32 pn, u8 *out)
{
	struct player *p = g_Vars.players[pn];
	struct netlpstate s;
	struct invitem *item;
	s32 i;
	s32 guard = 0;

	memset(&s, 0, sizeof(s));
	s.flags = (p->isdead ? NETLP_DEAD : 0) | (p->invincible ? NETLP_INVINCIBLE : 0);
	s.respawns = s_LpRespawns[slot];
	s.teleports = s_LpTeleports[slot];
	s.dual = p->gunctrl.dualwielding;
	s.pos[0] = p->prop->pos.x;
	s.pos[1] = p->prop->pos.y;
	s.pos[2] = p->prop->pos.z;

	for (i = 0; i < 8; i++) {
		s.rooms[i] = p->prop->rooms[i];
	}

	s.theta = p->vv_theta;
	s.verta = p->vv_verta;
	s.health = p->bondhealth;
	s.shield = p->prop->chr ? chrGetShield(p->prop->chr) : 0;
	s.weaponnum = p->gunctrl.weaponnum;
	s.loaded[0] = p->hands[HAND_RIGHT].loadedammo[0];
	s.loaded[1] = p->hands[HAND_RIGHT].loadedammo[1];
	s.loaded[2] = p->hands[HAND_LEFT].loadedammo[0];
	s.loaded[3] = p->hands[HAND_LEFT].loadedammo[1];

	for (i = 0; i < NETLP_NUMAMMO; i++) {
		const s32 a = p->ammoheldarr[i];

		s.ammo[i] = (u16)(a < 0 ? 0 : a > 0xffff ? 0xffff : a);
	}

	// the inventory as bits: unchanged, it costs nothing after the XOR
	item = p->weapons;

	while (item && guard < 512) {
		if (item->type == INVITEMTYPE_WEAP && item->type_weap.weapon1 >= 0 && item->type_weap.weapon1 < 256) {
			s.inv[item->type_weap.weapon1 >> 3] |= 1 << (item->type_weap.weapon1 & 7);
		} else if (item->type == INVITEMTYPE_DUAL && item->type_dual.weapon1 >= 0 && item->type_dual.weapon1 < 256) {
			s.invdual[item->type_dual.weapon1 >> 3] |= 1 << (item->type_dual.weapon1 & 7);
		}

		item = item->next;
		guard++;

		if (item == p->weapons) {
			break;
		}
	}

	netLpPack(&s, out);
}

/*
 * Host: one client's snapshot
 */

static f32 netDist2(const struct coord *a, const f32 *b)
{
	const f32 dx = a->x - b[0];
	const f32 dy = a->y - b[1];
	const f32 dz = a->z - b[2];

	return dx * dx + dy * dy + dz * dz;
}

static void netHostDumpSnap(s32 slot, const struct netsnaphdr *hdr, s32 n)
{
	FILE *f = netDumpFile();
	s32 i;

	if (!f || hdr->hosttick < s_DumpFrom || hdr->hosttick > s_DumpTo) {
		return;
	}

	for (i = 0; i < n; i++) {
		const struct netsnapent *e = &s_Ents[i];

		fprintf(f, "H %u %d %u %u %u %u %u", hdr->hosttick, slot, hdr->seq, e->id, e->desc.gen, e->desc.rec, e->status);
		netDumpState(f, &s_Cap[e->id].st);
	}
}

static void netHostSendSnap(s32 slot)
{
	struct netsnaphost *h = &s_Hosts[slot];
	struct netsnaphdr hdr;
	const s32 pn = netPlayerOfSlot(slot);
	struct prop *self = pn >= 0 ? g_Vars.players[pn]->prop : NULL;
	u32 *seen = pn >= 0 && pn < MAX_PLAYERS ? s_Seen[pn] : NULL;
	u8 lp[NETLP_SIZE];
	s32 n = 0;
	s32 len;
	s32 i;

	if (!h->maxids && netSnapHostInit(h, s_MaxIds) != 0) {
		return;
	}

	for (i = 0; i < s_MaxIds; i++) {
		const struct netcap *c = &s_Cap[i];
		struct netsnapent *e;
		f32 weight;
		s32 visible;
		s32 recent;
		s32 near;

		if (!c->valid || (self && &g_Vars.props[i] == self)) {
			continue;
		}

		visible = seen && seen[i] == g_NetTick + 1;
		recent = seen && seen[i] && seen[i] + NETENT_SEENTICKS >= g_NetTick + 1;
		near = self && netDist2(&self->pos, c->st.pos) < NETENT_NEAR * NETENT_NEAR;

		// scope: chrs (the radar shows them all), doors and lifts always;
		// objects when seen lately or near, projectiles from further
		switch (c->desc.rec) {
		case NETREC_CHR:
			weight = 8;
			break;
		case NETREC_DOOR:
		case NETREC_LIFT:
			weight = 3;
			break;
		default:
			if (c->isproj && self && netDist2(&self->pos, c->st.pos) < NETENT_PROJNEAR * NETENT_PROJNEAR) {
				weight = 6;
			} else if (recent || near || !self) {
				weight = 1;
			} else {
				continue;
			}
			break;
		}

		weight *= visible ? 3.f : near ? 1.5f : 1.f;

		e = &s_Ents[n++];
		e->id = (u16)i;
		e->desc = c->desc;
		memcpy(e->record, c->record, NETREC_MAX);
		e->weight = weight;
		e->status = 0;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.matchid = netSessionMatchId();
	hdr.hosttick = g_NetTick;
	hdr.lvupdate240 = (u8)(g_Vars.lvupdate240 < 0 ? 0 : g_Vars.lvupdate240 > 255 ? 255 : g_Vars.lvupdate240);
	hdr.lastcmd = (u32)netPlayersHostLastPlayed(slot);

	if (pn >= 0 && g_Vars.players[pn]->prop) {
		netLpCapture(slot, pn, lp);
	}

	len = netSnapHostBuild(h, &hdr, s_Ents, n, pn >= 0 && g_Vars.players[pn]->prop ? lp : NULL, s_Pkt, sizeof(s_Pkt));

	if (len <= 0) {
		return;
	}

	s_OfferedSum[slot] += n;
	netSessionSendSlot(slot, NET_CHAN_UNRELIABLE, s_Pkt, len, 0);
	netHostDumpSnap(slot, &hdr, n);
	s_NextSnap[slot] = g_NetTick + h->rate;
}

static void netHostLogSlot(s32 slot, const char *why)
{
	const struct netsnaphost *h = &s_Hosts[slot];

	if (!h->maxids) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "net: snap slot %d %s (tick %u): %u sent, bytes mean %u min %u max %u, keyframes %u, entity keyframes %u, records %u, descriptors %u, deferred %u, excluded %u, acks %u, nacks %u, resets %u, rate %d Hz (%u changes), entities offered mean %u",
			slot, why, g_NetTick, h->sent, h->sent ? h->bytes / h->sent : 0, h->sent ? h->bytesmin : 0, h->bytesmax,
			h->keyframes, h->entkeys, h->records, h->descs, h->deferred, h->excluded, h->acks, h->nacks, h->resets,
			60 / (h->rate ? h->rate : 2), h->ratechanges, h->sent ? s_OfferedSum[slot] / h->sent : 0);
}

/**
 * The host's tick has run (netTickEnd, before g_NetTick moves on): each
 * remote slot whose snapshot is due gets one (H4's work, from the tick's
 * end rather than a hook of its own)
 */
void netEntsHostTickEnd(void)
{
	s32 captured = 0;
	s32 slot;

	if (!s_MaxIds || !s_Cap) {
		return;
	}

	for (slot = 0; slot < MAX_PLAYERS; slot++) {
		s32 pn;

		if (!netPlayersHostSlotIsRemote(slot)) {
			if (s_Hosts[slot].maxids) {
				netHostLogSlot(slot, "at leaving");
				if (s_Hostile) sysLogPrintf(LOG_NOTE, "net: hostile: %u mangled commands fed to the parser", s_HostileFed);
				netSnapHostFree(&s_Hosts[slot]);
			}

			continue;
		}

		pn = netPlayerOfSlot(slot);

		if (pn >= 0) {
			netLpTrack(slot, pn);
		}

		if (g_NetTick < s_NextSnap[slot]) {
			continue;
		}

		if (!captured) {
			netCaptureAll();
			captured = 1;
		}

		netHostSendSnap(slot);
	}

	if (g_NetTick % 300 == 0 && g_NetTick) {
		for (slot = 0; slot < MAX_PLAYERS; slot++) {
			netHostLogSlot(slot, "so far");
		}
	}
}

void netEntsHostOnAck(s32 slot, const struct netsnapack *a)
{
	if (slot < 0 || slot >= MAX_PLAYERS || !s_Hosts[slot].maxids) {
		return;
	}

	netSnapHostOnAck(&s_Hosts[slot], a);
}

/*
 * --net-test-hostile on the host: mangled copies of each CMD into the
 * parser before the real one
 */

static u32 s_HostileRng = 0x9e3779b9;

static u32 netHostileRnd(void)
{
	s_HostileRng ^= s_HostileRng << 13;
	s_HostileRng ^= s_HostileRng >> 17;
	s_HostileRng ^= s_HostileRng << 5;
	return s_HostileRng;
}

static s32 netMangle(const u8 *in, s32 len, u8 *out, s32 cap)
{
	s32 k;
	s32 mlen = len < cap ? len : cap;

	memcpy(out, in, mlen);

	switch (netHostileRnd() % 5) {
	case 0:
		mlen = mlen > 1 ? 1 + netHostileRnd() % (mlen - 1) : mlen;
		break;
	case 1:
		if (mlen > 1) out[1 + netHostileRnd() % (mlen - 1)] ^= (u8)(1 << (netHostileRnd() % 8));
		break;
	case 2:
		for (k = 0; k < 6 && mlen > 1; k++) out[1 + netHostileRnd() % (mlen - 1)] = (u8)netHostileRnd();
		break;
	case 3:
		mlen = 1 + netHostileRnd() % cap;
		for (k = 1; k < mlen; k++) out[k] = (u8)netHostileRnd();
		break;
	default:
		// a count or length field pushed to its limit
		if (mlen > 10) out[9 + netHostileRnd() % 4] = 0xff;
		break;
	}

	return mlen;
}

s32 netEntsHostile(void)
{
	return s_Hostile;
}

void netEntsHostileCmd(s32 slot, const u8 *data, s32 len)
{
	static u8 buf[NET_MAXUNRELIABLE];
	s32 k;

	if (!s_Hostile || len < 1) {
		return;
	}

	for (k = 0; k < 3; k++) {
		struct netbuf b;
		const s32 mlen = netMangle(data, len, buf, sizeof(buf));

		netBufInitRead(&b, buf + 1, mlen - 1);
		netPlayersHostOnCmd(slot, &b);
		s_HostileFed++;
	}
}

/*
 * Client
 */

static s32 netResolveFits(const struct netdesc *d, struct prop *local)
{
	struct defaultobj *obj;

	if (d->rec == NETREC_CHR) {
		return (local->type == PROPTYPE_CHR || local->type == PROPTYPE_PLAYER) && local->chr;
	}

	if (local->type != PROPTYPE_OBJ && local->type != PROPTYPE_DOOR && local->type != PROPTYPE_WEAPON) {
		return 0;
	}

	obj = local->obj;

	if (!obj || (d->kind == NETDESC_SETUPOBJ && obj->type != d->objtype)) {
		return 0;
	}

	switch (d->rec) {
	case NETREC_DOOR:
		return local->type == PROPTYPE_DOOR;
	case NETREC_LIFT:
		return local->type == PROPTYPE_OBJ && obj->type == OBJTYPE_LIFT;
	case NETREC_OBJ:
		return local->type != PROPTYPE_DOOR && obj->type != OBJTYPE_LIFT;
	default:
		return 0;
	}
}

static s32 netResolve(const struct netdesc *d, struct prop **out)
{
	s32 i;

	*out = NULL;

	switch (d->kind) {
	case NETDESC_SETUPOBJ:
		if (d->key < s_NumCmds && s_SetupPropOfCmd && s_SetupPropOfCmd[d->key] >= 0) {
			const s32 idx = s_SetupPropOfCmd[d->key];

			// still the setup object it was at the stage's start
			if (netPropGen(idx) == s_SetupGenOfProp[idx]) {
				*out = &g_Vars.props[idx];
			}
		}
		break;
	case NETDESC_SIM:
		for (i = 0; i < g_BotCount && i < MAX_BOTS; i++) {
			struct chrdata *chr = g_MpBotChrPtrs[i];

			if (chr && chr->aibot && chr->aibot->config - g_BotConfigsArray == d->key && chr->prop) {
				*out = chr->prop;
				break;
			}
		}
		break;
	case NETDESC_PLAYER:
		for (i = 0; i < PLAYERCOUNT(); i++) {
			if ((g_Vars.playerstats[i].mpindex & 0xff) == d->key && g_Vars.players[i]) {
				*out = g_Vars.players[i]->prop;
				break;
			}
		}
		break;
	default:
		// dynamic props, bodies, hats: created by 4b's puppets
		break;
	}

	// 4b writes the record into what is mapped here: it must be the kind of
	// prop the record is for (a door record into a basic object would write
	// past it), and a setup object the type the host's is
	if (*out && !netResolveFits(d, *out)) {
		s_Misfits++;

		if (s_Misfits <= 8) {
			sysLogPrintf(LOG_WARNING, "net: descriptor kind %d rec %d key %d objtype %d does not fit local prop %d (type %d)",
					d->kind, d->rec, d->key, d->objtype, netPropIndex(*out), (*out)->type);
		}

		*out = NULL;
		return -1;
	}

	return *out != NULL;
}

static void netNackQueue(u16 id)
{
	if (s_Map[id].nackqueued || s_NackCount >= (s32)ARRAYCOUNT(s_NackRing)) {
		return;
	}

	s_Map[id].nackqueued = 1;
	s_NackRing[s_NackCount++] = id;
}

static void netClientMap(u16 seq)
{
	const struct netbaselineslot *slot = &s_Client.bl.slots[seq % NETBASELINE_SLOTS];
	s32 i;

	if (!s_Map) {
		s_Map = calloc(s_Client.maxids, sizeof(*s_Map));

		if (!s_Map) {
			return;
		}
	}

	// descriptors: map (again)
	for (i = 0; i < s_Client.ndescs; i++) {
		const struct netsnapdescin *in = &s_Client.descs[i];
		struct netmap *m = &s_Map[in->id];
		struct prop *local;

		m->hostgen = in->desc.gen;
		m->nackqueued = 0;

		if (netResolve(&in->desc, &local) > 0) {
			const s32 lidx = netPropIndex(local);

			m->local = local;
			m->localgen = netPropGen(lidx);
			m->state = NETMAP_MAPPED;
		} else {
			m->local = NULL;
			m->state = NETMAP_UNRESOLVED;
		}
	}

	// every present one: is the local prop still the one mapped?
	s_Mapped = 0;
	s_Unresolved = 0;
	s_PresentUnmapped = 0;

	for (i = 0; i < slot->count; i++) {
		const u16 id = slot->ids[i];
		const u8 *store = slot->records + (size_t)i * NETSNAP_STORE;
		struct netmap *m = &s_Map[id];

		if (m->seen && m->seen != s_LastMapSeq && m->state != NETMAP_NONE && m->hostgen == netStoreGen(store)) {
			// back after snapshots without it, against a baseline that still
			// had it (so no descriptor): the mapping kept is still good
			s_KeptOverGap++;
		}

		m->seen = seq;

		if (m->state != NETMAP_NONE && m->hostgen != netStoreGen(store)) {
			// a new entity in that slot whose descriptor this snapshot carried
			// was handled above; anything else is a mapping from before
			m->state = NETMAP_NONE;
			m->local = NULL;
		}

		if (m->state == NETMAP_MAPPED) {
			if (netPropGen(netPropIndex(m->local)) != m->localgen) {
				// the local prop was freed or reused (the client's own
				// weaponCreate can recycle one): ask for its descriptor
				m->state = NETMAP_STALE;
				m->local = NULL;
				s_StaleSeen++;
				netNackQueue(id);
			} else {
				s_Mapped++;
			}
		} else if (m->state == NETMAP_UNRESOLVED) {
			s_Unresolved++;
		} else if (m->state == NETMAP_STALE) {
			s_PresentUnmapped++;
			netNackQueue(id);
		} else {
			// present with no mapping and no descriptor (nothing should
			// lead here now, but a lost mapping must not stay lost): ask
			s_PresentUnmapped++;
			s_NoneNacked++;
			netNackQueue(id);
		}
	}

	// An entity gone from this snapshot keeps its mapping: the host sends a
	// descriptor only for ids its (older) acked baseline lacks, so one back
	// after a gap comes without one. A new generation in the slot always
	// brings a descriptor and replaces the entry. 4b applies records only
	// to ids present in the snapshot it plays.
	s_LastMapSeq = seq;
}

static void netClientDumpSnap(const struct netsnaphdr *hdr)
{
	const struct netbaselineslot *slot = &s_Client.bl.slots[hdr->seq % NETBASELINE_SLOTS];
	FILE *f = netDumpFile();
	s32 i;

	if (!f || hdr->hosttick < s_DumpFrom || hdr->hosttick > s_DumpTo) {
		return;
	}

	for (i = 0; i < slot->count; i++) {
		const u8 *store = slot->records + (size_t)i * NETSNAP_STORE;
		struct netentstate st;

		netRecUnpack(netStoreRec(store), store + NETSNAP_STOREHDR, &st);
		fprintf(f, "C %u %u %u %u %d", hdr->hosttick, hdr->seq, slot->ids[i], netStoreGen(store), netStoreRec(store));
		netDumpState(f, &st);
	}
}

/**
 * Phase 5 will reconcile; phase 4 measures. The host's position for this
 * player after the last command it played, against where this machine's
 * own run had the player after the same command.
 */
static void netClientMeasureLocal(const struct netsnaphdr *hdr)
{
	const struct netsnapinfo *info = netSnapClientInfo(&s_Client, hdr->seq);
	const struct netlocalhist *h;
	struct netlpstate lp;
	f32 dx;
	f32 dy;
	f32 dz;
	f32 err;

	if (!info || !info->haslp) {
		return;
	}

	netLpUnpack(info->lp, &lp);

	if (s_LpSeen) {
		s_RespawnsSeen += (u8)(lp.respawns - s_LastRespawns);
		s_TeleportsSeen += (u8)(lp.teleports - s_LastTeleports);
	}

	s_LastRespawns = lp.respawns;
	s_LastTeleports = lp.teleports;
	s_LpSeen = 1;

	if (hdr->lastcmd == 0xffffffff) {
		return;
	}

	h = &s_LocalHist[hdr->lastcmd % NET_LOCALHIST];

	if (!h->valid || h->tick != hdr->lastcmd) {
		s_ErrMissing++;
		return;
	}

	dx = lp.pos[0] - h->pos[0];
	dy = lp.pos[1] - h->pos[1];
	dz = lp.pos[2] - h->pos[2];
	err = sqrtf(dx * dx + dy * dy + dz * dz);

	s_ErrN++;
	s_ErrSum += err;

	if (err > s_ErrMax) {
		s_ErrMax = err;
	}

	if (err > NET_HARDCORRECT) {
		s_ErrOver++;
	}

	if (err > 100000.f && s_ErrHuge++ < 4) {
		sysLogPrintf(LOG_WARNING, "net: local error %.0f at command %u: host %.1f %.1f %.1f, here %.1f %.1f %.1f",
				err, hdr->lastcmd, lp.pos[0], lp.pos[1], lp.pos[2], h->pos[0], h->pos[1], h->pos[2]);
	}
}

static void netClientLog(const char *why)
{
	const struct netsnapclient *c = &s_Client;

	sysLogPrintf(LOG_NOTE, "net: snap client %s (tick %u): %u received, %u decoded, %u old, %u other match, %u no baseline, %u malformed, %u keyframes, %u entity keyframes, bytes mean %u; mapped %u, unresolved %u, present unmapped %u, stale seen %u, nacks sent %u, unmapped nacked %u, kept over gaps %u, misfits %u, resyncs %u; local error at the played command: n %u mean %.2f max %.2f, over %.0f: %u, unmatched %u; respawns %u, teleports %u; hostile fed %u rejected %u accepted %u dropped %u",
			why, g_NetTick, c->received, c->decoded, c->old, c->otherMatch, c->nobase, c->malformed, c->keyframes, c->entkeys,
			c->received ? c->bytes / c->received : 0, s_Mapped, s_Unresolved, s_PresentUnmapped, s_StaleSeen, s_NacksSent, s_NoneNacked, s_KeptOverGap, s_Misfits, c->resyncs,
			s_ErrN, s_ErrN ? (f32)(s_ErrSum / s_ErrN) : 0.f, s_ErrMax, NET_HARDCORRECT, s_ErrOver, s_ErrMissing,
			s_RespawnsSeen, s_TeleportsSeen, s_HostileFed, s_HostileRejected, s_HostileAccepted, s_HostileDropped);
}

static void netClientHostileSnap(const u8 *data, s32 len)
{
	static u8 buf[NET_MAXUNRELIABLE + 64];
	struct netsnapclient save;
	struct netsnaphdr hdr;
	struct netbuf b;
	s32 k;

	// Mangled copies go through the client's own decoder, against its real
	// baselines, in probe mode: parsed and checked to the end, nothing
	// stored, and its state and counts put back after each (so one that
	// passes cannot poison the honest stream). Before the first snapshot
	// there is nothing to probe against.
	if (!s_Client.maxids) {
		return;
	}

	for (k = 0; k < 4; k++) {
		const s32 mlen = netMangle(data, len, buf, sizeof(buf));
		s32 r;

		save = s_Client;
		s_Client.probe = 1;
		netBufInitRead(&b, buf + 1, mlen - 1);
		r = netSnapClientDecode(&s_Client, &b, netSessionMatchId(), &hdr);
		s_Client = save;
		s_HostileFed++;

		if (r < 0) {
			s_HostileRejected++;
		} else if (r > 0) {
			s_HostileAccepted++;
		} else {
			s_HostileDropped++;
		}
	}
}

/**
 * A SNAP from the host (netsession.c); data starts at its type byte
 */
void netEntsClientOnSnap(const u8 *data, s32 len)
{
	struct netsnaphdr hdr;
	struct netbuf b;
	s32 r;

	if (!s_MaxIds || len < 1 || !netSessionMatchLoading()) {
		return;
	}

	if (s_Hostile) {
		netClientHostileSnap(data, len);
	}

	netBufInitRead(&b, data + 1, len - 1);
	r = netSnapClientDecode(&s_Client, &b, netSessionMatchId(), &hdr);

	if (r < 0) {
		sysLogPrintf(LOG_WARNING, "net: a SNAP that did not decode (%d bytes)", len);
		return;
	}

	if (r == 0) {
		return;
	}

	netClientMap(hdr.seq);
	netClientDumpSnap(&hdr);
	netClientMeasureLocal(&hdr);
}

// The ack block of the next CMD (netplayers.c)
void netEntsClientWriteAck(struct netbuf *b)
{
	struct netsnapack a;
	s32 i;

	netSnapClientAck(&s_Client, &a);

	// stale mappings: their descriptors again, a few per command
	for (i = 0; i < s_NackCount && a.nnack < NETSNAP_MAXNACK; i++) {
		a.nack[a.nnack++] = s_NackRing[i];
	}

	if (a.nnack) {
		s_NacksSent += a.nnack;
		// sent once; queued again by the next snapshot if still stale
		for (i = 0; i < a.nnack; i++) {
			if (s_Map && a.nack[i] < s_Client.maxids) {
				s_Map[a.nack[i]].nackqueued = 0;
			}
		}

		memmove(s_NackRing, s_NackRing + a.nnack, (s_NackCount - a.nnack) * sizeof(u16));
		s_NackCount -= a.nnack;
	}

	netSnapAckWrite(b, &a);
}

// The client's tick has run (netTickEnd): where its own player is now
void netEntsClientTickEnd(void)
{
	struct player *p;
	struct netlocalhist *h;

	if (!s_MaxIds || g_NetLocalSlot < 0 || g_NetLocalSlot >= PLAYERCOUNT()) {
		return;
	}

	p = g_Vars.players[g_NetLocalSlot];

	if (!p || !p->prop) {
		return;
	}

	h = &s_LocalHist[g_NetTick % NET_LOCALHIST];
	h->tick = g_NetTick;
	h->pos[0] = p->prop->pos.x;
	h->pos[1] = p->prop->pos.y;
	h->pos[2] = p->prop->pos.z;
	h->valid = 1;

	// --net-test-stale: as if this machine's own sim had freed and reused a
	// mapped sim's prop slot; the next snapshot must find the mapping stale,
	// ask for the descriptor and map it again
	if (s_TestStaleTick && g_NetTick == s_TestStaleTick && s_Map) {
		s32 i;

		for (i = 0; i < s_Client.maxids; i++) {
			if (s_Map[i].state == NETMAP_MAPPED && s_Map[i].local && s_Map[i].local->type == PROPTYPE_CHR) {
				sysLogPrintf(LOG_NOTE, "net: stale test: host id %d's local prop %d gets a new generation", i, netPropIndex(s_Map[i].local));
				netPropGenBump(s_Map[i].local);
				netPropGenBump(s_Map[i].local);
				break;
			}
		}
	}

	if (g_NetTick % 300 == 0 && g_NetTick && s_Client.maxids) {
		netClientLog("so far");
	}
}

/**
 * H12: the match's stage stopped
 */
void netEntsMatchStopped(void)
{
	s32 i;

	if (g_NetMode == NETMODE_SERVER) {
		for (i = 0; i < MAX_PLAYERS; i++) {
			netHostLogSlot(i, "at the match's end");
		}
	} else if (g_NetMode == NETMODE_CLIENT && s_Client.maxids) {
		netClientLog("at the match's end");
	}

	if (s_DumpFile) {
		fclose(s_DumpFile);
		s_DumpFile = NULL;
	}

	netEntsFreeAll();
}
