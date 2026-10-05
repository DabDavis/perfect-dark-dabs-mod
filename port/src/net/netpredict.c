#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "input.h"
#include "screenshot.h"
#include "lib/joy.h"
#include "lib/vi.h"
#include "lib/anim.h"
#include "game/bondmove.h"
#include "game/bondwalk.h"
#include "game/options.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/timing.h"
#include "game/activemenu.h"
#include "gestan.h"
#include "net/net.h"
#include "net/netsnap.h"
#include "netint.h"

/**
 * Prediction (PLANS/NETPLAY.md "Prediction", phase 5a): a client runs its
 * own player on its own commands the tick it makes them, and squares that
 * with the host afterwards by re-simulating, Quake 3 and Source's way (our
 * own code: cg_predict.c and prediction.cpp were read for the shape only).
 *
 * Each tick the client keeps, in a ring of NETPRED_RING:
 *   - the command (the pad and mouse it sent, netplayers.c);
 *   - how bmoveTick ran for its player (its arguments, the level clock it
 *     ran on, anything that moved the player between the last tick's end
 *     and this tick's walk - a lift, a push);
 *   - the player's movement state at the tick's end: the netmove fields the
 *     host also sends, and the rest of what the walk carries from one tick
 *     into the next (the head bob's animation and model, the derived
 *     angles, the eye) as raw struct player bytes, which never leave this
 *     machine.
 *
 * A snapshot's local-player block is the host's state after command N (the
 * last of this client's it played). In the pose step of the next tick it is
 * compared with the ring's state after N: within NETPRED_POSEPS units, the
 * angles within NETPRED_ANGEPS degrees and the discrete state (move mode,
 * ladder, falling, crouch) the same, nothing is done. Otherwise the player
 * is put in the host's state (the ring's own fields at N under the host's)
 * and the commands N+1.. are played again through the real input path: each
 * one a pad sample consumed as a tick's (joyNetInjectSample) and bmoveTick
 * with the arguments it had, under g_NetReplaying, so nothing but the
 * movement happens again (no sound, door, death, gun, push: each such place
 * returns on the flag). The ring takes the corrected states. Everything in
 * struct player outside the movement is put back as it was, so the gun and
 * HUD never see the replay.
 *
 * Only the drawn view moves smoothly: the correction's jump (the eye's move
 * and the view's turn) is kept as an offset that eases out with a time
 * constant of ~100 ms (NETPRED_EASE per tick), unless it is larger than
 * NETPRED_SNAP (NETPRED_ANGSNAP degrees) or the block came with a respawn
 * or teleport.
 *
 * A respawn or teleport block is replayed from the player as this machine
 * has it (just respawned) under the host's movement, never the ring's state
 * at N, which was a dead player's; the ticks this machine ran dead (the
 * host ran them alive after the respawn) are played too and kept as moved,
 * so the blocks after it agree. Anything outside struct player that the
 * replayed input touches (gun functions in g_PlayerConfigsArray, the active
 * menu) is put back after; the walk's side tables outside it (GoldenEye's
 * climbs and force-crouch tiles, bondwalk.c and gestan.c) are kept per tick
 * with the rest of the state. A block for a command this client has not run
 * is never taken, nor a head animation or head data out of range. On a frame drawn between ticks the mouse moved since
 * the last tick turns the view (presentation only; the next tick turns the
 * player by the same amount).
 *
 * A lift moves the player between ticks (platformDisplaceProps): that move
 * is kept per tick and made again in a replay, so the replay does not fight
 * the lift. Doors and other props stand where they are now in a replay. A
 * jump is pressed after the walk (lv.c's use press, bwalkTryJump): the tick
 * that tried one tries it again after its replayed walk.
 *
 * Two things had to be the same on both machines for a replay to land where
 * the host does, and were not:
 *   - the head's animation and model (struct anim, bondheadsave): the walk
 *     is moved by the head bob read off it, so the host sends them too;
 *   - frames drawn between ticks: bmoveTick runs on them at a level clock of
 *     zero and its input handling still changed the walk (a held strafe
 *     counted over the whole sample ring), by however many such frames that
 *     machine drew. netPredictPresentBegin/End put the walk back after one;
 *   - the aim's crosshair: on the host a remote player's command mouse was
 *     read again on each frame drawn between ticks and moved the crosshair
 *     (swivelpos) to the edge, which turns the view while aiming; it is a
 *     tick's alone now (netMouseDelta), and the aim and zoom are in the
 *     ring and in the block.
 */

#define NETPRED_RING    256
#define NETPRED_RICHMAX 2048
#define NETPRED_POSEPS  0.5f
#define NETPRED_ANGEPS  0.05f
#define NETPRED_SNAP    64.f      // a correction past this is taken at once, not eased
#define NETPRED_ANGSNAP 20.f      // ... and a turn past this many degrees
#define NETPRED_EASE    0.8465f   // exp(-16.67 ms / 100 ms): the eased offset left after a tick
#define NETPRED_EXTMAX  40.f      // a move between ticks past this is a respawn or warp, not a lift
#define NETPRED_SANE    1000000.f

s32 g_NetReplaying = 0;

struct netpredcmd {
	u32 buttons;
	s8 sx;
	s8 sy;
	s8 rsx;
	s8 rsy;
	f32 mdx;
	f32 mdy;
	u8 flags;
};

struct netpredtick {
	u32 tick1;               // the tick + 1 this slot holds, 0 none
	struct netpredcmd cmd;
	u8 hascmd;
	u8 moved;                // bmoveTick ran for the player in a way a replay can repeat
	u8 hasstate;
	u8 jumptry;              // the tick tried a jump after its walk (lv.c's use press)
	u8 replayable;           // ... or would have but for the player being dead (a respawn's replay plays it)
	s8 args[4];
	f32 fovy;                // the view's fov the walk turned by (viGetFovY: the zoom)
	s32 lvupdate240;
	s32 lvupdate60;
	f32 lvupdate60f;
	f32 lvupdate60freal;
	s32 lvframe60;
	s32 lvframenum;
	f32 ext[3];              // the move before the walk, since the last tick's end
	f32 pos[3];
	RoomNum rooms[8];
	f32 theta;
	f32 verta;
	struct netmove mv;
	// the walk's side tables outside struct player (bondwalk.c, gestan.c:
	// GoldenEye's climbs and force-crouch tiles)
	s32 gecrouchhold;
	f32 geeyelag;
	f32 geclimbhold;
	s32 getile;
	s32 gefromtile;
	u8 rich[NETPRED_RICHMAX];
};

// The struct player fields the walk carries from tick to tick: a replay
// starts from the ring's copy of them and they are what it keeps
struct netpredfield {
	u16 off;
	u16 size;
};

#define PF(f) { offsetof(struct player, f), sizeof(((struct player *)0)->f) }

