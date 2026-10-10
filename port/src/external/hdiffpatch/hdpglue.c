/**
 * See hdpglue.h. Built without HDiffPatch's threads and window diffs, which
 * the one patch this is for does not use; its lzma plugin is the one in
 * decompress_plugin_demo.h over the port's own LZMA SDK (9.20, whose
 * allocator takes a plain pointer where the newer SDK's takes ISzAllocPtr).
 */
#include <stdlib.h>
#include <string.h>
#include "../lzma/LzmaDec.h"
typedef void *ISzAllocPtr;
#define _IsNeedIncludeDefaultCompressHead 0
#define _CompressPlugin_lzma
#define _ChecksumPlugin_fadler64
#include "file_for_patch.h"
#include "dirDiffPatch/dir_patch/dir_patch.h"
#include "libHDiffPatch/HPatch/patch.h"
#include "decompress_plugin_demo.h"
#include "checksum_plugin_demo.h"
#include "hdpglue.h"

#define HDP_CACHE_SIZE (16 << 20)
#define HDP_MAX_OPEN_FILES 64

struct hdpout {
	size_t oldlen;
	size_t newlen;
	int written;
	hpatch_FileError_t fileError;
};

static hpatch_BOOL hdpMakeDir(IDirPatchListener *listener, const char *dir)
{
	return hpatch_makeNewDir(dir);
}

/**
 * A file the patch leaves as it is. Under its own name the old folder still
 * has it, and the overlay is read first and falls back to the old folder, so
 * it is not copied. Under a new name - a rename is a "same" pair too, which is
 * how the Community Edition's sf1 and sf2 are the release's surface - it is.
 */
static hpatch_BOOL hdpSame(IDirPatchListener *listener, const char *oldName, const char *newName,
		hpatch_ICopyDataListener *copyListener)
{
	struct hdpout *out = (struct hdpout *)listener->listenerImport;

	if (strlen(oldName) >= out->oldlen && strlen(newName) >= out->newlen
			&& strcmp(oldName + out->oldlen, newName + out->newlen) == 0) {
		return hpatch_TRUE;
	}

	out->written++;

	return TDirPatcher_copyFile(oldName, newName, copyListener, &out->fileError);
}

static hpatch_BOOL hdpOpenNew(IDirPatchListener *listener, hpatch_TFileStreamOutput *file,
		const char *name, hpatch_StreamPos_t size)
{
	struct hdpout *out = (struct hdpout *)listener->listenerImport;

	out->written++;

	return hpatch_TFileStreamOutput_open(file, name, size);
}

static hpatch_BOOL hdpCloseNew(IDirPatchListener *listener, hpatch_TFileStreamOutput *file)
{
	return hpatch_TFileStreamOutput_close(file);
}

int hdpListOldRefs(const char *diffPath, hdpnamefn fn, void *arg)
{
	hpatch_TFileStreamInput diff;
	TDirPatcher dp;
	const TDirDiffInfo *info = NULL;
	char name[hpatch_kPathMaxSize];
	int ok = 0;

	hpatch_TFileStreamInput_init(&diff);
	TDirPatcher_init(&dp);

	if (hpatch_TFileStreamInput_open(&diff, diffPath)
			&& TDirPatcher_open(&dp, &diff.base, &info) && info->isDirDiff
			&& TDirPatcher_loadDirData(&dp, &lzmaDecompressPlugin, "", "")) {
		for (size_t i = 0; i < dp.dirDiffHead.oldRefFileCount; i++) {
			const char *path = TDirPatcher_getOldRefPathByRefIndex(&dp, i, name, name + sizeof(name));

			if (path) {
				fn(path, arg);
			}
		}

		// and the old side of every rename, which is copied (hdpSame()); the
		// new names are the output's, whose count TDirPatcher_openNewDirAsStream()
		// would set
		dp._newDir.newPathCount = dp.dirDiffHead.newPathCount;

		for (size_t i = 0; i < dp.dirDiffHead.sameFilePairCount; i++) {
			const hpatch_TSameFilePair *pair = &dp._newDir.dataSamePairList[i];
			char newName[hpatch_kPathMaxSize];
			const char *oldPath = TDirPatcher_getOldPathByIndex(&dp, pair->oldIndex, name, name + sizeof(name));
			const char *newPath = TDirPatcher_getNewPathByIndex(&dp, pair->newIndex, newName, newName + sizeof(newName));

			if (oldPath && newPath && strcmp(oldPath, newPath) != 0) {
				fn(oldPath, arg);
			}
		}

		ok = 1;
	}

	TDirPatcher_close(&dp);
	hpatch_TFileStreamInput_close(&diff);

	return ok;
}

