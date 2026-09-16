/**
 * GoldenEye 007 for the Xbox 360 (Rare's "Project Bean" build) drawn on
 * GoldenEye X's characters. See gebean.h, and CLAUDE-notes/ge-bean.md for the
 * formats.
 *
 * Every file of Bean's is a Rare CAFF 07.08.06.0036 bundle, uncompressed and
 * big-endian. A character is a rendergraph: vertex and index buffers in the
 * .gpu section, a command stream in .stream naming a vertex buffer, a
 * material, a bone palette and a draw, and a 16 bone SKEL_* skeleton whose
 * bind is translation only. What this does with one is what
 * .xbla-work/ge-bean/bean2pack.py did offline, with the one step the OBJ
 * could not carry kept: the weights. Each Bean bone is turned so its segment
 * lies along the matching joint of the N64 model's rest pose (which is a star:
 * arms out, legs splayed), the whole is scaled to the N64 skeleton, and every
 * vertex keeps up to three of Bean's bones - as the model's own matrices, with
 * each joint's rest as the inverse bind. The knees and elbows then bend with
 * the skin instead of creasing where two rigid pieces meet.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "config.h"
#include "system.h"
#include "fs.h"
#include "romdata.h"
#include "archive.h"
#include "x360.h"
#include "xblatex.h"
#include "xblamesh.h"
#include "gebean.h"
#include "geguns.h"
#include "mod.h"
#include "data.h"
#include "lib/model.h"

#ifndef PLATFORM_N64

#define GEBEAN_XBLA_DIR "xbla"
#define GEBEAN_CACHE_DIR "cache"
// Written once an archive's characters are out. The first one (".extracted")
// was written when only new/ was taken, the second (".extracted2") before the
// guns' pickups were and the third (".extracted3") before their first-person
// models were, so a cache holding any of them is unpacked again.
#define GEBEAN_DONE_FILE ".extracted4"
#define GEBEAN_SCAN_DEPTH 2

// What says a folder is Bean's, and which of an archive's entries are wanted:
// the characters and heads, where Rare put them - the HD ones in new/, and in
// original/ the N64-look ones Bean switched to, under the same names - and the
// guns' pickups, which Bean keeps among the props as chr<gun>. The rest of the
// archive is levels, first-person guns and music.
#define GEBEAN_TREE "files/new/char"
#define GEBEAN_WANT_CHARS "files/new/char/"
#define GEBEAN_WANT_HEADS "files/new/head/"
#define GEBEAN_WANT_ORIGINAL_CHARS "files/original/char/"
#define GEBEAN_WANT_ORIGINAL_HEADS "files/original/head/"
#define GEBEAN_WANT_PICKUPS "files/new/prop/chr"
#define GEBEAN_WANT_ORIGINAL_PICKUPS "files/original/prop/chr"
#define GEBEAN_WANT_GUNS "files/new/gun/"

#define GEBEAN_BODY           0
#define GEBEAN_BODY_WITH_HEAD 1
#define GEBEAN_HEAD           2
// A character GoldenEye drew with its own head, built whole onto a Perfect
// Dark body that takes no head (the Combat Simulator pool)
#define GEBEAN_WHOLE          3

/**
 * Bean's units in GoldenEye's. A body measures its own (the two skeletons'
 * limb lengths summed) and every one the offline batch converted came out at
 * 0.2130; a head file has no skeleton to measure against, and takes that.
 */
#define GEBEAN_HEAD_SCALE 0.213f

#define GEBEAN_MAXMTX   64
#define GEBEAN_MAXVERTS 65535
#define GEBEAN_MAXDRAWS 0x1000

#define BEAN_MAXDRAWS 512
#define BEAN_MAXBONES 32
#define BEAN_MAXPAL   64
#define BEAN_MAXIBS   256
#define CAFF_MAXSECTS 16

struct gebeanrow {
	const char *file;
	s16 numnodes;
	s16 numvertices;
	u8 kind;
	const char *source;
};

static const struct gebeanrow rows[] = {
#include "gebeantable.h"
};

static s32 enabled = 0;

PD_CONSTRUCTOR static void gebeanConfigInit(void)
{
	configRegisterInt("Mod.XblaGoldenEye", &enabled, 0, 1);
}

s32 gebeanGetEnabled(void)
{
	return enabled;
}

void gebeanSetEnabled(s32 on)
{
	enabled = on ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * The Combat Simulator pool
 * ------------------------------------------------------------------------- */

/**
 * GoldenEye's multiplayer characters, by the names and heads its own select
 * screen gives them (the decomp's mp_chr_setup[], with each body's model from
 * the order of its body table), and its heads. A body row's file is an alias
 * of a Perfect Dark body - Joanna for a woman, a dataDyne guard for a man -
 * whose rest skeleton is GoldenEye's star too, so the Bean mesh fits it the
 * way it fits GoldenEye X's; a head's is an alias of a Perfect Dark head.
 * Bean's blueman, bluewoman and greyman are broken in the release and left
 * out, as are its Bond heads (static N64-style models).
 */
struct gebeanpoolrow {
	struct gebeanrow row;
	const char *name;  // a body's Combat Simulator name
	u8 female;
	f32 scale;         // GoldenEye's own, on top of the host's
	const char *head;  // a body's head for simulants: a pool head's source, or NULL for any
};

#define POOLBODY(file, source, kind, name, female, scale, head) \
	{ { file, 0, 0, kind, source }, name "\n", female, scale, head }
#define POOLHEAD(file, source, female) \
	{ { file, 0, 0, GEBEAN_HEAD, source }, NULL, female, 1.0f, NULL }

static const struct gebeanpoolrow poolRows[] = {
	POOLBODY("CgeNatalyaZ",      "char/natalya",      GEBEAN_WHOLE, "Natalya",                 1, 0.9661f, NULL),
	POOLBODY("CgeTrevelyanZ",    "char/trevelyan",    GEBEAN_WHOLE, "Trevelyan",               0, 1.0f,    NULL),
	POOLBODY("CgeXeniaZ",        "char/xenia",        GEBEAN_WHOLE, "Xenia",                   2, 1.0f,    NULL),
	POOLBODY("CgeOurumovZ",      "char/orumov",       GEBEAN_WHOLE, "Ourumov",                 0, 1.0778f, NULL),
	POOLBODY("CgeBorisZ",        "char/boris",        GEBEAN_WHOLE, "Boris",                   0, 0.9702f, NULL),
	POOLBODY("CgeValentinZ",     "char/valentin",     GEBEAN_WHOLE, "Valentin",                0, 0.9324f, NULL),
	POOLBODY("CgeMaydayZ",       "char/mayday",       GEBEAN_WHOLE, "Mayday",                  2, 1.0f,    NULL),
	POOLBODY("CgeJawsZ",         "char/jaws",         GEBEAN_WHOLE, "Jaws",                    0, 1.199f,  NULL),
	POOLBODY("CgeOddjobZ",       "char/oddjob",       GEBEAN_WHOLE, "Oddjob",                  0, 0.7878f, NULL),
	POOLBODY("CgeBaronSamediZ",  "char/baronsamedi",  GEBEAN_WHOLE, "Baron Samedi",            0, 1.0f,    NULL),
	POOLBODY("CgeSnowguardZ",    "char/snowguard",    GEBEAN_WHOLE, "Siberian Special Forces", 0, 1.0f,    NULL),
	POOLBODY("CgePilotZ",        "char/pilot",        GEBEAN_WHOLE, "Helicopter Pilot",        0, 1.0f,    NULL),
	POOLBODY("CgeDjbondZ",       "char/djbond",       GEBEAN_BODY,  "Bond (Tuxedo)",           0, 1.0f,    NULL),
	POOLBODY("CgeBoilerbondZ",   "char/boilerbond",   GEBEAN_BODY,  "Bond (Boiler Suit)",      0, 1.0f,    NULL),
	POOLBODY("CgeSuitbondZ",     "char/suitbond",     GEBEAN_BODY,  "Bond (Suit)",             0, 1.0f,    NULL),
	POOLBODY("CgeTimberbondZ",   "char/timberbond",   GEBEAN_BODY,  "Bond (Jungle)",           0, 1.0f,    NULL),
	POOLBODY("CgeSnowbondZ",     "char/snowbond",     GEBEAN_BODY,  "Bond (Parka)",            0, 1.0f,    NULL),
	POOLBODY("CgeBoilertrevZ",   "char/boilertrev",   GEBEAN_BODY,  "Trevelyan (006)",         0, 1.0f,    NULL),
	POOLBODY("CgeOliveguardZ",   "char/oliveguard",   GEBEAN_BODY,  "Russian Soldier",         0, 1.0f,    "head/headmark"),
	POOLBODY("CgeRusguardZ",     "char/rusguard",     GEBEAN_BODY,  "Russian Infantry",        0, 1.0f,    "head/headkarl"),
	POOLBODY("CgeTechmanZ",      "char/techman",      GEBEAN_BODY,  "Scientist",               0, 1.0f,    "head/headdave"),
	POOLBODY("CgeTechwomanZ",    "char/techwoman",    GEBEAN_BODY,  "Scientist",               1, 1.0f,    "head/headsally"),
	POOLBODY("CgeCommguardZ",    "char/commguard",    GEBEAN_BODY,  "Russian Commandant",      0, 1.0f,    "head/headmartin"),
	POOLBODY("CgeArmourguardZ",  "char/armourguard",  GEBEAN_BODY,  "Janus Marine",            0, 1.0f,    "head/headstevee"),
	POOLBODY("CgeNavyguardZ",    "char/navyguard",    GEBEAN_BODY,  "Naval Officer",           0, 1.0f,    "head/headduncan"),
	POOLBODY("CgeGreyguardZ",    "char/greyguard",    GEBEAN_BODY,  "St. Petersburg Guard",    0, 1.0f,    "head/headken"),
	POOLBODY("CgeJeanwomanZ",    "char/jeanwoman",    GEBEAN_BODY,  "Civilian",                1, 1.0f,    "head/headmarion"),
	POOLBODY("CgeCardimanZ",     "char/cardiman",     GEBEAN_BODY,  "Civilian",                0, 1.0f,    NULL),
	POOLBODY("CgeCheckmanZ",     "char/checkman",     GEBEAN_BODY,  "Civilian",                0, 1.0f,    "head/headgrant"),
	POOLBODY("CgeRedmanZ",       "char/redman",       GEBEAN_BODY,  "Civilian",                0, 1.0f,    "head/headdwayne"),
	POOLBODY("CgeGreatguardZ",   "char/greatguard",   GEBEAN_BODY,  "Siberian Guard",          0, 1.0f,    "head/headlee"),
	POOLBODY("CgeBluecamguardZ", "char/bluecamguard", GEBEAN_BODY,  "Arctic Commando",         0, 1.0f,    "head/headchris"),
	POOLBODY("CgeGreatguard2Z",  "char/greatguard2",  GEBEAN_BODY,  "Siberian Guard",          0, 1.0f,    "head/headscott"),
	POOLBODY("CgeCamguardZ",     "char/camguard",     GEBEAN_BODY,  "Jungle Commando",         0, 1.0f,    "head/headjoel"),
	POOLBODY("CgeTrevguardZ",    "char/trevguard",    GEBEAN_BODY,  "Janus Special Forces",    0, 1.0f,    "head/headb"),
	POOLBODY("CgeMoonguardZ",    "char/moonguard",    GEBEAN_BODY,  "Moonraker Elite",         0, 1.0f,    "head/headneil"),
	POOLBODY("CgeMoonfemaleZ",   "char/moonfemale",   GEBEAN_BODY,  "Moonraker Elite",         1, 1.0f,    "head/headvivien"),
	POOLBODY("CgeFattechwomanZ", "char/fattechwoman", GEBEAN_BODY,  "Rosika",                  1, 0.8853f, "head/headmarion"),
	POOLHEAD("CgeheadKarlZ",     "head/headkarl",     0),
	POOLHEAD("CgeheadAlanZ",     "head/headalan",     0),
	POOLHEAD("CgeheadPeteZ",     "head/headpete",     0),
	POOLHEAD("CgeheadMartinZ",   "head/headmartin",   0),
	POOLHEAD("CgeheadMarkZ",     "head/headmark",     0),
	POOLHEAD("CgeheadDuncanZ",   "head/headduncan",   0),
	POOLHEAD("CgeheadShaunZ",    "head/headshaun",    0),
	POOLHEAD("CgeheadDwayneZ",   "head/headdwayne",   0),
	POOLHEAD("CgeheadBZ",        "head/headb",        0),
	POOLHEAD("CgeheadDaveZ",     "head/headdave",     0),
	POOLHEAD("CgeheadGrantZ",    "head/headgrant",    0),
	POOLHEAD("CgeheadDesZ",      "head/headdes",      0),
	POOLHEAD("CgeheadChrisZ",    "head/headchris",    0),
	POOLHEAD("CgeheadLeeZ",      "head/headlee",      0),
	POOLHEAD("CgeheadNeilZ",     "head/headneil",     0),
	POOLHEAD("CgeheadJimZ",      "head/headjim",      0),
	POOLHEAD("CgeheadRobinZ",    "head/headrobin",    0),
	POOLHEAD("CgeheadStevehZ",   "head/headsteveh",   0),
	POOLHEAD("CgeheadSteveeZ",   "head/headstevee",   0),
	POOLHEAD("CgeheadJoelZ",     "head/headjoel",     0),
	POOLHEAD("CgeheadScottZ",    "head/headscott",    0),
	POOLHEAD("CgeheadJoeZ",      "head/headjoe",      0),
	POOLHEAD("CgeheadKenZ",      "head/headken",      0),
	POOLHEAD("CgeheadMishkinZ",  "head/headmishkin",  0),
	POOLHEAD("CgeheadSallyZ",    "head/headsally",    1),
	POOLHEAD("CgeheadMarionZ",   "head/headmarion",   1),
	POOLHEAD("CgeheadMandyZ",    "head/headmandy",    1),
	POOLHEAD("CgeheadVivienZ",   "head/headvivien",   1),
};

// Where the pool's rows start in g_HeadsAndBodies: straight after the stock
// table's terminator, so nothing a mod's table can import (moddata.c takes 151)
#define GEBEAN_POOL_BASE 152

// The stock list lengths (mplayer.c's g_MpListCounts): a list of any other
// length is a mod's
#define GEBEAN_STOCK_MPHEADS  (VERSION == VERSION_JPN_FINAL ? 74 : 75)
#define GEBEAN_STOCK_MPBODIES 61

// The MP save holds an index in 7 bits, and the body index one past the list
// is the ROM's Dr Caroll
#define GEBEAN_MAX_MPINDEX 126

static s32 poolSlot[ARRAYCOUNT(poolRows)];

_Static_assert(GEBEAN_POOL_BASE + ARRAYCOUNT(poolRows) <= NUM_HEADSANDBODIES,
		"the GoldenEye pool must fit g_HeadsAndBodies");

/**
 * GoldenEye's guns (geguns.c): each one's pickup model state is an alias of
 * its host's pickup, and the release's pickup - a rigid mesh, laid onto
 * GoldenEye's N64 pickup whose frame is Perfect Dark's by a fit made offline
 * (gegunstable.h) - is drawn on it.
 */
struct gebeangunrow {
	struct gebeanrow row;
	s32 weaponnum;
	u8 perm[3];
	s8 sign[3];
	f32 scale;
	f32 beancentre[3];
	f32 n64centre[3];
};

// A rigid pickup on the model's own matrices (gebeanBuildRigid())
#define GEBEAN_RIGID 4

#define GUNROW(weapon, file, source, p0, p1, p2, s0, s1, s2, scale, bx, by, bz, nx, ny, nz) \
	{ { file, 0, 0, GEBEAN_RIGID, source }, weapon, { p0, p1, p2 }, { s0, s1, s2 }, scale, { bx, by, bz }, { nx, ny, nz } }

static const struct gebeangunrow gunRows[] = {
#include "gegunstable.h"
};

_Static_assert(ARRAYCOUNT(gunRows) == NUM_GE_WEAPONS, "a pickup row per GoldenEye gun");

static s32 gunSlot[ARRAYCOUNT(gunRows)];

/**
 * GoldenEye's first-person guns: each copy's hi_model is an alias of its
 * host's first-person model (gebeanGunsRefresh()), on which Bean's gun - less
 * its hand, since Perfect Dark's own hand model is drawn with the gun - is
 * skinned to the host's matrices (gebeanBuildFirstPerson()). Only in the
 * release's look: Bean's N64-look guns have the hand in their geometry, and
 * with the meshes off the host's own model draws, which for the classic guns
 * is GoldenEye's N64 gun already.
 */
#define GEBEAN_FIRSTPERSON 5

#define FPROW(file, source) { file, 0, 0, GEBEAN_FIRSTPERSON, source }

static const struct gebeanrow fpRows[NUM_GE_WEAPONS] = {
	[WEAPON_GE_PP7             - WEAPON_GE_FIRST] = FPROW("GgePP7Z",             "gun/ppk"),
	[WEAPON_GE_PP7SILENCED     - WEAPON_GE_FIRST] = FPROW("GgePP7silZ",          "gun/ppksilenced"),
	[WEAPON_GE_DD44            - WEAPON_GE_FIRST] = FPROW("GgeDD44Z",            "gun/tt33"),
	[WEAPON_GE_KLOBB           - WEAPON_GE_FIRST] = FPROW("GgeKlobbZ",           "gun/skorpion"),
	[WEAPON_GE_KF7SOVIET       - WEAPON_GE_FIRST] = FPROW("GgeKF7Z",             "gun/ak47"),
	[WEAPON_GE_ZMG             - WEAPON_GE_FIRST] = FPROW("GgeZMGZ",             "gun/uzi"),
	[WEAPON_GE_D5K             - WEAPON_GE_FIRST] = FPROW("GgeD5KZ",             "gun/mp5k"),
	[WEAPON_GE_D5KSILENCED     - WEAPON_GE_FIRST] = FPROW("GgeD5KsilZ",          "gun/mp5ksilenced"),
	[WEAPON_GE_PHANTOM         - WEAPON_GE_FIRST] = FPROW("GgePhantomZ",         "gun/spectre"),
	[WEAPON_GE_AR33            - WEAPON_GE_FIRST] = FPROW("GgeAR33Z",            "gun/m16"),
	[WEAPON_GE_RCP90           - WEAPON_GE_FIRST] = FPROW("GgeRCP90Z",           "gun/fnp90"),
	[WEAPON_GE_SHOTGUN         - WEAPON_GE_FIRST] = FPROW("GgeShotgunZ",         "gun/shotgun"),
	[WEAPON_GE_AUTOSHOTGUN     - WEAPON_GE_FIRST] = FPROW("GgeAutoShotgunZ",     "gun/automaticshotgun"),
	[WEAPON_GE_SNIPERRIFLE     - WEAPON_GE_FIRST] = FPROW("GgeSniperZ",          "gun/sniperrifle"),
	[WEAPON_GE_COUGARMAGNUM    - WEAPON_GE_FIRST] = FPROW("GgeCougarZ",          "gun/ruger"),
	[WEAPON_GE_GOLDENGUN       - WEAPON_GE_FIRST] = FPROW("GgeGoldenGunZ",       "gun/goldengun"),
	[WEAPON_GE_MOONRAKER       - WEAPON_GE_FIRST] = FPROW("GgeMoonrakerZ",       "gun/laser"),
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = FPROW("GgeGrenadeLauncherZ", "gun/grenadelauncher"),
	[WEAPON_GE_ROCKETLAUNCHER  - WEAPON_GE_FIRST] = FPROW("GgeRocketLauncherZ",  "gun/rocketlauncher"),
	[WEAPON_GE_HUNTINGKNIFE    - WEAPON_GE_FIRST] = FPROW("GgeKnifeZ",           "gun/knife"),
	[WEAPON_GE_THROWINGKNIFE   - WEAPON_GE_FIRST] = FPROW("GgeThrowingKnifeZ",   "gun/throwingknife"),
	[WEAPON_GE_GRENADE         - WEAPON_GE_FIRST] = FPROW("GgeGrenadeZ",         "gun/grenade"),
	[WEAPON_GE_TIMEDMINE       - WEAPON_GE_FIRST] = FPROW("GgeTimedMineZ",       "gun/timedmine"),
	[WEAPON_GE_PROXIMITYMINE   - WEAPON_GE_FIRST] = FPROW("GgeProximityMineZ",   "gun/proximitymine"),
	[WEAPON_GE_REMOTEMINE      - WEAPON_GE_FIRST] = FPROW("GgeRemoteMineZ",      "gun/remotemine"),
};

static s32 fpSlot[ARRAYCOUNT(fpRows)];

/**
 * Guns GoldenEye draws with no hands, which Perfect Dark's are taken off for
 * (WEAPONFLAG_HASHANDS) while Bean's gun is the one drawn. Only the pistols
 * carry a hand in Bean's geometry at all - the 512x511 picture - and the
 * hands Perfect Dark draws instead are posed on its own gun, so on a shape
 * they were never meant to hold they stand off it: the user picked these
 * three out on screen (2026-09-16). The rest keep their hands, which were
 * judged right in the 25-gun survey.
 */
static const u8 fpNoHands[ARRAYCOUNT(fpRows)] = {
	[WEAPON_GE_SNIPERRIFLE     - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_MOONRAKER       - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_ROCKETLAUNCHER  - WEAPON_GE_FIRST] = 1,
};

/**
 * Which first-person guns are drawn from Bean: those checked on screen against
 * their host's (2026-09-15, a 25-gun survey; the Moonraker joined them on
 * 2026-09-16 and it is now all of them). A gun taken back out keeps the host's
 * model, with GoldenEye's name, pickup and third-person gun still its own.
 */
static const u8 fpReady[ARRAYCOUNT(fpRows)] = {
	[WEAPON_GE_PP7             - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_PP7SILENCED     - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_DD44            - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_KLOBB           - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_KF7SOVIET       - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_ZMG             - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_D5K             - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_D5KSILENCED     - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_PHANTOM         - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_AR33            - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_RCP90           - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_SHOTGUN         - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_AUTOSHOTGUN     - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_SNIPERRIFLE     - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_COUGARMAGNUM    - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_GOLDENGUN       - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_MOONRAKER       - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_ROCKETLAUNCHER  - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_HUNTINGKNIFE    - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_THROWINGKNIFE   - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_GRENADE         - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_TIMEDMINE       - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_PROXIMITYMINE   - WEAPON_GE_FIRST] = 1,
	[WEAPON_GE_REMOTEMINE      - WEAPON_GE_FIRST] = 1,
};

/**
 * The plain gun a silenced one is measured on. Its host has no silencer, and
 * Bean's silenced file is the plain gun in the same place with the silencer as
 * one more picture in front of the muzzle, so fitting the whole length to the
 * host's shrank the gun by the silencer (the PP7's to 0.108 of Bean's size
 * against the plain PP7's 0.191).
 */
static const char *const fpFitSource[ARRAYCOUNT(fpRows)] = {
	[WEAPON_GE_PP7SILENCED     - WEAPON_GE_FIRST] = "gun/ppk",
	[WEAPON_GE_D5KSILENCED     - WEAPON_GE_FIRST] = "gun/mp5k",
};

/**
 * Where the hand closes on a gun that is not the host's shape, in Bean's
 * units: the gun is placed by it rather than centred on the host's lists.
 * Every host carries the hand's skeleton and hangs the gun off the palm
 * (matrix 2), so the same point of the hand grips every gun; the offset from
 * the palm to it was measured on the PP7, which the centring lays right (the
 * middle of its grip lands there). The sniper rifle is a bolt-action with its
 * grip far back on a bullpup host, whose centre put it across the screen to
 * the right with the hand in front of it.
 */
struct fpgrip {
	s32 set;
	f32 pos[3];
	f32 scale;  // 0 to keep the fit along the barrel
	// Which of Bean's axes feeds each of the host's, signed and 1 based, for
	// a gun whose model is not laid out the way its host's is; all zero for
	// Bean's axes as they are. The host's matrix turns whatever it is given,
	// so a gun authored along another axis is turned with it: Bean's knives
	// run up the y axis where Perfect Dark's runs along x, which drew them
	// across the bottom right corner.
	s8 axis[3];
};

#define FP_PALM_MTX 2

static const f32 fpGripFromPalm[3] = { 65.8f, -74.9f, 34.5f };

static const struct fpgrip fpGrip[ARRAYCOUNT(fpRows)] = {
	[WEAPON_GE_SNIPERRIFLE     - WEAPON_GE_FIRST] = { 1, { 0.0f, -273.0f, -590.0f }, 0.0f },

	// GoldenEye's launcher is a long tube on a host half its length, and its
	// box is longer still - the muzzle bone binds at z 1489 and geometry runs
	// to 4066 - so fitting it drew a toy (scale 0.079). Bean's guns are
	// GoldenEye's models at 4.7x and GoldenEye's units are Perfect Dark's
	// (the Klobb, KF7 and ZMG all fit at 0.213), so it is drawn at its own
	// size from the grip under the tube.
	[WEAPON_GE_ROCKETLAUNCHER  - WEAPON_GE_FIRST] = { 1, { -102.0f, -808.0f, -588.0f }, 1.0f / 4.7f },

	// The Moonraker has no hand of its own to be gripped by - GoldenEye draws
	// the laser with none, where the PP7, DD44 and Golden Gun each carry the
	// same hand mesh - and its shape gives no grip away either, so it is
	// placed the way GoldenEye lines its guns up instead: on SKEL_TOP, the
	// first bone of every gun file, which is the model's root. That hand sits
	// at the same offset from SKEL_TOP in all three pistols - within ten units
	// in y and eight in z, in the N64 files and the HD ones alike - so a gun's
	// place in the view is its SKEL_TOP's, whatever its own origin.
	//
	// Which point of a gun that makes the hand's is read off the PP7, whose
	// centring is right: its fitted centre lands at the middle of its host's
	// lists and fpGripFromPalm's point is 49 lower and 50 further back, which
	// at the PP7's 0.191 is Bean (-1.4, -519.7, -186) - the butt of the
	// pistol, and SKEL_TOP + (-1.4, -247.6, 158.4). On the Moonraker's
	// SKEL_TOP (0.4, -200, -1400) that is the point below, which is the front
	// of the handle under its body. Drawn at its own size for the same reason
	// as the launcher: the host laser is half its length.
	[WEAPON_GE_MOONRAKER       - WEAPON_GE_FIRST] = { 1, { -1.0f, -447.6f, -1241.6f }, 1.0f / 4.7f },

	// A knife's blade runs up the y axis, not along the barrel every gun is
	// fitted by, so fitting its z drew a sliver (0.102 and 0.092). Both are
	// drawn at their own size and placed the way the Moonraker is, on
	// SKEL_TOP: they share one (0, -307.8, -404.7) and they carry GoldenEye's
	// hand in their geometry, landing where the pistols' does, so the same
	// SKEL_TOP + (-1.4, -247.6, 158.4) holds. And they are turned a quarter
	// about z (Bean's y feeds the host's x), because the host's own knife is
	// modelled along x and it is the host's matrix that holds a knife up:
	// unturned they lay across the bottom right corner with the blade running
	// off it. GoldenEye's throwing knife is held by the blade, handle up, so
	// the same turn is right for both.
	// Perfect Dark has no Spectre. The Phantom's host is the CMP150, picked
	// for its kind rather than its shape, and it is a much shorter gun: the
	// length fit drew GoldenEye's at 0.126 against every same-gun host's
	// 0.19-0.21, which is 40% small, and the hands - posed on the CMP150 -
	// closed on nothing ("phantom is wrong hand position"). So it is placed
	// the way the Moonraker is, on its own SKEL_TOP (0, -188.5, -1637.8) plus
	// the same offset to GoldenEye's hand point, at GoldenEye's own size.
	[WEAPON_GE_PHANTOM         - WEAPON_GE_FIRST] = { 1, { -1.4f, -436.1f, -1479.4f }, 1.0f / 4.7f },

	[WEAPON_GE_HUNTINGKNIFE    - WEAPON_GE_FIRST] = { 1, { -1.4f, -555.4f, -246.3f }, 1.0f / 4.7f, { 2, -1, 3 } },
	[WEAPON_GE_THROWINGKNIFE   - WEAPON_GE_FIRST] = { 1, { -1.4f, -555.4f, -246.3f }, 1.0f / 4.7f, { 2, -1, 3 } },
};

/**
 * A colour a gun's vertices are drawn with (ARGB, 0 for their own). Bean's
 * Golden Gun is two near-white pictures, a scratch map and a shine map
 * (texture_gold_file521/522), which its shader lays under a 256x256 gold
 * reflection map on a second sampler; one sampler here drew it white.
 */
static const u32 fpTint[ARRAYCOUNT(fpRows)] = {
	[WEAPON_GE_GOLDENGUN       - WEAPON_GE_FIRST] = 0xfff0c86e,
};

// Each copy's first-person file as geguns.c made it - its host's - before
// this ever pointed it at an alias
static u16 fpHostFile[ARRAYCOUNT(fpRows)];

/**
 * Where the gun that was drawn ends, as an offset from the host's muzzle node
 * in the model's own space, for the guns a mesh was built for.
 *
 * Perfect Dark fires everything from `MODELPART_GUN_MUZZLEPOS` of the model in
 * the hand - the bullet stream, the beam, the smoke, a rocket - and that node
 * belongs to the host. A gun of another shape drawn on it has its barrel
 * ending somewhere else, so the Moonraker's beam left the air beside it and
 * several streams started off the barrel. Filled in by
 * gebeanBuildFirstPerson(), read by bondgun.c through
 * gebeanFirstPersonMuzzleOffset(), and cleared whenever a build does not
 * happen - with the release's meshes off, the host's own model draws and its
 * own node is right again.
 */
static f32 fpMuzzle[ARRAYCOUNT(fpRows)][3];
static s16 fpMuzzlePart[ARRAYCOUNT(fpRows)];
static u8 fpMuzzleSet[ARRAYCOUNT(fpRows)];

/** A row of any table: GoldenEye X's first, then the pool's, then the guns'. */
static const struct gebeanrow *gebeanRowAt(s32 row)
{
	if (row >= 0 && row < ARRAYCOUNT(rows)) {
		return &rows[row];
	}

	if (row >= ARRAYCOUNT(rows) && row < ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows)) {
		return &poolRows[row - ARRAYCOUNT(rows)].row;
	}

	if (row >= ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows)
			&& row < ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows) + ARRAYCOUNT(gunRows)) {
		return &gunRows[row - ARRAYCOUNT(rows) - ARRAYCOUNT(poolRows)].row;
	}

	if (row >= ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows) + ARRAYCOUNT(gunRows)
			&& row < ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows) + ARRAYCOUNT(gunRows) + ARRAYCOUNT(fpRows)) {
		return &fpRows[row - ARRAYCOUNT(rows) - ARRAYCOUNT(poolRows) - ARRAYCOUNT(gunRows)];
	}

	return NULL;
}

