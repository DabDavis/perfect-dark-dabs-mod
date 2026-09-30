# Perfect Dark port — simulant/mod fork

Fork of the [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark) port.
`port` tracks upstream; work happens on `dabs-mod`.

`git checkout port` returns to stock at any time.

## What is written down here

Each note in `CLAUDE-notes/` is a thing that was got wrong once. Read the note
for an area **before** touching it — none of them are inferable from the code,
and each one cost a detour. Every note opens with a **Digest**: one entry per
topic it covers (these used to live here). Read the digest first, then the
section it names. The notes are large (`ge-bean.md` is 850KB): grep the digest
or a section heading, don't read one whole.

- [windows-build.md](CLAUDE-notes/windows-build.md) — The Windows build, wine, the pd.ini format
- [chrs-and-memory.md](CLAUDE-notes/chrs-and-memory.md) — chrs, bodies, heads, simulants, memory pools, mpconfig; Simulants running on the spot at the head of a ladder; A head on a body it was not made for; A simulant's stat sliders, and what the player count really changes; A head past the Combat Simulator's list
- [save-format.md](CLAUDE-notes/save-format.md) — Saves, eeprom, where pd.ini lives, the migration
- [text-rendering.md](CLAUDE-notes/text-rendering.md) — Menu text, `textMeasure()`, reaching the widescreen pillars
- [stage-numbers.md](CLAUDE-notes/stage-numbers.md) — Adding stages
- [mods.md](CLAUDE-notes/mods.md) — The Stage Loader: every mod's maps as arenas beside the mod loaded; Mod directories, Load Mods, modconfig, `modcodediff`, the ROM symbol file, the data segment and importing a mod's weapon definitions; A rule or colour a mod's code changes that is not a weapon's
- [vulkan.md](CLAUDE-notes/vulkan.md) — The Vulkan renderer
- [post-process.md](CLAUDE-notes/post-process.md) — SMAA and FSR 1 upscaling
- [recording.md](CLAUDE-notes/recording.md) — Screenshots, the recorder, ffmpeg, GL capture; `bool` is two sizes here
- [ge-bean.md](CLAUDE-notes/ge-bean.md) — GE Plus (GoldenEye converted from the ROM) and GoldenEye XBLA "Bean": conversion, HD levels/characters/skies, missions, cinema, HUD, sounds, guns, guards, vehicles, CE. Topics: GE Plus's level music; GoldenEye's death replay; Music a mod or a conversion adds; GoldenEye 007 XBLA (Rare's "Project Bean" build) characters on GoldenEye X; GoldenEye XBLA's HD levels on the levels converted from the ROM; GE Plus's arenas converted from the player's GoldenEye ROM at startup; GoldenEye's HUD on GE Plus's levels; A converted level's doors and pickups sound like GoldenEye's; Every other sound of a converted level; A tester's seven on Dam, and the four conversion rules behind them; GoldenEye's tank; GE Plus's Cinema, and staying inside GE Plus; A converted mission ends on GoldenEye's report and statistics pages; GoldenEye's credits after the Cradle; A converted mission's opening and its ending; GE Plus's EXTRA page, the Cinema's Intro/Outro, and GoldenEye's monitor programmes; A converted mission's character rows, and testing the way a tester runs; GoldenEye's own 2-D collision on a converted level; A converted stair's rail and the wall beside it; A converted level's line tests follow the line; A guard skipping waypoints on a converted level; A converted level's hanging TVs under their mount; The sky's cloud plane and the two projection matrices; A converted prop's skeleton, and Caverns' eyelid and iris doors; A converted level's door in an outside wall; A guard inside a converted door, and a destroyed prop that stayed whole; A converted level's vents and crawl spaces; A converted level's ladders; An arena's weapon spots and ammo crates; Anything that rolls the weapon table on a converted level; The room a converted level's picture is drawn from; A converted level's portals are slabs; GoldenEye's Japanese and PAL cartridges; A converted mission's civilian casualties; A converted mission's switches and its truck's heading; A converted mission's truck is solid and waits at a shut gate; GoldenEye's gadgets, the dive off Dam, and rooms named by a pad; A converted mission's ending: the animation scale of four, and the room a cutscene camera draws from; Bond seen twice through Dam's dive; Black mirrors floating in the air on Dam and Facility; The GoldenEye XBLA Community Edition's fixes, taken as GoldenEye's own; GoldenEye XBLA's skies over an HD level; Guards invisible up close on an HD level; GoldenEye's own cameras outside an HD level; An HD model's UV scale is in its vertex shaders; A converted level's own backdrop drawn over the HD level; The release's look on a screen that is not a level; GE Plus's folder screens in the release's art; The folder screens' second and third pass, and Bean's own menu files; The GoldenEye XBLA Community Edition, applied by the game; A level's instancing records, and the pines that were one tree; A Xenos primitive 5 is a triangle fan, and the HD truck's wheels; GoldenEye's sight at the edge of the view; Frigate's sky and water, and the port's flat sea; Runway's table in the air, its plane's ending, and an opening shot of nothing; The tick a converted vehicle is given its animation; A long fall on a converted mission; Judging a conversion against GoldenEye itself; A converted GoldenEye level's floor vanishing under the player; A model whose two lists came out crossed at the load: Jungle's black vines; An HD prop's logos fighting its own walls; A helicopter's rotor is a held position; A GE Plus guard kneeling with his gun upright; GE Plus guards that never hit at all; Bullets through the HD look's railings; How often a GE Plus guard hits; A converted autogun, and a weapon standing on an object; Bond's parka hood, and anything a head cut off a body carries on the body's picture; A GoldenEye gun's definition; GE Plus in third person, and a converted pane's tail; HD light shafts, monitor recesses, cut-out rims and magenta placeholders
- [ghost-trials.md](CLAUDE-notes/ghost-trials.md) — Ghost Trials networking
- [crash-reports.md](CLAUDE-notes/crash-reports.md) — Crash reports
- [updater.md](CLAUDE-notes/updater.md) — Check for Updates
- [texture-packs.md](CLAUDE-notes/texture-packs.md) — Texture packs
- [xbla.md](CLAUDE-notes/xbla.md) — The Xbox 360 XBLA release: its textures, and drawing its models
- [level-sheen.md](CLAUDE-notes/level-sheen.md) — Level Reflections, and the all-surface Level Sheen that was removed
- [model-packs.md](CLAUDE-notes/model-packs.md) — Model packs and the asset dump
- [fojo-collab.md](CLAUDE-notes/fojo-collab.md) — The Friends of Joanna collab tree, `../pd-fojo-monorepo-collab/`
- [performance.md](CLAUDE-notes/performance.md) — Measuring a crowded match, `--rng-seed`/`--fixed-step`/`--exit-frame`, where the frame goes
- [third-person.md](CLAUDE-notes/third-person.md) — The third person camera, and why melee, rockets and beams came out of it; Sitting in the Institute's chairs
- [settings.md](CLAUDE-notes/settings.md) — Defaults and the Settings Preset
- [weapons.md](CLAUDE-notes/weapons.md) — Weapon numbers, `flags2`, converting a `weaponnum` comparison
- [randomizer.md](CLAUDE-notes/randomizer.md) — The Randomizer: a mission dealt again from its own pieces
- [randomizer-run.md](CLAUDE-notes/randomizer-run.md) — The Randomizer's run: a room at a time across every map
- [project-list.md](CLAUDE-notes/project-list.md) — Projects we can do, not started
- [languages.md](CLAUDE-notes/languages.md) — the Language setting, translations, CJK fonts
- [simnav.md](CLAUDE-notes/simnav.md) — The simulants' navmesh and its links; Mod.SimBrain (stock is bit-identical, modern walks stock's decisions along the navmesh); the traps met doing it

Long-range plans (modularity, simulant AI, GE converter decoupling,
optimisations, head fit) are in `~/perfect-dark/PLANS/`, outside this tree;
start at its `README.md`.

The GE-X import's pick-up list (2026-09-05) is in mods.md's digest.

**[DabDavisGitHub.md](DabDavisGitHub.md)** is the companion to this file: the
GitHub remote, how commits are written, how a push becomes a release, and how the
stable and dev channels reach a player. Read it before pushing or tagging —
`dabs-mod` is a public default branch and a push to it rebuilds what every
dev-channel player's Check for Updates points at. A push that changes only
`CLAUDE.md`, `CLAUDE-notes/` or `DabDavisGitHub.md` does not build.

When a session gets something wrong that the code could not have told it, write
it down: a new note, or a section in the one for its area, and a line here.

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
