#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "game/bondgun.h"
#include "game/chraction.h"
#include "game/file.h"
#include "game/game_0b0fd0.h"
#include "game/gfxmemory.h"
#include "game/hudmsg.h"
#include "game/inv.h"
#include "game/lang.h"
#include "game/modeldef.h"
#include "game/mtxf2lbulk.h"
#include "game/objectives.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/tex.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "system.h"
#include "video.h"
#include "romdata.h"
#include "modloader.h"
#include "gesfx.h"
#include "gewatch.h"
#include "geguns.h"
#include "gegadgets.h"

#ifndef PLATFORM_N64

/**
 * GoldenEye's gadgets in a converted mission (WEAPON_GE_COVERTMODEM and up).
 *
 * GoldenEye has three kinds. The ones its own code throws as it throws a mine
 * (gun.c's ITEM_BUG, ITEM_PLASTIQUE, ITEM_GOLDENEYEKEY) stand on the ECM mine.
 * The camera and the watch magnet are in the hand and do something when the
 * trigger is pulled. And six - the door decoder, the bomb defuser, the key
 * analyser, the data thief, Aztec's guidance data and its DAT tape - have **no
 * model in the hand at all** (their gitem rows say has_no_model): Bond equips
 * one from the watch and *uses the thing it is for*, and the mission's list
 * asks "was that object activated" and then "with this equipped"
 * (IFBondUsedGadgetOnObject, IFBondHasItemEquipped). All of those stand on the
 * Data Uplink, whose trigger is Perfect Dark's "activate what is in front of
 * me" (WEAPONFLAG_FIRETOACTIVATE) - which is the whole of what they need.
 *
 * A weapon number is an s8 in the gun control, so there are seven numbers for
 * eleven items: the six with nothing in the hand share two, and the mission
 * says which each is (g_Identities, the conversion's GE_GADGET_WEAPON).
 *
 * What is drawn in the hand is GoldenEye's own first person model out of the
 * ROM (the conversion's Igx%03dZ, by GoldenEye's item number), in place of the
 * host's: on the host's own root matrix, so it rises, lowers and sways as the
 * host does, moved to where GoldenEye's own weapon stats hold it.
 */

struct gegadgetidentity {
	s8 mission;       // the conversion's MISSIONS order; -1 for every mission
	u8 weaponnum;
	u8 item;          // GoldenEye's ITEM_IDS
	const char *name;
	u16 text;
};

// GoldenEye's own names (its LGUN bank). A mission's own row comes before the
// default for the same weapon.
static struct gegadgetidentity g_Identities[] = {
	{  1, WEAPON_GE_GADGETA,      38, "Door Decoder\n" },
	{  6, WEAPON_GE_GADGETA,      39, "Bomb Defuser\n" },
	{  6, WEAPON_GE_COVERTMODEM,  47, "Tracker Bug\n" },
	{  4, WEAPON_GE_GADGETA,      46, "Key Analyzer\n" },
	{  4, WEAPON_GE_GADGETB,      55, "Data Thief\n" },
	{ 18, WEAPON_GE_GADGETA,      50, "Guidance Data\n" },
	{ 18, WEAPON_GE_GADGETB,      73, "DAT Tape\n" },
	{ -1, WEAPON_GE_COVERTMODEM,  47, "Covert Modem\n" },
	{ -1, WEAPON_GE_PLASTIQUE,    34, "Plastique\n" },
	{ -1, WEAPON_GE_GOLDENEYEKEY, 61, "GoldenEye Key\n" },
	{ -1, WEAPON_GE_CAMERA,       40, "Camera\n" },
	{ -1, WEAPON_GE_WATCHMAGNET,  60, "Watch Magnet Attract\n" },
	{ -1, WEAPON_GE_GADGETA,       0, "Gadget\n" },
	{ -1, WEAPON_GE_GADGETB,       0, "Gadget\n" },
	{ -1, WEAPON_GE_TANKSHELLS,   33, "Tank\n" },
	{ -1, WEAPON_GE_DETONATOR,    30, "Detonator\n" },
};

// Bunker, where the key analyser copies the GoldenEye key
#define MISSION_BUNKER 4

// Train, where Bond's watch laser (ITEM_WATCHLASER, 23) stands on the
// Moonraker's weapon number (the conversion's g_GeItemWeapon: the same beam)
#define MISSION_TRAIN 13
#define ITEM_WATCHLASER 23
#define ITEM_TRIGGER 30

// The part whose position is the watch laser's muzzle: GoldenEye's switch 3,
// where gunfire.c hangs the flash and starts the beam (field_B58)
#define WATCH_PART_FLASH 3