/**
 * The row number of a pool or gun file - an alias this registered - or -1.
 * The alias keeps the table's own string, so a pointer compare names it.
 */
static s32 gebeanPoolRowForFile(u16 fileid)
{
	const char *name = fileid ? romdataFileGetName(fileid) : NULL;

	if (!name) {
		return -1;
	}

	for (s32 i = 0; i < ARRAYCOUNT(poolRows); i++) {
		if (poolSlot[i] == fileid && name == poolRows[i].row.file) {
			return ARRAYCOUNT(rows) + i;
		}
	}

	for (s32 i = 0; i < ARRAYCOUNT(gunRows); i++) {
		if (gunSlot[i] == fileid && name == gunRows[i].row.file) {
			return ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows) + i;
		}
	}

	for (s32 i = 0; i < ARRAYCOUNT(fpRows); i++) {
		if (fpSlot[i] == fileid && name == fpRows[i].file) {
			return ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows) + ARRAYCOUNT(gunRows) + i;
		}
	}

	return -1;
}

const char *gebeanPoolBodyName(s32 bodynum)
{
	const s32 i = bodynum - GEBEAN_POOL_BASE;

	if (i < 0 || i >= ARRAYCOUNT(poolRows) || !poolRows[i].name || !poolSlot[i]) {
		return NULL;
	}

	return poolRows[i].name;
}

/**
 * GoldenEye's guns (geguns.c): their Combat Simulator rows are shown, and
 * each one's model state is an alias of its host's pickup that the release's
 * pickup is drawn on, when the switch is on, a copy is in xbla/ and the weapon
 * list is the game's own; otherwise the rows are hidden and the model states
 * are the host's pickup again.
 */
static void gebeanGunsRefresh(void)
{
	const s32 show = enabled && !modDataMpWeaponsImported() && gebeanIsAvailable();
	s32 shown = 0;

	for (s32 i = 0; i < ARRAYCOUNT(gunRows); i++) {
		const s32 hostmodel = gegunsHostModel(i);
		struct modelstate *state = &g_ModelStates[MODEL_GE_FIRST + i];
		s32 fileid = 0;
		u16 scale = 0x199;

		if (hostmodel >= 0 && hostmodel < MODEL_GE_FIRST) {
			fileid = g_ModelStates[hostmodel].fileid;
			scale = g_ModelStates[hostmodel].scale;
		}

		gunSlot[i] = 0;

		if (show && fileid) {
			const s32 slot = romdataRegisterAliasFile(gunRows[i].row.file, fileid);

			if (slot) {
				gunSlot[i] = slot;
				fileid = slot;
				shown++;
			}
		}

		// A stage that loaded the other file keeps its own model; the next
		// load takes the new one
		if (state->fileid != fileid) {
			state->modeldef = NULL;
		}

		state->fileid = (u16)fileid;
		state->scale = scale;

		g_MpWeapons[MPWEAPON_GE_FIRST + i].unlockfeature = show ? 0 : MPFEATURE_NEVER;

		// And the first-person model: an alias of the host's, which Bean's gun
		// is drawn on, or the host's own again
		if (!fpHostFile[i]) {
			fpHostFile[i] = g_GeWeaponDefs[i].hi_model;
		}

		fpSlot[i] = 0;
		g_GeWeaponDefs[i].hi_model = fpHostFile[i];

		if (show && fpReady[i] && fpHostFile[i]) {
			const s32 slot = romdataRegisterAliasFile(fpRows[i].file, fpHostFile[i]);

			if (slot) {
				fpSlot[i] = slot;
				g_GeWeaponDefs[i].hi_model = (u16)slot;
			}
		}

		// And the host's hands, on or off with the model they hold
		g_GeWeaponDefs[i].flags &= ~WEAPONFLAG_HASHANDS;

		if (!fpSlot[i] || !fpNoHands[i]) {
			g_GeWeaponDefs[i].flags |= g_Weapons[g_GeWeaponHosts[i]]->flags & WEAPONFLAG_HASHANDS;
		}
	}

	if (show) {
		sysLogPrintf(LOG_NOTE, "gebean: %d GoldenEye guns in the Combat Simulator's weapons, %d with the release's pickup",
				ARRAYCOUNT(gunRows), shown);
	}
}

void gebeanPoolRefresh(void)
{
	s32 numbodies = g_MpListCounts.bodies;
	s32 numheads = g_MpListCounts.heads;
	s32 addedbodies = 0;
	s32 addedheads = 0;

	gebeanGunsRefresh();

	// Off with whatever this put on last time: the tail of each list whose
	// rows are the pool's
	while (numbodies > 0 && g_MpBodies[numbodies - 1].bodynum >= GEBEAN_POOL_BASE) {
		numbodies--;
	}

	while (numheads > 0 && g_MpHeads[numheads - 1].headnum >= GEBEAN_POOL_BASE) {
		numheads--;
	}

	g_MpListCounts.bodies = numbodies;
	g_MpListCounts.heads = numheads;

	if (!enabled || numbodies != GEBEAN_STOCK_MPBODIES || numheads != GEBEAN_STOCK_MPHEADS
			|| !gebeanIsAvailable()) {
		return;
	}

	for (s32 i = 0; i < ARRAYCOUNT(poolRows); i++) {
		const struct gebeanpoolrow *p = &poolRows[i];
		const s32 ishead = p->row.kind == GEBEAN_HEAD;
		// Bodies stand on the dataDyne guard, and the slight women (female 2)
		// on the Institute's female technician. Joanna's shoulders sit 70% of
		// the way up her back to her neck against 51% on GoldenEye's women, so
		// no fit of the torso met both: the Moonraker Elite's shoulders sloped
		// and narrowed and her collar rose over the head's neck. The guard fits
		// most of GoldenEye's women within a few percent, but widened Xenia's
		// torso by 12% and Mayday's by 13%; the technician takes them to 92%
		// and 93%. Her shoulders are as high as Joanna's, which sloped the
		// broader women.
		const s32 hostnum = ishead ? (p->female ? HEAD_ANKA : HEAD_JAMIE)
				: (p->female == 2 ? BODY_CIFEMTECH : BODY_DD_GUARD);
		const struct headorbody *host = &g_HeadsAndBodies[hostnum];
		struct headorbody *hb = &g_HeadsAndBodies[GEBEAN_POOL_BASE + i];
		s32 slot = romdataRegisterAliasFile(p->row.file, host->filenum);
		struct modeldef *keep;
		u32 height;

		poolSlot[i] = slot;

		if (!slot) {
			continue;
		}

		// A row a stage has already loaded keeps its model: a chr may be
		// wearing it, and this can run from the pause menu
		keep = hb->filenum == slot ? hb->modeldef : NULL;

		*hb = *host;
		hb->filenum = slot;
		hb->modeldef = keep;
		hb->scale = host->scale * p->scale;
		height = (u32)(host->height * p->scale + 0.5f);

		if (!ishead) {
			// the guard is a man; the sex picks voices and the default head
			hb->ismale = !p->female;
		}
		hb->height = height > 255 ? 255 : height;

		if (p->row.kind == GEBEAN_WHOLE) {
			// its head is in the mesh, so the body takes none
			hb->unk00_01 = 1;
		}

		if (ishead) {
			if (numheads + addedheads < ARRAYCOUNT(g_MpHeads) && numheads + addedheads <= GEBEAN_MAX_MPINDEX) {
				g_MpHeads[numheads + addedheads].headnum = GEBEAN_POOL_BASE + i;
				g_MpHeads[numheads + addedheads].requirefeature = 0;
				addedheads++;
			}
		} else if (numbodies + addedbodies < ARRAYCOUNT(g_MpBodies) && numbodies + addedbodies < GEBEAN_MAX_MPINDEX) {
			struct mpbody *body = &g_MpBodies[numbodies + addedbodies];

			body->bodynum = GEBEAN_POOL_BASE + i;
			body->name = 0;
			body->headnum = 1000; // any head of the body's sex
			body->requirefeature = 0;

			for (s32 j = 0; p->head && j < ARRAYCOUNT(poolRows); j++) {
				if (poolRows[j].row.kind == GEBEAN_HEAD && strcmp(poolRows[j].row.source, p->head) == 0) {
					body->headnum = GEBEAN_POOL_BASE + j;
					break;
				}
			}

			addedbodies++;
		}
	}

	g_MpListCounts.bodies = numbodies + addedbodies;
	g_MpListCounts.heads = numheads + addedheads;

	sysLogPrintf(LOG_NOTE, "gebean: %d GoldenEye characters and %d heads in the Combat Simulator's lists",
			addedbodies, addedheads);
}

static u32 gebeanBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u16 gebeanBE16(const u8 *p)
{
	return (u16)(((u32)p[0] << 8) | p[1]);
}

static f32 gebeanBEF32(const u8 *p)
{
	union { u32 u; f32 f; } bits;
	bits.u = gebeanBE32(p);
	return bits.f;
}

static void gebeanPutBE32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static void gebeanPutBEF32(u8 *p, f32 f)
{
	union { u32 u; f32 f; } bits;
	bits.f = f;
	gebeanPutBE32(p, bits.u);
}

/** [ofs, ofs + len) inside size, written so that it cannot wrap. */
static s32 gebeanFits(u64 ofs, u64 len, u64 size)
{
	return ofs <= size && len <= size - ofs;
}

/* -------------------------------------------------------------------------
 * Finding the copy
 * ------------------------------------------------------------------------- */

static char rootPath[FS_MAXPATH + 1];    // .../files, once it is on disk
static char archivePath[FS_MAXPATH + 1]; // what the player dropped, when it is an archive
static s32 scanned;
static s32 unpackFailed;

static s32 gebeanIsDir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static s32 gebeanHasTree(const char *dir)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/" GEBEAN_TREE, dir);

	return gebeanIsDir(path);
}

struct gebeanscan {
	const char *dir;
	s32 archives;
	s32 depth;
	char found[FS_MAXPATH + 1];
};

static s32 gebeanScan(const char *dir, s32 archives, s32 depth, char *dst, u32 dstLen);

