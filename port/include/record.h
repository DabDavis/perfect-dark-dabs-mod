#ifndef _IN_RECORD_H
#define _IN_RECORD_H

#include <PR/ultratypes.h>

/**
 * A key that records the game to an mp4.
 *
 * The frame is read back the same way the screenshot key reads it, from the
 * pre-swap callback, and handed to an ffmpeg started as a child process: the
 * video on one pipe as raw RGB, the sound on another as raw PCM, and ffmpeg
 * does the encoding and the muxing in another process. Nothing new is linked
 * in, and there is no encoder here to keep working.
 *
 * Sound is taken where the port already hands a finished buffer to SDL, in
 * audioEndFrame(), so what is recorded is what was played.
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

// libx264's CRF: lower is a bigger file and a better picture.
s32 recordGetQuality(void);
void recordSetQuality(s32 crf);

s32 recordGetIndicator(void);
void recordSetIndicator(s32 on);

// The sound the port is about to play, as it plays it. Bytes, s16 stereo.
void recordPushAudio(const void *buf, u32 len);

#endif
