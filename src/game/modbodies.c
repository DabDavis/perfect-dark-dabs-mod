#include <ultra64.h>
#include "constants.h"
#include "game/body.h"
#include "game/bot.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/modbodies.h"
#include "game/modelmgr.h"
#include "game/modoptions.h"
#include "game/cheats.h"
#include "game/gfxmemory.h"
#include "game/prop.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"
#include <string.h>

/**
 * Bodies that are left lying where they fell.
 *
 * Two things had to give for this to be possible, and they are different
 * problems.
 *
 * A body in a mission is already allowed to lie there forever - act_dead's
 * fadetimer60 is -1 for a chr with no aibot, which means never fade - and what
 * takes it away is chraTickBg(), which allows five chrs on screen at once and
 * fades corpses down to that number. That is a polygon budget written for an
 * N64 and it is the only reason a mission's dead disappear. A body this pool is
 * keeping is skipped by that selection and answers to the cap and the timer
 * here instead.
 *
 * A simulant's body is a harder case, and it is why this file exists at all.
 * fadetimer60 is not a corpse timer for a simulant, it is the respawn clock:
 * the end of the ninety tick fade in chrTickDead() is the one place in the game
 * that calls botSpawn() for a dead simulant, and the corpse and the simulant
 * that gets up are the same chrdata. Keeping the body would mean never
 * respawning its owner. So at the end of that window the body is handed a chr
 * of its own and the simulant is given a fresh model and prop to get up in.
 * The respawn still lands on the tick it always did.
 *
 * The handover swaps the pair rather than posing a copy, so the body keeps the
 * model it died in: the exact frame of its death animation, the limbs an
 * explosion took off, the blood on it, the splats the prop owns. A fresh model
 * posed to match would lose all of that at the moment of the swap, in view, a
 * second and a half after the kill.
 *
 * What is deliberately left alone is real memory pressure. vtxstoreAllocate()
 * still reaps off-screen corpses when chr vertex memory runs out, and it will
 * find these. A body that is kept is kept until something actually needs the
 * memory back.
 */

/**
 * What one kept body costs, and how much of the stage pool is out of bounds.
 *
 * The slots are the up front half: a chr, a prop, a model, an anim, all from
 * arrays that are sized at stage load and never grow. The rwdata is the running
 * half, and it is what modelmgrInstantiateModel() hands out for a chr model.
 *
 * MODBODIES_MEMFLOOR is what the rest of the level is owed. A body is not worth
 * the last of the stage pool: past this line new bodies simply are not kept and
 * fade the way they always did, which is a feature quietly doing less rather
 * than a match that dies of it.
 */
#define MODBODIES_RWDATA  ((256 + 128) * 4)
#define MODBODIES_BODYCOST (sizeof(struct chrdata) + sizeof(struct prop) \
		+ sizeof(struct model) + sizeof(struct anim) + MODBODIES_RWDATA)
#define MODBODIES_MEMFLOOR (256 * 1024)

/**
 * The share of what is free at stage load that the reserve may take.
 *
 * The simulants themselves are not allocated yet when the reserve is decided -
 * eighty of them cost several megabytes in head modeldefs alone, later - so the
 * free figure at that moment is generous and the fraction has to be modest.
 */
#define MODBODIES_MEMSHARE 8

static s32 g_ModBodiesReserve = 0;

/**
 * Debug: stop drawing kept bodies while leaving them in the world, so the cost
 * of drawing them can be told apart from the cost of having them. Flip it from
 * gdb; nothing sets it.
 */
s32 g_ModBodiesNoDraw = 0;

/**
 * How much further away a kept body is told it is, when its model is choosing
 * a level of detail.
 *
 * Once the frame stops being spent looking for things and starts being spent
 * drawing them, what costs is the triangle: gfx_sp_tri1() writes every one of
 * them out by hand, and a room of bodies is most of them. Batching does not
 * help that - the same triangles go through the same function however they are
 * grouped - so the only thing that does is asking for fewer.
 *
 * A body already has lower detail versions of itself, hanging off the distance
 * nodes its model was built with, and the game is already willing to show them
 * once it is far enough away. This tells the distance nodes the body is further
 * off than it is, so a corpse picks the detail a living chr would get from
 * twice the distance. A body lying on the floor is a thing you walk past, not
 * one you study, and it is the two hundredth of them that costs.
 *
 * Off by default, because it was never actually measured: the body count was
 * still climbing toward the cap while it was being tried, so the frame rate
 * moved for a reason that had nothing to do with it. Standing still in a full
 * room and setting this to 1 and then to 4 is the test it has not had. Until
 * then it does not get to change how anything looks.
 *
 * 1.0 is stock. Tune it from gdb; nothing writes it.
 */