static void gebeanScanEntry(const char *name, void *arg)
{
	struct gebeanscan *scan = arg;
	char path[FS_MAXPATH + 1];

	if (scan->found[0] || name[0] == '.') {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", scan->dir, name);

	if (gebeanIsDir(path)) {
		if (!scan->archives && gebeanHasTree(path)) {
			snprintf(scan->found, sizeof(scan->found), "%s", path);
			return;
		}

		if (scan->depth > 0) {
			gebeanScan(path, scan->archives, scan->depth - 1, scan->found, sizeof(scan->found));
		}

		return;
	}

	// An archive is looked into, never taken on its name: the Perfect Dark
	// release sits in the same folder, as an archive of its own.
	if (scan->archives && archiveIsSupported(path) && archiveFindEntry(path, GEBEAN_WANT_CHARS)) {
		snprintf(scan->found, sizeof(scan->found), "%s", path);
	}
}

/** The first Bean folder (holding files/new/char) or Bean archive at or under an expanded dir. */
static s32 gebeanScan(const char *dir, s32 archives, s32 depth, char *dst, u32 dstLen)
{
	struct gebeanscan scan;

	if (!archives && gebeanHasTree(dir)) {
		snprintf(dst, dstLen, "%s", dir);
		return 1;
	}

	memset(&scan, 0, sizeof(scan));
	scan.dir = dir;
	scan.archives = archives;
	scan.depth = depth;

	fsScanDir(dir, gebeanScanEntry, &scan);

	if (!scan.found[0]) {
		return 0;
	}

	snprintf(dst, dstLen, "%s", scan.found);

	return 1;
}

/** cache/xbla/goldeneye/, made if it has to be. dst gets the expanded path. */
static s32 gebeanCacheDir(char *dst, u32 dstLen)
{
	char rel[FS_MAXPATH + 1];
	char sub[FS_MAXPATH + 1];

	if (fsChooseOutputDir(GEBEAN_CACHE_DIR, rel, sizeof(rel)) != 0) {
		return 0;
	}

	snprintf(sub, sizeof(sub), "%s/xbla", rel);

	if (fsFileSize(sub) < 0) {
		fsCreateDir(sub);
	}

	snprintf(sub, sizeof(sub), "%s/xbla/goldeneye", rel);

	if (fsFileSize(sub) < 0) {
		fsCreateDir(sub);
	}

	snprintf(dst, dstLen, "%s", fsFullPath(sub));

	return 1;
}

static s32 gebeanWantEntry(const char *name, void *arg)
{
	char lower[FS_MAXPATH + 1];
	size_t i;

	for (i = 0; name[i] && i + 1 < sizeof(lower); i++) {
		char c = name[i] == '\\' ? '/' : name[i];
		lower[i] = (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
	}

	lower[i] = '\0';

	return strstr(lower, GEBEAN_WANT_CHARS) != NULL || strstr(lower, GEBEAN_WANT_HEADS) != NULL
		|| strstr(lower, GEBEAN_WANT_ORIGINAL_CHARS) != NULL || strstr(lower, GEBEAN_WANT_ORIGINAL_HEADS) != NULL
		|| strstr(lower, GEBEAN_WANT_PICKUPS) != NULL || strstr(lower, GEBEAN_WANT_ORIGINAL_PICKUPS) != NULL
		|| strstr(lower, GEBEAN_WANT_GUNS) != NULL;
}

static void gebeanSetRoot(const char *tree)
{
	snprintf(rootPath, sizeof(rootPath), "%s/files", tree);
	sysLogPrintf(LOG_NOTE, "gebean: GoldenEye XBLA characters in %s", rootPath);
}

/**
 * 1 when rootPath names the characters on disk. The folders are scanned once;
 * an archive is unpacked only when mayUnpack says the caller is the one to pay
 * for it, and only once.
 */
static s32 gebeanLocate(s32 mayUnpack)
{
	static const char *const dirs[] = {
		"$E/" GEBEAN_XBLA_DIR,
		"$H/" GEBEAN_XBLA_DIR,
		"./" GEBEAN_XBLA_DIR,
		"$S/" GEBEAN_XBLA_DIR,
	};
	char cache[FS_MAXPATH + 1];
	char marker[FS_MAXPATH + 1];
	char found[FS_MAXPATH + 1];
	s32 written;
	FILE *fp;

	if (rootPath[0]) {
		return 1;
	}

	if (!scanned) {
		scanned = 1;

		// A folder the player unpacked themselves before an archive to unpack.
		for (s32 d = 0; d < ARRAYCOUNT(dirs); d++) {
			char dir[FS_MAXPATH + 1];

			snprintf(dir, sizeof(dir), "%s", fsFullPath(dirs[d]));

			if (gebeanIsDir(dir) && gebeanScan(dir, 0, GEBEAN_SCAN_DEPTH, found, sizeof(found))) {
				gebeanSetRoot(found);
				return 1;
			}
		}

		for (s32 d = 0; d < ARRAYCOUNT(dirs) && !archivePath[0]; d++) {
			char dir[FS_MAXPATH + 1];

			snprintf(dir, sizeof(dir), "%s", fsFullPath(dirs[d]));

			if (gebeanIsDir(dir)) {
				gebeanScan(dir, 1, GEBEAN_SCAN_DEPTH, archivePath, sizeof(archivePath));
			}
		}

		if (archivePath[0]) {
			sysLogPrintf(LOG_NOTE, "gebean: the GoldenEye XBLA release is %s", archivePath);
		}
	}

	if (!archivePath[0] || !gebeanCacheDir(cache, sizeof(cache))) {
		return 0;
	}

	snprintf(marker, sizeof(marker), "%s/" GEBEAN_DONE_FILE, cache);

	if (fsFileSize(marker) >= 0 && gebeanScan(cache, 0, GEBEAN_SCAN_DEPTH + 1, found, sizeof(found))) {
		gebeanSetRoot(found);
		return 1;
	}

	if (!mayUnpack || unpackFailed) {
		return 0;
	}

	sysLogPrintf(LOG_NOTE, "gebean: unpacking the characters from %s into %s, this happens once",
			archivePath, cache);

	written = archiveExtractMatching(archivePath, cache, gebeanWantEntry, NULL);

	if (written <= 0 || !gebeanScan(cache, 0, GEBEAN_SCAN_DEPTH + 1, found, sizeof(found))) {
		sysLogPrintf(LOG_ERROR, "gebean: no GoldenEye characters came out of %s", archivePath);
		unpackFailed = 1;
		return 0;
	}

	fp = fopen(fsFullPath(marker), "wb");

	if (fp) {
		fclose(fp);
	}

	sysLogPrintf(LOG_NOTE, "gebean: unpacked %d files", written);
	gebeanSetRoot(found);

	return 1;
}

s32 gebeanIsAvailable(void)
{
	return gebeanLocate(0) || archivePath[0];
}

s32 gebeanPrepare(void)
{
	return gebeanLocate(1);
}

/* -------------------------------------------------------------------------
 * The model file this pairs with
 * ------------------------------------------------------------------------- */

/** The next node of the game's depth-first walk (xblaMeshEnumListNodes()'s order). */
static struct modelnode *gebeanNextNode(struct modelnode *node)
{
	if (node->child) {
		return node->child;
	}

	while (node) {
		if (node->next) {
			return node->next;
		}

		node = node->parent;
	}

	return NULL;
}

const char *gebeanRowName(s32 row)
{
	const struct gebeanrow *r = gebeanRowAt(row);

	return r ? r->file : "?";
}

s32 gebeanRowIsFirstPerson(s32 row)
{
	const s32 base = ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows) + ARRAYCOUNT(gunRows);

	return row >= base && row < base + ARRAYCOUNT(fpRows);
}

s32 gebeanRowIsPool(s32 row)
{
	// The guns' pickups too: they stand on Perfect Dark models, as the pool does
	return row >= ARRAYCOUNT(rows) && row < ARRAYCOUNT(rows) + ARRAYCOUNT(poolRows) + ARRAYCOUNT(gunRows);
}

s32 gebeanFindRow(u16 fileid, struct modeldef *modeldef)
{
	const char *name;
	s32 row = -1;
	s32 nodes = 0;
	s32 verts = 0;
	s32 walked = 0;

	if (!modeldef || !modeldef->rootnode) {
		return -1;
	}

	// The pool's own files are aliases, and their shape is a Perfect Dark
	// model's, which the rig reads rather than the table counting it
	row = gebeanPoolRowForFile(fileid);

	if (row >= 0) {
		return row;
	}

	if (romdataFileIsStock(fileid)) {
		return -1;
	}

	name = romdataFileGetName(fileid);

	if (!name) {
		return -1;
	}

	for (s32 i = 0; i < ARRAYCOUNT(rows); i++) {
		if (strcmp(rows[i].file, name) == 0) {
			row = i;
			break;
		}
	}

	if (row < 0) {
		return -1;
	}

	for (struct modelnode *node = modeldef->rootnode; node && walked < 4096; node = gebeanNextNode(node), walked++) {
		const u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_DL) {
			nodes++;
			verts += node->rodata->dl.numvertices;
		} else if (type == MODELNODETYPE_GUNDL) {
			nodes++;
			verts += node->rodata->gundl.numvertices;
		}
	}

	if (nodes != rows[row].numnodes || verts != rows[row].numvertices) {
		sysLogPrintf(LOG_NOTE, "gebean: model file %d is called %s but is not GoldenEye X's "
				"(%d lists, %d vertices against %d and %d) - left alone",
				fileid, name, nodes, verts, rows[row].numnodes, rows[row].numvertices);
		return -1;
	}

	return row;
}

s32 gebeanListNodeMatrix(const struct modelnode *node)
{
	s32 walked = 0;

	for (node = node ? node->parent : NULL; node && walked < 64; node = node->parent, walked++) {
		switch (node->type & 0xff) {
		case MODELNODETYPE_POSITION:
			return node->rodata->position.mtxindex0;
		case MODELNODETYPE_CHRINFO:
			return node->rodata->chrinfo.mtxindex;
		case MODELNODETYPE_POSITIONHELD:
			return node->rodata->positionheld.mtxindex;
		case MODELNODETYPE_HEADSPOT:
			return 0;
		}
	}

	return 0;
}

/**
 * A list address as a loaded model holds it: still a segment 5 address into
 * the file (with the low bit set once its textures are rewritten), or a
 * pointer already.
 */
static Gfx *beanResolveGdl(const u8 *base, Gfx *gdl)
{
	const uintptr_t addr = (uintptr_t)gdl;

	if (addr <= 0xffffffff && ((addr & 1) || ((UNSEGADDR(addr) >> 24) & 0xff) == 0x05)) {
		return base ? (Gfx *)(base + (UNSEGADDR(addr) & 0xffffff)) : NULL;
	}

	return gdl;
}

/**
 * Walks a list for the matrices it loads itself: the first into *first, and
 * the one each of its vertices is loaded under into vtxmtx (-1 before any).
 */
static void beanWalkListMatrices(const u8 *base, const Vtx *vertices, s32 numvertices, Gfx *gdl, s32 depth,
		s16 *cur, s16 *first, s16 *vtxmtx)
{
	s32 steps = 0;

	while (gdl && depth <= 8 && steps++ < 0x10000) {
		const u32 w0 = (u32)gdl->words.w0;
		const uintptr_t w1 = gdl->words.w1;

		switch ((u8)(w0 >> 24)) {
		case G_MTX:
			*cur = (s16)((UNSEGADDR(w1) & 0xffffff) / sizeof(Mtxf));

			if (*first < 0) {
				*first = *cur;
			}
			break;
		case G_VTX:
			if (vtxmtx && vertices) {
				const u32 off = (u32)(UNSEGADDR(w1) & 0xffffff);
				const s32 n = (s32)((w0 & 0xffff) / sizeof(Vtx));
				const intptr_t vi0 = ((UNSEGADDR(w1) >> 24) & 0xf) == SPSEGMENT_MODEL_VTX
					? (intptr_t)(off / sizeof(Vtx))
					: base ? ((intptr_t)(base + off) - (intptr_t)vertices) / (intptr_t)sizeof(Vtx) : -1;

				for (s32 i = 0; vi0 >= 0 && i < n; i++) {
					if (vi0 + i < numvertices) {
						vtxmtx[vi0 + i] = *cur;
					}
				}
			}
			break;
		case G_DL: {
			Gfx *target = beanResolveGdl(base, (Gfx *)w1);

			if (((w0 >> 16) & 1) == 0) {
				beanWalkListMatrices(base, vertices, numvertices, target, depth + 1, cur, first, vtxmtx);
			} else {
				gdl = target;
				continue;
			}
			break;
		}
		case (u8)G_ENDDL:
			return;
		}

		gdl++;
	}
}

/** Walks a list node's opaque list; see beanWalkListMatrices(). */
static s16 beanListMatrices(const struct modelnode *node, s16 *vtxmtx, s32 numvertices)
{
	const u32 type = node ? node->type & 0xff : 0;
	s16 cur = -1;
	s16 first = -1;

	for (s32 i = 0; vtxmtx && i < numvertices; i++) {
		vtxmtx[i] = -1;
	}

	if (type == MODELNODETYPE_GUNDL) {
		const u8 *base = node->rodata->gundl.baseaddr;

		beanWalkListMatrices(base, node->rodata->gundl.vertices, numvertices,
				beanResolveGdl(base, node->rodata->gundl.opagdl), 0, &cur, &first, vtxmtx);
	} else if (type == MODELNODETYPE_DL) {
		const u8 *base = (const u8 *)node->rodata->dl.colours;

		beanWalkListMatrices(base, node->rodata->dl.vertices, numvertices,
				beanResolveGdl(base, node->rodata->dl.opagdl), 0, &cur, &first, vtxmtx);
	}

	return first;
}

s32 gebeanListLoadedMatrix(const struct modelnode *node)
{
	return beanListMatrices(node, NULL, 0);
}

/* -------------------------------------------------------------------------
 * CAFF
 * ------------------------------------------------------------------------- */

struct caffsect {
	char name[16];
	u64 offset;
	u32 size;
};

struct cafffile {
	u32 asset;
	u32 start;
	u32 size;
	u8 sect;
};

struct caff {
	const u8 *d;
	u32 len;
	s32 be;
	s32 numsects;
	struct caffsect sects[CAFF_MAXSECTS];
	u32 numassets;
	u32 nametable; // numassets offsets into the labels
	u32 labels;
	u32 numfiles;
	struct cafffile *files;
};

static u32 caffHeader32(const struct caff *c, u64 o)
{
	const u8 *p;

	if (!gebeanFits(o, 4, c->len)) {
		return 0;
	}

	p = c->d + o;

	return c->be ? gebeanBE32(p) : ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
}

/**
 * The layout is CAFFeinated's BundleV36 (OlieGamerTV) checked against Bean's
 * own files: a header, the section table, the asset names, one more table the
 * files do not need, and a 14 byte record per file naming its asset, section,
 * start and size. The sections' data follows in section order.
 */
static s32 caffOpen(struct caff *c, const u8 *d, u32 len)
{
	u32 stroff[CAFF_MAXSECTS];
	u64 pos;
	u64 data;
	u32 hsize, namelen, sectsize, filesize, total, adb;

	memset(c, 0, sizeof(*c));
	c->d = d;
	c->len = len;

	if (len < 0x68 || memcmp(d, "CAFF", 4) != 0) {
		return 0;
	}

	c->be = d[0x48] == 1;
	hsize = caffHeader32(c, 0x14);
	c->numassets = caffHeader32(c, 0x1c);
	c->numfiles = caffHeader32(c, 0x20);
	c->numsects = d[0x49];
	namelen = caffHeader32(c, 0x4c);
	sectsize = caffHeader32(c, 0x50);
	filesize = caffHeader32(c, 0x64);

	if (d[0x4a] != 0) {
		sysLogPrintf(LOG_WARNING, "gebean: a compressed CAFF (%u) is not one this reads", d[0x4a]);
		return 0;
	}

	if (c->numsects > CAFF_MAXSECTS || c->numassets > 0x10000 || c->numfiles > 0x10000) {
		return 0;
	}

	pos = hsize;

	if (!gebeanFits(pos, 0x21ull * c->numsects, len)) {
		return 0;
	}

	for (s32 i = 0; i < c->numsects; i++) {
		stroff[i] = caffHeader32(c, pos);
		c->sects[i].size = caffHeader32(c, pos + 9);
		pos += 0x21;
	}

	for (s32 i = 0; i < c->numsects; i++) {
		const u64 at = pos + stroff[i];

		for (s32 j = 0; j < (s32)sizeof(c->sects[i].name) - 1 && at + j < len && d[at + j]; j++) {
			c->sects[i].name[j] = (char)d[at + j];
		}
	}

	pos += namelen;
	total = caffHeader32(c, pos);
	c->nametable = (u32)(pos + 4);
	c->labels = (u32)(pos + 4 + 4ull * c->numassets);
	pos = (u64)c->labels + total;
	adb = caffHeader32(c, pos);
	pos += 4ull + adb;

	if (!gebeanFits(pos, 14ull * c->numfiles, len)) {
		return 0;
	}

	c->files = calloc(c->numfiles ? c->numfiles : 1, sizeof(*c->files));

	if (!c->files) {
		return 0;
	}

	for (u32 i = 0; i < c->numfiles; i++) {
		c->files[i].asset = caffHeader32(c, pos);
		c->files[i].start = caffHeader32(c, pos + 4);
		c->files[i].size = caffHeader32(c, pos + 8);
		c->files[i].sect = d[pos + 12];
		pos += 14;
	}

	data = (u64)hsize + sectsize + filesize;

	for (s32 i = 0; i < c->numsects; i++) {
		c->sects[i].offset = data;
		data += c->sects[i].size;
	}

	return 1;
}

static void caffClose(struct caff *c)
{
	free(c->files);
	c->files = NULL;
}

static const u8 *caffBlob(const struct caff *c, s32 index, u32 *outLen)
{
	const struct cafffile *f;
	u64 at;

	if (index < 0 || (u32)index >= c->numfiles) {
		return NULL;
	}

	f = &c->files[index];

	if (f->sect == 0 || f->sect > c->numsects) {
		return NULL;
	}

	at = c->sects[f->sect - 1].offset + f->start;

	if (!gebeanFits(at, f->size, c->len)) {
		return NULL;
	}

	*outLen = f->size;

	return c->d + at;
}

static const char *caffSectName(const struct caff *c, s32 index)
{
	const u8 sect = c->files[index].sect;

	return sect && sect <= c->numsects ? c->sects[sect - 1].name : "";
}

/** The last component of an asset's name ("...\texture pairs"), or "". */
static const char *caffAssetName(const struct caff *c, u32 asset)
{
	u32 off;
	const char *name;
	const char *last;
	u64 at;

	if (asset == 0 || asset > c->numassets) {
		return "";
	}

	off = caffHeader32(c, c->nametable + 4ull * (asset - 1));
	at = (u64)c->labels + off;

	if (at >= c->len || !memchr(c->d + at, '\0', c->len - at)) {
		return "";
	}

	name = (const char *)c->d + at;
	last = strrchr(name, '\\');

	return last ? last + 1 : name;
}

/** The first file of an asset in a section whose name starts with prefix, or -1. */
static s32 caffFind(const struct caff *c, u32 asset, const char *prefix)
{
	for (u32 i = 0; i < c->numfiles; i++) {
		if (c->files[i].asset == asset && strncmp(caffSectName(c, i), prefix, strlen(prefix)) == 0) {
			return (s32)i;
		}
	}

	return -1;
}

/* -------------------------------------------------------------------------
 * The rendergraph
 * ------------------------------------------------------------------------- */

enum {
	SK_BASE, SK_BACK, SK_NECK, SK_POSITION,
	SK_LF_SHOULDER, SK_LF_ELBOW, SK_LF_WRIST,
	SK_RT_SHOULDER, SK_RT_ELBOW, SK_RT_WRIST,
	SK_LF_HIP, SK_LF_KNEE, SK_LF_ANKLE,
	SK_RT_HIP, SK_RT_KNEE, SK_RT_ANKLE,
	SK_COUNT
};

static const char *const skelNames[SK_COUNT] = {
	"SKEL_BASE", "SKEL_BACK", "SKEL_NECK", "SKEL_POSITION",
	"SKEL_LF_SHOULDER", "SKEL_LF_ELBOW", "SKEL_LF_WRIST",
	"SKEL_RT_SHOULDER", "SKEL_RT_ELBOW", "SKEL_RT_WRIST",
	"SKEL_LF_HIP", "SKEL_LF_KNEE", "SKEL_LF_ANKLE",
	"SKEL_RT_HIP", "SKEL_RT_KNEE", "SKEL_RT_ANKLE",
};

// The joint each bone's segment runs to, which is what the bone is turned to
// line up; -1 for the ends of the chains.
static const s8 skelChild[SK_COUNT] = {
	SK_BACK, SK_NECK, -1, -1,
	SK_LF_ELBOW, SK_LF_WRIST, -1,
	SK_RT_ELBOW, SK_RT_WRIST, -1,
	SK_LF_KNEE, SK_LF_ANKLE, -1,
	SK_RT_KNEE, SK_RT_ANKLE, -1,
};

// And the bone an end turns with.
static const s8 skelInherit[SK_COUNT] = {
	-1, -1, SK_BACK, SK_BASE,
	-1, -1, SK_LF_ELBOW,
	-1, -1, SK_RT_ELBOW,
	-1, -1, SK_LF_KNEE,
	-1, -1, SK_RT_KNEE,
};

struct beandraw {
	u32 vb;
	u32 tex;
	u32 prim;
	u32 count;
	u32 ib;
	u8 numpal;
	u8 pal[BEAN_MAXPAL];
};

struct beanib {
	u32 obj;
	u32 off;
	u32 size;
};

struct beanmodel {
	u8 *file;
	struct caff caff;
	const u8 *data;
	u32 datalen;
	const u8 *gpu;
	u32 gpulen;
	const u8 *stream;
	u32 streamlen;

	s32 numbones;
	s8 skel[BEAN_MAXBONES];      // SK_* per pose bone, or -1
	f32 bind[BEAN_MAXBONES][3];  // absolute

	s32 numremap;
	u16 remap[BEAN_MAXPAL];      // palette number -> pose bone

	s32 numdraws;
	struct beandraw *draws;

	s32 numtex;
	s32 texfile[GEBEAN_MAXMATS]; // a texture's header, by file index

	s32 numibs;
	struct beanib ibs[BEAN_MAXIBS];

	f32 uvscale;
};

struct beanvb {
	u32 stride;
	u32 off;
	u32 count;
	s32 col28; // stride 28 carries a colour rather than a UV
};

struct beanvtx {
	f32 pos[3];
	f32 nrm[3];
	f32 uv[2];
	s8 slot[4];   // palette slot, or -1
	u8 weight[4];
	u32 argb;     // a rigid prop's colour; white where the buffer has none
};

static s32 beanReadVb(const struct beanmodel *bm, u32 desc, struct beanvb *vb)
{
	u32 size;

	memset(vb, 0, sizeof(*vb));

	if (!gebeanFits(desc, 16, bm->datalen)) {
		return 0;
	}

	vb->stride = gebeanBE32(bm->data + desc);
	vb->off = gebeanBE32(bm->data + desc + 8);
	size = gebeanBE32(bm->data + desc + 12);

	if (vb->stride < 20 || !gebeanFits(vb->off, size, bm->gpulen)) {
		return 0;
	}

	vb->count = size / vb->stride;

	// Stride 28 is a skinned vertex with one of a UV and a colour: a colour's
	// alpha byte is 0xff on every vertex, a UV's high byte is not.
	if (vb->stride == 28) {
		vb->col28 = 1;

		for (u32 i = 0; i < vb->count; i++) {
			if (bm->gpu[vb->off + i * 28 + 24] != 0xff) {
				vb->col28 = 0;
				break;
			}
		}
	}

	return 1;
}

static void beanUnpackNormal(u32 w, f32 *out)
{
	for (s32 i = 0; i < 3; i++) {
		s32 v = (w >> (10 * i)) & 1023;

		if (v & 512) {
			v -= 1024;
		}

		out[i] = v / 511.0f;
	}
}

/**
 * One vertex. Every layout starts with a float position; a skinned one then
 * has four u16 palette slots written three times over (0xf000 for none), and
 * the stride 36 one four weight bytes in reverse; then a 10:10:10 normal, an
 * s16 UV and an ARGB colour, whichever of those the stride has room for.
 */
