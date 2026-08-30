#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <PR/ultratypes.h>
#include "config.h"
#include "system.h"
#include "platform.h"
#include "utils.h"
#include "fs.h"
#ifdef PLATFORM_WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#endif

#define DEFAULT_BASEDIR_NAME "data"

static char baseDir[FS_MAXPATH + 1]; // replaces $B
// Mod directories are overlaid on top of the base directory and searched in
// the order given, so the first one that has a file wins. $M expands to the
// first. The stock port had a single slot; mods that ship several directories
// need more than one.
#define FS_MAXMODDIRS 8
static char modDirs[FS_MAXMODDIRS][FS_MAXPATH + 1];
static s32 numModDirs;

// How many mod dirs take part in the general file search. Only the first does.
//
// Mod suites ship a full asset set per directory - textures, character models,
// text banks - all under the same names. Letting every mounted directory
// overlay would have one mod's textures replace stock ones everywhere, not
// just in its own levels. Directories past the first are reached solely
// through file slots pinned to them by the mod loader.
static s32 numOverlayModDirs;
static char saveDir[FS_MAXPATH + 1]; // replaces $S
static char homeDir[FS_MAXPATH + 1]; // replaces $H
static char exeDir[FS_MAXPATH + 1];  // replaces $E

static s32 fsPathIsWritable(const char *path)
{
#ifdef PLATFORM_WIN32
	// on windows access() on directories will only check if the directory exists, so
	char tmp[FS_MAXPATH + 1] = { 0 };
	snprintf(tmp, sizeof(tmp), "%s/.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (f) {
		fclose(f);
		remove(tmp);
		return 1;
	}
	return 0;
#else
	return (access(path, W_OK) == 0);
#endif
}

s32 fsPathIsAbsolute(const char *path)
{
 return (path[0] == '/' || (isalpha(path[0]) && path[1] == ':'));
}

s32 fsPathIsCwdRelative(const char *path)
{
	// ., .., ./, ../
	return (path[0] == '.' && (path[1] == '.' || path[1] == '/' || path[1] == '\\' || path[1] == '\0'));
}

const char *fsFullPath(const char *relPath)
{
	static char pathBuf[FS_MAXPATH + 1];

	if (relPath[0] == '$') {
		// expandable placeholder $X; will be replaced with the corresponding path, if any
		const char *expStr = NULL;
		switch (relPath[1]) {
			case 'E': expStr = exeDir; break;
			case 'H': expStr = homeDir; break;
			case 'M': expStr = numModDirs ? modDirs[0] : NULL; break;
			case 'B': expStr = baseDir; break;
			case 'S': expStr = saveDir; break;
			default: break;
		}
		if (expStr) {
			const u32 len = strlen(expStr);
			if (len > 0) {
				memcpy(pathBuf, expStr, len);
				strncpy(pathBuf + len, relPath + 2, FS_MAXPATH - len);
				return pathBuf;
			}
		}
		// couldn't expand anything, return as is
		return relPath;
	} else if (!baseDir[0] || fsPathIsAbsolute(relPath) || fsPathIsCwdRelative(relPath)) {
		// user explicitly wants working directory or this is an absolute path or we have no baseDir set up yet
		return relPath;
	}

	// path relative to mod or base dir; this will be a read request, so check where the file actually is
	for (s32 i = 0; i < numOverlayModDirs; ++i) {
		snprintf(pathBuf, FS_MAXPATH, "%s/%s", modDirs[i], relPath);
		if (fsFileSize(pathBuf) >= 0) {
			return pathBuf;
		}
	}
	// fall back to basedir
	snprintf(pathBuf, FS_MAXPATH, "%s/%s", baseDir, relPath);
	return pathBuf;
}

