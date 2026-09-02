// The build is -std=c11, which is strict enough that signal.h hides kill().
// Everything else this file needs off POSIX happens to be exposed anyway; this
// has to come before the first system header to be worth anything.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
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
#define RECORD_CODECNAME_LEN 24
#define RECORD_DEFAULT_CODEC "auto"

// What the encoder detection encodes: three frames of black, big enough that no
// encoder refuses it for being under its minimum.
#define RECORD_PROBE_SIZE "320x240"

// One argv, and the scratch every argument in it points into.
#define RECORD_MAX_ARGS 80
#define RECORD_ARG_SCRATCH 4096

// What a shell here quotes an argument with.
#ifdef PLATFORM_WIN32
#define RECORD_QUOTE '"'
#else
#define RECORD_QUOTE '\''
#endif

/**
 * Four frames of slack between the game and the encoder.
 *
 * The queue is there to absorb the encoder taking longer on one frame than on
 * another. A game frame that finds it full is not captured at all and the clock
 * goes back, so the next frame through is written as many times as the wait was
 * worth: a dropped frame in a raw stream would be a video that runs fast from
 * that point on, and a duplicated one is only a video that stutters.
 *
 * What it must never do is wait. The game thread is what produces sound as well
 * as pictures, so blocking it here stops the audio that ffmpeg interleaves the
 * video against - and an encoder waiting for sound that is waiting for the
 * encoder is a recording that ends itself several seconds later, having frozen
 * the game first. That is what an 80 simulant match at 1080p60 did.
 *
 * Four frames of 1280x720 is 14MB, allocated when recording starts and freed
 * when it stops. Frames are four bytes a pixel: that is what the GPU reads back
 * without repacking and what a GPU encoder wants uploaded, so nothing in between
 * has an opinion about it.
 */
#define RECORD_VIDEO_QUEUE 4

// Half a second of 22kHz stereo, which is far more than the frame's worth that
// is ever outstanding. Sound is small enough that the ring can simply be large.
#define RECORD_AUDIO_QUEUE (64 * 1024)

// A hitch should not be paid back as a hundred duplicated frames.
#define RECORD_MAX_DUPES 8

// How long the writer threads are given to finish once a recording is stopped.
// Nothing waits on the encoder while one is running any more, so this is only
// the shutdown deadline: long enough that a slow disk is not mistaken for a
// dead encoder, short enough that a dead one is not mistaken for a hang.
#define RECORD_STALL_MS 5000

// How long the tail of a recording may take to drain once the encoder has been
// found to be too slow rather than gone. Longer than the stall above, because by
// then it is known to be alive and working through a backlog, and every frame it
// gets through is a frame of the recording that survives.
#define RECORD_DRAIN_MS 10000

#define RECORD_FPS_MIN 10
#define RECORD_FPS_MAX 120

// A quantiser: lower is better and bigger. It is libx264's CRF scale, which is
// also near enough every hardware encoder's constant-QP scale, so one number
// covers them all - see recordQualityValue() for the two that count differently.
#define RECORD_QUALITY_MIN 12
#define RECORD_QUALITY_MAX 34

static char keyName[RECORD_KEYNAME_LEN] = RECORD_DEFAULT_KEY;
static char encoderPath[FS_MAXPATH + 1] = RECORD_DEFAULT_ENCODER;
static char codecName[RECORD_CODECNAME_LEN] = RECORD_DEFAULT_CODEC;
static s32 keyVk = -1; // -1 until keyName has been looked up
static s32 recordFps = 60;
static s32 recordQuality = 21;
static s32 recordIndicator = 1;

static bool active;
static bool finishing;

// Set once a writer thread has been let go still holding the pipe and the
// frames it was writing. Nothing can be reclaimed from it and nothing can tell
// it to stop, so a second recording would hand its encoder to a thread that is
// still writing someone else's frames. One abandoned recording ends recording
// for the session; the alternative is a mess that looks like a corrupt file.
static bool abandoned;
static s32 vidWidth;
static s32 vidHeight;
static s32 winWidth;  // what the window was when recording started, before
static s32 winHeight; // rounding down to the even size h264 wants
static u32 startTicks;
static u32 framesWritten;
static char outPath[FS_MAXPATH + 1];

/**
 * The name ffmpeg knows the readback format by, from videoCaptureFormatName().
 *
 * It starts as bgra rather than nothing so that the menu can probe encoders
 * before a recording has ever set it. That is not a guess at what the GPU will
 * hand over: both formats are four bytes a pixel and every chain here takes
 * either, so which one it is cannot change whether an encoder works - only
 * recordStart() needs the real answer, and it asks for it.
 */
static const char *capFormat = "bgra";

// What the recording cost the game, to say at the end. The capture is the
// readback and the copy out of it; the skips are frames that found the queue
// full and were covered by repeating their neighbour, and are the number that
// says whether the encoder is keeping up.
static f64 statCaptureTime;
static u32 statCaptureCount;
static u32 statSkipped;

// Consecutive skips, and whether the player has been told. A hitch is a few of
// these in a row; a second of them is a machine that cannot record at this size
// and rate, which is worth saying once and not once a frame.
static u32 skipRun;
static bool warnedSlow;

// Written by the writer threads, read by the game thread to decide whether to
// give up. Only ever set from false to true, so no lock is needed to read it.
static bool encoderGone;

// The encoder is still running but cannot take frames as fast as they are made,
// so the recording is being ended early rather than thrown away. The game thread
// is the only one that touches it. See the stall in recordPreSwap().
static bool fellBehind;

// Counted up by each writer thread as it finishes. SDL2 has no join with a
// timeout, so this is what recordStop() waits on instead - see the comment there.
static SDL_atomic_t writersDone;
static s32 writersStarted;

static SDL_mutex *vidMutex;
static SDL_cond *vidCanDrain; // a slot was filled. There is no "a slot came
                              // free" to match it: the game thread never waits
                              // for one, it skips the frame and carries on.
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

/**
 * Two ways to get a recording out, and which one is used comes down to fork().
 *
 * With it, ffmpeg is handed both streams at once on two pipes and does the
 * muxing itself - one process, one pass, and the sound lines up with the
 * picture without anything here knowing what a timestamp is.
 *
 * Without it there is only popen(), which gives one pipe. So the picture goes
 * down that pipe to an ffmpeg of its own while the sound is written here as a
 * plain wav, and a second ffmpeg puts them in one file when recording stops -
 * copying the video rather than encoding it again, so what that costs is a file
 * copy. RECORD_FORCE_TWOPASS builds that path on a machine that has fork(),
 * which is the only way to find out whether it works.
 */
#if defined(PLATFORM_POSIX) && !defined(RECORD_FORCE_TWOPASS)
#define RECORD_TWOPASS 0
#else
#define RECORD_TWOPASS 1
#endif

