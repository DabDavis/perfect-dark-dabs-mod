#ifndef _IN_GAME_MODGHOST_H
#define _IN_GAME_MODGHOST_H

#include <ultra64.h>
#include "types.h"

/**
 * Ghost Time Trial - a solo mission run recorded, saved, and raced against.
 *
 * A run is sampled while it is played and written out when it is completed.
 * The file it is written to is meant to leave this machine: the format below
 * is fixed size, little endian, self describing and hashed, so that a run
 * uploaded to a leaderboard and downloaded by someone else replays the same
 * way it was recorded. Nothing here talks to a network yet - a ghost arrives
 * by being dropped in the ghosts directory - but the format is the part that
 * has to be right before anything can.
 *
 * What is stored is what the body needs to be posed, not what the player did
 * to pose it. Recording inputs and replaying them through the movement code
 * would drift on the first frame the physics landed differently, and a time
 * trial that disagrees with itself about where the ghost was is worse than no
 * ghost. Positions replay exactly.
 */

/**
 * What a trial does: record on its own, or record while racing the field.
 *
 * Recording happens in both, because a run good enough to be worth keeping is
 * not announced in advance, and there is no mode that races without recording.
 *
 * OFF is not something the player can choose. It is what modGhostGetMode()
 * answers outside a trial, which is every mission started any other way -
 * runs are recorded in Ghost Trials and nowhere else, so that everything
 * reaching a leaderboard was set under the same rules.
 */
#define MODGHOST_OFF    0
#define MODGHOST_RECORD 1
#define MODGHOST_RACE   2

/**
 * Header flags: what a run was set under, kept with the run.
 *
 * A ghost outlives the settings that produced it and travels to machines whose
 * settings are different again, so the file has to answer for itself. One bit
 * so far, and it means the moves this fork added were off - which is what a
 * trial enforces and what makes two times worth comparing.
 *
 * Zero is "unknown", not "no rules": every ghost recorded before this was made
 * when the fork's moves were available and there is no asking now which were
 * on. A board that wants to compare like with like has to treat those as the
 * different thing they are.
 */
#define MODGHOSTHF_TRIALRULES 0x01

/**
 * Which of the ghosts on disk for this stage and difficulty gets raced.
 *
 * The directory is a pile of files rather than a slot per stage: every run you
 * finish is one file in it and anything downloaded is another, and none of
 * them knows about the others. The header says which stage and difficulty a
 * file is for, so the choice is made by reading headers rather than by naming
 * files.
 *
 * Because every attempt is kept, Fastest keeps only the best run per player -
 * otherwise a good session fills the field with ten copies of yourself and
 * hides everyone else. My Best is the mode that does want them: it is your own
 * quickest attempts, which is a lap against the version of you that was having
 * a better day.
 */
#define MODGHOSTPICK_FASTEST 0
#define MODGHOSTPICK_MINE    1
#define MODGHOSTPICK_CHOSEN  2

/**
 * How many ghosts may run at once, and how many the chooser lists.
 *
 * Ten bodies is the number that stays legible rather than the number the frame
 * can afford - a field of translucent Joannas taking the same corner is worth
 * watching, and a twentieth of one is a crowd. The cost is real either way:
 * each one is a chr, a prop, a model and an anim out of MEMPOOL_STAGE, so the
 * field is trimmed to what the pool can carry at stage load rather than
 * failing to load a level because somebody ticked ten boxes.
 *
 * The body models share their modeldefs. Ten separate loads would be ten
 * copies of the same head, which is the mistake body0f02ce8c() makes in
 * multiplayer and the one modbodies.c exists to avoid.
 */
#define MODGHOST_MAXRACERS 10

#define MODGHOST_DIR      "$S/ghosts"
#define MODGHOST_EXT      ".pdg"
#define MODGHOST_MAGIC    "PDGHOST"
#define MODGHOST_VERSION  2
#define MODGHOST_NAMELEN  32