f32 g_ModBodiesLodScale = 1.0f;

/**
 * A body the pool is keeping, or has promised to keep.
 *
 * A simulant's body is claimed when it lands and handed its own chr ninety
 * ticks later, so for that window this is true of the simulant's own chrdata.
 * chrTickDead() reads it there to hold the body at full opacity.
 */
bool modBodyIsKept(struct chrdata *chr)
{
	return chr->keptbody60 >= 0 && chr->actiontype == ACT_DEAD;
}

/**
 * Whether a kept body has already been told to go.
 *
 * A simulant's body is excluded from the fadetimer60 test because that timer is
 * its owner's respawn clock rather than a fade - it is counting for the whole
 * time the body is promised.
 */
static bool modBodyIsGoing(struct chrdata *chr)
{
	if (chr->act_dead.fadenow) {
		return true;
	}

	return chr->aibot == NULL && chr->act_dead.fadetimer60 >= 0;
}

/**
 * Send a body on its way, and stop counting it.
 *
 * The keptbody60 goes first because chrTickDead() holds a claimed body at full
 * opacity: while the claim stands, nothing else can start it fading.
 */
static void modBodyRetire(struct chrdata *chr)
{
	chr->keptbody60 = -1;
	chrFadeCorpse(chr);
}

/**
 * Claim a body as it lands, if the pool has room for it.
 *
 * Claiming happens at death rather than at the handover so that a simulant's
 * body does not spend the respawn window fading out of a pool it is about to
 * join, and so that the age the timer measures is the age of the death.
 *
 * The free chr check is the promise behind the claim: a simulant's body needs a
 * chr slot ninety ticks from now. modBodyHandOff() checks again when the time
 * comes, because other simulants die in between.
 */
void modBodiesClaim(struct chrdata *chr)
{
	chr->keptbody60 = -1;

	if (!modKeepsBodies()) {
		return;
	}

	if (chr->aibot && (chrsGetNumFree() < 2 || mempGetStageFreeTotal() < MODBODIES_MEMFLOOR)) {
		return;
	}

	chr->keptbody60 = g_Vars.lvframe60;
}

/**
 * A second model of the body that is dying, without loading anything.
 *
 * bodyAllocateModel() cannot be used here. In multiplayer it takes the head
 * branch that calls modeldefLoadToNew() every single time - a fresh fifty
 * kilobyte copy of the head, from MEMPOOL_STAGE, never freed, because each
 * simulant's head is offset to fit its own body. Stock pays that eighty times
 * at the start of a match and never again; paying it per death empties the
 * stage pool in about a minute, which is what the first version of this did.
 *
 * Handing body0f02d338() both modeldefs skips both loads. The corpse is the
 * same body and head as the simulant it came from, so the offset that was
 * calculated for it is the right one, and modeldefs are shared read-only data
 * with the per-instance state in rwdata anyway - which is what every chr in a
 * mission already does.
 *
 * The height is copied rather than rolled again: varyheight would give the body
 * a different build to the simulant that was standing there a moment ago.
 */
static struct model *modBodyAllocateModel(struct chrdata *chr)
{
	struct modeldef *bodydef = chr->model->definition;
	struct modeldef *headdef = NULL;
	struct modelnode *node = modelGetPart(bodydef, MODELPART_CHR_HEADSPOT);
	struct model *model;
	s32 headnum = chr->headnum;

	if (node) {
		struct modelrwdata_headspot *rwdata = modelGetNodeRwData(chr->model, node);
		headdef = rwdata->headmodeldef;
	}

	if (headdef) {
		// Only the branch that loads a head reads headnum, and having the
		// modeldef is what skips that branch. chr->headnum is an s8 and the
		// head it was set from is not, so it cannot be trusted to still be
		// positive.
		headnum = 1;
	}

	model = body0f02d338(chr->bodynum, headnum, bodydef, headdef, false, false);

