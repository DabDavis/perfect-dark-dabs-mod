#include <PR/ultratypes.h>
#include <stdio.h>
#include <SDL.h>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "record.h"
#include "system.h"

static SDL_AudioDeviceID dev;
static const s16 *nextBuf;
static u32 nextSize = 0;

static s32 bufferSize = 512;
static s32 queueLimit = 8192;
static s32 sampleRate = 22020;
static u64 queuedTotal = 0; // bytes ever queued: where the next mixed buffer lands in the output
static FILE *dumpFile = NULL; // --audio-dump FILE: every buffer queued, raw (s16 stereo), for tests

s32 audioInit(void)
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 22020; // TODO: this might cause trouble for some platforms
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = bufferSize;
	want.callback = NULL;

	nextBuf = NULL;

	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (dev == 0) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudio error: %s", SDL_GetError());
		return -1;
	}

	SDL_PauseAudioDevice(dev, 0);

	sampleRate = have.freq;

	if (sysArgGetString("--audio-dump")) {
		dumpFile = fopen(sysArgGetString("--audio-dump"), "wb");
	}

	return 0;
}

s32 audioGetSampleRate(void)
{
	return sampleRate;
}

s32 audioGetBytesBuffered(void)
{
	return SDL_GetQueuedAudioSize(dev);
}

s32 audioGetSamplesBuffered(void)
{
	return audioGetBytesBuffered() / 4;
}

// Stereo frames queued since boot: the output position the next mix lands at
// (netplay's event log puts its sounds against the mixed audio by it)
u64 audioGetFramesQueued(void)
{
	return queuedTotal / 4;
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
	nextBuf = buf;
	nextSize = len;
}

void audioEndFrame(void)
{
	if (nextBuf && nextSize) {
		if (audioGetSamplesBuffered() < queueLimit) {
			SDL_QueueAudio(dev, nextBuf, nextSize);
			queuedTotal += nextSize;

			if (dumpFile) {
				fwrite(nextBuf, 1, nextSize, dumpFile);
				fflush(dumpFile);
			}
			// Inside the check on purpose: a recording should hold what was
			// played, and a buffer dropped for a full queue was not.
			recordPushAudio(nextBuf, nextSize);
		}
		nextBuf = NULL;
		nextSize = 0;
	}
}

PD_CONSTRUCTOR static void audioConfigInit(void)
{
	configRegisterInt("Audio.BufferSize", &bufferSize, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 1 * 1024 * 1024);
}
