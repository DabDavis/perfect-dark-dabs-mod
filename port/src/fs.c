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
	// Per thread, not shared. Every file call in the port comes through here -
	// fsFileOpenRead/Write, fsFileSize, fsCreateDir and fsScanDir all expand
	// their argument into this buffer and hand the result straight to the
	// system - so two threads composing a path at once produce one path made
	// of both, and the loser opens or creates a file under a name nobody
	// asked for.
	//
	// The ghost server client is the second thread: it loads a run to upload
	// and writes a downloaded one from its worker while the menu that started
	// it is still drawing, and a menu draw reads files of its own. Making the
	// scratch thread local is the whole fix and costs a kilobyte a thread;
	// the directories it expands from are written once at startup and read
	// only afterwards, so they need nothing.
	static _Thread_local char pathBuf[FS_MAXPATH + 1];

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


/**
 * Moving a player's saves when the save directory moves under them.
 *
 * The default became the executable's own folder, so that a copy of the game is
 * one folder holding everything - which is worth nothing to somebody who has
 * played before and would find a fresh eeprom, no unlocks and the simulant
 * count back at four. So the old directory's contents are copied across the
 * first time, once, and left where they are as well: this is somebody's save
 * file and the safe failure is a duplicate, not a hole.
 *
 * Copied, not moved, and only what the game writes - the config, the saves, the
 * ghosts, the exported setups. Screenshots and recordings are deliberately left
 * behind: they can be gigabytes, they are the player's to move, and nothing
 * stops working if they stay.
 */
struct fsCopyCtx {
	const char *srcDir;
	const char *dstDir;
	s32 copied;
};

static void fsCopyOneFile(const char *src, const char *dst)
{
	FILE *in;
	FILE *out;
	char buf[16 * 1024];
	size_t n;

	// Never over the top of something already there. A second run of this must
	// not undo whatever the player has done since the first.
	struct stat st;
	if (stat(dst, &st) == 0) {
		return;
	}

	in = fopen(src, "rb");
	if (!in) {
		return;
	}

	out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return;
	}

	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			break;
		}
	}

	fclose(in);
	fclose(out);
}

static void fsCopyEntry(const char *name, void *arg)
{
	struct fsCopyCtx *ctx = (struct fsCopyCtx *)arg;
	char src[FS_MAXPATH + 1];
	char dst[FS_MAXPATH + 1];
	struct stat st;

	snprintf(src, sizeof(src), "%s/%s", ctx->srcDir, name);

	// fsScanDir hands back directories on POSIX and not on Windows, so they are
	// filtered here rather than relied on either way. The ones worth taking are
	// named explicitly by the caller.
	if (stat(src, &st) != 0 || S_ISDIR(st.st_mode)) {
		return;
	}

	snprintf(dst, sizeof(dst), "%s/%s", ctx->dstDir, name);

	if (stat(dst, &st) != 0) {
		fsCopyOneFile(src, dst);
		ctx->copied++;
	}
}

// The files directly inside srcDir, into dstDir, which is created if it is not
// there. Returns how many were copied.
static s32 fsCopyDirFiles(const char *srcDir, const char *dstDir)
{
	struct fsCopyCtx ctx = { srcDir, dstDir, 0 };
	struct stat st;

	if (stat(srcDir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return 0;
	}

	if (stat(dstDir, &st) != 0) {
#ifdef PLATFORM_WIN32
		_mkdir(dstDir);
#else
		mkdir(dstDir, 0777);
#endif
	}

	fsScanDir(srcDir, fsCopyEntry, &ctx);

	return ctx.copied;
}

static void fsMigrateSaves(const char *from, const char *to)
{
	// What the game writes into a save directory, and nothing else.
	static const char *const subdirs[] = { "ghosts", "exported" };
	char src[FS_MAXPATH + 1];
	char dst[FS_MAXPATH + 1];
	s32 copied;

	copied = fsCopyDirFiles(from, to);

	for (u32 i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
		snprintf(src, sizeof(src), "%s/%s", from, subdirs[i]);
		snprintf(dst, sizeof(dst), "%s/%s", to, subdirs[i]);
		copied += fsCopyDirFiles(src, dst);
	}

	if (copied > 0) {
		sysLogPrintf(LOG_NOTE, "moved %d save files from %s - the originals are still there", copied, from);
	}
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
			// Where it used to go, and still goes when the executable's own
			// folder cannot be written to.
			const char *legacy;
#if defined(PLATFORM_LINUX) || defined(PLATFORM_OSX)
			// check if there's a config in the working directory, otherwise default to homeDir
			if (fsFileSize("./" CONFIG_FNAME) >= 0) {
				legacy = ".";
			} else {
				legacy = "$H";
			}
#else
			// check if working directory is writable, otherwise default to homeDir
			if (fsPathIsWritable("./")) {
				legacy = ".";
			} else {
				sysLogPrintf(LOG_WARNING, "cannot write to working directory, will use %s for saves instead", homeDir);
				legacy = "$H";
			}
#endif

			// One folder holding the game, its saves, its screenshots and its
			// recordings, because that is the folder a player already has open
			// and ~/.local/share is not. An installed copy, a read-only mount
			// or an app bundle cannot have it, and keeps the old location.
			if (fsPathIsWritable(exeDir)) {
				if (fsFileSize("$E/" CONFIG_FNAME) < 0) {
					char from[FS_MAXPATH + 1];

					// fsFullPath hands back one buffer, so the old directory is
					// taken out of it before anything else expands a path.
					strncpy(from, fsFullPath(legacy), FS_MAXPATH);
					from[FS_MAXPATH] = '\0';

					fsMigrateSaves(from, exeDir);
				}

				path = "$E";
			} else {
				path = legacy;

				// The old default is not a guarantee either: on Linux and macOS
				// it is the working directory whenever a config happens to be
				// sitting in one, and that can be anywhere, including the
				// read-only place the executable was just refused.
				if (!fsPathIsWritable(fsFullPath(path))) {
					sysLogPrintf(LOG_WARNING, "cannot write to %s, using %s for saves instead",
							fsFullPath(path), homeDir);
					path = "$H";
				}
			}
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

/**
 * Where the player's own files go - the pictures and videos they made.
 *
 * Beside the executable, because for an extracted release that is the folder
 * they already have open, and not inside a save directory that is under
 * ~/.local/share on Linux and somewhere nobody browses to on macOS. A file the
 * player made and cannot find is most of the way to not having been made.
 *
 * Where the executable's directory cannot be written to - installed under /usr,
 * a read-only mount, an app bundle - the save directory takes them instead,
 * because a recording that is awkward to find still beats one that could not be
 * written at all.
 *
 * dst gets the "$E/name" or "$S/name" form rather than an expanded path, so the
 * caller can go on composing filenames with it and leave the expansion to
 * fsFullPath() the way everything else here does.
 */
s32 fsChooseOutputDir(const char *name, char *dst, u32 dstSize)
{
	static const char *const roots[] = { "$E", "$S" };

	for (u32 i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		snprintf(dst, dstSize, "%s/%s", roots[i], name);

		// Existing and not a directory is not worth working around: it is
		// somebody's file, and the next root will do.
		if (fsFileSize(dst) < 0 && fsCreateDir(dst) != 0) {
			continue;
		}

		if (fsPathIsWritable(fsFullPath(dst))) {
			return 0;
		}
	}

	dst[0] = '\0';

	return -1;
}

s32 fsCreateDir(const char *path)
{
#ifdef PLATFORM_WIN32
	return _mkdir(fsFullPath(path));
#else
	return mkdir(fsFullPath(path), 0777);
#endif
}
