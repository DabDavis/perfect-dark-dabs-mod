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
set is the whole definition graph, and that is what the next section does.

## Importing the data segment: where this stands (2026-09-04)

**Done and working.** GE-X boots into a Combat Simulator match with its own
arsenal in the weapon table (knife at 2, PP7 at 3, DD44 at 5, Klobb at 6, KF7 at 7, ZMG at
8, D5K at 9, AR33 at 12, RC-P90 at 13, shotgun at 14, Cougar Magnum at 19,
Golden Gun at 20 ...), the shotgun having inherited its pump-action flags.

- `tools/importmod` writes the mod's inflated data segment to `segs/data`,
  its file names one per id to `segs/data.names`, and a `datasegment` block
  at the top of `modconfig.txt` saying where the tables are. It finds each
  table by **following the mod's code**: the lui/addiu pairs that formed the
  stock address (from `pd.<romid>.datasym`) form the new one in a
  patched-in-place mod. GE-X moved `g_MpWeapons` to 0x800877c0 (36 entries,
  the count read from the `li v0,N` in `mp_get_num_mpweapons`) and put its
  14 weapon sets at 0x800873d0, over the old list's tail. Only references
  landing *inside* a table count: the word before `g_MpWeaponSets` is the end
  of `g_MpWeapons`, and a loop over that table holds a pointer to it, which
  is how the first version was 0x20 off. A rebuilt data segment (performance
  mod) is written out but gets no block - its tables cannot be located yet.
- `port/src/moddata.c` reads the block, loads the segment and walks the
  graph from every `g_Weapons` slot: `struct weapon`, the functions by type
  (sizes verified against symbol spacing: shootsingle 0x40, shootauto 0x54,
  projectile 0x64, throw 0x24, melee 0x4c, special 0x20, device 0x18), ammo
  (0x14), aim settings (0x20, two bitfields at the *top* of the word on
  MIPS), gun command lists (8-byte entries to `GUNCMD_END`, `include` and
  `random` carrying pointers), gunviscmds (10 bytes each to type 0), part
  visibility (2 bytes to part 255), noise and recoil settings, the 12-float
  vibration arrays. Every object is converted once, so sharing survives. It
  also copies `g_ModelStates` (fileid and scale), `g_MpWeapons` and
  `g_MpWeaponSets`, each validated before use - a table the mod moved leaves
  junk at the stock address, and importing that once put a weapon set of
  nonsense into a match (no gun, no pickups, the first symptom seen).
- File ids go through `segs/data.names` to the port's slot for that name;
  a name the port lacks (GE-X's `GplatinumZ`, `GrudolphwppkZ`) is registered
  as the mod's own with `romdataRegisterModFile()`. `flags2`,
  `unequippedreloadindex` and `pickupsound` are the port's fields, so a
  definition at a stock definition's address inherits them from the *stock*
  pointer table as it was before the import began - not from `g_Weapons[k]`
  mid-loop, which slot 72 once inherited the knife's flags from.
- `--moddata-trace` logs one line per slot and Combat Simulator entry.
- The port's `g_MpWeapons` is **not laid out like the ROM's**: it inserts
  two scanners and the classic guns in the middle and keeps the shield and
  the "disabled" entry at fixed indexes (`MPWEAPON_SHIELD` 0x2f,
  `MPWEAPON_DISABLED` 0x30) that `mpconfigs.c` names directly. A mod's
  entries go in order into the slots before the shield, its own shield and
  disabled entries are skipped, and the slots left before the shield are set
  to `WEAPON_NONE` behind feature 79, which nothing unlocks, so PD's extras
  do not show up naming GE-X's guns. `g_MpWeaponSets[12]` is fixed size and
  GE-X has 14: the last two are dropped with a warning. Growing it is the
  fixed-size-table class of change (see CLAUDE.md).
- **How it was verified, and how not to.** Feed the converter the stock
  ROM's own segment (a mod dir with just `segs/data`, `segs/data.names` and
  the block at the stock addresses; `--moddata-trace` then compares every
  converted field against the port's own struct at the same address and logs
  each difference). It is a no-op except for flag bits this fork itself added
  to stock (`FUNCFLAG_PROXIMITYMINE` on the Dragon's mine mode, a device bit
  on the scanners), which no ROM has - so those are carried across as a
  delta over the ROM's stock flags. Screenshots of a headless `--boot-stage
  0x32` match are **not** a check: the spawn pad is random, two identical
  stock runs look nothing alike, and an hour went into "the gun draws
  exploded" that was the Temple's own pillars from a different pad.
- The mod swap snapshot (`modTablesSnapshot`) now covers `g_Weapons`,
  `g_ModelStates`, `g_MpWeapons` and `g_MpWeaponSets` too; moot for a mod
  with `segs/` (restart rule) but right.

**Not done yet, in the order it probably wants doing:**

1. Play it, with a person at the keyboard: the match boots and the trace
   reads right, but nobody has yet held a GE-X gun in the port and fired it.
   `./build/pd.x86_64 --moddir build/mod_gex_data`, Combat Simulator, any
   arena. `#warning: memory pool ... is full` lines appear in that run with
   or without the data import - GE-X's resized segments fill the stage pool
   - and are a separate problem.
2. The heads and bodies: `g_HeadsAndBodies` (210 words changed), `g_MpHeads`,
   `g_MpBodies`, `g_BotHeads`, the guard head lists in `body.c`. Same shape
   as the model states (file ids through names) once
   [chrs-and-memory.md](chrs-and-memory.md) has been read.
3. `g_Stages` (44 words), `g_MpArenas` (30), `g_StageTracks` (48): stage
   blocks already cover some of this by hand; the block could be generated.
4. The TV screen command lists (`g_TvCmdlist*`, ~600 words): u32 arrays
   with the odd pointer, importable as blobs.
5. The remaining code changes are then the behaviours GE-X keyed on its new
   weapon numbers, which is what [weapons.md](weapons.md) is about - and with
   the definitions imported, `modcodediff`'s constant swaps (`li at,26` ->
   `li at,2`) could be turned into `weapon` blocks mechanically, since the
   port's flag for each stock site is known.
