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
stored choice quietly displacing them would be a bug nobody could see. The list
holds 64 (`MOD_MAX_MODS`); the archive trial's 44 overflowed the old 32.

**An archive in those places is unpacked, and a console patch is imported**
(2026-09-04): `modListPrepareDir()` in `mod.c` runs before each scan. It unpacks
any `.zip`, `.7z`, `.pk3` or `.rar` in `mods/` (and any `mod*.zip` beside the
executable) into a directory of the archive's name through `archiveExtract()`,
the texture packs' reader in `archive.c`, hoisting a wrapper folder
(`fsRename()` of the sole inner folder over the outer). RAR is RARLAB's own
unrar source under `port/src/external/unrar`, compiled as its library build
(`RARDLL`, the makefile's object list plus `isnt` and `motw` for Windows,
`powrprof` linked there; its capitalised Windows includes were lowercased for
mingw). Two of the archive's mods are RARs, one with a RAR nested inside. Then any
`.xdelta`, `.bps` or `.ips` it finds - at the top, or inside an unpacked folder
that is not itself a mod, three levels down, nested archives unpacked on the
way - goes through `modImportPatch()` into `mods/<patch name>/`, which is then
an ordinary mod directory. So GE-X's own download zip (patch, readmes, an
emulator texture pack) and the whole 34-mod archive both drop in as one file.
Everything is done once: an archive is skipped while its directory exists, a
patch while its directory holds an `IMPORT.txt` (written on failure too, so a
patch for another ROM is not retried every boot). Deleting the directory has it
done again. This runs at boot as well (`modListApplySelection()`), before
`romdataInit()`, so the importer loads the stock ROM itself.

**The in-game importer is `port/src/modimport.c`**, a port of `tools/importmod`
step for step, with `port/src/rompatch.c` decoding the patch: VCDIFF for
xdelta (RFC 3284 plus xdelta3's app header and per-window adler32; no patch in
the archive uses secondary compression or a custom code table, and those are
refused by name), BPS and IPS. Verified against xdelta3 on every patch in the
archive (40 applicable ones byte-identical; the other five need the Japanese
ROM or stack on another patch) and against the Python tool on GE-X (files,
segs, incompatible, unlocated and the modconfig block all identical). The
data symbol table it needs (`g_Weapons` and friends with their sizes, the five
`mp_get_num_*` function ranges) is embedded for ntsc-final only; another build
gets the segment without the block, as the tool does without a datasym. The
port's declared segment table comes from `romdataGetSegmentInfo()`, which
reads a copy taken before `romdataInitSegment()` rewrites the live one.
**Keep the two importers in step**: the Python one runs where there is no
game, and its `IMPORT.txt` is what the game's gets compared against.

