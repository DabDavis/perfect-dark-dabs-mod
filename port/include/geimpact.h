#ifndef _IN_GEIMPACT_H
#define _IN_GEIMPACT_H

#include <ultra64.h>
#include "types.h"

/**
 * GoldenEye's own bullet holes on a converted GoldenEye level: its twenty
 * impact types (explosion.c's g_ImpactTypes, oddtextures.c's s_impactimages)
 * picked per surface as tex.c's g_HitTypeSounds picks them, drawn with its own
 * images out of the level's textures/ - and in the release's look with the
 * release's own texture/bulletholes/ pictures. port/src/geimpact.c.
 *
 * They are wallhit texture numbers past Perfect Dark's eighteen, so
 * everything else about a wallhit (rooms, props, fading, limits) stays
 * Perfect Dark's.
 */
#define WALLHITTEX_GE_FIRST 0x12
#define GEIMPACT_NUMTYPES   20

// On every stage load, after geTexSurfaceReset() (lv.c)
void geImpactStageStart(s32 stagenum);

// GoldenEye's holes are what this stage leaves
s32 geImpactActive(void);

/**
 * The wallhit texture number a shot at a surface of this type leaves: the
 * one Perfect Dark picked (pdtexnum) when GoldenEye's are not in, else one of
 * GoldenEye's for that surface, or -1 when GoldenEye leaves none (water).
 */
s16 geImpactTexnum(s32 surfacetype, s16 pdtexnum);

s32 geImpactIsGe(s32 texnum);

// A GoldenEye wallhit's size and colouring (WALLHITTYPE_*)
void geImpactSize(s32 texnum, f32 *width, f32 *height, u8 *type);

// Its texture config, loaded, and the release's picture bound to it in its look
struct textureconfig *geImpactConfig(s32 texnum);

#endif
