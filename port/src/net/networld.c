#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "lib/model.h"
#include "game/nbomb.h"
#include "game/propobj.h"
#include "game/setuputils.h"
#include "game/explosions.h"
#include "game/wallhit.h"
#include "net/nettransport.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "netint.h"

/**
 * The world block (protocol 29): the world's latest-value-wins state that no
 * entity record carries, as EverQuest Online Adventures' DRDP keeps a state
 * channel - XOR-delta'd against what the client acked (the SNAP's baseline),
 * never resent, the newest wins. What had gone as reliable events and was
 * really state:
 *
 *   - the stage's glass broken (a bit per setup glass): a mission's broken
 *     glass is freed on the host, so no entity record could say so, and a
 *     joiner never heard of the panes broken before it came; a match's
 *     glass regenerates, and the bit follows it;
 *   - the N-bomb storms going (the last four, with the tick each began);
 *   - the gas let go (gasReleaseFromPos: its fog stays for the match).
 *
 * The client applies the newest block its render clock has reached, behind
 * no event: a lost EVENTS packet no longer holds up a pane, and a joiner's
 * first block is the world as it stands. A change it sees as it happens is
 * played as the host played it (shards, the storm's sound); one from before
 * it came is put in quietly (no shards for every pane broken before a join).
 *
 * The stage's setup is the same on every machine (the content hashes say
 * so), so its glass and windowed doors are numbered by their order among
 * the setup commands on each machine alike.
 *
 * Also here, for the test harness: --net-test-wreck breaks the glass and
 * windowed doors, crumples objects, starts a storm and the gas at host
 * ticks; --net-test-census counts them on every machine, so a late joiner's
 * world can be held against the host's (tools/ci/netcrowd.sh).
 *
 * Layout (NETWORLD_SIZE 320, netproto.h SNAP):
 *     0  u16 nglass         setup glass on the stage (the bits cover the first NETWORLD_MAXGLASS)
 *     2  u8 gas             1 once let go
 *     3  u8 0
 *     4  u32 gastick        the host tick it was let go
 *     8  f32 gaspos[3]
 *    20  storms[4]          u8 serial (1-255, 0 none), u8 0[3], u32 tick, f32 pos[3]
 *   100  u8 glass[220]      a bit per setup glass: broken now
 */

extern struct nbomb g_Nbombs[6];
extern struct fogenvironment *g_EnvTransitionFrom;
extern struct fogenvironment *g_EnvTransitionTo;
extern f32 g_GasReleaseTimerMax240;

#define NETWORLD_MAXLIST 4096
#define NETWORLD_MAXWRECK 4

static struct defaultobj **s_Glass = NULL;  // setup glass and tinted glass, in command order
static s32 s_NumGlass = 0;
static struct doorobj **s_Doors = NULL;     // setup doors with a windowed door's skeleton
static s32 s_NumDoors = 0;
static struct defaultobj **s_Basic = NULL;  // setup basic objects (the wreck's deform targets)
static s32 s_NumBasic = 0;
static struct defaultobj **s_Cams = NULL;   // setup cameras (the census's lenses)
static s32 s_NumCams = 0;

static u32 s_Deforms = 0;    // objDeform calls the host made / a client applied
static u32 s_WreckAt[NETWORLD_MAXWRECK];
static s32 s_NumWreck = 0;
static u32 s_CensusAt[NETWORLD_MAXWRECK];
static s32 s_NumCensus = 0;

#define NW_NGLASS    0
#define NW_GAS       2
#define NW_GASTICK   4
#define NW_GASPOS    8
#define NW_STORMS    20
#define NW_STORMSIZE 20
#define NW_NUMSTORMS 4
#define NW_GLASS     100
#define NETWORLD_MAXGLASS ((NETWORLD_SIZE - NW_GLASS) * 8)
#define NW_STORMLIFE 420  // ticks a storm is worth starting late (nbombTick's ~350, and some)
#define NW_LATE      30   // ticks past its own after which a change is put in as it stands now
#define NW_RING      16

typedef char nw_fits[(NW_STORMS + NW_NUMSTORMS * NW_STORMSIZE <= NW_GLASS) ? 1 : -1];

// host
static u8 s_HostBlock[NETWORLD_SIZE];
static s32 s_HostHave = 0;
static u8 s_StormSerial = 0;

// client
struct nwblock {
	u32 tick;
	u8 b[NETWORLD_SIZE];
};

static struct nwblock *s_Ring = NULL; // [NW_RING], oldest first
static s32 s_RingN = 0;
static u8 s_Applied[NETWORLD_SIZE];   // the last block applied
static s32 s_HaveApplied = 0;
static u32 s_ClGlassFresh = 0;   // panes broken as they broke (shards)
static u32 s_ClGlassQuiet = 0;   // ... put in as they stand (before this machine came, out of scope)
static u32 s_ClGlassBack = 0;    // a match's panes regenerated on the host
static u32 s_ClStorms = 0;
static u32 s_ClStormsLate = 0;   // started part way through
static u32 s_ClGas = 0;
static u32 s_ClBlocks = 0;       // blocks applied that changed something
static f64 s_ClLagSum = 0;       // render ticks past the block's tick they were applied at
static f64 s_ClLagMax = 0;
static u32 s_ClDoorGlass = 0;
static u32 s_ClDeforms = 0;      // objDeform calls made from records
static u32 s_ClUndeforms = 0;    // ... and objects made whole again
static u32 s_ClLenses = 0;       // camera lenses shot out from records

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static u32 get32(const u8 *p) { return get16(p) | ((u32)get16(p + 2) << 16); }

