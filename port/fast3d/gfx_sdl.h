#ifndef GFX_SDL_H
#define GFX_SDL_H

#include "gfx_window_manager_api.h"

extern struct GfxWindowManagerAPI gfx_sdl;

#ifdef __cplusplus
extern "C" {
#endif

// Make the window for the Vulkan renderer rather than OpenGL. Before init.
void gfx_sdl_set_vulkan(int enable);
int gfx_sdl_is_vulkan(void);

// SDL's video driver is the offscreen one, which has no Vulkan of its own:
// the Vulkan renderer then draws to a headless surface. After SDL_Init.
int gfx_sdl_vulkan_headless(void);

// Close the window (and its GL context), for putting it back up for another
// renderer when the first could not start.
void gfx_sdl_destroy_window(void);

#ifdef __cplusplus
}
#endif

#endif
