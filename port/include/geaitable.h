/**
 * The GoldenEye -> Perfect Dark AI command map.
 *
 * Started by .xbla-work/ge-arena/genaitable.py and kept by hand since - that
 * generator is older than this table and running it would throw away every row
 * read since. The reference copy is tools/geconvert/geaitable.py; a row is
 * changed in both, and the two must convert to the same bytes.
 *
 * `gepad`, `getext`, `geanim` and `gelist` are the arguments the script names
 * rather than numbers: a bit is set for each GoldenEye argument whose name holds
 * PAD (and is two bytes or wider), for each one named TEXT_SLOT, for each one
 * named ANIMATION_ID and for each one named AI_LIST_ID, which are the ones the
 * conversion rewrites rather than copies. A list id of 1024 or less is one of
 * GoldenEye's own global lists and moves with them (soloGlobalAiId()).
 * An animation id is written with GEAI_ANIM_TAG set, which is what tells one of
 * GoldenEye's from the number a stock Perfect Dark ailist's own
 * aiChrDoAnimation carries - a mission's chrs do fall back on those lists.
 */
#ifndef GEAITABLE_H
#define GEAITABLE_H

#define GEAI_NUM_COMMANDS 253
#define GEAI_MAX_ARGS 12
// Perfect Dark's own chr ids (constants.h). The rows below carry 0x00f2
// inlined, which is CHR_P1P2 - the generator called it CHR_BOND, and for a
// solo mission the two resolve to the same player.
#define GEAI_CHR_BOND 0x00f8
#define GEAI_CHR_SELF 0x00fd

// PlayAnimation's bitfield is Perfect Dark's chranimflags bit for bit where the
// two games kept the same meaning - chrStartAnim() is GoldenEye's own
// chrlvPerformAnimationForActor() line for line - and the conversion keeps only
// those bits: 0x01 mirror/FLIP, 0x02 (unknown)/MOVEWHENINVIS, 0x04 hold last
// frame/PAUSEATEND, 0x10 idle on end/SLOWUPDATE (both set chr->sleep = merge),
// 0x40 no translation/LOCKPOS and 0x80 reverse/REVERSE - and 0x08, which the
// decompilation calls "play the sneeze sound" and which is nothing of the kind:
// its one use in GoldenEye is chrHasStoppedOrPatroling(), where a chr playing
// an animation with it set *counts as stopped*, and CHRANIMFLAG_COMPLETED's one
// use in Perfect Dark is the same line of chrIsStopped(). Every idle animation
// of GoldenEye's carries it, because its standard guard list only looks and
// listens when the guard is stopped: with the bit dropped a guard scratching
// himself saw nothing, heard nothing and did not notice being shot at, for six
// seconds at a time and one animation after another. 0x20 is a translation
// scale of four that Perfect Dark has no flag for, and the port gives it one
// (CHRANIMFLAG_GE_TRANSLATE4X, chrStartAnim()): Dam's dive and the Cradle's fall
// are authored a quarter size, and with the bit dropped Bond stood to his knees
// in Dam's platform and fell a quarter of the way down it. Nothing goes.
#define GEAI_ANIM_FLAGS 0x00ff

// set on a converted animation id (gesolo.py's GE_ANIM_TAG)
#define GEAI_ANIM_TAG 0x8000

// The port's own command that plays one of GoldenEye's three **vehicle**
// animations on a truck's or an aircraft's model, past the game's table as
// aiGeExitOnButtonPress is. A vehicle's PlayAnimation becomes this: GoldenEye
// plays it straight on the model rather than through a chr's action, and its
// bitfield has no meaning there (chrai.c reads nothing but the interpolation
// time in its aircraft branch). Nine bytes, GoldenEye's own arguments:
//     01e2 <anim id:2> <start frame:2> <end frame:2> <interpolate:1>
#define GEVEH_ANIM_CMD 0x01e2

// And the one that asks whether Bond is under a height - GoldenEye's
// IFBondYPosLessThan, which Perfect Dark has no command for. Dam's dive waits
// on it. Seven bytes, the height already moved into the converted level:
//     01e3 <y:4, signed> <label:1>
#define GEAI_IFBONDY_CMD 0x01e3

