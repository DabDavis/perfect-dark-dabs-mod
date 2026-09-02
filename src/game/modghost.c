#include <ultra64.h>
#include "constants.h"
#include "game/body.h"
#include "game/bondgun.h"
#include "game/bondmove.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/game_0b0fd0.h"
#include "game/modelmgr.h"
#include "game/lang.h"
#include "game/mplayer/mplayer.h"
#include "game/modghost.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/bg.h"
#include "game/game_1531a0.h"
#include "lib/vi.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/model.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"
#include "fs.h"
#include "system.h"
#include "versioninfo.h"
#include "ghostnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * Ghost Time Trial. See modghost.h for the format and what is stored.
 *
 * Three things happen here and they are independent of each other: a run is
 * sampled while it is played, a ghost read off disk is replayed into a body,
 * and a completed run is written out if it was the best one. Recording does
 * not care whether there is a ghost to race, and racing does not care whether
 * this run will be worth keeping.
 *
 * The body is a chr of its own rather than anything the player owns, built the
 * way modbodies.c builds one: a model from the body pool and a prop from
 * chrAllocate(). It is posed each frame from the recording and then ticked
 * like any other chr, which is what advances its animation and builds its
 * matrices. Posing before the tick rather than after is not a detail - chrTick()
 * positions the model from prop->pos as it runs, so a position written
 * afterwards would be a position nothing had drawn from.
 *
 * That is also why one frame of the animation's own root motion is left in:
 * the body drifts a few units off the recorded spot during its own tick and is
 * put back at the start of the next one. playerTickThirdPerson() does exactly
 * this to a player's body in multiplayer, for the same reason, and it is what
 * makes the feet look planted rather than skated.
 */

// The two structs below are written to disk verbatim and read back on another
// machine, so their size is part of the format rather than an implementation
// detail. Both are laid out so that natural alignment gives the same answer
// everywhere; these say so out loud, because a field added in the middle would
// otherwise silently produce files nothing else can read.
_Static_assert(sizeof(struct modghostsample) == 24, "ghost sample layout is on-disk format");
_Static_assert(sizeof(struct modghostheader) == 128, "ghost header layout is on-disk format");

s32 g_ModGhostMode = MODGHOST_OFF;

/**
 * Whether this mission was started from Ghost Trials.
 *
 * A trial records and races whatever the saved setting says, because that is
 * what the player asked for by coming through that door. It is a flag rather
 * than a write to the setting above, which is saved to pd.ini: one mission
 * played as a trial must not leave every later mission recording.
 *
 * It stays armed across a retry of the same mission, which is the point of a
 * time trial, and is disarmed by the ordinary Solo Missions item on the way
 * past.
 */
static bool g_ModGhostTrial = false;

void modGhostArmTrial(void)
{
	g_ModGhostTrial = true;
}

void modGhostDisarmTrial(void)
{
	g_ModGhostTrial = false;
}

/**
 * Whether the moves this fork added are switched off right now.
 *
 * A trial is a time to be compared against other times, and a run made with a
 * jump the ghost beside it did not have is not the same run. The fork's own
 * settings are left alone - this overrides them for the duration rather than
 * writing to them, so a player who came in through Ghost Trials for one
 * mission gets their jump back afterwards without having to remember to.
 *
 * The multiplayer test is here because the armed flag outlives the mission: it
 * is meant to survive a retry, and is only cleared by the ordinary Solo
 * Missions item on the way past. Without this, arming a trial and then opening
 * the Combat Simulator would leave everyone in the arena unable to jump, which
 * is a strange bug to arrive at from a leaderboard.
 */
bool modGhostTrialRulesApply(void)
{
	return g_ModGhostTrial && g_Vars.normmplayerisrunning == false;
}

/**
 * The mode actually in force, which is nothing at all outside a trial.
 *
 * Recording used to happen in any mission with the setting on, and the runs it
 * produced went to the same leaderboard as the ones set under trial rules -
 * with a jump, against guards that could jump, on a route nobody else could
 * take. A board that mixes those is measuring settings rather than driving.
 *
 * So a run is recorded when it is a trial and not otherwise, and the setting
 * chooses what a trial does rather than which missions record. Somebody who
 * wants to play the game with the fork's moves on plays Solo Missions, which
 * is the door that means exactly that.
 *
 * The same predicate that switches the moves off decides this, so the two can
 * never disagree about whether a run counts - including the multiplayer case,
 * where an armed trial that outlived its mission must not start recording a
 * Combat Simulator match.
 */
static s32 modGhostGetMode(void)
{
	if (!modGhostTrialRulesApply()) {
		return MODGHOST_OFF;
	}

	return g_ModGhostMode < MODGHOST_RECORD ? MODGHOST_RECORD : g_ModGhostMode;
}
s32 g_ModGhostPick = MODGHOSTPICK_FASTEST;

/**
 * Who the player is in a trial, as a Combat Simulator body index plus one.
 *
 * Zero is Joanna, which is what solo has always given and what somebody who
 * never opens the picker keeps.
 */
s32 g_ModGhostBody = MODGHOST_BODY_DEFAULT;

/**
 * The head, separately, because the picker offers it separately.
 *
 * Zero means whichever head the body comes with, which is what choosing a body
 * alone should give and what a run recorded before heads were pickable has.
 */
s32 g_ModGhostHead = MODGHOST_BODY_DEFAULT;

/**
 * Turn a stored character into the body and head the model loader wants.
 *
 * Returns false for the default, which is the caller's cue to leave whatever
 * the game would have chosen alone rather than to substitute something.
 *
 * The index is clamped against the table rather than trusted: it arrives from
 * pd.ini or from a file somebody else wrote, and mpGetBodyId() reads one past
 * the end of its array for the value just above the last valid one - a bug in
 * the decomp that is documented where it lives and cheaper to avoid than to
 * fix under a matching build.
 */
static bool modGhostResolveCharacter(s32 storedbody, s32 storedhead, s32 *bodynum, s32 *headnum)
{
	s32 body = storedbody - 1;
	s32 head = storedhead - 1;

	if (storedbody <= MODGHOST_BODY_DEFAULT || body >= (s32)mpGetNumBodies()) {
		return false;
	}

	*bodynum = mpGetBodyId(body);

	// A head of its own if one was chosen and the table still has it, else the
	// one the body was built with. Heads past mpGetNumHeads2() are the ones the
	// arena treats as a face rather than a head model, and asking mpGetHeadId()
	// for those returns something that does not belong on this body.
	if (storedhead > MODGHOST_BODY_DEFAULT && head < mpGetNumHeads2()) {
		*headnum = mpGetHeadId(head);
	} else {
		*headnum = mpGetHeadId(mpGetMpheadnumByMpbodynum(body));
	}

	return true;
}

/**
 * The character this trial is being played as, for the player's own body.
 *
 * Only during a trial: the picker is a Ghost Trials setting and a mission
 * started any other way is the campaign, where Joanna is who you are.
 */
bool modGhostGetTrialCharacter(s32 *bodynum, s32 *headnum)
{
	if (!modGhostTrialRulesApply()) {
		return false;
	}

	return modGhostResolveCharacter(g_ModGhostBody, g_ModGhostHead, bodynum, headnum);
}

// How solid the ghost is drawn, out of 255. Low enough to read as not really
// there, high enough to follow across a lit room.
s32 g_ModGhostAlpha = 110;

// Whether the split against the ghost is drawn under the mission timer.
s32 g_ModGhostSplits = 1;

/**
 * How far either side of the last matched sample the split looks for the
 * ghost's position on the route.
 *
 * The split is "how long did the ghost take to reach where I am", so it needs
 * the sample nearest the player, and the honest way to find it is to search
 * the whole run. A window is used instead because the answer moves forward at
 * about the speed the run did: eight seconds of samples either side absorbs a
 * detour or a lift without letting a route that crosses itself match the wrong
 * lap.
 */
#define MODGHOST_SPLITWINDOW (8 * 60 / MODGHOST_RATE60)

// Beyond this the player is somewhere the ghost never went, and there is no
// honest split to show. Squared, in game units.
#define MODGHOST_SPLITMAXDISTSQ (700.0f * 700.0f)

/**
 * What the rest of the level is owed before a ghost may take a body.
 *
 * The same line modbodies.c draws, for the same reason: a ghost is not worth
 * the last of MEMPOOL_STAGE. Past this point the field is simply smaller than
 * was asked for, which is a feature quietly doing less rather than a mission
 * that will not load.
 */
#define MODGHOST_MEMFLOOR (256 * 1024)

// What the recording buffer grows by, in samples. Twenty seconds at a time.
#define MODGHOST_GROWBY (20 * 60 / MODGHOST_RATE60)

// The run being recorded.
static struct modghostsample *g_ModGhostRec = NULL;
static s32 g_ModGhostRecCount = 0;
static s32 g_ModGhostRecCap = 0;
static bool g_ModGhostRecFull = false;

/**
 * One ghost being raced: the run it is replaying and the body replaying it.
 *
 * The samples are this racer's own allocation; the chr, its number and the
 * weapon it is holding are the body, which comes and goes as the run starts
 * and finishes. angleoffset is the lean the animation chooser asks for and is
 * per body rather than shared, which is the whole reason it is in here - ten
 * ghosts strafing share nothing about how they are leaning.
 */
struct modghostracer {
	struct modghostsample *samples;
	s32 count;
	s32 rate60;
	s32 time60;
	char name[MODGHOST_NAMELEN];
	s32 body;                 // stored character, MODGHOST_BODY_DEFAULT for Joanna
	s32 head;
	struct chrdata *chr;
	s32 chrnum;
	s32 weaponheld;
	f32 angleoffset;
};

static struct modghostracer g_ModGhostRacers[MODGHOST_MAXRACERS];
static s32 g_ModGhostNumRacers = 0;

