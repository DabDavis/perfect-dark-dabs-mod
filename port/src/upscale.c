/**
 * Builds a texture pack by running Upscayl over the game's own textures.
 *
 * The game already knows how to write every texture out as an image and how to
 * read one back as a replacement; this puts an upscaler between the two. Doing
 * it on the player's machine rather than shipping the result keeps the game's
 * artwork where it is, and keeps the upscaling models' own terms out of it.
 *
 * The work is split by what may touch game state. Writing the textures out
 * loads each one through texLoadFromTextureNum(), which uses globals the
 * renderer is also reading, so that part runs on the main thread a few textures
 * at a time. Everything after it - the upscaler, and cropping its output back
 * down - is files, and runs on a worker while the game carries on.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <SDL.h>
#include <ultra64.h>
#include "bss.h"
#include "constants.h"
#include "game/tex.h"
#include "game/texdecompress.h"
#include "platform.h"
#include "config.h"
#include "fs.h"
#include "pngread.h"
#include "pngwrite.h"
#include "ghostnet.h"
#include "archive.h"
#include "system.h"
#include "texpack.h"
#include "types.h"
#include "upscale.h"

#define UPSCALE_MAXMODELS 32
#define UPSCALE_NAMELEN 64

// Textures written per frame while preparing. Loading one costs about a
// millisecond, so this is a frame's worth of work and the whole table takes a
// couple of seconds without the menu ever stopping.
#define UPSCALE_PREPARE_CHUNK 24

// Enough for the largest texture in the ROM, which is what texLoad wants.
#define UPSCALE_POOL (128 * 1024)

// Below this on either side there is nothing for an upscaler to work with, and
// the game keeps drawing the original.
#define UPSCALE_MIN_SIZE 8

#define UPSCALE_WORK_DIR ".upscale"

// Packs built here get their own folder rather than sharing texture-packs: one
// of these is the whole texture table at four times the size, and a player
// should be able to tell what they made from what they installed.
#define UPSCALE_PACKS_DIR "upscayl-packs"

// Where a downloaded Upscayl is put, beside the game.
#define UPSCALE_INSTALL_DIR "upscayl"

// Pinned rather than asked for. A release name here is a timestamp, so a
// "latest" URL cannot name an asset, and a version that is known to work beats
// whatever was published this morning. Mod.UpscaylNcnnTag overrides it.
#define UPSCALE_NCNN_REPO "https://github.com/upscayl/upscayl-ncnn/releases/download"
#define UPSCALE_NCNN_TAG  "20251207-174704"

// Models are not in that release; they live in the Upscayl repository and are
// fetched one at a time, because all seven come to 171MB and nobody needs the
// six they did not pick.
#define UPSCALE_MODELS_URL "https://raw.githubusercontent.com/upscayl/upscayl/main/resources/models"

static char upscaylPath[FS_MAXPATH + 1];
static char binPath[FS_MAXPATH + 1];
static char modelsPath[FS_MAXPATH + 1];
static s32 detected; // 0 = not looked yet, 1 = found, -1 = not found

static char modelNames[UPSCALE_MAXMODELS][UPSCALE_NAMELEN];
static s32 numModels;

static s32 optModel;
static s32 optScale = 4;
static s32 optCompress;
static s32 optTta;
static s32 optGpu = -1; // auto
static s32 optTileSize;  // 0 = auto
static s32 optPadding = 8;
static char ncnnTag[32] = UPSCALE_NCNN_TAG;
static char installDir[FS_MAXPATH + 1];
static SDL_atomic_t installBusy;
static SDL_atomic_t installFailed;
static char installStatus[128];
static SDL_Thread *installer;

static s32 state;
static s32 prepared;      // textures written out so far
static s32 preparedTotal; // how many will be
static char statusText[128];
static char packName[UPSCALE_NAMELEN];
static char inDir[FS_MAXPATH + 1];
static char outDir[FS_MAXPATH + 1];
static char packDir[FS_MAXPATH + 1];
static u8 *poolBuffer;
static s32 nextTexture;
static s32 pollTimer;

static SDL_Thread *worker;
static SDL_atomic_t workerDone;   // set when the worker has finished
static SDL_atomic_t workerFailed;
static SDL_atomic_t workerStage;  // UPSCALE_RUNNING or UPSCALE_FINISHING
static SDL_atomic_t workerCount;  // files handled in the current stage
static SDL_atomic_t cancelled;

static void upscaleSetStatus(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(statusText, sizeof(statusText), fmt, args);
	va_end(args);
}

/**
 * Looks for a file, and says so.
 */
static s32 upscaleFileExists(const char *path)
{
	return fsFileSize(path) >= 0;
}

