#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "config.h"
#include "lib/model.h"
#include "lib/anim.h"
#include "lib/mtx.h"
#include "lib/vi.h"
#include "game/bondmove.h"
#include "game/bondgun.h"
#include "game/prop.h"
#include "game/camera.h"
#include "game/chr.h"
#include "game/gfxmemory.h"
#include "game/bg.h"
#include "game/playermgr.h"
#include "game/game_0b0fd0.h"
#include "net/net.h"
#include "netint.h"

/**
 * Lag-compensated hits on the host (PLANS/NETPLAY.md "Hits", step 3; phase
 * 5b). Our own code: Source's player_lagcompensation.cpp and Unlagged's
 * g_unlagged.c were read for the shape only, and Overwatch's notes
 * (overwatch-netcode/NOTES.md) for the swept-volume broad phase and the rule
 * that only positions and poses are rewound.
 *
 * Host, after every tick (netTickEnd, before the gfx swap): each chr's pose
 * goes into a ring of NETLC_RING ticks - its prop's place and rooms, the
 * anim (struct anim whole), the root's chrinfo (position, yaw, ground) and
 * the aim values the joint callback bends the body by. A side table keyed by
 * chr slot; nothing is added to chrdata or prop.
 *
 * A client's command carries the host tick its puppets were drawn at (its
 * render clock, with the fraction; netplayers.c). When the host plays a
 * remote player's shot (handsTickAttack in that player's pass, so the chrs'
 * matrices are in that player's camera space), shotCalculateHits calls
 * netLagCompShotBegin before its chr loop: for every other chr on that
 * screen, the pose at the client's tick (two ring entries interpolated, the
 * newest being the live state) is tested first as a swept box along the
 * shot (the volume between where the chr is and where it was); only one the
 * ray crosses has its matrices rebuilt at the old pose, the normal way
 * (modelSetMatricesWithAnim with chrHandleJointPositioned, in the shooter's
 * camera) into fresh gfxAllocate memory. model->matrices points at those
 * only for the chr loop: netLagCompShotEnd puts every pointer back, so the
 * live matrices are never written. Held guns and hats ride along (their
 * matrices moved rigidly with the node they hang from). Health, shields and
 * who is alive stay current: only where the body was is taken back, and
 * never from an earlier life (a respawn or a teleport since ends the
 * history: struct netlclife). How far back is bounded by the host's own
 * numbers, never the client's (netLcViewFor): the network's share - the
 * round trip ENet measures and the command's wait in the host's queue - is
 * capped at Net.LagCompMaxMs (200), and an interpolation delay worked out
 * from the host's snapshot rate and the link's variance (at most
 * NETLC_MAXINTERP) comes on top (at a 150 ms round trip the harness
 * measures 17.4 ticks, 290 ms, in all). The host's own player's shots are
 * not rewound; Net.LagComp=0 turns it all off.
 *
 * The autoaim: a remote player's autoaim picks and follows targets from the
 * chrs' matrices before its shots, and its crosshair (where the shot goes)
 * follows that over ticks. With the autoaim on, the whole of that player's
 * screen is rewound around its autoaimTick too (netLagCompPassBegin), or it
 * locks on where the target is now and its shots leave toward that. And a
 * frame drawn between ticks may move no aim (netLagCompPresentBegin/End).
 *
 * Not rewound: doors (a shot through a door that has since closed meets the
 * door as it is now - the door's matrices come from its frac in the
 * object path, objTestHit, and rebuilding them is left for later), lifts,
 * melee, and thrown and fired projectiles (they live on the host). A chr
 * that is not on the shooter's screen now but was where its shot went then
 * is rebuilt too, and put on the screen's list and flagged for the loop.
 *
 * Client: nothing is changed; the shot's hits are logged for the harness
 * (--net-lagcomp-log), what this machine saw when it fired.
 */

#define NETLC_RING      64     // ticks of pose history per chr
#define NETLC_MAXREW    32     // chrs rebuilt for one shot or pass
#define NETLC_MAXRW     4096   // a model's rwdata, in bytes, we keep a copy of
#define NETLC_MAXKIDS   4      // held props moved with a rebuilt chr
#define NETLC_MAXINTERP 19.f   // ticks of a client's interpolation delay allowed for: netpuppets.c's
                               // most for an honest client (2 snapshot intervals of 3, twice a
                               // jitter of 6, 1)
#define NETLC_SLACK     2.f    // ticks of the host's own allowance on a claimed view (frame and
                               // service granularity on both machines)
#define NETLC_TELEPORT  300.f  // farther than this in one tick is a teleport or a respawn, not a move

/**
 * A chr slot's lives (beside the ring): a respawn (dead to alive), a jump
 * farther than anything walks in a tick, or a gap in the history starts
 * another, and no pose from an earlier one is ever used or blended across
 */
struct netlclife {
	u32 serial;
	u8 valid;             // the last tick's pose is below
	u8 dead;
	struct prop *prop;
	f32 pos[3];
};

struct netlcpose {
	u32 tick1;            // the tick + 1, 0 empty
	struct prop *prop;
	struct model *model;
	u16 gen;
	u8 dead;
	u32 life;             // the chr's life on this body (a respawn or teleport starts another)
	f32 pos[3];
	RoomNum rooms[8];
	struct anim anim;
	struct modelrwdata_chrinfo chrinfo;
	f32 aimuprshoulder;
	f32 aimuplshoulder;
	f32 aimupback;
	f32 aimsideback;
	f32 angleoffset;
};

struct netlckid {
	struct model *model;
	Mtxf *oldmtx;
};

struct netlcrew {
	struct prop *prop;
	struct chrdata *chr;
	struct model *model;
	Mtxf *oldmtx;
	f32 oldz;
	s32 nkids;
	struct netlckid kids[NETLC_MAXKIDS];
	f32 curpos[3];
	f32 rewpos[3];
	u8 offscreen;        // not on this screen now: on it for the rewind (the flag and the list)
};

extern struct chrdata *g_CurModelChr;
void chrHandleJointPositioned(s32 joint, Mtxf *mtx);

s32 g_NetLagComp = 1;
static s32 s_MaxMs = 200;

PD_CONSTRUCTOR static void netLagCompConfigInit(void)
{
	configRegisterInt("Net.LagComp", &g_NetLagComp, 0, 1);
	configRegisterInt("Net.LagCompMaxMs", &s_MaxMs, 0, 1000);
}

static struct netlcpose *s_Hist = NULL; // [s_HistSlots][NETLC_RING]
static struct netlclife *s_Life = NULL; // [s_HistSlots]
static s32 s_HistSlots = 0;

static struct netlcrew s_Rew[NETLC_MAXREW];
static s32 s_NumRew = 0;
static s32 s_ListAdded = 0;    // props put on the end of the on-screen list for the rewind
static s32 s_ListCount = 0;    // ... and the count before them
static u32 s_Offscreen = 0;
static s32 s_InShot = 0;
static s32 s_InPass = 0;       // a remote pass's autoaim and shots: everything on its screen rewound
static u32 s_Passes = 0;
static u32 s_DebugLines = 0;
static f64 s_ShotView = 0;
static f64 s_ShotRewind = 0;
static f32 s_ShotDelay = 0;
static s32 s_ShotCapped = 0;
static s32 s_ShotCands = 0;
static s32 s_ShotSwept = 0;
static u8 s_RwSave[NETLC_MAXRW];

// counts
static u32 s_Shots = 0;        // remote shots looked at
static u32 s_ShotsRewound = 0; // ... with a rewind to do
static u32 s_Capped = 0;
static u32 s_Cands = 0;        // chrs on screen
static u32 s_Rebuilt = 0;      // ... whose box the shot crossed
static u32 s_NoSpace = 0;      // ... with no gfx left for it
static u32 s_NoPose = 0;
static u32 s_OtherLife = 0;    // ... whose pose then was in an earlier life (a respawn or teleport since)
static u32 s_Claims = 0;       // views claimed older than the host's own bound for that client
static f64 s_RewindSum = 0;
static f64 s_RewindMax = 0;
static u32 s_GfxPeak[2] = {0, 0}; // vtx pool use at a tick's end: first half, second half of the match
static u32 s_GfxPool = 0;
static u32 s_PeakTick = 0;