// GoldenEye's PROPDEF_OBJECTIVE_COPY_ITEM asks one thing, "has the key been
// copied", and Perfect Dark has no such record: the conversion writes it as a
// complete-on-flag objective on this stage flag (gesolo.py's GE_COPYITEM_FLAG)
#define GEGADGET_COPY_FLAG 0x80000000

#define GESFX_CAMERA_CLICK 244
#define GESFX_KEY_ANALYSER 245

// Where the middle of each stands in the hand, in the camera's space.
// GoldenEye holds these with a hand animation playing (gunfire.c's field_8EC),
// and the model's own origin is a long way from the thing itself - the covert
// modem hangs thirty units under it - so its WeaponStats position places
// nothing without that animation. The model is measured instead and its middle
// put here, low and to the right where GoldenEye shows it.
// The models are authored at sizes of their own as well (GoldenEye scales each
// in the hand; the plastique is twice the modem and the watch arm six times the
// camera at the host's scale), so each is brought to a width across the screen.
//
// A width of 0 is a model GoldenEye holds with no animation, which is posed as
// GoldenEye poses it instead: at its own size, turned by `turn` (radians,
// gunfire.c's own for it) and its root at `pos`, its WeaponStats PosX/Y/Z.
struct gegadgethand {
	u8 weaponnum;
	f32 pos[3];
	f32 width;
	f32 turn[3];
};

static const struct gegadgethand g_Hands[] = {
	{ WEAPON_GE_COVERTMODEM,  { 11.0f, -10.5f, -30.0f }, 17.0f },
	{ WEAPON_GE_PLASTIQUE,    { 11.0f, -11.5f, -30.0f }, 19.0f },
	{ WEAPON_GE_GOLDENEYEKEY, { 11.0f, -10.5f, -30.0f }, 14.0f },
	{ WEAPON_GE_CAMERA,       { 11.0f, -10.0f, -30.0f }, 14.0f },
	// trigger_stats, and gunfire.c's D_80035C70 for ITEM_TRIGGER
	{ WEAPON_GE_DETONATOR,    { -2.0f, -21.5f, -19.0f }, 0.0f, { 6.2536321f, 6.2592888f, 0.204238f } },
};

#define GADGET_RWDATA_MAX 1024

/**
 * The detonator (GtriggerZ) is Bond's two hands at his watch, and GoldenEye
 * moves one piece of it: switch 6, the right hand, turns about an axis that
 * its switch 28 gives - an interlink node, which the conversion leaves out
 * (Perfect Dark has no such node), so its two points are here. Held still the
 * hand stands five degrees back off the watch; while the trigger is held it
 * turns in to press the button, and back when let go (gunfire.c, gun.c's
 * sub_GAME_7F05E6B4() and get_value_if_watch_is_on_hand_or_not()). The six
 * cuffs are switches 29 to 34, one of them worn (bondviewSelectCuff()).
 */
#define DETONATOR_PART_HAND  6
#define DETONATOR_PART_CUFF  29
#define DETONATOR_AXIS_X     20.208658f  // the interlink's first point, less its second (0, 0, 0)
#define DETONATOR_AXIS_Y     32.669170f
#define DETONATOR_AXIS_Z     (-18.414536f)
#define DETONATOR_PRESS      0.08726647f  // radians: the whole press
#define DETONATOR_IN         0.029088823f // a tick, pressing
#define DETONATOR_OUT        0.017453294f // a tick, letting go

static struct {
	s32 mission;
	s32 moddir;
	s32 item;          // the item whose model is loaded, -1 for none
	s32 failed;
	u8 *buf;
	u32 buflen;
	struct modeldef *def;
	f32 press;         // the detonator's hand, 0 off the watch to DETONATOR_PRESS on it
	f32 flash[3];      // the watch laser's muzzle in the camera's space, last drawn
	s32 flashframe;    // the frame it was drawn on, -1 for none
	u16 laserhostname; // the Moonraker's own name, while Train wears the watch laser's
	u16 laserhostshort;
	u16 lasertext;
	struct model model;
	u32 rwdata[GADGET_RWDATA_MAX];
	s32 photo;         // the camera's trigger was pulled: judged in the render
	struct prop *keyprop; // the GoldenEye key's own prop, while it is carried
	s32 centreitem;    // the item `centre` was measured on
	f32 centre[3];     // the model's middle from its root, in the camera's space
	f32 size[3];
} g_Gadgets = { .mission = -1, .moddir = -1, .item = -1, .failed = -1, .centreitem = -1, .flashframe = -1 };

s32 gegadgetsIsGadget(s32 weaponnum)
{
	return weaponnum >= WEAPON_GE_COVERTMODEM && weaponnum < NUM_WEAPONS;
}

