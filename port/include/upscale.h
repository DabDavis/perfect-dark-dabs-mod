#ifndef _IN_UPSCALE_H
#define _IN_UPSCALE_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Building a texture pack on the player's own machine, with Upscayl.
 *
 * Shipping upscaled textures means redistributing the game's artwork, and the
 * models that made them come with terms of their own. Running the upscaler
 * locally sidesteps both: the game writes out its own textures, hands them to
 * an upscaler the player already has, and reads the result back as a pack.
 *
 * Neither Upscayl nor its models are bundled. Mod.UpscaylPath says where an
 * install is; failing that a few usual places are tried, and then the PATH.
 */

#define UPSCALE_IDLE      0
#define UPSCALE_PREPARING 1 // writing the game's own textures out
#define UPSCALE_RUNNING   2 // the upscaler has them
#define UPSCALE_FINISHING 3 // cropping the padding back off
#define UPSCALE_DONE      4
#define UPSCALE_FAILED    5

/** Whether an upscaler and its models were found. */
s32 upscaleIsAvailable(void);
const char *upscaleGetBinPath(void);
const char *upscaleGetModelsPath(void);

/**
 * Every model Upscayl has, whether or not it is here yet: choosing one that is
 * missing fetches it. upscaleModelIsPresent() says which are already down.
 */
s32 upscaleGetNumModels(void);
const char *upscaleGetModelName(s32 index);
s32 upscaleModelIsPresent(s32 index);

/**
 * Downloading Upscayl, rather than shipping it. Started when the page opens, so
 * that somebody who has never used this finds it working rather than finding a
 * row telling them to install something. The binary comes from upscayl-ncnn's
 * releases and the chosen model from the Upscayl repository, one at a time.
 */
s32 upscaleInstall(void);

/**
 * How much a download would be, in megabytes, counting only what is missing.
 * Zero when there is nothing to fetch.
 */
s32 upscaleGetDownloadMb(void);
s32 upscaleIsInstalling(void);
const char *upscaleGetInstallStatus(void);
void upscaleRedetect(void);

/** --upscayl-fetch: download and exit, for scripted setup. */
void upscaleFetchFromCommandLine(void);

/** --upscayl-build: download if needed, build a pack, and exit. */
void upscaleBuildFromCommandLine(void);

/**
 * Settings, as the menu sets them. These are the knobs Upscayl itself exposes,
 * plus the padding, which is ours: most game textures tile, and an upscaler
 * given no context invents different detail along each edge - a seam on every
 * wall.
 */
s32 upscaleGetModel(void);
void upscaleSetModel(s32 index);
s32 upscaleGetScale(void);
void upscaleSetScale(s32 scale);
s32 upscaleGetCompress(void);
void upscaleSetCompress(s32 percent);
s32 upscaleGetTta(void);
void upscaleSetTta(s32 enabled);
s32 upscaleGetGpu(void);
void upscaleSetGpu(s32 gpu);
s32 upscaleGetTileSize(void);
void upscaleSetTileSize(s32 size);
s32 upscaleGetPadding(void);
void upscaleSetPadding(s32 texels);

/** Starts a run. Returns 0 if it could not be started. */
s32 upscaleStart(void);
void upscaleCancel(void);

/**
 * Driven from the scheduler, once a frame - not from the page that starts it,
 * so backing out of the menu does not leave a run half done. The textures are
 * written out from here, a few per call, because loading one touches game state
 * the worker thread must not; everything after that is files and a subprocess,
 * which the worker does while the game carries on.
 */
void upscaleTick(void);

s32 upscaleGetState(void);
s32 upscaleGetPercent(void);
const char *upscaleGetStatus(void);
const char *upscaleGetPackName(void);

#ifdef __cplusplus
}
#endif

#endif
