# Audio

## Digest

- **Surround, and the two bits the port's mixer dropped** — Sound Mode
  Surround was in the game all along and did nothing on PC: the N64 encodes a
  sound behind the listener as Dolby Surround with two phase bits the RSP
  microcode reads, and `port/src/mixer.c` threw them away. Now honoured, plus
  real 5.1 on a six-channel device (`Audio.SurroundOutput`), and Headphone
  Surround (that 5.1 mix heard through a modelled head) for stereo headsets.
  See "Surround" and "Headphone Surround".

## Surround

**What the N64 did.** `psCalculatePan3()` (propsnd.c), in Surround mode only,
returns a pan of 128-255 for a sound behind the listener. `sndAdjust()` and
`snd00010718()` turn that bit into fxmix + 128, `n_env.c` puts fxmix's bit 7
into the low bit of `em_dryamt` (and toggles the dry or wet low bit when it
changes later, `AL_FILTER_SET_FXAMT`); MP3 speech puts its own into the wet
amount (`mp3.c`, `var8009c3a6`). Nothing else ever sets those bits, so every
other Sound Mode is untouched by them.

The microcode is in the decomp as source: `pd-decomp/src/rsp/asp.s`,
`cmd_ENVMIXER`. It makes two masks, `0 - (dry & 1)` and `0 - (wet & 1)`, and
**xors** the input sample with the dry mask for everything the voice writes on
the left (dry and reverb send) and with the wet mask for everything on the
right. One bit set = antiphase = Dolby Surround's matrix; a Pro Logic receiver
steers it to the rear. Both set cancel out. The xor matters only in the last
bit (`~x` is `-x - 1`), and the port reproduces it.

`aSetVolume` lands at the microcode's 0x40-0x50: `A_LEFT|A_VOL` stores
cvolL/dry/wet at 0x50/0x4c/0x4e, `A_RIGHT|A_VOL` rtgt/rate at 0x46-0x4a,
`A_RATE` ltgt/rate at 0x40-0x44, and `A_INIT` takes cvolR from the command
word. The port's `rspa` fields are those.

**Why it did nothing on the port.** `aEnvMixerImpl()` multiplied by the dry and
wet amounts, low bits and all, and never looked at the bits as flags. Surround
came out as plain stereo with the rear sounds panned like the front ones.

**What the port does now** (`port/src/mixer.c`, `port/src/audio.c`):

- Two channels (Stereo device, or `Audio.SurroundOutput=2`): the bits are
  honoured exactly as the RSP does, so Surround is the N64's Dolby Surround
  again. Measured: a rear tone comes out with L/R correlation -0.995, a front
  one +0.994.
- Six channels (`Audio.SurroundOutput=1`, or Auto on a device SDL reports as six
  or more): the bit routes the voice instead. Rear voices' dry share goes to
  RL/RR at the game's own pan; front voices go to FL/C/FR, the centre being
  what the equal-power pair shared (`min(gl, gr)`), renormalised to the pair's
  power. A voice that changes side fades over 40 ms, its progress kept in the
  spare bytes of its `ENVMIX_STATE` (80 bytes, the port used 24); `n_env.c`
  zeroes the state when a voice starts a sound so a fade never carries over to
  the next sound. The reverb send stays in phase; MAIN is left holding only
  what the reverb returns, which goes 0.8 front / 0.6 rear (equal power). LFE
  is a 120 Hz Butterworth low pass of all five at `Audio.SurroundLFE`% of 0.5.

**The game only ever sees stereo.** Its output buffers stay two channels (the
front pair). A chunk's other four channels, plus a Stereo-equivalent mix for
the recorder, are filed by `aSaveBufferImpl()` under the address the chunk's
stereo was saved to, and `audioEndFrame()` takes them back by address when it
queues that buffer. By address because `amgrFrame()` hands the device the
buffer it mixed **the frame before**, from a ring of three; a FIFO would have
been off by a frame.

`osAiGetLength()`/`audioGetBytesBuffered()` still count stereo bytes (the
audio manager's 1100-frame throttle and netplay's `audioGetFramesQueued()`
assume four bytes a frame), so the six-channel queue is converted.
`--audio-dump` writes what the device got: two or six channels. The recorder
always gets stereo.

