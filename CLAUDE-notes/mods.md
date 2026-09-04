# Mod directories

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


## The ROM symbol file

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

## The data segment, and `pd.ntsc-final.datasym`

The game code segment is only half of what a console mod patches. The other
half is the ROM's **data segment** - one compressed blob at `ROMDATA_DATA_OFS`
(0x39850), inflated to 0x30e40 bytes at RAM 0x80059fe0 - which holds the
`.data` of every lib and game object: the weapon definitions (`invitem_*` and
everything they point at), `g_Weapons[]` (which number is which definition),
`g_MpWeapons`, `g_MpWeaponSets`, `g_MpHeads`/`g_MpBodies`, `g_Stages`,
`g_HeadsAndBodies`, `g_ModelStates`, the fog tables, the TV screen command
lists. None of that is in `pd.ntsc-final.sym` or `.files`, and until 2026-09-04
`modcodediff` did not look at it at all - so GE-X's whole weapon set, which is
a renumbering *and* a rewrite of nearly every definition, was invisible. Its
44 changed words in `g_Weapons` and 1207 in `invitems.c` explain the
`li at,26` -> `li at,2` constant swaps in the code: slot 2 is now the knife.

`tools/pd.ntsc-final.datasym` (2328 symbols, real addresses) and the `data`
lines in `pd.ntsc-final.files` were cut by `tools/mkdatasym` from the same
upstream decomp build as the `.sym` (`../pd-upstream/build/ntsc-final/pd.map`
and its `pd.z64`). Unlike the code segment, **the build's data addresses are
not the ROM's**: its lib is a different size, so everything is shifted by a
constant (-0x270 on ntsc-final) and the segment is 0x170 bytes shorter. The
tool finds the shift by correlating each object's content, then finds the base
by trying every candidate against the `lui`/`addiu` pairs in the stock game
code until every anchor table is one the code actually reaches - which is
what caught the first version being 0x90 off, and then 0x200 off with a
single hot global outvoting the rest. Regenerating it means re-running that
check, not trusting the map.

`modcodediff` now reports the segment after the code: per file, per symbol,
and for the tables whose layout it knows (`TABLES` and `STRUCTS` in the
script) as fields with the port's names on them. A mod that rebuilt its data
segment (the performance mod: 85.8% differs, -44560 bytes) gets one line
saying so, as with its code.

**What this means for importing a mod.** A `weapon N { definition M }` key -
"slot N uses stock definition M" - was written and thrown away the same day:
GE-X edits the definitions themselves (models, ammo, functions, positions,
text ids, flags; `invitem_falcon2` is gutted to a placeholder and
`invitem_falcon2silencer` becomes a Maian SMG), so pointing at stock ones
would give the wrong guns everywhere. The port equivalent of a mod's weapon
set is the whole definition graph - `struct weapon` and, through it, the
functions by type, ammos, aim settings, gun command lists (with their
`include`/`random` pointers), gunviscmds, part visibility, noise and recoil
settings, vibration arrays - read out of the mod's data segment and rebuilt
in the port's own layout. That is the next piece of work.