/**
 * Room for the ghost account that recorded a run, written into the file.
 *
 * The player name already in the header is the agent out of the game file,
 * which says who ran it and not who may publish it - two people can both play
 * as Joanna. This is the account, and the server refuses an upload whose owner
 * is not the account doing the uploading, so a run downloaded from somebody
 * else cannot be sent back up under your name by pressing a button.
 *
 * Sixteen bytes because that is what the header had reserved, and the account
 * name is capped at fifteen characters to fit it exactly rather than being
 * truncated into it - a truncated owner would match the wrong account.
 *
 * Empty means the file predates this, or was recorded by someone not signed
 * in. Those are still uploadable by the agent that set them, because the
 * alternative is telling players their existing runs are nobody's.
 */
#define MODGHOST_OWNERLEN 16

/**
 * One sample every this many 60ths of a second, and how many of them a run may
 * hold.
 *
 * Twenty samples a second rather than sixty: position and facing are
 * interpolated between them, and the animation is chosen from the speeds
 * rather than played back frame by frame, so the third that is kept carries
 * everything the body is posed from. It is the difference between a megabyte
 * an eight minute run and a third of one, and the file is meant to be
 * uploaded.
 *
 * The cap is a little over fifty minutes, which is longer than any mission on
 * any difficulty. Past it the recording stops and the run simply does not
 * produce a ghost, rather than growing a buffer without limit.
 */
#define MODGHOST_RATE60     3
#define MODGHOST_MAXSAMPLES 65536

// Sample flags.
#define MODGHOSTSF_FIRINGLEFT  0x01
#define MODGHOSTSF_FIRINGRIGHT 0x02
#define MODGHOSTSF_DEAD        0x04

/**
 * One moment of the run, 24 bytes.
 *
 * The position is the feet rather than the eye - the player's prop is the eye,
 * and where a body is drawn is decided by the ground under it - so what is
 * stored is the ground the run was standing on. Angles are 16 bit turns, which
 * is finer than the body can be seen to move. Speeds are hundredths, and the
 * animation chooser reads them in a range of about plus or minus one.
 *
 * theta is where the body faces. shootrotx and shootroty are a different thing
 * and both are needed: they are where the run was aiming, which the upper body
 * is twisted and tilted towards independently of where the feet point. Storing
 * only the look pitch is not enough, because the horizontal half is what
 * decides which way the shoulders are turned while strafing around a target.
 */
struct modghostsample {
	/*0x00*/ f32 x;
	/*0x04*/ f32 y;
	/*0x08*/ f32 z;
	/*0x0c*/ u16 theta;
	/*0x0e*/ s16 shootrotx;
	/*0x10*/ s8 speedforwards;
	/*0x11*/ s8 speedsideways;
	/*0x12*/ s8 speedtheta;
	/*0x13*/ u8 crouchpos;
	/*0x14*/ u8 weaponnum;
	/*0x15*/ u8 flags;
	/*0x16*/ s16 shootroty;
};

/**
 * The file header, 128 bytes, followed by numsamples samples.
 *
 * headersize and samplesize are written rather than assumed so that a later
 * version can add fields to either and still be read by a build that does not
 * know about them: the reader strides by what the file says, not by what it
 * was compiled with.
 *
 * hash covers the sample block alone. It is there so that a leaderboard can
 * tell a truncated upload from a complete one, not to make a run
 * unforgeable - anything that runs on the player's machine can be made to lie,
 * and a time trial mod is not the place to pretend otherwise.
 */
struct modghostheader {
	/*0x00*/ char magic[8];
	/*0x08*/ u32 version;
	/*0x0c*/ u32 headersize;
	/*0x10*/ u32 samplesize;
	/*0x14*/ u32 numsamples;
	/*0x18*/ u32 time60;
	/*0x1c*/ u32 rate60;
	/*0x20*/ u8 stagenum;
	/*0x21*/ u8 difficulty;
	/*0x22*/ u8 stageindex;
	/*0x23*/ u8 flags;
	/*0x24*/ u32 hash;
	/*0x28*/ u32 timestamp;
	/*0x2c*/ u32 pad;
	/*0x30*/ char player[MODGHOST_NAMELEN];
	/*0x50*/ char build[MODGHOST_NAMELEN];
	/*0x70*/ char owner[MODGHOST_OWNERLEN];
};

