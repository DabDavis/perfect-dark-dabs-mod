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
#include "net/net.h"
#include "netint.h"

/**
 * The world's latest-value-wins state outside any entity record: what the
 * test harness breaks and counts (--net-test-wreck, --net-test-census), on
 * both machines, so a late joiner's world can be held against the host's.
 *
 * The stage's setup is the same on every machine (the content hashes say
 * so), so its glass and windowed doors are numbered by their order among
 * the setup commands on each machine alike.
 */

extern struct nbomb g_Nbombs[6];

#define NETWORLD_MAXLIST 4096
#define NETWORLD_MAXWRECK 4

static struct defaultobj **s_Glass = NULL;  // setup glass and tinted glass, in command order
static s32 s_NumGlass = 0;
static struct doorobj **s_Doors = NULL;     // setup doors with a windowed door's skeleton
static s32 s_NumDoors = 0;
static struct defaultobj **s_Basic = NULL;  // setup basic objects (the wreck's deform targets)
static s32 s_NumBasic = 0;

static u32 s_Deforms = 0;    // objDeform calls the host made / a client applied
static u32 s_WreckAt[NETWORLD_MAXWRECK];
static s32 s_NumWreck = 0;
static u32 s_CensusAt = 0;

static void netWorldFreeLists(void)
{
	free(s_Glass);
	free(s_Doors);
	free(s_Basic);
	s_Glass = NULL;
	s_Doors = NULL;
	s_Basic = NULL;
	s_NumGlass = s_NumDoors = s_NumBasic = 0;
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

static s32 netWorldGlassGone(struct defaultobj *obj)
{
	return (obj->hidden2 & OBJH2FLAG_DESTROYED) || (obj->hidden & OBJHFLAG_DELETING) || obj->prop == NULL
		|| obj->prop->obj != obj;
}

void netWorldStageStart(void)
{
	u32 *cmd = g_StageSetup.props;
	s32 n = 0;
	const char *arg;

	netWorldFreeLists();
	s_Deforms = 0;
	s_NumWreck = 0;
	s_CensusAt = 0;

	if (g_NetMode == NETMODE_NONE || !cmd) {
		return;
	}

	s_Glass = calloc(NETWORLD_MAXLIST, sizeof(*s_Glass));
	s_Doors = calloc(NETWORLD_MAXLIST, sizeof(*s_Doors));
	s_Basic = calloc(NETWORLD_MAXLIST, sizeof(*s_Basic));

	if (!s_Glass || !s_Doors || !s_Basic) {
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
		} else if (type == OBJTYPE_BASIC && s_NumBasic < NETWORLD_MAXLIST) {
			s_Basic[s_NumBasic++] = obj;
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

	// --net-test-census T: both machines count the world at their tick T
	arg = sysArgGetString("--net-test-census");
	s_CensusAt = arg ? (u32)strtoul(arg, NULL, 0) : 0;
}

void netWorldMatchStopped(void)
{
	netWorldFreeLists();
}

void netWorldNoteDeform(void)
{
	s_Deforms++;
}

static void netWorldWreck(s32 phase)
{
	s32 broke = 0;
	s32 doors = 0;
	s32 deformed = 0;
	s32 i;
	struct coord at = {0, 0, 0};
	s32 haveat = 0;

	for (i = phase & 1; i < s_NumGlass; i += 2) {
		struct defaultobj *obj = s_Glass[i];

		if (!netWorldGlassGone(obj) && obj->model && objFindBboxRodata(obj)) {
			if (!haveat) {
				at = obj->prop->pos;
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
				haveat = 1;
			}
		}
	}

	if (haveat) {
		nbombCreateStorm(&at, NULL);

		if (phase == 0) {
			gasReleaseFromPos(&at);
		}
	}

	sysLogPrintf(LOG_NOTE, "net: world wreck %d at tick %u: glass %d broken, windowed doors %d, objects crumpled %d, storm %d, gas %d",
			phase, g_NetTick, broke, doors, deformed, haveat, haveat && phase == 0);
}

void netWorldCensus(const char *why)
{
	s32 glass = 0;
	s32 windowed = 0;
	s32 doorgone = 0;
	s32 storms = 0;
	s32 i;

	for (i = 0; i < s_NumGlass; i++) {
		glass += netWorldGlassGone(s_Glass[i]);
	}

	for (i = 0; i < s_NumDoors; i++) {
		if (netWorldDoorIsWindowed(s_Doors[i])) {
			windowed++;
			doorgone += netWorldDoorGlassGone(s_Doors[i]);
		}
	}

	for (i = 0; i < 6; i++) {
		storms += g_Nbombs[i].age240 >= 0;
	}

	sysLogPrintf(LOG_NOTE, "net: world census %s %s tick %u: glass broken %d/%d, door glass broken %d/%d, deforms %u, storms %d, gas %d (timer %.0f)",
			g_NetMode == NETMODE_SERVER ? "host" : "client", why, g_NetTick, glass, s_NumGlass, doorgone, windowed,
			s_Deforms, storms, g_GasReleaseTimer240 > 0, (f64)g_GasReleaseTimer240);
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
	if (s_CensusAt && g_NetTick == s_CensusAt && netSessionMatchActive()) {
		netWorldCensus("at the test's tick");
	}
}