static const struct netpredfield s_Fields[] = {
	PF(isfalling), PF(fallstart), PF(sumground), PF(vv_manground), PF(vv_ground), PF(bdeltapos),
	PF(sumcrouch), PF(crouchheight), PF(crouchtime240), PF(crouchfall), PF(swaypos), PF(swayoffset),
	PF(swaytarget), PF(swayoffset0), PF(swayoffset2), PF(crouchpos), PF(autocrouchpos),
	PF(crouchoffset), PF(crouchspeed), PF(gunspeed),
	PF(docentreupdown), PF(lastupdown60), PF(prevupdown), PF(movecentrerelease), PF(automovecentre),
	PF(vv_theta), PF(speedtheta), PF(vv_costheta), PF(vv_sintheta), PF(vv_verta), PF(vv_verta360),
	PF(speedverta), PF(vv_cosverta), PF(vv_sinverta), PF(speedsideways), PF(speedstrafe),
	PF(speedforwards), PF(speedboost), PF(speedmaxtime60), PF(bondshotspeed), PF(bondbreathing),
	PF(moveinitspeed), PF(bondmovemode), PF(unk01c0), PF(invdowntime), PF(usedowntime),
	PF(bondprevrooms), PF(liftground), PF(lift), PF(ladderupdown), PF(laddernormal), PF(onladder),
	PF(inlift), PF(bondprevpos), PF(bond2),
	PF(resetheadpos), PF(resetheadrot), PF(resetheadtick), PF(headanim), PF(headdamp),
	PF(headwalkingtime60), PF(headamplitude), PF(sideamplitude), PF(headpos), PF(headlook),
	PF(headup), PF(headpossum), PF(headlooksum), PF(headupsum), PF(headbodyoffset), PF(standheight),
	PF(standbodyoffset), PF(standfrac), PF(standlook), PF(standup), PF(standcnt), PF(bondheadsave),
	PF(floorcol), PF(floorflags), PF(floortype), PF(bondleandown), PF(speedgo), PF(crouchoffsetreal),
	PF(floorroom), PF(crouchoffsetsmall), PF(crouchoffsetrealsmall), PF(vv_height), PF(vv_headheight),
	PF(vv_eyeheight), PF(periminfo), PF(perimshoot), PF(bondprevtheta), PF(bondonground),
	PF(walkinitmove), PF(walkinitpos), PF(walkinitmtx), PF(walkinitt), PF(walkinitt2),
	PF(walkinitstart), PF(bondforcespeed), PF(speedthetacontrol), PF(altdowntime), PF(amdowntime),
	PF(rollspeed), PF(rolltime60),
	// the aim: the mouse's crosshair turns the view past the edge while
	// aiming, and the zoom scales every turn (bmoveProcessInput)
	PF(swivelpos), PF(insightaimmode), PF(zoomintime), PF(zoomintimemax), PF(zoominfovy),
	PF(zoominfovyold), PF(zoominfovynew),
};

#undef PF

static struct netpredtick *s_Ring = NULL;
static s32 s_RichSize = 0;
extern s16 g_NumAnimations;
static u8 s_SavePlayer[sizeof(struct player)];
static s32 netPredRichSize(void);
static u8 s_RichTmp[NETPRED_RICHMAX];
static u8 *s_JoySave = NULL;
static const struct netpredcmd *s_ReplayCmd = NULL;
static s32 s_HaveLast = 0;
static u32 s_LastCmd = 0;
static f32 s_EyeOff[3];
static FILE *s_Log = NULL;
static char s_LogPath[512];
static s32 s_LogDiff = 0;   // --net-predict-diff: which movement words differ, per block
#define NETPRED_MAXSHOTS 8
static u32 s_ShotTicks[NETPRED_MAXSHOTS]; // --net-predict-shots T,T,...: the client's screenshots, by tick
static s32 s_NumShots = 0;

// counts
static u32 s_Compared = 0;    // blocks compared with the ring
static u32 s_Matched = 0;     // ... and found within the bounds
static u32 s_Corrections = 0; // ... and found off: replayed
static f64 s_ErrSum = 0;      // the position error at the compared command, over the corrections
static f32 s_ErrMax = 0;
static f64 s_ShiftSum = 0;    // how far a correction moved the player now
static f32 s_ShiftMax = 0;
static u32 s_Snaps = 0;       // corrections too large to ease (not respawns)
static u32 s_AbsSnaps = 0;    // respawns and teleports
static u32 s_Missing = 0;     // no state in the ring for the block's command
static u32 s_Refused = 0;     // a block with no place or bad floats: not taken
static u32 s_Replayed = 0;    // ticks played again
static u32 s_Overrun = 0;     // the block's command older than the ring
static u32 s_DiscreteOff = 0; // corrections for the discrete state alone
static u32 s_AngleOff = 0;    // ... or the angles alone
static u32 s_StartTick = 0;
static u32 s_Future = 0;      // a block for a command this client has not sent: not taken
static u32 s_AnimRefused = 0; // a block's head animation or head data out of range: this machine's kept
static u32 s_RespawnFloor = 0; // blocks for commands before this (a respawn taken here) are not compared
static f32 s_AngOff[2];       // the eased view's angle offset (theta, verta), as s_EyeOff

void netPredictArgs(void)
{
	const char *log = sysArgGetString("--net-predict-log");

	if (log) {
		snprintf(s_LogPath, sizeof(s_LogPath), "%s", log);
	}

	s_LogDiff = sysArgCheck("--net-predict-diff");

	{
		const char *shots = sysArgGetString("--net-predict-shots");

		while (shots && *shots && s_NumShots < NETPRED_MAXSHOTS) {
			char *end;
			const unsigned long v = strtoul(shots, &end, 10);

			if (end == shots) {
				break;
			}

			s_ShotTicks[s_NumShots++] = (u32)v;
			shots = *end == ',' ? end + 1 : end;
		}
	}
}

// --exit-frame and the like end the process mid-match: the counts still go out
static void netPredictAtExit(void)
{
	if (s_Ring) {
		netPredictLog("at exit");
	}
}

void netPredictStageStart(void)
{
	static s32 atexitset = 0;

	if (!atexitset) {
		atexit(netPredictAtExit);
		atexitset = 1;
	}

	if (netPredRichSize() > NETPRED_RICHMAX) {
		sysLogPrintf(LOG_ERROR, "net: prediction state is %d bytes, more than %d: prediction off", s_RichSize, NETPRED_RICHMAX);
		return;
	}

	if (!s_Ring) {
		s_Ring = calloc(NETPRED_RING, sizeof(*s_Ring));
	} else {
		memset(s_Ring, 0, NETPRED_RING * sizeof(*s_Ring));
	}

	if (!s_JoySave) {
		s_JoySave = malloc(joyNetSaveSize());
	}

	s_HaveLast = 0;
	s_LastCmd = 0;
	s_EyeOff[0] = s_EyeOff[1] = s_EyeOff[2] = 0;
	s_Compared = s_Matched = s_Corrections = 0;
	s_ErrSum = 0;
	s_ErrMax = 0;
	s_ShiftSum = 0;
	s_ShiftMax = 0;
	s_Snaps = s_AbsSnaps = s_Missing = s_Refused = s_Replayed = s_Overrun = 0;
	s_DiscreteOff = s_AngleOff = 0;
	s_StartTick = 0;
	s_Future = s_AnimRefused = 0;
	s_RespawnFloor = 0;
	s_AngOff[0] = s_AngOff[1] = 0;

	if (s_LogPath[0] && !s_Log) {
		s_Log = fopen(s_LogPath, "w");

		if (s_Log) {
			setvbuf(s_Log, NULL, _IOLBF, 0);
		}
	}

	sysLogPrintf(LOG_NOTE, "net: prediction: ring %d ticks, %d bytes of player state a tick", NETPRED_RING, s_RichSize);
}

void netPredictMatchStopped(void)
{
	if (s_Ring) {
		netPredictLog("at the match's end");
	}

	if (s_Log) {
		fclose(s_Log);
		s_Log = NULL;
	}

	free(s_Ring);
	s_Ring = NULL;
	g_NetReplaying = 0;
}

void netPredictLog(const char *why)
{
	sysLogPrintf(LOG_NOTE, "net: prediction %s (tick %u): compared %u, matched %u (%.2f%%), corrections %u (position error at the command mean %.2f max %.2f; moved now mean %.2f max %.2f; angles only %u, discrete only %u), ticks replayed %u, snaps %u, respawn/teleport snaps %u, no state %u, too old %u, blocks refused %u, future commands %u, head data refused %u",
			why, g_NetTick, s_Compared, s_Matched, s_Compared ? 100.0 * s_Matched / s_Compared : 0.0, s_Corrections,
			s_Corrections ? (f32)(s_ErrSum / s_Corrections) : 0.f, s_ErrMax,
			s_Corrections ? (f32)(s_ShiftSum / s_Corrections) : 0.f, s_ShiftMax, s_AngleOff, s_DiscreteOff,
			s_Replayed, s_Snaps, s_AbsSnaps, s_Missing, s_Overrun, s_Refused, s_Future, s_AnimRefused);
}

static struct netpredtick *netPredAt(u32 tick)
{
	struct netpredtick *e;

	if (!s_Ring) {
		return NULL;
	}

	e = &s_Ring[tick % NETPRED_RING];