#if !RECORD_TWOPASS
static pid_t encoderPid = -1;
static int vidFd = -1;
static int sndFd = -1;
#else
static FILE *vidPipe;
static FILE *sndFile;
static u32 sndBytes;
static char tmpVideoPath[FS_MAXPATH + 1];
static char tmpAudioPath[FS_MAXPATH + 1];
#endif

static f64 recordNow(void)
{
	return (f64)SDL_GetPerformanceCounter() / (f64)SDL_GetPerformanceFrequency();
}

/**
 * cmd.exe strips the outer pair of quotes off a command line that begins with
 * one, which is how a quoted path to ffmpeg becomes a command it cannot find.
 * Wrapping the whole line in another pair is the documented way round it, and
 * on anything else it would just be wrong.
 */
static const char *recordShellWrap(char *dst, u32 dstSize, const char *cmd)
{
#ifdef PLATFORM_WIN32
	snprintf(dst, dstSize, "\"%s\"", cmd);
	return dst;
#else
	(void)dst;
	(void)dstSize;
	return cmd;
#endif
}

/**
 * How a codec's quality setting reaches it. Most take the number the menu sets
 * as it stands; these two do not.
 */
enum {
	RECORD_Q_QP,   // a quantiser on the CRF scale, low is good
	RECORD_Q_PCT,  // nought to a hundred, high is good
	RECORD_Q_KBPS, // a bitrate, worked out from the size of the picture
};

struct recordCodec {
	const char *name;       // what goes in the config, and what the log calls it
	const char *label;      // what the menu calls it: whose hardware, not ffmpeg's name
	const char *encoder;    // ffmpeg's -c:v
	const char *device;     // global arguments before the inputs, %s = a device
	const char *filters[3]; // chains to try after vflip, best first, NULL ended
	const char *rc;         // rate control arguments, every %d gets the quality
	s32 qmap;
};

/**
 * The encoders worth trying, best first. Every one of them is on the GPU.
 *
 * The filter chains are the interesting half. What arrives from the game is four
 * byte pixels and every encoder here wants NV12, so something has to convert:
 * the first chain has the GPU do it, the second falls back to the CPU for a
 * driver that will not take the upload. That choice is worth more than the codec
 * is. Measured on one Polaris card at 1080p, per frame of CPU:
 *
 *     libx264 veryfast                        59ms, across six cores
 *     h264_vaapi fed NV12 converted by swscale  29ms
 *     h264_vaapi fed BGRA, converting itself     5ms
 *
 * Which chain a driver will take is not worth guessing at, so both are put past
 * ffmpeg once and the first that survives is kept - see recordResolveCodec().
 */
static const struct recordCodec recordCodecs[] = {
#if defined(PLATFORM_WIN32)
	{ "nvenc", "Nvidia NVENC", "h264_nvenc", NULL,
		{ "hwupload_cuda,scale_cuda=format=nv12", "format=nv12", NULL },
		"-preset p4 -tune hq -rc constqp -qp %d -b:v 0", RECORD_Q_QP },
	{ "amf", "AMD AMF", "h264_amf", NULL,
		{ "format=nv12", NULL },
		"-usage transcoding -rc cqp -qp_i %d -qp_p %d -qp_b %d", RECORD_Q_QP },
	{ "qsv", "Intel QuickSync", "h264_qsv", "-init_hw_device qsv=pdhw -filter_hw_device pdhw",
		{ "format=nv12,hwupload=extra_hw_frames=64", "format=nv12", NULL },
		"-preset veryfast -global_quality %d", RECORD_Q_QP },
	{ "mf", "Media Foundation", "h264_mf", NULL,
		{ "format=nv12", NULL },
		"-rate_control quality -quality %d", RECORD_Q_PCT },
#elif defined(PLATFORM_OSX)
	// VideoToolbox takes a constant quality on Apple silicon and a bitrate on
	// the Intel machines, and says so by refusing the option rather than by
	// anything that can be asked in advance. Both are listed; the probe picks.
	{ "videotoolbox", "VideoToolbox", "h264_videotoolbox", NULL,
		{ "format=nv12", NULL },
		"-q:v %d", RECORD_Q_PCT },
	{ "videotoolbox-vbr", "VideoToolbox VBR", "h264_videotoolbox", NULL,
		{ "format=nv12", NULL },
		"-b:v %dk", RECORD_Q_KBPS },
#else
	{ "nvenc", "Nvidia NVENC", "h264_nvenc", NULL,
		{ "hwupload_cuda,scale_cuda=format=nv12", "format=nv12", NULL },
		"-preset p4 -tune hq -rc constqp -qp %d -b:v 0", RECORD_Q_QP },
	{ "vaapi", "VAAPI (AMD/Intel)", "h264_vaapi", "-init_hw_device vaapi=pdhw:%s -filter_hw_device pdhw",
		{ "hwupload,scale_vaapi=format=nv12", "format=nv12,hwupload", NULL },
		"-rc_mode CQP -qp %d", RECORD_Q_QP },
	{ "qsv", "Intel QuickSync", "h264_qsv", "-init_hw_device qsv=pdhw -filter_hw_device pdhw",
		{ "format=nv12,hwupload=extra_hw_frames=64", "format=nv12", NULL },
		"-preset veryfast -global_quality %d", RECORD_Q_QP },
#endif
};

#define RECORD_CODEC_COUNT ((s32)(sizeof(recordCodecs) / sizeof(recordCodecs[0])))

/**
 * Not in the table above, because detection must never land on it: six cores of
 * x264 is the thing this was all about getting away from. It is here so that a
 * machine with no encoder on its GPU can still make a recording when its owner
 * asks for one by name in the config.
 */
static const struct recordCodec recordSoftwareCodec = {
	"software", "Software (Slow)", "libx264", NULL,
	{ "format=yuv420p", NULL },
	"-preset veryfast -crf %d", RECORD_Q_QP,
};

// The codec that answered, with the device and chain it answered on.
static struct recordEncoder {
	const struct recordCodec *codec;
	char device[192];
	const char *filters;
} chosen;

static bool codecResolved;

/**
 * The bitrate to ask for, for the encoders that will not take a quantiser.
 *
 * Bits per pixel per frame, a sixth at the good end and a fortieth at the small
 * end, which is roughly where the constant-quality encoders land on footage of
 * this kind when they are left to choose for themselves.
 */
static s32 recordBitrateKbps(void)
{
	const f32 t = (f32)(recordQuality - RECORD_QUALITY_MIN) / (f32)(RECORD_QUALITY_MAX - RECORD_QUALITY_MIN);
	const f32 bpp = 0.16f - t * 0.13f;
	const f64 kbps = (f64)vidWidth * (f64)vidHeight * (f64)recordFps * (f64)bpp / 1000.0;

	if (kbps < 1000.0) {
		return 1000;
	}

	return kbps > 100000.0 ? 100000 : (s32)kbps;
}