static void putf(u8 *p, f32 f)
{
	u32 v;

	memcpy(&v, &f, 4);
	put32(p, v);
}

static f32 getf(const u8 *p)
{
	u32 v = get32(p);
	f32 f;

	memcpy(&f, &v, 4);

	// off the wire: finite and inside the world, or 0
	return f == f && f < 1048576.f && f > -1048576.f ? f : 0.f;
}

static void netWorldMarkCensus(s32 *count, f64 *sum);

static void netWorldFreeLists(void)
{
	free(s_Glass);
	free(s_Doors);
	free(s_Basic);
	free(s_Cams);
	s_Glass = NULL;
	s_Doors = NULL;
	s_Basic = NULL;
	s_Cams = NULL;
	s_NumGlass = s_NumDoors = s_NumBasic = s_NumCams = 0;
}

static s32 netWorldDoorIsWindowed(struct doorobj *door)
{
	struct model *model = door->base.model;

	return model && model->definition && model->definition->skel == &g_SkelWindowedDoor
		&& modelGetPart(model->definition, MODELPART_WINDOWEDDOOR_0001) != NULL;
}

static s32 netWorldDoorGlassGone(struct doorobj *door)
{
	struct model *model = door->base.model;
	union modelrwdata *rw = modelGetNodeRwData(model, modelGetPart(model->definition, MODELPART_WINDOWEDDOOR_0001));

	return rw && !rw->toggle.visible;
}

// (protocol 29) a windowed door's pane shot out: the DOOR record's NETDOOR_GLASSGONE
s32 netWorldDoorGlassOut(struct doorobj *door)
{
	return door && netWorldDoorIsWindowed(door) && netWorldDoorGlassGone(door);
}

/**
 * Client: the host's door has lost its pane. As the host broke it (shards)
 * when this machine saw the door whole before, else put in as it stands; a
 * door whose portal was shut takes a second call (doorDestroyGlass forces
 * the portal open on the first, as the stock third and fourth shots do)
 */
void netWorldClientDoorGlass(struct doorobj *door, s32 fresh)
{
	struct model *model;
	union modelrwdata *rw;

	if (!door || !door->base.prop || !netWorldDoorIsWindowed(door) || netWorldDoorGlassGone(door)) {
		return;
	}

	door->glasshits = 3;

	if (fresh) {
		doorDestroyGlass(door);

		if (!netWorldDoorGlassGone(door)) {
			doorDestroyGlass(door);
		}
	} else {
		model = door->base.model;

		if (door->portalnum >= 0) {
			g_BgPortals[door->portalnum].flags |= PORTALFLAG_FORCEOPEN;
		}

		wallhitsFreeByProp(door->base.prop, 1);
		rw = modelGetNodeRwData(model, modelGetPart(model->definition, MODELPART_WINDOWEDDOOR_0001));

		if (rw) {
			rw->toggle.visible = false;
		}
	}

	s_ClDoorGlass++;
}

// A pane broken: freed for good (a mission), being freed, or in a match
// waiting to regenerate (propExecuteTickOperation: OBJHFLAG_GONE with
// timetoregen, OBJH2FLAG_DESTROYED cleared again)
static s32 netWorldGlassGone(struct defaultobj *obj)
{
	return (obj->hidden2 & OBJH2FLAG_DESTROYED) || (obj->hidden & (OBJHFLAG_DELETING | OBJHFLAG_GONE)) || obj->prop == NULL
		|| obj->prop->obj != obj;
}

// Whether any of the object's display lists draws vertices of its own (objDeform's copies)
s32 netWorldObjDeformed(struct defaultobj *obj)
{
	struct model *model = obj->model;
	struct modelnode *node;
	s32 guard = 0;

	if (!obj->prop || obj->prop->obj != obj || !model || !model->definition || !model->rwdatas) {
		return 0;
	}

	node = model->definition->rootnode;

	while (node && guard++ < 4096) {
		if ((node->type & 0xff) == MODELNODETYPE_DL && node->rodata) {
			struct modelrodata_dl *rodata = &node->rodata->dl;
			struct modelrwdata_dl *rwdata = (struct modelrwdata_dl *)&model->rwdatas[rodata->rwdataindex];

			if (rwdata->vertices && rwdata->vertices != rodata->vertices) {
				return 1;
			}
		}

		if (node->child) {
			node = node->child;
		} else {
			while (node) {
				if (node->next) {
					node = node->next;
					break;
				}

				node = node->parent;
			}
		}
	}

	return 0;
}