/**
 * The body modeldefs, shared by every ghost wearing the same character.
 *
 * The first body of a character loads them; every later one is handed the same
 * pair. Ten loads would be ten copies of one head out of MEMPOOL_STAGE, never
 * freed, which is the trap CLAUDE.md names and modbodies.c already had to
 * climb out of.
 *
 * Keyed by character rather than being one shared pair, since ghosts stopped
 * all being Joanna. A field of ten runs by ten people costs ten loads because
 * it is ten different models; a field of one person's ten attempts costs one,
 * which is the case that made sharing worth doing in the first place.
 *
 * They belong to the stage, so they are dropped rather than freed when it ends.
 */
struct modghostbodydef {
	s32 bodynum;
	struct modeldef *bodydef;
	struct modeldef *headdef;
};

static struct modghostbodydef g_ModGhostBodyDefs[MODGHOST_MAXRACERS];
static s32 g_ModGhostNumBodyDefs = 0;

// What the ghost on hand was loaded for, so that a stage entered twice is not
// read off disk twice and a stage that has no ghost is not scanned for one
// every frame.
//
// The two settings are part of it as well as the stage, because both of them
// choose a different file: turning racing on, or switching between the fastest
// ghost and your own, has to be able to take effect without leaving the
// mission it was changed from.
static s32 g_ModGhostLoadedStage = -1;
static s32 g_ModGhostLoadedDiff = -1;
static s32 g_ModGhostLoadedMode = -1;
static s32 g_ModGhostLoadedPick = -1;
static s32 g_ModGhostLoadedMax = -1;

// The split against the ghost, and the sample it was measured at.
static s32 g_ModGhostSplitIdx = 0;
static s32 g_ModGhostSplit60 = 0;
static bool g_ModGhostSplitValid = false;

/**
 * Whether this is a stage a ghost means anything on.
 *
 * Solo missions only: a Combat Sim match has no route and no clock to race,
 * and the Institute is not a mission. The player has to exist and be alive -
 * playerGetMissionTime() reads through currentplayer - and there has to be one
 * of them, because a ghost belongs to a run and splitscreen has two.
 */
static bool modGhostStageIsEligible(void)
{
	if (g_Vars.mplayerisrunning || g_Vars.normmplayerisrunning) {
		return false;
	}

	if (g_Vars.stagenum >= STAGE_TITLE || g_Vars.stagenum == STAGE_CITRAINING) {
		return false;
	}

	if (PLAYERCOUNT() != 1 || g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0) {
		return false;
	}

	if (g_Vars.currentplayer == NULL || g_Vars.currentplayer->prop == NULL) {
		return false;
	}

	// Not during a cutscene. The mission clock is stopped, so there is nothing
	// to record against and nothing for a ghost to be ahead or behind of, and
	// the player's prop is wherever the camera work has left it rather than
	// where the run is. A translucent second Joanna standing in the back of
	// the briefing is the visible half of the same problem.
	if (g_Vars.in_cutscene) {
		return false;
	}

	return true;
}

bool modGhostIsChr(struct chrdata *chr)
{
	s32 i;

	if (chr == NULL) {
		return false;
	}

	for (i = 0; i < g_ModGhostNumRacers; i++) {
		if (g_ModGhostRacers[i].chr == chr) {
			return true;
		}
	}

	return false;
}

s32 modGhostGetAlpha(void)
{
	if (g_ModGhostAlpha < 8) {
		return 8;
	}

	if (g_ModGhostAlpha > 254) {
		return 254;
	}

	return g_ModGhostAlpha;
}

bool modGhostIsRacing(void)
{
	return modGhostGetMode() == MODGHOST_RACE && g_ModGhostNumRacers > 0;
}

s32 modGhostGetNumRacers(void)
{
	return g_ModGhostNumRacers;
}

/**
 * The ghost the split is measured against, which is the quickest of the field.
 *
 * With one ghost there is no choice to make. With ten there is, and the answer
 * is the one that finished first: a split is what you are chasing, and the
 * board is won against the best run present rather than against whichever of
 * them happened to load first.
 */
static struct modghostracer *modGhostGetTarget(void)
{
	struct modghostracer *best = NULL;
	s32 i;

	for (i = 0; i < g_ModGhostNumRacers; i++) {
		struct modghostracer *racer = &g_ModGhostRacers[i];

		if (racer->samples == NULL) {
			continue;
		}

		if (best == NULL || racer->time60 < best->time60) {
			best = racer;
		}
	}

	return best;
}

/**
 * FNV-1a over the sample block.
 *
 * It is a completeness check for something that has crossed a network, not a
 * signature. A file whose samples do not hash to what its header claims was
 * truncated or corrupted in transit and is not replayed; a file whose samples
 * were edited by someone who also fixed the hash is indistinguishable from a
 * real run, and nothing running on the player's own machine could tell the
 * difference anyway.
 */
static u32 modGhostHash(const void *data, u32 len)
{
	const u8 *p = data;
	u32 hash = 2166136261u;
	u32 i;

	for (i = 0; i < len; i++) {
		hash ^= p[i];
		hash *= 16777619u;
	}

	return hash;
}

static void modGhostFreeRecording(void)
{
	if (g_ModGhostRec) {
		free(g_ModGhostRec);
		g_ModGhostRec = NULL;
	}

	g_ModGhostRecCount = 0;
	g_ModGhostRecCap = 0;
	g_ModGhostRecFull = false;
}

static void modGhostFreeRacer(struct modghostracer *racer)
{
	if (racer->samples) {
		free(racer->samples);
		racer->samples = NULL;
	}

	racer->count = 0;
	racer->time60 = 0;
	racer->rate60 = MODGHOST_RATE60;
	racer->name[0] = '\0';
	racer->chr = NULL;
	racer->chrnum = -1;
	racer->weaponheld = -1;
	racer->angleoffset = 0.0f;
}

static void modGhostFreePlayback(void)
{
	s32 i;

	for (i = 0; i < MODGHOST_MAXRACERS; i++) {
		modGhostFreeRacer(&g_ModGhostRacers[i]);
	}

	g_ModGhostNumRacers = 0;
	g_ModGhostNumBodyDefs = 0;
}

/**
 * Make room for one more sample, growing the buffer if it is full.
 *
 * malloc rather than mempAlloc: a recording outlives the stage it was made in -
 * it is written out from the end screen, after MEMPOOL_STAGE has been handed
 * back - and it is far too big to take out of a pool the level is using.
 *
 * A failed grow stops the recording rather than ending the run. There is no
 * ghost from that mission and nothing says so, which is the right amount of
 * noise for a machine that has run out of memory during a mission.
 */
static bool modGhostRecordGrow(void)
{
	struct modghostsample *grown;
	s32 cap;

	if (g_ModGhostRecCount < g_ModGhostRecCap) {
		return true;
	}

	if (g_ModGhostRecCount >= MODGHOST_MAXSAMPLES) {
		return false;
	}

	cap = g_ModGhostRecCap + MODGHOST_GROWBY;

	if (cap > MODGHOST_MAXSAMPLES) {
		cap = MODGHOST_MAXSAMPLES;
	}

	grown = realloc(g_ModGhostRec, cap * sizeof(struct modghostsample));

	if (grown == NULL) {
		return false;
	}

	g_ModGhostRec = grown;
	g_ModGhostRecCap = cap;

	return true;
}

static s8 modGhostQuantiseSpeed(f32 speed)
{
	s32 value = speed * 100.0f;

	if (value > 127) {
		value = 127;
	} else if (value < -127) {
		value = -127;
	}

	return value;
}

static u16 modGhostQuantiseAngle(f32 degrees)
{
	while (degrees < 0.0f) {
		degrees += 360.0f;
	}

	while (degrees >= 360.0f) {
		degrees -= 360.0f;
	}

	return (u16)(degrees * (65536.0f / 360.0f));
}

/**
 * An angle in radians as a 16 bit turn, and back.
 *
 * The aim angles are radians because that is what the game computes and what
 * chrCalculateAimEndProperties() expects - shootrotx is an atan2f() plus the
 * look pitch converted from degrees, and handing it a value in degrees turns
 * the upper body through about fifty-seven times the angle it was asked for.
 *
 * A turn is the natural encoding for something that wraps: the whole circle
 * fits the type exactly, so overflow is the wrap rather than a bug, and the
 * resolution is finer than a tenth of a degree.
 */
static s16 modGhostQuantiseRadians(f32 radians)
{
	return (s16)(s32)(radians * (65536.0f / M_BADTAU));
}

static f32 modGhostUnquantiseRadians(s16 turn)
{
	return turn * (M_BADTAU / 65536.0f);
}

/**
 * Interpolate between two turns the short way round.
 *
 * The difference of two turns, taken in the type, is already the shorter arc -
 * that is the property the encoding is chosen for - so this is a plain lerp
 * with one cast in the middle of it. Doing it in unsigned keeps the wrap
 * defined rather than merely usual.
 */
static u16 modGhostLerpTurn(u16 from, u16 to, f32 frac)
{
	s32 delta = (s16)(u16)(to - from);

	return (u16)(from + (s32)(delta * frac));
}

/**
 * Take one sample of the run, if one is due.
 *
 * Called from the movement tick, after the walk has settled the position, so
 * that what is stored is where the run finished the frame rather than where it
 * started it.
 *
 * The clock is the mission timer rather than a count of frames. lvupdate60 is
 * not always one - a frame that took longer advances the mission by more than
 * a sixtieth - so a sample every third frame would be a sample every third of
 * however long the frames happened to be, and would replay at a different
 * speed on a different machine. Sampling against the clock means the file
 * describes a run in seconds and replays as one.
 */