static s32 recordQualityValue(const struct recordCodec *c)
{
	switch (c->qmap) {
	case RECORD_Q_PCT:
		// The ends of the quantiser range mapped onto a percentage that counts
		// the other way, stopping short of both extremes: a hundred is a file
		// nobody wants and nought is not a recording.
		return 95 - (recordQuality - RECORD_QUALITY_MIN) * 55 / (RECORD_QUALITY_MAX - RECORD_QUALITY_MIN);
	case RECORD_Q_KBPS:
		return recordBitrateKbps();
	}

	return recordQuality;
}

/**
 * ffmpeg's arguments, built as an argv because that is what the two pipe backend
 * execs. The popen backend turns it back into a command line, which is a smaller
 * thing to get right than a second copy of every encoder setting would be.
 */
struct recordArgs {
	char *argv[RECORD_MAX_ARGS];
	s32 count;
	char scratch[RECORD_ARG_SCRATCH];
	u32 used;
	bool ok;
};

static void recordArgsInit(struct recordArgs *a)
{
	a->count = 0;
	a->used = 0;
	a->ok = true;
	a->argv[0] = NULL;
}

static void recordArgAdd(struct recordArgs *a, const char *fmt, ...)
{
	char *p = a->scratch + a->used;
	const u32 room = RECORD_ARG_SCRATCH - a->used;
	va_list ap;
	s32 n;

	if (!a->ok || a->count >= RECORD_MAX_ARGS - 1) {
		a->ok = false;
		return;
	}

	va_start(ap, fmt);
	n = vsnprintf(p, room, fmt, ap);
	va_end(ap);

	if (n < 0 || (u32)n >= room) {
		a->ok = false;
		return;
	}

	a->argv[a->count++] = p;
	a->used += (u32)n + 1;
	a->argv[a->count] = NULL;
}

// A space separated run of settings out of the table above, split into the
// separate entries execvp wants. Nothing in the table has a space inside an
// argument, which is what makes this legitimate.
static void recordArgAddSplit(struct recordArgs *a, const char *args)
{
	const char *p = args;

	while (p && *p) {
		const char *end;

		while (*p == ' ') {
			p++;
		}

		if (!*p) {
			break;
		}

		for (end = p; *end && *end != ' '; end++) {
			// to the end of this one
		}

		recordArgAdd(a, "%.*s", (int)(end - p), p);
		p = end;
	}
}

// The same, with the quality put into every %d first. Four copies because
// h264_amf wants it in three places and no encoder wants it in more.
static void recordArgAddRc(struct recordArgs *a, const char *rc, s32 q)
{
	char expanded[192];

	snprintf(expanded, sizeof(expanded), rc, q, q, q, q);
	recordArgAddSplit(a, expanded);
}

/**
 * The argv back into one command line, for popen() and system(), neither of
 * which has an argv form. Everything is quoted: the path to ffmpeg is the
 * player's, and a filter chain is full of commas and equals signs.
 */
static bool recordJoinArgs(char *dst, u32 dstSize, char *const argv[])
{
	u32 len = 0;

	for (s32 i = 0; argv[i]; i++) {
		const char *p = argv[i];

		if (len + 3 >= dstSize) {
			return false;
		}

		if (i) {
			dst[len++] = ' ';
		}

		dst[len++] = RECORD_QUOTE;

		for (; *p; p++) {
#ifndef PLATFORM_WIN32
			// The one character a single quoted string cannot hold: close,
			// escape it, open again. Windows has no equivalent problem because
			// a double quote cannot be in a path there in the first place.
			if (*p == '\'') {
				if (len + 5 >= dstSize) {
					return false;
				}
				memcpy(dst + len, "'\\''", 4);
				len += 4;
				continue;
			}
#endif
			if (len + 3 >= dstSize) {
				return false;
			}

			dst[len++] = *p;
		}

		dst[len++] = RECORD_QUOTE;
	}

	dst[len] = '\0';

	return true;
}

/**
 * Runs a command to completion with nothing to say for itself, and answers
 * whether it liked what it was given. Only the encoder detection uses this, and
 * only ever on the game's thread while nothing is being recorded.
 */
static bool recordRunQuiet(char *const argv[])
{
	char cmd[FS_MAXPATH * 4];
	char wrapped[FS_MAXPATH * 4 + 4];

	if (!recordJoinArgs(cmd, sizeof(cmd) - 16, argv)) {
		return false;
	}

#ifdef PLATFORM_WIN32
	strcat(cmd, " >NUL 2>&1");
#else
	strcat(cmd, " >/dev/null 2>&1");
#endif

	return system(recordShellWrap(wrapped, sizeof(wrapped), cmd)) == 0;
}

// Three frames of black through the exact chain a recording would use. An
// encoder being compiled into ffmpeg says nothing about there being a card under
// it that will take the work, and a driver that is present but wrong fails in
// the same place - so the question is asked the only way that answers it.
static bool recordProbeChain(const struct recordCodec *c, const char *device, const char *filters)
{
	struct recordArgs a;

	recordArgsInit(&a);

	recordArgAdd(&a, "%s", encoderPath);
	recordArgAdd(&a, "-hide_banner");
	recordArgAdd(&a, "-nostdin");
	recordArgAdd(&a, "-loglevel"); recordArgAdd(&a, "quiet");
	recordArgAddSplit(&a, device);
	recordArgAdd(&a, "-f"); recordArgAdd(&a, "lavfi");
	recordArgAdd(&a, "-i"); recordArgAdd(&a, "color=c=black:s=" RECORD_PROBE_SIZE ":r=%d", recordFps);
	recordArgAdd(&a, "-vf"); recordArgAdd(&a, "format=%s,vflip,%s", capFormat, filters);
	recordArgAdd(&a, "-c:v"); recordArgAdd(&a, "%s", c->encoder);
	recordArgAddRc(&a, c->rc, recordQualityValue(c));
	recordArgAdd(&a, "-frames:v"); recordArgAdd(&a, "3");
	recordArgAdd(&a, "-f"); recordArgAdd(&a, "null"); recordArgAdd(&a, "-");

	return a.ok && recordRunQuiet(a.argv);
}

/**
 * The render nodes a VAAPI device could be. One GPU is renderD128; a machine
 * with two has the one that can encode second as often as first, so both are
 * tried rather than guessed at.
 */
static s32 recordRenderNodes(char nodes[][32], s32 max)
{
	s32 n = 0;

#ifdef PLATFORM_LINUX
	for (s32 i = 128; i < 136 && n < max; i++) {
		char path[32];

		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);

		if (access(path, R_OK | W_OK) == 0) {
			snprintf(nodes[n++], 32, "%s", path);
		}
	}
#else
	(void)nodes;
	(void)max;
#endif

	return n;
}