/**
 * Tries one Upscayl install directory, filling in the binary and model paths.
 *
 * An install keeps its binary under resources/<platform>/bin and its models
 * under resources/models. A source checkout has exactly the same shape, which
 * is why pointing this at one works.
 */
static s32 upscaleTryInstall(const char *root)
{
	static const char *const platforms[] = {
#if defined(PLATFORM_WIN32)
		"win",
#elif defined(PLATFORM_MACOS)
		"mac",
#else
		"linux",
#endif
	};

	char bin[FS_MAXPATH + 1];
	char models[FS_MAXPATH + 1];
	u32 i;

	if (!root || !root[0]) {
		return 0;
	}

	snprintf(models, sizeof(models), "%s/resources/models", root);

	if (!upscaleFileExists(models)) {
		snprintf(models, sizeof(models), "%s/models", root);

		if (!upscaleFileExists(models)) {
			return 0;
		}
	}

	for (i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
#ifdef PLATFORM_WIN32
		snprintf(bin, sizeof(bin), "%s/resources/%s/bin/upscayl-bin.exe", root, platforms[i]);
#else
		snprintf(bin, sizeof(bin), "%s/resources/%s/bin/upscayl-bin", root, platforms[i]);
#endif

		if (upscaleFileExists(bin)) {
			strncpy(binPath, bin, sizeof(binPath) - 1);
			strncpy(modelsPath, models, sizeof(modelsPath) - 1);
			return 1;
		}
	}

	// An install that keeps the binary beside the models, or a bare directory
	// of a build.
#ifdef PLATFORM_WIN32
	snprintf(bin, sizeof(bin), "%s/upscayl-bin.exe", root);
#else
	snprintf(bin, sizeof(bin), "%s/upscayl-bin", root);
#endif

	if (upscaleFileExists(bin)) {
		strncpy(binPath, bin, sizeof(binPath) - 1);
		strncpy(modelsPath, models, sizeof(modelsPath) - 1);
		return 1;
	}

	return 0;
}

/**
 * Upscayl's models, by name.
 *
 * All of them are offered whether or not they are here yet: a model is 2 to
 * 32MB and choosing one that is missing fetches it, which is what makes
 * switching between them a choice rather than a download decision. Listed in a
 * fixed order so the menu does not shuffle between runs.
 */
static const char *const upscaleKnownModels[] = {
	"upscayl-standard-4x",
	"upscayl-lite-4x",
	"high-fidelity-4x",
	"remacri-4x",
	"ultramix-balanced-4x",
	"ultrasharp-4x",
	"digital-art-4x",
};

// Roughly what each comes to on disk, for the prompt. They are not all the same
// size - the lite one is a fortieth of the others - so a single figure would be
// wrong for most of them.
static const s32 upscaleModelMb[] = { 32, 3, 32, 32, 32, 32, 9 };

// And the decoder itself, unpacked.
#define UPSCALE_BIN_MB 12

static void upscaleScanModels(void)
{
	s32 i;

	numModels = 0;

	for (i = 0; i < (s32)(sizeof(upscaleKnownModels) / sizeof(upscaleKnownModels[0]))
			&& numModels < UPSCALE_MAXMODELS; i++) {
		strncpy(modelNames[numModels], upscaleKnownModels[i], UPSCALE_NAMELEN - 1);
		numModels++;
	}
}

/**
 * Whether a model's files are here, as opposed to merely offered.
 */
s32 upscaleModelIsPresent(s32 index)
{
	char path[FS_MAXPATH + 1];

	if (index < 0 || index >= numModels || !modelsPath[0]) {
		return 0;
	}

	snprintf(path, sizeof(path), "%s/%s.bin", modelsPath, modelNames[index]);

	return fsFileSize(path) > 0;
}

