#ifndef _IN_AUDIO_H
#define _IN_AUDIO_H

#include <PR/ultratypes.h>

s32 audioInit(void);
s32 audioGetBytesBuffered(void);
s32 audioGetSamplesBuffered(void);

// What the device is actually running at, for anything that has to describe the
// stream it is handed - the recorder does. Always s16 stereo.
s32 audioGetSampleRate(void);
void audioSetNextBuffer(const s16 *buf, u32 len);
void audioEndFrame(void);

#endif