void netWorldStageStart(void)
{
	u32 *cmd = g_StageSetup.props;
	s32 n = 0;
	const char *arg;

	netWorldFreeLists();
	s_Deforms = 0;
	s_NumWreck = 0;
	s_NumCensus = 0;
	memset(s_HostBlock, 0, sizeof(s_HostBlock));
	s_HostHave = 0;
	s_StormSerial = 0;
	s_RingN = 0;
	memset(s_Applied, 0, sizeof(s_Applied));
	s_HaveApplied = 0;
	s_ClGlassFresh = s_ClGlassQuiet = s_ClGlassBack = s_ClStorms = s_ClStormsLate = s_ClGas = s_ClBlocks = 0;
	s_ClLagSum = s_ClLagMax = 0;
	s_ClDoorGlass = s_ClDeforms = s_ClUndeforms = s_ClLenses = 0;

	if (g_NetMode == NETMODE_NONE || !cmd) {
		return;
	}

	s_Glass = calloc(NETWORLD_MAXLIST, sizeof(*s_Glass));
	s_Doors = calloc(NETWORLD_MAXLIST, sizeof(*s_Doors));
	s_Basic = calloc(NETWORLD_MAXLIST, sizeof(*s_Basic));
	s_Cams = calloc(NETWORLD_MAXLIST, sizeof(*s_Cams));

	if (!s_Glass || !s_Doors || !s_Basic || !s_Cams) {
		netWorldFreeLists();
		return;
	}

	while ((u8)PD_BE32(cmd[0]) != OBJTYPE_END && n < 0xffff) {
		const u8 type = (u8)PD_BE32(cmd[0]);
		struct defaultobj *obj = (struct defaultobj *)cmd;

		if ((type == OBJTYPE_GLASS || type == OBJTYPE_TINTEDGLASS) && s_NumGlass < NETWORLD_MAXLIST) {
			s_Glass[s_NumGlass++] = obj;
		} else if (type == OBJTYPE_DOOR && s_NumDoors < NETWORLD_MAXLIST) {
			s_Doors[s_NumDoors++] = (struct doorobj *)obj;
		} else if ((type == OBJTYPE_BASIC || type == OBJTYPE_ALARM || type == OBJTYPE_CCTV || type == OBJTYPE_SINGLEMONITOR
					|| type == OBJTYPE_MULTIMONITOR || type == OBJTYPE_HANGINGMONITORS || type == OBJTYPE_GASBOTTLE
					|| type == OBJTYPE_SAFE || type == OBJTYPE_DEBRIS || type == OBJTYPE_AMMOCRATE
					|| type == OBJTYPE_MULTIAMMOCRATE) && s_NumBasic < NETWORLD_MAXLIST) {
			s_Basic[s_NumBasic++] = obj;
		}

		if (type == OBJTYPE_CCTV && s_NumCams < NETWORLD_MAXLIST) {
			s_Cams[s_NumCams++] = obj;
		}

		cmd += setupGetCmdLength(cmd);
		n++;
	}

	// --net-test-wreck T1[,T2..]: at each host tick, half the glass and
	// windowed doors broken (even, then odd), six objects crumpled, an
	// N-bomb storm; the first also lets the gas go
	arg = sysArgGetString("--net-test-wreck");

	while (arg && *arg && s_NumWreck < NETWORLD_MAXWRECK) {
		s_WreckAt[s_NumWreck++] = (u32)strtoul(arg, NULL, 0);
		arg = strchr(arg, ',');
		arg = arg ? arg + 1 : NULL;
	}

	// --net-test-census T1[,T2..]: both machines count the world at their tick T
	arg = sysArgGetString("--net-test-census");

	while (arg && *arg && s_NumCensus < NETWORLD_MAXWRECK) {
		s_CensusAt[s_NumCensus++] = (u32)strtoul(arg, NULL, 0);
		arg = strchr(arg, ',');
		arg = arg ? arg + 1 : NULL;
	}
}

void netWorldMatchStopped(void)
{
	if (g_NetMode == NETMODE_CLIENT && s_HaveApplied) {
		netWorldLog("at the match's end");
	}

	netWorldFreeLists();
	free(s_Ring);
	s_Ring = NULL;
	s_RingN = 0;
	s_HostHave = 0;
}

void netWorldNoteDeform(void)
{
	s_Deforms++;
}

static union modelrwdata *netWorldLensToggle(struct defaultobj *obj);

