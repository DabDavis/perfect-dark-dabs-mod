/**
 * Models' nodes drawn from the renderer's copy of their vertices on the GPU
 * (G_MESH_EXT, gSPModelMeshEXT(), and "How a model is drawn" in
 * CLAUDE-notes/gpu-vertices.md).
 *
 * A model's lists load each batch of vertices under one of the model's
 * matrices, which they name in segment 3 (modelRender()), and the renderer
 * has worked every vertex through its matrix on the CPU. Each of a node's
 * draws (modelRenderNodeGundl(), modelRenderNodeDl()) is bracketed here
 * instead: the renderer keeps the node's vertices on the GPU as they stand in
 * the model's file, learns from the lists which matrix and which colour each
 * one is loaded with, and draws a run of a list as one draw whose vertex
 * shader transforms every vertex by its own matrix of the frame.
 *
 * Nothing is registered or forgotten here. The renderer keys its copy by the
 * vertices' address and checks the array's contents once a frame it draws
 * them, so a model file loaded where another was, a head moved on its body
 * (bodyCalculateHeadOffset()) or a gun's texture slid in place (the
 * laser's liquid) is noticed there; an array that keeps changing is left to the CPU.
 */

#ifndef PLATFORM_N64

#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "gfxmesh.h"
#include "modelmesh.h"

static s32 modelMeshOpen;

Gfx *modelMeshBegin(Gfx *gdl, const Vtx *vertices, s32 numvertices)
{
	modelMeshOpen = 0;

	if (!vertices || numvertices <= 0 || numvertices > 0xffff || !gfxMeshGpuAvailable()) {
		return gdl;
	}

	gSPModelMeshEXT(gdl++, vertices, numvertices);
	modelMeshOpen = 1;

	return gdl;
}

Gfx *modelMeshEnd(Gfx *gdl)
{
	if (modelMeshOpen) {
		gSPMeshEXT(gdl++, NULL);
		modelMeshOpen = 0;
	}

	return gdl;
}

#endif