// Every device this codec could be on, against every chain it could take.
static bool recordTryCodec(const struct recordCodec *c, struct recordEncoder *out)
{
	const bool needsNode = c->device && strstr(c->device, "%s") != NULL;
	char nodes[8][32];
	s32 nodeCount = 1;

	if (needsNode) {
		nodeCount = recordRenderNodes(nodes, 8);

		if (!nodeCount) {
			return false;
		}
	}

	for (s32 d = 0; d < nodeCount; d++) {
		char device[sizeof(out->device)] = "";

		if (needsNode) {
			snprintf(device, sizeof(device), c->device, nodes[d]);
		} else if (c->device) {
			snprintf(device, sizeof(device), "%s", c->device);
		}

		for (s32 f = 0; f < 3 && c->filters[f]; f++) {
			if (recordProbeChain(c, device, c->filters[f])) {
				out->codec = c;
				out->filters = c->filters[f];
				snprintf(out->device, sizeof(out->device), "%s", device);
				return true;
			}
		}
	}

	return false;
}

/**
 * Which encoder this machine has, worked out once and then remembered.
 *
 * Detection is the several hundred milliseconds of the probes above, on the
 * first recording of the first session and never again: the answer goes back
 * into Mod.RecordCodec, which is saved on the way out. Naming a codec there by
 * hand skips the search - including "software", the only way to reach libx264,
 * for a machine with nothing on its GPU.
 *
 * A name that no longer works falls back to searching rather than failing, so a
 * config carried to another machine sorts itself out.
 */
static const struct recordEncoder *recordResolveCodec(void)
{
	if (codecResolved) {
		return chosen.codec ? &chosen : NULL;
	}

	codecResolved = true;

	if (codecName[0] && strcmp(codecName, RECORD_DEFAULT_CODEC)) {
		const struct recordCodec *named = NULL;

		if (!strcmp(codecName, recordSoftwareCodec.name)) {
			named = &recordSoftwareCodec;
		} else {
			for (s32 i = 0; i < RECORD_CODEC_COUNT; i++) {
				if (!strcmp(codecName, recordCodecs[i].name)) {
					named = &recordCodecs[i];
					break;
				}
			}
		}

		if (named && recordTryCodec(named, &chosen)) {
			sysLogPrintf(LOG_NOTE, "record: %s, as the config asks for", chosen.codec->encoder);
			return &chosen;
		}

		sysLogPrintf(LOG_WARNING, "record: %s does not work here, looking for one that does", codecName);
	}

	for (s32 i = 0; i < RECORD_CODEC_COUNT; i++) {
		if (recordTryCodec(&recordCodecs[i], &chosen)) {
			snprintf(codecName, sizeof(codecName), "%s", chosen.codec->name);
			sysLogPrintf(LOG_NOTE, "record: %s, converting on the %s", chosen.codec->encoder,
					strstr(chosen.filters, "hwupload") == chosen.filters ? "GPU" : "CPU");
			return &chosen;
		}
	}

	chosen.codec = NULL;

	sysLogPrintf(LOG_ERROR, "record: no encoder on this GPU that %s can drive", encoderPath);
	sysLogPrintf(LOG_ERROR, "record: put Mod.RecordCodec=%s in " CONFIG_FNAME " to encode on the CPU instead",
			recordSoftwareCodec.name);

	return NULL;
}

/**
 * The video half of ffmpeg's arguments: the input the picture arrives on and
 * everything about how it is encoded. Shared, because the two backends differ
 * only in where the sound goes.
 *
 * The picture arrives bottom row first, which is the order the GPU reads it back
 * in, so vflip is left to ffmpeg - where it costs nothing, being a matter of
 * walking the rows the other way rather than moving any of them.
 */
static void recordAddVideoInput(struct recordArgs *a, const char *pipeName)
{
	// probesize and analyzeduration are what stops ffmpeg reading five seconds
	// of one input before it will look at the other. Both streams are fully
	// described by the flags around them, so there is nothing to work out by
	// reading, and a demuxer that insists on reading anyway deadlocks the game:
	// it sits on the sound while the picture's pipe fills, and the game blocks
	// with a frame it cannot hand over.
	recordArgAdd(a, "-f"); recordArgAdd(a, "rawvideo");
	recordArgAdd(a, "-probesize"); recordArgAdd(a, "32");
	recordArgAdd(a, "-analyzeduration"); recordArgAdd(a, "0");
	recordArgAdd(a, "-pixel_format"); recordArgAdd(a, "%s", capFormat);
	recordArgAdd(a, "-video_size"); recordArgAdd(a, "%dx%d", vidWidth, vidHeight);
	recordArgAdd(a, "-framerate"); recordArgAdd(a, "%d", recordFps);
	recordArgAdd(a, "-thread_queue_size"); recordArgAdd(a, "64");
	recordArgAdd(a, "-i"); recordArgAdd(a, "%s", pipeName);
}

static void recordAddVideoEncoder(struct recordArgs *a, const struct recordEncoder *enc)
{
	recordArgAdd(a, "-vf"); recordArgAdd(a, "vflip,%s", enc->filters);
	recordArgAdd(a, "-c:v"); recordArgAdd(a, "%s", enc->codec->encoder);
	recordArgAddRc(a, enc->codec->rc, recordQualityValue(enc->codec));
}

#if !RECORD_TWOPASS
static bool recordBuildArgs(struct recordArgs *a, const struct recordEncoder *enc, const char *out)
{
	recordArgsInit(a);

	recordArgAdd(a, "%s", encoderPath);
	recordArgAdd(a, "-hide_banner");
	recordArgAdd(a, "-nostdin");
	recordArgAdd(a, "-loglevel"); recordArgAdd(a, "error");
	recordArgAdd(a, "-y");

	// The hardware device, if the codec needs one named, has to be set up
	// before the inputs it will be used on.
	recordArgAddSplit(a, enc->device);

	recordAddVideoInput(a, "pipe:3");

	recordArgAdd(a, "-f"); recordArgAdd(a, "s16le");
	recordArgAdd(a, "-probesize"); recordArgAdd(a, "32");
	recordArgAdd(a, "-analyzeduration"); recordArgAdd(a, "0");
	recordArgAdd(a, "-ar"); recordArgAdd(a, "%d", audioGetSampleRate());
	recordArgAdd(a, "-ac"); recordArgAdd(a, "2");
	recordArgAdd(a, "-thread_queue_size"); recordArgAdd(a, "512");
	recordArgAdd(a, "-i"); recordArgAdd(a, "pipe:4");

	recordAddVideoEncoder(a, enc);

	recordArgAdd(a, "-c:a"); recordArgAdd(a, "aac");
	recordArgAdd(a, "-b:a"); recordArgAdd(a, "128k");
	recordArgAdd(a, "-movflags"); recordArgAdd(a, "+faststart");
	recordArgAdd(a, "%s", out);

	return a->ok;
}

