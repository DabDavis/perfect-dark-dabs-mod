// The build is -std=c11, which is strict enough that signal.h hides kill().
// Everything else this file needs off POSIX happens to be exposed anyway; this
// has to come before the first system header to be worth anything.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include <PR/os_thread.h>
#include <PR/os_cont.h>
#include "platform.h"
#include "audio.h"
#include "config.h"
#include "fs.h"
#include "input.h"
#include "record.h"
#include "system.h"
#include "video.h"

#ifdef PLATFORM_POSIX
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#endif

#define RECORD_DIR "$S/recordings"
#define RECORD_KEYNAME_LEN 32
#define RECORD_DEFAULT_KEY "F11"
#define RECORD_DEFAULT_ENCODER "ffmpeg"

/**
 * Four frames of slack between the game and the encoder.
 *
 * The queue is there to absorb the encoder taking longer on one frame than on
 * another, not to let it fall behind: a game frame with nowhere to put its
 * picture waits rather than dropping it, because a dropped frame in a raw
 * stream is not a shorter video, it is a video that runs fast from that point
 * on. So the game stutters when the encoder cannot keep up, and the recording
 * stays honest. libx264 at veryfast has never made that happen here.
 *
 * Four frames of 1280x720 is 11MB, allocated when recording starts and freed
 * when it stops.
 */
#define RECORD_VIDEO_QUEUE 4

// Half a second of 22kHz stereo, which is far more than the frame's worth that
// is ever outstanding. Sound is small enough that the ring can simply be large.
#define RECORD_AUDIO_QUEUE (64 * 1024)

// A hitch should not be paid back as a hundred duplicated frames.
#define RECORD_MAX_DUPES 8

// How long a frame may wait for room in the queue before the recording is
// declared lost. Long enough that a slow disk is not mistaken for a dead
// encoder, short enough that a dead encoder is not mistaken for a hang.
#define RECORD_STALL_MS 5000

#define RECORD_FPS_MIN 10
#define RECORD_FPS_MAX 120

// libx264's constant rate factor: lower is better and bigger.
#define RECORD_CRF_MIN 12
#define RECORD_CRF_MAX 34

static char keyName[RECORD_KEYNAME_LEN] = RECORD_DEFAULT_KEY;
static char encoderPath[FS_MAXPATH + 1] = RECORD_DEFAULT_ENCODER;
static s32 keyVk = -1; // -1 until keyName has been looked up
static s32 recordFps = 60;
static s32 recordCrf = 21;
static s32 recordIndicator = 1;

static bool active;
static bool finishing;
static s32 vidWidth;
static s32 vidHeight;
static s32 winWidth;  // what the window was when recording started, before
static s32 winHeight; // rounding down to the even size x264 wants
static u32 startTicks;
static u32 framesWritten;
static char outPath[FS_MAXPATH + 1];

// Written by the writer threads, read by the game thread to decide whether to
// give up. Only ever set from false to true, so no lock is needed to read it.
static bool encoderGone;

static SDL_mutex *vidMutex;
static SDL_cond *vidCanFill;  // a slot came free
static SDL_cond *vidCanDrain; // a slot was filled
static SDL_Thread *vidThread;
static u8 *vidFrames[RECORD_VIDEO_QUEUE];
static s32 vidRepeat[RECORD_VIDEO_QUEUE];
static s32 vidHead, vidTail, vidCount;
static f64 vidNextFrameTime;

static SDL_mutex *sndMutex;
static SDL_cond *sndCanDrain;
static SDL_Thread *sndThread;
static u8 sndRing[RECORD_AUDIO_QUEUE];
static u32 sndHead, sndTail, sndCount;

#ifdef PLATFORM_POSIX
static pid_t encoderPid = -1;
static int vidFd = -1;
static int sndFd = -1;
#else
static FILE *vidPipe;
#endif

static f64 recordNow(void)
{
	return (f64)SDL_GetPerformanceCounter() / (f64)SDL_GetPerformanceFrequency();
}

/**
 * ffmpeg's argument list, built once so both the spawn and the log line it
 * prints on failure are talking about the same command.
 *
 * The video arrives bottom row first, which is the order glReadPixels gives, so
 * vflip is left to the encoder rather than paid for on the game's thread.
 *
 * yuv420p is not what x264 would pick for RGB input, but it is what everything
 * that is not a video editor can play.
 */