	if (model) {
		modelSetScale(model, chr->model->scale);
	}

	return model;
}

/**
 * Give a simulant's body a chr of its own, so its owner can respawn without it.
 *
 * The fresh chr is allocated at the body's own position - it is a known good
 * spot in a known room, and botSpawn() moves the simulant off it a moment
 * later - and then the two swap what they are wearing. The body keeps the model
 * and prop it died in, with everything that has happened to them; the simulant
 * takes the fresh pair.
 *
 * Only the chrdata fields that describe where the body is lying and what it
 * looks like are copied across. References the dead simulant still holds - its
 * fireslots, its cover, the props it was carrying - are deliberately not, since
 * two chrs pointing at one of those is two chrs freeing it.
 *
 * Returns false if there was nothing to build the body from, in which case the
 * caller respawns the simulant the stock way and the body is lost.
 */
static bool modBodyHandOff(struct chrdata *chr)
{
	struct prop *oldprop = chr->prop;
	struct model *oldmodel = chr->model;
	struct prop *newprop;
	struct model *newmodel;
	struct chrdata *body;
	struct prop *child;

	// chr0f020b14() dereferences the chr chrInit() failed to find, so the free
	// slot has to be there before chrAllocate() is called rather than after.
	if (chrsGetNumFree() < 2) {
		return false;
	}

	// Checked again here, and not only when the body was claimed, because the
	// ninety ticks in between are ninety ticks of other things allocating.
	if (mempGetStageFreeTotal() < MODBODIES_MEMFLOOR) {
		return false;
	}

	newmodel = modBodyAllocateModel(chr);

	if (newmodel == NULL) {
		return false;
	}

	newprop = chrAllocate(newmodel, &oldprop->pos, oldprop->rooms, 0.0f, NULL);

	if (newprop == NULL) {
		modelmgrFreeModel(newmodel);
		return false;
	}

	body = newprop->chr;

	propActivateThisFrame(newprop);
	propEnable(newprop);

	// The guns still in the dead hands are marked the way botSpawn() marks
	// them, before the swap puts them on the body's prop instead of the
	// simulant's. A kept body is unarmed, the same as a body that is not kept.
	child = oldprop->child;

	while (child) {
		struct defaultobj *obj = child->obj;

		if (obj) {
			obj->hidden |= OBJHFLAG_DELETING;
		}

		child = child->next;
	}

	// The swap itself. Model and prop move together and stay a pair, so
	// whichever of them holds the position, the pose and the vertices, both
	// chrs end up consistent.
	body->prop = oldprop;
	body->model = oldmodel;
	oldprop->chr = body;
	oldmodel->chr = body;

	chr->prop = newprop;
	chr->model = newmodel;
	newprop->chr = chr;
	newmodel->chr = chr;

	// Where it is lying. chrAllocate() worked these out for the fresh chr at
	// the position we handed it, which is the right spot, but the dead
	// simulant's are the ones the body has been standing on.
	body->ground = chr->ground;
	body->manground = chr->manground;
	body->sumground = chr->sumground;
	body->floorcol = chr->floorcol;
	body->floortype = chr->floortype;
	body->floorroom = chr->floorroom;
	body->prevpos = chr->prevpos;
	body->radius = chr->radius;
	body->height = chr->height;
	body->chrflags = chr->chrflags | CHRCFLAG_FORCETOGROUND;

	// What it looks like.
	body->headnum = chr->headnum;
	body->bodynum = chr->bodynum;
	body->race = chr->race;
	body->team = chr->team;
	body->squadron = chr->squadron;
	body->fadealpha = 255;
	body->shadecol[0] = chr->shadecol[0];
	body->shadecol[1] = chr->shadecol[1];
	body->shadecol[2] = chr->shadecol[2];
	body->shadecol[3] = chr->shadecol[3];
	body->nextcol[0] = chr->nextcol[0];
	body->nextcol[1] = chr->nextcol[1];
	body->nextcol[2] = chr->nextcol[2];
	body->nextcol[3] = chr->nextcol[3];
	body->cloakfadefrac = chr->cloakfadefrac;
	body->cloakfadefinished = chr->cloakfadefinished;
	body->cloakpause = chr->cloakpause;
	body->drcarollimage_left = chr->drcarollimage_left;
	body->drcarollimage_right = chr->drcarollimage_right;
	body->deathanim = chr->deathanim;
	body->hitpart = chr->hitpart;
	body->specialdie = chr->specialdie;
	body->noblood = chr->noblood;

	// Blood already spilt, so that a body which has bled does not bleed again
	// from the beginning the moment it changes hands.
	body->bulletstaken = chr->bulletstaken;
	body->woundedsplatsadded = chr->woundedsplatsadded;
	body->tickssincesplat = chr->tickssincesplat;
	body->splatsdroppedhere = chr->splatsdroppedhere;
	body->stdsplatsadded = chr->stdsplatsadded;
	body->deaddropsplatsadded = chr->deaddropsplatsadded;
	body->lastdroppos = chr->lastdroppos;

	// Dead, and staying that way. chrBeginDead() is not called for this,
	// because it would claim a fresh place in the pool for a body that already
	// holds one; the claim is transferred instead, with the age of the death
	// it was made at.
	body->actiontype = ACT_DEAD;
	body->act_dead.fadetimer60 = -1;
	body->act_dead.fadenow = false;
	body->act_dead.fadewheninvis = false;
	body->act_dead.invistimer60 = 0;
	body->act_dead.notifychrindex = 0;
	body->ailist = NULL;
	body->sleep = 0;

	body->keptbody60 = chr->keptbody60;
	chr->keptbody60 = -1;

	return true;
}