/**
 * The Moonraker's number on Train is GoldenEye's watch laser, which GoldenEye
 * draws as Bond's two hands at his watch (GwatchlaserZ), held and turned
 * exactly as the detonator (watchlaser_stats' PosX/Y/Z are trigger_stats',
 * gunfire.c turns both by D_80035C70, gun.c presses both alike) - and not as
 * the Moonraker's gun. The conversion leaves item 23 out (it is no gun of the
 * port's), so where there is no Igx023Z the detonator's own GtriggerZ
 * (Igx030Z) stands in for it: the two models have the same node count,
 * matrices, bounds and textures, and the native port draws them alike. A
 * conversion that writes GwatchlaserZ as Igx023Z is picked up by itself.
 */
static s32 gegadgetsIsWatchLaser(s32 weaponnum)
{
	return weaponnum == WEAPON_GE_MOONRAKER && g_Gadgets.moddir >= 0 && g_Gadgets.mission == MISSION_TRAIN;
}

static const struct gegadgetidentity *gegadgetsIdentity(s32 weaponnum)
{
	for (s32 i = 0; i < (s32)ARRAYCOUNT(g_Identities); i++) {
		const struct gegadgetidentity *id = &g_Identities[i];

		if (id->weaponnum == weaponnum && (id->mission < 0 || id->mission == g_Gadgets.mission)) {
			return id;
		}
	}

	return NULL;
}

/** GoldenEye's item number for what this weapon is on this mission, 0 for nothing. */
s32 gegadgetsItem(s32 weaponnum)
{
	const struct gegadgetidentity *id = gegadgetsIsGadget(weaponnum) ? gegadgetsIdentity(weaponnum) : NULL;

	return id ? id->item : 0;
}

static void gegadgetsUnloadModel(void)
{
	if (g_Gadgets.buf) {
		videoFreeCachedTextures(g_Gadgets.buf, g_Gadgets.buf + g_Gadgets.buflen);
		sysMemFree(g_Gadgets.buf);
		g_Gadgets.buf = NULL;
	}

	g_Gadgets.def = NULL;
	g_Gadgets.item = -1;
}

/**
 * A stage is loading: whose names the shared numbers wear, and nothing of the
 * last stage's in the hand.
 */
void gegadgetsStageLoad(s32 stagenum)
{
	gegadgetsUnloadModel();

	g_Gadgets.failed = -1;
	g_Gadgets.photo = 0;
	g_Gadgets.flashframe = -1;
	g_Gadgets.keyprop = NULL;
	g_Gadgets.mission = modloaderStageMission(stagenum);
	g_Gadgets.moddir = modloaderStageIsRemake(stagenum) ? modloaderGetStageModDirIndex(stagenum) : -1;

	for (s32 w = WEAPON_GE_COVERTMODEM; w < NUM_WEAPONS; w++) {
		struct gegadgetidentity *id = (struct gegadgetidentity *)gegadgetsIdentity(w);

		if (id) {
			if (!id->text) {
				id->text = langAddPortText(id->name);
			}

			g_GeWeaponDefs[w - WEAPON_GE_FIRST].name = id->text;
			g_GeWeaponDefs[w - WEAPON_GE_FIRST].shortname = id->text;
		}
	}

	// The watch laser on the Moonraker's number wears GoldenEye's name for
	// it (LGUN's GUN_STR_7B) in the inventory, the watch and the messages;
	// every other stage gives the Moonraker its own name back
	{
		struct weapon *laser = &g_GeWeaponDefs[WEAPON_GE_MOONRAKER - WEAPON_GE_FIRST];

		if (!g_Gadgets.lasertext) {
			g_Gadgets.lasertext = langAddPortText("Watch Laser\n");
			g_Gadgets.laserhostname = laser->name;
			g_Gadgets.laserhostshort = laser->shortname;
		}

		if (gegadgetsIsWatchLaser(WEAPON_GE_MOONRAKER)) {
			laser->name = g_Gadgets.lasertext;
			laser->shortname = g_Gadgets.lasertext;
		} else {
			laser->name = g_Gadgets.laserhostname;
			laser->shortname = g_Gadgets.laserhostshort;
		}

		// and its own numbers, ammunition and sound (geguns.c)
		gegunsSetWatchLaser(gegadgetsIsWatchLaser(WEAPON_GE_MOONRAKER));
	}
}