s32 fsInit(void)
{
	sysGetExecutablePath(exeDir, FS_MAXPATH);

	// if this is set, default to exe path for everything
	const s32 portable = sysArgCheck("--portable");
	if (portable) {
		strcpy(homeDir, exeDir);
	} else {
		sysGetHomePath(homeDir, FS_MAXPATH);
	}

	// get path to base dir and expand it if needed
	const char *path = sysArgGetString("--basedir");
	if (!path) {
		// check if there's a `data` directory in working directory or homeDir, otherwise default to exe directory
		path = "$E/" DEFAULT_BASEDIR_NAME;
		if (!portable) {
			if (fsFileSize("./" DEFAULT_BASEDIR_NAME) >= 0) {
				path = "./" DEFAULT_BASEDIR_NAME;
			} else if (fsFileSize("$H/" DEFAULT_BASEDIR_NAME) >= 0) {
				path = "$H/" DEFAULT_BASEDIR_NAME;
			}
		}
	}
	strncpy(baseDir, fsFullPath(path), FS_MAXPATH);

	// get paths to the mod dirs and expand them if needed
	// mod directories are overlaid on top of the base directory, in order
	{
		// --moddir may be repeated. The remaining names are the flags used by
		// the All in One mod's launcher, accepted so its command line works
		// unchanged.
		static const char *const modDirArgs[] = {
			"--moddir",
			"--gexmoddir",
			"--kakarikomoddir",
			"--darknoonmoddir",
			"--goldfinger64moddir",
		};

		for (u32 a = 0; a < sizeof(modDirArgs) / sizeof(modDirArgs[0]); ++a) {
			for (s32 n = 0; ; ++n) {
				path = sysArgGetStringN(modDirArgs[a], n);
				if (!path) {
					break;
				}

				if (numModDirs >= FS_MAXMODDIRS) {
					sysLogPrintf(LOG_WARNING, "too many mod dirs, ignoring `%s`", path);
					break;
				}

				char *dst = modDirs[numModDirs];
				dst[0] = '\0';

				if (fsPathIsAbsolute(path) || fsPathIsCwdRelative(path) || path[0] == '$') {
					// path is explicit; check as-is
					if (fsFileSize(path) >= 0) {
						strncpy(dst, fsFullPath(path), FS_MAXPATH);
					}
				} else {
					// path is relative to workdir; try to find it
					const char *priority[] = { ".", "$E", "$H" };
					for (s32 i = 0; i < 2 + (portable != 0); ++i) {
						char *tmp = strFmt("%s/%s", priority[i], path);
						if (fsFileSize(tmp) >= 0) {
							strncpy(dst, fsFullPath(tmp), FS_MAXPATH);
							break;
						}
					}
				}

				if (dst[0]) {
					// only the first mod dir overlays; see numOverlayModDirs
					if (numModDirs == 0) {
						numOverlayModDirs = 1;
					}
					++numModDirs;
				} else {
					sysLogPrintf(LOG_WARNING, "could not find specified moddir `%s`", path);
				}
			}
		}
	}

	// get path to save dir and expand it if needed
	path = sysArgGetString("--savedir");
	if (!path) {
		if (portable) {
			path = "$E";
		} else {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_OSX)
			// check if there's a config in the working directory, otherwise default to homeDir
			if (fsFileSize("./" CONFIG_FNAME) >= 0) {
				path = ".";
			} else {
				path = "$H";
			}
#else
			// check if working directory is writable, otherwise default to homeDir
			if (fsPathIsWritable("./")) {
				path = ".";
			} else {
				sysLogPrintf(LOG_WARNING, "cannot write to working directory, will use %s for saves instead", homeDir);
				path = "$H";
			}
#endif
		}
	}

	strncpy(saveDir, fsFullPath(path), FS_MAXPATH);

	for (s32 i = 0; i < numModDirs; ++i) {
		sysLogPrintf(LOG_NOTE, " mod dir: %s%s", modDirs[i], i < numOverlayModDirs ? "" : " (maps only)");
	}
	sysLogPrintf(LOG_NOTE, "base dir: %s", baseDir);
	sysLogPrintf(LOG_NOTE, "save dir: %s", saveDir);

	return 0;
}

