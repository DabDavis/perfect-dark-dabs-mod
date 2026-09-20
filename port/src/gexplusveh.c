/**
 * GoldenEye's own vehicles in a converted mission.
 *
 * Perfect Dark still carries GoldenEye's two vehicle records - `truckobj` is
 * its `VehichleRecord` and `heliobj` its `AircraftRecord`, field for field in
 * the same order - and `setupCreateProps()` still builds a prop for each,
 * resolving the AI list id the record holds. What it does not do is **tick**
 * one: nothing outside chrai.c so much as mentions `g_Vars.truck` or
 * `g_Vars.heli`, because the hovercar and the chopper took the job over and
 * the two older types are a fossil of the game Perfect Dark grew out of.
 *
 * So the seventeen vehicles in the twenty missions stood still and silent -
 * Dam's truck, Streets' eight parked ones, Runway's plane, the helicopters on
 * Frigate, Statue Park, Surface 2 and the Cradle, and the two tanks (which
 * Perfect Dark does not build at all: `OBJTYPE_TANK` is counted by
 * `setupCreateProps()` and has no case there, GoldenEye's tank being a thing
 * the player drives).
 *
 * What is here is GoldenEye's own tick (propobj.c, `PROPDEF_VEHICHLE` and
 * `PROPDEF_AIRCRAFT`) written against Perfect Dark's fields. Its three AI
 * commands need nothing: `VehicleStartPath`, `VehicleSpeed` and
 * `AircraftRotorSpeed` are Perfect Dark's own `aiHovercarBeginPath`,
 * `aiSetVehicleSpeed` and `aiSetRotorSpeed` line for line, and the first two
 * already write `g_Vars.truck`.
 */
#include <ultra64.h>
#include <math.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "lib/vars.h"
#include "lib/mtx.h"
#include "lib/model.h"
#include "game/chrai.h"
#include "game/lv.h"
#include "game/pad.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/bg.h"
#include "lib/collision.h"
#include "game/setup.h"
#include "modloader.h"
#include "gexplus.h"
#include "geaitable.h"
#include "geanimtable.h"
#include "gexplusveh.h"

#ifndef PLATFORM_N64

/**
 * GoldenEye's own ramp, which both vehicles use for every speed they have: the
 * value walks towards its aim over the time left, and arrives when the time
 * runs out.
 */
static void vehRamp(f32 *value, f32 aim, f32 *time60, f32 delta)
{
	if (*time60 >= 0.0f) {
		if (*time60 <= delta) {
			*value = aim;
		} else {
			*value += (aim - *value) * delta / *time60;
		}

		*time60 -= delta;
	}
}

/**
 * Whether the aircraft is flying GoldenEye's `plane_runway`, which is the one
 * of the three authored at ten times the size and facing the other way
 * (propobj.c compares the model's animation against
 * `animation_table_ptrs2[1]`). The number is whatever the conversion's
 * animation took when it was appended, so the id goes through the same lookup
 * the AI command uses.
 */
static s32 vehAnimIsPlane(struct model *model)
{
	const s32 ours = gexPlusMissionAnim(GEAI_ANIM_TAG | (GEVEH_ANIM_FIRST + 1));

	return ours > 0 && model->anim && model->anim->animnum == ours;
}

static f32 vehWrapTau(f32 angle)
{
	while (angle >= M_BADTAU) {
		angle -= M_BADTAU;
	}

	while (angle < 0.0f) {
		angle += M_BADTAU;
	}

	return angle;
}

/**
 * The aircraft: its AI list, its speed and the rotor's.
 *
 * GoldenEye advances the rotor's own angle in the *render*, once a drawn frame
 * (`rotoryrot += rotaryspeed`), and so does gexPlusVehicleUpdateModel() below.
 */