#ifdef PLATFORM_POSIX
static void recordBuildArgs(char *argv[], char *scratch, u32 scratchSize, const char *out)
{
	char *p = scratch;
	s32 n = 0;

#define ARG(...) do { \
		argv[n++] = p; \
		p += snprintf(p, scratchSize - (u32)(p - scratch), __VA_ARGS__) + 1; \
	} while (0)

	ARG("%s", encoderPath);
	ARG("-hide_banner");
	ARG("-nostdin");
	ARG("-loglevel"); ARG("error");
	ARG("-y");

	// probesize and analyzeduration are what stops ffmpeg reading five seconds
	// of one input before it will look at the other. Both streams are fully
	// described by the flags around them, so there is nothing to work out by
	// reading, and a demuxer that insists on reading anyway deadlocks the game:
	// it sits on the sound while the picture's pipe fills, and the game blocks
	// with a frame it cannot hand over.
	ARG("-f"); ARG("rawvideo");
	ARG("-probesize"); ARG("32");
	ARG("-analyzeduration"); ARG("0");
	ARG("-pixel_format"); ARG("rgb24");
	ARG("-video_size"); ARG("%dx%d", vidWidth, vidHeight);
	ARG("-framerate"); ARG("%d", recordFps);
	ARG("-thread_queue_size"); ARG("64");
	ARG("-i"); ARG("pipe:3");

	ARG("-f"); ARG("s16le");
	ARG("-probesize"); ARG("32");
	ARG("-analyzeduration"); ARG("0");
	ARG("-ar"); ARG("%d", audioGetSampleRate());
	ARG("-ac"); ARG("2");
	ARG("-thread_queue_size"); ARG("512");
	ARG("-i"); ARG("pipe:4");

	ARG("-vf"); ARG("vflip");
	ARG("-c:v"); ARG("libx264");
	ARG("-preset"); ARG("veryfast");
	ARG("-crf"); ARG("%d", recordCrf);
	ARG("-pix_fmt"); ARG("yuv420p");
	ARG("-c:a"); ARG("aac");
	ARG("-b:a"); ARG("128k");
	ARG("-movflags"); ARG("+faststart");
	ARG("%s", out);

#undef ARG

	argv[n] = NULL;
}

/**
 * ffmpeg reads the picture from fd 3 and the sound from fd 4, which is what
 * pipe:3 and pipe:4 name. Two pipes rather than one because a single stdin
 * cannot carry two streams, and letting ffmpeg interleave them itself is what
 * keeps the sound lined up with the picture without any timestamps of ours.
 */
static bool recordSpawnEncoder(const char *out)
{
	char scratch[4096];
	char *argv[64];
	int vpipe[2];
	int spipe[2];
	pid_t pid;

	recordBuildArgs(argv, scratch, sizeof(scratch), out);

	if (pipe(vpipe) != 0) {
		sysLogPrintf(LOG_ERROR, "record: could not create the video pipe: %s", strerror(errno));
		return false;
	}

	if (pipe(spipe) != 0) {
		sysLogPrintf(LOG_ERROR, "record: could not create the audio pipe: %s", strerror(errno));
		close(vpipe[0]);
		close(vpipe[1]);
		return false;
	}

	pid = fork();

	if (pid < 0) {
		sysLogPrintf(LOG_ERROR, "record: could not fork: %s", strerror(errno));
		close(vpipe[0]);
		close(vpipe[1]);
		close(spipe[0]);
		close(spipe[1]);
		return false;
	}

	if (pid == 0) {
		// The pipes were handed whatever descriptors were free, and those are
		// usually 3 to 6 - so dup2()ing straight onto 3 and 4 would land on one
		// of the other three. Both read ends move up out of the way first, then
		// everything the child does not need is closed, then they come back
		// down. dup2() clears FD_CLOEXEC on the copy, which is what lets 3 and 4
		// survive the exec and be pipe:3 and pipe:4 to ffmpeg.
		const int v = fcntl(vpipe[0], F_DUPFD, 10);
		const int a = fcntl(spipe[0], F_DUPFD, 10);

		if (v < 0 || a < 0) {
			_exit(127);
		}

		close(vpipe[0]);
		close(vpipe[1]);
		close(spipe[0]);
		close(spipe[1]);

		if (dup2(v, 3) < 0 || dup2(a, 4) < 0) {
			_exit(127);
		}

		close(v);
		close(a);
		execvp(argv[0], argv);
		_exit(127);
	}

	close(vpipe[0]);
	close(spipe[0]);

	encoderPid = pid;
	vidFd = vpipe[1];
	sndFd = spipe[1];

	return true;
}

