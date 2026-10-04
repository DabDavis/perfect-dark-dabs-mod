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
  own: the intro reel (GE Plus's is GoldenEye's). Its monitor programmes are
  its own since converter 109 (below).
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
- **Texture packs and the dump** (converter 122, 2026-10-04): Dump All writes
  its textures to `texture-dumps/gf64-n64/` (TND64's to `tnd64-n64/`, the
  `tag` of its `g_Layouts` row), `index.csv`'s `goldeneye_image` read from the
  `textures/remap.csv` the converter writes - the one record of which image
  `variantTexRemap()` moved where. A pack's `gf64-n64/` folder repaints this
  hack's levels and nothing else, numbers to 0x0fff (563 of its textures are
  at 0xdaf and up). No model dump for a hack yet. texture-packs.md has it.

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

**Flat-white pistols (converter 105).** Its PPKs, Colt, Luger and P38 drew flat
white where the cartridge draws dark metal (`guns/gunpics.py` photographs a hand
item on the cartridge). Two things, the first the cause:

- Their bodies are sphere-mapped (G_LIGHTING | G_TEXTURE_GEN) on 0ec7/0ec8,
  IA16 images GE Editor stored *uncompressed* (method 0). `texReadUncompressed()`
  stored a 16-bit texel as a host u16, little-endian, where the renderer's
  import reads big-endian, so every texel `00 ff` (black, opaque) read as
  intensity 255. No GoldenEye image is uncompressed and none of either game's
  is IA16 on the channel path; these two are the only ones it reached.
  `PD_BE16()` there now (RGBA16, IA16, RGB15).