void modGhostRecordSample(void)
{
	struct modghostsample *sample;
	struct player *player;
	s32 time60;

	if (modGhostGetMode() == MODGHOST_OFF || g_ModGhostRecFull) {
		return;
	}

	if (!modGhostStageIsEligible()) {
		return;
	}

	player = g_Vars.currentplayer;
	time60 = playerGetMissionTime();

	// The reader finds a moment by dividing the clock by the interval, so
	// sample n has to be the run at n intervals and nothing else. That is a
	// promise about the file rather than about how often this is called: a
	// frame that took four sixtieths owes two samples, and a stretch where
	// nothing was sampled at all - a cutscene, a frame the mode was off for -
	// owes one for every interval it covered.
	//
	// The debt is paid by repeating the run's state now rather than by leaving
	// a hole, because the alternative is samples whose index no longer says
	// when they were, which replays the whole rest of the run at the wrong
	// time. Standing still is what a missed sample looked like anyway.
	while (g_ModGhostRecCount * MODGHOST_RATE60 <= time60) {
		if (!modGhostRecordGrow()) {
			g_ModGhostRecFull = true;
			return;
		}

		sample = &g_ModGhostRec[g_ModGhostRecCount++];

		sample->x = player->prop->pos.x;
		sample->z = player->prop->pos.z;

		// The ground under the feet, not the eye the prop sits at: the body is
		// drawn from the floor it is standing on. See modGhostPose().
		sample->y = player->vv_manground;

		sample->theta = modGhostQuantiseAngle(player->vv_theta);

		// Where the run was aiming, taken from the player rather than worked
		// out from the look angles. playerUpdateShootRot() has already done
		// that work, including the part where aiming down a sight moves the
		// gun independently of the head, and these two are the values the body
		// is actually posed from.
		sample->shootrotx = modGhostQuantiseRadians(player->shootrotx);
		sample->shootroty = modGhostQuantiseRadians(player->shootroty);

		sample->speedforwards = modGhostQuantiseSpeed(player->speedforwards);
		sample->speedsideways = modGhostQuantiseSpeed(player->speedsideways);
		sample->speedtheta = modGhostQuantiseSpeed(player->speedtheta);

		sample->crouchpos = bmoveGetCrouchPosByPlayer(g_Vars.currentplayernum);
		sample->weaponnum = bgunGetWeaponNum2(HAND_RIGHT);

		sample->flags = 0;

		if (player->hands[HAND_LEFT].flashon) {
			sample->flags |= MODGHOSTSF_FIRINGLEFT;
		}

		if (player->hands[HAND_RIGHT].flashon) {
			sample->flags |= MODGHOSTSF_FIRINGRIGHT;
		}

		if (player->isdead) {
			sample->flags |= MODGHOSTSF_DEAD;
		}
	}
}

/**
 * The ghost's state at a given moment, interpolated between the two samples
 * either side of it.
 *
 * Position and facing are interpolated because they are what the eye follows;
 * everything else is taken from the earlier of the two, because a crouch or a
 * weapon halfway between two values is not a thing. The facing is interpolated
 * the short way round the circle, which is what the u16 turn encoding makes
 * cheap: the difference as a s16 is already the shorter arc.
 */
static void modGhostSampleAt(struct modghostracer *racer, struct modghostsample *dst, s32 time60)
{
	struct modghostsample *a;
	struct modghostsample *b;
	s32 index;
	s32 rate = racer->rate60 > 0 ? racer->rate60 : MODGHOST_RATE60;
	f32 frac;

	if (time60 < 0) {
		time60 = 0;
	}

	index = time60 / rate;

	if (index >= racer->count - 1) {
		*dst = racer->samples[racer->count - 1];
		return;
	}

	a = &racer->samples[index];
	b = &racer->samples[index + 1];
	frac = (time60 - index * rate) / (f32)rate;

	*dst = *a;

	dst->x = a->x + (b->x - a->x) * frac;
	dst->y = a->y + (b->y - a->y) * frac;
	dst->z = a->z + (b->z - a->z) * frac;

	dst->theta = modGhostLerpTurn(a->theta, b->theta, frac);

	// The aim is interpolated as well as the facing. It is the fastest moving
	// thing a body does - a flick onto a target crosses more angle in a tenth
	// of a second than the feet do in a second - so it is the one that shows
	// the twenty samples a second if it is left to step.
	dst->shootrotx = (s16)modGhostLerpTurn(a->shootrotx, b->shootrotx, frac);
	dst->shootroty = (s16)modGhostLerpTurn(a->shootroty, b->shootroty, frac);
}

/**
 * Put the ghost's gun in its hands, or take it away.
 *
 * The weapon is what the run was holding, so that a ghost being watched to
 * learn a route also shows what it was carrying while it took it. It matters
 * to how the body looks as well as what it is holding: the animation chooser
 * picks a different set of walks for one hand, two hands and none, so a ghost
 * with no gun runs like someone who has lost theirs.
 *
 * The old one is freed rather than dropped. objFree() with freeprop takes the
 * prop with it, which is what stops a ghost that changed weapons twenty times
 * from leaving twenty guns on the floor behind it.
 */
static void modGhostSetWeapon(struct modghostracer *racer, s32 weaponnum)
{
	struct prop *held;

	if (racer->chr == NULL || weaponnum == racer->weaponheld) {
		return;
	}

	held = chrGetHeldProp(racer->chr, HAND_RIGHT);

	if (held && held->obj) {
		objFree(held->obj, true, false);
	}

	racer->chr->weapons_held[HAND_RIGHT] = NULL;
	racer->weaponheld = weaponnum;

	if (weaponnum == WEAPON_NONE || weaponnum == WEAPON_UNARMED) {
		return;
	}

	if (playermgrGetModelOfWeapon(weaponnum) <= 0) {
		return;
	}

	chrGiveWeapon(racer->chr, playermgrGetModelOfWeapon(weaponnum), weaponnum, 0);
}

/**
 * Take the ghost's body away.
 *
 * Called when the ghost's run ends, when the mode is turned off mid-mission,
 * and on the way out of a stage. Everything it owns belongs to MEMPOOL_STAGE,
 * so at a stage load the pointer is stale rather than the memory being leaked -
 * modGhostReset() drops the pointer without touching it, and this is only for
 * the cases where the level is still standing.
 */
static void modGhostRetire(struct modghostracer *racer)
{
	struct chrdata *chr = racer->chr;

	if (chr == NULL) {
		return;
	}

	// Cleared first: chrRemove() ticks through code that can ask whether a chr
	// is the ghost, and by this point it is not going to be one for much
	// longer.
	racer->chr = NULL;
	racer->chrnum = -1;
	racer->weaponheld = -1;

	// The gun goes with it. chrRemove() walks the prop's children and frees
	// them, which is what the ghost's weapon is, so it is not freed here as
	// well - modGhostSetWeapon() is the only place that has to do that, being
	// the only one that takes a weapon off a body that is staying.
	if (chr->prop) {
		chrRemove(chr->prop, true);
	}
}

static void modGhostRetireAll(void)
{
	s32 i;

	for (i = 0; i < MODGHOST_MAXRACERS; i++) {
		modGhostRetire(&g_ModGhostRacers[i]);
	}
}

/**
 * Whether the body the ghost was posing has been taken away underneath it.
 *
 * Nothing in this file removes it except modGhostRetire(), and nothing else
 * should - the ghost is invincible, untargetable and has no AI to run itself
 * out of. But chrTick() can return TICKOP_FREE from several directions, some
 * of which are counters over every chr in the level rather than anything about
 * this one, and a chr slot that has been freed is a chr slot that gets reused.
 * Posing a pointer into a reused slot would drive a real chr around the level
 * along a recorded route, which is a strange enough bug to be worth one
 * comparison a frame to make impossible.
 */
static bool modGhostBodyIsGone(struct modghostracer *racer)
{
	struct chrdata *chr = racer->chr;

	if (chr == NULL) {
		return false;
	}

	return chr->chrnum != racer->chrnum
		|| chr->chrnum < 0
		|| chr->prop == NULL
		|| chr->prop->chr != chr
		|| chr->model == NULL;
}

/**
 * Take the ghost out of the world's collision, and keep it out.
 *
 * Two flags rather than one, and re-asserted every frame rather than set once,
 * because CHRHFLAG_PERIMDISABLED is not a setting - it is scratch space. The
 * chr movement helpers turn a chr's own perimeter off so that a move test does
 * not collide the chr with itself, and turn it back on afterwards by setting
 * it to true rather than by restoring what it was (chr0f01f264() and the push
 * handler in chr.c both do this). Anything that asks to be intangible and then
 * moves is solid again by the end of the frame, which is exactly what a ghost
 * does.
 *
 * CHRCFLAG_PERIMDISABLEDTMP is checked in both of the places that matter -
 * the collision type filter in propGetCdTypes() and chrUpdateGeometry(), which
 * is what builds the cylinder the player walks into - and nothing borrows it,
 * despite the name. That is the one that holds. The other is set as well
 * because it is the cheaper of the two tests and costs nothing to keep true.
 */
static void modGhostSetIntangible(struct chrdata *chr)
{
	chr->chrflags |= CHRCFLAG_PERIMDISABLEDTMP;
	chr->hidden |= CHRHFLAG_PERIMDISABLED;

	if (chr->prop) {
		propSetPerimEnabled(chr->prop, false);
	}
}

/**
 * Build the body the ghost is replayed into.
 *
 * The model is the one the player would have worn on this mission, because a
 * ghost of your own run through this level should look like the run through
 * this level - Joanna in whatever she is wearing here, not a generic guard.
 *
 * It answers to the same memory floor a kept body does, and for the same
 * reason: a ghost is not worth the last of the stage pool. Failing here means
 * the mission runs without one, and it is tried again on the next frame in
 * case whatever was holding the pool has given it back.
 */
/**
 * Build a body for one ghost, sharing the model definitions across the field.
 *
 * The first ghost to be built loads the body and head the ordinary way and
 * keeps the definitions it was given; every ghost after it is handed the same
 * pair. body0f02d338() only loads what it is not given, so the second through
 * tenth bodies cost a model each rather than a model and a fresh fifty
 * kilobyte copy of the same head. That distinction is the difference between
 * ten ghosts and an empty stage pool.
 *
 * Definitions are read-only shared data; what is per body is the rwdata that
 * modelmgrInstantiateModel() hands out, so sharing them is what every chr in a
 * mission already does with its race's models.
 */