static s32 beanVertex(const struct beanmodel *bm, const struct beanvb *vb, u32 i, struct beanvtx *v)
{
	const u8 *p;
	u32 k;
	s32 hasuv = 1;

	if (i >= vb->count) {
		return 0;
	}

	p = bm->gpu + vb->off + i * vb->stride;
	memset(v, 0, sizeof(*v));

	for (s32 j = 0; j < 3; j++) {
		v->pos[j] = gebeanBEF32(p + j * 4);
	}

	for (s32 j = 0; j < 4; j++) {
		v->slot[j] = -1;
	}

	v->weight[0] = 255;
	v->argb = 0xffffffff;

	switch (vb->stride) {
	case 28:
	case 32:
	case 36:
		for (s32 j = 0; j < 4; j++) {
			const u16 s = gebeanBE16(p + 12 + j * 2);
			v->slot[j] = s == 0xf000 || s / 3 >= BEAN_MAXPAL ? -1 : (s8)(s / 3);
		}

		if (vb->stride == 36) {
			for (s32 j = 0; j < 4; j++) {
				v->weight[j] = p[23 - j];
			}

			k = 24;
		} else {
			k = 20;
		}

		hasuv = !(vb->stride == 28 && vb->col28);
		break;
	case 20:
		k = 12;
		hasuv = 0;
		v->argb = gebeanBE32(p + 16);
		break;
	case 24:
		k = 12;
		v->argb = gebeanBE32(p + 20);
		break;
	default:
		return 0;
	}

	beanUnpackNormal(gebeanBE32(p + k), v->nrm);

	if (hasuv) {
		v->uv[0] = (s16)gebeanBE16(p + k + 4) / bm->uvscale;
		v->uv[1] = (s16)gebeanBE16(p + k + 6) / bm->uvscale;
	}

	return 1;
}

static s32 beanTriangles(const struct beanmodel *bm, const struct beandraw *d, u16 **out);

/**
 * What one texture repeat is in this file's s16 UVs, which nothing in the file
 * says - the material records are the same whatever the range. The HD files
 * use 1/16384 or 1/32768; the N64-look originals whatever power of two their
 * export took, per file: 2048 on the tuxedo Bond, 4096 on most uniformed
 * guards, 8192 on most heads, 16384 on Natalya. Divided by the wrong one, a
 * model samples a corner of every picture (the black guards and Boris's
 * patchwork of the first batch).
 *
 * A texture's UVs cover about one repeat, and a few wrap well past it (the N64
 * tiled some), so the answer is the smallest power of two within a quarter of
 * the median texture's largest UV, over the triangles each texture is drawn
 * on. On every HD file that is what the old rule said (the largest UV in the
 * file, split at 17000); on the originals that rule read the wrapping textures
 * and said 32768. A picture of four texels or fewer is an untextured span's,
 * and its UVs mean nothing.
 */
static f32 beanMeasureUvScale(struct beanmodel *bm)
{
	u32 biggest[GEBEAN_MAXMATS];
	u8 drawn[GEBEAN_MAXMATS];
	u32 sorted[GEBEAN_MAXMATS];
	s32 n = 0;
	u32 scale;

	memset(biggest, 0, sizeof(biggest));
	memset(drawn, 0, sizeof(drawn));

	for (s32 di = 0; di < bm->numdraws; di++) {
		const struct beandraw *d = &bm->draws[di];
		struct beanvb vb;
		u32 uvo;
		u16 *tris = NULL;
		s32 numtris;

		if (d->tex >= GEBEAN_MAXMATS || !beanReadVb(bm, d->vb, &vb)) {
			continue;
		}

		if (d->tex < (u32)bm->numtex) {
			u32 blen;
			const u8 *b = caffBlob(&bm->caff, bm->texfile[d->tex], &blen);

			if (b && blen >= 0x28 && (u32)gebeanBE16(b + 0x24) * gebeanBE16(b + 0x26) <= 4) {
				continue;
			}
		}

		switch (vb.stride) {
		case 36: uvo = 28; break;
		case 32: uvo = 24; break;
		case 28: uvo = 24; break;
		case 24: uvo = 16; break;
		default: continue;
		}

		if (vb.stride == 28 && vb.col28) {
			continue;
		}

		numtris = beanTriangles(bm, d, &tris);

		for (s32 i = 0; i < numtris * 3; i++) {
			const u8 *p;
			s32 u, w;

			if (tris[i] >= vb.count) {
				continue;
			}

			p = bm->gpu + vb.off + tris[i] * vb.stride + uvo;
			u = (s16)gebeanBE16(p);
			w = (s16)gebeanBE16(p + 2);
			u = u < 0 ? -u : u;
			w = w < 0 ? -w : w;

			if ((u32)u > biggest[d->tex]) {
				biggest[d->tex] = u;
			}

			if ((u32)w > biggest[d->tex]) {
				biggest[d->tex] = w;
			}
		}

		if (numtris > 0) {
			drawn[d->tex] = 1;
		}

		free(tris);
	}

	for (s32 t = 0; t < GEBEAN_MAXMATS; t++) {
		if (drawn[t]) {
			s32 at = n++;

			while (at > 0 && sorted[at - 1] > biggest[t]) {
				sorted[at] = sorted[at - 1];
				at--;
			}

			sorted[at] = biggest[t];
		}
	}

	if (n == 0) {
		return 32768.0f;
	}

	for (scale = 1024; scale < 32768 && scale * 1.25f < sorted[(n - 1) / 2]; scale *= 2) {
	}

	return (f32)scale;
}

/**
 * A point of Bean's gun in the host's axes: out[a] is the Bean axis named by
 * axis[a], 1 based and signed, or the same axis when there is none.
 */
static void beanAxisMap(const s8 *axis, const f32 *in, f32 *out)
{
	for (s32 a = 0; a < 3; a++) {
		if (axis && axis[a]) {
			const s32 which = (axis[a] < 0 ? -axis[a] : axis[a]) - 1;

			out[a] = axis[a] < 0 ? -in[which] : in[which];
		} else {
			out[a] = in[a];
		}
	}
}

/** The texels in a texture, from its header, without decoding it. */
static u32 beanTexArea(const struct beanmodel *bm, u32 t)
{
	u32 blen = 0;
	const u8 *b = t < (u32)bm->numtex ? caffBlob((struct caff *)&bm->caff, bm->texfile[t], &blen) : NULL;

	return b && blen >= 0x40 ? (u32)gebeanBE16(b + 0x24) * gebeanBE16(b + 0x26) : 0;
}

/**
 * Which of a material's textures is the gun's own picture. A material lists
 * one (index, sampler) pair per texture after its count, and a second is an
 * environment map the release's shader lays over the first - but the two are
 * not in a fixed order: the Golden Gun's gold sphere map comes first and its
 * pictures second, the knife's picture first and its sphere map second. The
 * picture is the bigger of them every time (512x512 against 256x256 or less),
 * so the largest is taken, and the last of equals. Reading the second always
 * painted both knives in a cloudy sphere map.
 */
static u32 beanMaterialTexture(const struct beanmodel *bm, const u8 *st, u32 pc, u32 size, u32 len)
{
	const u32 count = size >= 12 ? gebeanBE32(st + pc + 8) >> 16 : 0;
	u32 best = 0;
	u32 bestarea = 0;
	s32 found = 0;

	for (u32 k = 0; k < count && gebeanFits(pc + 12 + 8 * k, 4, len) && 12 + 8 * k + 4 <= size; k++) {
		const u32 t = gebeanBE32(st + pc + 12 + 8 * k);
		const u32 area = beanTexArea(bm, t);

		if (!found || area >= bestarea) {
			found = 1;
			best = t;
			bestarea = area;
		}
	}

	return found ? best : 0;
}

/**
 * The command stream, first alternative at every switch. Each record is a
 * tagged u32 (size << 16 | type << 8): 0x12 palette remap, 0x13 bone palette,
 * 0x16 switch, 0x17 conditional section, 0x19 jump, 0x1d end, 0x2d material,
 * 0x2e vertex buffer, 0x01 draw, and 0x30 the N64-look originals' draw.
 */
static void beanWalkStream(struct beanmodel *bm)
{
	const u8 *st = bm->stream;
	const u32 len = bm->streamlen;
	u32 end;
	u32 pc = 0x24;
	u32 vb = 0;
	u32 tex = 0;
	u8 pal[BEAN_MAXPAL];
	u8 numpal = 1;

	pal[0] = 0;

	if (len < 0x28) {
		return;
	}

	end = gebeanBE32(st + 4);

	if (end > len) {
		end = len;
	}

	for (s32 steps = 0; pc + 4 <= end && steps < 100000; steps++) {
		const u32 tag = gebeanBE32(st + pc);
		const u32 size = tag >> 16;
		const u32 type = (tag >> 8) & 0xff;

		if (size < 4 || !gebeanFits(pc, size, len)) {
			break;
		}

		if (type == 0x16) {
			if (!gebeanFits(pc, 12, len)) {
				break;
			}

			pc = gebeanBE32(st + pc + 8);
			continue;
		}

		if (type == 0x19) {
			pc = gebeanBE32(st + pc + 4);
			continue;
		}

		if (type == 0x1d) {
			break;
		}

		// A section behind a condition: 0x17 {kind, where it ends}. Kind 2 is
		// only in the originals, round a head's sunglasses, which GoldenEye's
		// multiplayer heads do not wear. Kind 0 is in both, and its sections
		// are drawn - the HD characters always had them.
		if (type == 0x17 && size >= 12 && gebeanBE32(st + pc + 4) == 2) {
			pc = gebeanBE32(st + pc + 8);
			continue;
		}

		if (type == 0x12 && size >= 12) {
			u32 count = gebeanBE16(st + pc + 8);

			if (count > BEAN_MAXPAL) {
				count = BEAN_MAXPAL;
			}

			bm->numremap = 0;

			for (u32 k = 0; k < count && gebeanFits(pc + 12 + 8 * k, 2, len); k++) {
				bm->remap[bm->numremap++] = gebeanBE16(st + pc + 12 + 8 * k);
			}
		} else if (type == 0x2e && size >= 12) {
			vb = gebeanBE32(st + pc + 8);
		} else if (type == 0x2d && size >= 20) {
			tex = beanMaterialTexture(bm, st, pc, size, len);
		} else if (type == 0x13 && size >= 12) {
			u32 count = gebeanBE16(st + pc + 8);

			if (count > BEAN_MAXPAL) {
				count = BEAN_MAXPAL;
			}

			if (!gebeanFits(pc + 12, count, len)) {
				count = 0;
			}

			memcpy(pal, st + pc + 12, count);
			numpal = (u8)count;
		} else if ((type == 0x01 || (type == 0x30 && size >= 20 && gebeanBE32(st + pc + 16) == 0))
				&& size >= 16 && bm->numdraws < BEAN_MAXDRAWS) {
			// 0x30 is the originals' other draw: the same three words and a
			// fourth. The heads' draws marked 1 or 2 are extras over a whole
			// face - the sunglasses' arms, a stray piece a body's height below
			// Head B - and only a zero is part of the character.
			struct beandraw *d = &bm->draws[bm->numdraws++];

			d->vb = vb;
			d->tex = tex;
			d->prim = gebeanBE32(st + pc + 4);
			d->count = gebeanBE32(st + pc + 8);
			d->ib = gebeanBE32(st + pc + 12);
			d->numpal = numpal;
			memcpy(d->pal, pal, numpal);
		}

		pc += size;
	}
}

/**
 * The skeleton: a 'pose' record (19.12.06.0036) holds a count at +0x18 and
 * its entries at the offset in +0x34, 52 bytes each - a local translation,
 * the absolute bind, a spare, 1.0, then parent/child and sibling/self - in
 * the pool's SKEL_* name order.
 */
static void beanReadPose(struct beanmodel *bm, const char **names, s32 numnames)
{
	const u8 *d = bm->data;
	u32 at;
	u32 count;
	u32 entries;

	for (at = 0; at + 0x38 <= bm->datalen; at += 4) {
		if (memcmp(d + at, "pose\0\0\0\0", 8) == 0) {
			break;
		}
	}

	if (at + 0x38 > bm->datalen) {
		return;
	}

	count = gebeanBE32(d + at + 0x18);
	entries = gebeanBE32(d + at + 0x34);

	if (count > BEAN_MAXBONES) {
		count = BEAN_MAXBONES;
	}

	for (u32 i = 0; i < count && gebeanFits(entries + 52ull * i, 52, bm->datalen); i++) {
		const u8 *e = d + entries + 52 * i;

		bm->skel[i] = -1;

		for (s32 k = 0; k < 3; k++) {
			bm->bind[i][k] = gebeanBEF32(e + 12 + k * 4);
		}

		if ((s32)i < numnames) {
			for (s32 s = 0; s < SK_COUNT; s++) {
				if (strcmp(names[i], skelNames[s]) == 0) {
					bm->skel[i] = (s8)s;
					break;
				}
			}
		}

		bm->numbones = (s32)i + 1;
	}
}

/**
 * The index buffers. A draw names a runtime object; the descriptor is the
 * table row {object, .gpu offset, byte size, 1} that holds it, found by
 * looking for rows of that shape in .data - first one wins.
 */
static void beanFindIndexBuffers(struct beanmodel *bm)
{
	const u8 *d = bm->data;

	for (u32 o = 0; o + 16 <= bm->datalen && bm->numibs < BEAN_MAXIBS; o += 4) {
		const u32 obj = gebeanBE32(d + o);
		const u32 off = gebeanBE32(d + o + 4);
		const u32 size = gebeanBE32(d + o + 8);
		s32 known = 0;

		if (gebeanBE32(d + o + 12) != 1 || size == 0 || (size & 1) || !gebeanFits(off, size, bm->gpulen)
				|| obj >= bm->datalen) {
			continue;
		}

		for (s32 i = 0; i < bm->numibs; i++) {
			if (bm->ibs[i].obj == obj) {
				known = 1;
				break;
			}
		}

		if (!known) {
			bm->ibs[bm->numibs].obj = obj;
			bm->ibs[bm->numibs].off = off;
			bm->ibs[bm->numibs].size = size;
			bm->numibs++;
		}
	}
}

/** A draw's triangles as index triples into its vertex buffer. The caller frees *out. */
static s32 beanTriangles(const struct beanmodel *bm, const struct beandraw *d, u16 **out)
{
	const struct beanib *ib = NULL;
	const u8 *idx;
	u16 *tris;
	s32 n = 0;

	*out = NULL;

	for (s32 i = 0; i < bm->numibs; i++) {
		if (bm->ibs[i].obj == d->ib) {
			ib = &bm->ibs[i];
			break;
		}
	}

	if (!ib || d->count < 3 || (u64)d->count * 2 > ib->size) {
		return 0;
	}

	idx = bm->gpu + ib->off;
	tris = malloc(sizeof(u16) * 3 * ((size_t)d->count * 2));

	if (!tris) {
		return 0;
	}

	if (d->prim == 4) {
		for (u32 i = 0; i + 2 < d->count; i += 3, n++) {
			tris[n * 3] = gebeanBE16(idx + i * 2);
			tris[n * 3 + 1] = gebeanBE16(idx + (i + 1) * 2);
			tris[n * 3 + 2] = gebeanBE16(idx + (i + 2) * 2);
		}
	} else if (d->prim == 13) {
		for (u32 i = 0; i + 3 < d->count; i += 4) {
			const u16 a = gebeanBE16(idx + i * 2);
			const u16 b = gebeanBE16(idx + (i + 1) * 2);
			const u16 c = gebeanBE16(idx + (i + 2) * 2);
			const u16 e = gebeanBE16(idx + (i + 3) * 2);

			tris[n * 3] = a; tris[n * 3 + 1] = b; tris[n * 3 + 2] = c; n++;
			tris[n * 3] = a; tris[n * 3 + 1] = c; tris[n * 3 + 2] = e; n++;
		}
	} else if (d->prim == 5) {
		for (u32 i = 0; i + 2 < d->count; i++, n++) {
			const u16 a = gebeanBE16(idx + i * 2);
			const u16 b = gebeanBE16(idx + (i + 1) * 2);
			const u16 c = gebeanBE16(idx + (i + 2) * 2);

			tris[n * 3] = (i & 1) ? b : a;
			tris[n * 3 + 1] = (i & 1) ? a : b;
			tris[n * 3 + 2] = c;
		}
	}

	*out = tris;

	return n;
}

static void beanFree(struct beanmodel *bm)
{
	caffClose(&bm->caff);
	free(bm->draws);
	free(bm->file);
	memset(bm, 0, sizeof(*bm));
}

static s32 beanLoad(struct beanmodel *bm, const char *source)
{
	const char *names[BEAN_MAXBONES];
	s32 numnames = 0;
	char path[FS_MAXPATH + 1];
	FILE *fp;
	long size;
	struct caff *c = &bm->caff;
	s32 idata = -1, igpu = -1, istream = -1, ipool = -1;
	const u8 *pool;
	u32 poollen = 0;

	memset(bm, 0, sizeof(*bm));
	snprintf(path, sizeof(path), "%s/%s/default.bin", rootPath, source);

	fp = fopen(path, "rb");

	if (!fp) {
		sysLogPrintf(LOG_ERROR, "gebean: %s is missing", path);
		return 0;
	}

	if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) <= 0 || size > 64 * 1024 * 1024) {
		fclose(fp);
		return 0;
	}

	rewind(fp);
	bm->file = malloc((size_t)size);

	if (!bm->file || fread(bm->file, 1, (size_t)size, fp) != (size_t)size) {
		fclose(fp);
		beanFree(bm);
		return 0;
	}

	fclose(fp);

	bm->draws = calloc(BEAN_MAXDRAWS, sizeof(*bm->draws));

	if (!bm->draws || !caffOpen(c, bm->file, (u32)size)) {
		sysLogPrintf(LOG_ERROR, "gebean: %s is not a CAFF this reads", path);
		beanFree(bm);
		return 0;
	}

	idata = caffFind(c, 1, ".data");
	igpu = caffFind(c, 1, ".gpu");
	istream = caffFind(c, 1, ".stream");
	ipool = caffFind(c, 2, ".data");

	if (idata < 0 || igpu < 0 || istream < 0) {
		sysLogPrintf(LOG_ERROR, "gebean: %s is not a rendergraph", path);
		beanFree(bm);
		return 0;
	}

	bm->data = caffBlob(c, idata, &bm->datalen);
	bm->gpu = caffBlob(c, igpu, &bm->gpulen);
	bm->stream = caffBlob(c, istream, &bm->streamlen);

	if (!bm->data || !bm->gpu || !bm->stream) {
		beanFree(bm);
		return 0;
	}

	// The pool names the bones in pose order among its other strings.
	pool = ipool >= 0 ? caffBlob(c, ipool, &poollen) : NULL;

	for (u32 at = 0; pool && at < poollen && numnames < BEAN_MAXBONES; ) {
		const u8 *nul = memchr(pool + at, '\0', poollen - at);
		const u32 end = nul ? (u32)(nul - pool) : poollen;

		if (nul && end - at > 5 && memcmp(pool + at, "SKEL_", 5) == 0) {
			names[numnames++] = (const char *)pool + at;
		}

		at = end + 1;
	}

	for (u32 i = 0; i < c->numfiles && bm->numtex < GEBEAN_MAXMATS; i++) {
		u32 blen;
		const u8 *b = caffBlob(c, (s32)i, &blen);

		if (b && blen >= 8 && memcmp(b, "texture\0", 8) == 0) {
			bm->texfile[bm->numtex++] = (s32)i;
		}
	}

	beanReadPose(bm, names, numnames);
	beanWalkStream(bm);
	beanFindIndexBuffers(bm);
	bm->uvscale = beanMeasureUvScale(bm);

	return 1;
}

/* -------------------------------------------------------------------------
 * The pictures
 * ------------------------------------------------------------------------- */

#define GEBEAN_TEXCACHE 512

static struct {
	char key[80];
	const void *tile;
	u8 alpha;
	u8 soft;
} texCache[GEBEAN_TEXCACHE];
static s32 numTexCache;

/**
 * A texture as RGBA in the game's row order. The header is a D3D texture: the
 * format word at +0x18 (its low byte 0x52/0x53/0x54 DXT1/3/5, 0x86 8888, with
 * 0x40 the tiled bit and byte +0x1a the 8-in-16 swap), width and height at
 * +0x24. The texels are the asset's own .gpu entry, or - where it has none -
 * the file's shared "texture pairs" .gpu entry at the base offset in +0x28.
 */
