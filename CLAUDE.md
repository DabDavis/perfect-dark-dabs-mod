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
- **Screenshots, the recorder, ffmpeg, GL capture** — [recording.md](CLAUDE-notes/recording.md): the frame is presented before `videoEndFrame()`; NV12 on the GPU; encoder detection; why it must never wait for the encoder
- **Ghost Trials networking** — [ghost-trials.md](CLAUDE-notes/ghost-trials.md): WinHTTP and libcurl, why not one of them, and what the worker thread may touch
- **Check for Updates** — [updater.md](CLAUDE-notes/updater.md): `update.txt`, the baked-in channel, the two-rename swap
- **Mod directories, Load Mods, modconfig, `modcodediff`, the ROM symbol file, the data segment and importing a mod's weapon definitions** — [mods.md](CLAUDE-notes/mods.md): only the first mod dir joins the file search; files swap live, segments cannot; the `datasegment` block, `moddata.c`, and "where this stands" for continuing the import work
- **Texture packs** — [texture-packs.md](CLAUDE-notes/texture-packs.md): where a pack goes (one of four directories is read by nothing); decoding off the render thread and the backlog; the kept store and why a decode must never be handed over (textures flicker to the original otherwise); an emulator pack's image is the tile, the renderer maps the padded row; PNG is ours, JPEG is stb_image, and the two row orders; font glyphs (the image is the whole tile, the outline pass wants both images); F7–F10
- **The Friends of Joanna collab tree, `../pd-fojo-monorepo-collab/`** — [fojo-collab.md](CLAUDE-notes/fojo-collab.md): what their mod loader does that ours does not, why the trees cannot merge, and what is worth borrowing
- **Weapon numbers, `flags2`, converting a `weaponnum` comparison** — [weapons.md](CLAUDE-notes/weapons.md): the four checks, and what is deliberately not converted

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
data tables, missions, music, environments, star field and weather all import
(`build/mods/GE-X_6a_01-19-25/`, importer version 12); what is left is the code
GE-X *rewrote*, which `modcodediff` lists and nothing follows yet. Read
[mods.md](CLAUDE-notes/mods.md) from "GE-X's solo missions in the port" to the
end before touching any of it, then:

1. **Regenerate the list** — the summary is the work queue, largest first:
   ```sh
   python3 tools/modcodediff --rom ../pd-upstream/pd.ntsc-final.z64 \
       --patch build/mods/GE-X_6a_01-19-25/GE-X_6a_01-19-25.xdelta --summary
   ```
   `constants` regions are tables to follow (most are done); `rewritten` ones
   need reading. Remaining rewritten, by size: `shieldhit_health_to_rgb` (32
   words), `bgun_play_prop_hit_sound` (22), `player_tick` (21),
   `bgun_tick_inc_attacking_shoot` (19), `casing_create_for_hand` (17),
   `projectile_tick` and `chr_damage` (16 each), `beam_render` (14),
   `hand_tick_attack`, `bot_tick_unpaused` (9 each), then a long tail of 1–7
   words (`menu_render`, `koh_tick`/`koh_init`, `cheat_is_unlocked`,
   `botinv_score_weapon`, `bwalk_update_horizontal` is a speed cave ...).
   `--prepare-diff DIR` writes both binaries; `mips-linux-gnu-objdump -b binary
   -m mips:4300 -EB -D --adjust-vma=0x7f000000` reads them.
2. **Two ways to follow a change.** A renumbered compare is a
   `follow_immediate(s)` site (the `playerconst`/`bgstage` pattern, one table
   row in both importers). A rewritten function is run on the toy MIPS
   (`emulate()` in `tools/importmod`, `emuRun()` in `port/src/modimport.c`,
   the weather is the worked example) and written out as the port's own
   config. Whatever is added goes in **both importers**, `IMPORT.txt`
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
   two importers order the older `playerconst`/`bgstage`/`roomstage` lines
   differently (content identical); mod-only stage ids (GE-X's 0x4d–0x50)
   are skipped by `stage {}` blocks with a warning.

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

stdout is block-buffered when redirected, so log lines sit unwritten. Flush a live
process before reading or killing it — `SIGKILL` discards the buffer:

```sh
gdb -p $(pgrep -x pd.x86_64) -batch -ex 'call (int)fflush(0)'
```

A hung process ignores `SIGTERM`, because the shutdown handler cannot run.

For "state disappears" bugs, instrument every mutating path rather than reading code.
The simulant-clearing hunt was solved by logging that showed a count of zero at every
suspected site — nothing was being cleared; nothing had ever been created.
