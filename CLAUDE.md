# Perfect Dark port — simulant/mod fork

Fork of the [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark) port.
`port` tracks upstream; work happens on `dabs-mod`.

`git checkout port` returns to stock at any time.

## What is written down here

Each section is a thing that was got wrong once. If you are about to touch the
area, read its section first — none of them are inferable from the code.

- [Build and run](#build-and-run) — the two build trees, and which warnings are not yours
- [This is a decompilation](#this-is-a-decompilation) — ROM layout, and why raising a limit is how every bug here started
- [A chr's prop and model are read before its tick, not during](#a-chrs-prop-and-model-are-read-before-its-tick-not-during) — where body swaps have to happen
- [Memory: what an MP chr costs, and which pool it comes from](#memory-what-an-mp-chr-costs-and-which-pool-it-comes-from) — the ~50KB head copy that empties the stage pool
- [One head modeldef cannot sit on two bodies](#one-head-modeldef-cannot-sit-on-two-bodies)
- [ROM-resident structures — never grow these](#rom-resident-structures--never-grow-these)
- [Save format](#save-format) — eeprom, where everything now lives, and the migration that keeps it
- [Text rendering](#text-rendering) — fonts, and menu units versus pixels
- [Stage numbers](#stage-numbers)
- [The frame is presented before videoEndFrame()](#the-frame-is-presented-before-videoendframe) — why the pre-swap callback exists
- [Feeding ffmpeg two raw pipes](#feeding-ffmpeg-two-raw-pipes) — the recorder: NV12 conversion on the GPU, encoder detection, and why it must never wait for one
- [Ghost Trials talks over two different transports](#ghost-trials-talks-over-two-different-transports) — WinHTTP and libcurl, and why not one of them
- [The game replaces itself](#the-game-replaces-itself) — the updater's internals
- [Mod directories](#mod-directories)
- [Debugging](#debugging) — the two commands that actually find things

**[DabDavisGitHub.md](DabDavisGitHub.md)** is the companion to this file: the
GitHub remote, how commits are written, how a push becomes a release, and how the
stable and dev channels reach a player. Read it before pushing or tagging —
`dabs-mod` is a public default branch and a push to it rebuilds what every
dev-channel player's Check for Updates points at.

## Build and run

```sh
cmake -G"Unix Makefiles" -Bbuild .     # only after adding/removing source files
cmake --build build -j8
./build/pd.x86_64                      # needs build/data/pd.ntsc-final.z64
./build/pd-modded.sh                   # with the All in One mod
```

Release builds use `-Og`, not `-O2` — see the comment in `CMakeLists.txt`. Warnings
about uninitialised locals in `collision.c`, `model.c`, `menu.c` and `mplayer/setup.c`
are pre-existing decomp artifacts; check `git diff` before assuming one is yours.

`build/` is gitignored, including the mod directories and `pd-modded.sh`. Keep the
mod zip: a clean rebuild takes them with it.

## This is a decompilation

Much of `src/` is decompiled N64 code. Two consequences that matter:

**Structs with offset comments** (`/*0x1be7*/`) document the original ROM layout.
Growing them breaks matching builds. Ours sets `MATCHING=0`, but treat it as a real
cost when considering upstreaming.

**Some code is unreachable by construction on N64** and becomes reachable the moment
a limit is raised. Every bug in this fork's history was of that shape: a fixed-size
table or an assumption nobody wrote down. `chrInit()` dereferenced NULL when the chr
pool was exhausted; `mpCreateBotFromProfile()` looped forever once chrs outnumbered
the 53 available heads; `langGetLangBankIndexFromStagenum()` had `default: while(true){}`.
When raising any cap, grep for fixed-size arrays indexed by the thing you are scaling.

## A chr's prop and model are read before its tick, not during

`chrTick()` (`src/game/chr.c`) reads `prop` and `model` once, then calls into the
action handler and keeps using those locals afterwards — `chrRemove(prop, true)` at
the end is on the prop it read at the top. So anything that moves a body between chrs
has to happen outside the prop tick. `modBodiesTick()` runs from `chraTickBg()`, which
is the first thing `propsTick()` does and the one point in the frame where no prop is
part way through its own tick; the dead simulant is left waiting a tick rather than
respawned from inside `chrTickDead()`.

## Memory: what an MP chr costs, and which pool it comes from

`body0f02ce8c()` (`src/game/body.c`) takes a different path in multiplayer: with no
`headmodeldef` passed in it calls `modeldefLoadToNew()` **every time**, a fresh ~50KB
copy of the head from `MEMPOOL_STAGE` that is never freed, because each simulant's
head is offset to fit its own body. Stock pays that once per bot at match start.
Anything that creates chr models *during* a match must pass the modeldefs in, or it
empties the stage pool in about a minute.

`mempAlloc()` fills the onboard bank first and falls through to the expansion bank,
which is a hardcoded 8MB (`MEMP_EXPANSION_POOL_SIZE`) no matter what `Game.MemorySize`
says — that setting grows the onboard side only. So `mempGetStageFree()`, which
answers for one bank, is not "how much room is left"; `mempGetStageFreeTotal()` is.
A pool-full warning naming one pool is the onboard bank spilling over, not the end.

`mempAlloc()` returns NULL when both are out, and most callers do not check.
`modelmgrInstantiateModel()` now does.

## One head modeldef cannot sit on two bodies

`modelAttachHead()` re-parents the head's root nodes to the headspot of
whichever body attached it last, and `modelGetNodeRwData()` walks *up* from a
head node to find a headspot and take its `rwdatas`. So a head modeldef shared
between two *different* body definitions resolves correctly for one of them and
reads a meaningless index into the other's rwdata — which showed as a write to
`0x5008b7d`, the same address every run, because what was being read as a
pointer was model data out of the ROM.

Stock never hits it: solo shares one modeldef per head number but only ever
pairs a head with the body it was built for, and multiplayer loads a fresh copy
per chr *and* calls `bodyCalculateHeadOffset()` for the body it is going on.
Ghost Trials puts any head on any body in solo, so a trial takes the
multiplayer path — in **both** places that decision is made: `body0f02ce8c()`
for everyone, and the player's own copy in `player.c`, which is what runs the
moment third person is switched on. Fixing the first alone moved the crash and
did not remove it.

ASan only reports if the game's own `SIGSEGV` handler is out of the way: run
with `--no-crash-handler`, or every report is a bare backtrace in a dialog.

## ROM-resident structures — never grow these

`preprocessMpConfigs()` (`port/src/preprocess/misc.c`) casts raw ROM bytes to
`struct mpconfig` and strides by `sizeof`. `struct mpconfig` and `struct mpstrings`
are therefore layout-locked; `_Static_assert`s in `mplayer.c` enforce it.

That is why `MAX_BOTS_CONFIG` (8, ROM-resident) is separate from `MAX_BOTS` (80,
runtime). Anything reading `config->simulants[]` or `strings.aibotnames[]` must
bound by `MAX_BOTS_CONFIG`.

`mpsetup.chrslots` is a `u16` inside that struct, so simulant participation lives in
a separate runtime array (`g_MpSimSlots`), mirrored back into `chrslots` for the low
8 slots. It was a `u64` bitmask until 80 exceeded 64 — prefer per-slot flags over
bitmasks here.

## Save format

`mpsetupfileSaveWad()`/`LoadWad()` take a version. The port's file has a version byte
and per-version block sizes (`MPSETUP_BLOCKSIZE_V1` vs `MPSETUP_BLOCKSIZE`). Files
stay in the base format unless a setup actually needs >8 simulants, so unmodified
builds can still read them; changing version re-encodes every block.

`savebufferOr()` does no bounds checking. `stagenum` is stored in 7 bits.

**Everything lives beside the executable now** — saves, `pd.ini`, screenshots and
recordings — because a copy of the game should be one folder a player can open,
back up or move. `~/.local/share/perfectdark` is where it used to be and is still
the fallback for a copy that cannot write to its own directory: installed under
`/usr`, on a read-only mount, inside an app bundle. `fsChooseOutputDir()` makes
that choice for the player's own files and `fsInit()` makes it for the saves.

**The first run of a build that does this copies the old save directory across**
(`fsMigrateSaves()`), because otherwise someone who has played before starts with
a fresh eeprom, no unlocks and the simulant count back at four — which looks
exactly like the fork being broken. It copies rather than moves, never writes
over a file already there, and only takes what the game writes: the config, the
saves, `ghosts/` and `exported/`. Screenshots and recordings are left behind
deliberately, being potentially gigabytes and nothing that stops working.

The old default was the working directory whenever a config happened to be
sitting in one, and that was never checked for writability — it is now, or a
read-only working directory silently swallows every save.

## Text rendering

`textMeasure()` adds a line of height only when it sees `\n`. A string without a
trailing newline measures as zero height and renders clipped. ROM strings have it;
anything synthesised needs it too.

`text0f15568c()` drops any glyph whose x is past `viGetWidth()`, whatever the
scissor says. The menu is 320x220 units at every window size and is drawn
centred at the view's own aspect, so on a widescreen display there is a pillar
of screen either side that no dialog reaches — and a bigger coordinate does not
get you there, because the text stops at the edge of the view. What does is
`G_ASPECT_CENTER_EXT` being `LEFT | RIGHT`: dropping one half holds what
follows against that edge, at the size and shape it already had. Ghost Trials'
nameplate and rules windows do that. A scissor set before the alignment stays
behind in the middle of the screen, because fast3d turns a scissor into pixels
when the command is sent.

## Stage numbers

Adding stages at runtime is constrained from several directions at once:

- `stagenum < STAGE_TITLE` (0x5a) is the engine's "is this a real level" test, in
  16 places. At or above it, `setupLoadFiles()` silently does nothing.
- `STAGE_TITLE`, `STAGE_BOOTPAKMENU`, `STAGE_CREDITS`, `STAGE_4MBMENU` are used
  outside `g_Stages`, so scanning the table alone will not show them as taken.
- `langGetLangBankIndexFromStagenum()` must know the stage (now falls back safely).
- `g_StageAllocations8Mb` has no entry for runtime stages, so they get a default
  allocation. Too small means `MEMPOOL_STAGE` exhaustion surfacing far from the cause.

Only 27 ids are free below `STAGE_TITLE`.

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

## Ghost Trials talks over two different transports

`port/src/ghostnet.c` has one seam, `ghostnetSend()`, and two implementations
behind it. Windows uses **WinHTTP**, which is part of the OS: nothing to ship and
certificates are the system's business. Everywhere else uses **libcurl**, there
being no system HTTP API to use instead — macOS has it in the SDK, Linux wants
`libcurl4-openssl-dev`.

Do not "simplify" this back to one backend. libcurl on Windows means shipping a
dozen DLLs *and* answering for a CA bundle OpenSSL looks for at a compile-time
path no player's machine has — which fails on Windows only, while Linux and macOS
work perfectly. The CI packaging step checks the transport survived on each
platform, because the build degrades to "network support is not built into this
copy" rather than failing, and that silently shipped for a while.

The WinHTTP half can be built and run from Linux: extract it with the mingw
compiler into a harness and run it under wine against a local `pdghostd`.

**The worker thread touches nothing the menu owns.** A job is decided on the main
thread and carried out on the worker, which reads only the `g_Job*` snapshot.
Uploading used to rebuild the ghost catalogue from the worker while the page that
started it was drawing rows out of that array. `fsFullPath()` is `_Thread_local`
for the same reason — it is a single scratch buffer every file call expands into.

## The game replaces itself

Check for Updates is `port/src/update.c`. It reads one file — `update.txt` on
the release, written by `dabs-mod.yml` — and downloads one bare executable,
because the releases are a zip and two tarballs, one of them xz with no
decompressor linked in.

**The channel is baked in at build time.** The release job runs
`git checkout -B dabs-mod` for tag builds and branch builds alike, so the
binary has nothing in it that says which it was; CMake takes `UPDATE_CHANNEL`
and puts it in `versioninfo.h`. A tag build follows the stable release, a
branch build follows the rolling `dabs-mod-dev` prerelease, and nothing moves a
player between the two.

**`git rev-parse --short` picks its own length** from how many objects the
repository has, so the manifest and the binary can disagree about the same
commit — a CI runner's fresh clone says seven characters where a working copy
says nine. Compare by the shorter of the two.

**The swap is two renames, never a write.** The file being replaced is the
program doing the replacing, which on Windows cannot be opened for writing at
all. Everything that can fail — the download, the size, the hash — happens to a
file under another name first, and the handover is the last thing `cleanup()`
does, after the window and the audio device are closed. On POSIX `execv` keeps
the pid.

`Mod.UpdateServer` points the whole thing at a server of your own, which is the
only way to exercise it without cutting a release. It is left in `pd.ini` once
set — a test that leaves it there is a game that cannot see real updates.

## Mod directories

Only the **first** mod dir joins the general file search. Later ones are reached
solely through file slots pinned to them (`romfile.moddir`). Mod suites ship full
asset sets under stock names — the All in One suite shares 245 filenames across five
directories — so overlaying them all replaces stock textures everywhere.

Files are cached per file id, so two mods shipping the same filename need separate
slots. `--modstages` enables runtime stage registration; it is incomplete.

## Debugging

Guessing from source failed repeatedly here; the stack was right every time.

```sh
# crash: symbolise the offsets the game prints
addr2line -f -C -e build/pd.x86_64 0x11abf5

# hang: main thread only, Mesa worker threads are noise
gdb -p $(pgrep -x pd.x86_64) -batch -ex "thread 1" -ex "bt 14"
```

stdout is block-buffered when redirected, so log lines sit unwritten. Flush a live
process before reading or killing it — `SIGKILL` discards the buffer:

```sh
gdb -p $(pgrep -x pd.x86_64) -batch -ex 'call (int)fflush(0)'
```

A hung process ignores `SIGTERM`, because the shutdown handler cannot run.

For "state disappears" bugs, instrument every mutating path rather than reading code.
The simulant-clearing hunt was solved by logging that showed a count of zero at every
suspected site — nothing was being cleared; nothing had ever been created.