/**
 * Hand over the bodies of simulants whose respawn is waiting on them, then hold
 * the pool to its cap and its timer.
 *
 * This runs from chraTickBg(), which is the top of propsTick() and the one
 * place in the frame where no prop is part way through its own tick. That
 * matters for the handover: chrTick() holds the prop and the model it read
 * before calling into the action, and swapping either out from under it would
 * leave the rest of that tick working on the wrong body.
 */
void modBodiesTick(void)
{
	s32 numchrs = chrsGetNumSlots();
	s32 cap = modGetBodiesKept();
	s32 time60 = modGetBodyTime() * TICKS(60);
	struct chrdata *oldest = NULL;
	s32 count = 0;
	s32 i;

	for (i = 0; i < numchrs; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr->model == NULL || chr->prop == NULL || chr->actiontype != ACT_DEAD) {
			continue;
		}

		// A simulant held at the end of its window by chrTickDead(), waiting
		// for its body to be taken off it. If the body cannot be kept after
		// all, the simulant still gets up on this tick.
		if (chr->aibot && chr->act_dead.fadetimer60 >= TICKS(90)) {
			if (!modBodyIsKept(chr) || chr->act_dead.fadenow || !modBodyHandOff(chr)) {
				chr->keptbody60 = -1;
				chr->fadealpha = 0;
			}

			botSpawn(chr, true);
			continue;
		}

		if (!modBodyIsKept(chr) || modBodyIsGoing(chr)) {
			continue;
		}

		if (time60 > 0 && g_Vars.lvframe60 - chr->keptbody60 >= time60) {
			modBodyRetire(chr);
			continue;
		}

		count++;

		if (chr->aibot == NULL && (oldest == NULL || chr->keptbody60 - oldest->keptbody60 < 0)) {
			oldest = chr;
		}
	}

	// One a tick is enough to follow a cap the player has just lowered, and it
	// is all that is ever needed for a cap being reached in the ordinary way.
	if (count > cap && oldest) {
		modBodyRetire(oldest);
	}
}

/**
 * The pose a kept body was left in, held in model space.
 *
 * What makes a body expensive to have on screen is not the drawing, which the
 * draw budget in propsSort() already holds down. It is that chrTick() rebuilds
 * the pose from scratch every frame for every body the camera can see: the
 * animation frame is decompressed out of its bit packed form for each joint,
 * the skeleton is walked from the root, and thirty odd matrices are built and
 * concatenated. For a corpse that work produces the same answer every time. A
 * dead chr never advances its animation - chrTick() does not tick one for
 * ACT_DEAD - so framea, frac and the rest are the values the death left behind
 * and stay there.
 *
 * So the answer is computed once and kept. The only part of it that is not the
 * same from one frame to the next is the camera, and the camera is applied
 * exactly once, at the root, as the matrix every other joint is ultimately
 * multiplied by. Capturing the pose with an identity there instead leaves the
 * matrices in model space; multiplying each of them by the real camera on a
 * later frame gives back precisely what the full pass would have produced.
 * That leaves one matrix multiply per joint, and no animation work at all.
 *
 * The pieces of the pipeline that genuinely do depend on the camera are the
 * distance and reorder nodes, which choose a level of detail and a draw order
 * from screen space. Those are not part of the pose and are recomputed every
 * frame by modelUpdateRelations(), on matrices that have the camera in them.
 */