Auto relies on `SDL_GetDefaultAudioInfo()` (SDL 2.24+): PulseAudio and WASAPI
report the default device's channels; the disk and dummy drivers do not (the
menu says "Speakers: not reported"), and Auto is then Dolby Surround. On
PulseAudio SDL finds the default device **by its description**, so two sinks
with the same description (every bare `module-null-sink` is "Null Output")
all answer as the first one: give test sinks
`sink_properties=device.description=...`. Measured that way: a stereo
headset DAC gets Dolby Surround, a headset whose driver presents 7.1 (virtual
surround) gets 5.1 - Pulse then spreads the rear pair into its sides too.
Nothing here picks a device: it is always SDL's default output, before and
after the reopen, and always PCM (never a Dolby Digital/DTS bitstream), so an
analogue or USB DAC is as good as any.

**Testing it headless.** `SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=...` writes the
device stream, six channels once the device is reopened (a reopen truncates the
file: copy it from gdb with `shell cp` before switching). Drive it from gdb on
`amgrFrame` with `ignore`: `call optionsSetMusicVolume(0)`, `call
sndSetSoundMode(3)`, then `call snd00010718($h, 0, 0x7fff, 0xC0, 163, (float)1,
0, -1, 1)` for SFX_ALARM_DEFAULT behind (0x40 front, 0x00 front left, 0xFF rear
right) and `call audioStop(*$h)`. For Auto against a real backend, start a
private PulseAudio with a six-channel null sink (no D-Bus, or it refuses to
start beside the session's):

```sh
DBUS_SESSION_BUS_ADDRESS=unix:path=$S/nobus XDG_RUNTIME_DIR=$S/rt HOME=$S \
  pulseaudio -n --daemonize=yes --exit-idle-time=-1 \
  --load="module-native-protocol-unix socket=$S/rt/pulse.sock auth-anonymous=1" \
  --load="module-null-sink sink_name=surr51 channels=6 channel_map=front-left,front-right,front-center,lfe,rear-left,rear-right sink_properties=device.description=HDMI_5.1"
export PULSE_SERVER=unix:$S/rt/pulse.sock
parecord --device=surr51.monitor --channels=6 --format=s16le --rate=22020 --raw out.raw &
```

and run the game with `SDL_AUDIODRIVER=pulseaudio`. `pulseaudio -k` with the
same environment stops it.

## Headphone Surround

`Audio.SurroundOutput=3`: the mixer runs the 5.1 routing above and renders
the five speakers (ITU angles: front pair at 30 degrees, rear at 110) and the
subwoofer to two ears (`hpRender()` in mixer.c), on a two-channel device.
Nothing can tell headphones from speakers on a stereo device, so Auto never
picks it.

The model is Brown and Duda's structural one (1998), from its equations: a
head shadow per speaker per ear (one pole, one zero, bilinear prewarped at
c/a; treble gain alpha 2 facing the ear, 0.1 at 150 degrees) and the sphere's
delay (0.27 ms across the head for the front pair, 0.59 ms for the rear,
measured). No measured HRTF is shipped, so there is no data licence to
carry. A sphere cannot tell front from back, and the pinna cues that do sit
above what 22 kHz sampling holds, so the rear pair gets a -5 dB shelf at
2.5 kHz and +2 dB at 1 kHz (behind measures 6 dB duller than ahead). Six
fixed reflection taps a side (3-21 ms, low passed at 4 kHz) take the sound out
of the head; their level is the Headphone Room slider (`Audio.HeadphoneRoom`,
percent of a gain of 1.4, default 50 = 0.7): measured on an impulse from the
front left, none at 0%, -15.2 dB against the direct sound at 50%, -9.2 dB at
100%. The calibration below was made at 50%.

Calibration (pink noise, the harness in the commit message): the sphere puts
the rear speakers 20 degrees off the ear's axis, where its near-ear treble
lift made a voice behind 3 dB louder than one ahead; `HP_REAR` 0.72 trims
that, and `HP_GAIN` 0.78 puts a voice ahead, hard left and behind within
0.3 dB of Stereo. White noise reads 3 dB quieter ahead, because the model
dulls the treble of a source in front: that is the model, not the level.
The Subwoofer slider is a bass lift here. Cost: about 0.45% of a core over
Stereo with 24 voices.

Measured in the game (disk driver, the gdb tones above): ahead L/R
correlation +0.99, front left 6.8 dB louder left, rear right 6.9 dB louder
right, and the treble share of a tone behind 0.5% against 4.8% ahead.