static void vehHeliTick(struct prop *prop)
{
	struct heliobj *heli = (struct heliobj *)prop->obj;
	struct model *model = heli->base.model;
	const f32 delta = g_Vars.lvupdate60freal;

	chraiExecute(heli, PROPTYPE_OBJ);

	// The animation its list asked for, which is how GoldenEye flies an
	// aircraft: the model is ticked and its **root motion** is what moves the
	// prop. GoldenEye's three calls are Perfect Dark's own under other names -
	// `setsuboffset` is modelSetRootPosition(), `subcalcpos` is
	// modelUpdateInfo() and `getsuboffset` is modelGetRootPosition() - and the
	// height comes from the record's own pad rather than from the motion, the
	// animation's y being an offset from where the aircraft was placed.
	if (model && model->anim) {
		struct coord pos;
		struct pad pad;

		pos.x = prop->pos.x;
		pos.y = prop->pos.y;
		pos.z = prop->pos.z;

		// GoldenEye's own two scales: the runway plane is authored ten times
		// the size of the rest, and its shot faces the other way
		if (vehAnimIsPlane(model)) {
			modelSetAnimScale(model, 10.438f);
			modelSetChrRotY(model, M_BADPI);
		} else {
			modelSetAnimScale(model, 1.0438f);
			modelSetChrRotY(model, 0);
		}

		modelSetRootPosition(model, &pos);
		modelTickAnim(model, g_Vars.lvupdate240, true);
		modelUpdateInfo(model);
		modelGetRootPosition(model, &pos);

		prop->pos.x = pos.x;
		prop->pos.z = pos.z;

		padUnpack(heli->base.pad, PADFIELD_POS, &pad);
		prop->pos.y = pad.pos.y + pos.y;
		pos.y = prop->pos.y;
		modelSetRootPosition(model, &pos);

		propDeregisterRooms(prop);
		propRegisterRooms(prop);
	}

	vehRamp(&heli->speed, heli->speedaim, &heli->speedtime60, delta);
	vehRamp(&heli->rotoryspeed, heli->rotoryspeedaim, &heli->rotoryspeedtime, delta);
}

/**
 * The truck: its AI list, its speed, and the path it was told to follow.
 *
 * GoldenEye steers towards the pad its path is heading for and walks the stan
 * forward, refusing a step that leaves the walkable surface. Perfect Dark's own
 * `hovercarTick()` is that function's descendant and does the same thing
 * through `cd*`; what is kept here is the steering and the arrival, with the
 * ground asked for the height so the truck sits on the road rather than flying
 * the pads' own line.
 */
static union modelrodata *vehPartRodata(struct model *model, s32 partnum, u32 type);

/** How far a truck's origin rides over the ground: the bottom of its front wheel, turned up. */
static f32 vehTruckClearance(struct model *model)
{
	union modelrodata *wheel;
	union modelrodata *box;

	if (!model) {
		return 0.0f;
	}

	wheel = vehPartRodata(model, 1, MODELNODETYPE_POSITION);
	box = vehPartRodata(model, 6, MODELNODETYPE_BBOX);

	if (!wheel || !box) {
		return 0.0f;
	}

	return -(box->bbox.ymin + wheel->position.pos.y) * model->scale;
}

/**
 * The truck's matrix from its heading, which is GoldenEye's own
 * (sub_GAME_7F044B38()'s level branch: a rotation about y, times the scale).
 *
 * `realrot` is not a rotation: Perfect Dark folds the object's own scale into
 * it, and a prop placed at a tenth carries rows a tenth long (Streets' jeeps:
 * 0.100, against the 0.10987 of Dam's truck). So the scale is read off the
 * matrix that is there before the heading is written over it - a bare unit
 * rotation draws the truck nine times its size, which was the grey slab across
 * the dam, and writing only its four horizontal terms leaves something that is
 * not even a rotation.
 */
static void vehTruckFace(struct truckobj *truck)
{
	f32 (*m)[3] = truck->base.realrot;
	const f32 scale = sqrtf(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
	Mtxf rot;
	s32 i;
	s32 j;

	mtx4LoadYRotation(truck->roty, &rot);
	mtx4ToMtx3(&rot, truck->base.realrot);

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			m[i][j] *= scale;
		}
	}
}

