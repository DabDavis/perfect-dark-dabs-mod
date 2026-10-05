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
#include "lib/model.h"
#include "lib/memp.h"
#include "lib/anim.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/body.h"
#include "game/bg.h"
#include "game/footstep.h"
#include "game/game_0b0fd0.h"
#include "game/gunfx.h"
#include "game/timing.h"
#include "game/modelmgr.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/propsnd.h"
#include "game/setuputils.h"
#include "game/smoke.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "netint.h"

/**
 * Puppets on the client (PLANS/netplay/spec-entities.md §2, §3, §4).
 *
 * In a match's stage the client's world is the host's. The gates C1-C14 keep
 * this machine's own simulation off every prop but its player (no AI, no
 * anim root motion, no object ticks, damage, drops, pickups, projectiles or
 * scenarios), and the pose step below, run once a tick before chraTickBg(),
 * writes the snapshots into the props:
 *
 *   - the render clock runs about two snapshot intervals plus the arrival
 *     jitter behind the newest snapshot; the pose interpolates between the
 *     two snapshots around it (position, yaw, aim, anim frames stepped), and
 *     with none after it extrapolates positions at most 100 ms;
 *   - only ids present in the snapshot posed are applied, and only through
 *     a mapping whose local generation still holds (netEntsMapped) or a prop
 *     this file made for the id itself;
 *   - sims (mapped by bot config slot) and other machines' players (by
 *     mpindex) are chrs posed modghost's way, with the anim fields, the
 *     chrinfo root and yaw, the aim, rooms, render state, held items and
 *     muzzle flash; a sim stays hidden and disabled until its first record;
 *   - dropped weapons, projectiles, ammo crates, hats and Mod.Bodies'
 *     corpses are made from their descriptors and taken away when their id
 *     leaves the snapshot;
 *   - doors (frac, tiles, portals, the sounds of their mode changes), lifts,
 *     regenerating pickups (GONE and back, with the regen sound), moved
 *     paused props unpaused, projectile trails made here.
 *
 * Everything this file keeps is a side table keyed by host id or local prop
 * index; nothing is added to chrdata, prop or aibot.
 */

s32 g_NetClientWorld = 0;

#define NETPUP_EXTRAPMAX   6.f   // ticks: 100 ms at most past the newest snapshot
#define NETPUP_MAXDELAY    40.f  // ticks the render clock may sit behind
#define NETPUP_WRAPFRAMES  0.5f  // an anim frame step this share of the anim's length is a wrap

// per host id
struct netpup {
	struct prop *dyn;    // a prop this machine made for the id (dynamic kinds)
	u16 dyngen;          // its local generation then
	u16 hostgen;         // the host generation it was made for
	u32 posed;           // the pose serial it was last posed in
	u8 teleports;
	u8 havetele;
	s8 doormode;
	u8 gone;
	u8 havestate;
	u8 heldfail[2];      // a held weapon number that could not be made (0 none)
	u16 hatfail;
	u8 failkind;         // a descriptor that could not be made, by kind
	f32 lastpos[3];
	f32 lastfrac;        // a door's or lift's last frac/level
	u8 lastb[NETREC_MAX]; // the records last applied (unchanged statics are skipped)
	u8 lasta[NETREC_MAX];
	f32 lastt;
	s32 smoketimer240;   // a projectile's trail, at projectileTick's interval
};

static struct netpup *s_Pup = NULL;
static s32 s_PupMax = 0;

// per local prop index: a chr puppet that has had its first record
static u8 *s_Live = NULL;
static u16 *s_LiveGen = NULL;
static s32 s_LiveMax = 0;

// the render clock
static f64 s_Off = 0;     // g_NetTick - hosttick when a snapshot arrives, smoothed
static f64 s_Jit = 0;     // its mean deviation
static s32 s_HaveOff = 0;
static s32 s_Rate = 2;
static u32 s_Serial = 0;

// counts
static u32 s_Poses = 0;
static u32 s_PlayerDeaths = 0; // player puppets marked dead from their records
static u32 s_Interp = 0;
static u32 s_Extrap = 0;
static u32 s_Held = 0;       // no snapshot after and past the extrapolation limit
static u32 s_Behind = 0;     // the render tick before every snapshot held
static u32 s_Created[NETDESC_COUNT];
static u32 s_CreateFail = 0;
static u32 s_Freed = 0;
static u32 s_Stolen = 0;     // a made prop this machine's own weaponCreate took back
static u32 s_NoDesc = 0;
static u32 s_HeldSwaps = 0;
static u32 s_Snaps = 0;      // teleport counters seen moving
static u32 s_DoorMoves = 0;
static u32 s_DoorSounds = 0;
static u32 s_Regens = 0;
static u32 s_Unpaused = 0;
static u32 s_Trails = 0;
static u32 s_LiftMoves = 0;  // lift records that moved a lift
static u32 s_HatsWorn = 0;   // hats put on a chr from its record
static u32 s_GeGunsHeld = 0; // GoldenEye guns put in a chr's hand from its record
static u32 s_HeldFails = 0;  // held guns that could not be made (no model)
static u32 s_BodyLoads = 0;  // corpses whose body and head no chr here wore (not made)
static u32 s_GeGunsMade = 0; // GoldenEye guns made from descriptors (dropped, thrown, a pickup)
static u32 s_GoldenGuns = 0; // ... of them the Golden Gun (its scenario's one gun)
static u32 s_GoldenHands = 0;  // the Golden Gun put in a puppet's hand
static u32 s_GoldenHolders = 0; // ... by how many holders in turn (it passes on with a death)
static struct chrdata *s_GoldenHolder = NULL;
static u32 s_Deaths = 0;
static u32 s_FirstRecords = 0;
static u32 s_BadAnims = 0;
static u32 s_Resyncs = 0;
static f32 s_DelayLast = 0;

static u32 s_TraceEvery = 0; // --net-puppet-trace N: every N ticks, each puppet's pose
static FILE *s_TraceFile = NULL;

#define NET_TRACEQ 512

struct nettraceq {
	f64 rt;
	struct prop *prop;
	u16 id;
	s16 kind;
	u8 type;
};

static struct nettraceq s_TraceQ[NET_TRACEQ];
static s32 s_TraceQN = 0;


static s32 netPupLocalIndex(const struct prop *prop)
{
	return netEntsPropIndex(prop);
}

static s32 netPupIsLive(struct prop *prop)
{
	const s32 idx = netPupLocalIndex(prop);

	return idx >= 0 && idx < s_LiveMax && s_Live[idx] && s_LiveGen[idx] == netEntsPropGen(idx);
}

static void netPupSetLive(struct prop *prop)
{
	const s32 idx = netPupLocalIndex(prop);

	if (idx >= 0 && idx < s_LiveMax) {
		s_Live[idx] = 1;
		s_LiveGen[idx] = netEntsPropGen(idx);
	}
}

static struct prop *netLocalPlayerProp(void)
{
	if (g_NetLocalSlot >= 0 && g_NetLocalSlot < PLAYERCOUNT() && g_Vars.players[g_NetLocalSlot]) {
		return g_Vars.players[g_NetLocalSlot]->prop;
	}

	return NULL;
}

s32 netIsPuppet(struct prop *prop)
{
	if (!g_NetClientWorld || !prop) {
		return 0;
	}

	if (prop->type == PROPTYPE_CHR) {
		return 1;
	}

	if (prop->type == PROPTYPE_PLAYER) {
		return prop != netLocalPlayerProp() && netPupIsLive(prop);
	}

	return 0;
}

/*
 * Stage
 */

void netPuppetsStop(void)
{
	free(s_Pup);
	free(s_Live);
	free(s_LiveGen);
	s_Pup = NULL;
	s_Live = NULL;
	s_LiveGen = NULL;
	s_PupMax = 0;
	s_LiveMax = 0;
	s_HaveOff = 0;

	s_TraceQN = 0;

	if (s_TraceFile) {
		fclose(s_TraceFile);
		s_TraceFile = NULL;
	}
}