#define MODBODIES_POSEMATRICES 48

/**
 * What the pose was built from.
 *
 * Everything the capture reads that is not fixed for the life of the model. It
 * is compared each frame and the pose is rebuilt when it differs, which is
 * what makes the cache safe rather than merely fast: a body that is shot while
 * it lies there flinches, and a flinch moves the neck and the shoulders for
 * half a second before it settles again.
 *
 * The animation fields are in here too. A corpse's animation does not move, so
 * they never differ in practice, and the handful of bytes they cost per frame
 * buys the cache against ever being wrong if that stops being true.
 */
struct modbodyposekey {
	s16 animnum;
	s16 animnum2;
	s16 framea;
	s16 frameb;
	s16 frame2a;
	s16 frame2b;
	s8 flip;
	s8 flip2;
	u8 dkmode;
	u8 incutscene;
	f32 frac;
	f32 frac2;
	f32 fracmerge;
	f32 animscale;
	f32 scale;
	struct coord pos;
	f32 yrot;
	f32 yrottween;
	f32 yrottweenfrac;
	f32 aimuplshoulder;
	f32 aimuprshoulder;
	f32 aimupback;
	f32 aimsideback;
	f32 angleoffset;
	f32 lookangle;
	s32 flinchcnt;
	u16 hidden2;
};

struct modbodypose {
	// The model the matrices were built for, and the test that a pose belongs
	// to the body asking for it. Chr slots are reused, and a simulant's body
	// is handed a slot of its own partway through its life, so a pose is only
	// ever this model's if it still points at this model.
	struct model *model;
	s32 nummatrices;
	struct modbodyposekey key;
	// The camera the body was held with this frame, copied rather than pointed
	// at because the one chrTick() passes can be a stack local: Dr Caroll is
	// drawn through a camera with an offset built into it. Written every frame
	// a body is held, and read by the draw that comes after the budget.
	Mtxf view;
};

/**
 * The headers and the matrices they describe, in step with each other and both
 * indexed by chr slot.
 *
 * Kept apart rather than one struct holding both because a matrix is sixty
 * four bytes and wants to stay aligned to them: an array of nothing but
 * matrices is, whatever the header happens to be padded to.
 */
static struct modbodypose *g_ModBodiesPoses = NULL;
static Mtxf *g_ModBodiesPoseMatrices = NULL;
static s32 g_ModBodiesNumPoses = 0;

#define MODBODIES_POSESIZE (sizeof(struct modbodypose) \
		+ MODBODIES_POSEMATRICES * sizeof(Mtxf))

/**
 * Debug: counts the bodies turned away because their model has more matrices
 * than a pose slot holds, which is the one way this can quietly do nothing.
 * Read it from gdb; if it is not zero, MODBODIES_POSEMATRICES is too small for
 * something in the stage.
 */
s32 g_ModBodiesPoseTooBig = 0;

/**
 * Set aside a pose for every chr slot, once, at stage load.
 *
 * One per slot rather than a pool with a free list, because a chr slot is
 * already the thing whose lifetime a pose follows, and because the stage pool
 * cannot hand memory back: anything allocated per body as bodies come and go
 * would only ever grow. Indexing by slot also means nothing has to be freed,
 * transferred or invalidated by hand. A pose that finds itself in a slot some
 * other chr has since taken simply does not match the model it is asked about
 * and is rebuilt.
 *
 * Nothing is taken at all when the feature is off, and a failed allocation is
 * not an error: every body then takes the path it took before.
 */