static void netWorldWreck(s32 phase)
{
	s32 broke = 0;
	s32 doors = 0;
	s32 deformed = 0;
	s32 lenses = 0;
	s32 i;
	struct coord at = {0, 0, 0};
	s32 haveat = 0;
	RoomNum *atrooms = NULL;

	for (i = phase & 1; i < s_NumGlass; i += 2) {
		struct defaultobj *obj = s_Glass[i];

		if (!netWorldGlassGone(obj) && obj->model && objFindBboxRodata(obj)) {
			if (!haveat) {
				at = obj->prop->pos;
				atrooms = obj->prop->rooms;
				haveat = 1;
			}

			glassDestroy(obj);
			broke++;
		}
	}

	for (i = phase & 1; i < s_NumDoors; i += 2) {
		struct doorobj *door = s_Doors[i];

		if (door->base.prop && netWorldDoorIsWindowed(door) && !netWorldDoorGlassGone(door)) {
			door->glasshits = 3;
			doorDestroyGlass(door);

			if (!netWorldDoorGlassGone(door)) {
				doorDestroyGlass(door); // its portal was shut: the first only forced it open
			}

			doors++;
		}
	}

	for (i = 0; i < s_NumBasic && deformed < 6; i++) {
		struct defaultobj *obj = s_Basic[(i + phase * 6) % s_NumBasic];

		if (obj->prop && obj->prop->obj == obj && obj->model && (obj->prop->flags & PROPFLAG_ENABLED)) {
			objDeform(obj, 1 + phase);
			deformed++;

			if (!haveat) {
				at = obj->prop->pos;
				atrooms = obj->prop->rooms;
				haveat = 1;
			}
		}
	}

	// every other camera's lens shot out (even, then odd)
	for (i = phase & 1; i < s_NumCams; i += 2) {
		if (s_Cams[i]->prop && s_Cams[i]->prop->obj == s_Cams[i] && netWorldLensToggle(s_Cams[i]) && !netWorldLensOut(s_Cams[i])) {
			cctvHandleLensShot(s_Cams[i]);
			lenses++;
		}
	}

	// the third: a gas barrel's long burn (178 ticks), for a joiner to see still burning
	if (haveat && atrooms && phase == 2) {
		explosionCreateSimple(NULL, &at, atrooms, EXPLOSIONTYPE_GASBARREL, 0);
	}

	if (haveat) {
		nbombCreateStorm(&at, NULL);

		// only where the stage has the gas's second sky (gasTick reads it)
		if (phase == 0 && g_EnvTransitionFrom && g_EnvTransitionTo) {
			gasReleaseFromPos(&at);
		}
	}

	sysLogPrintf(LOG_NOTE, "net: world wreck %d at tick %u: glass %d broken, windowed doors %d, objects crumpled %d, lenses %d, storm %d, gas %d",
			phase, g_NetTick, broke, doors, deformed, lenses, haveat, haveat && phase == 0 && g_EnvTransitionFrom && g_EnvTransitionTo);
}

void netWorldCensus(const char *why)
{
	s32 glass = 0;
	s32 windowed = 0;
	s32 doorgone = 0;
	s32 storms = 0;
	s32 deformed = 0;
	s32 lenses = 0;
	s32 cams = 0;
	s32 i;
	char map[NETWORLD_MAXLIST / 8 + 1];
	s32 m = 0;

	for (i = 0; i < s_NumGlass; i++) {
		glass += netWorldGlassGone(s_Glass[i]);

		if (m < (s32)sizeof(map) - 1 && i < 64) {
			map[m++] = netWorldGlassGone(s_Glass[i]) ? 'x' : (s_Glass[i]->prop ? 'o' : '-');
		}
	}

	map[m] = '\0';

	for (i = 0; i < s_NumDoors; i++) {
		if (netWorldDoorIsWindowed(s_Doors[i])) {
			windowed++;
			doorgone += netWorldDoorGlassGone(s_Doors[i]);
		}
	}

	for (i = 0; i < 6; i++) {
		storms += g_Nbombs[i].age240 >= 0;
	}

	for (i = 0; i < s_NumBasic; i++) {
		deformed += netWorldObjDeformed(s_Basic[i]);
	}

	for (i = 0; i < s_NumCams; i++) {
		lenses += netWorldLensOut(s_Cams[i]);
		cams += netWorldLensToggle(s_Cams[i]) != NULL;
	}

	sysLogPrintf(LOG_NOTE, "net: world census %s %s tick %u: glass %s", g_NetMode == NETMODE_SERVER ? "host" : "client", why, g_NetTick, map);
	{
		s32 marks;
		f64 sum;

		netWorldMarkCensus(&marks, &sum);
		sysLogPrintf(LOG_NOTE, "net: world census %s %s tick %u: marks on the walls and floors %d (places summed %.0f)",
				g_NetMode == NETMODE_SERVER ? "host" : "client", why, g_NetTick, marks, sum);
	}
	sysLogPrintf(LOG_NOTE, "net: world census %s %s tick %u: glass broken %d/%d, door glass broken %d/%d, objects deformed %d/%d, lenses out %d/%d, deforms %u, storms %d, gas %d (timer %.0f)",
			g_NetMode == NETMODE_SERVER ? "host" : "client", why, g_NetTick, glass, s_NumGlass, doorgone, windowed,
			deformed, s_NumBasic, lenses, cams, s_Deforms, storms, g_GasReleaseTimer240 > 0, (f64)g_GasReleaseTimer240);
}

// netTickBegin: the host's test wrecks, before the tick runs
void netWorldTickBegin(void)
{
	s32 i;

	if (g_NetMode != NETMODE_SERVER || !netSessionMatchActive() || netSessionBarrierHeld()) {
		return;
	}

	for (i = 0; i < s_NumWreck; i++) {
		if (s_WreckAt[i] && g_NetTick == s_WreckAt[i]) {
			netWorldWreck(i);
		}
	}
}

