# Perfect Dark port — simulant/mod fork

Fork of the [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark) port.
`port` tracks upstream; work happens on `dabs-mod`.

`git checkout port` returns to stock at any time.

## What is written down here

Each note in `CLAUDE-notes/` is a thing that was got wrong once. Read the note
for an area **before** touching it — none of them are inferable from the code,
and each one cost a detour. This file keeps only what every session needs; the
notes are read when their area comes up.

- **The Windows build, wine, the pd.ini format** — [windows-build.md](CLAUDE-notes/windows-build.md): mingw prefix, WinHTTP, and why `Mod.LoadTextures=1` on its own line does nothing
- **chrs, bodies, heads, simulants, memory pools, mpconfig** — [chrs-and-memory.md](CLAUDE-notes/chrs-and-memory.md): a chr's prop is read before its tick; the ~50KB head copy that empties the stage pool; one head modeldef cannot sit on two bodies; ROM-resident structures never grow
- **Saves, eeprom, where pd.ini lives, the migration** — [save-format.md](CLAUDE-notes/save-format.md)
- **Menu text, `textMeasure()`, reaching the widescreen pillars** — [text-rendering.md](CLAUDE-notes/text-rendering.md)
- **Adding stages** — [stage-numbers.md](CLAUDE-notes/stage-numbers.md): only 27 ids are free, and four are taken outside the table
- **Screenshots, the recorder, ffmpeg, GL capture** — [recording.md](CLAUDE-notes/recording.md): the frame is presented before `videoEndFrame()`; NV12 on the GPU; encoder detection; why it must never wait for the encoder; running on the real GPU with no window (llvmpipe hides driver limits)
- **Ghost Trials networking** — [ghost-trials.md](CLAUDE-notes/ghost-trials.md): WinHTTP and libcurl, why not one of them, and what the worker thread may touch
- **Check for Updates** — [updater.md](CLAUDE-notes/updater.md): `update.txt`, the baked-in channel, the two-rename swap
- **Mod directories, Load Mods, modconfig, `modcodediff`, the ROM symbol file, the data segment and importing a mod's weapon definitions** — [mods.md](CLAUDE-notes/mods.md): only the first mod dir joins the file search; files swap live, segments cannot; the `datasegment` block, `moddata.c`, and "where this stands" for continuing the import work
- **Texture packs** — [texture-packs.md](CLAUDE-notes/texture-packs.md): where a pack goes (one of four directories is read by nothing); decoding off the render thread and the backlog; the kept store and why a decode must never be handed over (textures flicker to the original otherwise); an emulator pack's image is the tile, the renderer maps the padded row; PNG is ours, JPEG is stb_image, and the two row orders; font glyphs (the image is the whole tile, the outline pass wants both images); F7–F10
- **The Friends of Joanna collab tree, `../pd-fojo-monorepo-collab/`** — [fojo-collab.md](CLAUDE-notes/fojo-collab.md): what their mod loader does that ours does not, why the trees cannot merge, and what is worth borrowing
- **Measuring a crowded match, `--rng-seed`/`--fixed-step`, where the frame goes, why Increase Poly Models was inert** — [performance.md](CLAUDE-notes/performance.md): compare instructions per frame on a seeded fixed-step match; the renderer is 60% of the main thread and is built at -O2; why the decomp at -O2 played a different game (game-defined sinf/cosf, an uninitialised pad flag) and how a divergence is bisected; the smoothing gate wanted RSP lighting the game never uses
- **Weapon numbers, `flags2`, converting a `weaponnum` comparison** — [weapons.md](CLAUDE-notes/weapons.md): the four checks, and what is deliberately not converted; a launcher branch keyed on the number must test the function's type, or a mod's table crashes it

**[DabDavisGitHub.md](DabDavisGitHub.md)** is the companion to this file: the
GitHub remote, how commits are written, how a push becomes a release, and how the
stable and dev channels reach a player. Read it before pushing or tagging —
`dabs-mod` is a public default branch and a push to it rebuilds what every
dev-channel player's Check for Updates points at. A push that changes only
`CLAUDE.md`, `CLAUDE-notes/` or `DabDavisGitHub.md` does not build.

When a session gets something wrong that the code could not have told it, write
it down: a new note, or a section in the one for its area, and a line here.

## GE-X import: where to pick up (2026-09-05)

The console mod GE-X 6a is the reference case for the mod loader. Its assets,
data tables, missions, music, environments, star field, weather, shield
colours, hit sounds, co-op buddies, the weapon lists behind eighteen flags, two damage rules and the unlocks all import (`build/mods/GE-X_6a_01-19-25/`, importer version 23); what is left is the code
GE-X *rewrote*, which `modcodediff` lists and nothing follows yet. Read
[mods.md](CLAUDE-notes/mods.md) from "GE-X's solo missions in the port" to the
end before touching any of it, then:

