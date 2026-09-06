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

#endif