/** GoldenEye's own first person model for an item, the conversion's Igx%03dZ. */
static s32 gegadgetsLoadModel(s32 item)
{
	char name[16];
	s32 fileid;
	s32 size;

	if (item == g_Gadgets.item) {
		return 1;
	}

	if (item == g_Gadgets.failed || g_Gadgets.moddir < 0) {
		return 0;
	}

	gegadgetsUnloadModel();
	g_Gadgets.failed = item;

	snprintf(name, sizeof(name), "Igx%03dZ", item);
	fileid = romdataRegisterModFile(name, g_Gadgets.moddir);
	size = fileid > 0 ? fileGetInflatedSize(fileid, LOADTYPE_MODEL) : 0;

	// GwatchlaserZ where the conversion writes it, the detonator's GtriggerZ
	// in its place where it does not (gegadgetsIsWatchLaser())
	if (size <= 0 && item == ITEM_WATCHLASER) {
		snprintf(name, sizeof(name), "Igx%03dZ", ITEM_TRIGGER);
		fileid = romdataRegisterModFile(name, g_Gadgets.moddir);
		size = fileid > 0 ? fileGetInflatedSize(fileid, LOADTYPE_MODEL) : 0;
	}

	if (size <= 0) {
		return 0;
	}

	g_Gadgets.buflen = ALIGN64(size) + 0x20000;
	g_Gadgets.buf = sysMemZeroAlloc(g_Gadgets.buflen);

	if (!g_Gadgets.buf) {
		return 0;
	}

	g_Gadgets.def = modeldefLoad(fileid, g_Gadgets.buf, g_Gadgets.buflen, NULL);

	if (!g_Gadgets.def) {
		gegadgetsUnloadModel();
		return 0;
	}

	modelAllocateRwData(g_Gadgets.def);

	if (g_Gadgets.def->rwdatalen > GADGET_RWDATA_MAX) {
		gegadgetsUnloadModel();
		return 0;
	}

	memset(g_Gadgets.rwdata, 0, sizeof(g_Gadgets.rwdata));
	modelInit(&g_Gadgets.model, g_Gadgets.def, g_Gadgets.rwdata, false);
	g_Gadgets.model.anim = NULL;
	modelSetScale(&g_Gadgets.model, 1.0f);

	g_Gadgets.item = item;
	g_Gadgets.failed = -1;

	return 1;
}

static struct modelnode *gegadgetsNextNode(struct modelnode *node)
{
	if (node->child) {
		return node->child;
	}

	while (node && !node->next) {
		node = node->parent;
	}

	return node ? node->next : NULL;
}

/**
 * The middle of everything the posed model draws, from its root: once a model,
 * on the first frame it is posed. `--gadget-measure` says what it found.
 */
static void gegadgetsMeasure(void)
{
	f32 min[3] = { 1e9f, 1e9f, 1e9f };
	f32 max[3] = { -1e9f, -1e9f, -1e9f };
	s32 any = 0;

	for (struct modelnode *node = g_Gadgets.def->rootnode; node; node = gegadgetsNextNode(node)) {
		const Mtxf *mtx;
		struct modelnode *up;
		s32 index = 0;

		if ((node->type & 0xff) != MODELNODETYPE_DL || !node->rodata || !node->rodata->dl.vertices) {
			continue;
		}

		for (up = node->parent; up; up = up->parent) {
			if ((up->type & 0xff) == MODELNODETYPE_POSITION) {
				index = up->rodata->position.mtxindex0;
				break;
			}

			if ((up->type & 0xff) == MODELNODETYPE_POSITIONHELD) {
				index = up->rodata->positionheld.mtxindex;
				break;
			}
		}

		if (index < 0 || index >= g_Gadgets.def->nummatrices) {
			continue;
		}

		mtx = &g_Gadgets.model.matrices[index];

		for (s32 i = 0; i < node->rodata->dl.numvertices; i++) {
			const Vtx *v = &node->rodata->dl.vertices[i];
			const f32 in[3] = { v->x, v->y, v->z };

			for (s32 a = 0; a < 3; a++) {
				const f32 out = in[0] * mtx->m[0][a] + in[1] * mtx->m[1][a] + in[2] * mtx->m[2][a] + mtx->m[3][a];

				if (out < min[a]) min[a] = out;
				if (out > max[a]) max[a] = out;
			}

			any = 1;
		}
	}

	for (s32 a = 0; a < 3; a++) {
		g_Gadgets.centre[a] = any ? (min[a] + max[a]) * 0.5f : 0.0f;
		g_Gadgets.size[a] = any ? max[a] - min[a] : 0.0f;
	}

	g_Gadgets.centreitem = g_Gadgets.item;

	if (sysArgCheck("--gadget-measure")) {
		sysLogPrintf(LOG_NOTE, "gadget: item %d middle %.1f %.1f %.1f size %.1f %.1f %.1f from its root",
				g_Gadgets.item, g_Gadgets.centre[0], g_Gadgets.centre[1], g_Gadgets.centre[2],
				g_Gadgets.size[0], g_Gadgets.size[1], g_Gadgets.size[2]);
	}
}

/** The detonator's cuff for the player's outfit, before its matrices are set. */
static void gegadgetsDetonatorCuff(void)
{
	const s32 wear = geWatchCuff();

	for (s32 i = 0; i < 6; i++) {
		struct modelnode *node = modelGetPart(g_Gadgets.def, DETONATOR_PART_CUFF + i);
		union modelrwdata *rwdata;

		if (node && (node->type & 0xff) == MODELNODETYPE_TOGGLE) {
			rwdata = modelGetNodeRwData(&g_Gadgets.model, node);

			if (rwdata) {
				rwdata->toggle.visible = i == wear;
			}
		}
	}
}