// And GoldenEye's IFChrWasShotSinceLastCheck, which asks CHRFLAG_WAS_HIT - set
// by every shot that lands, invincible or not - where Perfect Dark's nearest,
// aiIfInjured, is its IFChrWasDamagedSinceLastCheck and never passes while the
// chr is invincible. The Cradle's Trevelyan waits, invincible, to be shot at
// before he runs on. aiIfInjured's own four bytes, and an ordinary row below:
//     01e4 <chr:1> <label:1>
#define GEAI_WASHIT_CMD 0x01e4

// And GoldenEye's ObjectRocketLaunch, which Perfect Dark has no command for:
// the tagged object becomes a projectile that climbs away (Aztec's shuttle in
// its ending). Three bytes: 01e5 <object tag:1>
#define GEAI_ROCKET_CMD 0x01e5

// And its ChrRemoveItemInHand: the gun a chr holds in one hand goes (Silo's
// ending, where Bond puts his away and crosses his arms). Four bytes:
//     01e6 <chr:1> <hand:1>
#define GEAI_REMOVEITEM_CMD 0x01e6

// GoldenEye's chr flags are one byte of its own (chr->flags2, set and tested by
// six of its commands), and neither of Perfect Dark's two banks has eight bits
// to spare - every bit of theirs means something to the game. The byte gets a
// bank of its own in the port instead: BANK_GE, chrdata.geflags2, which nothing
// but a converted list ever reads. The six rows are then Perfect Dark's own
// flag commands with that bank named, and its test is "any of these bits"
// exactly as GoldenEye's is.
#define GEAI_BANK_GE 0x0002

// an argument spec: `from` is the GoldenEye argument, or -1 for a constant.
// `mask`, where it is not 0, is ANDed over the argument's value (PlayAnimation's
// bitfield, whose bits are only partly Perfect Dark's chranimflags)
struct geaiarg {
	int8_t from;
	uint8_t width;
	uint16_t value;
	uint16_t mask;
};

struct geaicmd {
	uint8_t len;      // GoldenEye's length in bytes, 0 when it is measured (PRINT)
	int16_t pd;       // Perfect Dark's opcode, -1 when there is no equivalent
	uint8_t numge;    // GoldenEye's own arguments, in order
	uint8_t gewidth[GEAI_MAX_ARGS];
	uint16_t gepad;   // a bit per GoldenEye argument that is a pad id
	uint16_t getext;  // and a bit per one that is a text id (gesolo.py's text_id)
	uint16_t geanim;  // and a bit per one that is a GoldenEye animation id
	uint16_t gelist;  // and a bit per one that is an AI list id (soloGlobalAiId)

	uint8_t numargs;  // and what Perfect Dark's command is given
	struct geaiarg args[GEAI_MAX_ARGS];
};