void modBodiesAllocatePoses(s32 numchrslots)
{
	u32 affordable;
	s32 i;

	g_ModBodiesPoses = NULL;
	g_ModBodiesPoseMatrices = NULL;
	g_ModBodiesNumPoses = 0;

	if (!modKeepsBodies()) {
		return;
	}

	// The same share of the stage pool the body reserve answers to, and for
	// the same reason: most of the level has still to load. Falling short is
	// not a failure. The slots that do get a pose are cached and the rest take
	// the path they took before, so a stage that can only afford half of them
	// gets half the saving rather than none of it.
	affordable = mempGetStageFreeTotal() / MODBODIES_MEMSHARE / MODBODIES_POSESIZE;

	if (affordable > (u32)numchrslots) {
		affordable = numchrslots;
	}

	if (affordable == 0) {
		return;
	}

	g_ModBodiesPoseMatrices = mempAlloc(
			ALIGN16(affordable * MODBODIES_POSEMATRICES * sizeof(Mtxf)), MEMPOOL_STAGE);

	if (g_ModBodiesPoseMatrices == NULL) {
		return;
	}

	g_ModBodiesPoses = mempAlloc(ALIGN16(affordable * sizeof(struct modbodypose)), MEMPOOL_STAGE);

	if (g_ModBodiesPoses == NULL) {
		g_ModBodiesPoseMatrices = NULL;
		return;
	}

	g_ModBodiesNumPoses = affordable;

	for (i = 0; i < g_ModBodiesNumPoses; i++) {
		g_ModBodiesPoses[i].model = NULL;
	}
}

/**
 * Read off everything the capture depends on.
 */
static void modBodyPoseReadKey(struct chrdata *chr, struct modbodyposekey *key)
{
	struct model *model = chr->model;
	struct modeldef *modeldef = model->definition;
	struct anim *anim = model->anim;

	memset(key, 0, sizeof(*key));

	key->scale = model->scale;
	key->dkmode = cheatIsActive(CHEAT_DKMODE);
	key->incutscene = g_Vars.in_cutscene;

	if (anim) {
		key->animnum = anim->animnum;
		key->animnum2 = anim->animnum2;
		key->framea = anim->framea;
		key->frameb = anim->frameb;
		key->frame2a = anim->frame2a;
		key->frame2b = anim->frame2b;
		key->flip = anim->flip;
		key->flip2 = anim->flip2;
		key->frac = anim->frac;
		key->frac2 = anim->frac2;
		key->fracmerge = anim->fracmerge;
		key->animscale = anim->animscale;
	}

	// Where the body is lying and which way it is facing, which the root node
	// holds rather than the prop.
	if ((modeldef->rootnode->type & 0xff) == MODELNODETYPE_CHRINFO) {
		struct modelrwdata_chrinfo *rwdata = modelGetNodeRwData(model, modeldef->rootnode);

		key->pos = rwdata->pos;
		key->yrot = rwdata->yrot;
		key->yrottween = rwdata->unk1c;
		key->yrottweenfrac = rwdata->unk18;
	}

	// The aim and flinch rotations chrHandleJointPositioned() lays over the
	// shoulders, waist and neck. The aim ones stop moving once aimendcount has
	// run down; the flinch one runs for half a second after a hit and is the
	// reason this is checked at all.
	key->aimuplshoulder = chr->aimuplshoulder;
	key->aimuprshoulder = chr->aimuprshoulder;
	key->aimupback = chr->aimupback;
	key->aimsideback = chr->aimsideback;
	key->flinchcnt = chr->flinchcnt;
	key->hidden2 = chr->hidden2;

	// A simulant's body spends its first ninety ticks still owned by the
	// simulant, and it is the aibot that holds which way it is facing over
	// that window rather than the root node.
	if (chr->aibot) {
		key->angleoffset = chr->aibot->angleoffset;
		key->lookangle = chr->aibot->lookangle;
	}
}

/**
 * Find a body's pose slot, capturing the pose into it first if what is there
 * does not describe the body as it is lying now.
 *
 * The capture runs the same pipeline the caller would have run, with the same
 * renderdata, differing only in where the matrices land and in the identity
 * standing in for the camera. That matters: it means a captured pose is not an
 * approximation of the real one but the same computation with the last
 * multiply left off, and everything hanging off it - the joint callback, the
 * head, the held positions - behaves exactly as it does on any other frame.
 *
 * It has to happen here, in the tick, rather than later when the body is known
 * to be drawn, because it is the tick that has set up g_CurModelChr and the
 * joint callback the capture reads.
 *
 * Returns NULL for a body that cannot have a pose, which then takes the
 * ordinary path.
 */