static u8 *beanDecodeTexture(const struct beanmodel *bm, s32 t, s32 *outW, s32 *outH)
{
	const struct caff *c = &bm->caff;
	struct x360fetch fetch;
	u32 blen, glen;
	const u8 *b = caffBlob(c, bm->texfile[t], &blen);
	const u8 *g;
	s32 gi;
	u32 base, first, w, h, bpe, ew, eh;
	u64 need, have;
	u8 *copy;
	u8 *rgba;

	if (!b || blen < 0x40) {
		return NULL;
	}

	w = gebeanBE16(b + 0x24);
	h = gebeanBE16(b + 0x26);
	base = gebeanBE32(b + 0x28);
	first = 0;

	if (gebeanBE32(b + 0x38) && gebeanFits(gebeanBE32(b + 0x3c), 4, blen)) {
		first = gebeanBE32(b + gebeanBE32(b + 0x3c));
	}

	gi = caffFind(c, c->files[bm->texfile[t]].asset, ".gpu");

	if (gi >= 0) {
		base = 0;
	} else {
		for (u32 i = 0; i < c->numfiles && gi < 0; i++) {
			if (strcmp(caffAssetName(c, c->files[i].asset), "texture pairs") == 0
					&& strncmp(caffSectName(c, (s32)i), ".gpu", 4) == 0) {
				gi = (s32)i;
			}
		}
	}

	g = gi >= 0 ? caffBlob(c, gi, &glen) : NULL;

	if (!g || w == 0 || h == 0 || w > 4096 || h > 4096 || (u64)base + first > glen) {
		return NULL;
	}

	memset(&fetch, 0, sizeof(fetch));
	fetch.width = w;
	fetch.height = h;
	fetch.format = b[0x1b] & 0x3f;
	fetch.tiled = 1;
	fetch.endian = b[0x1a] ? (fetch.format == X360_FMT_8888 ? 2 : 1) : 0;

	if (!x360FetchSupported(&fetch)) {
		sysLogPrintf(LOG_WARNING, "gebean: texture format %02x is not one this decodes", b[0x1b]);
		return NULL;
	}

	// The whole tiled surface is what the decode reaches into; a file that
	// stops short of it is padded rather than refused.
	if (fetch.format == X360_FMT_8888) {
		bpe = 4;
		fetch.pitch = w;
		ew = (w + 31) & ~31u;
		eh = (h + 31) & ~31u;
	} else {
		bpe = fetch.format == X360_FMT_DXT1 ? 8 : 16;
		fetch.pitch = (w + 3) & ~3u;
		ew = (((w + 3) / 4) + 31) & ~31u;
		eh = (((h + 3) / 4) + 31) & ~31u;
	}

	need = (u64)ew * eh * bpe;
	have = glen - ((u64)base + first);
	copy = calloc((size_t)need, 1);
	rgba = malloc((size_t)w * h * 4);

	if (!copy || !rgba) {
		free(copy);
		free(rgba);
		return NULL;
	}

	memcpy(copy, g + base + first, (size_t)(have < need ? have : need));

	if (!x360DecodeTexture(copy, (u32)need, &fetch, rgba)) {
		free(copy);
		free(rgba);
		return NULL;
	}

	free(copy);

	// Decoded top row first, as a PNG of it would be; the renderer wants the
	// first uploaded row first (modelpackBindMaterial() does the same).
	for (u32 y = 0; y < h / 2; y++) {
		u8 *ra = rgba + (size_t)y * w * 4;
		u8 *rb = rgba + (size_t)(h - 1 - y) * w * 4;

		for (u32 x = 0; x < w * 4; x++) {
			const u8 tmp = ra[x];
			ra[x] = rb[x];
			rb[x] = tmp;
		}
	}

	*outW = (s32)w;
	*outH = (s32)h;

	return rgba;
}

static s32 beanBindTexture(const struct beanmodel *bm, const char *source, s32 t,
		const void **tile, u8 *alpha, u8 *soft)
{
	char key[80];
	s32 w, h, a = 0, s = 0;
	u8 *rgba;

	snprintf(key, sizeof(key), "gebean:%s:%d", source, t);

	for (s32 i = 0; i < numTexCache; i++) {
		if (strcmp(texCache[i].key, key) == 0) {
			*tile = texCache[i].tile;
			*alpha = texCache[i].alpha;
			*soft = texCache[i].soft;
			return *tile != NULL;
		}
	}

	rgba = beanDecodeTexture(bm, t, &w, &h);
	*tile = rgba ? xblaTexBindImage(key, rgba, w, h) : NULL;

	if (*tile) {
		xblaTexImageInfo(*tile, &a, &s);
	} else {
		sysLogPrintf(LOG_WARNING, "gebean: texture %d of %s would not decode", t, source);
	}

	*alpha = (u8)a;
	*soft = (u8)s;

	if (numTexCache < GEBEAN_TEXCACHE) {
		snprintf(texCache[numTexCache].key, sizeof(texCache[numTexCache].key), "%s", key);
		texCache[numTexCache].tile = *tile;
		texCache[numTexCache].alpha = *alpha;
		texCache[numTexCache].soft = *soft;
		numTexCache++;
	}

	return *tile != NULL;
}

/* -------------------------------------------------------------------------
 * The mesh
 * ------------------------------------------------------------------------- */

static f32 vecLen(const f32 *a)
{
	return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

/** The shortest rotation taking direction a to direction b (Rodrigues), rows. */
static void rotationBetween(const f32 *a0, const f32 *b0, f32 r[3][3])
{
	const f32 la = vecLen(a0);
	const f32 lb = vecLen(b0);
	f32 a[3], b[3], v[3], vx[3][3];
	f32 c, k;

	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			r[i][j] = i == j ? 1.0f : 0.0f;
		}
	}

	if (la < 1e-6f || lb < 1e-6f) {
		return;
	}

	for (s32 i = 0; i < 3; i++) {
		a[i] = a0[i] / la;
		b[i] = b0[i] / lb;
	}

	v[0] = a[1] * b[2] - a[2] * b[1];
	v[1] = a[2] * b[0] - a[0] * b[2];
	v[2] = a[0] * b[1] - a[1] * b[0];
	c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

	if (vecLen(v) < 1e-9f) {
		if (c < 0.0f) {
			for (s32 i = 0; i < 3; i++) {
				r[i][i] = -1.0f;
			}
		}

		return;
	}

	vx[0][0] = 0.0f;  vx[0][1] = -v[2]; vx[0][2] = v[1];
	vx[1][0] = v[2];  vx[1][1] = 0.0f;  vx[1][2] = -v[0];
	vx[2][0] = -v[1]; vx[2][1] = v[0];  vx[2][2] = 0.0f;
	k = 1.0f / (1.0f + c);

	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			f32 sq = 0.0f;

			for (s32 m = 0; m < 3; m++) {
				sq += vx[i][m] * vx[m][j];
			}

			r[i][j] += vx[i][j] + sq * k;
		}
	}
}

static void rotApply(const f32 r[3][3], const f32 *p, f32 *out)
{
	for (s32 i = 0; i < 3; i++) {
		out[i] = r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2];
	}
}

/** What the N64 model's rest skeleton says, by Bean bone. */
struct beanrig {
	s32 have[SK_COUNT];
	f32 joint[SK_COUNT][3];
	s32 mtx[SK_COUNT];
	f32 rot[SK_COUNT][3][3];
	f32 scale;

	// Each bone's linear map from Bean's bind (at the rig's scale) onto the
	// model's rest, about the bone's own joint: beanFitRig().
	f32 lin[SK_COUNT][3][3];

	// Every joint of the model by matrix, for the palette's inverse binds.
	s32 hasrest[GEBEAN_MAXMTX];
	f32 rest[GEBEAN_MAXMTX][3];

	// The palette entry for each matrix a Bean bone moves: three rows of four.
	s32 haspal[GEBEAN_MAXMTX];
	f32 pal[GEBEAN_MAXMTX][12];
};

struct beanlimbjoint {
	f32 ax;
	struct modelnode *node;
};

/**
 * The model's joints named as Bean's bones, from their rest positions: the
 * chrinfo root is the base, a joint on the middle line is the back below 200
 * units and the neck above, and the rest are three to a limb - an arm above
 * 200, a leg below, left where x is positive - in order outwards. That is
 * GoldenEye's star rest pose, where the limbs run along x.
 */
static s32 beanRigFromModel(struct modeldef *modeldef, struct beanrig *rig,
		struct modelnode **joints, s8 *jointskel, s32 *outnumjoints)
{
	struct beanlimbjoint limbs[4][8];
	s32 numlimb[4] = { 0, 0, 0, 0 };
	s32 numjoints = 0;
	s32 walked = 0;

	memset(rig, 0, sizeof(*rig));

	for (struct modelnode *node = modeldef->rootnode; node && walked < 4096; node = gebeanNextNode(node), walked++) {
		const u32 type = node->type & 0xff;
		f32 rest[3];
		s32 mtx;
		s32 skel = -1;

		if (type == MODELNODETYPE_CHRINFO) {
			mtx = node->rodata->chrinfo.mtxindex;
			rest[0] = rest[1] = rest[2] = 0.0f;
		} else if (type == MODELNODETYPE_POSITION) {
			mtx = node->rodata->position.mtxindex0;
			xblaMeshNodeRestOffset(node, rest);
		} else {
			continue;
		}

		if (mtx >= 0 && mtx < GEBEAN_MAXMTX) {
			rig->hasrest[mtx] = 1;
			memcpy(rig->rest[mtx], rest, sizeof(rest));
		}

		if (type == MODELNODETYPE_CHRINFO) {
			skel = rig->have[SK_BASE] ? -1 : SK_BASE;
		} else if (fabsf(rest[0]) < 30.0f) {
			skel = rest[1] < 200.0f ? SK_BACK : SK_NECK;

			if (rig->have[skel]) {
				skel = -1;
			}
		} else {
			const s32 limb = (rest[0] > 0.0f ? 0 : 1) * 2 + (rest[1] > 200.0f ? 0 : 1);

			if (numlimb[limb] < 8) {
				limbs[limb][numlimb[limb]].ax = fabsf(rest[0]);
				limbs[limb][numlimb[limb]].node = node;
				numlimb[limb]++;
			}
		}

		if (numjoints < 64) {
			joints[numjoints] = node;
			jointskel[numjoints] = (s8)skel;
			numjoints++;
		}

		if (skel >= 0) {
			rig->have[skel] = 1;
			rig->mtx[skel] = mtx;
			memcpy(rig->joint[skel], rest, sizeof(rest));
		}
	}

	// limb 0 left arm, 1 left leg, 2 right arm, 3 right leg
	for (s32 limb = 0; limb < 4; limb++) {
		static const s8 order[4][3] = {
			{ SK_LF_SHOULDER, SK_LF_ELBOW, SK_LF_WRIST },
			{ SK_LF_HIP, SK_LF_KNEE, SK_LF_ANKLE },
			{ SK_RT_SHOULDER, SK_RT_ELBOW, SK_RT_WRIST },
			{ SK_RT_HIP, SK_RT_KNEE, SK_RT_ANKLE },
		};

		if (numlimb[limb] != 3) {
			sysLogPrintf(LOG_WARNING, "gebean: a limb of the model has %d joints, not 3", numlimb[limb]);
			return 0;
		}

		// Three, so a sort is a few swaps.
		for (s32 i = 0; i < 3; i++) {
			for (s32 j = i + 1; j < 3; j++) {
				if (limbs[limb][j].ax < limbs[limb][i].ax) {
					const struct beanlimbjoint tmp = limbs[limb][i];
					limbs[limb][i] = limbs[limb][j];
					limbs[limb][j] = tmp;
				}
			}
		}

		for (s32 i = 0; i < 3; i++) {
			const struct modelnode *node = limbs[limb][i].node;
			const s32 skel = order[limb][i];

			rig->have[skel] = 1;
			rig->mtx[skel] = node->rodata->position.mtxindex0;
			xblaMeshNodeRestOffset(node, rig->joint[skel]);

			for (s32 j = 0; j < numjoints; j++) {
				if (joints[j] == node) {
					jointskel[j] = (s8)skel;
				}
			}
		}
	}

	for (s32 s = 0; s < SK_COUNT; s++) {
		if (s != SK_POSITION && !rig->have[s]) {
			sysLogPrintf(LOG_WARNING, "gebean: the model has no joint for %s", skelNames[s]);
			return 0;
		}
	}

	rig->have[SK_POSITION] = 1;
	rig->mtx[SK_POSITION] = rig->mtx[SK_BASE];
	memcpy(rig->joint[SK_POSITION], rig->joint[SK_BASE], sizeof(rig->joint[0]));

	*outnumjoints = numjoints;

	return 1;
}

static void mat3Mul(const f32 a[3][3], const f32 b[3][3], f32 out[3][3])
{
	for (s32 i = 0; i < 3; i++) {
		for (s32 j = 0; j < 3; j++) {
			out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		}
	}
}