/**
 * The detonator's right hand turned about its hinge, once its matrices are
 * set: GoldenEye's `rwmtx[switch 6] = gun * (position * turn)` where the
 * matrix was `gun * position`, so the turn goes on the right of it.
 */
static void gegadgetsDetonatorPress(Mtxf *matrices)
{
	struct modelnode *node = modelGetPart(g_Gadgets.def, DETONATOR_PART_HAND);
	const f32 step = g_Vars.lvupdate60freal;
	Mtxf turn;
	Mtxf out;
	s32 index;

	if (g_Vars.currentplayer->hands[HAND_RIGHT].triggeron) {
		g_Gadgets.press += DETONATOR_IN * step;
	} else {
		g_Gadgets.press -= DETONATOR_OUT * step;
	}

	if (g_Gadgets.press > DETONATOR_PRESS) {
		g_Gadgets.press = DETONATOR_PRESS;
	}

	if (g_Gadgets.press < 0.0f) {
		g_Gadgets.press = 0.0f;
	}

	if (!node || (node->type & 0xff) != MODELNODETYPE_POSITION) {
		return;
	}

	index = node->rodata->position.mtxindex0;

	if (index < 0 || index >= g_Gadgets.def->nummatrices) {
		return;
	}

	guRotateF(turn.m, (g_Gadgets.press - DETONATOR_PRESS) * (180.0f / 3.1415927f),
			DETONATOR_AXIS_X, DETONATOR_AXIS_Y, DETONATOR_AXIS_Z);
	mtx4MultMtx4(&matrices[index], &turn, &out);
	mtx4Copy(&out, &matrices[index]);
}

/**
 * The hand's gun, for bgunRender(): 0 when the weapon is no gadget and the
 * host's own model is to be drawn as ever; 1 when the gadget has been dealt
 * with - GoldenEye's model drawn on the host's root matrix, or nothing drawn
 * because GoldenEye draws nothing - and the host's model and Perfect Dark's
 * hand are to be left out.
 */