static struct modbodypose *modBodyPoseCapture(struct chrdata *chr, struct modelrenderdata *renderdata)
{
	struct model *model = chr->model;
	s32 nummatrices = model->definition->nummatrices;
	struct modbodypose *pose;
	struct modbodyposekey key;
	s32 index;

	if (g_ModBodiesPoses == NULL || renderdata->unk00 == NULL || !modBodyIsKept(chr)) {
		return NULL;
	}

	if (nummatrices > MODBODIES_POSEMATRICES) {
		g_ModBodiesPoseTooBig++;
		return NULL;
	}

	index = chr - g_ChrSlots;

	if (index < 0 || index >= g_ModBodiesNumPoses) {
		return NULL;
	}

	pose = &g_ModBodiesPoses[index];
	modBodyPoseReadKey(chr, &key);

	if (pose->model != model
			|| pose->nummatrices != nummatrices
			|| memcmp(&pose->key, &key, sizeof(key)) != 0) {
		struct modelrenderdata capture = *renderdata;
		Mtxf identity;

		mtx4LoadIdentity(&identity);

		capture.unk00 = &identity;
		capture.unk10 = &g_ModBodiesPoseMatrices[index * MODBODIES_POSEMATRICES];

		g_ModelPoseCapture = true;
		modelSetMatricesWithAnim(&capture, model);
		g_ModelPoseCapture = false;

		pose->model = model;
		pose->nummatrices = nummatrices;

		// memcpy rather than an assignment so that the padding is copied too,
		// because the padding is part of what the memcmp above reads.
		memcpy(&pose->key, &key, sizeof(key));
	}

	return pose;
}

/**
 * Put the camera back on a pose, into matrices from this frame's pool.
 *
 * modelSetMatrices() would have done this as part of building each joint; all
 * that is left of it here is the multiply.
 */
static void modBodyPoseConcat(struct chrdata *chr, struct modbodypose *pose, Mtxf *view, Mtxf *matrices)
{
	struct model *model = chr->model;
	Mtxf *cached = &g_ModBodiesPoseMatrices[(pose - g_ModBodiesPoses) * MODBODIES_POSEMATRICES];
	s32 i;

	model->matrices = matrices;

	for (i = 0; i < pose->nummatrices; i++) {
		mtx00015be4(view, &cached[i], &matrices[i]);
	}

	// Level of detail and draw order, which are the parts of the pass that do
	// depend on where the camera is. The capture left every distance node
	// showing all of its children so that the matrices below them were all
	// written; this is what chooses between them for the frame being drawn.
	if (g_ModBodiesLodScale != 1.0f) {
		f32 prevscale = g_ModelDistanceScale;

		modelSetDistanceScale(prevscale * g_ModBodiesLodScale);
		modelUpdateRelations(model);
		modelSetDistanceScale(prevscale);
	} else {
		modelUpdateRelations(model);
	}
}

/**
 * Build this frame's matrices for a kept body from its pose.
 *
 * Returns false if the body has no pose to work with, in which case the caller
 * builds the matrices the ordinary way.
 */
bool modBodyPoseApply(struct chrdata *chr, struct modelrenderdata *renderdata)
{
	struct modbodypose *pose = modBodyPoseCapture(chr, renderdata);
	Mtxf *matrices;

	if (pose == NULL) {
		return false;
	}

	matrices = renderdata->unk10;
	renderdata->unk10 += pose->nummatrices;

	modBodyPoseConcat(chr, pose, renderdata->unk00, matrices);

	return true;
}

/**
 * Hold a kept body's matrices back until the draw budget has spoken.
 *
 * Even reduced to a multiply per joint, a body that is not going to be drawn
 * is paying for matrices nothing reads, and two kilobytes of the frame's pool
 * to put them in - which is the pool that runs out and starts taking bodies
 * off the screen. The budget knows which bodies those are, but not until
 * propsSort() has put the frame in depth order, which is after every prop has
 * ticked. So the tick stops at the one thing the sort needs, which is how far
 * away the body is, and modBodyPoseDraw() finishes the job for the bodies that
 * win a place.
 *
 * A body that is still holding something is not held back. Its child prop is
 * positioned from one of these matrices during the same tick, so they have to
 * exist by the end of it. Simulant bodies have had theirs taken off them at
 * the handover and always qualify; a body in a mission may still have the gun
 * it died with.
 *
 * Returns true if the matrices were held, in which case the caller must leave
 * prop->z alone - it is set here - and must not read model->matrices for the
 * rest of the tick.
 */