static void upscaleDetect(void)
{
	static const char *const guesses[] = {
#ifdef PLATFORM_WIN32
		"C:/Program Files/Upscayl/resources",
#elif defined(PLATFORM_MACOS)
		"/Applications/Upscayl.app/Contents/Resources",
#else
		"/opt/Upscayl/resources",
		"/usr/lib/upscayl/resources",
		"/usr/share/upscayl/resources",
#endif
	};

	char home[FS_MAXPATH + 1];
	u32 i;

	detected = -1;
	binPath[0] = '\0';
	modelsPath[0] = '\0';

	if (upscaylPath[0] && upscaleTryInstall(upscaylPath)) {
		detected = 1;
	}

	// What this page downloaded, before anything a player installed
	// themselves: it is the copy the settings here were chosen against.
	if (detected < 0) {
		snprintf(home, sizeof(home), "%s/" UPSCALE_INSTALL_DIR, fsFullPath("$E"));

		if (upscaleTryInstall(home)) {
			detected = 1;
		}
	}

	if (detected < 0) {
		snprintf(home, sizeof(home), "%s/" UPSCALE_INSTALL_DIR, fsFullPath("$S"));

		if (upscaleTryInstall(home)) {
			detected = 1;
		}
	}

	if (detected < 0) {
		// The player's own home directory, where a checkout or an unpacked
		// AppImage tends to land. fsFullPath("$H") is the game's data
		// directory, which is not the same thing.
		const char *userHome = getenv("HOME");

#ifdef PLATFORM_WIN32
		if (!userHome) {
			userHome = getenv("USERPROFILE");
		}
#endif

		if (userHome && userHome[0]) {
			snprintf(home, sizeof(home), "%s/upscayl", userHome);

			if (upscaleTryInstall(home)) {
				detected = 1;
			}

			if (detected < 0) {
				snprintf(home, sizeof(home), "%s/Upscayl", userHome);

				if (upscaleTryInstall(home)) {
					detected = 1;
				}
			}
		}
	}

	if (detected < 0) {
		// Beside the game, and in its data directory, for someone who would
		// rather keep it with the mod than in their home.
		snprintf(home, sizeof(home), "%s/upscayl", fsFullPath("$H"));

		if (upscaleTryInstall(home)) {
			detected = 1;
		}
	}

	for (i = 0; detected < 0 && i < sizeof(guesses) / sizeof(guesses[0]); i++) {
		if (upscaleTryInstall(guesses[i])) {
			detected = 1;
		}
	}

	if (detected < 0) {
		sysLogPrintf(LOG_NOTE, "upscale: no Upscayl install found - set Mod.UpscaylPath");
	}

	upscaleScanModels();

	if (detected > 0) {
		sysLogPrintf(LOG_NOTE, "upscale: using %s", binPath);
	}
}


/**
 * Fetching Upscayl, rather than shipping it.
 *
 * The binary is a few megabytes and comes from upscayl-ncnn's releases; the
 * models are not in those and come one at a time from the Upscayl repository,
 * because all seven together are 171MB and nobody needs the six they did not
 * choose. Both land beside the game, in the same shape an install has, so the
 * detection above finds them without knowing they were downloaded.
 */
static const char *upscalePlatformName(void)
{
#if defined(PLATFORM_WIN32)
	return "windows";
#elif defined(PLATFORM_MACOS)
	return "macos";
#else
	return "linux";
#endif
}

static void upscaleSetInstallStatus(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(installStatus, sizeof(installStatus), fmt, args);
	va_end(args);
}

/**
 * One file, streamed to disk. Uses the same HTTP the updater does.
 */
static s32 upscaleFetch(const char *url, const char *path)
{
	struct ghostnetreq req;
	struct ghostnetbuf buf;
	char err[256];
	s32 status = 0;
	FILE *f = fopen(path, "wb");

	if (!f) {
		sysLogPrintf(LOG_ERROR, "upscale: cannot write %s", path);
		return 0;
	}

	memset(&req, 0, sizeof(req));
	memset(&buf, 0, sizeof(buf));
	req.url = url;
	req.redirect = true; // a release asset is a redirect to wherever it lives
	req.timeout = 600;
	buf.sink = f;

	if (!ghostnetSend(&req, &buf, &status, err, sizeof(err)) || status != 200) {
		fclose(f);
		remove(path);
		sysLogPrintf(LOG_ERROR, "upscale: %s: %s", url, err[0] ? err : "download failed");
		return 0;
	}

	fclose(f);

	return 1;
}

/**
 * Puts one model beside the binary, if it is not already there.
 */
static s32 upscaleFetchModel(const char *name)
{
	char url[512];
	char path[FS_MAXPATH + 1];
	const char *ext[] = { "param", "bin" };
	u32 i;

	for (i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/models/%s.%s", installDir, name, ext[i]);

		if (fsFileSize(path) > 0) {
			continue;
		}

		snprintf(url, sizeof(url), UPSCALE_MODELS_URL "/%s.%s", name, ext[i]);
		upscaleSetInstallStatus("Downloading %s.%s...", name, ext[i]);

		if (!upscaleFetch(url, path)) {
			return 0;
		}
	}

	return 1;
}

