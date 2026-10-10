#include <PR/ultratypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "mixer.h"
#include "record.h"
#include "system.h"

static SDL_AudioDeviceID dev;
static const s16 *nextBuf;
static u32 nextSize = 0;

static s32 bufferSize = 512;
static s32 queueLimit = 8192;
static s32 sampleRate = 22020;
static u64 queuedTotal = 0; // stereo bytes ever queued: where the next mixed buffer lands in the output
static FILE *dumpFile = NULL; // --audio-dump FILE: every buffer queued, raw, as the device got it (s16, 2 or 6 channels), for tests

/**
 * Surround. The game's own Sound Mode picks it (sndSetSoundMode() calls
 * audioSetSurround()), and Audio.SurroundOutput says what it comes out as:
 *
 * - Dolby Surround: two channels, a sound behind the listener in antiphase,
 *   exactly what the N64 sent a Pro Logic receiver (port/src/mixer.c).
 * - 5.1: six channels, the same sounds on the rear speakers themselves, a
 *   centre and a subwoofer.
 * - Auto: 5.1 when the default device has six channels or more, else Dolby
 *   Surround. A device whose layout SDL cannot report counts as stereo.
 *
 * Every other Sound Mode is two channels. The game only ever sees stereo: its
 * buffers hold the front pair, the rest is added here as each one is queued.
 */
static s32 channels = 2; // what the device is open with
static s32 surroundOutput = AUDIO_SURROUND_AUTO; // Audio.SurroundOutput
static s32 surroundLfe = 50; // Audio.SurroundLFE, percent of a gain of 0.5
static s32 surroundWanted = 0; // Sound Mode is Surround
static s32 deviceChannels = 0; // the default device's own, 0 when SDL cannot say

static s16 *wideBuf = NULL; // a stereo buffer made 5.1
static s16 *recBuf = NULL; // and the stereo the recorder gets for it
static u32 wideFrames = 0;

static s32 audioQueryDeviceChannels(void)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
	SDL_AudioSpec spec;

	if (SDL_GetDefaultAudioInfo(NULL, &spec, 0) == 0) {
		return spec.channels;
	}
#endif

	return 0;
}

static s32 audioOpenDevice(s32 nchannels)
{
	SDL_AudioSpec want, have;

	if (dev) {
		SDL_CloseAudioDevice(dev);
		dev = 0;
	}

	SDL_zero(want);
	want.freq = 22020; // TODO: this might cause trouble for some platforms
	want.format = AUDIO_S16SYS;
	want.channels = nchannels;
	want.samples = bufferSize;
	want.callback = NULL;

	// No allowed changes: SDL converts to whatever the device really is,
	// including folding 5.1 down for a stereo one
	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (dev == 0) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudio error (%d channels): %s", nchannels, SDL_GetError());
		return -1;
	}

	channels = nchannels;
	sampleRate = have.freq;

	SDL_PauseAudioDevice(dev, 0);

	return 0;
}

static void audioApplySurround(void)
{
	s32 want = 2;

	if (surroundWanted) {
		switch (surroundOutput) {
		case AUDIO_SURROUND_51:
			want = 6;
			break;
		case AUDIO_SURROUND_MATRIX:
			want = 2;
			break;
		default:
			want = deviceChannels >= 6 ? 6 : 2;
			break;
		}
	}

	if (dev && want != channels) {
		if (audioOpenDevice(want) != 0 && want != 2) {
			audioOpenDevice(2);
		}

		sysLogPrintf(LOG_NOTE, "audio: %s (%d channels out, default device has %d)",
			channels == 6 ? "surround as 5.1" : surroundWanted ? "surround as Dolby Surround" : "stereo",
			channels, deviceChannels);
	}

	mixerSetSurround51(channels == 6, surroundLfe * (0.5f / 100.f));
}

s32 audioInit(void)
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}

	nextBuf = NULL;
	deviceChannels = audioQueryDeviceChannels();

	if (audioOpenDevice(2) != 0) {
		return -1;
	}

	if (sysArgGetString("--audio-dump")) {
		dumpFile = fopen(sysArgGetString("--audio-dump"), "wb");
	}

	// the save may have chosen Surround before there was a device
	audioApplySurround();

	return 0;
}