static struct model *modGhostBuildModel(struct modghostracer *racer)
{
	s32 bodynum = BODY_DARK_COMBAT;
	s32 headnum = HEAD_DARK_COMBAT;
	s32 sunglasses = false;
	struct model *model;
	s32 i;

	// The character the run was set as, out of its own file, and Joanna when
	// the file names nobody.
	//
	// It used to ask playerChooseBodyAndHead() for that second case, which was
	// right while every ghost was Joanna and became a bug the moment a trial
	// character existed: that function now answers with the character the
	// player picked, so changing your own outfit restyled every older ghost on
	// the track. A ghost is a record of somebody's run and does not follow the
	// person watching it.
	if (!modGhostResolveCharacter(racer->body, racer->head, &bodynum, &headnum)) {
		bodynum = BODY_DARK_COMBAT;
		headnum = HEAD_DARK_COMBAT;
	}

	(void)sunglasses;

	for (i = 0; i < g_ModGhostNumBodyDefs; i++) {
		if (g_ModGhostBodyDefs[i].bodynum != bodynum) {
			continue;
		}

		// The head definition is allowed to be NULL - some bodies carry their
		// own - and passing it back as NULL is the same instruction it was the
		// first time. What must not happen is passing NULL for the body and
		// having the head loaded again against it.
		return body0f02d338(bodynum,
				g_ModGhostBodyDefs[i].headdef ? 1 : headnum,
				g_ModGhostBodyDefs[i].bodydef, g_ModGhostBodyDefs[i].headdef,
				false, false);
	}

	model = bodyAllocateModel(bodynum, headnum, SPAWNFLAG_FIXEDHEIGHT);

	if (model && model->definition && g_ModGhostNumBodyDefs < MODGHOST_MAXRACERS) {
		struct modelnode *node = modelGetPart(model->definition, MODELPART_CHR_HEADSPOT);
		struct modghostbodydef *def = &g_ModGhostBodyDefs[g_ModGhostNumBodyDefs++];

		def->bodynum = bodynum;
		def->bodydef = model->definition;
		def->headdef = NULL;

		if (node) {
			struct modelrwdata_headspot *rwdata = modelGetNodeRwData(model, node);
			def->headdef = rwdata->headmodeldef;
		}
	}

	return model;
}

static bool modGhostBuild(struct modghostracer *racer, struct modghostsample *sample)
{
	struct model *model;
	struct prop *prop;
	struct coord pos;
	RoomNum rooms[8];

	// Two chr slots free rather than one, the way modbodies.c asks: chrInit()
	// returning nothing is dereferenced by its caller, so the slot has to be
	// known to be there before chrAllocate() is called.
	if (chrsGetNumFree() < 2 || mempGetStageFreeTotal() < MODGHOST_MEMFLOOR) {
		return false;
	}

	model = modGhostBuildModel(racer);

	if (model == NULL) {
		return false;
	}

	pos.x = sample->x;
	pos.y = sample->y;
	pos.z = sample->z;

	bgFindRoomsByPos(&pos, g_Vars.currentplayer->prop->rooms, rooms, ARRAYCOUNT(rooms), NULL);

	prop = chrAllocate(model, &pos, rooms, 0.0f, NULL);

	if (prop == NULL) {
		modelmgrFreeModel(model);
		return false;
	}

	racer->chr = prop->chr;
	racer->chrnum = prop->chr->chrnum;
	racer->weaponheld = -1;
	racer->angleoffset = 0.0f;

	propActivateThisFrame(prop);
	propEnable(prop);

	// A ghost is a picture of a run, not a participant in this one. Nothing
	// may aim at it, damage it or blow it up, and nothing may walk into it.
	modGhostSetIntangible(racer->chr);

	racer->chr->chrflags |= CHRCFLAG_INVINCIBLE | CHRCFLAG_UNEXPLODABLE
		| CHRCFLAG_NOAUTOAIM | CHRCFLAG_NEVERSLEEP;
	racer->chr->chrflags &= ~(CHRCFLAG_KILLCOUNTABLE | CHRCFLAG_FORCETOGROUND);
	racer->chr->hidden |= CHRHFLAG_UNTARGETABLE;

	// The action the animation chooser drives. There is no case for it in
	// chraTick()'s switch, which is the point: nothing here decides what the
	// body does except the recording.
	racer->chr->actiontype = ACT_BONDMULTI;
	racer->chr->act_bondmulti.animcfg = NULL;

	racer->chr->maxdamage = 0x7fff;
	racer->chr->fadealpha = modGhostGetAlpha();

	return true;
}

/**
 * Pose the body for one frame of the recording.
 *
 * The order is the one playerTickThirdPerson() uses on a multiplayer body:
 * choose the animation from the speeds, put the root where the sample says,
 * set the facing, and leave the tick to do the rest. Everything read here is
 * the sample; nothing is read from the world, because the world is where this
 * run is and the ghost is somewhere else.
 */
static void modGhostPose(struct modghostracer *racer, struct modghostsample *sample)
{
	struct chrdata *chr = racer->chr;
	struct prop *prop = chr->prop;
	struct coord dstpos;
	struct coord rootpos;
	RoomNum dstrooms[8];
	f32 angle;
	f32 shootrotx;
	f32 shootroty;
	s32 animnum;

	modGhostSetWeapon(racer, sample->weaponnum);

	// The prop is carried to the sample by the same portal walk the spectator
	// camera uses. It never returns an empty room list - a step that ends
	// outside the level keeps the rooms it had - so a ghost replaying through
	// a door that this run has not opened stays in a room the renderer can
	// draw it from.
	dstpos.x = sample->x;
	dstpos.y = sample->y + MODGHOST_PROPLIFT;
	dstpos.z = sample->z;

	func0f065e74(&prop->pos, prop->rooms, &dstpos, dstrooms);

	prop->pos.x = dstpos.x;
	prop->pos.y = dstpos.y;
	prop->pos.z = dstpos.z;

	propDeregisterRooms(prop);
	roomsCopy(dstrooms, prop->rooms);
	propActivateThisFrame(prop);

	// Where the feet are. chrTick() draws the body from the ground the chr
	// says it is standing on rather than from the prop, so the recorded ground
	// is what puts a ghost on the stairs it climbed instead of on the floor
	// below them.
	chr->ground = sample->y;
	chr->manground = sample->y;
	chr->sumground = sample->y * (PAL ? 8.417509f : 9.999998f);
	chr->fadealpha = modGhostGetAlpha();

	// Both of these are put back every frame rather than set once: the body's
	// own tick clears the collision flags, and the fade is the sort of thing a
	// cheat or a vision mode writes.
	modGhostSetIntangible(chr);

	animnum = modelGetAnimNum(chr->model);

	playerChooseThirdPersonAnimation(chr, sample->crouchpos,
			sample->speedsideways / 100.0f,
			sample->speedforwards / 100.0f,
			sample->speedtheta / 100.0f,
			&racer->angleoffset,
			&chr->act_bondmulti.animcfg);

	// Where the ghost was aiming. The chooser having left the animation alone
	// means it is mid transition and the aim would fight it, which is the same
	// test the multiplayer body makes.
	//
	// Both halves are needed and they go to different places. shootrotx tilts
	// the body up and down and is what the aim end properties are calculated
	// from; shootroty turns the shoulders towards the target and is assigned
	// straight across, outside the animation test, exactly as
	// playerTickThirdPerson() does it. Setting the first and not the second
	// leaves a body that aims up and down correctly and never turns.
	shootrotx = modGhostUnquantiseRadians(sample->shootrotx);
	shootroty = modGhostUnquantiseRadians(sample->shootroty);

	if (modelGetAnimNum(chr->model) == animnum) {
		if (chr->act_bondmulti.animcfg) {
			chr->hidden2 &= ~CHRH2FLAG_AUTOANIM;
			chrCalculateAimEndProperties(chr, chr->act_bondmulti.animcfg,
					chrGetHeldProp(chr, HAND_LEFT) != NULL,
					chrGetHeldProp(chr, HAND_RIGHT) != NULL, shootrotx);
		} else {
			chr->hidden2 |= CHRH2FLAG_AUTOANIM;
			chr->aimendback = shootrotx;
			chr->aimendrshoulder = 0;
			chr->aimendlshoulder = 0;
		}
	}

	chr->aimendsideback = shootroty;
	chr->aimendcount = 10;

	chrSetFiring(chr, HAND_LEFT, (sample->flags & MODGHOSTSF_FIRINGLEFT) != 0);
	chrSetFiring(chr, HAND_RIGHT, (sample->flags & MODGHOSTSF_FIRINGRIGHT) != 0);

	modelGetRootPosition(chr->model, &rootpos);
	rootpos.x = prop->pos.x;
	rootpos.z = prop->pos.z;
	modelSetRootPosition(chr->model, &rootpos);

	// The chooser's angleoffset is the lean the strafe animations are drawn
	// with, and it is subtracted here the way the multiplayer body subtracts
	// it, so that a body sidestepping still faces where the run was facing.
	angle = (360.0f - sample->theta * (360.0f / 65536.0f)) * 0.017450513318181f
		- racer->angleoffset;

	if (angle >= M_BADTAU) {
		angle -= M_BADTAU;
	} else if (angle < 0.0f) {
		angle += M_BADTAU;
	}

	chrSetLookAngle(chr, angle);
	chr0f0220ac(chr);
}

/**
 * How far behind the ghost this run is, at the point on the route it has
 * reached.
 *
 * A split is not a difference of clocks, it is the difference between how long
 * each of them took to get to the same place, so it is measured in space: the
 * ghost's sample nearest the player is the moment it was here, and its time is
 * what this run's clock is compared against. Ahead of the ghost is negative.
 *
 * The search walks out from wherever it matched last frame, which is what
 * makes it cheap and also what keeps a route that doubles back from matching
 * the wrong pass through a room. A player who has left the ghost's route
 * entirely - a different way round, or a fight in a side room - matches
 * nothing within the distance limit and the split simply stops being shown
 * rather than showing a number about somewhere else.
 */
