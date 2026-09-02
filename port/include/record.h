#ifndef _IN_RECORD_H
#define _IN_RECORD_H

#include <PR/ultratypes.h>

/**
 * A key that records the game to an mp4.
 *
 * The frame is read back from the pre-swap callback through the capture calls in
 * video.h - a ring of pixel buffer objects, so the readback does not stall the
 * frame the way the screenshot key's does - and handed to an ffmpeg started as a
 * child process: the video on one pipe as raw four-byte pixels, the sound on
 * another as raw PCM, and ffmpeg does the encoding and the muxing in another
 * process. It encodes on the GPU wherever the machine has something that can;
 * nothing new is linked in, and there is no encoder here to keep working.
 *
 * Sound is taken where the port already hands a finished buffer to SDL, in
 * audioEndFrame(), so what is recorded is what was played.
 *
 * Where there is no fork() there is no second pipe, so the picture goes down the
 * one popen() gives and the sound is written here as a wav, and a second ffmpeg
 * puts them in one file when recording stops.
 */

void recordInit(void);

// Once per frame, after inputUpdate(). Reads the key.
void recordTick(void);

void recordToggle(void);
void recordStop(void);

s32 recordIsActive(void);

// Whether the red dot belongs on screen. False while not recording.
s32 recordShowsIndicator(void);

// Seconds since recording started.
f32 recordGetElapsed(void);

// VK_ value of the key, or 0 if unbound.
s32 recordGetKey(void);
void recordSetKey(s32 vk);

// The recording's frame rate, not the game's. Ignored while one is running,
// because the rate is in the arguments ffmpeg was started with.
s32 recordGetFps(void);
void recordSetFps(s32 fps);

// A quantiser on libx264's CRF scale: lower is a bigger file and a better
// picture. Every encoder is asked for its own equivalent of this one number.
s32 recordGetQuality(void);
void recordSetQuality(s32 crf);

/**
 * Which encoder to use, for the menu. Index 0 is Auto - try each of this
 * machine's in turn and keep the first that works - and the rest are the
 * encoders this platform knows, ending with the software one.
 *
 * The list is platform-dependent and nothing outside record.c should assume a
 * length or an order. Auto does not stick: the search writes what it found back,
 * so the index moves off 0 by itself once a recording has been made.
 */
s32 recordGetCodecCount(void);
const char *recordGetCodecLabel(s32 index);
s32 recordGetCodecIndex(void);
void recordSetCodecIndex(s32 index);

/**
 * Start finding out which of them this machine can actually use, for the menu
 * to say so before the player picks one rather than after.
 *
 * Returns at once and does the work on a thread - it is the best part of a
 * second of starting ffmpeg over and over. Call it when the dropdown is opened;
 * it does nothing on every call after the first. The answers appear in
 * recordGetCodecLabel(), which says "checking" until they do.
 */
void recordProbeCodecs(void);

s32 recordGetIndicator(void);
void recordSetIndicator(s32 on);

// The sound the port is about to play, as it plays it. Bytes, s16 stereo.
void recordPushAudio(const void *buf, u32 len);

#endif
