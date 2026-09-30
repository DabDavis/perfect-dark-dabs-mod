#ifndef _IN_SIMBRAIN_H
#define _IN_SIMBRAIN_H

/**
 * The simulants' brain switch, Mod.SimBrain (PLANS/AI-REWORK.md M3 onwards).
 *
 * "stock" (the default) is Perfect Dark's simulant AI with this fork's fixes,
 * bit for bit: nothing below runs, nothing is built or allocated.
 *
 * "modern" keeps stock's decisions - bot.c still picks where a simulant
 * wants to go and calls chrGoToRoomPos() - and gets it there along the stage's
 * navmesh (port/src/simnav*.cpp): a Detour path, followed through its
 * polygons, crossing ladders, lifts, drops and jumps as off-mesh links, with
 * local avoidance of the other chrs. What it hands the chr code is only what
 * a controller would: which way to walk (the point the simulant turns to),
 * how fast, when to stand, jump or duck. Stock chr movement, collision and
 * physics do the rest. Where there is no mesh (GoldenEye's converted levels,
 * a failed build) or no path, the simulant follows stock's waypoints.
 *
 * The choice is read as a match starts (lvReset()); changing it during one
 * takes effect at the next.
 */

#include <ultra64.h>
#include "types.h"

struct simnavmesh;

// A simulant's controller for one tick, as the navmesh follower fills it
struct simcontrol {
	f32 aim[3];  // the point it walks to (it turns straight to it)
	f32 speed;   // of its top speed, 0 to 1
	u8 wait;     // stand still here
	u8 duck;     // GOPOSFLAG_DUCK: lower over the next stretch
	u8 crouch;   // GOPOSFLAG_CROUCH
	u8 jump;     // botTryJump() this tick
};

// What simbrainTickGoPos() did with the tick
#define SIMBRAIN_TICK_STOCK 0 // nothing: stock's route follower has it
#define SIMBRAIN_TICK_WALK  1 // walk to control's aim
#define SIMBRAIN_TICK_FINAL 2 // on the last stretch: arrive at the end as stock does

extern s32 g_SimBrainModern; // this match's choice, latched at its start

// Whether the option asks for the modern brain (Mod.SimBrain, --simbrain)
s32 simbrainWanted(void);
s32 simbrainGetOption(void);
void simbrainSetOption(s32 modern);

// simnavStageStart(): the match's choice, and the agents on the stage's mesh
void simbrainStageStart(const struct simnavmesh *mesh);
void simbrainStageStop(void);

// Whether the navmesh has this simulant's legs right now
bool simbrainOwns(struct chrdata *chr);

// chrGoToRoomPos(), first: a simulant crossing a link keeps crossing it, and
// takes up the new goal afterwards. True if the call is to go no further.
bool simbrainHoldsGoTo(struct chrdata *chr, struct coord *pos, RoomNum *rooms);

// chrGoToRoomPos(), last: stock has routed (or not); the navmesh's path is
// asked for. What chrGoToRoomPos() returns.
bool simbrainAfterGoTo(struct chrdata *chr, struct coord *pos, RoomNum *rooms, u32 goposflags, bool stockrouted);

// chrTickGoPos(): the simulant's tick along its path; SIMBRAIN_TICK_*
s32 simbrainTickGoPos(struct chrdata *chr, struct coord *aim);

// The debug view (Mod.SimNavDebug): the corners of a simulant's path ahead
s32 simbrainDebugPath(struct chrdata *chr, f32 *points, s32 maxpoints);

#endif