1. **Regenerate the list** — the summary is the work queue, largest first:
   ```sh
   python3 tools/modcodediff --rom ../pd-upstream/pd.ntsc-final.z64 \
       --patch build/mods/GE-X_6a_01-19-25/GE-X_6a_01-19-25.xdelta --summary
   ```
   `constants` regions are tables to follow (most are done); `rewritten` ones
   need reading. Every rewritten function of 9 words or more is read, and
   the tail's unlocks family too; what remains of the tail is listed in
   mods.md ("The tail, and the unlocks": the menu palette, the run-speed
   cave, King of the Hill, GE-X's hats ...), and the weapon-number sites
   the port still tests literally (weapons.md: ~85 in bondgun.c, ~70 in
   propobj.c), each a `FLAG_SITES` row once converted.
   `--prepare-diff DIR` writes both binaries; `mips-linux-gnu-objdump -b binary
   -m mips:4300 -EB -D --adjust-vma=0x7f000000` reads them.
2. **Two ways to follow a change.** A renumbered compare is a
   `follow_immediate(s)` site (the `playerconst`/`bgstage` pattern, one table
   row in both importers). A rewritten function is run on the toy MIPS
   (`emulate()` in `tools/importmod`, `emuRunArgs()` in `port/src/modimport.c`,
   the weather is the worked example, the shield colour the one for a
   function that tests its caller, the hit sounds the one for a list of
   weapon numbers: seed the tables, run per number, read what it stored)
   and written out as the port's own config. A weapon-number test in the
   port becomes a flag (weapons.md) and the importer writes `weaponflags`;
   arguments to a call are followed by call and register
   (`follow_call_args()`, the co-op buddies), since one function can hold
   four number spaces; a weapon test that became a list is read as a
   compare chain (`follow_compare_chain()`, `FLAG_SITES`), and a flag is
   written only when every site of it agrees. Across the archive the most rewritten functions are
   `player_tick` (38 mods, 27 of them a no-op word - see mods.md), then a
   block of 23 that every "all solos in multi" patch shares (`tex_init`,
   `setup_create_props`, `mp_start_match`, the unlock handlers);
   `modcodediff --summary` over every patch takes four minutes with `xargs
   -P 8` and is how to know whether a function is one mod's or the
   archive's. Whatever is added goes in **both importers**, `IMPORT.txt`
   lines identical, and bumps `MODIMPORT_VERSION` so old imports redo
   themselves.
3. **Test headlessly** on Runway (0x22) or later — Dam (0x30) and Facility
   (0x33) are unfinished in the patch and prove nothing:
   ```sh
   cd build && timeout -k 5 60 xvfb-run -a ./pd.x86_64 --moddir mods/GE-X_6a_01-19-25 \
       --savedir /tmp/pdsave --skip-intro --no-sound --boot-stage 0x22 --log
   ```
   Exit 124 is "ran until stopped"; 137 is a crash or hang, read the log.
   `--moddata-trace`, `--chr-trace`, `--setup-trace` and the
   `weather:`/`music:` log lines say what applied. `--boot-stage` takes the
   stage id, not the menu slot: GE-X's `g_SoloStages` remaps six missions.
4. **Open tester reports**: no guards on Dam (GE-X spawns them by AI as the
   player advances; a headless run cannot exercise it — a person must). Check
   the tester's binary commit before debugging a report from
   `sdg@10.8.0.3:~/pd-test/` (`strings pd.x86_64-linux | grep -m1 -E
   '^[0-9a-f]{7}$'`).
5. **Known gaps, deliberate**: stages a mod took weather away from keep the
   port's stock weather entry (inert without a rain/snow script command); the
   two importers order the `playerconst`/`bgstage`/`roomstage`/`buddyconst`
   lines differently (content identical); mod-only stage ids (GE-X's 0x4d–0x50)
   are skipped by `stage {}` blocks with a warning.

## Build and run

```sh
cmake -G"Unix Makefiles" -Bbuild .     # only after adding/removing source files
cmake --build build -j8
./build/pd.x86_64                      # needs build/data/pd.ntsc-final.z64
./build/pd-modded.sh                   # with the All in One mod
```

Release builds use `-Og` for the decomp except the hot files (`PD_HOT_O2`) — see the comment in `CMakeLists.txt` and [performance.md](CLAUDE-notes/performance.md). Warnings
about uninitialised locals in `collision.c`, `model.c`, `menu.c` and `mplayer/setup.c`
are pre-existing decomp artifacts; check `git diff` before assuming one is yours.

`build/` is gitignored, including the mod directories and `pd-modded.sh`. Keep the
mod zip: a clean rebuild takes them with it.

The Windows cross-build is in [CLAUDE-notes/windows-build.md](CLAUDE-notes/windows-build.md).

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

## Debugging

Guessing from source failed repeatedly here; the stack was right every time.

```sh
# crash: symbolise the offsets the game prints
addr2line -f -C -e build/pd.x86_64 0x11abf5

# hang: main thread only, Mesa worker threads are noise
gdb -p $(pgrep -x pd.x86_64) -batch -ex "thread 1" -ex "bt 14"
```

**A Windows crash dialog** gives `PC` and `MAIN MODULE: [base]`, and a
backtrace of `[base]+offset` lines. The offset is from the image base, which
the release exe has at `0x140000000`, and the exe keeps its DWARF (30 MB for a
reason), so:

```sh
gh release download v3.1.2 -p pd.x86_64-windows.exe -O pd-v3.1.2.exe
x86_64-w64-mingw32-addr2line -f -C -i -e pd-v3.1.2.exe 0x1401ba7a3   # 0x140000000 + offset
```

Plain `addr2line` says `??` for every line. When the report does not say
which build, try each stable exe: the right one symbolises to a stack that
makes sense as a call chain, the wrong ones to functions that could never
have called each other. The reporter's data is not ours: a crash in a mod's
model that no stage here reproduces is a model their import has and ours does
not, and the question to ask them is which mission and what the first line of
their `mods/<mod>/IMPORT.txt` says.

stdout is block-buffered when redirected, so log lines sit unwritten. Flush a live
process before reading or killing it — `SIGKILL` discards the buffer:

```sh
gdb -p $(pgrep -x pd.x86_64) -batch -ex 'call (int)fflush(0)'
```

A hung process ignores `SIGTERM`, because the shutdown handler cannot run.

For "state disappears" bugs, instrument every mutating path rather than reading code.
The simulant-clearing hunt was solved by logging that showed a count of zero at every
suspected site — nothing was being cleared; nothing had ever been created.
