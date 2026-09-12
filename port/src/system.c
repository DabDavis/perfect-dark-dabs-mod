#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "crashreport.h"

#ifdef PLATFORM_WIN32

#include <windows.h>

// on win32 we use waitable timers instead of nanosleep
typedef HANDLE WINAPI (*CREATEWAITABLETIMEREXAFN)(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);
static HANDLE timer;
static CREATEWAITABLETIMEREXAFN pfnCreateWaitableTimerExA;

// winapi also provides a yield macro
#define DO_YIELD() YieldProcessor()

// ask system for high performance GPU, if any
__attribute__((dllexport)) u32 NvOptimusEnablement = 1;
__attribute__((dllexport)) u32 AmdPowerXpressRequestHighPerformance = 1;

#else

#include <unistd.h>

// figure out how to yield
#if defined(PLATFORM_X86) || defined(PLATFORM_X86_64)
// this should work even if the code is not built with SSE enabled, at least on gcc and clang,
// but if it doesn't we'll have to use  __builtin_ia32_pause() or something
#include <immintrin.h>
#define DO_YIELD() _mm_pause()
#elif defined(PLATFORM_ARM) && (defined(PLATFORM_64BIT) || PLATFORM_ARM == 7 || PLATFORM_ARM == 8)
// same as YieldProcessor() on ARM Windows
#define DO_YIELD() __asm__ volatile("dmb ishst\n\tyield":::"memory")
#else
// fuck it
#define DO_YIELD() do { } while (0)
#endif

#endif

#define LOG_FNAME "pd.log"
#define CRASHLOG_FNAME "pd.crash.log"
#define USEC_IN_SEC 1000000ULL

static u64 startTick = 0;
static char logPath[2048];

static s32 sysArgc;
static const char **sysArgv;

static inline void sysLogSetPath(const char *fname)
{
	// figure out where the log is and clear it
	// try working dir first
	snprintf(logPath, sizeof(logPath), "./%s", fname);
	FILE *f = fopen(logPath, "wb");
	if (!f) {
		// try home dir
		sysGetHomePath(logPath, sizeof(logPath) - 1);
		strncat(logPath, "/", sizeof(logPath) - 1);
		strncat(logPath, fname, sizeof(logPath) - 1);
		f = fopen(logPath, "wb");
	}
	if (f) {
		fclose(f);
	}
}

void sysInitArgs(s32 argc, const char **argv)
{
	sysArgc = argc;
	sysArgv = argv;
}

void sysInit(void)
{
	startTick = sysGetMicroseconds();

	if (sysArgCheck("--log")) {
		sysLogSetPath(LOG_FNAME);
	}

#ifdef VERSION_HASH
	sysLogPrintf(LOG_NOTE, "version: " VERSION_BRANCH " " VERSION_HASH " (" VERSION_TARGET ")");
#endif

	char timestr[256];
	const time_t curtime = time(NULL);
	strftime(timestr, sizeof(timestr), "%d %b %Y %H:%M:%S", localtime(&curtime));
	sysLogPrintf(LOG_NOTE, "startup date: %s", timestr);

#ifdef PLATFORM_WIN32
	// this function is only present on Vista+, so try to import it from kernel32 by hand
	pfnCreateWaitableTimerExA = (CREATEWAITABLETIMEREXAFN)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExA");
	if (pfnCreateWaitableTimerExA) {
		// function exists, try to create a hires timer
		timer = pfnCreateWaitableTimerExA(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	}
	if (!timer) {
		// no function or hires timers not supported, fallback to lower resolution timer
		sysLogPrintf(LOG_WARNING, "SYS: hires waitable timers not available");
		timer = CreateWaitableTimerA(NULL, FALSE, NULL);
	}
#endif
}

s32 sysArgCheck(const char *arg)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			return 1;
		}
	}
	return 0;
}

/**
 * The command line this process was started with.
 *
 * For the updater, which hands it to the copy of the game it downloaded: a
 * player who started this one with --savedir or a list of mods gets the same
 * game back rather than a default one.
 */