static void modGhostUpdateSplit(s32 time60)
{
	struct modghostracer *racer = modGhostGetTarget();
	struct coord *pos;
	s32 rate;
	s32 best = -1;
	f32 bestdistsq = MODGHOST_SPLITMAXDISTSQ;
	s32 from;
	s32 to;
	s32 i;

	if (!g_ModGhostSplits || racer == NULL || racer->count < 1) {
		g_ModGhostSplitValid = false;
		return;
	}

	rate = racer->rate60 > 0 ? racer->rate60 : MODGHOST_RATE60;
	pos = &g_Vars.currentplayer->prop->pos;

	from = g_ModGhostSplitIdx - MODGHOST_SPLITWINDOW;
	to = g_ModGhostSplitIdx + MODGHOST_SPLITWINDOW;

	if (from < 0) {
		from = 0;
	}

	if (to >= racer->count) {
		to = racer->count - 1;
	}

	for (i = from; i <= to; i++) {
		f32 dx = racer->samples[i].x - pos->x;
		f32 dz = racer->samples[i].z - pos->z;

		// The vertical difference is between the ghost's feet and the player's
		// eye, so it is compared against the player's ground for the same
		// reason the sample stores ground: a floor above is a different place,
		// but a floor above measured to the eye is only half a body away.
		f32 dy = racer->samples[i].y - g_Vars.currentplayer->vv_manground;
		f32 distsq = dx * dx + dy * dy + dz * dz;

		if (distsq < bestdistsq) {
			bestdistsq = distsq;
			best = i;
		}
	}

	if (best < 0) {
		g_ModGhostSplitValid = false;
		return;
	}

	g_ModGhostSplitIdx = best;
	g_ModGhostSplit60 = time60 - best * rate;
	g_ModGhostSplitValid = true;
}

bool modGhostHasSplit(void)
{
	return modGhostIsRacing() && g_ModGhostSplitValid;
}

s32 modGhostGetSplit60(void)
{
	return g_ModGhostSplit60;
}

s32 modGhostGetTargetTime60(void)
{
	struct modghostracer *racer = modGhostGetTarget();

	return racer ? racer->time60 : 0;
}

const char *modGhostGetTargetName(void)
{
	struct modghostracer *racer = modGhostGetTarget();

	return racer ? racer->name : "";
}

/**
 * Read a ghost file, checking everything about it before believing any of it.
 *
 * The file may have come from anywhere. Every field that is used to size an
 * allocation or to stride a buffer is checked against what is actually there
 * before it is used, and the sample block is hashed against what the header
 * claims, because a truncated download is the ordinary case rather than the
 * exotic one.
 *
 * Samples are read by the size the file declares rather than by sizeof, so a
 * file written by a later build with a bigger sample still replays here: the
 * fields this build knows about are at the front of the struct and a longer
 * one is read into the front of ours with the tail skipped. Both directions of
 * that are why headersize and samplesize are in the header at all.
 */
static bool modGhostReadFile(const char *rel, struct modghostheader *hdr, struct modghostsample **outsamples)
{
	FILE *f = fsFileOpenRead(rel);
	struct modghostsample *samples = NULL;
	u32 samplesize;
	u32 i;

	if (f == NULL) {
		return false;
	}

	if (fread(hdr, sizeof(struct modghostheader), 1, f) != 1) {
		fclose(f);
		return false;
	}

	if (memcmp(hdr->magic, MODGHOST_MAGIC, sizeof(MODGHOST_MAGIC)) != 0
			|| hdr->version < 1
			|| hdr->version > MODGHOST_VERSION
			|| hdr->headersize < sizeof(struct modghostheader)
			|| hdr->numsamples < 2
			|| hdr->numsamples > MODGHOST_MAXSAMPLES
			|| hdr->samplesize < sizeof(struct modghostsample)
			|| hdr->samplesize > 1024
			|| hdr->rate60 < 1
			|| hdr->rate60 > 60) {
		fclose(f);
		return false;
	}

	if (outsamples == NULL) {
		// A header-only read, for choosing between the files in the directory
		// before committing to reading one of them.
		fclose(f);
		return true;
	}

	if (fseek(f, hdr->headersize, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}

	samplesize = hdr->samplesize;
	samples = calloc(hdr->numsamples, sizeof(struct modghostsample));

	if (samples == NULL) {
		fclose(f);
		return false;
	}

	for (i = 0; i < hdr->numsamples; i++) {
		if (fread(&samples[i], sizeof(struct modghostsample), 1, f) != 1) {
			free(samples);
			fclose(f);
			return false;
		}

		if (samplesize > sizeof(struct modghostsample)
				&& fseek(f, samplesize - sizeof(struct modghostsample), SEEK_CUR) != 0) {
			free(samples);
			fclose(f);
			return false;
		}
	}

	fclose(f);

	// The hash is over the samples as this build understands them, which is
	// the same bytes the writer hashed as long as the sample size matches. A
	// file from a build with a longer sample is read but not checked, there
	// being nothing here that could reproduce its hash.
	//
	// Checked before the version conversion below rather than after, because
	// what the hash describes is the file as it arrived. Converting first
	// would be hashing this build's opinion of it.
	if (samplesize == sizeof(struct modghostsample)
			&& hdr->hash != modGhostHash(samples, hdr->numsamples * sizeof(struct modghostsample))) {
		sysLogPrintf(LOG_WARNING, "ghost: %s failed its checksum, ignoring it", rel);
		free(samples);
		return false;
	}

	// Version 1 stored the look pitch in degrees where version 2 stores the
	// aim in radians, and needs no conversion at all: both are 16 bit turns of
	// a whole circle, and a turn does not know which unit it was divided into.
	// The pitch a version 1 ghost replays with is therefore exactly right.
	//
	// What it does not have is the horizontal half of the aim, which version 1
	// never recorded - the bug that prompted the version. Those ghosts replay
	// with shoulders that do not turn towards what they are shooting at, which
	// is wrong in a way you have to be looking for, and much better than
	// refusing to replay a run somebody earned.
	if (hdr->version < 2) {
		for (i = 0; i < hdr->numsamples; i++) {
			samples[i].shootroty = 0;
		}
	}

	*outsamples = samples;

	return true;
}

/**
 * Whether a filename ends in the ghost extension, either case.
 *
 * Spelled out rather than reaching for strcasecmp, which is a POSIX name that
 * the Windows build would need a header of its own for.
 */
static bool modGhostHasExt(const char *name, u32 len)
{
	const char *ext = MODGHOST_EXT;
	u32 extlen = sizeof(MODGHOST_EXT) - 1;
	u32 i;

	for (i = 0; i < extlen; i++) {
		char a = name[len - extlen + i];
		char b = ext[i];

		if (a >= 'A' && a <= 'Z') {
			a += 'a' - 'A';
		}

		if (a != b) {
			return false;
		}
	}

	return true;
}

/**
 * A scan of the ghosts directory, collecting the field for one stage.
 *
 * Candidates are kept sorted by time as they are found, so a directory of a
 * hundred files still costs one header read each and leaves the quickest ten
 * in hand. Insertion sort over ten entries is the cheapest thing that could
 * work and the list is never longer than that.
 */
struct modghostcandidate {
	char filename[64];
	char player[MODGHOST_NAMELEN];
	u32 time60;
};

struct modghostscan {
	s32 stagenum;
	s32 difficulty;
	s32 pick;
	s32 wanted;
	s32 count;
	struct modghostcandidate entries[MODGHOST_MAXRACERS];
};

/**
 * Consider one file in the ghosts directory.
 *
 * Which stage and difficulty a ghost is for comes out of its header rather
 * than its name, because most of the files here will not have been named by
 * this build. A run downloaded from someone else keeps whatever it was called
 * and is still found by the stage it was run on.
 */
/**
 * How many of the field to race, and which ghosts have been ticked.
 *
 * The choice is a list of filenames rather than anything derived from the
 * files, because the directory is what the player is choosing from and it
 * moves underneath them: a download adds a file, beating your own best
 * rewrites one. A ticked name that no longer resolves is not raced and is not
 * forgotten either, in case whatever it named comes back.
 */
s32 g_ModGhostMaxRacers = 1;

static struct modghostchoice g_ModGhostChoices[MODGHOST_MAXRACERS];
static s32 g_ModGhostNumChoices = 0;

static struct modghostentry g_ModGhostCatalogue[MODGHOST_MAXCATALOGUE];
static s32 g_ModGhostCatalogueCount = 0;

/**
 * Which stage and difficulty the catalogue is being read for, or -1 for all.
 *
 * The chooser asks about one mission at a time, because a field is raced on one
 * mission and a list of every run on disk is a list you scroll past to find the
 * three rows you meant. My Ghosts asks for all of them, because that one is an
 * inventory rather than a question about a race.
 */
static s32 g_ModGhostCatalogueStage = -1;
static s32 g_ModGhostCatalogueDiff = -1;

void modGhostSetCatalogueFilter(s32 stagenum, s32 difficulty)
{
	g_ModGhostCatalogueStage = stagenum;
	g_ModGhostCatalogueDiff = difficulty;
}

#define MODGHOST_CHOSENFILE MODGHOST_DIR "/chosen.txt"

static bool modGhostIsChosen(const char *name)
{
	s32 i;

	for (i = 0; i < g_ModGhostNumChoices; i++) {
		if (strncmp(g_ModGhostChoices[i].filename, name, sizeof(g_ModGhostChoices[0].filename)) == 0) {
			return true;
		}
	}

	return false;
}

s32 modGhostGetNumChosen(void)
{
	return g_ModGhostNumChoices;
}

void modGhostClearChosen(void)
{
	g_ModGhostNumChoices = 0;
	memset(g_ModGhostChoices, 0, sizeof(g_ModGhostChoices));
}

/**
 * The chosen list, kept beside the ghosts it names.
 *
 * A plain list of filenames, one per line, in the directory it describes -
 * which means a ghosts directory copied to another machine takes its selection
 * with it, and a line edited by hand does what it looks like it does. It is
 * not in pd.ini because it belongs to the ghosts rather than to the game's
 * settings, and because ten filenames is not what an ini is for.
 */
void modGhostLoadChoices(void)
{
	FILE *f = fsFileOpenRead(MODGHOST_CHOSENFILE);
	char line[128];

	modGhostClearChosen();

	if (f == NULL) {
		return;
	}

	while (g_ModGhostNumChoices < MODGHOST_MAXRACERS && fgets(line, sizeof(line), f)) {
		u32 len = strlen(line);

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
			line[--len] = '\0';
		}

		if (len == 0 || len >= sizeof(g_ModGhostChoices[0].filename)) {
			continue;
		}

		strncpy(g_ModGhostChoices[g_ModGhostNumChoices].filename, line,
				sizeof(g_ModGhostChoices[0].filename) - 1);
		g_ModGhostNumChoices++;
	}

	fclose(f);
}

