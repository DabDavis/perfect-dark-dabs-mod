#ifndef _IN_MODAUDIO_H
#define _IN_MODAUDIO_H

#include <PR/ultratypes.h>

// A mod's sound and music banks, swapped at a stage boundary (feat/pdmods-audio)
void modAudioEnter(const char *moddir);
void modAudioLeave(void);
s32 modAudioQuiesce(void);

#endif