static FILE *s_Log = NULL;
static s32 s_Debug = 0;
static s32 s_God = 0;
static s32 s_Invincible = 0; // --net-test-invincible
static s32 s_InvinciblePad = -1; // --net-test-invincible-pad N: that slot's player alone
static s32 s_Compare = 0;       // --net-lagcomp-compare
static f32 s_ShotDistance = 0;  // the shot's reach before the chr loop (hits shorten it)

static void netLagCompAtExit(void);

void netLagCompArgs(void)
{
	const char *path = sysArgGetString("--net-lagcomp-log");

	if (path && !s_Log) {
		s_Log = fopen(path, "w");

		if (s_Log) {
			setvbuf(s_Log, NULL, _IOLBF, 0);
		}
	}

	s_Debug = sysArgCheck("--net-lagcomp-debug");

	// --exit-frame and the like end the process mid-match: the counts still go out
	atexit(netLagCompAtExit);
	s_God = sysArgCheck("--net-test-god");
	s_Invincible = sysArgCheck("--net-test-invincible");
	s_InvinciblePad = sysArgGetString("--net-test-invincible-pad") ? atoi(sysArgGetString("--net-test-invincible-pad")) : -1;
	s_Compare = sysArgCheck("--net-lagcomp-compare");
}

static s32 netLcSlotOf(const struct chrdata *chr)
{
	const s32 i = (s32)(chr - g_ChrSlots);

	return g_ChrSlots && i >= 0 && i < g_NumChrSlots && i < s_HistSlots ? i : -1;
}

static s32 netLcDeadPlayer(struct prop *prop);

static s32 netLcCapture(struct chrdata *chr, struct netlcpose *p)
{
	struct prop *prop = chr->prop;
	struct model *model = chr->model;
	struct modelnode *root;
	s32 i;
	s32 idx;

	if (!prop || !model || !model->anim || !model->definition || !model->rwdatas) {
		return 0;
	}

	root = model->definition->rootnode;

	if (!root || (root->type & 0xff) != MODELNODETYPE_CHRINFO) {
		return 0;
	}

	p->prop = prop;
	p->model = model;
	idx = netEntsPropIndex(prop);
	p->gen = idx >= 0 ? netEntsPropGen(idx) : 0;
	p->dead = chr->actiontype == ACT_DEAD || chr->actiontype == ACT_DIE || netLcDeadPlayer(prop);
	p->life = 0;
	p->pos[0] = prop->pos.x;
	p->pos[1] = prop->pos.y;
	p->pos[2] = prop->pos.z;

	for (i = 0; i < 8; i++) {
		p->rooms[i] = prop->rooms[i];

		if (prop->rooms[i] == -1) {
			break;
		}
	}

	for (; i < 8; i++) {
		p->rooms[i] = -1;
	}

	p->anim = *model->anim;
	memcpy(&p->chrinfo, modelGetNodeRwData(model, root), sizeof(p->chrinfo));
	p->aimuprshoulder = chr->aimuprshoulder;
	p->aimuplshoulder = chr->aimuplshoulder;
	p->aimupback = chr->aimupback;
	p->aimsideback = chr->aimsideback;

	if (chr->aibot) {
		p->angleoffset = chr->aibot->angleoffset;
	} else if (prop->type == PROPTYPE_PLAYER) {
		const s32 pn = playermgrGetPlayerNumByProp(prop);

		p->angleoffset = pn >= 0 && pn < MAX_PLAYERS && g_Vars.players[pn] ? g_Vars.players[pn]->angleoffset : 0;
	} else {
		p->angleoffset = 0;
	}

	return 1;
}

/**
 * The life a pose just captured for the slot belongs to: the last tick's,
 * or the next if since then the chr came back from the dead, jumped farther
 * than a tick's move, or changed body
 */
static u32 netLcLifeOf(s32 slot, const struct netlcpose *p)
{
	const struct netlclife *l = &s_Life[slot];
	f32 dx;
	f32 dy;
	f32 dz;

	if (!l->valid) {
		return l->serial;
	}

	if (l->prop != p->prop || (l->dead && !p->dead)) {
		return l->serial + 1;
	}

	dx = p->pos[0] - l->pos[0];
	dy = p->pos[1] - l->pos[1];
	dz = p->pos[2] - l->pos[2];

	return dx * dx + dy * dy + dz * dz > NETLC_TELEPORT * NETLC_TELEPORT ? l->serial + 1 : l->serial;
}

static f32 netLcLerp(f32 a, f32 b, f32 t)
{
	return a + (b - a) * t;
}

static f32 netLcLerpAngle(f32 a, f32 b, f32 t)
{
	f32 d = b - a;

	while (d > M_PI) d -= 2 * M_PI;
	while (d < -M_PI) d += 2 * M_PI;

	a += d * t;

	while (a >= 2 * M_PI) a -= 2 * M_PI;
	while (a < 0) a += 2 * M_PI;

	return a;
}

/**
 * An anim frame between two ticks' (a looped cycle may wrap between them,
 * the way it is running), as a client's puppet steps it (netpuppets.c)
 */
static f32 netLcFrameLerp(s16 animnum, f32 fa, f32 fb, f32 t, s32 rev)
{
	f32 n;
	f32 d;

	if (animnum <= 0 || animnum >= g_NumAnimations) {
		return t < 0.5f ? fa : fb;
	}

	n = (f32)animGetNumFrames(animnum);
	d = fb - fa;

	if (n > 1 && fabsf(d) > n * 0.5f) {
		if (!rev && d < 0) {
			d += n;
		} else if (rev && d > 0) {
			d -= n;
		} else {
			return t < 0.5f ? fa : fb;
		}
	}

	fa += d * t;

	if (n > 1) {
		while (fa >= n) {
			fa -= n;
		}

		while (fa < 0) {
			fa += n;
		}
	}

	return fa;
}

/**
 * The chr's pose at host tick `view` (whole ticks and a fraction): the ring's
 * entry for the tick before it and the one after (the live state for the
 * current tick) interpolated; the anim from the nearer
 */
static s32 netLcPoseAt(struct chrdata *chr, s32 slot, f64 view, struct netlcpose *out)
{
	struct netlcpose live;
	const struct netlcpose *a;
	const struct netlcpose *b;
	const u32 t0 = (u32)floor(view);
	const f32 f = (f32)(view - floor(view));

	if (!netLcCapture(chr, &live)) {
		return 0;
	}

	live.life = netLcLifeOf(slot, &live);

	a = &s_Hist[slot * NETLC_RING + t0 % NETLC_RING];

	if (t0 + 1 >= g_NetTick) {
		b = &live;
	} else {
		b = &s_Hist[slot * NETLC_RING + (t0 + 1) % NETLC_RING];

		if (b->tick1 != t0 + 2) {
			b = NULL;
		}
	}

	if (a->tick1 != t0 + 1) {
		a = NULL;
	}

	// a slot reused, or the body changed since: not this chr's history; nor
	// one from before a respawn or teleport (its corpse, its old place), so
	// nothing is blended across one either: the nearer entry of this life,
	// or none and the chr is tested where it is
	if (a && (a->prop != live.prop || a->model != live.model || a->gen != live.gen || a->life != live.life)) {
		s_OtherLife += a->life != live.life;
		a = NULL;
	}

	if (b && (b->prop != live.prop || b->model != live.model || b->gen != live.gen || b->life != live.life)) {
		s_OtherLife += b->life != live.life;
		b = NULL;
	}

	if (!a && !b) {
		return 0;
	}

	// dead then and alive now can only be another life: never a body to hit
	if (!live.dead && ((a && a->dead) || (b && b->dead))) {
		if ((!a || a->dead) && (!b || b->dead)) {
			return 0;
		}

		if (a && a->dead) {
			a = NULL;
		} else {
			b = NULL;
		}
	}

	if (!a || !b || a->dead != b->dead || a->anim.animnum != b->anim.animnum) {
		// one side, or nothing in between worth blending: the nearer whole tick
		*out = (!b || (a && f < 0.5f)) ? *a : *b;
		return 1;
	}

	*out = f < 0.5f ? *a : *b;
	out->pos[0] = netLcLerp(a->pos[0], b->pos[0], f);
	out->pos[1] = netLcLerp(a->pos[1], b->pos[1], f);
	out->pos[2] = netLcLerp(a->pos[2], b->pos[2], f);
	out->chrinfo.pos.x = netLcLerp(a->chrinfo.pos.x, b->chrinfo.pos.x, f);
	out->chrinfo.pos.y = netLcLerp(a->chrinfo.pos.y, b->chrinfo.pos.y, f);
	out->chrinfo.pos.z = netLcLerp(a->chrinfo.pos.z, b->chrinfo.pos.z, f);
	out->chrinfo.ground = netLcLerp(a->chrinfo.ground, b->chrinfo.ground, f);
	out->chrinfo.yrot = netLcLerpAngle(a->chrinfo.yrot, b->chrinfo.yrot, f);
	out->aimuprshoulder = netLcLerp(a->aimuprshoulder, b->aimuprshoulder, f);
	out->aimuplshoulder = netLcLerp(a->aimuplshoulder, b->aimuplshoulder, f);
	out->aimupback = netLcLerp(a->aimupback, b->aimupback, f);
	out->aimsideback = netLcLerp(a->aimsideback, b->aimsideback, f);
	out->angleoffset = netLcLerp(a->angleoffset, b->angleoffset, f);

	// the frames stepped between the two, as a puppet's are
	if (a->anim.flip == b->anim.flip && (a->anim.speed < 0) == (b->anim.speed < 0)) {
		out->anim.frame = netLcFrameLerp(a->anim.animnum, a->anim.frame, b->anim.frame, f, a->anim.speed < 0);
	}

	if (a->anim.animnum2 && a->anim.animnum2 == b->anim.animnum2 && a->anim.flip2 == b->anim.flip2
			&& (a->anim.speed2 < 0) == (b->anim.speed2 < 0)) {
		out->anim.frame2 = netLcFrameLerp(a->anim.animnum2, a->anim.frame2, b->anim.frame2, f, a->anim.speed2 < 0);
	}

	out->anim.fracmerge = netLcLerp(a->anim.fracmerge, b->anim.fracmerge, f);

	return 1;
}

