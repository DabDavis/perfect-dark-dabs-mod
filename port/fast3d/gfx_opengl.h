#ifndef GFX_OPENGL_H
#define GFX_OPENGL_H

#include "gfx_rendering_api.h"

extern struct GfxRenderingAPI gfx_opengl_api;

#ifdef __cplusplus
extern "C" {
#endif
// GL_RENDERER as the driver named it when the context came up, "" before
const char *gfx_opengl_device_name(void);
#ifdef __cplusplus
}
#endif

#endif
