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

**A folder that is not a mod but holds mods is looked into** (2026-09-07):
the All in One bundle's zip is a whole Windows distribution - `pd.exe`, the
DLLs, `data/` and five `mod_*` folders beside them - so after the hoist the
unpacked directory had nothing the list recognises, and Load Mods on the
tester's box showed only GE-X. `modListAddAt()` descends such a directory to
`MOD_UNPACK_DEPTH` and lists every mod it finds under its own folder's name
(`mod_allinone`, `mod_gex`, ...); a name already listed, say the loose
`mod_allinone` beside the executable, wins over a nested copy.

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

**Every block's keys are set through one function** (2026-09-07): the
parsers in mod.c call `modWeaponSetKey()`, `modDamageSetKey()`,
`modStageSetKey()`, `modDataSpecSetTable()` and the rest (mod.h, "the
settings a modconfig block can make"), which hold a key's name and range
once; the `weaponfunc` block and the unlocks each had a private copy of
their key table before. Adding a key is one function, and the parser block
that names it. `modConfigParse()` is `modConfigLoad()` split from its file
read, for text that is already in memory.

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
3. ~~`g_Stages` (44 words) and `g_StageTracks` (48)~~ - done: `g_Stages`
   in "GE-X's solo missions in the port", `g_StageTracks` and `g_MpTracks`
   in "Stage music and the Combat Simulator's tracks" below. (`g_MpArenas`
   is done - see "The arena list" below.)
4. The TV screen command lists (`g_TvCmdlist*`, ~600 words): u32 arrays
   with the odd pointer, importable as blobs. Also still stock:
   `g_StageAllocations8Mb` (GE-X moves ten stage ids between entries; the
   port only consults it for two or more players), `g_GuardQuipBank`,
   `g_BotProfiles`, `g_HovTypes`, `g_Skeletons` - all changed by GE-X,
   none looked at yet. The tables that are imported are listed under
   "The rest of the data segment" below.
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
2. ~~Weather~~: done, "The weather" at the end. (The Weather Mod's own rain
   and snow are still its code driving a hovercar; what the port now follows
   is the game's weather system: which stages have it, in which rooms.)

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

**"Blocky fog that culls very closely", same report.** Two things, found
in that order with `--no-fog` (a debug switch in `envStartFog()` that draws
a fogged stage without its fog) and a one-frame trace of the renderer's
display list:

- **The pinned room, again, in `bgRenderScene()`.** bg.c has its own copy
  of `roomPopulateMtx()`'s stage and room tests: for those stages it draws
  the pinned room *first*, with fog stopped, and a star field for six stage
  ids written as literals. On GE-X's Runway that was the start area, drawn
  through its room list's baked fog blender (`G_RM_FOG_SHADE_A`) with the
  fog geometry bit clear - a full-fog blend, since the renderer's factor for
  a vertex without the bit is the fog colour's alpha, 0xff - and the fogged
  pass behind it lost the depth test. The tester's "stars" were the star
  field. The constant follower now runs over `bg_render_scene` too: its
  `g_Stages[index].id` loads join the `roomstage` list (rows sharing a key
  share one list, so a stage found in both functions comes out once), and
  its `li` stage ids go out as `bgstage`; bg.c compares through
  `ROOMSTAGE()`, `MOD_ROOM()` and `MOD_BGSTAGE()`. GE-X points all six star
  field stages at its stage 9 or 0x2e. Importer version 9.
- **Fog is now evaluated per fragment.** The renderer used to compute the
  fog factor per vertex, before the GPU clipped the triangle, and a vertex
  behind the camera got a factor of 0 that was then lerped in clip space
  across the near plane - a room polygon reaching from behind the camera to
  the far wall came out fogged wrong along most of its length ("blocky").
  The N64 clips first and evaluates the fog line at the new vertices.
  `aFog` now carries the fog colour and the RSP's multiplier, `aFogOffset`
  the offset, and the fragment shader evaluates `z/w * mul + offset` at its
  own depth (`vFogZW` is the clip-space z and w, interpolated perspective
  correct, so their ratio is the true depth). A vertex loaded without the
  fog geometry bit gets multiplier 0 and the fog colour's alpha as offset,
  the constant factor it had before. The Runway's fog itself is the mod's:
  996-1000 of a 10..15000 z range, from about 21 m to full at 150 m.

## Stage music and the Combat Simulator's tracks (2026-09-05)

`stagetracks ADDR COUNT` and `mptracks ADDR COUNT` in the `datasegment`
block; `importStageTracks()` and `importMpTracks()` in `moddata.c`. Found
after "the music for levels is not correct for GE-X": every GE-X mission
played a random Combat Simulator tune.

- **`g_StageTracks` is looked up by stage id and falls through to
  `mpChooseTrack()`** when the id is not in it. GE-X keeps the table at its
  stock address with its own 24 stage ids in it (0x30, 0x33, 0x22 ...), none
  of which the port's table has, so every mission took the fall-through.
  The table is now a pointer (`stageSetTracks()` in `stagemusic.c`, NULL for
  the port's own) and the import hands it a 0-terminated copy; the mod swap
  restore resets the pointer before copying the snapshot back.
- **`g_MpTracks` is what a match plays and what the music menu lists.** GE-X
  rewrites all of it (its sequences, durations, two names in its own bank,
  everything unlocked) and has 44 tracks to the stock 42, so it starts the
  list 12 bytes earlier, which the code references show (8 agree). The count
  is the immediate `mp_get_num_unlocked_tracks` loads (0x7f18c200), the way
  `mp_get_num_stages` gives the arenas. The port's array is `MP_MAX_TRACKS`
  (48: the save's `multipletracknums` has 48 bits) with `g_MpNumTracks` in
  place of `ARRAYCOUNT`.
- **A byte compare at the stock offset is not a check that a table is
  unchanged.** The volume table (`g_SeqVolumes`, 124 entries) really is
  untouched by GE-X, but the same compare on `g_MpTracks` read the list two
  entries in and looked merely rewritten; only following the code showed it
  moved. Ask the importer where a table is before reading it.
- **How to see what plays, headlessly:** `lib/music.c` logs
  `music: sequence N starts as track type T` (1 is the primary) as each
  track starts, and `--moddata-trace` prints both tables as imported. A
  stage's own AI script can override the table: GE-X's hub (0x26) starts 108
  and 89, its intro sting and the menu music, not its table entry - so test
  a mission. GE-X's first two missions, Dam (0x30) and Facility (0x33), are
  unfinished and start no music at all; Runway (0x22) is the first that
  works, and starts sequence 63, its table's main theme. A match on arena 0
  starts a track from the imported list.
- `IMPORT.txt` `importer: 10`; a GE-X directory from 9 re-imports itself.

## The star field, and GE-X's Aztec crash (2026-09-05)

`bgRenderScene()` draws a star field for six stages, tested by stage id
through `modDataBgStage()` - the `bgstage` lines, from the constants GE-X's
scene code changed (Defection 0x30 -> 0x2e, Extraction and the Attack Ship
-> 9). GE-X's Aztec is stage 0x2e, so the port drew Defection's stars there.
But `lvReset()`'s switch that calls `starsReset()`, and the tests inside
`stars.c`, used the raw ids, so 0x2e never allocated a table: after Runway
(0x22, which does), the pointer was stale in a wiped stage pool and
`starsRender()` crashed at `stars.c:310` reading it. Booting straight into
Aztec never showed it (the pointer was NULL, and `starsRender()` returns on
NULL); the player's backtrace did. Now `lv.c` and `stars.c` go through the
same mapping, and `starsClear()` drops the table on every `lvReset()`.

`modcodediff` on GE-X shows exactly this: `stars_reset` and `stars_render`
have the same three constants changed as `bg_render_scene`, and `lv_reset`'s
switch is a jump table in lv.c's data that GE-X patched (17 words). When a
stage test in one function is mapped, find the others that test the same
thing; they are compiled apart and change apart.

## The rest of the data segment (2026-09-05)

Found by diffing GE-X's data segment against stock per `datasym` symbol with
pointer-shaped words masked (a struct with a string or function pointer
differs wherever the layout shifted; only the other words mean anything).
Seven more tables in the `datasegment` block, all edited in place through
count helpers beside the arrays (`bgunGetNumAmmoTypes()` and the like), so
the sizes stay where the arrays are:

- `ammotypes` (33; `AMMOTYPE_ECM_MINE` is the last, and the symbol spacing
  runs 48 bytes past the table - both importers pin the count),
  `explosiontypes` (one s8 a model), `autoswitchprimary` / `autoswitchsecondary`
  (weapon numbers, best first; the mod's shorter list is padded with unarmed,
  which every player has, since the reader runs to `ARRAYCOUNT`),
  `botweaponprefs` (`g_AibotWeaponPreferences`, `g_BotWeaponConfigs` in the
  datasym; 16 bytes, two bit fields read out by hand since IDO packs from
  the top of the halfword), `hudmsgtypes` (the two font pointers stay the
  port's), `globalailists`.
- **Global AI lists are `{list, id}` pairs and the lists sit one after
  another**, the table after them, so a list's length is the distance to the
  next higher pointer (or the table). Same 46 ids in GE-X, seven rewritten
  (alerted, bored, buddy init and warp, objective-failed). The copy replaces
  the port's list of the same id; a mod's own id has no slot and is reported.
- The per-symbol diff is `python3 - <<EOF` in the session, not a tool; if it
  is wanted again, the two data segments are `build/mod_stockdata/segs/data`
  and the mod's, base 0x80059fe0, symbols from `tools/pd.ntsc-final.datasym`.
- `IMPORT.txt` `importer: 11`.

## The weather (2026-09-05)

"weather_is_room_weatherproof looks like the biggest": 179 of GE-X's rewritten
words are that one function, and `weather_reset` beside it. Both decide by
stage index - is there weather here, which rooms keep it out, how hard the
wind blows - and GE-X did not renumber the stock compares, it wrote new chains
of them (Surface, Surface 2, Bunker, Runway and stage 0x2d instead of Chicago,
Air Base, the G5 Building and the Crash Site), with the freed words reused as
code caves for a jump from `bwalk_update_horizontal` and for `weather_reset`'s
own new test. Constant-following reads nothing from that.

- **The code is read by running it.** `emulate()` in `tools/importmod` and
  `emuRun()` in `port/src/modimport.c` are a toy MIPS: integer ops, branches,
  single-float moves and compares, a scratch memory where a load of
  `g_StageIndex` reads the index asked about and a `jal` is not followed but
  hands back a fake pointer and is counted. `weather_reset` run per stage
  index says whether weather is allocated (a call was made) and the wind
  speed (the float it stored at +0x14 of what the first call returned);
  `weather_is_room_weatherproof` run per room, 1 to 1023, gives the rooms.
  The four special cases stock keys on an index - no weather below z -2000
  and the fixed wind (Air Base, in `weather_render` and `weather_tick_rain`),
  cutscenes only (the G5 Building), the narrower particle bounds (the Crash
  Site, in `weather_allocate_particles`) - are still `li` sites and follow
  the way the outfit constants do.
- **Run on stock, the reading is the port's own table** (`g_WeatherConfig` in
  weather.c: Chicago's 53 rooms, Air Base's 19 with the fixed wind and zmax,
  the Crash Site's 44 at wind 10 and ±500, the G5 Building cutscenes-only),
  which is the check that the machine reads right.
- **What comes out is the port's own `stage N { weather { } }` block**, one
  per stage whose reading differs from stock's at that index, between
  `# importer: weather begin` and `end` lines after the datasegment block
  (both importers cut the region out again before rewriting it). Every value
  is written and the room list starts with `clear`, because the port merges
  a block into its existing entry for that stage number and GE-X's Bunker
  sits at Chicago's number: without `clear`, Chicago's rooms would have
  stayed under Bunker's. The port's rooms parser appends by design (a
  user's block can add to stock); `clear` is the word for replacing.
- Stages the mod took the weather away from (GE-X: indexes 8, 10, 19) keep
  the port's entries. A weatherdata is allocated there but its type stays -1
  until a stage script says rain or snow, and those scripts come with the
  setup files, so nothing shows; the report says which.
- `WEATHERCFG_MAX_STAGES` went from 16 to 64: PDE_E2_LZT (a Combat Simulator
  weather mod in the archive) made `weather_reset` unconditional and gets a
  block for every one of its 61 stages, and the 17th used to fail the whole
  modconfig. A block with no room left now skips itself with a warning.
  `weatherTick()` in the port had kept the stock `g_StageIndex` tests for the
  Air Base and G5 special cases while `weatherRender()` read the config; it
  reads the config now too.
- `weatherReset()` logs `weather: stage 0xNN has weather except in / only in
  N rooms, wind W`, so a headless boot shows what applied. GE-X Surface
  (0x2c) booted with a screenshot of snow falling; Bunker (0x1d) and Runway
  (0x22) took theirs too. Across the archive, five more mods got blocks
  (G5 Car Park's Chicago, PD Kakariko, both Weather Mod editions, the two
  PDE_E2_LZT patches), none reported the machine failing.
- `IMPORT.txt` `importer: 12`. The two importers' weather lines and blocks
  are identical for GE-X; the pre-existing difference between them is the
  order of the `playerconst`/`bgstage`/`roomstage` lines (the tool sorts,
  the game writes in code order), which is not a difference in content.

## The shield flash colour, and a function that tests its caller (2026-09-07)

`shieldhit_health_to_rgb` was the largest rewritten function left after the
weather. It turns shield left into a colour, dark green at none through to
orange at full, and the game asks in two places: `chrRenderShieldComponent()`
for the hexagons on a chr or object, and `playerRenderShield()` for the
player's own flash over the screen. What GE-X did is 32 words that read
nothing from constant-following: `lui at,0x7f0c; ori at,0x4b0; beq at,ra` -
**the function tests its own return address**, and answers white (255,255,255)
to `player_render_shield` and dark grey (64,64,64) to everyone else, whatever
the shield. **Five mods in the archive carry that identical patch** (GE-X,
DEEP SEA X, pdrealguns, both Weather Mod editions), with their own colours in
it - the others give the player (96,96,0) - so it is a hack that travels.

- **Read by running it, per call site.** `emulate()` / `emuRunArgs()` took
  the extra arguments this needed: `args`/a1-a3 (the three result pointers,
  at `SHIELD_SCRATCH`), `f12` (the shield, as a float), and `ra` - the run
  ends when the function returns *there* rather than to the sentinel, so a
  function that branches on its caller can be asked about each caller in
  turn. The sites are found by the `jal` and named by the function each sits
  in (`shield_call_sites()` / `shieldCallSites()`); a mod with more or fewer
  than one in each is reported and left alone. 2049 samples over 0..8 shield,
  1/256 apart, which is exact in a float so the machine, the port and the
  ROM all see the same number.
- **The port's ramp is data now.** `g_ShieldColourStock` in chr.c is the
  old `chr0f0295f8()` body as rows of `top, base rgb, slope rgb` (below
  `top`: `base - (s32)((top - shield) * slope)`, the ROM's arithmetic) with
  a flat tail; `shieldColourGet(site, ...)` evaluates it or a mod's table
  for that site, and both decomp callers go through it. The check that the
  machine reads right: run on stock, every sample at both sites is what
  `shield_eval()` / `shieldEval()` (the same formula) gives from that table.
- **What a reading can be**, in `shield_from_code()` / `shieldReadSite()`:
  *stock* (nothing written); *constant* (every sample one colour, the case
  the archive has); *ramp* (the stock breakpoints and slopes with each row's
  colour re-read at its top and the tail at 8, verified back against every
  sample - so a recoloured ramp imports, a reshaped one does not); *unset*
  (a channel the code never stored somewhere: the port keeps its ramp and the
  report says which channel over what shield); *other* (reported, left
  alone). Spooky Dark Vault is the unset case: one word changed,
  `sw zero,0(a3)` to `sw t8,0(a1)`, which writes red a second time and never
  writes blue above 6 shield. That is not a colour, it is a slip, and the
  toy machine is the only thing that told the difference - the first read of
  the diff had it as "blue 162 at full shield". Ask the machine what the
  code *stores*, not what the changed word looks like it means.
- **The block** is the port's own: `shieldcolour hit|player { ramp TOP R G B
  SR SG SB ... constant R G B }`, a top-level block (mod.c
  `modConfigParseShieldColour()`), `constant` alone for one colour, between
  `# importer: shieldcolour begin` / `end` after the weather region; both
  importers cut it out again before rewriting. A mod swap drops a mod's
  tables (`shieldColourSet(site, NULL, 0)`), the stock ramp being compiled
  in. GE-X and the tool write identical blocks; importer version 13.

## The hit sounds, and a list read by running the code (2026-09-07)

`bgun_play_prop_hit_sound` after the shield colour: 22 words, weapon numbers.
What the port does with a weapon-number test is in
[weapons.md](weapons.md) ("The hit sounds are three flags"); what belongs
here is the reading. The function's decisions are the sound number it stores,
so the machine runs it once per weapon number and per case - a chr with the
primary function, a chr with the secondary, an object - and the stored sound
says which branch it took. The branches that copy a table to the stack read
that table through the data segment, which the machine does not have, so
`emulate()` / `emuRunArgs()` took a `mem` seed: a marker word in each table
(the object default has 20 entries and the fake `rngRandom()` indexes entry
12, so the whole table is seeded), and `g_Vars.lvupdate240`, which the
function returns on at once when 0. Two things cost time: the chr branch and
the object branch keep their `soundnum` in different stack slots (182 and
154), and the tables are indexed by a random the machine fakes as its call
counter. Run on stock, the reading is the port's three lists exactly, and
that is checked before the mod is read. The block is the `weaponflags` lines
between `# importer: hitsounds begin` / `end`, always written when the
function is read (it is the code's truth for the mod's table, inheritance
being a guess); importer version 14.

Not carried over: GE-X made the chr-hit shield check unconditional, so its
guards never play the shield damage sound. That is not a weapon property and
was left.

## The co-operative buddies, and a constant that is one of four kinds (2026-09-07)

`player_tick` is the most rewritten function in the archive - 38 of 48 mods -
and nearly all of that is one word: `move a1,s4` made `li a1,19` at the
Hotshot buddy's spawn, which is the value `s4` holds there (`li s4,19`
earlier in the function; HEAD_CHRIST). Twenty-seven mods share it, it is a
reassembler materialising a constant, and it changes nothing. What the
rest change (GE-X 21 words, Mario 22, the Warehouse 9) is the co-operative
buddies: `playerTick()` spawns Velvet and the four cheats' companions with
`chrSpawnAtCoord(body, head, ...)` and arms them with `chrGiveWeapon(chr,
model, weapon, ...)`, all constants, and GE-X gives them Bond's allies,
Mario gives them Mario.

- **Four number spaces in one function.** `follow_immediates()` would have
  put a body 0x56 and a weapon 2 in one list, and the outfit code already
  maps 0x56 to something else (GE-X's hero is 88, its Velvet 122). So the
  follow is by *call*: `follow_call_args()` / `followCallArgs()` take every
  `jal` to a named callee and the last `li aN,` into each argument register
  in the twelve words before it (or its delay slot), and a table says what
  each register means for that callee - `chr_spawn_at_coord` a0 body, a1
  head; `chr_give_weapon` a1 model, a2 weapon. An argument stock computes
  rather than loads (the `move a1,s4`) is not an `li` and is left alone,
  which is what makes the 27-mod word a no-op without a special case.
- The lines are `buddyconst KIND 0xSTOCK MOD` in the datasegment block;
  `modDataBuddyConst(kind, def)` answers, and the fifteen sites in
  `playerTick()` go through `MOD_BUDDY_BODY/HEAD/MODEL/WEAPON()`. A mapping
  is per function: the same stock body maps differently here and in the
  outfit chooser, and that is right. Importer version 15. The two importers
  write the same lines in a different order, the tool sorted and the game
  in code order, like `playerconst`.
- **The datasegment block is built in a stack buffer** (`char lines[]` in
  `writeDataSegment()`), and `LINE()` trusted `snprintf()`'s return value:
  eighteen more lines took GE-X's block past 2 KB and the stack was smashed
  with the block's own text (the backtrace was ASCII: `buddyconst ...`).
  8 KB now, and `LINE()` clamps. Anything that adds lines to the block adds
  to that buffer.
- A co-operative buddy needs a co-operative mission, which a headless boot
  does not start: the mapping was checked live with `gdb -batch -ex 'call
  modDataBuddyConst(1, 0x56)'` on a GE-X boot (122), not by seeing Bond's
  ally spawn.

## One weapon that became two: the compare chains (2026-09-07)

`bgun_tick_inc_attacking_shoot`, 19 words in GE-X, is two things. The
shotgun's pump-action test, `weaponnum == 19 && animmode == 2`, became
`weaponnum == 15 || weaponnum == 14` - GE-X has two shotguns - and every
instruction after it shifted a word, which is why modcodediff calls the
function rewritten. And the Mauler's charge reset at the end of the same
function, `if (weaponnum == 6) matmot1 = 0`, was zeroed out. The port has
both as flags already (`WEAPONFLAG2_PUMPACTION`, `WEAPONFLAG2_CHARGEABLE`);
what was missing was the mod's list.

- **A chain of compares is a shape.** `follow_compare_chain()` /
  `followCompareChain()` read `li at,N; beq at,reg` (a match, on to the
  next) ... `li at,N; bne at,reg` (the last) from the stock site's offset,
  the next `li` allowed in the branch's delay slot - which is how a hack
  turns one weapon into a list in place. Zeros at the site are a test taken
  out, an empty list; anything else does not read. `FLAG_SITES` /
  `flagSites[]` name each flag's sites as (function, stock constant).
- **A flag means the same at every site, so its sites must agree.** GE-X
  took the charge reset out of the shoot tick and kept the Mauler test in
  `bgun0f09a6f8` (the charge sound), so `chargeable` is reported and left:
  its PP7 sits at the Mauler's address and keeps the inherited flag, which
  is the port's charge sound on a gun GE-X's own code half-treats as one.
  Pump action has one site and writes `weaponflags pumpaction { clear 14 15
  }` in a `# importer: weaponsites` region; importer version 16. A mod that
  left the test alone gets the stock list written, which is the same
  authority-over-inheritance rule as the hit sounds.
- The port's flag site and the ROM's compare are not always in the same
  function: `bgun0f09a6f8`'s only GE-X change is 29 -> 21, another laser
  site, not the Mauler's. Read the diff of every function a flag lives in
  before assuming a constant that did not change is one the mod agrees with.
- **Three shapes the compiler gives a chain** (from `casing_create_for_hand`,
  17 words, the next in the queue): the branch can sit up to four words
  after its `li` with the body's own words between (the magnums' casing
  test: `li at,8; addiu; swc1; lw; beq`); the next `li` can follow a word
  of delay slot rather than be in it; and the closing `bne`'s match is its
  delay slot as often as the word after. And two tests on one register are
  run together - `bgun_create_fx` has the magnums' no-eject skip (8, 9) and
  the Reaper's eject part (20) as one run of compares on t1 - so a chain is
  the compares whose `beq`s share a target, and a `bne` belongs to it only
  when falling through lands there. The reader had to learn each of these
  from a site that read as nothing. `casing_create_for_hand` gave the port
  `WEAPONFLAG2_PISTOLCASING` (the four small pistols' tumbling casing; GE-X
  3, 4, 5, 19), put the magnums' straight-down casing on `NOCARTEJECT` (GE-X
  17 alone: its 19 and 20 sit at the stock magnums' addresses and lose the
  inherited flag, as GE-X's code says) and the two Reaper tests on
  `MINIGUN`, which GE-X zeroed and which stays deliberately on the
  definition (weapons.md, the Reaper). Importer version 17.

## projectile_tick: a site is a chain's head, and a flag has one reading (2026-09-07)

Sixteen words in GE-X, all lists: the sticks-where-it-lands chain (mines,
N-bombs, bolt, knife, ECM mine - the port's `STICKSTOWALL`, whose set the
chain matches exactly), the bolt-or-knife test at four places (a thrown
blade embedding: `BLADEHIT`, whose set it is), and a `PROPTYPE_CHR /
PLAYER` test feeding `chrGetShield()` that GE-X made unconditional-off -
another shield check removed, not a weapon, left like the hit sound's.

- **A site is a chain's head.** Looking for the bolt's `li at,86` found it
  first *inside* the sticks chain (86 is in that list too) and read a
  suffix of that chain as a fifth bolt site. The word before a head is not
  a branch on `at`; an `li` after one is the chain's next element, or the
  delay-slot form of it. `follow_flag_sites()` / `followFlagSite()` now read
  every head of a value in a function (the bolt has four) and each is a
  site that must agree.
- **A flag has one reading.** The hit sound reader also writes `bladehit`;
  its reading is direct (what the code stores) and stands over the sites'
  (what the code compares), which are checked against it and reported if
  they differ. GE-X: both say 2 and 86.
- **A chain can be nine long** - the reader's cap was eight, and both the
  stock and the mod list quietly lost the ECM mine. Mario's "stock" list
  was wrong the same way and read as right. When a stock reading is the
  check, compare it with the port's set from a running game (the
  `flags4.gdb` loop over `g_Weapons[]`), not with itself.
- GE-X writes 153 where stock had 63 (a number no table has): dropped from
  the list with a note, not passed to a `weaponflags` line that would warn
  on every load. Importer version 18.

## chr_damage: rules, not lists (2026-09-07)

Sixteen words in GE-X and, for once, mostly not weapon numbers: GE-X tunes
the damage model. What came out, and what did not:

- **Two one-site flags**, read as compare chains like the rest: the
  shotgun's distance falloff on a chr (`WEAPONFLAG2_SHOTGUNDAMAGE`; GE-X's
  15 alone, not the 14 its pump-action list has) and the FarSight's
  shield-piercing tenfold hit (`WEAPONFLAG2_PIERCESSHIELD`; GE-X's 38).
- **The player's headshot scale** is a float the code loads with `lui
  at,0x41c8` (25.0f) at three sites - solo, and the two co-operative
  branches - and GE-X loads 1.0f: no headshot bonus. `follow_float_immediate()`
  / `followFloatImmediate()` read a `lui at` the way `follow_immediate()`
  reads an `addiu`, and it is `g_ModPlayerHeadshotScale`, from a `damage {
  playerheadshotscale N }` block.
- **A hit that breaks the shield still lands.** Stock zeroes the damage in
  the delay slot of the `bc1fl` past `c.le.s` (`mtc1 zero,$f20`, the "shield
  is now gone" branch, which the C reads as `damage = 0`); GE-X, DEEP SEA X
  and both Weather Mods make that one word a nop, so the whole hit goes on
  to health after the shield breaks. The reader looks for the stock pair
  of words and asks whether the second became 0: `g_ModShieldBreakHits`,
  `shieldbreakhits 1`. It was read first as "the shield absorbs nothing",
  which is what the C looks like it says; the other `mtc1 zero,$f20` at
  the branch's target is not on the taken path, and the delay slot is the
  only zeroing there. Read the asm's path, not the C's shape, for a
  one-word change.
- **Left, and why.** The knife's three sites (the poison choke, the
  knife-in-the-back kill, the headshot doubling) and the tranquilizer's
  are the "one weapon with one quirk" class of weapons.md: GE-X's knife has
  no poison function, so its 26 -> 2 buys nothing there, and it took the
  tranquilizer test out. A code cave at 0x7f1ac06c (bondgun's data, where
  modcodediff lists it as `var7f1ac060`) skips shield damage for hits to
  the gun (`HITPART_GUN`) and the hat: a rule a `damage` key could carry,
  not read yet. And the shotgun's mid-range multiplier went 3.0 to 3.25.
  Importer version 19.

## beam_render: an element belongs to a chain by its target (2026-09-07)

Fourteen words in GE-X: the laser's beam tests at four places (29 -> 21,
the first of them also taking 22) and the Cyclone's half-alpha tracer
folded away so every beam draws solid. The port had all of them as
`WEAPON_LASER` / `WEAPON_CYCLONE`; they are four flags now - `LASERBEAM`
(the first site: texture and width), `CROSSBEAM` (the other three: two
crossed quads), `FAINTTRACER` (the Cyclone), and `LASERFLIGHT` for
`beam_create`'s laser-or-watch-laser flight, which GE-X also renumbered
(29, 90 -> 21, 22). A `FLAG_SITES` row can now name *which* occurrences of
a value in a function are its (`(0,)` / `(1, 2, 3)`; the C row's `occ`
bitmask), because one stock weapon test can be two behaviours to a mod:
GE-X's Moonraker (22) gets the laser's texture and flight but is drawn as
one quad.

- **The compiler hoists an `li at`** as far as thirteen words ahead of its
  branch. `at` is the assembler's temporary and nothing between writes it,
  so the window is sixteen words and stops at any other write to `at`.
- **An element belongs to a chain by where it branches**, and that reads
  the rearrangements a hack makes. GE-X reused the Cyclone test's offset
  for its hoisted laser compare, and read as "fainttracer is 22" until the
  chain's body was compared with stock's: a body that moved more than
  three words is another test on the stock site's offset, and does not
  read (so GE-X's Cyclone keeps the inherited flag - it sits at the stock
  Cyclone's address - where GE-X draws it solid; noted, small). The same
  rule finds a hoisted element: the words before the site are searched for
  an `li at,N` branching to the same body, give or take the word a likely
  branch's delay slot puts before it, which is how the 22 came back.
- A weapon number is 1..254; the Cyclone chain's partner is `-1` (no
  weapon, a beam without one), and the site's value is "in the chain",
  not its head. Importer version 20.
- **The game's importer hung on the first site that did not read.** The C
  site loop `break`s out of a flag's rows on a site it cannot read, and
  then restarted at the same row (`i = j`), reporting the same line
  forever; no mod had taken that path before the Cyclone's. A loop over a
  flag's rows now finds the rows' end first and advances past it whatever
  happens in them. It looked like a boot that never imported with an empty
  log, because `timeout -k`'s SIGKILL discards the buffered stdout
  (CLAUDE.md's flush warning, again): `gdb -p PID -batch -ex 'call
  (int)fflush(0)'` before the kill showed "importing ..." with no end, and
  `bt` showed `rep()`.
- Not read: `beam_create_for_hand`, where the laser's secondary stream is
  keyed on weapon and function (GE-X 22, function 0) and the Mauler's
  charge beam on the Mauler (GE-X 119, no weapon: taken out, like its
  charge reset), which is the chargeable flag's disagreement again.

## hand_tick_attack and bot_tick_unpaused, and a third flags word (2026-09-07)

Nine words each in GE-X, both lists. The shotgun's six pellets per pull
(`hand_tick_attack`, `WEAPONFLAG2_PELLETS`; GE-X 14 and 15, its pump-action
pair), a simulant's limitless clip of the laser (`bot_tick_unpaused`,
`WEAPONFLAG2_BOTLIMITLESS`; GE-X 21), and the RC-P120's cloak-by-ammo,
which GE-X took out: its 13 is an FN P90 that the port's number test would
have had weighing its clip to cloak. That one is `WEAPONFLAG3_CLOAKAMMO`,
because **`flags2` ran out of bits at `BOTLIMITLESS`** (0x80000000): `struct
weapon` has a `flags3` now, after `pickupsound`, with `weaponHasFlag3()`,
a `word` on each name in mod.c's flag table, and the data import copying
it with `flags2` by address. A definition that had no `flags2` line yet
gets the three positional fields before `flags3` written out (the RC-P120:
`0, // flags2`, `0, // unequipped reload index`, `0, // pickup sound`),
and a running game's dump says the two stayed 0 - weapons.md's warning
about positional initialisers, checked rather than trusted.

- The second `13` in `bot_tick_unpaused` is the bots' weapon-preference
  `switch`, a dispatch, not the cloak: the row selects occurrence 0.
- Left: the Reaper's tests in both (MINIGUN, on the definition on purpose),
  the Cyclone's secondary discharge for simulants (weapon and function
  together), and two fire-rate numbers (30 -> 44 ticks, a modulo made
  unconditional). Importer version 21.

## The tail, and the unlocks (2026-09-07)

With every rewritten function of nine words or more read, the tail of one-
to-seven-word changes sorts into families:

- **"Everything unlocked"** - the patch twenty-three mods of the archive
  share and GE-X carries: `cheat_is_unlocked`'s six loads made `li 1`,
  `is_stage_difficulty_unlocked`'s two `beqzl` made nops, eight of the
  sixty-nine `jal challenge_is_feature_unlocked` made `li v0,1` (the slow
  motion and one-hit-kills handlers, match start, bot slots, and two in
  challenge.c - *not* the arenas' or weapons' feature tests, so it is the
  options, not every feature), the firing range's three loads made 255,
  the special assignments' two loads and a branch, the alternative title's
  and Perfect Dark mode's flags, the Slayer's three stage tests. Each is a
  family in `game/modunlocks.h` (`g_ModUnlocks`), a key in the port's
  `unlocks { }` block, and a list of (stock address, what the mod's word
  must be: an `li` of a value, a nop, an unconditional branch) in
  `UNLOCK_SITES` / `unlockSites[]`, on when every site of the family
  matches. The port honours each at the function the family names -
  `cheatIsUnlocked()`, `isStageDifficultyUnlocked()`,
  `challengeIsFeatureUnlocked()` for the two options,
  `frIsWeaponFound()`/`ciGetFiringRangeScore()`,
  `getNumUnlockedSpecialStages()`, the alt title and PD mode handlers and
  `bossfileSetDefaults()`, the three Slayer tests in inv.c. GE-X: all
  seven; PD Classics: `mpoptions` only; Mario: five, with `completion` at
  one site of four - reported, left. Importer version 22. Live on a GE-X
  boot: cheat 0 unlocked, slow motion and one-hit kills unlocked with an
  arena's feature still gated, four special stages (three by difficulty and
  the duel), the Slayer allowed, every firing range score gold.
- **GE-X's shield removals**, four more one-word `bc1fl -> b` sites
  (`player_get_shield_frac`, `chr_test_hit`, `chr_emit_sparks`,
  `projectile_0f06c28c`) on top of the hit sound's and `chr_damage`'s: GE
  has no shields, and the port keeps its shield logic, which never fires
  when nothing gives a shield. Left as one deliberate gap.
*(2026-09-07, later: five of these are settings now, importer 29 -
`src/include/game/modrules.h`, each with the stock value as its default, a
modconfig block, one setter in mod.c and a reading in both importers. The
run-speed cave is `movement { fastspeed 1.375 fastcheat 6 }`: the cave
multiplies by 1.375 in a match with the option on and in a mission while
cheat 6 is active, and `fastmoveFromCode()` reads the constant in place or
follows the `j` into the cave for the `lui at` and the `li a0` beside its
`jal cheat_is_active`. The zeroed slow-motion test is `cheats { slowmotion
-1 }`. `chr_set_poisoned`'s two nopped stores are `poisonmatch 0
poisonmission 0` in the `damage` block. King of the Hill's constants are
`koh { hillcolour R G B freecolour R G B }`, read as the float a function
stores to an offset of a register - GE-X writes them as `li at,HI; sh
at,OFF`, the float's top half as a halfword, so `followFloatStore()` takes a
`swc1` fed by `mtc1 at` from a `lui at` or an `sh at` fed by an `li at`, 32
words back. Ten of the palette's sites - the two timers' text, the horizon
scanner's four lines, the menu's join text and its blend, the Slayer rocket
view's interlace - are `colours { NAME 0xRRGGBBAA }` over `g_ModColours[]`,
read by `followColourAt()` as a `lui`/`ori` pair, a lone `lui` or an `li`
(addi and addiu sign-extend: `li s5,-16385` is 0xffffbfff). Left: bot
weapon scoring, the sight, the hats, the keyboard hack, menugfx's colours
built from parts, the shield removals. The rest of this list stands.)*

- **The run-speed cave** (`bwalk_update_horizontal`, `j 0x7f132ac0` into
  the weather's freed words): fast movement times 1.375 rather than 1.25,
  and in a mission the same when cheat 6 is on - `CHEAT_SLOMO`, which GE-X
  repurposed (`lv_get_slow_motion_type` is zeroed). A `movement { }` rule
  could carry the multiplier; the cheat repurposing is GE-X's own.
- **The menu palette** (`menu_render`, `menuitem_keyboard_render`,
  `menugfx_*`, `bview_draw_slayer_rocket_interlace`, ~30 words of colour
  constants), **King of the Hill**'s constants (`koh_tick`/`koh_init`,
  floats made halfword stores), **bot weapon scoring** (`botinv_score_weapon`,
  three weapon numbers), `chr_hit`'s mine list (already the mines' flags),
  `gset_get_sight` and `func0f0b278c` (the sight: type and a white colour),
  `bot_reset` (a float init removed), `hat_get_type` (a jump into a data
  cave: GE-X's hats, a feature the port has no code for), `chr_set_poisoned`
  (the poison timer never set), `tex_init` and `env_choose_and_apply` (table
  addresses the segment locator and env import already follow),
  `stub0f00b200` (a stub made to return), `setup_create_props` and
  `obj_get_hov_bob_offset_y` (one branch each). None read; each is a line
  here so the next session need not disassemble it to know that.

## The number sites: shot_calculate_hits and obj_hit (2026-09-07)

The first of the literal weapon-number sites, chosen where GE-X's
renumbering and the port's compares overlap. `shotCalculateHits()` and
`objHit()` share three lists: the no-bullet-hole one (unarmed, laser,
tranquilizer, FarSight) is exactly `WEAPONFLAG2_NOWALLHIT`'s set, reused
at both; the FarSight's through-walls shot (x-ray, no background hit, the
glass and bullet-slowing arguments to `hitCreate()`) is
`WEAPONFLAG3_XRAYSHOT`, five sites; the no-sparks pair (a fist, the
tranquilizer) is `WEAPONFLAG3_NOSPARKS`. GE-X: no wall hit on 1, 22, 38;
x-ray on 38; and no sparks on 1 and 28 - it never touched that chain, so
its 28 (a proximity mine) inherits the tranquilizer's number there, which
is what its code does and is harmless. The laser's stream test is weapon
and function, left.

- **A row can name its site by stock address** (`('fn', value, 0x7f...)`;
  the C row's `at`), for a site the head rule cannot see: the FarSight's
  `li at,22` in `obj_hit` sits in a delay slot behind a test on another
  register, and the no-bullet-hole chain's head `li at,1` is the fourth
  `li at,1` of the function. Read `objdump` for the site and write the
  address down rather than teaching the finder another shape. One site
  cannot be read at all: `obj_hit`'s first FarSight `li at,22` is a bnel's
  delay slot, overwritten by the next `li at` before any linear use - the
  22 reaches its compare only along the taken branch; the second, which
  changes with it, stands for both.
- **A negated list ends in a `beql`.** `if (w != a && w != b && ...)`
  compiles as `beq`s to the skip and a final `beql` to the word *after*
  the skip (its delay slot having done the body's first word), so the
  terminal is a beq whose target is one word past the chain's: the reader
  takes a target within a word of the first as the same. It read the
  no-bullet-hole list as three of four - and stock's own reading agreed
  with itself - until the sets were dumped from a running game. Importer
  version 23.
- The order of the remaining sites, by overlap with GE-X's constant
  regions: `weapon_tick` (8 changed, 12 literal - the thrown weapons'
  per-kind ticks, weapons.md's "belongs to the shot" class),
  `bgun0f0a5550` (20 / 7: the gun's held position for thrown devices, the
  laser's update, the shotgun's flash), `bgun_create_thrown_projectile`
  (8 / 7), `obj_damage` (12 / 4), `bot_is_obj_collectable` (5 / 8).

## The number sites: weapon_tick, bgun0f0a5550, the thrown projectile, obj_damage and bot_is_obj_collectable (2026-09-07)

The five functions the last section queued, in GE-X's order of overlap, and
`bgun_create_thrown_projectile2` and `bgun0f0a4e44` with them because they
share the lists. Eighteen new `flags3` (constants.h, the names in mod.c) and
twenty-one `FLAG_SITES` rows; importer version 24. GE-X's readings: grenade
26, timed mine 27, proximity mine 28, remote mine 29 (fuse, count, arming,
detonator hand, explodes-when-shot, the bots' hands-off list); knife 2
(`THROWNBLADE`); its sniper 16 (`SNIPERSCOPE`), magnum 17 (`REVOLVER`),
second shotgun 15 (`SHELLPARTS`); no laser sight, no dart case, no shotgun
model update (19 -> 90), the Laptop Gun's deployment on 51. Every one of them
would otherwise have landed on whatever GE-X keeps at the stock number - its
pistol at 19 animating as a shotgun and losing its flash, its proximity mine
at 28 ejecting a tranquilizer dart, its grenades and mines never going off.

- **The game's importer has its own function table.** `codeSyms[]` in
  modimport.c is a hand-kept list of (name, start, end); the Python reads
  `tools/pd.ntsc-final.sym`. A row naming a function the C table lacked was
  skipped without a word - the first boot at version 24 wrote a GE-X import
  with none of the new lines and nothing in IMPORT.txt to say so - and now
  reports "cannot be read: no bounds for the function". Adding a row means
  adding its function's bounds there (the next symbol's address is the end).
- **A rebuilt binary at the same version does not redo an import.**
  `modImportIsStale()` compares IMPORT.txt's first line with
  `MODIMPORT_VERSION_LINE`; to force one, `sed -i '1s/^importer: 24/importer:
  23/' mods/*/IMPORT.txt`. The 48 patches re-import in about a minute.
- **Two more site shapes.** `'imm'` (C: `SITE_IMM`): the `li REG,N` at a
  stock address, read as the mod's li into the same register - for a constant
  hoisted into a register for the whole function (`weapon_tick` keeps the
  grenade's 30 in a2, and compares `bnel a2,t7`), and for one loaded in the
  delay slot of the branch that skips its body, whose compare is far off (the
  mines' 32, 34, 33 at 0x7f06fa94, fc00, fc10, fc8c). `'range'`
  (`SITE_RANGE`): `slti at,REG,LO` then `slti at,REG,HI` within eight words,
  the weapons LO..HI-1; GE-X's laser sight is `1 <= w < 1`.
- **Heads that are not, and a dead site.** `weapon_tick` has a second `li
  at,32` at 0x7f06fbf8 on the path a rocket takes when its timer is not yet
  0 - unreachable as a mine test, and GE-X left it 32; a by-value row would
  have read it and disagreed with the live one. Its `li at,34` at fc10 passes
  the head rule (the word before is a `bltzl` on v1, not a branch on at) but
  is the delay slot's copy. And `bgun0f0a5550` compares a hand state with 8
  (0x7f0a6234) before the magnums' 8. All of those rows name their address.
- **`FLAG_BYNUMBER`** (C: `flagByNumber[]`) takes numbers out of both the
  stock and the mod's chain before agreement and writing: the rocket (83) in
  `obj_damage`'s explodes-when-shot chain and the Skedar rocket (88) in the
  bots' list share `invitem_rocket`, and the port tests the odd one by
  number; and the Reaper's 20 in `bgun0f0a4e44`, whose `beq` lands on the
  shotgun's body and which the hoisted-element rule would otherwise make part
  of `SHELLPARTS` (it is `MINIGUN`'s). Without it the game wrote a stock list
  of "19, 20" and the Python, run before the fix, the same.
- **One shotgun test is two flags.** `bgun0f0a5550` skips its flash call and
  runs `bgunUpdateShotgun()` for the shotgun (both 19 -> 90 in GE-X: off);
  `bgun0f0a4e44` skips positioning parts 0x50-0x52 as flash sprites for it
  (19 -> 15: GE-X's second shotgun, whose model has the shells). One flag
  would have disagreed across its sites and left GE-X's pistol at 19 with
  the stock behaviour; `SHOTGUNMODEL` and `SHELLPARTS` each read.
- **The idle-matrix switch is a jump table, read and left.** `bgun0f0a5550`
  decides whether a hand can share its idle matrices by a `switch` the
  compiler made a table of (`addiu t3,t2,-20; sltiu at,t3,34; ... lw
  t3,OFF(at); jr t3`), and GE-X rebuilt the table in a cave at 0x7f1b4000
  for 2..63: the knife case is {2}, the thrown case {26, 27, 28, 29, 53, 61,
  62} (its sticks-to-wall list less the blades). Both tables read with a
  dozen lines of Python (entries grouped by target). Not converted: the port
  made the cache per hand (`hands[handnum].unk0dd8`), so the list only
  chooses a code path with the same result. If a jump table ever needs
  reading for real, that is the shape: base, count, table address, targets.
- **Left, and why.** The laser's update (weapon 29 -> 22 here, and
  `bgun_update_laser`'s own `li at,1` -> 0: GE-X's Moonraker streams from
  the primary; a function flag's job, with `beam_create_for_hand`). The
  N-bomb's storm and the grenade's `nbomb` cases (GE-X leaves 31; its 31
  gets `EJECTSPIN` and `BOTIGNORES` because its code says so). The Dragon's
  `EXPLOSIONTYPE_DRAGONBOMBSPY` (15 -> 0; one weapon, one quirk, and its
  mine mode is by function anyway). The rockets' tick (83, 84, 88,
  untouched). `obj_damage`'s "a remote mine does not set off a homing rocket"
  (the *attacker's* 34 -> 29: a rule about two weapons). `obj_damage` also
  turns object type 21 (`OBJTYPE_SHIELD`) into 16 - not a weapon; GE-X has
  no shields. `laptop_deploy`'s `li a1,14 -> 51` is a call argument, the
  deployed gun's weapon, not a test.
- **What GE-X's code names that stock's table does not use:** 90 for the
  shotgun tests it took out, 51 for the Laptop Gun's, 31 kept. All three are
  under the table's length and the loader raised no "no weapon N to flag",
  so GE-X has definitions there; what they are is the mod's business.
- Across the archive (every patch re-imported at 24): no row misreads on
  any mod; GE-X is the only one to renumber most of these, and two others
  (the 2XW-NR patch, Spooky Dark Vault) move the tranquilizer to 27
  (`ejectsdart` follows). A row that
  reads the stock list on a mod that never touched the site writes the
  stock list, the same authority-over-inheritance rule as before.
- Next, by GE-X's constants regions still unread and shaped like weapon
  numbers: `fr_is_ammo_wasted`'s projectile lists (its laser test is
  `freeshots` now), `beam_create_for_hand`'s Mauler (the stream is read),
  `obj_child_tick_player` (20 -> 0, `MINIGUN`), and `prop_play_pickup_sound`,
  the `pickupsound` field's sites (inheritance by address already gives GE-X
  the right sounds; a reader would run the function per weapon, as the reload
  index does below, and trap the sound call's argument).

## The pickup rules: bgun_draw_hud, ammo_handle_pickup, weapon_get_pickup_ammo_qty (2026-09-07)

The next three of GE-X's constants regions. Importer version 25.

- **`bgun_draw_hud`'s 29 words are 27 colours and two weapons.** The colours
  are the menu palette family (not read). The two numbers: the detonator
  hand's remote mine (34 -> 29, a second `detonatorhand` site) and the
  combat boost whose time the HUD counts down (35 -> 31, `WEAPONFLAG3_BOOSTHUD`).
  GE-X's 31 is therefore a real item - it also inherits `EJECTSPIN` and
  `BOTIGNORES` from the N-bomb tests GE-X left at 31, and its flags3 on a
  live boot read 0x300040, which is those three.
- **Pickup quantities are a table, not a flag.** `weapon_get_pickup_ammo_qty`
  is two `switch`es on the ammo type (a match's, a mission's), each a jump
  table whose entries are `b end` with `li v1,QTY` in the delay slot, or the
  default that loads nothing (1). GE-X halves most of them and moves two
  entries (its 16 takes the dart's quantities, its 17 the default). Read
  by `ammo_from_code()` / the `ammocfg` block in modimport.c into a
  `pickupqty { mp TYPE QTY ... solo TYPE QTY }` block and
  `g_ModPickupQty[2][]`, applied after the switch and before the mission's
  ammo scale. The compiler interleaves the two prologues - `sltiu at,t0,21`
  (the mission's) sits in a `beqzl`'s delay slot thirty-eight words before
  the mission's `jr` - so a switch is found from its `jr rY` backwards:
  `lw rY,LO(at)`, `lui at,HI`, `sll rZ,rZ,2` in the four words before it,
  then the `sltiu at,rZ,N` anywhere back to the function's start. Finding
  the count by distance read both switches as the match's. And `jr ra` is a
  jr too: a table that does not read is skipped, not counted.
- **What an ammo pickup gives is a chain of pairs.** `ammo_handle_pickup`
  maps the ammo type to the weapon it puts in the inventory: `li at,TYPE
  ... bnel s0,at,next; li at,NEXT; b end; li a0,WEAPON`. The port keeps the
  map in `g_AmmoTypeWeapons[]` (and `botactGetWeaponByAmmoType()` reads the
  same table now, its own six cases having been the same numbers); an
  `ammotypeweapon { TYPE WEAPON ... }` block edits it. GE-X: grenade ammo
  gives 26, the mines' 27-29, knife ammo 2 - without which picking up
  grenades in GE-X armed the player with whatever GE-X keeps at 30.
- **GE-X renumbers ammo types too, and that is not read.** Four of the
  chain's `li at,TYPE` changed (32 -> 27, 30 -> 26, 20 -> 0, 21 -> 16), and
  the quantity tables move 17 to 16 in both modes. The port's ammo types
  are whatever the mod's `g_AmmoTypes` data table says (the data segment
  import), and what GE-X's code means by 16 is not knowable from the code
  alone; a pair whose type changed is counted and reported, not written.
  The ones that were only weapon changes are.
- Left: `weapon_get_pickup_ammo_qty`'s knife/bolt test is `pickupsingle`,
  a row (GE-X 2, 86). `ammocrate_get_pickup_ammo_qty` (the crates' table,
  inlined after `ammo_handle_pickup` in the ROM) GE-X did not touch.

## The hand state machine, the bots' throws and the laptop's number (2026-09-07)

The rest of GE-X's weapon-number regions of four words and up. Importer
version 26. Three more flags3 and eight rows; GE-X reads at every one.

- **The knife's reload is one quirk at four places** (`bgun_tick_inc_reload`
  three times, `bgun_tick_inc_autoswitch` once): no reload animation when
  the reload starts, the animation once the hand is empty, the hand coming
  back up idle, and `HANDMODE_11` on an autoswitch. weapons.md had left the
  first two as "one weapon with one quirk whose intent is not visible";
  read together they are one thing, `WEAPONFLAG3_KNIFERELOAD`, and GE-X's
  knife at 2 would otherwise have reloaded as a gun while its grenade at
  26 kept the knife's hand. `KEEPSFUNCTION` is the mines' function
  surviving an autoswitch (GE-X 27, 28), and the grenade's no-casing reload
  there is the port's `noeject`, a row.
- **`obj_free`'s eight words are the proximity chain again** (`33 || (15 &&
  f) || (31 && f) || (30 && f)`): a third `isproximitymine` site, and the
  port already had it as `weaponIsProximityMine()`. `obj_stick` is the
  port's `objLand()`: its `{53, 61, 62, 63}` is `hardwhenlanded` (GE-X
  names 153 for the three it took out, off the end of the table, so its
  ECM mine alone), and its three knife tests are the `knifeLand()` dispatch
  weapons.md leaves as a jump table.
- **`chr_shoot` is `chrTickShoot()`.** The laser's shot always drawing a
  beam is `CHRSHOTBEAM` (GE-X 21, not its Moonraker 22 - so not the
  `LASERBEAM` set, and a second flag rather than a disagreement that would
  have unwritten `laserbeam`). A simulant's FarSight shooting through walls
  is `xrayshot`, a fourth site - read as one immediate, because the likely
  branch between the `li at,22` and its `bnel` carries `lui at,0x4780` in
  its delay slot, a write to `at` that ends a chain. The Reaper's 20 -> 0
  is `MINIGUN`, on the definition; the 29s inside `chrTickShoot()`'s
  `case` chains GE-X left alone.
- **The laptop is named by the gset, or by its flag.** `laptop_deploy` has
  the gset of the throw and reads `gset->weaponnum`; picking a deployed
  laptop back up (`propobjInteract()`) and the autogun's own shot at a
  target (`autogunTickShoot()`) have no gset and no field on the autogun
  (`struct autogunobj` is a setup file's layout), so `weaponFindDeployable()`
  finds the weapon carrying `DEPLOYS` - GE-X's 51 - and falls back to the
  Laptop Gun. That was `propobj_interact`'s five `li a0,14 -> 51`.
- **The bots' throwables are the table's, not a list.**
  `botactIsWeaponThrowable()` listed the Laptop Gun's, Dragon's and knife's
  secondary and the grenade's, N-bomb's and mines' either function; that
  is "the primary function is a throw, or the function asked about is" (a
  mine's secondary is not a throw - the first rule drafted, "the function
  asked about is a throw", lost the mines' secondary against the dumped
  set, check 3 of weapons.md). `botactGetProjectileThrowInterval()` paces
  by `THROWNBLADE` (120 ticks) and `GRENADEARC` (90) over the default 60.
  GE-X rebuilt both switches' jump tables in caves (2..34 and 2..31 at
  0x7f1b3f00 and 0x7f1b3c60); neither needs reading now.
- Left: `fr_is_ammo_wasted` (the firing range's eleven-weapon list of
  shots that may still be live, and the projectiles they leave; six of
  GE-X's renumberings, in a mode GE-X does not have), `beam_create_for_hand`
  (weapon and function, and the Mauler's 6 -> 119 - `chargeable`'s
  disagreement again), `chr_grunt` (its 15, 41, 58 and 12, 47, 60 are not
  weapons - body numbers, by the look of the sets GE-X made 5 and 4), and
  `chr_shoot`'s 7 -> 39 and 1 -> 0 at 0x7f0418e4, unread.

## The last of the number sites, and a field read by running the code (2026-09-07)

Importer version 27. Two more flags3 and, for the first time, a `weapon N {
}` block written by the importer.

- **`bgun_tick_gameplay2`'s FarSight is x-ray vision** (aiming through its
  sight switches the vision mode, and the scanner defers to it): two more
  `xrayshot` sites, and `obj_attachment_test_hit`'s first `xori` (a
  bulletproof object's glass gives way to it) a third. GE-X 38 at all of
  them. The two RC-P120 tests there are `cloakammo`; GE-X took one out and
  left the other, so the flag is reported and left - and GE-X's 13 does not
  sit at the RC-P120's address, so it inherits nothing either way.
- **An `xori` is an immediate too.** `obj_attachment_test_hit` compares with
  `xori v0,v1,N; sltu` rather than `li at,N; bne`, which no chain reader
  sees; the `'imm'` kind takes an xori of the tested register as well as an
  li. Its magnum-and-FarSight pair (a shot through a bulletproof object's
  parts) is two xoris that are one list, so an `'imm'` site may name a tuple
  of addresses (C: `at2`), and `FLAG_BYNUMBER` takes the FarSight's 22 out
  as `XRAYSHOT`'s: `WEAPONFLAG3_PIERCESBULLETPROOF` is the magnum's, and GE-X
  puts two pistols there (17, 19), not its FarSight - reading the second
  xori as an `xrayshot` site had the flag disagreeing with itself.
- **The grenade launchers are a function flag the ROM already had.**
  `bgun_create_fired_projectile` picked the grenade round by two weapon
  numbers (the Devastator's, the SuperDragon's); `FUNCFLAG_10000000` sits on
  exactly those three functions (and the round's own two, which no hand
  fires), so the port tests it, and the SuperDragon's small burst - its
  rounds carry `FUNC_2` - is `WEAPONFLAG3_SDGRENADE`. GE-X has none.
- **A value per weapon is read by running the function per weapon.**
  `bgun_get_unequipped_reload_index` is pure - a chain of compares returning
  the animation index or -1 - so `reload_from_code()` / the `reloadcfg`
  block run it for every weapon number on both binaries (`emulate()` /
  `emuRun()`, the weather's toy MIPS) and write `weaponflags
  unequippedreload { clear ... }` and a `weapon N { unequippedreloadindex I
  }` per weapon in a `# importer: reload` region. GE-X: its second shotgun
  15 with animation 1 and magnum 17 with 2, the crossbow's and the LX's
  gone - and its 14, at the stock shotgun's address, *loses* the inherited
  reload, which is what its code says. Live: 15's flags2 has the bit and
  index 1, 14's has neither.
- Left: `fr_is_ammo_wasted`, `beam_create_for_hand`, `chr_grunt`,
  `prop_play_pickup_sound` (above), and in `bgun_create_fired_projectile`
  the crossbow's bolt and the launchers' rocket, which GE-X does not
  renumber and which are a projectile number per function - a field, if
  ever, not a flag.

## The laser's stream is a function flag (2026-09-07)

Importer version 28. The one weapon-and-function test worth its own
machinery: GE-X's Moonraker (22) streams from its *primary*, so every site
that said `weaponnum == 29 && weaponfunc == 1` had both halves renumbered,
and neither a weapon flag nor the function test alone could carry it.

- **`FUNCFLAG_LASERSTREAM`** (0x10, a bit stock's table leaves free - GE-X's
  function flags were dumped from a live boot to be sure it sets no unknown
  bit) sits on the laser's stream function. `shotCalculateHits()`,
  `beamCreateForHand()` and `bgunUpdateLaser()` test the gset's function
  for it; `bgun0f0a5550()` calls the update when either function has it.
  The hit sounds' `weaponfunc == FUNC_PRIMARY` beside `LASERHIT` is left:
  it is "not the stream", GE-X's own `beqz` there is unchanged, and for a
  stream on the primary both codes play the sound every hit.
- **A `FUNCFLAG_SITES` row is a weapon site and a function address.** The
  weapon half is read as any `FLAG_SITES` site (by value, or a chain at an
  address); the function half is the `li at,F` beside it, read in both
  binaries with 0 allowed (`immediate_pair_at()` / `immediatePairAt()`), and
  may be in a callee: `bgun0f0a5550` tests the weapon and
  `bgun_update_laser` the function. A site reads as pairs, the sites must
  agree, and the line is `weaponfuncflags FLAG { clear W F ... }` in the
  weaponsites region - a new block, `weaponflags` for a function's flags,
  with a name table of its own in mod.c (`laserstream`, and the two the
  `weaponfunc` block already knew). GE-X: (22, 0) at all three.
- **`WEAPONFLAG3_FREESHOTS`** is the laser's shots spending no ammo, at
  `bgun0f09a6f8` (the hand's shot count) and `fr_is_ammo_wasted`; GE-X's 21,
  which is its laser proper: 21 beams and costs nothing, 22 streams and
  crackles. `gset_populate`'s 29 -> 21 (the burst count the hit sound
  cadence reads) is left: for a stream on the primary the cadence test is
  never reached.

## The Stage Loader (2026-09-07)

**What it is.** Extended Options > Stage Loader: every installed mod's maps
as extra Combat Simulator arenas, beside whatever mod is loaded, and no
stock map or file replaced. `Mod.MapMods` in the config holds the choice:
`*` for every installed mod, a `;`-separated list of mod names, or empty.
The page has a checkbox for every mod, a dropdown and a checkbox for one
mod at a time, a status line and Restart Now.

**How it works.** The file layer already had the mechanism: only the first
mounted directory overlays the file search, and any mounted after it is
reached solely through file slots pinned to it (`romfile.moddir`). The
loader mounts each chosen mod with `fsAddMapsDir()`, which never overlays
whatever its position - `fsGetModDir()` answers the overlay only, and
`fsGetNumOverlayModDirs()` is what the texture pack scanner and the mod
loader iterate by; the limit went from 8 dirs to 128. `modloaderInit()`
(port/src/modloader.c, the `--modstages` mechanism that was opt-in and
incomplete) then scans every maps-only dir's `files/bgdata/` for
`bg_NAME.seg` with a `Ump_setupNAMEZ`, registers the bg, pads, setup and
tiles as slots pinned to that mod, clones `STAGE_MP_SKEDAR`'s table entry
under a free stage id, and adds an arena named `NAME (mod)` cut to the
row's 30 characters. It runs again on a live swap (`modListSwap()`, which
`modMapsApply()` calls with the loaded mod's own index): the tables are
restored from the snapshot, the slots dropped, the mounts redone, and the
scan starts from nothing - the next slot and the stage-to-mod map are reset
at its top, which the old code did not do.

**What the unfinished branch had found** (`wip/modstages`, merged here):
a runtime stage has no `g_StageAllocations8Mb` entry and fell through to
the sentinel's `-mvtx98`, half an arena's vertex pool, and the pools do not
bounds check - `modloaderGetStageAllocation()` gives it Skedar's, and
`gfxCheckVtxPool()`/`gfxCheckGfxPool()` log the first overrun of a stage
rather than corrupt `g_HudMessages`. A mod's room can understate its
gfxdatalen, so `bgLoadRoom()` sizes for the compressed read too and logs a
room it cannot render instead of leaving a hole. A 512-byte placeholder bg
(GE-X ships three) is skipped below `MODSTAGE_MIN_BG_SIZE`. And textures:
`texLoad()` asks the mods before the ROM table, since a mod's ids run past
`NUM_TEXTURES`, and `modTextureLoad()` reads `textures/%04x.bin` from the
running stage's own mod first (`modloaderGetStageModDir()`), with
`modSetTextureFromStage(0)` around a model's display lists so a stock prop
does not wear the map's art. Such a texture is also registered as the mod's
(`TEXPACK_ART_MODSTAGE`) along with which mod it came from, so no texture pack
and no XBLA record repaints it, and the mod's *own* pack is read for it out of
an index built from its `textures/` - texture-packs.md, "A texture number
belongs to whatever supplied the texels".

**The textures come from the importer**, version 30: a mod that rebuilt its
texture table used to ship the two segments whole and nothing else, so its
maps played beside another mod drew with stock art. Both importers now also
write every texture of the mod's own table that differs from stock's at
the same number, or is past stock's end, as `textures/%04x.bin` (GE-X:
2103), leaving out one too big for the 4 KB load buffer with a report
line. The overlay case is unchanged: segments whole, and the files beside
them agree with them.

**Limits, all of the fixed-size kind.** 27 stage ids are free below
`STAGE_TITLE`, and since 2026-09-07 the 162 above `STAGE_4MBMENU` are usable
too: the sixteen "is this a real level" tests ask `STAGE_IS_LEVEL()`, which
admits both ranges (stage-numbers.md has the details and the save-format
clamp), so the whole archive's maps register at once. `g_Stages` and
`g_MpArenas` have `MAX_MODSTAGES` (189) spare rows, one per usable id; past that the status line
says "N of M maps: out of stage numbers". A map's props are the stock files under
the setup's ids: a mod that renumbered its file table (GE-X) gets stock
props on its maps, the crate GE-X calls `Pmulti_ammo_crateZ` being a
different model - `--modfiles` in romdata.c follows the stage's mod for
every unpinned slot and is the experiment for that, not safe in general.
A map whose setup depends on the mod's own AI commands or weapon numbers
plays as the port has them.

**Testing.** `Mod.MapMods=*` in a scratch savedir's pd.ini, boot with no
`--moddir`, and `grep modloader pd.log` lists every registration; then
`--boot-stage <id> --mpsims 4 --rng-seed 1` boots a match on one (the ids
are in the log), `gdb -p PID -batch -ex 'call (void)screenshotRequest()'`
takes the picture into `build/screenshots/`. `mod: stage 0x.. draws with
textures from ...` says a map used its own mod's art. A match that ran
logs `lv: 5 chrs with a prop at frame 300 of 143 slots`; `0 chrs ... of 11
slots` with the N64 logo in the picture means the boot fell back to the
title, which `--boot-stage` did for every id above 0x5d until 2026-09-07.

**Every map at once (2026-09-07).** The "27 ids" cap is gone: the sixteen
"real level" tests ask `STAGE_IS_LEVEL()` (stage-numbers.md), so ids
0x5e-0xff are handed out after the 27, `MAX_MODSTAGES` is 189 (one per
usable id), and `ROMDATA_MAX_MODFILES` is four names per stage - the name
pool of 512 was the next wall, at 130 maps. With this tree's every mod dir
mounted (build/mods plus the `mod_*` dirs, 158 maps) all 158 register, the
arena list scrolls through them, and matches ran on 0xc0 (All-in-One's
cave) and 0xe0 (GE-X's mp11, its own textures). One map crashes at any id:
GE-X's `ear` (53 chr entries, a solo setup with a multiplayer name) dies in
`hudmsgCreateFromArgs()` from the setup's own commands - not the loader's.
All-in-One's `cave` shows only "Press START" at any id (0x0a with that mod
alone, 0xc0 with everything): the match runs (`lv:` counts the chrs) but
the player is not standing in it, so that is the map's setup too. A mod's
name for `Mod.MapMods` is its directory name as listed, prefix included:
`mod_allinone`, not `allinone`. The game rewrites pd.ini at exit, so edit
the scratch config only after the process has gone, or the edit is lost.
The tester's first crash on a bundle map (All in One's azt as an arena,
"stack smashing detected" under `playerChooseSpawnLocation`) was the intro's
spawn pad count: `g_SpawnPoints` and the chooser's three per-pad arrays held
24, stock's most, and the setup lists more. `MAX_SPAWNPOINTS` (64) sizes all
four now, the intro reader warns and drops the rest past it, and the chooser
clamps its count for any caller's list.

**The maps block: a mod's own arenas, by its own names (2026-09-08).** The
scan above pairs `bg_NAME.seg` with `bg_NAME_padsZ` and `Ump_setupNAMEZ` and
calls the arena `NAME`, which is wrong four ways on any console mod, because
the file names are **Perfect Dark's slots** and say nothing about the map. For
GE-X: `crad` is **Aztec**, and Cradle is `mp18`; its Egyptian is
`bg_dest.seg` with mp11's pads, while the `bg_mp11.seg` it ships is referenced
by no stage row and was being registered with mp11's pads instead; Train
(`bg_run.seg`), Bunker (`bg_tra.seg`) and Facility BZ (`bg_mp5.seg`) borrow
another map's geometry, so the scan never found them at all; and `ear`, `rit`,
`mp1` and `mp15` have a `Ump_` setup but are not in its arena list - `ear` is
the solo setup that crashes in `hudmsgCreateFromArgs()`.

All of it is in the mod's own tables, so **both importers now write a `maps`
block** (version 31), one line a map:

```
maps {
  map "Aztec" bg "bgdata/bg_crad.seg" tiles "bgdata/bg_crad_tilesZ" pads "bgdata/bg_crad_padsZ" mpsetup "Ump_setupcradZ"
}
```

`g_MpArenas` says which stages are arenas and what each is called (a text id:
bank in the top bits, index in the low nine), `g_Stages` says which files a
stage loads, and the name comes out of the mod's own language file for that
bank - `g_LangFiles` (0x80084124, a file id a bank) into `data.names`, then
the file's u32 offset table (`lang_string()` / `langString()`; bank 40 is
`LmpmenuE`). A mod that did not ship the file gets the stock one, which is the
name the ROM it was patched from had. Nothing else reads `g_LangFiles`, so it
is in the symbol tables only for this.

`modloaderAddFromConfig()` reads **only that block**, straight out of the
mod's directory - a maps-only mount is never loaded and its config is never
parsed, so its weapons stay where they are; `mod.c` skips the block for the
mod that *is* loaded, whose arenas come from its own tables anyway. With no
block the scan runs as before, which is every mod that was not imported here.
Two rules matter in the loader: a file the mod does not ship comes from the
**port's stock slot** (`modloaderFileSlot()`), since a console mod ships only
what it changed - that is how the all-solo-levels and weather mods, which ship
`Ump_setup*Z` for solo stages and no geometry, register 15 maps each where the
scan found none; and a map is skipped unless the mod ships **at least one** of
its four files (`modloaderMapIsOwn()`), or every mod's block would put all of
Perfect Dark's own arenas in the list a second time.

With all of this tree's mod dirs mounted the arena list is 189 maps of 223
found - the 189 usable stage ids are now the wall, and the status line says
so. GE-X's 22 register under Temple, Complex, Caves, Library, Basement, Stack,
Facility, Bunker, Archives, Caverns, Egyptian, Facility BZ, Frigate, Archives
BZ, Streets, Train, Cradle, Aztec, Citadel, Labyrinth, Icicle Pyramid and
Cliff Base. Verified by screenshot: Cradle is the girder walkway under a night
sky, Egyptian the sandstone room, and Air Base (stock geometry, the mod's own
pads and setup) draws the Skedar ship.