void netPuppetsStageStart(void)
{
	const char *trace = sysArgGetString("--net-puppet-trace");
	s32 i;

	netPuppetsStop();

	s_LiveMax = g_Vars.maxprops;
	s_Live = calloc(s_LiveMax, 1);
	s_LiveGen = calloc(s_LiveMax, sizeof(u16));

	if (!s_Live || !s_LiveGen) {
		netPuppetsStop();
		return;
	}

	s_Serial = 0;
	s_Poses = s_Interp = s_Extrap = s_Held = s_Behind = 0;
	s_CreateFail = s_Freed = s_Stolen = s_NoDesc = s_HeldSwaps = s_Snaps = 0;
	s_DoorMoves = s_DoorSounds = s_Regens = s_Unpaused = s_Trails = s_Deaths = s_FirstRecords = s_BadAnims = s_Resyncs = 0;
	s_PlayerDeaths = 0;
	s_LiftMoves = s_HatsWorn = s_GeGunsHeld = s_BodyLoads = s_GeGunsMade = s_GoldenGuns = s_HeldFails = 0;
	s_GoldenHands = s_GoldenHolders = 0;
	s_GoldenHolder = NULL;
	memset(s_Created, 0, sizeof(s_Created));

	// --net-puppet-trace FILE[,N]: what each puppet is posed with, every N ticks
	if (trace) {
		char path[512];
		const char *comma = strrchr(trace, ',');

		snprintf(path, sizeof(path), "%s", trace);
		s_TraceEvery = 10;

		if (comma) {
			path[comma - trace] = '\0';
			s_TraceEvery = (u32)atoi(comma + 1);
		}

		if (s_TraceEvery < 1) {
			s_TraceEvery = 1;
		}

		s_TraceFile = fopen(path, "w");
	}

	// The sims setup made are hidden and still until the host's first record
	// for each says where it is (no AI here ever spawns them)
	for (i = 0; i < g_BotCount && i < MAX_BOTS; i++) {
		struct chrdata *chr = g_MpBotChrPtrs[i];

		if (chr && chr->prop) {
			chr->chrflags |= CHRCFLAG_HIDDEN;
			propDisable(chr->prop);
		}
	}
}

/*
 * The render clock
 */

void netPuppetsOnSnap(u32 hosttick, s32 rate)
{
	const f64 off = (f64)(s32)(g_NetTick - hosttick);

	if (rate >= 1 && rate <= 6) {
		s_Rate = rate;
	}

	if (!s_HaveOff || fabs(off - s_Off) > 60) {
		// the first, or the clocks jumped (a long stall): start over
		if (s_HaveOff) {
			s_Resyncs++;
		}

		s_Off = off;
		s_Jit = 1;
		s_HaveOff = 1;
		return;
	}

	// a snapshot early pulls the clock quickly, a late one slowly: the
	// newest seen bounds how far ahead the render may go. One stall (a
	// hitch on either machine) counts as a few ticks of jitter, not its length.
	{
		const f64 dev = fabs(off - s_Off);

		s_Jit += ((dev < 6 ? dev : 6) - s_Jit) * 0.05;
		s_Off += (off - s_Off) * (off < s_Off ? 0.1 : 0.03);
	}
}

static f32 netPupDelay(void)
{
	f32 delay = 2.f * s_Rate + 2.f * (f32)s_Jit + 1.f;

	if (delay > NETPUP_MAXDELAY) {
		delay = NETPUP_MAXDELAY;
	}

	return delay;
}

/**
 * The host tick (and fraction) this tick's pose step draws the puppets at:
 * the same sum netClientPosePuppetsRun makes, which the tick's command
 * carries for the host's rewind (netlagcomp.c)
 */
s32 netPuppetsViewTick(f64 *view, f32 *delay)
{
	f64 rt;
	f32 d;

	if (!g_NetClientWorld || !s_HaveOff) {
		return 0;
	}

	d = netPupDelay();
	rt = (f64)g_NetTick - s_Off - d;

	if (delay) {
		*delay = d;
	}

	if (rt < 0) {
		return 0;
	}

	*view = rt;

	return 1;
}

/*
 * Snapshot lookups
 */

static const struct netbaselineslot *netPupSlot(const struct netsnapclient *c, u16 seq)
{
	const struct netbaselineslot *slot;

	if (!seq) {
		return NULL;
	}

	slot = &c->bl.slots[seq % NETBASELINE_SLOTS];

	return slot->valid && slot->seq == seq ? slot : NULL;
}