void modGhostSaveChoices(void)
{
	FILE *f;
	s32 i;

	if (fsFileSize(MODGHOST_DIR) < 0 && fsCreateDir(MODGHOST_DIR) != 0) {
		return;
	}

	f = fsFileOpenWrite(MODGHOST_CHOSENFILE);

	if (f == NULL) {
		return;
	}

	for (i = 0; i < g_ModGhostNumChoices; i++) {
		fprintf(f, "%s\n", g_ModGhostChoices[i].filename);
	}

	fclose(f);
}

/**
 * The mission a ghost was run on, as the menus name it.
 *
 * g_SoloStages is the table the mission select reads, so a ghost is listed
 * under the same words the player chose the mission with. A stage that is not
 * in it - a Combat Sim arena, something a later build added - has no name to
 * give and says so rather than showing a number nobody can act on.
 */
const char *modGhostStageName(s32 stagenum)
{
	s32 i;

	for (i = 0; i < NUM_SOLOSTAGES; i++) {
		if (g_SoloStages[i].stagenum == (u32)stagenum) {
			return langGet(g_SoloStages[i].name3);
		}
	}

	return "Unknown";
}

static void modGhostCatalogueFile(const char *name, void *arg)
{
	struct modghostheader hdr;
	struct modghostentry *entry;
	char rel[FS_MAXPATH + 1];
	u32 len = strlen(name);
	s32 at;
	s32 i;

	if (len < sizeof(MODGHOST_EXT) || len >= sizeof(entry->filename)) {
		return;
	}

	if (!modGhostHasExt(name, len)) {
		return;
	}

	snprintf(rel, sizeof(rel), MODGHOST_DIR "/%s", name);

	if (!modGhostReadFile(rel, &hdr, NULL)) {
		return;
	}

	if ((g_ModGhostCatalogueStage >= 0 && hdr.stagenum != g_ModGhostCatalogueStage)
			|| (g_ModGhostCatalogueDiff >= 0 && hdr.difficulty != g_ModGhostCatalogueDiff)) {
		return;
	}

	// Sorted by stage, then difficulty, then time, so the list reads as the
	// mission order the player already knows with the quickest run of each
	// at the top of its group.
	for (at = 0; at < g_ModGhostCatalogueCount; at++) {
		struct modghostentry *other = &g_ModGhostCatalogue[at];

		if (hdr.stagenum != other->stagenum) {
			if (hdr.stagenum < other->stagenum) {
				break;
			}

			continue;
		}

		if (hdr.difficulty != other->difficulty) {
			if (hdr.difficulty < other->difficulty) {
				break;
			}

			continue;
		}

		if (hdr.time60 < other->time60) {
			break;
		}
	}

	// A full catalogue drops the run that sorts last rather than the one that
	// arrived last, so what the page lists depends on the times in the files
	// and not on the order the directory was read in.
	if (g_ModGhostCatalogueCount >= MODGHOST_MAXCATALOGUE) {
		if (at >= MODGHOST_MAXCATALOGUE) {
			return;
		}

		g_ModGhostCatalogueCount = MODGHOST_MAXCATALOGUE - 1;
	}

	for (i = g_ModGhostCatalogueCount; i > at; i--) {
		g_ModGhostCatalogue[i] = g_ModGhostCatalogue[i - 1];
	}

	entry = &g_ModGhostCatalogue[at];
	memset(entry, 0, sizeof(*entry));

	strncpy(entry->filename, name, sizeof(entry->filename) - 1);
	strncpy(entry->player, hdr.player, sizeof(entry->player) - 1);
	strncpy(entry->owner, hdr.owner, sizeof(entry->owner) - 1);
	entry->flags = hdr.flags;
	entry->time60 = hdr.time60;
	entry->stagenum = hdr.stagenum;
	entry->difficulty = hdr.difficulty;
	entry->chosen = modGhostIsChosen(name);

	g_ModGhostCatalogueCount++;
}

/**
 * Read every ghost in the directory, for the chooser to list.
 *
 * Done when the chooser is opened rather than continuously: it is a header
 * read per file, and the directory only changes when the player downloaded
 * something or finished a run.
 */
s32 modGhostScanCatalogue(void)
{
	g_ModGhostCatalogueCount = 0;

	modGhostLoadChoices();
	fsScanDir(MODGHOST_DIR, modGhostCatalogueFile, NULL);

	return g_ModGhostCatalogueCount;
}

s32 modGhostGetCatalogueCount(void)
{
	return g_ModGhostCatalogueCount;
}

struct modghostentry *modGhostGetCatalogueEntry(s32 index)
{
	if (index < 0 || index >= g_ModGhostCatalogueCount) {
		return NULL;
	}

	return &g_ModGhostCatalogue[index];
}

/**
 * Tick or untick one ghost, keeping the saved list in step.
 *
 * Ticking an eleventh is refused rather than silently dropping one of the ten,
 * because the player is choosing a field and being quietly given a different
 * one is worse than being told no.
 */
void modGhostToggleChosen(s32 index)
{
	struct modghostentry *entry = modGhostGetCatalogueEntry(index);
	s32 i;

	if (entry == NULL) {
		return;
	}

	if (entry->chosen) {
		for (i = 0; i < g_ModGhostNumChoices; i++) {
			if (strncmp(g_ModGhostChoices[i].filename, entry->filename,
					sizeof(g_ModGhostChoices[0].filename)) == 0) {
				for (; i < g_ModGhostNumChoices - 1; i++) {
					g_ModGhostChoices[i] = g_ModGhostChoices[i + 1];
				}

				g_ModGhostNumChoices--;
				break;
			}
		}

		entry->chosen = false;
	} else {
		if (g_ModGhostNumChoices >= MODGHOST_MAXRACERS) {
			return;
		}

		strncpy(g_ModGhostChoices[g_ModGhostNumChoices].filename, entry->filename,
				sizeof(g_ModGhostChoices[0].filename) - 1);
		g_ModGhostNumChoices++;
		entry->chosen = true;
	}

	modGhostSaveChoices();
}

/**
 * Delete one ghost off the disk, by its place in the catalogue.
 *
 * The local directory keeps every finished run, which is the point of it - the
 * one that turns out to matter is not knowable while it is being set. What
 * that costs is a directory nobody curates, so this is the curation: the page
 * that lists them is the page that removes them.
 *
 * Unticked first if it was ticked, because a chosen list holding a name that
 * no longer resolves is a field that quietly races one ghost fewer than the
 * player picked. The catalogue is then rebuilt rather than patched, since
 * every index after this one has moved.
 */
bool modGhostDeleteCatalogueEntry(s32 index)
{
	struct modghostentry *entry = modGhostGetCatalogueEntry(index);
	char rel[FS_MAXPATH + 1];

	if (entry == NULL) {
		return false;
	}

	if (entry->chosen) {
		modGhostToggleChosen(index);
	}

	snprintf(rel, sizeof(rel), MODGHOST_DIR "/%s", entry->filename);

	if (remove(fsFullPath(rel)) != 0) {
		sysLogPrintf(LOG_ERROR, "ghost: could not delete %s", fsFullPath(rel));
		return false;
	}

	sysLogPrintf(LOG_NOTE, "ghost: deleted %s", rel);
	modGhostScanCatalogue();

	return true;
}