/**
 * Between the fork and the exec, and only there: everything the child does not
 * need goes, so that the encoder starts with the three standard descriptors and
 * the two pipes and nothing else of ours.
 */
static void recordCloseInheritedFds(void)
{
#ifdef __GLIBC__
	closefrom(5);
#else
	const long max = sysconf(_SC_OPEN_MAX);

	for (int fd = 5; fd < (int)(max > 0 ? max : 4096); fd++) {
		close(fd);
	}
#endif
}

/**
 * ffmpeg reads the picture from fd 3 and the sound from fd 4, which is what
 * pipe:3 and pipe:4 name. Two pipes rather than one because a single stdin
 * cannot carry two streams, and letting ffmpeg interleave them itself is what
 * keeps the sound lined up with the picture without any timestamps of ours.
 */
static bool recordSpawnEncoder(const struct recordEncoder *enc, const char *out)
{
	static struct recordArgs a;
	int vpipe[2];
	int spipe[2];
	pid_t pid;

	if (!recordBuildArgs(&a, enc, out)) {
		sysLogPrintf(LOG_ERROR, "record: the encoder's arguments do not fit");
		return false;
	}

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

#ifdef F_SETPIPE_SZ
	// A 1080p frame is 8MB and a pipe holds 64KB, which is a hundred and thirty
	// handovers a frame between the writer thread and ffmpeg. A megabyte is what
	// an unprivileged process is usually allowed, and being refused it is not an
	// error - the recording works either way, it just wakes more often.
	fcntl(vpipe[1], F_SETPIPE_SZ, 1 << 20);
#endif

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
		const int s = fcntl(spipe[0], F_DUPFD, 10);

		if (v < 0 || s < 0) {
			_exit(127);
		}

		close(vpipe[0]);
		close(vpipe[1]);
		close(spipe[0]);
		close(spipe[1]);

		if (dup2(v, 3) < 0 || dup2(s, 4) < 0) {
			_exit(127);
		}

		close(v);
		close(s);

		// Everything above the two pipes is the game's: the X11 socket, the
		// audio device, the save files, the ROM. ffmpeg has no business holding
		// any of it, and a child that does keeps it alive for as long as it
		// runs.
		recordCloseInheritedFds();

		execvp(a.argv[0], a.argv);
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

/**
 * Whether the encoder is still there to be waited on, asked without waiting.
 *
 * From the game thread an encoder that is too slow and one that has stopped
 * altogether look identical - the queue is full and stays full - but they do not
 * end the same way, and this is the only thing that tells them apart. One that
 * has exited is reaped here, so that nothing later waits on a process that has
 * already gone.
 */
static bool recordEncoderAlive(void)
{
	int status = 0;
	pid_t r;

	if (encoderPid <= 0) {
		return false;
	}

	while ((r = waitpid(encoderPid, &status, WNOHANG)) < 0 && errno == EINTR) {
		// ask again
	}

	if (r == 0) {
		return true;
	}

	if (r == encoderPid) {
		if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
			sysLogPrintf(LOG_ERROR, "record: %s exited with %d", encoderPath, WEXITSTATUS(status));
		}

		encoderPid = -1;
	}

	return false;
}
#else

#ifdef PLATFORM_WIN32
#define recordPopen(cmd) _popen(cmd, "wb")
#define recordPclose(f)  _pclose(f)
#else
#define recordPopen(cmd) popen(cmd, "w")
#define recordPclose(f)  pclose(f)
#endif

/**
 * A 44 byte canonical wav header. The two lengths in it are not known until the
 * recording stops, so they go in as zero and are written again over the top.
 */
static void recordWriteWavHeader(FILE *f, u32 dataBytes)
{
	const s32 rate = audioGetSampleRate();
	const u16 channels = 2;
	const u16 bits = 16;
	const u16 blockAlign = channels * bits / 8;
	const u32 byteRate = (u32)rate * blockAlign;

#define U32(v) do { const u32 t_ = (u32)(v); const u8 b_[4] = { t_, t_ >> 8, t_ >> 16, t_ >> 24 }; fwrite(b_, 1, 4, f); } while (0)
#define U16(v) do { const u16 t_ = (u16)(v); const u8 b_[2] = { t_, t_ >> 8 }; fwrite(b_, 1, 2, f); } while (0)

	fwrite("RIFF", 1, 4, f);
	U32(36 + dataBytes);
	fwrite("WAVE", 1, 4, f);

	fwrite("fmt ", 1, 4, f);
	U32(16);
	U16(1); // PCM
	U16(channels);
	U32(rate);
	U32(byteRate);
	U16(blockAlign);
	U16(bits);

	fwrite("data", 1, 4, f);
	U32(dataBytes);

#undef U32
#undef U16
}

/**
 * The picture goes to an encoder of its own, into a file beside the one being
 * recorded rather than a temp directory - it is the size of the recording, and
 * a disk with room for one has room for the other.
 */
static bool recordSpawnEncoder(const struct recordEncoder *enc, const char *out)
{
	static struct recordArgs a;
	char cmd[FS_MAXPATH * 3];
	char wrapped[FS_MAXPATH * 3 + 4];

	snprintf(tmpVideoPath, sizeof(tmpVideoPath), "%s.video.mp4", out);
	snprintf(tmpAudioPath, sizeof(tmpAudioPath), "%s.audio.wav", out);

	recordArgsInit(&a);

	recordArgAdd(&a, "%s", encoderPath);
	recordArgAdd(&a, "-hide_banner");
	recordArgAdd(&a, "-nostdin");
	recordArgAdd(&a, "-loglevel"); recordArgAdd(&a, "error");
	recordArgAdd(&a, "-y");
	recordArgAddSplit(&a, enc->device);
	recordAddVideoInput(&a, "pipe:0");
	recordAddVideoEncoder(&a, enc);
	recordArgAdd(&a, "%s", tmpVideoPath);

	if (!a.ok || !recordJoinArgs(cmd, sizeof(cmd), a.argv)) {
		sysLogPrintf(LOG_ERROR, "record: the encoder's arguments do not fit");
		return false;
	}

	vidPipe = recordPopen(recordShellWrap(wrapped, sizeof(wrapped), cmd));

	if (!vidPipe) {
		sysLogPrintf(LOG_ERROR, "record: could not start %s", encoderPath);
		return false;
	}

	sndFile = fopen(tmpAudioPath, "wb");

	if (!sndFile) {
		sysLogPrintf(LOG_ERROR, "record: could not open %s for writing", tmpAudioPath);
		recordPclose(vidPipe);
		vidPipe = NULL;
		remove(tmpVideoPath);
		return false;
	}

	sndBytes = 0;
	recordWriteWavHeader(sndFile, 0);

	return true;
}