/**
 * Call cb() once per regular file in a directory. Returns the number of
 * entries visited, or -1 if the directory could not be opened.
 *
 * Used to discover what a mod ships rather than requiring it to list its
 * contents somewhere.
 */
s32 fsScanDir(const char *path, fsScanCallback cb, void *arg)
{
	const char *full = fsFullPath(path);
	s32 count = 0;

#ifdef PLATFORM_WIN32
	char pattern[FS_MAXPATH + 1];
	snprintf(pattern, sizeof(pattern), "%s/*", full);

	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return -1;
	}

	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			if (cb) {
				cb(fd.cFileName, arg);
			}
			++count;
		}
	} while (FindNextFile(h, &fd));

	FindClose(h);
#else
	DIR *d = opendir(full);
	if (!d) {
		return -1;
	}

	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		if (cb) {
			cb(e->d_name, arg);
		}
		++count;
	}

	closedir(d);
#endif

	return count;
}

s32 fsGetNumModDirs(void)
{
	return numModDirs;
}

/**
 * Path of the nth mod directory, or NULL. Used to pin a file to one specific
 * mod rather than letting the search order decide.
 */
const char *fsGetModDirAt(s32 index)
{
	if (index < 0 || index >= numModDirs) {
		return NULL;
	}

	return modDirs[index];
}

const char *fsGetModDir(void)
{
	return numModDirs ? modDirs[0] : NULL;
}

s32 fsFileLoadTo(const char *name, void *dst, u32 dstSize)
{
	const char *fullName = fsFullPath(name);

	FILE *f = fopen(fullName, "rb");
	if (!f) {
		return -1;
	}

	fseek(f, 0, SEEK_END);
	const s32 size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size < 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoadTo: empty file or invalid size (%d): %s", size, fullName);
		fclose(f);
		return -1;
	}

	if ((u32)size > dstSize) {
		sysLogPrintf(LOG_ERROR, "fsFileLoadTo: file too big for buffer (%u > %u): %s", size, dstSize, fullName);
		fclose(f);
		return -1;
	}

	fread(dst, 1, size, f);
	fclose(f);

	return size;
}

void *fsFileLoad(const char *name, u32 *outSize)
{
	const char *fullName = fsFullPath(name);

	FILE *f = fopen(fullName, "rb");
	if (!f) {
		sysLogPrintf(LOG_ERROR, "fsFileLoad: could not find file: %s", fullName);
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	const s32 size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size < 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoad: empty file or invalid size (%d): %s", size, fullName);
		fclose(f);
		return NULL;
	}

	void *buf = NULL;
	if (size) {
		buf = sysMemZeroAlloc(size + 1); // sick hack for a free null terminator
		if (!buf) {
			sysLogPrintf(LOG_ERROR, "fsFileLoad: could not alloc %d bytes for file: %s", size, fullName);
			fclose(f);
			return NULL;
		}
		fread(buf, 1, size, f);
	}

	fclose(f);

	if (outSize) {
		*outSize = size;
	}

	return buf;
}

s32 fsFileSize(const char *name)
{
	const char *fullName = fsFullPath(name);
	struct stat st;
	if (stat(fullName, &st) < 0) {
		return -1;
	} else {
		return st.st_size;
	}
}

FILE *fsFileOpenWrite(const char *name)
{
	return fopen(fsFullPath(name), "wb");
}

FILE *fsFileOpenRead(const char *name)
{
	return fopen(fsFullPath(name), "rb");
}

void fsFileFree(FILE *f)
{
	fclose(f);
}

s32 fsCreateDir(const char *path)
{
#ifdef PLATFORM_WIN32
	return _mkdir(fsFullPath(path));
#else
	return mkdir(fsFullPath(path), 0777);
#endif
}