s32 gegadgetsRenderHand(struct modelrenderdata *renderdata, struct model *hostmodel, s32 weaponnum)
{
	const struct gegadgethand *held = NULL;
	const struct weapon *host;
	const s32 watchlaser = gegadgetsIsWatchLaser(weaponnum);
	s32 watch;
	Mtxf base;
	Mtxf *matrices;
	f32 fit = 1.0f;
	s32 item;

	if (!gegadgetsIsGadget(weaponnum) && !watchlaser) {
		return 0;
	}

	// The watch's detonator is GoldenEye's own model or nothing: its host is
	// the Data Uplink, which is no detonator, and GoldenEye's remote mines go
	// wherever the Combat Simulator offers its guns
	if (g_Gadgets.moddir < 0) {
		return weaponnum == WEAPON_GE_DETONATOR;
	}

	for (s32 i = 0; i < (s32)ARRAYCOUNT(g_Hands); i++) {
		if (g_Hands[i].weaponnum == (watchlaser ? WEAPON_GE_DETONATOR : weaponnum)) {
			held = &g_Hands[i];
		}
	}

	// the six GoldenEye gives no model, and the watch magnet, whose
	// watchmagnetattract_stats carry WEAPONSTATBITFLAG_HIDE_FIRST_PERSON_HAND
	// (gunfire.c then leaves field_87F clear and draws nothing): an empty
	// hand, as it has it. Measured and fitted to a width, the magnet's watch
	// arm (GwatchmagnetattractZ) was a giant watch floating at the lower
	// right (F3 20260922-000405); the native port shows nothing in the hand
	// 30, 90 and 200 frames after equipping it, attract or repel.
	if (!held) {
		return 1;
	}

	item = watchlaser ? ITEM_WATCHLASER : gegadgetsItem(weaponnum);
	watch = weaponnum == WEAPON_GE_DETONATOR || watchlaser;

	if (!hostmodel->matrices || !gegadgetsLoadModel(item)) {
		return weaponnum == WEAPON_GE_DETONATOR;
	}

	// the host's root for its turn and its size, posed about the eye first so
	// that the model can be measured from its own root. The watch laser's is
	// the Moonraker's own definition, whose place is GoldenEye's laser's or
	// its host's by the look (geguns.c)
	host = watchlaser ? weaponFindById(weaponnum)
		: g_Weapons[g_GeWeaponHosts[weaponnum - WEAPON_GE_FIRST]];
	mtx4Copy(&hostmodel->matrices[0], &base);
	base.m[3][0] = 0.0f;
	base.m[3][1] = 0.0f;
	base.m[3][2] = 0.0f;

	if (held->width <= 0.0f) {
		// GoldenEye's own turn, under whatever the host's root turns by (the
		// sway of a walk): its gunmtx is the look times this, at the size
		// the host is drawn at, which is GoldenEye's own (0.1 a unit)
		struct coord turn = { held->turn[0], held->turn[1], held->turn[2] };
		Mtxf rot;
		Mtxf out;

		mtx4LoadRotation(&turn, &rot);
		mtx4MultMtx4(&base, &rot, &out);
		mtx4Copy(&out, &base);
	} else if (g_Gadgets.centreitem == item && g_Gadgets.size[0] > 0.0f) {
		fit = held->width / g_Gadgets.size[0];

		for (s32 r = 0; r < 3; r++) {
			for (s32 c = 0; c < 3; c++) {
				base.m[r][c] *= fit;
			}
		}
	}

	matrices = gfxAllocate(g_Gadgets.def->nummatrices * sizeof(Mtxf));

	for (s32 i = 0; i < g_Gadgets.def->nummatrices; i++) {
		mtx4LoadIdentity(&matrices[i]);
	}

	mtx4Copy(&base, matrices);
	g_Gadgets.model.matrices = matrices;

	{
		Mtxf *prevbase = renderdata->unk00;
		Mtxf *prevmatrices = renderdata->unk10;

		renderdata->unk00 = &base;
		renderdata->unk10 = matrices;

		modelSetDistanceChecksDisabled(true);

		if (watch) {
			gegadgetsDetonatorCuff();
		}

		modelUpdateRelations(&g_Gadgets.model);
		modelSetMatrices(renderdata, &g_Gadgets.model);

		if (watch) {
			gegadgetsDetonatorPress(matrices);
		}

		if (held->width > 0.0f && g_Gadgets.centreitem != g_Gadgets.item) {
			// the first frame of a model: measured at the host's own size,
			// and drawn from the next frame on, once it has a size of its own
			gegadgetsMeasure();
			modelSetDistanceChecksDisabled(false);
			renderdata->unk00 = prevbase;
			renderdata->unk10 = prevmatrices;
			mtxF2LBulk(matrices, g_Gadgets.def->nummatrices);

			return 1;
		}

		// its middle to its place, and with it whatever the host's own root
		// has moved from where the host is held: the rise and fall of an
		// equip, and the sway of a walk
		for (s32 a = 0; a < 3; a++) {
			const f32 hostrest = a == 0 ? host->posx : (a == 1 ? host->posy : host->posz);
			const f32 middle = held->width > 0.0f ? g_Gadgets.centre[a] * fit : 0.0f;
			const f32 shift = held->pos[a] - middle + hostmodel->matrices[0].m[3][a] - hostrest;

			for (s32 i = 0; i < g_Gadgets.def->nummatrices; i++) {
				matrices[i].m[3][a] += shift;
			}
		}

		modelRender(renderdata, &g_Gadgets.model);
		modelSetDistanceChecksDisabled(false);

		// the watch laser's muzzle, as gunfire.c takes it: gunmtx (the
		// model's root) times switch 3's position
		if (watchlaser) {
			struct modelnode *flash = modelGetPart(g_Gadgets.def, WATCH_PART_FLASH);

			if (flash && (flash->type & 0xff) == MODELNODETYPE_POSITION) {
				const struct coord *at = &flash->rodata->position.pos;

				for (s32 a = 0; a < 3; a++) {
					g_Gadgets.flash[a] = at->x * matrices[0].m[0][a] + at->y * matrices[0].m[1][a]
						+ at->z * matrices[0].m[2][a] + matrices[0].m[3][a];
				}

				g_Gadgets.flashframe = g_Vars.lvframenum;
			}
		}

		renderdata->unk00 = prevbase;
		renderdata->unk10 = prevmatrices;
	}

	mtxF2LBulk(matrices, g_Gadgets.def->nummatrices);

	return 1;
}

// Train's StartAmmo for it (UsetuptraZ.c: AMMO_WATCH_LASER, 300)
#define WATCHLASER_START_AMMO 300

/**
 * A weapon the stage's intro gives the player (playerreset.c): the watch
 * laser comes with GoldenEye's charge for it. The conversion's ammunition
 * table stops before AMMO_WATCH_LASER (24), so the intro's own grant of 300
 * is not in the converted setup; a conversion that writes it is left alone.
 */
void gegadgetsIntroWeapon(s32 weaponnum)
{
	if (gegadgetsIsWatchLaser(weaponnum) && bgunGetReservedAmmoCount(AMMOTYPE_WATCHLASER) == 0) {
		bgunSetAmmoQuantity(AMMOTYPE_WATCHLASER, WATCHLASER_START_AMMO);
	}
}