static bool recordWriteVideo(const u8 *buf, u32 len)
{
	return vidPipe && fwrite(buf, 1, len, vidPipe) == len;
}

static bool recordWriteAudio(const u8 *buf, u32 len)
{
	if (!sndFile || fwrite(buf, 1, len, sndFile) != len) {
		return false;
	}

	sndBytes += len;

	return true;
}

static void recordCloseVideo(void)
{
	if (vidPipe) {
		recordPclose(vidPipe);
		vidPipe = NULL;
	}
}

static void recordCloseAudio(void)
{
	if (sndFile) {
		// Over the top of the zeroes, now that there is a length to write.
		fseek(sndFile, 0, SEEK_SET);
		recordWriteWavHeader(sndFile, sndBytes);
		fclose(sndFile);
		sndFile = NULL;
	}
}

/**
 * There is no process handle to kill: popen() gives back a stream and nothing
 * else. An encoder that stops reading blocks its writer thread in fwrite() and
 * the recording is lost, which is the price of not having fork().
 */
static void recordKillEncoder(void)
{
}

/**
 * Unanswerable here, and answered no. popen() gives back a stream and no
 * process, so an encoder that is merely slow cannot be told from one that has
 * stopped - and without that, ending the recording early rather than losing it
 * would be a guess that hangs the game when it guesses wrong.
 */
static bool recordEncoderAlive(void)
{
	return false;
}

/**
 * The second pass. The video is copied rather than re-encoded, so this costs
 * what copying the file costs; the sound is encoded, being raw PCM until now.
 */
static void recordReapEncoder(void)
{
	char cmd[FS_MAXPATH * 4];
	char wrapped[FS_MAXPATH * 4 + 4];
	s32 status;

	if (!tmpVideoPath[0]) {
		return;
	}

	if (encoderGone) {
		// Neither half is finished, and half an mp4 has no index to play from.
		remove(tmpVideoPath);
		remove(tmpAudioPath);
		tmpVideoPath[0] = tmpAudioPath[0] = '\0';
		return;
	}

	snprintf(cmd, sizeof(cmd),
			"\"%s\" -hide_banner -nostdin -loglevel error -y -i \"%s\" -i \"%s\""
			" -c:v copy -c:a aac -b:a 128k -shortest -movflags +faststart \"%s\"",
			encoderPath, tmpVideoPath, tmpAudioPath, outPath);

	status = system(recordShellWrap(wrapped, sizeof(wrapped), cmd));

	if (status != 0) {
		sysLogPrintf(LOG_ERROR, "record: could not mux %s and the sound into %s",
				tmpVideoPath, outPath);
		// The picture alone is worth more than nothing, so it is left where it
		// is and named, rather than cleared away behind the player's back.
		sysLogPrintf(LOG_ERROR, "record: the picture is in %s", tmpVideoPath);
		remove(tmpAudioPath);
	} else {
		remove(tmpVideoPath);
		remove(tmpAudioPath);
	}

	tmpVideoPath[0] = tmpAudioPath[0] = '\0';
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
			if (!recordWriteVideo(frame, (u32)vidWidth * vidHeight * 4)) {
				encoderGone = true;
				break;
			}
		}

		SDL_LockMutex(vidMutex);
		vidHead = (vidHead + 1) % RECORD_VIDEO_QUEUE;
		vidCount--;
		SDL_UnlockMutex(vidMutex);

		if (encoderGone) {
			break;
		}
	}

	recordCloseVideo();

	SDL_AtomicIncRef(&writersDone);

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

	SDL_AtomicIncRef(&writersDone);

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
	const struct recordEncoder *enc;
	u32 frameSize;

	if (abandoned) {
		sysLogPrintf(LOG_ERROR, "record: not recording again after an abandoned one, restart the game");
		return;
	}

	winWidth = videoGetWindowWidth();
	winHeight = videoGetWindowHeight();

	if (winWidth <= 0 || winHeight <= 0) {
		return;
	}

	// H.264 wants even dimensions for 4:2:0 chroma, and an odd window is easy to
	// get by dragging a corner. The row dropped is the bottom one, being where
	// the readback starts.
	vidWidth = winWidth & ~1;
	vidHeight = winHeight & ~1;
	frameSize = (u32)vidWidth * vidHeight * 4;

	// Before the codec is chosen, because which pixels come out of the GPU is
	// the first thing the chain it is probed with has to know.
	capFormat = videoCaptureFormatName(videoCaptureStart(vidWidth, vidHeight));

	if (!capFormat) {
		sysLogPrintf(LOG_ERROR, "record: this renderer cannot hand frames back");
		return;
	}

	enc = recordResolveCodec();

	if (!enc) {
		sysLogPrintf(LOG_ERROR, "record: no encoder, not recording");
		videoCaptureStop();
		return;
	}

	if (!recordPickFilename(outPath, sizeof(outPath))) {
		videoCaptureStop();
		return;
	}

	for (s32 i = 0; i < RECORD_VIDEO_QUEUE; i++) {
		vidFrames[i] = malloc(frameSize);

		if (!vidFrames[i]) {
			sysLogPrintf(LOG_ERROR, "record: could not alloc %u bytes of frame queue", frameSize);
			recordFreeQueues();
			videoCaptureStop();
			return;
		}
	}

	if (!recordSpawnEncoder(enc, outPath)) {
		recordFreeQueues();
		videoCaptureStop();
		return;
	}

	vidHead = vidTail = vidCount = 0;
	sndHead = sndTail = sndCount = 0;
	framesWritten = 0;
	statCaptureTime = 0.0;
	statCaptureCount = 0;
	statSkipped = 0;
	skipRun = 0;
	warnedSlow = false;
	encoderGone = false;
	fellBehind = false;
	finishing = false;
	vidNextFrameTime = recordNow();
	startTicks = SDL_GetTicks();
	active = true;

	SDL_AtomicSet(&writersDone, 0);
	writersStarted = 0;

	vidThread = SDL_CreateThread(recordVideoThread, "pd-rec-vid", NULL);
	writersStarted += (vidThread != NULL);
	sndThread = SDL_CreateThread(recordAudioThread, "pd-rec-snd", NULL);
	writersStarted += (sndThread != NULL);

	if (!vidThread || !sndThread) {
		sysLogPrintf(LOG_ERROR, "record: could not start the writer threads");
		recordStop();
		return;
	}

	sysLogPrintf(LOG_NOTE, "record: %dx%d at %dfps, %s, to %s",
			vidWidth, vidHeight, recordFps, enc->codec->encoder, outPath);
}

/**
 * True once every writer thread that started has finished, or false if they are
 * still going after ms. Polled rather than waited on: the threads that have to
 * be given up on are the ones stuck in a write nothing can wake.
 */