/**
 * The broad phase: whether the shot's ray crosses the box that holds the
 * chr both where it is and where it was
 */
static s32 netLcSwept(const struct shotdata *sd, const f32 *cur, const f32 *rew, f32 radius)
{
	f32 lo[3];
	f32 hi[3];
	f32 o[3] = {sd->gunpos3d.x, sd->gunpos3d.y, sd->gunpos3d.z};
	f32 d[3] = {sd->gundir3d.x, sd->gundir3d.y, sd->gundir3d.z};
	f32 tmin = 0;
	f32 tmax = 1e30f;
	s32 k;

	for (k = 0; k < 3; k++) {
		const f32 pad = k == 1 ? 220.f : radius;

		lo[k] = (cur[k] < rew[k] ? cur[k] : rew[k]) - pad;
		hi[k] = (cur[k] > rew[k] ? cur[k] : rew[k]) + pad;

		if (fabsf(d[k]) < 1e-9f) {
			if (o[k] < lo[k] || o[k] > hi[k]) {
				return 0;
			}
		} else {
			f32 t1 = (lo[k] - o[k]) / d[k];
			f32 t2 = (hi[k] - o[k]) / d[k];

			if (t1 > t2) {
				const f32 tt = t1;
				t1 = t2;
				t2 = tt;
			}

			if (t1 > tmin) tmin = t1;
			if (t2 < tmax) tmax = t2;

			if (tmin > tmax) {
				return 0;
			}
		}
	}

	return 1;
}

// The distance from the shot's ray to a point (the debug log)
static f32 netLcRayDist(const struct shotdata *sd, const f32 *p)
{
	const f32 dx = sd->gundir3d.x;
	const f32 dy = sd->gundir3d.y;
	const f32 dz = sd->gundir3d.z;
	const f32 len = sqrtf(dx * dx + dy * dy + dz * dz);
	f32 v[3];
	f32 t;

	if (len <= 0) {
		return -1;
	}

	v[0] = p[0] - sd->gunpos3d.x;
	v[1] = p[1] - sd->gunpos3d.y;
	v[2] = p[2] - sd->gunpos3d.z;
	t = (v[0] * dx + v[1] * dy + v[2] * dz) / len;
	v[0] -= dx / len * t;
	v[1] -= dy / len * t;
	v[2] -= dz / len * t;

	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/*
 * Affine matrices, PD's row-vector way (mtx4TransformVec): a point is
 * p * M, the translation in row 3
 */

static void netLcAffMul(const Mtxf *a, const Mtxf *b, Mtxf *dst)
{
	Mtxf r;
	s32 i;
	s32 j;

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 3; j++) {
			r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j]
				+ (i == 3 ? b->m[3][j] : 0);
		}

		r.m[i][3] = i == 3 ? 1 : 0;
	}

	*dst = r;
}

static s32 netLcAffInv(const Mtxf *m, Mtxf *dst)
{
	const f32 a = m->m[0][0], b = m->m[0][1], c = m->m[0][2];
	const f32 d = m->m[1][0], e = m->m[1][1], f = m->m[1][2];
	const f32 g = m->m[2][0], h = m->m[2][1], k = m->m[2][2];
	const f32 det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
	f32 inv;
	s32 j;

	if (fabsf(det) < 1e-12f) {
		return 0;
	}

	inv = 1.f / det;
	dst->m[0][0] = (e * k - f * h) * inv;
	dst->m[0][1] = (c * h - b * k) * inv;
	dst->m[0][2] = (b * f - c * e) * inv;
	dst->m[1][0] = (f * g - d * k) * inv;
	dst->m[1][1] = (a * k - c * g) * inv;
	dst->m[1][2] = (c * d - a * f) * inv;
	dst->m[2][0] = (d * h - e * g) * inv;
	dst->m[2][1] = (b * g - a * h) * inv;
	dst->m[2][2] = (a * e - b * d) * inv;

	for (j = 0; j < 3; j++) {
		dst->m[3][j] = -(m->m[3][0] * dst->m[0][j] + m->m[3][1] * dst->m[1][j] + m->m[3][2] * dst->m[2][j]);
	}

	dst->m[0][3] = dst->m[1][3] = dst->m[2][3] = 0;
	dst->m[3][3] = 1;

	return 1;
}

/**
 * A held prop's matrices for the rebuilt body: moved with the node it hangs
 * from, rigidly (child * inverse(old node) * new node), into fresh memory
 */