	return e->tick1 == tick + 1 ? e : NULL;
}

// The slot for this tick, taken over if it held an older one
static struct netpredtick *netPredSlot(u32 tick)
{
	struct netpredtick *e;

	if (!s_Ring) {
		return NULL;
	}

	e = &s_Ring[tick % NETPRED_RING];

	if (e->tick1 != tick + 1) {
		e->tick1 = tick + 1;
		e->hascmd = 0;
		e->moved = 0;
		e->hasstate = 0;
		e->jumptry = 0;
		e->replayable = 0;
	}

	return e;
}

static struct player *netPredLocal(void)
{
	struct player *p;

	if (g_NetLocalSlot < 0 || g_NetLocalSlot >= PLAYERCOUNT()) {
		return NULL;
	}

	p = g_Vars.players[g_NetLocalSlot];

	return p && p->prop && p->prop->chr ? p : NULL;
}

/*
 * The state
 */

static s32 netPredRichSize(void)
{
	s32 i;

	if (s_RichSize == 0) {
		for (i = 0; i < (s32)(sizeof(s_Fields) / sizeof(s_Fields[0])); i++) {
			s_RichSize += s_Fields[i].size;
		}
	}

	return s_RichSize;
}

static void netPredSaveRich(const struct player *p, u8 *out)
{
	const u8 *base = (const u8 *)p;
	s32 i;

	for (i = 0; i < (s32)(sizeof(s_Fields) / sizeof(s_Fields[0])); i++) {
		memcpy(out, base + s_Fields[i].off, s_Fields[i].size);
		out += s_Fields[i].size;
	}
}

static void netPredLoadRich(struct player *p, const u8 *in)
{
	u8 *base = (u8 *)p;
	s32 i;

	for (i = 0; i < (s32)(sizeof(s_Fields) / sizeof(s_Fields[0])); i++) {
		memcpy(base + s_Fields[i].off, in, s_Fields[i].size);
		in += s_Fields[i].size;
	}
}

void netPredictCaptureMove(struct player *p, struct netmove *mv)
{
	memset(mv, 0, sizeof(*mv));
	mv->speedtheta = p->speedtheta;
	mv->speedverta = p->speedverta;
	mv->speedthetacontrol = p->speedthetacontrol;
	mv->speedsideways = p->speedsideways;
	mv->speedstrafe = p->speedstrafe;
	mv->speedforwards = p->speedforwards;
	mv->speedboost = p->speedboost;
	mv->speedgo = p->speedgo;
	mv->speedmaxtime60 = p->speedmaxtime60;
	mv->shotspeed[0] = p->bondshotspeed.x;
	mv->shotspeed[1] = p->bondshotspeed.y;
	mv->shotspeed[2] = p->bondshotspeed.z;
	mv->moveinitspeed[0] = p->moveinitspeed.x;
	mv->moveinitspeed[1] = p->moveinitspeed.y;
	mv->moveinitspeed[2] = p->moveinitspeed.z;
	mv->forcespeed[0] = p->bondforcespeed.x;
	mv->forcespeed[1] = p->bondforcespeed.y;
	mv->forcespeed[2] = p->bondforcespeed.z;
	mv->rollspeed[0] = p->rollspeed.x;
	mv->rollspeed[1] = p->rollspeed.y;
	mv->rollspeed[2] = p->rollspeed.z;
	mv->rolltime60 = p->rolltime60;
	mv->vely = p->bdeltapos.y;
	mv->sumground = p->sumground;
	mv->manground = p->vv_manground;
	mv->ground = p->vv_ground;
	mv->onground = p->bondonground;
	mv->fallage = p->isfalling ? g_Vars.lvframe60 - p->fallstart : 0;
	mv->crouchoffset = p->crouchoffset;
	mv->crouchspeed = p->crouchspeed;
	mv->crouchheight = p->crouchheight;
	mv->crouchfall = p->crouchfall;
	mv->sumcrouch = p->sumcrouch;
	mv->crouchoffsetsmall = p->crouchoffsetsmall;
	mv->crouchtime240 = p->crouchtime240;
	mv->crouchoffsetreal = p->crouchoffsetreal;
	mv->crouchoffsetrealsmall = p->crouchoffsetrealsmall;
	mv->swaytarget = p->swaytarget;
	mv->swayoffset0 = p->swayoffset0;
	mv->swayoffset2 = p->swayoffset2;
	mv->laddernormal[0] = p->laddernormal.x;
	mv->laddernormal[1] = p->laddernormal.y;
	mv->laddernormal[2] = p->laddernormal.z;
	mv->ladderupdown = p->ladderupdown;
	mv->liftground = p->liftground;
	mv->height = p->vv_height;
	mv->eyeheight = p->vv_eyeheight;
	mv->gunspeed = p->gunspeed;
	mv->breathing = p->bondbreathing;
	mv->headpossum[0] = p->headpossum.x;
	mv->headpossum[1] = p->headpossum.y;
	mv->headpossum[2] = p->headpossum.z;
	mv->headwalkingtime60 = p->headwalkingtime60;
	mv->floorroom = p->floorroom;
	mv->floorflags = p->floorflags;
	mv->isfalling = p->isfalling ? 1 : 0;
	mv->onladder = p->onladder ? 1 : 0;
	mv->inlift = p->inlift ? 1 : 0;
	mv->movemode = (u8)p->bondmovemode;
	mv->crouchpos = (s8)p->crouchpos;
	mv->autocrouchpos = (s8)p->autocrouchpos;
	mv->headanim = (s8)p->headanim;
	mv->floortype = p->floortype;

	{
		const struct anim *a = &p->unk01c0;
		s32 i;

		mv->animnum = a->animnum;
		mv->animnum2 = a->animnum2;
		mv->flip = a->flip;
		mv->flip2 = a->flip2;
		mv->looping = a->looping;
		mv->average = a->average;
		mv->framea = a->framea;
		mv->frameb = a->frameb;
		mv->frame2a = a->frame2a;
		mv->frame2b = a->frame2b;
		mv->frame = a->frame;
		mv->frac = a->frac;
		mv->endframe = a->endframe;
		mv->speed = a->speed;
		mv->newspeed = a->newspeed;
		mv->oldspeed = a->oldspeed;
		mv->timespeed = a->timespeed;
		mv->elapsespeed = a->elapsespeed;
		mv->frame2 = a->frame2;
		mv->frac2 = a->frac2;
		mv->endframe2 = a->endframe2;
		mv->speed2 = a->speed2;
		mv->newspeed2 = a->newspeed2;
		mv->oldspeed2 = a->oldspeed2;
		mv->timespeed2 = a->timespeed2;
		mv->elapsespeed2 = a->elapsespeed2;
		mv->fracmerge = a->fracmerge;
		mv->timemerge = a->timemerge;
		mv->elapsemerge = a->elapsemerge;
		mv->loopframe = a->loopframe;
		mv->loopmerge = a->loopmerge;
		mv->playspeed = a->playspeed;
		mv->newplay = a->newplay;
		mv->oldplay = a->oldplay;
		mv->timeplay = a->timeplay;
		mv->elapseplay = a->elapseplay;
		mv->animscale = a->animscale;

		for (i = 0; i < NETMOVE_HEADWORDS && i < (s32)ARRAYCOUNT(p->bondheadsave); i++) {
			mv->headsave[i] = p->bondheadsave[i];
		}
	}

	mv->swivelpos[0] = p->swivelpos[0];
	mv->swivelpos[1] = p->swivelpos[1];
	mv->insightaimmode = p->insightaimmode ? 1 : 0;
}

// A frame index of an animation's, off the wire: inside the animation
static s32 netPredFrameOk(s32 animnum, s32 frame, s32 numframes)
{
	(void)animnum;
	return frame >= 0 && frame < numframes;
}

// A frame position (f32) of an animation's: a number about its length
static s32 netPredFrameFOk(f32 v, s32 numframes)
{
	return isfinite(v) && v >= -1.f && v <= (f32)numframes + 1.f;
}