// netTickEnd: the census, on both machines
void netWorldTickEnd(void)
{
	s32 i;

	for (i = 0; i < s_NumCensus; i++) {
		if (s_CensusAt[i] && g_NetTick == s_CensusAt[i] && netSessionMatchActive()) {
			netWorldCensus("at the test's tick");
		}
	}
}

/*
 * Host
 */

// Whether the world block carries this glass (else E5's GLASS event does)
s32 netWorldGlassCovered(struct defaultobj *obj)
{
	s32 i;

	for (i = 0; i < s_NumGlass && i < NETWORLD_MAXGLASS; i++) {
		if (s_Glass[i] == obj) {
			return 1;
		}
	}

	return 0;
}

// nbombCreateStorm on the host: the next of the four storm slots
void netWorldHostStorm(struct coord *pos)
{
	u8 *p;

	if (g_NetMode != NETMODE_SERVER || !pos) {
		return;
	}

	s_StormSerial = s_StormSerial == 255 ? 1 : s_StormSerial + 1;
	p = s_HostBlock + NW_STORMS + (s_StormSerial % NW_NUMSTORMS) * NW_STORMSIZE;
	memset(p, 0, NW_STORMSIZE);
	p[0] = s_StormSerial;
	put32(p + 4, g_NetTick);
	putf(p + 8, pos->x);
	putf(p + 12, pos->y);
	putf(p + 16, pos->z);
}

// gasReleaseFromPos on the host: let go for the rest of the match
void netWorldHostGas(struct coord *pos)
{
	if (g_NetMode != NETMODE_SERVER || !pos) {
		return;
	}

	s_HostBlock[NW_GAS] = 1;
	put32(s_HostBlock + NW_GASTICK, g_NetTick);
	putf(s_HostBlock + NW_GASPOS + 0, pos->x);
	putf(s_HostBlock + NW_GASPOS + 4, pos->y);
	putf(s_HostBlock + NW_GASPOS + 8, pos->z);
}

/**
 * netTickEnd, before the snapshots: the glass as it stands now (broken, or
 * regenerated in a match), so the block is the host's world at the tick
 */
void netWorldHostTickEnd(void)
{
	const s32 n = s_NumGlass < NETWORLD_MAXGLASS ? s_NumGlass : NETWORLD_MAXGLASS;
	s32 i;

	if (g_NetMode != NETMODE_SERVER || !s_Glass) {
		return;
	}

	put16(s_HostBlock + NW_NGLASS, (u32)n);
	memset(s_HostBlock + NW_GLASS, 0, NETWORLD_SIZE - NW_GLASS);

	for (i = 0; i < n; i++) {
		if (netWorldGlassGone(s_Glass[i])) {
			s_HostBlock[NW_GLASS + (i >> 3)] |= (u8)(1 << (i & 7));
		}
	}

	s_HostHave = 1;
}

const u8 *netWorldHostBlock(void)
{
	return g_NetMode == NETMODE_SERVER && s_HostHave ? s_HostBlock : NULL;
}

/*
 * Client
 */

void netWorldClientOnSnap(u32 hosttick, const u8 *world)
{
	if (!s_Ring) {
		s_Ring = calloc(NW_RING, sizeof(*s_Ring));

		if (!s_Ring) {
			return;
		}
	}

	// snapshots come newest last (an older one is dropped as it is decoded)
	if (s_RingN && hosttick <= s_Ring[s_RingN - 1].tick) {
		return;
	}

	if (s_RingN == NW_RING) {
		memmove(s_Ring, s_Ring + 1, sizeof(*s_Ring) * (NW_RING - 1));
		s_RingN--;
	}

	s_Ring[s_RingN].tick = hosttick;
	memcpy(s_Ring[s_RingN].b, world, NETWORLD_SIZE);
	s_RingN++;
}

// A pane broken here as it stands: no shards, its marks gone (glassDestroy without the show)
static void netWorldGlassQuiet(struct defaultobj *obj)
{
	wallhitsFreeByProp(obj->prop, 0);
	wallhitsFreeByProp(obj->prop, 1);
	obj->damage = 0;
	obj->hidden |= OBJHFLAG_DELETING;
	obj->hidden2 |= OBJH2FLAG_DESTROYED;
}