static s32 upscaleInstaller(void *arg)
{
	char url[512];
	char zip[FS_MAXPATH + 1];
	char models[FS_MAXPATH + 1];
	char bin[FS_MAXPATH + 1];

	snprintf(models, sizeof(models), "%s/models", installDir);
	fsCreateDir(models);

	// The binary, if it is not already here from a previous go.
	if (!upscaleTryInstall(installDir)) {
		snprintf(url, sizeof(url), UPSCALE_NCNN_REPO "/%s/upscayl-bin-%s-%s.zip",
				ncnnTag, ncnnTag, upscalePlatformName());
		snprintf(zip, sizeof(zip), "%s/upscayl-bin.zip", installDir);

		upscaleSetInstallStatus("Downloading Upscayl...");

		if (!upscaleFetch(url, zip)) {
			upscaleSetInstallStatus("Could not download Upscayl");
			SDL_AtomicSet(&installFailed, 1);
			SDL_AtomicSet(&installBusy, 0);
			return 0;
		}

		upscaleSetInstallStatus("Unpacking Upscayl...");

		if (archiveExtract(zip, installDir) <= 0) {
			upscaleSetInstallStatus("Could not unpack Upscayl");
			SDL_AtomicSet(&installFailed, 1);
			SDL_AtomicSet(&installBusy, 0);
			return 0;
		}

		remove(zip);

		// The zip holds one directory named after the release; the binary is
		// moved up so the layout matches an install and the name stops
		// mattering.
		snprintf(bin, sizeof(bin), "%s/upscayl-bin-%s-%s/upscayl-bin%s",
				installDir, ncnnTag, upscalePlatformName(),
#ifdef PLATFORM_WIN32
				".exe");
#else
				"");
#endif
		{
			char dst[FS_MAXPATH + 1];
#ifdef PLATFORM_WIN32
			snprintf(dst, sizeof(dst), "%s/upscayl-bin.exe", installDir);
#else
			snprintf(dst, sizeof(dst), "%s/upscayl-bin", installDir);
#endif
			rename(bin, dst);

#ifndef PLATFORM_WIN32
			// The zip does not carry the executable bit through.
			chmod(dst, 0755);
#endif
		}
	}

	// And the model that is selected, which is the only one that is needed.
	// A decoder with no model cannot upscale anything, so no model is a
	// failure rather than a thing to pass over.
	if (numModels <= 0) {
		upscaleSetInstallStatus("No models to choose from");
		SDL_AtomicSet(&installFailed, 1);
		SDL_AtomicSet(&installBusy, 0);
		return 0;
	}

	if (!upscaleFetchModel(modelNames[optModel])) {
		upscaleSetInstallStatus("Could not download the model");
		SDL_AtomicSet(&installFailed, 1);
		SDL_AtomicSet(&installBusy, 0);
		return 0;
	}

	upscaleSetInstallStatus("Ready");
	SDL_AtomicSet(&installBusy, 0);

	return 0;
}

/**
 * How much would be downloaded if it started now, in megabytes.
 *
 * Only what is actually missing: with the decoder already down, choosing
 * another model is just that model.
 */
s32 upscaleGetDownloadMb(void)
{
	s32 mb = 0;

	if (!upscaleIsAvailable()) {
		mb += UPSCALE_BIN_MB;
	}

	if (!upscaleModelIsPresent(optModel)) {
		mb += optModel < (s32)(sizeof(upscaleModelMb) / sizeof(upscaleModelMb[0]))
				? upscaleModelMb[optModel] : 32;
	}

	return mb;
}

s32 upscaleIsInstalling(void)
{
	return SDL_AtomicGet(&installBusy);
}

const char *upscaleGetInstallStatus(void)
{
	return installStatus;
}

/**
 * Starts the download, if there is anything to download.
 *
 * Called when the page opens, so that somebody who has never used this finds
 * it working rather than finding a row telling them to go and install
 * something.
 */
s32 upscaleInstall(void)
{
	char rel[FS_MAXPATH + 1];

	if (SDL_AtomicGet(&installBusy)) {
		return 0;
	}

	// Fills in the model list, which the worker needs to know which model to
	// fetch. Without it a download quietly brings the decoder and no model,
	// and then reports success.
	upscaleIsAvailable();

	if (installer) {
		SDL_WaitThread(installer, NULL);
		installer = NULL;
	}

	if (fsChooseOutputDir(UPSCALE_INSTALL_DIR, rel, sizeof(rel)) != 0) {
		upscaleSetInstallStatus("Nowhere to install Upscayl");
		return 0;
	}

	strncpy(installDir, fsFullPath(rel), sizeof(installDir) - 1);
	installDir[sizeof(installDir) - 1] = '\0';

	SDL_AtomicSet(&installBusy, 1);
	SDL_AtomicSet(&installFailed, 0);
	upscaleSetInstallStatus("Starting...");

	installer = SDL_CreateThread(upscaleInstaller, "pd-upscayl-get", NULL);

	if (!installer) {
		SDL_AtomicSet(&installBusy, 0);
		upscaleSetInstallStatus("Could not start the download");
		return 0;
	}

	return 1;
}

s32 upscaleIsAvailable(void)
{
	if (!detected) {
		upscaleDetect();
	}

	return detected > 0;
}

/**
 * Looks again, after something has been put in place.
 */