bool modBodyPoseHold(struct chrdata *chr, struct modelrenderdata *renderdata)
{
	struct prop *prop = chr->prop;
	struct modbodypose *pose;
	Mtxf *cached;
	Mtxf rootmtx;
	s32 rootindex;

	chr->bodyposeheld = false;

	if (prop->child || chr->weapons_held[2]) {
		return false;
	}

	rootindex = modelFindNodeMtxIndex(chr->model->definition->rootnode, 0);

	if (rootindex < 0) {
		return false;
	}

	pose = modBodyPoseCapture(chr, renderdata);

	if (pose == NULL) {
		return false;
	}

	mtx4Copy(renderdata->unk00, &pose->view);

	// What modelGetScreenDistance() would have returned, which reads the root
	// joint and nothing else. One multiply rather than the whole skeleton.
	cached = &g_ModBodiesPoseMatrices[(pose - g_ModBodiesPoses) * MODBODIES_POSEMATRICES];
	mtx00015be4(&pose->view, &cached[rootindex], &rootmtx);
	prop->z = -rootmtx.m[3][2];

	chr->bodyposeheld = true;

	return true;
}

/**
 * Finish a held body that has won a place in the frame's draw budget.
 *
 * Called from propsSort(), which is the first moment the answer is known and
 * still early enough that the frame's pool is open.
 *
 * Returns false if the body could not be given matrices after all, which the
 * caller has to treat the same way it treats a body that lost the budget:
 * there is nothing for the frame to draw or to test a shot against. A body
 * that was never held returns true, because it already has its matrices.
 */
bool modBodyPoseDraw(struct chrdata *chr)
{
	struct model *model = chr->model;
	struct modbodypose *pose;
	u32 size;

	if (!chr->bodyposeheld) {
		return true;
	}

	chr->bodyposeheld = false;

	pose = &g_ModBodiesPoses[chr - g_ChrSlots];
	size = model->definition->nummatrices * sizeof(Mtxf);

	// The same answer chrTick() gives when the pool has run out: a body that
	// cannot be given matrices comes off this frame's screen rather than being
	// drawn from a pointer past the end of it.
	if (!gfxHasVtxSpace(size)) {
		modBodyPoseDrop(chr);
		return false;
	}

	modBodyPoseConcat(chr, pose, &pose->view, gfxAllocate(size));

	return true;
}

/**
 * Take a held body that is not being drawn off this frame's screen.
 *
 * It has no matrices for this frame and is not getting any, so it is put in
 * the state an off screen body is already in, which everything downstream
 * knows how to leave alone.
 */
void modBodyPoseDrop(struct chrdata *chr)
{
	chr->bodyposeheld = false;
	chr->prop->flags &= ~PROPFLAG_ONTHISSCREENTHISTICK;
}

/**
 * Chr, model, anim and prop slots to set aside at stage load for bodies that
 * outlive their owner.
 *
 * Only simulants need them. A body left behind in a mission is the chr that
 * died, which the setup file already paid for.
 */
s32 modBodiesGetReserve(void)
{
	return g_ModBodiesReserve;
}

/**
 * Decide the reserve for the stage about to load, and remember it.
 *
 * Called once, from setupLoadFiles(), before either pool is sized; everything
 * downstream reads the number back rather than working it out again, because
 * the two sites that need it are separated by a good deal of allocation and
 * arrays that disagree about how many chrs there are would be worse than a
 * small reserve.
 *
 * The cap is what the player asked for. This is what the stage pool can carry,
 * which on the default 16MB of Game.MemorySize and eighty simulants is rather
 * less. Raising Game.MemorySize is what buys the rest.
 */
s32 modBodiesSetReserve(void)
{
	u32 affordable;

	g_ModBodiesReserve = 0;

	if (!g_Vars.normmplayerisrunning) {
		return 0;
	}

	g_ModBodiesReserve = modGetBodiesKept();
	affordable = mempGetStageFreeTotal() / MODBODIES_MEMSHARE / MODBODIES_BODYCOST;

	if ((u32)g_ModBodiesReserve > affordable) {
		g_ModBodiesReserve = affordable;
	}

	return g_ModBodiesReserve;
}