// The head's animation and model data in a block, each value one the walk
// can index and draw with (a hostile or broken host's would read past
// g_HeadAnims or past the animation's frames, or put NaN in the eye)
static s32 netPredHeadOk(const struct netmove *mv)
{
	s32 n1;
	s32 n2;
	s32 i;

	if (mv->headanim < 0 || mv->headanim >= (s32)ARRAYCOUNT(g_HeadAnims)) {
		return 0;
	}

	if (mv->animnum < 0 || mv->animnum >= g_NumAnimations || mv->animnum2 < 0 || mv->animnum2 >= g_NumAnimations
			|| !animHasFrames(mv->animnum)) {
		return 0;
	}

	n1 = animGetNumFrames(mv->animnum);

	if (n1 <= 0
			|| !netPredFrameOk(mv->animnum, mv->framea, n1) || !netPredFrameOk(mv->animnum, mv->frameb, n1)
			|| !netPredFrameFOk(mv->frame, n1) || !netPredFrameFOk(mv->endframe, n1) || !netPredFrameFOk(mv->loopframe, n1)) {
		return 0;
	}

	// the merge's second animation, where there is one (animnum2 0 is none:
	// its frames are then stale and never read, model.c)
	if (mv->animnum2 != 0) {
		if (!animHasFrames(mv->animnum2)) {
			return 0;
		}

		n2 = animGetNumFrames(mv->animnum2);

		if (n2 <= 0
				|| !netPredFrameOk(mv->animnum2, mv->frame2a, n2) || !netPredFrameOk(mv->animnum2, mv->frame2b, n2)
				|| !netPredFrameFOk(mv->frame2, n2) || !netPredFrameFOk(mv->endframe2, n2)) {
			return 0;
		}
	}

	// the head model's rwdata: any word that is an infinity or NaN as a
	// float is refused with the rest (its floats are the root's place)
	for (i = 0; i < NETMOVE_HEADWORDS; i++) {
		if ((mv->headsave[i] & 0x7f800000) == 0x7f800000) {
			return 0;
		}
	}

	return 1;
}

// a float off the wire only if it is a number
#define NETF(dst, v) do { const f32 v_ = (v); if (isfinite(v_)) (dst) = v_; } while (0)

// The host's movement state into the player (lvframe60: the level clock the
// command ran on here, for the fall's start)
static void netPredApplyMove(struct player *p, const struct netmove *mv, s32 lvframe60)
{
	NETF(p->speedtheta, mv->speedtheta);
	NETF(p->speedverta, mv->speedverta);
	NETF(p->speedthetacontrol, mv->speedthetacontrol);
	NETF(p->speedsideways, mv->speedsideways);
	NETF(p->speedstrafe, mv->speedstrafe);
	NETF(p->speedforwards, mv->speedforwards);
	NETF(p->speedboost, mv->speedboost);
	NETF(p->speedgo, mv->speedgo);
	p->speedmaxtime60 = mv->speedmaxtime60;
	NETF(p->bondshotspeed.x, mv->shotspeed[0]);
	NETF(p->bondshotspeed.y, mv->shotspeed[1]);
	NETF(p->bondshotspeed.z, mv->shotspeed[2]);
	NETF(p->moveinitspeed.x, mv->moveinitspeed[0]);
	NETF(p->moveinitspeed.y, mv->moveinitspeed[1]);
	NETF(p->moveinitspeed.z, mv->moveinitspeed[2]);
	NETF(p->bondforcespeed.x, mv->forcespeed[0]);
	NETF(p->bondforcespeed.y, mv->forcespeed[1]);
	NETF(p->bondforcespeed.z, mv->forcespeed[2]);
	NETF(p->rollspeed.x, mv->rollspeed[0]);
	NETF(p->rollspeed.y, mv->rollspeed[1]);
	NETF(p->rollspeed.z, mv->rollspeed[2]);
	p->rolltime60 = mv->rolltime60;
	NETF(p->bdeltapos.y, mv->vely);
	NETF(p->sumground, mv->sumground);
	NETF(p->vv_manground, mv->manground);
	NETF(p->vv_ground, mv->ground);
	NETF(p->bondonground, mv->onground);
	p->isfalling = mv->isfalling;
	p->fallstart = mv->isfalling ? lvframe60 - mv->fallage : p->fallstart;
	NETF(p->crouchoffset, mv->crouchoffset);
	NETF(p->crouchspeed, mv->crouchspeed);
	NETF(p->crouchheight, mv->crouchheight);
	NETF(p->crouchfall, mv->crouchfall);
	NETF(p->sumcrouch, mv->sumcrouch);
	NETF(p->crouchoffsetsmall, mv->crouchoffsetsmall);
	p->crouchtime240 = mv->crouchtime240;
	p->crouchoffsetreal = mv->crouchoffsetreal;
	p->crouchoffsetrealsmall = mv->crouchoffsetrealsmall;
	NETF(p->swaytarget, mv->swaytarget);
	NETF(p->swayoffset0, mv->swayoffset0);
	NETF(p->swayoffset2, mv->swayoffset2);
	NETF(p->laddernormal.x, mv->laddernormal[0]);
	NETF(p->laddernormal.y, mv->laddernormal[1]);
	NETF(p->laddernormal.z, mv->laddernormal[2]);
	NETF(p->ladderupdown, mv->ladderupdown);
	NETF(p->liftground, mv->liftground);
	NETF(p->vv_height, mv->height);
	NETF(p->vv_eyeheight, mv->eyeheight);
	NETF(p->gunspeed, mv->gunspeed);
	NETF(p->bondbreathing, mv->breathing);
	NETF(p->headpossum.x, mv->headpossum[0]);
	NETF(p->headpossum.y, mv->headpossum[1]);
	NETF(p->headpossum.z, mv->headpossum[2]);
	p->headwalkingtime60 = mv->headwalkingtime60;

	if (mv->floorroom >= -1 && mv->floorroom < g_Vars.roomcount) {
		p->floorroom = mv->floorroom;
	}

	p->floorflags = mv->floorflags;
	p->floortype = mv->floortype;
	p->onladder = mv->onladder;
	p->inlift = mv->inlift;

	// a lift the host has the player in is a prop this machine may not have
	// mapped: the player stays on its own (or none) until its walk finds it
	if (!p->inlift) {
		p->lift = NULL;
	}

	if (mv->movemode == MOVEMODE_WALK) {
		p->bondmovemode = MOVEMODE_WALK;
	}

	if (mv->crouchpos >= CROUCHPOS_SQUAT && mv->crouchpos <= CROUCHPOS_STAND) {
		p->crouchpos = mv->crouchpos;
	}

	// the head's animation, when it is one this machine has (a death's
	// is never taken: the player lives in every block that gets here)
	if (!netPredHeadOk(mv)) {
		if (s_AnimRefused++ < 4) {
			sysLogPrintf(LOG_WARNING, "net: prediction: a block's head animation not taken (headanim %d, anims %d %d, frames %d %d %d %d, at %g %g %g / %g %g)",
					mv->headanim, mv->animnum, mv->animnum2, mv->framea, mv->frameb, mv->frame2a, mv->frame2b,
					mv->frame, mv->endframe, mv->loopframe, mv->frame2, mv->endframe2);
		}
	} else {
		struct anim *a = &p->unk01c0;
		s32 i;

		a->animnum = mv->animnum;
		a->animnum2 = mv->animnum2;
		a->flip = mv->flip;
		a->flip2 = mv->flip2;
		a->looping = mv->looping;
		a->average = mv->average;
		a->framea = mv->framea;
		a->frameb = mv->frameb;
		a->frame2a = mv->frame2a;
		a->frame2b = mv->frame2b;
		NETF(a->frame, mv->frame);
		NETF(a->frac, mv->frac);
		NETF(a->endframe, mv->endframe);
		NETF(a->speed, mv->speed);
		NETF(a->newspeed, mv->newspeed);
		NETF(a->oldspeed, mv->oldspeed);
		NETF(a->timespeed, mv->timespeed);
		NETF(a->elapsespeed, mv->elapsespeed);
		NETF(a->frame2, mv->frame2);
		NETF(a->frac2, mv->frac2);
		NETF(a->endframe2, mv->endframe2);
		NETF(a->speed2, mv->speed2);
		NETF(a->newspeed2, mv->newspeed2);
		NETF(a->oldspeed2, mv->oldspeed2);
		NETF(a->timespeed2, mv->timespeed2);
		NETF(a->elapsespeed2, mv->elapsespeed2);
		NETF(a->fracmerge, mv->fracmerge);
		NETF(a->timemerge, mv->timemerge);
		NETF(a->elapsemerge, mv->elapsemerge);
		NETF(a->loopframe, mv->loopframe);
		NETF(a->loopmerge, mv->loopmerge);
		NETF(a->playspeed, mv->playspeed);
		NETF(a->newplay, mv->newplay);
		NETF(a->oldplay, mv->oldplay);
		NETF(a->timeplay, mv->timeplay);
		NETF(a->elapseplay, mv->elapseplay);
		NETF(a->animscale, mv->animscale);
		p->headanim = mv->headanim;

		for (i = 0; i < NETMOVE_HEADWORDS && i < (s32)ARRAYCOUNT(p->bondheadsave); i++) {
			p->bondheadsave[i] = mv->headsave[i];
		}
	}

	if (mv->autocrouchpos >= CROUCHPOS_SQUAT && mv->autocrouchpos <= CROUCHPOS_STAND) {
		p->autocrouchpos = mv->autocrouchpos;
	}

	// the aim's crosshair (bmoveProcessInput keeps it in -1..1)
	if (isfinite(mv->swivelpos[0]) && isfinite(mv->swivelpos[1])
			&& fabsf(mv->swivelpos[0]) <= 1.f && fabsf(mv->swivelpos[1]) <= 1.f) {
		p->swivelpos[0] = mv->swivelpos[0];
		p->swivelpos[1] = mv->swivelpos[1];
	}

	p->insightaimmode = mv->insightaimmode ? true : false;
}