static bool recordWriteAll(int fd, const u8 *buf, u32 len)
{
	while (len) {
		const ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}

		buf += n;
		len -= (u32)n;
	}

	return true;
}

static bool recordWriteVideo(const u8 *buf, u32 len) { return recordWriteAll(vidFd, buf, len); }
static bool recordWriteAudio(const u8 *buf, u32 len) { return recordWriteAll(sndFd, buf, len); }

static void recordCloseVideo(void)
{
	if (vidFd >= 0) {
		close(vidFd);
		vidFd = -1;
	}
}

static void recordCloseAudio(void)
{
	if (sndFd >= 0) {
		close(sndFd);
		sndFd = -1;
	}
}

/**
 * For the failure path only. A writer thread blocked handing a frame to an
 * encoder that has stopped reading but not exited would never come back, and
 * recordStop() waits for those threads - so the process goes first, and the
 * write fails with EPIPE instead.
 */
static void recordKillEncoder(void)
{
	if (encoderPid > 0) {
		kill(encoderPid, SIGKILL);
	}
}

static void recordReapEncoder(void)
{
	if (encoderPid > 0) {
		int status = 0;
		while (waitpid(encoderPid, &status, 0) < 0 && errno == EINTR) {
			// keep waiting
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
			sysLogPrintf(LOG_ERROR, "record: %s exited with %d", encoderPath, WEXITSTATUS(status));
		}
		encoderPid = -1;
	}
}
#else
/**
 * Without fork() there is no second pipe to hand the encoder, so this records
 * the picture only and says so. Muxing sound in would mean writing it to a file
 * of its own and running the encoder a second time when recording stops, which
 * is worth doing the day somebody records on Windows.
 */
static bool recordSpawnEncoder(const char *out)
{
	char cmd[2048];

	snprintf(cmd, sizeof(cmd),
			"\"%s\" -hide_banner -nostdin -loglevel error -y"
			" -f rawvideo -pixel_format rgb24 -video_size %dx%d -framerate %d -i pipe:0"
			" -vf vflip -c:v libx264 -preset veryfast -crf %d -pix_fmt yuv420p"
			" -movflags +faststart \"%s\"",
			encoderPath, vidWidth, vidHeight, recordFps, recordCrf, out);

	vidPipe = _popen(cmd, "wb");

	if (!vidPipe) {
		sysLogPrintf(LOG_ERROR, "record: could not start %s", encoderPath);
		return false;
	}

	sysLogPrintf(LOG_WARNING, "record: no sound on this platform, picture only");

	return true;
}

static bool recordWriteVideo(const u8 *buf, u32 len)
{
	return vidPipe && fwrite(buf, 1, len, vidPipe) == len;
}

static bool recordWriteAudio(const u8 *buf, u32 len)
{
	(void)buf;
	(void)len;
	return true;
}

static void recordCloseVideo(void)
{
	if (vidPipe) {
		_pclose(vidPipe);
		vidPipe = NULL;
	}
}

static void recordCloseAudio(void)
{
}

static void recordKillEncoder(void)
{
}

static void recordReapEncoder(void)
{
}
#endif

static int recordVideoThread(void *arg)
{
	(void)arg;

	for (;;) {
		const u8 *frame;
		s32 repeat;

		SDL_LockMutex(vidMutex);

		while (vidCount == 0 && !finishing) {
			SDL_CondWait(vidCanDrain, vidMutex);
		}

		if (vidCount == 0) {
			SDL_UnlockMutex(vidMutex);
			break;
		}

		frame = vidFrames[vidHead];
		repeat = vidRepeat[vidHead];

		SDL_UnlockMutex(vidMutex);

		// Safe to touch unlocked: the slot counts as full until it is released
		// below, and the game thread only ever fills a free one.
		while (repeat-- > 0) {
			if (!recordWriteVideo(frame, (u32)vidWidth * vidHeight * 3)) {
				encoderGone = true;
				break;
			}
		}

		SDL_LockMutex(vidMutex);
		vidHead = (vidHead + 1) % RECORD_VIDEO_QUEUE;
		vidCount--;
		SDL_CondSignal(vidCanFill);
		SDL_UnlockMutex(vidMutex);

		if (encoderGone) {
			break;
		}
	}

	recordCloseVideo();

	return 0;
}