- Perfect Dark set the gun's lights and LookAt only for its own
  WEAPONFLAG_00008000 guns, so a GoldenEye gun took whatever the world drew
  last. `gegunsLightsAndLookAt()` gives GoldenEye's own: the weapon envmap light
  for the six items `gunRenderFirstPersonGunModels()` lists (7F062CA8, an
  immediate each, which a hack renumbers: GoldenEye 19 18 2 3 20 21,
  Goldfinger 19 23 2 3 22 21 - its pistols are not on it), the level's
  GlobalLight otherwise, and `camGetLookAt()` either way, in world terms
  against a modelview that ends at the camera as on the cartridge (gun.c builds
  a model's matrices on camGetWorldToScreenMtxf()). The list is
  `romlayout.envmapitems`, written as `geguns.bin`'s mask (GGN2). An eye-space
  LookAt was tried first and put the Colt's highlight on its rear sight.

GoldenEye's own conversion byte-identical; Goldfinger's differs in
`menu/geguns.bin` alone. GE Plus's PP7, Magnum, Golden Gun and knife look as
before (and as the cartridge). Not done: the watch inventory draws the guns
without GoldenEye's watch lights (`set_enviro_fog_for_items_in_solo_watch_menu()`,
whose list Goldfinger left at GoldenEye's numbers).

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

## Against the cartridge (converters 100-103)

The ares run (feat/gf-ares, merged; `twin.py --game gf`, report in
`~/wt/gf-ares-run/REPORT.md`) compared all twenty missions on Agent. The
converter's share of it, fixed one at a time, each with GoldenEye's own
conversion checked against the one before:

- **100, fog.** Goldfinger blanked GE's 12th fog row (Runway +100) to
  0xffffffff, and the fog readers stopped at any id past 0xffff. GoldenEye's
  `fogLoadLevelEnvironment()` walks to the id-0 row and takes the first match,
  and so do they now: ten missions and five arenas had no fog, sky, water or
  draw distance. GoldenEye byte-identical.
- **101, textures a list loads.** A model's lists may load (0xc0) an image its
  texture table does not name; it was remapped and never written, and drew
  with the last texture bound - 57 models, Dink's head the Miami guest's
  white box among them. `modelLists()` adds them; 110 more images. GoldenEye
  byte-identical; `convdiff/missingtex.py` finds none missing.
- **102, head pools.** Goldfinger rewrote `bodyChooseHead()` (7F0235AC): head
  -1 draws from eleven men or six women in its code, -2 from six Korean men,
  each pool from a start its level init draws (7F000F88). The data segment's
  `random_male_heads` it left is sixteen 67s, which every pool guard wore. The
  pools are `romlayout.headpools`, read off the code; `gecast.bin` carries any
  after the first behind the Bond, a guard record keeps -2 as 0xfe, and
  `gexPlusRomOwnHead()` hands a body that asked for a further pool that pool's
  four heads for the level in turn. GoldenEye byte-identical.
- **103, pads under their tiles.** GoldenEye stands a chr, Bond and a floor
  object on the pad's own tile whatever the pad's height
  (`sub_GAME_7F04088C()`); Perfect Dark searches down from the pad.
  Goldfinger has 170 pads more than 5 under their tiles (Forest's start 290:
  Bond fell 29,000 units). `geSoloDoors()` measures each pad on its tile
  (`geTileY()`, stan.c's `stanGetPositionYValue()`) and lifts one more than a
  unit under onto it, but never a pad an object hangs in the air from (flags
  2/4/8: 72 of Cartel's wall objects sit four under, and lifting them moved
  them) or a camera's or autogun's aim. GoldenEye moves nine pads (Surface 5,
  Surface 2 2, Frigate 2); its sweep shows those pads and no object or chr
  moved.

- **104, collectables.** A pickup whose item is no gun and no gadget
  (documents, tapes, glasses, a weapon case, Shipyard's eight gold bars) had
  no weapon number and was never made: Miami's orders, so objective A failed
  at once, and the contents of Club's, Capture's and Ranch's safes.
  `soloCollectablesBegin()` gives each mission's such items Perfect Dark's
  key cards 0x45-0x4c in the order its pickups name them (two at most in a
  mission), so the pickup, a rename, an objective and an AI command all take
  the same number; a rename naming only its object takes its collectable's.
  `menu/geslots.bin` ("GES1", u16 rows, {mission, weapon, item, 0}) says which
  item each stands for. The key cards share one definition, so gegadgets.c
  names each by a text override of its number (inserted as
  `setupCreateProps()` empties the list, so the setup's own renames go
  before): the hack's gun table's short and long names out of its LgunE
  (Goldfinger's orders are its "Folder"), title-cased, and GoldenEye's own
  pickup words for an item it has none for, "Picked up a new weapon." The
  watch neither draws nor equips one. GoldenEye's own Silo has one: the
  briefcase character 0 carries, now dropped as a pickup when it dies, as the
  cartridge's record says.
  Against the cartridge (`world/collect.py` + `collectdiff.py`, Bond walked
  onto each on both sides): all twelve missions with collectables (Cartel,
  Miami, Shipyard, Club (its safe's spool), Airport, Alps, Capture, Ranch,
  Hideout, Knox, Plane and Crab Key) make, carry and complete the same objectives at
  each pickup. Two that look different and are not: Airport's ticket (record
  97) is on a guard on both sides; Plane's record 105 is assigned to chr 4,
  whose gun takes his hand first, so the cartridge's `chrEquipWeapon()` turns
  it away unparented - a prop at the origin in no room, which nobody reaches
  and nothing drops - and ours never makes it.

- **106, objects on a pad with no tile.** GoldenEye makes every object but a
  door through `domakedefaultobj()`, whose `getposstan()` makes nothing when
  the object's pad (or bound pad) has no tile - the named tile does not hold
  the pad and none walks to it (`init_pathtable_something()`, the
  converter's `gePadTile()` -1). An object inside another (0x8000) or a
  chr's (0x4000) is not placed by its pad and is made regardless. Perfect Dark
  stood such an object on the first floor below its pad: two cups on each of
  Cartel (records 1155, 1160) and Bodega (705, 706) the cartridge never makes,
  a storey down under their table. `geSoloDoors()` marks them and
  `writeSoloProps()` sets flags2 0xf0 (excluded on all four difficulties);
  the converter logs each ("GoldenEye never makes it"). These four are all of
  them; GoldenEye's own missions have none, and its conversion is
  byte-identical.

The replay test was the same on all eight cases after each.

**Walls drawn under coverage for alpha (fast3d).** GE Editor writes its rooms'
lists under G_RM_AA_ZB_OPA_TERR2 (`0xc8102078` with fog, `0x0c182078` with
G_RM_PASS): ALPHA_CVG_SEL, no FORCE_BL, and the blend formula's "memory times
one minus alpha" - with a combiner whose alpha is nought outright. The RDP
blends a covered pixel only under FORCE_BL, and takes coverage for the alpha,
so the console draws them solid; `gfx_derive_batch_state()` saw the blend
formula and blended by the combiner's 0, so Cartel, Bodega, China and Crab Key
drew no walls or floors. Such a draw is now opaque (`gfx_cc_alpha_zero()`):
only a combiner alpha that is constant 0, never a texture's or a vertex's, so
nothing else moves (Defection's release rooms need their texture's alpha under
that mode; Extraction's lit spans were mended at the vertex, 5cb512e2e).
Scanned in both ROMs: GoldenEye has no room list and no model it applies to;
Goldfinger has ten level files (Cartel's 10,851 triangles) and parts of nine
models (707, JetStar, lockers, cooling tanks). A runtime census of Perfect
Dark's ten solo stages in both looks found none. Against the cartridge (view
diff medians, before -> after): Cartel 0.49 -> 0.18, Bodega 0.95 -> 0.15,
China 0.30 -> 0.13, Crab Key 1.15 -> 0.16, Grounds 0.49 -> 0.15, Alps 0.10
unchanged - 519 of 741 views closer, none further. The ares run's crude patch
(every room's combiner alpha rewritten) darkened Grounds' and Alps' terrain;
this leaves Alps alone and brings Grounds closer.

**Cartel's club as bright as the cartridge (converter 107).** After the
coverage fix the club still read dark: the ceiling drape (lit and texgenned)
dark with bright streaks where the cartridge has a light-grey sheet, and the
walls round the door at under half the cartridge's brightness. Four causes,
each measured on the cartridge (tools/gefidelity/view/texsample_ares.py
overwrites an image in RDRAM with a ramp, or fills it white, so a surface's
brightness reads off its texel or its shade; ramfind_ares.py and
ramdump_ares.py look in RDRAM):

- The drape's image is not 0x28c (that guess was wrong): the draw state's own
  texture is a 64x64 I4 sky map, bright in its top and bottom rows, the same
  bytes in the cartridge's RDRAM (mip levels too). Under the row ramp the
  cartridge's whole drape is one grey: every corner on the same row.
- `roomHighlight()` scales each room colour so its largest byte is at most the
  room's brightness (210 there). A lit vertex's colour is its normal, and a
  component past 210 (a small negative, 0xd3-0xff) was scaled down to it -
  the logged normals full of -46 (0xd2). The conversion now flags every vertex
  its list loads under G_LIGHTING (vertex flags 0x01, the bit roomHighlight()
  already reads; a converted room keeps one colour a vertex, so the colour
  index it uses is right), and on a converted level such a colour is copied as
  it is. GoldenEye's own conversion changes only those flags: Surface 118
  vertices, Bunker 118, Egyptian 39 (its few lit, texgenned room spans).
- GoldenEye lights a level with GlobalLight (ambient 150, white from 77,77,46)
  and the camera's LookAt. Perfect Dark lit a converted room by the room's
  brightness; `lightsSetForGeRoom()` (dlights.c) hands it GlobalLight and the
  LookAt. (First written turning both out of eye space by the camera's axes:
  WRONG, the cartridge takes both in world space - see "Shade across the
  screen, and the room light's space" below. The drape cannot tell the two
  apart; its gain here was GlobalLight's ambient.)
- Mod.LevelReflectFollow (on by default) bent each texgen lookup by the eye
  ray; a converted level's rooms no longer take it. (The earlier note that it
  made no difference was wrong: twin.py runs on a fresh save dir, so the ini
  setting never reached the run - the two pictures were identical. Under gdb,
  `roomSheenSetStockFollow(0)`.)

The door wall was the fourth: its I4 texture hands its intensity on as the
combined alpha, and under the same coverage-for-alpha mode we blended by it
(the cartridge, filled white, shows the shade alone at 0.93, ours drew texel x
shade x texel). `G_COVERAGE_ALPHA_EXT` (gbiex.h), set over the whole scene of
a converted level in the N64 look (`bgRenderScene()`), makes any combined
alpha pass over as the RDP does there; Perfect Dark's own stages and the HD
looks keep the narrow rule above. And a converted room's vertex colours are no
longer capped at its brightness in the N64 look (the HD look's rule in
`roomHighlight()`, taken against the room's full brightness, so a shot light
still dims): Cartel's floor tiles were capped at 210.

The club view (pad 134, heading 270), cartridge / before / after, picture
medians: drape 131 / 32 / 120, door wall 111 / 42 / 110, left wall 51 / 23 /
49, pillars 46 / 21 / 45, floor 46 / 37 / 43.
Against the cartridge (view diff, the cartridge's pictures reused, a pair
"closer" or "further" by more than 0.02): Goldfinger 64's six missions from
the coverage fix, Cartel 0.181 -> 0.147, Bodega 0.148 -> 0.141, China 0.127 ->
0.092, Crab Key 0.164 -> 0.165, Grounds 0.151 -> 0.112, Alps 0.103 -> 0.103;
198 of 741 views closer, 3 further (Crab Key at heading 90, +0.02-0.03).
GoldenEye's own (the same tree before and after, eight missions): Surface,
Surface 2, Bunker, Bunker 2, Facility the same or a hair better, Egyptian
0.102 -> 0.086, Caverns 0.063 -> 0.059, Dam 0.103 -> 0.105; 34 of 809 views
closer, 5 further. Split with both changes switchable, the colours uncapped
make Caverns' gains and Dam pad 163's loss; the coverage rule barely moves
GoldenEye except Caverns' lake. The views further each had a second
difference the old blending happened to hide:

- Caverns' lake (pads 289-307, heading 90): one huge triangle, a corner black
  4000 out, the rest teal. The RDP carries shade linearly across the screen,
  which darkens most of it; our renderer carries it with perspective, so it
  stays teal. Blended by its texels before, it read dark by accident. The
  port already carries fog linearly on a converted level (G_FOG_VERTEX_EXT);
  the colour is not.
- Crab Key's orange pillar (pad 85, heading 90) is NOT a prop (every setup
  object moved away, it stays): a lit, texgenned room surface, dark under the
  eye-space light. Fixed below.
- Dam pad 163's walls, colours uncapped, at 59 where the cartridge has 47.

**Miami's banner is there.** The ares run's "Miami's blimp has no banner
trail" was the opening's random shot: Miami has two (`gecinema.c` picks one
by `rngRandom()`), the cartridge showed the sky over the hotel and ours the
corridor. Held on the same shot (`g_GeIntroShot = g_GeCinemaShots[0]` under
gdb), ours shows the small plane (`PplanewelcomeZ`, record 614) towing "WELCOME
TO MIAMI BEACH" past the airship at the same ticks. The banner is the plane's
second list (mode 4, the XLU pass), loading matrix 0 itself as the body does.
Still different: the cartridge's banner reads as a pale, fogged streak and
ours as thinner, darker letters - fog on a translucent prop list, not
anything missing.

## Shade across the screen, and the room light's space (2026-10-02, later)

**Caverns' lake:** `G_SHADE_LINEAR_EXT` (gbiex.h), set with
`G_COVERAGE_ALPHA_EXT` over a converted level's scene in the N64 look
(`bgRenderScene()`), makes the combiner's inputs `noperspective` in both
renderers (SHADER_OPT_SHADE_LINEAR, bit 31 of the options; GLSL ES keeps
perspective) and cuts a triangle crossing the RSP's clip volume on the CPU
first, as `G_FOG_VERTEX_EXT` does (`gfx_emit_tri3()`; rooms inside the volume
keep the GPU path, models under it go to the CPU). The lake now as dark as the
cartridge's. Views from converter 107: Goldfinger 69 of 741 closer, 1 further;
GoldenEye 116 of 809 closer, 1 further (Caverns 0.059 -> 0.052, Egyptian
0.086 -> 0.068, Dam 0.105 -> 0.089). GL and Vulkan alike. c70068cc1.

**Room light in world space.** Crab Key's copper pillar read 86 (red) where
the cartridge draws 154. Its triangles (a pixel-pick log in a debug build:
every triangle over one screen point with its state, textures and vertex
normals - not committed) are lit and texgenned, normals about
(0.70, 0.17, 0.69), a 32x32 CI8 sphere map; ours lit them by ambient alone.
On the cartridge, the palette filled white (`texsample_ares.py`, GF_FILLLEN)
shows the shade at 255. `lightpoke_ares.py` rewrote GlobalLight (0x80044840;
a second copy of the bytes at 0x8002a970 does nothing to rooms) to ambient 0
and one light of 200: +x 132, -x 0, +z 142, -z 0, the camera turned 35 degrees
+z 143 and +x at half strength 66 - world space. bgLevelRender() sets lights
and LookAt before it loads the camera's matrix. The LookAt goes the same way:
A/B over nine missions (VIEW_SET='g_DbgGeLightSpace=n', a debug switch),
light in world 4 views closer on Crab Key and nothing else moved; LookAt in
world too, 4 more closer (Cartel's club 134/270 -0.038: its black pillars
reflect the cartridge's streaks), none further. `lightsSetForGeRoom()` now
hands GlobalLight and `camGetLookAt()` over as they are - which also agrees
with the guns (converter 105: an eye-space LookAt was wrong there too).
Pillar now 147 against 154. Final sweep against the shade change alone:
Goldfinger 7 of 741 closer, GoldenEye 2 of 809 (Egyptian), none further. Both
changes from converter 107: Goldfinger 75 closer, 1 further (Cartel 236/90,
+0.03, a wall's gradient); GoldenEye 118 closer, 1 further (Dam 269/180,
+0.03). Crab Key's median 0.165 -> 0.151. Replay test 8 of 8 the same.

Trap: lightpoke's first picture after `orig` was once a frame stale (read the
same as orig); put a throwaway setting first.

## Levels of detail, parked cars and Oddjob (2026-10-03)

**Crab Key's grille (pad 85, heading 0).** A room surface, not a prop: an 8x8
CI8 cell (a white frame round transparent black, mipmapped, G_CC_TRILERP under
the TERR render mode) repeated 64 times, in front of the wall. Up close both
sides draw a dark mesh; from a few steps back the cartridge's is white and
ours stayed dark. Two causes, both measured on the cartridge:

- The levels' bytes. GoldenEye makes a texture's missing levels with its own
  shrink, whose palette search is a binary search over brightness that assumes
  the palette sorted, then four entries either side; this palette is not
  sorted, and each half-transparent 2x2 lands on entry 32 (0xffff). The
  cartridge's RDRAM (view/ramfind_ares.py, ramdump_ares.py; the image's odd
  rows are word-swapped there, search for row 0 then row 1 swapped) holds
  white levels from 4x4 down, Perfect Dark's shrink made dark ones; ours
  written into the cartridge's RDRAM (texsample_ares.py GF_PASSES) turned its
  grille from 88 to 15. getexshrink.c is GoldenEye's code (decomp image.c),
  used for a texture read out of a ROM conversion's folder
  (modloaderDirIndexIsConversion()). Perfect Dark's own non-paletted shrink
  reads big-endian texels as host words - garbage levels on PC, unseen while
  nothing drew them; GoldenEye's port here loads and stores big-endian.
- Nothing drew the levels. fast3d uploaded level 0 and let the GPU make the
  rest. G_TEX_OWN_LODS_EXT (bit 0x2: an extra geometry mode command clears
  bits 24 and up), set with the other two RDP modes over a converted level's
  N64 scene, uploads a converted texture with its tiles' levels
  (gfx_import_own_lods(), upload_texture_levels() in OpenGL and Vulkan,
  LoadedTexture.block_bytes, the texture cache key's own_lods). Perfect Dark's
  models in the same scene keep the made levels (texpackTextureIsConverted()).

Painted level by level, the cartridge samples level 2 for 82% here and level 1
for the rest; at 640x480 ours samples one level finer (the GPU picks the level
at the screen's resolution), so the grille reads 46 against the cartridge's
88, up from 25; view score 0.234 -> 0.171. A level-of-detail bias of
log2(height / 240) on converted levels would match it at any resolution, at the
price of the N64's blur at a distance - not done. Views from the light-space
commit: Goldfinger 4 of 741 closer, none further; GoldenEye none either way,
medians a hair better. 226c37b77, e2046f61d, c30c60d9a.

**Parked cars (#6).** GoldenEye stands every object, a parked vehicle too, on
the ground under its pad by the box chrobjGetBboxFromObjFile() takes (the
root's children, then its first child's). Perfect Dark's floor placement took
the first box anywhere, which on Goldfinger's cars is a wheel's under its
position node: Mercedes (Pgx309Z, 26 of them) 53.7 into the road, covered cars
(Pgx297Z) 136 over it, Rancheros (302) 17 in. A parked one is never re-seated
(gexplusveh.c's first-tick height is for a record flagged as moving).
func0f06a650()/func0f06a730() now take objFindBboxRodata(), which already
answered GoldenEye's box for a truck or aircraft on a mission. World diff: every
car and Miami's airship agree; Miami's plane differs at tick 1 by the flight
step GoldenEye took before its first dump. GoldenEye's seven vehicle missions
unchanged. 77cc8ddf1.

**Vaults' Oddjob (#7).** lvRender() ticks no prop for a level's first five
frames (lockscreen), while chraTickBg() ran the background lists from the
first; GoldenEye runs both from frame 1, background lists first. Vaults'
background list sent Oddjob to his last list before his own first ran (frame
6), so his health/armour/accuracy were never set (4/0/0, cartridge 50/200/100).
On a converted mission the background lists now wait while lvHoldsLevelStart()
holds the props. World diff over all 40 converted missions: Vaults 23 -> 18;
tick 300 otherwise unmoved but for Miami's two guards whose lists list 4098
deals at random (command 0x37); five missions' tick-1 dumps now lack flags the
cartridge's background lists set on its frame 1 (agree by tick 300). 65688853f.
Trap: twin.py passes `--rng-seed 1` first and the port takes a repeated
argument's first value, so `--pd-arg=--rng-seed --pd-arg=N` does nothing - a
"same under every seed" check needs twin.py's own argument changed.

## Crab Key's copper doors (2026-10-03, later)

The "copper walls" of Crab Key's worst views (pads 43, 31, 25) are doors:
prop 69 at pad 310 (model 672, scaled 0.45 x 0.27 x 0.16) and its kind, two
faces 4.7 units apart, culling off. Each face is two passes: a lit, texgenned
32x32 CI8 sphere map whose palette is the copper (G_CC_TRILERP x shade), then
a 64x64 I4 overlay of rivets and seams in copper, a translucent decal (texel =
its own alpha, transparent but for the rivets and seams).

**Measured on the cartridge** (texsample_ares.py, the door's images found by
their first bytes): every texel white gives the shade alone - 153/177/194 down
the screen against ours 151/177/192, so lighting and normals agree. The new
GF_CI ramps (texel = its row, then its column, over a grey palette) give where
each pixel reads the sphere map: the cartridge s 0.7-1.6, t 13.3-10.8; ours s
10.5-13.8, t 19.7-12.7. A pixel-pick log (probe build; the command ring before
the door's triangles) showed the door drawn under G_TEXGEN_EYE_EXT |
G_TEXGEN_TURN_EXT: objRender() wrapped every prop in roomSheenStockBegin(),
Perfect Dark's Level Reflections walk, which bgRenderRoomOpaque() already
leaves off a converted level's rooms. Off for props on a converted level (and
for a broken pane's shards, which follow their pane), the plain texgen under
the LookAt as it is reads s 0.7-1.7, t 13.4-11.1: within 0.2 of a texel at ten
of eleven points (the eleventh is under the gun). The door's colour down the
screen 80/75/65/54 red against the cartridge's 80/76/65/56 (ours was
94/94/90/79), and the streak is gone. Views against HEAD: Goldfinger 16 of
741 closer, none further (the doors 0.353 -> 0.157, 0.339 -> 0.176, 0.354 ->
0.202; Grounds pad 57, heading 270, 0.952 -> 0.202, washed pale before);
GoldenEye 28 of 809 closer, none further (Dam's median 0.087 -> 0.079,
Facility 0.064 -> 0.059, Bunker 0.093 -> 0.086) - GE Plus's reflective props
had the walk too. Replay test 8 of 8 the same.

Still different: the rivet overlay. Filled solid on the cartridge it draws on
one of the near face's two triangles and not the other, the edge between them
exactly the line where it stops; ours draws it on both. The overlay's corners
are the base's to five decimals in our log, and where it fails changes from
view to view in the cartridge's pictures (pad 25 shows most of it, pad 31 a
band at the left) - the RDP's decal test (|z - z_mem| within the larger dz,
z_mem stored at 14 bits) against a face 70 units away whose dz is about the
stored z's step. Matching it means the RDP's depth arithmetic, not done.

Trap: the cartridge keeps the sphere map's palette 1400 bytes past the image,
ours 1392 (its levels hold 8 bytes more, perhaps a 1x1 level; not checked).
texsample_ares.py's GF_CIPAL says where; ramfind_ares.py finds a palette by its
first entries. texsample_pd.py is the same instrument on our side.

## Its weapon sets (2026-10-03)

Its mode's Combat Simulator (and the folder's multiplayer) lists its own
fourteen sets out of its `menu/gesets.bin` - "Hats 'n' Clubs", "Gangster",
"Limpet Mines" - where it listed GoldenEye's. `gexplus.c` keeps a group of sets
a conversion (`geSetsGroup()`, read once a mod dir) and one block of the list
that holds the mode's: GoldenEye's at boot and whenever no hack is chosen, the
hack's while `g_GexPlusVariant` is (`geSetsPut()`, over the block in place).
They are not rows of their own after GoldenEye's, which Perfect Dark's
Combat Simulator, listing the whole list, would offer too.
`mpSetGexPlusMode(false)` puts GoldenEye's back. A set chosen in the block is
applied again when the block changes hands, so GE Plus's "Pistols" becomes
Goldfinger's "Pistols", not GoldenEye's slots under its name.

- **Names.** The menus name a GoldenEye gun by the mode's gun set
  (`gegunsMenuNameId()`, asked by `mpGetWeaponLabel()`): its AK47 where
  GoldenEye has the KF7 Soviet, its Golf Club on the Hunting Knife's row.
  Between matches the menus are over the Carrington Institute, whose gun set
  is GoldenEye's (`gegunsStageSet()`), so it follows the mode, not the stage.
- **Its four pistols** have Combat Simulator rows now (`MPWEAPON_GE_EXTRA1`,
  0x4a-0x4d, after GoldenEye's 25, so no saved index moves; 7 bits in a setup
  file). `gegunsExtraRowsRefresh()` shows them where a hack's set names them
  (its stage, its mode's menus) and hides them everywhere else, at a stage's
  gun set, at `mpSetGexPlusMode()` and after a mod swap. A mission's Start
  Armed roll skips them unless a hack's set is in (`gegunsHackSetIn()`). The
  randomizer still rolls only the rows before them (`MODRANDOM_MPWEAPONS`), so
  a seed deals what it dealt. On Junkyard its "Gangster" set lays its two Smith
  & Wessons on their own props (Pgx230Z/Pgx231Z), picked up and fired.
- **Bug found:** 5049fc076 made a weapon prop's `dualweaponnum` a u8 with 0xff
  for none, but `invGiveWeaponsByProp()` still tested `>= 0`, always true: every
  pickup in a match tried to give a pair with weapon 0xff ("Double Colt
  M1911", Perfect Dark's own guns too). It tests `!= 0xff` now. `dabs-mod` never
  had it.

Probes: `~/wt/gf-sets-run/probe/sets.py` (every mode's sets, options and
slots) and `match.py` (GF_SET, GF_PICK: a set on Junkyard, what lies on the
floor, one picked up). The rig needs `Mod.MapMods=GoldenEye Arenas;Goldfinger
64` in its pd.ini, or the hack's arenas are not mounted and its mode lists
nothing.

## Its gadgets (converter 108, 2026-10-03)

gegadgets.c knew GoldenEye's gadgets by GoldenEye's missions' numbers
(`g_Identities`: mission 1's gadget A the door decoder, 6's the bomb defuser,
the tracker bug on 6), and a hack's missions are others in the same slots. Its
items past the hand's keep GoldenEye's numbers, many renamed: 34 is its
"Explosive" (watch text "timed explosive"), 47 its Homer, 50 a Canister Filter,
73's long name Shipping Records. So its mission 0 handed Bond "Plastique", 3 and
6 a "Covert Modem"/"Tracker Bug", 14 a "Gadget".

- **Converter 108** writes, for a variant only, rows in `menu/geslots.bin`
  beside the collectables': the item each gadget weapon (0x77-0x7f) is on a
  mission, recorded as the mission's conversion names it (`soloGadgetUse()`,
  from `soloItemWeapon()`), and after them rows of mission 0xff for a weapon one
  item alone is (`soloGadgetsEveryMission()`) - and two its code hands out that
  no item table names: the detonator, the item whose file is GtriggerZ
  (Goldfinger's 31, GoldenEye's 30), and the tank's shells, 32. GoldenEye's own
  output is byte-identical; Goldfinger's differs in `geslots.bin` alone. Its
  uses: 0 Explosive, 3 Homer and DAT tape, 5 and 9 the camera, 6 Homer, 14
  Canister Filter.
- **At run time** a hack's stage (`g_HackGadgets`, `gegadgetsSlotsLoad()`)
  takes each gadget weapon's item from those rows, the mission's before 0xff
  (an arena, which has no mission, the 0xff rows: its Claymores set's
  detonator),
  and names it by the item's watch text, each word's first letter up, out of
  its own `geitems.bin` and `LgunE` (the long name where there is none).
  `gegadgetsItem()` answers by it, so the watch and the hand use the hack's
  item: its detonator in the hand was GoldenEye's item 30, which is its
  Claymore's model, and drew an empty hand; now Bond's hands at his watch.
  GoldenEye's own stages are as they were.
- The key analyser runs on its item (46), not on gadget A of mission 4 -
  Goldfinger's mission 4 is not Bunker.
- `g_Identities` had GoldenEye's tank shells as item 33, which is the bomb case
  (ITEM_TANKSHELLS is 32, bondconstants.h); 32 now.

Probes: `~/wt/gf-sets-run/probe/gadgets.py` (each gadget weapon's item and
name and the inventory, GF_AT frames in) and `det.py` (the detonator in the
hand, then as item 30). A rig converts again only when its CONVERT.txt's
version is older: set it back to force one, and let the game run without gdb
until it says the new version - a probe's breakpoint lands mid-conversion.

## Its monitor programmes (converter 109, 2026-10-03)

Until 109 a variant's conversion wrote no `menu/gemonitors.bin` (its programmes
are not GoldenEye's), so every screen in its missions showed nothing - Club's ten
fireplaces among them: Goldfinger burns their fire as monitor programme 7.

- **The block is where GoldenEye's is**, 0x80030b74, 1334 words, and decodes
  end to end; Goldfinger rewrote its programmes in it (15% of their words
  GoldenEye's), ending at word 969, and filled the rest with yields (0x0c).
  Jumps stay inside it.
- **Where each starts** is `monitorSetImageByNum()` (7F049C98): 52 cases
  through the jump table at 0x80052b98, each `lui a2,0x8003` / `addiu a2` (the
  last one falls through, no branch). The code is GoldenEye's shape; every
  address but programme 0's moved. `romlayout.monprograms` holds them as word
  offsets, read off the code (`~/wt/gf-work/monscan_gf.py` decodes both ROMs);
  an empty row is gemonitortable.h's.
- **Its pictures**: `s_monitorimages` sits in the ROM's global image table
  (`_GlobalimagetableSegment`, copied raw by `texReset()`), at ROM 0x29debc in
  both games (`romlayout.monimages`). Goldfinger changed nine of the fifty rows:
  25 and 26 are new images (2948, 2949), seven another size or format (picture 6
  is a 3x11 RGBA32, picture 29 says RGBA at 8 bits - harmless, `texSelect()`
  takes format and depth from the loaded image, as GoldenEye does). Its
  programmes use pictures 10 and 11, which GoldenEye's never do, and not 48.
- GoldenEye's own conversion is byte-identical; Goldfinger's gains
  `gemonitors.bin` and 48 pictures, nothing else moves (no reserved number).
- **At run time** nothing new was needed (gemonitor.c reads any counts) but
  two things: the HD look's release pictures (`g_GeMonBean`, by GoldenEye's
  image number) never bind over a hack's (`g_GeMonHack`), and the folder's
  Monitor Programmes page captions a hack's programmes "N of 52" without
  GoldenEye's names, which would misname them.
- **Against the cartridge** (ares, `viewdiff.py run --only/--heads` from the
  pads nearest each screen): Club's fireplaces (pads 205, 264, 390; scores
  0.03-0.13, fire in both, the flames' frame differs with the tick). Crab Key's
  eleven `PtvflatZ` on programme 26 (the new pictures) face an area no pad
  frames - from pad 90 at heading 50 both show their black backs. Miami's
  screens are a floor above every pad near them. Per-mission screens on the
  cartridge (`wide.props` imagenum in the ares sweep's world dumps): capture 9,
  cartel 3, china 10, club 10, crabkey 13, foundry 5, knox 2, miami 11, prison 3,
  vaults 3.
- Seen on the way, not from this (there without `gemonitors.bin` too): from
  Crab Key pad 247 (outside, heading 8-20) ours draws a thin pink strip in the
  sky that the cartridge does not.

Rig `~/wt/gf-mon-run` (`monpages.py`: the folder's page, `GF_VARIANT=0` for
GoldenEye's, `GF_TAG`; `view-mon*` the view runs), `~/wt/gf-mon-nomon` (the
same without the file).

## F3 pass 30 gameplay (2026-10-03)

- **Cartel's first silo** (F3 20261003-051322, "hitbox too thick"): a sliver
  tile (399/400, y 150, linked to nothing) lies along the top of the silo wall
  across the doorway in plan. gestan's stanTileUnder() picks the tile whose
  height is nearest the foot's limit, so in the doorway the sliver was the
  player's floor and all its walls stood round him. GoldenEye walks Bond's tile
  through the links (bondviewTryMoveToStan()). Fix: the player's own cylinder
  tests (bwalkCalculateNewPosition, bwalkTryMoveUpwards, bwalkCanMoveUpwards)
  set geStanSetMover(); stanWallFind() then takes geStanFloorAhead()'s walked
  tile (or the one the move started on) when it holds the point at a footable
  height. The doorway's open door turns about its middle on the cartridge too
  (same picture) and Bond slides along it. Probe: tools/gefidelity/probe-gfplay
  door2.py (untracked); walkcmp.py showed GE Dam/Facility/... walks unchanged.
- **Cartel opening** (F3 20261003-061541): stage_intro_anim_table ids went
  untagged to gexPlusMissionAnim(), so rows 97-105/176 played Perfect Dark's
  dodges/flinches. gecinema now asks geChrAnim(). Affects GE Plus openings that
  pick rows 2-8 too.
- **Joanna in third person on Ranch** (F3 20261003-071337): g_GeRomRows shared
  one cap of 24 between bodies and heads; Ranch's 12 bodies + 12 own heads used
  it up and gexPlusMissionBond() got no row. Now 24 bodies + 128 heads.
- **Oddjob's hat** (F3 20261003-071520): the big black hat lower right is the
  hack's item 3 (GthrowknifeZ) in the hand; the cartridge draws it the same.

## Its sounds and explosions (converter 111, F3 pass 30)

Its sound bank keeps GoldenEye's 261 numbers and replaces 41 samples in
place (`probe/ctl.py` in the session: 3-6, 12, 16-17, 50, 55-57, 63, 74,
85-89, 95-97, 100-101, 109-121, 166-167, 205-207, 233, 248-250, 256; 5, 6,
16, 17 and 205-207 with a 66-byte silent raw wave). Nothing is renumbered,
and its code plays the numbers GoldenEye's does (the reload's
GUN_RIFLECOCK, 50, for every gun item from 4 to 26 but the laser: the jump
table at 0x80054294). Two faults, both its own:

- **Shots on the remap's reserved slots.** gesfx.c's remap leaves 2, 7, 9,
  16, 43, 55, 100, 101 and 245 Perfect Dark's, since its own code plays them
  for a meaning of its own; no GoldenEye gun fires with one. Goldfinger's
  Luger fires with 100 (GoldenEye's taser, PD's shield damage), its MP40
  with 101 (PD's looped laser stream: "electricity constantly") and its M1
  Carbine with 55 (PD's "no sound", dropped by sndStart()). Its P38 and Uzi
  use 56 and 57, which remap. `geSfxGunShot()` now hands a reserved slot's
  shot as the bank's own appended number, and `geSfxHeardAsRemapped()`
  gives that number the remap's volume share and GoldenEye's falloff
  (n_sndplayer.c, propsnd.c). GoldenEye's guns never take the branch.
- **No prop exploded.** The layout's `object_explosion_details` was
  0x8070f028, a block of mostly -1 rows; the code loads the table from
  0x8070dd28 (the lh at 7F04CE6C, GoldenEye's 0x8003b224; the crumple
  seeds' sites at +2 and +8). 149 of its 416 props explode now; GoldenEye's
  output is byte-identical (only Goldfinger's `menu/geexplosions.bin`
  changes).

- **Most of its shots were still silent (F3 pass 31, 20261003-163557).**
  34 of its re-recorded samples carry an 11.1 s envelope decay (11148977
  us; 3-6, 12, 16-17, 43, 50, 54-56, 74, 79, 87, 89, 95-97, 100-101,
  109-113, 115-116, 121, 160, 205-207, 233): the shots of the Colt, Luger,
  P38, AK47, MP40, both Thompsons, M14, Remington, Over/Under, Kar98k,
  Carbine, AR7, both S&Ws, the Golden Magnum and the M79, and the reload's
  50. Perfect Dark's n_sndplayer.c stops a sound whose attack, decay or
  release is past 5.5 s (`delta > 5500000` -> func00033090) the moment it
  starts; GoldenEye's snd.c has no such ceiling. "STARTED ok" at
  func00033820 says nothing: the voice is cut in the PLAY event. Now
  `sndpEnvTooLong()` clamps the phase to 5.5 s for a sound out of a
  converted bank (`geSfxIsBankSound()`, numbers sndAppendSound() gave
  gesfx.c) and keeps the stop for Perfect Dark's own. GoldenEye's own bank
  has no phase that long, so GE Plus is unchanged. Test: SDL_AUDIODRIVER=disk,
  gdb `sndStart(0, geSfxGet(id), ...)` 1.5 s apart, RMS per 0.1 s.

Probe: `snd.py` (gdb, the session's run dir): fires GUNS, forces a reload,
logs every sound the player starts (n_sndplayer.c:714/730), then damages
explosive props and counts explosion props.

## Its HUD, watch and folder look (F3 pass 30, 2026-10-03)

- **Ammunition pictures.** gehud.c drew GoldenEye's `ammo_related[]` rows
  (image, size, format) from a table of its own. Goldfinger 64 redrew most of
  them at other sizes in the global image table (segment 2, ROM 0x29d160 in
  both ROMs, 12-byte rows): its 9mm round is 4x13 where GoldenEye's is 5x12,
  the magnum's 4x14, the knife's (Oddjob's hat) and the mines' 16x16 - and
  gave AmmoType 2 (its M1 Carbine and S&W Model 22) image 2237 and type 14
  image 2193. Drawn through GoldenEye's boxes the 9mm came out a smear with
  the next row's colours over its tip (F3 20261003-050551, 20261003-061317).
  The conversion now writes `menu/geammo.bin` for a variant ("GEA1", a row a
  type: image as written, the row's seven bytes, IconYOffset's float), read
  at the data segment's 0x80035ef0 through the image table, and adds the
  images it names; gehud.c draws a gun's type from it (`ICON_OWN + type`,
  `hudIconBox()`), HUD and watch alike. GoldenEye's conversion writes none
  and keeps its table.
- **The watch is yellow.** Its code (7F0A3420-7F0AD000, mpmenu.c's pause
  and `gunDrawWatchAmmoDisplay()`'s 7F069C3C) has every green of
  GoldenEye's watch patched to red = green: 0x00ff00b0 -> 0xffff00b0,
  0xa0ffa0f0 -> 0xffffa0f0, the face's ramp (`sub_GAME_7F0A33F8` gives its
  red the green's value), the page rectangles, the slider bars, the pulses
  stepped 0x10100000. Left green: the static scanline, the ring's prim colour
  and the objectives' INCOMPLETE pulse, whose red is 0 (0x000040ff | pulse <<
  16). It left one of the two 0x00800050 bars (7F0A8BE0) green; both are
  tinted here. `romlayout.watchtint` -> `menu/gewatch.bin` ("GEW1", the tint);
  gewatch.c's colours go through `watchTint()` (F3 20261003-050712).
- **The folder in the cartridge's look.** The hack's folder came up on the
  release's paper, Brosnan photo and fonts where the release was installed:
  gefolder.c's gates (repaint, menu pictures, fonts, backdrop, Bean folder)
  are `folderHdLook()`, off while `g_GexPlusVariant` is chosen unless the
  stage is GE Plus's own (F3 20261003-051445). Its folder is now its own
  Connery folder in both looks (the HD look's
  picture pixel-identical to the N64 look's).
- Checked against the cartridge, nothing to change: Bodega's corridor sconce
  has no animation (four ticks apart, the same picture); its candle is a
  translucent quad on image 0x28c, white head-on in both, but from a steep
  side ours shows a grey streak above the candle where the cartridge does
  not. Grounds' black car's windows are the same blocky checker on the
  cartridge.

## Open

- Props' shading against GoldenEye's: no measured case now (Crab Key's
  pillar was a room). Club's fireplace surrounds read darker in ours than on
  the cartridge (pad 264 heading 268) - a candidate.
- Crab Key pad 247's pink strip in the sky (above).
- The watch draws nothing for a collectable where GoldenEye draws its model
  (converter 104's are checked against the cartridge otherwise).
- Crab Key's copper doors: the rivet overlay, a decal the cartridge's RDP
  drops on some triangles by its depth precision (above). Close-up flat walls
  also score high on edges from the cartridge's 16-bit dither, which our
  pictures lack, and pad 97's from the gun caught at another point of its sway.

## Watch gun rows and Monitor Programmes sets (F3 pass 31, 2026-10-03, fix/f3-1003c-watch)

- **Watch gun too large (F3 20261003-103819).** gewatch.c kept
  `menu/geitems.bin` from the session's first stage: after any GE Plus stage a
  Goldfinger gun was placed by GoldenEye's row of its item number (the M1
  Garand, its item 19: 1950 from the camera, GoldenEye's 19 is 629), and after
  a Goldfinger stage GoldenEye's PP7 was named by the hack's rows.
  `watchGunItemsEnsure()` reloads by `g_Watch.moddir` (and drops the loaded
  models and the own-model boxes). Probe: `~/wt/f3-1003c-watch-run/gfwatch2.py`
  (`STAGES=0x60,0x7d`, `GUN=`; Cartel is 0x7d there).
- **Monitor Programmes sets three times their cells (F3 20261003-123824).**
  Its Pgx075Z is its own set, box 1250 x 1000 against GoldenEye's 408 x 386.
  `frontLoadTvs()` fits the box to GoldenEye's (`g_TvFit`, 1 for GoldenEye's)
  and centres it. Probe: `~/wt/f3-1003c-watch-run/mon.py` (`GF=1`, `LOOK`).

## F3 pass 32 (2026-10-04, fix/f3-1003d-gfgame)

- **Hats ring like helmets (F3 20261003-195003, converter 120).** Its code
  keeps get_hat_model() (7F052684) and deals the jump table at 0x800532f4
  anew: 212 and 217 helmets, 213/216 fur, 214/218/221-223 peaked, 215 side
  cap, 219/220 berets; the default `li v0,-1` became 4. Cartel's guards wear
  218 (GoldenEye's grey helmet). `writeHatTypes()` -> `menu/hattypes.bin`
  ("GHT1", first, count, kinds), read by `gexPlusHatType()` from
  hatGetType(); the default is not carried (no hat outside 212-223 seen).
- **Stuck in Cartel's second silo doorway (F3 20261003-214052).** Silo 2 is
  silo 1 moved 5000 in z: same sliver tiles (540/541) across the doorway's
  crouch tiles 535/536. geStanForcesCrouch() found the tile by height (the
  sliver), so a player stopped there stood up and every step was held for
  the squat; at 3/240 steps it never finished. Now the walked tile
  (stanMoverTile()), no hold from a crouch tile, give-way counted in 240ths.
  Probe: `~/wt/f3-1003d-gfgame-run/probe/stop.py` (STEP240=3 reproduces on
  the base binary; gdbpd's frames() deletes every breakpoint - use a local one).
- **Capture's armour (F3 20261003-200644)**: record 316 on pad 708 is a type
  3 prop with the armour model in its own setup - scenery on the cartridge
  too; the collectable one is record 399 (pad 598).
- **Reload sound (F3 20261003-164059, on 4e98c8a)**: GUN_RIFLECOCK 50 was
  one of the 11 s envelopes cut by n_sndplayer; fixed by b68af49f9.

## F3 pass 34 (2026-10-04, fix/f3-1004b-gf64)

- **Stuck in a Cartel silo doorway (F3 20261004-084819, "silo door closing on
  Bond").** The doorway at the door on pad 1440 (-6351, -6786; a swinging door
  turning about its middle) is two crouch tiles (728/729) crossed by an
  unlinked sliver pair (730/731) lying at the doorway floor's own height (-540).
  A player easing to a stop just over the 729/728 line was off the tile
  geStanFloorAhead() remembered; it re-found his floor by height, took the
  sliver, and all its walls stood round him - stuck with the door open or
  shut (the door is a bystander: a blocked closing door stops in GoldenEye
  too, door7F054FB4()). Now `stanWalkOnTo()` walks on from the remembered tile
  (or the move's start tile) through the links first. Probe:
  `~/wt/f3-1004b-gf64-run/probe/walkjam7.py` (CLOSE=0/1); walkcmp unchanged
  on GF 0/1/5/9 and GE 0/5/9.
- **Bond sinks into the floor in Cartel's ending (F3 20261004-171154).** Its
  ending list (0x44f, kicked from 0x1003 +147; gecinemaFindEnding() does not
  find it) plays animation 45, `bond_watch`. Goldfinger 64 and Tomorrow Never
  Dies 64 replaced 45 with Bond's whole body raising his wrist (GoldenEye's is
  the arm alone); TND64 also redid 129, 170 and 171. gexPlusMissionAnimLoad()
  appended one set a session, so after any GoldenEye mission Cartel played
  GoldenEye's arm animation on Bond's body. Now one set per conversion, rows
  byte-identical to an earlier set's shared (GF64 appends 1 after GoldenEye's
  173); gewatch.c's watch arm likewise loads its `bond_watch` per mod. Probe:
  `probe/outro7.py` (GEFIRST=1 loads GoldenEye's set before Cartel's).