static s32 netPredFinite(f32 v)
{
	return isfinite(v) && v > -NETPRED_SANE && v < NETPRED_SANE;
}

// Whether a block can be taken: a position somewhere (never the host's "no
// ground" y of -2^32, open issue (b)) and angles. A movement float that is
// not a number is not taken (netPredApplyMove); one out of any range the walk
// uses may be stale (an unused ladder normal) and is the host's too.
static s32 netPredBlockSane(const struct netlpstate *lp)
{
	s32 i;

	for (i = 0; i < 3; i++) {
		if (!netPredFinite(lp->pos[i])) {
			return 0;
		}
	}

	if (!netPredFinite(lp->theta) || !netPredFinite(lp->verta)) {
		return 0;
	}

	return 1;
}

// The player's prop to a place and rooms (the rooms checked; none fit: found)
static void netPredMove(struct player *p, const f32 *pos, const RoomNum *rooms)
{
	struct prop *prop = p->prop;
	s32 n = 0;
	s32 i;

	propDeregisterRooms(prop);
	prop->pos.x = pos[0];
	prop->pos.y = pos[1];
	prop->pos.z = pos[2];

	if (rooms) {
		for (i = 0; i < 8 && n < 7; i++) {
			if (rooms[i] < 0) {
				break;
			}

			if (rooms[i] > 0 && rooms[i] < g_Vars.roomcount) {
				prop->rooms[n++] = rooms[i];
			}
		}
	}

	prop->rooms[n] = -1;

	if (n == 0) {
		bmoveFindEnteredRooms(p, prop->rooms);
	}

	propRegisterRooms(prop);
}

static f32 netPredWrap(f32 a)
{
	while (a > 180.f) {
		a -= 360.f;
	}

	while (a < -180.f) {
		a += 360.f;
	}

	return a;
}

static void netPredRecordState(struct netpredtick *e, struct player *p)
{
	s32 i;

	e->pos[0] = p->prop->pos.x;
	e->pos[1] = p->prop->pos.y;
	e->pos[2] = p->prop->pos.z;

	for (i = 0; i < 8; i++) {
		e->rooms[i] = p->prop->rooms[i];

		if (p->prop->rooms[i] < 0) {
			break;
		}
	}

	for (; i < 8; i++) {
		e->rooms[i] = -1;
	}

	e->theta = p->vv_theta;
	e->verta = p->vv_verta;
	netPredictCaptureMove(p, &e->mv);
	netPredSaveRich(p, e->rich);
	bwalkNetSide(g_NetLocalSlot, 1, &e->gecrouchhold, &e->geeyelag, &e->geclimbhold);
	geStanNetPlayerTile(g_NetLocalSlot, 1, &e->getile, &e->gefromtile);
	e->hasstate = 1;
}

/*
 * The tick
 */

void netPredictRecordCmd(u32 tick, u32 buttons, s8 sx, s8 sy, s8 rsx, s8 rsy, f32 mdx, f32 mdy, u8 flags)
{
	struct netpredtick *e = netPredSlot(tick);

	if (!e) {
		return;
	}

	e->cmd.buttons = buttons;
	e->cmd.sx = sx;
	e->cmd.sy = sy;
	e->cmd.rsx = rsx;
	e->cmd.rsy = rsy;
	e->cmd.mdx = mdx;
	e->cmd.mdy = mdy;
	e->cmd.flags = flags;
	e->hascmd = 1;
}

void netPredictReplayMouse(f32 *dx, f32 *dy)
{
	// as the host takes it: only with the mouse locked (netMouseDelta)
	const s32 locked = s_ReplayCmd && (s_ReplayCmd->flags & NETCMD_MOUSELOCKED);

	*dx = locked ? s_ReplayCmd->mdx : 0;
	*dy = locked ? s_ReplayCmd->mdy : 0;
}

/**
 * bmoveTick's head on the client: the local player's tick's movement, as a
 * replay must make it again
 */
void netPredictMoveBegin(s32 allowc1x, s32 allowc1y, s32 allowc1buttons, s32 ignorec2)
{
	struct netpredtick *e;
	struct netpredtick *prev;
	struct player *p = g_Vars.currentplayer;
	s32 i;

	if (g_NetReplaying || !s_Ring || g_NetPass < NETPASS_TICK || g_Vars.currentplayernum != g_NetLocalSlot || !p || !p->prop) {
		return;
	}

	e = netPredSlot(g_NetTick);
	e->args[0] = (s8)allowc1x;
	e->args[1] = (s8)allowc1y;
	e->args[2] = (s8)allowc1buttons;
	e->args[3] = (s8)ignorec2;
	e->lvupdate240 = g_Vars.lvupdate240;
	e->lvupdate60 = g_Vars.lvupdate60;
	e->lvupdate60f = g_Vars.lvupdate60f;
	e->lvupdate60freal = g_Vars.lvupdate60freal;
	e->lvframe60 = g_Vars.lvframe60;
	e->lvframenum = g_Vars.lvframenum;
	e->fovy = viGetFovY();
	e->replayable = g_Vars.tickmode == TICKMODE_NORMAL
		&& p->bondmovemode == MOVEMODE_WALK
		&& p->visionmode != VISIONMODE_SLAYERROCKET
		&& p->eyespy == NULL
		&& !p->walkinitmove;
	e->moved = e->replayable && !p->isdead;

	// what moved the player since the last tick's end (a lift)
	e->ext[0] = e->ext[1] = e->ext[2] = 0;
	prev = netPredAt(g_NetTick - 1);

	if (prev && prev->hasstate) {
		f32 d2 = 0;

		e->ext[0] = p->prop->pos.x - prev->pos[0];
		e->ext[1] = p->prop->pos.y - prev->pos[1];
		e->ext[2] = p->prop->pos.z - prev->pos[2];

		for (i = 0; i < 3; i++) {
			d2 += e->ext[i] * e->ext[i];
		}

		if (!(d2 <= NETPRED_EXTMAX * NETPRED_EXTMAX)) {
			e->ext[0] = e->ext[1] = e->ext[2] = 0;
		}
	}
}

