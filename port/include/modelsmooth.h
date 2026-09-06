#ifndef _IN_MODELSMOOTH_H
#define _IN_MODELSMOOTH_H

#include <PR/ultratypes.h>
#include "types.h"

/**
 * Model Smoothing's mesh pass: read a freshly loaded model's triangles back
 * out of its display lists and hand the renderer, per triangle corner, the
 * normal the surface really has there. See modelsmooth.c.
 */
void modelSmoothClassify(struct modeldef *modeldef);

/**
 * A node about to be drawn from a copy of its vertices rather than the
 * model's own (modelRenderNodeDl): tell the renderer what the copy stands
 * for, so its triangles keep the mesh pass's normals.
 */
void modelSmoothNoteCopy(const Vtx *copy, const Vtx *orig, s32 numvertices);

/**
 * The frame's vertex buffer is about to be rebuilt: the copies noted in it
 * are gone.
 */
void modelSmoothForgetRange(const void *start, const void *end);

#endif