static s32 netWorldApply(const u8 *b, u32 tick, f64 rt)
{
	const s32 n = get16(b + NW_NGLASS);
	const f64 age = rt - tick;
	s32 changed = 0;
	s32 i;
	s32 k;

	// the glass: a pane the host has broken that stands here, and (a
	// match's) one it has put back. A client never frees or regenerates an
	// object itself (netPuppetObjTick): glassDestroy's flags stay on its pane
	// until this takes them off; a match's pane hidden while it waits to
	// regenerate is the record's NETOBJ_GONE
	for (i = 0; i < n && i < s_NumGlass && i < NETWORLD_MAXGLASS; i++) {
		const u8 bit = (u8)(1 << (i & 7));
		const s32 was = s_HaveApplied && (s_Applied[NW_GLASS + (i >> 3)] & bit);
		struct defaultobj *obj = s_Glass[i];
		s32 dead;

		if (!obj->prop || obj->prop->obj != obj) {
			continue; // freed (a mission's)
		}

		dead = (obj->hidden2 & OBJH2FLAG_DESTROYED) || (obj->hidden & OBJHFLAG_DELETING);

		if ((b[NW_GLASS + (i >> 3)] & bit) == 0) {
			if (was && dead) {
				obj->hidden &= ~OBJHFLAG_DELETING;
				obj->hidden2 &= ~OBJH2FLAG_DESTROYED;
				obj->damage = 0;
				s_ClGlassBack++;
				changed = 1;
			}

			continue;
		}

		// glassDestroy reads the glass's bbox unchecked
		if (dead || !obj->model || !objFindBboxRodata(obj)) {
			continue;
		}

		// seen whole in the last block and broken in this one, in time: as
		// the host broke it (shards, even when the record has hidden it
		// already); else (a joiner's first block, a pane out of its scope)
		// as it stands
		if (s_HaveApplied && !was && age <= NW_LATE) {
			glassDestroy(obj);
			s_ClGlassFresh++;
		} else if ((obj->hidden & OBJHFLAG_GONE) == 0) {
			netWorldGlassQuiet(obj);
			s_ClGlassQuiet++;
		} else {
			continue;
		}

		changed = 1;
	}

	// the storms: one this machine has not started
	for (k = 0; k < NW_NUMSTORMS; k++) {
		const u8 *p = b + NW_STORMS + k * NW_STORMSIZE;
		const u8 *q = s_Applied + NW_STORMS + k * NW_STORMSIZE;
		const f64 sage = rt - get32(p + 4);
		struct coord pos;

		if (p[0] == 0 || (s_HaveApplied && p[0] == q[0])) {
			continue;
		}

		changed = 1;

		if (sage > NW_STORMLIFE) {
			continue; // over before this machine heard of it
		}

		pos.x = getf(p + 8);
		pos.y = getf(p + 12);
		pos.z = getf(p + 16);
		nbombCreateStorm(&pos, NULL);
		s_ClStorms++;

		// part way through: as old as the host's
		if (sage > NW_LATE) {
			s32 j;

			for (j = 0; j < 6; j++) {
				if (g_Nbombs[j].age240 == 0 && g_Nbombs[j].pos.x == pos.x && g_Nbombs[j].pos.z == pos.z) {
					g_Nbombs[j].age240 = (s32)(sage * 4);
					s_ClStormsLate++;
					break;
				}
			}
		}
	}

	// the gas, once
	if (b[NW_GAS] && !(s_HaveApplied && s_Applied[NW_GAS])) {
		const f64 gage = rt - get32(b + NW_GASTICK);
		struct coord pos;

		changed = 1;

		// gasTick fades to the stage's second sky, which a stage with no
		// sky of its own has not got (envApplyTransitionFrac would read NULL)
		if (g_EnvTransitionFrom && g_EnvTransitionTo) {
			pos.x = getf(b + NW_GASPOS + 0);
			pos.y = getf(b + NW_GASPOS + 4);
			pos.z = getf(b + NW_GASPOS + 8);
			gasReleaseFromPos(&pos);
			s_ClGas++;

			// let go a while ago: as far through its fade as the host's
			// (gasTick adds a 60th a 60th)
			if (gage > NW_LATE) {
				g_GasReleaseTimer240 = (f32)gage < g_GasReleaseTimerMax240 ? (f32)gage : g_GasReleaseTimerMax240;

				if (g_GasReleaseTimer240 >= g_GasReleaseTimerMax240) {
					g_GasReleasing = false;
				}
			}
		}
	}

	memcpy(s_Applied, b, NETWORLD_SIZE);
	s_HaveApplied = 1;

	return changed;
}

/**
 * After the pose step and the events (netClientPosePuppets): the newest
 * block the render clock has reached, never held for an event
 */
void netWorldClientUpTo(f64 rt)
{
	s32 i;
	s32 at = -1;

	if (g_NetMode != NETMODE_CLIENT || !s_Ring || !s_RingN || !s_Glass) {
		return;
	}

	for (i = 0; i < s_RingN; i++) {
		if ((f64)s_Ring[i].tick <= rt) {
			at = i;
		}
	}

	if (at < 0) {
		return;
	}

	if (!s_HaveApplied || memcmp(s_Ring[at].b, s_Applied, NETWORLD_SIZE) != 0) {
		if (netWorldApply(s_Ring[at].b, s_Ring[at].tick, rt)) {
			const f64 lag = rt - s_Ring[at].tick;

			s_ClBlocks++;
			s_ClLagSum += lag;

			if (lag > s_ClLagMax) {
				s_ClLagMax = lag;
			}
		}
	}

	memmove(s_Ring, s_Ring + at + 1, sizeof(*s_Ring) * (s_RingN - at - 1));
	s_RingN -= at + 1;
}

/**
 * Client (netpuppets.c, an OBJ or DOOR record posed): the host's crumpling
 * of the object, from the record's NETDEFORM_* byte; applied is what this
 * machine has made of it so far. Returns the new applied byte
 */