static void vehTruckTick(struct prop *prop)
{
	struct truckobj *truck = (struct truckobj *)prop->obj;
	const f32 delta = g_Vars.lvupdate60freal;
	struct coord target;
	struct coord next;
	RoomNum rooms[8];
	f32 aimangle = 0.0f;
	f32 haspath = false;
	f32 turnedby = 0.0f;
	struct pad pad;

	chraiExecute(truck, PROPTYPE_OBJ);

	vehRamp(&truck->speed, truck->speedaim, &truck->speedtime60, delta);

	if (truck->path && truck->path->pads[truck->nextstep] >= 0) {
		padUnpack(truck->path->pads[truck->nextstep], PADFIELD_POS, &pad);
		target.x = pad.pos.x;
		target.y = pad.pos.y;
		target.z = pad.pos.z;
		haspath = true;
	}

	if (haspath) {
		aimangle = atan2f(target.x - prop->pos.x, target.z - prop->pos.z);

		if (aimangle < 0.0f) {
			aimangle += M_BADTAU;
		}
	}

	// GoldenEye's own first tick (propobj.c, `PROPFLAG_INMOTION`, which is this
	// bit and which a vehicle's record carries): the heading is **not** in the
	// record - both games zero `roty` at the load - so it is taken from the
	// path the truck has been given, or from the matrix it was placed with.
	// Without it the heading started at nought while the model stood as it was
	// placed, the steering turned both by the same amounts, and Dam's truck
	// drove its whole route pointing that far off the way it was going.
	if (truck->base.flags & OBJFLAG_CHOPPER_INIT) {
		truck->base.flags &= ~OBJFLAG_CHOPPER_INIT;
		truck->roty = haspath ? aimangle
			: vehWrapTau(atan2f(truck->base.realrot[2][0], truck->base.realrot[2][2]));
		vehTruckFace(truck);
	}

	if (truck->speed <= 0.0f) {
		return;
	}

	if (haspath) {
		// GoldenEye's own: steer towards the pad the path is heading for, at
		// the turn rate its truck has and no faster
		{
			f32 diff = aimangle - truck->roty;

			while (diff > M_BADTAU * 0.5f) {
				diff -= M_BADTAU;
			}

			while (diff < -M_BADTAU * 0.5f) {
				diff += M_BADTAU;
			}

			// the turn is capped the way GoldenEye caps it, a twentieth of a
			// turn a second, so a truck rounds a corner rather than pivoting
			{
				const f32 most = 0.05f * M_BADTAU / 60.0f * delta;

				if (diff > most) {
					diff = most;
				} else if (diff < -most) {
					diff = -most;
				}
			}

			truck->turnrot60 = diff / (delta > 0.0f ? delta : 1.0f);
			truck->roty = vehWrapTau(truck->roty + diff);
			turnedby = diff;
		}
	}

	next.x = prop->pos.x + sinf(truck->roty) * truck->speed * delta;
	next.y = prop->pos.y;
	next.z = prop->pos.z + cosf(truck->roty) * truck->speed * delta;

	// The ground under the step, the way a chr's move asks for it: a converted
	// road is not level and the pads are on it, not above it.
	{
		RoomNum inrooms[21];
		RoomNum aboverooms[21];
		RoomNum bestroom;
		f32 ground;

		bgFindRoomsByPos(&next, inrooms, aboverooms, 20, &bestroom);

		if (inrooms[0] != -1) {
			ground = cdFindGroundInfoAtCyl(&next, 30, inrooms, NULL, NULL, NULL, NULL, NULL, NULL);

			if (ground > -1000000.0f) {
				// GoldenEye's own height (sub_GAME_7F044B38()'s level branch):
				// the ground less the bottom of the front wheel, which is the
				// wheel's box under the wheel's node (parts 6 and 1), at the
				// model's scale. The bare ground put the truck's *origin* on
				// the road and its wheels fifty units under it.
				next.y = ground + vehTruckClearance(truck->base.model);
			}

			roomsCopy(inrooms, rooms);
		} else {
			return;
		}
	}

	prop->pos.x = next.x;
	prop->pos.y = next.y;
	prop->pos.z = next.z;

	if (turnedby != 0.0f) {
		vehTruckFace(truck);
	}

	propDeregisterRooms(prop);
	roomsCopy(rooms, prop->rooms);
	propRegisterRooms(prop);

	// Arrived at the pad: the next one, and the path's end stops the truck
	// over a second, which is GoldenEye's own wind-down
	if (haspath) {
		const f32 dx = target.x - prop->pos.x;
		const f32 dz = target.z - prop->pos.z;

		if (dx * dx + dz * dz < 100.0f * 100.0f) {
			truck->nextstep++;

			if (truck->path->pads[truck->nextstep] < 0) {
				truck->path = NULL;
				truck->speedaim = 0;
				truck->speedtime60 = 60;
			}
		}
	}
}