/** Whether this weapon is the watch laser on this stage (gunfx.c's beam). */
s32 gegadgetsWatchLaserActive(s32 weaponnum)
{
	return gegadgetsIsWatchLaser(weaponnum);
}

/**
 * Where the watch laser's beam starts, in the camera's space: GoldenEye starts
 * it at the watch (gunfire.c's field_B58, the flash node on the watch model),
 * not at the Moonraker's muzzle, which in the XBLA look is the host's, off at
 * the left of the screen. 0 for any other weapon, or before the watch has
 * been drawn; bondgun.c keeps its own muzzle then.
 */
s32 gegadgetsWatchLaserMuzzle(s32 weaponnum, f32 *campos)
{
	if (!gegadgetsIsWatchLaser(weaponnum) || g_Gadgets.item != ITEM_WATCHLASER
			|| g_Gadgets.flashframe < 0 || g_Vars.lvframenum - g_Gadgets.flashframe > 2) {
		return 0;
	}

	campos[0] = g_Gadgets.flash[0];
	campos[1] = g_Gadgets.flash[1];
	campos[2] = g_Gadgets.flash[2];

	return 1;
}

/**
 * GoldenEye's prop for a thrown gadget, as the conversion's `models` block
 * numbers it (MODEL_REMAKE_FIRST + its PROP number), or -1 where the
 * conversion is not loaded and the host's own is what there is.
 */
s32 gegadgetsPropModel(s32 weaponnum)
{
	s32 prop;

	switch (weaponnum) {
	case WEAPON_GE_COVERTMODEM:  prop = 245; break; // PROP_CHRBUG
	case WEAPON_GE_PLASTIQUE:    prop = 273; break; // PROP_CHRPLASTIQUE
	case WEAPON_GE_GOLDENEYEKEY: prop = 248; break; // PROP_CHRGOLDENEYEKEY
	default: return -1;
	}

	return g_ModelStates[MODEL_REMAKE_FIRST + prop].fileid ? MODEL_REMAKE_FIRST + prop : -1;
}

/**
 * The watch magnet: GoldenEye draws whatever a guard could drop towards Bond.
 * Here the nearest thing that can be picked up, in front of the player and
 * within reach of the magnet, comes to hand.
 */
#define MAGNET_REACH 1000.0f
#define MAGNET_CONE  0.8f

static void gegadgetsMagnet(void)
{
	struct player *player = g_Vars.currentplayer;
	const f32 theta = player->vv_theta * M_BADTAU / 360.0f;
	const f32 lookx = -sinf(theta);
	const f32 lookz = cosf(theta);
	struct prop *best = NULL;
	f32 bestdist = MAGNET_REACH * MAGNET_REACH;

	// a prop nobody is looking at is on the paused list, and the key Bunker 2
	// hangs outside its cell is exactly that until the player turns to it
	for (s32 list = 0; list < 2; list++)
	for (struct prop *prop = list ? g_Vars.pausedprops : g_Vars.activeprops; prop; prop = prop->next) {
		f32 dx, dy, dz, dist;

		if ((prop->type != PROPTYPE_WEAPON && prop->type != PROPTYPE_OBJ) || !prop->obj || prop->parent) {
			continue;
		}

		if (prop->type == PROPTYPE_OBJ && prop->obj->type != OBJTYPE_KEY) {
			continue;
		}

		dx = prop->pos.x - player->prop->pos.x;
		dy = prop->pos.y - player->prop->pos.y;
		dz = prop->pos.z - player->prop->pos.z;
		dist = dx * dx + dy * dy + dz * dz;

		if (dist >= bestdist || dist < 1.0f) {
			continue;
		}

		if ((dx * lookx + dz * lookz) / sqrtf(dx * dx + dz * dz + 1.0f) < MAGNET_CONE) {
			continue;
		}

		best = prop;
		bestdist = dist;
	}

	if (best) {
		// what a pickup asks to have done with the prop - given to the
		// player, freed - is the caller's to carry out
		propExecuteTickOperation(best, propPickupByPlayer(best, true));
	}
}

/**
 * GoldenEye's "put it back" objective (PROPDEF_OBJECTIVE_DEPOSIT_OBJECT, which
 * Perfect Dark kept as OBJECTIVETYPE_THROWOBJ) asks whether the tagged object's
 * own prop is still in the inventory, and GoldenEye throws that very prop
 * (gun.c's bondinvRemovePropWeaponByID() for ITEM_GOLDENEYEKEY). Perfect Dark
 * gives a picked up weapon by its number and keeps a tagged one's prop out of
 * the inventory, so the objective was complete before the key was ever touched.
 * The prop goes into the inventory beside the item, hidden from its list, and
 * comes out when the key leaves the hand.
 */