void audioSetSurround(s32 on)
{
	surroundWanted = on;
	audioApplySurround();
}

s32 audioGetSurroundOutput(void)
{
	return surroundOutput;
}

void audioSetSurroundOutput(s32 output)
{
	surroundOutput = output;
	deviceChannels = audioQueryDeviceChannels(); // the player may have changed it since the boot
	audioApplySurround();
}

s32 audioGetSurroundLfe(void)
{
	return surroundLfe;
}

void audioSetSurroundLfe(s32 percent)
{
	surroundLfe = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	audioApplySurround();
}

s32 audioGetChannels(void)
{
	return channels;
}

s32 audioGetSurroundWanted(void)
{
	return surroundWanted;
}

s32 audioGetDeviceChannels(void)
{
	return deviceChannels;
}

s32 audioGetSampleRate(void)
{
	return sampleRate;
}

// In stereo bytes whatever the device is open with: osAiGetLength() and the
// game's audio manager count four bytes a frame
s32 audioGetBytesBuffered(void)
{
	return SDL_GetQueuedAudioSize(dev) / (2 * channels) * 4;
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

// nextBuf as six channels in wideBuf, and what the recorder should have in recBuf
static void audioWiden(void)
{
	const u32 frames = nextSize / 4;

	if (frames > wideFrames) {
		wideBuf = realloc(wideBuf, frames * 6 * sizeof(s16));
		recBuf = realloc(recBuf, frames * 2 * sizeof(s16));
		wideFrames = frames;
	}

	for (u32 first = 0; first < frames; first += MIXER_CHUNK_FRAMES) {
		// NULL for a buffer mixed before 5.1 was on: it is all front
		const s16 *side = mixerSurroundTake(nextBuf + first * 2);
		const u32 count = frames - first < MIXER_CHUNK_FRAMES ? frames - first : MIXER_CHUNK_FRAMES;

		for (u32 i = 0; i < count; ++i) {
			const u32 f = first + i;
			s16 *out = wideBuf + f * 6;

			out[0] = nextBuf[f * 2];
			out[1] = nextBuf[f * 2 + 1];

			if (side) {
				const s16 *s = side + i * MIXER_SURR_COUNT;
				out[2] = s[MIXER_SURR_C];
				out[3] = s[MIXER_SURR_LFE];
				out[4] = s[MIXER_SURR_RL];
				out[5] = s[MIXER_SURR_RR];
				recBuf[f * 2] = s[MIXER_SURR_STEREO_L];
				recBuf[f * 2 + 1] = s[MIXER_SURR_STEREO_R];
			} else {
				out[2] = out[3] = out[4] = out[5] = 0;
				recBuf[f * 2] = out[0];
				recBuf[f * 2 + 1] = out[1];
			}
		}
	}
}

void audioEndFrame(void)
{
	if (nextBuf && nextSize) {
		if (audioGetSamplesBuffered() < queueLimit) {
			const void *out = nextBuf;
			u32 outSize = nextSize;
			const void *rec = nextBuf;

			if (channels == 6) {
				audioWiden();
				out = wideBuf;
				outSize = nextSize * 3;
				rec = recBuf;
			}

			SDL_QueueAudio(dev, out, outSize);
			queuedTotal += nextSize;

			if (dumpFile) {
				fwrite(out, 1, outSize, dumpFile);
				fflush(dumpFile);
			}
			// Inside the check on purpose: a recording should hold what was
			// played, and a buffer dropped for a full queue was not. It is
			// stereo whatever the speakers are.
			recordPushAudio(rec, nextSize);
		} else if (channels == 6) {
			// the chunks' other channels go with it
			for (u32 first = 0; first < nextSize / 4; first += MIXER_CHUNK_FRAMES) {
				mixerSurroundTake(nextBuf + first * 2);
			}
		}
		nextBuf = NULL;
		nextSize = 0;
	}
}

PD_CONSTRUCTOR static void audioConfigInit(void)
{
	configRegisterInt("Audio.BufferSize", &bufferSize, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.SurroundOutput", &surroundOutput, AUDIO_SURROUND_AUTO, AUDIO_SURROUND_MATRIX);
	configRegisterInt("Audio.SurroundLFE", &surroundLfe, 0, 100);
}
