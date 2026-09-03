#ifndef _IN_VIDEO_H
#define _IN_VIDEO_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

// maximum framerate; if the game runs faster than this, things will break
#if PAL
#define VIDEO_MAX_FPS 200
#else
#define VIDEO_MAX_FPS 240
#endif

typedef struct {
	s32 width;
	s32 height;
} displaymode;

s32 videoInit(void);
void videoStartFrame(void);
void videoSubmitCommands(Gfx *cmds);
void videoClearScreen(void);
void videoEndFrame(void);

void *videoGetWindowHandle(void);

void videoUpdateNativeResolution(s32 w, s32 h);
s32 videoGetNativeWidth(void);
s32 videoGetNativeHeight(void);

s32 videoGetWidth(void);
s32 videoGetHeight(void);
s32 videoGetWindowWidth(void);
s32 videoGetWindowHeight(void);
f32 videoGetAspect(void);
s32 videoGetFullscreen(void);
s32 videoGetFullscreenMode(void);
s32 videoGetMaximizeWindow(void);
void videoSetMaximizeWindow(s32 fs);
s32 videoGetCenterWindow(void);
void videoSetCenterWindow(s32 center);
u32 videoGetTextureFilter(void);
s32 videoGetTextureFilter2D(void);
u32 videoGetAnisotropicFilter(void);
u32 videoGetMaxAnisotropyLevel(void);
s32 videoGetDetailTextures(void);
s32 videoGetDisplayModeIndex(void);
s32 videoGetDisplayMode(displaymode *out, const s32 index);
s32 videoGetNumDisplayModes(void);
s32 videoGetVsync(void);
s32 videoGetFramerateLimit(void);
s32 videoGetDisplayFPS(void);
s32 videoGetMSAA(void);
f32 videoGetGlareBrightness(void);
f32 videoGetOverexposureScale(void);

f32 videoGetAverageFPS(void);

void videoSetWindowOffset(s32 x, s32 y);
void videoSetFullscreen(s32 fs);
void videoSetFullscreenMode(s32 mode);
void videoSetTextureFilter(u32 filter);
void videoSetTextureFilter2D(s32 filter);
void videoSetAnisotropicFilter(u32 filter);
void videoSetDetailTextures(s32 detail);
void videoSetDisplayMode(const s32 index);
void videoSetVsync(const s32 vsync);
void videoSetFramerateLimit(const s32 limit);
void videoSetDisplayFPS(const s32 displayfps);
void videoSetMSAA(const s32 msaa);
void videoSetGlareBrightness(f32 bright);
void videoSetOverexposureScale(f32 scale);

s32 videoCreateFramebuffer(u32 w, u32 h, s32 upscale, s32 autoresize);
void videoSetFramebuffer(s32 target);
void videoResetFramebuffer(void);
void videoCopyFramebuffer(s32 dst, s32 src, s32 left, s32 top);
void videoResizeFramebuffer(s32 target, u32 w, u32 h, s32 upscale, s32 autoresize);
s32 videoFramebuffersSupported(void);

// Called once per frame with the frame drawn and not yet presented, in the order
// they were added. Anything that wants the finished image registers here.
void videoAddPreSwapCallback(void (*cb)(void));

// Reads the window's back buffer into rgb as tightly packed RGB triples, bottom
// row first. Only meaningful from the pre-swap callback above. Stalls the frame:
// for anything that runs every frame use the capture calls below.
s32 videoReadScreenPixels(void *rgb, s32 width, s32 height);

/**
 * Streaming capture of the back buffer, for the recorder. Unlike the read above
 * this never waits on the GPU - the frame it hands back is the one from the call
 * before. See gfx_capture_start() in port/fast3d/gfx_api.h.
 *
 * NV12 frames are a byte and a half a pixel, top row first, already converted:
 * that is a third of the traffic of the RGB formats and the reason to prefer it.
 * The RGB ones are four bytes a pixel, bottom row first, in whichever of them
 * the driver reads back without repacking.
 *
 * videoCaptureFrameSize() is how many bytes a frame of the reported format is.
 */
#define VIDEO_CAPTURE_NONE 0
#define VIDEO_CAPTURE_BGRA 1
#define VIDEO_CAPTURE_RGBA 2
#define VIDEO_CAPTURE_NV12 3

s32 videoCaptureStart(s32 width, s32 height);
s32 videoCaptureRead(void *dst);
s32 videoCaptureDrain(void *dst);
void videoCaptureStop(void);

// The ffmpeg name for a VIDEO_CAPTURE_ format, or NULL.
const char *videoCaptureFormatName(s32 fmt);

// Bytes in one frame of that format, or 0 if it is not a format.
u32 videoCaptureFrameSize(s32 fmt, s32 width, s32 height);

// Whether the format arrives bottom row first, and so needs flipping.
s32 videoCaptureIsFlipped(s32 fmt);

void videoResetTextureCache(void);
void videoFreeCachedTexture(const void *texptr);
void videoFreeCachedTextures(const void *start, const void *end);

void videoShutdown(void);

#endif