void upscaleRedetect(void)
{
	detected = 0;
	upscaleIsAvailable();
}

const char *upscaleGetBinPath(void)
{
	upscaleIsAvailable();
	return binPath;
}

const char *upscaleGetModelsPath(void)
{
	upscaleIsAvailable();
	return modelsPath;
}

s32 upscaleGetNumModels(void)
{
	upscaleIsAvailable();
	return numModels;
}

const char *upscaleGetModelName(s32 index)
{
	upscaleIsAvailable();

	if (index < 0 || index >= numModels) {
		return "";
	}

	return modelNames[index];
}

s32 upscaleGetModel(void) { return optModel; }
void upscaleSetModel(s32 index)
{
	// The list is filled by the first look for an install, which may not have
	// happened yet - without this, setting a model before anything has asked
	// how many there are is quietly ignored.
	upscaleIsAvailable();

	if (index >= 0 && index < numModels) {
		optModel = index;
	}
}
s32 upscaleGetScale(void) { return optScale; }
void upscaleSetScale(s32 scale) { if (scale >= 2 && scale <= 4) optScale = scale; }
s32 upscaleGetCompress(void) { return optCompress; }
void upscaleSetCompress(s32 percent) { if (percent >= 0 && percent <= 100) optCompress = percent; }
s32 upscaleGetTta(void) { return optTta; }
void upscaleSetTta(s32 enabled) { optTta = enabled ? 1 : 0; }
s32 upscaleGetGpu(void) { return optGpu; }
void upscaleSetGpu(s32 gpu) { if (gpu >= -1 && gpu <= 7) optGpu = gpu; }
s32 upscaleGetTileSize(void) { return optTileSize; }
void upscaleSetTileSize(s32 size) { optTileSize = size; }
s32 upscaleGetPadding(void) { return optPadding; }
void upscaleSetPadding(s32 texels) { if (texels >= 0 && texels <= 32) optPadding = texels; }

s32 upscaleGetState(void) { return state; }
const char *upscaleGetStatus(void) { return statusText; }
const char *upscaleGetPackName(void) { return packName; }

s32 upscaleGetPercent(void)
{
	switch (state) {
	case UPSCALE_PREPARING:
		return preparedTotal ? (prepared * 100) / preparedTotal : 0;
	case UPSCALE_RUNNING:
	case UPSCALE_FINISHING:
		return preparedTotal ? (SDL_AtomicGet(&workerCount) * 100) / preparedTotal : 0;
	case UPSCALE_DONE:
		return 100;
	}

	return 0;
}

/**
 * Surrounds an image with copies of its own opposite edges.
 *
 * A tiling texture's left edge continues into its right, so this gives the
 * upscaler the context it would have had in the game and the result still meets
 * itself. The padding is cropped off again afterwards.
 */
static u8 *upscalePadWrapped(const u8 *rgba, s32 width, s32 height, s32 pad,
		s32 *outWidth, s32 *outHeight)
{
	const s32 pw = width + pad * 2;
	const s32 ph = height + pad * 2;
	u8 *out = malloc((size_t)pw * ph * 4);
	s32 y;

	if (!out) {
		return NULL;
	}

	for (y = 0; y < ph; y++) {
		// Wrapping in both directions, so a corner takes the opposite corner.
		const s32 sy = ((y - pad) % height + height) % height;
		u8 *dst = out + (size_t)pw * 4 * y;
		s32 x;

		for (x = 0; x < pw; x++, dst += 4) {
			const s32 sx = ((x - pad) % width + width) % width;
			memcpy(dst, rgba + ((size_t)width * sy + sx) * 4, 4);
		}
	}

	*outWidth = pw;
	*outHeight = ph;

	return out;
}

/**
 * Quotes one path for a shell command line.
 *
 * The upscaler is run through the shell, and a player's install can sit under a
 * directory with a space in it - which is the usual case on Windows and a
 * common one everywhere else.
 */
static void upscaleQuote(char *dst, u32 dstSize, const char *src)
{
#ifdef PLATFORM_WIN32
	snprintf(dst, dstSize, "\"%s\"", src);
#else
	u32 o = 0;
	u32 i;

	if (dstSize < 3) {
		dst[0] = '\0';
		return;
	}

	dst[o++] = '\'';

	for (i = 0; src[i] && o + 5 < dstSize; i++) {
		if (src[i] == '\'') {
			// End the quoted run, add an escaped quote, start another.
			memcpy(dst + o, "'\\''", 4);
			o += 4;
		} else {
			dst[o++] = src[i];
		}
	}

	dst[o++] = '\'';
	dst[o] = '\0';
#endif
}

static void upscaleRemoveFile(const char *name, void *arg)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/%s", (const char *)arg, name);
	remove(path);
}

