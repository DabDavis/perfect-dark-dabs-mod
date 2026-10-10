#ifndef _IN_AUDIO_H
#define _IN_AUDIO_H

#include <PR/ultratypes.h>

s32 audioInit(void);
s32 audioGetBytesBuffered(void);
s32 audioGetSamplesBuffered(void);
u64 audioGetFramesQueued(void);

// What the device is actually running at, for anything that has to describe the
// stream it is handed - the recorder does. The recorder's is always s16 stereo,
// even when the speakers get 5.1.
s32 audioGetSampleRate(void);
void audioSetNextBuffer(const s16 *buf, u32 len);
void audioEndFrame(void);

// Audio.SurroundOutput: what Sound Mode Surround comes out as
#define AUDIO_SURROUND_AUTO   0 // 5.1 on a device with six channels, else Dolby Surround
#define AUDIO_SURROUND_51     1 // six channels, SDL folds them down for a smaller device
#define AUDIO_SURROUND_MATRIX 2 // Dolby Surround on two channels, as the N64 did
#define AUDIO_SURROUND_HEADPHONES 3 // the 5.1 mix heard through a head, on two channels

void audioSetSurround(s32 on); // sndSetSoundMode()
s32 audioGetSurroundOutput(void);
void audioSetSurroundOutput(s32 output);
s32 audioGetSurroundLfe(void);
void audioSetSurroundLfe(s32 percent);
s32 audioGetHeadphoneRoom(void);
void audioSetHeadphoneRoom(s32 percent); // Headphone Surround's room reflections, 0 none, 50 as made
s32 audioGetChannels(void);
s32 audioGetSurroundWanted(void);
s32 audioGetDeviceChannels(void);

#endif
