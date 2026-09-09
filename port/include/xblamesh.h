#ifndef _IN_XBLAMESH_H
#define _IN_XBLAMESH_H

#include <PR/ultratypes.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Drawing the XBLA release's high resolution meshes in place of the game's own
 * display lists.
 *
 * The console release is the N64 game with its art replaced, and the models it
 * replaced are 596 files in 4J's own format sitting past the game's file ids in
 * the package's PackedSegFile. What says which one stands in for what is a mesh
 * id 4J wrote into the two padding bytes after `struct modelnode`'s type, which
 * this port has never read - see CLAUDE-notes/xbla.md for the format and for
 * how all of it was checked.
 *
 * Two things shape this more than they look like they should.
 *
 * **The id is in their model file, not ours.** Our copy of a model comes from
 * the ROM and its padding is zero, so the ids have to be read out of the
 * release's copy of the same file and matched up node for node. They cannot be
 * matched by file offset: the port rewrites a model file into native pointers
 * as it loads it (filemodel.c), so a node sits somewhere else in our buffer
 * than in theirs, and a 64-bit build moves it further. The two trees are walked
 * together instead, and a model whose shape does not match is left alone.
 *
 * **The geometry is in the model's own space, 1:1.** 4J kept Perfect Dark's
 * coordinates: an Area 51 crate is 100 units across in the ROM and 100 units
 * across in their mesh, and a lab door 4000 by 2800 in both. So a mesh needs no
 * transform of its own - it is drawn under the node's matrix like the display
 * list it replaces - and the floats quantise back to the s16 the game's own
 * vertices already are without losing anything.
 *
 * What it draws with is an ordinary Perfect Dark display list built once per
 * mesh, so the renderer, the cull modes and everything else downstream need
 * no changes at all. The one liberty taken is the size of a
 * vertex batch: `gSP1Triangle` multiplies its indices by 10 into a byte, so a
 * batch is 25 vertices rather than the 16 the real microcode's cache holds.
 * That is a lie the RSP would not accept and the renderer does not care about,
 * and it is why none of this is built for PLATFORM_N64.
 */

/** Whether a package with meshes in it was found. */
s32 xblaMeshIsAvailable(void);

/** Mod.XblaMeshes: whether to draw them. Off unless the player asks. */
s32 xblaMeshGetEnabled(void);

/**
 * A live switch either way, in a level as much as out of one: switching it on
 * matches every model the stage has already loaded, which is what the loaded
 * list in xblamesh.c is kept for. On a machine whose package is still inside
 * its archive, this is also where the archive comes apart.
 */
void xblaMeshSetEnabled(s32 enabled);

/**
 * Mod.XblaMeshKey (F6): the switch above from a key, polled by xblaMeshTick()
 * each frame beside texpackTick(). Bound from Dab's Mod Options with the
 * texture pack keys.
 */
s32 xblaMeshToggleGetKey(void);
void xblaMeshToggleSetKey(s32 vk);
void xblaMeshTick(void);

/**
 * Drops everything keyed on a model - the node registry, the palette uses, the
 * list of what is loaded - because the stage pool holding all of those
 * addresses has just been handed back. Called from lvReset(), beside the
 * texture ids that go for the same reason. Built meshes are keyed on a slot in
 * the package and stay.
 *
 * A backstop rather than the guard: memory is recycled inside a stage too, and
 * that is caught at the load that recycles it (xblaMeshRegisterModel). What is
 * left for this is the entries of models that are never loaded again.
 */
void xblaMeshResetModels(void);

/**
 * The meshes were switched on in a level that could not have any: the player's
 * copy was still inside its archive as the level loaded, so nothing in it was
 * matched, and it was this switch that took the archive apart. The level after
 * this one has the meshes like anybody else's.
 *
 * The one thing about either checkbox that is not obvious from watching it,
 * and true for exactly as long as it is true - xblaMeshResetModels() clears it
 * at lvReset(), before the next stage's models load.
 */
s32 xblaMeshModelsAreLate(void);

/**
 * A model has just been loaded and its pointers made real: note which of its
 * nodes the release replaces, and with what.
 *
 * Called from modeldefLoad(), which is the one place that has the file id, the
 * buffer and a promoted tree at the same time.
 *
 * Called for every model whether or not the meshes are switched on, which is
 * what makes switching them on a live thing to do, and so also for models the
 * release has no copy of. Those matter too: this is where a model drops
 * anything registered against the memory it has just been given, which is the
 * only thing standing between a reused modeldef and the meshes of whatever
 * held that address before it.
 */
void xblaMeshRegisterModel(struct modeldef *modeldef, u16 fileid);

/**
 * Draws a node from the release's mesh instead of its own display list.
 *
 * Returns 0 when there is nothing to draw it from, and the caller carries on
 * with the game's geometry.
 */
s32 xblaMeshRenderNode(struct modelrenderdata *renderdata, struct model *model,
		struct modelnode *node);

/**
 * A frame is starting: the vertices posed for the last one are two frames old
 * and their arena can be reused.
 */
void xblaMeshFrameReset(void);

/** --xbla-mesh-verbose: log each replaced node's box against its mesh's. */
void xblaMeshSetVerbose(s32 verbose);

/** How much is loaded, for gdb. */
extern u32 g_XblaMeshNumMeshes;
extern u32 g_XblaMeshNumNodes;
extern u32 g_XblaMeshNumSlots;
extern u32 g_XblaMeshNumTris;
extern u32 g_XblaMeshBytes;

#ifdef __cplusplus
}
#endif

#endif