const char *const *sysGetArgv(void)
{
	return sysArgv;
}

const char *sysArgGetString(const char *arg)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				return sysArgv[i + 1];
			}
		}
	}
	return NULL;
}

/**
 * Value of the nth (0-based) occurrence of a repeated argument, or NULL.
 */
const char *sysArgGetStringN(const char *arg, s32 n)
{
	s32 seen = 0;

	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				if (seen == n) {
					return sysArgv[i + 1];
				}
				++seen;
			}
		}
	}

	return NULL;
}

s32 sysArgGetInt(const char *arg, s32 defval)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				return strtol(sysArgv[i + 1], NULL, 0);
			}
		}
	}
	return defval;
}

u64 sysGetMicroseconds(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return ((u64)tv.tv_sec * USEC_IN_SEC + (u64)tv.tv_usec) - startTick;
}

s32 sysLogIsOpen(void)
{
	return (logPath[0] != '\0');
}

void sysLogPrintf(s32 level, const char *fmt, ...)
{
	static const char *prefix[3] = {
		"", "WARNING: ", "ERROR: "
	};

	char logmsg[2048];

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(logmsg, sizeof(logmsg), fmt, ap);
	va_end(ap);

	if (logPath[0]) {
		FILE *f = fopen(logPath, "ab");
		if (f) {
			fprintf(f, "%s%s\n", prefix[level], logmsg);
			fclose(f);
		}
	}

	FILE *fout = (level == LOG_NOTE) ? stdout : stderr;
	fprintf(fout, "%s%s\n", prefix[level], logmsg);

	// And into the ring a crash report is built from, which is the only copy
	// of these lines a player who never passed --log has.
	crashReportLogLine(logmsg);
}

/**
 * The last thing a player sees, and the one moment they will report a crash.
 *
 * The report is on disk before the box goes up, so pressing Close loses
 * nothing: the menu offers it again next time the game starts, with somewhere
 * to type what they were doing. Sending from here is the same report with no
 * note, because a message box cannot take text and the player who is about to
 * shut the game is the player most likely to say yes now and never again.
 *
 * Sending happens inside the crash: the process is already broken and this
 * asks it to do one more thing. That is a real risk and it is taken knowingly
 * - the file is written first, so the worst case is a second crash with the
 * report still there for next time.
 */
static void sysFatalDialog(const char *shown, const char *full)
{
	const char *path = crashReportSave(full);
	char text[2560];
	char err[256];
	SDL_MessageBoxButtonData buttons[2];
	SDL_MessageBoxData box;
	s32 hit = 0;

	if (path == NULL || !crashReportCanSend()) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fatal error", shown, NULL);
		return;
	}

	// What is in it, said before it is offered rather than after. A player
	// deciding whether to send their log should be told it is their log.
	snprintf(text, sizeof(text),
			"%s\n\nA report of this was saved.\n"
			"Sending it to Dab includes the message above, which build this is, "
			"your [Mod] settings and the last few hundred lines of the log.\n"
			"You can also send it from Send Crash Report on the Perfect Menu next time, "
			"with a note about what you were doing.",
			shown);

	memset(buttons, 0, sizeof(buttons));
	buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
	buttons[0].buttonid = 1;
	buttons[0].text = "Send report to Dab";
	buttons[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
	buttons[1].buttonid = 0;
	buttons[1].text = "Close";

	memset(&box, 0, sizeof(box));
	box.flags = SDL_MESSAGEBOX_ERROR;
	box.title = "Fatal error";
	box.message = text;
	box.numbuttons = 2;
	box.buttons = buttons;

	if (SDL_ShowMessageBox(&box, &hit) < 0) {
		// No message box at all - a headless run, or a display that has gone
		// with the crash. The report is still on disk.
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fatal error", shown, NULL);
		return;
	}

	if (hit != 1) {
		return;
	}

	err[0] = '\0';

	if (crashReportSend(path, "", err, sizeof(err))) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Report sent",
				"Thank you. The report is on its way to Dab.", NULL);
	} else {
		char msg[512];

		snprintf(msg, sizeof(msg),
				"The report could not be sent: %s\n\n"
				"It is still saved, and Send Crash Report on the Perfect Menu will try again.",
				err);

		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Not sent", msg, NULL);
	}
}

