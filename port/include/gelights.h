#ifndef _IN_GELIGHTS_H
#define _IN_GELIGHTS_H

#include <ultra64.h>
#include "types.h"

/**
 * GoldenEye's light fixtures on a converted level (the remake stages).
 *
 * GoldenEye breaks a fixture when a shot lands on any triangle drawn with a
 * light texture, darkens the whole fixture (each vertex colour >> 2) and
 * sheds glass off it (lightfixture.c). The conversion gives each fixture one
 * or more Perfect Dark lights, a flat rectangle each; these make a shot, the
 * breaking and the darkening follow GoldenEye's fixture instead.
 */

/** A shot's hit on the room's surface at hitpos: breaks the fixture it lands on. */
bool geLightsHandleHit(struct coord *gunpos, struct coord *hitpos, s32 roomnum);

/** The light has just been broken (roomSetLightBroken()): darken its fixture. */
void geLightsBroken(s32 roomnum, s32 lightnum, bool washealthy);

/** The room's data has just been (re)loaded: darken the fixtures already broken. */
void geLightsRoomLoaded(s32 roomnum);

#endif