/**
 * A frame drawn between ticks (host and client, every player): bmoveTick
 * still runs on it for the picture, at a level clock of zero, but its input
 * handling is not a tick's and would change the walk by however many such
 * frames this machine drew (a held strafe counted again on each). The walk's
 * state is put back as the last tick left it.
 */
static u8 s_PresentSave[NETPRED_RICHMAX];
static struct player *s_PresentPlayer = NULL;

void netPredictPresentBegin(void)
{
	s_PresentPlayer = NULL;

	// (not a dead player's: its death starts its animation on the first walk
	// after it, which may be such a frame, and a death moves no one)
	if (g_NetMode == NETMODE_NONE || g_NetPass != NETPASS_PRESENT_ONLY || !g_Vars.currentplayer
			|| g_Vars.currentplayer->isdead || netPredRichSize() > NETPRED_RICHMAX) {
		return;
	}

	s_PresentPlayer = g_Vars.currentplayer;
	netPredSaveRich(s_PresentPlayer, s_PresentSave);
}

void netPredictPresentEnd(void)
{
	if (s_PresentPlayer && s_PresentPlayer == g_Vars.currentplayer) {
		netPredLoadRich(s_PresentPlayer, s_PresentSave);
	}

	s_PresentPlayer = NULL;
}

// bwalkTryJump on the client: the use press that reached no door, after the
// tick's walk, tried again after the same tick's walk in a replay
void netPredictJumpTry(void)
{
	struct netpredtick *e;

	if (g_NetReplaying || !s_Ring || g_NetPass < NETPASS_TICK || g_Vars.currentplayernum != g_NetLocalSlot) {
		return;
	}

	e = netPredSlot(g_NetTick);
	e->jumptry = 1;
}

/**
 * The host, with --net-predict-log: each remote player after each tick, by
 * the command it played (for laying beside the client's log)
 */
void netPredictHostTickEnd(s32 slot, struct player *p)
{
	s8 sx;
	s8 sy;
	u32 buttons;

	if (!s_LogPath[0] || !p || !p->prop) {
		return;
	}

	if (!s_Log) {
		s_Log = fopen(s_LogPath, "w");

		if (!s_Log) {
			s_LogPath[0] = '\0';
			return;
		}

		setvbuf(s_Log, NULL, _IOLBF, 0);
	}

	buttons = netPlayersHostCurButtons(slot, &sx, &sy);
	fprintf(s_Log, "H %u %d %d %x %d %d %.4f %.4f %.4f %.4f %.4f %.5f %.5f %d %d %d %.4f %.4f %.3f %.3f\n", g_NetTick, slot, netPlayersHostLastPlayed(slot),
			buttons, sx, sy, p->prop->pos.x, p->prop->pos.y, p->prop->pos.z, p->vv_theta, p->vv_verta,
			p->speedstrafe, p->speedgo, p->crouchpos, p->insightaimmode, joyGetNumSamples(),
			p->swivelpos[0], p->swivelpos[1], p->aspect, viGetFovY());
}

// The client's tick has run: its player as the tick left it
void netPredictTickEnd(void)
{
	struct player *p = netPredLocal();
	struct netpredtick *e;
	s32 i;

	if (!s_Ring || !p) {
		return;
	}

	e = netPredSlot(g_NetTick);
	netPredRecordState(e, p);

	if (s_StartTick == 0) {
		s_StartTick = g_NetTick ? g_NetTick : 1;
	}

	if (s_Log) {
		fprintf(s_Log, "T %u %x %d %d %.3f %.4f %.4f %.4f %.4f %.4f %d %.3f %.3f %.3f %.5f %.5f %d %d %d %.4f %.4f %.3f %.3f\n", g_NetTick,
				e->hascmd ? e->cmd.buttons : 0, e->hascmd ? e->cmd.sx : 0, e->hascmd ? e->cmd.sy : 0,
				e->hascmd ? e->cmd.mdx : 0.f, e->pos[0], e->pos[1], e->pos[2], e->theta, e->verta, e->moved,
				s_EyeOff[0], s_EyeOff[1], s_EyeOff[2], p->speedstrafe, p->speedgo, p->crouchpos, p->insightaimmode, joyGetNumSamples(),
				p->swivelpos[0], p->swivelpos[1], p->aspect, viGetFovY());
	}

	// the eased eye's offset and turn, a tick's worth smaller
	for (i = 0; i < 3; i++) {
		s_EyeOff[i] *= NETPRED_EASE;

		if (s_EyeOff[i] > -0.01f && s_EyeOff[i] < 0.01f) {
			s_EyeOff[i] = 0;
		}
	}

	for (i = 0; i < 2; i++) {
		s_AngOff[i] *= NETPRED_EASE;

		if (s_AngOff[i] > -0.005f && s_AngOff[i] < 0.005f) {
			s_AngOff[i] = 0;
		}
	}

	if (g_NetTick % 600 == 0 && g_NetTick) {
		netPredictLog("so far");
	}

	// the tests' pictures, taken by the game itself (a debugger's stop would
	// starve the host of commands)
	for (i = 0; i < s_NumShots; i++) {
		if (s_ShotTicks[i] == g_NetTick) {
			sysLogPrintf(LOG_NOTE, "net: prediction: screenshot at tick %u", g_NetTick);
			screenshotRequest();
		}
	}
}

/*
 * The replay
 */

static void netPredInject(const struct netpredcmd *c)
{
	OSContPad pads[MAXCONTROLLERS];
	const s32 pad = netPlayersLocalPad();

	memset(pads, 0, sizeof(pads));

	if (c && pad >= 0 && pad < MAXCONTROLLERS) {
		pads[pad].button = c->buttons;
		pads[pad].stick_x = c->sx;
		pads[pad].stick_y = c->sy;
		pads[pad].rstick_x = c->rsx;
		pads[pad].rstick_y = c->rsy;
	}

	joyNetInjectSample(pads);
}

/**
 * The player in the host's state after command n, then commands n+1 up to
 * this tick's played again. Returns the ticks played. abs: a respawn or
 * teleport, the player as this machine has it now (just respawned: its walk
 * as playerStartNewLife left it) under the host's, never the ring's at n
 * (a dead player's); the ticks this machine ran dead, which the host ran
 * alive after the respawn, are played too.
 */
static struct mpplayerconfig s_SaveConfigs[MAX_MPPLAYERCONFIGS];
static struct activemenu s_SaveAmMenus[MAX_PLAYERS];
static u32 s_ReplaySkipped = 0; // the last replay's newest tick it could not play (0 none)