void sysFatalError(const char *fmt, ...)
{
	static s32 alreadyCrashed = 0;

	// The whole message, which a stack trace runs to thousands of characters
	// of, and the shorter copy the box shows. Static because this runs once
	// and the stack it would otherwise sit on may be the thing that overflowed.
	static char errmsg[8192];
	static char shown[2048];

	if (alreadyCrashed) {
		abort();
	}

	alreadyCrashed = 1;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errmsg, sizeof(errmsg), fmt, ap);
	va_end(ap);

	snprintf(shown, sizeof(shown), "%s", errmsg);

	sysLogPrintf(LOG_ERROR, "FATAL: %s", shown);

	fflush(stdout);
	fflush(stderr);

	sysFatalDialog(shown, errmsg);

	exit(1);
}

static s32 restartRequested;

void sysRequestRestart(void)
{
	restartRequested = 1;
}

s32 sysRestartRequested(void)
{
	return restartRequested;
}

void sysGetExecutablePath(char *outPath, const u32 outLen)
{
	// try asking SDL
	char *sdlPath = SDL_GetBasePath();

	if (sdlPath && *sdlPath) {
		// -1 to trim trailing slash
		const u32 len = strlen(sdlPath) - 1;
		if (len < outLen) {
			memcpy(outPath, sdlPath, len);
			outPath[len] = '\0';
		}
	} else if (sysArgc && sysArgv[0] && sysArgv[0][0]) {
		// get exe path from argv[0]
		strncpy(outPath, sysArgv[0], outLen - 1);
		outPath[outLen - 1] = '\0';
	} else if (outLen > 1) {
		// give up, use working directory instead
		outPath[0] = '.';
		outPath[1] = '\0';
	}

#ifdef PLATFORM_WIN32
	// replace all backslashes with forward slashes, windows supports both
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') {
			outPath[i] = '/';
		}
	}
#endif

	SDL_free(sdlPath);
}

void sysGetHomePath(char *outPath, const u32 outLen)
{
	// try asking SDL
	char *sdlPath = SDL_GetPrefPath("", "perfectdark");

	if (sdlPath && *sdlPath) {
		// -1 to trim trailing slash
		const u32 len = strlen(sdlPath) - 1;
		if (len < outLen) {
			memcpy(outPath, sdlPath, len);
			outPath[len] = '\0';
		}
	} else if (outLen > 1) {
		// give up, use working directory instead
		outPath[0] = '.';
		outPath[1] = '\0';
	}

#ifdef PLATFORM_WIN32
	// replace all backslashes with forward slashes, windows supports both
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') {
			outPath[i] = '/';
		}
	}
#endif

	SDL_free(sdlPath);
}

void *sysMemAlloc(const u32 size)
{
	return malloc(size);
}

void *sysMemZeroAlloc(const u32 size)
{
	return calloc(1, size);
}

void *sysMemRealloc(void *ptr, const u32 newSize)
{
	return realloc(ptr, newSize);
}

void sysMemFree(void *ptr)
{
	free(ptr);
}

void sysSleep(const s64 hns)
{
#ifdef PLATFORM_WIN32
	static LARGE_INTEGER li;
	li.QuadPart = -hns;
	SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
	WaitForSingleObject(timer, INFINITE);
#else
	const struct timespec spec = { 0, hns * 100 };
	nanosleep(&spec, NULL);
#endif
}

void sysCpuRelax(void)
{
	DO_YIELD();
}

const char *sysGetVersionString(void)
{
	return VERSION_BRANCH " " VERSION_HASH " (" VERSION_TARGET ")";
}