static int recordAudioThread(void *arg)
{
	(void)arg;

	for (;;) {
		u8 chunk[4096];
		u32 len;

		SDL_LockMutex(sndMutex);

		while (sndCount == 0 && !finishing) {
			SDL_CondWait(sndCanDrain, sndMutex);
		}

		if (sndCount == 0) {
			SDL_UnlockMutex(sndMutex);
			break;
		}

		len = sndCount < sizeof(chunk) ? sndCount : sizeof(chunk);
		{
			const u32 tolerance = RECORD_AUDIO_QUEUE - sndHead;
			const u32 first = len < tolerance ? len : tolerance;
			memcpy(chunk, sndRing + sndHead, first);
			memcpy(chunk + first, sndRing, len - first);
		}

		sndHead = (sndHead + len) % RECORD_AUDIO_QUEUE;
		sndCount -= len;

		SDL_UnlockMutex(sndMutex);

		if (!recordWriteAudio(chunk, len)) {
			encoderGone = true;
			break;
		}
	}

	recordCloseAudio();

	return 0;
}

static bool recordPickFilename(char *out, u32 outSize)
{
	const time_t now = time(NULL);
	const struct tm *lt = localtime(&now);
	char stamp[32];
	char rel[FS_MAXPATH + 1];

	if (fsFileSize(RECORD_DIR) < 0 && fsCreateDir(RECORD_DIR) != 0) {
		sysLogPrintf(LOG_ERROR, "record: could not create %s", fsFullPath(RECORD_DIR));
		return false;
	}

	if (lt) {
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", lt);
	} else {
		snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)now);
	}

	snprintf(rel, sizeof(rel), RECORD_DIR "/pd-%s.mp4", stamp);
	strncpy(out, fsFullPath(rel), outSize - 1);
	out[outSize - 1] = '\0';

	return true;
}

static void recordFreeQueues(void)
{
	for (s32 i = 0; i < RECORD_VIDEO_QUEUE; i++) {
		free(vidFrames[i]);
		vidFrames[i] = NULL;
	}
}

static void recordStart(void)
{
	u32 frameSize;

	winWidth = videoGetWindowWidth();
	winHeight = videoGetWindowHeight();

	if (winWidth <= 0 || winHeight <= 0) {
		return;
	}

	// x264 wants even dimensions for yuv420p, and an odd window is easy to get
	// by dragging a corner. The row dropped is the bottom one, being where the
	// readback starts.
	vidWidth = winWidth & ~1;
	vidHeight = winHeight & ~1;
	frameSize = (u32)vidWidth * vidHeight * 3;

	if (!recordPickFilename(outPath, sizeof(outPath))) {
		return;
	}

	for (s32 i = 0; i < RECORD_VIDEO_QUEUE; i++) {
		vidFrames[i] = malloc(frameSize);

		if (!vidFrames[i]) {
			sysLogPrintf(LOG_ERROR, "record: could not alloc %u bytes of frame queue", frameSize);
			recordFreeQueues();
			return;
		}
	}

	if (!recordSpawnEncoder(outPath)) {
		recordFreeQueues();
		return;
	}

	vidHead = vidTail = vidCount = 0;
	sndHead = sndTail = sndCount = 0;
	framesWritten = 0;
	encoderGone = false;
	finishing = false;
	vidNextFrameTime = recordNow();
	startTicks = SDL_GetTicks();
	active = true;

	vidThread = SDL_CreateThread(recordVideoThread, "pd-rec-vid", NULL);
	sndThread = SDL_CreateThread(recordAudioThread, "pd-rec-snd", NULL);

	if (!vidThread || !sndThread) {
		sysLogPrintf(LOG_ERROR, "record: could not start the writer threads");
		recordStop();
		return;
	}

	sysLogPrintf(LOG_NOTE, "record: %dx%d at %dfps to %s", vidWidth, vidHeight, recordFps, outPath);
}