Three things to know about the import. xdelta3 declares a window's source
segment by its window size and lets it run past the end of the file (Mario
Characters' Peach), so the decoder keeps the declared length for address
arithmetic and bounds reads by what exists. 45 patches at once take under a
minute at boot with nothing on screen but the log; a single mod is a second
or two. And **a patch that does not apply to the stock ROM is tried on top of
each patch beside it that did** - GE Gun Name Display's "PD Names" is a patch
against its own first patch. `modImportPatch()` returns `MODIMPORT_NEEDS_BASE`
for a checksum mismatch (xdelta and BPS carry one; an IPS cannot tell), and
`modListPrepareDir()`'s second pass retries with a `basePatchPath`, which is
applied to the stock ROM first; the result is still diffed against stock, so
the mod directory carries both patches' changes. The tool's earlier
`pd_names` directory had missed the second patch's own change to
`UsetupdishZ`; the in-game import's ROM matches xdelta3's stacked result.

**The sample tables are placed by the samples a mod kept** (2026-09-04,
after GE-X's sound came out as noise). `sfxtbl` and `seqtbl` hold nothing
but sample data, so nothing in them says where they start; the first method
measured each back from the segment after it by the bytes its `.ctl`
references, which is exact for stock and wrong for GE-X, whose bank file
drops waves and leaves 81KB of their data at the table's end - so the start
landed 81KB late and every offset in the table pointed into the wrong
sample. `tbl_start_by_samples()` / `tblStartBySamples()` instead search the
patched ROM for the first 48 bytes of each stock wave; a hit votes for
`hit - base` of every mod wave of the same length, and the true start wins
by hundreds (638 for GE-X's sfx, 92 for its music, the runner-up under a
third of that). Both tables turned out to sit at their stock offsets with
their stock sizes. The check that proves it: 642 of GE-X's 793 sfx waves are
byte-identical to a stock sample at their offset in the extracted table. The
measure-back stays as the fallback for a mod that kept no stock samples.
`IMPORT.txt` now opens with `importer: N`, and a directory whose report is
older is imported again on the next start (`MODIMPORT_VERSION`), so a
tester's GE-X from the first build fixes itself.

**A mod's emulator texture pack comes with it.** `modListAdoptTextureCaches()`
copies any `.htc` found beside an imported patch (two folders down) into the
mod's `textures/`, where `texpack.c` reads it - see "Emulator cache files" in
[texture-packs.md](texture-packs.md). It needs `Mod.LoadTextures` on ("Use Texture Packs" on the
Texture Packs page), which the log says when it copies one.

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
2. ~~The heads and bodies~~ - done, see "Heads, bodies and the model
   validator" below.
3. `g_Stages` (44 words) and `g_StageTracks` (48): stage blocks already
   cover some of this by hand; the block could be generated. (`g_MpArenas`
   is done - see "The arena list" below.)
4. The TV screen command lists (`g_TvCmdlist*`, ~600 words): u32 arrays
   with the odd pointer, importable as blobs.
5. The remaining code changes are then the behaviours GE-X keyed on its new
   weapon numbers, which is what [weapons.md](weapons.md) is about - and with
   the definitions imported, `modcodediff`'s constant swaps (`li at,26` ->
   `li at,2`) could be turned into `weapon` blocks mechanically, since the
   port's flag for each stock site is known.

## The mod archive trial (2026-09-04)

`../Perfect Dark mods.zip` holds 34 console mods, 48 xdelta patches between
them; every one went through `tools/importmod` and then a headless boot of
every stage whose setup it changed (`--boot-stage N`, `--mpsims 1` for the
Combat Simulator ones). What that trial changed in the importer:

- **Emptied files are not overrides.** Mods zero the files they do not need -
  the 408 non-English language files, always - to make room, and the importer
  was writing each as a 0-byte `files/` entry and counting it as "changed".
  The port ignores an empty override (`romdataFileLoad()` wants a size above
  0), so they are now skipped and reported once as "emptied in the mod".
- **A ROM without a file name table** (GE Gun Name Display, 2009: zeros where
  the table was) uses the stock ROM's names, since the game never reads the
  table and the ids still mean the same files. Its second patch ("PD Names")
  applies on top of the first, so it goes in as `--patched-rom`.
- **Segments are located by following the mod's code first.** The lib segment
  (`LIB_OFS` 0x3050) forms the ROM offsets of the audio banks, sequences and
  copyright in lui/addiu pairs; the game binary forms `texturesdata`, and the
  `textureslist` start *and* end (texinit.c measures the list between them).
  Mod tools rewrite those numbers, so `locate_by_code()` reads them back the
  way the data segment tables are followed. The structural search had taken
  the file *name* table for the texture table in Mr. X Stalker, Kakariko
  Village, Spooky Dark and the Mario Characters (ascending offsets, so it
  looked right), shipping a 2.7 MB "list" that put garbage at every texture
  index and crashed `texAlignIndices()` at boot. Those mods lay the list at
  2-byte alignment with junk in the pointer words, which is why the finder
  now also demands the zero pointer word and why the code is asked first.
  A code reference that still says the stock offset while the stock bytes are
  no longer there is a pointer the tool did not update (the Japanese fonts,
  which the NTSC game never draws) and is ignored.
- **A segment placed only against a neighbour, somewhere new, without the
  stock bytes** is parked in `segs.unlocated/` like an assumed one. Suburb,
  the Mario mods, Facility and Car Park drop the Japanese fonts outright;
  what sat where adjacency put them was file data.
- **Stub backgrounds are incompatible files.** A mod that removes a stage
  leaves a 124-byte primary section in its `bg_*.seg` (Aviation Trilogy's Air
  Force One, Chicago, Skedar and Attack Ship; the Mario mods' ark/mp7/mp8),
  and `preprocessBgSection1()` fatals on it ("overflow when trying to
  preprocess a bg file, size 144 newsize 164"). `check_bg()` sends them to
  `files.incompatible/`, so the port keeps the stock stage.

And in the port: `setupCreateProps()` dereferenced a hovercar's or chopper's
prop without checking that `setupCreateObject()` made one. The Weather Mod's
Temple setup carries a hovercar (its rain emitter, placed by its own code) on a
pad with no room, so no prop; the port now skips it instead of crashing.

A crash in a headless run does not exit: the crash handler forks a helper and
waits on it, so `timeout` alone leaves the game hanging. `timeout -k 5` is
what to run under.

**What the trial found, mod by mod.** The imported directories are in
`build/mods/` (44 of them, 307 MB; the names are the patch names) and the
port's Load Mods page lists them. Headless results, `--boot-stage` into each
stage whose setup the mod changed:

- Boot and run their stages, files loading from the mod: 2X Weapons No
  Reloads, All Solos in Multi (14 solo stages as arenas), both Weather
  editions (16 each; the Temple needed the hovercar guard), both Aviation
  Trilogy editions (15 each once the stub backgrounds were parked), Chicago
  Restaurant (custom map), G5 Car Park, dataDyne Facility, dataDyne Compound,
  dataDyne Warehouse Cessation, Deep Sea 2X teleports, G5 Base mission, GE
  Gun Name Display (both patches), Investigation, Mr. X Stalker (both, 21
  stages each, his own model `CmrxZ` registered as the mod's), Kakariko
  Village (7 arenas in the test-stage slots), PD Plus, CSMP/CVMP, Suburb
  (custom geometry, 36 MB ROM), Dark Corps (audio segments), PD Classics
  Enabled and PD Real Guns (data segment only: the Combat Simulator list
  comes through with every feature lock cleared; nobody has yet checked the
  eight classic guns land in the right slots).
- **Custom character models crash the port's model preprocessor**, and that
  is the open class: Mario Characters (both v1.3 patches) dies in
  `modelInitRwData()` the first time a replaced body is instantiated, in 10
  of 25 stages, and Spooky Dark's skeleton dies in `convertContent()`
  (`port/src/preprocess/filemodel.c:587`) the moment a chr fires. Mr. X's
  model is fine, so it is not "any custom model" - it is a model shape
  `filemodel.c` does not expect. (Since fixed: the importer's `check_model()`
  parks such files in `files.incompatible/`, the port's `modelCheck()`
  refuses instead of crashing, and the tables are imported - "Heads, bodies
  and the model validator" below.)
- Not mod problems: booting a Combat Simulator map as a solo mission
  (Kakariko's seven, `--boot-stage N` without `--mpsims`) crashes in
  `setupPlaceWeapon()`, and a match on a stage with no arena setup (any
  solo stage with the stock 64-byte `Ump_setup*`, mod or not - checked
  against stock Investigation) never finds a spawn and loops until killed.
  The test stages (0x1a dest, mp6/mp7/mp8) have stub backgrounds in the
  stock ROM too.
- Could not apply: the two JPN patches (Facility, Car Park) and both
  JPN-English patches want the Japanese ROM, which is not here.

**What these mods need next from the loader**, by how many want it:

1. ~~Heads and bodies plus the model validator~~: done, next section but one.
2. Weather: the Weather Mod's rain and snow are its own code driving a
   hovercar; the port has a `weather` stage key, which is the translation.

(`g_MpArenas`, which every "solo stages in multi" mod rewrites, was the first
item here and is done - next section.)

## The arena list (2026-09-04)

`mparenas ADDR COUNT` in the `datasegment` block; `importMpArenas()` in
`moddata.c` copies the mod's `g_MpArenas` whole, through `mpImportArenas()` in
`mplayer/setup.c`, and the Combat Simulator's Arena list is then exactly what
the mod's own game offered. Verified in the menu (Xvfb, driven with xdotool -
see the memory note on headless driving for the recipe): All Solos lists
"dataDyne Central ... Chicago ... Attack Ship, Skedar Ruins", and a match
started from it lands in Defection's offices; Kakariko lists "Noche cerrada,
Stormy, Bizarre" under its own "Kakariko Village" heading; Chicago Restaurant
lists the stock seventeen and nothing after "Random".

- **Nearly every mod rewrites the table in place** rather than growing it:
  All Solos, Weather, Aviation and Deep Sea keep seventeen entries and put
  solo stage numbers over the sixteen arenas, with the names as text ids into
  `LoptionsE`'s mission titles (bank 43, which the port always has) - so the
  "solo stages in multi" family needs no name file of its own. The ones that
  add arenas move the table (Kakariko: 21 at 0x80089fe0, PD Plus 19, 2X
  Weapons 18, GE-X 23) and are followed the way `g_MpWeapons` is; their
  count is the `li v0,17` in `mp_get_num_stages`, read back from the mod's
  code (`follow_immediate`), with `count_mparenas()` as the fallback.
- **An entry that does not read as one drops the whole list.** CSMP, CVMP and
  PD11GE rebuilt `struct mparena` as four bytes (stage and feature a byte
  each) - the same fields, the wrong stride - and the first entry happens to
  read plausibly. A list of one arena is a broken menu, so unlike the weapon
  tables a short read leaves the block without an `mparenas` line and the
  port keeps its own list. Those three mods therefore still get the static
  behaviour below.
- **Without an imported list, any mod dir shows the nineteen all-in-one
  arenas** (`MP_NUM_MOD_ARENAS_STATIC` in `constants.h`, named from
  `g_MpArenaModNames`), setups or not, and a match on one with no setup
  loops looking for a spawn pad. That predates this work; it is what
  `mod_allinone` relies on, and a hand-built mod dir has no way to say
  otherwise yet. An imported list replaces those entries and clears the tail,
  and `mpRegisterArena()` (`--modstages`) then appends after the mod's
  count - which is why `g_MpModArenaNames` is now indexed by name slot,
  since the count can sit below the static nineteen.
- **Names**: `mpGetArenaName()` uses `langGet()` on the entry's text id when
  the list is imported and the id is nonzero, copying the string to add the
  newline `textMeasure()` needs when the bank's string lacks one; a
  registered arena (id 0) still reads `g_MpArenaModNames`. The group
  headings ("Dark" 0-12, "Classic" 13-15, "Random" 16-) are the port's, as
  they are the ROM's: All Solos shows Felicity under "Random" on a console
  too, and Kakariko renamed "Random" to "Kakariko Village" in its `LmpmenuE`.
- The snapshot for mod swaps covers `g_MpArenas`, the count and the imported
  flag. `g_MpArenas` is sized in `data.h` now (`MP_NUM_ARENAS_STATIC +
  MAX_MODSTAGES`) so `mod.c` can take `sizeof` it.

## Heads, bodies and the model validator (2026-09-04)

The datasegment block now carries `headsandbodies`, `mpheads`, `mpbodies`,
`mpbeauheads`, `botheads`, `mpmaleheads` and `mpfemaleheads`, each located the
way the weapon tables are (stock address from the datasym, or followed through
the mod's code when moved) and counted by walking the table until an entry
stops reading as one. `moddata.c` imports them in that order:
`importHeadsAndBodies()` copies `g_HeadsAndBodies` index for index with the
file ids resolved through `segs/data.names`, and the Combat Simulator lists
(`importMpHeads()`, `importMpBodies()`, `importHeadList()`) are validated entry
by entry against the table's size and the feature range; a bad entry drops the
list and the port keeps its own. The counts live in `g_MpListCounts` so the
menus stop at the mod's count rather than the port's. The swap snapshot covers
all of it.

- **Model validator.** `check_model()` in `tools/importmod` walks a model file
  the way `convertModel()` does: the parts and texture config tables, every
  node breadth first with the rodata size the port has for its type, and each
  display list to `G_ENDDL` following `G_DL` calls and branches, refusing any
  pointer outside segment 5 or past the end of the file. Files that fail go
  to `files.incompatible/`. Spooky Dark's CMP150 fails it (its muzzle flash
  texture lives in another segment). `modelCheck()` in
  `port/src/preprocess/filemodel.c` is the same walk in C and fatals with the
  file name and the reason where `convertContent()` used to segfault.
- **Two guards in `body.c`.** A body whose model has no headspot gets no head
  attached (Mario Characters' Yoshi: head built in, "has own head" bit clear,
  and the player path passes a head modeldef regardless). A body model
  offered as a head is refused with a warning and the chr goes headless:
  `modelAttachHead()` would re-parent its root under the body's headspot, and
  the next chr wearing that model as a body would read another model's rwdata
  through it. Both come from a mod's table index that the port's code still
  reads as a stock head number.
- Every dir in `build/mods/` was re-imported with this; Mario Characters,
  Spooky Dark and Mr. X carry the block.

## GE-X's solo missions in the port (2026-09-05)

The tester's first run of GE-X's missions: Runway crashed on load, Dam loaded
with no guards and the player in a Santa suit. Four things, found by booting
every solo slot and arena headlessly with the mod mounted (`--boot-stage N`,
four at a time under `timeout -k`) and reading the crash handler's backtrace
through `addr2line`, or gdb when the handler had none (`PC=(nil)`):

- **The stage table is imported now** (`stages ADDR COUNT` in the datasegment
  block, `importStages()` in moddata.c). GE-X puts its levels in the stock
  slots and points them at its own files - Runway is Extraction's slot with
  `bg_ark` where stock has `bg_ame` - and with the port's table a slot loaded
  the stock background under the mod's setup. The ROM entry is 0x38 bytes
  (the port's struct has `alarm` and `extragunmem` after it, which stay);
  entries match by stage number and the five file ids go through the names.
  Diffing the tables anchored on an entry's known bytes is how the stride was
  found: the first attempt walked at the port's 0x3c and read junk.
- **The solo player's body and head come from the mod's code**
  (`playerbody`, `playerhead`): `follow_immediate` over
  `player_choose_body_and_head` for the stock constants 0x56 and 0x04. GE-X
  says 88 and 6 - `CpresidentZ` and `Ca51faceplateZ` by name, its Bond
  overwriting those files - and the port's default index had become GE-X's
  Santa. `MOD_PLAYER_BODY/HEAD` in player.c route the three stock sites.
- **A mod's own AI commands.** `g_CommandPointers` has empty slots (0x64,
  0xe6, 0xe7, 0x12d ...) and GE-X fills 0xe6 and 0xe7 with "if <its option
  byte> goto label" commands, three bytes each. The dispatcher called through
  the NULL pointer; now a slot with no handler is stepped over by the length
  the mod's `g_CommandLengths` gives it (`commandlengths ADDR COUNT`,
  `chraiSetModCommandLength()`), logged once, and a list stops only when no
  length is known either. Stepping over is "condition false", which is one
  of the two branches; without the mod's code that is the most the port can do.
- **The setup converter misread gas bottles and safes.** `filesetup.c` had
  `OBJTYPE_GASBOTTLE` and `OBJTYPE_SAFE` in the four-byte objective group,
  while `setupGetCmdLength()` sizes both as a full default object, so every
  object after one was read at the wrong offset - autoguns with model 0,
  monitors with model 31754, a truck whose model pointer was 1.0f. No stock
  setup has either object; GE-X's Facility and Frigate do. Found with
  `--setup-trace`, which logs each object the converter walks and warns
  when the bytes it wrote disagree with `setupGetCmdLength()`.

- **The mission list is imported** (`solostages ADDR COUNT`, `importSoloStages()`):
  `g_SoloStages` says which stage each menu slot loads and its title ids. GE-X
  keeps the stock order but sends six missions to stage ids the stock menu
  never lists - Surface 2 to 0x24, Bunker 2 to 0x25, Train to 0x23, Cradle to
  0x2b, Aztec to 0x2e, Egyptian to 0x1a - each with its own stage table
  entry. Through the stock list "Aztec" loaded stage 0x09, whose GE-X files
  are a leftover pair that do not belong together: a 460-pad pads file under
  a setup whose chrs stand on pad 865, which read garbage positions and then
  garbage rooms. That crash cost two false fixes first (a door sibling that
  was never created, which is real and stays guarded, and a roomless
  waypoint guard, also kept). Booting a slot by stage id with `--boot-stage`
  is not the same as picking it from the menu once a mod remaps the list:
  read the mod's `g_SoloStages` first.
- The 21-entry `struct solostage` is 12 bytes in the ROM: `u32 stagenum`,
  `u8 unk04`, pad, three `u16` text ids.

- **Every outfit's body and head, not just the default's** (2026-09-05, "the
  hand is white"): `playerChooseBodyAndHead()` has a body and head constant
  per outfit, and GE-X changed 47 of them. Following only the default pair
  left Runway's outfit on stock `BODY_DARK_TRENCH` (0x62), which GE-X had
  turned into its Santa - the white glove was Santa's hand, drawn correctly.
  `follow_immediates()` / the C loop in `modimport.c` now record every `li`
  in the function the mod changed as `playerconst 0xSTOCK MOD`, one per
  line, and `modDataPlayerBody/Head()` look each site's constant up
  (`MOD_BODY(x)`/`MOD_HEAD(x)` on all 45 sites). It is right because
  modcodediff classes the function as constants-only, so the instructions
  still mean what they meant. Two traps from that hour: a comma list on one
  config line breaks the block parser (it tokenises on `=`), and the whole
  datasegment block is dropped when one line is bad - a stray "invalid key"
  error in the log means nothing else in the block loaded either. A texture
  that draws white is almost never a texture problem: `--dump-texture N`
  writes the port's decode of any texture, and `--texpack-trace` shows the
  pack image that replaced one when drawn.

Also from that day: `--chr-trace` logs every AI spawn and its result, the
log gets a chr census at frame 300 and every 20s, `setupCreateProps()` reports
how many chr entries spawned, and an autogun with no model no longer
dereferences its missing prop. `IMPORT.txt` is at importer version 3, so
every earlier import is redone.

## GE-X's guard faces and the KF7's white magazine (2026-09-05)

Two reports from the same tester, with two different causes, neither of them
a texture problem in the sense of the paragraph above. What it took to find
each is written down because both hunts began in the wrong place.

- **Scrambled faces on some soldiers: the random guard heads.** A solo stage
  hands its guards heads from `g_MaleGuardHeads` / `g_FemaleGuardHeads` (and
  the team lists beside them, body.c) through `bodiesReset()` and
  `bodyChooseHead()`. Those four lists were not in the datasegment block: the
  port drew from PD's stock list, and GE-X reused the texture slots behind
  several of those heads (Graham, Duncan, Jon, Mark, Stevem ... decode as
  foliage and rubble in GE-X's table), so a guard wearing one showed that.
  The block now carries `maleguardheads`, `maleguardteamheads`,
  `femaleguardheads` and `femaleguardteamheads`; GE-X's are 25, 14, 4 and 4.
  These lists end at a -1 rather than at a count in the code, so the table
  loop in both importers has a `terminated` flag: `countIndexes()` stops at
  the first entry that is not a head index and whatever it counted is the
  list. `importGuardHeads()` in moddata.c reads every entry before writing
  any, writes the terminator, and sets the count - `bodiesInit()` counts
  again later, so either order works. The C importer resolves symbols from its
  own `dataSyms[]` table, not the datasym file: a new list needs its address
  and size added there, or the import says nothing at all (the first attempt
  wrote no line and no note). Importer version 5.
- **The KF7's white magazine: an IA16 palette read backwards.** The
  magazine is texture 0x3f6, a 64x1 `IA16_CI8` strip of 49 opaque greys
  (`1dff 1fff ...`). Both `palette_to_rgba32()` in fast3d/gfx_pc.cpp and
  `texpackPaletteEntryToRgba()` in texpack.c took the low byte as intensity
  and the high byte as alpha; an IA16 entry is intensity high, alpha low,
  like an IA16 texel, and the TLUT is byte-swapped to that on load. So every
  dark opaque grey became a nearly transparent white. Stock PD has seven such
  textures (0x1e7 on the dataDyne fan roof and Skedar plinths, 0x1e8 on the
  hovercars, 0xb25 on the submarine) which drew wrong all along; GE-X has 56,
  on the KF7, the rocket launchers, the minigun and many props. The
  `--dump-textures` PNG of such a texture was wrong the same way, which is
  why the dump "confirmed" the strip as faint - the game's own decode of a
  texture is only as trustworthy as the decoder it shares with the renderer.
- **Getting a gun into the player's hands headlessly.** Combat Simulator
  spawns unarmed; `Mod.StartArmed=1` in the scratch pd.ini hands out slot 1,
  and `--mp-weapons 6,5,20,20,47,4` (Combat Simulator weapon indexes as
  `--moddata-trace` numbers them) fills the slots, `--mp-weaponset N` picks a
  whole set. Both apply only with `--mpsims`. A real mouse press (xdotool
  `mousedown 1`) fires and `r` reloads, which is what brought the magazine
  into frame: the first-person KF7 keeps it below the screen otherwise.
- **The animated texture numbers follow the mod's code too.** `texLoadFromGdl()`
  picks the rivers, the ocean, the teleport and the power juice by texture
  number, and GE-X changed three of those constants (0x6cb and 0x6e2 to
  0x1c7, 0x90f to 0xc90). The constant-following loop that made `playerconst`
  now runs over a table of functions and writes `texconst 0xSTOCK MOD` lines
  for `tex_load_from_gdl` as well; `modDataTexNum()` maps each site's
  stock number and tex.c compares through `MOD_TEX(x)` at all nine sites.
  Two stock numbers mapping to the same mod number is what the mod's code
  does, and the sites test in the same order, so the last match wins there
  as here. Importer version 6. GE-X's face textures 0x561/0x562/0x56a and
  0x58d/0x58e decode as dithered photo faces and split faces; they are the
  mod's own data and the console reads them the same way.
- **The palette fix broke every font (same day).** The text palettes are
  `u16` C arrays in game_1531a0.c, in host byte order, and `gfx_dp_load_tlut()`
  byte-swaps every TLUT entry as if it came from the ROM. The swapped read
  and the swapped load had cancelled for the fonts, so putting the read
  right left each glyph a filled block. The arrays are now written with
  `PD_BE16()` so they sit in memory like a ROM palette; `texpackGlyphIndexBuild()`
  hashes them as bytes. Any in-code TLUT a future change adds needs the same.

## The stage environments: sky, fog and clouds (2026-09-05)

"The starting room of Runway is black and the sky is black with pixels." A
stage's sky colour, fog, clouds, water and suns come from two tables in env.c,
`g_FogEnvironments` (44 bytes an entry, s16 stage) and `g_NoFogEnvironments`
(56 bytes, s32 stage), each walked to a 0 stage by `envChooseAndApply()`.
Neither was imported, so GE-X's Runway, in Extraction's slot, got Extraction's
entry: black sky, no clouds. GE-X grows the fog table to 55 entries over the
no-fog table's old place and moves that one (23 entries) to 0x80081b04,
which `followTable()` finds from the two code references; its Runway entry
is a fog one, sky 0x103040 with the Area 51 sun.

The block carries `fogenvs` and `nofogenvs`; `importEnvs()` in moddata.c
rebuilds both in the port's layout (the ROM's differs by the suns pointer,
which `importSuns()` resolves into the segment) with a zeroed terminator,
and `envSetTables()` in env.c points the chooser at them - the port's arrays
are too small for GE-X's fog table, so they are replaced rather than
overwritten. `--moddata-trace` lists every entry. Importer version 7.

## The camera-pinned rooms (2026-09-05)

"The starting area of Runway is missing its wall and ground textures, and
there is a black bar across the top of the sky." Neither was a texture:
`roomPopulateMtx()` (room.c) pins a few rooms to the camera - the moon in
Defection, Extraction and Maian SOS's room 1, the Skedar ruins' room 2, the
Attack Ship's backdrop - by comparing the stage against `g_Stages[index].id`
and the room against a literal. GE-X's Runway sits in Extraction's slot, so
its room 1, the start area, was drawn at the camera's offset: walls and
floor elsewhere, and the room's dark ceiling across the top of the view.
GE-X's own code points every one of those checks at its stage table's
index 0 (never a played stage) except one, moved to index 26, whose id is
0x2e in GE-X's table.

The constant-following loop now has a `stages` kind that reads the `lh
rt,OFF(v1)` loads of `g_Stages[OFF / 0x38].id`, and writes them as stage
ids: `roomstage STOCKINDEX MODID`, the mod id read from the mod's own stage
table at the mod's index (its table may be in another order, so an index is
not carried across). The room literals go through the `li` kind as
`roomnum`. room.c compares through `ROOMSTAGE(idx)` and `MOD_ROOM(x)`,
which `modDataRoomStage()` and `modDataRoomNum()` answer. Importer
version 8. Two suns now show on the Runway, which is GE-X's own
environment entry (one sun with a lens flare).

Seen on the way: `--boot-stage 0x24` on stock stops with "overflow when
trying to preprocess a bg file"; 0x24 is not a playable stage.

**The band along the top of the sky (same day, later).** The tester still
saw it after the room fix, and a timelapse of the Runway's intro flyover
(`import` every two seconds from six seconds in) reproduced it headlessly.
The Runway's fog entry has `clouds_height` 30, a value no playable stock
stage has. `skyGetWorldPosFromScreenPos()` casts the corner rays from that
many rows below the screen top, and the N64 path draws what it finds that
many rows higher again (`skyConvertVertex()`), so the horizon sits higher on
screen than the camera's. The port's `#else` branches in `skyRender()` draw
the sky and water planes as 3D triangles and never applied that shift, so a
band that high went unpainted along the top. `skyPitchForCloudHeight()`
turns the branch's matrix up through `atan(clouds_height * c_scaley)` (a
row is `c_scaley` of view-space y at unit depth, `cam0f0b4c3c()`), with the
rotation written out from the ratio because the game's own `atan2f` has its
own argument convention. Positive is up; checked on the flyover frame.

**"Blocky fog that culls very closely", same report.** `--no-fog` (a debug
switch in `envStartFog()`) showed the navy wall at the hangar opening is fog
on the far cliff, not rooms left undrawn. GE-X's Runway fog is 996-1000 of
the 10..15000 z range: fog from about 21 m, full at 150 m, as heavy as the
original game's Runway. The fog is per vertex on both machines, so large
triangles shade in blocks there too. Not changed; if a console capture shows
the runway from inside the hangar, the port's z range is the next suspect.
