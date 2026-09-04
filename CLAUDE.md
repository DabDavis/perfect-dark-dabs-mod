# Perfect Dark port — simulant/mod fork

Fork of the [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark) port.
`port` tracks upstream; work happens on `dabs-mod`.

`git checkout port` returns to stock at any time.

## What is written down here

Each section is a thing that was got wrong once. If you are about to touch the
area, read its section first — none of them are inferable from the code.

- [Build and run](#build-and-run) — the two build trees, which warnings are not yours, and cross-building for Windows
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
- [Where a texture pack goes](#where-a-texture-pack-goes) — four directories, one of which is read by nothing
- [Replacement textures are decoded off the render thread](#replacement-textures-are-decoded-off-the-render-thread) — why the stutter was never the decoder
- [Pack image formats, and which way up they go](#pack-image-formats-and-which-way-up-they-go) — PNG is ours, JPEG is stb_image; and the two row orders
- [Texture pack keys](#texture-pack-keys) — F7/F8/F9/F10
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

### Windows, from Linux

The Windows build is worth doing before pushing anything that touches files,
paths, subprocesses or the network: those are where the two platforms differ and
where nothing in the Linux build will tell you.

```sh
cmake -Bbuild-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake .
cmake --build build-win -j8
```

The toolchain file expects a prefix at `~/.local/mingw64`, built once:

- **SDL2** — unpack `SDL2-devel-<ver>-mingw.tar.gz` from libsdl.org and copy its
  `x86_64-w64-mingw32/*` into the prefix.
- **zlib** — `make -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- BINARY_PATH=...
  INCLUDE_PATH=... LIBRARY_PATH=...`, then again with `install`. Static `libz.a`,
  so there is no `zlib1.dll` to ship.
- `apt install g++-mingw-w64-x86-64` — the C compiler alone is not enough,
  fast3d is C++.

Watch the configure output for `using WinHTTP - ghost server support enabled`;
without it the updater and the Upscayl download are compiled out.

Running it under wine: copy `SDL2.dll` from the prefix and
`/usr/lib/gcc/x86_64-w64-mingw32/*-win32/libgcc_s_seh-1.dll` next to the exe, put
the ROM in `build-win/data/`, and

```sh
DISPLAY=:99 WINEDEBUG=-all wine pd.x86_64.exe --savedir 'C:\pdsave'
```

with that directory made under `~/.wine/drive_c/` first. The
`glDebugMessage*KHR` errors in the log are wine's GL lacking `KHR_debug`.

`pd.ini` is a sectioned INI, not a flat one: `Mod.LoadTextures=1` on its own
line is silently ignored, and has to be `[Mod]` then `LoadTextures=1`. The keys
the code registers are `Section.Key`.

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
exactly like the fork being broken. It copies rather than moves and never writes
over a file already there. It takes every file at the top of the old directory
rather than a named list — that directory is one the game itself chose and has
been saving into, and a save missed by an allowlist is exactly the failure this
exists to prevent — plus `ghosts/` and `exported/`. Screenshots and recordings
are left behind deliberately, being potentially gigabytes and nothing that stops
working. A copy that fails part way removes what it wrote, because nothing here
overwrites an existing file and a truncated eeprom would be loaded every run
from then on.

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

**Load Mods** (Extended Options) picks one of them, and only one — a menu that
let you stack them would be offering something the single overlay slot does not
do. The name goes in the config as `Mod.ModDir`; `modListApplySelection()`
mounts it in `main()` **between `configInit()` and `romdataInit()`**, which is
the only window there is: the config is where the name lives, and romdata is
what goes looking for the files. `fsInit()` cannot do it, being what has to be
up before the config can be found at all.

**Files swap live; segments cannot.** Choosing a mod calls `modListSwap()`,
which drops every file slot (`romdataResetFiles()`), the sizes the game
remembers for them (`filesInit()`), the stage tables and the caches saying which
of a mod's optional directories exist, then re-runs `modloaderInit()` and
`modConfigLoad()`. Nothing outside romdata holds file data — `fileLoad()`
inflates into the stage pool and reads no further — so this is safe with a stage
loaded; the pool is wiped on the next stage load and everything comes back from
the new mod. The menu backdrop you are looking at stays as it was until then.

A mod with a `segs/` directory is the exception and gets the restart path
instead, in both directions. Segments are read once at boot and land in
`MEMPOOL_PERMANENT`, which is never cleared and is closed off as soon as the
stage pool is placed after it (see the header comment in `memp.c`) — `texInit()`
copies the texture list into it, `sndInit()` builds the audio banks once, and
the game holds raw pointers into all of it. Swapping only such a mod's files
would leave the game half converted, which is worse than restarting. Restart Now
goes out through `exit()` and comes back in `cleanup()` via
`updateRelaunchSelf()`; that ordering is the updater's, and for its reason:
everything is written before anything starts again.

The list is directories under `mods/` plus loose `mod*` folders beside the
executable, each one having to contain `files/`, `segs/`, `textures/` or a
`modconfig.txt` before it counts. A `--moddir` on the command line wins over the
config and the page says so — the All in One launcher passes several, and a
stored choice quietly displacing them would be a bug nobody could see.

`tools/importmod` builds a mod directory out of a console mod's xdelta, and
`tools/modcodediff` shows what that mod changed in the ROM's code.

**A modconfig is optional, and one bad block used to cost the rest of the file.**
`files/`, `segs/` and `textures/` each make a mod dir on their own, so an
imported console mod has no `modconfig.txt` at all and `modConfigLoad()` checks
before asking for it — `fsFileLoad()` logs every miss as an error. Inside the
file, a `stage` block naming a stage this build does not have is skipped rather
than fatal to the parse: mod configs are written against the mod's own stage
table, and the top-level loop stops at the first block it cannot parse. GE-X
opens with `stage 0x10`, which took its three valid remaps down with it; stage
0x49 then loaded `bg_mp17` under `bg_mp5`'s tiles and died in
`preprocessBgSection1()`. The remaining warnings are the mod's, not ours.

### Where a texture pack goes

Four directories are involved and only one of them puts a pack in the menu.

- `texture-packs/<name>/` **beside the executable** (or in the save directory if
  that is not writable) is the pack list — the two `fsChooseOutputDir()` tries.
  A `.zip` or `.7z` counts, and is unpacked once into `texture-packs/.cache/`.
- `textures/` in the **base** directory (the one holding the ROM, so `data/` in a
  normal build) and in **every mod directory** is read as well, by
  `texpackScan()`, but is not a pack: it cannot be chosen or switched off from
  the menu, and the chosen pack outranks it.
- `textures/` **beside the executable is read by nothing.** It is the obvious
  place to put one and it silently does nothing.

All of it needs `Mod.LoadTextures=1`. The images are `<texnum>.png`, four
lowercase hex digits; a mod's `textures/*.bin` is the raw-N64-data path instead
and is unrelated. `tools/texpack/riceconvert.py` writes the first kind.

### Replacement textures are decoded off the render thread

`texpackLoadReplacement()` used to decode where it was called, which is inside
`import_texture()` in `gfx_pc.cpp` - so the first draw of each texture paid for a
whole PNG on the render thread. That is the stutter testers describe as a pack
"streaming in": measured over the PD Plus pack, 57.9 Mpx/s, about 4.6ms for an
average texture and roughly a whole frame for a 1024x1024.

**The decoder was never the slow part.** `pngread.c` and stb_image measure the
same to within a fraction of a percent on PNG (57.9 vs 57.8 Mpx/s), so swapping
in a vendored library buys nothing. Being on the render thread is the whole cost.

So a request queues the work and returns NULL, the caller draws the original
exactly as it does for a texture no pack replaces, and `gfx_texpack_poll()` at
the top of the next frame drops the cache entries holding the original so the
draw after that asks again and gets the replacement. Two things this depends on:

- `gfx_texture_cache_lookup()` answers **before** the pack is ever consulted, so
  the entry has to be erased or the original stays on screen forever. Eviction
  goes by `texpackGetTextureNum(key.texture_addr)`, not by address: one texture
  number can sit at several addresses.
- `rendering_state.textures` holds pointers into that map, so erasing anything
  means clearing it and setting `textures_changed`, the same as
  `gfx_texture_cache_clear()` does.

Decoded images wait in their slot to be claimed - normally one frame - under a
byte budget, because a texture that goes off screen may never ask again. Losing
one costs a re-decode and nothing else. `texpackFreeIndex()` stops the worker
before freeing anything: the worker reads the index and only stops between jobs.

JPEG is decoded the same way, on the same thread - see the format note below.

### Pack image formats, and which way up they go

**Formats.** `<texnum>.png` goes through `pngread.c`, which is ours because PNG is
a zlib stream in chunks and zlib was already linked. `<texnum>.jpg` / `.jpeg` goes
through `jpegread.c`, which is stb_image and the only thing in the port that uses
it - baseline JPEG is Huffman tables, an inverse DCT and chroma upsampling, and
progressive is more again, none of it worth writing. `STBI_ONLY_JPEG` keeps that
vendored header to the one decoder, so the two can never disagree about a file.
It is also the fallback for a PNG that `pngread.c` declines: that decoder handles
what image editors write and refuses the rest rather than guessing, and Adam7
interlacing is one of the refusals - which the PD Plus pack ships several hundred
of among its font glyphs. `pngRead()` is still tried first, so nothing that
worked before changes hands. JPEG has no alpha: stb fills it with 255, and a
texture needing transparency has to ship as PNG. The Rice naming (`_all`, `_rgb`, `_a`) is still PNG-only, those
packs being PNG by convention.

**Row order.** A texture's data in the port has its first row at the bottom, and
two conventions exist that a filename cannot tell apart:

- Our own dumps are written the right way up, for editing, and are turned over on
  load. This is the default for `<texnum>.png`.
- Emulator (Rice) packs, and packs built for the VR fork, are already in N64 row
  order and must **not** be turned over.

So the folder says which it is, and `replaceFlip[]` records it per texture:

- a folder named **`ext_tex`** - what the VR fork reads, so a pack built for it
  works unpacked and dropped in as it comes.
- a folder holding a **`bottomup.txt`** - the same, for a pack under any other
  name.

Inherited by subfolders, so the marker goes at the top of the pack once. It is
logged when it fires, because getting it wrong means every texture in the pack is
upside down and nothing else says so. The trap in `pd-texture-data-is-bottom-up`
applies to checking this by eye: pick a texture with lettering, not a symmetric
one.

### Texture pack keys

`Mod.DumpTexturesKey` (F7), `Mod.TexturePackKey` (F8, packs on/off),
`Mod.TexturePackReloadKey` (F9, re-read the pack where you stand) and
`Mod.TexturePackCycleKey` (F10, next pack, round through "none"). All four are in
`texpackTick()` and bindable in Extended Options. F9 and F10 exist because
comparing packs otherwise meant quitting, swapping folders and relaunching.

Both are safe with the decode worker running - the reload path stops it first -
and both are worth re-testing on Windows, threads being what they are.

### The ROM symbol file

`tools/pd.ntsc-final.sym` is 5149 game-segment symbols, and `modcodediff` reads
it to put real names on a mod's changes. It was cut from a matching decomp
build, which this tree cannot produce — its `platform.h` detects win32/linux and
x86/arm only, so `ultra64.h` drags in the host's `stdint.h` and IDO's `cfe`
cannot parse it. The port fork had let the ROM build rot before it dropped the
Makefile.

It came from a clone of the upstream decomp instead, built with the IDO 5.3 in
`../n64-toolchain/ido5.3_recomp` (`build/recomp/{5.3,7.1}`, with `recomp` itself
touched old so make does not try to rebuild it), `armips` on PATH, and zeroed
stubs for `build/*/rsp/*.bin` because armips 0.11 rejects the RSP microcode.
IDO 7.1 is not needed: the files that want it are all `src/lib/` audio, which is
the lib segment.

`modcodediff --prepare-diff DIR` writes both binaries and a `pd-diff` wrapper
there, for reading a rewritten function with asm-differ - which aligns the two
instruction streams rather than pairing them by address, and is the difference
between reading a rewritten function and staring at it. diff.py needs colorama,
watchdog and levenshtein, and pip refuses to install them system-wide (PEP 668),
so they are in a venv at `../n64-toolchain/venv`.

**Why the symbols are exact even though that ROM does not match.** The game
segment is linked at a fixed `0x7f000000`, so nothing outside it moves its
contents. The built segment is the same length as the real one and every one of
its 3264 `jal` entry points is at the same address; the 3.1% of words that
differ are all `addiu`/`lw`/`sw`/`jal` immediates pointing at the segments that
did move. Function addresses are therefore right, and that check - entry points
identical - is the one to repeat if the file is ever regenerated.

## Weapon behaviour belongs on the weapon

The game decides a lot by comparing the weapon number - `if (weaponnum ==
WEAPON_SHOTGUN)` - which is a question a mod cannot answer, because a mod that
brings its own guns numbers them its own way. GE-X patched 28 functions in
`bondgun.c` and 31 regions in `propobj.c` for exactly that reason, nearly all of
them number swaps; `tools/modcodediff` says which functions a given mod cares
about.

So the behaviour moves onto the weapon:

- `struct weapon.flags2` - a second flags word, the first having all 32 bits
  spoken for. 19 behaviours so far, read with `weaponHasFlag2()`.
- `struct weapon.pickupsound` and `.unequippedreloadindex` - where the answer is
  a value rather than a yes.
- `struct weaponfunc.flags` - for what belongs to one *function* of a weapon
  rather than the weapon. The Dragon is a rifle until you throw it down and then
  it is a mine. Read with `weaponfuncHasFlag()`, or `gsetHasFunctionFlags()`
  where a `gset` is to hand.

A modconfig `weapon`, `weaponfunc` or `tvscreen` block sets any of them:

```
weapon 15 { unequippedreload 1 unequippedreloadindex 1 pumpaction 1 }
weaponfunc 21 1 { proximitymine 1 }
tvscreen 5 { sameas 3 }
```

### Converting another one

`bondgun.c` still has around 100 of these comparisons and `propobj.c` around 74.
Three checks before the edit, one after. Each of them has already caught a
silent bug.

1. **Is the weapon definition shared?** Three are: `invitem_keycard` by eight
   numbers, `invitem_hammer` by four, `invitem_rocket` by the rocket and the
   Skedar rocket. A field on the definition cannot tell those apart, and the
   rocket pair genuinely want different answers - opposite ones, in
   `objTestForPickup`. Those tests stay keyed on the number, with a comment.

2. **Is the *function* definition shared?** Ten are, and this bites harder.
   Flagging the five functions that leave a proxy also flagged the timed mine,
   which shares its threat detector with the proximity mine. "Is a proximity
   mine" ended up a weapon flag for the mine and a function flag for the three
   that only become one on their second function.

3. **Does the flag's set differ from the list *inside its enclosing
   condition*?** A flag can be exactly right in isolation and still change
   behaviour. Substituting `FUNCFLAG_PROXIMITYMINE` for the Dragon clause in
   `objDamage` would have armed the N-bomb, which carries that function flag but
   is not on the explodes-when-shot list. `FUNCFLAG_WALLHUGGER` would have set
   the Devastator hugging walls, because the wall hugger function is the
   launcher's own and already carries `FUNCFLAG_STICKTOWALL`.

4. **Afterwards, dump the sets and compare them against the lists you replaced**,
   resolved through the `g_Weapons[]` designators so a shared definition shows up
   as all of its numbers. For a function flag, enumerate all 188
   weapon-and-function pairs - a shared function is invisible from the weapon
   side. For a mapping, read the old chain back out of `git show HEAD:` and
   compare entry by entry.

   This is not optional. Appending a second `flags2` initialiser to a weapon that
   already had one is not a duplicate, it is the next field: that put
   `WEAPONFLAG2_LANDSONHIT` into `unequippedreloadindex` and gave the remote mine
   a reload index of 32, and it built cleanly.

`g_Weapons[]` is written with designated initialisers - `[WEAPON_SHOTGUN] =
&invitem_shotgun` - so a behaviour can no longer land on the wrong gun by
miscounting, with a `_Static_assert` tying its length to the enum. It was a
positional list until the first of these conversions.

### What is deliberately not converted

**Dispatch is not behaviour.** `objLand` picking `boltLand` or `knifeLand` by
weapon number, and the `case WEAPON_X:` labels in `bondgun.c`, are jump tables.
A function pointer on the weapon would do it and would be a different kind of
change - moving code identity into data rather than parameters.

**Some tests belong to the shot, not the gun.** `weaponTick`'s grenade timers and
the Devastator's wall hugger read weapon and function together with live timer
state; a flag on either one does not hold them.

**Some are one weapon with one quirk** whose intent is not visible from the
surrounding window - the combat knife's two sites in the hand state machine, the
remote mine's left-hand rule before it was understood as the detonator hand.
Naming those from a guess is worse than leaving the comparison in place.

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