/**
 * One ghost that has been ticked in the chooser, remembered by filename.
 *
 * The choice is stored as names rather than as anything derived from them,
 * because the pile of files in the ghosts directory is the thing the player is
 * choosing from and it changes underneath this - a download adds one, and so
 * does every run finished since. A name that no longer resolves is simply not
 * raced, and stays ticked in case the file comes back.
 */
struct modghostchoice {
	char filename[64];
};

extern s32 g_ModGhostMode;
extern s32 g_ModGhostPick;
extern s32 g_ModGhostAlpha;
extern s32 g_ModGhostSplits;
extern s32 g_ModGhostMaxRacers;

bool modGhostIsChr(struct chrdata *chr);
s32 modGhostGetAlpha(void);

void modGhostArmTrial(void);
bool modGhostTrialRulesApply(void);
void modGhostDisarmTrial(void);
void modGhostReset(void);
void modGhostRecordSample(void);
void modGhostTick(void);
void modGhostSaveRun(void);

bool modGhostIsRacing(void);
bool modGhostHasSplit(void);
s32 modGhostGetSplit60(void);
s32 modGhostGetTargetTime60(void);
const char *modGhostGetTargetName(void);
s32 modGhostGetNumRacers(void);

/**
 * The ghosts on disk, for the chooser to list and tick.
 *
 * The catalogue is built by reading every header in the directory, which is
 * done when the menu opens rather than continuously: it is file I/O, and the
 * directory only changes when something the player did changed it.
 */
struct modghostentry {
	char filename[64];
	char player[MODGHOST_NAMELEN];
	char owner[MODGHOST_OWNERLEN];
	u8 flags;
	u32 time60;
	u8 stagenum;
	u8 difficulty;
	bool chosen;
};

/**
 * How many ghosts the chooser and My Ghosts will list.
 *
 * This was a hundred and twenty eight when a stage and difficulty held one run
 * per player. Keeping every finished run instead means a single evening on one
 * mission can be dozens of files, so the ceiling is one a real directory is
 * unlikely to reach - it is a hundred bytes an entry in a static array, which
 * is nothing here and would have been the whole budget on the N64.
 *
 * Reaching it drops the runs that sort last rather than whichever ones the
 * directory happened to hand over last, so an over-full directory still lists
 * the same rows every time it is opened.
 */
#define MODGHOST_MAXCATALOGUE 512

/**
 * Where a ghost's name floats, and what colour it is drawn in.
 *
 * Measured from the ghost's feet, which is not where its prop is: the prop is
 * carried a hundred units up so that the portal walk that moves it between
 * rooms starts from inside the body rather than at the floor. The name height
 * subtracts that again rather than being tuned against it, because a number
 * that silently depends on another number is one that goes wrong the day the
 * other one changes - which is how the first version ended up drawing the tag
 * a whole body length above the head.
 *
 * A hundred and eighty five is just clear of the scalp: vv_eyeheight is 159 for
 * Joanna and the top of the head is a little above that. The text is then drawn
 * upwards from the anchor, so this is where it sits, not where it starts.
 *
 * Solid white at a fixed alpha rather than the ghost's own translucency: the
 * body is faint on purpose and the label saying whose it is is the part that
 * has to be readable while both of you are moving.
 */
#define MODGHOST_NAMEHEIGHT 185.0f
#define MODGHOST_PROPLIFT   100.0f
#define MODGHOST_NAMECOLOUR 0xffffffb4

s32 modGhostScanCatalogue(void);
s32 modGhostGetCatalogueCount(void);
struct modghostentry *modGhostGetCatalogueEntry(s32 index);
s32 modGhostGetNumChosen(void);
void modGhostToggleChosen(s32 index);
bool modGhostDeleteCatalogueEntry(s32 index);
Gfx *modGhostRenderNames(Gfx *gdl);
void modGhostClearChosen(void);
void modGhostLoadChoices(void);
void modGhostSaveChoices(void);
const char *modGhostStageName(s32 stagenum);

#endif