void recordStop(void)
{
	if (!active) {
		return;
	}

	active = false;

	if (encoderGone) {
		recordKillEncoder();
	}

	SDL_LockMutex(vidMutex);
	finishing = true;
	SDL_CondBroadcast(vidCanDrain);
	SDL_CondBroadcast(vidCanFill);
	SDL_UnlockMutex(vidMutex);

	SDL_LockMutex(sndMutex);
	SDL_CondBroadcast(sndCanDrain);
	SDL_UnlockMutex(sndMutex);

	// The wait is the encoder draining what it has been given and writing the
	// container's index, which is a moment of stutter at the end of a recording
	// rather than something to hide behind another thread: the file is not
	// playable until it is done, and saying so late would be worse.
	if (vidThread) {
		SDL_WaitThread(vidThread, NULL);
		vidThread = NULL;
	}

	if (sndThread) {
		SDL_WaitThread(sndThread, NULL);
		sndThread = NULL;
	}

	recordCloseVideo();
	recordCloseAudio();
	recordReapEncoder();
	recordFreeQueues();

	if (encoderGone) {
		// Nothing wrote the container's index, so what is on disk will not play.
		// An encoder that never started leaves the file empty, and that much can
		// be cleared up without wondering whose it is.
		if (fsFileSize(outPath) == 0) {
			remove(outPath);
		}

		sysLogPrintf(LOG_ERROR, "record: gave up on %s", outPath);
	} else {
		sysLogPrintf(LOG_NOTE, "record: %u frames to %s", framesWritten, outPath);
	}
}

void recordToggle(void)
{
	if (active) {
		recordStop();
	} else {
		recordStart();
	}
}

s32 recordIsActive(void)
{
	return active;
}

s32 recordShowsIndicator(void)
{
	return active && recordIndicator;
}

f32 recordGetElapsed(void)
{
	if (!active) {
		return 0.f;
	}

	return (f32)(SDL_GetTicks() - startTicks) / 1000.f;
}

void recordPushAudio(const void *buf, u32 len)
{
	if (!active || !buf || !len) {
		return;
	}

	SDL_LockMutex(sndMutex);

	if (RECORD_AUDIO_QUEUE - sndCount < len) {
		// Half a second behind means the encoder has stopped reading, and the
		// video queue is where that gets noticed. Losing sound is better than
		// blocking the tick that produces it.
		SDL_UnlockMutex(sndMutex);
		return;
	}

	{
		const u32 tolerance = RECORD_AUDIO_QUEUE - sndTail;
		const u32 first = len < tolerance ? len : tolerance;
		memcpy(sndRing + sndTail, buf, first);
		memcpy(sndRing, (const u8 *)buf + first, len - first);
	}

	sndTail = (sndTail + len) % RECORD_AUDIO_QUEUE;
	sndCount += len;

	SDL_CondSignal(sndCanDrain);
	SDL_UnlockMutex(sndMutex);
}

/**
 * Runs with the frame drawn and not yet presented.
 *
 * The video's clock is the wall clock, not the game's frame counter, so a
 * recording plays back at the speed the match was actually watched at. A game
 * frame that arrives before the next video frame is due is not captured at all;
 * one that arrives late is written more than once. Both are what a fixed rate
 * stream needs from a game whose frame rate is not fixed.
 */
