# Screenshots and the recorder

## The frame is presented before videoEndFrame()

`gfx_run()` ends with `swap_buffers_begin()`, which is the `SDL_GL_SwapWindow()`
call, so by the time `videoEndFrame()` runs the back buffer is already gone and
reading it back gives garbage. Anything that needs the finished image hooks
`videoAddPreSwapCallback()`, which fires inside `gfx_run()` with the frame drawn
and not yet swapped. That is where the screenshot key reads its pixels.

`gfx_current_window_dimensions` is `SDL_GL_GetDrawableSize()`, and under Xvfb
with no window manager a fullscreen-desktop window reports the screen size while
the X window stays 640x480. Reading a rect larger than the real drawable leaves
those pixels undefined, which looks like heap garbage in the corner of the
image - set `DefaultFullscreen=0` in pd.ini before trusting a headless capture.

## Feeding ffmpeg two raw pipes

The recorder has two backends and `fork()` picks which. With it, ffmpeg gets the
picture on fd 3 and the sound on fd 4 and muxes them itself. Without it there is
only `popen()` and one pipe, so the picture goes to an ffmpeg of its own while
the sound is written as a wav here, and a second ffmpeg (`-c:v copy`) puts them
together at stop - about half a second for a twelve second recording.

**Build the second one on Linux to test it**: `#define RECORD_FORCE_TWOPASS 1`
above the includes in `record.c`. It is the only way to find out whether the
Windows path works, and it did not the first time.

Two things about the two-pipe backend are not obvious and both hang the game:

**ffmpeg probes an input before it reads the next one.** With default
`probesize`/`analyzeduration` it will read five seconds of the audio pipe before
it looks at the video pipe at all. The video pipe fills at 64KB, the game blocks
in `recordPreSwap()` holding a frame it cannot hand over, which stops the tick
that produces audio, and nothing moves again. `-probesize 32 -analyzeduration 0`
on both inputs is the fix - the flags already say everything there is to know
about a raw stream, so there is nothing to probe for.

**`pipe(2)` hands out whatever is free, usually 3 to 6.** So `dup2(fd, 3)` in the
child lands on one of the other three ends. Both read ends go up out of the way
with `F_DUPFD, 10` first, then everything else closes, then they come back down.

`kill()` needs `_DEFAULT_SOURCE` defined before the first system header: the
build is `-std=c11`, and that is strict enough to hide it while leaving `fork()`,
`pipe()` and `waitpid()` visible.

SDL2 has no join with a timeout, and a thread stuck in a write to an encoder
that has stopped reading cannot be woken - `popen()` gives back a stream and no
process to kill. So the writers count themselves out, `recordStop()` polls that,
and past the deadline it detaches them and leaks their buffers rather than
freeing memory they are still reading. That ends recording for the session:
the next one would hand its encoder to a thread still writing the last one's
frames.

**The readback must not stall the frame.** `videoReadScreenPixels()` waits for
the GPU to finish and repacks every row into RGB triples, which is right for one
screenshot and ruinous sixty times a second. The recorder uses the capture calls
instead (`gfx_capture_*`, `port/fast3d/gfx_opengl.cpp`): `glReadPixels` into a
bound pixel buffer object is a request rather than a transfer, so a ring of two
PBOs issues this frame's read and collects the one issued last frame, and nothing
waits on the GPU. What comes out is a frame behind - which is latency and not
drift, because a fixed-rate stream timestamps by index and no index is skipped.
`gfx_capture_drain()` collects what is still in flight at stop, or the recording
loses its tail to the ring.

**Convert before you download.** The readback costs in proportion to the bytes
moved, and four bytes a pixel at 1080p60 is 497 MB/s off the GPU, down a pipe and
back onto the GPU for the encoder - measured, this card tops out at 77fps with
nothing else running, which an eighty simulant match does not leave room inside
of. So the frame is converted to NV12 first, by two shader passes into an R8 luma
target and a half size R8G8 chroma one, and only then read back: a byte and a
half a pixel, 187 MB/s, and the encoder wanted NV12 anyway. This is what OBS does
and where it was taken from - `libobs/obs.c` and `format_conversion.effect` - and
it is not DMA-BUF or zero-copy or anything needing a library linked in.

The flip comes free by rendering the planes upside down, so ffmpeg no longer does
it. Desktop GL 3.0 and up only; anything older keeps the four-byte readback,
which is why `GFX_CAPTURE_BGRA` is still there. **The passes must put back every
piece of GL state they touch** - `gfx_opengl_start_frame()` only counts frames,
so nothing else restores the program, the vertex array, the viewport or the
enables, and the symptom of missing one is the next frame drawn wrongly.

The colour is BT.709 limited range, and it is tagged with the `h264_metadata`
bitstream filter rather than `-colorspace`. Those are a request to *convert*, and
ffmpeg answers one by inserting a scaler that cannot touch a frame already on the
GPU: "Impossible to convert between the formats supported by the filter
Parsed_scale_vaapi_1 and the filter auto_scale_0". Leaving it untagged is also
wrong, because a player then guesses from the picture's size and gets 480p wrong.

**Which encoder a machine has is asked, not guessed.** nvenc, vaapi and qsv on
Linux, nvenc/amf/qsv/mf on Windows, videotoolbox on macOS - each put past ffmpeg
once as three frames of black, first that survives kept, answer remembered in
`Mod.RecordCodec` so the search never runs twice. The filter chains are probed
the same way and matter more than the codec: the first has the GPU convert to
NV12, the second falls back to swscale for a driver that will not take the
upload. On the Polaris card here at 1080p that is 5ms a frame against 29ms, and
against 59ms across six cores for libx264 - which is deliberately not in the
table and reachable only by naming it, for a machine with nothing on its GPU.

**Never wait for the encoder.** A frame that finds the queue full is not
captured; the clock goes back and the next frame that gets a slot is written for
both of them, which is what the repeat counts already did for a game running
below the recording's rate. The stream stays fixed rate and the sound stays put.

Waiting was the original design - stutter rather than drop a frame - and it was
wrong for a reason worth remembering: **the game thread produces the sound as
well as the pictures.** Blocking it stops the audio that ffmpeg interleaves the
video against, so the encoder ends up waiting for sound that is waiting for the
encoder, and what that looks like from the outside is the recorder dying a few
seconds into an eighty simulant match, having frozen the game first. Anything
added here that can block the render thread brings that back.

An encoder that has actually exited is still given up on - the writers find it
when a write fails - and the unplayable file it leaves behind is removed, since
nothing without a moov atom will open in anything.
