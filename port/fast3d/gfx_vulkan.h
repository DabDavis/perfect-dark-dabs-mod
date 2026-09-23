#ifndef GFX_VULKAN_H
#define GFX_VULKAN_H

#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// The Vulkan renderer. Only built where the Vulkan headers and shaderc were
// found (PD_HAVE_VULKAN); see gfx_vulkan.cpp.
extern struct GfxRenderingAPI gfx_vulkan_api;

// Nonzero once init() has given up (no loader, no device that can draw the
// way the renderer needs, no surface). Everything it created is gone by then
// and the caller is expected to put the window back up for OpenGL.
int gfx_vulkan_failed(void);

// What the device is, for the log and the F3 dump: "AMD Radeon RX 580 (RADV
// POLARIS10), Vulkan 1.4.305". Empty before init.
const char *gfx_vulkan_device_name(void);

#ifdef __cplusplus
}
#endif

#endif