void gexPlusVehicleTick(struct prop *prop)
{
	struct defaultobj *obj = prop->obj;

	if (!modloaderStageIsMission(g_Vars.stagenum)) {
		return;
	}

	if (obj->type == OBJTYPE_HELI) {
		vehHeliTick(prop);
	} else if (obj->type == OBJTYPE_TRUCK) {
		vehTruckTick(prop);
	}
}

/**
 * A node of the model, turned on its own matrix and put back under the body's.
 *
 * GoldenEye's three lines for every spinning part it has: the rotation, then
 * the node's own position into that matrix, then the whole thing multiplied by
 * the model's own (propobj.c, `matrix_4x4_set_position` and
 * `matrix_4x4_multiply_homogeneous_in_place`).
 */
static void vehPutPart(struct model *model, s32 partnum, Mtxf *rot)
{
	struct modelnode *node = modelGetPart(model->definition, partnum);
	struct modelrodata_position *rodata;
	Mtxf local;
	Mtxf *parent;

	if (node == NULL || (node->type & 0xff) != MODELNODETYPE_POSITION) {
		return;
	}

	rodata = &node->rodata->position;

	// Perfect Dark's own, from the branch of modelUpdatePositionNodeMtx() that
	// runs when a model has no animation: the node's position is a
	// **translation in the model's own units**, multiplied by the parent's
	// matrix - which is where the model's scale lives - and written to
	// `matrices[mtxindex0]`. Doing any of those three by hand instead (the
	// root's matrix for the parent, mtx4MultMtx4InPlace, the matrix
	// modelFindNodeMtx hands back) puts the part somewhere else entirely, and
	// a truck's four wheels then draw as slabs across the level.
	parent = node->parent ? modelFindNodeMtx(model, node->parent, 0) : NULL;

	mtx4Copy(rot, &local);
	mtx4SetTranslation(&rodata->pos, &local);

	if (parent) {
		mtx00015be4(parent, &local, &model->matrices[rodata->mtxindex0]);
	} else {
		mtx4Copy(&local, &model->matrices[rodata->mtxindex0]);
	}
}

/** A part's rodata, where it is the type wanted, or NULL. */
static union modelrodata *vehPartRodata(struct model *model, s32 partnum, u32 type)
{
	struct modelnode *node = modelGetPart(model->definition, partnum);

	return node && (node->type & 0xff) == type ? node->rodata : NULL;
}

/**
 * The helicopter's rotor.
 *
 * GoldenEye turns it in the render pass rather than the tick, once a drawn
 * frame: part 2 is the main rotor, turned about y, and part 3 the tail rotor,
 * turned about x. A model's parts keep GoldenEye's own numbering through the
 * conversion - `gemodelconv.py` writes its switch table in order - so the two
 * are found by number here as they are there.
 *
 * (GoldenEye turns the main rotor about **z** instead while the prop carries
 * `PROPFLAG_INMOTION`, which is the flag a *thrown* object has; none of the
 * twenty missions throws an aircraft.)
 */