static s32 netPredReplay(struct player *p, const struct netlpstate *lp, u32 n, s32 abs)
{
	struct netpredtick *start = netPredAt(n);
	struct chrdata *chr = p->prop->chr;
	const s32 lvupdate240 = g_Vars.lvupdate240;
	const s32 lvupdate60 = g_Vars.lvupdate60;
	const f32 lvupdate60f = g_Vars.lvupdate60f;
	const f32 lvupdate60freal = g_Vars.lvupdate60freal;
	const s32 lvframe60 = g_Vars.lvframe60;
	const s32 lvframenum = g_Vars.lvframenum;
	const s32 enableslopes = g_Vars.enableslopes;
	const s32 remotepass = g_NetRemotePass;
	const s32 passplayer = g_NetPassPlayer;
	const f32 fovy = viGetFovY();
	const s32 amindex = g_AmIndex;
	const bool withcontrol = g_PlayersWithControl[g_NetLocalSlot];
	const u8 footstep = chr->footstep;
	const u8 floortype = chr->floortype;
	s32 played = 0;
	u32 t;

	s_ReplaySkipped = 0;
	memcpy(s_SavePlayer, p, sizeof(*p));
	joyNetSave(s_JoySave);
	// what the replayed input may touch outside the player (a gun function
	// toggled, the active menu): the live ticks did it once already
	memcpy(s_SaveConfigs, g_PlayerConfigsArray, sizeof(s_SaveConfigs));
	memcpy(s_SaveAmMenus, g_AmMenus, sizeof(s_SaveAmMenus));

	// the state after n: this machine's own, the host's over it
	if (!abs && start && start->hasstate) {
		netPredLoadRich(p, start->rich);
		bwalkNetSide(g_NetLocalSlot, 0, &start->gecrouchhold, &start->geeyelag, &start->geclimbhold);
		geStanNetPlayerTile(g_NetLocalSlot, 0, &start->getile, &start->gefromtile);
	}

	netPredApplyMove(p, &lp->mv, start && start->hasstate ? start->lvframe60 : g_Vars.lvframe60);
	p->vv_theta = lp->theta;
	p->vv_verta = lp->verta;
	netPredMove(p, lp->pos, lp->rooms);

	// what the walk derives from the state, for when nothing is replayed
	bmoveUpdateVerta();
	bwalkUpdatePrevPos();
	bmove0f0cc19c(&p->prop->pos);
	playerUpdatePerimInfo();

	if (start) {
		netPredRecordState(start, p);
	}

	g_NetReplaying = 1;
	g_NetRemotePass = 1;
	g_NetPassPlayer = -1;

	// the sample before n+1's, for its presses
	netPredInject(start && start->hascmd ? &start->cmd : NULL);

	for (t = n + 1; t != g_NetTick; t++) {
		struct netpredtick *e = netPredAt(t);

		if (!e) {
			break;
		}

		netPredInject(e->hascmd ? &e->cmd : NULL);

		if (e->hascmd && (e->moved || (abs && e->replayable && e->lvupdate240 > 0))) {
			g_Vars.lvupdate240 = e->lvupdate240;
			g_Vars.lvupdate60 = e->lvupdate60;
			g_Vars.lvupdate60f = e->lvupdate60f;
			g_Vars.lvupdate60freal = e->lvupdate60freal;
			g_Vars.lvframe60 = e->lvframe60;
			g_Vars.lvframenum = e->lvframenum;

			// the zoom the tick's turn was scaled by (bmoveProcessInput
			// tweens it on from there, as it did)
			if (e->fovy > 0 && e->fovy < 180.f) {
				viSetFovY(e->fovy);
			}

			if (e->ext[0] != 0 || e->ext[1] != 0 || e->ext[2] != 0) {
				p->prop->pos.x += e->ext[0];
				p->prop->pos.y += e->ext[1];
				p->prop->pos.z += e->ext[2];
			}

			s_ReplayCmd = &e->cmd;

			if (e->moved) {
				bmoveTick(e->args[0], e->args[1], e->args[2], e->args[3]);
			} else {
				// a tick run dead here: the host's player walked it as any
				// living player's, and so does every later replay of it
				e->args[0] = e->args[1] = e->args[2] = 1;
				e->args[3] = 0;
				e->moved = 1;
				bmoveTick(1, 1, 1, 0);
			}

			s_ReplayCmd = NULL;

			if (e->jumptry) {
				bwalkTryJump();
			}

			played++;
		} else if (e->hascmd) {
			s_ReplaySkipped = t;
		}

		netPredRecordState(e, p);
	}

	g_NetReplaying = 0;
	g_NetRemotePass = remotepass;
	g_NetPassPlayer = passplayer;

	// everything but the movement as it was: the gun, the HUD, the hands
	// never saw the replay
	netPredSaveRich(p, s_RichTmp);
	memcpy(p, s_SavePlayer, sizeof(*p));
	netPredLoadRich(p, s_RichTmp);

	joyNetRestore(s_JoySave);
	memcpy(g_PlayerConfigsArray, s_SaveConfigs, sizeof(s_SaveConfigs));
	memcpy(g_AmMenus, s_SaveAmMenus, sizeof(s_SaveAmMenus));
	g_AmIndex = amindex;
	g_PlayersWithControl[g_NetLocalSlot] = withcontrol;
	g_Vars.lvupdate240 = lvupdate240;
	g_Vars.lvupdate60 = lvupdate60;
	g_Vars.lvupdate60f = lvupdate60f;
	g_Vars.lvupdate60freal = lvupdate60freal;
	g_Vars.lvframe60 = lvframe60;
	g_Vars.lvframenum = lvframenum;
	g_Vars.enableslopes = enableslopes;
	chr->footstep = footstep;
	chr->floortype = floortype;

	if (viGetFovY() != fovy) {
		viSetFovY(fovy);
	}

	s_Replayed += played;

	return played;
}

/**
 * The pose step (netEntsClientApplyLocal): the newest local-player block
 * against what this machine had after the same command
 */
s32 netPredictReconcile(struct player *p, const struct netlpstate *lp, u32 cmd, s32 abs)
{
	struct netpredtick *e;
	f32 oldpos[3];
	f32 oldtheta;
	f32 oldverta;
	f32 shift[3];
	f32 dist;
	f32 err = 0;
	s32 played;
	s32 i;

	if (!s_Ring || !p || !p->prop || p->isdead || (lp->flags & NETLP_DEAD)) {
		return 0;
	}

	if (!netPredBlockSane(lp)) {
		if (s_Refused++ < 4) {
			sysLogPrintf(LOG_WARNING, "net: prediction: a block with no place or bad angles not taken (command %u, %.0f %.0f %.0f)",
					cmd, lp->pos[0], lp->pos[1], lp->pos[2]);
		}

		return 0;
	}

	if (cmd == 0xffffffff) {
		return 0;
	}

	// a command this machine has not run yet (a broken or hostile host, a
	// wrapped number): nothing to compare, and never the mark the next
	// blocks are measured from, or one such block would stop every later
	// correction
	if (cmd >= g_NetTick) {
		if (s_Future++ < 4) {
			sysLogPrintf(LOG_WARNING, "net: prediction: a block for command %u, not yet run here (tick %u): not taken", cmd, g_NetTick);
		}

		return 0;
	}

	if (!abs && s_HaveLast && cmd <= s_LastCmd) {
		// nothing played since the last block
		return 0;
	}

	if (!abs && cmd < s_RespawnFloor) {
		// the host's run of the ticks this machine spent dead before its
		// respawn reached here: compared from the respawn's tick on
		return 0;
	}

	if (g_NetTick - cmd > NETPRED_RING - 8) {
		// older than the ring holds: nothing to replay from; a respawn or
		// teleport is still taken as it is
		s_Overrun++;

		if (abs) {
			netPredMove(p, lp->pos, lp->rooms);
			p->vv_theta = lp->theta;
			s_EyeOff[0] = s_EyeOff[1] = s_EyeOff[2] = 0;
			s_AngOff[0] = s_AngOff[1] = 0;
			s_AbsSnaps++;
			s_RespawnFloor = g_NetTick - 1;
		}

		s_HaveLast = 1;
		s_LastCmd = cmd;
		return abs ? 1 : 0;
	}

	s_HaveLast = 1;
	s_LastCmd = cmd;
	e = netPredAt(cmd);

	if (!abs) {
		s32 posoff;
		s32 angoff;
		s32 discoff;
		f32 dx;
		f32 dy;
		f32 dz;

		if (!e || !e->hasstate) {
			s_Missing++;
			return 0;
		}

		dx = lp->pos[0] - e->pos[0];
		dy = lp->pos[1] - e->pos[1];
		dz = lp->pos[2] - e->pos[2];
		err = sqrtf(dx * dx + dy * dy + dz * dz);
		posoff = !(err <= NETPRED_POSEPS);
		angoff = fabsf(netPredWrap(lp->theta - e->theta)) > NETPRED_ANGEPS || fabsf(lp->verta - e->verta) > NETPRED_ANGEPS;
		discoff = lp->mv.movemode != e->mv.movemode || lp->mv.onladder != e->mv.onladder
			|| lp->mv.isfalling != e->mv.isfalling || lp->mv.crouchpos != e->mv.crouchpos
			|| lp->mv.autocrouchpos != e->mv.autocrouchpos || lp->mv.insightaimmode != e->mv.insightaimmode;

		s_Compared++;

		if (s_Log) {
			fprintf(s_Log, "C %u %u %.4f %d %d %d %.4f %.4f %.4f\n", cmd, g_NetTick, err, posoff, angoff, discoff,
					lp->pos[0], lp->pos[1], lp->pos[2]);

			if (s_LogDiff) {
				// which movement words differ (offsets into struct netmove)
				const u8 *a = (const u8 *)&lp->mv;
				const u8 *b = (const u8 *)&e->mv;
				u32 o;

				fprintf(s_Log, "D %u", cmd);

				for (o = 0; o + 4 <= sizeof(struct netmove); o += 4) {
					if (memcmp(a + o, b + o, 4) != 0) {
						f32 fa;
						f32 fb;

						memcpy(&fa, a + o, 4);
						memcpy(&fb, b + o, 4);
						fprintf(s_Log, " %u:%g/%g", o, fa, fb);
					}
				}

				fprintf(s_Log, "\n");
			}
		}

		if (!posoff && !angoff && !discoff) {
			s_Matched++;
			return 0;
		}

		if (!posoff) {
			if (angoff) {
				s_AngleOff++;
			} else {
				s_DiscreteOff++;
			}
		}
	}

	oldpos[0] = p->prop->pos.x;
	oldpos[1] = p->prop->pos.y;
	oldpos[2] = p->prop->pos.z;
	oldtheta = p->vv_theta;
	oldverta = p->vv_verta;

	played = netPredReplay(p, lp, cmd, abs);

	for (i = 0; i < 3; i++) {
		shift[i] = oldpos[i] - (&p->prop->pos.x)[i];
	}

	dist = sqrtf(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);

	if (abs) {
		s_AbsSnaps++;
		s_EyeOff[0] = s_EyeOff[1] = s_EyeOff[2] = 0;
		s_AngOff[0] = s_AngOff[1] = 0;

		// ticks run dead here that the replay could not play: the host's
		// blocks for them are not this machine's to compare with
		s_RespawnFloor = s_ReplaySkipped ? s_ReplaySkipped + 1 : 0;
	} else {
		f32 dtheta = netPredWrap(oldtheta - p->vv_theta);
		f32 dverta = oldverta - p->vv_verta;

		s_Corrections++;
		s_ErrSum += err;
		s_ShiftSum += dist;

		if (err > s_ErrMax) {
			s_ErrMax = err;
		}

		if (dist > s_ShiftMax) {
			s_ShiftMax = dist;
		}

		if (dist > NETPRED_SNAP) {
			s_Snaps++;
			s_EyeOff[0] = s_EyeOff[1] = s_EyeOff[2] = 0;
			s_AngOff[0] = s_AngOff[1] = 0;

			if (s_Snaps < 8) {
				sysLogPrintf(LOG_NOTE, "net: prediction: a correction of %.1f at command %u (tick %u) taken at once", dist, cmd, g_NetTick);
			}
		} else {
			for (i = 0; i < 3; i++) {
				s_EyeOff[i] += shift[i];
			}

			if (s_EyeOff[0] * s_EyeOff[0] + s_EyeOff[1] * s_EyeOff[1] + s_EyeOff[2] * s_EyeOff[2] > NETPRED_SNAP * NETPRED_SNAP) {
				s_EyeOff[0] = s_EyeOff[1] = s_EyeOff[2] = 0;
			}

			// the view's turn eased the same way (presentation only: the
			// player, its aim and its shots have the corrected angles)
			s_AngOff[0] += dtheta;
			s_AngOff[1] += dverta;

			if (fabsf(s_AngOff[0]) > NETPRED_ANGSNAP || fabsf(s_AngOff[1]) > NETPRED_ANGSNAP) {
				s_AngOff[0] = s_AngOff[1] = 0;
			}
		}
	}

	if (s_Log) {
		fprintf(s_Log, "R %u %u %.4f %.4f %d %d\n", cmd, g_NetTick, err, dist, played, abs);
	}

	return 1;
}