static const u8 *netPupFind(const struct netbaselineslot *slot, u16 id, u16 gen, s32 rec)
{
	s32 lo = 0;
	s32 hi;

	if (!slot) {
		return NULL;
	}

	hi = slot->count - 1;

	while (lo <= hi) {
		const s32 mid = (lo + hi) / 2;

		if (slot->ids[mid] == id) {
			const u8 *store = slot->records + (size_t)mid * NETSNAP_STORE;

			return netStoreGen(store) == gen && netStoreRec(store) == rec ? store : NULL;
		}

		if (slot->ids[mid] < id) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	return NULL;
}

/*
 * Interpolation
 */

static f32 netAngWrap(f32 a)
{
	while (a >= M_BADTAU) {
		a -= M_BADTAU;
	}

	while (a < 0) {
		a += M_BADTAU;
	}

	return a;
}

static f32 netAngLerp(f32 a, f32 b, f32 t)
{
	f32 d = b - a;

	while (d > M_PI) {
		d -= M_BADTAU;
	}

	while (d < -M_PI) {
		d += M_BADTAU;
	}

	return a + d * t;
}

static f32 netFrameLerp(s16 animnum, f32 fa, f32 fb, f32 t, s32 rev)
{
	f32 n;
	f32 d;

	if (animnum <= 0 || animnum >= g_NumAnimations) {
		return t < 0.5f ? fa : fb;
	}

	n = (f32)animGetNumFrames(animnum);
	d = fb - fa;

	// a looped cycle that wrapped between the two snapshots
	if (n > 1 && fabsf(d) > n * NETPUP_WRAPFRAMES) {
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

static void netQuatNlerp(const f32 *a, const f32 *b, f32 t, f32 *out)
{
	f32 sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.f : 1.f;
	f32 len;
	s32 i;

	for (i = 0; i < 4; i++) {
		out[i] = a[i] + (b[i] * sign - a[i]) * t;
	}

	len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);

	if (len < 0.0001f) {
		memcpy(out, a, sizeof(f32) * 4);
		return;
	}

	for (i = 0; i < 4; i++) {
		out[i] /= len;
	}
}

/**
 * The state at the render tick: b's, moved towards a's by t, or past b by
 * dt ticks at the speed from p to b (extrapolated). A teleport between the
 * two, or a different anim, is a step, not a blend.
 */
static void netPupBlend(s32 rec, const struct netentstate *sb, const struct netentstate *sa, f32 t,
		const struct netentstate *sp, f32 ptick, f32 dt, struct netentstate *out)
{
	s32 i;

	*out = *sb;

	if (sa && t > 0) {
		if (rec == NETREC_CHR && sa->teleports != sb->teleports) {
			if (t >= 1) {
				*out = *sa;
			}

			return;
		}

		for (i = 0; i < 3; i++) {
			out->pos[i] = sb->pos[i] + (sa->pos[i] - sb->pos[i]) * t;
		}

		switch (rec) {
		case NETREC_CHR:
			out->yaw = netAngWrap(netAngLerp(sb->yaw, sa->yaw, t));
			out->rooty = sb->rooty + (sa->rooty - sb->rooty) * t;
			out->groundy = sb->groundy + (sa->groundy - sb->groundy) * t;

			for (i = 0; i < 4; i++) {
				out->aim[i] = netAngLerp(sb->aim[i], sa->aim[i], t);
			}

			if (sa->animnum == sb->animnum && ((sa->animflags ^ sb->animflags) & (NETANIM_FLIP | NETANIM_REV)) == 0) {
				out->frame = netFrameLerp(sb->animnum, sb->frame, sa->frame, t, (sb->animflags & NETANIM_REV) != 0);
			} else if (t >= 0.5f) {
				out->animnum = sa->animnum;
				out->animflags = sa->animflags;
				out->frame = sa->frame;
				out->animnum2 = sa->animnum2;
				out->frame2 = sa->frame2;
				out->fracmerge = sa->fracmerge;
				return;
			}

			if (sa->animnum2 == sb->animnum2 && sb->animnum2 && ((sa->animflags ^ sb->animflags) & (NETANIM_FLIP2 | NETANIM_REV2 | NETANIM_HAS2)) == 0) {
				out->frame2 = netFrameLerp(sb->animnum2, sb->frame2, sa->frame2, t, (sb->animflags & NETANIM_REV2) != 0);
			}

			out->fracmerge = sb->fracmerge + (sa->fracmerge - sb->fracmerge) * t;
			out->cshield = sb->cshield + (sa->cshield - sb->cshield) * t;
			break;
		case NETREC_OBJ:
			netQuatNlerp(sb->quat, sa->quat, t, out->quat);
			break;
		case NETREC_DOOR:
			out->doorfrac = sb->doorfrac + (sa->doorfrac - sb->doorfrac) * t;
			break;
		default:
			break;
		}

		return;
	}

	// past the newest: positions carried on at the last speed, briefly
	if (sp && ptick > 0 && dt > 0 && (rec != NETREC_CHR || sp->teleports == sb->teleports)) {
		for (i = 0; i < 3; i++) {
			const f32 v = (sb->pos[i] - sp->pos[i]) / ptick;

			out->pos[i] = sb->pos[i] + v * dt;
		}
	}
}

/*
 * Chrs: sims, other machines' players, (later) bodies
 */

static void netPupHeld(struct netpup *u, struct chrdata *chr, s32 hand, u8 want)
{
	struct prop *held = chr->weapons_held[hand];
	u8 have = 0xff;

	if (held && held->obj && held->obj->type == OBJTYPE_WEAPON && held->weapon) {
		have = held->weapon->weaponnum;
	}

	if (have == want || (want != 0xff && u->heldfail[hand] == want)) {
		return;
	}

	if (held) {
		objDetach(held);

		if (held->obj) {
			objFreePermanently(held->obj, true);
		}

		chr->weapons_held[hand] = NULL;
	}

	s_HeldSwaps++;
	u->heldfail[hand] = 0;

	if (want != 0xff && want > 0 && want < NUM_WEAPONS) {
		s32 modelnum = playermgrGetModelOfWeapon(want);

		if (hand == HAND_LEFT && weaponHasFlag2(want, WEAPONFLAG2_DETONATORHAND)) {
			modelnum = -1;
		}

		if (modelnum < 0 || modelnum >= NUM_MODELS
				|| !weaponCreateForChr(chr, modelnum, want, hand == HAND_LEFT ? OBJFLAG_WEAPON_LEFTHANDED : 0, NULL, NULL)) {
			u->heldfail[hand] = want;
			s_HeldFails += modelnum >= 0;
		} else if (WEAPON_IS_GE(want)) {
			s_GeGunsHeld++;

			if (want == WEAPON_GE_GOLDENGUN) {
				s_GoldenHands++;
				s_GoldenHolders += chr != s_GoldenHolder;
				s_GoldenHolder = chr;
			}
		}
	}
}

static void netPupHat(struct netpup *u, struct chrdata *chr, u16 want)
{
	struct prop *hat = chr->weapons_held[2];
	u16 have = hat && hat->obj ? (u16)hat->obj->modelnum : 0xffff;

	if (have == want || (want != 0xffff && u->hatfail == want)) {
		return;
	}

	if (hat) {
		objDetach(hat);

		if (hat->obj) {
			objFreePermanently(hat->obj, true);
		}

		chr->weapons_held[2] = NULL;
	}

	u->hatfail = 0;

	if (want != 0xffff && want < NUM_MODELS) {
		if (hatCreateForChr(chr, want, 0)) {
			s_HatsWorn++;
		} else {
			u->hatfail = want;
		}
	}
}

static void netPupSeedRooms(struct prop *prop, s32 room);

static void netPupChr(struct netpup *u, struct prop *prop, s32 kind, const struct netentstate *s, s32 snapped)
{
	struct chrdata *chr = prop->chr;
	struct model *model;
	struct anim *anim;
	s32 life = s->flags & NETCHR_LIFEMASK;
	s32 isplayer = prop->type == PROPTYPE_PLAYER;
	s32 first;

	if (!chr || !(model = chr->model) || !(anim = model->anim) || !model->definition || !model->definition->rootnode) {
		return;
	}

	// an anim this build has not got is no anim: the last good one stands
	if (s->animnum < 0 || s->animnum >= g_NumAnimations
			|| ((s->animflags & NETANIM_HAS2) && (s->animnum2 <= 0 || s->animnum2 >= g_NumAnimations))) {
		s_BadAnims++;
		return;
	}

	first = !netPupIsLive(prop);

	if (first) {
		netPupSetLive(prop);
		s_FirstRecords++;
		snapped = 1;
	}

	// life: a sim's action follows the record (a player's body plays the
	// host's anim and keeps its own action; its player's isdead, which
	// chrIsDead, targeting and collisions read, follows the record)
	if (isplayer) {
		const s32 pn = playermgrGetPlayerNumByProp(prop);

		if (pn >= 0 && pn < PLAYERCOUNT() && pn != g_NetLocalSlot && g_Vars.players[pn]) {
			struct player *player = g_Vars.players[pn];

			if (life != 0 && !player->isdead) {
				player->isdead = true;
				// never ticked here: nothing would finish these
				player->redbloodfinished = true;
				player->deathanimfinished = true;
				s_PlayerDeaths++;
			} else if (life == 0 && player->isdead) {
				player->isdead = false;
			}
		}
	} else {
		if (life == 0 && chr->actiontype != ACT_BONDMULTI) {
			chr->actiontype = ACT_BONDMULTI;
			memset(&chr->act_bondmulti, 0, sizeof(chr->act_bondmulti));
			chr->act_bondmulti.animcfg = NULL;
		} else if (life == 1 && chr->actiontype != ACT_DIE) {
			chr->actiontype = ACT_DIE;
			memset(&chr->act_die, 0, sizeof(chr->act_die));
			chr->act_die.thudframe1 = -1;
			chr->act_die.thudframe2 = -1;
			s_Deaths++;
		} else if (life == 2 && chr->actiontype != ACT_DEAD) {
			chr->actiontype = ACT_DEAD;
			memset(&chr->act_dead, 0, sizeof(chr->act_dead));
		}
	}

	// flags
	if (s->flags & NETCHR_HIDDEN) {
		chr->chrflags |= CHRCFLAG_HIDDEN;
	} else {
		chr->chrflags &= ~CHRCFLAG_HIDDEN;
	}

	if (s->flags & NETCHR_PERIMOFF) {
		chr->chrflags |= CHRCFLAG_PERIMDISABLEDTMP;
	} else {
		chr->chrflags &= ~CHRCFLAG_PERIMDISABLEDTMP;
	}

	if (s->flags & NETCHR_AUTOANIM) {
		chr->hidden2 |= CHRH2FLAG_AUTOANIM;
	} else {
		chr->hidden2 &= ~CHRH2FLAG_AUTOANIM;
	}

	if (s->flags & NETCHR_CLOAKED) {
		chr->hidden |= CHRHFLAG_CLOAKED;
	} else {
		chr->hidden &= ~CHRHFLAG_CLOAKED;
	}

	chr->onladder = (s->flags & NETCHR_LADDER) != 0;

	if (!isplayer) {
		if ((s->flags & NETCHR_ENABLED) && (prop->flags & PROPFLAG_ENABLED) == 0) {
			propEnable(prop);
		} else if ((s->flags & NETCHR_ENABLED) == 0 && (prop->flags & PROPFLAG_ENABLED)) {
			propDisable(prop);
		}
	}

	// position and rooms: bgFindEnteredRooms() floods out from the rooms
	// the prop has, so the host's first room seeds it (a sim this machine
	// never spawned has none at all, and a teleport leaves them behind)
	prop->pos.x = s->pos[0];
	prop->pos.y = s->pos[1];
	prop->pos.z = s->pos[2];
	netPupSeedRooms(prop, s->room);
	chr0f0220ac(chr);

	chr->ground = s->pos[1] + s->groundy;
	chr->manground = chr->ground;
	chr->sumground = chr->manground * (PAL ? 8.417509f : 9.999998f);

	// the anim (the frames load at the matrix build; speed at most 0.5 so a
	// build with several players keeps the frac, and only its sign is read)
	anim->animnum = s->animnum;
	anim->flip = (s->animflags & NETANIM_FLIP) != 0;
	anim->looping = (s->animflags & NETANIM_LOOP) != 0;
	anim->endframe = -1;
	anim->speed = (s->animflags & NETANIM_REV) ? -0.5f : 0.5f;

	if (s->animflags & NETANIM_HAS2) {
		anim->animnum2 = s->animnum2;
		anim->flip2 = (s->animflags & NETANIM_FLIP2) != 0;
		anim->endframe2 = -1;
		anim->speed2 = (s->animflags & NETANIM_REV2) ? -0.5f : 0.5f;
		anim->fracmerge = s->fracmerge;
	} else {
		anim->animnum2 = 0;
		anim->fracmerge = 0;
		anim->frac2 = 0;
	}

	if (anim->animnum) {
		modelSetAnimFrame2(model, s->frame, s->frame2);
	}

	// the root: where the body stands and which way it faces
	if ((model->definition->rootnode->type & 0xff) == MODELNODETYPE_CHRINFO) {
		struct modelrwdata_chrinfo *rw = modelGetNodeRwData(model, model->definition->rootnode);

		rw->pos.x = s->pos[0];
		rw->pos.y = s->pos[1] + s->rooty;
		rw->pos.z = s->pos[2];
		rw->ground = chr->ground;
		rw->unk18 = 0;

		if (snapped) {
			rw->unk20 = rw->unk30 = rw->yrot = s->yaw;
		}
	}

	modelSetChrRotY(model, s->yaw);

	if (chr->aibot) {
		chr->aibot->lookangle = s->yaw;
		chr->aibot->angleoffset = 0;
		chr->aibot->fadeintimer60 = s->fadeintimer;
	} else if (isplayer) {
		const s32 pn = playermgrGetPlayerNumByProp(prop);

		if (pn >= 0 && pn < PLAYERCOUNT()) {
			g_Vars.players[pn]->angleoffset = 0;
		}
	}

	// the aim (angleoffset is folded into the waist by the host)
	chr->aimendlshoulder = s->aim[0];
	chr->aimendrshoulder = s->aim[1];
	chr->aimendback = s->aim[2];
	chr->aimendsideback = s->aim[3];
	chr->aimendcount = 0;

	if (snapped) {
		chr->aimuplshoulder = s->aim[0];
		chr->aimuprshoulder = s->aim[1];
		chr->aimupback = s->aim[2];
		chr->aimsideback = s->aim[3];
	}

	// render state
	chr->fadealpha = s->fadealpha;
	chr->cloakfadefrac = s->cloakfrac & 0x7f;
	chr->cloakfadefinished = (s->cloakfrac & 0x80) != 0;
	chr->cshield = s->cshield;
	chr->drugheadsway = s->drugheadsway;

	// held items, then the flash (chrSetFiring reads the held prop and sets
	// forcetick: both hands every pose, never the FIRING flags)
	netPupHeld(u, chr, HAND_RIGHT, s->weapon[0]);
	netPupHeld(u, chr, HAND_LEFT, s->weapon[1]);

	netPupHat(u, chr, s->hat);
	chrSetFiring(chr, HAND_LEFT, (s->flags & NETCHR_FIRINGL) != 0);
	chrSetFiring(chr, HAND_RIGHT, (s->flags & NETCHR_FIRINGR) != 0);

	// a sim's footfalls (stock plays none with more than one player)
	if (!isplayer && life == 0) {
		footstepCheckDefault(chr);
	}
}

/*
 * Objects
 */

static void netQuatToRot(const f32 *q, f32 scale, f32 m[3][3])
{
	const f32 x = q[0], y = q[1], z = q[2], w = q[3];

	// column-major as the game's Mtx3 (m[col][row]), the inverse of netQuatFromMatrix
	m[0][0] = (1 - 2 * (y * y + z * z)) * scale;
	m[0][1] = (2 * (x * y + z * w)) * scale;
	m[0][2] = (2 * (x * z - y * w)) * scale;
	m[1][0] = (2 * (x * y - z * w)) * scale;
	m[1][1] = (1 - 2 * (x * x + z * z)) * scale;
	m[1][2] = (2 * (y * z + x * w)) * scale;
	m[2][0] = (2 * (x * z + y * w)) * scale;
	m[2][1] = (2 * (y * z - x * w)) * scale;
	m[2][2] = (1 - 2 * (x * x + y * y)) * scale;
}

static f32 netRotScale(f32 m[3][3])
{
	const f32 len = sqrtf(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);

	return len > 0.000001f && len < 100000.f ? len : 1.f;
}

static void netPupSeedRooms(struct prop *prop, s32 room)
{
	if (room > 0 && room < g_Vars.roomcount) {
		propDeregisterRooms(prop);
		prop->rooms[0] = room;
		prop->rooms[1] = -1;
	}
}

// in the paused list (propUnpause() takes only those: an active one would
// be unlinked from the active list's ends without them being moved)
static s32 netPupIsPaused(struct prop *prop)
{
	return !prop->active && (prop->prev || prop->next || prop == g_Vars.pausedprops);
}

static void netPupPlaceObj(struct defaultobj *obj, const struct netentstate *s, f32 scale)
{
	struct prop *prop = obj->prop;

	netQuatToRot(s->quat, scale, obj->realrot);
	prop->pos.x = s->pos[0];
	prop->pos.y = s->pos[1];
	prop->pos.z = s->pos[2];

	// rooms flooded out by the bbox from the host's first (setup0f0923d4),
	// registered; shading, geometry
	netPupSeedRooms(prop, s->room);
	func0f069c70(obj, true, true);
}

static void netPupObj(struct netpup *u, struct prop *prop, const struct netentstate *s, s32 isdyn)
{
	struct defaultobj *obj = prop->obj;
	const s32 gone = (s->flags & NETOBJ_GONE) != 0;
	s32 moved;

	if (!obj) {
		return;
	}

	moved = !u->havestate || u->lastpos[0] != s->pos[0] || u->lastpos[1] != s->pos[1] || u->lastpos[2] != s->pos[2];

	// a regenerating pickup: gone and back (its regen sound is derived here)
	if (gone && (obj->hidden & OBJHFLAG_GONE) == 0) {
		obj->hidden |= OBJHFLAG_GONE;
	} else if (!gone && (obj->hidden & OBJHFLAG_GONE)) {
		obj->hidden &= ~OBJHFLAG_GONE;
		prop->timetoregen = 0;

		if (u->havestate) {
			psCreate(NULL, prop, SFX_REGEN, -1, -1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
			s_Regens++;
		}

		moved = 1;
	}

	if ((s->flags & NETOBJ_ENABLED) && (prop->flags & PROPFLAG_ENABLED) == 0) {
		propEnable(prop);
		moved = 1;
	} else if ((s->flags & NETOBJ_ENABLED) == 0 && (prop->flags & PROPFLAG_ENABLED)) {
		propDisable(prop);
	}

	if (s->flags & NETOBJ_INVISIBLE) {
		obj->flags2 |= OBJFLAG2_INVISIBLE;
	} else {
		obj->flags2 &= ~OBJFLAG2_INVISIBLE;
	}

	if (s->damage >= 0) {
		obj->damage = s->damage;
	}

	if (moved || isdyn) {
		netPupPlaceObj(obj, s, isdyn ? obj->model->scale : netRotScale(obj->realrot));

		// rooms unpause their props only when they come on screen: one moved
		// into a room already in view would never draw
		if (moved && !gone && netPupIsPaused(prop)) {
			propUnpause(prop);
			s_Unpaused++;
		}

		// a projectile's trail, which its tick would have made
		if (moved && u->havestate && obj->type == OBJTYPE_WEAPON && prop->weapon
				&& ((s->flags & NETOBJ_PROJECTILE) || isdyn)) {
			const s32 host = weaponHost(prop->weapon->weaponnum);
			const f32 dx = s->pos[0] - u->lastpos[0];
			const f32 dz = s->pos[2] - u->lastpos[2];
			const f32 dy = s->pos[1] - u->lastpos[1];

			// counted on tick passes only, a puff each TICKS(24) like the tick's
			if (g_NetPass != NETPASS_PRESENT_ONLY) {
				u->smoketimer240 -= g_Vars.lvupdate240;
			}

			if (dx * dx + dy * dy + dz * dz > 0.01f && g_NetPass != NETPASS_PRESENT_ONLY && u->smoketimer240 <= 0) {
				u->smoketimer240 = TICKS(24);

				if (host == WEAPON_ROCKETLAUNCHER || host == WEAPON_ROCKETLAUNCHER_34) {
					smokeCreateSimple(&prop->pos, prop->rooms, SMOKETYPE_ROCKETTAIL);
					s_Trails++;
				} else if (host == WEAPON_HOMINGROCKET) {
					smokeCreateSimple(&prop->pos, prop->rooms, SMOKETYPE_HOMINGTAIL);
					s_Trails++;
				} else if (host == WEAPON_GRENADEROUND) {
					smokeCreateSimple(&prop->pos, prop->rooms, SMOKETYPE_GRENADETAIL);
					s_Trails++;
				}
			}
		}
	}

	u->lastpos[0] = s->pos[0];
	u->lastpos[1] = s->pos[1];
	u->lastpos[2] = s->pos[2];
	u->gone = gone;
	u->havestate = 1;
}

static void netPupDoor(struct netpup *u, struct prop *prop, const struct netentstate *s)
{
	struct doorobj *door = (struct doorobj *)prop->obj;
	struct doorobj *loop;
	f32 frac;
	s32 shut;

	if (!door || door->base.type != OBJTYPE_DOOR || prop->type != PROPTYPE_DOOR) {
		return;
	}

	frac = s->doorfrac * door->maxfrac;

	// the sounds a door makes as its mode changes, as doorTick makes them
	if (u->havestate && s->doormode != u->doormode) {
		if (s->doormode == DOORMODE_OPENING) {
			doorPlayOpeningSound(door->soundtype, prop);
			s_DoorSounds++;
		} else if (s->doormode == DOORMODE_CLOSING) {
			doorPlayClosingSound(door->soundtype, prop);
			s_DoorSounds++;
		} else if (s->doormode == DOORMODE_IDLE && u->doormode == DOORMODE_OPENING) {
			doorPlayOpenedSound(door->soundtype, prop);
			s_DoorSounds++;
		} else if (s->doormode == DOORMODE_IDLE && u->doormode == DOORMODE_CLOSING) {
			doorPlayClosedSound(door->soundtype, prop);
			s_DoorSounds++;
		}
	}

	door->mode = s->doormode;
	door->laserfade = s->laserfade;

	if (!u->havestate || frac != door->frac) {
		door->frac = frac;
		doorUpdateTiles(door);
		setup0f0923d4(&door->base);
		s_DoorMoves++;

		if (frac > 0) {
			doorActivatePortal(door);
		} else {
			// shut only when every sibling sharing the portal is (doorFinishClose)
			shut = 1;
			loop = door;

			while (loop) {
				if (loop->frac > 0 && loop->portalnum == door->portalnum) {
					shut = 0;
				}

				loop = loop->sibling;

				if (loop == door) {
					break;
				}
			}

			if (shut) {
				doorDeactivatePortal(door);
			}
		}
	}

	u->doormode = s->doormode;
	u->havestate = 1;
}

static void netPupLift(struct netpup *u, struct prop *prop, const struct netentstate *s)
{
	struct liftobj *lift = (struct liftobj *)prop->obj;
	const s32 moving = (s->flags & 1) != 0;
	s32 moved;

	if (!lift || lift->base.type != OBJTYPE_LIFT) {
		return;
	}

	moved = !u->havestate || u->lastpos[0] != s->pos[0] || u->lastpos[1] != s->pos[1] || u->lastpos[2] != s->pos[2];

	if (u->havestate && moving && !u->gone) {
		doorPlayOpeningSound(lift->soundtype, prop);
	} else if (u->havestate && !moving && u->gone) {
		doorPlayOpenedSound(lift->soundtype, prop);
	}

	lift->levelcur = s->levelcur;
	lift->levelaim = s->levelaim;

	if (moved) {
		s_LiftMoves += u->havestate;
		lift->prevpos = prop->pos;
		prop->pos.x = s->pos[0];
		prop->pos.y = s->pos[1];
		prop->pos.z = s->pos[2];
		netPupSeedRooms(prop, s->room);
		func0f069c70(&lift->base, true, true);
		liftUpdateTiles(lift, lift->levelcur == lift->levelaim);
	}

	u->lastpos[0] = s->pos[0];
	u->lastpos[1] = s->pos[1];
	u->lastpos[2] = s->pos[2];
	u->gone = moving; // reused: whether it was moving
	u->havestate = 1;
}

/*
 * Props made here from descriptors
 */

static struct prop *netPupMakeWeapon(const struct netdesc *d, const struct netentstate *s)
{
	struct modeldef *modeldef;
	struct weaponobj *weapon;
	struct prop *prop;
	struct model *model;

	if (d->modelnum < 0 || d->modelnum >= NUM_MODELS || d->weaponnum >= NUM_WEAPONS
			|| g_ModelStates[d->modelnum].fileid == 0 || (setupLoadModeldef(d->modelnum), 0) || !(modeldef = g_ModelStates[d->modelnum].modeldef)) {
		return NULL;
	}

	prop = propAllocate();
	model = modelmgrInstantiateModelWithoutAnim(modeldef);
	weapon = weaponCreate(prop == NULL, model == NULL, modeldef);

	if (prop == NULL) {
		prop = propAllocate();
	}

	if (model == NULL) {
		model = modelmgrInstantiateModelWithoutAnim(modeldef);
	}

	if (!weapon || !prop || !model) {
		if (model) {
			modelmgrFreeModel(model);
		}

		if (prop) {
			propFree(prop);
		}

		if (weapon) {
			weapon->base.prop = NULL;
			weapon->base.model = NULL;
		}

		return NULL;
	}

	{
		struct weaponobj tmp = {
			256,                    // extrascale
			0,                      // hidden2
			OBJTYPE_WEAPON,         // type
			0,                      // modelnum
			-1,                     // pad
			0,                      // flags: still, the snapshots move it
			0,                      // flags2
			0,                      // flags3
			NULL,                   // prop
			NULL,                   // model
			1, 0, 0,                // realrot
			0, 1, 0,
			0, 0, 1,
			0,                      // hidden
			NULL,                   // geo
			NULL,                   // projectile
			0,                      // damage
			1000,                   // maxdamage
			0xff, 0xff, 0xff, 0x00, // shadecol
			0xff, 0xff, 0xff, 0x00, // nextcol
			0x0fff,                 // floorcol
			0,                      // tiles
			0,                      // weaponnum
			0,                      // unk5d
			0,                      // unk5e
			0,                      // gunfunc
			0,                      // fadeouttimer60
			0xff,                   // dualweaponnum
			-1,                     // timer240
			NULL,                   // dualweapon
		};

		*weapon = tmp;
	}

	weapon->weaponnum = d->weaponnum;
	weapon->gunfunc = d->gunfunc;
	weapon->base.modelnum = d->modelnum;

	prop = func0f08adc8(weapon, modeldef, prop, model);

	if (!prop) {
		return NULL;
	}

	// never OBJHFLAG_PROJECTILE here: it has no projectile struct, and
	// objTickPlayer would read one
	weapon->base.hidden &= ~(OBJHFLAG_PROJECTILE | OBJHFLAG_DELETING);
	propActivate(prop);
	propEnable(prop);
	netPupPlaceObj(&weapon->base, s, weapon->base.model->scale);

	return prop;
}

static struct prop *netPupMakeHat(const struct netdesc *d, const struct netentstate *s)
{
	struct modeldef *modeldef;
	struct hatobj *hat;
	struct prop *prop;
	struct model *model;

	if (d->modelnum < 0 || d->modelnum >= NUM_MODELS
			|| g_ModelStates[d->modelnum].fileid == 0 || (setupLoadModeldef(d->modelnum), 0) || !(modeldef = g_ModelStates[d->modelnum].modeldef)) {
		return NULL;
	}

	prop = propAllocate();
	model = modelmgrInstantiateModelWithoutAnim(modeldef);
	hat = hatCreate(prop == NULL, model == NULL, modeldef);

	if (prop == NULL) {
		prop = propAllocate();
	}

	if (model == NULL) {
		model = modelmgrInstantiateModelWithoutAnim(modeldef);
	}

	if (!hat || !prop || !model) {
		if (model) {
			modelmgrFreeModel(model);
		}

		if (prop) {
			propFree(prop);
		}

		if (hat) {
			hat->base.prop = NULL;
			hat->base.model = NULL;
		}

		return NULL;
	}

	{
		struct hatobj tmp = {
			256,                    // extrascale
			0,                      // hidden2
			OBJTYPE_HAT,            // type
			0,                      // modelnum
			-1,                     // pad
			0,                      // flags
			0,                      // flags2
			0,                      // flags3
			NULL,                   // prop
			NULL,                   // model
			1, 0, 0,                // realrot
			0, 1, 0,
			0, 0, 1,
			0,                      // hidden
			NULL,                   // geo
			NULL,                   // projectile
			0,                      // damage
			1000,                   // maxdamage
			0xff, 0xff, 0xff, 0x00, // shadecol
			0xff, 0xff, 0xff, 0x00, // nextcol
			0x0fff,                 // floorcol
			0,                      // tiles
		};

		*hat = tmp;
	}

	hat->base.modelnum = d->modelnum;
	prop = objInit(&hat->base, modeldef, prop, model);

	if (!prop) {
		return NULL;
	}

	propActivate(prop);
	propEnable(prop);
	netPupPlaceObj(&hat->base, s, hat->base.model->scale);

	return prop;
}

static struct prop *netPupMakeCrate(const struct netdesc *d, const struct netentstate *s)
{
	struct ammocrateobj *crate;
	struct prop *prop;

	if (d->modelnum < 0 || d->modelnum >= NUM_MODELS || g_ModelStates[d->modelnum].fileid == 0
			|| (setupLoadModeldef(d->modelnum), 0) || !g_ModelStates[d->modelnum].modeldef || !(crate = ammocrateAllocate())) {
		return NULL;
	}

	{
		struct defaultobj tmp = {
			256,                    // extrascale
			0,                      // hidden2
			OBJTYPE_AMMOCRATE,      // type
			0,                      // modelnum
			-1,                     // pad
			0,                      // flags
			0,                      // flags2
			0,                      // flags3
			NULL,                   // prop
			NULL,                   // model
			1, 0, 0,                // realrot
			0, 1, 0,
			0, 0, 1,
			0,                      // hidden
			NULL,                   // geo
			NULL,                   // projectile
			0,                      // damage
			1000,                   // maxdamage
			0xff, 0xff, 0xff, 0x00, // shadecol
			0xff, 0xff, 0xff, 0x00, // nextcol
			0x0fff,                 // floorcol
			0,                      // tiles
		};

		crate->base = tmp;
	}

	crate->base.modelnum = d->modelnum;
	crate->ammotype = 1;
	prop = objInitWithModelDef(&crate->base, g_ModelStates[d->modelnum].modeldef);

	if (!prop) {
		return NULL;
	}

	propActivate(prop);
	propEnable(prop);
	netPupPlaceObj(&crate->base, s, crate->base.model->scale);

	return prop;
}

/**
 * A Mod.Bodies corpse (BODY): the host hands a dead sim's prop and model to a
 * chr of its own (modbodies.c) and the sim gets up in a fresh pair, so the
 * corpse is an entity of its own here. It is built as modBodyAllocateModel
 * builds the host's, from the body and head a chr here already wears (the
 * sim it was), so nothing is loaded again; posed from its record like any
 * chr puppet (its death anim's last frame), never ticked (C1, C4), and
 * removed when its id leaves the snapshot.
 */
static struct prop *netPupMakeBody(const struct netdesc *d, const struct netentstate *s)
{
	struct chrdata *src = NULL;
	struct model *model = NULL;
	struct chrdata *chr;
	struct prop *prop;
	struct coord pos;
	RoomNum inrooms[8];
	RoomNum aboverooms[8];
	RoomNum seed[2];
	RoomNum *rooms;
	s32 i;

	// chrInit() dereferences the slot it failed to find (modbodies.c)
	if (chrsGetNumFree() < 2 || mempGetStageFreeTotal() < 256 * 1024) {
		return NULL;
	}

	for (i = 0; i < chrsGetNumSlots(); i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->model && chr->model->definition && chr->prop
				&& chr->bodynum == d->bodynum && chr->headnum == d->headnum) {
			src = chr;
			break;
		}
	}

	if (src) {
		struct modeldef *bodydef = src->model->definition;
		struct modeldef *headdef = NULL;
		struct modelnode *node = modelGetPart(bodydef, MODELPART_CHR_HEADSPOT);

		if (node) {
			struct modelrwdata_headspot *rwdata = modelGetNodeRwData(src->model, node);
			headdef = rwdata->headmodeldef;
		}

		model = body0f02d338(src->bodynum, headdef ? 1 : src->headnum, bodydef, headdef, false, false);

		if (model) {
			modelSetScale(model, src->model->scale);
		}
	} else {
		// No chr here wears the pair: never load one from the wire's numbers.
		// Those are the host's to choose and unbounded (headnum indexes the
		// head tables unchecked), and a per-corpse head load is the stage
		// pool leak modbodies.c avoids. The corpse is simply not made.
		s_BodyLoads++;
	}

	if (!model) {
		return NULL;
	}

	pos.x = s->pos[0];
	pos.y = s->pos[1];
	pos.z = s->pos[2];

	// seven at most into arrays of eight (bgFindRoomsByPos writes its
	// terminator at max, modghost.c)
	bgFindRoomsByPos(&pos, inrooms, aboverooms, 7, NULL);

	if (inrooms[0] != -1) {
		rooms = inrooms;
	} else if (aboverooms[0] != -1) {
		rooms = aboverooms;
	} else if (s->room >= 0 && s->room < g_Vars.roomcount) {
		seed[0] = s->room;
		seed[1] = -1;
		rooms = seed;
	} else {
		modelmgrFreeModel(model);
		return NULL;
	}

	prop = chrAllocate(model, &pos, rooms, 0.0f, NULL);

	if (!prop) {
		modelmgrFreeModel(model);
		return NULL;
	}

	chr = prop->chr;
	propActivateThisFrame(prop);
	propEnable(prop);

	chr->actiontype = ACT_DEAD;
	memset(&chr->act_dead, 0, sizeof(chr->act_dead));
	chr->act_dead.fadetimer60 = -1;
	chr->ailist = NULL;
	chr->sleep = 0;
	chr->keptbody60 = g_Vars.lvframe60;
	chr->fadealpha = 255;
	chr->chrflags |= CHRCFLAG_INVINCIBLE | CHRCFLAG_UNEXPLODABLE | CHRCFLAG_NOAUTOAIM | CHRCFLAG_NEVERSLEEP;
	chr->chrflags &= ~CHRCFLAG_KILLCOUNTABLE;
	chr->hidden |= CHRHFLAG_UNTARGETABLE;

	if (src) {
		chr->race = src->race;
		chr->team = src->team;
	}

	return prop;
}

/**
 * A scenario's prop (SCENOBJ, netscen.c): a briefcase or the uplink as a
 * weapon at the host's scale with its team, or Hacker Central's terminal as
 * scenarioCreateObj makes it. Never made by this machine's own
 * scenarioInitProps, so nothing else here owns or reaps it.
 */
static struct prop *netPupMakeScen(const struct netdesc *d, const struct netentstate *s)
{
	const f32 scale = d->extrascale * (1.0f / 256.0f);
	struct prop *prop;

	if (d->objtype == OBJTYPE_WEAPON) {
		struct weaponobj *weapon;

		prop = netPupMakeWeapon(d, s);

		if (!prop || !(weapon = prop->weapon) || !weapon->base.model) {
			return prop;
		}

		weapon->base.extrascale = d->extrascale;
		weapon->team = d->team;
		modelSetScale(weapon->base.model, weapon->base.model->scale * scale);
		netPupPlaceObj(&weapon->base, s, weapon->base.model->scale);

		return prop;
	}

	if (d->modelnum < 0 || d->modelnum >= NUM_MODELS || g_ModelStates[d->modelnum].fileid == 0
			|| (setupLoadModeldef(d->modelnum), 0) || !g_ModelStates[d->modelnum].modeldef) {
		return NULL;
	}

	{
		struct defaultobj tmp = {
			256,                    // extrascale
			0,                      // hidden2
			OBJTYPE_BASIC,          // type
			0,                      // modelnum
			-1,                     // pad
			0,                      // flags: still, the snapshots place it
			OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS,
			0,                      // flags3
			NULL,                   // prop
			NULL,                   // model
			1, 0, 0,                // realrot
			0, 1, 0,
			0, 0, 1,
			0,                      // hidden
			NULL,                   // geo
			NULL,                   // projectile
			0,                      // damage
			1000,                   // maxdamage
			0xff, 0xff, 0xff, 0x00, // shadecol
			0xff, 0xff, 0xff, 0x00, // nextcol
			0x0fff,                 // floorcol
			0,                      // tiles
		};
		struct defaultobj *obj = mempAlloc(ALIGN16(sizeof(struct defaultobj)), MEMPOOL_STAGE);

		if (!obj) {
			return NULL;
		}

		*obj = tmp;
		obj->modelnum = d->modelnum;
		obj->extrascale = d->extrascale;
		obj->flags |= OBJFLAG_INVINCIBLE;

		// the use key near it does what it does on the host (netClientInteract)
		if (d->scenflags & NETSCENOBJ_TERMINAL) {
			obj->flags3 |= OBJFLAG3_HTMTERMINAL | OBJFLAG3_INTERACTABLE;
		}

		prop = objInitWithModelDef(obj, g_ModelStates[d->modelnum].modeldef);

		if (!prop || !obj->model) {
			return NULL;
		}

		modelSetScale(obj->model, obj->model->scale * scale);
		propActivate(prop);
		propEnable(prop);
		netPupPlaceObj(obj, s, obj->model->scale);
	}

	return prop;
}

static void netPupFreeDyn(struct netpup *u)
{
	struct prop *prop = u->dyn;

	u->dyn = NULL;

	if (!prop || netEntsPropGen(netPupLocalIndex(prop)) != u->dyngen) {
		return;
	}

	if (prop->type == PROPTYPE_CHR && prop->chr && prop->chr->prop == prop) {
		// a corpse made here (BODY): chrRemove leaves the prop to its
		// caller (player.c's way; the pose step runs before propsTick's walk)
		chrRemove(prop, true);
		propDeregisterRooms(prop);
		propDelist(prop);
		propDisable(prop);
		propFree(prop);
		s_Freed++;
	} else if (prop->type != PROPTYPE_CHR && prop->obj && prop->obj->prop == prop) {
		objFreePermanently(prop->obj, true);
		s_Freed++;
	}
}

static struct prop *netPupDyn(struct netpup *u, u16 id, u16 gen, s32 kind, s32 rec, const struct netentstate *s)
{
	const struct netdesc *d;
	struct prop *prop = NULL;

	if (u->dyn) {
		if (netEntsPropGen(netPupLocalIndex(u->dyn)) != u->dyngen) {
			// this machine's weaponCreate/ammocrateAllocate took it back
			// (spec-entities §1's slot-steal trap): made again below
			u->dyn = NULL;
			s_Stolen++;
		} else if (u->hostgen != gen) {
			netPupFreeDyn(u);
		} else {
			return u->dyn;
		}
	}

	if (rec != NETREC_OBJ && !(rec == NETREC_CHR && kind == NETDESC_BODY)) {
		return NULL;
	}

	d = netEntsDesc(id);

	if (!d || d->gen != gen || d->kind != kind) {
		s_NoDesc++;
		return NULL;
	}

	// one that would not be made is not tried every tick
	if (u->failkind == kind && u->hostgen == gen) {
		return NULL;
	}

	switch (kind) {
	case NETDESC_DYNWEAPON:
		prop = netPupMakeWeapon(d, s);

		if (prop && WEAPON_IS_GE(d->weaponnum)) {
			s_GeGunsMade++;
			s_GoldenGuns += d->weaponnum == WEAPON_GE_GOLDENGUN;
		}
		break;
	case NETDESC_HAT:
		prop = netPupMakeHat(d, s);
		break;
	case NETDESC_AMMOCRATE:
		prop = netPupMakeCrate(d, s);
		break;
	case NETDESC_SCENOBJ:
		prop = netPupMakeScen(d, s);
		break;
	case NETDESC_BODY:
		prop = netPupMakeBody(d, s);
		break;
	default:
		// DYNOBJ (debris and the like) is not made
		break;
	}

	u->hostgen = gen;

	if (!prop) {
		u->failkind = kind;
		s_CreateFail++;
		return NULL;
	}

	u->failkind = 0;
	u->dyn = prop;
	u->dyngen = netEntsPropGen(netPupLocalIndex(prop));
	u->havestate = 0;
	s_Created[kind]++;

	return prop;
}

/*
 * The trace (tools/ci/netpuppettest.sh): host tick, id, kind, pos, yaw, anim
 */

static s32 s_TraceNow = 0; // this pose is traced: a tick pass's first on a traced tick
static u32 s_TraceLast = 0xffffffff;

/*
 * A traced pose is written after the frame's lvRender (netPuppetsTraceFlush
 * from netTickEnd), from what was drawn: the prop's position, the model's
 * yaw and anim as they stood after every later write of the frame, and
 * whether it was on this machine's screen. (A model's built matrices live
 * in the frame's graphics pool and are reused by then, so they are not
 * read here.)
 */
static void netPupTrace(f64 rt, u16 id, s32 kind, struct prop *prop)
{
	struct nettraceq *q;

	if (!s_TraceNow || s_TraceQN >= NET_TRACEQ) {
		return;
	}

	q = &s_TraceQ[s_TraceQN++];
	q->rt = rt;
	q->prop = prop;
	q->id = id;
	q->kind = kind;
	q->type = prop->type;
}

void netPuppetsTraceFlush(void)
{
	s32 i;

	if (!s_TraceFile || s_TraceQN == 0) {
		s_TraceQN = 0;
		return;
	}

	for (i = 0; i < s_TraceQN; i++) {
		const struct nettraceq *q = &s_TraceQ[i];
		struct prop *prop = q->prop;
		struct chrdata *chr;

		if (prop->type != q->type) {
			continue;
		}

		if (prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) {
			chr = prop->chr;

			if (!chr || !chr->model || !chr->model->anim) {
				continue;
			}

			// the last field: drawn on this machine's screen this frame
			fprintf(s_TraceFile, "P %u %.4f %u %d %.3f %.3f %.3f %.5f %d %.3f %d %d %d %d\n", g_NetTick, q->rt, q->id, q->kind,
					prop->pos.x, prop->pos.y, prop->pos.z, modelGetChrRotY(chr->model),
					chr->model->anim->animnum, chr->model->anim->frame, chr->actiontype,
					(chr->chrflags & CHRCFLAG_HIDDEN) ? 1 : 0, prop->rooms[0], (prop->flags & PROPFLAG_ONTHISSCREENTHISTICK) ? 1 : 0);
		} else if (prop->type == PROPTYPE_DOOR && prop->obj) {
			struct doorobj *door = (struct doorobj *)prop->obj;

			fprintf(s_TraceFile, "D %u %.4f %u %.5f %d\n", g_NetTick, q->rt, q->id,
					door->maxfrac > 0 ? door->frac / door->maxfrac : 0, door->mode);
		} else if (prop->obj) {
			fprintf(s_TraceFile, "O %u %.4f %u %d %.3f %.3f %.3f %d %d %d\n", g_NetTick, q->rt, q->id, q->kind,
					prop->pos.x, prop->pos.y, prop->pos.z,
					(prop->obj->hidden & OBJHFLAG_GONE) ? 1 : 0, prop->obj->type, prop->rooms[0]);
		}
	}

	s_TraceQN = 0;
}

/*
 * The pose step
 */

// the render clock as the last pose step had it, for the events
static f64 s_PoseRt = 0;
static s32 s_PoseRtValid = 0;

static void netClientPosePuppetsRun(void);

/**
 * The pose step, then every event the render clock has reached: a tick's
 * records are posed before its events are applied (spec-entities.md §5)
 */
void netClientPosePuppets(void)
{
	s_PoseRtValid = 0;
	netClientPosePuppetsRun();

	if (g_NetClientWorld && g_NetPass != NETPASS_PRESENT_ONLY) {
		netEventsClientDrain(s_PoseRtValid, s_PoseRt);
	}
}

static void netClientPosePuppetsRun(void)
{
	const struct netsnapclient *c;
	const struct netbaselineslot *sb;
	const struct netbaselineslot *sa = NULL;
	const struct netbaselineslot *sp = NULL;
	const struct netsnapinfo *ib;
	const struct netsnapinfo *ia = NULL;
	const struct netsnapinfo *ip = NULL;
	u16 before;
	u16 after;
	u16 pb;
	u16 pa;
	f64 rt;
	u32 ht;
	f32 t = 0;
	f32 dt = 0;
	f32 ptick = 0;
	s32 i;
	s32 maxids;

	if (!g_NetClientWorld) {
		return;
	}

	// this machine's own player, as the host has it
	netEntsClientApplyLocal();

	maxids = netEntsMaxIds();

	if (!maxids || !s_HaveOff || !s_Live) {
		return;
	}

	if (!s_Pup || s_PupMax != maxids) {
		free(s_Pup);
		s_Pup = calloc(maxids, sizeof(*s_Pup));
		s_PupMax = s_Pup ? maxids : 0;

		if (!s_Pup) {
			return;
		}
	}

	c = netEntsClient();
	s_DelayLast = netPupDelay();
	rt = (f64)g_NetTick - s_Off - s_DelayLast;

	// a frame drawn between ticks is that much later (the presented pass)
	if (g_NetPass == NETPASS_PRESENT_ONLY) {
		rt += frametimeNetAlpha();
	}

	ht = rt <= 0 ? 0 : (u32)floor(rt);
	s_PoseRt = rt;
	s_PoseRtValid = 1;

	netSnapClientBracket(c, ht, &before, &after);

	if (!before) {
		// before every snapshot held: the oldest after it stands
		if (!after) {
			return;
		}

		before = after;
		after = 0;
		s_Behind++;
	}

	sb = netPupSlot(c, before);
	ib = netSnapClientInfo(c, before);

	if (!sb || !ib) {
		return;
	}

	if (after) {
		sa = netPupSlot(c, after);
		ia = netSnapClientInfo(c, after);

		if (!sa || !ia || ia->hosttick <= ib->hosttick) {
			sa = NULL;
		}
	}

	if (sa) {
		t = (f32)((rt - ib->hosttick) / (f64)(ia->hosttick - ib->hosttick));
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		s_Interp++;
	} else {
		// past the newest: the one before it gives the speed
		dt = (f32)(rt - ib->hosttick);

		if (dt > NETPUP_EXTRAPMAX) {
			dt = NETPUP_EXTRAPMAX;
			s_Held++;
		} else {
			s_Extrap++;
		}

		if (ib->hosttick > 0 && netSnapClientBracket(c, ib->hosttick - 1, &pb, &pa) && pb) {
			sp = netPupSlot(c, pb);
			ip = netSnapClientInfo(c, pb);

			if (!sp || !ip || ip->hosttick >= ib->hosttick) {
				sp = NULL;
			} else {
				ptick = (f32)(ib->hosttick - ip->hosttick);
			}
		}
	}

	s_Serial++;
	s_Poses++;
	s_TraceNow = s_TraceFile && g_NetPass != NETPASS_PRESENT_ONLY && (g_NetTick % s_TraceEvery) == 0 && s_TraceLast != g_NetTick;

	if (s_TraceNow) {
		s_TraceLast = g_NetTick;
	}

	for (i = 0; i < sb->count; i++) {
		const u16 id = sb->ids[i];
		const u8 *rb = sb->records + (size_t)i * NETSNAP_STORE;
		const u16 gen = netStoreGen(rb);
		const s32 rec = netStoreRec(rb);
		const s32 kind = netStoreKind(rb);
		const u8 *ra;
		const u8 *rp;
		struct netentstate stb;
		struct netentstate sta;
		struct netentstate stp;
		struct netentstate st;
		struct netpup *u;
		struct prop *prop;
		s32 isdyn;
		s32 snapped = 0;

		if (id >= s_PupMax) {
			continue;
		}

		u = &s_Pup[id];
		ra = sa ? netPupFind(sa, id, gen, rec) : NULL;
		rp = sp ? netPupFind(sp, id, gen, rec) : NULL;

		netRecUnpack(rec, rb + NETSNAP_STOREHDR, &stb);

		if (ra) {
			netRecUnpack(rec, ra + NETSNAP_STOREHDR, &sta);
		}

		if (rp) {
			netRecUnpack(rec, rp + NETSNAP_STOREHDR, &stp);
		}

		netPupBlend(rec, &stb, ra ? &sta : NULL, t, rp ? &stp : NULL, ptick, dt, &st);

		isdyn = kind != NETDESC_SETUPOBJ && kind != NETDESC_SIM && kind != NETDESC_PLAYER;

		// what was made for this id and generation stays while the id is
		// present: a corpse is the same host prop as the sim before it, and
		// a render clock that steps back a tick poses that sim's last record
		// again (nothing to pose then: the kept body is not made twice)
		if (u->dyn && u->hostgen == gen) {
			u->posed = s_Serial;
		}

		if (isdyn) {
			prop = netPupDyn(u, id, gen, kind, rec, &st);
		} else {
			prop = netEntsMapped(id, gen);

			// mapped, then this machine's own prop went: the next snapshot
			// NACKs it; nothing to pose meanwhile
			if (prop && prop == netLocalPlayerProp()) {
				prop = NULL;
			}
		}

		if (!prop) {
			continue;
		}

		u->posed = s_Serial;

		// a skip for what has not changed since the last pose (most
		// objects and doors most of the time)
		if (rec != NETREC_CHR && u->havestate && t == u->lastt
				&& memcmp(u->lastb, rb + NETSNAP_STOREHDR, NETREC_MAX) == 0
				&& (!ra || memcmp(u->lasta, ra + NETSNAP_STOREHDR, NETREC_MAX) == 0)) {
			netPupTrace(rt, id, kind, prop);
			continue;
		}

		memcpy(u->lastb, rb + NETSNAP_STOREHDR, NETREC_MAX);

		if (ra) {
			memcpy(u->lasta, ra + NETSNAP_STOREHDR, NETREC_MAX);
		} else {
			memset(u->lasta, 0, NETREC_MAX);
		}

		u->lastt = t;

		switch (rec) {
		case NETREC_CHR:
			if (!u->havetele || u->teleports != st.teleports) {
				snapped = u->havetele;
				s_Snaps += u->havetele;
				u->teleports = st.teleports;
				u->havetele = 1;
			}

			netPupChr(u, prop, kind, &st, snapped);
			break;
		case NETREC_OBJ:
			netPupObj(u, prop, &st, isdyn);
			break;
		case NETREC_DOOR:
			netPupDoor(u, prop, &st);
			break;
		case NETREC_LIFT:
			netPupLift(u, prop, &st);
			break;
		default:
			break;
		}

		netPupTrace(rt, id, kind, prop);
	}

	// what this machine made for ids no longer present goes
	for (i = 0; i < s_PupMax; i++) {
		if (s_Pup[i].dyn && s_Pup[i].posed != s_Serial) {
			netPupFreeDyn(&s_Pup[i]);
			s_Pup[i].havestate = 0;
		}
	}

	if (s_TraceNow) {
		fprintf(s_TraceFile, "T %u %.4f %u %u %.3f %.3f\n", g_NetTick, rt, ib->hosttick, sa ? ia->hosttick : 0, t, s_DelayLast);
	}
}

/**
 * A host entity's prop here: one made for it (a dynamic kind) or the one
 * mapped to it, while its local generation holds; NULL otherwise
 */
struct prop *netPuppetsLocalProp(u16 id, u16 gen)
{
	if (s_Pup && id < s_PupMax && s_Pup[id].dyn) {
		struct netpup *u = &s_Pup[id];

		if (u->hostgen == gen && netEntsPropGen(netPupLocalIndex(u->dyn)) == u->dyngen) {
			return u->dyn;
		}
	}

	// (one made for an older generation of the id goes at the next pose)
	return netEntsMapped(id, gen);
}

/*
 * The gates' helpers
 */

/**
 * C5: objTick on the client: no regen countdown, no lift (the snapshots
 * carry both); a beam still fades and an escalator still turns
 */
u32 netPuppetObjTick(struct prop *prop)
{
	struct defaultobj *obj = prop->obj;

	if (!obj) {
		return TICKOP_NONE;
	}

	if (obj->type == OBJTYPE_AUTOGUN) {
		struct autogunobj *autogun = (struct autogunobj *)obj;

		if (autogun->beam) {
			beamTick(autogun->beam);
		}
	} else if (obj->type == OBJTYPE_CHOPPER) {
		struct chopperobj *chopper = (struct chopperobj *)obj;

		if (chopper->fireslotthing && chopper->fireslotthing->beam) {
			beamTick(chopper->fireslotthing->beam);
		}
	}

	return TICKOP_NONE;
}

s32 netClientInteract(s32 eyespy)
{
	return propFindForInteract(eyespy) == NULL;
}

static s32 netPupPlayerSkipped(s32 playernum)
{
	struct player *player;

	if (!g_NetClientWorld || playernum == g_NetLocalSlot || playernum < 0 || playernum >= PLAYERCOUNT()) {
		return 0;
	}

	player = g_Vars.players[playernum];

	return player && player->prop && netPupIsLive(player->prop);
}

/**
 * pdmain.c, before lvTickPlayer (currentplayer is playernum): another
 * machine's player is posed, not simulated. Its body stays built (the pose
 * writes into it); until its first record it runs as before, hidden.
 */
s32 netClientPuppetPlayerTick(s32 playernum)
{
	struct player *player;

	if (!g_NetClientWorld || playernum == g_NetLocalSlot || playernum < 0 || playernum >= PLAYERCOUNT()) {
		return 0;
	}

	player = g_Vars.players[playernum];

	if (!player || !player->prop) {
		return 0;
	}

	if (!netPupIsLive(player->prop)) {
		if (player->prop->chr) {
			player->prop->chr->chrflags |= CHRCFLAG_HIDDEN;
		}

		return 0;
	}

	if (!player->haschrbody) {
		const s32 idx = netPupLocalIndex(player->prop);

		playerTickChrBody();

		// a body built anew is the same prop: still live
		if (idx >= 0 && idx < s_LiveMax) {
			s_LiveGen[idx] = netEntsPropGen(idx);
		}
	}

	return 1;
}

s32 netClientRenderPass(s32 order, s32 count, s32 *islast)
{
	s32 j;

	if (!g_NetClientWorld) {
		return 0;
	}

	if (netPupPlayerSkipped(playermgrGetPlayerAtOrder(order))) {
		return 1;
	}

	// the last pass that runs is the last (propsTickPlayer's updateframe)
	*islast = 1;

	for (j = order + 1; j < count; j++) {
		if (!netPupPlayerSkipped(playermgrGetPlayerAtOrder(j))) {
			*islast = 0;
			break;
		}
	}

	return 0;
}

void netClientOrderPlayers(void)
{
	s32 i;
	s32 first = -1;
	s32 local = -1;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_Vars.players[g_Vars.playerorder[i]]) {
			if (first < 0) {
				first = i;
			}

			if (g_Vars.playerorder[i] == g_NetLocalSlot) {
				local = i;
			}
		}
	}

	if (first >= 0 && local > first) {
		const s32 tmp = g_Vars.playerorder[first];

		g_Vars.playerorder[first] = g_Vars.playerorder[local];
		g_Vars.playerorder[local] = tmp;
	}
}