static void vehHeliUpdateModel(struct prop *prop)
{
	struct heliobj *heli = (struct heliobj *)prop->obj;
	struct model *model = heli->base.model;
	Mtxf rot;

	if (g_Vars.lvupdate240 > 0) {
		heli->rotoryrot = vehWrapTau(heli->rotoryrot + heli->rotoryspeed);
	}

	mtx4LoadYRotation(heli->rotoryrot, &rot);
	vehPutPart(model, 2, &rot);

	mtx4LoadXRotation(heli->rotoryrot, &rot);
	vehPutPart(model, 3, &rot);
}

/**
 * The truck's four wheels: they roll with the road and the front two steer.
 *
 * Parts 1 and 2 are the front wheels and 3 and 4 the rear, and part 6 is a
 * front wheel's own bounding box, whose **height is the wheel's diameter** -
 * which is where the rolling comes from, GoldenEye turning the wheel by the
 * distance it covered over its radius rather than by any authored rate. The
 * steering angle is GoldenEye's own arithmetic over the wheelbase (the z
 * between a rear wheel and a front one) and the rate the truck is turning at,
 * held to at least that rate and mirrored when it turns the other way.
 *
 * GoldenEye adds the rolling angle **twice** where it owns the simulation
 * (propobj.c: once inside the `isSimOwner` test and again after it), which
 * would spin the wheels at twice the road speed; the distance over the radius
 * is added once here, which is the geometry both games' numbers describe.
 */
static void vehTruckUpdateModel(struct prop *prop)
{
	struct truckobj *truck = (struct truckobj *)prop->obj;
	struct model *model = truck->base.model;
	union modelrodata *wheelbox = vehPartRodata(model, 6, MODELNODETYPE_BBOX);
	union modelrodata *front = vehPartRodata(model, 1, MODELNODETYPE_POSITION);
	union modelrodata *rear = vehPartRodata(model, 3, MODELNODETYPE_POSITION);
	Mtxf roll;
	Mtxf steer;

	if (wheelbox && g_Vars.lvupdate240 > 0) {
		const f32 diameter = (wheelbox->bbox.ymax - wheelbox->bbox.ymin) * model->scale;

		if (diameter > 0.0f) {
			truck->wheelxrot = vehWrapTau(truck->wheelxrot
					+ truck->speed * g_Vars.lvupdate60freal / (diameter * 0.5f));
		}
	}

	if (truck->speed > 0.0f && front && rear) {
		const f32 wheelbase = (rear->position.pos.z - front->position.pos.z) * model->scale;
		const f32 rate = truck->turnrot60 < 0.0f ? -truck->turnrot60 : truck->turnrot60;

		truck->wheelyrot = atan2f(sinf(rate) * wheelbase,
				cosf(rate) * wheelbase - (wheelbase - truck->speed));

		if (truck->wheelyrot < rate) {
			truck->wheelyrot = rate;
		}

		if (truck->turnrot60 > 0.0f) {
			truck->wheelyrot = M_BADTAU - truck->wheelyrot;
		}
	}

	// the rear two roll and nothing else; the front two roll inside their
	// steering, which is the order GoldenEye multiplies them in
	mtx4LoadXRotation(truck->wheelxrot, &roll);
	vehPutPart(model, 3, &roll);
	vehPutPart(model, 4, &roll);

	mtx4LoadYRotation(truck->wheelyrot, &steer);
	mtx4MultMtx4InPlace(&steer, &roll);
	vehPutPart(model, 1, &roll);
	vehPutPart(model, 2, &roll);
}

void gexPlusVehicleUpdateModel(struct prop *prop)
{
	struct defaultobj *obj = prop->obj;

	if (obj->model == NULL || obj->model->definition == NULL) {
		return;
	}

	if (obj->type == OBJTYPE_HELI) {
		vehHeliUpdateModel(prop);
	} else if (obj->type == OBJTYPE_TRUCK) {
		vehTruckUpdateModel(prop);
	}
}

#endif