static f32 mat3Det(const f32 m[3][3])
{
	return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
		- m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
		+ m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

static s32 mat3Inverse(const f32 m[3][3], f32 out[3][3])
{
	const f32 det = mat3Det(m);

	if (fabsf(det) < 1e-12f) {
		return 0;
	}

	out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
	out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
	out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
	out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
	out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
	out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
	out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
	out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
	out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;

	return 1;
}

static void vecCross(const f32 *a, const f32 *b, f32 *out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/**
 * A bone with three children - the base (back and both hips) and the back
 * (neck and both shoulders) - as the linear map that takes Bean's three
 * segments nearest onto the model's, least squares. The three lie in the
 * figure's front plane, so the depth is added as a fourth pair: across the two
 * sides, kept at the rig's scale. Where Bean's hips or shoulders sit wider or
 * higher than the model's, one turn cannot put both on their joints, and a
 * crotch or armpit blended between the base and a hip, or the back and a
 * shoulder, is pulled apart by the difference. False when the fit is
 * degenerate or would mirror the figure.
 */
static s32 beanFitBranch(struct beanrig *rig, const f32 bind[SK_COUNT][3], s32 a, const s8 children[3])
{
	f32 db[4][3], dj[4][3], cj[3];
	f32 sjb[3][3], sbb[3][3], inv[3][3], lin[3][3];
	f32 lb, lj;

	for (s32 c = 0; c < 3; c++) {
		for (s32 k = 0; k < 3; k++) {
			db[c][k] = (bind[(s32)children[c]][k] - bind[a][k]) * rig->scale;
			dj[c][k] = rig->joint[(s32)children[c]][k] - rig->joint[a][k];
		}
	}

	vecCross(db[1], db[2], db[3]);
	vecCross(dj[1], dj[2], cj);
	lb = vecLen(db[3]);
	lj = vecLen(cj);

	if (lb < 1e-6f || lj < 1e-6f) {
		return 0;
	}

	for (s32 k = 0; k < 3; k++) {
		dj[3][k] = cj[k] * lb / lj;
	}

	memset(sjb, 0, sizeof(sjb));
	memset(sbb, 0, sizeof(sbb));

	for (s32 p = 0; p < 4; p++) {
		for (s32 i = 0; i < 3; i++) {
			for (s32 j = 0; j < 3; j++) {
				sjb[i][j] += dj[p][i] * db[p][j];
				sbb[i][j] += db[p][i] * db[p][j];
			}
		}
	}

	if (!mat3Inverse(sbb, inv)) {
		return 0;
	}

	mat3Mul(sjb, inv, lin);

	if (mat3Det(lin) <= 0.0f) {
		return 0;
	}

	memcpy(rig->lin[a], lin, sizeof(lin));

	return 1;
}

/**
 * Each bone's map onto the rig, and from those the palette. A bone on a limb
 * is turned so its segment lies along the model's and stretched along it to
 * the model's length, so the parent's reading of the child's joint is the
 * joint - with one scale for the whole figure the two disagreed by however
 * much Bean's limb proportions differ from the model's, which on Perfect
 * Dark's own bodies was enough to stretch an arm and pinch an elbow. The base
 * and back are fitted to their three children. The ends of the chains turn
 * with their parent and are not stretched.
 */
static void beanFitPalette(struct beanrig *rig, const f32 bind[SK_COUNT][3])
{
	static const s8 basechildren[3] = { SK_BACK, SK_LF_HIP, SK_RT_HIP };
	static const s8 backchildren[3] = { SK_NECK, SK_LF_SHOULDER, SK_RT_SHOULDER };

	for (s32 a = 0; a < SK_COUNT; a++) {
		const s32 b = skelChild[a];

		memcpy(rig->lin[a], rig->rot[a], sizeof(rig->lin[a]));

		if ((a == SK_BASE && beanFitBranch(rig, bind, a, basechildren))
				|| (a == SK_BACK && beanFitBranch(rig, bind, a, backchildren))) {
			continue;
		}

		if (b >= 0) {
			f32 db[3], dj[3], u[3], stretch[3][3];
			f32 lb, lj, k;

			for (s32 i = 0; i < 3; i++) {
				db[i] = bind[b][i] - bind[a][i];
				dj[i] = rig->joint[b][i] - rig->joint[a][i];
			}

			lb = vecLen(db) * rig->scale;
			lj = vecLen(dj);

			if (lb < 1e-6f) {
				continue;
			}

			k = lj / lb;

			for (s32 i = 0; i < 3; i++) {
				u[i] = db[i] * rig->scale / lb;
			}

			for (s32 i = 0; i < 3; i++) {
				for (s32 j = 0; j < 3; j++) {
					stretch[i][j] = (i == j ? 1.0f : 0.0f) + (k - 1.0f) * u[i] * u[j];
				}
			}

			mat3Mul((const f32 (*)[3])rig->rot[a], (const f32 (*)[3])stretch, rig->lin[a]);
		}
	}

	// An end takes its parent's turn, not its parent's fit
	for (s32 a = 0; a < SK_COUNT; a++) {
		if (skelInherit[a] >= 0 && a != SK_POSITION) {
			memcpy(rig->lin[a], rig->rot[a], sizeof(rig->lin[a]));
		}
	}

	// The position bone shares the base's matrix and takes the base's fit
	memcpy(rig->lin[SK_POSITION], rig->lin[SK_BASE], sizeof(rig->lin[SK_POSITION]));

	memset(rig->haspal, 0, sizeof(rig->haspal));

	// Palette entry m, for the first bone that moves matrix m: out of Bean's
	// bind about the bone's joint, through its fit, onto the joint, less the
	// rest the game's matrix for it already carries.
	for (s32 a = 0; a < SK_COUNT; a++) {
		const s32 m = rig->mtx[a];
		f32 sb[3], turned[3], target[3];

		if (!rig->have[a] || m < 0 || m >= GEBEAN_MAXMTX || rig->haspal[m]) {
			continue;
		}

		for (s32 k = 0; k < 3; k++) {
			sb[k] = bind[a][k] * rig->scale;
		}

		rotApply((const f32 (*)[3])rig->lin[a], sb, turned);

		memcpy(target, rig->joint[a], sizeof(target));

		if (a == SK_NECK) {
			// The neck goes where the back's fit carries Bean's, not onto the
			// model's joint: the fit is a compromise between the neck and the
			// shoulders, and the model's shoulders sit higher up its back, so
			// the collar came out above the joint and swallowed the neck (15
			// units on Xenia on the guard, 36 on the technician). The game
			// turns the head about the joint, a few units off.
			f32 d[3], fitted[3];

			for (s32 k = 0; k < 3; k++) {
				d[k] = (bind[SK_NECK][k] - bind[SK_BACK][k]) * rig->scale;
			}

			rotApply((const f32 (*)[3])rig->lin[SK_BACK], d, fitted);

			for (s32 k = 0; k < 3; k++) {
				target[k] = rig->joint[SK_BACK][k] + fitted[k];
			}
		}

		for (s32 r = 0; r < 3; r++) {
			const f32 rest = rig->hasrest[m] ? rig->rest[m][r] : rig->joint[a][r];

			for (s32 c = 0; c < 3; c++) {
				rig->pal[m][r * 4 + c] = rig->lin[a][r][c];
			}

			rig->pal[m][r * 4 + 3] = target[r] - rest - turned[r];
		}

		rig->haspal[m] = 1;
	}
}

/** The scale and each bone's turn onto the rig, from Bean's bind. */
static s32 beanFitRig(struct beanrig *rig, const f32 bind[SK_COUNT][3], const s32 *havebind)
{
	f32 num = 0.0f;
	f32 den = 0.0f;

	for (s32 a = 0; a < SK_COUNT; a++) {
		const s32 b = skelChild[a];
		f32 dj[3], db[3];

		if (b < 0) {
			continue;
		}

		if (!havebind[a] || !havebind[b]) {
			sysLogPrintf(LOG_WARNING, "gebean: Bean's skeleton has no %s or %s", skelNames[a], skelNames[b]);
			return 0;
		}

		for (s32 k = 0; k < 3; k++) {
			dj[k] = rig->joint[b][k] - rig->joint[a][k];
			db[k] = bind[b][k] - bind[a][k];
		}

		if (a != SK_BASE) {
			num += vecLen(dj);
			den += vecLen(db);
		}

		rotationBetween(db, dj, rig->rot[a]);
	}

	for (s32 a = 0; a < SK_COUNT; a++) {
		if (skelInherit[a] >= 0) {
			memcpy(rig->rot[a], rig->rot[skelInherit[a]], sizeof(rig->rot[a]));
		}
	}

	rig->scale = den > 0.0f ? num / den : GEBEAN_HEAD_SCALE;

	beanFitPalette(rig, bind);

	return 1;
}

struct beantri {
	u16 group;
	u16 tex;
	u32 order;
	u16 v[3];
};

struct beanout {
	s32 numverts, capverts;
	f32 *pos;    // 3 per vertex
	f32 *nrm;    // 3
	f32 *uv;     // 2
	f32 *weight; // 3
	u8 *bone;    // 3
	u32 *argb;   // 1
	s32 numtris, captris;
	struct beantri *tris;
};

/**
 * argb is the vertex colour the mesh is drawn with: white for a character,
 * whose textures carry the colour, and a rigid prop's own, which tints the
 * N64-look originals' intensity textures (a pickup would be white otherwise).
 */
static s32 beanAddVertex(struct beanout *o, const f32 *pos, const f32 *nrm, const f32 *uv,
		const u8 *bone, const f32 *weight, u32 argb)
{
	if (o->numverts >= GEBEAN_MAXVERTS) {
		return -1;
	}

	if (o->numverts >= o->capverts) {
		const s32 cap = o->capverts ? o->capverts * 2 : 4096;
		f32 *p = realloc(o->pos, cap * 3 * sizeof(f32));
		f32 *n = p ? realloc(o->nrm, cap * 3 * sizeof(f32)) : NULL;
		f32 *u = n ? realloc(o->uv, cap * 2 * sizeof(f32)) : NULL;
		f32 *w = u ? realloc(o->weight, cap * 3 * sizeof(f32)) : NULL;
		u8 *b = w ? realloc(o->bone, cap * 3) : NULL;
		u32 *c = b ? realloc(o->argb, cap * sizeof(u32)) : NULL;

		if (p) o->pos = p;
		if (n) o->nrm = n;
		if (u) o->uv = u;
		if (w) o->weight = w;
		if (b) o->bone = b;
		if (c) o->argb = c;

		if (!c) {
			return -1;
		}

		o->capverts = cap;
	}

	memcpy(o->pos + o->numverts * 3, pos, 3 * sizeof(f32));
	memcpy(o->nrm + o->numverts * 3, nrm, 3 * sizeof(f32));
	memcpy(o->uv + o->numverts * 2, uv, 2 * sizeof(f32));
	memcpy(o->weight + o->numverts * 3, weight, 3 * sizeof(f32));
	memcpy(o->bone + o->numverts * 3, bone, 3);
	o->argb[o->numverts] = argb;

	return o->numverts++;
}

static s32 beanAddTri(struct beanout *o, s32 group, s32 tex, u16 a, u16 b, u16 c)
{
	struct beantri *t;

	if (o->numtris >= o->captris) {
		const s32 cap = o->captris ? o->captris * 2 : 8192;
		struct beantri *grown = realloc(o->tris, cap * sizeof(*grown));

		if (!grown) {
			return 0;
		}

		o->tris = grown;
		o->captris = cap;
	}

	t = &o->tris[o->numtris];
	t->group = (u16)group;
	t->tex = (u16)tex;
	t->order = (u32)o->numtris;
	t->v[0] = a;
	t->v[1] = b;
	t->v[2] = c;
	o->numtris++;

	return 1;
}

static void beanOutFree(struct beanout *o)
{
	free(o->pos);
	free(o->nrm);
	free(o->uv);
	free(o->weight);
	free(o->bone);
	free(o->argb);
	free(o->tris);
}

static int beanTriCompare(const void *a, const void *b)
{
	const struct beantri *x = a;
	const struct beantri *y = b;

	if (x->group != y->group) {
		return x->group < y->group ? -1 : 1;
	}

	if (x->tex != y->tex) {
		return x->tex < y->tex ? -1 : 1;
	}

	return x->order < y->order ? -1 : x->order > y->order;
}

/**
 * The triangles laid out in 4J's mesh layout (xblamesh.py, CLAUDE-notes/
 * xbla.md "The mesh format"): a group per list node, a draw per material
 * within it, a skinned vertex of stride 48, and a palette of the model's
 * matrices whose entries translate each joint's rest back to its origin. The
 * header's scale is 100, so everything is in the model's own units.
 */
static u8 *beanWriteMesh(struct beanout *o, s32 numgroups, s32 nummatrices, const struct beanrig *rig,
		const u32 *matwords, s32 nummatwords, u64 *outAbsent, u32 *outLen)
{
	u32 numdraws = 0;
	u32 groupoffset, drawoffset, vertexoffset, indexoffset, len;
	u8 *file;
	u32 drawat = 0;
	s32 t = 0;

	if (o->numtris == 0 || o->numverts == 0) {
		return NULL;
	}

	qsort(o->tris, o->numtris, sizeof(*o->tris), beanTriCompare);

	for (s32 i = 0; i < o->numtris; i++) {
		if (i == 0 || o->tris[i].group != o->tris[i - 1].group || o->tris[i].tex != o->tris[i - 1].tex) {
			numdraws++;
		}
	}

	if (numdraws > GEBEAN_MAXDRAWS) {
		return NULL;
	}

	groupoffset = 32 + 48 * (u32)nummatrices;
	drawoffset = groupoffset + 12 * (u32)numgroups;
	vertexoffset = drawoffset + 12 * numdraws;
	indexoffset = vertexoffset + 48 * (u32)o->numverts;
	len = indexoffset + 6 * (u32)o->numtris;

	file = calloc(len, 1);

	if (!file) {
		return NULL;
	}

	gebeanPutBE32(file, (u32)o->numverts);
	gebeanPutBE32(file + 4, vertexoffset);
	gebeanPutBE32(file + 8, indexoffset);
	gebeanPutBE32(file + 12, numdraws);
	gebeanPutBE32(file + 16, drawoffset);
	gebeanPutBE32(file + 20, (u32)nummatrices);
	gebeanPutBEF32(file + 24, 100.0f);
	gebeanPutBE32(file + 28, groupoffset);

	// Three rows of four: a body's fitted inverse binds (beanFitPalette()),
	// and for any other matrix an identity turn with its joint's rest taken
	// back off.
	for (s32 i = 0; i < nummatrices; i++) {
		u8 *mtx = file + 32 + 48 * i;

		if (rig && i < GEBEAN_MAXMTX && rig->haspal[i]) {
			for (s32 k = 0; k < 12; k++) {
				gebeanPutBEF32(mtx + k * 4, rig->pal[i][k]);
			}

			continue;
		}

		for (s32 r = 0; r < 3; r++) {
			gebeanPutBEF32(mtx + (r * 4 + r) * 4, 1.0f);
			gebeanPutBEF32(mtx + (r * 4 + 3) * 4, rig && i < GEBEAN_MAXMTX && rig->hasrest[i] ? -rig->rest[i][r] : 0.0f);
		}
	}

	*outAbsent = 0;

	for (s32 g = 0; g < numgroups; g++) {
		const u8 *group = file + groupoffset + 12 * g;
		const u32 firstdraw = drawat;

		while (t < o->numtris && o->tris[t].group == g) {
			const s32 from = t;
			const u16 tex = o->tris[t].tex;
			u8 *draw = file + drawoffset + 12 * drawat;

			while (t < o->numtris && o->tris[t].group == g && o->tris[t].tex == tex) {
				t++;
			}

			gebeanPutBE32(draw, (u32)from);
			gebeanPutBE32(draw + 4, (u32)(t - from));
			gebeanPutBE32(draw + 8, matwords[tex < nummatwords ? tex : nummatwords - 1]);
			drawat++;
		}

		gebeanPutBE32((u8 *)group, firstdraw);
		gebeanPutBE32((u8 *)group + 4, drawat - firstdraw);
		gebeanPutBE32((u8 *)group + 8, 0);

		if (drawat == firstdraw && g < 64) {
			*outAbsent |= 1ull << g;
		}
	}

	for (s32 i = 0; i < o->numverts; i++) {
		u8 *v = file + vertexoffset + 48 * i;
		const u8 *bone = o->bone + i * 3;
		const f32 *w = o->weight + i * 3;

		for (s32 k = 0; k < 3; k++) {
			gebeanPutBEF32(v + k * 4, o->pos[i * 3 + k]);
			gebeanPutBEF32(v + 20 + k * 4, o->nrm[i * 3 + k]);
		}

		gebeanPutBEF32(v + 12, o->uv[i * 2]);
		gebeanPutBEF32(v + 16, o->uv[i * 2 + 1]);
		gebeanPutBE32(v + 32, o->argb[i]);
		gebeanPutBEF32(v + 36, w[0]);
		gebeanPutBEF32(v + 40, w[1]);
		gebeanPutBE32(v + 44, ((u32)bone[0] << 24) | ((u32)bone[1] << 16) | ((u32)bone[2] << 8) | 3);
	}

	for (s32 i = 0; i < o->numtris; i++) {
		u8 *idx = file + indexoffset + 6 * i;

		for (s32 k = 0; k < 3; k++) {
			idx[k * 2] = (u8)(o->tris[i].v[k] >> 8);
			idx[k * 2 + 1] = (u8)o->tris[i].v[k];
		}
	}

	*outLen = len;

	return file;
}

/** A node under a toggle: a head's glasses, hat or second hair, which Bean's head already has. */
static s32 beanNodeIsToggled(const struct modelnode *node)
{
	s32 walked = 0;

	for (node = node->parent; node && walked < 64; node = node->parent, walked++) {
		if ((node->type & 0xff) == MODELNODETYPE_TOGGLE) {
			return 1;
		}
	}

	return 0;
}

/** The Bean bone a list node of the body hangs off, or -1. */
static s32 beanNodeSkel(const struct modelnode *node, struct modelnode **joints, const s8 *jointskel, s32 numjoints)
{
	s32 walked = 0;

	for (node = node->parent; node && walked < 64; node = node->parent, walked++) {
		const u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_POSITION || type == MODELNODETYPE_CHRINFO) {
			for (s32 j = 0; j < numjoints; j++) {
				if (joints[j] == node) {
					return jointskel[j];
				}
			}

			return -1;
		}
	}

	return -1;
}

/**
 * A vertex whose share between the neck and the back is far from its
 * neighbours' takes theirs. Natalya's mouth has one two thirds on her back
 * among neighbours on it by a thirtieth: Bean's own bind hid it, but the
 * game turns the head against the torso, and that vertex went with the torso
 * and drew a spike through her upper lip.
 */
static void beanSmoothNeckWeights(struct beanout *o, s32 neck, s32 back)
{
	f32 *share;
	f32 *sum;
	s32 *count;

	if (neck < 0 || back < 0 || neck == back || o->numverts <= 0) {
		return;
	}

	share = malloc(o->numverts * sizeof(f32));
	sum = calloc(o->numverts, sizeof(f32));
	count = calloc(o->numverts, sizeof(s32));

	if (share && sum && count) {
		for (s32 v = 0; v < o->numverts; v++) {
			f32 n = 0.0f, b = 0.0f, other = 0.0f;

			for (s32 k = 0; k < 3; k++) {
				const f32 w = o->weight[v * 3 + k];

				if (w <= 0.0f) {
					continue;
				}

				if (o->bone[v * 3 + k] == neck) {
					n += w;
				} else if (o->bone[v * 3 + k] == back) {
					b += w;
				} else {
					other += w;
				}
			}

			share[v] = n > 0.0f && other <= 0.0f ? b / (n + b) : -1.0f;
		}

		for (s32 t = 0; t < o->numtris; t++) {
			const u16 *tv = o->tris[t].v;

			for (s32 i = 0; i < 3; i++) {
				const s32 a = tv[i];
				const s32 c = tv[(i + 1) % 3];

				if (a == c || share[a] < 0.0f || share[c] < 0.0f) {
					continue;
				}

				sum[a] += share[c];
				count[a]++;
				sum[c] += share[a];
				count[c]++;
			}
		}

		for (s32 v = 0; v < o->numverts; v++) {
			f32 mean;

			if (share[v] < 0.0f || count[v] < 4) {
				continue;
			}

			mean = sum[v] / count[v];

			if (fabsf(share[v] - mean) <= 0.3f) {
				continue;
			}

			for (s32 k = 0; k < 3; k++) {
				o->bone[v * 3 + k] = (u8)(k == 1 ? back : neck);
				o->weight[v * 3 + k] = k == 0 ? 1.0f - mean : k == 1 ? mean : 0.0f;
			}
		}
	}

	free(share);
	free(sum);
	free(count);
}

/**
 * A gun's pickup (gunRows): rigid, every vertex on the matrix of the model's
 * first list node, laid onto GoldenEye's N64 pickup by the row's fit - Bean's
 * axis perm[k] times sign[k] becomes axis k, centred, scaled and moved to the
 * N64 pickup's centre, which is in the list node's own space. The fit's axes
 * are a mirror when the permutation and the signs are, so the triangles are
 * wound the other way then. The model's other list nodes draw nothing.
 */
static u8 *gebeanBuildRigid(s32 gun, s32 original, struct modeldef *modeldef, struct modelnode **nodes, s32 numnodes,
		struct gebeanmats *mats, u64 *outAbsent, u32 *outLen)
{
	const struct gebeangunrow *g = &gunRows[gun];
	char source[64];
	struct beanmodel bm;
	struct beanout out;
	u32 matwords[GEBEAN_MAXMATS];
	s32 nummatwords;
	s32 nummatrices = modeldef->nummatrices;
	s32 mtx = gebeanListNodeMatrix(nodes[0]);
	s32 mirror;
	u8 *file;

	// An odd permutation of three axes swaps two; each negative sign mirrors once
	mirror = (g->perm[0] == 0) + (g->perm[1] == 1) + (g->perm[2] == 2) == 1;

	for (s32 k = 0; k < 3; k++) {
		mirror ^= g->sign[k] < 0;
	}

	if (nummatrices <= 0 || nummatrices > GEBEAN_MAXMTX) {
		return NULL;
	}

	if (mtx < 0 || mtx >= nummatrices) {
		mtx = 0;
	}

	snprintf(source, sizeof(source), "%s/%s", original ? "original" : "new", g->row.source);

	if (!gebeanLocate(1) || !beanLoad(&bm, source)) {
		return NULL;
	}

	memset(&out, 0, sizeof(out));

	for (s32 di = 0; di < bm.numdraws; di++) {
		const struct beandraw *d = &bm.draws[di];
		struct beanvb vb;
		u16 *tris;
		s32 numtris;
		s32 *mapped;

		if (!beanReadVb(&bm, d->vb, &vb)) {
			continue;
		}

		numtris = beanTriangles(&bm, d, &tris);

		if (numtris <= 0) {
			free(tris);
			continue;
		}

		mapped = malloc(vb.count * sizeof(s32));

		if (!mapped) {
			free(tris);
			continue;
		}

		for (u32 i = 0; i < vb.count; i++) {
			mapped[i] = -1;
		}

		for (s32 t = 0; t < numtris; t++) {
			u16 idx[3];
			s32 ok = 1;

			for (s32 i = 0; i < 3 && ok; i++) {
				const u16 vi = tris[t * 3 + i];
				struct beanvtx v;
				f32 pos[3];
				f32 nrm[3];
				const u8 bone[3] = { (u8)mtx, (u8)mtx, (u8)mtx };
				const f32 weight[3] = { 1.0f, 0.0f, 0.0f };

				if (mapped[vi] >= 0) {
					idx[i] = (u16)mapped[vi];
					continue;
				}

				if (!beanVertex(&bm, &vb, vi, &v)) {
					ok = 0;
					break;
				}

				for (s32 k = 0; k < 3; k++) {
					const f32 p = g->sign[k] * v.pos[g->perm[k]];

					pos[k] = (p - g->beancentre[k]) * g->scale + g->n64centre[k];
					nrm[k] = g->sign[k] * v.nrm[g->perm[k]];
				}

				mapped[vi] = beanAddVertex(&out, pos, nrm, v.uv, bone, weight, v.argb);

				if (mapped[vi] < 0) {
					ok = 0;
					break;
				}

				idx[i] = (u16)mapped[vi];
			}

			if (!ok) {
				continue;
			}

			if (!beanAddTri(&out, 0, (s32)d->tex, idx[0], mirror ? idx[2] : idx[1], mirror ? idx[1] : idx[2])) {
				break;
			}
		}

		free(mapped);
		free(tris);
	}

	if (out.numverts > 0) {
		for (s32 k = 1; k < numnodes; k++) {
			beanAddTri(&out, k, 0, 0, 0, 0);
		}
	}

	nummatwords = bm.numtex + 1 < GEBEAN_MAXMATS ? bm.numtex + 1 : GEBEAN_MAXMATS;
	memset(mats, 0, sizeof(*mats));
	mats->num = nummatwords;

	for (s32 i = 0; i < nummatwords; i++) {
		matwords[i] = XBLAMESH_MAT_TABLE | (u32)i;
	}

	for (s32 i = 0; i < bm.numtex && i < nummatwords; i++) {
		s32 used = 0;

		for (s32 t = 0; t < out.numtris; t++) {
			if (out.tris[t].tex == i) {
				used = 1;
				break;
			}
		}

		if (used && beanBindTexture(&bm, source, i, &mats->tile[i], &mats->alpha[i], &mats->soft[i])
				&& mats->alpha[i]) {
			matwords[i] |= 0x8000;
		}
	}

	for (s32 t = 0; t < out.numtris; t++) {
		if (out.tris[t].tex >= bm.numtex) {
			out.tris[t].tex = (u16)(nummatwords - 1);
		}
	}

	file = beanWriteMesh(&out, numnodes, nummatrices, NULL, matwords, nummatwords, outAbsent, outLen);

	sysLogPrintf(LOG_NOTE, "gebean: %s <- %s: %d vertices, %d triangles, rigid on matrix %d of %d%s%s",
			g->row.file, source, out.numverts, out.numtris, mtx, nummatrices,
			mirror ? ", mirrored" : "", file ? "" : " - did not write");

	beanOutFree(&out);
	beanFree(&bm);

	return file;
}

/** A texture's width and height from its header, without decoding it. */
static s32 beanTextureSize(const struct beanmodel *bm, s32 t, s32 *w, s32 *h)
{
	u32 blen = 0;
	const u8 *b = t >= 0 && t < bm->numtex ? caffBlob(&bm->caff, bm->texfile[t], &blen) : NULL;

	if (!b || blen < 0x40) {
		return 0;
	}

	*w = gebeanBE16(b + 0x24);
	*h = gebeanBE16(b + 0x26);

	return 1;
}

/**
 * A tinted gun's vertex colour, lit by its normal from above and in front of
 * the eye. The shine Bean's shader gets from its reflection map has no pass
 * here, and a flat tint drew the Golden Gun as matte paint; a first-person
 * gun hardly turns against the view, so light baked into the colour holds.
 * Bean's axes are the host's: y up, the barrel along z, the eye towards -z.
 */
static u32 beanShadeTint(u32 argb, const f32 *nrm)
{
	const f32 light[3] = { 0.0f, 0.75f, -0.66f };
	const f32 len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
	f32 d = len > 0.0f ? (nrm[0] * light[0] + nrm[1] * light[1] + nrm[2] * light[2]) / len : 0.0f;
	f32 shade;
	f32 d2;
	u32 out = argb & 0xff000000;

	d = d < 0.0f ? 0.0f : d;
	d2 = d * d;
	shade = 0.5f + 0.45f * d + 0.4f * d2 * d2 * d2 * d2;

	for (s32 shift = 16; shift >= 0; shift -= 8) {
		f32 c = ((argb >> shift) & 0xff) * shade;

		out |= (u32)(c > 255.0f ? 255.0f : c) << shift;
	}

	return out;
}

/**
 * A first-person gun's pictures to leave out, into hand (the count is
 * returned), and the extent of what is left. Out with the hand go Bean's
 * muzzle flashes, the 32x32 sprites on the muzzle bones, which it switches on
 * itself - here they would draw always (the host's toggled flash still
 * fires), and they would stretch the gun's length the fit is measured by.
 */
/**
 * The points of a gun, kept so a placement can be settled against the host's
 * own vertices rather than against the middle of its box.
 */
struct fpcloud {
	f32 *pos;
	s32 num;
	s32 cap;
};

static void fpCloudAdd(struct fpcloud *c, const f32 *p)
{
	if (!c) {
		return;
	}

	if (c->num >= c->cap) {
		const s32 cap = c->cap ? c->cap * 2 : 256;
		f32 *grown = realloc(c->pos, (size_t)cap * 3 * sizeof(f32));

		if (!grown) {
			return;
		}

		c->pos = grown;
		c->cap = cap;
	}

	memcpy(&c->pos[c->num * 3], p, 3 * sizeof(f32));
	c->num++;
}

static void fpCloudFree(struct fpcloud *c)
{
	free(c->pos);
	c->pos = NULL;
	c->num = c->cap = 0;
}

static s32 beanGunExtent(struct beanmodel *bm, u8 *hand, f32 lo[3], f32 hi[3], struct fpcloud *cloud)
{
	s32 numhand = 0;

	for (s32 t = 0; t < bm->numtex && t < GEBEAN_MAXMATS; t++) {
		s32 w = 0;
		s32 h = 0;

		hand[t] = beanTextureSize(bm, t, &w, &h) && ((w == 512 && h == 511) || (w <= 64 && h <= 64));
		numhand += hand[t];
	}

	for (s32 di = 0; di < bm->numdraws; di++) {
		const struct beandraw *d = &bm->draws[di];
		struct beanvb vb;
		u16 *tris;
		s32 numtris;

		if ((d->tex < GEBEAN_MAXMATS && d->tex < (u32)bm->numtex && hand[d->tex]) || !beanReadVb(bm, d->vb, &vb)) {
			continue;
		}

		numtris = beanTriangles(bm, d, &tris);

		for (s32 t = 0; t < numtris * 3; t++) {
			struct beanvtx v;

			if (beanVertex(bm, &vb, tris[t], &v)) {
				for (s32 a = 0; a < 3; a++) {
					if (v.pos[a] < lo[a]) lo[a] = v.pos[a];
					if (v.pos[a] > hi[a]) hi[a] = v.pos[a];
				}

				fpCloudAdd(cloud, v.pos);
			}
		}

		free(tris);
	}

	return numhand;
}

/** Bean's point p in the host model's space, under a fit. */
static void fpPlace(const f32 *p, const s8 *axis, const f32 *beanc, f32 scale, const f32 *hostc, f32 *out)
{
	f32 rel[3];
	f32 turned[3];

	for (s32 a = 0; a < 3; a++) {
		rel[a] = p[a] - beanc[a];
	}

	beanAxisMap(axis, rel, turned);

	for (s32 a = 0; a < 3; a++) {
		out[a] = turned[a] * scale + hostc[a];
	}
}

static int fpCompareF32(const void *a, const void *b)
{
	const f32 x = *(const f32 *)a;
	const f32 y = *(const f32 *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

/**
 * Settle a gun's place on the host's own vertices rather than on the middle of
 * its box.
 *
 * Perfect Dark's conversions of GoldenEye's guns *are* GoldenEye's guns - the
 * PP9i is the PP7, the RC-P45 the RC-P90, the DMC the D5K - so where the host
 * model's metal is, the release's gun's metal belongs, and there is an answer
 * rather than a centring. Centring is only the shape's middle, and for the PP7
 * that left the grip a few units out of the hand it is drawn closing on: "pp7s
 * are slightly off on hand position, finger clipping through".
 *
 * So the box fit is the start and this walks it in: each of the host's
 * vertices takes the nearest of Bean's, and the translation moves by the mean
 * of the closest `FP_ICP_KEEP` of those offsets. Trimmed, because the two
 * models are the same gun and not the same mesh - a host part the release
 * modelled differently, or left off, would otherwise drag the gun towards it -
 * and one-way from the host, because Bean's cloud is the dense one and every
 * one of the host's vertices is a place the gun really is.
 *
 * The scale is left alone: it is the host's own barrel length and the fault
 * being fixed is a placement.
 */
#define FP_ICP_ROUNDS 8
#define FP_ICP_KEEP   0.7f
#define FP_ICP_POINTS 800

static void fpRefinePlacement(const struct fpcloud *bean, const struct fpcloud *host,
		const s8 *axis, const f32 *beanc, f32 scale, f32 *hostc)
{
	const f32 zero[3] = { 0.0f, 0.0f, 0.0f };
	f32 *base;
	f32 *dist;
	f32 *delta;
	f32 *sorted;
	s32 stride;
	s32 num = 0;
	s32 keep;

	if (bean->num < 8 || host->num < 8) {
		return;
	}

	// Only the translation moves between rounds, so each of Bean's points is
	// placed once with none of it and the rounds are arithmetic. A gun can
	// carry a few thousand and every one of them would otherwise be measured
	// against every one of the host's, eight times over, at a model load.
	stride = bean->num / FP_ICP_POINTS + 1;
	base = malloc((size_t)(bean->num / stride + 1) * 3 * sizeof(f32));
	dist = malloc((size_t)host->num * sizeof(f32));
	delta = malloc((size_t)host->num * 3 * sizeof(f32));
	sorted = malloc((size_t)host->num * sizeof(f32));

	if (!base || !dist || !delta || !sorted) {
		free(base);
		free(dist);
		free(delta);
		free(sorted);
		return;
	}

	for (s32 b = 0; b < bean->num; b += stride) {
		fpPlace(&bean->pos[b * 3], axis, beanc, scale, zero, &base[num * 3]);
		num++;
	}

	keep = (s32)(host->num * FP_ICP_KEEP);

	if (keep < 8) {
		keep = 8;
	}

	for (s32 round = 0; round < FP_ICP_ROUNDS; round++) {
		f32 cut;
		f32 move[3] = { 0.0f, 0.0f, 0.0f };
		s32 taken = 0;

		for (s32 h = 0; h < host->num; h++) {
			const f32 *q = &host->pos[h * 3];
			f32 best = 1e30f;

			for (s32 b = 0; b < num; b++) {
				f32 d[3];
				f32 d2;

				for (s32 a = 0; a < 3; a++) {
					d[a] = q[a] - (base[b * 3 + a] + hostc[a]);
				}

				d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];

				if (d2 < best) {
					best = d2;
					memcpy(&delta[h * 3], d, sizeof(d));
				}
			}

			dist[h] = best;
		}

		memcpy(sorted, dist, (size_t)host->num * sizeof(f32));
		qsort(sorted, host->num, sizeof(f32), fpCompareF32);
		cut = sorted[keep - 1];

		for (s32 h = 0; h < host->num; h++) {
			if (dist[h] <= cut) {
				for (s32 a = 0; a < 3; a++) {
					move[a] += delta[h * 3 + a];
				}

				taken++;
			}
		}

		if (!taken) {
			break;
		}

		for (s32 a = 0; a < 3; a++) {
			hostc[a] += move[a] / taken;
		}
	}

	free(base);
	free(dist);
	free(delta);
	free(sorted);
}

/**
 * The point of a gun its shot comes out of: the middle of whatever is at the
 * far end of the barrel.
 *
 * The barrel is z in both games, and which way along it is forward is read off
 * the host's own muzzle node rather than assumed - it is the end of the host's
 * box that node sits at. Everything within FP_MUZZLE_BAND of the gun's far end
 * is averaged, which on a barrel is its bore and on a knife its point.
 */
#define FP_MUZZLE_BAND 0.03f

static s32 fpMuzzlePoint(const struct fpcloud *bean, const s8 *axis, const f32 *beanc, f32 scale,
		const f32 *hostc, f32 forward, f32 *out)
{
	f32 far = -1e30f;
	f32 sum[3] = { 0.0f, 0.0f, 0.0f };
	f32 lo = 1e30f;
	f32 hi = -1e30f;
	f32 cut;
	s32 taken = 0;

	if (bean->num < 8) {
		return 0;
	}

	for (s32 b = 0; b < bean->num; b++) {
		f32 placed[3];
		f32 along;

		fpPlace(&bean->pos[b * 3], axis, beanc, scale, hostc, placed);
		along = placed[2] * forward;

		if (along > far) far = along;
		if (along < lo) lo = along;
		if (along > hi) hi = along;
	}

	if (hi - lo <= 1.0f) {
		return 0;
	}

	cut = far - (hi - lo) * FP_MUZZLE_BAND;

	for (s32 b = 0; b < bean->num; b++) {
		f32 placed[3];

		fpPlace(&bean->pos[b * 3], axis, beanc, scale, hostc, placed);

		if (placed[2] * forward >= cut) {
			for (s32 a = 0; a < 3; a++) {
				sum[a] += placed[a];
			}

			taken++;
		}
	}

	if (!taken) {
		return 0;
	}

	for (s32 a = 0; a < 3; a++) {
		out[a] = sum[a] / taken;
	}

	return 1;
}

/**
 * A first-person gun (fpRows): Bean's gun, which is GoldenEye's N64 gun at
 * Bean's scale with the same joints, on the host's first-person model.
 *
 * The hand goes: it is the one 512x511 picture in every gun that has one
 * (the pistols and knives), and Perfect Dark draws its own hand model with
 * the gun's matrices. What is left is laid onto the host's visible lists -
 * scaled to the same length along the barrel (z, as in both games) and
 * centred on them - and each of Bean's bones goes on the host matrix whose
 * rest is nearest its joint (a slide on the slide), or on the body's. Every
 * vertex is then in the model's space with its matrix's rest taken off by
 * the palette, as a character's is. A toggled list (a muzzle flash) keeps its
 * own geometry; any other list Bean's gun does not reach draws nothing.
 */
static u8 *gebeanBuildFirstPerson(s32 fp, s32 original, struct modeldef *modeldef, struct modelnode **nodes,
		s32 numnodes, struct gebeanmats *mats, u64 *outAbsent, u32 *outLen)
{
	const struct gebeanrow *r = &fpRows[fp];
	const s32 nummatrices = modeldef->nummatrices;
	char source[64];
	struct beanmodel bm;
	struct beanout out;
	struct beanrig rig;
	u32 matwords[GEBEAN_MAXMATS];
	s32 nummatwords;
	s32 nodemtx[64];
	u8 nodevisible[64];
	u8 nodeused[64];
	s32 mtxnode[GEBEAN_MAXMTX];
	s32 bodynode = -1;
	s32 bodyverts = 0;
	u8 hand[GEBEAN_MAXMATS];
	f32 hostlo[3] = { 1e30f, 1e30f, 1e30f };
	f32 hosthi[3] = { -1e30f, -1e30f, -1e30f };
	f32 beanlo[3] = { 1e30f, 1e30f, 1e30f };
	f32 beanhi[3] = { -1e30f, -1e30f, -1e30f };
	f32 hostc[3];
	f32 beanc[3];
	f32 scale;
	struct fpcloud owncloud;   // the gun's own points, which are the ones drawn
	struct fpcloud fitcloud;   // the points the placement is measured on
	struct fpcloud hostcloud;  // the host's visible lists, in the model's space
	s32 bonemtx[BEAN_MAXBONES];
	s32 numhand = 0;
	const s8 *fpaxis = NULL;
	u8 *file;

	// Bean's N64-look guns have the hand in their geometry; the host draws there
	if (original || nummatrices <= 0 || nummatrices > GEBEAN_MAXMTX) {
		return NULL;
	}

	memset(&rig, 0, sizeof(rig));
	memset(nodeused, 0, sizeof(nodeused));
	memset(&owncloud, 0, sizeof(owncloud));
	memset(&fitcloud, 0, sizeof(fitcloud));
	memset(&hostcloud, 0, sizeof(hostcloud));
	fpMuzzleSet[fp] = 0;

	for (s32 m = 0; m < GEBEAN_MAXMTX; m++) {
		mtxnode[m] = -1;
	}

	// Every matrix's rest, from the position nodes that own them
	{
		s32 walked = 0;

		for (struct modelnode *node = modeldef->rootnode; node && walked < 4096; node = gebeanNextNode(node), walked++) {
			const u32 type = node->type & 0xff;
			s32 mtx = -1;

			if (type == MODELNODETYPE_POSITION) {
				mtx = node->rodata->position.mtxindex0;
			} else if (type == MODELNODETYPE_POSITIONHELD) {
				mtx = node->rodata->positionheld.mtxindex;
			}

			if (mtx >= 0 && mtx < GEBEAN_MAXMTX && !rig.hasrest[mtx]) {
				rig.hasrest[mtx] = 1;
				xblaMeshNodeRestOffset(node, rig.rest[mtx]);
			}
		}
	}

	// The host's visible lists: their extent in the model's space, and the
	// biggest of them, which is the body. A list is drawn under the matrices
	// its own display list loads, not the position node's above it, so that
	// is the matrix each list's group goes under and each vertex is placed by
	// the rest of the one loaded before it. The PP9i's gun list hangs under
	// the root and loads matrices 33 and 34, 69 units across and 61 along
	// from the root's: fitted from the root, the PP7 drew that far to the
	// side of the gun it stands in for, below the hand.
	// A second pass takes the toggled lists too, for a host that has nothing
	// else: the knives, the grenade and the remote mine are one list apiece
	// under a toggle (the game switches the held one against the thrown one),
	// and skipping those - the rule that leaves a muzzle flash alone - left
	// them with no geometry to fit at all.
	for (s32 pass = 0; pass < 2 && bodynode < 0; pass++)
	for (s32 k = 0; k < numnodes; k++) {
		const u32 type = nodes[k]->type & 0xff;
		const s32 loaded = gebeanListLoadedMatrix(nodes[k]);
		const Vtx *v = NULL;
		s32 n = 0;
		f32 rest[3];
		s16 *vtxmtx;

		nodemtx[k] = loaded >= 0 ? loaded : gebeanListNodeMatrix(nodes[k]);

		if (nodemtx[k] < 0 || nodemtx[k] >= nummatrices) {
			nodemtx[k] = 0;
		}

		nodevisible[k] = pass || !beanNodeIsToggled(nodes[k]);

		if (!nodevisible[k]) {
			continue;
		}

		if (type == MODELNODETYPE_DL) {
			v = nodes[k]->rodata->dl.vertices;
			n = nodes[k]->rodata->dl.numvertices;
		} else if (type == MODELNODETYPE_GUNDL) {
			v = nodes[k]->rodata->gundl.vertices;
			n = nodes[k]->rodata->gundl.numvertices;
		}

		if (!v || n <= 0) {
			continue;
		}

		if (mtxnode[nodemtx[k]] < 0) {
			mtxnode[nodemtx[k]] = k;
		}

		xblaMeshNodeRestOffset(nodes[k], rest);

		// A list that loads no matrix is drawn from its own rest - the sum of
		// the positions above it - which is what its matrix carries; the first
		// position node naming that matrix need not be that one. A matrix the
		// list loads has its own position node's rest.
		if (loaded < 0 || !rig.hasrest[nodemtx[k]]) {
			rig.hasrest[nodemtx[k]] = 1;
			memcpy(rig.rest[nodemtx[k]], rest, sizeof(rest));
		}

		vtxmtx = malloc((size_t)n * sizeof(*vtxmtx));

		if (vtxmtx) {
			beanListMatrices(nodes[k], vtxmtx, n);
		}

		for (s32 j = 0; j < n; j++) {
			const s32 mtx = vtxmtx && vtxmtx[j] >= 0 && vtxmtx[j] < nummatrices && rig.hasrest[vtxmtx[j]]
				? vtxmtx[j] : nodemtx[k];

			f32 p[3];

			for (s32 a = 0; a < 3; a++) {
				p[a] = v[j].v[a] + rig.rest[mtx][a];

				if (p[a] < hostlo[a]) hostlo[a] = p[a];
				if (p[a] > hosthi[a]) hosthi[a] = p[a];
			}

			if (!pass) {
				fpCloudAdd(&hostcloud, p);
			}
		}

		free(vtxmtx);

		if (n > bodyverts) {
			bodyverts = n;
			bodynode = k;
		}
	}

	if (bodynode < 0) {
		fpCloudFree(&hostcloud);
		return NULL;
	}

	snprintf(source, sizeof(source), "new/%s", r->source);

	if (!gebeanLocate(1) || !beanLoad(&bm, source)) {
		fpCloudFree(&hostcloud);
		return NULL;
	}

	numhand = beanGunExtent(&bm, hand, beanlo, beanhi, &owncloud);

	// A silenced gun is measured on its plain twin, which shares its place
	if (fpFitSource[fp]) {
		struct beanmodel twin;
		char twinsource[64];
		u8 twinhand[GEBEAN_MAXMATS];
		f32 twinlo[3] = { 1e30f, 1e30f, 1e30f };
		f32 twinhi[3] = { -1e30f, -1e30f, -1e30f };

		snprintf(twinsource, sizeof(twinsource), "new/%s", fpFitSource[fp]);

		if (beanLoad(&twin, twinsource)) {
			beanGunExtent(&twin, twinhand, twinlo, twinhi, &fitcloud);

			if (twinhi[2] - twinlo[2] > 1.0f) {
				memcpy(beanlo, twinlo, sizeof(twinlo));
				memcpy(beanhi, twinhi, sizeof(twinhi));
			} else {
				fpCloudFree(&fitcloud);
			}

			beanFree(&twin);
		}
	}

	if (beanhi[2] - beanlo[2] <= 1.0f || hosthi[2] - hostlo[2] <= 1.0f) {
		fpCloudFree(&owncloud);
		fpCloudFree(&fitcloud);
		fpCloudFree(&hostcloud);
		beanFree(&bm);
		return NULL;
	}

	scale = (hosthi[2] - hostlo[2]) / (beanhi[2] - beanlo[2]);

	for (s32 a = 0; a < 3; a++) {
		hostc[a] = (hostlo[a] + hosthi[a]) * 0.5f;
		beanc[a] = (beanlo[a] + beanhi[a]) * 0.5f;
	}

	fpaxis = fpGrip[fp].axis[0] || fpGrip[fp].axis[1] || fpGrip[fp].axis[2] ? fpGrip[fp].axis : NULL;

	// A gun placed by its grip: that point of Bean's gun onto the hand's
	if (fpGrip[fp].set && FP_PALM_MTX < nummatrices && rig.hasrest[FP_PALM_MTX]) {
		if (fpGrip[fp].scale > 0.0f) {
			scale = fpGrip[fp].scale;
		}

		for (s32 a = 0; a < 3; a++) {
			beanc[a] = fpGrip[fp].pos[a];
			hostc[a] = rig.rest[FP_PALM_MTX][a] + fpGripFromPalm[a];
		}
	}

	// The host is the same gun, so walk the placement onto its own vertices.
	// Not for a gun placed by its grip: those are the ones whose host is a
	// different shape or half their length, which is why they are placed that
	// way, and the nearest vertex has nothing to say about them.
	if (!fpGrip[fp].set) {
		fpRefinePlacement(fitcloud.num ? &fitcloud : &owncloud, &hostcloud,
				fpaxis, beanc, scale, hostc);
	}

	// Where the gun that is drawn ends, so the shot comes out of it rather
	// than out of the host's own muzzle node (bondgun.c). Forward along the
	// barrel is whichever end of the host's box its muzzle node sits at.
	{
		// The node the offset is measured from. Perfect Dark's own conversions
		// of GoldenEye's submachine guns and rifles - the KL01313, the KF7
		// Special, the DMC, the AR53, the RC-P45 - carry no
		// MODELPART_GUN_MUZZLEPOS at all, and without one bondgun.c starts the
		// stream at the gun's origin, a barrel's length behind the muzzle.
		// They do carry a muzzle flash, which is at the muzzle by definition,
		// so that is the fallback.
		static const s32 parts[] = {
			MODELPART_GUN_MUZZLEPOS,
			MODELPART_GUN_MUZZLEFLASH1,
			MODELPART_GUN_MUZZLEFLASH2,
			MODELPART_GUN_MUZZLEFLASH3,
		};
		struct modelnode *muzzle = NULL;
		s32 muzzlemtx = -1;
		s32 part = -1;

		for (s32 k = 0; k < (s32)ARRAYCOUNT(parts) && muzzlemtx < 0; k++) {
			muzzle = modelGetPart(modeldef, parts[k]);
			muzzlemtx = muzzle ? modelFindNodeMtxIndex(muzzle, 0) : -1;

			if (muzzlemtx >= 0 && muzzlemtx < GEBEAN_MAXMTX && rig.hasrest[muzzlemtx]) {
				part = parts[k];
			} else {
				muzzlemtx = -1;
			}
		}

		fpMuzzlePart[fp] = (s16)part;

		if (part >= 0) {
			// Forward along the barrel is +z, in both games and in every
			// first-person model here: the host's own muzzle node sits at the
			// top of its box in z in the PP7's, the Phantom's, the sniper
			// rifle's, the Cougar's and the Laser's, to the tenth of a unit.
			// Which end the node is at is *not* the test - the rocket
			// launcher's sits at 206 in a box running -59 to 500, and reading
			// that as "the node is at the back" took the muzzle to be the
			// shoulder end and put its rocket 744 units behind the tube.
			const f32 forward = 1.0f;
			f32 point[3];

			if (xblaMeshIsVerbose()) {
				sysLogPrintf(LOG_NOTE, "gebean: %s muzzle: part 0x%02x rest (%.1f %.1f %.1f), host box z %.1f..%.1f",
						r->file, part, rig.rest[muzzlemtx][0], rig.rest[muzzlemtx][1], rig.rest[muzzlemtx][2],
						hostlo[2], hosthi[2]);
			}

			if (fpMuzzlePoint(&owncloud, fpaxis, beanc, scale, hostc, forward, point)) {
				for (s32 a = 0; a < 3; a++) {
					fpMuzzle[fp][a] = point[a] - rig.rest[muzzlemtx][a];
				}

				fpMuzzleSet[fp] = 1;
			}
		}
	}

	// Each bone onto the nearest host matrix with a visible list, within 30
	// units of its joint, or onto the body's
	for (s32 b = 0; b < BEAN_MAXBONES; b++) {
		f32 best = 30.0f * 30.0f;

		bonemtx[b] = nodemtx[bodynode];

		if (b == 0 || b >= bm.numbones) {
			continue;
		}

		for (s32 m = 0; m < nummatrices; m++) {
			f32 d2 = 0.0f;

			if (mtxnode[m] < 0 || !rig.hasrest[m]) {
				continue;
			}

			{
				f32 rel[3], turned[3];

				for (s32 a = 0; a < 3; a++) {
					rel[a] = bm.bind[b][a] - beanc[a];
				}

				beanAxisMap(fpaxis, rel, turned);

				for (s32 a = 0; a < 3; a++) {
					const f32 joint = turned[a] * scale + hostc[a];

					d2 += (joint - rig.rest[m][a]) * (joint - rig.rest[m][a]);
				}
			}

			if (d2 < best) {
				best = d2;
				bonemtx[b] = m;
			}
		}

	}

	memset(&out, 0, sizeof(out));

	for (s32 di = 0; di < bm.numdraws; di++) {
		const struct beandraw *d = &bm.draws[di];
		struct beanvb vb;
		u16 *tris;
		s32 numtris;
		s32 *mapped;
		s32 *mappedmtx;

		if ((d->tex < GEBEAN_MAXMATS && d->tex < (u32)bm.numtex && hand[d->tex]) || !beanReadVb(&bm, d->vb, &vb)) {
			continue;
		}

		numtris = beanTriangles(&bm, d, &tris);

		if (numtris <= 0) {
			free(tris);
			continue;
		}

		mapped = malloc(vb.count * sizeof(s32));
		mappedmtx = malloc(vb.count * sizeof(s32));

		if (!mapped || !mappedmtx) {
			free(mapped);
			free(mappedmtx);
			free(tris);
			continue;
		}

		for (u32 i = 0; i < vb.count; i++) {
			mapped[i] = -1;
		}

		for (s32 t = 0; t < numtris; t++) {
			u16 idx[3];
			s32 group;
			s32 ok = 1;

			for (s32 i = 0; i < 3 && ok; i++) {
				const u16 vi = tris[t * 3 + i];
				struct beanvtx v;
				f32 pos[3];
				s32 bone = 0;
				s32 mtx;
				u8 bones[3];
				const f32 weight[3] = { 1.0f, 0.0f, 0.0f };

				if (mapped[vi] >= 0) {
					idx[i] = (u16)mapped[vi];
					continue;
				}

				if (!beanVertex(&bm, &vb, vi, &v)) {
					ok = 0;
					break;
				}

				if (v.slot[0] >= 0 && v.slot[0] < d->numpal) {
					bone = d->pal[(s32)v.slot[0]];
					bone = bm.numremap && bone < bm.numremap ? bm.remap[bone] : bone;
				}

				mtx = bone >= 0 && bone < BEAN_MAXBONES ? bonemtx[bone] : nodemtx[bodynode];
				bones[0] = bones[1] = bones[2] = (u8)mtx;

				// In the space of the list's matrix, the way the host's own
				// vertices are: the model's space less the matrix's rest
				{
					f32 rel[3], turned[3], nrm[3];

					for (s32 a = 0; a < 3; a++) {
						rel[a] = v.pos[a] - beanc[a];
					}

					beanAxisMap(fpaxis, rel, turned);
					beanAxisMap(fpaxis, v.nrm, nrm);
					memcpy(v.nrm, nrm, sizeof(nrm));

					for (s32 a = 0; a < 3; a++) {
						pos[a] = turned[a] * scale + hostc[a]
							- (rig.hasrest[mtx] ? rig.rest[mtx][a] : 0.0f);
					}
				}

				mapped[vi] = beanAddVertex(&out, pos, v.nrm, v.uv, bones, weight,
						fpTint[fp] ? beanShadeTint(fpTint[fp], v.nrm) : v.argb);
				mappedmtx[vi] = mtx;

				if (mapped[vi] < 0) {
					ok = 0;
					break;
				}

				idx[i] = (u16)mapped[vi];
			}

			if (!ok) {
				continue;
			}

			group = mtxnode[mappedmtx[tris[t * 3]]];

			if (group < 0) {
				group = bodynode;
			}

			nodeused[group] = 1;

			if (!beanAddTri(&out, group, (s32)d->tex, idx[0], idx[1], idx[2])) {
				break;
			}
		}

		free(mapped);
		free(mappedmtx);
		free(tris);
	}

	// Cover every list of the host's that Bean's gun did not take, so the
	// host's own gun cannot draw beside it: the Laser's red element (its
	// LASERLIQUID part, a toggle) drew through the Moonraker's left arm. The
	// muzzle flashes are the exception and are left to flash, since Bean's
	// own flash pictures are dropped with the hand.
	if (out.numverts > 0) {
		struct modelnode *flash[3];

		flash[0] = modelGetPart(modeldef, MODELPART_GUN_MUZZLEFLASH1);
		flash[1] = modelGetPart(modeldef, MODELPART_GUN_MUZZLEFLASH2);
		flash[2] = modelGetPart(modeldef, MODELPART_GUN_MUZZLEFLASH3);

		for (s32 k = 0; k < numnodes; k++) {
			if (nodeused[k] || nodes[k] == flash[0] || nodes[k] == flash[1] || nodes[k] == flash[2]) {
				continue;
			}

			beanAddTri(&out, k, 0, 0, 0, 0);
		}
	}

	nummatwords = bm.numtex + 1 < GEBEAN_MAXMATS ? bm.numtex + 1 : GEBEAN_MAXMATS;
	memset(mats, 0, sizeof(*mats));
	mats->num = nummatwords;

	for (s32 i = 0; i < nummatwords; i++) {
		matwords[i] = XBLAMESH_MAT_TABLE | (u32)i;
	}

	for (s32 i = 0; i < bm.numtex && i < nummatwords; i++) {
		s32 used = 0;

		for (s32 t = 0; t < out.numtris; t++) {
			if (out.tris[t].tex == i) {
				used = 1;
				break;
			}
		}

		if (used && beanBindTexture(&bm, source, i, &mats->tile[i], &mats->alpha[i], &mats->soft[i])
				&& mats->alpha[i]) {
			matwords[i] |= 0x8000;
		}
	}

	for (s32 t = 0; t < out.numtris; t++) {
		if (out.tris[t].tex >= bm.numtex) {
			out.tris[t].tex = (u16)(nummatwords - 1);
		}
	}

	// Each group's vertices are in its own list's space, so the mesh has no
	// palette: xblamesh.c draws it like a model pack's, under each node's own
	// matrix (gebeanRowIsFirstPerson())
	file = beanWriteMesh(&out, numnodes, 0, NULL, matwords, nummatwords, outAbsent, outLen);

	sysLogPrintf(LOG_NOTE, "gebean: %s <- %s: %d vertices, %d triangles, %d hand and flash pictures left out, "
			"scale %.4f%s%s, body list %d on matrix %d of %d %s",
			r->file, source, out.numverts, out.numtris, numhand, scale,
			fpFitSource[fp] ? " measured on " : "", fpFitSource[fp] ? fpFitSource[fp] : "",
			bodynode, nodemtx[bodynode], nummatrices, file ? "" : " - did not write");

	// Where a gun was put, which is what a placement is judged from: the point
	// of Bean's gun that was laid on the host, and where on the host that is
	if (xblaMeshIsVerbose()) {
		f32 outlo[3] = { 1e30f, 1e30f, 1e30f };
		f32 outhi[3] = { -1e30f, -1e30f, -1e30f };

		for (s32 i = 0; i < out.numverts; i++) {
			for (s32 a = 0; a < 3; a++) {
				const f32 p = out.pos[i * 3 + a];

				if (p < outlo[a]) outlo[a] = p;
				if (p > outhi[a]) outhi[a] = p;
			}
		}

		sysLogPrintf(LOG_NOTE, "gebean: %s place: bean (%.1f %.1f %.1f) -> host (%.1f %.1f %.1f), palm rest (%.1f %.1f %.1f), "
				"host box (%.1f %.1f %.1f)..(%.1f %.1f %.1f), bean box (%.1f %.1f %.1f)..(%.1f %.1f %.1f), "
				"drawn (%.1f %.1f %.1f)..(%.1f %.1f %.1f)",
				r->file, beanc[0], beanc[1], beanc[2], hostc[0], hostc[1], hostc[2],
				rig.rest[FP_PALM_MTX][0], rig.rest[FP_PALM_MTX][1], rig.rest[FP_PALM_MTX][2],
				hostlo[0], hostlo[1], hostlo[2], hosthi[0], hosthi[1], hosthi[2],
				beanlo[0], beanlo[1], beanlo[2], beanhi[0], beanhi[1], beanhi[2],
				outlo[0], outlo[1], outlo[2], outhi[0], outhi[1], outhi[2]);

		for (s32 b = 0; b < bm.numbones && b < 8; b++) {
			sysLogPrintf(LOG_NOTE, "gebean:   bone %d -> matrix %d, list %d",
					b, bonemtx[b], mtxnode[bonemtx[b]]);
		}

		if (fpMuzzleSet[fp]) {
			sysLogPrintf(LOG_NOTE, "gebean:   muzzle offset (%.1f %.1f %.1f) from the host's node",
					fpMuzzle[fp][0], fpMuzzle[fp][1], fpMuzzle[fp][2]);
		} else {
			sysLogPrintf(LOG_NOTE, "gebean:   no muzzle of its own; the host's node stands");
		}
	}

	fpCloudFree(&owncloud);
	fpCloudFree(&fitcloud);
	fpCloudFree(&hostcloud);
	beanOutFree(&out);
	beanFree(&bm);

	return file;
}

/**
 * Where the gun drawn for this weapon ends, as an offset from its host's
 * muzzle node in the model's own space; 0 if the host's own model is the one
 * in the hand and its own node is right.
 */
s32 gebeanFirstPersonMuzzleOffset(s32 weaponnum, s32 *outpart, f32 *out)
{
	const s32 i = weaponnum - WEAPON_GE_FIRST;

	if (i < 0 || i >= (s32)ARRAYCOUNT(fpRows) || !fpSlot[i] || !fpMuzzleSet[i]) {
		return 0;
	}

	*outpart = fpMuzzlePart[i];
	memcpy(out, fpMuzzle[i], 3 * sizeof(f32));

	return 1;
}

u8 *gebeanBuild(s32 row, s32 original, struct modeldef *modeldef, struct modelnode **nodes, s32 numnodes,
		struct gebeanmats *mats, u64 *outAbsent, u32 *outLen)
{
	{
		const s32 fp = row - ARRAYCOUNT(rows) - ARRAYCOUNT(poolRows) - ARRAYCOUNT(gunRows);

		if (fp >= 0 && fp < ARRAYCOUNT(fpRows)) {
			*outLen = 0;
			*outAbsent = 0;

			return modeldef && numnodes > 0 && numnodes <= 64
				? gebeanBuildFirstPerson(fp, original, modeldef, nodes, numnodes, mats, outAbsent, outLen) : NULL;
		}
	}

	{
		const s32 gun = row - ARRAYCOUNT(rows) - ARRAYCOUNT(poolRows);

		if (gun >= 0 && gun < ARRAYCOUNT(gunRows)) {
			*outLen = 0;
			*outAbsent = 0;

			return modeldef && numnodes > 0 && numnodes <= 64
				? gebeanBuildRigid(gun, original, modeldef, nodes, numnodes, mats, outAbsent, outLen) : NULL;
		}
	}

	const struct gebeanrow *r;
	char source[64];
	struct beanmodel bm;
	struct beanrig rig;
	struct beanout out;
	struct modelnode *joints[64];
	s8 jointskel[64];
	s8 nodeskel[64];
	s32 numjoints = 0;
	f32 bind[SK_COUNT][3];
	s32 havebind[SK_COUNT];
	f32 headrot[3][3];
	f32 headscale = GEBEAN_HEAD_SCALE;
	u32 matwords[GEBEAN_MAXMATS];
	s32 nummatwords;
	s32 ishead;
	s32 fromchar;
	s32 dropped = 0;
	u8 *file = NULL;
	s32 nummatrices;

	*outLen = 0;
	*outAbsent = 0;

	r = gebeanRowAt(row);

	if (!r || !modeldef || numnodes <= 0 || numnodes > 64) {
		return NULL;
	}

	ishead = r->kind == GEBEAN_HEAD;
	fromchar = strncmp(r->source, "char/", 5) == 0;

	// Also what the pictures are keyed on, so the two looks never share one
	snprintf(source, sizeof(source), "%s/%s", original ? "original" : "new", r->source);

	if (!gebeanLocate(1) || !beanLoad(&bm, source)) {
		return NULL;
	}

	if (!bm.numbones || !bm.numdraws) {
		sysLogPrintf(LOG_WARNING, "gebean: %s has no skeleton or no draws", source);
		beanFree(&bm);
		return NULL;
	}

	memset(havebind, 0, sizeof(havebind));
	memset(bind, 0, sizeof(bind));

	for (s32 b = 0; b < bm.numbones; b++) {
		if (bm.skel[b] >= 0) {
			memcpy(bind[(s32)bm.skel[b]], bm.bind[b], sizeof(bind[0]));
			havebind[(s32)bm.skel[b]] = 1;
		}
	}

	// Four of the originals' heads (Karl, Martin, Duncan, Dwayne) carry a pose
	// of zeros while their vertices stand where every other head's do, which
	// would hang the face a neck's height over the body. The same head's HD
	// file has the joints it is cut at.
	// (Their bones are named SKEL_NECK_P_ and so on, which name no joint here.)
	if (ishead && original && (!havebind[SK_NECK]
				|| (bind[SK_NECK][0] == 0.0f && bind[SK_NECK][1] == 0.0f && bind[SK_NECK][2] == 0.0f))) {
		struct beanmodel *hd = malloc(sizeof(*hd));
		char hdsource[64];

		snprintf(hdsource, sizeof(hdsource), "new/%s", r->source);

		if (hd && beanLoad(hd, hdsource)) {
			for (s32 b = 0; b < hd->numbones; b++) {
				if (hd->skel[b] == SK_NECK || hd->skel[b] == SK_BACK) {
					memcpy(bind[(s32)hd->skel[b]], hd->bind[b], sizeof(bind[0]));
					havebind[(s32)hd->skel[b]] = 1;
				}
			}

			beanFree(hd);
		}

		free(hd);
	}

	memset(&out, 0, sizeof(out));
	memset(&rig, 0, sizeof(rig));

	if (ishead) {
		// A head is rigid on the neck, in the head file's own space, whose
		// origin is the body's neck joint. One taken from a whole character
		// is turned so Bean's spine points straight up, which GoldenEye's
		// does to within a third of a degree; a head file of Bean's stands
		// as it is.
		static const f32 up[3] = { 0.0f, 1.0f, 0.0f };

		if (!havebind[SK_NECK] || (fromchar && !havebind[SK_BACK])) {
			sysLogPrintf(LOG_WARNING, "gebean: %s has no neck", source);
			beanFree(&bm);
			return NULL;
		}

		if (fromchar) {
			f32 spine[3];

			for (s32 k = 0; k < 3; k++) {
				spine[k] = bind[SK_NECK][k] - bind[SK_BACK][k];
			}

			rotationBetween(spine, up, headrot);
		} else {
			rotationBetween(up, up, headrot);
		}

		nummatrices = 1;
	} else {
		if (!beanRigFromModel(modeldef, &rig, joints, jointskel, &numjoints)
				|| !beanFitRig(&rig, (const f32 (*)[3])bind, havebind)) {
			beanFree(&bm);
			return NULL;
		}

		for (s32 k = 0; k < numnodes; k++) {
			nodeskel[k] = (s8)beanNodeSkel(nodes[k], joints, jointskel, numjoints);
		}

		nummatrices = modeldef->nummatrices;

		if (nummatrices <= 0 || nummatrices > GEBEAN_MAXMTX) {
			beanFree(&bm);
			return NULL;
		}
	}

	for (s32 di = 0; di < bm.numdraws; di++) {
		const struct beandraw *d = &bm.draws[di];
		struct beanvb vb;
		u16 *tris;
		s32 numtris;
		s32 *mapped;

		if (!beanReadVb(&bm, d->vb, &vb)) {
			continue;
		}

		numtris = beanTriangles(&bm, d, &tris);

		if (numtris <= 0) {
			free(tris);
			continue;
		}

		// A buffer is shared by draws with different palettes, so a vertex is
		// taken once per draw: its bones mean different things in each.
		mapped = malloc(vb.count * sizeof(s32));

		if (!mapped) {
			free(tris);
			continue;
		}

		for (u32 i = 0; i < vb.count; i++) {
			mapped[i] = -1;
		}

		for (s32 t = 0; t < numtris; t++) {
			struct beanvtx v3[3];
			s32 sk[3][4];
			f32 wt[3][4];
			f32 total[SK_COUNT];
			s32 dominant = -1;
			s32 ok = 1;
			u16 idx[3];

			memset(total, 0, sizeof(total));

			for (s32 i = 0; i < 3 && ok; i++) {
				ok = beanVertex(&bm, &vb, tris[t * 3 + i], &v3[i]);

				for (s32 s = 0; s < 4 && ok; s++) {
					const s32 slot = v3[i].slot[s];
					s32 bone;

					sk[i][s] = -1;
					wt[i][s] = 0.0f;

					if (slot < 0 || slot >= d->numpal || v3[i].weight[s] == 0) {
						continue;
					}

					bone = d->pal[slot];
					bone = bm.numremap && bone < bm.numremap ? bm.remap[bone] : bone;

					if (bone >= bm.numbones || bm.skel[bone] < 0) {
						continue;
					}

					sk[i][s] = bm.skel[bone];
					wt[i][s] = (f32)v3[i].weight[s];
					total[sk[i][s]] += wt[i][s];
				}
			}

			if (!ok) {
				continue;
			}

			for (s32 s = 0; s < SK_COUNT; s++) {
				if (total[s] > 0.0f && (dominant < 0 || total[s] > total[dominant])) {
					dominant = s;
				}
			}

			// A head drawn with no bone palette (Dave's original is all
			// stride 24) is the head's all the same: rigid on the neck
			if (dominant < 0 && ishead) {
				dominant = SK_NECK;
			}

			if (dominant < 0) {
				continue;
			}

			// The neck belongs to the head file: a body leaves it out, and a
			// head takes only it. A whole character keeps both. An original's
			// head file weights its face to the back as often as to the neck
			// (Head B, Joel, Sally), and its only other bones are a stray pair
			// of shoes (Head B's, a body's height below), so there it is
			// anything but the limbs.
			if (ishead && original && !fromchar && dominant == SK_BACK) {
				dominant = SK_NECK;
			}

			if (r->kind != GEBEAN_WHOLE && (dominant == SK_NECK) != (ishead != 0)) {
				dropped++;
				continue;
			}

			for (s32 i = 0; i < 3 && ok; i++) {
				const u16 vi = tris[t * 3 + i];
				f32 pos[3] = { 0.0f, 0.0f, 0.0f };
				f32 nrm[3];
				f32 uv[2];
				u8 bone[3] = { 0, 0, 0 };
				f32 weight[3] = { 1.0f, 0.0f, 0.0f };

				if (mapped[vi] >= 0) {
					idx[i] = (u16)mapped[vi];
					continue;
				}

				uv[0] = v3[i].uv[0];
				uv[1] = v3[i].uv[1];

				if (ishead) {
					f32 rel[3];

					for (s32 k = 0; k < 3; k++) {
						rel[k] = v3[i].pos[k] - bind[SK_NECK][k];
					}

					rotApply(headrot, rel, pos);

					for (s32 k = 0; k < 3; k++) {
						pos[k] *= headscale;
					}

					rotApply(headrot, v3[i].nrm, nrm);
				} else {
					// Bean's own bind, at the rig's scale. Each bone's palette
					// entry (beanFitPalette()) takes a vertex from there onto the
					// model's rest, so the figure is skinned once, from the pose
					// Bean's weights were painted for; re-posing it onto the star
					// first and skinning it back again folded every armpit and
					// crotch through two blends of turns up to a right angle
					// apart. The bones become the model's matrices, merged where
					// two Bean bones share one, the three heaviest kept.
					s32 mtx[4];
					f32 mw[4];
					s32 nm = 0;
					f32 sum = 0.0f;

					for (s32 k = 0; k < 3; k++) {
						pos[k] = v3[i].pos[k] * rig.scale;
						nrm[k] = v3[i].nrm[k];
					}

					for (s32 s = 0; s < 4; s++) {
						const s32 b = sk[i][s];
						s32 at = -1;

						if (b < 0) {
							continue;
						}

						sum += wt[i][s];

						for (s32 m = 0; m < nm; m++) {
							if (mtx[m] == rig.mtx[b]) {
								at = m;
							}
						}

						if (at >= 0) {
							mw[at] += wt[i][s];
						} else {
							mtx[nm] = rig.mtx[b];
							mw[nm] = wt[i][s];
							nm++;
						}
					}

					if (sum <= 0.0f) {
						ok = 0;
						break;
					}

					for (s32 m = 0; m < nm; m++) {
						for (s32 n = m + 1; n < nm; n++) {
							if (mw[n] > mw[m]) {
								const f32 tw = mw[m];
								const s32 tm = mtx[m];
								mw[m] = mw[n]; mtx[m] = mtx[n];
								mw[n] = tw; mtx[n] = tm;
							}
						}
					}

					if (nm > 3) {
						nm = 3;
					}

					sum = 0.0f;

					for (s32 m = 0; m < nm; m++) {
						sum += mw[m];
					}

					for (s32 m = 0; m < 3; m++) {
						bone[m] = (u8)(m < nm ? mtx[m] : mtx[0]);
						weight[m] = m < nm ? mw[m] / sum : 0.0f;
					}

				}

				mapped[vi] = beanAddVertex(&out, pos, nrm, uv, bone, weight, 0xffffffff);

				if (mapped[vi] < 0) {
					ok = 0;
					break;
				}

				idx[i] = (u16)mapped[vi];
			}

			if (!ok) {
				continue;
			}

			for (s32 k = 0; k < numnodes; k++) {
				s32 takes;

				if (ishead) {
					takes = !beanNodeIsToggled(nodes[k]);
				} else {
					s32 want = dominant == SK_POSITION ? SK_BASE : dominant;
					s32 any = 0;

					for (s32 j = 0; j < numnodes; j++) {
						if (nodeskel[j] == want) {
							any = 1;
							break;
						}
					}

					if (!any) {
						want = SK_BASE;
					}

					takes = nodeskel[k] == want;
				}

				if (takes && !beanAddTri(&out, k, (s32)d->tex, idx[0], idx[1], idx[2])) {
					ok = 0;
					break;
				}
			}
		}

		free(mapped);
		free(tris);
	}

	// Nodes that must draw nothing rather than keep their N64 geometry: a
	// head's toggled pieces, which Bean's head has already, and the neck of a
	// body whose head file takes Bean's neck, or that carries its own. A generic
	// body's neck is left absent - the N64 stub stays under whatever head
	// GoldenEye X grafts on.
	if (out.numverts > 0) {
		for (s32 k = 0; k < numnodes; k++) {
			const s32 blank = ishead ? beanNodeIsToggled(nodes[k])
					: (r->kind == GEBEAN_BODY_WITH_HEAD || r->kind == GEBEAN_WHOLE) && nodeskel[k] == SK_NECK;

			if (blank) {
				beanAddTri(&out, k, 0, 0, 0, 0);
			}
		}
	}

	if (!ishead) {
		beanSmoothNeckWeights(&out, rig.mtx[SK_NECK], rig.mtx[SK_BACK]);
	}

	// The pictures: only those a draw names, bound once per character.
	nummatwords = bm.numtex + 1 < GEBEAN_MAXMATS ? bm.numtex + 1 : GEBEAN_MAXMATS;
	memset(mats, 0, sizeof(*mats));
	mats->num = nummatwords;

	for (s32 i = 0; i < nummatwords; i++) {
		matwords[i] = XBLAMESH_MAT_TABLE | (u32)i;
	}

	for (s32 i = 0; i < bm.numtex && i < nummatwords; i++) {
		s32 used = 0;

		for (s32 t = 0; t < out.numtris; t++) {
			if (out.tris[t].tex == i) {
				used = 1;
				break;
			}
		}

		if (used && beanBindTexture(&bm, source, i, &mats->tile[i], &mats->alpha[i], &mats->soft[i])
				&& mats->alpha[i]) {
			matwords[i] |= 0x8000;
		}
	}

	// A draw naming a texture the file does not have takes the untextured entry at the end.
	for (s32 t = 0; t < out.numtris; t++) {
		if (out.tris[t].tex >= bm.numtex) {
			out.tris[t].tex = (u16)(nummatwords - 1);
		}
	}

	file = beanWriteMesh(&out, numnodes, nummatrices, ishead ? NULL : &rig, matwords, nummatwords, outAbsent, outLen);

	sysLogPrintf(LOG_NOTE, "gebean: %s <- %s: %d vertices, %d triangles over %d lists, %s %.4f%s",
			r->file, source, out.numverts, out.numtris, numnodes,
			ishead ? "rigid on the neck, scale" : "skinned to the model's matrices, scale",
			ishead ? headscale : rig.scale, file ? "" : " - did not write");

	(void)dropped;

	beanOutFree(&out);
	beanFree(&bm);

	return file;
}

#endif