static const struct geaicmd g_GeAiCommands[GEAI_NUM_COMMANDS] = {
	/* 00 GotoNext                               */ {  2, 0x0000,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 01 GotoFirst                              */ {  2, 0x0001,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 02 Label                                  */ {  2, 0x0002,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 03 Yield                                  */ {  1, 0x0003,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 04 EndList                                */ {  1, 0x0004,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 05 SetChrAiList                           */ {  4, 0x0005,  2, { 1, 2 }, 0x0000, 0x0000, 0x0000, 0x0002,  2, { {0, 1, 0}, {1, 2, 0} } },
	/* 06 SetReturnAiList                        */ {  3, 0x0006,  1, { 2 }, 0x0000, 0x0000, 0x0000, 0x0001,  2, { {-1, 1, 0x00fd}, {0, 2, 0} } },
	/* 07 Return                                 */ {  1, 0x0008,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 08 Stop                                   */ {  1, 0x0009,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 09 Kneel                                  */ {  1, 0x000a,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 0a PlayAnimation                          */ {  9, 0x000b,  5, { 2, 2, 2, 1, 1 }, 0x0000, 0x0000, 0x0001, 0x0000,  7, { {0, 2, 0}, {1, 2, 0}, {2, 2, 0}, {3, 1, 0, GEAI_ANIM_FLAGS}, {4, 1, 0}, {-1, 1, GEAI_CHR_SELF}, {-1, 1, 2} } },
	/* 0b IFPlayingAnimation                     */ {  2, 0x000c,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 0c PointAtBond                            */ {  1, 0x000d,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 0d LookSurprised                          */ {  1, 0x000e,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 0e TRYSidestepping                        */ {  2, 0x000f,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 0f TRYSideHopping                         */ {  2, 0x0010,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 10 TRYSideRunning                         */ {  2, 0x0011,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 11 TRYFiringWalk                          */ {  2, 0x0012,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 12 TRYFiringRun                           */ {  2, 0x0013,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 13 TRYFiringRoll                          */ {  2, 0x0014,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 14 TRYFireOrAimAtTarget                   */ {  6, 0x0015,  3, { 2, 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 2, 0}, {2, 1, 0} } },
	/* 15 TRYFireOrAimAtTargetKneel              */ {  6, 0x0016,  3, { 2, 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 2, 0}, {2, 1, 0} } },
	/* 16 TRYFireOrAimAtTargetUpdate             */ {  6, 0x0017,  3, { 2, 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 2, 0}, {2, 1, 0} } },
	/* 17 TRYFacingTarget                        */ {  6, 0x0018,  3, { 2, 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 2, 0}, {2, 1, 0} } },
	/* 18 HitChrWithItem                         */ {  4,     -1,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 19 ChrHitChr                              */ {  4, 0x001a,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 1a TRYThrowingGrenade                     */ {  2, 0x001b,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {-1, 2, 0x0200}, {-1, 2, 0x0000}, {0, 1, 0} } },
	/* 1b TRYDroppingItem                        */ {  5, 0x001c,  3, { 2, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 1c RunToPad                               */ {  3, 0x001d,  1, { 2 }, 0x0001, 0x0000, 0x0000, 0x0000,  1, { {0, 2, 0} } },
	/* 1d RunToPadPreset                         */ {  1, 0x001e,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {-1, 1, 0x0000} } },
	/* 1e WalkToPad                              */ {  3, 0x001f,  1, { 2 }, 0x0001, 0x0000, 0x0000, 0x0000,  1, { {0, 2, 0} } },
	/* 1f SprintToPad                            */ {  3, 0x0020,  1, { 2 }, 0x0001, 0x0000, 0x0000, 0x0000,  1, { {0, 2, 0} } },
	/* 20 StartPatrol                            */ {  2, 0x0021,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },  // + aiStartPatrol, writeSoloAilist()
	/* 21 Surrender                              */ {  1, 0x0024,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 22 RemoveMe                               */ {  1, 0x0025,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 23 ChrRemoveInstant                       */ {  2, 0x0026,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 24 TRYTriggeringAlarmAtPad                */ {  4, 0x0027,  2, { 2, 1 }, 0x0001, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 25 AlarmOn                                */ {  1, 0x0028,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 26 AlarmOff                               */ {  1, 0x0029,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 27 TRYRunFromBond                         */ {  2, 0x002a,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 28 TRYRunToBond                           */ {  2, 0x002b,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 29 TRYWalkToBond                          */ {  2, 0x002c,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 2a TRYSprintToBond                        */ {  2, 0x002d,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 2b TRYFindCover                           */ {  2, 0x002e,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 2c TRYRunToChr                            */ {  3, 0x002f,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 2d TRYWalkToChr                           */ {  3, 0x0030,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 2e TRYSprintToChr                         */ {  3, 0x0031,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 2f IFImOnPatrolOrStopped                  */ {  2, 0x0032,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 30 IFChrDyingOrDead                       */ {  3, 0x0033,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 31 IFChrDoesNotExist                      */ {  3, 0x0034,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 32 IFISeeBond                             */ {  2, 0x0035,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 33 SetNewRandom                           */ {  1, 0x0036,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 34 IFRandomLessThan                       */ {  3, 0x0037,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 35 IFRandomGreaterThan                    */ {  3, 0x0038,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 36 IFICanHearAlarm                        */ {  2, 0x0039,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 37 IFAlarmIsOn                            */ {  2, 0x003a,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 38 IFGasIsLeaking                         */ {  2, 0x003b,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 39 IFIHeardBond                           */ {  2, 0x003c,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 3a IFISeeSomeoneShot                      */ {  2, 0x003d,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x0002}, {0, 1, 0} } },
	/* 3b IFISeeSomeoneDie                       */ {  2, 0x003e,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x0002}, {0, 1, 0} } },
	/* 3c IFICouldSeeBond                        */ {  2, 0x003f,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 3d IFICouldSeeBondsStan                   */ {  2, 0x003f,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 3e IFIWasShotRecently                     */ {  2, 0x0043,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 3f IFIHeardBondRecently                   */ {  2, 0x0044,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 40 IFImInRoomWithChr                      */ {  3, 0x0045,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 41 IFIveNotBeenSeen                       */ {  2, 0x0046,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 42 IFImOnScreen                           */ {  2, 0x0047,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 43 IFMyRoomIsOnScreen                     */ {  2, 0x0048,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x00fd}, {0, 1, 0} } },
	/* 44 IFRoomWithPadIsOnScreen                */ {  4, 0x0049,  2, { 2, 1 }, 0x0001, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 45 IFImTargetedByBond                     */ {  2, 0x004a,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 46 IFBondMissedMe                         */ {  2, 0x004b,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 47 IFMyAngleToBondLessThan                */ {  3, 0x004d,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 48 IFMyAngleToBondGreaterThan             */ {  3, 0x004f,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 49 IFMyAngleFromBondLessThan              */ {  3,     -1,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 4a IFMyAngleFromBondGreaterThan           */ {  3,     -1,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* 4b IFMyDistanceToBondLessThanDecimeter    */ {  4, 0x0052,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 4c IFMyDistanceToBondGreaterThanDecimeter */ {  4, 0x0053,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 4d IFChrDistanceToPadLessThanDecimeter    */ {  7, 0x0054,  4, { 1, 2, 2, 1 }, 0x0004, 0x0000, 0x0000, 0x0000,  4, { {0, 1, 0}, {1, 2, 0}, {2, 2, 0}, {3, 1, 0} } },
	/* 4e IFChrDistanceToPadGreaterThanDecimeter */ {  7, 0x0055,  4, { 1, 2, 2, 1 }, 0x0004, 0x0000, 0x0000, 0x0000,  4, { {0, 1, 0}, {1, 2, 0}, {2, 2, 0}, {3, 1, 0} } },
	/* 4f IFMyDistanceToChrLessThanDecimeter     */ {  5, 0x0056,  3, { 2, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 50 IFMyDistanceToChrGreaterThanDecimeter  */ {  5, 0x0057,  3, { 2, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 51 TRYSettingMyPresetToChrWithinDistanceDecimeter */ {  4, 0x0058,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 52 IFBondDistanceToPadLessThanDecimeter   */ {  6, 0x0059,  3, { 2, 2, 1 }, 0x0002, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 2, 0}, {2, 1, 0} } },
	/* 53 IFBondDistanceToPadGreaterThanDecimeter */ {  6, 0x005a,  3, { 2, 2, 1 }, 0x0002, 0x0000, 0x0000, 0x0000,  3, { {0, 2, 0}, {1, 2, 0}, {2, 1, 0} } },
	/* 54 IFChrInRoomWithPad                     */ {  5, 0x005b,  3, { 1, 2, 1 }, 0x0002, 0x0000, 0x0000, 0x0000,  4, { {0, 1, 0}, {-1, 1, 0x0000}, {1, 2, 0}, {2, 1, 0} } },
	/* 55 IFBondInRoomWithPad                    */ {  4, 0x005c,  2, { 2, 1 }, 0x0001, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 56 IFBondCollectedObject                  */ {  3, 0x005d,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {-1, 1, 0x00f2}, {0, 1, 0}, {1, 1, 0} } },
	/* 57 IFKeyDropped                           */ {  3, 0x005e,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 58 IFItemIsAttachedToObject               */ {  4, 0x005f,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 59 IFBondHasItemEquipped                  */ {  3, 0x0060,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {-1, 1, 0x00f2}, {0, 1, 0}, {1, 1, 0} } },
	/* 5a IFObjectExists                         */ {  3, 0x0062,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 5b IFObjectNotDestroyed                   */ {  3, 0x0062,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 5c IFObjectWasActivated                   */ {  3, 0x0063,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {-1, 1, 0x00f2}, {0, 1, 0}, {1, 1, 0} } },
	/* 5d IFBondUsedGadgetOnObject               */ {  3, 0x0063,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {-1, 1, 0x00f2}, {0, 1, 0}, {1, 1, 0} } },
	/* 5e ActivateObject                         */ {  2, 0x0065,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 5f DestroyObject                          */ {  2, 0x0066,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 60 DropObject                             */ {  2, 0x0067,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 61 ChrDropAllConcealedItems               */ {  2, 0x0068,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 62 ChrDropAllHeldItems                    */ {  2, 0x0069,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 63 BondCollectObject                      */ {  2, 0x006a,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {-1, 1, 0x00f2} } },
	/* 64 ChrEquipObject                         */ {  3, 0x006a,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 65 MoveObject                             */ {  4, 0x006b,  2, { 1, 2 }, 0x0002, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 2, 0} } },
	/* 66 DoorOpen                               */ {  2, 0x006c,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 67 DoorClose                              */ {  2, 0x006d,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 68 IFDoorStateEqual                       */ {  4, 0x006e,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 69 IFDoorHasBeenOpenedBefore              */ {  3, 0x006f,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 6a DoorSetLock                            */ {  3, 0x0070,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 6b DoorUnsetLock                          */ {  3, 0x0071,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 6c IFDoorLockEqual                        */ {  4, 0x0072,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 6d IFObjectiveNumComplete                 */ {  3, 0x0073,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 6e TRYUnknown6e                           */ {  3, 0x0075,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 6f TRYUnknown6f                           */ {  3, 0x0076,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 70 IFGameDifficultyLessThan               */ {  3, 0x0077,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 71 IFGameDifficultyGreaterThan            */ {  3, 0x0078,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 72 IFMissionTimeLessThan                  */ {  4, 0x0079,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 73 IFMissionTimeGreaterThan               */ {  4, 0x007a,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 74 IFSystemPowerTimeLessThan              */ {  4, 0x0079,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 75 IFSystemPowerTimeGreaterThan           */ {  4, 0x007a,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* 76 IFLevelIdLessThan                      */ {  3, 0x007b,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 77 IFLevelIdGreaterThan                   */ {  3, 0x007c,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 78 IFMyNumArghsLessThan                   */ {  3, 0x007d,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 79 IFMyNumArghsGreaterThan                */ {  3, 0x007e,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 7a IFMyNumCloseArghsLessThan              */ {  3, 0x007f,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 7b IFMyNumCloseArghsGreaterThan           */ {  3, 0x0080,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 7c IFChrHealthLessThan                    */ {  4, 0x0081,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 7d IFChrHealthGreaterThan                 */ {  4, 0x0082,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* 7e IFChrWasDamagedSinceLastCheck          */ {  3, 0x0083,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 7f IFBondHealthLessThan                   */ {  3, 0x0081,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {-1, 1, 0x00f2}, {0, 1, 0}, {1, 1, 0} } },
	/* 80 IFBondHealthGreaterThan                */ {  3, 0x0082,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {-1, 1, 0x00f2}, {0, 1, 0}, {1, 1, 0} } },
	/* 81 SetMyMorale                            */ {  2, 0x0084,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 82 AddToMyMorale                          */ {  2, 0x0085,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 83 SubtractFromMyMorale                   */ {  2, 0x0087,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 84 IFMyMoraleLessThan                     */ {  3, 0x0088,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* 85 IFMyMoraleLessThanRandom               */ {  2, 0x0089,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 86 SetMyAlertness                         */ {  2, 0x008a,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 87 AddToMyAlertness                       */ {  2, 0x008b,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 88 SubtractFromMyAlertness                */ {  2, 0x008d,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 89 IFMyAlertnessLessThan                  */ {  3, 0x008f,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {-1, 1, 0x00fd}, {1, 1, 0} } },
	/* 8a IFMyAlertnessLessThanRandom            */ {  2, 0x0090,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 8b SetMyHearingScale                      */ {  3, 0x0092,  1, { 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 2, 0} } },
	/* 8c SetMyVisionRange                       */ {  2, 0x0093,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 8d SetMyGrenadeProbability                */ {  2, 0x0094,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 8e SetMyChrNum                            */ {  2, 0x0095,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 8f SetMyHealthTotal                       */ {  3, 0x0096,  1, { 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x00fd}, {0, 2, 0} } },
	/* 90 SetMyArmour                            */ {  3, 0x0097,  1, { 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 2, 0} } },
	/* 91 SetMySpeedRating                       */ {  2, 0x0098,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 92 SetMyArghRating                        */ {  2, 0x0099,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 93 SetMyAccuracyRating                    */ {  2, 0x009a,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* 94 SetMyFlags2                            */ {  2, 0x009b,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 4, 0}, {-1, 1, GEAI_BANK_GE} } },
	/* 95 UnsetMyFlags2                          */ {  2, 0x009c,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 4, 0}, {-1, 1, GEAI_BANK_GE} } },
	/* 96 IFMyFlags2Has                          */ {  3, 0x009d,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  4, { {0, 4, 0}, {-1, 1, 0x0001}, {-1, 1, GEAI_BANK_GE}, {1, 1, 0} } },
	/* 97 SetChrBitfield                         */ {  3, 0x009e,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 4, 0}, {-1, 1, GEAI_BANK_GE} } },
	/* 98 UnsetChrBitfield                       */ {  3, 0x009f,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 4, 0}, {-1, 1, GEAI_BANK_GE} } },
	/* 99 IFChrBitfieldHas                       */ {  4, 0x00a0,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  4, { {0, 1, 0}, {1, 4, 0}, {-1, 1, GEAI_BANK_GE}, {2, 1, 0} } },
	/* 9a SetObjectiveBitfield                   */ {  5, 0x00a1,  1, { 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 4, 0} } },
	/* 9b UnsetObjectiveBitfield                 */ {  5, 0x00a2,  1, { 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 4, 0} } },
	/* 9c IFObjectiveBitfieldHas                 */ {  6, 0x00a3,  2, { 4, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 4, 0}, {-1, 1, 0x0001}, {1, 1, 0} } },
	/* 9d SetMychrflags                          */ {  5, 0x00a4,  1, { 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 4, 0} } },
	/* 9e UnsetMychrflags                        */ {  5, 0x00a5,  1, { 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 4, 0} } },
	/* 9f IFMychrflagsHas                        */ {  6, 0x00a6,  2, { 4, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 4, 0}, {1, 1, 0} } },
	/* a0 SetChrchrflags                         */ {  6, 0x00a7,  2, { 1, 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 4, 0} } },
	/* a1 UnsetChrchrflags                       */ {  6, 0x00a8,  2, { 1, 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 4, 0} } },
	/* a2 IFChrchrflagsHas                       */ {  7, 0x00a9,  3, { 1, 4, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 4, 0}, {2, 1, 0} } },
	/* a3 SetObjectFlags                         */ {  6, 0x00aa,  2, { 1, 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 4, 0} } },
	/* a4 UnsetObjectFlags                       */ {  6, 0x00ab,  2, { 1, 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 4, 0} } },
	/* a5 IFObjectFlagsHas                       */ {  7, 0x00ac,  3, { 1, 4, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 4, 0}, {2, 1, 0} } },
	/* a6 SetObjectFlags2                        */ {  6, 0x00ad,  2, { 1, 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 4, 0} } },
	/* a7 UnsetObjectFlags2                      */ {  6, 0x00ae,  2, { 1, 4 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 4, 0} } },
	/* a8 IFObjectFlags2Has                      */ {  7, 0x00af,  3, { 1, 4, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 4, 0}, {2, 1, 0} } },
	/* a9 SetMyChrPreset                         */ {  2, 0x00b0,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* aa SetChrChrPreset                        */ {  3, 0x00b1,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* ab SetMyPadPreset                         */ {  3, 0x00b2,  1, { 2 }, 0x0001, 0x0000, 0x0000, 0x0000,  1, { {0, 2, 0} } },
	/* ac SetChrPadPreset                        */ {  4, 0x00b3,  2, { 1, 2 }, 0x0002, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 2, 0} } },
	/* ad PRINT                                  */ {  0,     -1,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* ae MyTimerStart                           */ {  1, 0x00b6,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* af MyTimerReset                           */ {  1, 0x00b7,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* b0 MyTimerPause                           */ {  1, 0x00b8,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* b1 MyTimerResume                          */ {  1, 0x00b9,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* b2 IFMyTimerIsNotRunning                  */ {  2, 0x00ba,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* b3 IFMyTimerLessThanTicks                 */ {  5, 0x00bc,  2, { 3, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 3, 0}, {1, 1, 0} } },
	/* b4 IFMyTimerGreaterThanTicks              */ {  5, 0x00bd,  2, { 3, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 3, 0}, {1, 1, 0} } },
	/* b5 HudCountdownShow                       */ {  1, 0x00be,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* b6 HudCountdownHide                       */ {  1, 0x00bf,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* b7 HudCountdownSet                        */ {  3, 0x00c0,  1, { 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 2, 0} } },
	/* b8 HudCountdownStop                       */ {  1, 0x00c1,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* b9 HudCountdownStart                      */ {  1, 0x00c2,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* ba IFHudCountdownIsNotRunning             */ {  2, 0x00c3,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* bb IFHudCountdownLessThan                 */ {  4, 0x00c4,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* bc IFHudCountdownGreaterThan              */ {  4, 0x00c5,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* bd TRYSpawningChrAtPad                    */ { 12, 0x00c6,  6, { 1, 1, 2, 2, 4, 1 }, 0x0004, 0x0000, 0x0000, 0x0008,  6, { {0, 1, 0}, {1, 1, 0}, {2, 2, 0}, {3, 2, 0}, {4, 4, 0}, {5, 1, 0} } },
	/* be TRYSpawningChrNextToChr                */ { 11, 0x00c7,  6, { 1, 1, 1, 2, 4, 1 }, 0x0000, 0x0000, 0x0000, 0x0008,  6, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0}, {3, 2, 0}, {4, 4, 0}, {5, 1, 0} } },
	/* bf TRYGiveMeItem                          */ {  9, 0x00c8,  4, { 2, 1, 4, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  4, { {0, 2, 0}, {1, 1, 0}, {2, 4, 0}, {3, 1, 0} } },
	/* c0 TRYGiveMeHat                           */ {  8,     -1,  3, { 2, 4, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* c1 TRYCloningChr                          */ {  5,     -1,  3, { 1, 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0002,  0, { {0, 0, 0} } },
	/* c2 TextPrintBottom                        */ {  3, 0x00cb,  1, { 2 }, 0x0000, 0x0001, 0x0000, 0x0000,  2, { {-1, 1, 0x00f8}, {0, 2, 0} } },
	/* c3 TextPrintTop                           */ {  3, 0x00cc,  1, { 2 }, 0x0000, 0x0001, 0x0000, 0x0000,  3, { {-1, 1, 0x00f8}, {0, 2, 0}, {-1, 1, 2} } },
	/* c4 SfxPlay                                */ {  4, 0x00ce,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 1, 0} } },
	/* c5 SfxEmitFromObject                      */ {  5, 0x00cf,  3, { 1, 1, 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 2, 0} } },
	/* c6 SfxEmitFromPad                         */ {  6, 0x00d0,  3, { 1, 2, 2 }, 0x0002, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 2, 0}, {2, 2, 0} } },
	/* c7 SfxSetChannelVolume                    */ {  6, 0x00d1,  3, { 1, 2, 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 2, 0}, {2, 2, 0} } },
	/* c8 SfxFadeChannelVolume                   */ {  6, 0x00d2,  3, { 1, 2, 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 2, 0}, {2, 2, 0} } },
	/* c9 SfxStopChannel                         */ {  2, 0x00d3,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* ca IFSfxChannelVolumeLessThan             */ {  5, 0x00d4,  3, { 1, 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 2, 0}, {2, 1, 0} } },
	/* cb VehicleStartPath                       */ {  2, 0x00d5,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* cc VehicleSpeed                           */ {  5, 0x00d6,  2, { 2, 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 2, 0} } },
	/* cd AircraftRotorSpeed                     */ {  5, 0x00d7,  2, { 2, 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 2, 0}, {1, 2, 0} } },
	/* ce IFCameraIsInIntro                      */ {  2, 0x00d8,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* cf IFCameraIsInBondSwirl                  */ {  2, 0x00d9,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* d0 TvChangeScreenBank                     */ {  4, 0x00da,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* d1 IFBondInTank                           */ {  2, 0x00db,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* d2 EndLevel                               */ {  1, 0x00dc,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* d3 CameraReturnToBond                     */ {  1,     -1,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* d4 CameraLookAtBondFromPad                */ {  3,     -1,  1, { 2 }, 0x0001, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* d5 CameraSwitch                           */ {  6, 0x00df,  3, { 1, 2, 2 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 2, 0}, {2, 2, 0} } },
	/* d6 IFBondYPosLessThan                     */ {  4,     -1,  2, { 2, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* d7 BondDisableControl                     */ {  2, 0x00e0,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x00f2}, {0, 1, 0} } },
	/* d8 BondEnableControl                      */ {  1, 0x00e1,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {-1, 1, 0x00f2} } },
	/* d9 TRYTeleportingChrToPad                 */ {  5, 0x00e2,  3, { 1, 2, 1 }, 0x0002, 0x0000, 0x0000, 0x0000,  4, { {0, 1, 0}, {1, 2, 0}, {-1, 1, 0x0001}, {2, 1, 0} } },
	/* da ScreenFadeToBlack                      */ {  1, 0x01cb,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 4, 0x00ff}, {-1, 2, 60} } },
	/* db ScreenFadeFromBlack                    */ {  1, 0x01cb,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 4, 0x0000}, {-1, 2, 60} } },
	/* dc IFScreenFadeCompleted                  */ {  2, 0x01cc,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* dd HideAllChrs                            */ {  1, 0x01d5,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {-1, 1, 0x0000} } },
	/* de ShowAllChrs                            */ {  1, 0x01d5,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {-1, 1, 0x0001} } },
	/* df DoorOpenInstant                        */ {  2, 0x00e8,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* e0 ChrRemoveItemInHand                    */ {  3, 0x01e6,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* e1 IfNumberOfActivePlayersLessThan        */ {  3, 0x00ea,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* e2 IFBondItemTotalAmmoLessThan            */ {  4,     -1,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* e3 BondEquipItem                          */ {  2, 0x00ec,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x00f2}, {0, 1, 0} } },
	/* e4 BondEquipItemCinema                    */ {  2, 0x00ed,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x00f2}, {0, 1, 0} } },
	/* e5 BondSetLockedVelocity                  */ {  3, 0x00ee,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {-1, 1, 0x00f2}, {0, 1, 0}, {1, 1, 0} } },
	/* e6 IFObjectInRoomWithPad                  */ {  5, 0x00ef,  3, { 1, 2, 1 }, 0x0002, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 2, 0}, {2, 1, 0} } },
	/* e7 IFImFiringAndLockedForward             */ {  2, 0x00f0,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* e8 IFImFiring                             */ {  2, 0x00f1,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* e9 SwitchSky                              */ {  1, 0x00f2,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* ea TriggerFadeAndExitLevelOnButtonPress   */ {  1, 0x01e1,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* eb IFBondIsDead                           */ {  2, 0x0034,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x00f2}, {0, 1, 0} } },
	/* ec BondDisableDamageAndPickups            */ {  1, 0x00f3,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {-1, 1, 0x00f2} } },
	/* ed BondHideWeapons                        */ {  1, 0x00ed,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x00f2}, {-1, 1, 0x0000} } },
	/* ee CameraOrbitPad                         */ { 13,     -1,  6, { 2, 2, 2, 2, 2, 2 }, 0x0008, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* ef CreditsRoll                            */ {  1,     -1,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* f0 IFCreditsHasCompleted                  */ {  2,     -1,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* f1 IFObjectiveAllCompleted                */ {  2, 0x00f7,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* f2 IFFolderActorIsEqual                   */ {  3,     -1,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* f3 IFBondDamageAndPickupsDisabled         */ {  2, 0x00f8,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {-1, 1, 0x00f2}, {0, 1, 0} } },
	/* f4 MusicPlaySlot                          */ {  4, 0x00f9,  3, { 1, 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  3, { {0, 1, 0}, {1, 1, 0}, {2, 1, 0} } },
	/* f5 MusicStopSlot                          */ {  2, 0x00fa,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
	/* f6 TriggerExplosionsAroundBond            */ {  1, 0x00fb,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {-1, 1, 0x00f2} } },
	/* f7 IFKilledCiviliansGreaterThan           */ {  3,     -1,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* f8 IFChrWasShotSinceLastCheck             */ {  3, 0x01e4,  2, { 1, 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  2, { {0, 1, 0}, {1, 1, 0} } },
	/* f9 BondKilledInAction                     */ {  1, 0x00fe,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* fa RaiseArms                              */ {  1, 0x00ff,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* fb GasLeakAndFadeFog                      */ {  1,     -1,  0, { 0 }, 0x0000, 0x0000, 0x0000, 0x0000,  0, { {0, 0, 0} } },
	/* fc ObjectRocketLaunch                     */ {  2, 0x01e5,  1, { 1 }, 0x0000, 0x0000, 0x0000, 0x0000,  1, { {0, 1, 0} } },
};

#endif