/**
 * Crops one upscaled image back to the texture it came from and files it in the
 * pack.
 *
 * The padding was there to give the upscaler context across the texture's own
 * edges; keeping it would make every texture bigger than the one it replaces
 * and shift it besides.
 */
static void upscaleCropFile(const char *name, void *arg)
{
	char src[FS_MAXPATH + 1];
	char dst[FS_MAXPATH + 1];
	const s32 pad = optPadding * optScale;
	s32 width;
	s32 height;
	u8 *rgba;

	if (SDL_AtomicGet(&cancelled)) {
		return;
	}

	SDL_AtomicAdd(&workerCount, 1);

	snprintf(src, sizeof(src), "%s/%s", outDir, name);
	snprintf(dst, sizeof(dst), "%s/%s", packDir, name);

	if (!pad) {
		// Nothing to do but move it across.
		rename(src, dst);
		return;
	}

	rgba = pngRead(src, &width, &height);

	if (!rgba) {
		return;
	}

	if (width > pad * 2 && height > pad * 2) {
		const s32 cw = width - pad * 2;
		const s32 ch = height - pad * 2;
		u8 *cropped = malloc((size_t)cw * ch * 4);

		if (cropped) {
			s32 y;

			for (y = 0; y < ch; y++) {
				memcpy(cropped + (size_t)cw * 4 * y,
						rgba + ((size_t)width * (y + pad) + pad) * 4,
						(size_t)cw * 4);
			}

			// Written the same way up as it was read: the padding came off
			// both ends evenly, so the row order is unchanged.
			pngWrite(dst, cropped, cw, ch, 4, 0);
			free(cropped);
		}
	}

	free(rgba);
	remove(src);
}

/**
 * Runs the upscaler and crops its output back down.
 *
 * Both halves are files and a subprocess, so they happen off the main thread.
 * Progress is counted by looking at what has been written rather than by
 * reading the upscaler's own output, which differs between versions.
 */
static s32 upscaleWorker(void *arg)
{
	char cmd[FS_MAXPATH * 4];
	char qbin[FS_MAXPATH + 4];
	char qin[FS_MAXPATH + 4];
	char qout[FS_MAXPATH + 4];
	char qmodels[FS_MAXPATH + 4];
	s32 n;

	upscaleQuote(qbin, sizeof(qbin), binPath);
	upscaleQuote(qin, sizeof(qin), inDir);
	upscaleQuote(qout, sizeof(qout), outDir);
	upscaleQuote(qmodels, sizeof(qmodels), modelsPath);

	snprintf(cmd, sizeof(cmd), "%s -i %s -o %s -m %s -n %s -s %d -f png",
			qbin, qin, qout, qmodels, modelNames[optModel], optScale);

	if (optCompress > 0) {
		n = strlen(cmd);
		snprintf(cmd + n, sizeof(cmd) - n, " -c %d", optCompress);
	}

	if (optTta) {
		n = strlen(cmd);
		snprintf(cmd + n, sizeof(cmd) - n, " -x");
	}

	if (optGpu >= 0) {
		n = strlen(cmd);
		snprintf(cmd + n, sizeof(cmd) - n, " -g %d", optGpu);
	}

	if (optTileSize > 0) {
		n = strlen(cmd);
		snprintf(cmd + n, sizeof(cmd) - n, " -t %d", optTileSize);
	}

	sysLogPrintf(LOG_NOTE, "upscale: %s", cmd);

	SDL_AtomicSet(&workerStage, UPSCALE_RUNNING);
	SDL_AtomicSet(&workerCount, 0);

#ifdef PLATFORM_WIN32
	{
		// cmd.exe strips the first and last quote of a command line that
		// begins with one - so a quoted program path followed by quoted
		// arguments comes apart into nonsense. Wrapping the whole line in one
		// more pair is the documented way round it, and every path here is
		// quoted because an install can sit under a directory with a space.
		char wrapped[sizeof(cmd) + 4];

		snprintf(wrapped, sizeof(wrapped), "\"%s\"", cmd);
		memcpy(cmd, wrapped, sizeof(cmd) - 1);
		cmd[sizeof(cmd) - 1] = '\0';
	}
#endif

	if (system(cmd) != 0) {
		sysLogPrintf(LOG_ERROR, "upscale: the upscaler did not finish");
		SDL_AtomicSet(&workerFailed, 1);
		SDL_AtomicSet(&workerDone, 1);
		return 0;
	}

	SDL_AtomicSet(&workerStage, UPSCALE_FINISHING);
	SDL_AtomicSet(&workerCount, 0);

	fsScanDir(outDir, upscaleCropFile, NULL);

	// The scratch copies are the whole texture table twice over; leaving them
	// behind would cost more disk than the pack does.
	fsScanDir(inDir, upscaleRemoveFile, inDir);
	fsScanDir(outDir, upscaleRemoveFile, outDir);

	SDL_AtomicSet(&workerDone, 1);

	return 0;
}

