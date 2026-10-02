#ifndef _IN_MODELMESH_H
#define _IN_MODELMESH_H

#include <ultra64.h>

/**
 * A model node's lists drawn from the renderer's copy of the node's vertices
 * on the GPU (G_MESH_EXT, gSPModelMeshEXT()). modelMeshBegin() opens the
 * node's draw ahead of its lists and modelMeshEnd() closes it; with the GPU
 * path off they put nothing in the list.
 */
Gfx *modelMeshBegin(Gfx *gdl, const Vtx *vertices, s32 numvertices);
Gfx *modelMeshEnd(Gfx *gdl);

#endif