u8 netWorldClientDeform(struct defaultobj *obj, u8 rec, u8 applied)
{
	const s32 c = NETDEFORM_COUNT(rec);
	s32 a = NETDEFORM_COUNT(applied);
	const s32 level = NETDEFORM_LEVEL(rec) ? NETDEFORM_LEVEL(rec) : 1;

	if (!obj || !obj->model || c == a) {
		return rec;
	}

	if (c > a) {
		for (; a < c; a++) {
			objDeform(obj, level);
			netWorldNoteDeform();
			s_ClDeforms++;
		}
	} else if (c == 0 && netWorldObjDeformed(obj)) {
		// whole again on the host (a match's prop regenerated)
		modelFreeVertices(VTXSTORETYPE_OBJVTX, obj->model);
		s_ClUndeforms++;
	}

	return rec;
}

void netWorldLog(const char *why)
{
	sysLogPrintf(LOG_NOTE, "net: world client %s (tick %u): blocks that changed something %u (lag mean %.1f max %.1f ticks), glass broken as it broke %u, as it stood %u, back %u, storms %u (part way %u), gas %u, door panes %u, camera lenses %u, deforms from records %u (made whole %u)",
			why, g_NetTick, s_ClBlocks, s_ClBlocks ? s_ClLagSum / s_ClBlocks : 0.0, s_ClLagMax, s_ClGlassFresh, s_ClGlassQuiet, s_ClGlassBack,
			s_ClStorms, s_ClStormsLate, s_ClGas, s_ClDoorGlass, s_ClLenses, s_ClDeforms, s_ClUndeforms);
}

/*
 * (protocol 29) A camera's lens shot out (CCTV, Perfect Dark's and
 * GoldenEye's, whose ROM model keeps the lens's toggle): OBJ record byte 0
 * bit 0x80, so every client and a joiner see it out. The camera's watching
 * is the host's (cctvTick); a client only draws it
 */
static union modelrwdata *netWorldLensToggle(struct defaultobj *obj)
{
	struct modelnode *node;

	if (!obj || obj->type != OBJTYPE_CCTV || !obj->model || !obj->model->definition || !obj->model->rwdatas) {
		return NULL;
	}

	node = modelGetPart(obj->model->definition, MODELPART_CCTV_0003);

	return node ? modelGetNodeRwData(obj->model, node) : NULL;
}

s32 netWorldLensOut(struct defaultobj *obj)
{
	union modelrwdata *rw = netWorldLensToggle(obj);

	return rw && !rw->toggle.visible;
}

void netWorldClientLens(struct defaultobj *obj, s32 out, s32 fresh)
{
	union modelrwdata *rw = netWorldLensToggle(obj);

	if (!rw || !obj->prop || (rw->toggle.visible != 0) == !out) {
		return;
	}

	if (!out) {
		rw->toggle.visible = true; // the host's camera is whole again (a match's regenerated one)
		return;
	}

	if (fresh) {
		cctvHandleLensShot(obj); // its glass, as the host's shot made it
	} else {
		wallhitsFreeByProp(obj->prop, 1);
		rw->toggle.visible = false;
	}

	s_ClLenses++;
}

/*
 * (protocol 29) The marks on the walls and floors - bullet holes, scorches,
 * splats on the bg - as the host has them, to a joiner once (MARKS on the
 * bulk channel, beside its first EVENTS): the settled ones, oldest first,
 * as many as the pool keeps. Live play goes on making its own as before;
 * marks on props and chrs, which move or go, are left out
 */
#define NW_MARKMAX   1024
#define NW_MARKBYTES 90
#define NW_MARKMSG   16384

static struct wallhitnet *s_Marks = NULL; // [NW_MARKMAX]
static u32 s_MarksSent = 0;
static u32 s_MarksBytes = 0;
static u32 s_MarksGot = 0;
static u32 s_MarksPut = 0;

static s32 netWorldMarksAlloc(void)
{
	if (!s_Marks) {
		s_Marks = calloc(NW_MARKMAX, sizeof(*s_Marks));
	}

	return s_Marks != NULL;
}

static void netWorldMarkWrite(struct netbuf *b, const struct wallhitnet *w)
{
	s32 j;

	netBufWriteU32(b, *(const u32 *)&w->relpos[0]);
	netBufWriteU32(b, *(const u32 *)&w->relpos[1]);
	netBufWriteU32(b, *(const u32 *)&w->relpos[2]);
	netBufWriteS16(b, w->room);
	netBufWriteU8(b, w->texnum);
	netBufWriteU8(b, w->bits);
	netBufWriteS8(b, w->mtxindex);

	for (j = 0; j < 4; j++) {
		netBufWriteS16(b, w->xyz[j][0]);
		netBufWriteS16(b, w->xyz[j][1]);
		netBufWriteS16(b, w->xyz[j][2]);
		netBufWriteS16(b, w->st[j][0]);
		netBufWriteS16(b, w->st[j][1]);
		netBufWriteBytes(b, w->base[j], 4);
		netBufWriteBytes(b, w->final[j], 4);
	}

	netBufWriteU8(b, 0); // to NW_MARKBYTES
}