int hdpApplyOverlay(const char *oldDir, const char *diffPath, const char *outDir)
{
	hpatch_TFileStreamInput diff;
	TDirPatcher dp;
	const TDirDiffInfo *info = NULL;
	const hpatch_TStreamInput *oldStream = NULL;
	const hpatch_TStreamOutput *newStream = NULL;
	TPatchChecksumSet checks;
	struct hdpout out = { strlen(oldDir), strlen(outDir), 0, 0 };
	IDirPatchListener listener = { &out, hdpMakeDir, hdpSame, hdpOpenNew, hdpCloseNew };
	unsigned char *cache = malloc(HDP_CACHE_SIZE);
	int result = 0;

	hpatch_TFileStreamInput_init(&diff);
	TDirPatcher_init(&dp);
	memset(&checks, 0, sizeof(checks));
	checks.checksumPlugin = &fadler64ChecksumPlugin;
	checks.isCheck_diffData = 1;
	checks.isCheck_oldRefData = 1;
	checks.isCheck_newRefData = 1;

	if (!cache) {
		result = -1;
	} else if (!hpatch_TFileStreamInput_open(&diff, diffPath)
			|| !TDirPatcher_open(&dp, &diff.base, &info) || !info->isDirDiff) {
		result = -1;
	} else if (!TDirPatcher_loadDirData(&dp, &lzmaDecompressPlugin, oldDir, outDir)) {
		result = -2;
	} else if (!TDirPatcher_checksum(&dp, &checks, cache, cache + HDP_CACHE_SIZE)) {
		result = -3;
	} else if (!TDirPatcher_openOldRefAsStream(&dp, HDP_MAX_OPEN_FILES, &oldStream)) {
		result = -4;
	} else if (!TDirPatcher_openNewDirAsStream(&dp, &listener, &newStream)) {
		result = -5;
	} else if (!TDirPatcher_patch(&dp, newStream, oldStream, cache, cache + HDP_CACHE_SIZE, 1)) {
		result = -6;
	} else {
		result = out.written;
	}

	TDirPatcher_closeNewDirStream(&dp);
	TDirPatcher_closeOldRefStream(&dp);
	TDirPatcher_close(&dp);
	hpatch_TFileStreamInput_close(&diff);
	free(cache);

	return result;
}

int hdpApplyMem(const unsigned char *old, size_t oldLen, const unsigned char *diff, size_t diffLen,
		unsigned char **out, size_t *outLen)
{
	hpatch_compressedDiffInfo info;
	unsigned char *data;

	*out = NULL;
	*outLen = 0;

	if (!getCompressedDiffInfo_mem(&info, diff, diff + diffLen) || info.oldDataSize != oldLen) {
		return 0;
	}

	data = malloc(info.newDataSize ? info.newDataSize : 1);

	if (!data) {
		return 0;
	}

	if (!patch_decompress_mem(data, data + info.newDataSize, old, old + oldLen, diff, diff + diffLen, &lzmaDecompressPlugin)) {
		free(data);
		return 0;
	}

	*out = data;
	*outLen = info.newDataSize;

	return 1;
}