static bool recordWaitForWriters(u32 ms)
{
	const u32 deadline = SDL_GetTicks() + ms;

	while (SDL_AtomicGet(&writersDone) < writersStarted) {
		if ((s32)(SDL_GetTicks() - deadline) >= 0) {
			return false;
		}

		SDL_Delay(10);
	}

	return true;
}

/**
 * The frame the GPU was still copying when the key was pressed.
 *
 * Reads at a ring behind, so at any moment one frame that has been asked for has
 * not been collected. Without this the recording would be a frame short of what
 * the player watched, which is not a sync problem but is a missing end.
 *
 * Only if there is a slot free: waiting for one here would mean waiting on the
 * encoder, and a recording being stopped has better things to do than block for
 * a sixtieth of a second at the end.
 */
static void recordDrainCapture(void)
{
	u8 *slot = NULL;

	SDL_LockMutex(vidMutex);

	if (vidCount < RECORD_VIDEO_QUEUE) {
		slot = vidFrames[vidTail];
	}

	SDL_UnlockMutex(vidMutex);

	if (!slot || !videoCaptureDrain(slot)) {
		return;
	}

	SDL_LockMutex(vidMutex);
	vidRepeat[vidTail] = 1;
	vidTail = (vidTail + 1) % RECORD_VIDEO_QUEUE;
	vidCount++;
	framesWritten++;
	SDL_CondSignal(vidCanDrain);
	SDL_UnlockMutex(vidMutex);
}