static void netWorldMarkRead(struct netbuf *b, struct wallhitnet *w)
{
	s32 j;
	u32 v;

	for (j = 0; j < 3; j++) {
		v = netBufReadU32(b);
		memcpy(&w->relpos[j], &v, 4);
	}

	w->room = netBufReadS16(b);
	w->texnum = netBufReadU8(b);
	w->bits = netBufReadU8(b);
	w->mtxindex = netBufReadS8(b);

	for (j = 0; j < 4; j++) {
		w->xyz[j][0] = netBufReadS16(b);
		w->xyz[j][1] = netBufReadS16(b);
		w->xyz[j][2] = netBufReadS16(b);
		w->st[j][0] = netBufReadS16(b);
		w->st[j][1] = netBufReadS16(b);
		netBufReadBytes(b, w->base[j], 4);
		netBufReadBytes(b, w->final[j], 4);
	}

	netBufReadU8(b);
	w->frame = 0;
}

// Host: a joiner's view, its first EVENTS going (netEventsHostCatchUp)
void netWorldHostSendMarks(s32 view)
{
	static u8 msg[NW_MARKMSG];
	s32 n;
	s32 i = 0;
	s32 msgs = 0;
	u32 bytes = 0;

	if (g_NetMode != NETMODE_SERVER || !netWorldMarksAlloc()) {
		return;
	}

	n = wallhitNetList(s_Marks, NW_MARKMAX);

	while (i < n) {
		struct netbuf b;
		u8 *countp;
		s32 count = 0;

		netBufInitWrite(&b, msg, sizeof(msg));
		netBufWriteU8(&b, NETMSG_MARKS);
		netBufWriteU32(&b, netSessionMatchId());
		countp = netBufReserve(&b, 2);

		while (i < n && netBufLen(&b) + NW_MARKBYTES <= NW_MARKMSG) {
			if (s_Marks[i].relpos[0] == s_Marks[i].relpos[0] && s_Marks[i].relpos[1] == s_Marks[i].relpos[1]
					&& s_Marks[i].relpos[2] == s_Marks[i].relpos[2]) {
				netWorldMarkWrite(&b, &s_Marks[i]);
				count++;
			}

			i++;
		}

		if (!countp || !netBufOk(&b)) {
			break;
		}

		countp[0] = (u8)count;
		countp[1] = (u8)(count >> 8);
		netSessionSendSlot(view, NET_CHAN_BULK, msg, netBufLen(&b), NET_SEND_RELIABLE);
		msgs++;
		bytes += netBufLen(&b);
		s_MarksSent += count;
	}

	s_MarksBytes += bytes;
	sysLogPrintf(LOG_NOTE, "net: world marks to view %d at tick %u: %d of the host's marks on the walls and floors, %d messages, %u bytes",
			view, g_NetTick, n, msgs, bytes);
}

// Client: MARKS
void netWorldClientOnMarks(const u8 *data, s32 len)
{
	struct netbuf b;
	struct wallhitnet w;
	s32 n;
	s32 i;
	s32 put = 0;

	if (g_NetMode != NETMODE_CLIENT || !netSessionMatchLoading()) {
		return;
	}

	netBufInitRead(&b, data + 1, len - 1);

	if (netBufReadU32(&b) != netSessionMatchId()) {
		return;
	}

	n = netBufReadU16(&b);

	if (!netBufOk(&b) || netBufRemaining(&b) != n * NW_MARKBYTES) {
		sysLogPrintf(LOG_WARNING, "net: a MARKS message that did not decode (%d bytes)", len);
		return;
	}

	for (i = 0; i < n; i++) {
		netWorldMarkRead(&b, &w);

		if (!netBufOk(&b)) {
			return;
		}

		// off the wire: a place inside the world (a NaN fails it too)
		if (!(w.relpos[0] > -1048576.f && w.relpos[0] < 1048576.f && w.relpos[1] > -1048576.f && w.relpos[1] < 1048576.f
					&& w.relpos[2] > -1048576.f && w.relpos[2] < 1048576.f)) {
			continue;
		}

		put += wallhitNetAdd(&w) ? 1 : 0;
	}

	s_MarksGot += n;
	s_MarksPut += put;
	sysLogPrintf(LOG_NOTE, "net: world marks from the host: %d in this message, %d put on the walls (%u of %u so far)", n, put, s_MarksPut, s_MarksGot);
}

// The census's marks: how many, and a sum of their places (a placement check)
static void netWorldMarkCensus(s32 *count, f64 *sum)
{
	s32 n;
	s32 i;

	*count = 0;
	*sum = 0;

	if (!netWorldMarksAlloc()) {
		return;
	}

	n = wallhitNetList(s_Marks, NW_MARKMAX);
	*count = n;

	for (i = 0; i < n; i++) {
		*sum += (s32)s_Marks[i].relpos[0] + (s32)s_Marks[i].relpos[1] + (s32)s_Marks[i].relpos[2];
	}
}
