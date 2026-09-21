#ifndef _IN_GEXPLUSVEH_H
#define _IN_GEXPLUSVEH_H

#include <ultra64.h>
#include <PR/ultratypes.h>
#include "types.h"

/**
 * GoldenEye's own vehicles in a converted mission (port/src/gexplusveh.c).
 *
 * Perfect Dark still carries GoldenEye's truck and aircraft records -
 * `OBJTYPE_TRUCK` and `OBJTYPE_HELI` are its `VehichleRecord` and
 * `AircraftRecord` field for field, and `setupCreateProps()` still builds one -
 * but nothing has ticked either since GoldenEye: the hovercar and the chopper
 * took over and the two types are a fossil. The seventeen in the twenty
 * missions (Dam's truck, Streets' eight, Runway's plane, four helicopters and
 * two tanks) stood still and silent.
 *
 * These are GoldenEye's own ticks, on Perfect Dark's own fields.
 */

// One vehicle prop, from objTick().
void gexPlusVehicleTick(struct prop *prop);

// And its spinning parts, from the same place the fan's model is updated.
void gexPlusVehicleUpdateModel(struct prop *prop);

/** Whether the prop is a converted mission's aircraft with one of its animations on its model. */
s32 gexPlusVehicleFliesAnim(struct prop *prop);

// A node of a vehicle's model turned on its own pivot (a wheel, a rotor, a turret)
void gexPlusVehiclePutPart(struct model *model, s32 partnum, Mtxf *rot);

#endif