static void recordPreSwap(void)
{
	const f64 period = 1.0 / (f64)recordFps;
	f64 now;
	s32 repeat = 0;
	u8 *slot;

	if (!active) {
		return;
	}

	if (encoderGone) {
		sysLogPrintf(LOG_ERROR, "record: the encoder stopped reading, giving up");
		recordStop();
		return;
	}

	if (videoGetWindowWidth() != winWidth || videoGetWindowHeight() != winHeight) {
		// A raw stream carries its size once, in the arguments ffmpeg was
		// started with, and there is no way to change it part way through.
		sysLogPrintf(LOG_WARNING, "record: the window was resized, stopping");
		recordStop();
		return;
	}

	now = recordNow();

	if (now < vidNextFrameTime) {
		return;
	}

	do {
		vidNextFrameTime += period;
		repeat++;
	} while (vidNextFrameTime <= now && repeat < RECORD_MAX_DUPES);

	if (vidNextFrameTime < now) {
		// Further behind than the cap allows: give up on the lost time rather
		// than spend the rest of the recording catching up on it.
		vidNextFrameTime = now + period;
	}

	// Waiting for a free slot is what makes the game stutter rather than drop a
	// frame, and it is only ever meant to be a wait. An encoder that has stopped
	// reading altogether would otherwise hang the game for good, which is why
	// the wait has an end: past it the recording is over, not the game.
	SDL_LockMutex(vidMutex);

	while (vidCount == RECORD_VIDEO_QUEUE && !encoderGone) {
		if (SDL_CondWaitTimeout(vidCanFill, vidMutex, RECORD_STALL_MS) == SDL_MUTEX_TIMEDOUT) {
			SDL_UnlockMutex(vidMutex);
			sysLogPrintf(LOG_ERROR, "record: the encoder stopped keeping up, stopping");
			encoderGone = true;
			recordStop();
			return;
		}
	}

	slot = vidFrames[vidTail];

	SDL_UnlockMutex(vidMutex);

	if (encoderGone || !slot) {
		return;
	}

	if (!videoReadScreenPixels(slot, vidWidth, vidHeight)) {
		sysLogPrintf(LOG_ERROR, "record: could not read the frame back, stopping");
		recordStop();
		return;
	}

	SDL_LockMutex(vidMutex);
	vidRepeat[vidTail] = repeat;
	vidTail = (vidTail + 1) % RECORD_VIDEO_QUEUE;
	vidCount++;
	framesWritten += (u32)repeat;
	SDL_CondSignal(vidCanDrain);
	SDL_UnlockMutex(vidMutex);
}

s32 recordGetKey(void)
{
	if (keyVk < 0) {
		if (!keyName[0] || !strcmp(keyName, "NONE")) {
			keyVk = 0;
		} else {
			keyVk = inputGetKeyByName(keyName);

			if (keyVk < 0) {
				keyVk = 0;
			}
		}
	}

	return keyVk;
}

void recordSetKey(s32 vk)
{
	if (vk <= 0 || vk >= VK_TOTAL_COUNT) {
		keyName[0] = '\0';
		keyVk = 0;
		return;
	}

	strncpy(keyName, inputGetKeyName(vk), sizeof(keyName) - 1);
	keyName[sizeof(keyName) - 1] = '\0';
	keyVk = vk;
}

s32 recordGetFps(void)
{
	return recordFps;
}

void recordSetFps(s32 fps)
{
	if (fps >= RECORD_FPS_MIN && fps <= RECORD_FPS_MAX) {
		recordFps = fps;
	}
}

s32 recordGetQuality(void)
{
	return recordCrf;
}

void recordSetQuality(s32 crf)
{
	if (crf >= RECORD_CRF_MIN && crf <= RECORD_CRF_MAX) {
		recordCrf = crf;
	}
}

s32 recordGetIndicator(void)
{
	return recordIndicator;
}

void recordSetIndicator(s32 on)
{
	recordIndicator = on ? 1 : 0;
}

void recordTick(void)
{
	const s32 vk = recordGetKey();

	if (vk > 0 && inputKeyJustPressed(vk)) {
		recordToggle();
	}
}

void recordInit(void)
{
#ifdef PLATFORM_POSIX
	// A write to a pipe whose reader has gone raises SIGPIPE, which by default
	// takes the game down. The writer threads check for the error instead.
	signal(SIGPIPE, SIG_IGN);
#endif

	vidMutex = SDL_CreateMutex();
	vidCanFill = SDL_CreateCond();
	vidCanDrain = SDL_CreateCond();
	sndMutex = SDL_CreateMutex();
	sndCanDrain = SDL_CreateCond();

	videoAddPreSwapCallback(recordPreSwap);
}

PD_CONSTRUCTOR static void recordConfigInit(void)
{
	configRegisterString("Mod.RecordKey", keyName, sizeof(keyName));
	configRegisterString("Mod.RecordEncoder", encoderPath, sizeof(encoderPath));
	configRegisterInt("Mod.RecordFps", &recordFps, RECORD_FPS_MIN, RECORD_FPS_MAX);
	configRegisterInt("Mod.RecordQuality", &recordCrf, RECORD_CRF_MIN, RECORD_CRF_MAX);
	configRegisterInt("Mod.RecordIndicator", &recordIndicator, 0, 1);
}