static void modGhostScanFile(const char *name, void *arg)
{
	struct modghostscan *scan = arg;
	struct modghostheader hdr;
	char rel[FS_MAXPATH + 1];
	u32 len = strlen(name);
	s32 at;
	s32 i;

	if (len < sizeof(MODGHOST_EXT) || len >= sizeof(scan->entries[0].filename)) {
		return;
	}

	if (!modGhostHasExt(name, len)) {
		return;
	}

	snprintf(rel, sizeof(rel), MODGHOST_DIR "/%s", name);

	if (!modGhostReadFile(rel, &hdr, NULL)) {
		return;
	}

	if (hdr.stagenum != scan->stagenum || hdr.difficulty != scan->difficulty) {
		return;
	}

	// Raced under the same rules or not raced. A ghost recorded when jump and
	// the combat roll were available took a route this run cannot take, so
	// pacing yourself against it teaches the wrong thing - and the boards
	// refuse it for the same reason, which would otherwise leave the field and
	// the leaderboard disagreeing about which runs exist.
	//
	// This is checked here rather than in the catalogue because My Ghosts is an
	// inventory: a file that cannot be raced is still a file the player has,
	// and hiding it would leave them unable to find the thing they wanted to
	// delete.
	if ((hdr.flags & MODGHOSTHF_TRIALRULES) == 0) {
		return;
	}

	// Mine means this account's, not this agent's. The agent is a save file
	// and two accounts on one machine share it, so comparing it meant My Best
	// Only raced somebody else's runs - dab2 signed in, pacing against dab.
	// Runs with no owner cannot race at all, so there is nothing to fall back
	// to and nothing that needs one.
	if (scan->pick == MODGHOSTPICK_MINE
			&& strncmp(hdr.owner, ghostnetGetAccountName(), MODGHOST_OWNERLEN) != 0) {
		return;
	}

	if (scan->pick == MODGHOSTPICK_CHOSEN && !modGhostIsChosen(name)) {
		return;
	}

	// Fastest is a field of people, so one run each. Every finished run is on
	// disk now, and without this a decent session of your own is the whole
	// field and everybody else's run is off the bottom of it. The other two
	// picks are asked for by name - My Best is your attempts and wants them
	// all, Chosen is exactly what was ticked - so neither thins anything out.
	//
	// Slower duplicates are dropped and quicker ones replace what is held,
	// which is the same answer whatever order the directory is scanned in.
	if (scan->pick == MODGHOSTPICK_FASTEST) {
		for (i = 0; i < scan->count; i++) {
			if (strncmp(scan->entries[i].player, hdr.player, MODGHOST_NAMELEN) != 0) {
				continue;
			}

			if (hdr.time60 >= scan->entries[i].time60) {
				return;
			}

			// Held by this player and beaten, so the slot goes and the run is
			// inserted below at whatever place its time earns.
			for (; i < scan->count - 1; i++) {
				scan->entries[i] = scan->entries[i + 1];
			}

			scan->count--;
			break;
		}
	}

	// Where this run belongs in the field, quickest first.
	for (at = 0; at < scan->count; at++) {
		if (hdr.time60 < scan->entries[at].time60) {
			break;
		}
	}

	if (at >= scan->wanted) {
		return;
	}

	if (scan->count < scan->wanted) {
		scan->count++;
	}

	for (i = scan->count - 1; i > at; i--) {
		scan->entries[i] = scan->entries[i - 1];
	}

	scan->entries[at].time60 = hdr.time60;
	strncpy(scan->entries[at].filename, name, sizeof(scan->entries[at].filename) - 1);
	scan->entries[at].filename[sizeof(scan->entries[at].filename) - 1] = '\0';
	strncpy(scan->entries[at].player, hdr.player, MODGHOST_NAMELEN - 1);
	scan->entries[at].player[MODGHOST_NAMELEN - 1] = '\0';
}

/**
 * Find and load the ghost to race on this stage, if there is one.
 *
 * The pick is made across the whole directory rather than from a file named
 * after the stage, which is what lets a downloaded run be raced by dropping it
 * in. MODGHOSTPICK_FASTEST takes the best time there is, yours or anyone's;
 * MODGHOSTPICK_MINE ignores everyone else's, for practising against your own
 * pace rather than chasing a run you have no chance of catching yet.
 */
static void modGhostLoad(void)
{
	struct modghostscan scan;
	struct modghostheader hdr;
	struct modghostsample *samples;
	char rel[FS_MAXPATH + 1];
	s32 i;

	// The ticked list is read here rather than only by the chooser. It used to
	// be loaded as a side effect of opening Choose Ghosts, so racing Chosen
	// Ghosts worked in a session where that page had been visited and raced
	// nothing at all in one where it had not - including every session that
	// started the mission straight from the menu.
	modGhostLoadChoices();

	memset(&scan, 0, sizeof(scan));
	scan.stagenum = g_Vars.stagenum;
	scan.difficulty = g_MissionConfig.difficulty;
	scan.pick = g_ModGhostPick;
	scan.wanted = g_ModGhostMaxRacers;

	if (scan.wanted < 1) {
		scan.wanted = 1;
	}

	if (scan.wanted > MODGHOST_MAXRACERS) {
		scan.wanted = MODGHOST_MAXRACERS;
	}

	if (fsScanDir(MODGHOST_DIR, modGhostScanFile, &scan) < 0 || scan.count < 1) {
		return;
	}

	for (i = 0; i < scan.count; i++) {
		struct modghostracer *racer = &g_ModGhostRacers[g_ModGhostNumRacers];

		samples = NULL;
		snprintf(rel, sizeof(rel), MODGHOST_DIR "/%s", scan.entries[i].filename);

		// A file that passed its header read during the scan and fails now is
		// one that changed underneath us, which is what a download landing
		// mid-scan looks like. Skip it and take the rest of the field.
		if (!modGhostReadFile(rel, &hdr, &samples)) {
			continue;
		}

		racer->samples = samples;
		racer->count = hdr.numsamples;
		racer->rate60 = hdr.rate60;
		racer->time60 = hdr.time60;
		racer->chr = NULL;
		racer->chrnum = -1;
		racer->weaponheld = -1;
		racer->angleoffset = 0.0f;

		// The account, when the file has one. A nametag is there to say who
		// you are racing on the board you are trying to climb, and that is an
		// account rather than an agent: the agent name is whatever the person
		// called their save file, and two of them can be Joanna. Runs from
		// before ghosts carried an owner fall back to it, because a name that
		// is only sometimes right beats no name at all.
		racer->body = hdr.mpbody;
		racer->head = hdr.mphead;

		strncpy(racer->name, hdr.owner[0] ? hdr.owner : hdr.player, MODGHOST_NAMELEN - 1);
		racer->name[MODGHOST_NAMELEN - 1] = '\0';

		g_ModGhostNumRacers++;
	}

	sysLogPrintf(LOG_NOTE, "ghost: racing %d ghost%s on stage 0x%02x",
			g_ModGhostNumRacers, g_ModGhostNumRacers == 1 ? "" : "s", g_Vars.stagenum);
}

/**
 * Replay one frame of the ghost.
 *
 * Runs from chraTickBg(), which is the first thing propsTick() does and
 * therefore before the ghost's own prop is ticked. That ordering is what makes
 * the pose the one the body is drawn from - see the note at the top of this
 * file - and it is also the one point in the frame where a prop can be created
 * or removed without some other prop being part way through its tick.
 */
void modGhostTick(void)
{
	struct modghostsample sample;
	s32 time60;
	s32 i;

	// A body taken away underneath a racer is dropped before anything else
	// looks at it. See modGhostBodyIsGone().
	for (i = 0; i < g_ModGhostNumRacers; i++) {
		if (modGhostBodyIsGone(&g_ModGhostRacers[i])) {
			g_ModGhostRacers[i].chr = NULL;
			g_ModGhostRacers[i].chrnum = -1;
			g_ModGhostRacers[i].weaponheld = -1;
		}
	}

	if (!modGhostStageIsEligible()) {
		modGhostRetireAll();
		return;
	}

	// The first tick of a mission is where the field is read, because it is
	// the first point at which the stage, the difficulty and the player all
	// exist. A stage with no ghosts is remembered as such so that the
	// directory is scanned once rather than once a frame.
	if (g_ModGhostLoadedStage != g_Vars.stagenum
			|| g_ModGhostLoadedDiff != g_MissionConfig.difficulty
			|| g_ModGhostLoadedMode != modGhostGetMode()
			|| g_ModGhostLoadedPick != g_ModGhostPick
			|| g_ModGhostLoadedMax != g_ModGhostMaxRacers) {
		g_ModGhostLoadedStage = g_Vars.stagenum;
		g_ModGhostLoadedDiff = g_MissionConfig.difficulty;
		g_ModGhostLoadedMode = modGhostGetMode();
		g_ModGhostLoadedPick = g_ModGhostPick;
		g_ModGhostLoadedMax = g_ModGhostMaxRacers;

		// The bodies belong to the field being replaced, so they go with it.
		modGhostRetireAll();
		modGhostFreePlayback();

		if (modGhostGetMode() == MODGHOST_RACE) {
			modGhostLoad();
		}
	}

	if (!modGhostIsRacing()) {
		modGhostRetireAll();
		return;
	}

	time60 = playerGetMissionTime();

	for (i = 0; i < g_ModGhostNumRacers; i++) {
		struct modghostracer *racer = &g_ModGhostRacers[i];

		if (racer->samples == NULL) {
			continue;
		}

		// This ghost's run has finished. Its body goes rather than freezing on
		// the last frame, because a ghost standing still in a doorway for the
		// rest of the mission is something to walk into rather than something
		// to chase. The rest of the field carries on.
		if (time60 >= racer->time60) {
			modGhostRetire(racer);
			continue;
		}

		modGhostSampleAt(racer, &sample, time60);

		// A body that cannot be built is tried again next frame, in case
		// whatever was holding the stage pool has given it back. With a field
		// of ten this is also how a pool too small for all of them settles on
		// the number it can carry.
		if (racer->chr == NULL && !modGhostBuild(racer, &sample)) {
			continue;
		}

		modGhostPose(racer, &sample);
	}

	modGhostUpdateSplit(time60);
}

/**
 * Write the run out, if it is worth keeping.
 *
 * Called from the end screen on a completed mission. The conditions are the
 * ones the game already applies to a best time - no cheats, not Perfect
 * Darkness, alive, not aborted, every objective done - because a ghost is a
 * time trial's evidence and a run that could not set a time should not be able
 * to set the pace either.
 *
 * What it is compared against is the ghost that was raced, not the game file's
 * best time. They usually agree, but the ghost directory is the thing being
 * written to and it is the one that has to stay consistent: a best time
 * carried over from a save file whose ghost was never recorded would otherwise
 * lock the directory out of ever getting its first one.
 */
/**
 * Whose name goes in the filename, reduced to something safe to put in a path.
 *
 * The account, when there is one. A ghost downloaded from the server is named
 * after the account that set it, so naming local runs the same way means one
 * directory with one convention in it rather than two - and it stops the same
 * run appearing twice under two names when the agent is renamed or a second
 * agent records on the same account, which is what happened before this.
 *
 * The agent name is the fallback, because a player with no account still needs
 * their files to be told apart from a housemate's.
 *
 * Either way it ends up in a path. The character sets both names come from are
 * small, but neither is this build's to guarantee, and a ghost file is meant to
 * be handed around: a name that is awkward on one filesystem is awkward on
 * somebody else's. Letters and digits survive, everything else becomes an
 * underscore, and the names inside the header are untouched because those are
 * the ones that get shown.
 */