static void netLcMoveKids(struct netlcrew *r, Mtxf *newmtx)
{
	struct prop *child = r->prop->child;

	r->nkids = 0;

	while (child && r->nkids < NETLC_MAXKIDS) {
		struct model *km = (child->type == PROPTYPE_OBJ || child->type == PROPTYPE_WEAPON) && child->obj ? child->obj->model : NULL;

		if (km && km->matrices && km->definition && km->attachedtomodel == r->model && km->attachedtonode
				&& (child->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
			Mtxf *oldnode = modelFindNodeMtx(r->model, km->attachedtonode, 0);
			const s32 n = km->definition->nummatrices;

			if (oldnode && oldnode >= r->oldmtx && oldnode < r->oldmtx + r->model->definition->nummatrices
					&& n > 0 && gfxHasVtxSpace(n * sizeof(Mtxf))) {
				Mtxf *newnode = newmtx + (oldnode - r->oldmtx);
				Mtxf inv;
				Mtxf delta;
				Mtxf *dst;
				s32 i;

				if (netLcAffInv(oldnode, &inv)) {
					netLcAffMul(&inv, newnode, &delta);
					dst = gfxAllocate(n * sizeof(Mtxf));

					for (i = 0; i < n; i++) {
						netLcAffMul(&km->matrices[i], &delta, &dst[i]);
					}

					r->kids[r->nkids].model = km;
					r->kids[r->nkids].oldmtx = km->matrices;
					r->nkids++;
					km->matrices = dst;
				}
			}
		}

		child = child->next;
	}
}

/**
 * The chr's matrices at pose p, in this pass's camera, into fresh gfx
 * memory; the anim, rwdata and aim are put back at once, the matrices and
 * depth stay swapped until netLagCompShotEnd
 */
static s32 netLcRebuild(struct netlcrew *r, const struct netlcpose *p)
{
	struct chrdata *chr = r->chr;
	struct model *model = r->model;
	struct prop *prop = r->prop;
	struct modelrenderdata rd = {0, 1, 3};
	struct anim saveanim;
	struct modelrwdata_chrinfo *ci = modelGetNodeRwData(model, model->definition->rootnode);
	const s32 rwbytes = model->rwdatalen * 4;
	const s32 n = model->definition->nummatrices;
	struct player *player = NULL;
	struct aibot *aibot = chr->aibot;
	f32 saveaim[5];
	Mtxf *newmtx;

	if (rwbytes <= 0 || rwbytes > NETLC_MAXRW || n <= 0) {
		return 0;
	}

	if (!gfxHasVtxSpace(n * sizeof(Mtxf))) {
		s_NoSpace++;
		return 0;
	}

	if (!aibot && prop->type == PROPTYPE_PLAYER) {
		const s32 pn = playermgrGetPlayerNumByProp(prop);

		player = pn >= 0 && pn < MAX_PLAYERS ? g_Vars.players[pn] : NULL;
	}

	saveanim = *model->anim;
	memcpy(s_RwSave, model->rwdatas, rwbytes);
	saveaim[0] = chr->aimuprshoulder;
	saveaim[1] = chr->aimuplshoulder;
	saveaim[2] = chr->aimupback;
	saveaim[3] = chr->aimsideback;
	saveaim[4] = aibot ? aibot->angleoffset : player ? player->angleoffset : 0;

	*model->anim = p->anim;

	// as a client's puppet is posed (netpuppets.c): the frames set from the
	// frame numbers, a speed of a half so a build with several players
	// keeps the frac (modelSetMatricesWithAnim), no end frame
	if (model->anim->animnum) {
		struct anim *an = model->anim;

		an->speed = an->speed < 0 ? -0.5f : 0.5f;
		an->endframe = -1;

		if (an->animnum2) {
			an->speed2 = an->speed2 < 0 ? -0.5f : 0.5f;
			an->endframe2 = -1;
		}

		modelSetAnimFrame2(model, p->anim.frame, p->anim.frame2);
	}

	*ci = p->chrinfo;
	chr->aimuprshoulder = p->aimuprshoulder;
	chr->aimuplshoulder = p->aimuplshoulder;
	chr->aimupback = p->aimupback;
	chr->aimsideback = p->aimsideback;

	if (aibot) {
		aibot->angleoffset = p->angleoffset;
	} else if (player) {
		player->angleoffset = p->angleoffset;
	}

	rd.unk00 = camGetWorldToScreenMtxf();
	rd.unk10 = gfxAllocate(n * sizeof(Mtxf));
	newmtx = rd.unk10;

	g_ModelJointPositionedFunc = &chrHandleJointPositioned;
	g_CurModelChr = chr;
	modelSetMatricesWithAnim(&rd, model);
	g_ModelJointPositionedFunc = NULL;

	prop->z = modelGetScreenDistance(model);

	// everything but the matrices back as it was
	*model->anim = saveanim;
	memcpy(model->rwdatas, s_RwSave, rwbytes);
	chr->aimuprshoulder = saveaim[0];
	chr->aimuplshoulder = saveaim[1];
	chr->aimupback = saveaim[2];
	chr->aimsideback = saveaim[3];

	if (aibot) {
		aibot->angleoffset = saveaim[4];
	} else if (player) {
		player->angleoffset = saveaim[4];
	}

	model->matrices = newmtx;

	if (!r->offscreen) {
		netLcMoveKids(r, newmtx);
	}

	return 1;
}

static void netLcHitsText(const struct shotdata *sd, char *buf, s32 size)
{
	s32 len = 0;
	s32 i;

	buf[0] = '\0';

	for (i = 0; i < ARRAYCOUNT(sd->hits) && len < size - 16; i++) {
		struct prop *root = sd->hits[i].prop;

		if (!root) {
			continue;
		}

		while (root->parent) {
			root = root->parent;
		}

		if (root->type == PROPTYPE_PLAYER) {
			len += snprintf(buf + len, size - len, "%sp%d", len ? "," : "", playermgrGetPlayerNumByProp(root));
		} else if (root->type == PROPTYPE_CHR && root->chr) {
			len += snprintf(buf + len, size - len, "%sc%d", len ? "," : "", (s32)(root->chr - g_ChrSlots));
		}
	}

	if (!len) {
		snprintf(buf, size, "-");
	}
}

/*
 * The hooks
 */

/**
 * The tick the pass's remote player saw the others at, capped (see the top);
 * 0 if it is not to be rewound
 */
static s32 netLcViewFor(s32 slot, f64 *viewout)
{
	f64 view;
	f64 rewind;
	f64 cap;
	f32 delay;

	s_ShotView = -1;
	s_ShotRewind = 0;
	s_ShotCapped = 0;
	s_ShotDelay = 0;

	if (!g_NetLagComp || !s_Hist || !netPlayersHostView(slot, &view, &delay)) {
		return 0;
	}

	// How far back the client may claim it saw, from the host's own numbers
	// alone (the client's are never trusted; its reported delay is only
	// logged): the network's share is the round trip ENet measures to that
	// peer and the command's wait in the queue here, with a little slack,
	// capped at Net.LagCompMaxMs (200: a worse link is no longer fully
	// favoured, Overwatch's ping cutoff). The interpolation delay comes on
	// top, as netpuppets.c's sum makes it (2 snapshot intervals, twice the
	// jitter, 1) at the slower snapshot rate and with the jitter taken from
	// the round trip's variance, itself never past NETLC_MAXINTERP.
	{
		s32 rttms = -1;
		s32 rttvar = 0;
		f64 net;
		f64 jit;
		f64 interp;
		const f64 maxnet = s_MaxMs * 60.0 / 1000.0;

		netSessionSlotRtt(slot, &rttms, &rttvar);

		net = rttms >= 0 ? rttms * 60.0 / 1000.0 + netPlayersHostDepth(slot) + NETLC_SLACK : maxnet;

		if (net > maxnet) {
			net = maxnet;
		}

		jit = rttvar * 60.0 / 1000.0;
		jit = jit < 0.5 ? 0.5 : jit > 6 ? 6 : jit;
		interp = 2.0 * 3 + 2.0 * jit + 1.0;

		if (interp > NETLC_MAXINTERP) {
			interp = NETLC_MAXINTERP;
		}

		cap = net + interp;
	}

	rewind = (f64)g_NetTick - view;
	s_ShotDelay = delay;

	if (rewind > cap) {
		rewind = cap;
		view = (f64)g_NetTick - cap;
		s_ShotCapped = 1;
		s_Claims++;
	}

	s_ShotView = view;
	s_ShotRewind = rewind;
	*viewout = view;

	return rewind > 0.01;
}

static s32 netLcDeadPlayer(struct prop *prop)
{
	if (prop->type == PROPTYPE_PLAYER) {
		const s32 pn = playermgrGetPlayerNumByProp(prop);

		return pn < 0 || pn >= MAX_PLAYERS || !g_Vars.players[pn] || g_Vars.players[pn]->isdead;
	}

	return 0;
}

/**
 * The chrs not on this screen now that were where the shooter looked then:
 * a shot's whose swept box its ray crosses, or (the autoaim) whose old place
 * is in front of this camera. Rebuilt the same way, flagged on this screen
 * and put on the end of the on-screen list for the chr loop; both taken off
 * again with the matrices (netLcRestore)
 */
static void netLcRewindOffscreen(const struct shotdata *sd, f64 view)
{
	s32 i;

	s_ListCount = g_Vars.numonscreenprops;
	s_ListAdded = 0;

	if (!g_ChrSlots || !g_Vars.onscreenprops || g_Vars.endonscreenprops != g_Vars.onscreenprops + g_Vars.numonscreenprops) {
		return;
	}

	for (i = 0; i < s_HistSlots && s_NumRew < NETLC_MAXREW; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct prop *prop = chr->prop;
		struct netlcpose pose;
		struct netlcrew *r;
		f32 cur[3];

		if (chr->chrnum < 0 || !prop || !(prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) || prop->chr != chr
				|| prop == g_Vars.currentplayer->prop
				|| (prop->flags & (PROPFLAG_ONTHISSCREENTHISTICK | PROPFLAG_ENABLED)) != PROPFLAG_ENABLED
				|| (chr->chrflags & CHRCFLAG_HIDDEN) || !chr->model || chr->actiontype == ACT_DEAD || netLcDeadPlayer(prop)) {
			continue;
		}

		if (s_ListCount + s_ListAdded >= MAX_ONSCREENPROPS) {
			break;
		}

		if (!netLcPoseAt(chr, i, view, &pose)) {
			continue;
		}

		cur[0] = prop->pos.x;
		cur[1] = prop->pos.y;
		cur[2] = prop->pos.z;

		if (sd) {
			if (!netLcSwept(sd, cur, pose.pos, chrGetHitRadius(chr) * 1.5f + 40.f)) {
				continue;
			}
		} else {
			// in front of this camera, within a generous cone
			struct coord w = {pose.pos[0], pose.pos[1], pose.pos[2]};
			struct coord c;

			s32 k;
			s32 seen = 0;

			mtx4TransformVec(camGetWorldToScreenMtxf(), &w, &c);

			// the view's own cone, a body's width wider
			{
				const f32 tanv = tanf(viGetFovY() * (f32)(3.14159265 / 360.0));
				const f32 tanh = tanv * camGetPerspAspect();

				if (!(c.z < -1.f) || fabsf(c.x) > -c.z * tanh + 100.f || fabsf(c.y) > -c.z * tanv + 150.f) {
					continue;
				}
			}

			// and in a room this pass draws (what its machine could have seen)
			for (k = 0; k < 8 && pose.rooms[k] != -1; k++) {
				if (pose.rooms[k] >= 0 && pose.rooms[k] < g_Vars.roomcount && bgRoomIsOnscreen(pose.rooms[k])) {
					seen = 1;
					break;
				}
			}

			if (!seen) {
				continue;
			}
		}

		r = &s_Rew[s_NumRew];
		r->prop = prop;
		r->chr = chr;
		r->model = chr->model;
		r->oldmtx = chr->model->matrices;
		r->oldz = prop->z;
		r->nkids = 0;
		r->offscreen = 1;
		memcpy(r->curpos, cur, sizeof(cur));
		memcpy(r->rewpos, pose.pos, sizeof(pose.pos));

		if (netLcRebuild(r, &pose)) {
			s_NumRew++;
			s_Rebuilt++;
			s_ShotSwept++;
			s_Offscreen++;
			prop->flags |= PROPFLAG_ONTHISSCREENTHISTICK;
			g_Vars.onscreenprops[s_ListCount + s_ListAdded] = prop;
			s_ListAdded++;
		} else {
			r->model->matrices = r->oldmtx;
		}
	}

	if (s_ListAdded) {
		g_Vars.numonscreenprops = s_ListCount + s_ListAdded;
		g_Vars.endonscreenprops = g_Vars.onscreenprops + g_Vars.numonscreenprops;
		g_Vars.onscreenprops[g_Vars.numonscreenprops] = NULL;
	}
}

/**
 * Every other chr on this pass's screen at `view`: those the shot's ray
 * crosses the swept box of (sd), or all of them (sd NULL, for the autoaim)
 */
static void netLcRewindScreen(const struct shotdata *sd, f64 view)
{
	struct prop **propptr;

	for (propptr = g_Vars.endonscreenprops - 1; propptr >= g_Vars.onscreenprops && s_NumRew < NETLC_MAXREW; propptr--) {
		struct prop *prop = *propptr;
		struct chrdata *chr;
		struct netlcpose pose;
		struct netlcrew *r;
		f32 cur[3];
		s32 cslot;

		if (!prop || !(prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) || !prop->chr
				|| prop == g_Vars.currentplayer->prop || !(prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
			continue;
		}

		chr = prop->chr;

		if ((chr->chrflags & CHRCFLAG_HIDDEN) || !chr->model || !chr->model->matrices
				|| chr->actiontype == ACT_DEAD || netLcDeadPlayer(prop)) {
			continue;
		}

		cslot = netLcSlotOf(chr);
		s_ShotCands++;
		s_Cands++;

		if (cslot < 0 || !netLcPoseAt(chr, cslot, view, &pose)) {
			s_NoPose++;
			continue;
		}

		cur[0] = prop->pos.x;
		cur[1] = prop->pos.y;
		cur[2] = prop->pos.z;

		if (sd && !netLcSwept(sd, cur, pose.pos, chrGetHitRadius(chr) * 1.5f + 40.f)) {
			continue;
		}

		r = &s_Rew[s_NumRew];
		r->prop = prop;
		r->chr = chr;
		r->model = chr->model;
		r->oldmtx = chr->model->matrices;
		r->oldz = prop->z;
		r->nkids = 0;
		r->offscreen = 0;
		memcpy(r->curpos, cur, sizeof(cur));
		memcpy(r->rewpos, pose.pos, sizeof(pose.pos));

		if (netLcRebuild(r, &pose)) {
			s_NumRew++;
			s_Rebuilt++;
			s_ShotSwept++;
		}
	}

	netLcRewindOffscreen(sd, view);
}

static void netLcRestore(void)
{
	s32 i;
	s32 k;

	for (i = s_NumRew - 1; i >= 0; i--) {
		struct netlcrew *r = &s_Rew[i];

		for (k = r->nkids - 1; k >= 0; k--) {
			r->kids[k].model->matrices = r->kids[k].oldmtx;
		}

		r->model->matrices = r->oldmtx;
		r->prop->z = r->oldz;

		if (r->offscreen) {
			r->prop->flags &= ~PROPFLAG_ONTHISSCREENTHISTICK;
		}
	}

	if (s_ListAdded) {
		g_Vars.numonscreenprops = s_ListCount;
		g_Vars.endonscreenprops = g_Vars.onscreenprops + s_ListCount;
		g_Vars.onscreenprops[s_ListCount] = NULL;
		s_ListAdded = 0;
	}

	s_NumRew = 0;
}

/**
 * The place the autoaim's line of sight is tested to (func0f06438c): a chr
 * rewound for this pass is where the remote player saw it, as its matrices
 * are, not where it is now (a strafing target was 17 ticks on, and the line
 * to it cleared a wall's edge on the host ticks before it did on the client:
 * the host locked on first, and the two crosshairs parted for every lock)
 */
const struct coord *netLagCompAimPos(struct prop *prop)
{
	static struct coord pos;
	s32 i;

	if (g_NetMode != NETMODE_SERVER || !s_InPass) {
		return &prop->pos;
	}

	for (i = 0; i < s_NumRew; i++) {
		if (s_Rew[i].prop == prop) {
			pos.x = s_Rew[i].rewpos[0];
			pos.y = s_Rew[i].rewpos[1];
			pos.z = s_Rew[i].rewpos[2];
			return &pos;
		}
	}

	return &prop->pos;
}

/**
 * lvRender, before autoaimTick in a remote player's pass: with the autoaim
 * on for its gun, its autoaim picks and follows the others where it saw
 * them (as its own machine's did), so every chr on its screen then is
 * rewound now, until its first shot's chr loop is over (or
 * netLagCompPassEnd if it fires none). Otherwise nothing: each shot rewinds
 * what its ray may cross.
 */
void netLagCompPassBegin(void)
{
	const s32 slot = g_Vars.currentplayernum;
	f64 view;

	s_NumRew = 0;
	s_InPass = 0;

	if (!netPlayersHostSlotIsRemote(slot) || !netSessionMatchActive() || g_NetPass < NETPASS_TICK) {
		return;
	}

	if (!bmoveIsAutoAimXEnabledForCurrentWeapon() && !bmoveIsAutoAimYEnabledForCurrentWeapon()) {
		return;
	}

	// only a gun that shoots: melee (whose autoaim is always on) deals its
	// damage in handInflictMeleeDamage, outside shotCalculateHits, and must
	// never run on swapped matrices or a lengthened on-screen list; melee
	// is not rewound
	{
		struct weaponfunc *func = currentPlayerGetWeaponFunction(0);

		if (!func || (func->type & 0xff) != INVENTORYFUNCTYPE_SHOOT) {
			return;
		}
	}

	s_ShotCands = 0;
	s_ShotSwept = 0;

	if (!netLcViewFor(slot, &view)) {
		return;
	}

	s_InPass = 1;
	s_Passes++;
	netLcRewindScreen(NULL, view);
}

void netLagCompPassEnd(void)
{
	if (s_InPass) {
		netLcRestore();
		s_InPass = 0;
	}
}

void netLagCompShotBegin(struct shotdata *sd, s32 isshooting)
{
	f64 view;

	s_InShot = 0;

	if (g_NetMode != NETMODE_SERVER || !isshooting || g_NetPassPlayer < 0 || !netSessionMatchActive()) {
		return;
	}

	s_InShot = 1;
	s_Shots++;
	s_ShotDistance = sd->distance;

	if (s_InPass) {
		// the pass has them all where the shooter saw them already
		s_ShotsRewound++;
		s_RewindSum += s_ShotRewind;
		s_Capped += s_ShotCapped;

		if (s_ShotRewind > s_RewindMax) {
			s_RewindMax = s_ShotRewind;
		}

		return;
	}

	s_NumRew = 0;
	s_ShotCands = 0;
	s_ShotSwept = 0;

	if (!netLcViewFor(g_NetPassPlayer, &view)) {
		return;
	}

	s_ShotsRewound++;
	s_RewindSum += s_ShotRewind;
	s_Capped += s_ShotCapped;

	if (s_ShotRewind > s_RewindMax) {
		s_RewindMax = s_ShotRewind;
	}

	netLcRewindScreen(sd, view);
}

void netLagCompShotEnd(struct shotdata *sd, s32 isshooting, s32 cheap)
{
	char hits[128];
	s32 i;

	if (g_NetMode == NETMODE_CLIENT) {
		// what this machine saw when its player fired (the harness's side)
		if (s_Log && isshooting && !g_NetReplaying && g_Vars.currentplayernum == g_NetLocalSlot && netSessionMatchActive()) {
			f64 view = -1;
			char rays[256];
			s32 len = 0;
			s32 pn;

			netPuppetsViewTick(&view, NULL);
			netLcHitsText(sd, hits, sizeof(hits));
			rays[0] = '\0';

			// how far the shot passed from each other player's place as drawn here
			for (pn = 0; pn < PLAYERCOUNT() && len < (s32)sizeof(rays) - 48; pn++) {
				if (pn != g_NetLocalSlot && g_Vars.players[pn] && g_Vars.players[pn]->prop) {
					f32 p[3] = {g_Vars.players[pn]->prop->pos.x, g_Vars.players[pn]->prop->pos.y, g_Vars.players[pn]->prop->pos.z};

					len += snprintf(rays + len, sizeof(rays) - len, " p%d %.1f %.1f %.1f %.1f", pn, netLcRayDist(sd, p), p[0], p[1], p[2]);
				}
			}

			fprintf(s_Log, "C %u slot %d cmd %u view %.2f hits %s rays%s shot %.1f %.1f %.1f dir %.3f %.3f %.3f cross %.1f %.1f ax %.3f ay %.3f st %.3f sway %.2f\n", g_NetTick, g_NetLocalSlot, g_NetTick, view, hits, rays,
					sd->gunpos3d.x, sd->gunpos3d.y, sd->gunpos3d.z, sd->gundir3d.x, sd->gundir3d.y, sd->gundir3d.z,
					g_Vars.currentplayer->crosspos[0], g_Vars.currentplayer->crosspos[1], g_Vars.currentplayer->autoaimx, g_Vars.currentplayer->autoaimy, g_Vars.currentplayer->speedtheta, PLAYER_EXTCFG().crosshairsway);
		}

		return;
	}

	if (!s_InShot) {
		return;
	}

	s_InShot = 0;

	if (s_Log) {
		netLcHitsText(sd, hits, sizeof(hits));
		fprintf(s_Log, "H %u slot %d cmd %d view %.2f rewind %.2f capped %d lagcomp %d cands %d rebuilt %d hits %s interp %.2f pass %d\n",
				g_NetTick, g_NetPassPlayer, netPlayersHostLastPlayed(g_NetPassPlayer), s_ShotView, s_ShotRewind, s_ShotCapped,
				g_NetLagComp, s_ShotCands, s_ShotSwept, hits, s_ShotDelay, s_InPass);

		if (s_Debug) {
			fprintf(s_Log, "S %u shot %.1f %.1f %.1f dir %.3f %.3f %.3f cross %.1f %.1f ax %.3f ay %.3f st %.3f sway %.2f\n", g_NetTick, sd->gunpos3d.x, sd->gunpos3d.y, sd->gunpos3d.z,
					sd->gundir3d.x, sd->gundir3d.y, sd->gundir3d.z,
					g_Vars.currentplayer->crosspos[0], g_Vars.currentplayer->crosspos[1], g_Vars.currentplayer->autoaimx, g_Vars.currentplayer->autoaimy, g_Vars.currentplayer->speedtheta, PLAYER_EXTCFG().crosshairsway);
		}
	}

	if (s_Debug) {
		for (i = 0; i < s_NumRew; i++) {
			struct netlcrew *r = &s_Rew[i];
			char who[16];
			char line[384];

			if (r->prop->type == PROPTYPE_PLAYER) {
				snprintf(who, sizeof(who), "p%d", playermgrGetPlayerNumByProp(r->prop));
			} else {
				snprintf(who, sizeof(who), "c%d", (s32)(r->chr - g_ChrSlots));
			}

			// the root joint's place, from the live matrices and the rebuilt
			// ones, back in the world (camera space to world)
			struct coord rootnow = {0, 0, 0};
			struct coord rootthen = {0, 0, 0};
			const s32 ri = r->model->definition->rootnode ? modelFindNodeMtxIndex(r->model->definition->rootnode, 0) : 0;

			if (!r->offscreen && r->oldmtx) {
				struct coord c = {r->oldmtx[ri].m[3][0], r->oldmtx[ri].m[3][1], r->oldmtx[ri].m[3][2]};
				mtx4TransformVec(camGetProjectionMtxF(), &c, &rootnow);
			}

			if (r->model->matrices) {
				struct coord c = {r->model->matrices[ri].m[3][0], r->model->matrices[ri].m[3][1], r->model->matrices[ri].m[3][2]};
				mtx4TransformVec(camGetProjectionMtxF(), &c, &rootthen);
			}

			snprintf(line, sizeof(line), "D %u shooter %d chr %s now %.1f %.1f %.1f then %.1f %.1f %.1f moved %.1f ray-now %.1f ray-then %.1f root-now %.1f %.1f %.1f root-then %.1f %.1f %.1f%s",
					g_NetTick, g_NetPassPlayer, who, r->curpos[0], r->curpos[1], r->curpos[2],
					r->rewpos[0], r->rewpos[1], r->rewpos[2],
					sqrtf((r->curpos[0] - r->rewpos[0]) * (r->curpos[0] - r->rewpos[0])
						+ (r->curpos[2] - r->rewpos[2]) * (r->curpos[2] - r->rewpos[2])),
					netLcRayDist(sd, r->curpos), netLcRayDist(sd, r->rewpos),
					rootnow.x, rootnow.y, rootnow.z, rootthen.x, rootthen.y, rootthen.z, r->offscreen ? " offscreen" : "");
			s_DebugLines++;

			if (s_Log) {
				fprintf(s_Log, "%s\n", line);
			} else {
				sysLogPrintf(LOG_NOTE, "net: lagcomp %s", line);
			}
		}
	}

	// back before the shot's hits are dealt (a death drops what the chr
	// held, a held grenade goes off): a pass's rewind is spent too, and the
	// pass's later shots (a shotgun's pellets, the left gun) rewind for
	// themselves
	netLcRestore();
	s_InPass = 0;

	// --net-lagcomp-compare: the same shot against the chrs as they are now
	// (the harness's A/B on identical shots; chrTestHit as a query, no near
	// miss counted)
	if (s_Compare && s_Log && s_ShotRewind > 0.01) {
		struct shotdata now = *sd;
		struct prop **propptr;
		s32 k;

		now.distance = s_ShotDistance;

		for (k = 0; k < ARRAYCOUNT(now.hits); k++) {
			now.hits[k].prop = NULL;
			now.hits[k].hitpart = 0;
			now.hits[k].bboxnode = NULL;
		}

		for (propptr = g_Vars.endonscreenprops - 1; propptr >= g_Vars.onscreenprops; propptr--) {
			struct prop *prop = *propptr;

			if (prop && prop->chr && (prop->type == PROPTYPE_CHR
						|| (prop->type == PROPTYPE_PLAYER && prop != g_Vars.currentplayer->prop))) {
				chrTestHit(prop, &now, false, cheap);
			}
		}

		netLcHitsText(&now, hits, sizeof(hits));
		fprintf(s_Log, "N %u slot %d cmd %d hits-now %s\n", g_NetTick, g_NetPassPlayer, netPlayersHostLastPlayed(g_NetPassPlayer), hits);
	}
}

/*
 * The aim between ticks. A frame drawn between ticks (a present-only pass)
 * still runs each player's walk and its pass's autoaimTick with no time
 * passing, and those move the crosshair (bgunSwivel's sums, the autoaim's
 * target and its damping) toward what that frame shows: on the host the
 * others as they are now, not as the remote player saw them; on a client
 * its puppets a fraction of a tick on. Each machine draws its own number of
 * such frames, so the crosshair a shot leaves along came out different on
 * the two. A player's aim moves only on ticks here: what such a frame does
 * to it is put back at its end (the host's remote players, a client's own).
 */

struct netlcaim {
	u8 autoaim[offsetof(struct player, autoxaimtime60) + sizeof(s32) - offsetof(struct player, autoyaimenabled)];
	u8 cross[offsetof(struct player, gunaimdamp) + sizeof(f32) - offsetof(struct player, aimtype)];
	f32 oldcrosspos[2];
	f32 autoaimdamp;
	f32 handcross[2][4];
	u8 gangsta;
};

static struct netlcaim s_Aim[MAX_PLAYERS];
static u8 s_AimSaved[MAX_PLAYERS];

static s32 netLcAimOwned(s32 pn)
{
	if (g_NetMode == NETMODE_SERVER) {
		return netPlayersHostSlotIsRemote(pn);
	}

	return g_NetMode == NETMODE_CLIENT && pn == g_NetLocalSlot;
}

static void netLcAimSave(s32 pn, struct player *p)
{
	struct netlcaim *a = &s_Aim[pn];
	s32 h;

	memcpy(a->autoaim, (u8 *)p + offsetof(struct player, autoyaimenabled), sizeof(a->autoaim));
	memcpy(a->cross, (u8 *)p + offsetof(struct player, aimtype), sizeof(a->cross));
	a->oldcrosspos[0] = p->oldcrosspos[0];
	a->oldcrosspos[1] = p->oldcrosspos[1];
	a->autoaimdamp = p->autoaimdamp;
	a->gangsta = p->gunctrl.gangsta;

	for (h = 0; h < 2; h++) {
		a->handcross[h][0] = p->hands[h].crosspos[0];
		a->handcross[h][1] = p->hands[h].crosspos[1];
		a->handcross[h][2] = p->hands[h].guncrosspossum[0];
		a->handcross[h][3] = p->hands[h].guncrosspossum[1];
	}

	s_AimSaved[pn] = 1;
}

void netLagCompPresentBegin(void)
{
	s32 pn;

	for (pn = 0; pn < MAX_PLAYERS; pn++) {
		struct player *p = pn < PLAYERCOUNT() ? g_Vars.players[pn] : NULL;

		s_AimSaved[pn] = 0;

		if (!p || !netSessionMatchActive() || !netLcAimOwned(pn)) {
			continue;
		}

		netLcAimSave(pn, p);
	}
}

/**
 * A correction replayed on a frame between ticks (netents.c, the client's
 * own player): the aim it left is the tick's, so that is what the frame's
 * end puts back, not the aim from before it
 */
void netLagCompAimResave(s32 pn)
{
	struct player *p = pn >= 0 && pn < PLAYERCOUNT() ? g_Vars.players[pn] : NULL;

	if (p && pn < MAX_PLAYERS && s_AimSaved[pn]) {
		netLcAimSave(pn, p);
	}
}

void netLagCompPresentEnd(void)
{
	s32 pn;

	for (pn = 0; pn < MAX_PLAYERS; pn++) {
		struct player *p = pn < PLAYERCOUNT() ? g_Vars.players[pn] : NULL;
		const struct netlcaim *a = &s_Aim[pn];
		s32 h;

		if (!s_AimSaved[pn] || !p) {
			continue;
		}

		s_AimSaved[pn] = 0;
		memcpy((u8 *)p + offsetof(struct player, autoyaimenabled), a->autoaim, sizeof(a->autoaim));
		memcpy((u8 *)p + offsetof(struct player, aimtype), a->cross, sizeof(a->cross));
		p->oldcrosspos[0] = a->oldcrosspos[0];
		p->oldcrosspos[1] = a->oldcrosspos[1];
		p->autoaimdamp = a->autoaimdamp;
		p->gunctrl.gangsta = a->gangsta;

		for (h = 0; h < 2; h++) {
			p->hands[h].crosspos[0] = a->handcross[h][0];
			p->hands[h].crosspos[1] = a->handcross[h][1];
			p->hands[h].guncrosspossum[0] = a->handcross[h][2];
			p->hands[h].guncrosspossum[1] = a->handcross[h][3];
		}
	}
}

/**
 * The host's tick has run: every chr's pose for it into the ring. Before the
 * gfx swap; the live matrices are not read.
 */
void netLagCompHostTickEnd(void)
{
	s32 i;

	if (!g_ChrSlots || g_NumChrSlots <= 0) {
		return;
	}

	if (!s_Hist || s_HistSlots != g_NumChrSlots) {
		free(s_Hist);
		free(s_Life);
		s_Hist = calloc((size_t)g_NumChrSlots * NETLC_RING, sizeof(*s_Hist));
		s_Life = calloc((size_t)g_NumChrSlots, sizeof(*s_Life));
		s_HistSlots = s_Hist && s_Life ? g_NumChrSlots : 0;

		if (!s_HistSlots) {
			free(s_Hist);
			free(s_Life);
			s_Hist = NULL;
			s_Life = NULL;
			return;
		}
	}

	for (i = 0; i < s_HistSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct netlcpose *p = &s_Hist[i * NETLC_RING + g_NetTick % NETLC_RING];

		struct netlclife *l = &s_Life[i];

		p->tick1 = 0;

		if (chr->chrnum >= 0 && chr->prop && netLcCapture(chr, p)) {
			p->life = netLcLifeOf(i, p);
			p->tick1 = g_NetTick + 1;
			l->serial = p->life;
			l->valid = 1;
			l->dead = p->dead;
			l->prop = p->prop;
			memcpy(l->pos, p->pos, sizeof(l->pos));
		} else if (l->valid) {
			// a gap in the history: whatever comes next is another life
			l->serial++;
			l->valid = 0;
		}
	}

	// the vtx pool's use at the tick's end (the harness's leak check)
	if (g_VtxBuffers[g_GfxActiveBufferIndex] && g_GfxMemPos) {
		const u32 used = (u32)(g_GfxMemPos - g_VtxBuffers[g_GfxActiveBufferIndex]);
		const s32 half = g_NetTick >= 1500;

		s_GfxPool = (u32)(g_VtxBuffers[g_GfxActiveBufferIndex + 1] - g_VtxBuffers[g_GfxActiveBufferIndex]);

		if (used > s_GfxPeak[half]) {
			s_GfxPeak[half] = used;
			s_PeakTick = g_NetTick;
		}
	}

	// --net-test-invincible: the players take no damage at all, or with
	// --net-test-invincible-pad the one on that slot (an
	// explosion is more than --net-test-god's one point of health a tick)
	if (s_Invincible || s_InvinciblePad >= 0) {
		for (i = 0; i < PLAYERCOUNT(); i++) {
			if (g_Vars.players[i] && (s_Invincible || g_Vars.playerstats[i].mpindex % MAX_PLAYERS == s_InvinciblePad)) {
				g_Vars.players[i]->invincible = 1;
			}
		}
	}

	// --net-test-god: nobody dies and nobody runs dry (the harness's target
	// and shooter stay where they are)
	if (s_God) {
		for (i = 0; i < PLAYERCOUNT(); i++) {
			struct player *pl = g_Vars.players[i];
			s32 k;

			if (!pl || pl->isdead) {
				continue;
			}

			pl->bondhealth = 1;

			for (k = 0; k < ARRAYCOUNT(pl->ammoheldarr); k++) {
				if (pl->ammoheldarr[k] < 40) {
					pl->ammoheldarr[k] = 40;
				}
			}
		}
	}
}

void netLagCompLog(const char *why)
{
	if (g_NetMode != NETMODE_SERVER) {
		return;
	}

	sysLogPrintf(LOG_NOTE, "net: lagcomp %s: on %d, cap %d ms, remote shots %u, rewound %u (mean %.2f ticks, most %.2f, capped %u), "
			"autoaim passes rewound %u, chrs on screen %u, rebuilt %u (%u not on screen now), no history %u (another life then %u), claims past the host's bound %u, no gfx space %u, debug lines %u; "
			"vtx pool peak %u / %u bytes (first half), %u (second half, tick %u)",
			why, g_NetLagComp, s_MaxMs, s_Shots, s_ShotsRewound, s_ShotsRewound ? s_RewindSum / s_ShotsRewound : 0.0, s_RewindMax, s_Capped,
			s_Passes, s_Cands, s_Rebuilt, s_Offscreen, s_NoPose, s_OtherLife, s_Claims, s_NoSpace, s_DebugLines, s_GfxPeak[0], s_GfxPool, s_GfxPeak[1], s_PeakTick);
}

static void netLagCompAtExit(void)
{
	if (s_Hist) {
		netLagCompLog("at exit");
	}
}

void netLagCompStageStart(void)
{
	free(s_Hist);
	free(s_Life);
	s_Hist = NULL;
	s_Life = NULL;
	s_HistSlots = 0;
	s_NumRew = 0;
	s_InShot = 0;
	s_InPass = 0;
	s_Passes = 0;
	s_DebugLines = 0;
	s_Offscreen = 0;
	s_ListAdded = 0;
	s_Shots = s_ShotsRewound = s_Capped = s_Cands = s_Rebuilt = s_NoSpace = s_NoPose = s_OtherLife = s_Claims = 0;
	s_RewindSum = s_RewindMax = 0;
	s_GfxPeak[0] = s_GfxPeak[1] = 0;
	s_PeakTick = 0;
}

void netLagCompMatchStopped(void)
{
	netLagCompLog("at the match's end");
	free(s_Hist);
	free(s_Life);
	s_Hist = NULL;
	s_Life = NULL;
	s_HistSlots = 0;
}

/*
 * --net-lagcomp-debug: one line per tick of a player's aim, on the host for
 * each remote player (keyed by the command it played) and on a client for
 * its own (keyed by the tick), after the pass's autoaimTick and the shots:
 * what the two machines' autoaim and crosshair did with the same commands
 */
static void netLcPropName(const struct prop *prop, char *buf, s32 size)
{
	if (!prop) {
		snprintf(buf, size, "-");
	} else if (prop->type == PROPTYPE_PLAYER) {
		snprintf(buf, size, "p%d", playermgrGetPlayerNumByProp((struct prop *)prop));
	} else if (prop->type == PROPTYPE_CHR) {
		snprintf(buf, size, "c%d", (s32)(prop - g_Vars.props));
	} else {
		snprintf(buf, size, "o%d", (s32)(prop - g_Vars.props));
	}
}

void netLagCompAimTrace(void)
{
	const s32 slot = g_Vars.currentplayernum;
	struct player *p = g_Vars.currentplayer;
	char xp[16];
	char yp[16];
	u32 cmd;
	f64 view = -1;
	s32 tg;
	s32 op;
	f32 tpos[3] = {0, 0, 0};

	if (!s_Log || !s_Debug || !p || g_NetReplaying || g_NetPass < NETPASS_TICK || !netSessionMatchActive()) {
		return;
	}

	if (g_NetMode == NETMODE_SERVER) {
		if (!netPlayersHostSlotIsRemote(slot)) {
			return;
		}

		cmd = netPlayersHostLastPlayed(slot);
	} else if (g_NetMode == NETMODE_CLIENT && slot == g_NetLocalSlot) {
		cmd = g_NetTick;
		netPuppetsViewTick(&view, NULL);
	} else {
		return;
	}

	netLcPropName(p->autoxaimprop, xp, sizeof(xp));
	netLcPropName(p->autoyaimprop, yp, sizeof(yp));

	// the last other player's place as this machine has it (the harness's target)
	for (tg = PLAYERCOUNT() - 1; tg >= 0; tg--) {
		if (tg != slot && g_Vars.players[tg] && g_Vars.players[tg]->prop) {
			tpos[0] = g_Vars.players[tg]->prop->pos.x;
			tpos[1] = g_Vars.players[tg]->prop->pos.y;
			tpos[2] = g_Vars.players[tg]->prop->pos.z;
			break;
		}
	}

	// the lock test's inputs for every other player on this screen, as autoaimTick would see them
	for (op = 0; op < PLAYERCOUNT(); op++) {
		struct prop *tp = op != slot && g_Vars.players[op] ? g_Vars.players[op]->prop : NULL;
		struct model *tm = tp && tp->chr ? tp->chr->model : NULL;
		struct coord c = {0, 0, 0};
		f32 ex[2] = {0, 0};
		f32 ey[2] = {0, 0};
		f32 aim[2] = {0, 0};
		s32 vis = 0;
		f32 res = -9;

		if (tp && tm && tm->matrices && tm->definition && tm->definition->nummatrices >= 2 && (tp->flags & PROPFLAG_ONTHISSCREENTHISTICK) && g_NetTick > 0) {
			vis = chrCalculateAutoAim(tp, &c, ex, ey) ? 1 : 0;

			if (vis) {
				res = func0f06438c(tp, &c, ex, ey, aim, false, false, 0);
			}

			fprintf(s_Log, "M %u cmd %u tg %d flags %x m0 %.2f %.2f %.2f m1 %.2f %.2f %.2f vis %d c %.2f %.2f %.2f ex %.2f %.2f ey %.2f %.2f res %.3f aim %.2f %.2f cam %.1f %.1f %.1f look %.4f %.4f %.4f\n",
					g_NetTick, cmd, op, tp->flags & PROPFLAG_ONTHISSCREENTHISTICK ? 1 : 0,
					tm->matrices[0].m[3][0], tm->matrices[0].m[3][1], tm->matrices[0].m[3][2],
					tm->matrices[1].m[3][0], tm->matrices[1].m[3][1], tm->matrices[1].m[3][2],
					vis, c.x, c.y, c.z, ex[0], ex[1], ey[0], ey[1], res, aim[0], aim[1],
					p->cam_pos.x, p->cam_pos.y, p->cam_pos.z, p->cam_look.x, p->cam_look.y, p->cam_look.z);
		}
	}

	fprintf(s_Log, "A %u slot %d cmd %u view %.2f wpn %d en %d%d ins %d prop %s %s ax %.4f ay %.4f t %d %d damp %.4f cross %.2f %.2f hand %.2f %.2f sum %.4f %.4f cd %.4f swv %.3f %.3f scr %d th %.3f vt %.3f tg %d %.1f %.1f %.1f\n",
			g_NetTick, slot, cmd, view, bgunGetWeaponNum(HAND_RIGHT),
			bmoveIsAutoAimXEnabledForCurrentWeapon() ? 1 : 0, bmoveIsAutoAimYEnabledForCurrentWeapon() ? 1 : 0,
			p->insightaimmode ? 1 : 0, xp, yp, p->autoaimx, p->autoaimy, p->autoxaimtime60, p->autoyaimtime60, p->autoaimdamp,
			p->crosspos[0], p->crosspos[1], p->hands[0].crosspos[0], p->hands[0].crosspos[1],
			p->crosspossum[0], p->crosspossum[1], p->guncrossdamp, p->swivelpos[0], p->swivelpos[1],
			g_Vars.numonscreenprops, p->vv_theta, p->vv_verta, tg, tpos[0], tpos[1], tpos[2]);
}