/*
 * The view
 */

// v turned by a degrees about the world's up, theta's way round
static void netPredYaw(struct coord *v, f32 a)
{
	const f32 r = a * 0.017453292f;
	const f32 c = cosf(r);
	const f32 s = sinf(r);
	const f32 x = v->x;
	const f32 z = v->z;

	v->x = x * c - z * s;
	v->z = x * s + z * c;
}

// v turned by r radians about the unit axis k
static void netPredRotate(struct coord *v, const f32 *k, f32 r)
{
	const f32 c = cosf(r);
	const f32 s = sinf(r);
	const f32 d = k[0] * v->x + k[1] * v->y + k[2] * v->z;
	const f32 x = v->x;
	const f32 y = v->y;
	const f32 z = v->z;

	v->x = x * c + (k[1] * z - k[2] * y) * s + k[0] * d * (1 - c);
	v->y = y * c + (k[2] * x - k[0] * z) * s + k[1] * d * (1 - c);
	v->z = z * c + (k[0] * y - k[1] * x) * s + k[2] * d * (1 - c);
}

// The view turned by dtheta (theta's way) and dverta degrees (up positive)
static void netPredTurnView(struct coord *up, struct coord *look, f32 dtheta, f32 dverta)
{
	if (dverta != 0) {
		// about the view's right: up with dverta positive
		f32 k[3];
		f32 len;
		f32 r = dverta * 0.017453292f;
		struct coord test = *look;

		k[0] = up->y * look->z - up->z * look->y;
		k[1] = up->z * look->x - up->x * look->z;
		k[2] = up->x * look->y - up->y * look->x;
		len = sqrtf(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);

		if (len > 0.0001f) {
			k[0] /= len;
			k[1] /= len;
			k[2] /= len;
			netPredRotate(&test, k, r);

			if (test.y < look->y) {
				r = -r;
			}

			netPredRotate(look, k, r);
			netPredRotate(up, k, r);
		}
	}

	if (dtheta != 0) {
		netPredYaw(look, dtheta);
		netPredYaw(up, dtheta);
	}
}

void netPredictView(struct coord *eye, struct coord *up, struct coord *look)
{
	struct player *p = g_Vars.currentplayer;
	f32 ease = 1;
	s32 rdx = 0;
	s32 rdy = 0;

	if (!s_Ring || !p || p->isdead || !NET_CLIENT) {
		return;
	}

	// a frame drawn between ticks is that much of a tick later
	if (g_NetPass == NETPASS_PRESENT_ONLY) {
		ease = powf(NETPRED_EASE, frametimeNetAlpha());
	}

	eye->x += s_EyeOff[0] * ease;
	eye->y += s_EyeOff[1] * ease;
	eye->z += s_EyeOff[2] * ease;

	// a correction's turn, eased out like the eye's move
	if (s_AngOff[0] != 0 || s_AngOff[1] != 0) {
		f32 dverta = s_AngOff[1] * ease;

		if (dverta + p->vv_verta > 90.f) {
			dverta = 90.f - p->vv_verta;
		} else if (dverta + p->vv_verta < -90.f) {
			dverta = -90.f - p->vv_verta;
		}

		netPredTurnView(up, look, s_AngOff[0] * ease, dverta);
	}

	// the mouse moved since the last tick, turned in as that tick's walk
	// will turn it (bmoveProcessInput: the scaled delta times 3.5 degrees,
	// by the fov) - on a frame drawn between ticks only, and not while
	// aiming, where the mouse moves the crosshair instead
	if (g_NetPass != NETPASS_PRESENT_ONLY || !inputMouseIsLocked() || p->insightaimmode) {
		return;
	}

	netPendingMouse(&rdx, &rdy);

	if (rdx || rdy) {
		f32 sx;
		f32 sy;
		f32 fovscale = viGetFovY() / PLAYER_DEFAULT_FOV;
		f32 dtheta;
		f32 dverta;

		inputMouseGetSpeed(&sx, &sy);
		dtheta = rdx * 0.022f * sx * fovscale;
		dverta = -rdy * 0.022f * sy * fovscale * (optionsGetForwardPitch(g_Vars.currentplayerstats->mpindex) ? 1.f : -1.f);

		if (dverta + p->vv_verta > 90.f) {
			dverta = 90.f - p->vv_verta;
		} else if (dverta + p->vv_verta < -90.f) {
			dverta = -90.f - p->vv_verta;
		}

		netPredTurnView(up, look, dtheta, dverta);
	}
}