void recordStop(void)
{
	bool stuck;

	if (!active) {
		return;
	}

	active = false;

	if (encoderGone) {
		recordKillEncoder();
	} else {
		recordDrainCapture();
	}

	// Nothing else touches the GL context from here on, and the frames still
	// inside it have either been collected above or are not coming.
	videoCaptureStop();

	SDL_LockMutex(vidMutex);
	finishing = true;

	if (fellBehind) {
		// Every queued frame stands for several at the stream's rate, and the
		// encoder that cannot keep up is the one being asked to write them all
		// before the game moves again. One apiece is the difference between a
		// second of drain and half a minute of it; what it costs is a last
		// handful of frames that pass quickly, at the end of a recording that
		// was being cut short anyway. The slot the writer already took keeps
		// its repeats, having been read before this.
		for (s32 i = 0; i < RECORD_VIDEO_QUEUE; i++) {
			vidRepeat[i] = 1;
		}
	}

	SDL_CondBroadcast(vidCanDrain);
	SDL_UnlockMutex(vidMutex);

	SDL_LockMutex(sndMutex);
	SDL_CondBroadcast(sndCanDrain);
	SDL_UnlockMutex(sndMutex);

	// The wait is the encoder draining what it has been given and writing the
	// container's index, which is a moment of stutter at the end of a recording
	// rather than something to hide behind another thread: the file is not
	// playable until it is done, and saying so late would be worse.
	//
	// It is a wait with an end, though, because a writer sitting inside a write
	// to an encoder that has stopped reading and not exited cannot be woken -
	// and on the two-pass path popen() gives back a stream and no process to
	// kill. Past the end the threads are let go and their buffers deliberately
	// leaked rather than freed underneath them. A few megabytes is a cheaper
	// thing to lose than the session.
	stuck = !recordWaitForWriters(fellBehind ? RECORD_DRAIN_MS : RECORD_STALL_MS);

	if (stuck) {
		sysLogPrintf(LOG_ERROR, "record: the encoder never let go, abandoning the recording");
		encoderGone = true;
		abandoned = true;

		if (vidThread) {
			SDL_DetachThread(vidThread);
		}

		if (sndThread) {
			SDL_DetachThread(sndThread);
		}
	} else {
		if (vidThread) {
			SDL_WaitThread(vidThread, NULL);
		}

		if (sndThread) {
			SDL_WaitThread(sndThread, NULL);
		}
	}

	vidThread = NULL;
	sndThread = NULL;

	if (!stuck) {
		recordCloseVideo();
		recordCloseAudio();
		recordReapEncoder();
		recordFreeQueues();
	}

	if (encoderGone) {
		// Nothing wrote the container's index, so what is on disk will not open
		// in anything and no tool will repair it. It goes, rather than sit in
		// the recordings folder looking like a recording.
		remove(outPath);

		sysLogPrintf(LOG_ERROR, "record: gave up on %s", outPath);
	} else {
		const f64 n = statCaptureCount ? (f64)statCaptureCount : 1.0;

		// What the recording cost the game, per captured frame, and how often
		// the encoder had nowhere to put one. Skips are not lost time - each is
		// covered by repeating the frame either side of it - but a lot of them
		// is a recording that is smooth in the file and was not on the screen.
		sysLogPrintf(LOG_NOTE, "record: %u frames to %s (capture %.2fms, %u skipped)",
				framesWritten, outPath, statCaptureTime * 1000.0 / n, statSkipped);
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
 *
 * What videoCaptureRead() hands back is the frame from the call before this one,
 * because waiting for the one just drawn would mean waiting for the GPU to
 * finish it. That is a frame of latency and not a frame of drift: the stream is
 * timestamped by index and no index is skipped, so the sound stays where it was.
 */
static void recordPreSwap(void)
{
	const f64 period = 1.0 / (f64)recordFps;
	f64 prevFrameTime;
	f64 now;
	f64 mark;
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

	prevFrameTime = vidNextFrameTime;

	do {
		vidNextFrameTime += period;
		repeat++;
	} while (vidNextFrameTime <= now && repeat < RECORD_MAX_DUPES);

	if (vidNextFrameTime < now) {
		// Further behind than the cap allows: give up on the lost time rather
		// than spend the rest of the recording catching up on it.
		vidNextFrameTime = now + period;
	}

	// Nothing is waited for. A full queue means the encoder has not got through
	// what it already has, and the answer is to leave it alone until it does -
	// see the comment on RECORD_VIDEO_QUEUE for why waiting here is worse than
	// anything it could buy.
	SDL_LockMutex(vidMutex);

	if (vidCount == RECORD_VIDEO_QUEUE) {
		SDL_UnlockMutex(vidMutex);

		// The clock goes back, so this frame's worth of time is carried and the
		// next frame that does get a slot is written for both of them.
		vidNextFrameTime = prevFrameTime;
		statSkipped++;
		skipRun++;

		// A hitch is a handful of these; a second of them is a machine that
		// cannot record at this size and rate, and the player is better told
		// than left to wonder why the file is not smooth. Said once.
		if (skipRun >= recordFps && !warnedSlow) {
			warnedSlow = true;
			fellBehind = true;

			if (recordEncoderAlive()) {
				sysLogPrintf(LOG_WARNING,
						"record: %s cannot take %dx%d at %dfps - the recording is being padded out"
						" with repeated frames. A lower Recording Frame Rate is the fix.",
						chosen.codec ? chosen.codec->encoder : "the encoder",
						vidWidth, vidHeight, recordFps);
			} else {
				sysLogPrintf(LOG_ERROR, "record: the encoder is no longer running, giving up");
				encoderGone = true;
				recordStop();
			}
		}

		return;
	}

	skipRun = 0;
	slot = vidFrames[vidTail];

	SDL_UnlockMutex(vidMutex);

	if (encoderGone || !slot) {
		return;
	}

	mark = recordNow();

	if (!videoCaptureRead(slot)) {
		// The ring has not come round yet, which is only true for the first
		// frame or two of a recording. Nothing has been lost - the picture is
		// still inside the GPU - so the time goes back, and the frame that does
		// arrive is written as many times as the wait was worth.
		vidNextFrameTime = prevFrameTime;
		return;
	}

	statCaptureTime += recordNow() - mark;
	statCaptureCount++;

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
	return recordQuality;
}

void recordSetQuality(s32 q)
{
	if (q >= RECORD_QUALITY_MIN && q <= RECORD_QUALITY_MAX) {
		recordQuality = q;
	}
}

/**
 * The codec list as the menu sees it: Auto, then this platform's encoders in
 * the order they would be probed, then the software one at the end.
 *
 * Auto is not a codec but a decision not yet made, and it does not survive being
 * made: recordResolveCodec() writes what it found back into codecName, so a
 * moment after the first recording starts the menu is showing that instead. That
 * is the honest thing for it to show, and picking Auto again is how the search
 * is asked to run a second time - on another machine, or after a driver arrives.
 */
static const struct recordCodec *recordCodecByIndex(s32 index)
{
	if (index >= 1 && index <= RECORD_CODEC_COUNT) {
		return &recordCodecs[index - 1];
	}

	if (index == RECORD_CODEC_COUNT + 1) {
		return &recordSoftwareCodec;
	}

	return NULL;
}

s32 recordGetCodecCount(void)
{
	return RECORD_CODEC_COUNT + 2;
}

/**
 * Which of them this machine can actually use.
 *
 * Most of the list cannot work anywhere: an AMD card has no NVENC and an Nvidia
 * one has no VAAPI, so a menu that offers all of them equally is offering a
 * player several ways to pick something that will quietly be replaced by
 * something else at the next recording. The probe that recordResolveCodec()
 * would run eventually is run when the dropdown is opened instead, and what it
 * finds is written next to each name.
 *
 * On a thread, because it is the best part of a second of starting ffmpeg over
 * and over - a menu that stops dead for that long looks like a game that has
 * crashed, and a driver nobody here has seen could take far longer. The list
 * fills in as the answers arrive.
 *
 * Each entry is a byte written once by that thread and read by the menu; a torn
 * read is not possible and a stale one costs a frame of saying "checking".
 */
#define RECORD_AVAIL_UNKNOWN 0
#define RECORD_AVAIL_WORKS   1
#define RECORD_AVAIL_NO      2

static u8 codecAvail[RECORD_CODEC_COUNT + 2];
static bool availStarted;
static char codecLabelText[RECORD_CODEC_COUNT + 2][48];

static int recordAvailThread(void *arg)
{
	struct recordEncoder scratch;

	(void)arg;

	for (s32 i = 1; i < recordGetCodecCount(); i++) {
		const struct recordCodec *c = recordCodecByIndex(i);

		codecAvail[i] = recordTryCodec(c, &scratch) ? RECORD_AVAIL_WORKS : RECORD_AVAIL_NO;
	}

	return 0;
}

void recordProbeCodecs(void)
{
	SDL_Thread *thread;

	// Once a session is enough, and never while recording: the probe starts
	// ffmpeg several times over, and the encoder that is running wants the CPU
	// more than the menu does. The row is disabled during a recording anyway.
	if (availStarted || active) {
		return;
	}

	availStarted = true;

	for (s32 i = 0; i < recordGetCodecCount(); i++) {
		codecAvail[i] = RECORD_AVAIL_UNKNOWN;
	}

	thread = SDL_CreateThread(recordAvailThread, "pd-rec-probe", NULL);

	if (thread) {
		// Nothing waits for it. It touches only the array above and is finished
		// long before anything could care, so there is nothing to join.
		SDL_DetachThread(thread);
	} else {
		// No thread, so no answers: the names go back to standing alone rather
		// than all saying "checking" for the rest of the session.
		availStarted = false;
	}
}

const char *recordGetCodecLabel(s32 index)
{
	const struct recordCodec *c = recordCodecByIndex(index);
	const char *suffix;

	// Auto is the one entry that is always right, being a decision to let the
	// same probe run and take whatever it finds.
	if (!c) {
		return "Auto";
	}

	if (!availStarted) {
		return c->label;
	}

	switch (codecAvail[index]) {
	case RECORD_AVAIL_WORKS:
		return c->label;
	case RECORD_AVAIL_NO:
		suffix = " (unavailable)";
		break;
	default:
		suffix = " (checking)";
		break;
	}

	snprintf(codecLabelText[index], sizeof(codecLabelText[index]), "%s%s", c->label, suffix);

	return codecLabelText[index];
}

s32 recordGetCodecIndex(void)
{
	for (s32 i = 1; i < recordGetCodecCount(); i++) {
		if (!strcmp(codecName, recordCodecByIndex(i)->name)) {
			return i;
		}
	}

	return 0;
}

/**
 * Nothing is probed here - the menu is not the place to spend half a second
 * finding out - so this only says what to try, and the next recording finds out
 * whether it works. One that does not falls back to the search, which then
 * writes what it found into codecName and so into this menu: a pick that cannot
 * work corrects itself in front of the player rather than silently recording
 * with something else.
 */
void recordSetCodecIndex(s32 index)
{
	const struct recordCodec *c = recordCodecByIndex(index);

	snprintf(codecName, sizeof(codecName), "%s", c ? c->name : RECORD_DEFAULT_CODEC);

	// Whatever was worked out before was for the old answer.
	codecResolved = false;
	chosen.codec = NULL;
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
	vidCanDrain = SDL_CreateCond();
	sndMutex = SDL_CreateMutex();
	sndCanDrain = SDL_CreateCond();

	videoAddPreSwapCallback(recordPreSwap);
}

PD_CONSTRUCTOR static void recordConfigInit(void)
{
	configRegisterString("Mod.RecordKey", keyName, sizeof(keyName));
	configRegisterString("Mod.RecordEncoder", encoderPath, sizeof(encoderPath));
	configRegisterString("Mod.RecordCodec", codecName, sizeof(codecName));
	configRegisterInt("Mod.RecordFps", &recordFps, RECORD_FPS_MIN, RECORD_FPS_MAX);
	configRegisterInt("Mod.RecordQuality", &recordQuality, RECORD_QUALITY_MIN, RECORD_QUALITY_MAX);
	configRegisterInt("Mod.RecordIndicator", &recordIndicator, 0, 1);
}