static void modGhostSafeName(char *dst, u32 dstsize)
{
	const char *src = ghostnetGetAccountName();
	u32 i;

	if (src[0] == '\0') {
		src = g_GameFile.name;
	}

	for (i = 0; i + 1 < dstsize && src[i]; i++) {
		char c = src[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
			dst[i] = c;
		} else {
			dst[i] = '_';
		}
	}

	dst[i] = '\0';

	if (i == 0) {
		strncpy(dst, "player", dstsize - 1);
		dst[dstsize - 1] = '\0';
	}
}

/**
 * Draw each racing ghost's name above it.
 *
 * A field of translucent Joannas all running the same corner is unreadable
 * without this: they are the same model in the same suit, and which one is the
 * world record and which one is you last Tuesday is the whole point of racing
 * more than one. The name comes out of the file's header, so a downloaded run
 * carries whoever set it.
 *
 * Anchored above the head rather than on it - MODGHOST_NAMEHEIGHT is a little
 * over Joanna's standing height.
 *
 * Drawn through everything, on purpose. Two narrower versions of this were
 * tried and both were wrong for a race: a line of sight test lost the name
 * whenever the body went behind a pillar, and the renderer's own
 * PROPFLAG_ONTHISSCREENTHISTICK lost it whenever the ghost was in a room the
 * player is not currently looking into - which, for a run that is ahead of
 * you, is most of the run. Where the thing you are chasing has got to is the
 * question the whole mode exists to answer, and it does not stop being worth
 * answering because there is a wall in the way.
 *
 * What is left is geometry rather than visibility. A ghost behind the camera
 * is dropped, because the projection of a point behind you produces a
 * coordinate in front of you, and one whose name would land outside this
 * player's viewport is dropped rather than clamped to the edge - a name pinned
 * to the border reads as a ghost that is there when it is not.
 */
Gfx *modGhostRenderNames(Gfx *gdl)
{
	s32 i;

	if (!modGhostIsRacing()) {
		return gdl;
	}

	for (i = 0; i < g_ModGhostNumRacers; i++) {
		struct modghostracer *racer = &g_ModGhostRacers[i];
		struct coord world;
		struct coord screen;
		s32 textwidth;
		s32 textheight;
		s32 x;
		s32 y;

		if (racer->chr == NULL || modGhostBodyIsGone(racer)) {
			continue;
		}

		world.x = racer->chr->prop->pos.x;
		world.y = racer->chr->prop->pos.y - MODGHOST_PROPLIFT + MODGHOST_NAMEHEIGHT;
		world.z = racer->chr->prop->pos.z;

		// False means the point is behind the camera, where the projection
		// still produces coordinates and they are the mirror of where the
		// ghost is - a name for something behind you, drawn in front of you.
		if (!bg3dPosTo2dPos(&world, &screen)) {
			continue;
		}

		textMeasure(&textheight, &textwidth, racer->name,
				g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);

		x = (s32)screen.x - textwidth / 2;
		y = (s32)screen.y - textheight;

		// Off the side of this player's viewport is off, rather than clamped
		// to the edge: a name pinned to the border of the screen reads as a
		// ghost that is there when it is not.
		if (x < viGetViewLeft() || x + textwidth > viGetViewLeft() + viGetViewWidth()
				|| y < viGetViewTop() || y + textheight > viGetViewTop() + viGetViewHeight()) {
			continue;
		}

		gdl = textRender(gdl, &x, &y, racer->name,
				g_CharsHandelGothicXs, g_FontHandelGothicXs,
				MODGHOST_NAMECOLOUR, 0x000000a0, viGetWidth(), viGetHeight(), 0, 0);
	}

	return gdl;
}

void modGhostSaveRun(void)
{
	struct modghostheader hdr;
	FILE *f;
	char rel[FS_MAXPATH + 1];
	char safename[MODGHOST_NAMELEN];
	s32 time60;

	if (modGhostGetMode() == MODGHOST_OFF || g_ModGhostRecCount < 2) {
		return;
	}

	time60 = playerGetMissionTime();

	if (time60 < 1) {
		return;
	}

	if (fsFileSize(MODGHOST_DIR) < 0 && fsCreateDir(MODGHOST_DIR) != 0) {
		sysLogPrintf(LOG_ERROR, "ghost: could not create %s", fsFullPath(MODGHOST_DIR));
		return;
	}

	// One file per run rather than one per player: every finished run is kept,
	// and the time is part of the name so that a slower one does not land on
	// top of a faster one. Nothing here decides which run is the good one -
	// that is read back out of the headers when a field is picked, so keeping
	// them all costs a file and loses nothing.
	//
	// The time in the name also makes the write idempotent. A run repeated to
	// the same sixtieth overwrites itself instead of adding a near duplicate,
	// which is what a retried cutscene skip or a paused frame looks like.
	//
	// A downloaded ghost is named by whoever made it, without a time, and so
	// cannot collide with this.
	modGhostSafeName(safename, sizeof(safename));

	snprintf(rel, sizeof(rel), MODGHOST_DIR "/pd-s%02d-d%d-%s-%06u" MODGHOST_EXT,
			g_Vars.stagenum, g_MissionConfig.difficulty, safename, (u32)time60);

	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, MODGHOST_MAGIC, sizeof(MODGHOST_MAGIC));

	hdr.version = MODGHOST_VERSION;
	hdr.headersize = sizeof(struct modghostheader);
	hdr.samplesize = sizeof(struct modghostsample);
	hdr.numsamples = g_ModGhostRecCount;
	hdr.time60 = time60;
	hdr.rate60 = MODGHOST_RATE60;
	// What the run was set under, so that a file outlives the settings that
	// made it. Runs recorded before this carry a zero here, which reads as
	// "unknown" rather than as "no rules" - they were made when the fork's
	// moves were available and there is no way to ask now which were on.
	hdr.flags = MODGHOSTHF_TRIALRULES;

	// Who the run was set as, so the ghost of it looks like the person who set
	// it wherever it ends up. Clamped to a byte because that is what the field
	// is; the picker cannot reach a value that would not fit, but a config
	// file edited by hand can.
	hdr.mpbody = (u8)(g_ModGhostBody > 0 && g_ModGhostBody < 256 ? g_ModGhostBody : 0);
	hdr.mphead = (u8)(g_ModGhostHead > 0 && g_ModGhostHead < 256 ? g_ModGhostHead : 0);

	hdr.stagenum = g_Vars.stagenum;
	hdr.difficulty = g_MissionConfig.difficulty;
	hdr.stageindex = g_MissionConfig.stageindex;
	hdr.hash = modGhostHash(g_ModGhostRec, g_ModGhostRecCount * sizeof(struct modghostsample));
	hdr.timestamp = (u32)time(NULL);

	strncpy(hdr.player, g_GameFile.name[0] ? g_GameFile.name : "player", MODGHOST_NAMELEN - 1);
	strncpy(hdr.build, VERSION_BUILD, MODGHOST_NAMELEN - 1);

	// Whose run this is to publish, as opposed to who ran it. Written now
	// rather than at upload time because the file is what travels: by the time
	// somebody else is holding it, the only thing that can say it was not
	// theirs is the file itself. Empty when nobody is signed in, which is not
	// a failure - it is a run recorded before an account existed, and it
	// stays uploadable by the agent that set it.
	strncpy(hdr.owner, ghostnetGetAccountName(), MODGHOST_OWNERLEN - 1);

	f = fsFileOpenWrite(rel);

	if (f == NULL) {
		sysLogPrintf(LOG_ERROR, "ghost: could not write %s", fsFullPath(rel));
		return;
	}

	if (fwrite(&hdr, sizeof(hdr), 1, f) != 1
			|| fwrite(g_ModGhostRec, sizeof(struct modghostsample), g_ModGhostRecCount, f)
				!= (size_t)g_ModGhostRecCount) {
		sysLogPrintf(LOG_ERROR, "ghost: %s was not written in full", fsFullPath(rel));
	} else {
		sysLogPrintf(LOG_NOTE, "ghost: saved %s (%d:%02d)", fsFullPath(rel),
				time60 / 3600, (time60 / 60) % 60);
	}

	fclose(f);
}

/**
 * Forget the stage that has just ended.
 *
 * The body's chr, prop and model all came out of MEMPOOL_STAGE, which has gone
 * by the time this runs, so the pointer is dropped rather than the body being
 * removed - there is nothing left to remove it from.
 *
 * The next stage's ghost is not read here. This runs from
 * playermgrAllocatePlayer(), which is early enough that the player has no prop
 * yet and, on the way into the Institute or a Combat Sim match, early enough
 * that what the stage turns out to be is not yet worth asking. So the read is
 * left to the first tick that finds itself in a mission, which is also the
 * first moment at which there is anything to race.
 */
void modGhostReset(void)
{
	s32 i;

	// The bodies came out of a stage pool that has gone, so the pointers are
	// dropped rather than the bodies removed - there is nothing left to remove
	// them from. The samples are this file's own malloc and survive, but the
	// field is re-read for the next stage anyway.
	for (i = 0; i < MODGHOST_MAXRACERS; i++) {
		g_ModGhostRacers[i].chr = NULL;
		g_ModGhostRacers[i].chrnum = -1;
		g_ModGhostRacers[i].weaponheld = -1;
		g_ModGhostRacers[i].angleoffset = 0.0f;
	}

	// The shared definitions belong to the stage as well.
	g_ModGhostNumBodyDefs = 0;

	g_ModGhostSplitIdx = 0;
	g_ModGhostSplit60 = 0;
	g_ModGhostSplitValid = false;

	modGhostFreeRecording();
}
