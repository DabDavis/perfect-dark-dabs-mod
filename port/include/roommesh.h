#ifndef _IN_ROOMMESH_H
#define _IN_ROOMMESH_H

#include <ultra64.h>

/**
 * A room's passes drawn from the renderer's copy of its vertices on the GPU
 * (G_MESH_EXT, gfxmesh.h). roomMeshBegin() opens the room's draw ahead of its
 * blocks' lists and roomMeshEnd() closes it; with the GPU path off they put
 * nothing in the list.
 */
Gfx *roomMeshBegin(Gfx *gdl, s32 roomnum);
Gfx *roomMeshEnd(Gfx *gdl);

/**
 * The room's vertices are going away (bgUnloadRoom()) or were rewritten in
 * place (a broken GoldenEye light darkens a vertex by changing its colour
 * byte): the GPU's copy and everything kept of its lists go with them.
 */
void roomMeshForget(s32 roomnum);

#endif