void upscaleCancel(void)
{
	SDL_AtomicSet(&cancelled, 1);
}

s32 upscaleStart(void)
{
	char rel[FS_MAXPATH + 1];

	if (state == UPSCALE_PREPARING || state == UPSCALE_RUNNING || state == UPSCALE_FINISHING) {
		return 0;
	}

	if (!upscaleIsAvailable()) {
		upscaleSetStatus("Upscayl was not found");
		state = UPSCALE_FAILED;
		return 0;
	}

	if (fsChooseOutputDir(UPSCALE_PACKS_DIR, rel, sizeof(rel)) != 0) {
		upscaleSetStatus("nowhere to write a pack");
		state = UPSCALE_FAILED;
		return 0;
	}

	// The scale is in the name so that the same model at two sizes gives two
	// packs rather than one overwriting the other.
	snprintf(packName, sizeof(packName), "%s %dx", modelNames[optModel], optScale);

	snprintf(inDir, sizeof(inDir), "%s/" UPSCALE_WORK_DIR, fsFullPath(rel));
	fsCreateDir(inDir);
	snprintf(outDir, sizeof(outDir), "%s/" UPSCALE_WORK_DIR "/out", fsFullPath(rel));
	snprintf(packDir, sizeof(packDir), "%s/%s", fsFullPath(rel), packName);
	fsCreateDir(packDir);

	snprintf(inDir, sizeof(inDir), "%s/" UPSCALE_WORK_DIR "/in", fsFullPath(rel));
	fsCreateDir(inDir);
	fsCreateDir(outDir);

	snprintf(packDir, sizeof(packDir), "%s/%s/textures", fsFullPath(rel), packName);
	fsCreateDir(packDir);

	poolBuffer = malloc(UPSCALE_POOL);

	if (!poolBuffer) {
		upscaleSetStatus("out of memory");
		state = UPSCALE_FAILED;
		return 0;
	}

	SDL_AtomicSet(&workerDone, 0);
	SDL_AtomicSet(&workerFailed, 0);
	SDL_AtomicSet(&workerCount, 0);
	SDL_AtomicSet(&cancelled, 0);

	nextTexture = 0;
	prepared = 0;
	pollTimer = 0;
	preparedTotal = NUM_TEXTURES;
	state = UPSCALE_PREPARING;
	upscaleSetStatus("Writing textures out...");

	return 1;
}

/**
 * Writes a few more of the game's textures out, padded and ready.
 */
static void upscalePrepareChunk(void)
{
	struct texpool pool;
	s32 done = 0;

	while (done < UPSCALE_PREPARE_CHUNK && nextTexture < NUM_TEXTURES) {
		const s32 n = nextTexture++;
		struct tex *tex;
		s32 width;
		s32 height;
		u8 *rgba;

		done++;
		prepared++;

		texInitPool(&pool, poolBuffer, UPSCALE_POOL);
		texLoadFromTextureNum(n, &pool);

		tex = texFindInPool(n, &pool);

		if (!tex || !tex->data) {
			continue;
		}

		rgba = texpackTexToRgba(tex, &width, &height);

		if (!rgba) {
			continue;
		}

		if (width >= UPSCALE_MIN_SIZE && height >= UPSCALE_MIN_SIZE) {
			char path[FS_MAXPATH + 1];
			s32 pw = width;
			s32 ph = height;
			u8 *padded = optPadding
					? upscalePadWrapped(rgba, width, height, optPadding, &pw, &ph) : NULL;

			snprintf(path, sizeof(path), "%s/%04x.png", inDir, n);
			pngWrite(path, padded ? padded : rgba, pw, ph, 4, 0);
			free(padded);
		}

		free(rgba);
	}

	if (nextTexture >= NUM_TEXTURES) {
		state = UPSCALE_RUNNING;
		upscaleSetStatus("Upscaling, this takes a while...");
		free(poolBuffer);
		poolBuffer = NULL;

		worker = SDL_CreateThread(upscaleWorker, "pd-upscale", NULL);

		if (!worker) {
			upscaleSetStatus("could not start the upscaler");
			state = UPSCALE_FAILED;
		}
	}
}

/**
 * --upscayl-fetch: download Upscayl and stop, without touching the menus.
 *
 * The same job the page does, for a machine being set up by a script and for
 * checking the download works on a platform where driving the menus is not
 * practical.
 */
