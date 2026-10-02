# Goldfinger 64: a GoldenEye ROM hack converted beside GE Plus

Goldfinger 64 (2017, made with SubDrag's GoldenEye Setup Editor) is a patch
against GoldenEye 007 (US). The game converts it the way it converts GoldenEye
for GE Plus, into a folder of its own, `mods/Goldfinger 64/`, and lists its
arenas through the Stage Loader and its missions and arenas under its own row on the Perfect Menu (GE Plus's folder screens in its mode). It is
never part of GE Plus, which is GoldenEye's ROM alone.

## Where it comes from

`added-content/` is where it goes, like the GoldenEye ROM and both XBLA
releases: the zip it is passed around as (`goldfinger64.zip`, holding
`Goldfinger64_1_0.xdelta`), the bare patch, or a Goldfinger ROM patched
already. `gexPlusRomConvertVariants()` (gexplusrom.c) runs after GE Plus's
conversion at startup:

- a patch is applied to the GoldenEye 007 (US) ROM in `.z64` order (the
  player's may be `.n64`: `geconvertIsGoldenEyeUs()` swaps it in place first;
  applied to the swapped bytes the xdelta's window checksum fails);
- a zip is looked into for a patch (`archiveFindEntry()` wants the path
  expanded: `$E/...` finds nothing) and unpacked once into `cache/romhacks/`;
- what comes out is converted when `geconvertVariantName()` knows it.

Done once per source: `CONVERT.txt` has the converter's line and `Converted
from <path>`, the path made canonical (`realpath`), since `$E/added-content`
and `./added-content` are the same folder spelt two ways. Two sources of one
hack (its zip and the patch out of it) used to convert over each other on every
start, each finding the folder stamped by the other; a hack is converted once a
start, by whichever source is seen first.

A drop in `mods/` is a Perfect Dark mod to the Mod list, which imported the
patch, found it applies to no Perfect Dark ROM and left an IMPORT.txt saying so.
Now a new drop there - a patch nothing imported yet, an archive nothing
unpacked yet - is tried on GoldenEye's ROM first (`variantAdoptFromMods()`,
before the Mod list is read) and moved to `added-content/` when it makes a
hack, so it converts and registers at the same start. The Mod list's import
asks too (`gexPlusRomPatchIsHack()` in `modListPrepareDir()`), for an archive
unpacked by an older build: it moves the archive, clears what the import and
the unpack wrote, and the hack converts at the next start.

The XBLA importer took `goldfinger64.zip` for the Perfect Dark XBLA release
where the release was not there (and would before it, named with a capital,
where it was): an archive holding a ROM patch is skipped (xblaimport.c).

## The converter: a layout per ROM

GE Editor patches GoldenEye's code in place. What stays where it was: the data
segment (ROM 0x21990, 0x80020d90), the file table (727 rows, the same names -
characters renamed, `CusgruntZ`), the fog, level info and special portal tables,
`multi_stage_setups`, `setup_text_pointers`. What moved is found in
`g_Layouts` (geconvert.c), each table at the address the hack's code loads it
from (the lui/addiu pair at the US code's site for it):

| | US | Goldfinger 64 |
|---|---|---|
| ROM | 12 MB, NGEE | 24 MB, NGFE, needs the Expansion Pak |
| props | 0x8003a228, 340 | 0x8070b400, 416 |
| characters | 0x8003de10, 80 | 0x80700fc0, 126 |
| images | 0x80049300, 8-byte rows, 24-bit size, 2698 from ROM 0x8f7df0 | 4-byte rows, 23-bit size, 3915 from ROM 0xeae714 |

Goldfinger's boot takes 1 MB off `osMemSize` (0x80000318 reads 0x700000) and
copies three pieces of the ROM's tail into it. Its loader was not found; the
pieces are ares's RAM at the title matched against the ROM page by page:
ROM 0x17fd000 -> 0x80700000 (characters), 0x17f8800 -> 0x80703000,
0x17f0000 -> 0x80708000 (props). `romOpen()` grows the data segment over them,
so a pointer into them reads as `ptr - DATA_VRAM` as any other does. The pages
of them that do not match the ROM at the title are model headers the game has
written pointers into since.

ares runs Goldfinger only with the Expansion Pak: `n64twin --expansion-pak`
(tools/gefidelity/ares/twin.cpp; GoldenEye's runs stay 4 MB).

A new hack is a new row: the code sites give its addresses
(`tools/gefidelity` has none for it yet - the sites were paired by a script
in the session, lui/lo at the same PCs in both ROMs), ares its pieces.

## Converting a variant

- The levels are its arenas' and its missions', each once (`variantLevels()`).
  An arena is a row of `multi_stage_setups` (0x8002b074): name from LtitleE,
  setup `Ump_` + the level's `setup_text_pointers` name, and its picture (the
  row's photo is a row of `mpstageselimages`, GoldenEye's sixteen arena
  pictures and Random, which the hack fills with its own: the maps line's
  `picture`). A mission is a MISSION_PART row of
  `mission_folder_setup_entries` (0x8002abe4, 28-byte rows): Goldfinger keeps
  GoldenEye's twenty mission numbers, levels, briefing files and text banks
  and renames and regroups them (nine chapters, Mexico to Bonus); the setup is
  the `setup_text_pointers` name itself. A level's bg, tiles, scale and fog's
  visibility are its levelinfotable row. Goldfinger put its arenas on mission
  levels (Offices is Dam's level 33, China is Bunker 1's with tiles of its own,
  `Tbg_wax`, Tunnels is Surface 2's slot with `bg_lue`), so nothing of
  `g_Levels` is assumed.
- A variant level's key is its bg and level id (`dam33`). No GoldenEye key is
  one, so nothing keyed on GoldenEye's levels reaches it. What is keyed on
  GoldenEye's *file names* - which the hack reuses - is switched off by
  `g_Layout->variant` instead: the later cartridges' setups and portals
  (`revisionSetup()`, `revisionPortals()`) and the Community Edition's fixes
  (`romPatched()`). The special portals are read from the ROM by level id and
  are the hack's own.
- Its missions block is its own (`modloaderReadMissions()` is per mod: a
  stage carries its mission number, `g_ModStageMission[]`, and a number is
  resolved in the set of the mode chosen, `modloaderMissionStage()`).
  Cuba/the credits are GoldenEye's only.
- Its menu files are its own, from where its layout says: the fonts (moved),
  sound effects and wave table (a quarter again GoldenEye's), instruments and
  sequences (its own music), the folders' backdrop, the briefings and level
  text banks, LtitleE, the watch banks, `menu/missionfolder.bin` (its folder,
  `writeMissionFolder()`), its 126 characters (`Cgx%03dZ`) with
  `gechrs.bin`'s new head flag (4: a model with no skeleton, since its heads
  and bodies are interleaved), `gecast.bin` (random head pools and
  `solo_char_load()`'s Bond, a body and a head an outfit: Goldfinger's Bond is
  head 74 on golf/tuxedo/alpine/sneaking/ranch/Fort Knox bodies),
  `headhats.bin` with a GEH1 header (its hats cover all 126), and
  `geexplosions.bin` (`object_explosion_details`, moved to 0x8070f028 with a
  row for each of its 416 props - it renamed 318 of GoldenEye's 340). Not its
  own: the intro reel (GE Plus's is GoldenEye's) and the monitor programmes
  (gemonitortable.h is GoldenEye's; its screens show nothing).
- A variant's level or character that does not convert is left out with a
  note rather than failing the whole conversion (nested `g_Fail`).
  Goldfinger's characters 108 and 109 (CheadwreckZ, Chead00actionZ) carry a
  body's header over a 3 KB file and are left out; nothing names them.
- **A level wider than 16 bits** (`tilesAreWide()`): Goldfinger's Alpine
  Highway (Bunker 2's slot, level 27) is drawn at a scale of 0.2 and runs
  233,000 units across. Its tiles are written as `GEOTYPE_TILE_F` (float
  vertices: a 16-byte header, then a coord a vertex; Perfect Dark's collision
  reads them beside the int kind, and `filetiles.c` swaps them) and its graph
  as `GST2` (32-bit points; gestan.c's points and tile bounds are 32 bits now,
  and its wall match takes float tiles). Room boxes, room vertices, portals and
  lights all fit: they are room-relative or floats. GoldenEye's own output is
  byte-identical - every one of its levels fits.
- Textures: the hack's images run past GoldenEye's last, over the numbers
  `g_TexRemap` moves GoldenEye's to, so a variant moves its own at first use
  past its last image (Goldfinger: 31 of the 181 free). `geconvertTexRemap()`,
  which the runtime uses, is always the US table; no image the runtime fetches
  by number is a reserved one.
- `NUM_REMAKE_MODELS` is 1024 (was 340): a stage fills the slots from its own
  mod, GoldenEye's and a hack's side by side.

## At run time

- `modloaderStageIsRemake()` is true on a hack's stage (it has a models block),
  so it gets GoldenEye's rooms, lights and props. `modloaderStageIsGexPlus()`
  is the GoldenEye conversion's alone and is what GE Plus's lists use;
  `modloaderStageInGexPlusList()` is the list of the mode chosen, GoldenEye's or
  the hack's (`g_GexPlusVariant`, set by the Perfect Menu's "Goldfinger 64",
  cleared by GE Plus's own row and the Combat Simulator's).
- **The folder**: the hack's row opens GE Plus's folder (`gexFrontOpen()`) in
  its mode - its mission grid from `missionfolder.bin`, its fonts, LtitleE,
  briefings and music (`frontWantDir()`: a level's own mod in a level, the
  mode's otherwise); MULTIPLAYER keeps the hack's arenas; EXTRA's cinema plays
  its missions' openings. Best times are a file a set
  (`$S/goldfinger64-times.txt`). No translation applies to a hack's text.
- **Sound**: `gemusic.c` and `gesfx.c` hold a bank a converted mod (up to
  four), the playing one by the stage's mod or the folder's; a mod with none
  plays GoldenEye's. Perfect Dark's stages keep GoldenEye's for a GoldenEye
  gun.
- Characters: `GEROM_NUM_CHRS` 128; heads by the table's flag; pools and Bond
  from `gecast.bin`. The Combat Simulator's ROM characters are GoldenEye's
  conversion's only, wherever the two are mounted
  (`modloaderGexPlusDirIndex()`: mount order is readdir order). The guns
  are the stage's gun set (Its guns, below).
- The model lend (`modloaderLendRemakeModel()`) takes GoldenEye's own only.
- **No XBLA on a hack's stage** (user, 2026-10-02: incomplete textures).
  `xblaSwitchStageHeld()` makes every part's getter answer 0 there; pd.ini and
  the parts' own switches are untouched, F6 says it is off. Each part's file
  read its own `optEnabled` directly in places - all of them go through the
  getter now. And the release's GoldenEye models are matched by file name in
  both looks (`gebeanFindRow()`, and `gebeanPoolRowForFile()`'s remake-file
  branch, which is where Goldfinger's `Pgx030Z` became GoldenEye's console):
  `gebeanFileIsRomHack()` keeps them off a hack's files.

## Its guns (converter 99)

GE Editor's hacks renumber the hand items and patch the code that tests an
item by number to follow. Goldfinger 64 put a Luger and a P38 at 7 and 8 and
moved every gun after them on (its AK47 is item 9, where GoldenEye's ZMG was);
its laser beam (CapBeamLengthAndDecideIfRendered()) tests 24 for 22, its mines'
ammunition 28-30 for 27-29, the weapon case's sniper rifle 20 for 17, and
getPropForHeldItem()'s jump table (0x8005762c) was reordered with them. 138 of
GoldenEye's functions differ, most of them item constants. So a hack's gun is
**the GoldenEye gun whose file it took** - its GsniperrifleZ at 20 is the
sniper rifle as the Armalite AR7, its GknifeZ the golf club, GthrowknifeZ
Oddjob's hat - and stands on that gun's weapon (`itemWeaponsBuild()`,
geconvert.c, by `g_GeItemFiles`). Four are on files GoldenEye has no weapon of
its own for (GtaserZ, GwatchlaserZ, GgoldwppkZ, GsilverwppkZ: the Luger P08,
Walther P38, S&W Model 36 and Model 22) and take `WEAPON_GE_EXTRA1-4`
(0x80-0x83), pistols on the PP9i. Weapon numbers ran out at 0x7f (s8):
gunctrl's are s16 now, a beam's s16, a weapon prop's `dualweaponnum` u8 with
0xff for none; `GE_GUN_INDEX()` is "a gun, GoldenEye's or a hack's".

The conversion writes `menu/geguns.bin` for a hack (`writeGuns()`): a row a
gun of its weapon, item, held prop (`heldprops`, the layout's copy of the jump
table's case immediates), name (its LgunE weapon-of-choice text) and its raw
gunWeaponStat row. Its setups, AI and weapon sets name its guns through the
same map, and its Igx files are written under its own item numbers.

At run time `gegunsStageSet()` (setup.c, before the props are made) swaps the
gun set: on a hack's stage geguns.c's tables (stats, sounds, item numbers,
held props, placement, names, determiners) point at the hack's, its
definitions are built once from them and swapped in whole; anywhere else
GoldenEye's come back as they were (the watch laser's swap is the precedent).
GoldenEye's rows read from the ROM give gegunstats.h to the last field (all
25 checked), so a hack's rows parse the same way. A gun that fires otherwise
than its host (Goldfinger's Karabiner 98k and M1 Carbine are single shot on
the Phantom's and RC-P90's automatic hosts) gets a function of the row's kind.
The HUD's icon follows the row's AmmoType; the watch draws the set's item.
gegadgets.c gave the Moonraker back the name it had on the first stage
loaded; it asks the set now (`gegunsNameId()`), or a session that began on a
Goldfinger stage named GoldenEye's Moonraker the Portable Laser. Probe for
the swap both ways: `probe/swap.py` (STAGES=0x7d,0x15,0x7d).

Trap found: Goldfinger's M14 and M1 Carbine have a muzzle flash node whose list
loads no vertices but whose count says 6 and 3 quads, the gun's whole mesh
under it, so the flash's per-frame turn wrote over the gun's own vertices and
the heap (a SIGBUS in the GPU vertex cache). The converter writes no more quads
than the list loads (GoldenEye's own models all match: output unchanged).

Probe: `~/wt/gf-guns-run/probe/gfguns.py` (gdb; GUNS, AT, EVERY): every gun on
a stage equipped, photographed and fired. On Cartel all 29 draw their own
model with their own name and clip and fire.

## Bugs found on the way (2026-10-02)

- `rzipInflate1172()` started every 8 KB step at the buffer's start again and
  gave the input 8 KB at most: a 1172 stream inflating past 8192 bytes wrote
  its tail over its own head. GoldenEye's sequences are all under 6 KB;
  Goldfinger's Airport theme is 8368 and crashed in `n_alCSeqNew()`. Perfect
  Dark's own sequences are 1173 and never went through it; GoldenEye X's
  borrowed music did.
- `preprocessALBankFile()` sized its output at `size * 3`; Goldfinger's sfx
  bank takes 3.1x and was written past the end ("malloc(): invalid size").
- `bgBuildTables()` reads section 3 rounded up to 16 bytes, past the end of a
  converted bg file's buffer (a read: a page-boundary crash at worst);
  `fileLoadPartToAddr()` clamps it.
- The aim and face AI commands (0x14-0x17) with GoldenEye's TARGET_PAD flag
  carried their pad raw: a bound pad (10000+) was never moved. GoldenEye's
  own Bunker 2 has one (list 0x40e faces 10027); Goldfinger's Prison, SPECTRE
  Island and Vaults have more.
- `gexPlusMissionAnimLoad()` marked itself loaded before finding a file, so a
  remake stage with none (a hack's arena) turned GoldenEye's animations off
  for the session.

## Open

- Its arenas' weapon sets: `menu/gesets.bin` is written right for it, but GE
  Plus's Combat Simulator appends GoldenEye's only; on its arenas the GE rows
  draw and fire as its guns (the stage's gun set) under GoldenEye's names in
  the menu. Its four extra pistols have no Combat Simulator row.
- Its gadgets' names by mission (gegadgets.c's identities are GoldenEye's).
- Its monitor programmes (its block is edited in place, 16% of GoldenEye's
  words the same; gemonitortable.h is GoldenEye's).
- Judged against ares: not yet. The missions run (20, with openings, 1700
  frames, sound on) and the folder draws; nothing has been compared with the
  cartridge.