s32 netClientInMatch(void)
{
	return g_NetMode == NETMODE_CLIENT && netSessionMatchLoading();
}

void netPuppetsLog(const char *why)
{
	sysLogPrintf(LOG_NOTE, "net: puppets %s (tick %u): poses %u (interpolated %u, extrapolated %u, held past 100 ms %u, before every snapshot %u), render delay %.1f ticks (jitter %.2f, clock resyncs %u); first records %u, teleport snaps %u, sim deaths %u; made: weapons %u, hats %u, crates %u, scenario props %u; make failed %u, no descriptor %u, freed %u, taken back by this machine %u; held-item swaps %u, bad anims %u; doors moved %u, door sounds %u, regens %u, unpaused %u, trails %u, player puppet deaths %u; content: bodies made %u (no wearer %u), hats worn %u, GE guns held %u, GE guns made %u (Golden Gun %u, in a puppet's hand %u, holders in turn %u), held guns not made %u, lift moves %u",
			why, g_NetTick, s_Poses, s_Interp, s_Extrap, s_Held, s_Behind, s_DelayLast, s_Jit, s_Resyncs,
			s_FirstRecords, s_Snaps, s_Deaths, s_Created[NETDESC_DYNWEAPON], s_Created[NETDESC_HAT], s_Created[NETDESC_AMMOCRATE], s_Created[NETDESC_SCENOBJ],
			s_CreateFail, s_NoDesc, s_Freed, s_Stolen, s_HeldSwaps, s_BadAnims, s_DoorMoves, s_DoorSounds, s_Regens, s_Unpaused, s_Trails, s_PlayerDeaths,
			s_Created[NETDESC_BODY], s_BodyLoads, s_HatsWorn, s_GeGunsHeld, s_GeGunsMade, s_GoldenGuns, s_GoldenHands, s_GoldenHolders, s_HeldFails, s_LiftMoves);
}