void upscaleFetchFromCommandLine(void)
{
	if (!sysArgCheck("--upscayl-fetch")) {
		return;
	}

	if (!upscaleInstall()) {
		sysLogPrintf(LOG_ERROR, "upscale: could not start the download");
		exit(1);
	}

	while (SDL_AtomicGet(&installBusy)) {
		SDL_Delay(200);
	}

	upscaleRedetect();

	sysLogPrintf(LOG_NOTE, "upscale: %s (%s)",
			upscaleIsAvailable() ? "installed" : "failed", upscaleGetInstallStatus());

	exit(upscaleIsAvailable() ? 0 : 1);
}

/**
 * --upscayl-build: build a pack and stop, without touching the menus.
 *
 * Downloads whatever is missing first, so one command takes a machine from
 * nothing to a finished pack. Also the only way to exercise the run on a
 * platform where driving the menus is not practical.
 */
void upscaleBuildFromCommandLine(void)
{
	s32 last = -1;

	if (!sysArgCheck("--upscayl-build")) {
		return;
	}

	if (upscaleGetDownloadMb() > 0) {
		if (!upscaleInstall()) {
			sysLogPrintf(LOG_ERROR, "upscale: could not start the download");
			exit(1);
		}

		while (SDL_AtomicGet(&installBusy)) {
			SDL_Delay(200);
		}

		upscaleRedetect();
	}

	if (!upscaleIsAvailable()) {
		sysLogPrintf(LOG_ERROR, "upscale: %s", installStatus);
		exit(1);
	}

	if (!upscaleStart()) {
		sysLogPrintf(LOG_ERROR, "upscale: %s", statusText);
		exit(1);
	}

	while (state != UPSCALE_DONE && state != UPSCALE_FAILED) {
		const s32 pct = upscaleGetPercent();

		upscaleTick();

		if (pct / 10 != last) {
			last = pct / 10;
			sysLogPrintf(LOG_NOTE, "upscale: %s %d%%", statusText, pct);
		}

		// Only the writing-out stage wants the main thread; the rest is a
		// worker, and spinning on it would take a core off the upscaler.
		if (state != UPSCALE_PREPARING) {
			SDL_Delay(250);
		}
	}

	sysLogPrintf(LOG_NOTE, "upscale: %s", statusText);

	exit(state == UPSCALE_DONE ? 0 : 1);
}

void upscaleTick(void)
{
	if (state == UPSCALE_PREPARING) {
		if (SDL_AtomicGet(&cancelled)) {
			free(poolBuffer);
			poolBuffer = NULL;
			state = UPSCALE_IDLE;
			upscaleSetStatus("Cancelled");
			return;
		}

		upscalePrepareChunk();
		return;
	}

	if (state == UPSCALE_RUNNING || state == UPSCALE_FINISHING) {
		state = SDL_AtomicGet(&workerStage);

		// The upscaler says nothing this can rely on across versions, so
		// progress is how many images it has written. Counting them is a
		// directory read, so it happens about once a second rather than every
		// frame.
		if (state == UPSCALE_RUNNING && ++pollTimer >= 60) {
			const s32 done = fsScanDir(outDir, NULL, NULL);

			pollTimer = 0;

			if (done > 0) {
				SDL_AtomicSet(&workerCount, done);
			}
		}

		if (SDL_AtomicGet(&workerDone)) {
			SDL_WaitThread(worker, NULL);
			worker = NULL;

			if (SDL_AtomicGet(&workerFailed)) {
				state = UPSCALE_FAILED;
				upscaleSetStatus("The upscaler failed - see the log");
			} else {
				s32 i;

				state = UPSCALE_DONE;

				// Select it, rather than telling someone where to find what
				// they just waited ten minutes for.
				texpackRefreshPacks();

				for (i = 0; i < texpackGetNumPacks(); i++) {
					if (!strcmp(texpackGetPackName(i), packName)) {
						texpackSetSelectedPack(i);
						break;
					}
				}

				upscaleSetStatus("Done - %s is now selected", packName);
			}
		}
	}
}

PD_CONSTRUCTOR static void upscaleConfigInit(void)
{
	configRegisterString("Mod.UpscaylPath", upscaylPath, sizeof(upscaylPath));
	configRegisterString("Mod.UpscaylNcnnTag", ncnnTag, sizeof(ncnnTag));
	configRegisterInt("Mod.UpscaleScale", &optScale, 2, 4);
	configRegisterInt("Mod.UpscaleCompress", &optCompress, 0, 100);
	configRegisterInt("Mod.UpscaleTta", &optTta, 0, 1);
	configRegisterInt("Mod.UpscaleGpu", &optGpu, -1, 7);
	configRegisterInt("Mod.UpscaleTileSize", &optTileSize, 0, 1024);
	configRegisterInt("Mod.UpscalePadding", &optPadding, 0, 32);
}
