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
	const f32 delta = g_Vars.lvupdate60freal;

	chraiExecute(heli, PROPTYPE_OBJ);

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
static void vehTruckTick(struct prop *prop)
{
	struct truckobj *truck = (struct truckobj *)prop->obj;
	const f32 delta = g_Vars.lvupdate60freal;
	struct coord target;
	struct coord next;
	RoomNum rooms[8];
	f32 aimangle;
	f32 haspath = false;
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

	if (truck->speed <= 0.0f) {
		return;
	}

	if (haspath) {
		// GoldenEye's own: steer towards the pad the path is heading for, at
		// the turn rate its truck has and no faster
		aimangle = atan2f(target.x - prop->pos.x, target.z - prop->pos.z);

		if (aimangle < 0.0f) {
			aimangle += M_BADTAU;
		}

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
				next.y = ground;
			}

			roomsCopy(inrooms, rooms);
		} else {
			return;
		}
	}

	prop->pos.x = next.x;
	prop->pos.y = next.y;
	prop->pos.z = next.z;
	truck->base.realrot[0][0] = cosf(truck->roty);
	truck->base.realrot[0][2] = -sinf(truck->roty);
	truck->base.realrot[2][0] = sinf(truck->roty);
	truck->base.realrot[2][2] = cosf(truck->roty);

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
 * The rotor, and the wheels.
 *
 * GoldenEye turns the rotor in the render pass rather than the tick, once a
 * drawn frame, and puts the rotation on the node's own matrix: part 2 is the
 * main rotor, turned about y, and part 3 the tail rotor, turned about x
 * (propobj.c's `PROPDEF_AIRCRAFT` render). A model's parts keep GoldenEye's own
 * numbering through the conversion - `gemodelconv.py` writes its switch table
 * in order - so the two parts are found by number here as they are there.
 *
 * (GoldenEye turns the main rotor about **z** instead while the prop carries
 * `PROPFLAG_INMOTION`, which is the flag a *thrown* object has; none of the
 * twenty missions throws an aircraft.)
 */
void gexPlusVehicleUpdateModel(struct prop *prop)
{
	struct defaultobj *obj = prop->obj;
	struct heliobj *heli = (struct heliobj *)obj;
	struct model *model = obj->model;
	struct modelnode *node;
	Mtxf rot;
	s32 i;

	if (obj->type != OBJTYPE_HELI || model == NULL || model->definition == NULL) {
		return;
	}

	if (g_Vars.lvupdate240 > 0) {
		heli->rotoryrot = vehWrapTau(heli->rotoryrot + heli->rotoryspeed);
	}

	for (i = 2; i <= 3; i++) {
		struct modelrodata_position *rodata;
		Mtxf *mtx;

		node = modelGetPart(model->definition, i);

		if (node == NULL || (node->type & 0xff) != MODELNODETYPE_POSITION) {
			continue;
		}

		mtx = modelFindNodeMtx(model, node, 0);

		if (mtx == NULL) {
			continue;
		}

		rodata = &node->rodata->position;

		if (i == 2) {
			mtx4LoadYRotation(heli->rotoryrot, &rot);
		} else {
			mtx4LoadXRotation(heli->rotoryrot, &rot);
		}

		mtx4SetTranslation(&rodata->pos, &rot);
		mtx4MultMtx4InPlace(&model->matrices[0], &rot);
		*mtx = rot;
	}
}

#endif
