#ifndef _IN_MODELPACK_H
#define _IN_MODELPACK_H

#include <PR/ultratypes.h>
#include "objmesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Model packs: replacement geometry dropped in beside the executable.
 *
 * A pack is a folder in model-packs/, the way a texture pack is a folder in
 * texture-packs/, holding OBJ files under two names:
 *
 *   model-packs/<pack>/n64/<file name>.obj    replaces one of the ROM's models
 *   model-packs/<pack>/xbla/<file name>.obj   replaces the XBLA release's mesh
 *                                             for that model, when the meshes
 *                                             are on
 *
 * The file name is the ROM's own name for the model (CcarringtonZ, Pcrate,
 * ...), which is what the asset dump writes them out as - so a pack is made
 * by dumping, editing and dropping the file back in. A dump from
 * model-dumps/n64/ goes in n64/ and one from model-dumps/xbla/ goes in xbla/;
 * the two are not interchangeable, since an XBLA mesh is one piece in the
 * model's space and a Perfect Dark model is a piece per list node in the
 * node's own space (objmesh.h).
 *
 * What draws them is the XBLA mesh loader (xblamesh.c): an OBJ is read into
 * the same shape as one of the release's meshes and goes through the same
 * builder, so everything that was worked out for those - the lighting, the
 * translucent pass, the door trim, the skinning - holds for a pack's file.
 * Which is also why a replacement for a skinned character keeps working after
 * being edited: it has no bone weights of its own and takes them from the
 * nearest vertex of the mesh it replaces.
 *
 * Mod.LoadModels switches the lot on and Mod.ModelPack names the pack. Both
 * are live: every model files its list nodes as it loads (whenever there is a
 * pack installed at all), and what draws them is decided at the draw, so a
 * pack chosen from the menu is on screen from the next frame without leaving
 * the level.
 *
 * Where a model has both an n64/ file and a mesh of the release's,
 * Mod.ModelPackPrefer says which of the two draws.
 */

/** Mod.LoadModels. */
s32 modelpackLoadEnabled(void);
void modelpackSetLoadEnabled(s32 enabled);

/**
 * Mod.ModelPackPrefer: which side wins for a model that has a pack's n64/
 * file and one of the release's meshes at the same time.
 *
 * 0 - the pack's own model, which is what the player put there.
 * 1 - the XBLA mesh (the release's, or the pack's xbla/ file for it), which
 *     is what somebody running the release's art wants a pack's odd N64
 *     replacement not to punch a hole in.
 *
 * Read at the draw, so it is live like the rest of it.
 */
#define MODELPACK_PREFER_N64  0
#define MODELPACK_PREFER_XBLA 1

s32 modelpackGetPrefer(void);
void modelpackSetPrefer(s32 prefer);

/** Whether model-packs/ holds a pack at all - see modelpackHavePacks(). */
s32 modelpackHavePacks(void);

/**
 * Takes the folder list and the pack's files again, and says everything built
 * from them is stale - for looking at an OBJ that has just been edited without
 * leaving the level. The texture packs' reload key does this too.
 */
void modelpackReload(void);

/** The pack list, re-read from model-packs/ on each refresh. */
void modelpackRefreshPacks(void);
s32 modelpackGetNumPacks(void);
const char *modelpackGetPackName(s32 index);
s32 modelpackGetSelectedPack(void);       // -1 for none
void modelpackSetSelectedPack(s32 index); // -1 for none
const char *modelpackGetPacksDirPath(void);

/**
 * The pack's file for one of the game's models, by file id, or NULL. The n64
 * one stands in for the ROM's own geometry and the xbla one for the release's
 * mesh. Answer NULL while packs are off.
 */
const char *modelpackFindN64(s32 fileid);
const char *modelpackFindXbla(s32 fileid);

/** Goes up whenever the answers above, or the preference, may have changed. */
u32 modelpackGetGeneration(void);

/**
 * The stand-in tile for a material that draws with a picture of its own -
 * one of the ROM's numbered textures, or a file beside the OBJ - bound
 * through xblatex.c. NULL when the picture cannot be had, and the material
 * draws shaded. Whether the picture has alpha and whether it is soft come
 * back with it.
 *
 * A numbered texture binds the ROM's own picture and the number with it, so
 * that the texture pack's picture for it is asked for at the draw: a texture
 * pack repaints a model pack's mesh, live, the same as it repaints anything
 * else.
 */
const void *modelpackBindMaterial(const struct objmaterial *mat, s32 *outAlpha, s32 *outSoft);

#ifdef __cplusplus
}
#endif

#endif