void gegadgetsKept(struct prop *prop)
{
	if (g_Gadgets.moddir < 0 || !prop || prop->type != PROPTYPE_WEAPON || !prop->weapon
			|| prop->weapon->weaponnum != WEAPON_GE_GOLDENEYEKEY
			|| !(prop->weapon->base.hidden & OBJHFLAG_TAGGED)) {
		return;
	}

	prop->weapon->base.flags2 |= OBJFLAG2_INVHIDDEN;
	invGiveProp(prop);
	g_Gadgets.keyprop = prop;
}

void gegadgetsThrown(s32 weaponnum, struct weaponobj *thrown)
{
	struct prop *prop = g_Gadgets.keyprop;
	struct defaultobj *obj;

	if (weaponnum != WEAPON_GE_GOLDENEYEKEY || !prop) {
		return;
	}

	g_Gadgets.keyprop = NULL;
	invRemoveProp(prop);

	if (!thrown || prop->type != PROPTYPE_WEAPON || !prop->obj) {
		return;
	}

	// GoldenEye throws the very prop it picked up (gun.c: the key is taken
	// out of the inventory with bondinvRemovePropWeaponByID(), objDetach()ed
	// and thrown). Perfect Dark's throw builds a new prop, and the one picked
	// up stayed a child of the player's prop - which invHasProp() counts as
	// carried - so the "put it back" objective never completed however often
	// the key was thrown (F3 20260925-231553). The tag moves to the thrown
	// prop, which is the key from here on (picked up again, it is carried
	// again), and the one in the hand is freed.
	obj = prop->obj;

	for (struct tag *tag = g_TagsLinkedList; tag; tag = tag->next) {
		if (tag->obj == obj) {
			tag->obj = &thrown->base;
			thrown->base.hidden |= OBJHFLAG_TAGGED;
		}
	}

	obj->hidden &= ~OBJHFLAG_TAGGED;
	objDetach(prop);
	objFreePermanently(obj, true);
}

/**
 * lvRender(), once the player's props have been drawn, which is where Perfect
 * Dark judges the CamSpy's holograph: GoldenEye's photograph objective is that
 * one kept whole - the object on the screen, all of it, and in one piece.
 */
void gegadgetsAfterProps(void)
{
	if (g_Gadgets.photo) {
		g_Gadgets.photo = 0;
		objectiveCheckHolograph(0.0f);
	}
}

/**
 * GoldenEye's key analyser (gunfire.c's analyzeGEKey()), run when its trigger
 * is pulled - gunTickHandState() takes ITEM_KEYANALYSERCASE through
 * GUN_ANIM_STATE_TRIGGER_PRESS to GUN_ANIM_STATE_USE_ITEM and analyses on that
 * state's first frame; equipping it does nothing. With the GoldenEye key in
 * the inventory the key is copied (`copiedgoldeneye`, which is all Bunker's
 * PROPDEF_OBJECTIVE_COPY_ITEM asks) and the key is put in the right hand, the
 * left emptied, to be put back (the same objective's DEPOSIT_OBJECT half);
 * without it the player is told so. Until 2026-09-26 this ran when the
 * analyser was equipped, so an analyser already in the hand when the key was
 * picked up never analysed at all.
 */
static void gegadgetsAnalyseKey(void)
{
	if (invHasSingleWeaponIncAllGuns(WEAPON_GE_GOLDENEYEKEY)) {
		hudmsgCreate("Analyzing the GoldenEye key...\n", HUDMSGTYPE_DEFAULT);
		geSfxPlay(GESFX_KEY_ANALYSER, GESFX_VOLUME);
		chrSetStageFlag(NULL, GEGADGET_COPY_FLAG);
		bgunEquipWeapon2(HAND_RIGHT, WEAPON_GE_GOLDENEYEKEY);
		bgunEquipWeapon2(HAND_LEFT, WEAPON_NONE);
	} else {
		hudmsgCreate("You do not have the GoldenEye key.\n", HUDMSGTYPE_DEFAULT);
	}
}

/** The trigger, pulled with a gadget in the hand (bondmove.c). */
void gegadgetsFire(s32 weaponnum)
{
	if (g_Gadgets.moddir < 0) {
		return;
	}

	if (weaponnum == WEAPON_GE_CAMERA) {
		// judged in the render (gegadgetsAfterProps()): the trigger is read
		// in the tick, when the props' matrices are last frame's and already
		// in the hardware's fixed point
		geSfxPlay(GESFX_CAMERA_CLICK, GESFX_VOLUME);
		g_Gadgets.photo = 1;
	} else if (weaponnum == WEAPON_GE_WATCHMAGNET) {
		gegadgetsMagnet();
	} else if (weaponnum == WEAPON_GE_GADGETA && g_Gadgets.mission == MISSION_BUNKER) {
		gegadgetsAnalyseKey();
	}
}

#endif
